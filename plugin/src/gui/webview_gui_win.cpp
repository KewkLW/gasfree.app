#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "webview_gui.h"

#include <choc/gui/choc_WebView.h>
#include <windows.h>
#include <atomic>

namespace gasfree {

namespace {

class WinWebViewGui final : public WebViewGui {
public:
    WinWebViewGui(
        std::shared_ptr<ResourceProvider> resources,
        MessageCallback onMessage,
        ReadyCallback onReady)
        : resources_(std::move(resources)),
          onMessage_(std::move(onMessage)),
          onReady_(std::move(onReady)) {}

    ~WinWebViewGui() override { destroy(); }

    bool create(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;

        choc::ui::WebView::Options opts;
        opts.enableDebugMode = true;
        opts.customSchemeURI = "https://gasfree.local";
        opts.fetchResource = [res = resources_](const std::string& path)
            -> std::optional<choc::ui::WebView::Options::Resource> {
            auto r = res->fetch(path);
            if (!r) return std::nullopt;
            choc::ui::WebView::Options::Resource out;
            out.data = std::move(r->data);
            out.mimeType = std::move(r->mimeType);
            return out;
        };

        webView_ = std::make_unique<choc::ui::WebView>(opts);
        if (!webView_->loadedOK()) {
            webView_.reset();
            return false;
        }

        webView_->bind("gasfreePost", [this](const choc::value::ValueView& args) -> choc::value::Value {
            if (args.size() > 0 && args[0].isString() && onMessage_)
                onMessage_(std::string(args[0].getString()));
            return choc::value::createInt32(0);
        });

        webView_->bind("gasfreeReady", [this](const choc::value::ValueView&) -> choc::value::Value {
            ready_.store(true, std::memory_order_release);
            if (onReady_) onReady_();
            return choc::value::createInt32(1);
        });

        return true;
    }

    bool setParent(void* nativeWindowHandle) override {
        if (!webView_ || !nativeWindowHandle) return false;
        HWND parent = static_cast<HWND>(nativeWindowHandle);
        HWND child = static_cast<HWND>(webView_->getViewHandle());
        if (!child) return false;
        SetParent(child, parent);
        SetWindowLongPtr(child, GWL_STYLE, (GetWindowLongPtr(child, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
        MoveWindow(child, 0, 0, static_cast<int>(width_), static_cast<int>(height_), TRUE);
        ShowWindow(child, SW_SHOW);
        return true;
    }

    bool setSize(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;
        if (webView_) {
            HWND child = static_cast<HWND>(webView_->getViewHandle());
            MoveWindow(child, 0, 0, static_cast<int>(width_), static_cast<int>(height_), TRUE);
        }
        return true;
    }

    bool show() override {
        if (webView_) ShowWindow(static_cast<HWND>(webView_->getViewHandle()), SW_SHOW);
        return true;
    }

    bool hide() override {
        if (webView_) ShowWindow(static_cast<HWND>(webView_->getViewHandle()), SW_HIDE);
        return true;
    }

    void destroy() override {
        ready_.store(false, std::memory_order_release);
        webView_.reset();
    }

    void evaluateJs(const std::string& js) override {
        if (webView_ && ready_.load(std::memory_order_acquire))
            webView_->evaluateJavascript(js);
    }

private:
    std::shared_ptr<ResourceProvider> resources_;
    MessageCallback onMessage_;
    ReadyCallback onReady_;
    std::unique_ptr<choc::ui::WebView> webView_;
    uint32_t width_ = 1280;
    uint32_t height_ = 820;
    std::atomic<bool> ready_{false};
};

} // namespace

std::unique_ptr<WebViewGui> WebViewGui::createPlatformGui(
    std::shared_ptr<ResourceProvider> resources,
    MessageCallback onMessage,
    ReadyCallback onReady) {
    return std::make_unique<WinWebViewGui>(
        std::move(resources), std::move(onMessage), std::move(onReady));
}

} // namespace gasfree
