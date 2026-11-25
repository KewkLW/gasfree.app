// macOS GUI smoke test: loads GASFREE.clap like a host, opens its Cocoa GUI in a
// real NSWindow, runs audio on a background thread, clicks Subharm PLAY with an
// in-process mouse event, and snapshots the WKWebView to PNGs (no screen-recording
// permission needed). Must run inside a logged-in GUI session.
//
//   test_gui_mac <path/to/GASFREE.clap> <out_dir>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <clap/clap.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

static const clap_plugin_t* gPlug = nullptr;
static const clap_plugin_timer_support_t* gPlugTimer = nullptr;
static std::vector<NSTimer*> gTimers;

static bool hostRegisterTimer(const clap_host_t*, uint32_t periodMs, clap_id* id) {
    clap_id tid = (clap_id)gTimers.size();
    NSTimer* t = [NSTimer scheduledTimerWithTimeInterval:periodMs / 1000.0 repeats:YES block:^(NSTimer*) {
        if (gPlugTimer) gPlugTimer->on_timer(gPlug, tid);
    }];
    gTimers.push_back(t);
    *id = tid;
    return true;
}
static bool hostUnregisterTimer(const clap_host_t*, clap_id id) {
    if (id < gTimers.size() && gTimers[id]) { [gTimers[id] invalidate]; gTimers[id] = nil; }
    return true;
}
static const clap_host_timer_support_t kHostTimer = { hostRegisterTimer, hostUnregisterTimer };

static const void* hostGetExt(const clap_host_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kHostTimer;
    return nullptr;
}
static void hostNoop(const clap_host_t*) {}
static void hostMainThread(const clap_host_t*) {
    dispatch_async(dispatch_get_main_queue(), ^{ if (gPlug) gPlug->on_main_thread(gPlug); });
}

static clap_host_t gHost = {
    CLAP_VERSION, nullptr, "gasfree-gui-test", "test", "", "0.1",
    hostGetExt, hostNoop, hostNoop, hostMainThread
};

static uint32_t emptySize(const clap_input_events_t*) { return 0; }
static const clap_event_header_t* emptyGet(const clap_input_events_t*, uint32_t) { return nullptr; }
static bool outPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

static WKWebView* findWebView(NSView* v) {
    if ([v isKindOfClass:[WKWebView class]]) return (WKWebView*)v;
    for (NSView* s in v.subviews) if (WKWebView* w = findWebView(s)) return w;
    return nil;
}

static void spin(double seconds) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0) {
        NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                            inMode:NSDefaultRunLoopMode dequeue:YES];
        if (ev) [NSApp sendEvent:ev];
    }
}

static bool snapshot(WKWebView* wv, NSString* path) {
    __block bool done = false, ok = false;
    [wv takeSnapshotWithConfiguration:nil completionHandler:^(NSImage* img, NSError* err) {
        if (img) {
            CGImageRef cg = [img CGImageForProposedRect:nil context:nil hints:nil];
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
            ok = [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
        } else {
            fprintf(stderr, "snapshot error: %s\n", err.localizedDescription.UTF8String);
        }
        done = true;
    }];
    while (!done) spin(0.05);
    return ok;
}

static NSString* evalJs(WKWebView* wv, NSString* js) {
    __block bool done = false;
    __block NSString* out = @"";
    [wv evaluateJavaScript:js completionHandler:^(id r, NSError* e) {
        out = r ? [NSString stringWithFormat:@"%@", r] : [NSString stringWithFormat:@"ERR %@", e.localizedDescription];
        done = true;
    }];
    while (!done) spin(0.05);
    return out;
}

// Click at (x, y) in the plugin view's top-left coordinate space.
static void click(NSWindow* win, NSView* content, double x, double y) {
    NSPoint p = NSMakePoint(x, content.bounds.size.height - y);
    for (NSEventType t : { NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp }) {
        NSEvent* ev = [NSEvent mouseEventWithType:t location:p modifierFlags:0
                                        timestamp:[[NSProcessInfo processInfo] systemUptime]
                                     windowNumber:win.windowNumber context:nil
                                      eventNumber:0 clickCount:1 pressure:(t == NSEventTypeLeftMouseDown ? 1.0f : 0.0f)];
        [win sendEvent:ev];
        spin(0.08);
    }
}

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char** argv) {
    @autoreleasepool {
        REQUIRE(argc >= 3);
        NSString* bundle = [NSString stringWithUTF8String:argv[1]];
        NSString* outDir = [NSString stringWithUTF8String:argv[2]];
        NSString* bin = [bundle stringByAppendingPathComponent:
            [NSString stringWithFormat:@"Contents/MacOS/%@", [[bundle lastPathComponent] stringByDeletingPathExtension]]];

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];

        void* h = dlopen(bin.UTF8String, RTLD_NOW | RTLD_LOCAL);
        REQUIRE(h);
        auto* entry = (const clap_plugin_entry_t*)dlsym(h, "clap_entry");
        REQUIRE(entry && entry->init(bundle.UTF8String));
        auto* fac = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        REQUIRE(fac && fac->get_plugin_count(fac) > 0);
        auto* desc = fac->get_plugin_descriptor(fac, 0);
        gPlug = fac->create_plugin(fac, &gHost, desc->id);
        REQUIRE(gPlug && gPlug->init(gPlug));
        gPlugTimer = (const clap_plugin_timer_support_t*)gPlug->get_extension(gPlug, CLAP_EXT_TIMER_SUPPORT);
        auto* gui = (const clap_plugin_gui_t*)gPlug->get_extension(gPlug, CLAP_EXT_GUI);
        REQUIRE(gui && gui->is_api_supported(gPlug, CLAP_WINDOW_API_COCOA, false));

        REQUIRE(gPlug->activate(gPlug, 48000.0, 32, 256));
        std::atomic<bool> running{true};
        std::thread audio([&] {
            gPlug->start_processing(gPlug);
            std::vector<float> iL(256), iR(256), oL(256), oR(256);
            float* ip[2] = { iL.data(), iR.data() };
            float* op[2] = { oL.data(), oR.data() };
            clap_audio_buffer_t in{}, out{};
            in.data32 = ip; in.channel_count = 2;
            out.data32 = op; out.channel_count = 2;
            clap_input_events_t ie{ nullptr, emptySize, emptyGet };
            clap_output_events_t oe{ nullptr, outPush };
            int64_t steady = 0;
            while (running) {
                clap_process_t pr{};
                pr.steady_time = steady; pr.frames_count = 256;
                pr.audio_inputs = &in; pr.audio_inputs_count = 1;
                pr.audio_outputs = &out; pr.audio_outputs_count = 1;
                pr.in_events = &ie; pr.out_events = &oe;
                gPlug->process(gPlug, &pr);
                steady += 256;
                std::this_thread::sleep_for(std::chrono::microseconds(5333));
            }
            gPlug->stop_processing(gPlug);
        });

        uint32_t w = 1280, hgt = 820;
        REQUIRE(gui->create(gPlug, CLAP_WINDOW_API_COCOA, false));
        gui->get_size(gPlug, &w, &hgt);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, w, hgt)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"GASFREE GUI test";
        clap_window_t cw{}; cw.api = CLAP_WINDOW_API_COCOA; cw.cocoa = (__bridge void*)win.contentView;
        REQUIRE(gui->set_parent(gPlug, &cw));
        REQUIRE(gui->show(gPlug));
        [win makeKeyAndOrderFront:nil];
        [win orderFrontRegardless];
        [NSApp activateIgnoringOtherApps:YES];

        spin(6.0);
        {
            NSDictionary* sess = CFBridgingRelease(CGSessionCopyCurrentDictionary());
            printf("[test_gui_mac] occlusionVisible=%d isVisible=%d appActive=%d displayAsleep=%d onConsole=%s locked=%s\n",
                (int)((win.occlusionState & NSWindowOcclusionStateVisible) != 0), (int)win.isVisible, (int)NSApp.isActive,
                (int)CGDisplayIsAsleep(CGMainDisplayID()),
                [[sess[@"kCGSSessionOnConsoleKey"] description] UTF8String] ?: "?",
                [[sess[@"CGSSessionScreenIsLocked"] description] UTF8String] ?: "0");
        }
        WKWebView* wv = findWebView(win.contentView);
        REQUIRE(wv != nil);
        printf("[test_gui_mac] page: %s\n", evalJs(wv,
            @"JSON.stringify({url: location.href, title: document.title, canvases: document.querySelectorAll('canvas').length,"
             " bridge: typeof window.gasfreePost, text: document.body.innerText.slice(0, 120)})").UTF8String);
        REQUIRE(snapshot(wv, [outDir stringByAppendingPathComponent:@"mac_gui_loaded.png"]));

        // Probes: count native->JS messages and pointer events reaching the page.
        evalJs(wv, @"window.__probe={native:0,ptr:0,raf:0,last:''};(function t(){__probe.raf++;requestAnimationFrame(t);})();"
                    "(function(){var f=window.__gasfreeOnNative;window.__gasfreeOnNative=function(m){__probe.native++;if(m&&m.meter)__probe.meter=Math.max(m.meter[0],m.meter[1]);__probe.last=JSON.stringify(m).slice(0,160);return f&&f(m);};})();"
                    "addEventListener('pointerdown',function(e){__probe.ptr++;__probe.at=[e.clientX,e.clientY];},true);0");
        spin(1.0);
        printf("[test_gui_mac] probe before click: %s\n", evalJs(wv, @"JSON.stringify({raf:__probe.raf,native:__probe.native,vis:document.visibilityState,hidden:document.hidden})").UTF8String);
        click(win, win.contentView, 163, 600);   // Subharm PLAY in the default rack (macOS layout)
        spin(4.0);
        printf("[test_gui_mac] probe after click: %s\n", evalJs(wv, @"JSON.stringify(__probe)").UTF8String);
        REQUIRE([evalJs(wv, @"String(!document.hidden && __probe.raf > 30)") isEqualToString:@"true"]);
        REQUIRE([evalJs(wv, @"String(__probe.ptr >= 1)") isEqualToString:@"true"]);
        REQUIRE([evalJs(wv, @"String(__probe.meter > 0)") isEqualToString:@"true"]);  // audio after PLAY
        printf("[test_gui_mac] after PLAY: %s\n", evalJs(wv, @"document.body.innerText.match(/CPU[^\\n]*/)?.[0] || \"no cpu\"").UTF8String);
        REQUIRE(snapshot(wv, [outDir stringByAppendingPathComponent:@"mac_gui_playing.png"]));
        click(win, win.contentView, 163, 600);   // stop again
        spin(0.5);

        gui->hide(gPlug);
        gui->destroy(gPlug);
        running = false;
        audio.join();
        gPlug->deactivate(gPlug);
        gPlug->destroy(gPlug);
        entry->deinit();
        printf("[test_gui_mac] snapshots written to %s\n", outDir.UTF8String);
        printf("[test_gui_mac] ALL GUI CHECKS PASSED\n");
    }
    return 0;
}
