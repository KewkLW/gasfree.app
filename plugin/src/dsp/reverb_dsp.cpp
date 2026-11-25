#include "reverb_dsp.h"

#include <cmath>
#include <mutex>
#include <unordered_map>

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 2> kInIds = {"in_l", "in_r"};
constexpr std::array<std::string_view, 2> kOutIds = {"out_l", "out_r"};

struct RoomEntry {
    const char* name;
    const char* relPath; // nullptr => synthetic plate
};

constexpr RoomEntry kRooms[] = {
    {"DRUM ROOM S", "data/impulses/Small Drum Room.wav"},
    {"DRUM ROOM",   "data/impulses/Nice Drum Room.wav"},
    {"CAVE",        "data/impulses/Small Prehistoric Cave.wav"},
    {"SALON",       "data/impulses/French 18th Century Salon.wav"},
    {"PLATE",       nullptr},
    {"OPERA HALL",  "data/impulses/Scala Milan Opera Hall.wav"},
    {"SILO",        "data/impulses/In The Silo.wav"},
    {"HALL A",      "data/BIG HALL E002 M2S.wav"},
    {"HALL B",      "data/BIG HALL E003 M2S.wav"},
    {"GARAGE",      "data/impulses/Parking Garage.wav"},
    {"ECHO HALL",   "data/impulses/Large Long Echo Hall.wav"},
    {"CHURCH",      "data/impulses/St Nicolaes Church.wav"},
};

const RoomEntry* findRoom(const std::string& name) {
    for (const auto& r : kRooms) {
        if (name == r.name) return &r;
    }
    return &kRooms[8]; // HALL B default
}
} // namespace

std::shared_ptr<const PreparedConvolverIR> ReverbDSP::loadRoomIR(
    const std::string& appDir, const std::string& roomName, double sr) {
    static std::mutex cacheMu;
    static std::unordered_map<std::string, std::shared_ptr<const PreparedConvolverIR>> cache;

    const auto* entry = findRoom(roomName);
    const std::string key = std::string(entry->name) + "@" + std::to_string(static_cast<int>(std::llround(sr)));

    {
        std::lock_guard<std::mutex> lk(cacheMu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    ImpulseBuffer buf;
    bool ok = false;
    if (entry->relPath && !appDir.empty()) {
        std::string fullPath = appDir + "/" + entry->relPath;
        ok = loadWavFile(fullPath, buf, sr);
    }
    if (!ok) {
        buf = makeSyntheticPlate(sr, 2.6);
    }

    auto prep = prepareConvolverIR(buf, 512);
    {
        std::lock_guard<std::mutex> lk(cacheMu);
        cache[key] = prep;
    }
    return prep;
}

ReverbDSP::ReverbDSP(std::string appDir) : appDir_(std::move(appDir)) {
    if (appDir_.empty()) appDir_ = GASFREE_DEFAULT_APP_DIR;
}

std::span<const std::string_view> ReverbDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> ReverbDSP::outIds() const noexcept { return kOutIds; }

void ReverbDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    preSize_ = static_cast<int>(std::ceil(1.0 * sr)) + 4;
    preL_.assign(preSize_, 0.0f);
    preR_.assign(preSize_, 0.0f);
    prePos_ = 0;
    preCur_ = s_.predelay * sr;
    z1L_ = z2L_ = z1R_ = z2R_ = 0.0;
    smooth_ = 1.0 - std::exp(-1.0 / (0.03 * sr));
    if (!s_.ir) {
        s_.ir = loadRoomIR(appDir_, s_.room, sr);
    }
    convolver_.setIR(s_.ir);
    activeIr_ = s_.ir.get();
}

std::shared_ptr<const ModuleSettings> ReverbDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    if (j.is_object()) {
        if (j.contains("on") && j["on"].is_boolean()) out->on = j["on"].get<bool>();
        if (j.contains("mix") && j["mix"].is_number()) out->mix = clampd(j["mix"].get<double>(), 0.0, 1.0);
        if (j.contains("room")) {
            if (j["room"].is_string()) out->room = j["room"].get<std::string>();
            else if (j["room"].is_number()) out->room = (j["room"].get<int>() == 0) ? "HALL A" : "HALL B";
        }
        if (j.contains("predelay") && j["predelay"].is_number()) out->predelay = clampd(j["predelay"].get<double>(), 0.0, 0.5);
        if (j.contains("tone") && j["tone"].is_number()) out->tone = clampd(j["tone"].get<double>(), 200.0, 20000.0);
        if (j.contains("dry") && j["dry"].is_boolean()) out->dry = j["dry"].get<bool>();
    }
    out->ir = loadRoomIR(appDir_, out->room, sampleRate > 0.0 ? sampleRate : 48000.0);
    return out;
}

void ReverbDSP::applySettings(const ModuleSettings& s) {
    s_ = static_cast<const Settings&>(s);
    if (s_.ir && s_.ir.get() != activeIr_) {
        convolver_.setIR(s_.ir);
        activeIr_ = s_.ir.get();
    }
}

void ReverbDSP::beginBlock(int) {
    preTarget_ = clampd(s_.predelay * sampleRate, 0.0, static_cast<double>(preSize_ - 4));
    wetTarget_ = s_.on ? (s_.mix * s_.mix * 0.8) : 0.0;
    dryTarget_ = (s_.on && !s_.dry) ? 0.0 : 1.0;

    // 2-pole Butterworth lowpass (Q = 0.7071) matching WebAudio BiquadFilterNode default
    const double fc = clampd(s_.tone, 100.0, sampleRate * 0.45);
    const double w0 = TWO_PI * fc / sampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 - cosw) * 0.5 / a0;
    b1_ = (1.0 - cosw) / a0;
    b2_ = b0_;
    a1_ = (-2.0 * cosw) / a0;
    a2_ = (1.0 - alpha) / a0;
}

float ReverbDSP::tick() {
    const float xl = ins[0];
    const float xr = patched[1] ? ins[1] : xl;

    preCur_ += (preTarget_ - preCur_) * smooth_;
    wetGain_ += (wetTarget_ - wetGain_) * smooth_;
    dryGain_ += (dryTarget_ - dryGain_) * smooth_;

    preL_[prePos_] = xl;
    preR_[prePos_] = xr;

    double rp = static_cast<double>(prePos_) - preCur_;
    while (rp < 0.0) rp += static_cast<double>(preSize_);
    int idx = static_cast<int>(rp);
    if (idx >= preSize_) idx -= preSize_;
    const float frac = static_cast<float>(rp - std::floor(rp));
    const int idx1 = (idx + 1 < preSize_) ? (idx + 1) : 0;
    const float pl = preL_[idx] + (preL_[idx1] - preL_[idx]) * frac;
    const float pr = preR_[idx] + (preR_[idx1] - preR_[idx]) * frac;
    if (++prePos_ >= preSize_) prePos_ = 0;

    float cl = 0.0f, cr = 0.0f;
    convolver_.processSample(pl, pr, cl, cr);

    // Tone biquad (Direct Form II Transposed)
    const double yl = b0_ * cl + z1L_;
    z1L_ = b1_ * cl - a1_ * yl + z2L_;
    z2L_ = b2_ * cl - a2_ * yl;

    const double yr = b0_ * cr + z1R_;
    z1R_ = b1_ * cr - a1_ * yr + z2R_;
    z2R_ = b2_ * cr - a2_ * yr;

    outs[0] = static_cast<float>(xl * dryGain_ + yl * wetGain_);
    outs[1] = static_cast<float>(xr * dryGain_ + yr * wetGain_);
    return 0.0f;
}

} // namespace gasfree
