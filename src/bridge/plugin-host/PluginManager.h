/*
 * PluginManager — C++ façade that scans a plugins directory, loads each
 * plugin shared library, and registers discovered devices in the engine's
 * DeviceRegistry.
 *
 * Three file-format paths share one manager:
 *   1. BareDAW-native: file exports `baredaw_plugin_init` — manager calls it
 *      directly with the hostApi. The plugin populates a descriptor and
 *      registers one or more devices via hostApi.register_device().
 *   2. CLAP: file exports `clap_entry` — manager delegates to ClapAdapter,
 *      which iterates clap.plugin-factory descriptors and registers each as
 *      a Device.
 *   3. VST3: a `.vst3` bundle is opened by Vst3Adapter, which enumerates its
 *      audio-effect classes and registers each through the same HostApi.
 *
 * Shared-library discovery is filter-free at the symbol level: we try
 * `baredaw_plugin_init`, fall back to `clap_entry`, and skip files that have
 * neither. `.vst3` bundles are the one extension-routed case because the VST3
 * SDK owns their platform-specific bundle loading and class enumeration.
 *
 * Threading: scanAndLoad is called on the control thread (= main thread per
 * CLAP). All other ops (register_device, factory, destroy_device on
 * individual devices) are handled by their respective owners.
 *
 * Lifetime: PluginManager owns its dlopen handles. unloadAll unregisters
 * every type and closes each library only when no plugin-backed Device
 * instances remain; otherwise it refuses the unload without changing state.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bridge/plugin-host/HostApi.h"

namespace pluginhost
{

class PluginManager
{
public:
    struct TypeMetadata
    {
        std::string type;
        std::string displayName;
        uint8_t accepts = 0;
        uint8_t produces = 0;
    };

    // severity: 0 = info, 1 = warn, 2 = error
    using Logger = std::function<void(int severity, const std::string &message)>;

    PluginManager(std::filesystem::path pluginsDir,
                  double sampleRate,
                  uint32_t maxBlock,
                  Logger logger = {});
    PluginManager(std::vector<std::filesystem::path> pluginDirs,
                  double sampleRate,
                  uint32_t maxBlock,
                  Logger logger = {});
    ~PluginManager();

    PluginManager(const PluginManager &) = delete;
    PluginManager &operator=(const PluginManager &) = delete;

    // Iterates every entry under each configured directory (non-recursive),
    // opens shared libraries or VST3 bundles, and registers devices. Returns
    // the count of plugins successfully loaded (0 means no plugins found,
    // all failed, or pluginsDir didn't exist).
    //
    // Pre: no plugins are currently loaded (don't call twice without
    // unloadAll in between).
    size_t scanAndLoad();

    // Loads one explicit bundle/file into this manager without unloading
    // already-loaded plugins. Used by the native startup screen to keep the UI
    // responsive and show progress one cached bundle at a time.
    size_t scanAndLoadPath(const std::filesystem::path &pluginPath);
    void registerLazyBundle(const std::filesystem::path &pluginPath,
                            const std::vector<TypeMetadata> &types);
    std::vector<TypeMetadata> typeMetadata() const;

    // Unregisters every type this manager registered and dlcloses each lib.
    // Safe to call multiple times. After a successful unloadAll, the manager
    // is reusable. Returns false without changing state when plugin-backed
    // devices are still alive.
    bool unloadAll();

    // Number of device types currently registered by this manager.
    size_t registeredTypeCount() const { return registeredTypes_.size(); }

    // Snapshot of type names this manager registered. For diagnostics and
    // tests — these are the types that appear in AudioEngine::getAvailableInstruments
    // / getAvailableAudioEffects() until unloadAll() is called.
    std::vector<std::string> registeredTypes() const { return registeredTypes_; }
    std::string displayNameForType(const std::string &type) const;

    // The trampolines can't rely on the plugin's hostCtx being this manager
    // (CLAP-via-Adapter passes its own per-type data there), so they read a
    // thread-local pointer set by scanAndLoad before invoking plugin code.
    // Public for the .cpp's file-static trampolines; tests/diagnostics only.
    static PluginManager *currentManager();

private:
    struct LibRecord
    {
        void *dlHandle = nullptr;       // platform-specific (void* to keep loader types out of header)
        const void *clapEntry = nullptr; // non-null after successful clap_entry.init()
        std::vector<void *> clapRegistrationData;
        std::vector<void *> vst3RegistrationData;
        std::vector<std::string> types; // types registered from this lib
    };

    // HostApi trampolines — called by plugins via the hostApi function-pointer
    // table. They route back to PluginManager::onRegister / onUnregister / etc.
    //
    // The `hostCtx` argument here is whatever the plugin passed as the first
    // parameter to register_device / unregister_device / etc. For CLAP-via-
    // Adapter this is the ClapAdapter's per-type ClapRegistrationInfo*; for
    // BareDAW-native plugins it's typically hostApi.host_ctx. PluginManager
    // treats it as opaque and stashes it per-type so the engine's later
    // factory() call can hand the same value back to the plugin (see
    // pluginDataByType_ below).
    static int trampolineRegister(void *hostCtx, const char *typeName,
                                  void *(*factory)(void *, double, uint32_t),
                                  void (*destroy)(void *, void *),
                                  uint8_t accepts, uint8_t produces);
    static int trampolineUnregister(void *hostCtx, const char *typeName);
    static void trampolineExclusive(void *hostCtx, void (*fn)(void *), void *pluginCtx);
    static void trampolineLog(void *hostCtx, int severity, const char *message);

    bool onRegister(const char *typeName, LibRecord *lib, void *pluginData,
                    void *(*factory)(void *, double, uint32_t),
                    void (*destroy)(void *, void *),
                    uint8_t accepts, uint8_t produces);
    void onUnregister(const char *typeName);
    void onLog(int severity, const char *message);
    // Internal logger — written as `logMessage` rather than `log` so it doesn't
    // collide with std::log from <cmath>.
    void logMessage(int severity, const std::string &message);
    size_t scanPaths(const std::vector<std::filesystem::path> &paths);
    bool activateLazyType(const std::string &type);
    void installLazyBundle(const std::filesystem::path &path,
                           const std::vector<TypeMetadata> &types);

    BareDAW_HostApi hostApi_{}; // populated in ctor

    std::vector<std::filesystem::path> pluginDirs_;
    double sampleRate_;
    uint32_t maxBlock_;
    Logger logger_;

    std::vector<LibRecord> libs_;
    std::vector<std::string> registeredTypes_;

    // Per-type plugin data, set when a plugin calls register_device. The
    // engine later invokes factory(pluginData, sampleRate, maxBlock) when it
    // wants to instantiate the device — this is how CLAP-via-Adapter gives
    // its ClapRegistrationInfo back to clapFactory, and how a future
    // BareDAW-native plugin could carry per-type state into factory().
    std::unordered_map<std::string, void *> pluginDataByType_;
    std::unordered_map<std::string, std::string> displayNamesByType_;
    std::unordered_map<std::string, TypeMetadata> metadataByType_;
    std::unordered_map<std::string, std::filesystem::path> lazyPathByType_;
    std::unordered_map<std::string, std::vector<TypeMetadata>> lazyTypesByPath_;
};

}
