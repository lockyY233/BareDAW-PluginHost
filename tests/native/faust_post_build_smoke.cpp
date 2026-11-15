#include "bridge/plugin-host/PluginManager.h"
#include "engine/public/AudioEngine.h"

#include <cassert>
#include <iostream>
#include <set>

int main(int argc, char **argv) {
    assert(argc == 2);
    pluginhost::PluginManager plugins(argv[1], 48000.0, 64);
    // Manager must outlive every Device whose vtable/code resides in a plugin.
    AudioEngine engine;
    engine.prepare(48000.0, 64);
    assert(plugins.scanAndLoad() == 5);
    const std::set<std::string> expected = {"post.dcblock", "post.gain", "post.invert", "post.onepole", "post.softclip"};
    const auto registered = plugins.registeredTypes();
    const std::set<std::string> actual(registered.begin(), registered.end());
    assert(actual == expected);
    const auto track = engine.addTrack();
    for (const auto &type : expected)
        assert(engine.addDevice(track, type) != DAWID::InvalidDeviceID);
    std::cout << "faust_post_build_smoke: 5 independent plugins scanned and registered\n";
}
