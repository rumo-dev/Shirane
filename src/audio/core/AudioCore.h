#pragma once
#include <cstdint>

namespace shirane::audio {
inline constexpr std::uint32_t kSampleRateHz = 48'000;
// Core/Mixer primitives are available; no running audio workers or device ownership yet.
enum class EngineState { Unavailable };
[[nodiscard]] EngineState engineState() noexcept;
}
