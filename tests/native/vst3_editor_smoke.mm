#import <Cocoa/Cocoa.h>

#include "bridge/plugin-host/NativePluginEditor.h"
#include "bridge/plugin-host/PluginManager.h"
#include "engine/public/AudioEngine.h"

#include <cstdio>
#include <filesystem>
#include <string>

static void pumpMainRunLoop(NSTimeInterval seconds)
{
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0.0) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
    }
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            std::fprintf(stderr, "usage: %s <vst3-directory>\n", argv[0]);
            return 2;
        }
        [NSApplication sharedApplication];
        constexpr double sampleRate = 48000.0;
        constexpr uint32_t maxBlock = 512;
        pluginhost::PluginManager manager(std::filesystem::path(argv[1]),
                                          sampleRate, maxBlock);
        AudioEngine engine;
        engine.prepare(sampleRate, maxBlock);
        if (manager.scanAndLoad() == 0 || manager.registeredTypes().empty()) return 1;
        const std::string type = manager.registeredTypes().front();
        const auto track = engine.addTrack();
        const auto device = engine.addDevice(track, type);
        if (device == DAWID::InvalidDeviceID) return 1;

        auto *editor = static_cast<pluginhost::NativePluginEditor *>(
            engine.queryDeviceExtension(device,
                                        pluginhost::kNativePluginEditorExtension));
        if (!editor || !editor->supports(pluginhost::kNativeEditorCocoa)) return 1;
        NSView *parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 800)];
        uint32_t width = 900, height = 600;
        const bool created = editor->create(pluginhost::kNativeEditorCocoa,
                                            parent, width, height);
        std::fprintf(stderr, "[vst3_editor_smoke] %s editor: %ux%u\n",
                     type.c_str(), width, height);
        if (!created || width == 0 || height == 0) {
            [parent release];
            return 1;
        }
        editor->setVisible(true);
        // Native framework editors often defer construction and teardown to
        // timers. Exercise both sides so an abandoned Qt/AppKit callback is a
        // test failure instead of a later crash in the full app.
        pumpMainRunLoop(0.25);
        editor->destroy();
        pumpMainRunLoop(0.25);
        [parent release];
        engine.removeDevice(device);
        if (!manager.unloadAll()) return 1;
        return 0;
    }
}
