// tests/native/plugin_host_smoke.cpp
//
// Headless smoke test for the plugin host. Builds nothing — relies on
// CMake having built tests/fixtures/fake_clap_plugin.cpp as a dylib into
// ${CMAKE_BINARY_DIR}/plugin-fixtures/. Loads that fixture through the
// PluginManager, asserts it appears in the engine's available-instruments
// list, builds a track + adds the device, sends a MIDI note-on, processes a
// block, and asserts the output is non-silent.
//
// Run with:    ./baredaw_plugin_host_smoke <plugins-dir>
// (CMake wires BAREDAW_PLUGIN_FIXTURES_DIR in via argv at test time.)
//
// On any failure: std::fprintf + std::abort. CTest's --output-on-failure
// will surface the message.

#include "bridge/plugin-host/PluginManager.h"
#include "bridge/plugin-host/NativePluginEditor.h"
#include "core/AudioBuffer.h"
#include "core/DeviceRegistry.h"
#include "core/IDTypes.h"
#include "core/MidiBuffer.h"
#include "engine/public/AudioEngine.h"
#include "engine/public/Track.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

bool contains(const std::vector<std::string> &v, const std::string &needle)
{
    return std::find(v.begin(), v.end(), needle) != v.end();
}

}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugins-dir>\n", argv[0]);
        return 2;
    }
    const fs::path pluginsDir = argv[1];
    const bool playInstrumentTransport =
        argc > 2 && std::string(argv[2]) == "--play-instruments";
    if (!fs::exists(pluginsDir)) {
        std::fprintf(stderr, "[plugin_host_smoke] plugins dir missing: %s\n",
                     pluginsDir.string().c_str());
        return 2;
    }

    constexpr double kSampleRate = 44100.0;
    constexpr uint32_t kBlockSize = 128;

    // Capture PluginManager log so failures surface in CI output. The plugin's
    // baredaw_plugin_init / clap_entry::init may print diagnostic info we
    // want to see if something goes wrong.
    pluginhost::PluginManager manager(
        pluginsDir, kSampleRate, kBlockSize,
        [](int severity, const std::string &msg) {
            const char *tag = severity == 2 ? "ERR" : severity == 1 ? "warn" : "info";
            std::fprintf(stderr, "[plugin-host %s] %s\n", tag, msg.c_str());
        });

    // Engine is constructed AFTER the manager — but its destructor runs FIRST
    // (stack reverse order), which is what we want: the engine tears down its
    // ClapPluginDevice instances (calling plugin->destroy / deactivate /
    // stop_processing) while the dylib is still loaded. PluginManager's
    // destructor then dlcloses. Reversing the declaration would crash on
    // shutdown because the dylib would already be unmapped when ClapPlugin-
    // Device::~ClapPluginDevice tries to call into it.
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize);

    const size_t loaded = manager.scanAndLoad();
    std::fprintf(stderr, "[plugin_host_smoke] loaded %zu plugin(s) from %s\n",
                 loaded, pluginsDir.string().c_str());
    if (loaded < 1) {
        std::fprintf(stderr, "[plugin_host_smoke] FAIL: no plugins loaded\n");
        return 1;
    }

    // (1) Manager's own registry matches expectations.
    const auto registered = manager.registeredTypes();
    // When pointed at a real plugin directory, use this executable as a
    // discovery/instantiation smoke test instead of requiring the in-tree
    // TestTone fixture. Every registered type must be constructible through
    // the same AudioEngine::addDevice path the UI uses.
    if (!contains(registered, std::string("com.baredaw.testtone"))) {
        const DAWID::TrackID realTrack = engine.addTrack();
        DAWID::DeviceID sourceDevice = DAWID::InvalidDeviceID;
        for (const auto &type : registered) {
            const bool isAudioEffect = contains(engine.getAvailableAudioEffects(), type);
            const bool isInstrument = contains(engine.getAvailableInstruments(), type);
            if (isAudioEffect && sourceDevice == DAWID::InvalidDeviceID) {
                sourceDevice = engine.addDevice(realTrack, "simplosc");
                engine.setTrackArmed(realTrack, true);
            }
            const DAWID::DeviceID realDevice = engine.addDevice(realTrack, type);
            if (realDevice == DAWID::InvalidDeviceID) {
                std::fprintf(stderr,
                             "[plugin_host_smoke] FAIL: could not instantiate %s\n",
                             type.c_str());
                return 1;
            }
            std::fprintf(stderr, "[plugin_host_smoke] instantiated %s\n",
                         type.c_str());
            std::fprintf(stderr, "[plugin_host_smoke] embedded Cocoa GUI: %s\n",
                         [&] {
                             auto *editor = static_cast<pluginhost::NativePluginEditor *>(
                                 engine.queryDeviceExtension(
                                     realDevice,
                                     pluginhost::kNativePluginEditorExtension));
                             return editor && editor->supports(
                                                  pluginhost::kNativeEditorCocoa);
                         }()
                             ? "supported" : "not supported");
            const auto params = engine.getDeviceParams(realDevice);
            if (!params.empty() &&
                !engine.setDeviceParam(realDevice, params.front().id,
                                       params.front().currentValue)) {
                std::fprintf(stderr,
                             "[plugin_host_smoke] FAIL: parameter event rejected by %s\n",
                             type.c_str());
                return 1;
            }
            if (isInstrument) engine.setTrackArmed(realTrack, true);
            if (isAudioEffect || isInstrument)
                engine.queueMidiInput(0x90, 69, 100, 0);
            // Instruments deliberately remain transport-stopped here. Live
            // monitoring must consume MIDI and render without pressing Play.
            if (isAudioEffect || (isInstrument && playInstrumentTransport)) engine.play();
            for (int block = 0; block < 32; ++block)
                engine.process(static_cast<int>(kBlockSize));
            if (isAudioEffect || (isInstrument && playInstrumentTransport)) engine.stop();
            if (isAudioEffect || isInstrument) {
                float maxAbs = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int frame = 0; frame < engine.getBlockSize(); ++frame)
                        maxAbs = std::max(maxAbs,
                                          std::fabs(engine.getOutputBuffer(ch)[frame]));
                if (maxAbs < 0.001f) {
                    std::fprintf(stderr,
                                 isInstrument
                                     ? "[plugin_host_smoke] FAIL: instrument %s produced silence from MIDI\n"
                                     : "[plugin_host_smoke] FAIL: effect %s produced silence\n",
                                 type.c_str());
                    return 1;
                }
            }
            if (isInstrument && !playInstrumentTransport) {
                engine.queueMidiInput(0x80, 69, 0, 0);
                for (int block = 0; block < 4; ++block)
                    engine.process(static_cast<int>(kBlockSize));
            }
            engine.removeDevice(realDevice);
        }
        if (!manager.unloadAll()) {
            std::fprintf(stderr,
                         "[plugin_host_smoke] FAIL: real plugins remained live after removal\n");
            return 1;
        }
        std::fprintf(stderr, "[plugin_host_smoke] real-plugin scan OK\n");
        return 0;
    }
    // (2) DeviceRegistry sees the new type — engines surface it in their
    //     available-instruments list.
    const auto instruments = engine.getAvailableInstruments();
    if (!contains(instruments, std::string("com.baredaw.testtone"))) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: testtone not in getAvailableInstruments\n");
        return 1;
    }

    // (3) Add the device to a fresh track. Arm the track so queueMidiInput's
    //     queueRawMidiToArmedTracks path actually delivers events to it.
    const DAWID::TrackID trackID = engine.addTrack();
    if (trackID == DAWID::InvalidTrackID) {
        std::fprintf(stderr, "[plugin_host_smoke] FAIL: addTrack returned Invalid\n");
        return 1;
    }
    const DAWID::DeviceID devID = engine.addDevice(trackID, "com.baredaw.testtone");
    if (devID == DAWID::InvalidDeviceID) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: addDevice returned InvalidDeviceID\n");
        return 1;
    }
    engine.setTrackArmed(trackID, true);

    if (!engine.setDeviceParam(devID, "1", 0.25f)) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: CLAP parameter event was rejected\n");
        return 1;
    }
    if (!engine.setDeviceDryWet(devID, 0.5f) ||
        std::fabs(engine.getDeviceDryWet(devID) - 0.5f) > 0.001f) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: host wrapper dry/wet was not applied\n");
        return 1;
    }

    // (4) Drive the engine. queueMidiInput schedules a MIDI byte triple for
    //     delivery N frames in the future; process() advances and routes it
    //     through the device. Audio should be non-silent after a note-on.
    //
    // Do not start transport: a plugin instrument must monitor live MIDI while
    // stopped. This is the regression case for adapters' continuous process
    // requirement (note-on through note-off, with no queued-event burst).
    engine.queueMidiInput(0x90 /*note-on ch 0*/, 69 /*A4*/, 100, 0);

    // Drain a few blocks so the plugin's phase advances through enough of a
    // sine cycle for at least one sample to clear the 0.01 threshold. 16
    // blocks * 128 frames / 44100 Hz ≈ 46 ms — covers a ~20 Hz fundamental
    // comfortably.
    for (int i = 0; i < 16; ++i) {
        engine.process(static_cast<int>(kBlockSize));
    }
    const auto appliedParams = engine.getDeviceParams(devID);
    const auto amplitudeIt = std::find_if(
        appliedParams.begin(), appliedParams.end(),
        [](const InstrumentParameterDescriptor &p) { return p.id == "1"; });
    if (amplitudeIt == appliedParams.end() ||
        std::fabs(amplitudeIt->currentValue - 0.25f) > 0.001f) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: CLAP parameter event was not applied\n");
        return 1;
    }

    // (5) Pull the master output buffer; assert at least one channel has a
    //     sample whose magnitude exceeds a small threshold. The fixture
    //     generates amplitude * sin(...) at the note-on frequency, so any
    //     non-zero amplitude produces non-trivial samples — but the first
    //     sample may sit near phase=0 by chance, so we scan a few blocks.
    // Public API only: getOutputBuffer(channel) + getBlockSize().
    constexpr int kNumChannels = 2; // engine.prepare() with 2 ch in the host
    bool nonSilent = false;
    float maxAbs = 0.0f;
    const int frames = engine.getBlockSize();
    for (int ch = 0; ch < kNumChannels; ++ch) {
        const float *p = engine.getOutputBuffer(ch);
        for (int i = 0; i < frames; ++i) {
            const float mag = std::fabs(p[i]);
            if (mag > maxAbs) maxAbs = mag;
            if (mag > 0.01f) nonSilent = true;
        }
    }
    std::fprintf(stderr, "[plugin_host_smoke] maxAbs across 1 block = %f\n", maxAbs);
    if (!nonSilent) {
        std::fprintf(stderr, "[plugin_host_smoke] FAIL: master output is silent\n");
        return 1;
    }
    engine.queueMidiInput(0x80 /*note-off ch 0*/, 69, 0, 0);
    for (int i = 0; i < 4; ++i)
        engine.process(static_cast<int>(kBlockSize));

    // (6) Tearing down the manager should unregister the type — exercising
    //     PluginManager::unloadAll(). The plugin device is still owned by the
    //     engine's track, so we must remove it first to avoid a dangling
    //     plugin pointer when the dylib unloads (ClapPluginDevice::~Clap-
    //     PluginDevice calls plugin->destroy()).
    if (manager.unloadAll()) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: unloaded library with a live device\n");
        return 1;
    }
    if (manager.registeredTypeCount() == 0) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: refused unload damaged registrations\n");
        return 1;
    }
    engine.removeDevice(devID);
    if (!manager.unloadAll()) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: unload refused after device removal\n");
        return 1;
    }
    if (manager.registeredTypeCount() != 0) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: unloadAll did not clear registrations\n");
        return 1;
    }
    const auto instrumentsAfter = engine.getAvailableInstruments();
    if (contains(instrumentsAfter, std::string("com.baredaw.testtone"))) {
        std::fprintf(stderr,
                     "[plugin_host_smoke] FAIL: testtone still listed after unloadAll\n");
        return 1;
    }

    std::fprintf(stderr, "[plugin_host_smoke] OK\n");
    return 0;
}
