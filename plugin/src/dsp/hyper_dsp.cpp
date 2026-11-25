#include "hyper_dsp.h"

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 5> kInIds = {"in_l", "in_r", "drive_in", "bits_in", "rate_in"};
constexpr std::array<std::string_view, 2> kOutIds = {"out_l", "out_r"};
constexpr int I_L = 0, I_R = 1, I_DRIVE = 2, I_BITS = 3, I_RATE = 4;

int parseType(const std::string& t) {
    if (t == "tube") return 1;
    if (t == "fuzz") return 2;
    if (t == "fold") return 3;
    return 0; // tape
}
} // namespace

std::span<const std::string_view> HyperDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> HyperDSP::outIds() const noexcept { return kOutIds; }

void HyperDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    wl_.fill(0.0f);
    wr_.fill(0.0f);
    wp_ = 0;
    wow_ = flutter_ = 0.0;
    toneL_ = toneR_ = 0.0;
    ratePh_ = holdL_ = holdR_ = 0.0;
    dcL_ = dcR_ = 0.0;
}

std::shared_ptr<const ModuleSettings> HyperDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    if (!j.is_object()) return out;
    if (j.contains("on") && j["on"].is_boolean()) out->on = j["on"].get<bool>();
    if (j.contains("drive") && j["drive"].is_number()) out->drive = clampd(j["drive"].get<double>(), 0.0, 1.0);
    if (j.contains("type") && j["type"].is_string()) out->type = parseType(j["type"].get<std::string>());
    if (j.contains("tone") && j["tone"].is_number()) out->tone = clampd(j["tone"].get<double>(), 300.0, 20000.0);
    if (j.contains("bits") && j["bits"].is_number()) out->bits = clampd(j["bits"].get<double>(), 1.0, 16.0);
    if (j.contains("rate") && j["rate"].is_number()) out->rate = clampd(j["rate"].get<double>(), 100.0, 48000.0);
    if (j.contains("wobble") && j["wobble"].is_number()) out->wobble = clampd(j["wobble"].get<double>(), 0.0, 1.0);
    if (j.contains("mix") && j["mix"].is_number()) out->mix = clampd(j["mix"].get<double>(), 0.0, 1.0);
    if (j.contains("output") && j["output"].is_number()) out->output = clampd(j["output"].get<double>(), 0.0, 1.5);
    return out;
}

void HyperDSP::applySettings(const ModuleSettings& s) {
    s_ = static_cast<const Settings&>(s);
}

void HyperDSP::beginBlock(int) {
    const double sr = sampleRate;
    const double drive = clampd(s_.drive + (patched[I_DRIVE] ? ins[I_DRIVE] / 5.0 : 0.0), 0.0, 1.0);
    gain_ = 1.0 + drive * drive * 60.0;
    makeup_ = 1.0 / (1.0 + drive * 2.2);
    type_ = s_.type;
    tone_ = 1.0 - std::exp(-TWO_PI * std::min(s_.tone, sr * 0.45) / sr);
    bits_ = clampd(s_.bits - (patched[I_BITS] ? ins[I_BITS] * 3.0 : 0.0), 1.0, 16.0);
    levels_ = std::pow(2.0, bits_ - 1.0);
    rate_ = clampd(s_.rate * std::pow(2.0, patched[I_RATE] ? static_cast<double>(ins[I_RATE]) : 0.0), 100.0, sr);
    wobDepth_ = s_.wobble * s_.wobble * 0.004 * sr;
}

double HyperDSP::shape(double x) const noexcept {
    switch (type_) {
        case 1: return fastTanhD(x + 0.3) - 0.2913;
        case 2: return clampd(x * 1.5, -0.8, 0.8) * 1.2;
        case 3: {
            double y = x * 0.5 + 0.25;
            y = y - std::floor(y);
            return (y < 0.5) ? (4.0 * y - 1.0) : (3.0 - 4.0 * y);
        }
        default: return fastTanhD(x);
    }
}

float HyperDSP::tick() {
    const double xl = ins[I_L];
    const double xr = patched[I_R] ? static_cast<double>(ins[I_R]) : xl;
    if (!s_.on) {
        outs[0] = static_cast<float>(xl);
        outs[1] = static_cast<float>(xr);
        return 0.0f;
    }

    double l = xl, r = xr;
    wl_[wp_] = static_cast<float>(xl);
    wr_[wp_] = static_cast<float>(xr);
    if (wobDepth_ > 0.5) {
        wow_ += 0.55 / sampleRate;
        if (wow_ >= 1.0) wow_ -= 1.0;
        flutter_ += 6.3 / sampleRate;
        if (flutter_ >= 1.0) flutter_ -= 1.0;
        const double d = 8.0 + wobDepth_ * (1.0 + std::sin(TWO_PI * wow_) * 0.8 + std::sin(TWO_PI * flutter_) * 0.2);
        double p = static_cast<double>(wp_) - d;
        if (p < 0.0) p += WOB;
        const int i = static_cast<int>(p) & (WOB - 1);
        const double f = p - std::floor(p);
        const int j = (i + 1) & (WOB - 1);
        l = wl_[i] + (wl_[j] - wl_[i]) * f;
        r = wr_[i] + (wr_[j] - wr_[i]) * f;
    }
    wp_ = (wp_ + 1) & (WOB - 1);

    l = shape(l * gain_) * makeup_;
    r = shape(r * gain_) * makeup_;
    dcL_ += (l - dcL_) * 0.0005;
    dcR_ += (r - dcR_) * 0.0005;
    l -= dcL_;
    r -= dcR_;
    toneL_ += (l - toneL_) * tone_;
    toneR_ += (r - toneR_) * tone_;
    l = toneL_;
    r = toneR_;

    ratePh_ += rate_ / sampleRate;
    if (ratePh_ >= 1.0) {
        ratePh_ -= std::floor(ratePh_);
        holdL_ = l;
        holdR_ = r;
    }
    if (rate_ < sampleRate * 0.999) {
        l = holdL_;
        r = holdR_;
    }
    if (bits_ < 15.99) {
        l = std::round(l * levels_) / levels_;
        r = std::round(r * levels_) / levels_;
    }

    const double m = s_.mix;
    outs[0] = static_cast<float>((xl * (1.0 - m) + l * m) * s_.output);
    outs[1] = static_cast<float>((xr * (1.0 - m) + r * m) * s_.output);
    return 0.0f;
}

void HyperDSP::snapshotTelemetry() {
    telBits_.store(bits_, std::memory_order_release);
    telRate_.store(rate_, std::memory_order_release);
}

nlohmann::json HyperDSP::readTelemetryJson() const {
    return {
        {"bits", telBits_.load(std::memory_order_acquire)},
        {"rate", telRate_.load(std::memory_order_acquire)}
    };
}

} // namespace gasfree
