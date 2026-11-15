/*
 * PosixDlopen — the platform loader TU for plugin-host on Linux/macOS.
 * One per OS family; selected by CMake's generator expression. Mirrors
 * Win32LoadLibrary.cpp on Windows.
 *
 * Kept as a single TU per OS so the host only links the platform-specific
 * loader it actually needs; with BAREDAW_ENABLE_PLUGINS=OFF, none of these
 * are linked into baredaw_app.
 */
#include "bridge/plugin-host/loader/Loader.h"

#include <dlfcn.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace pluginhost
{

LoadedLib load(const char *libPath)
{
    LoadedLib r;
    std::filesystem::path loadPath(libPath ? libPath : "");
#if defined(__APPLE__)
    // A macOS CLAP is normally a bundle directory. dlopen needs the Mach-O
    // executable inside Contents/MacOS, while clap_entry::init still receives
    // the outer bundle path from PluginManager.
    if (std::filesystem::is_directory(loadPath) && loadPath.extension() == ".clap") {
        const auto executables = loadPath / "Contents" / "MacOS";
        const auto conventional = executables / loadPath.stem();
        if (std::filesystem::is_regular_file(conventional)) {
            loadPath = conventional;
        } else if (std::filesystem::is_directory(executables)) {
            for (const auto &entry : std::filesystem::directory_iterator(executables)) {
                if (entry.is_regular_file()) {
                    loadPath = entry.path();
                    break;
                }
            }
        }
    }
#endif
    // RTLD_LOCAL: plugin symbols don't pollute our global table (important —
    // two plugins with the same internal symbol name must not collide).
    // RTLD_NOW: resolve all symbols NOW so we fail fast instead of at first call.
    r.dlHandle = dlopen(loadPath.string().c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!r.dlHandle) return r;

    // BareDAW-native plugin entry point.
    r.initFn = dlsym(r.dlHandle, "baredaw_plugin_init");

    // CLAP plugin entry point — clap_entry is a const struct symbol, not a
    // function, so we just take its address.
    r.clapEntry = dlsym(r.dlHandle, "clap_entry");

    // Neither present → not a BareDAW or CLAP plugin. Caller will unload.
    if (!r.initFn && !r.clapEntry) {
        dlclose(r.dlHandle);
        r.dlHandle = nullptr;
    }
    return r;
}

void unload(LoadedLib &lib)
{
    if (lib.dlHandle) {
        dlclose(lib.dlHandle);
        lib.dlHandle = nullptr;
        lib.initFn = nullptr;
        lib.clapEntry = nullptr;
    }
}

std::string lastError()
{
    const char *e = dlerror();
    return e ? std::string(e) : std::string();
}

}
