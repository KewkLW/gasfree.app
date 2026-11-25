#pragma once

#include "module_dsp.h"
#include <array>
#include <atomic>

namespace gasfree {

class SubharmDSP final : public ModuleDSP {
public:
    SubharmDSP();

    std::string_view typeName() const noexcept override { return "subharm"; }
    std::span<const std::string_view> inIds() const noexcept override;
    std::span<const std::string_view> outIds() const noexcept override;
    std::string_view midiInId() const noexcept override { return "midi_in"; }

    void prepare(double sr) override;
    std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json& j) const override;
    void applySettings(const ModuleSettings& s) override;
    void setScale(const Scale* scale) override;
    void handleMessage(const DspMessage& msg) override;
    void setMidiConnected(bool connected) override;
    void beginBlock(int numSamples) override;
    float tick() override;
    void endBlock(int numSamples) override;
    void snapshotTelemetry() override;
    nlohmann::json readTelemetryJson() const override;

    struct VcoSettings {
        double freq = 261.63;
        int wave = 2; // 0=square, 1=pwm, 2=saw
        int sub1 = 2;
        int sub2 = 4;
        double level = 0.55;
        double sub1_level = 0.6;
        double sub2_level = 0.45;
    };

    struct SeqAssign {
        bool osc = true;
        bool sub1 = false;
        bool sub2 = false;
    };

    struct Settings : ModuleSettings {
        VcoSettings vco[2];
        double seq[2][4] = {{0, 0.25, 5.0 / 12.0, 7.0 / 12.0}, {0, -0.25, 2.0 / 12.0, -1.0 / 12.0}};
        SeqAssign seq_assign[2];
        double seq_oct = 1.0;
        int quantize = 1; // 0=OFF, 1=12-ET, 2=8-ET, 3=12-JI, 4=8-JI, 5=SCL
        double tempo = 300.0;
        int rhythm[4] = {2, 3, 5, 7};
        bool rhythm_seq1[4] = {true, false, true, false};
        bool rhythm_seq2[4] = {false, true, false, true};
        double cutoff = 900.0;
        double resonance = 0.35;
        double vcf_eg_amt = 0.45;
        double vcf_attack = 0.004;
        double vcf_decay = 0.35;
        double vca_attack = 0.004;
        double vca_decay = 0.6;
        double volume = 0.7;
        bool eg_hold = false;
    };

private:
    struct VcoState {
        double phase = 0.0;
        int cnt1 = 0, cnt2 = 0;
        bool hasKnob = false;
        double knob = 0.0;
        double drift = 0.0;
        double pitch = 0.0;
        double freq = C4_FREQ;
        double prePitch = 0.0;
        const Scale* scale = nullptr;
        double lastCv = 1e30;
        double lastQ = 0.0;
        int div1 = 1, div2 = 1;
        int curDiv1 = 1, curDiv2 = 1;
    };

    struct EgState {
        double level = 0.0;
        int stage = 0; // 0=idle, 1=attack, 2=hold, 3=decay
    };

    struct Telemetry {
        bool playing = false;
        int step[2] = {0, 0};
        bool rhythm[4] = {false, false, false, false};
        bool clock = false;
        int clockSource = 0; // 0=internal, 1=midi, 2=patch
        double freq[2] = {C4_FREQ, C4_FREQ};
        int div[2][2] = {{1, 1}, {1, 1}};
        double vca_eg = 0.0;
        double vcf_eg = 0.0;
    };

    void setPlaying(bool v) noexcept;
    void resetSeq() noexcept;
    void advance(int s) noexcept;
    void triggerEGs() noexcept;
    void masterTick() noexcept;
    void updateControl(bool smoothKnob) noexcept;
    double stepValue(int i) const noexcept;
    void stepEg(EgState& eg, double ac, double dc, bool hold) noexcept;
    bool gateEdge(int idx) noexcept;

    Settings s_;
    FastRng rng_;
    std::array<uint8_t, 24> gateHigh_{};
    std::array<int32_t, 16> pulses_{};

    VcoState vcos_[2];
    double lp_[4] = {0, 0, 0, 0};
    bool hasSmooth_ = false;
    double smLv_[6] = {0, 0, 0, 0, 0, 0};
    double smCut_ = 0.0;
    double smRes_ = 0.0;
    double smVol_ = 0.0;
    double fbk_ = 0.0;
    double resComp_ = 1.0;
    int waveMode_[2] = {2, 0};
    double vcaAc_ = 0.0, vcaDc_ = 0.0;
    double vcfAc_ = 0.0, vcfDc_ = 0.0;
    double egAmt_ = 0.0;

    EgState vcaEg_;
    EgState vcfEg_;

    bool playing_ = false;
    double clockPhase_ = 0.0;
    int rhythmCnt_[4] = {0, 0, 0, 0};
    int rhythmFlash_[4] = {0, 0, 0, 0};
    int clockFlash_ = 0;
    int step_[2] = {0, 0};
    bool stepPending_[2] = {true, true};
    bool controlDirty_ = true;

    bool holdButton_ = false;
    bool holdMidi_ = false;
    bool holdInput_ = false;
    double midiPitch_ = 0.0;
    double bend_ = 0.0;
    int midiTicks_ = 0;
    int midiClockCount_ = 0;
    int64_t lastMidiClock_ = -1000000000000LL;
    int64_t sampleCount_ = 0;
    int clockSource_ = 0;
    bool extClock_ = false;
    bool internalClock_ = true;
    double clockInc_ = 0.0;
    double twoPiOver2sr_ = 0.0;

    Telemetry tel_[2];
    std::atomic<int> telIdx_{0};
};

} // namespace gasfree
