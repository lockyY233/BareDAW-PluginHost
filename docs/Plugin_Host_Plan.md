# Plugin Host — Planning Document

Branch: `feat/plugin-host` (cut from `main`, no other work yet).
Scope: **plugin host only.** The scripting / extension system sketched in
`docs/Modding_Extension_Notes.md` is a separate milestone and is deliberately
not planned here.

> Implementation status (November 2026): CLAP and VST3 adapters now live in
> the optional `baredaw_plugin_host` bridge target. VST3 is independently
> controlled by `BAREDAW_ENABLE_VST3` and is OFF by default; the official SDK
> is fetched only when that flag and `BAREDAW_ENABLE_PLUGINS` are both ON.
> Native editor hosting is exposed through the bridge-owned
> `NativePluginEditor` extension rather than UI or SDK methods in the core.
> The current VST3 MVP supports the main mono/stereo audio bus, MIDI note input,
> parameters, and native editors. State chunks, sidechains/multiple buses,
> latency compensation, scan sandboxing, and plugin MIDI output remain later
> work. This document retains the original phase plan below as design history.

This document is the design; no code lands until it is reviewed.

---

## 0. Recap of fixed decisions (already agreed)

- **Format priority**: CLAP first → VST3 second → AU deferred.
- **Architecture**: C ABI plugin model. `dlopen` / `LoadLibrary`; plugin exports
  a single entry point, host hands the plugin a versioned `HostApi` struct of
  function pointers. Plugin never sees raw engine pointers.
- **Realtime contract**: plugin lifecycle (init / shutdown / param descriptors /
  UI) runs on the control thread only. Audio-thread DSP, if any, is invoked by
  the engine through a `Device::process()` wrapper — the only path from the
  realtime thread into plugin code.
- **Discovery**: scan a plugins directory, validate ABI version, refuse
  incompatible plugins with a clear error.
- **Headless-first**: works with zero UI (the ImGui app, when present, gets a
  device-picker populated from discovered plugins; headless / tests don't care).

---

## 1. Where the code lives

The plugin host sits at the **bridge layer** (`src/bridge/`), parallel to the
audio backends, the direct/web adapters and the emscripten bindings. It is not
part of `baredaw_core`, not part of `src/ui`, not part of `src/engine`. Rationale:

- The plugin host touches the OS (dlopen / file scan / future sandbox) — that
  is the bridge layer's job.
- The plugins it loads are themselves shared libraries; the host is what makes
  them into something the engine can use. That translation is a bridge concern.
- Keeping it out of `baredaw_core` is what makes `BAREDAW_ENABLE_PLUGINS=OFF`
  truly free at link time (see §5).

```
src/bridge/plugin-host/
    HostApi.h           # the C ABI plugins see (function-pointer table + version)
    PluginDescriptor.h  # POD the plugin returns from init()
    PluginManager.h     # C++ façade: scan, load, unload, list
    PluginManager.cpp
    PluginDevice.h      # Device subclass that wraps one plugin instance
    PluginDevice.cpp
    loader/
        PosixDlopen.cpp    # dlopen-based loader; one TU per OS family
        Win32LoadLibrary.cpp
    clap/
        ClapAdapter.h      # implements BareDAW's plugin ABI for a CLAP plugin
        ClapAdapter.cpp    # the smallest CLAP subset (see §7)
```

The CLAP adapter is one folder; the VST3 adapter (future) will be a parallel
folder. Both speak the same internal BareDAW ABI to `PluginManager` /
`PluginDevice`.

`src/bridge/plugin-host/` is NOT included by anything in `src/ui/`. The UI talks
to `EngineClient` (or its `EngineState` snapshot); plugin discovery surfaces as
extra entries in `EngineClient::getAvailableInstruments()` /
`getAvailableAudioEffects()`, which already exist as POD lists.

---

## 2. `HostApi` v1 — the function-pointer table

The single struct every plugin receives from `init()`. Versioned from day one.

```cpp
// src/bridge/plugin-host/HostApi.h  — extern "C", no C++ types
typedef struct BareDAW_HostApi {
    uint32_t abi_version;          // BAREDAW_PLUGIN_ABI_VERSION (currently 1)
    uint32_t struct_size;          // sizeof(BareDAW_HostApi), for forward-compat probes
    const char *host_name;         // "BareDAW" (UTF-8, NUL-terminated, lifetime = host)
    const char *host_version;      // "x.y.z" (informational)
    uint32_t sample_rate_hz;
    uint32_t max_block_size;

    // -------- registration (control-thread only) --------
    // Plugin calls these from init() / shutdown(). `type_name` must be a stable
    // ASCII identifier unique within the plugin; matches the DeviceRegistry key
    // (so "getAvailableInstruments()" lists the plugin). `accepts` / `produces`
    // mirror DeviceIO (1=Midi, 2=Audio). Factory returns a heap-allocated
    // PluginDevice*; host takes ownership and deletes via the destroy_device
    // pointer below.
    int  (*register_device)(void *host_ctx, const char *type_name,
                            void *(*factory)(void *host_ctx, double sample_rate,
                                             uint32_t max_block),
                            void  (*destroy_device)(void *host_ctx, void *instance),
                            uint8_t accepts, uint8_t produces);
    int  (*unregister_device)(void *host_ctx, const char *type_name);

    // -------- control-thread mutation gate (mirrors the host's runExclusive) --------
    // Plugin calls into this when it wants to apply a batch of changes between
    // audio blocks. The host's implementation runs fn on the audio thread
    // between blocks (no silence gap), then returns. fn MUST NOT allocate,
    // lock, or call any non-realtime-safe API. Mirrors SaveFileEditorUI's
    // runInteractive seam in src/app/main.mm:91-103.
    void (*host_exclusive)(void *host_ctx, void (*fn)(void *plugin_ctx), void *plugin_ctx);

    // -------- log / error reporting (control-thread only) --------
    void (*log)(void *host_ctx, int severity, const char *message); // 0=info,1=warn,2=err
} BareDAW_HostApi;
```

**What v1 deliberately omits** (each is a future feature, not a v1 need):

- **Asset / sample registration.** Phase 1 doesn't need plugins to register
  custom sample data with the host. CLAP's `clap_plugin_state` save/load is
  deferred; project save stays plugin-instance-internal.
- **GUI.** No GUI extension requests. The host's UI handles param editing via
  `InstrumentParameterDescriptor` (already in `Device.h`) and `setParameterValue`
  (already on `Device`). Plugins that want a custom GUI cannot, in v1.
- **Capability queries** (does the host support timer, file access, etc.).
  Capability advertising can wait until there is a consumer.
- **Async / timer / file IO host-side helpers.** Plugins get none; if they need
  a file, they load it in their own init() on the control thread.
- **MIDI in / MIDI out port enumeration beyond a single global port.** The
  plugin's device gets MIDI in via the existing `Device::processMidi()` path
  (already used by SimplOSC / RackDevice).

**Versioning rules** (encoded as a comment block on `HostApi`):

- `MAJOR` (high 16 bits of `abi_version`) = breaking change: any field reorder,
  removal, semantic change. Old plugins refused at load.
- `MINOR` (low 16 bits) = additive tail. New fields may be appended; existing
  field offsets are stable. Old plugins continue to work as long as they
  pre-check `struct_size` against what they were built against.
- `host_version` string is informational only (logged on plugin load).

`BAREDAW_PLUGIN_ABI_VERSION` starts at `0x00010000` (major 1, minor 0).

---

## 3. `PluginDescriptor` — what `init()` returns

A POD the plugin hands back to the host. The host uses it for discovery UI
("name", "vendor") and for sanity-checking on load.

```cpp
// PluginDescriptor.h — extern "C"
typedef struct BareDAW_PluginDescriptor {
    uint32_t abi_version;          // must match BAREDAW_PLUGIN_ABI_VERSION major
    uint32_t struct_size;
    const char *id;                // reverse-DNS, stable across versions ("com.acme.saw")
    const char *name;              // human-facing
    const char *vendor;
    const char *version;           // plugin's own version string
    uint8_t  kind;                 // 0=instrument, 1=effect, 2=analyzer (future)
    uint8_t  accepts_audio;        // channel count expected at audio in (0 if N/A)
    uint8_t  produces_audio;       // channel count produced at audio out
    uint8_t  accepts_midi;         // 0/1
    uint8_t  produces_midi;        // 0/1
    const char *features;          // optional, comma-separated tags ("stereo,saturator")
} BareDAW_PluginDescriptor;
```

All strings are UTF-8, lifetime = plugin-shared-library (not freed by host).

---

## 4. Lifecycle: `init` / `shutdown`

A plugin shared library exports **exactly one** C symbol:

```cpp
extern "C" int baredaw_plugin_init(const BareDAW_HostApi *host,
                                   BareDAW_PluginDescriptor *out_descriptor);
extern "C" void baredaw_plugin_shutdown(void); // optional; host tolerates its absence
```

`init` returns 0 on success, non-zero on failure. On success, the descriptor is
populated and the plugin's calls into `host->register_device(...)` have added
one or more device types to the engine's `DeviceRegistry`. From this point, the
plugin is a regular citizen of the engine — same `addDevice(track, type)` /
`setDeviceParam` / `getDeviceParams` flows as a built-in device.

`shutdown` is called when the host tears down (process exit / plugin disabled
at runtime). It may call `host->unregister_device(...)` for every type it
registered, but the host also force-cleans on dlclose — plugins don't have to
be tidy.

**What happens when the engine wants to instantiate one**: `EngineClient::addDevice(track, "com.acme.saw")` →
`DeviceRegistry::create("com.acme.saw")` → plugin's `factory()` is invoked on
the control thread → returns a heap-allocated `PluginDevice*` (a `Device`
subclass defined inside the plugin). The host's `DeviceRegistry::create`
already wraps the factory; no new seam needed.

**On the realtime contract**: `factory()` runs on the control thread (inside
`runExclusive`). `PluginDevice::process()` runs on the audio thread, but it is
the engine's audio thread — same as every other built-in device — and the
plugin author is responsible for real-time safety inside it (no alloc, no
locks, no syscalls). This is the same deal Faust / hand-written `Device`s
already get.

---

## 5. Discovery + load

`PluginManager` is a C++ class owned by the host (`src/app/main.mm` on macOS,
`src/app/main_win32.cpp` on Windows). Construction arguments:

- `plugins_dir` (string): where to look. Resolution order:
  1. `BAREDAW_PLUGINS_DIR` env var (developer workflow).
  2. `~/Library/Audio/Plug-Ins/BareDAW/` on macOS, `%APPDATA%/BareDAW/plugins/`
     on Windows (per-OS conventions, OS-specific TU).
  3. Empty → no plugin discovery happens at all; the engine starts plain.
- A vector of `std::filesystem::path` (future: comma-separated `BAREDAW_PLUGIN_PATHS`).

For each candidate file in `plugins_dir`:

1. `dlopen(path, RTLD_LOCAL | RTLD_NOW)` (or `LoadLibrary` on Windows). If it
   fails to load at all, log and skip — never crash the host.
2. `dlsym(RTLD_DEFAULT, "baredaw_plugin_init")`. If missing → not a BareDAW
   plugin (or a CLAP-only plugin), skip silently. (CLAP plugins are *not*
   picked up by the bare dlopen scan — the CLAP adapter discovers them via
   the CLAP entry-point convention. See §7.)
3. Construct a v1 `HostApi`, call `init(&host, &desc)`. Check:
   - `desc.abi_version >> 16 == 1` (major match). Mismatch → log, dlclose,
     continue.
   - `desc.struct_size >= sizeof(BareDAW_PluginDescriptor)` (size probe).
4. On success, store `(dl_handle, descriptor)` in the manager's loaded-plugins
   list. The descriptor is used to populate `getAvailableInstruments()` /
   `getAvailableAudioEffects()`.

**Load failure modes and how they surface** (the user explicitly asked about
this):

| Failure | Behaviour |
|---|---|
| Library won't dlopen | Log `severity=warn` ("could not load <path>: <dlerror>"). Skip silently. |
| Symbol `baredaw_plugin_init` missing | Skip silently (not our plugin). |
| `init` returns non-zero | Log `severity=err` ("<name>: init failed (rc=N)"). Skip. |
| ABI major mismatch | Log `severity=err` ("<name>: built for abi MAJOR.X, host is MAJOR.Y"). Skip. |
| `init` succeeds but registers zero devices | Log `severity=warn` ("<name>: registered no devices"). Keep loaded (caller may inspect descriptor). |
| Two plugins register the same type name | Log `severity=err` ("<name>: type '<type>' already registered by <other>"). Second registration is rejected; first wins. Deterministic tie-break by scan order. |

Logging goes through the `HostApi::log` callback once the plugin is loaded,
and through the host's own stderr/file logger during load itself (so a
broken plugin can still be diagnosed).

A future Phase 2 may add a `--list-plugins` CLI command and a JSON catalog;
out of scope for Phase 1.

---

## 6. CMake integration

`BAREDAW_ENABLE_PLUGINS` (cache BOOL, **default `OFF`**).

```
option(BAREDAW_ENABLE_PLUGINS "Enable plugin host (CLAP, future VST3)" OFF)
```

A new static library is added only when the option is on:

```cmake
if (BAREDAW_ENABLE_PLUGINS AND NOT EMSCRIPTEN)
    add_library(baredaw_plugin_host STATIC
        src/bridge/plugin-host/PluginManager.cpp
        src/bridge/plugin-host/PluginDevice.cpp
        src/bridge/plugin-host/clap/ClapAdapter.cpp
        # platform loader picked per-OS:
        $<$<PLATFORM_ID:Windows>:src/bridge/plugin-host/loader/Win32LoadLibrary.cpp>
        $<$<NOT:$<PLATFORM_ID:Windows>>:src/bridge/plugin-host/loader/PosixDlopen.cpp>
    )
    target_include_directories(baredaw_plugin_host PUBLIC src)
    target_compile_definitions(baredaw_plugin_host
        PUBLIC BAREDAW_PLUGIN_ABI_VERSION=0x00010000)
endif()
```

`baredaw_core` does **not** depend on `baredaw_plugin_host`, never includes
any header from `src/bridge/plugin-host/`, and contains zero plugin-related
symbols. With the option off, `baredaw_plugin_host` simply does not exist as a
target — verified by `nm build-app/baredaw_app | grep -i plugin` returning
empty, and by `BAREDAW_CORE_SOURCES` not having changed.

The host links `baredaw_plugin_host` privately:

```cmake
if (BAREDAW_ENABLE_PLUGINS AND BAREDAW_BUILD_APP AND NOT EMSCRIPTEN)
    target_link_libraries(baredaw_app PRIVATE baredaw_plugin_host)
endif()
```

Default per build profile:

| Profile | Default |
|---|---|
| `BAREDAW_BUILD_TESTS` ON | `BAREDAW_ENABLE_PLUGINS=OFF` (tests link core only). |
| `BAREDAW_BUILD_APP` ON (macOS/Windows) | `BAREDAW_ENABLE_PLUGINS=OFF`. Developer opts in. |
| Emscripten (any profile) | `BAREDAW_ENABLE_PLUGINS=OFF` (enforced; see §7). |

"Zero cost when OFF" is enforced by:

1. CMake guard at the top of the option block — when off, no `baredaw_plugin_host`
   target, no plugin source compiled, no plugin object linked.
2. The `DeviceRegistry` interface stays plugin-aware only via the existing
   `(type, factory, accepts, produces)` entry point — no plugin-specific
   types leak into the core.
3. A grep-based test (similar in spirit to `tests/ui_api_guard.sh`) that
   scans `src/core/`, `src/engine/`, `src/io/` for any include of a
   `bridge/plugin-host` path. Should always be empty.

---

## 7. Web/WASM posture

**Native-only for v1.** The emscripten build never sees the plugin host:

- The CMake option is gated `AND NOT EMSCRIPTEN`. The plugin host target is
  not created in WASM builds at all.
- `baredaw_bindings` and `baredaw_wasm` link only `baredaw_core`. No symbol
  from `baredaw_plugin_host` ever reaches the wasm artifact.

Rationale: dynamic loading of native plugin binaries inside a WebAssembly
sandbox is a non-trivial problem (WASI preview2 / component-model work,
sandboxed FS, capability negotiation). It is not free, and no current web
target users have asked for it. Defer.

If/when a web plugin story is wanted, it will be a separate milestone —
likely a WASM-of-CLAP compiled to wasm per-plugin (clap has been exploring
this), with its own ABI shim that does NOT leak into the native plugin host.

---

## 8. Minimum viable CLAP adapter

CLAP is a C ABI with several `clap_*_t` extension structs the plugin
implements. We wrap a strict subset that maps cleanly to BareDAW's
`Device`/`HostApi` model.

**What we wrap** (subset for Phase 1):

| CLAP surface | Maps to |
|---|---|
| `clap_plugin_entry`, `clap_plugin_factory`, `clap_plugin_descriptor` | Discovered via `clap_entry` symbol → `clap_plugin_entry.get_factory` → `clap_plugin_factory.create_plugin`. The host iterates factory IDs matching its required extension set. |
| `clap_plugin_t.init` / `destroy` | BareDAW `factory()` / `destroy_device()`. |
| `clap_plugin_t.activate` / `deactivate` | Maps to BareDAW `Device::prepare()` (also re-called on `setSampleRate`). |
| `clap_plugin_t.process` | BareDAW `PluginDevice::process()` — invoked from the engine audio thread. |
| `clap_plugin_audio_ports_t` | Used once at load to declare audio in / out channel counts; populated into `PluginDescriptor::accepts_audio / produces_audio`. |
| `clap_plugin_params_t.count / get_info / get_value / value_to_text / text_to_value` | Maps to BareDAW `InstrumentParameterDescriptor` + `setParameterValue` / `getParameterValue`. |
| `clap_plugin_note_ports_t` | Used once at load; CLAP's "MIDI in, no MIDI out" → `accepts_midi=1, produces_midi=0`. |
| `clap_plugin_state_t.save / load` | **DEFERRED**. Phase 1 has no project-state integration for plugins; the plugin's own state is uninitialised at load. Documented as a known gap. |

**What we deliberately do NOT wrap** in Phase 1 (and tell users):

- GUI (`clap_plugin_gui_t`, `clap_plugin_timer_support_t`)
- File IO / asset paths (`clap_plugin_resources_t`)
- Thread-pool / async (`clap_plugin_thread_pool_t`, `clap_plugin_thread_check_t`)
- Voice info (`clap_plugin_voice_info_t`) — assumed mono-voice / direct
- Context-menu / extensions
- Track info (`clap_plugin_track_info_t`) — engine will set fixed context
- State save / load (above)
- Latency reporting (`clap_plugin_latency_t`) — treated as zero latency

A plugin that requires any of these at load (`plugin->init(plugin, ...)`)
returns a non-zero error → log → skip. The skip is logged with the plugin
name and the missing capability ("<name>: requires GUI; v1 host has no GUI
support"), so a user can diagnose.

**Vendoring decision**: vendor the upstream single-file CLAP header
(`clap/include/clap/clap.h` from a tagged release of `free-audio/clap`) under
`third_party/clap/clap.h`, and the minimum number of companion headers
(`clap.h` is self-contained per upstream's design — `clap/helpers/...` is a
separate convenience library we do NOT vendor). The adapter `#include`s the
upstream header verbatim. Total vendor footprint: one ~3K-line header. This
gives us:

- The actual `clap_*_t` ABI for free, with zero translation risk.
- A clear upgrade path (`git -C third_party/clap pull <tag>` when upstream
  ships a new minor).
- No `clap-helpers` (which pulls in RtAudio, etc. — wrong for a host).
- No hand-rolled mirror that drifts.

**Why "one real plugin end-to-end" is reachable**: a CLAP instrument that
declares `audio-ports: 0 in / 2 out, note-ports: 1 MIDI in, params: N floats`,
needs no GUI, no state save, no latency report, no thread-pool → loads cleanly
through the subset above. Plenty of free CLAP synths fit this profile.

---

## 9. Out of scope for this milestone

Stated explicitly so we don't drift:

- VST3 adapter (mirrors the clap/ folder layout, future milestone).
- AU adapter (macOS-only, pull-style — different paradigm; future).
- Scripting / Lua / WASM-extension runtime (covered by
  `docs/Modding_Extension_Notes.md` — separate milestone).
- Hot-reload of plugins (loading a new plugin at runtime is in; swapping a
  loaded one mid-session is out).
- Sandboxing (no capability gating, no seccomp, no entitlement checks — we
  trust the user-installed plugin like every other DAW does).
- Plugin GUI / window embedding (no `clap_plugin_gui_t`).
- Plugin state save/load into `.baredaw` projects (Phase 1 only persists
  which device types are used and their param values; plugin's internal
  state is lost across reload — documented as known limitation).
- Embedded WASM-of-plugin targets (web milestone).

---

## 10. Answers to the open questions

**Q1. Is `DeviceRegistry::Factory` sufficient, or do we need a richer
descriptor?**

Sufficient as the **registration primitive**, not as the **load-time
descriptor**. The factory is `(type-name, factory-fn, accepts, produces)` —
that is exactly the runtime-create shape and we keep using it
unchanged. The **load-time** shape needs more, and that's what
`BareDAW_PluginDescriptor` (§3) is for: channel counts, kind
(instrument/effect), id, vendor, version. The plugin's `init()` fills both —
`register_device(...)` calls add factory entries to the live
`DeviceRegistry`, while the descriptor feeds the static
`getAvailableInstruments()` / `getAvailableAudioEffects()` lists and any
future "About this plugin" UI.

**Q2. Where does `runExclusive` factor in?**

Two layers:

- The host's existing `runExclusive` lambda (already in `SaveFileEditorUI`'s
  `HostServices`, already implemented in `src/app/main.mm:91-103` as
  `runInteractive`) wraps **control-thread-from-control-thread** UI
  mutations. The plugin manager is just another control-thread caller of
  it — for instance, a future "load plugin X on track Y" UI action would
  call `runExclusive([&] { plugin_manager.addDevice(track, "type"); })`.
  The plugin manager itself does not need to know about `runExclusive`; it
  just needs to be reentrant-from-control-thread-safe.
- The `HostApi::host_exclusive` callback (§2) lets **plugin code** apply
  multi-call mutations between audio blocks. This mirrors
  `runExclusive` but on the plugin side — the plugin calls
  `host->host_exclusive(ctx, &my_batch_fn, my_ctx)`, and the host applies it
  in the same audio-thread-between-blocks window as the UI's
  `runInteractive`. Same semantics, opposite direction across the FFI
  boundary. Critical for plugins that need to atomically apply
  "change preset + reset voices" without glitches.

**Q3. CLAP vendoring?**

Vendoring strategy: **single upstream header from
`free-audio/clap`** under `third_party/clap/`. Wrap with ~50 lines of
`ClapAdapter.cpp` that speaks CLAP on one side and BareDAW's
`HostApi`/`PluginDevice` on the other. **Do not vendor `clap-helpers`**
(it pulls in RtAudio and a lot of opinion). Trade-offs vs alternatives:

| Option | Pros | Cons |
|---|---|---|
| **Vendor single upstream `clap.h`** ✅ | Smallest vendor footprint; upstream is the spec; trivial upgrades via tag bump | Manual wrapper for the subset we use |
| Wrap `clap-helpers` | Less hand-written C | Pulls in RtAudio + helpers; heavier than necessary |
| Our own minimal `clap.h` mirror | Zero external surface | Drifts from upstream; re-implementing the ABI = bugs |

**Q4. ABI versioning?**

Monotonic `uint32_t abi_version`, encoded `MAJOR(16) | MINOR(16)`, with a
human-facing string kept alongside for display. Policy:

- **Major bump** (= breaking change): any struct field reorder, removal,
  semantic change, function-pointer removal/reorder. Old plugins refused at
  load with a clear log message.
- **Minor bump** (= additive): new fields appended at the tail, new
  function pointers added at the tail. Old plugins keep working as long as
  they pre-check `struct_size` against their build-time expectation.
- Old plugins are **not silently supported for years**; we promise one
  major-version coexistence window — when major N+1 ships, plugins built
  for major N keep loading until the next major after that, then they
  fail loudly. The exact window is a project-policy decision; one year /
  two majors is a reasonable starting point. The fail-loud policy is the
  important part.

---

## 11. Phase 1 — the smallest end-to-end slice

**Goal**: one free CLAP instrument, loaded as a BareDAW device, plays MIDI
through the engine. Headless smoke test only — no UI changes required.

### Deliverables

1. **CMake wiring**
   - `option(BAREDAW_ENABLE_PLUGINS "Enable plugin host" OFF)`.
   - `baredaw_plugin_host` static library built when on.
   - Default-off for tests and the native app.

2. **ABI headers** (`src/bridge/plugin-host/HostApi.h`,
   `PluginDescriptor.h`)
   - POD structs, extern "C", no C++ types crossing the boundary.
   - `BAREDAW_PLUGIN_ABI_VERSION = 0x00010000`.
   - Header comment block encoding the MAJOR/MINOR policy (§10).

3. **Loader** (`src/bridge/plugin-host/loader/PosixDlopen.cpp`)
   - `dlopen` + `dlsym("baredaw_plugin_init")` + `init` + descriptor
     capture + error reporting.
   - `Win32LoadLibrary.cpp` mirrors it for Windows (stub OK in Phase 1 if
     the dev loop is macOS-only; CI on Windows comes in the same PR or
     the next).

4. **`PluginManager`**
   - Takes `plugins_dir` + a logger.
   - `scanAndLoad()` populates the loaded list; logs each
     success/skip/reject.
   - `unloadAll()` for shutdown.
   - `availableTypes(kind)` returns strings for
     `EngineClient::getAvailableInstruments()` /
     `getAvailableAudioEffects()`. Wired in `main.mm` /
     `main_win32.cpp` by prepending the manager's lists to the existing
     `DeviceRegistry::listTypes()` results, so the UI's existing
     `addInstrumentMenu(...)` shows plugin devices with zero UI changes.

5. **`PluginDevice`** (`PluginDevice.h/.cpp`)
   - Subclass of `Device` (already exists at `src/core/Device.h`).
   - Holds a `(host_ctx, factory, instance)` triple from the plugin's
     `factory()`.
   - Forwards `accepts()`, `produces()`, `prepare()`,
     `process()`, `processMidi()`, `getParameterDescriptors()`,
     `setParameterValue()`, `getParameterValue()` to the plugin.
   - One `PluginDevice` instance = one plugin instance, mirroring the
     built-in devices.

6. **CLAP adapter** (`src/bridge/plugin-host/clap/ClapAdapter.cpp`)
   - Implements BareDAW's `baredaw_plugin_init` for any plugin shared
     library that exposes a CLAP entry point.
   - Subsets CLAP to the table in §8.
   - Logs every skipped extension ("GUI requested but not supported").

7. **CLAP single header vendored**
   - `third_party/clap/clap.h` from a tagged upstream release
     (e.g., `free-audio/clap` 1.2.x).
   - LICENSE file alongside; attributed in `third_party/README.md`.

8. **Headless smoke test** (`tests/native/plugin_host_smoke.cpp`)
   - Registers a ctest entry `baredaw_plugin_host_smoke`.
   - Builds a *fake* CLAP-style plugin as a tiny shared library (in-tree,
     under `tests/fixtures/`) that exposes:
     - `clap_entry` symbol
     - a "TestTone" descriptor (0 audio in, 2 audio out, MIDI in, 1 param
       "amplitude")
     - `process()` that emits `amplitude * sin(2π·440·t)` on both channels
   - Sets `BAREDAW_PLUGINS_DIR` to the fixture's directory at test time.
   - Spins up an `AudioEngine`, runs the plugin manager, asserts the
     plugin type appears in `getAvailableInstruments()`, creates a track,
     calls `addDevice(track, "com.baredaw.testtone")`, feeds a MIDI note,
     drives a few `process()` blocks, asserts the output buffer contains
     non-zero samples.
   - Verifies log-on-skip for a second fixture that requires GUI support.
   - Run only when `BAREDAW_ENABLE_PLUGINS=ON`; otherwise `ctest` skips it
     (or the test target isn't built at all — both are acceptable;
     skipping is preferable).

9. **Real-plugin smoke** (manual or scripted, not a ctest entry)
   - README section in the PR description:
     - `cmake -B build-plugins -DBAREDAW_ENABLE_PLUGINS=ON -DBAREDAW_BUILD_TESTS=ON`
     - `cmake --build build-plugins`
     - `BAREDAW_PLUGINS_DIR=~/.local/share/clap ./build-plugins/tests/plugin_host_smoke_real_plugin`
   - The smoke loads one of: Surge XT (CLAP), Vital (free), Dexed
     (CLAP build), or another small free CLAP synth that needs only the
     §8 subset.
   - Pass criterion: the smoke renders non-silent output for one MIDI
     note. We don't assert anything about the plugin's own sound — just
     that the host's pipeline produced audio.

10. **Grep guard** (`tests/plugin_host_isolation.sh`)
    - Asserts no file under `src/core/`, `src/engine/`, `src/io/`
      includes anything from `src/bridge/plugin-host/`. Fails the test if
      a future change accidentally leaks plugin types into core.
    - Asserts `baredaw_core` link table contains no `baredaw_plugin_*`
      symbols when the option is off.

11. **Docs**
    - `docs/Plugin_Host_Plan.md` (this file) lands alongside the PR.
    - `README.md` plugin-host section: opt-in flag, `plugins_dir`
      resolution, known Phase 1 limitations (no GUI, no state save, no
      latency report).

### Acceptance

Phase 1 is "done" when:

- `cmake -B build -DBAREDAW_ENABLE_PLUGINS=OFF && cmake --build build`
  succeeds and the produced binary contains zero symbols matching
  `baredaw_plugin_*` / `clap_*`.
- `cmake -B build -DBAREDAW_ENABLE_PLUGINS=ON -DBAREDAW_BUILD_TESTS=ON`
  succeeds; `ctest` runs the plugin-host smoke green; the manual
  real-plugin smoke produces audio for one MIDI note.
- The grep guard passes in both configurations.
- No header from `src/bridge/plugin-host/` is reachable from
  `src/core/` / `src/engine/` / `src/io/` / `src/ui/`.

### Out of Phase 1 (deferred to later milestones)

State save/load, GUI embedding, VST3 adapter, AU adapter, hot-reload,
sandboxing, web/WASM plugins, latency reporting, voice-info,
thread-pool, asset path registration, timer support, track-info,
context-menu extensions.
