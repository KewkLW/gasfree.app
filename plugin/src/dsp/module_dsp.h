#pragma once

#include "dsp_common.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gasfree {

// Plain POD message delivered to a module on the audio thread (zero heap allocations)
struct DspMessage {
    enum class Type : uint8_t {
        None = 0,
        Play,
        Reset,
        Next,
        Trigger,
        MidiMono,      // { pitch, gate, retrigger, velocity }
        Note,          // poly note { note, on, velocity }
        AllNotesOff,
        Bend,          // value in octaves (+-2/12)
        ModWheel,      // 0..1
        Pressure,      // 0..1
        PolyPressure,  // note + 0..1
        Sustain,       // gate
        MidiClock,
        MidiStart,
        MidiStop,
        MidiContinue,
    };

    Type type = Type::None;
    bool hasValue = false;
    bool gate = false;
    bool retrigger = false;
    bool hasPitch = false;
    int note = 0;
    double pitch = 0.0;
    double value = 0.0;
    double velocity = 1.0;
};

// Base polymorphic settings container (parsed on UI thread, applied lock-free on audio thread)
struct ModuleSettings {
    virtual ~ModuleSettings() = default;
};

// Base polymorphic telemetry container (populated into pre-allocated POD on audio thread,
// converted to JSON on UI thread)
struct ModuleTelemetry {
    virtual ~ModuleTelemetry() = default;
    virtual nlohmann::json toJson() const { return nlohmann::json::object(); }
};

class RackEngine;

class ModuleDSP {
public:
    virtual ~ModuleDSP() = default;

    // Metadata
    virtual std::string_view typeName() const noexcept = 0;
    virtual std::span<const std::string_view> inIds() const noexcept = 0;
    virtual std::span<const std::string_view> outIds() const noexcept = 0;
    virtual std::string_view midiInId() const noexcept { return {}; }
    virtual std::string_view midiOutId() const noexcept { return {}; }
    virtual bool isPoly() const noexcept { return false; }

    // Lifecycle (called on non-RT thread or during prepare)
    virtual void prepare(double sr) {
        sampleRate = sr;
        ins.assign(inIds().size(), 0.0f);
        outs.assign(outIds().size(), 0.0f);
        patched.assign(inIds().size(), 0);
        outPatched.assign(outIds().size(), 0);
    }

    // Parse JSON settings into a typed ModuleSettings object (UI/control thread ONLY)
    virtual std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json& j) const = 0;

    // Apply pre-parsed settings on audio thread (RT-safe, no heap allocs)
    virtual void applySettings(const ModuleSettings& s) = 0;

    // Set custom Scala scale (pointer owned by RackEngine, kept alive until retired)
    virtual void setScale(const Scale* scale) { customScale = scale; }

    // Deliver real-time message (RT-safe)
    virtual void handleMessage(const DspMessage& msg) {}

    virtual void setMidiConnected(bool connected) { midiConnected = connected; }

    virtual void beginBlock(int numSamples) {}
    virtual float tick() = 0;
    virtual void endBlock(int numSamples) {}

    // Telemetry: audio thread writes to pre-allocated slot, UI thread converts to JSON.
    // For simplicity and strict RT-safety, each module holds an internal double-buffered POD
    // snapshot updated on audio thread via snapshotTelemetry(), read on UI thread via readTelemetryJson().
    virtual void snapshotTelemetry() {}
    virtual nlohmann::json readTelemetryJson() const { return nlohmann::json::object(); }

    // Public jack buffers accessed directly by RackEngine's flattened cable copier
    std::vector<float> ins;
    std::vector<float> outs;
    std::vector<uint8_t> patched;
    std::vector<uint8_t> outPatched;

    double sampleRate = 48000.0;
    bool midiConnected = false;
    const Scale* customScale = nullptr;
    RackEngine* host = nullptr;
};

} // namespace gasfree
