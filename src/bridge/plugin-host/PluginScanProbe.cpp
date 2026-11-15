#include "bridge/plugin-host/PluginManager.h"
#include "engine/public/AudioEngine.h"

#include <cstdint>
#include <filesystem>
#include <string>

// One-bundle crash/hang boundary used by the native app's scanner. The parent
// process enforces a timeout; a vendor crash only kills this helper. Success
// means every advertised class can be instantiated and destroyed through the
// same bridge path used by BareDAW.
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    constexpr double sampleRate = 48000.0;
    constexpr uint32_t maxBlock = 512;
    pluginhost::PluginManager manager(std::filesystem::path(argv[1]),
                                      sampleRate, maxBlock);
    AudioEngine engine;
    engine.prepare(sampleRate, static_cast<int>(maxBlock));
    if (manager.scanAndLoad() == 0 || manager.registeredTypes().empty()) return 1;
    const auto track = engine.addTrack();
    for (const std::string &type : manager.registeredTypes()) {
        const auto device = engine.addDevice(track, type);
        if (device == DAWID::InvalidDeviceID) return 1;
        // Exercise a couple of stopped-transport blocks too. Plugin editors
        // commonly report "DAW suspended" when hosts instantiate successfully
        // but fail to service idle processing.
        engine.process(static_cast<int>(maxBlock));
        engine.process(static_cast<int>(maxBlock));
        if (!engine.removeDevice(device)) return 1;
    }
    return manager.unloadAll() ? 0 : 1;
}
