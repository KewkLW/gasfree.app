#pragma once

#include "module_dsp.h"
#include <array>
#include <atomic>

namespace gasfree {

class MixerDSP final : public ModuleDSP {
public:
    MixerDSP() = default;

    std::string_view typeName() const noexcept override { return "mixer"; }
    std::span<const std::string_view> inIds() const noexcept override;
    std::span<const std::string_view> outIds() const noexcept override;

    void prepare(double sr) override;
    std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json& j) const override;
    void applySettings(const ModuleSettings& s) override;
    void beginBlock(int numSamples) override;
    float tick() override;
    void snapshotTelemetry() override;
    nlohmann::json readTelemetryJson() const override;

    static constexpr int CHANNELS = 6;

    struct ChanSettings {
        double level = 0.75;
        double pan = 0.0;
        double aux = 0.0;
        bool mute = false;
        bool solo = false;
    };

    struct Settings : ModuleSettings {
        std::array<ChanSettings, CHANNELS> ch{};
        double master = 0.75;
        double aux_send = 0.75;
        double aux_return = 0.75;
    };

private:
    struct Telemetry {
        std::array<float, CHANNELS> level{};
        std::array<float, 2> main{};
    };

    Settings s_;
    std::array<float, CHANNELS> gain_{};
    std::array<float, CHANNELS> gl_{};
    std::array<float, CHANNELS> gr_{};
    std::array<float, CHANNELS> ga_{};
    std::array<float, CHANNELS> peak_{};
    std::array<float, 2> peakMain_{};
    float master_ = 0.0f;
    float ret_ = 0.0f;
    float auxMaster_ = 0.0f;

    Telemetry tel_[2];
    std::atomic<int> telIdx_{0};
};

} // namespace gasfree
