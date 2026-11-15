/*
 * ClapAdapter — host-side TU that wraps a CLAP-format plugin as one or more
 * BareDAW Devices. Lives in src/bridge/plugin-host/clap/ and is linked into
 * baredaw_plugin_host only when BAREDAW_ENABLE_PLUGINS=ON. Not visible to
 * baredaw_core; the engine talks to the wrapped devices through the same
 * DeviceRegistry the built-in devices use.
 *
 * Phase 1 wraps a strict CLAP subset (§8 of docs/Plugin_Host_Plan.md):
 *   - clap_plugin_entry.init / deinit / get_factory
 *   - clap_plugin_factory.get_plugin_descriptor / create_plugin
 *   - clap_plugin.init / destroy / activate / deactivate / start_processing /
 *     stop_processing / process
 *   - clap_plugin_audio_ports.count / get     (declares channel counts at load)
 *   - clap_plugin_params.count / get_info / get_value
 *   - clap_plugin_note_ports.count / get     (declares MIDI in/out at load)
 *   - clap_plugin_gui embedded native editor lifecycle (Cocoa host today)
 *
 * Anything not in the subset (state, latency, voice-info, thread-pool,
 * timer-support, ...) is treated as "this plugin requires capabilities we
 * don't ship in v1" → plugin is skipped with a clear log message.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/plugin-host/HostApi.h"
#include "clap/clap.h" // pulls in clap_plugin_entry_t and friends

namespace pluginhost::clapadapter
{

// Wraps one CLAP-format shared library as zero or more BareDAW Devices,
// registering each through hostApi.register_device. Returns the number of
// plugins successfully registered (0 means the file was CLAP but had no
// loadable plugins).
//
// Pre: entry is the result of dlsym(libHandle, "clap_entry") — non-null.
//      libPath is the file path passed to entry->init(libPath).
//      hostApi is the function-pointer table from PluginManager.
//
// Post: If return > 0, the registered devices are alive in the host's
//       DeviceRegistry. PluginManager owns the dl_handle; this TU does
//       NOT call entry->deinit() — PluginManager does that on unloadAll
//       after unregister_device has been called for every registered type.
size_t registerFromLib(void *dlHandle,
                       const char *libPath,
                       const clap_plugin_entry_t *entry,
                       double sampleRate,
                       uint32_t maxBlock,
                       const BareDAW_HostApi &hostApi,
                       std::vector<void *> &ownedRegistrationData);

// Registration records are adapter-owned and must outlive all instances.
bool hasLiveInstances(const void *registrationData);
void releaseRegistrationData(void *registrationData);
std::string registrationTypeName(const void *registrationData);
std::string registrationDisplayName(const void *registrationData);

}
