#include "webview_gui.h"

#include <choc/gui/choc_WebView.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace gasfree {

namespace {

bool ensureGtkInitialized() {
    static std::once_flag flag;
    static bool ok = false;
    std::call_once(flag, []() {
        // CLAP hosts on Linux pass X11 Window handles (even on Wayland via XWayland).
        // Override GDK_BACKEND before gtk_init_check so GTK3 always opens the X11 display.
        // Also disable WebKitGTK DMABUF renderer by default (avoids blank white window on
        // NVIDIA / XWayland when GBM buffer allocation fails).
        setenv("GDK_BACKEND", "x11", 1);
        setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
        gdk_set_allowed_backends("x11");
        ok = gtk_init_check(nullptr, nullptr) != FALSE;
        if (!ok) {
            std::fprintf(stderr, "[GASFREE] gtk_init_check failed (DISPLAY=%s)\n",
                         std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "<null>");
        } else {
            GdkDisplay* dpy = gdk_display_get_default();
            std::fprintf(stderr, "[GASFREE] GTK initialized, is_x11=%d DISPLAY=%s\n",
                         dpy && GDK_IS_X11_DISPLAY(dpy) ? 1 : 0,
                         std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "<null>");
        }
        std::fflush(stderr);
    });
    return ok;
}

class LinuxWebViewGui final : public WebViewGui {
public:
    LinuxWebViewGui(
        std::shared_ptr<ResourceProvider> resources,
        MessageCallback onMessage,
        ReadyCallback onReady)
        : resources_(std::move(resources)),
          onMessage_(std::move(onMessage)),
          onReady_(std::move(onReady)) {}

    ~LinuxWebViewGui() override { destroy(); }

    bool create(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;
        if (!ensureGtkInitialized()) return false;

        choc::ui::WebView::Options opts;
        opts.enableDebugMode = true;
        opts.transparentBackground = false;
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

        // Allow fetch() (patches.json, scales, impulses) and localStorage on our custom URI scheme
        if (auto* wv = WEBKIT_WEB_VIEW(webView_->getViewHandle())) {
            if (auto* ctx = webkit_web_view_get_context(wv)) {
                if (auto* sec = webkit_web_context_get_security_manager(ctx)) {
                    webkit_security_manager_register_uri_scheme_as_secure(sec, "gasfree");
                    webkit_security_manager_register_uri_scheme_as_cors_enabled(sec, "gasfree");
                }
            }
        }

        webView_->bind("gasfreePost", [this](const choc::value::ValueView& args) -> choc::value::Value {
            if (args.size() > 0 && args[0].isString() && onMessage_) {
                onMessage_(std::string(args[0].getString()));
            }
            return choc::value::createInt32(0);
        });

        webView_->bind("gasfreeReady", [this](const choc::value::ValueView&) -> choc::value::Value {
            std::fprintf(stderr, "[GASFREE] JS bridge ready!\n");
            std::fflush(stderr);
            ready_.store(true, std::memory_order_release);
            if (onReady_) onReady_();
            return choc::value::createInt32(1);
        });

        return true;
    }

    bool setParent(void* nativeWindowHandle) override {
        if (!webView_) return false;
        const auto parentXid = static_cast<Window>(reinterpret_cast<uintptr_t>(nativeWindowHandle));
        if (parentXid == 0) return false;

        if (plug_) {
            gtk_widget_destroy(plug_);
            plug_ = nullptr;
        }

        plug_ = gtk_plug_new(parentXid);
        if (!plug_) {
            std::fprintf(stderr, "[GASFREE] gtk_plug_new(%lu) failed\n", static_cast<unsigned long>(parentXid));
            std::fflush(stderr);
            return false;
        }

        GtkWidget* viewWidget = static_cast<GtkWidget*>(webView_->getViewHandle());
        if (!viewWidget) return false;

        gtk_widget_set_size_request(plug_, static_cast<gint>(width_), static_cast<gint>(height_));
        gtk_container_add(GTK_CONTAINER(plug_), viewWidget);
        gtk_widget_show_all(plug_);
        pumpEvents();
        std::fprintf(stderr, "[GASFREE] setParent(%lu) OK (%ux%u)\n",
                     static_cast<unsigned long>(parentXid), width_, height_);
        std::fflush(stderr);
        return true;
    }

    bool setSize(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;
        if (plug_) {
            gtk_widget_set_size_request(plug_, static_cast<gint>(width), static_cast<gint>(height));
            if (GdkWindow* gwin = gtk_widget_get_window(plug_)) {
                gdk_window_resize(gwin, static_cast<gint>(width), static_cast<gint>(height));
            }
            gtk_widget_queue_resize(plug_);
            pumpEvents();
        }
        return true;
    }

    bool show() override {
        if (plug_) {
            gtk_widget_show_all(plug_);
            pumpEvents();
        }
        return true;
    }

    bool hide() override {
        if (plug_) {
            gtk_widget_hide(plug_);
            pumpEvents();
        }
        return true;
    }

    void destroy() override {
        ready_.store(false, std::memory_order_release);
        if (plug_) {
            if (webView_) {
                GtkWidget* viewWidget = static_cast<GtkWidget*>(webView_->getViewHandle());
                if (viewWidget && gtk_widget_get_parent(viewWidget) == plug_) {
                    gtk_container_remove(GTK_CONTAINER(plug_), viewWidget);
                }
            }
            gtk_widget_destroy(plug_);
            plug_ = nullptr;
        }
        webView_.reset();
        pumpEvents();
    }

    void pumpEvents() override {
        if (!ensureGtkInitialized()) return;
        int guard = 0;
        while (gtk_events_pending() && guard++ < 64) {
            gtk_main_iteration_do(FALSE);
        }
    }

    void evaluateJs(const std::string& js) override {
        if (webView_ && ready_.load(std::memory_order_acquire)) {
            webView_->evaluateJavascript(js);
        }
    }

private:
    std::shared_ptr<ResourceProvider> resources_;
    MessageCallback onMessage_;
    ReadyCallback onReady_;
    std::unique_ptr<choc::ui::WebView> webView_;
    GtkWidget* plug_ = nullptr;
    uint32_t width_ = 1280;
    uint32_t height_ = 820;
    std::atomic<bool> ready_{false};
};

} // namespace

std::unique_ptr<WebViewGui> WebViewGui::createPlatformGui(
    std::shared_ptr<ResourceProvider> resources,
    MessageCallback onMessage,
    ReadyCallback onReady) {
    return std::make_unique<LinuxWebViewGui>(
        std::move(resources), std::move(onMessage), std::move(onReady));
}

} // namespace gasfree
