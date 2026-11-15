/*
 * Loader.h — platform-neutral interface to the per-OS plugin loader TU
 * (PosixDlopen.cpp on Linux/macOS, Win32LoadLibrary.cpp on Windows).
 *
 * CMake's generator expression selects exactly one loader TU per build:
 *   $<$<PLATFORM_ID:Windows>:loader/Win32LoadLibrary.cpp>
 *   $<$<NOT:$<PLATFORM_ID:Windows>>:loader/PosixDlopen.cpp>
 * so the function definitions are non-inline (regular symbols in a TU) and
 * the linker picks the platform's implementation.
 *
 * PluginManager calls load/unload/lastError without naming the platform —
 * the loader stays an implementation detail.
 */
#pragma once

#include <string>

namespace pluginhost
{

struct LoadedLib
{
    void *dlHandle = nullptr;       // platform-specific (dlopen / HMODULE)
    void *initFn = nullptr;         // baredaw_plugin_init, if present
    void *clapEntry = nullptr;      // &clap_entry (struct symbol), if present
};

// Resolves the file at libPath. Returns LoadedLib{} if dlopen fails.
// A non-null result that has both initFn and clapEntry null means the file
// loaded but is not a BareDAW or CLAP plugin (caller should unload and skip).
//
// The handle ownership transfers to the caller; pass it back to unload().
LoadedLib load(const char *libPath);

// dlclose/FreeLibrary on lib.dlHandle and zero the struct. Safe to call
// multiple times.
void unload(LoadedLib &lib);

// Snapshot of dlerror()/GetLastError() for the most recent failed dl* call.
// Safe to read on the calling thread before any further dl* operation.
std::string lastError();

}