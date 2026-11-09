#pragma once
#include "audio/dsp/AudioProcessor.h"
#include "audio/dsp/GainRamp.h"
#include <cmath>
#include <limits>

namespace shirane::audio {
class GainProcessor final : public IAudioProcessor {
public:
    ProcessorStatus prepare(ProcessSpec spec) override {
        if (!validSpec(spec)) return ProcessorStatus::InvalidSpec;
        spec_ = spec; prepared_ = true; reset(); return ProcessorStatus::Ok;
    }
    // Non-RT conversion; applyGain is called by the engine at a block boundary.
    static bool gainFromDb(float db, float& destination) noexcept {
        if (!std::isfinite(db) || db < -60 || db > 6) return false;
        destination = std::pow(10.0f, db / 20.0f); return true;
    }
    ProcessorStatus applyGain(float amplitude) noexcept {
        if (!prepared_) return ProcessorStatus::NotPrepared;
        if (!std::isfinite(amplitude) || amplitude < 0 || amplitude > 2 || (amplitude > 0 && amplitude < 0.001f))
            return ProcessorStatus::InvalidParameter;
        gain_.setTarget(amplitude, 240);
        return ProcessorStatus::Ok;
    }
    ProcessorStatus process(AudioBlockView block) noexcept override {
        if (!prepared_) return ProcessorStatus::NotPrepared;
        if (!validBlock(asConst(block), spec_)) return ProcessorStatus::InvalidBlock;
        bool finite = true;
        for (std::size_t c = 0; c < block.channelCount; ++c)
            for (std::size_t f = 0; f < block.frameCount; ++f) finite &= std::isfinite(block.channels[c][f]);
        auto result = finite ? ProcessorStatus::Ok : ProcessorStatus::NonFiniteInput;
        for (std::size_t f = 0; f < block.frameCount; ++f) {
            const float gain = gain_.next();
            for (std::size_t c = 0; c < block.channelCount; ++c) {
                auto& sample = block.channels[c][f];
                const double value = finite && std::fpclassify(sample) != FP_SUBNORMAL ? static_cast<double>(sample) * gain : 0;
                if (std::abs(value) > std::numeric_limits<float>::max()) { sample = 0; result = ProcessorStatus::Overflow; }
                else { sample = static_cast<float>(value); if (std::fpclassify(sample) == FP_SUBNORMAL) sample = 0; }
            }
        }
        return result;
    }
    void reset() noexcept override { gain_.reset(); }
    std::uint32_t latencyFrames() const noexcept override { return 0; }
private:
    ProcessSpec spec_{};
    bool prepared_ = false;
    GainRamp gain_;
};
}
