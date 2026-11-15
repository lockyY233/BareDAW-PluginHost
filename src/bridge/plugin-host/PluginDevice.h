/*
 * BareDAW plugin device — the base class plugin authors subclass.
 *
 * Each plugin instance IS a PluginDevice (a Device subclass). The plugin's
 * factory (called from HostApi::register_device) returns a PluginDevice*
 * (cast to void* per the C ABI); the host's PluginManager wraps it in the
 * DeviceRegistry as a Device* via static_cast.
 *
 * Plugins override only the audio virtuals they need; everything else
 * inherits Device's no-op defaults. Lifetime: the host deletes via the
 * destroy_device callback passed at registration time; the plugin's
 * destroy_device typically just `delete`s the instance, which dispatches
 * through PluginDevice's virtual destructor.
 *
 * See docs/Plugin_Host_Plan.md §4.
 */
#pragma once

#include "core/Device.h"

class PluginDevice : public Device
{
public:
    ~PluginDevice() override = default;

protected:
    // accept/produces — plugin author may either override these per-instance
    // (e.g. for plugins whose IO changes based on a config), OR leave the
    // Device defaults and rely on the host's registration-time accepts/produces
    // flags (the typical CLAP-via-Adapter case, where accepts/produces are
    // pulled from the descriptor once at load).
};