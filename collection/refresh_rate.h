#pragma once

// The refresh rate the app asks of a screen whose rate it picks (Android's:
// GameActivity's preferredRefreshRate). The games make 60 frames a second.
// With Frame Interpolation on, a screen at twice that shows a picture between
// each two of them (runtime/frame_interpolation.h), so 120 is asked where
// the screen has it; otherwise 60, since at 120 Hz each frame would only be
// presented twice.

#include <span>

namespace zx::app
{
    inline constexpr float kGameRefreshRate = 60.0f;
    inline constexpr float kInterpolatedRefreshRate = 120.0f;

    // `offeredRates`: the screen's rates at its present size, in Hz (a rate
    // within 1 Hz of another counts as it).
    float refreshRateToAsk(bool frameInterpolation, std::span<const float> offeredRates);
}
