/*
 * BareDAW plugin descriptor — the POD the plugin populates and hands back from
 * baredaw_plugin_init(). extern "C" — never put C++ types in here. Lifetime:
 * the descriptor's strings are owned by the plugin shared library and stay
 * valid until dlclose (the host copies out what it needs into its own
 * registry, so this is a "do not free" contract, not a "must outlive
 * unload" contract).
 *
 * See docs/Plugin_Host_Plan.md §3.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BareDAW_PluginDescriptor {
    uint32_t abi_version;          // must match BAREDAW_PLUGIN_ABI_VERSION major
    uint32_t struct_size;          // sizeof(BareDAW_PluginDescriptor) at plugin build time
    const char *id;                // reverse-DNS, stable across versions ("com.acme.saw")
    const char *name;              // human-facing
    const char *vendor;
    const char *version;           // plugin's own version string (informational)
    uint8_t  kind;                // 0 = instrument, 1 = effect, 2 = analyzer (future)
    uint8_t  accepts_audio;       // channel count expected at audio in (0 if N/A)
    uint8_t  produces_audio;      // channel count produced at audio out
    uint8_t  accepts_midi;        // 0/1
    uint8_t  produces_midi;       // 0/1
    const char *features;         // optional, comma-separated tags ("stereo,saturator")
} BareDAW_PluginDescriptor;

#ifdef __cplusplus
}
#endif