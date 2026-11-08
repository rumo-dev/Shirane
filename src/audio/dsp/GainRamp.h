#pragma once
#include <cstdint>

namespace shirane::audio {
// Engine-thread owned. Internal coefficients must be finite, nonnegative values.
class GainRamp final {
public:
    void reset(float value = 0.0f) noexcept { current_ = target_ = value; step_ = 0; remaining_ = 0; }
    void setTarget(float target, std::uint32_t frames) noexcept {
        if (target == target_) return;
        target_ = target;
        if (!frames) { reset(target); return; }
        remaining_ = frames;
        step_ = (target_ - current_) / static_cast<float>(frames);
    }
    float next() noexcept {
        if (remaining_) {
            --remaining_;
            current_ = remaining_ ? current_ + step_ : target_;
        }
        return current_;
    }
private:
    float current_ = 0, target_ = 0, step_ = 0;
    std::uint32_t remaining_ = 0;
};
}
