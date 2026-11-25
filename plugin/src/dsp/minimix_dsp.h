#pragma once

#include "module_dsp.h"
#include <array>
#include <atomic>

namespace gasfree {

class MinimixDSP final : public ModuleDSP {
public:
    MinimixDSP() = default;

    std::string_view typeName() const noexcept override { return "minimix"; }
    std::span<const std::string_view> inIds() const noexcept override;
    std::span<const std::string_view> outIds() const noexcept override;

    void prepare(double sr) override;
    std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json& j) const override;
    void applySettings(const ModuleSettings& s) override;
    float tick() override;
    void snapshotTelemetry() override;
    nlohmann::json readTelemetryJson() const override;

    static constexpr int CHANNELS = 8;

    struct Settings : ModuleSettings {
        std::array<double, CHANNELS> level = {0.75, 0.75, 0.75, 0.75, 0.75, 0.75, 0.75, 0.75};
        double master = 0.75;
    };

private:
    std::array<float, CHANNELS> gain_{};
    std::array<float, CHANNELS> target_{};
    float master_ = 0.0f;
    float masterTarget_ = 1.0f;
    float smooth_ = 0.01f;
    float peak_ = 0.0f;
    std::atomic<float> telPeak_{0.0f};
};

} // namespace gasfree
