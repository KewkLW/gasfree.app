#include "subharm_dsp.h"

#include <cmath>

namespace gasfree {

namespace {
constexpr std::array<std::string_view, 16> kOutIds = {
    "vco1", "vco1_sub1", "vco1_sub2", "vco2", "vco2_sub1", "vco2_sub2",
    "vca", "vca_eg", "vcf_eg", "seq1", "seq1_clk", "seq2", "seq2_clk",
    "clock_out", "trigger_out", "audio"
};

constexpr std::array<std::string_view, 24> kInIds = {
    "vco1_in", "vco1_sub_in", "vco1_pwm", "vca_in",
    "vco2_in", "vco2_sub_in", "vco2_pwm", "cutoff_in",
    "play_in", "reset_in", "trigger_in",
    "rhythm1_in", "rhythm2_in", "rhythm3_in", "rhythm4_in",
    "clock_in",
    "seq1_s1", "seq1_s2", "seq1_s3", "seq1_s4",
    "seq2_s1", "seq2_s2", "seq2_s3", "seq2_s4"
};

constexpr int O_VCO[2] = {0, 3};
constexpr int O_VCA = 6, O_VCA_EG = 7, O_VCF_EG = 8;
constexpr int O_SEQ[2] = {9, 11};
constexpr int O_SEQ_CLK[2] = {10, 12};
constexpr int O_CLOCK = 13, O_TRIG = 14, O_AUDIO = 15;

constexpr int I_VCO_PITCH[2] = {0, 4};
constexpr int I_VCO_SUB[2] = {1, 5};
constexpr int I_VCO_PWM[2] = {2, 6};
constexpr int I_VCA = 3, I_CUTOFF = 7, I_PLAY = 8, I_RESET = 9, I_TRIG = 10, I_RHYTHM = 11, I_CLOCK = 15;
constexpr int I_STEP[2] = {16, 20};
constexpr int PULSE_OUTS[4] = {10, 12, 13, 14};

constexpr double GATE_V = 5.0;
constexpr double GATE_THRESHOLD = 1.0;
constexpr double PULSE_TIME = 0.005;
constexpr double SUB_DIV_PER_VOLT = 3.0;
constexpr double CUTOFF_MIN = 20.0;
constexpr double EG_CUTOFF_OCTAVES = 7.0;
constexpr double MIDI_CLOCK_TIMEOUT = 0.5;
constexpr double ATTACK_TARGET = 1.3;
const double ATTACK_TC = std::log(ATTACK_TARGET / (ATTACK_TARGET - 1.0));
const double DECAY_TC = std::log(1.0 / 0.01);

const Scale& builtinScale(int idx) {
    static const Scale k12ET = {{0, 1.0/12, 2.0/12, 3.0/12, 4.0/12, 5.0/12, 6.0/12, 7.0/12, 8.0/12, 9.0/12, 10.0/12, 11.0/12}, 1.0};
    static const Scale k8ET = {{0, 2.0/12, 4.0/12, 5.0/12, 7.0/12, 9.0/12, 11.0/12}, 1.0};
    static const Scale k12JI = {{
        std::log2(1.0), std::log2(16.0/15.0), std::log2(9.0/8.0), std::log2(6.0/5.0),
        std::log2(5.0/4.0), std::log2(4.0/3.0), std::log2(45.0/32.0), std::log2(3.0/2.0),
        std::log2(8.0/5.0), std::log2(5.0/3.0), std::log2(9.0/5.0), std::log2(15.0/8.0)
    }, 1.0};
    static const Scale k8JI = {{
        std::log2(1.0), std::log2(9.0/8.0), std::log2(5.0/4.0), std::log2(4.0/3.0),
        std::log2(3.0/2.0), std::log2(5.0/3.0), std::log2(15.0/8.0)
    }, 1.0};

    switch (idx) {
        case 1: return k12ET;
        case 2: return k8ET;
        case 3: return k12JI;
        case 4: return k8JI;
        default: return k12ET;
    }
}

int parseWave(const std::string& w) {
    if (w == "square") return 0;
    if (w == "pwm") return 1;
    return 2; // saw
}

int parseQuantize(const std::string& q) {
    if (q == "12-ET") return 1;
    if (q == "8-ET") return 2;
    if (q == "12-JI") return 3;
    if (q == "8-JI") return 4;
    if (q == "SCL") return 5;
    return 0;
}

} // namespace

SubharmDSP::SubharmDSP() {
    rng_.seed(0x53554248u);
    s_.vco[0] = {261.63, 2, 2, 4, 0.55, 0.6, 0.45};
    s_.vco[1] = {392.00, 0, 4, 6, 0.35, 0.4, 0.30};
    vcos_[0].phase = rng_.next01();
    vcos_[1].phase = rng_.next01();
}

std::span<const std::string_view> SubharmDSP::inIds() const noexcept { return kInIds; }
std::span<const std::string_view> SubharmDSP::outIds() const noexcept { return kOutIds; }

void SubharmDSP::prepare(double sr) {
    ModuleDSP::prepare(sr);
    gateHigh_.fill(0);
    pulses_.fill(0);
    controlDirty_ = true;
}

std::shared_ptr<const ModuleSettings> SubharmDSP::parseSettings(const nlohmann::json& j) const {
    auto out = std::make_shared<Settings>();
    out->vco[0] = {261.63, 2, 2, 4, 0.55, 0.6, 0.45};
    out->vco[1] = {392.00, 0, 4, 6, 0.35, 0.4, 0.30};
    if (!j.is_object()) return out;

    if (j.contains("vco") && j["vco"].is_array()) {
        for (int i = 0; i < 2 && i < static_cast<int>(j["vco"].size()); ++i) {
            const auto& v = j["vco"][i];
            if (!v.is_object()) continue;
            if (v.contains("freq") && v["freq"].is_number()) out->vco[i].freq = clampd(v["freq"].get<double>(), 16.0, 8000.0);
            if (v.contains("wave") && v["wave"].is_string()) out->vco[i].wave = parseWave(v["wave"].get<std::string>());
            if (v.contains("sub1") && v["sub1"].is_number()) out->vco[i].sub1 = clampi(static_cast<int>(std::lround(v["sub1"].get<double>())), 1, 16);
            if (v.contains("sub2") && v["sub2"].is_number()) out->vco[i].sub2 = clampi(static_cast<int>(std::lround(v["sub2"].get<double>())), 1, 16);
            if (v.contains("level") && v["level"].is_number()) out->vco[i].level = clampd(v["level"].get<double>(), 0.0, 1.0);
            if (v.contains("sub1_level") && v["sub1_level"].is_number()) out->vco[i].sub1_level = clampd(v["sub1_level"].get<double>(), 0.0, 1.0);
            if (v.contains("sub2_level") && v["sub2_level"].is_number()) out->vco[i].sub2_level = clampd(v["sub2_level"].get<double>(), 0.0, 1.0);
        }
    }
    if (j.contains("seq") && j["seq"].is_array()) {
        for (int i = 0; i < 2 && i < static_cast<int>(j["seq"].size()); ++i) {
            if (!j["seq"][i].is_array()) continue;
            for (int k = 0; k < 4 && k < static_cast<int>(j["seq"][i].size()); ++k) {
                if (j["seq"][i][k].is_number())
                    out->seq[i][k] = clampd(j["seq"][i][k].get<double>(), -1.0, 1.0);
            }
        }
    }
    if (j.contains("seq_assign") && j["seq_assign"].is_array()) {
        for (int i = 0; i < 2 && i < static_cast<int>(j["seq_assign"].size()); ++i) {
            const auto& a = j["seq_assign"][i];
            if (!a.is_object()) continue;
            if (a.contains("osc") && a["osc"].is_boolean()) out->seq_assign[i].osc = a["osc"].get<bool>();
            if (a.contains("sub1") && a["sub1"].is_boolean()) out->seq_assign[i].sub1 = a["sub1"].get<bool>();
            if (a.contains("sub2") && a["sub2"].is_boolean()) out->seq_assign[i].sub2 = a["sub2"].get<bool>();
        }
    }
    if (j.contains("seq_oct") && j["seq_oct"].is_number()) out->seq_oct = clampd(j["seq_oct"].get<double>(), 0.0, 5.0);
    if (j.contains("quantize") && j["quantize"].is_string()) out->quantize = parseQuantize(j["quantize"].get<std::string>());
    if (j.contains("tempo") && j["tempo"].is_number()) out->tempo = clampd(j["tempo"].get<double>(), 20.0, 3000.0);
    if (j.contains("rhythm") && j["rhythm"].is_array()) {
        for (int i = 0; i < 4 && i < static_cast<int>(j["rhythm"].size()); ++i)
            if (j["rhythm"][i].is_number()) out->rhythm[i] = clampi(static_cast<int>(std::lround(j["rhythm"][i].get<double>())), 1, 16);
    }
    if (j.contains("rhythm_seq1") && j["rhythm_seq1"].is_array()) {
        for (int i = 0; i < 4 && i < static_cast<int>(j["rhythm_seq1"].size()); ++i)
            if (j["rhythm_seq1"][i].is_boolean()) out->rhythm_seq1[i] = j["rhythm_seq1"][i].get<bool>();
    }
    if (j.contains("rhythm_seq2") && j["rhythm_seq2"].is_array()) {
        for (int i = 0; i < 4 && i < static_cast<int>(j["rhythm_seq2"].size()); ++i)
            if (j["rhythm_seq2"][i].is_boolean()) out->rhythm_seq2[i] = j["rhythm_seq2"][i].get<bool>();
    }
    if (j.contains("cutoff") && j["cutoff"].is_number()) out->cutoff = clampd(j["cutoff"].get<double>(), 20.0, 20000.0);
    if (j.contains("resonance") && j["resonance"].is_number()) out->resonance = clampd(j["resonance"].get<double>(), 0.0, 1.0);
    if (j.contains("vcf_eg_amt") && j["vcf_eg_amt"].is_number()) out->vcf_eg_amt = clampd(j["vcf_eg_amt"].get<double>(), -1.0, 1.0);
    if (j.contains("vcf_attack") && j["vcf_attack"].is_number()) out->vcf_attack = clampd(j["vcf_attack"].get<double>(), 0.0005, 10.0);
    if (j.contains("vcf_decay") && j["vcf_decay"].is_number()) out->vcf_decay = clampd(j["vcf_decay"].get<double>(), 0.001, 10.0);
    if (j.contains("vca_attack") && j["vca_attack"].is_number()) out->vca_attack = clampd(j["vca_attack"].get<double>(), 0.0005, 10.0);
    if (j.contains("vca_decay") && j["vca_decay"].is_number()) out->vca_decay = clampd(j["vca_decay"].get<double>(), 0.001, 10.0);
    if (j.contains("volume") && j["volume"].is_number()) out->volume = clampd(j["volume"].get<double>(), 0.0, 1.5);
    if (j.contains("eg_hold") && j["eg_hold"].is_boolean()) out->eg_hold = j["eg_hold"].get<bool>();
    return out;
}

void SubharmDSP::applySettings(const ModuleSettings& s) {
    s_ = static_cast<const Settings&>(s);
    controlDirty_ = true;
}

void SubharmDSP::setScale(const Scale* scale) {
    customScale = scale;
    controlDirty_ = true;
}

void SubharmDSP::handleMessage(const DspMessage& m) {
    switch (m.type) {
        case DspMessage::Type::Play:
            setPlaying(m.hasValue ? m.gate : !playing_);
            break;
        case DspMessage::Type::Reset:
            resetSeq();
            break;
        case DspMessage::Type::Next:
            advance(0);
            advance(1);
            triggerEGs();
            break;
        case DspMessage::Type::Trigger:
            holdButton_ = m.gate;
            if (m.gate) triggerEGs();
            break;
        case DspMessage::Type::MidiMono:
            if (m.hasPitch) {
                midiPitch_ = m.pitch;
                controlDirty_ = true;
            }
            holdMidi_ = m.gate;
            if (m.retrigger) triggerEGs();
            break;
        case DspMessage::Type::AllNotesOff:
            holdMidi_ = false;
            break;
        case DspMessage::Type::Bend:
            bend_ = m.value;
            controlDirty_ = true;
            break;
        case DspMessage::Type::MidiClock:
            midiTicks_++;
            lastMidiClock_ = sampleCount_;
            break;
        case DspMessage::Type::MidiStart:
            resetSeq();
            midiClockCount_ = 0;
            midiTicks_ = 0;
            setPlaying(true);
            break;
        case DspMessage::Type::MidiStop:
            setPlaying(false);
            break;
        case DspMessage::Type::MidiContinue:
            setPlaying(true);
            break;
        default:
            break;
    }
}

void SubharmDSP::setMidiConnected(bool connected) {
    if (!connected && midiConnected) {
        holdMidi_ = false;
        midiPitch_ = 0.0;
        bend_ = 0.0;
        lastMidiClock_ = -1000000000000LL;
        controlDirty_ = true;
    }
    midiConnected = connected;
}

void SubharmDSP::setPlaying(bool v) noexcept {
    if (v && !playing_) clockPhase_ = 1.0;
    playing_ = v;
}

void SubharmDSP::resetSeq() noexcept {
    for (int i = 0; i < 4; ++i) rhythmCnt_[i] = 0;
    step_[0] = step_[1] = 0;
    stepPending_[0] = stepPending_[1] = true;
    clockPhase_ = 1.0;
    midiClockCount_ = 0;
    controlDirty_ = true;
}

void SubharmDSP::advance(int s) noexcept {
    if (stepPending_[s]) stepPending_[s] = false;
    else step_[s] = (step_[s] + 1) % 4;
    pulses_[O_SEQ_CLK[s]] = static_cast<int32_t>(std::llround(PULSE_TIME * sampleRate));
    controlDirty_ = true;
}

void SubharmDSP::triggerEGs() noexcept {
    vcaEg_.stage = 1;
    vcfEg_.stage = 1;
    pulses_[O_TRIG] = static_cast<int32_t>(std::llround(PULSE_TIME * sampleRate));
}

void SubharmDSP::masterTick() noexcept {
    pulses_[O_CLOCK] = static_cast<int32_t>(std::llround(PULSE_TIME * sampleRate));
    clockFlash_ = static_cast<int>(std::llround(0.05 * sampleRate));
    bool adv1 = false, adv2 = false;
    for (int i = 0; i < 4; ++i) {
        int div = s_.rhythm[i];
        if (patched[I_RHYTHM + i])
            div += static_cast<int>(std::lround(ins[I_RHYTHM + i] * SUB_DIV_PER_VOLT));
        div = clampi(div, 1, 16);
        if (rhythmCnt_[i] >= div) rhythmCnt_[i] = 0;
        if (rhythmCnt_[i] == 0) {
            rhythmFlash_[i] = static_cast<int>(std::llround(0.06 * sampleRate));
            if (s_.rhythm_seq1[i]) adv1 = true;
            if (s_.rhythm_seq2[i]) adv2 = true;
        }
        rhythmCnt_[i]++;
        if (rhythmCnt_[i] >= div) rhythmCnt_[i] = 0;
    }
    if (adv1) advance(0);
    if (adv2) advance(1);
    if (adv1 || adv2) triggerEGs();
}

double SubharmDSP::stepValue(int i) const noexcept {
    const int k = step_[i];
    const int j = I_STEP[i] + k;
    const double v = s_.seq[i][k] + (patched[j] ? static_cast<double>(ins[j]) : 0.0);
    return clampd(v, -1.0, 1.0);
}

void SubharmDSP::updateControl(bool smoothKnob) noexcept {
    const Scale* scale = nullptr;
    if (s_.quantize == 5) {
        if (customScale && customScale->valid()) scale = customScale;
    } else if (s_.quantize >= 1 && s_.quantize <= 4) {
        scale = &builtinScale(s_.quantize);
    }

    for (int i = 0; i < 2; ++i) {
        auto& v = vcos_[i];
        const auto& sv = s_.vco[i];
        const double target = std::log2(sv.freq / C4_FREQ);
        if (!v.hasKnob || !smoothKnob) {
            v.knob = target;
            v.hasKnob = true;
        } else {
            v.knob += (target - v.knob) * 0.25;
        }
        if (std::abs(target - v.knob) < 1e-6) v.knob = target;

        const double seqCv = stepValue(i) * s_.seq_oct;
        const auto& a = s_.seq_assign[i];
        double p = v.knob + (a.osc ? seqCv : 0.0) + midiPitch_;
        v.prePitch = p;
        v.scale = scale;
        v.lastCv = 1e30;
        if (scale) p = quantizePitch(p, *scale);
        v.pitch = p + bend_;

        const int seqDiv = static_cast<int>(std::lround(seqCv * SUB_DIV_PER_VOLT));
        v.div1 = sv.sub1 + (a.sub1 ? seqDiv : 0);
        v.div2 = sv.sub2 + (a.sub2 ? seqDiv : 0);
    }
}

void SubharmDSP::stepEg(EgState& eg, double ac, double dc, bool hold) noexcept {
    if (eg.stage == 1) {
        eg.level += (ATTACK_TARGET - eg.level) * ac;
        if (eg.level >= 1.0) {
            eg.level = 1.0;
            eg.stage = hold ? 2 : 3;
        }
    } else if (eg.stage == 2) {
        if (!hold) eg.stage = 3;
    } else if (eg.stage == 3) {
        eg.level *= dc;
        if (eg.level < 1e-6) {
            eg.level = 0.0;
            eg.stage = 0;
        }
    }
}

bool SubharmDSP::gateEdge(int idx) noexcept {
    const uint8_t high = ins[idx] > GATE_THRESHOLD ? 1 : 0;
    const bool rising = high && !gateHigh_[idx];
    gateHigh_[idx] = high;
    return rising;
}

void SubharmDSP::beginBlock(int) {
    const double sr = sampleRate;
    for (int i = 0; i < 2; ++i) {
        vcos_[i].drift += (rng_.next01() - 0.5) * 0.0004;
        vcos_[i].drift *= 0.995;
    }
    updateControl(true);
    controlDirty_ = false;
    lp_[0] += (rng_.next01() - 0.5) * 1e-5;

    const double lvTarget[6] = {
        s_.vco[0].level * s_.vco[0].level, s_.vco[0].sub1_level * s_.vco[0].sub1_level, s_.vco[0].sub2_level * s_.vco[0].sub2_level,
        s_.vco[1].level * s_.vco[1].level, s_.vco[1].sub1_level * s_.vco[1].sub1_level, s_.vco[1].sub2_level * s_.vco[1].sub2_level
    };
    const double cutTarget = std::log2(clampd(s_.cutoff, CUTOFF_MIN, 20000.0) / CUTOFF_MIN);
    const double volTarget = s_.volume * s_.volume;
    if (!hasSmooth_) {
        for (int i = 0; i < 6; ++i) smLv_[i] = lvTarget[i];
        smCut_ = cutTarget;
        smRes_ = s_.resonance;
        smVol_ = volTarget;
        hasSmooth_ = true;
    }
    constexpr double k = 0.3;
    for (int i = 0; i < 6; ++i) smLv_[i] += (lvTarget[i] - smLv_[i]) * k;
    smCut_ += (cutTarget - smCut_) * k;
    smRes_ += (s_.resonance - smRes_) * k;
    smVol_ += (volTarget - smVol_) * k;
    fbk_ = smRes_ * 4.6;
    resComp_ = 1.0 + fbk_ * 0.3;

    waveMode_[0] = s_.vco[0].wave;
    waveMode_[1] = s_.vco[1].wave;
    vcaAc_ = 1.0 - std::exp(-ATTACK_TC / (std::max(s_.vca_attack, 0.0005) * sr));
    vcaDc_ = std::exp(-DECAY_TC / (std::max(s_.vca_decay, 0.001) * sr));
    vcfAc_ = 1.0 - std::exp(-ATTACK_TC / (std::max(s_.vcf_attack, 0.0005) * sr));
    vcfDc_ = std::exp(-DECAY_TC / (std::max(s_.vcf_decay, 0.001) * sr));
    egAmt_ = s_.vcf_eg_amt * EG_CUTOFF_OCTAVES;

    const bool extClock = patched[I_CLOCK] == 1;
    const bool midiClock = !extClock && (sampleCount_ - lastMidiClock_) < static_cast<int64_t>(MIDI_CLOCK_TIMEOUT * sr);
    extClock_ = extClock;
    internalClock_ = !extClock && !midiClock;
    clockSource_ = extClock ? 2 : (midiClock ? 1 : 0);
    if (midiClock) {
        for (; midiTicks_ > 0; --midiTicks_) {
            if (playing_ && (midiClockCount_ % 6 == 0)) masterTick();
            midiClockCount_++;
        }
    }
    midiTicks_ = 0;
    clockInc_ = s_.tempo / 60.0 / sr;
    twoPiOver2sr_ = TWO_PI / (2.0 * sr);
}

float SubharmDSP::tick() {
    if (gateEdge(I_PLAY)) setPlaying(!playing_);
    if (gateEdge(I_RESET)) resetSeq();
    if (gateEdge(I_TRIG)) triggerEGs();
    holdInput_ = gateHigh_[I_TRIG] == 1;
    const bool clockEdge = gateEdge(I_CLOCK);

    if (playing_) {
        if (internalClock_) {
            clockPhase_ += clockInc_;
            if (clockPhase_ >= 1.0) {
                clockPhase_ -= std::floor(clockPhase_);
                masterTick();
            }
        } else if (extClock_ && clockEdge) {
            masterTick();
        }
    }
    if (controlDirty_) {
        updateControl(false);
        controlDirty_ = false;
    }

    double mix = 0.0;
    for (int i = 0; i < 2; ++i) {
        auto& v = vcos_[i];
        double pitch = v.pitch + v.drift;
        if (patched[I_VCO_PITCH[i]]) {
            const double cv = ins[I_VCO_PITCH[i]];
            if (!v.scale) {
                pitch += cv;
            } else {
                if (cv != v.lastCv) {
                    v.lastCv = cv;
                    v.lastQ = quantizePitch(v.prePitch + cv, *v.scale);
                }
                pitch = v.lastQ + bend_ + v.drift;
            }
        }
        const double f = C4_FREQ * std::exp(pitch * LN2);
        const double dt = clampd(f / sampleRate, 0.0, 0.45);
        v.freq = f;

        int subCv = 0;
        if (patched[I_VCO_SUB[i]]) subCv = static_cast<int>(std::lround(ins[I_VCO_SUB[i]] * SUB_DIV_PER_VOLT));
        const int d1 = clampi(v.div1 + subCv, 1, 16);
        const int d2 = clampi(v.div2 + subCv, 1, 16);
        v.curDiv1 = d1;
        v.curDiv2 = d2;
        if (v.cnt1 >= d1) v.cnt1 %= d1;
        if (v.cnt2 >= d2) v.cnt2 %= d2;

        v.phase += dt;
        if (v.phase >= 1.0) {
            v.phase -= 1.0;
            if (++v.cnt1 >= d1) v.cnt1 = 0;
            if (++v.cnt2 >= d2) v.cnt2 = 0;
        }
        const double ph = v.phase;
        const double sp1 = (v.cnt1 + ph) / d1, dt1 = dt / d1;
        const double sp2 = (v.cnt2 + ph) / d2, dt2 = dt / d2;

        double o = 0.0, o1 = 0.0, o2 = 0.0;
        const int mode = waveMode_[i];
        if (mode == 0) {
            double pw = 0.5;
            if (patched[I_VCO_PWM[i]]) pw = clampd(0.5 + ins[I_VCO_PWM[i]] * 0.09, 0.05, 0.95);
            o = blepPulse(ph, dt, pw);
            o1 = blepPulse(sp1, dt1, 0.5);
            o2 = blepPulse(sp2, dt2, 0.5);
        } else if (mode == 1) {
            o1 = blepSaw(sp1, dt1);
            o2 = blepSaw(sp2, dt2);
            const double pw = patched[I_VCO_PWM[i]]
                ? clampd(0.5 + ins[I_VCO_PWM[i]] * 0.09, 0.05, 0.95)
                : 0.5 + o1 * 0.4;
            o = blepPulse(ph, dt, pw);
        } else {
            o = blepSaw(ph, dt);
            o1 = blepSaw(sp1, dt1);
            o2 = blepSaw(sp2, dt2);
        }
        const int b = O_VCO[i];
        outs[b] = static_cast<float>(o * 5.0);
        outs[b + 1] = static_cast<float>(o1 * 5.0);
        outs[b + 2] = static_cast<float>(o2 * 5.0);
        mix += o * smLv_[i * 3] + o1 * smLv_[i * 3 + 1] + o2 * smLv_[i * 3 + 2];
    }

    const bool hold = holdButton_ || holdMidi_ || holdInput_;
    stepEg(vcaEg_, vcaAc_, vcaDc_, hold);
    stepEg(vcfEg_, vcfAc_, vcfDc_, hold);
    if (s_.eg_hold) {
        vcaEg_.level = 1.0;
        vcfEg_.level = 1.0;
    }

    double cutOct = smCut_ + vcfEg_.level * egAmt_;
    if (patched[I_CUTOFF]) cutOct += ins[I_CUTOFF];
    const double fc = clampd(CUTOFF_MIN * std::exp(cutOct * LN2), 5.0, sampleRate * 0.45);
    const double g = 1.0 - std::exp(-twoPiOver2sr_ * fc);
    const double input = mix * 0.5;
    for (int os = 0; os < 2; ++os) {
        const double x = fastTanhD(input - fbk_ * lp_[3]);
        lp_[0] += g * (x - lp_[0]);
        lp_[1] += g * (lp_[0] - lp_[1]);
        lp_[2] += g * (lp_[1] - lp_[2]);
        lp_[3] += g * (lp_[2] - lp_[3]);
    }
    const double filtered = lp_[3] * resComp_;

    const double vca = patched[I_VCA] ? clampd(ins[I_VCA] / 5.0, 0.0, 1.5) : vcaEg_.level;
    const double vcaOut = filtered * vca;

    outs[O_VCA] = static_cast<float>(vcaOut * 5.0);
    outs[O_VCA_EG] = static_cast<float>(vcaEg_.level * 5.0);
    outs[O_VCF_EG] = static_cast<float>(vcfEg_.level * 5.0);
    outs[O_SEQ[0]] = static_cast<float>(stepValue(0) * s_.seq_oct);
    outs[O_SEQ[1]] = static_cast<float>(stepValue(1) * s_.seq_oct);
    for (int q = 0; q < 4; ++q) {
        const int idx = PULSE_OUTS[q];
        if (pulses_[idx] > 0) {
            outs[idx] = static_cast<float>(GATE_V);
            pulses_[idx]--;
        } else {
            outs[idx] = 0.0f;
        }
    }
    const float audio = static_cast<float>(vcaOut * smVol_ * 2.0);
    outs[O_AUDIO] = audio;
    return audio;
}

void SubharmDSP::endBlock(int n) {
    sampleCount_ += n;
    for (int i = 0; i < 4; ++i) rhythmFlash_[i] = std::max(0, rhythmFlash_[i] - n);
    clockFlash_ = std::max(0, clockFlash_ - n);
}

void SubharmDSP::snapshotTelemetry() {
    const int w = 1 - telIdx_.load(std::memory_order_relaxed);
    auto& t = tel_[w];
    t.playing = playing_;
    t.step[0] = step_[0];
    t.step[1] = step_[1];
    for (int i = 0; i < 4; ++i) t.rhythm[i] = rhythmFlash_[i] > 0;
    t.clock = clockFlash_ > 0;
    t.clockSource = clockSource_;
    t.freq[0] = vcos_[0].freq;
    t.freq[1] = vcos_[1].freq;
    t.div[0][0] = vcos_[0].curDiv1;
    t.div[0][1] = vcos_[0].curDiv2;
    t.div[1][0] = vcos_[1].curDiv1;
    t.div[1][1] = vcos_[1].curDiv2;
    t.vca_eg = vcaEg_.level;
    t.vcf_eg = vcfEg_.level;
    telIdx_.store(w, std::memory_order_release);
}

nlohmann::json SubharmDSP::readTelemetryJson() const {
    const auto& t = tel_[telIdx_.load(std::memory_order_acquire)];
    const char* src = t.clockSource == 2 ? "patch" : (t.clockSource == 1 ? "midi" : "internal");
    return {
        {"playing", t.playing},
        {"step", {t.step[0], t.step[1]}},
        {"rhythm", {t.rhythm[0], t.rhythm[1], t.rhythm[2], t.rhythm[3]}},
        {"clock", t.clock},
        {"clock_source", src},
        {"freq", {t.freq[0], t.freq[1]}},
        {"div", {{t.div[0][0], t.div[0][1]}, {t.div[1][0], t.div[1][1]}}},
        {"vca_eg", t.vca_eg},
        {"vcf_eg", t.vcf_eg}
    };
}

} // namespace gasfree
