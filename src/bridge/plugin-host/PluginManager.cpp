/*
 * PluginManager — implementation. See PluginManager.h.
 */
#include "PluginManager.h"
#include "bridge/plugin-host/HostApi.h"
#include "bridge/plugin-host/PluginDescriptor.h"
#include "bridge/plugin-host/loader/Loader.h"
#if BAREDAW_ENABLE_CLAP
#include "bridge/plugin-host/clap/ClapAdapter.h"
#include "clap/clap.h"
#endif
#if BAREDAW_ENABLE_VST3
#include "bridge/plugin-host/vst3/Vst3Adapter.h"
#endif

#include "core/DeviceRegistry.h"
#include "core/Device.h"
#include "bridge/plugin-host/PluginDevice.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace pluginhost
{

namespace
{

// Owned by trampolineRegister — captures the per-type registration so we can
// unregister on unloadAll even if the plugin's baredaw_plugin_shutdown never
// cooperates.
struct PendingRegistration
{
    std::string typeName;
    void *(*factory)(void *, double, uint32_t) = nullptr;
    void (*destroy)(void *, void *) = nullptr;
    uint8_t accepts = 0;
    uint8_t produces = 0;
};

PendingRegistration *findPending(std::vector<PendingRegistration> &pending,
                                 const std::string &typeName)
{
    for (auto &p : pending)
        if (p.typeName == typeName) return &p;
    return nullptr;
}

bool isSkippedByEnvironment(const fs::path &path)
{
    const char *raw = std::getenv("BAREDAW_PLUGIN_SKIP");
    if (!raw || !*raw) return false;
    const std::string list(raw);
    size_t begin = 0;
    while (begin <= list.size()) {
        const size_t end = list.find_first_of(";,", begin);
        const size_t tokenEnd = end == std::string::npos ? list.size() : end;
        size_t first = begin;
        size_t last = tokenEnd;
        while (first < last && std::isspace(static_cast<unsigned char>(list[first]))) ++first;
        while (last > first && std::isspace(static_cast<unsigned char>(list[last - 1]))) --last;
        const std::string token = list.substr(first, last - first);
        if (!token.empty() && (token == path.filename().string() || token == path.string()))
            return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

constexpr const char *kHostName = "BareDAW";
constexpr const char *kHostVersion = "0.0.0-plugin-host";

}

// Thread-local pointer to the manager currently driving plugin code. The
// trampolines read this to find their `self` — they can't rely on the
// plugin's hostCtx argument being the manager, because for CLAP-via-Adapter
// the plugin passes its own per-type pointer (ClapRegistrationInfo*) there
// instead. Setting it before scanAndLoad (and clearing after) keeps the
// trampolines stateless on the manager side. `static` at namespace scope
// gives internal linkage (TU-local), so other TUs of the plugin_host target
// don't see this symbol.
static thread_local PluginManager *s_currentManager = nullptr;

PluginManager *PluginManager::currentManager()
{
    return s_currentManager;
}

PluginManager::PluginManager(fs::path pluginsDir,
                             double sampleRate,
                             uint32_t maxBlock,
                             Logger logger)
    : PluginManager(std::vector<fs::path>{std::move(pluginsDir)},
                    sampleRate, maxBlock, std::move(logger))
{
}

PluginManager::PluginManager(std::vector<fs::path> pluginDirs,
                             double sampleRate,
                             uint32_t maxBlock,
                             Logger logger)
    : pluginDirs_(std::move(pluginDirs)),
      sampleRate_(sampleRate),
      maxBlock_(maxBlock),
      logger_(std::move(logger))
{
    // Build the hostApi — function pointers point at our static trampolines,
    // host_ctx points at this manager (the trampolines read it via the
    // thread-local s_currentManager, but plugins may also use host_ctx for
    // their own purposes).
    hostApi_.abi_version = BAREDAW_PLUGIN_ABI_VERSION;
    hostApi_.struct_size = sizeof(BareDAW_HostApi);
    hostApi_.host_name = kHostName;
    hostApi_.host_version = kHostVersion;
    hostApi_.sample_rate_hz = static_cast<uint32_t>(sampleRate);
    hostApi_.max_block_size = maxBlock;
    hostApi_.host_ctx = this;
    hostApi_.register_device = &PluginManager::trampolineRegister;
    hostApi_.unregister_device = &PluginManager::trampolineUnregister;
    hostApi_.host_exclusive = &PluginManager::trampolineExclusive;
    hostApi_.log = &PluginManager::trampolineLog;
}

PluginManager::~PluginManager()
{
    unloadAll();
}

bool PluginManager::unloadAll()
{
    for (const auto &lib : libs_) {
#if BAREDAW_ENABLE_CLAP
        for (const void *data : lib.clapRegistrationData) {
            if (clapadapter::hasLiveInstances(data)) {
                logMessage(1, "cannot rescan/unload CLAP plugins while plugin devices are on tracks");
                return false;
            }
        }
#endif
#if BAREDAW_ENABLE_VST3
        for (const void *data : lib.vst3RegistrationData) {
            if (vst3adapter::hasLiveInstances(data)) {
                logMessage(1, "cannot rescan/unload VST3 plugins while plugin devices are on tracks");
                return false;
            }
        }
#endif
    }

    auto &registry = DeviceRegistry::instance();
    for (const auto &type : registeredTypes_) {
        // Unregister from the engine's DeviceRegistry. The plugin's
        // baredaw_plugin_shutdown (if it exists) would also call this, but
        // we don't rely on it — force-cleanup is the contract.
        registry.unregisterDevice(type);
        pluginDataByType_.erase(type);
    }
    for (const auto &[type, path] : lazyPathByType_)
        registry.unregisterDevice(type);
    registeredTypes_.clear();
    displayNamesByType_.clear();
    metadataByType_.clear();
    lazyPathByType_.clear();
    lazyTypesByPath_.clear();

    for (auto &lib : libs_) {
#if BAREDAW_ENABLE_CLAP
        for (void *data : lib.clapRegistrationData)
            clapadapter::releaseRegistrationData(data);
        lib.clapRegistrationData.clear();
        if (lib.clapEntry) {
            static_cast<const clap_plugin_entry_t *>(lib.clapEntry)->deinit();
            lib.clapEntry = nullptr;
        }
#endif
#if BAREDAW_ENABLE_VST3
        for (void *data : lib.vst3RegistrationData)
            vst3adapter::releaseRegistrationData(data);
        lib.vst3RegistrationData.clear();
#endif
        // Reconstruct the per-platform LoadedLib so we can call the loader's
        // unload() — the loader TU owns dlclose semantics, not us. unload()
        // only reads lib.dlHandle; the initFn / clapEntry fields are ignored.
        if (lib.dlHandle) {
            LoadedLib ll;
            ll.dlHandle = lib.dlHandle;
            ll.initFn = nullptr;
            ll.clapEntry = nullptr;
            pluginhost::unload(ll);
        }
    }
    libs_.clear();
    return true;
}

size_t PluginManager::scanAndLoad()
{
    if (!libs_.empty()) {
        logMessage(1, "plugins are already loaded; unload them before scanning again");
        return 0;
    }
    return scanPaths(pluginDirs_);
}

size_t PluginManager::scanAndLoadPath(const fs::path &pluginPath)
{
    return scanPaths(std::vector<fs::path>{pluginPath});
}

void PluginManager::installLazyBundle(const fs::path &path,
                                      const std::vector<TypeMetadata> &types)
{
    const std::string key = path.lexically_normal().string();
    lazyTypesByPath_[key] = types;
    for (const auto &meta : types) {
        if (meta.type.empty() || DeviceRegistry::instance().hasType(meta.type)) continue;
        lazyPathByType_[meta.type] = path;
        metadataByType_[meta.type] = meta;
        displayNamesByType_[meta.type] = meta.displayName.empty() ? meta.type : meta.displayName;
        registeredTypes_.push_back(meta.type);
        DeviceRegistry::instance().registerDevice(
            meta.type,
            [this, type = meta.type]() -> std::unique_ptr<Device> {
                if (!activateLazyType(type)) return nullptr;
                return DeviceRegistry::instance().create(type);
            },
            static_cast<DeviceIO>(meta.accepts),
            static_cast<DeviceIO>(meta.produces));
    }
}

void PluginManager::registerLazyBundle(const fs::path &pluginPath,
                                       const std::vector<TypeMetadata> &types)
{
    if (!types.empty()) installLazyBundle(pluginPath, types);
}

bool PluginManager::activateLazyType(const std::string &type)
{
    const auto pathIt = lazyPathByType_.find(type);
    if (pathIt == lazyPathByType_.end()) return DeviceRegistry::instance().hasType(type);
    const fs::path path = pathIt->second;
    const std::string key = path.lexically_normal().string();
    const auto bundleIt = lazyTypesByPath_.find(key);
    if (bundleIt == lazyTypesByPath_.end()) return false;
    const auto cachedTypes = bundleIt->second;

    for (const auto &meta : cachedTypes) {
        DeviceRegistry::instance().unregisterDevice(meta.type);
        lazyPathByType_.erase(meta.type);
        registeredTypes_.erase(std::remove(registeredTypes_.begin(), registeredTypes_.end(),
                                           meta.type), registeredTypes_.end());
    }
    lazyTypesByPath_.erase(key);
    if (scanAndLoadPath(path) != 0 && DeviceRegistry::instance().hasType(type))
        return true;

    logMessage(2, "lazy load failed: " + path.string());
    installLazyBundle(path, cachedTypes);
    return false;
}

std::vector<PluginManager::TypeMetadata> PluginManager::typeMetadata() const
{
    std::vector<TypeMetadata> result;
    result.reserve(metadataByType_.size());
    for (const auto &[type, metadata] : metadataByType_) {
        auto copy = metadata;
        copy.displayName = displayNameForType(type);
        result.push_back(std::move(copy));
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.type < b.type;
    });
    return result;
}

size_t PluginManager::scanPaths(const std::vector<fs::path> &paths)
{
    // Install ourselves as the thread-local "current manager" so the static
    // trampolines can route register_device / log calls back to us. Plugins
    // are allowed to call these from any thread they own (init/destroy are
    // main-thread per CLAP; plugins that fork helpers need their own scan
    // path — v1 keeps it single-threaded).
    PluginManager *prev = s_currentManager;
    s_currentManager = this;

    size_t loaded = 0;
    for (const fs::path &pluginPath : paths) {
        if (!fs::exists(pluginPath)) {
            logMessage(1, "plugins path does not exist: " + pluginPath.string());
            continue;
        }
        std::vector<fs::directory_entry> entries;
        const bool directBundle = fs::is_directory(pluginPath) &&
            (pluginPath.extension() == ".vst3" || pluginPath.extension() == ".clap");
        if (directBundle || fs::is_regular_file(pluginPath)) {
            entries.emplace_back(pluginPath);
        } else if (fs::is_directory(pluginPath)) {
            for (const auto &entry : fs::directory_iterator(pluginPath))
                entries.push_back(entry);
        } else {
            logMessage(1, "unsupported plugins path: " + pluginPath.string());
            continue;
        }
    for (const auto &entry : entries) {
        const std::string filename = entry.path().filename().string();
        if (!filename.empty() && filename.front() == '.') continue;
        if (isSkippedByEnvironment(entry.path())) {
            logMessage(1, "skipping plugin from BAREDAW_PLUGIN_SKIP: " + filename);
            continue;
        }
        const bool isMacClapBundle = entry.is_directory() &&
                                      entry.path().extension() == ".clap";
        const bool isVst3Bundle = entry.is_directory() &&
                                  entry.path().extension() == ".vst3";
        if (!entry.is_regular_file() && !isMacClapBundle && !isVst3Bundle) continue;

        const fs::path &path = entry.path();
        LibRecord rec;

#if BAREDAW_ENABLE_VST3
        if (isVst3Bundle) {
            const size_t n = vst3adapter::registerFromBundle(
                path.string().c_str(), sampleRate_, maxBlock_, hostApi_,
                rec.vst3RegistrationData);
            if (n == 0) continue;
            for (const void *data : rec.vst3RegistrationData) {
                displayNamesByType_[vst3adapter::registrationTypeName(data)] =
                    vst3adapter::registrationDisplayName(data);
            }
            logMessage(0, "loaded VST3 plugin: " + path.filename().string() +
                    " (" + std::to_string(n) + " device type" +
                    (n == 1 ? "" : "s") + ")");
            ++loaded;
            libs_.push_back(std::move(rec));
            continue;
        }
#else
        if (isVst3Bundle) continue;
#endif

        // Per-platform loader. Resolves both baredaw_plugin_init and clap_entry.
        auto loaded_lib = pluginhost::load(path.string().c_str());
        rec.dlHandle = loaded_lib.dlHandle;

        if (!rec.dlHandle) {
            // dlopen failed — log if it was something we tried to open (not a
            // plain text file). For now: log warn if dlerror has content.
            std::string err = pluginhost::lastError();
            if (!err.empty()) {
                logMessage(1, "could not load " + path.string() + ": " + err);
            }
            continue;
        }

        if (loaded_lib.initFn) {
            // BareDAW-native plugin
            using InitFn = int (*)(const BareDAW_HostApi *, BareDAW_PluginDescriptor *);
            auto init = reinterpret_cast<InitFn>(loaded_lib.initFn);

            BareDAW_PluginDescriptor desc{};
            desc.struct_size = sizeof(BareDAW_PluginDescriptor);

            const int rc = init(&hostApi_, &desc);
            if (rc != 0) {
                logMessage(2, path.string() + ": baredaw_plugin_init returned " +
                        std::to_string(rc));
                pluginhost::unload(loaded_lib);
                continue;
            }
            if ((desc.abi_version >> 16) != BAREDAW_PLUGIN_ABI_VERSION_MAJOR) {
                logMessage(2, path.string() + ": ABI major mismatch (plugin=" +
                        std::to_string(desc.abi_version >> 16) +
                        ", host=" +
                        std::to_string(BAREDAW_PLUGIN_ABI_VERSION_MAJOR) + ")");
                pluginhost::unload(loaded_lib);
                continue;
            }
            logMessage(0, std::string("loaded plugin: ") +
                    (desc.name ? desc.name : desc.id ? desc.id : "<unnamed>") +
                    " v" + (desc.version ? desc.version : "?"));
            ++loaded;
#if BAREDAW_ENABLE_CLAP
        } else if (loaded_lib.clapEntry) {
            // CLAP-format plugin — delegate to ClapAdapter
            const size_t n = clapadapter::registerFromLib(
                rec.dlHandle,
                path.string().c_str(),
                static_cast<const clap_plugin_entry_t *>(loaded_lib.clapEntry),
                sampleRate_,
                maxBlock_,
                hostApi_,
                rec.clapRegistrationData);
            if (n == 0) {
                pluginhost::unload(loaded_lib);
                continue;
            }
            rec.clapEntry = loaded_lib.clapEntry;
            for (const void *data : rec.clapRegistrationData) {
                displayNamesByType_[clapadapter::registrationTypeName(data)] =
                    clapadapter::registrationDisplayName(data);
            }
            logMessage(0, "loaded CLAP plugin: " + path.filename().string() +
                    " (" + std::to_string(n) + " device type" +
                    (n == 1 ? "" : "s") + ")");
            ++loaded;
#endif
        } else {
            // Neither symbol — not a BareDAW or CLAP plugin. Skip silently.
            pluginhost::unload(loaded_lib);
            continue;
        }

        libs_.push_back(std::move(rec));
    }
    }

    s_currentManager = prev;
    return loaded;
}

// ──── trampolines ─────────────────────────────────────────────────────────

int PluginManager::trampolineRegister(void *hostCtx, const char *typeName,
                                     void *(*factory)(void *, double, uint32_t),
                                     void (*destroy)(void *, void *),
                                     uint8_t accepts, uint8_t produces)
{
    // hostCtx is whatever the plugin passed as the first arg to register_device.
    // For CLAP-via-Adapter that's the ClapAdapter's ClapRegistrationInfo*; for
    // BareDAW-native plugins it's typically hostApi.host_ctx. We stash it as
    // opaque pluginData and forward it to factory() when the engine later
    // instantiates the device. (PluginManager::scanAndLoad sets a thread-local
    // current-manager pointer before invoking plugin code, so the trampolines
    // don't need hostCtx to be self — the plugin's choice is opaque to us.)
    auto *self = currentManager();
    if (!self) return -1;
    return self->onRegister(typeName, nullptr, hostCtx, factory, destroy,
                            accepts, produces) ? 0 : -1;
}

int PluginManager::trampolineUnregister(void *hostCtx, const char *typeName)
{
    (void)hostCtx;
    auto *self = currentManager();
    if (!self) return -1;
    self->onUnregister(typeName);
    return 0;
}

void PluginManager::trampolineExclusive(void *hostCtx, void (*fn)(void *), void *pluginCtx)
{
    (void)hostCtx;
    // v1: run synchronously. The host (main.mm) can replace this with a
    // runInteractive-wrapped version if it wants between-blocks semantics.
    if (fn) fn(pluginCtx);
}

void PluginManager::trampolineLog(void *hostCtx, int severity, const char *message)
{
    (void)hostCtx;
    auto *self = currentManager();
    if (!self) return;
    self->onLog(severity, message);
}

// ──── onRegister / onUnregister / onLog ──────────────────────────────────

bool PluginManager::onRegister(const char *typeName,
                               LibRecord *lib, void *pluginData,
                               void *(*factory)(void *, double, uint32_t),
                               void (*destroy)(void *, void *),
                               uint8_t accepts, uint8_t produces)
{
    if (!typeName || !*typeName) {
        logMessage(2, "plugin tried to register a device with empty type name");
        return false;
    }
    if (DeviceRegistry::instance().hasType(typeName)) {
        logMessage(2, std::string("type already registered, refusing: ") + typeName);
        return false;
    }

    // Stash the per-type pluginData so the lambda below can hand the same
    // value back to factory() when the engine instantiates the device.
    // Plugins are required to pass the same pluginData every time; we don't
    // copy, just store the pointer — the plugin owns the memory.
    pluginDataByType_[typeName] = pluginData;

    DeviceRegistry::instance().registerDevice(
        typeName,
        [factory, pluginData, sr = sampleRate_, mb = maxBlock_]() -> std::unique_ptr<Device> {
            // factory is the C-ABI function pointer the plugin passed at
            // register_device. Plugins return a PluginDevice* (their subclass
            // of Device). pluginData is whatever the plugin passed as hostCtx
            // to register_device — for CLAP-via-Adapter it's the
            // ClapRegistrationInfo* clapFactory needs; for BareDAW-native
            // plugins it's typically hostApi.host_ctx (which is the
            // PluginManager*; plugins may ignore it or use it for lookups).
            //
            // Plugins that want per-instance host_ctx (e.g. a stable handle
            // back into a manager-owned struct) is a future ABI extension —
            // see docs/Plugin_Host_Plan.md §10 Q2.
            //
            // sr/mb are the host's sample rate / block size at registration
            // time — plugins use them to size internal buffers (CLAP's
            // activate() is called with these). Passing the host's actual
            // values (not 0.0 / 0) avoids plugins getting divide-by-zero in
            // any per-sample math that runs before activate() fixes the rate.
            void *raw = factory(pluginData, sr, mb);
            // PluginDevice is a Device subclass with a virtual destructor.
            // We cast through Device* (the base) so deletion runs through
            // Device's vtable — the plugin author's subclass destructor is
            // invoked correctly.
            return std::unique_ptr<Device>(static_cast<Device *>(raw));
        },
        static_cast<DeviceIO>(accepts),
        static_cast<DeviceIO>(produces));

    if (lib) lib->types.emplace_back(typeName);
    registeredTypes_.emplace_back(typeName);
    metadataByType_[typeName] = TypeMetadata{
        typeName, displayNameForType(typeName), accepts, produces};
    (void)destroy; // captured indirectly: Device's destructor is virtual,
                   // PluginDevice's destructor is virtual (it overrides
                   // Device::~Device). Registry::create returns a
                   // unique_ptr<Device>; on reset the unique_ptr deletes
                   // through the Device vtable → PluginDevice::~PluginDevice
                   // → no plugin-side cleanup. The plan's "destroy_device"
                   // callback is a future-work hook for plugins that need
                   // to release non-Device resources (e.g. external MIDI
                   // ports). For v1 we leave destroy unused.
    return true;
}

void PluginManager::onUnregister(const char *typeName)
{
    if (!typeName) return;
    DeviceRegistry::instance().unregisterDevice(typeName);
    pluginDataByType_.erase(typeName);
    auto it = std::find(registeredTypes_.begin(), registeredTypes_.end(),
                        std::string(typeName));
    if (it != registeredTypes_.end()) registeredTypes_.erase(it);
}

void PluginManager::onLog(int severity, const char *message)
{
    logMessage(severity, message ? message : "");
}

void PluginManager::logMessage(int severity, const std::string &message)
{
    if (logger_) logger_(severity, message);
    else {
        // Default: stderr if no logger was supplied.
        const char *tag = severity == 2 ? "ERR" : severity == 1 ? "warn" : "info";
        std::fprintf(stderr, "[plugin-host %s] %s\n", tag, message.c_str());
    }
}

std::string PluginManager::displayNameForType(const std::string &type) const
{
    const auto it = displayNamesByType_.find(type);
    return it == displayNamesByType_.end() ? type : it->second;
}

}
