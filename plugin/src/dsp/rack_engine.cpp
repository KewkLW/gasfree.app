#include "rack_engine.h"

#include "comp_dsp.h"
#include "delay_dsp.h"
#include "hyper_dsp.h"
#include "lfo_dsp.h"
#include "minimix_dsp.h"
#include "mixer_dsp.h"
#include "passthrough_dsp.h"
#include "reverb_dsp.h"
#include "subharm_dsp.h"

#include <chrono>
#include <cmath>
#include <unordered_set>

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 9> kRackOutIds = {
    "kb_gate1", "kb_key1", "kb_gate2", "kb_key2", "kb_trig",
    "in_l", "in_r", "io_input_l", "io_input_r"
};
constexpr std::array<std::string_view, 2> kRackInIds = {"io_main_l", "io_main_r"};

constexpr int O_KB_GATE[2] = {0, 2};
constexpr int O_KB_KEY[2] = {1, 3};
constexpr int O_KB_TRIG = 4;
constexpr int O_IN_L = 5, O_IN_R = 6;
constexpr int O_IO_IN[2] = {7, 8};
constexpr int I_MAIN[2] = {0, 1};
constexpr float GATE_V = 5.0f;
constexpr double PULSE_TIME = 0.005;
constexpr double LOAD_INTERVAL = 0.5;

int findIndex(std::span<const std::string_view> ids, std::string_view id) {
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == id) return static_cast<int>(i);
    }
    return -1;
}
} // namespace

RackEngine::RackEngine(std::string appDir) : appDir_(std::move(appDir)) {
    if (appDir_.empty()) appDir_ = GASFREE_DEFAULT_APP_DIR;
}

RackEngine::~RackEngine() {
    reclaimTrash();
}

void RackEngine::setAppDir(std::string dir) {
    std::lock_guard<std::mutex> lk(uiMutex_);
    appDir_ = std::move(dir);
}

std::vector<std::string> RackEngine::supportedNativeTypes() {
    return {"subharm", "lfo", "comp", "delay", "reverb", "mixer", "minimix", "hyper"};
}

nlohmann::json RackEngine::defaultRackJson() {
    return {
        {"version", 5},
        {"scale_file", nullptr},
        {"kb", {{"split", false}, {"split_note", 60}, {"octave", 0}, {"channel", 1}}},
        {"modules", {
            {
                {"id", "subharm"},
                {"type", "subharm"},
                {"settings", {
                    {"vco", {
                        {{"freq", 261.63}, {"wave", "saw"}, {"sub1", 2}, {"sub2", 4}, {"level", 0.55}, {"sub1_level", 0.6}, {"sub2_level", 0.45}},
                        {{"freq", 392.00}, {"wave", "square"}, {"sub1", 4}, {"sub2", 6}, {"level", 0.35}, {"sub1_level", 0.4}, {"sub2_level", 0.3}}
                    }},
                    {"seq", {{0.0, 0.25, 5.0 / 12.0, 7.0 / 12.0}, {0.0, -0.25, 2.0 / 12.0, -1.0 / 12.0}}},
                    {"seq_assign", {{{"osc", true}, {"sub1", false}, {"sub2", false}}, {{"osc", true}, {"sub1", false}, {"sub2", false}}}},
                    {"seq_oct", 1},
                    {"quantize", "12-ET"},
                    {"tempo", 300},
                    {"rhythm", {2, 3, 5, 7}},
                    {"rhythm_seq1", {true, false, true, false}},
                    {"rhythm_seq2", {false, true, false, true}},
                    {"cutoff", 900},
                    {"resonance", 0.35},
                    {"vcf_eg_amt", 0.45},
                    {"vcf_attack", 0.004},
                    {"vcf_decay", 0.35},
                    {"vca_attack", 0.004},
                    {"vca_decay", 0.6},
                    {"volume", 0.7},
                    {"eg_hold", false},
                    {"midi_master", true},
                    {"midi_channel", 0}
                }}
            },
            {
                {"id", "lfo"},
                {"type", "lfo"},
                {"settings", {{"lfo", {{{"rate", 0.5}, {"shape", "sine"}, {"level", 1.0}}, {{"rate", 4.0}, {"shape", "tri"}, {"level", 0.5}}}}}}
            },
            {
                {"id", "comp"},
                {"type", "comp"},
                {"settings", {{"on", true}, {"threshold", -12}, {"ratio", 3}, {"attack", 0.005}, {"release", 0.12}, {"makeup", 0}, {"mix", 1}, {"scope_time", 0.02}}}
            },
            {
                {"id", "reverb"},
                {"type", "reverb"},
                {"settings", {{"on", true}, {"mix", 0.15}, {"room", "HALL B"}, {"predelay", 0.02}, {"tone", 12000}, {"dry", true}}}
            },
            {
                {"id", "delay"},
                {"type", "delay"},
                {"settings", {{"on", false}, {"sync", true}, {"div", 3}, {"time", 0.3}, {"feedback", 0.4}, {"tone", 5000}, {"mix", 0.3}, {"pingpong", true}, {"dry", true}}}
            }
        }},
        {"patches", {
            {"comp.out_l", "delay.in_l", 1},
            {"comp.out_r", "delay.in_r", 5},
            {"delay.out_l", "reverb.in_l", 1},
            {"delay.out_r", "reverb.in_r", 5},
            {"reverb.out_l", "io_main_l", 1},
            {"reverb.out_r", "io_main_r", 5},
            {"subharm.audio", "comp.in_l", 2}
        }}
    };
}

std::shared_ptr<ModuleDSP> RackEngine::createModuleDsp(const std::string& type) {
    std::shared_ptr<ModuleDSP> m;
    if (type == "subharm") m = std::make_shared<SubharmDSP>();
    else if (type == "lfo") m = std::make_shared<LfoDSP>();
    else if (type == "comp") m = std::make_shared<CompDSP>();
    else if (type == "delay") m = std::make_shared<DelayDSP>();
    else if (type == "reverb") m = std::make_shared<ReverbDSP>(appDir_);
    else if (type == "mixer") m = std::make_shared<MixerDSP>();
    else if (type == "minimix") m = std::make_shared<MinimixDSP>();
    else if (type == "hyper") m = std::make_shared<HyperDSP>();
    else m = std::make_shared<PassthroughDSP>(type);

    m->host = this;
    m->prepare(sampleRate_.load(std::memory_order_relaxed));
    return m;
}

void RackEngine::prepare(double sr, int) {
    sampleRate_.store(sr, std::memory_order_relaxed);
    ioSmooth_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.02 * sr)));

    std::lock_guard<std::mutex> lk(uiMutex_);
    for (auto& [id, rec] : uiModules_) {
        rec.dsp->prepare(sr);
    }
    if (uiModules_.empty()) {
        // Release lock before calling applyRawSettings which re-locks
        uiMutex_.unlock();
        applyRawSettings(defaultRackJson());
        uiMutex_.lock();
    }
}

void RackEngine::applyRawSettings(const nlohmann::json& raw) {
    if (!raw.is_object()) return;

    // Flatten off-modules just like WR.flatten() in webrack.js
    nlohmann::json flat = raw;
    nlohmann::json mods = nlohmann::json::array();
    std::unordered_set<std::string> offIds;

    double tempoBpm = 120.0;
    bool gotTempo = false;

    if (raw.contains("modules") && raw["modules"].is_array()) {
        for (const auto& m : raw["modules"]) {
            if (!m.is_object() || !m.contains("id") || !m.contains("type")) continue;
            const bool isOff = m.contains("off") && m["off"].is_boolean() && m["off"].get<bool>();
            if (isOff) {
                offIds.insert(m["id"].get<std::string>());
                continue;
            }
            mods.push_back(m);
            if (!gotTempo && m.contains("settings") && m["settings"].is_object()) {
                const auto& st = m["settings"];
                if (st.contains("tempo") && st["tempo"].is_number() && st["tempo"].get<double>() > 0.0) {
                    tempoBpm = st["tempo"].get<double>();
                    gotTempo = true;
                }
            }
        }
    }
    flat["modules"] = std::move(mods);

    if (!offIds.empty() && raw.contains("patches") && raw["patches"].is_array()) {
        nlohmann::json kept = nlohmann::json::array();
        for (const auto& p : raw["patches"]) {
            if (!p.is_array() || p.size() < 2 || !p[0].is_string() || !p[1].is_string()) continue;
            auto modOf = [](const std::string& s) -> std::string {
                auto dot = s.find('.');
                return dot == std::string::npos ? "" : s.substr(0, dot);
            };
            if (offIds.count(modOf(p[0].get<std::string>())) || offIds.count(modOf(p[1].get<std::string>())))
                continue;
            kept.push_back(p);
        }
        flat["patches"] = std::move(kept);
    }

    applyFlattenedSettings(flat, tempoBpm);
}

void RackEngine::applyFlattenedSettings(const nlohmann::json& s, double tempoBpm) {
    if (!s.is_object()) return;

    std::lock_guard<std::mutex> lk(uiMutex_);
    auto plan = std::make_shared<RackPlan>();
    plan->tempo = tempoBpm > 0.0 ? tempoBpm : 120.0;

    std::unordered_map<std::string, UiModRecord> nextUiModules;
    std::unordered_map<std::string, int> indexById;
    uiActiveOrder_.clear();

    if (s.contains("modules") && s["modules"].is_array()) {
        for (const auto& m : s["modules"]) {
            if (!m.is_object() || !m.contains("id") || !m.contains("type")) continue;
            const std::string id = m["id"].get<std::string>();
            const std::string type = m["type"].get<std::string>();
            const nlohmann::json emptyObj = nlohmann::json::object();
            const nlohmann::json& settingsJson = (m.contains("settings") && m["settings"].is_object())
                ? m["settings"] : emptyObj;

            std::shared_ptr<ModuleDSP> dsp;
            auto it = uiModules_.find(id);
            if (it != uiModules_.end() && it->second.type == type) {
                dsp = it->second.dsp;
            } else {
                dsp = createModuleDsp(type);
            }
            nextUiModules[id] = {type, dsp};
            uiActiveOrder_.push_back({id, dsp});

            ModEntry entry;
            entry.id = id;
            entry.idHash = fnv1aHash(id);
            entry.type = type;
            entry.dsp = dsp;
            entry.settings = dsp->parseSettings(settingsJson);
            entry.midiMaster = settingsJson.contains("midi_master") && settingsJson["midi_master"].is_boolean()
                && settingsJson["midi_master"].get<bool>();
            entry.midiChannel = (settingsJson.contains("midi_channel") && settingsJson["midi_channel"].is_number())
                ? clampi(static_cast<int>(settingsJson["midi_channel"].get<double>()), 0, 16) : 0;
            entry.patched.assign(dsp->inIds().size(), 0);
            entry.outPatched.assign(dsp->outIds().size(), 0);

            indexById[id] = static_cast<int>(plan->modules.size());
            plan->modules.push_back(std::move(entry));
        }
    }
    uiModules_ = std::move(nextUiModules);

    // Resolve patch cables
    std::string patchesSig;
    if (s.contains("patches") && s["patches"].is_array()) {
        patchesSig = s["patches"].dump();
    }
    // Also include module ids/order in patch version so adding/removing a module refreshes pointers
    for (const auto& mod : plan->modules) {
        patchesSig += "|" + mod.id + ":" + mod.type + ":" + (mod.midiMaster ? "1" : "0");
    }
    if (patchesSig != uiLastPatchesSig_) {
        uiLastPatchesSig_ = patchesSig;
        uiPatchVersion_++;
    }
    plan->patchVersion = uiPatchVersion_;

    std::vector<uint8_t> midiCabled(plan->modules.size(), 0);
    std::vector<uint8_t> midiFed(plan->modules.size(), 0);

    auto resolveOut = [&](const std::string& jackId) -> std::pair<const float*, std::pair<int, int>> {
        const auto dot = jackId.find('.');
        if (dot == std::string::npos) {
            int idx = findIndex(kRackOutIds, jackId);
            if (idx < 0) return {nullptr, {-1, -1}};
            return {&rackOuts_[idx], {-1, idx}};
        }
        auto it = indexById.find(jackId.substr(0, dot));
        if (it == indexById.end()) return {nullptr, {-1, -1}};
        auto& mod = plan->modules[it->second];
        int idx = findIndex(mod.dsp->outIds(), jackId.substr(dot + 1));
        if (idx < 0) return {nullptr, {-1, -1}};
        return {&mod.dsp->outs[idx], {it->second, idx}};
    };

    auto resolveIn = [&](const std::string& jackId) -> std::pair<float*, std::pair<int, int>> {
        const auto dot = jackId.find('.');
        if (dot == std::string::npos) {
            int idx = findIndex(kRackInIds, jackId);
            if (idx < 0) return {nullptr, {-1, -1}};
            return {&rackIns_[idx], {-1, idx}};
        }
        auto it = indexById.find(jackId.substr(0, dot));
        if (it == indexById.end()) return {nullptr, {-1, -1}};
        auto& mod = plan->modules[it->second];
        int idx = findIndex(mod.dsp->inIds(), jackId.substr(dot + 1));
        if (idx < 0) return {nullptr, {-1, -1}};
        return {&mod.dsp->ins[idx], {it->second, idx}};
    };

    if (s.contains("patches") && s["patches"].is_array()) {
        for (const auto& p : s["patches"]) {
            if (!p.is_array() || p.size() < 2 || !p[0].is_string() || !p[1].is_string()) continue;
            const std::string srcId = p[0].get<std::string>();
            const std::string dstId = p[1].get<std::string>();

            if (srcId == "kb_midi") {
                const auto dot = dstId.find('.');
                if (dot != std::string::npos) {
                    auto it = indexById.find(dstId.substr(0, dot));
                    if (it != indexById.end() && plan->modules[it->second].dsp->midiInId() == dstId.substr(dot + 1)) {
                        midiCabled[it->second] = 1;
                    }
                }
                continue;
            }

            // Module MIDI OUT -> Module MIDI IN
            const auto sdot = srcId.find('.');
            if (sdot != std::string::npos) {
                auto sit = indexById.find(srcId.substr(0, sdot));
                if (sit != indexById.end()) {
                    const auto midiOut = plan->modules[sit->second].dsp->midiOutId();
                    if (!midiOut.empty() && midiOut == srcId.substr(sdot + 1)) {
                        const auto ddot = dstId.find('.');
                        if (ddot != std::string::npos) {
                            auto dit = indexById.find(dstId.substr(0, ddot));
                            if (dit != indexById.end() && dit->second != sit->second &&
                                plan->modules[dit->second].dsp->midiInId() == dstId.substr(ddot + 1)) {
                                plan->modules[sit->second].midiTargetIndices.push_back(dit->second);
                                midiFed[dit->second] = 1;
                            }
                        }
                        continue;
                    }
                }
            }

            if (static_cast<int>(plan->cables.size()) >= kMaxCables) continue;
            auto [srcPtr, srcLoc] = resolveOut(srcId);
            auto [dstPtr, dstLoc] = resolveIn(dstId);
            if (!srcPtr || !dstPtr) continue;

            plan->cables.push_back({srcPtr, dstPtr});
            if (dstLoc.first < 0) {
                if (!plan->rackPatched[dstLoc.second]) {
                    plan->rackPatched[dstLoc.second] = 1;
                    plan->zeroTargets.push_back(dstPtr);
                }
            } else {
                auto& dmod = plan->modules[dstLoc.first];
                if (!dmod.patched[dstLoc.second]) {
                    dmod.patched[dstLoc.second] = 1;
                    plan->zeroTargets.push_back(dstPtr);
                }
            }
            if (srcLoc.first >= 0) {
                plan->modules[srcLoc.first].outPatched[srcLoc.second] = 1;
            }
        }
    }

    for (size_t i = 0; i < plan->modules.size(); ++i) {
        plan->modules[i].onBus = (midiCabled[i] != 0) || plan->modules[i].midiMaster;
        plan->modules[i].midiConnected = plan->modules[i].onBus || (midiFed[i] != 0);
    }

    // Push to audio thread (if queue full, drop oldest? Better: push, and if full retry/keep in active if not running)
    if (!planQueue_.push(plan)) {
        // Audio thread hasn't started processing yet; safe to reclaim on next cycle or wait
        // When offline/inactive, apply directly if audio isn't active? No, keep strict SPSC:
        // CLAP guarantees deactivate before non-RT calls, and during active playback 16 slots is ample.
    }
}

void RackEngine::setCustomScale(const nlohmann::json& scaleJson) {
    std::shared_ptr<Scale> sc;
    if (scaleJson.is_object() && scaleJson.contains("degrees") && scaleJson["degrees"].is_array()) {
        sc = std::make_shared<Scale>();
        sc->period = (scaleJson.contains("period") && scaleJson["period"].is_number())
            ? scaleJson["period"].get<double>() : 1.0;
        for (const auto& d : scaleJson["degrees"]) {
            if (d.is_number()) sc->degrees.push_back(d.get<double>());
        }
        if (!sc->valid()) sc.reset();
    }
    scaleQueue_.push(std::move(sc));
}

bool RackEngine::pushUiEvent(const RtEvent& ev) noexcept {
    return uiEventQueue_.push(ev);
}

void RackEngine::postModuleMessageJson(const std::string& moduleId, const nlohmann::json& msg) {
    if (!msg.is_object() || !msg.contains("type") || !msg["type"].is_string()) return;
    const std::string t = msg["type"].get<std::string>();

    RtEvent ev;
    ev.kind = RtEvent::Kind::ModuleMsg;
    ev.moduleHash = fnv1aHash(moduleId);

    if (t == "play") {
        ev.dspMsg.type = DspMessage::Type::Play;
        if (msg.contains("value") && msg["value"].is_boolean()) {
            ev.dspMsg.hasValue = true;
            ev.dspMsg.gate = msg["value"].get<bool>();
        }
    } else if (t == "reset") {
        ev.dspMsg.type = DspMessage::Type::Reset;
    } else if (t == "next") {
        ev.dspMsg.type = DspMessage::Type::Next;
    } else if (t == "trigger") {
        ev.dspMsg.type = DspMessage::Type::Trigger;
        ev.dspMsg.gate = msg.contains("gate") && msg["gate"].is_boolean() && msg["gate"].get<bool>();
    } else {
        return;
    }
    pushUiEvent(ev);
}

void RackEngine::reclaimTrash() {
    TrashItem item;
    while (trashQueue_.pop(item)) {
        item.plan.reset();
        item.scale.reset();
    }
}

nlohmann::json RackEngine::buildTelemetryJson() const {
    nlohmann::json modulesJson = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lk(uiMutex_);
        for (const auto& [id, dsp] : uiActiveOrder_) {
            modulesJson[id] = dsp->readTelemetryJson();
        }
    }
    return {
        {"type", "state"},
        {"modules", std::move(modulesJson)},
        {"load", telCpuLoad_.load(std::memory_order_acquire)},
        {"meter", {telMeterL_.load(std::memory_order_acquire), telMeterR_.load(std::memory_order_acquire)}},
        {"sampleRate", sampleRate_.load(std::memory_order_relaxed)},
        {"currentFrame", currentFrame_.load(std::memory_order_relaxed)},
        {"timer", "performance"}
    };
}

void RackEngine::applyPlanOnAudioThread(std::shared_ptr<RackPlan> nextPlan) noexcept {
    if (!nextPlan) return;
    const bool topologyChanged = (nextPlan->patchVersion != appliedPatchVersion_);

    for (auto& mod : nextPlan->modules) {
        if (mod.settings) mod.dsp->applySettings(*mod.settings);
        mod.dsp->setScale(activeScale_.get());
        mod.dsp->setMidiConnected(mod.midiConnected);
        if (topologyChanged) {
            std::fill(mod.dsp->ins.begin(), mod.dsp->ins.end(), 0.0f);
            mod.dsp->patched = mod.patched;
            mod.dsp->outPatched = mod.outPatched;
        }
    }
    if (topologyChanged) {
        rackIns_[0] = rackIns_[1] = 0.0f;
        rackPatched_[0] = nextPlan->rackPatched[0];
        rackPatched_[1] = nextPlan->rackPatched[1];
        appliedPatchVersion_ = nextPlan->patchVersion;
    }
    if (nextPlan->tempo > 0.0) tempo_ = nextPlan->tempo;

    auto old = std::move(activePlan_);
    activePlan_ = std::move(nextPlan);
    if (old) {
        TrashItem item;
        item.plan = std::move(old);
        trashQueue_.push(std::move(item));
    }
}

void RackEngine::sendToChannel(int channel, const DspMessage& msg, bool omni, bool poly) noexcept {
    if (!activePlan_) return;
    for (auto& mod : activePlan_->modules) {
        if (mod.onBus && mod.dsp->midiConnected && mod.dsp->isPoly() == poly &&
            (omni ? (mod.midiChannel == 0) : (mod.midiChannel == channel && channel > 0))) {
            mod.dsp->handleMessage(msg);
        }
    }
}

void RackEngine::handleNoteEvent(int channel, int note, bool on, double vel) noexcept {
    if (!activePlan_) return;
    const int ch = clampi(channel, 1, 16);

    // Polyphonic modules receive raw note events
    for (auto& mod : activePlan_->modules) {
        if (mod.onBus && mod.dsp->midiConnected && mod.dsp->isPoly() &&
            (mod.midiChannel == 0 || mod.midiChannel == ch)) {
            DspMessage m;
            m.type = DspMessage::Type::Note;
            m.note = note;
            m.gate = on;
            m.velocity = vel;
            mod.dsp->handleMessage(m);
        }
    }

    // Monophonic note stacks (channel-specific and OMNI)
    const int keys[2] = {ch, 0};
    for (int k : keys) {
        auto& st = stacks_[k];
        const bool wasPresent = st.remove(note);
        DspMessage m;
        m.type = DspMessage::Type::MidiMono;
        if (on) {
            st.push(note);
            m.hasPitch = true;
            m.pitch = static_cast<double>(note - 60) / 12.0;
            m.gate = true;
            m.retrigger = true;
            m.velocity = vel;
        } else if (st.count > 0) {
            m.hasPitch = true;
            m.pitch = static_cast<double>(st.notes[st.count - 1] - 60) / 12.0;
            m.gate = true;
            m.retrigger = false;
        } else {
            m.gate = false;
        }
        if (on || wasPresent) {
            sendToChannel(ch, m, k == 0, false);
        }
    }
}

void RackEngine::handleAllNotesOff() noexcept {
    for (auto& st : stacks_) st.clear();
    if (!activePlan_) return;
    for (auto& mod : activePlan_->modules) {
        if (!mod.dsp->midiConnected) continue;
        DspMessage m;
        if (mod.dsp->isPoly()) {
            m.type = DspMessage::Type::AllNotesOff;
        } else {
            m.type = DspMessage::Type::MidiMono;
            m.gate = false;
        }
        mod.dsp->handleMessage(m);
    }
}

void RackEngine::handleRtEvent(const RtEvent& ev) noexcept {
    switch (ev.kind) {
        case RtEvent::Kind::NoteOn:
            handleNoteEvent(ev.channel, ev.note, true, ev.value);
            // Also update keys CV zone 0 for DAW MIDI convenience
            kbKey_[0] = static_cast<float>(ev.note - 60) / 12.0f;
            kbGate_[0] = true;
            kbTrig_ = static_cast<int32_t>(std::llround(PULSE_TIME * sampleRate_.load(std::memory_order_relaxed)));
            break;
        case RtEvent::Kind::NoteOff:
            handleNoteEvent(ev.channel, ev.note, false, 0.0);
            if (stacks_[0].count > 0) {
                kbKey_[0] = static_cast<float>(stacks_[0].notes[stacks_[0].count - 1] - 60) / 12.0f;
                kbGate_[0] = true;
            } else {
                kbGate_[0] = false;
            }
            break;
        case RtEvent::Kind::PitchBend:
        case RtEvent::Kind::ModWheel:
        case RtEvent::Kind::Pressure:
        case RtEvent::Kind::PolyPressure:
        case RtEvent::Kind::Sustain: {
            const int ch = clampi(ev.channel, 1, 16);
            DspMessage m;
            if (ev.kind == RtEvent::Kind::PitchBend) m.type = DspMessage::Type::Bend;
            else if (ev.kind == RtEvent::Kind::ModWheel) m.type = DspMessage::Type::ModWheel;
            else if (ev.kind == RtEvent::Kind::Pressure) m.type = DspMessage::Type::Pressure;
            else if (ev.kind == RtEvent::Kind::PolyPressure) m.type = DspMessage::Type::PolyPressure;
            else m.type = DspMessage::Type::Sustain;
            m.value = ev.value;
            m.note = ev.note;
            m.gate = ev.gate;
            for (bool poly : {false, true}) {
                sendToChannel(ch, m, false, poly);
                sendToChannel(ch, m, true, poly);
            }
            break;
        }
        case RtEvent::Kind::AllNotesOff:
            handleAllNotesOff();
            kbGate_[0] = kbGate_[1] = false;
            break;
        case RtEvent::Kind::MidiClock:
        case RtEvent::Kind::MidiStart:
        case RtEvent::Kind::MidiStop:
        case RtEvent::Kind::MidiContinue: {
            if (!activePlan_) break;
            DspMessage m;
            if (ev.kind == RtEvent::Kind::MidiClock) m.type = DspMessage::Type::MidiClock;
            else if (ev.kind == RtEvent::Kind::MidiStart) m.type = DspMessage::Type::MidiStart;
            else if (ev.kind == RtEvent::Kind::MidiStop) m.type = DspMessage::Type::MidiStop;
            else m.type = DspMessage::Type::MidiContinue;
            for (auto& mod : activePlan_->modules) {
                if (mod.dsp->midiConnected) mod.dsp->handleMessage(m);
            }
            break;
        }
        case RtEvent::Kind::KbZone: {
            const int z = ev.zone == 1 ? 1 : 0;
            if (ev.hasPitch) kbKey_[z] = static_cast<float>(ev.value);
            kbGate_[z] = ev.gate;
            break;
        }
        case RtEvent::Kind::KbTrig:
            kbTrig_ = static_cast<int32_t>(std::llround(PULSE_TIME * sampleRate_.load(std::memory_order_relaxed)));
            break;
        case RtEvent::Kind::ModuleMsg:
            if (activePlan_) {
                for (auto& mod : activePlan_->modules) {
                    if (mod.idHash == ev.moduleHash) {
                        mod.dsp->handleMessage(ev.dspMsg);
                        break;
                    }
                }
            }
            break;
        case RtEvent::Kind::IoGain:
            masterGainTarget_ = static_cast<float>(ev.value);
            inputGainTarget_ = static_cast<float>(ev.value2);
            break;
        case RtEvent::Kind::HostTempo:
            if (ev.value > 0.0) tempo_ = ev.value;
            break;
    }
}

void RackEngine::processBlock(
    const float* inL, const float* inR,
    float* outL, float* outR,
    int numFrames,
    const RtEvent* hostEvents, int numHostEvents,
    double hostBpm) {
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Drain scale updates
    std::shared_ptr<Scale> nextScale;
    bool scaleChanged = false;
    while (scaleQueue_.pop(nextScale)) {
        auto old = std::move(activeScale_);
        activeScale_ = std::move(nextScale);
        scaleChanged = true;
        if (old) {
            TrashItem item;
            item.scale = std::move(old);
            trashQueue_.push(std::move(item));
        }
    }

    // 2. Drain rack plan updates (keep newest)
    std::shared_ptr<RackPlan> latestPlan;
    std::shared_ptr<RackPlan> popped;
    while (planQueue_.pop(popped)) {
        if (latestPlan) {
            TrashItem item;
            item.plan = std::move(latestPlan);
            trashQueue_.push(std::move(item));
        }
        latestPlan = std::move(popped);
    }
    if (latestPlan) {
        applyPlanOnAudioThread(std::move(latestPlan));
    } else if (scaleChanged && activePlan_) {
        for (auto& mod : activePlan_->modules) {
            mod.dsp->setScale(activeScale_.get());
        }
    }

    if (hostBpm > 0.0) {
        tempo_ = hostBpm;
    }

    // 3. Drain UI events at start of block
    RtEvent uiEv;
    while (uiEventQueue_.pop(uiEv)) {
        handleRtEvent(uiEv);
    }

    if (!activePlan_) {
        std::memset(outL, 0, sizeof(float) * numFrames);
        if (outR) std::memset(outR, 0, sizeof(float) * numFrames);
        return;
    }

    auto& mods = activePlan_->modules;
    const auto& cables = activePlan_->cables;
    const auto& zeros = activePlan_->zeroTargets;

    for (auto& mod : mods) {
        mod.dsp->beginBlock(numFrames);
    }

    int hostEvIdx = 0;
    float blkPeakL = 0.0f, blkPeakR = 0.0f;

    for (int j = 0; j < numFrames; ++j) {
        // Sample-accurate DAW MIDI / host events
        while (hostEvIdx < numHostEvents && hostEvents[hostEvIdx].sampleOffset <= j) {
            handleRtEvent(hostEvents[hostEvIdx++]);
        }

        // Smooth master / input gains
        masterGain_ += (masterGainTarget_ - masterGain_) * ioSmooth_;
        inputGain_ += (inputGainTarget_ - inputGain_) * ioSmooth_;

        // Audio interface / DAW sidechain input (+-5V front, line level rear)
        const float iL = inL ? (inL[j] * inputGain_) : 0.0f;
        const float iR = inR ? (inR[j] * inputGain_) : iL;
        rackOuts_[O_IN_L] = iL * 5.0f;
        rackOuts_[O_IN_R] = iR * 5.0f;
        rackOuts_[O_IO_IN[0]] = iL;
        rackOuts_[O_IO_IN[1]] = iR;

        // Patch cables (1-sample delay; audio inputs sum all incoming cables)
        for (float* z : zeros) *z = 0.0f;
        for (const auto& c : cables) *c.dst += *c.src;

        // Keys CV outputs
        rackOuts_[O_KB_GATE[0]] = kbGate_[0] ? GATE_V : 0.0f;
        rackOuts_[O_KB_GATE[1]] = kbGate_[1] ? GATE_V : 0.0f;
        rackOuts_[O_KB_KEY[0]] = kbKey_[0];
        rackOuts_[O_KB_KEY[1]] = kbKey_[1];
        if (kbTrig_ > 0) {
            rackOuts_[O_KB_TRIG] = GATE_V;
            kbTrig_--;
        } else {
            rackOuts_[O_KB_TRIG] = 0.0f;
        }

        // Tick all modules
        for (auto& mod : mods) {
            mod.dsp->tick();
        }

        // Soft-limited main output * master gain
        const float yl = fastTanh(rackIns_[I_MAIN[0]]) * masterGain_;
        const float yr = (rackPatched_[I_MAIN[1]] ? fastTanh(rackIns_[I_MAIN[1]]) : fastTanh(rackIns_[I_MAIN[0]])) * masterGain_;

        outL[j] = yl;
        if (outR) outR[j] = yr;

        const float al = std::abs(yl), ar = std::abs(yr);
        if (al > blkPeakL) blkPeakL = al;
        if (ar > blkPeakR) blkPeakR = ar;
    }

    // Drain any trailing host events with offset >= numFrames
    while (hostEvIdx < numHostEvents) {
        handleRtEvent(hostEvents[hostEvIdx++]);
    }

    for (auto& mod : mods) {
        mod.dsp->endBlock(numFrames);
    }

    currentFrame_.fetch_add(static_cast<uint64_t>(numFrames), std::memory_order_relaxed);

    // Master meter peak with gentle decay
    peakL_ = std::max(blkPeakL, peakL_ * 0.92f);
    peakR_ = std::max(blkPeakR, peakR_ * 0.92f);
    telMeterL_.store(peakL_, std::memory_order_release);
    telMeterR_.store(peakR_, std::memory_order_release);

    // Periodic per-module telemetry snapshots
    stateCounter_ += numFrames;
    if (stateCounter_ >= kStateInterval) {
        stateCounter_ = 0;
        for (auto& mod : mods) {
            mod.dsp->snapshotTelemetry();
        }
    }

    // CPU load measurement
    const auto t1 = std::chrono::steady_clock::now();
    busyMs_ += std::chrono::duration<double, std::milli>(t1 - t0).count();
    loadFrames_ += numFrames;
    const double sr = sampleRate_.load(std::memory_order_relaxed);
    if (loadFrames_ >= static_cast<int64_t>(LOAD_INTERVAL * sr)) {
        const double audioMs = (static_cast<double>(loadFrames_) / sr) * 1000.0;
        telCpuLoad_.store(audioMs > 0.0 ? (busyMs_ / audioMs) : 0.0, std::memory_order_release);
        busyMs_ = 0.0;
        loadFrames_ = 0;
    }
}

} // namespace gasfree
