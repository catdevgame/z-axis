#pragma once

// The three volume sliders the front end and the pause menu share.

#include <cstdint>

namespace aggressive
{
    constexpr uint32_t kVolumeSfx = 0, kVolumeMusic = 1, kVolumeSpeech = 2;

    // (slider, muted) the slider's value, 0 .. 127, as the channel's volume.
    int32_t setChannelVolume(uint32_t function, uint32_t arguments, uint32_t channel);
    // (slider,) the channel's volume shown on the slider, rounded up to even.
    int32_t showChannelVolume(uint32_t function, uint32_t arguments, uint32_t channel, uint32_t currentValueName, uint32_t onChangeScript);
}
