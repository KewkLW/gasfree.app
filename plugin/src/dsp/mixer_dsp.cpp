#include "mixer_dsp.h"

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 14> kInIds = {
    "in1", "in2", "in3", "in4", "in5", "in6",
    "aux_ret_l", "aux_ret_r",
    "cv1", "cv2", "cv3", "cv4", "cv5", "cv6"
};
constexpr std::array<std::string_view, 4> kOutIds = {"main_l", "main_r", "aux_send", "mix"};
constexpr int I_RET_L = 6, I_RET_R = 7, I_CV = 8;
constexpr int O_MAIN_L = 0, O_MAIN_R = 1, O_AUX = 2, O_MIX = 3;

float faderGain(double pos) noexcept {
    const double g = pos / 0.75;
    return static_cast<float>(g * g * g);
}

float knobGain(double v) noexcept {
    const double g = v / 0.75;
    return static_cast<float>(g * g);
}
} // namespace

std::span<const std::string_view> MixerDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> MixerDSP::outIds() const noexcept { return kOutIds; }

void MixerDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    gain_.fill(0.0f);
    peak_.fill(0.0f);
    peakMain_ = {0.0f, 0.0f};
}

std::shared_ptr<const ModuleSettings> MixerDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    if (!j.is_object()) return out;
    if (j.contains("ch") && j["ch"].is_array()) {
        for (int i = 0; i < CHANNELS && i < static_cast<int>(j["ch"].size()); ++i) {
            const auto& c = j["ch"][i];
            if (!c.is_object()) continue;
            if (c.contains("level") && c["level"].is_number()) out->ch[i].level = clampd(c["level"].get<double>(), 0.0, 1.0);
            if (c.contains("pan") && c["pan"].is_number()) out->ch[i].pan = clampd(c["pan"].get<double>(), -1.0, 1.0);
            if (c.contains("aux") && c["aux"].is_number()) out->ch[i].aux = clampd(c["aux"].get<double>(), 0.0, 1.0);
            if (c.contains("mute") && c["mute"].is_boolean()) out->ch[i].mute = c["mute"].get<bool>();
            if (c.contains("solo") && c["solo"].is_boolean()) out->ch[i].solo = c["solo"].get<bool>();
        }
    }
    if (j.contains("master") && j["master"].is_number()) out->master = clampd(j["master"].get<double>(), 0.0, 1.0);
    if (j.contains("aux_send") && j["aux_send"].is_number()) out->aux_send = clampd(j["aux_send"].get<double>(), 0.0, 1.0);
    if (j.contains("aux_return") && j["aux_return"].is_number()) out->aux_return = clampd(j["aux_return"].get<double>(), 0.0, 1.0);
    return out;
}

void MixerDSP::applySettings(const ModuleSettings& s) {
    s_ = static_cast<const Settings&>(s);
}

void MixerDSP::beginBlock(int) {
    bool anySolo = false;
    for (int i = 0; i < CHANNELS; ++i) {
        if (s_.ch[i].solo) { anySolo = true; break; }
    }
    for (int i = 0; i < CHANNELS; ++i) {
        const auto& c = s_.ch[i];
        const bool on = !c.mute && (!anySolo || c.solo);
        const float target = on ? faderGain(c.level) : 0.0f;
        gain_[i] += (target - gain_[i]) * 0.3f;
        gl_[i] = static_cast<float>(std::min(1.0, 1.0 - c.pan));
        gr_[i] = static_cast<float>(std::min(1.0, 1.0 + c.pan));
        ga_[i] = knobGain(c.aux);
    }
    master_ += (faderGain(s_.master) - master_) * 0.3f;
    ret_ += (knobGain(s_.aux_return) - ret_) * 0.3f;
    auxMaster_ += (knobGain(s_.aux_send) - auxMaster_) * 0.3f;
}

float MixerDSP::tick() {
    float l = 0.0f, r = 0.0f, aux = 0.0f;
    for (int i = 0; i < CHANNELS; ++i) {
        float g = gain_[i];
        if (patched[I_CV + i]) {
            const float cv = clampf(ins[I_CV + i] / 5.0f, 0.0f, 1.5f);
            g *= cv;
        }
        const float v = ins[i] * g;
        const float a = std::abs(v);
        if (a > peak_[i]) peak_[i] = a;
        l += v * gl_[i];
        r += v * gr_[i];
        aux += v * ga_[i];
    }
    const float retL = ins[I_RET_L];
    const float retR = patched[I_RET_R] ? ins[I_RET_R] : retL;
    l = (l + retL * ret_) * master_;
    r = (r + retR * ret_) * master_;
    outs[O_MAIN_L] = l;
    outs[O_MAIN_R] = r;
    outs[O_AUX] = aux * auxMaster_;
    outs[O_MIX] = (l + r) * 2.5f;
    const float al = std::abs(l), ar = std::abs(r);
    if (al > peakMain_[0]) peakMain_[0] = al;
    if (ar > peakMain_[1]) peakMain_[1] = ar;
    return 0.0f;
}

void MixerDSP::snapshotTelemetry() {
    const int w = 1 - telIdx_.load(std::memory_order_relaxed);
    tel_[w].level = peak_;
    tel_[w].main = peakMain_;
    peak_.fill(0.0f);
    peakMain_ = {0.0f, 0.0f};
    telIdx_.store(w, std::memory_order_release);
}

nlohmann::json MixerDSP::readTelemetryJson() const {
    const auto& t = tel_[telIdx_.load(std::memory_order_acquire)];
    return {
        {"level", t.level},
        {"main", t.main}
    };
}

} // namespace gasfree
