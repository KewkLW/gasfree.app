#pragma once

#include "dsp_common.h"
#include "module_dsp.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gasfree {

// Sample-accurate MIDI/control event delivered to RackEngine::processBlock
struct RtEvent {
    enum class Kind : uint8_t {
        NoteOn = 0,
        NoteOff,
        PitchBend,     // channel 1..16, value in octaves (+-2/12)
        ModWheel,      // channel 1..16, value 0..1
        Pressure,      // channel 1..16, value 0..1
        PolyPressure,  // channel 1..16, note, value 0..1
        Sustain,       // channel 1..16, gate
        AllNotesOff,
        MidiClock,
        MidiStart,
        MidiStop,
        MidiContinue,
        KbZone,        // zone 0|1, hasPitch, pitch (octaves from C4), gate
        KbTrig,        // trigger pulse
        ModuleMsg,     // target moduleIndex (or hashed id) + DspMessage
        IoGain,        // masterGain (linear), inputGain (linear)
        HostTempo,     // bpm
    };

    Kind kind = Kind::NoteOn;
    int32_t sampleOffset = 0; // sample index within current block (0 for UI events)
    int32_t channel = 1;      // 1..16
    int32_t note = 60;
    int32_t zone = 0;
    uint32_t moduleHash = 0;  // FNV-1a hash of module id for ModuleMsg
    bool gate = false;
    bool hasPitch = false;
    double value = 0.0;
    double value2 = 0.0;
    DspMessage dspMsg{};
};

inline uint32_t fnv1aHash(std::string_view s) noexcept {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

class RackEngine {
public:
    explicit RackEngine(std::string appDir = {});
    ~RackEngine();

    // Non-RT setup
    void prepare(double sampleRate, int maxBlockSize);
    void setAppDir(std::string dir);
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    double tempo() const noexcept { return tempo_; }
    uint64_t currentFrame() const noexcept { return currentFrame_.load(std::memory_order_relaxed); }

    // Default GASFREE rack settings JSON (matches WRContext.defaultSettings())
    static nlohmann::json defaultRackJson();

    // Returns list of natively implemented DSP device types
    static std::vector<std::string> supportedNativeTypes();

    // UI/control thread: update rack from flattened settings JSON (from WebView or DAW state load)
    void applyFlattenedSettings(const nlohmann::json& flattenedSettings, double tempoBpm);

    // UI/control thread: apply raw (unflattened) rack settings JSON (used when DAW loads state headless)
    void applyRawSettings(const nlohmann::json& rawSettings);

    // UI/control thread: parse and install a Scala scale (null/empty clears it)
    void setCustomScale(const nlohmann::json& scaleJson);

    // UI/control thread: push an event (note, module message, IO gain, kb) into lock-free queue
    bool pushUiEvent(const RtEvent& ev) noexcept;

    // UI/control thread: push a module message by module id string + JSON payload
    void postModuleMessageJson(const std::string& moduleId, const nlohmann::json& msgJson);

    // UI/control thread: free retired plans/scales and build telemetry JSON for WebView
    void reclaimTrash();
    nlohmann::json buildTelemetryJson() const;

    // Audio thread ONLY (lock-free, zero allocations)
    void processBlock(
        const float* inL, const float* inR,
        float* outL, float* outR,
        int numFrames,
        const RtEvent* hostEvents, int numHostEvents,
        double hostBpm = 0.0);

    // Master IO gains (from UI or DAW)
    static double masterKnobToGain(double v) noexcept {
        const double g = v / 0.8;
        return g * g;
    }

private:
    static constexpr int kNumRackOuts = 9;
    static constexpr int kNumRackIns = 2;
    static constexpr int kMaxCables = 512;
    static constexpr int kStateInterval = 1024;

    struct CableOp {
        const float* src = nullptr;
        float* dst = nullptr;
    };

    struct ModEntry {
        std::string id;
        uint32_t idHash = 0;
        std::string type;
        std::shared_ptr<ModuleDSP> dsp;
        std::shared_ptr<const ModuleSettings> settings;
        bool midiMaster = false;
        int midiChannel = 0; // 0 = OMNI, 1..16
        bool onBus = false;
        bool midiConnected = false;
        std::vector<uint8_t> patched;
        std::vector<uint8_t> outPatched;
        std::vector<int> midiTargetIndices;
    };

    struct RackPlan {
        std::vector<ModEntry> modules;
        std::vector<CableOp> cables;
        std::vector<float*> zeroTargets;
        uint8_t rackPatched[kNumRackIns] = {0, 0};
        uint64_t patchVersion = 0;
        double tempo = 120.0;
    };

    struct TrashItem {
        std::shared_ptr<RackPlan> plan;
        std::shared_ptr<Scale> scale;
    };

    std::shared_ptr<ModuleDSP> createModuleDsp(const std::string& type);
    void applyPlanOnAudioThread(std::shared_ptr<RackPlan> nextPlan) noexcept;
    void handleRtEvent(const RtEvent& ev) noexcept;
    void handleNoteEvent(int channel, int note, bool on, double vel) noexcept;
    void handleAllNotesOff() noexcept;
    void sendToChannel(int channel, const DspMessage& msg, bool omni, bool poly) noexcept;

    std::string appDir_;
    std::atomic<double> sampleRate_{48000.0};
    double tempo_ = 120.0;
    std::atomic<uint64_t> currentFrame_{0};

    // UI-thread authoritative state
    mutable std::mutex uiMutex_;
    struct UiModRecord {
        std::string type;
        std::shared_ptr<ModuleDSP> dsp;
    };
    std::unordered_map<std::string, UiModRecord> uiModules_;
    std::vector<std::pair<std::string, std::shared_ptr<ModuleDSP>>> uiActiveOrder_;
    uint64_t uiPatchVersion_ = 0;
    std::string uiLastPatchesSig_;

    // Lock-free queues between UI and Audio
    SpscQueue<std::shared_ptr<RackPlan>, 16> planQueue_;
    SpscQueue<std::shared_ptr<Scale>, 8> scaleQueue_;
    SpscQueue<RtEvent, 512> uiEventQueue_;
    SpscQueue<TrashItem, 64> trashQueue_;

    // Audio-thread state
    std::shared_ptr<RackPlan> activePlan_;
    std::shared_ptr<Scale> activeScale_;
    uint64_t appliedPatchVersion_ = 0;

    float rackOuts_[kNumRackOuts] = {};
    float rackIns_[kNumRackIns] = {};
    uint8_t rackPatched_[kNumRackIns] = {};

    bool kbGate_[2] = {false, false};
    float kbKey_[2] = {0.0f, 0.0f};
    int32_t kbTrig_ = 0;

    // Monophonic note stacks per channel (0 = OMNI, 1..16 = channel), fixed capacity for zero alloc
    struct NoteStack {
        int notes[32] = {};
        int count = 0;
        void clear() noexcept { count = 0; }
        bool remove(int note) noexcept {
            for (int i = 0; i < count; ++i) {
                if (notes[i] == note) {
                    for (int j = i; j + 1 < count; ++j) notes[j] = notes[j + 1];
                    count--;
                    return true;
                }
            }
            return false;
        }
        void push(int note) noexcept {
            remove(note);
            if (count < 32) notes[count++] = note;
        }
    };
    NoteStack stacks_[17];

    // Master & input gains (smoothed on audio thread)
    float masterGain_ = 1.0f, masterGainTarget_ = 1.0f;
    float inputGain_ = 1.0f, inputGainTarget_ = 1.0f;
    float ioSmooth_ = 0.001f;

    // Telemetry
    int stateCounter_ = 0;
    float peakL_ = 0.0f, peakR_ = 0.0f;
    double busyMs_ = 0.0;
    int64_t loadFrames_ = 0;
    std::atomic<float> telMeterL_{0.0f};
    std::atomic<float> telMeterR_{0.0f};
    std::atomic<double> telCpuLoad_{0.0};
};

} // namespace gasfree
