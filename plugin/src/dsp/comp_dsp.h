#pragma once

#include "module_dsp.h"
#include <array>
#include <atomic>

namespace gasfree {

class CompDSP final : public ModuleDSP {
public:
    CompDSP();

    std::string_view typeName() const noexcept override { return "comp"; }
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
        double threshold = -12.0;
        double ratio = 3.0;
        double attack = 0.005;
        double release = 0.12;
        double makeup = 0.0;
        double mix = 1.0;
        double scope_time = 0.02;
    };

private:
    static constexpr int SCOPE_RING = 1024;
    static constexpr int SCOPE_POINTS = 256;

    void scopeFeed(float v, double timeBase) noexcept;

    struct Telemetry {
        double gr = 0.0;
        bool input = false;
        std::array<float, SCOPE_POINTS> min{};
        std::array<float, SCOPE_POINTS> max{};
    };

    Settings s_;
    double env_ = 0.0;
    double grPeak_ = 0.0;
    double atk_ = 0.0, rel_ = 0.0, slope_ = 0.0, makeup_ = 1.0;

    std::array<float, SCOPE_RING> scopeMin_{};
    std::array<float, SCOPE_RING> scopeMax_{};
    int scopePos_ = 0;
    int scopeCount_ = 0;
    double scopeTime_ = 0.0;
    int scopeBucket_ = 1;
    float scopeLo_ = 0.0f;
    float scopeHi_ = 0.0f;

    Telemetry tel_[2];
    std::atomic<int> telIdx_{0};
};

} // namespace gasfree
