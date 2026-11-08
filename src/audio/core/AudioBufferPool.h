#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace shirane::audio {
struct AudioBlockView {
    std::array<std::span<float>, 2> channels{};
    std::size_t frameCount = 0;
    std::size_t channelCount = 0;
};
// Single owning thread for acquire/view/release. Borrowed views expire at release.
// Construct before processing, keep address stable, destroy after all borrowers stop.
template<std::size_t BlockCount, std::size_t MaxFrames>
class AudioBufferPool final {
    static_assert(BlockCount > 0 && MaxFrames > 0);
public:
    struct Handle {
        const AudioBufferPool* owner = nullptr;
        std::size_t index = 0;
        std::uint64_t generation = 0;
    };
    AudioBufferPool() noexcept {
        for (std::size_t i = 0; i < BlockCount; ++i) free_[i] = i;
        // Volatile stores touch the complete sample storage before processing begins.
        for (auto& block : blocks_) for (auto& channel : block.samples)
            for (auto& sample : channel) { volatile float* address = &sample; *address = 0.0f; }
    }
    AudioBufferPool(const AudioBufferPool&) = delete;
    AudioBufferPool& operator=(const AudioBufferPool&) = delete;
    [[nodiscard]] bool acquire(std::size_t frames, std::size_t channels, Handle& destination) noexcept {
        if (!frames || frames > MaxFrames || !channels || channels > 2 || !available_) return false;
        const auto index = free_[--available_];
        auto& block = blocks_[index];
        ++block.generation;
        block.inUse = true; block.frames = frames; block.channels = channels;
        // Reused blocks never expose previous audio as the new signal.
        for (std::size_t c = 0; c < channels; ++c)
            for (std::size_t f = 0; f < frames; ++f) block.samples[c][f] = 0.0f;
        destination = {this, index, block.generation};
        return true;
    }
    [[nodiscard]] AudioBlockView view(Handle handle) noexcept {
        if (!valid(handle)) return {};
        auto& block = blocks_[handle.index];
        AudioBlockView result{{}, block.frames, block.channels};
        for (std::size_t c = 0; c < block.channels; ++c)
            result.channels[c] = std::span<float>(block.samples[c].data(), block.frames);
        return result;
    }
    [[nodiscard]] bool release(Handle handle) noexcept {
        if (!valid(handle)) return false;
        auto& block = blocks_[handle.index];
        block.inUse = false;
        // Retire rather than wrap a generation and accept an ancient handle.
        if (block.generation != std::numeric_limits<std::uint64_t>::max()) free_[available_++] = handle.index;
        return true;
    }
    std::size_t available() const noexcept { return available_; }
private:
    struct Block {
        std::array<std::array<float, MaxFrames>, 2> samples{};
        std::uint64_t generation = 0;
        std::size_t frames = 0, channels = 0;
        bool inUse = false;
    };
    bool valid(Handle handle) const noexcept {
        return handle.owner == this && handle.index < BlockCount && blocks_[handle.index].inUse &&
            blocks_[handle.index].generation == handle.generation;
    }
    std::array<Block, BlockCount> blocks_{};
    std::array<std::size_t, BlockCount> free_{};
    std::size_t available_ = BlockCount;
};
}
