#pragma once

#include "module_dsp.h"

namespace gasfree {

// Safe passthrough/placeholder DSP for device types not yet ported to native C++.
// Preserves stereo audio thru (in_l/in_r -> out_l/out_r) so inserting an unported
// effect never breaks the audio chain.
class PassthroughDSP final : public ModuleDSP {
public:
    explicit PassthroughDSP(std::string type) : type_(std::move(type)) {}

    std::string_view typeName() const noexcept override { return type_; }
    std::span<const std::string_view> inIds() const noexcept override { return kIns; }
    std::span<const std::string_view> outIds() const noexcept override { return kOuts; }

    struct EmptySettings : ModuleSettings {};

    std::shared_ptr<const ModuleSettings> parseSettings(const nlohmann::json&) const override {
        return std::make_shared<EmptySettings>();
    }

    void applySettings(const ModuleSettings&) override {}

    float tick() override {
        const float l = ins[0];
        const float r = patched[1] ? ins[1] : l;
        outs[0] = l;
        outs[1] = r;
        return 0.0f;
    }

private:
    std::string type_;
    static constexpr std::array<std::string_view, 2> kIns = {"in_l", "in_r"};
    static constexpr std::array<std::string_view, 2> kOuts = {"out_l", "out_r"};
};

} // namespace gasfree
