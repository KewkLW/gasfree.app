#include "delay_dsp.h"
#include "rack_engine.h"

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 2> kInIds = {"in_l", "in_r"};
constexpr std::array<std::string_view, 2> kOutIds = {"out_l", "out_r"};
constexpr double kDivs[9] = {0.25, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0, 1.5, 2.0, 4.0};
constexpr double MAX_TIME = 5.0;
} // namespace

std::span<const std::string_view> DelayDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> DelayDSP::outIds() const noexcept { return kOutIds; }

void DelayDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    size_ = static_cast<int>(std::ceil(MAX_TIME * sr)) + 4;
    bufL_.assign(size_, 0.0f);
    bufR_.assign(size_, 0.0f);
    pos_ = 0;
    cur_ = -1.0;
    lpL_ = lpR_ = 0.0;
    cleared_ = true;
}

std::shared_ptr<const ModuleSettings> DelayDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    if (!j.is_object()) return out;
    if (j.contains("on") && j["on"].is_boolean()) out->on = j["on"].get<bool>();
    if (j.contains("sync") && j["sync"].is_boolean()) out->sync = j["sync"].get<bool>();
    if (j.contains("div") && j["div"].is_number()) out->div = clampi(static_cast<int>(j["div"].get<double>()), 0, 8);
    if (j.contains("time") && j["time"].is_number()) out->time = clampd(j["time"].get<double>(), 0.001, 4.9);
    if (j.contains("feedback") && j["feedback"].is_number()) out->feedback = clampd(j["feedback"].get<double>(), 0.0, 0.95);
    if (j.contains("tone") && j["tone"].is_number()) out->tone = clampd(j["tone"].get<double>(), 100.0, 20000.0);
    if (j.contains("mix") && j["mix"].is_number()) out->mix = clampd(j["mix"].get<double>(), 0.0, 1.0);
    if (j.contains("pingpong") && j["pingpong"].is_boolean()) out->pingpong = j["pingpong"].get<bool>();
    if (j.contains("dry") && j["dry"].is_boolean()) out->dry = j["dry"].get<bool>();
    return out;
}

void DelayDSP::applySettings(const ModuleSettings& s) {
    s_ = static_cast<const Settings&>(s);
}

double DelayDSP::calcTime() const noexcept {
    if (s_.sync) {
        const double bpm = (host && host->tempo() > 0.0) ? host->tempo() : 120.0;
        const int idx = clampi(s_.div, 0, 8);
        return std::min(MAX_TIME - 0.1, (60.0 / bpm) * kDivs[idx]);
    }
    return clampd(s_.time, 0.001, MAX_TIME - 0.1);
}

void DelayDSP::beginBlock(int) {
    target_ = calcTime() * sampleRate;
    if (cur_ < 0.0) cur_ = target_;
    glide_ = 1.0 - std::exp(-1.0 / (0.05 * sampleRate));
    tone_ = 1.0 - std::exp(-TWO_PI * std::min(s_.tone, sampleRate * 0.45) / sampleRate);
    fb_ = clampd(s_.feedback, 0.0, 0.95);
}

float DelayDSP::readDelay(const std::vector<float>& buf, double d) const noexcept {
    double p = static_cast<double>(pos_) - d;
    while (p < 0.0) p += static_cast<double>(size_);
    int i = static_cast<int>(p);
    if (i >= size_) i -= size_;
    const float f = static_cast<float>(p - std::floor(p));
    const float a = buf[i];
    const float b = buf[(i + 1 < size_) ? (i + 1) : 0];
    return a + (b - a) * f;
}

float DelayDSP::tick() {
    const float xl = ins[0];
    const float xr = patched[1] ? ins[1] : xl;
    if (!s_.on) {
        if (!cleared_) {
            std::fill(bufL_.begin(), bufL_.end(), 0.0f);
            std::fill(bufR_.begin(), bufR_.end(), 0.0f);
            lpL_ = lpR_ = 0.0;
            cleared_ = true;
        }
        outs[0] = xl;
        outs[1] = xr;
        return 0.0f;
    }
    cleared_ = false;
    cur_ += (target_ - cur_) * glide_;
    const double d = cur_ < 1.0 ? 1.0 : cur_;
    lpL_ += (static_cast<double>(readDelay(bufL_, d)) - lpL_) * tone_;
    lpR_ += (static_cast<double>(readDelay(bufR_, d)) - lpR_) * tone_;
    const float wl = static_cast<float>(lpL_);
    const float wr = static_cast<float>(lpR_);
    const float fb = static_cast<float>(fb_);
    if (s_.pingpong) {
        bufL_[pos_] = (xl + xr) * 0.5f + wr * fb;
        bufR_[pos_] = wl * fb;
    } else {
        bufL_[pos_] = xl + wl * fb;
        bufR_[pos_] = xr + wr * fb;
    }
    if (++pos_ >= size_) pos_ = 0;
    const float dry = s_.dry ? 1.0f : 0.0f;
    const float mix = static_cast<float>(s_.mix);
    outs[0] = xl * dry + wl * mix;
    outs[1] = xr * dry + wr * mix;
    return 0.0f;
}

void DelayDSP::snapshotTelemetry() {
    telTime_.store(cur_ / sampleRate, std::memory_order_release);
}

nlohmann::json DelayDSP::readTelemetryJson() const {
    return {{"time", telTime_.load(std::memory_order_acquire)}};
}

} // namespace gasfree
