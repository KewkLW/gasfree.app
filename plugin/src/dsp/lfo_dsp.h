#pragma once

#include "module_dsp.h"
#include <array>
#include <atomic>

namespace gasfree {

class LfoDSP final : public ModuleDSP {
public:
    LfoDSP();

    std::string_view typeName() const noexcept override { return "lfo"; }
    std::span<const std::string_view> inIds() const noexcept override;
    std::span<const std::string_view> outIds() const noexcept override;

    void prepare(double sr) override;
    std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json& j) const override;
    void applySettings(const ModuleSettings& s) override;
    void beginBlock(int numSamples) override;
    float tick() override;
    void snapshotTelemetry() override;
    nlohmann::json readTelemetryJson() const override;

    struct LfoChan {
        double rate = 0.5;
        int shape = 0; // 0=sine, 1=tri, 2=saw, 3=ramp, 4=square, 5=sh
        double level = 1.0;
    };

    struct Settings : ModuleSettings {
        LfoChan lfo[2];
    };

private:
    bool gateEdge(int i) noexcept;

    Settings s_;
    FastRng rng_;
    std::array<uint8_t, 4> gateHigh_{};
    double phase_[2] = {0.0, 0.0};
    double hold_[2] = {0.0, 0.0};
    double value_[2] = {0.0, 0.0};
    int shape_[2] = {0, 1};

    double tel_[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    std::atomic<int> telIdx_{0};
};

} // namespace gasfree
