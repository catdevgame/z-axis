#include "spu2_audio.h"

#include "host_backend.h"
#include "runtime/audio_mix.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    std::mutex g_instanceMutex;
    PS2Spu2Audio *g_instance = nullptr;

    long envLong(const char *name, long fallback)
    {
        const char *value = std::getenv(name);
        if (!value || !*value)
            return fallback;
        char *end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        return end != value ? parsed : fallback;
    }

    void putLe32(uint8_t *p, uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    void putLe16(uint8_t *p, uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    void writeWavHeader(std::FILE *file, uint64_t frames)
    {
        const uint32_t dataBytes = static_cast<uint32_t>(std::min<uint64_t>(frames * 4u, 0xFFFFFFFFull - 36u));
        uint8_t header[44];
        std::memcpy(header, "RIFF", 4);
        putLe32(header + 4, 36u + dataBytes);
        std::memcpy(header + 8, "WAVEfmt ", 8);
        putLe32(header + 16, 16u);     // fmt chunk size
        putLe16(header + 20, 1u);      // PCM
        putLe16(header + 22, 2u);      // channels
        putLe32(header + 24, 48000u);  // sample rate
        putLe32(header + 28, 48000u * 4u);
        putLe16(header + 32, 4u);      // block align
        putLe16(header + 34, 16u);     // bits per sample
        std::memcpy(header + 36, "data", 4);
        putLe32(header + 40, dataBytes);
        const long position = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        std::fwrite(header, 1, sizeof(header), file);
        std::fseek(file, position < 44 ? 44 : position, SEEK_SET);
    }

    // SDL asks for more data when the device has played what it had:
    // render exactly that many frames.
    void SDLCALL streamCallback(void *, SDL_AudioStream *stream, int additionalBytes, int)
    {
        if (additionalBytes <= 0)
            return;
        thread_local std::vector<int16_t> buffer;
        const size_t frames = static_cast<size_t>(additionalBytes) / 4u;
        buffer.resize(frames * 2u);
        {
            std::unique_lock lock(g_instanceMutex, std::try_to_lock);
            if (!lock.owns_lock() || !g_instance)
                std::fill(buffer.begin(), buffer.end(), int16_t{0});
            else
                g_instance->render(buffer.data(), frames);
        }
        SDL_PutAudioStreamData(stream, buffer.data(), static_cast<int>(frames * 4u));
    }
}

PS2Spu2Audio::PS2Spu2Audio()
    : m_ring(kRingFrames * 2u, 0)
{
    m_streamWanted = envLong("ZX_SPU2_AUDIO", 1) != 0;
    const long latencyMs = std::clamp(envLong("ZX_SPU2_LATENCY_MS", 60), 10L, 400L);
    m_targetFrames = static_cast<size_t>(latencyMs) * kSampleRate / 1000u;

    const long statsSeconds = std::max(0L, envLong("ZX_SPU2_STATS", 30));
    m_statsEveryFrames = static_cast<uint64_t>(statsSeconds) * kSampleRate;
    m_statsNextAt = m_statsEveryFrames;

    if (const char *path = std::getenv("ZX_SPU2_WAV"); path && *path)
    {
        m_wavPath = path;
        const long seconds = std::max(1L, envLong("ZX_SPU2_WAV_SECONDS", 120));
        m_wavLimitFrames = static_cast<uint64_t>(seconds) * kSampleRate;
        m_wav = std::fopen(path, "wb");
        if (m_wav)
        {
            writeWavHeader(m_wav, 0);
            std::cerr << "[spu2] writing mixed output to " << m_wavPath << " (first " << seconds << " s)" << std::endl;
        }
        else
        {
            std::cerr << "[spu2] cannot open WAV output " << m_wavPath << std::endl;
        }
    }
}

PS2Spu2Audio::~PS2Spu2Audio()
{
    closeStream();
    finishWav();
}

void PS2Spu2Audio::openStream()
{
    m_streamTried = true;
    if (!m_streamWanted || !ps2_host::audioReady())
    {
        std::cerr << "[spu2] host audio output disabled ("
                  << (m_streamWanted ? "no audio device" : "ZX_SPU2_AUDIO=0") << ")" << std::endl;
        return;
    }
    // s16 stereo 48 kHz in; SDL converts to the device's format and rate.
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(kSampleRate)};
    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, nullptr);
    if (!stream)
    {
        std::cerr << "[spu2] failed to create the host audio stream: " << SDL_GetError() << std::endl;
        return;
    }
    {
        std::lock_guard lock(g_instanceMutex);
        g_instance = this;
    }
    SDL_SetAudioStreamGetCallback(stream, streamCallback, nullptr);
    if (!SDL_BindAudioStream(static_cast<SDL_AudioDeviceID>(ps2_host::audioDevice()), stream))
    {
        std::cerr << "[spu2] cannot play the host audio stream: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        std::lock_guard lock(g_instanceMutex);
        g_instance = nullptr;
        return;
    }
    m_stream = stream;
    m_streamOpen = true;
    int deviceFrames = 0;
    SDL_AudioSpec deviceSpec{};
    SDL_GetAudioDeviceFormat(static_cast<SDL_AudioDeviceID>(ps2_host::audioDevice()), &deviceSpec, &deviceFrames);
    std::cerr << "[spu2] host audio stream open: 48000 Hz stereo, target latency "
              << (m_targetFrames * 1000u / kSampleRate) << " ms (" << ps2_host::audioDriver() << " device "
              << deviceSpec.freq << " Hz, "
              << deviceFrames << " frames per buffer)" << std::endl;
}

void PS2Spu2Audio::closeStream()
{
    if (m_stream)
    {
        // Unbinds it; the callback is not running afterwards.
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream *>(m_stream));
        m_stream = nullptr;
    }
    std::lock_guard lock(g_instanceMutex);
    if (g_instance == this)
        g_instance = nullptr;
    m_streamOpen = false;
}

size_t PS2Spu2Audio::ringFill() const noexcept
{
    return static_cast<size_t>(m_writePos.load(std::memory_order_acquire) -
                               m_readPos.load(std::memory_order_acquire));
}

void PS2Spu2Audio::submit(const int16_t *frames, size_t count)
{
    if (!frames || count == 0)
        return;
    // Host sound that replaces something the game played through the SPU2
    // (runtime/audio_mix.h) joins here, in emulated time.
    thread_local std::vector<int16_t> mixed;
    if (zx::audio_mix::hasSources())
    {
        mixed.assign(frames, frames + count * 2u);
        zx::audio_mix::mix(mixed.data(), count);
        frames = mixed.data();
    }
    if (!m_streamTried)
    {
        m_firstSubmit = std::chrono::steady_clock::now();
        openStream();
    }

    writeWav(frames, count);
    updateStats(frames, count);

    size_t lastSound = count;
    for (size_t i = count; i-- > 0;)
    {
        if (frames[i * 2u] != 0 || frames[i * 2u + 1u] != 0)
        {
            lastSound = i;
            break;
        }
    }
    const bool silent = lastSound == count;
    if (silent)
        m_silentFrames.fetch_add(count, std::memory_order_relaxed);
    else
        m_silentFrames.store(count - 1u - lastSound, std::memory_order_relaxed);

    if (!m_streamOpen)
        return;

    const uint64_t write = m_writePos.load(std::memory_order_relaxed);
    const uint64_t read = m_readPos.load(std::memory_order_acquire);
    // Silence beyond the target fill is not kept: when the guest runs ahead
    // of real time (a fast-forwarded loading screen) it would only delay the
    // sound that follows.
    if (silent && static_cast<size_t>(write - read) >= m_targetFrames)
        return;
    const size_t space = kRingFrames - static_cast<size_t>(write - read);
    const size_t accepted = std::min(count, space);
    if (accepted < count)
        m_dropped.fetch_add(count - accepted, std::memory_order_relaxed);
    for (size_t i = 0; i < accepted; ++i)
    {
        const size_t slot = static_cast<size_t>((write + i) & (kRingFrames - 1u)) * 2u;
        m_ring[slot] = frames[i * 2u];
        m_ring[slot + 1u] = frames[i * 2u + 1u];
    }
    m_writePos.store(write + accepted, std::memory_order_release);
}

void PS2Spu2Audio::render(int16_t *out, size_t frames)
{
    size_t produced = 0;
    size_t fill = ringFill();

    if (m_buffering && fill >= m_targetFrames)
    {
        m_buffering = false;
        m_frac = 0.0;
    }

    if (!m_buffering)
    {
        // Far too much buffered (emulation ran ahead, or the device stalled): drop to target.
        if (fill > m_targetFrames * 3u + frames)
        {
            const size_t drop = fill - m_targetFrames;
            m_readPos.fetch_add(drop, std::memory_order_acq_rel);
            m_dropped.fetch_add(drop, std::memory_order_relaxed);
            fill -= drop;
        }

        // Keep the fill near the target with the playback rate. Within a
        // quarter of the target the rate is exactly 1 (with m_frac 0 the
        // output is the mix itself); beyond it up to +-2 %, reached at
        // three quarters off, so a short emulation stall slows playback
        // down before the ring runs dry.
        const double error = (static_cast<double>(fill) - static_cast<double>(m_targetFrames)) /
                             static_cast<double>(m_targetFrames);
        double step = 1.0;
        if (error > 0.25)
            step += std::min((error - 0.25) * 0.04, 0.02);
        else if (error < -0.25)
            step -= std::min((-error - 0.25) * 0.04, 0.02);

        uint64_t read = m_readPos.load(std::memory_order_relaxed);
        const uint64_t write = m_writePos.load(std::memory_order_acquire);
        auto at = [this](uint64_t frame, int c)
        {
            return static_cast<double>(m_ring[static_cast<size_t>(frame & (kRingFrames - 1u)) * 2u + static_cast<size_t>(c)]);
        };
        for (; produced < frames; ++produced)
        {
            // 4-point cubic (Catmull-Rom) between read and read + 1; needs
            // one frame before and two after.
            if (read + 2u >= write)
            {
                m_buffering = true;
                m_underruns.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            for (int c = 0; c < 2; ++c)
            {
                double v;
                if (m_frac == 0.0)
                {
                    v = at(read, c);
                }
                else
                {
                    const double p0 = at(read - 1u, c);
                    const double p1 = at(read, c);
                    const double p2 = at(read + 1u, c);
                    const double p3 = at(read + 2u, c);
                    const double t = m_frac;
                    v = p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
                }
                out[produced * 2u + static_cast<size_t>(c)] =
                    static_cast<int16_t>(std::clamp<long>(std::lround(v), -32768L, 32767L));
            }
            m_last[0] = out[produced * 2u];
            m_last[1] = out[produced * 2u + 1u];
            m_frac += step;
            const double whole = std::floor(m_frac);
            m_frac -= whole;
            read += static_cast<uint64_t>(whole);
        }
        m_readPos.store(read, std::memory_order_release);
    }

    // Underrun or still buffering: fade the last frame out to avoid a click.
    for (size_t i = produced; i < frames; ++i)
    {
        m_last[0] = static_cast<int16_t>(m_last[0] * 15 / 16);
        m_last[1] = static_cast<int16_t>(m_last[1] * 15 / 16);
        out[i * 2u] = m_last[0];
        out[i * 2u + 1u] = m_last[1];
    }
}

void PS2Spu2Audio::writeWav(const int16_t *frames, size_t count)
{
    if (!m_wav || m_wavFrames >= m_wavLimitFrames)
        return;
    const size_t take = static_cast<size_t>(std::min<uint64_t>(count, m_wavLimitFrames - m_wavFrames));
    std::fwrite(frames, sizeof(int16_t) * 2u, take, m_wav);
    m_wavFrames += take;
    // Keep the header valid so a killed process still leaves a playable file.
    if (m_wavFrames - m_wavHeaderUpdatedAt >= kSampleRate || m_wavFrames >= m_wavLimitFrames)
    {
        writeWavHeader(m_wav, m_wavFrames);
        std::fflush(m_wav);
        m_wavHeaderUpdatedAt = m_wavFrames;
    }
    if (m_wavFrames >= m_wavLimitFrames)
    {
        std::cerr << "[spu2] WAV complete: " << m_wavPath << " (" << (m_wavFrames / kSampleRate) << " s)" << std::endl;
        finishWav();
    }
}

void PS2Spu2Audio::finishWav()
{
    if (!m_wav)
        return;
    writeWavHeader(m_wav, m_wavFrames);
    std::fclose(m_wav);
    m_wav = nullptr;
}

void PS2Spu2Audio::updateStats(const int16_t *frames, size_t count)
{
    m_totalFrames += count;
    if (m_statsEveryFrames == 0)
        return;
    for (size_t i = 0; i < count * 2u; ++i)
    {
        const int v = frames[i];
        m_statsSumSquares += static_cast<double>(v) * v;
        m_statsPeak = std::max(m_statsPeak, v < 0 ? -v : v);
    }
    m_statsCount += count * 2u;
    if (m_totalFrames < m_statsNextAt)
        return;

    const double rms = m_statsCount ? std::sqrt(m_statsSumSquares / static_cast<double>(m_statsCount)) : 0.0;
    const double dbfs = rms > 0.0 ? 20.0 * std::log10(rms / 32768.0) : -120.0;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_firstSubmit).count();
    std::cerr << "[spu2] " << (m_totalFrames / kSampleRate) << " s mixed in " << static_cast<int>(wall * 10.0) / 10.0
              << " s wall (" << static_cast<int>(100.0 * static_cast<double>(m_totalFrames) / kSampleRate / (wall > 0.0 ? wall : 1.0))
              << " % speed): rms " << static_cast<int>(rms)
              << " (" << static_cast<int>(std::lround(dbfs)) << " dBFS) peak " << m_statsPeak
              << ", host ring " << (ringFill() * 1000u / kSampleRate) << " ms, underruns "
              << m_underruns.load(std::memory_order_relaxed) << ", dropped "
              << m_dropped.load(std::memory_order_relaxed) << " frames" << std::endl;
    m_statsNextAt += m_statsEveryFrames;
    m_statsSumSquares = 0.0;
    m_statsCount = 0;
    m_statsPeak = 0;
}
