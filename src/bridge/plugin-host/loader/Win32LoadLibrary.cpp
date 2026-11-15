/*
 * Win32LoadLibrary — the platform loader TU for plugin-host on Windows.
 *
 * Phase 1 stub. The native dev loop runs on macOS; the macOS path (PosixDlopen)
 * is what the smoke test exercises. The Windows TU exists so CMake's
 * generator-expression platform picker has somewhere to point on a future
 * Windows plugin-host build.
 */
#include "bridge/plugin-host/loader/Loader.h"

namespace pluginhost
{

LoadedLib load(const char * /*libPath*/)
{
    // TODO(plugin-host, win32): implement with LoadLibraryA + GetProcAddress.
    // See PosixDlopen.cpp for the reference shape.
    return LoadedLib{};
}

void unload(LoadedLib & /*lib*/)
{
    // TODO(plugin-host, win32): FreeLibrary on dlHandle.
}

std::string lastError()
{
    // TODO(plugin-host, win32): FormatMessageA on GetLastError().
    return {};
}

}