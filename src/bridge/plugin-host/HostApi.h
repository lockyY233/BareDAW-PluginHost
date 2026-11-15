/*
 * BareDAW plugin host ABI — the function-pointer table every BareDAW plugin
 * receives from baredaw_plugin_init(). extern "C", POD only. Lives at the
 * bridge layer (src/bridge/plugin-host/) so the engine core never sees it.
 *
 * Versioning policy (encoded here for grep-ability — bump on every edit):
 *   MAJOR (high 16 bits of abi_version) — breaking change: any field reorder,
 *     removal, semantic change, or function-pointer removal/reorder. Old
 *     plugins are refused at load with a clear log message.
 *   MINOR (low 16 bits) — additive tail. New fields may be appended; new
 *     function pointers may be appended; existing offsets are stable. Old
 *     plugins keep working as long as they pre-check struct_size against
 *     their build-time expectation.
 *
 * Plugins consume HostApi via the ABI version constants below and pre-check
 * struct_size before reading any field. See docs/Plugin_Host_Plan.md §2, §10.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAREDAW_PLUGIN_ABI_VERSION_MAJOR 1
#define BAREDAW_PLUGIN_ABI_VERSION_MINOR 0
#define BAREDAW_PLUGIN_ABI_VERSION \
    ((BAREDAW_PLUGIN_ABI_VERSION_MAJOR << 16) | BAREDAW_PLUGIN_ABI_VERSION_MINOR)

typedef struct BareDAW_HostApi {
    uint32_t abi_version;          // BAREDAW_PLUGIN_ABI_VERSION at host build time
    uint32_t struct_size;          // sizeof(BareDAW_HostApi) at host build time
    const char *host_name;         // "BareDAW", UTF-8 NUL-terminated, lifetime = host
    const char *host_version;      // "x.y.z" — informational
    uint32_t sample_rate_hz;       // engine sample rate at the time init() runs
    uint32_t max_block_size;       // engine max block size at the time init() runs

    // Opaque host handle. Read by the plugin and passed back as the first
    // argument to every function pointer in this struct. Plugins never
    // dereference it.
    void *host_ctx;

    // -------- registration (control-thread only) --------
    // Plugin calls these from init() (or later, from host_exclusive). type_name
    // must be a stable ASCII identifier unique within the plugin; it matches the
    // DeviceRegistry key (so getAvailableInstruments() lists the plugin).
    //
    // `accepts` and `produces` are 1-bit flags mirroring DeviceIO: 1 = Midi, 2 =
    // Audio. A plugin can register multiple devices by calling this once per
    // type; the second registration of an identical type is logged + rejected.
    //
    // `factory` returns a heap-allocated PluginDevice* (cast to void* per the
    // ABI); host takes ownership and deletes via `destroy_device`.
    int (*register_device)(void *host_ctx,
                           const char *type_name,
                           void *(*factory)(void *host_ctx,
                                            double sample_rate,
                                            uint32_t max_block),
                           void (*destroy_device)(void *host_ctx, void *instance),
                           uint8_t accepts,
                           uint8_t produces);

    // Unregister a device type. Optional — host tolerates the lib being
    // dlclose'd without unregistering.
    int (*unregister_device)(void *host_ctx, const char *type_name);

    // -------- control-thread mutation gate --------
    // Plugin calls this when it wants to apply a batch of changes between
    // audio blocks. The host's implementation applies `fn(plugin_ctx)`
    // synchronously on the thread that called host_exclusive (NOT on the
    // realtime audio thread). fn MUST NOT allocate, lock, or call any
    // non-realtime-safe API. Mirrors the host's runInteractive seam used by
    // the UI.
    void (*host_exclusive)(void *host_ctx,
                           void (*fn)(void *plugin_ctx),
                           void *plugin_ctx);

    // -------- log / error reporting (control-thread only) --------
    // severity: 0 = info, 1 = warn, 2 = error. message is UTF-8 NUL-terminated.
    void (*log)(void *host_ctx, int severity, const char *message);
} BareDAW_HostApi;

#ifdef __cplusplus
}
#endif