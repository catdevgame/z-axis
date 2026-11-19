#pragma once

// Host side of the emulated SPU2: plays the 48 kHz stereo frames that iop/
// mixes (IopHost::audioSamples) through an SDL3 audio stream, and optionally
// dumps them to a WAV file.
//
// Timing: frames are produced in emulated IOP time on the game thread and
// consumed by the audio device thread. A lock-free ring buffer sits between
// them. The consumer keeps the fill level near a target with a small
// (at most +-2 %, none within a quarter of the target) playback-rate
// correction (4-point cubic resampling); on overflow it drops the oldest
// frames, on underrun it fades the last frame out and re-buffers.
//
// Environment:
//   PS2X_SPU2_AUDIO=0           do not open a host audio stream
//   PS2X_SPU2_WAV=<path>        write the mixed output (s16 stereo 48 kHz WAV)
//   PS2X_SPU2_WAV_SECONDS=<n>   stop the WAV after n seconds (default 120)
//   PS2X_SPU2_LATENCY_MS=<n>    ring buffer target latency (default 60)
//   PS2X_SPU2_STATS=<n>         log level/buffer stats every n emulated seconds (default 30, 0 = off)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

class PS2Spu2Audio
{
public:
    PS2Spu2Audio();
    ~PS2Spu2Audio();

    PS2Spu2Audio(const PS2Spu2Audio &) = delete;
    PS2Spu2Audio &operator=(const PS2Spu2Audio &) = delete;

    // Game/IOP thread: append interleaved stereo frames.
    void submit(const int16_t *interleavedStereo, size_t frames);

    // Audio thread: fill `frames` interleaved stereo frames.
    void render(int16_t *out, size_t frames);

    // Frames submitted since the last non-zero sample (any thread).
    [[nodiscard]] uint64_t silentFrames() const noexcept { return m_silentFrames.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kSampleRate = 48000u;
    static constexpr size_t kRingFrames = 1u << 15; // ~680 ms

    void openStream();
    void closeStream();
    void writeWav(const int16_t *frames, size_t count);
    void finishWav();
    void updateStats(const int16_t *frames, size_t count);

    size_t ringFill() const noexcept;

    // Ring buffer (single producer, single consumer), interleaved stereo.
    std::vector<int16_t> m_ring;
    std::atomic<uint64_t> m_writePos{0};
    std::atomic<uint64_t> m_readPos{0};

    // Consumer state (audio thread only).
    double m_frac = 0.0;
    bool m_buffering = true;
    int16_t m_last[2] = {0, 0};
    std::atomic<uint64_t> m_underruns{0};
    std::atomic<uint64_t> m_dropped{0};
    std::atomic<uint64_t> m_silentFrames{0};

    // Configuration.
    size_t m_targetFrames = 2880;
    bool m_streamWanted = true;
    bool m_streamOpen = false;
    bool m_streamTried = false;
    void *m_stream = nullptr; // SDL_AudioStream (opaque here)

    // WAV dump (producer thread only).
    std::FILE *m_wav = nullptr;
    uint64_t m_wavFrames = 0;
    uint64_t m_wavLimitFrames = 0;
    uint64_t m_wavHeaderUpdatedAt = 0;
    std::string m_wavPath;

    // Stats (producer thread only).
    std::chrono::steady_clock::time_point m_firstSubmit{};
    uint64_t m_totalFrames = 0;
    uint64_t m_statsEveryFrames = 0;
    uint64_t m_statsNextAt = 0;
    double m_statsSumSquares = 0.0;
    uint64_t m_statsCount = 0;
    int m_statsPeak = 0;
};
