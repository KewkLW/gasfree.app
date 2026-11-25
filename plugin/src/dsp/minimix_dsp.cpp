#include "minimix_dsp.h"

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 8> kInIds = {"in1", "in2", "in3", "in4", "in5", "in6", "in7", "in8"};
constexpr std::array<std::string_view, 2> kOutIds = {"main_l", "main_r"};

float knobGain(double v) noexcept {
    const double g = v / 0.75;
    return static_cast<float>(g * g);
}
} // namespace

std::span<const std::string_view> MinimixDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> MinimixDSP::outIds() const noexcept { return kOutIds; }

void MinimixDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    gain_.fill(0.0f);
    target_.fill(1.0f);
    master_ = 0.0f;
    masterTarget_ = 1.0f;
    smooth_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sr)));
    peak_ = 0.0f;
}

std::shared_ptr<const ModuleSettings> MinimixDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    if (!j.is_object()) return out;
    if (j.contains("level") && j["level"].is_array()) {
        for (int i = 0; i < CHANNELS && i < static_cast<int>(j["level"].size()); ++i)
            if (j["level"][i].is_number()) out->level[i] = clampd(j["level"][i].get<double>(), 0.0, 1.0);
    }
    if (j.contains("master") && j["master"].is_number()) out->master = clampd(j["master"].get<double>(), 0.0, 1.0);
    return out;
}

void MinimixDSP::applySettings(const ModuleSettings& s) {
    const auto& st = static_cast<const Settings&>(s);
    for (int i = 0; i < CHANNELS; ++i) target_[i] = knobGain(st.level[i]);
    masterTarget_ = knobGain(st.master);
}

float MinimixDSP::tick() {
    float sum = 0.0f;
    for (int i = 0; i < CHANNELS; ++i) {
        gain_[i] += (target_[i] - gain_[i]) * smooth_;
        sum += ins[i] * gain_[i];
    }
    master_ += (masterTarget_ - master_) * smooth_;
    const float y = sum * master_;
    outs[0] = y;
    outs[1] = y;
    const float a = std::abs(y);
    if (a > peak_) peak_ = a;
    return 0.0f;
}

void MinimixDSP::snapshotTelemetry() {
    telPeak_.store(peak_, std::memory_order_release);
    peak_ = 0.0f;
}

nlohmann::json MinimixDSP::readTelemetryJson() const {
    return {{"level", telPeak_.load(std::memory_order_acquire)}};
}

} // namespace gasfree
