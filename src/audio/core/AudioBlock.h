#pragma once
#include <array>
#include <cstddef>
#include <span>

namespace shirane::audio {
struct AudioBlockView {
    std::array<std::span<float>, 2> channels{};
    std::size_t frameCount = 0;
    std::size_t channelCount = 0;
};
struct ConstAudioBlockView {
    std::array<std::span<const float>, 2> channels{};
    std::size_t frameCount = 0;
    std::size_t channelCount = 0;
};
inline ConstAudioBlockView asConst(AudioBlockView block) noexcept {
    return {{block.channels[0], block.channels[1]}, block.frameCount, block.channelCount};
}
}
