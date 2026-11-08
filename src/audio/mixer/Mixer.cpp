#include "audio/mixer/Mixer.h"
#include <cmath>
#include <limits>
#include <numbers>

namespace shirane::audio {
namespace {
bool validDb(float value) noexcept { return std::isfinite(value) && value >= -60 && value <= 6; }
float clean(float value) noexcept { return std::fpclassify(value) == FP_SUBNORMAL ? 0.0f : value; }
}
bool PreparedMixerParameters::prepare(const MixerParameters& input, PreparedMixerParameters& destination) noexcept {
    PreparedMixerParameters candidate;
    std::array<bool, 2> solo{};
    for (std::size_t b = 0; b < 2; ++b) {
        if (!validDb(input.masterDb[b])) return false;
        candidate.masters_[b] = std::pow(10.0f, input.masterDb[b] / 20.0f);
        for (const auto& source : input.sources) {
            const auto& send = source[b];
            if (!validDb(send.gainDb) || !std::isfinite(send.pan) || send.pan < -1 || send.pan > 1) return false;
            solo[b] |= send.solo;
        }
    }
    for (std::size_t s = 0; s < kMaxMixerSources; ++s) for (std::size_t b = 0; b < 2; ++b) {
        const auto& send = input.sources[s][b];
        if (send.mute || (solo[b] && !send.solo)) continue; // Mute wins over solo.
        const float gain = std::pow(10.0f, send.gainDb / 20.0f);
        const float angle = (send.pan + 1.0f) * std::numbers::pi_v<float> / 4.0f;
        candidate.gains_[s][b] = {send.pan == 1 ? 0.0f : gain * std::cos(angle),
            send.pan == -1 ? 0.0f : gain * std::sin(angle)};
    }
    destination = candidate;
    return true;
}
void Mixer::reset() noexcept {
    for (auto& source : gains_) for (auto& bus : source) for (auto& gain : bus) gain.reset();
    for (auto& master : masters_) master.reset(1);
    emergencyGain_.reset(1); emergency_.store(false, std::memory_order_relaxed);
    for (auto& bus : outputs_) for (auto& channel : bus) channel.fill(0);
    frames_ = 0;
}
void Mixer::apply(const PreparedMixerParameters& parameters) noexcept {
    for (std::size_t s = 0; s < kMaxMixerSources; ++s) for (std::size_t b = 0; b < 2; ++b)
        for (std::size_t c = 0; c < 2; ++c) gains_[s][b][c].setTarget(parameters.gains_[s][b][c], kGainRampFrames);
    for (std::size_t b = 0; b < 2; ++b) masters_[b].setTarget(parameters.masters_[b], kGainRampFrames);
}
MixResult Mixer::process(std::span<const MixerInput> inputs, std::size_t frames) noexcept {
    // Fixed bound even on malformed input. Clear old output before returning an error.
    for (auto& bus : outputs_) for (auto& channel : bus) channel.fill(0);
    frames_ = frames <= kMaxMixerFrames ? frames : 0;
    if (frames > kMaxMixerFrames || inputs.size() > kMaxMixerSources) return {MixStatus::InvalidInput};
    std::array<const MixerInput*, kMaxMixerSources> sources{};
    for (const auto& input : inputs) {
        if (input.source >= kMaxMixerSources || sources[input.source] || input.channelCount < 1 || input.channelCount > 2)
            return {MixStatus::InvalidInput};
        for (std::size_t c = 0; c < input.channelCount; ++c) {
            if (input.channels[c].size() < frames) return {MixStatus::InvalidInput};
            const auto begin = reinterpret_cast<std::uintptr_t>(input.channels[c].data());
            const auto own = reinterpret_cast<std::uintptr_t>(outputs_.data());
            if (frames && begin < own + sizeof(outputs_) && begin + frames * sizeof(float) > own)
                return {MixStatus::InvalidInput}; // No feedback from our borrowed output buffers.
        }
        sources[input.source] = &input;
    }
    MixResult result;
    // One non-finite sample silences that source for this block, on both buses.
    for (auto& source : sources) if (source) {
        bool valid = true;
        for (std::size_t c = 0; c < source->channelCount; ++c)
            for (std::size_t f = 0; f < frames; ++f) valid &= std::isfinite(source->channels[c][f]);
        if (!valid) { source = nullptr; ++result.invalidSources; }
    }
    if (emergency_.load(std::memory_order_relaxed)) emergencyGain_.setTarget(0, kEmergencyRampFrames);
    for (std::size_t f = 0; f < frames; ++f) {
        std::array<std::array<double, 2>, 2> sum{};
        for (std::size_t s = 0; s < kMaxMixerSources; ++s) {
            const auto* source = sources[s];
            const float left = source ? clean(source->channels[0][f]) : 0;
            const float right = source ? clean(source->channels[source->channelCount == 1 ? 0 : 1][f]) : 0;
            for (std::size_t b = 0; b < 2; ++b) {
                sum[b][0] += static_cast<double>(left) * gains_[s][b][0].next();
                sum[b][1] += static_cast<double>(right) * gains_[s][b][1].next();
            }
        }
        const float gate = emergencyGain_.next();
        for (std::size_t b = 0; b < 2; ++b) {
            const double master = static_cast<double>(masters_[b].next()) * gate;
            for (std::size_t c = 0; c < 2; ++c) {
                const double sample = sum[b][c] * master;
                if (!std::isfinite(sample) || std::abs(sample) > std::numeric_limits<float>::max()) ++result.overflowSamples;
                else outputs_[b][c][f] = clean(static_cast<float>(sample));
            }
        }
    }
    return result;
}
std::span<const float> Mixer::output(Bus bus, std::size_t channel) const noexcept {
    const auto index = static_cast<std::size_t>(bus);
    if (index >= 2 || channel >= 2) return {};
    return {outputs_[index][channel].data(), frames_};
}
}
