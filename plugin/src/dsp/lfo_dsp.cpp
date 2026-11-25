#include "lfo_dsp.h"

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 4> kInIds = {"rate1", "rate2", "sync1", "sync2"};
constexpr std::array<std::string_view, 2> kOutIds = {"lfo1", "lfo2"};
constexpr int I_RATE[2] = {0, 1};
constexpr int I_SYNC[2] = {2, 3};

int parseShape(const std::string& sh) {
    if (sh == "sine") return 0;
    if (sh == "tri") return 1;
    if (sh == "saw") return 2;
    if (sh == "ramp") return 3;
    if (sh == "square") return 4;
    if (sh == "sh") return 5;
    return 0;
}
} // namespace

LfoDSP::LfoDSP() {
    rng_.seed(0x4C464F31u);
    s_.lfo[0] = {0.5, 0, 1.0};
    s_.lfo[1] = {4.0, 1, 0.5};
    hold_[0] = rng_.nextBipolar();
    hold_[1] = rng_.nextBipolar();
}

std::span<const std::string_view> LfoDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> LfoDSP::outIds() const noexcept { return kOutIds; }

void LfoDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    gateHigh_.fill(0);
}

std::shared_ptr<const ModuleSettings> LfoDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    out->lfo[0] = {0.5, 0, 1.0};
    out->lfo[1] = {4.0, 1, 0.5};
    if (j.is_object() && j.contains("lfo") && j["lfo"].is_array()) {
        for (int i = 0; i < 2 && i < static_cast<int>(j["lfo"].size()); ++i) {
            const auto& c = j["lfo"][i];
            if (!c.is_object()) continue;
            if (c.contains("rate") && c["rate"].is_number()) out->lfo[i].rate = clampd(c["rate"].get<double>(), 0.001, 200.0);
            if (c.contains("shape") && c["shape"].is_string()) out->lfo[i].shape = parseShape(c["shape"].get<std::string>());
            if (c.contains("level") && c["level"].is_number()) out->lfo[i].level = clampd(c["level"].get<double>(), 0.0, 1.0);
        }
    }
    return out;
}

void LfoDSP::applySettings(const ModuleSettings& s) {
    s_ = static_cast<const Settings&>(s);
}

void LfoDSP::beginBlock(int) {
    shape_[0] = s_.lfo[0].shape;
    shape_[1] = s_.lfo[1].shape;
}

bool LfoDSP::gateEdge(int i) noexcept {
    const uint8_t high = ins[i] > 1.0f ? 1 : 0;
    const bool rising = high && !gateHigh_[i];
    gateHigh_[i] = high;
    return rising;
}

float LfoDSP::tick() {
    for (int i = 0; i < 2; ++i) {
        const auto& lfo = s_.lfo[i];
        if (gateEdge(I_SYNC[i])) {
            phase_[i] = 0.0;
            hold_[i] = rng_.nextBipolar();
        }
        double rate = lfo.rate;
        if (patched[I_RATE[i]]) rate *= std::exp(static_cast<double>(ins[I_RATE[i]]) * LN2);
        rate = clampd(rate, 0.001, 200.0);
        double p = phase_[i] + rate / sampleRate;
        if (p >= 1.0) {
            p -= std::floor(p);
            hold_[i] = rng_.nextBipolar();
        }
        phase_[i] = p;
        double v = 0.0;
        switch (shape_[i]) {
            case 0: v = std::sin(TWO_PI * p); break;
            case 1: v = 1.0 - 4.0 * std::abs(p - 0.5); break;
            case 2: v = 1.0 - 2.0 * p; break;
            case 3: v = 2.0 * p - 1.0; break;
            case 4: v = p < 0.5 ? 1.0 : -1.0; break;
            default: v = hold_[i]; break;
        }
        value_[i] = v * lfo.level;
        outs[i] = static_cast<float>(v * lfo.level * 5.0);
    }
    return 0.0f;
}

void LfoDSP::snapshotTelemetry() {
    const int w = 1 - telIdx_.load(std::memory_order_relaxed);
    tel_[w][0] = value_[0];
    tel_[w][1] = value_[1];
    telIdx_.store(w, std::memory_order_release);
}

nlohmann::json LfoDSP::readTelemetryJson() const {
    const int r = telIdx_.load(std::memory_order_acquire);
    return {{"lfo", {tel_[r][0], tel_[r][1]}}};
}

} // namespace gasfree
