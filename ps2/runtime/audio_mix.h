#pragma once

// Host sound mixed into the emulated SPU2's output: a native replacement for
// something the game played through the SPU2 (a movie's sound, say) adds its
// 48 kHz stereo frames here. They are mixed in where the SPU2's frames are
// produced, in emulated time, so they reach the host output (and the
// ZX_SPU2_WAV dump) in step with the rest of the game's sound, after the
// same DC-blocking high-pass the SPU2's output stage applies.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace zx::audio_mix
{
    // Adds `frames` interleaved stereo frames to `mix` (it may leave it
    // unchanged when it has nothing to play). Runs on the thread that
    // produces the SPU2's frames.
    using Source = std::function<void(int32_t *mix, size_t frames)>;

    // Returns an id for removeSource.
    int addSource(Source source);
    void removeSource(int id);

    // Mixes every source into `stereo` (saturating). Called by the SPU2 host
    // output for each block it produces; does nothing without sources.
    void mix(int16_t *stereo, size_t frames);
    // The movie sound volume (the sources' sum is scaled by it; 1 = as the
    // game made it). Any thread.
    void setGain(float gain);
    [[nodiscard]] float gain();
    [[nodiscard]] bool hasSources();
}
