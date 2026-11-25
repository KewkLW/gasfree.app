#pragma once

#include "module_dsp.h"
#include <atomic>
#include <vector>

namespace gasfree {

class DelayDSP final : public ModuleDSP {
public:
    DelayDSP() = default;

    std::string_view typeName() const noexcept override { return "delay"; }
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
        bool on = false;
        bool sync = true;
        int div = 3;
        double time = 0.3;
        double feedback = 0.4;
        double tone = 5000.0;
        double mix = 0.3;
        bool pingpong = true;
        bool dry = true;
    };

private:
    double calcTime() const noexcept;
    float readDelay(const std::vector<float>& buf, double d) const noexcept;

    Settings s_;
    int size_ = 0;
    std::vector<float> bufL_;
    std::vector<float> bufR_;
    int pos_ = 0;
    double cur_ = -1.0;
    double target_ = 0.0;
    double glide_ = 0.0;
    double tone_ = 0.0;
    double fb_ = 0.0;
    double lpL_ = 0.0;
    double lpR_ = 0.0;
    bool cleared_ = true;

    std::atomic<double> telTime_{0.3};
};

} // namespace gasfree
