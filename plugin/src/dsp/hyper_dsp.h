#pragma once

#include "module_dsp.h"
#include <array>
#include <atomic>

namespace gasfree {

class HyperDSP final : public ModuleDSP {
public:
    HyperDSP() = default;

    std::string_view typeName() const noexcept override { return "hyper"; }
    std::span<const std::string_view> inIds() const noexcept override;
    std::span<const std::string_view> outIds() const noexcept override;

    void prepare(double sr) override;
    std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json& j) const override;
    void applySettings(const ModuleSettings& s) override;
    void beginBlock(int numSamples) override;
    float tick() override;
    void snapshotTelemetry() override;
    nlohmann::json readTelemetryJson() const override;

    struct Settings : ModuleSettings {
        bool on = true;
        double drive = 0.35;
        int type = 0; // 0=tape, 1=tube, 2=fuzz, 3=fold
        double tone = 9000.0;
        double bits = 8.0;
        double rate = 16000.0;
        double wobble = 0.15;
        double mix = 1.0;
        double output = 0.9;
    };

private:
    static constexpr int WOB = 2048;

    double shape(double x) const noexcept;

    Settings s_;
    std::array<float, WOB> wl_{};
    std::array<float, WOB> wr_{};
    int wp_ = 0;
    double wow_ = 0.0, flutter_ = 0.0;
    double toneL_ = 0.0, toneR_ = 0.0;
    double ratePh_ = 0.0, holdL_ = 0.0, holdR_ = 0.0;
    double dcL_ = 0.0, dcR_ = 0.0;

    double gain_ = 1.0, makeup_ = 1.0, tone_ = 1.0, bits_ = 8.0, levels_ = 128.0, rate_ = 16000.0, wobDepth_ = 0.0;
    int type_ = 0;

    std::atomic<double> telBits_{8.0};
    std::atomic<double> telRate_{16000.0};
};

} // namespace gasfree
