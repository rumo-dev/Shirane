#pragma once
#include "audio/dsp/GainRamp.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace shirane::audio {
inline constexpr std::size_t kMaxMixerSources = 16;
inline constexpr std::size_t kMaxMixerFrames = 256;
inline constexpr std::uint32_t kGainRampFrames = 240;
inline constexpr std::uint32_t kEmergencyRampFrames = 48;
enum class Bus : std::size_t { Monitor, Stream };
struct MixerSend {
    float gainDb = 0, pan = 0;
    bool mute = true, solo = false;
};
struct MixerParameters {
    std::array<std::array<MixerSend, 2>, kMaxMixerSources> sources{};
    std::array<float, 2> masterDb{};
};
class PreparedMixerParameters {
public:
    // Non-RT conversion. Rejects invalid parameters without modifying destination.
    static bool prepare(const MixerParameters& input, PreparedMixerParameters& destination) noexcept;
private:
    friend class Mixer;
    std::array<std::array<std::array<float, 2>, 2>, kMaxMixerSources> gains_{};
    std::array<float, 2> masters_{1, 1};
};
struct MixerInput {
    std::size_t source = 0; // Stable slot, not input-list position.
    std::size_t channelCount = 0;
    std::array<std::span<const float>, 2> channels{};
};
enum class MixStatus { Ok, InvalidInput };
struct MixResult {
    MixStatus status = MixStatus::Ok;
    std::uint32_t invalidSources = 0;
    std::uint32_t overflowSamples = 0;
};
// Owns all output/ramp state. One engine thread calls apply/process/output.
// Only requestEmergencyMute may be called concurrently. Reset only while stopped.
class Mixer final {
public:
    Mixer() noexcept { reset(); }
    void reset() noexcept;
    void apply(const PreparedMixerParameters& parameters) noexcept;
    [[nodiscard]] MixResult process(std::span<const MixerInput> inputs, std::size_t frames) noexcept;
    [[nodiscard]] std::span<const float> output(Bus bus, std::size_t channel) const noexcept;
    void requestEmergencyMute() noexcept { emergency_.store(true, std::memory_order_relaxed); }
private:
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::array<std::array<std::array<GainRamp, 2>, 2>, kMaxMixerSources> gains_{};
    std::array<GainRamp, 2> masters_{};
    GainRamp emergencyGain_;
    std::atomic<bool> emergency_{false};
    std::array<std::array<std::array<float, kMaxMixerFrames>, 2>, 2> outputs_{};
    std::size_t frames_ = 0;
};
}
