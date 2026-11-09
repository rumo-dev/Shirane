#pragma once
#include "audio/core/AudioBlock.h"
#include <cstdint>

namespace shirane::audio {
struct ProcessSpec {
    std::uint32_t sampleRateHz = 48000;
    std::uint32_t maxBlockFrames = 256;
    std::uint32_t channelCount = 2;
};
enum class ProcessorStatus { Ok, InvalidSpec, NotPrepared, InvalidBlock, InvalidParameter, NonFiniteInput, Overflow };
inline bool validSpec(ProcessSpec spec) noexcept {
    return spec.sampleRateHz == 48000 && spec.maxBlockFrames > 0 && spec.maxBlockFrames <= 256 &&
        spec.channelCount > 0 && spec.channelCount <= 2;
}
inline bool validBlock(ConstAudioBlockView block, ProcessSpec spec) noexcept {
    if (!validSpec(spec) || block.channelCount != spec.channelCount || block.frameCount > spec.maxBlockFrames) return false;
    for (std::size_t c = 0; c < block.channelCount; ++c)
        if (block.channels[c].size() < block.frameCount) return false;
    if (block.channelCount == 2 && block.frameCount) {
        const auto first = reinterpret_cast<std::uintptr_t>(block.channels[0].data());
        const auto second = reinterpret_cast<std::uintptr_t>(block.channels[1].data());
        const auto bytes = block.frameCount * sizeof(float);
        if (first <= second ? second - first < bytes : first - second < bytes) return false;
    }
    return true;
}
class IAudioProcessor {
public:
    virtual ProcessorStatus prepare(ProcessSpec spec) = 0; // Non-RT, stopped only.
    virtual ProcessorStatus process(AudioBlockView block) noexcept = 0;
    virtual void reset() noexcept = 0; // Bounded, owner thread only.
    virtual std::uint32_t latencyFrames() const noexcept = 0;
    virtual ~IAudioProcessor() = default; // Destroy on non-RT thread.
};
}
