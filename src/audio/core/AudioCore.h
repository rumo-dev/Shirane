#pragma once
#include <cstdint>

namespace shirane::audio {
inline constexpr std::uint32_t kSampleRateHz = 48'000;
// Core primitives are available; no running audio workers, DSP or device ownership yet.
enum class EngineState { Unavailable };
[[nodiscard]] EngineState engineState() noexcept;
}
