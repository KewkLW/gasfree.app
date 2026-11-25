#include "clap_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gasfree {

namespace {

const char* kFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr
};

const char* nativeWindowApi() noexcept {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

inline GasfreeClapPlugin* selfOf(const clap_plugin_t* p) noexcept {
    return static_cast<GasfreeClapPlugin*>(p->plugin_data);
}

} // namespace

const clap_plugin_descriptor_t GasfreeClapPlugin::kDescriptor = {
    CLAP_VERSION_INIT,
    "app.gasfree.rack",
    "GASFREE",
    "GASFREE",
    "https://gasfree.app",
    "https://gasfree.app",
    "https://gasfree.app",
    "0.1.0",
    "Modular rack of synthesizers, drum machines, sequencers and effects",
    kFeatures
};

const clap_plugin_t* GasfreeClapPlugin::create(const clap_host_t* host) {
    auto* inst = new GasfreeClapPlugin(host);
    return inst->clapPlugin();
}

GasfreeClapPlugin::GasfreeClapPlugin(const clap_host_t* host)
    : host_(host),
      resources_(std::make_shared<ResourceProvider>(GASFREE_DEFAULT_APP_DIR, GASFREE_BRIDGE_JS_PATH)),
      engine_(resources_->appDir()),
      rawSettingsJson_(RackEngine::defaultRackJson()) {
    params_[0].store(0.8, std::memory_order_relaxed); // Master Volume default = 0.8 (unity)
    for (uint32_t i = 1; i < kNumParams; ++i) {
        params_[i].store(0.5, std::memory_order_relaxed);
    }

    plugin_.desc = &kDescriptor;
    plugin_.plugin_data = this;
    plugin_.init = sInit;
    plugin_.destroy = sDestroy;
    plugin_.activate = sActivate;
    plugin_.deactivate = sDeactivate;
    plugin_.start_processing = sStartProcessing;
    plugin_.stop_processing = sStopProcessing;
    plugin_.reset = sReset;
    plugin_.process = sProcess;
    plugin_.get_extension = sGetExtension;
    plugin_.on_main_thread = sOnMainThread;
}

GasfreeClapPlugin::~GasfreeClapPlugin() {
    guiDestroy();
}

bool GasfreeClapPlugin::init() {
    if (host_) {
        hostTimer_ = static_cast<const clap_host_timer_support_t*>(
            host_->get_extension(host_, CLAP_EXT_TIMER_SUPPORT));
    }
    engine_.prepare(48000.0, 512);
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        engine_.applyRawSettings(rawSettingsJson_);
    }
    return true;
}

bool GasfreeClapPlugin::activate(double sampleRate, uint32_t, uint32_t maxFrames) {
    engine_.prepare(sampleRate, static_cast<int>(maxFrames));
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        engine_.applyRawSettings(rawSettingsJson_);
    }
    RtEvent io;
    io.kind = RtEvent::Kind::IoGain;
    io.value = RackEngine::masterKnobToGain(params_[0].load(std::memory_order_relaxed));
    io.value2 = 4.0 * inputGainKnob_ * inputGainKnob_;
    engine_.pushUiEvent(io);
    return true;
}

void GasfreeClapPlugin::deactivate() {
    engine_.reclaimTrash();
}

bool GasfreeClapPlugin::startProcessing() { return true; }
void GasfreeClapPlugin::stopProcessing() {}

void GasfreeClapPlugin::reset() {
    RtEvent ev;
    ev.kind = RtEvent::Kind::AllNotesOff;
    engine_.pushUiEvent(ev);
}

clap_process_status GasfreeClapPlugin::process(const clap_process_t* proc) {
    if (!proc || proc->audio_outputs_count == 0 || proc->audio_outputs[0].channel_count == 0)
        return CLAP_PROCESS_CONTINUE;

    const uint32_t nframes = proc->frames_count;
    float* outL = proc->audio_outputs[0].data32[0];
    float* outR = proc->audio_outputs[0].channel_count > 1 ? proc->audio_outputs[0].data32[1] : nullptr;

    const float* inL = nullptr;
    const float* inR = nullptr;
    if (proc->audio_inputs_count > 0 && proc->audio_inputs[0].channel_count > 0 && proc->audio_inputs[0].data32) {
        inL = proc->audio_inputs[0].data32[0];
        inR = proc->audio_inputs[0].channel_count > 1 ? proc->audio_inputs[0].data32[1] : inL;
    }

    // Translate CLAP input events into fixed-size stack array (zero heap allocations)
    constexpr int kMaxEvents = 256;
    RtEvent evBuf[kMaxEvents];
    int evCount = 0;

    auto pushEv = [&](const RtEvent& e) {
        if (evCount < kMaxEvents) evBuf[evCount++] = e;
    };

    if (proc->in_events) {
        const uint32_t count = proc->in_events->size(proc->in_events);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t* hdr = proc->in_events->get(proc->in_events, i);
            if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;

            if (hdr->type == CLAP_EVENT_NOTE_ON) {
                const auto* ne = reinterpret_cast<const clap_event_note_t*>(hdr);
                RtEvent e;
                e.kind = RtEvent::Kind::NoteOn;
                e.sampleOffset = static_cast<int32_t>(hdr->time);
                e.channel = clampi(ne->channel + 1, 1, 16);
                e.note = clampi(ne->key, 0, 127);
                e.value = clampd(ne->velocity, 0.0, 1.0);
                pushEv(e);
            } else if (hdr->type == CLAP_EVENT_NOTE_OFF || hdr->type == CLAP_EVENT_NOTE_CHOKE) {
                const auto* ne = reinterpret_cast<const clap_event_note_t*>(hdr);
                RtEvent e;
                e.kind = RtEvent::Kind::NoteOff;
                e.sampleOffset = static_cast<int32_t>(hdr->time);
                e.channel = clampi(ne->channel + 1, 1, 16);
                e.note = clampi(ne->key, 0, 127);
                pushEv(e);
            } else if (hdr->type == CLAP_EVENT_MIDI) {
                const auto* me = reinterpret_cast<const clap_event_midi_t*>(hdr);
                const uint8_t st = me->data[0];
                const uint8_t d1 = me->data[1];
                const uint8_t d2 = me->data[2];
                RtEvent e;
                e.sampleOffset = static_cast<int32_t>(hdr->time);

                if (st == 0xF8) { e.kind = RtEvent::Kind::MidiClock; pushEv(e); continue; }
                if (st == 0xFA) { e.kind = RtEvent::Kind::MidiStart; pushEv(e); continue; }
                if (st == 0xFB) { e.kind = RtEvent::Kind::MidiContinue; pushEv(e); continue; }
                if (st == 0xFC) { e.kind = RtEvent::Kind::MidiStop; pushEv(e); continue; }
                if (st >= 0xF0) continue;

                const uint8_t cmd = st & 0xF0;
                e.channel = (st & 0x0F) + 1;

                if (cmd == 0x90 && d2 > 0) {
                    e.kind = RtEvent::Kind::NoteOn;
                    e.note = d1;
                    e.value = static_cast<double>(d2) / 127.0;
                    pushEv(e);
                } else if (cmd == 0x80 || cmd == 0x90) {
                    e.kind = RtEvent::Kind::NoteOff;
                    e.note = d1;
                    pushEv(e);
                } else if (cmd == 0xE0) {
                    const int bend = ((static_cast<int>(d2) << 7) | d1) - 8192;
                    e.kind = RtEvent::Kind::PitchBend;
                    e.value = (static_cast<double>(bend) / 8192.0) * (2.0 / 12.0);
                    pushEv(e);
                } else if (cmd == 0xB0 && (d1 == 120 || d1 == 123)) {
                    e.kind = RtEvent::Kind::AllNotesOff;
                    pushEv(e);
                } else if (cmd == 0xB0 && d1 == 1) {
                    e.kind = RtEvent::Kind::ModWheel;
                    e.value = static_cast<double>(d2) / 127.0;
                    pushEv(e);
                } else if (cmd == 0xB0 && d1 == 64) {
                    e.kind = RtEvent::Kind::Sustain;
                    e.gate = d2 >= 64;
                    pushEv(e);
                } else if (cmd == 0xD0) {
                    e.kind = RtEvent::Kind::Pressure;
                    e.value = static_cast<double>(d1) / 127.0;
                    pushEv(e);
                } else if (cmd == 0xA0) {
                    e.kind = RtEvent::Kind::PolyPressure;
                    e.note = d1;
                    e.value = static_cast<double>(d2) / 127.0;
                    pushEv(e);
                }
            } else if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* pe = reinterpret_cast<const clap_event_param_value_t*>(hdr);
                if (pe->param_id < kNumParams) {
                    const double v = clampd(pe->value, 0.0, 1.0);
                    params_[pe->param_id].store(v, std::memory_order_relaxed);
                    if (pe->param_id == 0) {
                        RtEvent e;
                        e.kind = RtEvent::Kind::IoGain;
                        e.sampleOffset = static_cast<int32_t>(hdr->time);
                        e.value = RackEngine::masterKnobToGain(v);
                        e.value2 = 4.0 * inputGainKnob_ * inputGainKnob_;
                        pushEv(e);
                    }
                }
            }
        }
    }

    double hostBpm = 0.0;
    if (proc->transport && (proc->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && proc->transport->tempo > 0.0) {
        hostBpm = proc->transport->tempo;
    }

    engine_.processBlock(inL, inR, outL, outR, static_cast<int>(nframes), evBuf, evCount, hostBpm);
    return CLAP_PROCESS_CONTINUE;
}

const void* GasfreeClapPlugin::getExtension(const char* id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPortsExt;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &kNotePortsExt;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParamsExt;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kStateExt;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &kGuiExt;
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &kTimerExt;
    return nullptr;
}

void GasfreeClapPlugin::onMainThread() {
    engine_.reclaimTrash();
}

// ---- GUI ----

bool GasfreeClapPlugin::guiCreate(const char* api, bool isFloating) {
    if (isFloating || !api || std::strcmp(api, nativeWindowApi()) != 0)
        return false;

    gui_ = WebViewGui::createPlatformGui(
        resources_,
        [this](const std::string& msg) { handleWebMessage(msg); },
        [this]() { sendStateToWebView(); });

    if (!gui_ || !gui_->create(guiWidth_, guiHeight_)) {
        std::fprintf(stderr, "[GASFREE] guiCreate failed (api=%s)\n", api);
        std::fflush(stderr);
        gui_.reset();
        return false;
    }

    if (hostTimer_ && timerId_ == CLAP_INVALID_ID) {
        const bool ok = hostTimer_->register_timer(host_, 16, &timerId_);
        std::fprintf(stderr, "[GASFREE] guiCreate OK, timer registered=%d id=%u\n", ok ? 1 : 0, timerId_);
        std::fflush(stderr);
    }
    return true;
}

void GasfreeClapPlugin::guiDestroy() {
    if (hostTimer_ && timerId_ != CLAP_INVALID_ID) {
        hostTimer_->unregister_timer(host_, timerId_);
        timerId_ = CLAP_INVALID_ID;
    }
    if (gui_) {
        gui_->destroy();
        gui_.reset();
    }
}

bool GasfreeClapPlugin::guiGetSize(uint32_t* width, uint32_t* height) {
    *width = guiWidth_;
    *height = guiHeight_;
    return true;
}

bool GasfreeClapPlugin::guiSetSize(uint32_t width, uint32_t height) {
    guiWidth_ = std::max<uint32_t>(640, width);
    guiHeight_ = std::max<uint32_t>(400, height);
    return gui_ ? gui_->setSize(guiWidth_, guiHeight_) : true;
}

bool GasfreeClapPlugin::guiSetParent(const clap_window_t* window) {
    if (!gui_ || !window) return false;
#if defined(__APPLE__)
    return gui_->setParent(window->cocoa);
#elif defined(_WIN32)
    return gui_->setParent(window->win32);
#else
    return gui_->setParent(reinterpret_cast<void*>(static_cast<uintptr_t>(window->x11)));
#endif
}

bool GasfreeClapPlugin::guiShow() { return gui_ ? gui_->show() : false; }
bool GasfreeClapPlugin::guiHide() { return gui_ ? gui_->hide() : false; }

void GasfreeClapPlugin::onTimer(clap_id id) {
    if (id != timerId_) return;
    if (gui_) gui_->pumpEvents();
    engine_.reclaimTrash();

    // Push telemetry to WebView at ~30 Hz (every 2nd 16ms tick)
    if (gui_ && (++timerTickCount_ & 1u) == 0) {
        sendNativeMessageToWebView(engine_.buildTelemetryJson());
    }
}

// ---- WebView <-> Native Bridge ----

void GasfreeClapPlugin::sendNativeMessageToWebView(const nlohmann::json& payload) {
    if (!gui_) return;
    std::string js = "if(window.__gasfreeOnNative) window.__gasfreeOnNative(" + payload.dump() + ");";
    gui_->evaluateJs(js);
}

void GasfreeClapPlugin::sendStateToWebView() {
    nlohmann::json msg;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        msg = {
            {"type", "init"},
            {"sampleRate", engine_.sampleRate()},
            {"master", params_[0].load(std::memory_order_relaxed)},
            {"nativeTypes", RackEngine::supportedNativeTypes()},
            {"hasState", hasCustomState_},
            {"settings", rawSettingsJson_}
        };
    }
    sendNativeMessageToWebView(msg);
}

void GasfreeClapPlugin::handleWebMessage(const std::string& jsonStr) {
    nlohmann::json m;
    try {
        m = nlohmann::json::parse(jsonStr);
    } catch (...) {
        return;
    }
    if (!m.is_object() || !m.contains("type") || !m["type"].is_string()) return;
    const std::string type = m["type"].get<std::string>();

    if (type == "settings") {
        if (m.contains("rawSettings") && m["rawSettings"].is_object()) {
            std::lock_guard<std::mutex> lk(stateMutex_);
            rawSettingsJson_ = m["rawSettings"];
            hasCustomState_ = true;
        }
        if (m.contains("settings") && m["settings"].is_object()) {
            double tempo = (m.contains("tempo") && m["tempo"].is_number()) ? m["tempo"].get<double>() : 120.0;
            engine_.applyFlattenedSettings(m["settings"], tempo);
        }
    } else if (type == "module") {
        if (m.contains("id") && m["id"].is_string() && m.contains("msg")) {
            engine_.postModuleMessageJson(m["id"].get<std::string>(), m["msg"]);
        }
    } else if (type == "note") {
        RtEvent ev;
        const bool on = m.contains("on") && m["on"].is_boolean() && m["on"].get<bool>();
        ev.kind = on ? RtEvent::Kind::NoteOn : RtEvent::Kind::NoteOff;
        ev.channel = m.contains("channel") && m["channel"].is_number() ? clampi(m["channel"].get<int>(), 1, 16) : 1;
        ev.note = m.contains("note") && m["note"].is_number() ? clampi(m["note"].get<int>(), 0, 127) : 60;
        ev.value = m.contains("vel") && m["vel"].is_number() ? clampd(m["vel"].get<double>(), 0.0, 1.0) : 1.0;
        engine_.pushUiEvent(ev);
    } else if (type == "notesOff") {
        RtEvent ev;
        ev.kind = RtEvent::Kind::AllNotesOff;
        engine_.pushUiEvent(ev);
    } else if (type == "kb") {
        RtEvent ev;
        ev.kind = RtEvent::Kind::KbZone;
        ev.zone = m.contains("zone") && m["zone"].is_number() ? m["zone"].get<int>() : 0;
        ev.gate = m.contains("gate") && m["gate"].is_boolean() && m["gate"].get<bool>();
        if (m.contains("key") && m["key"].is_number()) {
            ev.hasPitch = true;
            ev.value = m["key"].get<double>();
        }
        engine_.pushUiEvent(ev);
    } else if (type == "kbTrig") {
        RtEvent ev;
        ev.kind = RtEvent::Kind::KbTrig;
        engine_.pushUiEvent(ev);
    } else if (type == "scale") {
        engine_.setCustomScale(m.contains("scale") ? m["scale"] : nlohmann::json(nullptr));
    } else if (type == "io") {
        const double master = m.contains("master") && m["master"].is_number() ? clampd(m["master"].get<double>(), 0.0, 1.0) : 0.8;
        const double inGain = m.contains("input_gain") && m["input_gain"].is_number() ? clampd(m["input_gain"].get<double>(), 0.0, 1.0) : 0.5;
        params_[0].store(master, std::memory_order_relaxed);
        inputGainKnob_ = inGain;
        RtEvent ev;
        ev.kind = RtEvent::Kind::IoGain;
        ev.value = RackEngine::masterKnobToGain(master);
        ev.value2 = 4.0 * inGain * inGain;
        engine_.pushUiEvent(ev);
    } else if (type == "bend" || type == "modwheel" || type == "pressure" || type == "polypressure" || type == "sustain") {
        RtEvent ev;
        ev.channel = m.contains("channel") && m["channel"].is_number() ? clampi(m["channel"].get<int>(), 1, 16) : 1;
        if (type == "bend") ev.kind = RtEvent::Kind::PitchBend;
        else if (type == "modwheel") ev.kind = RtEvent::Kind::ModWheel;
        else if (type == "pressure") ev.kind = RtEvent::Kind::Pressure;
        else if (type == "polypressure") ev.kind = RtEvent::Kind::PolyPressure;
        else ev.kind = RtEvent::Kind::Sustain;
        if (m.contains("value") && m["value"].is_number()) ev.value = m["value"].get<double>();
        if (m.contains("value") && m["value"].is_boolean()) ev.gate = m["value"].get<bool>();
        if (m.contains("note") && m["note"].is_number()) ev.note = m["note"].get<int>();
        engine_.pushUiEvent(ev);
    } else if (type == "midiClock") {
        RtEvent ev; ev.kind = RtEvent::Kind::MidiClock; engine_.pushUiEvent(ev);
    } else if (type == "midiStart") {
        RtEvent ev; ev.kind = RtEvent::Kind::MidiStart; engine_.pushUiEvent(ev);
    } else if (type == "midiStop") {
        RtEvent ev; ev.kind = RtEvent::Kind::MidiStop; engine_.pushUiEvent(ev);
    } else if (type == "midiContinue") {
        RtEvent ev; ev.kind = RtEvent::Kind::MidiContinue; engine_.pushUiEvent(ev);
    }
}

// ---- State Persistence ----

bool GasfreeClapPlugin::stateSave(const clap_ostream_t* stream) {
    if (!stream) return false;
    nlohmann::json doc;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        doc = {
            {"format", "gasfree-clap-state"},
            {"version", 1},
            {"master", params_[0].load(std::memory_order_relaxed)},
            {"input_gain", inputGainKnob_},
            {"settings", rawSettingsJson_}
        };
    }
    const std::string bytes = doc.dump();
    size_t written = 0;
    while (written < bytes.size()) {
        const int64_t n = stream->write(stream, bytes.data() + written, bytes.size() - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

bool GasfreeClapPlugin::stateLoad(const clap_istream_t* stream) {
    if (!stream) return false;
    std::string bytes;
    char buf[4096];
    for (;;) {
        const int64_t n = stream->read(stream, buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) break;
        bytes.append(buf, static_cast<size_t>(n));
        if (bytes.size() > 64 * 1024 * 1024) return false; // 64MB sanity cap
    }
    try {
        auto doc = nlohmann::json::parse(bytes);
        if (!doc.is_object() || !doc.contains("settings") || !doc["settings"].is_object())
            return false;
        if (doc.contains("master") && doc["master"].is_number())
            params_[0].store(clampd(doc["master"].get<double>(), 0.0, 1.0), std::memory_order_relaxed);
        if (doc.contains("input_gain") && doc["input_gain"].is_number())
            inputGainKnob_ = clampd(doc["input_gain"].get<double>(), 0.0, 1.0);
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            rawSettingsJson_ = doc["settings"];
            hasCustomState_ = true;
        }
        engine_.applyRawSettings(doc["settings"]);
        RtEvent io;
        io.kind = RtEvent::Kind::IoGain;
        io.value = RackEngine::masterKnobToGain(params_[0].load(std::memory_order_relaxed));
        io.value2 = 4.0 * inputGainKnob_ * inputGainKnob_;
        engine_.pushUiEvent(io);

        if (gui_) sendStateToWebView();
        return true;
    } catch (...) {
        return false;
    }
}

// ---- Static CLAP callbacks & extensions ----

bool GasfreeClapPlugin::sInit(const clap_plugin_t* p) { return selfOf(p)->init(); }
void GasfreeClapPlugin::sDestroy(const clap_plugin_t* p) { delete selfOf(p); }
bool GasfreeClapPlugin::sActivate(const clap_plugin_t* p, double sr, uint32_t minF, uint32_t maxF) { return selfOf(p)->activate(sr, minF, maxF); }
void GasfreeClapPlugin::sDeactivate(const clap_plugin_t* p) { selfOf(p)->deactivate(); }
bool GasfreeClapPlugin::sStartProcessing(const clap_plugin_t* p) { return selfOf(p)->startProcessing(); }
void GasfreeClapPlugin::sStopProcessing(const clap_plugin_t* p) { selfOf(p)->stopProcessing(); }
void GasfreeClapPlugin::sReset(const clap_plugin_t* p) { selfOf(p)->reset(); }
clap_process_status GasfreeClapPlugin::sProcess(const clap_plugin_t* p, const clap_process_t* proc) { return selfOf(p)->process(proc); }
const void* GasfreeClapPlugin::sGetExtension(const clap_plugin_t* p, const char* id) { return selfOf(p)->getExtension(id); }
void GasfreeClapPlugin::sOnMainThread(const clap_plugin_t* p) { selfOf(p)->onMainThread(); }

const clap_plugin_audio_ports_t GasfreeClapPlugin::kAudioPortsExt = {
    [](const clap_plugin_t*, bool /*is_input*/) -> uint32_t { return 1; },
    [](const clap_plugin_t*, uint32_t index, bool is_input, clap_audio_port_info_t* info) -> bool {
        if (index != 0) return false;
        std::memset(info, 0, sizeof(*info));
        info->id = is_input ? 0 : 1;
        std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Audio In" : "Main Out");
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }
};

const clap_plugin_note_ports_t GasfreeClapPlugin::kNotePortsExt = {
    [](const clap_plugin_t*, bool is_input) -> uint32_t { return is_input ? 1 : 0; },
    [](const clap_plugin_t*, uint32_t index, bool is_input, clap_note_port_info_t* info) -> bool {
        if (!is_input || index != 0) return false;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        std::snprintf(info->name, sizeof(info->name), "MIDI In");
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        return true;
    }
};

const clap_plugin_params_t GasfreeClapPlugin::kParamsExt = {
    [](const clap_plugin_t*) -> uint32_t { return kNumParams; },
    [](const clap_plugin_t*, uint32_t idx, clap_param_info_t* info) -> bool {
        if (idx >= kNumParams) return false;
        std::memset(info, 0, sizeof(*info));
        info->id = idx;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        info->min_value = 0.0;
        info->max_value = 1.0;
        if (idx == 0) {
            std::snprintf(info->name, sizeof(info->name), "Master Volume");
            std::snprintf(info->module, sizeof(info->module), "Audio I/O");
            info->default_value = 0.8;
        } else {
            std::snprintf(info->name, sizeof(info->name), "Macro %u", idx);
            std::snprintf(info->module, sizeof(info->module), "Macros");
            info->default_value = 0.5;
        }
        return true;
    },
    [](const clap_plugin_t* p, clap_id paramId, double* outValue) -> bool {
        if (paramId >= kNumParams) return false;
        *outValue = selfOf(p)->params_[paramId].load(std::memory_order_relaxed);
        return true;
    },
    [](const clap_plugin_t*, clap_id paramId, double value, char* outBuf, uint32_t bufSize) -> bool {
        if (paramId == 0) {
            const double g = RackEngine::masterKnobToGain(value);
            if (g < 1e-4) std::snprintf(outBuf, bufSize, "-inf dB");
            else std::snprintf(outBuf, bufSize, "%+.1f dB", 20.0 * std::log10(g));
            return true;
        }
        std::snprintf(outBuf, bufSize, "%.1f%%", value * 100.0);
        return true;
    },
    [](const clap_plugin_t*, clap_id, const char* text, double* outValue) -> bool {
        char* end = nullptr;
        double v = std::strtod(text, &end);
        if (end == text) return false;
        *outValue = clampd(v, 0.0, 1.0);
        return true;
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
        if (!in) return;
        auto* self = selfOf(p);
        const uint32_t count = in->size(in);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* hdr = in->get(in, i);
            if (hdr && hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* pe = reinterpret_cast<const clap_event_param_value_t*>(hdr);
                if (pe->param_id < kNumParams) {
                    const double v = clampd(pe->value, 0.0, 1.0);
                    self->params_[pe->param_id].store(v, std::memory_order_relaxed);
                    if (pe->param_id == 0) {
                        RtEvent ev;
                        ev.kind = RtEvent::Kind::IoGain;
                        ev.value = RackEngine::masterKnobToGain(v);
                        ev.value2 = 4.0 * self->inputGainKnob_ * self->inputGainKnob_;
                        self->engine_.pushUiEvent(ev);
                    }
                }
            }
        }
    }
};

const clap_plugin_state_t GasfreeClapPlugin::kStateExt = {
    [](const clap_plugin_t* p, const clap_ostream_t* s) -> bool { return selfOf(p)->stateSave(s); },
    [](const clap_plugin_t* p, const clap_istream_t* s) -> bool { return selfOf(p)->stateLoad(s); }
};

const clap_plugin_gui_t GasfreeClapPlugin::kGuiExt = {
    [](const clap_plugin_t*, const char* api, bool isFloating) -> bool {
        return !isFloating && api && std::strcmp(api, nativeWindowApi()) == 0;
    },
    [](const clap_plugin_t*, const char** api, bool* isFloating) -> bool {
        *api = nativeWindowApi();
        *isFloating = false;
        return true;
    },
    [](const clap_plugin_t* p, const char* api, bool isFloating) -> bool { return selfOf(p)->guiCreate(api, isFloating); },
    [](const clap_plugin_t* p) { selfOf(p)->guiDestroy(); },
    [](const clap_plugin_t*, double) -> bool { return false; },
    [](const clap_plugin_t* p, uint32_t* w, uint32_t* h) -> bool { return selfOf(p)->guiGetSize(w, h); },
    [](const clap_plugin_t*) -> bool { return true; },
    [](const clap_plugin_t*, clap_gui_resize_hints_t* hints) -> bool {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        return true;
    },
    [](const clap_plugin_t*, uint32_t* w, uint32_t* h) -> bool {
        *w = std::max<uint32_t>(640, *w);
        *h = std::max<uint32_t>(400, *h);
        return true;
    },
    [](const clap_plugin_t* p, uint32_t w, uint32_t h) -> bool { return selfOf(p)->guiSetSize(w, h); },
    [](const clap_plugin_t* p, const clap_window_t* win) -> bool { return selfOf(p)->guiSetParent(win); },
    [](const clap_plugin_t*, const clap_window_t*) -> bool { return false; },
    [](const clap_plugin_t*, const char*) {},
    [](const clap_plugin_t* p) -> bool { return selfOf(p)->guiShow(); },
    [](const clap_plugin_t* p) -> bool { return selfOf(p)->guiHide(); }
};

const clap_plugin_timer_support_t GasfreeClapPlugin::kTimerExt = {
    [](const clap_plugin_t* p, clap_id timerId) { selfOf(p)->onTimer(timerId); }
};

// ---- CLAP Plugin Factory ----

namespace {

const clap_plugin_factory_t kPluginFactory = {
    [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* {
        return index == 0 ? &GasfreeClapPlugin::kDescriptor : nullptr;
    },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* pluginId) -> const clap_plugin_t* {
        if (!pluginId || std::strcmp(pluginId, GasfreeClapPlugin::kDescriptor.id) != 0)
            return nullptr;
        return GasfreeClapPlugin::create(host);
    }
};

} // namespace

bool gasfreeClapInit(const char*) { return true; }
void gasfreeClapDeinit() {}
const void* gasfreeClapGetFactory(const char* factoryId) {
    if (factoryId && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &kPluginFactory;
    return nullptr;
}

} // namespace gasfree
