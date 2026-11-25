#pragma once

#include "dsp/rack_engine.h"
#include "gui/resource_provider.h"
#include "gui/webview_gui.h"

#include <clap/clap.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace gasfree {

class GasfreeClapPlugin {
public:
    static const clap_plugin_descriptor_t kDescriptor;
    static const clap_plugin_t* create(const clap_host_t* host);

    explicit GasfreeClapPlugin(const clap_host_t* host);
    ~GasfreeClapPlugin();

    clap_plugin_t* clapPlugin() noexcept { return &plugin_; }

private:
    static constexpr uint32_t kNumMacros = 16;
    static constexpr uint32_t kNumParams = 1 + kNumMacros; // 0 = Master Volume, 1..16 = Macros
    static constexpr uint32_t kDefaultGuiWidth = 1280;
    static constexpr uint32_t kDefaultGuiHeight = 820;

    // Lifecycle
    bool init();
    bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames);
    void deactivate();
    bool startProcessing();
    void stopProcessing();
    void reset();
    clap_process_status process(const clap_process_t* process);
    const void* getExtension(const char* id);
    void onMainThread();

    // GUI
    bool guiCreate(const char* api, bool isFloating);
    void guiDestroy();
    bool guiGetSize(uint32_t* width, uint32_t* height);
    bool guiSetSize(uint32_t width, uint32_t height);
    bool guiSetParent(const clap_window_t* window);
    bool guiShow();
    bool guiHide();

    // Timer (main thread)
    void onTimer(clap_id timerId);

    // State
    bool stateSave(const clap_ostream_t* stream);
    bool stateLoad(const clap_istream_t* stream);

    // Bridge messages from WebView (main thread)
    void handleWebMessage(const std::string& jsonStr);
    void sendStateToWebView();
    void sendNativeMessageToWebView(const nlohmann::json& payload);

    // Static CLAP callbacks
    static bool sInit(const clap_plugin_t* p);
    static void sDestroy(const clap_plugin_t* p);
    static bool sActivate(const clap_plugin_t* p, double sr, uint32_t minF, uint32_t maxF);
    static void sDeactivate(const clap_plugin_t* p);
    static bool sStartProcessing(const clap_plugin_t* p);
    static void sStopProcessing(const clap_plugin_t* p);
    static void sReset(const clap_plugin_t* p);
    static clap_process_status sProcess(const clap_plugin_t* p, const clap_process_t* proc);
    static const void* sGetExtension(const clap_plugin_t* p, const char* id);
    static void sOnMainThread(const clap_plugin_t* p);

    static const clap_plugin_audio_ports_t kAudioPortsExt;
    static const clap_plugin_note_ports_t kNotePortsExt;
    static const clap_plugin_params_t kParamsExt;
    static const clap_plugin_state_t kStateExt;
    static const clap_plugin_gui_t kGuiExt;
    static const clap_plugin_timer_support_t kTimerExt;

    clap_plugin_t plugin_{};
    const clap_host_t* host_ = nullptr;
    const clap_host_timer_support_t* hostTimer_ = nullptr;
    clap_id timerId_ = CLAP_INVALID_ID;

    std::shared_ptr<ResourceProvider> resources_;
    RackEngine engine_;
    std::unique_ptr<WebViewGui> gui_;
    uint32_t guiWidth_ = kDefaultGuiWidth;
    uint32_t guiHeight_ = kDefaultGuiHeight;
    uint32_t timerTickCount_ = 0;

    // Authoritative DAW state (guarded by stateMutex_)
    std::mutex stateMutex_;
    nlohmann::json rawSettingsJson_;
    bool hasCustomState_ = false;

    // DAW automatable parameters
    std::array<std::atomic<double>, kNumParams> params_{};
    double inputGainKnob_ = 0.5;
};

// Factory entry helpers (used by plugin_entry.cpp and clap-wrapper)
bool gasfreeClapInit(const char* pluginPath);
void gasfreeClapDeinit();
const void* gasfreeClapGetFactory(const char* factoryId);

} // namespace gasfree
