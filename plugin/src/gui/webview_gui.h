#pragma once

#include "resource_provider.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gasfree {

class WebViewGui {
public:
    using MessageCallback = std::function<void(const std::string& jsonMessage)>;
    using ReadyCallback = std::function<void()>;

    virtual ~WebViewGui() = default;

    virtual bool create(uint32_t width, uint32_t height) = 0;
    virtual bool setParent(void* nativeWindowHandle) = 0;
    virtual bool setSize(uint32_t width, uint32_t height) = 0;
    virtual bool show() = 0;
    virtual bool hide() = 0;
    virtual void destroy() = 0;

    // Pump platform GUI events on host main-thread timer (needed for GTK3 on Linux)
    virtual void pumpEvents() {}

    // Evaluate JS inside the WebView (main thread only)
    virtual void evaluateJs(const std::string& js) = 0;

    static std::unique_ptr<WebViewGui> createPlatformGui(
        std::shared_ptr<ResourceProvider> resources,
        MessageCallback onMessage,
        ReadyCallback onReady);
};

} // namespace gasfree
