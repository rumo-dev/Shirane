#pragma once
#include <cstdint>
#include <limits>

namespace shirane::audio {
struct FramePosition {
    std::uint64_t epoch = 0;
    std::uint64_t frames = 0;
};
// Internal 48kHz timeline only, not a device clock or a drift estimator.
// Engine-thread owned; restartEpoch is called after the old engine has stopped.
class AudioClock final {
public:
    [[nodiscard]] bool advance(std::uint32_t frames) noexcept {
        if (frames > 256 || position_.frames > std::numeric_limits<std::uint64_t>::max() - frames) return false;
        position_.frames += frames;
        return true;
    }
    [[nodiscard]] bool restartEpoch(std::uint64_t initialFrames = 0) noexcept {
        if (position_.epoch == std::numeric_limits<std::uint64_t>::max()) return false;
        ++position_.epoch; position_.frames = initialFrames;
        return true;
    }
    FramePosition position() const noexcept { return position_; }
    // Floor to 100ns units. Absolute conversion avoids per-block rounding drift.
    [[nodiscard]] bool time100ns(std::uint64_t& destination) const noexcept {
        constexpr std::uint64_t kHz = 48000, kTicksPerSecond = 10000000;
        const auto seconds = position_.frames / kHz;
        const auto remainder = (position_.frames % kHz) * kTicksPerSecond / kHz;
        if (seconds > (std::numeric_limits<std::uint64_t>::max() - remainder) / kTicksPerSecond) return false;
        destination = seconds * kTicksPerSecond + remainder;
        return true;
    }
private:
    FramePosition position_{};
};
}
