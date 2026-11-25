#include "webview_gui.h"

#import <Cocoa/Cocoa.h>
#include <choc/gui/choc_WebView.h>
#include <atomic>

namespace gasfree {

namespace {

class MacWebViewGui final : public WebViewGui {
public:
    MacWebViewGui(
        std::shared_ptr<ResourceProvider> resources,
        MessageCallback onMessage,
        ReadyCallback onReady)
        : resources_(std::move(resources)),
          onMessage_(std::move(onMessage)),
          onReady_(std::move(onReady)) {}

    ~MacWebViewGui() override { destroy(); }

    bool create(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;

        choc::ui::WebView::Options opts;
        opts.enableDebugMode = true;
        opts.acceptsFirstMouseClick = true;
        opts.customSchemeURI = "gasfree://app.local";
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
        NSView* parent = (__bridge NSView*)nativeWindowHandle;
        NSView* view = (__bridge NSView*)webView_->getViewHandle();
        if (!parent || !view) return false;
        [view setFrame:NSMakeRect(0, 0, width_, height_)];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [parent addSubview:view];
        return true;
    }

    bool setSize(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;
        if (webView_) {
            NSView* view = (__bridge NSView*)webView_->getViewHandle();
            [view setFrame:NSMakeRect(0, 0, width_, height_)];
        }
        return true;
    }

    bool show() override {
        if (webView_) [(__bridge NSView*)webView_->getViewHandle() setHidden:NO];
        return true;
    }

    bool hide() override {
        if (webView_) [(__bridge NSView*)webView_->getViewHandle() setHidden:YES];
        return true;
    }

    void destroy() override {
        ready_.store(false, std::memory_order_release);
        if (webView_) {
            [(__bridge NSView*)webView_->getViewHandle() removeFromSuperview];
            webView_.reset();
        }
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
    return std::make_unique<MacWebViewGui>(
        std::move(resources), std::move(onMessage), std::move(onReady));
}

} // namespace gasfree
