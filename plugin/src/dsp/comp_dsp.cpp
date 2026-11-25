#include "comp_dsp.h"
#include <limits>

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 4> kInIds = {"in_l", "in_r", "sidechain", "scope"};
constexpr std::array<std::string_view, 3> kOutIds = {"out_l", "out_r", "gr"};
constexpr int I_L = 0, I_R = 1, I_SC = 2, I_SCOPE = 3;
constexpr int O_L = 0, O_R = 1, O_GR = 2;
constexpr double SCOPE_ROLL_TIME = 0.2;
} // namespace

CompDSP::CompDSP() = default;

std::span<const std::string_view> CompDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> CompDSP::outIds() const noexcept { return kOutIds; }

void CompDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    env_ = 0.0;
    grPeak_ = 0.0;
    scopeMin_.fill(0.0f);
    scopeMax_.fill(0.0f);
    scopePos_ = 0;
    scopeCount_ = 0;
    scopeTime_ = 0.0;
}

std::shared_ptr<const ModuleSettings> CompDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    if (!j.is_object()) return out;
    if (j.contains("on") && j["on"].is_boolean()) out->on = j["on"].get<bool>();
    if (j.contains("threshold") && j["threshold"].is_number()) out->threshold = clampd(j["threshold"].get<double>(), -60.0, 0.0);
    if (j.contains("ratio") && j["ratio"].is_number()) out->ratio = clampd(j["ratio"].get<double>(), 1.0, 30.0);
    if (j.contains("attack") && j["attack"].is_number()) out->attack = clampd(j["attack"].get<double>(), 0.0001, 1.0);
    if (j.contains("release") && j["release"].is_number()) out->release = clampd(j["release"].get<double>(), 0.001, 3.0);
    if (j.contains("makeup") && j["makeup"].is_number()) out->makeup = clampd(j["makeup"].get<double>(), -12.0, 36.0);
    if (j.contains("mix") && j["mix"].is_number()) out->mix = clampd(j["mix"].get<double>(), 0.0, 1.0);
    if (j.contains("scope_time") && j["scope_time"].is_number()) out->scope_time = clampd(j["scope_time"].get<double>(), 0.001, 10.0);
    return out;
}

void CompDSP::applySettings(const ModuleSettings& s) {
    s_ = static_cast<const Settings&>(s);
}

void CompDSP::beginBlock(int) {
    atk_ = 1.0 - std::exp(-1.0 / (std::max(s_.attack, 0.0001) * sampleRate));
    rel_ = 1.0 - std::exp(-1.0 / (std::max(s_.release, 0.001) * sampleRate));
    slope_ = 1.0 - 1.0 / std::max(s_.ratio, 1.0);
    makeup_ = std::exp(s_.makeup * DB_TO_LN);
}

void CompDSP::scopeFeed(float v, double timeBase) noexcept {
    if (timeBase != scopeTime_) {
        scopeTime_ = timeBase;
        scopeBucket_ = std::max(1, static_cast<int>(std::lround(timeBase * sampleRate / SCOPE_POINTS)));
        scopeMin_.fill(0.0f);
        scopeMax_.fill(0.0f);
        scopeCount_ = 0;
    }
    if (scopeCount_ == 0) {
        scopeLo_ = v;
        scopeHi_ = v;
    } else {
        if (v < scopeLo_) scopeLo_ = v;
        if (v > scopeHi_) scopeHi_ = v;
    }
    if (++scopeCount_ >= scopeBucket_) {
        scopeMin_[scopePos_] = scopeLo_;
        scopeMax_[scopePos_] = scopeHi_;
        scopePos_ = (scopePos_ + 1) & (SCOPE_RING - 1);
        scopeCount_ = 0;
    }
}

float CompDSP::tick() {
    double l = ins[I_L];
    double r = patched[I_R] ? static_cast<double>(ins[I_R]) : l;
    if (s_.on) {
        double det = patched[I_SC] ? (ins[I_SC] / 5.0) : std::max(std::abs(l), std::abs(r));
        if (det < 0.0) det = -det;
        env_ += (det > env_ ? atk_ : rel_) * (det - env_);
        const double over = 20.0 * std::log10(env_ + 1e-9) - s_.threshold;
        double gr = 0.0;
        if (over > 3.0) gr = over * slope_;
        else if (over > -3.0) gr = slope_ * (over + 3.0) * (over + 3.0) / 12.0;
        const double g = (1.0 - s_.mix) + s_.mix * std::exp(-gr * DB_TO_LN) * makeup_;
        l *= g;
        r *= g;
        outs[O_GR] = static_cast<float>(clampd(gr / 4.0, 0.0, 10.0));
        if (gr > grPeak_) grPeak_ = gr;
    } else {
        outs[O_GR] = 0.0f;
    }
    outs[O_L] = static_cast<float>(l);
    outs[O_R] = static_cast<float>(r);
    scopeFeed(patched[I_SCOPE] ? (ins[I_SCOPE] / 5.0f) : static_cast<float>((l + r) * 0.5), s_.scope_time);
    return 0.0f;
}

void CompDSP::snapshotTelemetry() {
    const int w = 1 - telIdx_.load(std::memory_order_relaxed);
    auto& t = tel_[w];
    t.gr = grPeak_;
    grPeak_ = 0.0;
    t.input = patched[I_SCOPE] == 1;

    constexpr int mask = SCOPE_RING - 1;
    constexpr int n = SCOPE_POINTS;
    int start = (scopePos_ - n + SCOPE_RING) & mask;
    if (scopeTime_ <= SCOPE_ROLL_TIME) {
        const int from = (scopePos_ - 2 * n + SCOPE_RING) & mask;
        float lo = std::numeric_limits<float>::infinity();
        float hi = -std::numeric_limits<float>::infinity();
        for (int k = 0; k < 2 * n; ++k) {
            const int i = (from + k) & mask;
            if (scopeMin_[i] < lo) lo = scopeMin_[i];
            if (scopeMax_[i] > hi) hi = scopeMax_[i];
        }
        const float level = (lo + hi) * 0.5f;
        for (int k = 1; k < n; ++k) {
            const int i = (from + k) & mask;
            const int j = (i - 1) & mask;
            const float prev = (scopeMin_[j] + scopeMax_[j]) * 0.5f;
            const float cur = (scopeMin_[i] + scopeMax_[i]) * 0.5f;
            if (prev < level && cur >= level && (hi - lo) > 1e-4f) {
                start = i;
                break;
            }
        }
    }
    for (int k = 0; k < n; ++k) {
        t.min[k] = scopeMin_[(start + k) & mask];
        t.max[k] = scopeMax_[(start + k) & mask];
    }
    telIdx_.store(w, std::memory_order_release);
}

nlohmann::json CompDSP::readTelemetryJson() const {
    const auto& t = tel_[telIdx_.load(std::memory_order_acquire)];
    return {
        {"gr", t.gr},
        {"scope", {
            {"min", t.min},
            {"max", t.max},
            {"input", t.input}
        }}
    };
}

} // namespace gasfree
