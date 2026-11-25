#include "dsp/rack_engine.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#define REQUIRE(cond) do { if (!(cond)) { std::cerr << "FAIL at " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; std::exit(1); } } while (0)

using namespace gasfree;

static double computeRms(const std::vector<float>& buf) {
    double sum = 0.0;
    for (float x : buf) {
        if (!std::isfinite(x)) return -1.0;
        sum += static_cast<double>(x) * x;
    }
    return std::sqrt(sum / std::max<size_t>(1, buf.size()));
}

int main() {
    std::cout << "[test_dsp] Starting native RackEngine verification...\n";

    RackEngine engine(GASFREE_DEFAULT_APP_DIR);
    engine.prepare(48000.0, 256);

    // 1. Default rack: Subharm -> Comp -> Delay(off) -> Reverb(HALL B) -> Main Out
    std::vector<float> outL(48000, 0.0f), outR(48000, 0.0f);
    RtEvent noteOn;
    noteOn.kind = RtEvent::Kind::NoteOn;
    noteOn.sampleOffset = 0;
    noteOn.channel = 1;
    noteOn.note = 60;
    noteOn.value = 1.0;

    for (int off = 0; off < 48000; off += 256) {
        const int n = std::min(256, 48000 - off);
        engine.processBlock(
            nullptr, nullptr,
            outL.data() + off, outR.data() + off, n,
            off == 0 ? &noteOn : nullptr, off == 0 ? 1 : 0,
            128.0);
    }

    const double rmsL = computeRms(outL);
    const double rmsR = computeRms(outR);
    std::cout << "[test_dsp] Default rack (Subharm + Comp + Reverb HALL B): rmsL=" << rmsL << " rmsR=" << rmsR << "\n";
    REQUIRE(rmsL > 0.01 && rmsL < 1.5);
    REQUIRE(rmsR > 0.01 && rmsR < 1.5);

    auto tel = engine.buildTelemetryJson();
    REQUIRE(tel.contains("modules") && tel["modules"].contains("subharm") && tel["modules"].contains("comp"));
    REQUIRE(tel["modules"]["comp"].contains("scope"));
    std::cout << "[test_dsp] Telemetry snapshot OK (subharm freq=" << tel["modules"]["subharm"]["freq"][0] << " Hz)\n";

    // 2. Test full FX chain: Subharm -> Mixer -> Hyper -> Delay (pingpong on) -> Reverb (PLATE) -> Main
    //    plus LFO modulating Subharm cutoff via CV cable
    nlohmann::json custom = RackEngine::defaultRackJson();
    custom["modules"].push_back({
        {"id", "mixer"},
        {"type", "mixer"},
        {"settings", {
            {"ch", {
                {{"level", 0.75}, {"pan", 0.0}, {"aux", 0.0}, {"mute", false}, {"solo", false}},
                {{"level", 0.75}, {"pan", 0.0}, {"aux", 0.0}, {"mute", false}, {"solo", false}},
                {{"level", 0.75}, {"pan", 0.0}, {"aux", 0.0}, {"mute", false}, {"solo", false}},
                {{"level", 0.75}, {"pan", 0.0}, {"aux", 0.0}, {"mute", false}, {"solo", false}},
                {{"level", 0.75}, {"pan", 0.0}, {"aux", 0.0}, {"mute", false}, {"solo", false}},
                {{"level", 0.75}, {"pan", 0.0}, {"aux", 0.0}, {"mute", false}, {"solo", false}}
            }},
            {"master", 0.75}, {"aux_send", 0.75}, {"aux_return", 0.75}
        }}
    });
    custom["modules"].push_back({
        {"id", "hyper"},
        {"type", "hyper"},
        {"settings", {{"on", true}, {"drive", 0.4}, {"type", "tube"}, {"tone", 8000}, {"bits", 8}, {"rate", 16000}, {"wobble", 0.2}, {"mix", 1.0}, {"output", 0.9}}}
    });
    // Enable delay and switch reverb to synthetic PLATE
    for (auto& m : custom["modules"]) {
        if (m["id"] == "delay") m["settings"]["on"] = true;
        if (m["id"] == "reverb") m["settings"]["room"] = "PLATE";
    }
    custom["patches"] = {
        {"lfo.lfo1", "subharm.cutoff_in", 0},
        {"subharm.audio", "mixer.in1", 2},
        {"mixer.main_l", "hyper.in_l", 1},
        {"mixer.main_r", "hyper.in_r", 5},
        {"hyper.out_l", "comp.in_l", 1},
        {"hyper.out_r", "comp.in_r", 5},
        {"comp.out_l", "delay.in_l", 1},
        {"comp.out_r", "delay.in_r", 5},
        {"delay.out_l", "reverb.in_l", 1},
        {"delay.out_r", "reverb.in_r", 5},
        {"reverb.out_l", "io_main_l", 1},
        {"reverb.out_r", "io_main_r", 5}
    };
    engine.applyRawSettings(custom);
    engine.postModuleMessageJson("subharm", {{"type", "play"}, {"value", true}});

    std::fill(outL.begin(), outL.end(), 0.0f);
    std::fill(outR.begin(), outR.end(), 0.0f);
    for (int off = 0; off < 48000; off += 256) {
        const int n = std::min(256, 48000 - off);
        engine.processBlock(nullptr, nullptr, outL.data() + off, outR.data() + off, n, nullptr, 0, 120.0);
    }
    engine.reclaimTrash();

    const double rms2L = computeRms(outL);
    const double rms2R = computeRms(outR);
    std::cout << "[test_dsp] Full chain (Subharm seq + LFO CV + Mixer + Hyper + Comp + Delay + Plate): rmsL="
              << rms2L << " rmsR=" << rms2R << "\n";
    REQUIRE(rms2L > 0.01 && rms2L < 1.5);
    REQUIRE(rms2R > 0.01 && rms2R < 1.5);

    std::cout << "[test_dsp] ALL PASSED\n";
    return 0;
}
