#pragma once

#include <cstdint>

namespace pluginhost
{

// Optional bridge-side capability implemented by plugin-backed Devices. The
// reusable engine never includes this header and only exposes a format-neutral
// queryExtension() hook; Cocoa/Win32 and plugin-format details stay here.
inline constexpr const char *kNativePluginEditorExtension =
    "com.baredaw.plugin-host.native-editor/1";
inline constexpr const char *kNativeEditorCocoa = "cocoa";
inline constexpr const char *kNativeEditorWin32 = "win32";

class NativePluginEditor
{
public:
    using ResizeCallback = void (*)(void *context, uint32_t width, uint32_t height);
    virtual ~NativePluginEditor() = default;
    virtual bool supports(const char *platformType) const = 0;
    virtual bool create(const char *platformType, void *parent,
                        uint32_t &width, uint32_t &height) = 0;
    virtual bool setVisible(bool visible) = 0;
    virtual void destroy() = 0;
    virtual void setResizeCallback(void *context, ResizeCallback callback)
    {
        (void)context;
        (void)callback;
    }
    virtual uint64_t processCallCount() const { return 0; }
};

}
