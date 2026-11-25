#pragma once

#include "fft_convolver.h"
#include "module_dsp.h"
#include <string>
#include <vector>

namespace gasfree {

// Native stereo convolution reverb replacing WebAudio ConvolverNode + 8-slot bridge.
// Impulse responses are loaded/prepared on the UI/control thread inside parseSettings()
// and cached per room+sampleRate; applySettings() installs the prepared IR pointer.
class ReverbDSP final : public ModuleDSP {
public:
    explicit ReverbDSP(std::string appDir = {});

    std::string_view typeName() const noexcept override { return "reverb"; }
    std::span<const std::string_view> inIds() const noexcept override;
    std::span<const std::string_view> outIds() const noexcept override;

    void prepare(double sr) override;
    std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json& j) const override;
    void applySettings(const ModuleSettings& s) override;
    void beginBlock(int numSamples) override;
    float tick() override;

    struct Settings : ModuleSettings {
        bool on = true;
        double mix = 0.15;
        std::string room = "HALL B";
        double predelay = 0.02;
        double tone = 12000.0;
        bool dry = true;
        std::shared_ptr<const PreparedConvolverIR> ir;
    };

    static std::shared_ptr<const PreparedConvolverIR> loadRoomIR(
        const std::string& appDir, const std::string& room, double sampleRate);

private:
    std::string appDir_;
    Settings s_;
    StereoPartitionedConvolver convolver_{512};

    std::vector<float> preL_;
    std::vector<float> preR_;
    int preSize_ = 0;
    int prePos_ = 0;
    double preCur_ = 0.0;
    double preTarget_ = 0.0;

    // 2-pole lowpass (matches WebAudio BiquadFilterNode lowpass)
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1L_ = 0.0, z2L_ = 0.0, z1R_ = 0.0, z2R_ = 0.0;
    double wetGain_ = 0.0, wetTarget_ = 0.0;
    double dryGain_ = 1.0, dryTarget_ = 1.0;
    double smooth_ = 0.0;
    const PreparedConvolverIR* activeIr_ = nullptr;
};

} // namespace gasfree
