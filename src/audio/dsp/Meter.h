#pragma once
#include "audio/dsp/AudioProcessor.h"
#include "audio/core/SpscQueue.h"
#include <cmath>
#include <limits>

namespace shirane::audio {
struct MeterSnapshot {
    std::array<float, 2> peak{}, rms{};
    std::uint64_t sequence = 0, droppedWindows = 0;
    std::uint32_t invalidSamples = 0, channelCount = 0;
};
// One audio producer, one UI/diagnostics consumer. Each packet is a complete 50ms window.
// No reset of the queue while either thread runs; prepare once before threads start.
class Meter final {
public:
    ProcessorStatus prepare(ProcessSpec spec) noexcept {
        if (prepared_ || !validSpec(spec)) return ProcessorStatus::InvalidSpec;
        spec_ = spec; prepared_ = true; return ProcessorStatus::Ok;
    }
    ProcessorStatus process(ConstAudioBlockView block) noexcept {
        if (!prepared_) return ProcessorStatus::NotPrepared;
        if (!validBlock(block, spec_)) return ProcessorStatus::InvalidBlock;
        for (std::size_t f = 0; f < block.frameCount; ++f) {
            for (std::size_t c = 0; c < block.channelCount; ++c) {
                float sample = block.channels[c][f];
                if (!std::isfinite(sample)) { sample = 0; ++invalid_; }
                if (std::fpclassify(sample) == FP_SUBNORMAL) sample = 0;
                const float magnitude = std::abs(sample);
                if (magnitude > peak_[c]) peak_[c] = magnitude;
                sumSquares_[c] += static_cast<double>(sample) * sample;
            }
            if (++frames_ == kWindowFrames) publish();
        }
        return ProcessorStatus::Ok;
    }
    // Consumer only, copies one whole snapshot. No shared mutable double-buffer.
    [[nodiscard]] bool tryRead(MeterSnapshot& snapshot) noexcept { return snapshots_.tryPop(snapshot); }
    static constexpr std::uint32_t kWindowFrames = 2400;
private:
    void publish() noexcept {
        MeterSnapshot snapshot;
        snapshot.peak = peak_; snapshot.channelCount = spec_.channelCount;
        snapshot.invalidSamples = invalid_;
        if (sequence_ != std::numeric_limits<std::uint64_t>::max()) ++sequence_;
        snapshot.sequence = sequence_; snapshot.droppedWindows = dropped_;
        for (std::size_t c = 0; c < spec_.channelCount; ++c) {
            const double rms = std::sqrt(sumSquares_[c] / kWindowFrames);
            snapshot.rms[c] = rms > std::numeric_limits<float>::max() ? std::numeric_limits<float>::max() : static_cast<float>(rms);
            if (std::fpclassify(snapshot.rms[c]) == FP_SUBNORMAL) snapshot.rms[c] = 0;
        }
        if (!snapshots_.tryPush(snapshot) && dropped_ != std::numeric_limits<std::uint64_t>::max()) ++dropped_;
        frames_ = invalid_ = 0; peak_ = {}; sumSquares_ = {};
    }
    ProcessSpec spec_{};
    bool prepared_ = false;
    std::uint32_t frames_ = 0, invalid_ = 0;
    std::uint64_t sequence_ = 0, dropped_ = 0;
    std::array<float, 2> peak_{};
    std::array<double, 2> sumSquares_{};
    SpscQueue<MeterSnapshot, 8> snapshots_;
};
}
