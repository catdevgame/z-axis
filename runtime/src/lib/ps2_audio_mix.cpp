#include "runtime/ps2_audio_mix.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace zx::audio_mix
{
    namespace
    {
        struct Registry
        {
            std::mutex mutex;
            std::vector<std::pair<int, Source>> sources;
            int nextId = 1;
            std::atomic<bool> any{false};
            std::atomic<float> gain{1.0f};
            // The SPU2 output's DC-blocking high-pass (iop/ spu2_mixer.cpp
            // Output32, from PCSX2), run on the sources' sum.
            float dcIn[2] = {0.0f, 0.0f};
            float dcOut[2] = {0.0f, 0.0f};
        };

        Registry &registry()
        {
            static Registry instance;
            return instance;
        }
    }

    int addSource(Source source)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        const int id = r.nextId++;
        r.sources.emplace_back(id, std::move(source));
        r.dcIn[0] = r.dcIn[1] = r.dcOut[0] = r.dcOut[1] = 0.0f;
        r.any.store(true, std::memory_order_release);
        return id;
    }

    void removeSource(int id)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.sources.erase(std::remove_if(r.sources.begin(), r.sources.end(),
                                       [id](const std::pair<int, Source> &s) { return s.first == id; }),
                        r.sources.end());
        r.any.store(!r.sources.empty(), std::memory_order_release);
    }

    void setGain(float gain)
    {
        registry().gain.store(std::clamp(gain, 0.0f, 4.0f), std::memory_order_relaxed);
    }

    float gain()
    {
        return registry().gain.load(std::memory_order_relaxed);
    }

    bool hasSources()
    {
        return registry().any.load(std::memory_order_acquire);
    }

    void mix(int16_t *stereo, size_t frames)
    {
        Registry &r = registry();
        if (!stereo || frames == 0u || !r.any.load(std::memory_order_acquire))
            return;
        // The sources go through the high-pass the SPU2's own output went
        // through, so they sound as if the SPU2 had mixed them (the filter
        // is linear, so filtering their sum apart is the same).
        thread_local std::vector<int32_t> accumulator;
        accumulator.assign(frames * 2u, 0);
        const float gain = r.gain.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(r.mutex);
        for (const auto &source : r.sources)
            if (source.second)
                source.second(accumulator.data(), frames);
        for (size_t i = 0; i < frames; ++i)
        {
            for (size_t c = 0; c < 2u; ++c)
            {
                const float in = static_cast<float>(accumulator[i * 2u + c]) * gain / 32767.0f;
                const float out = in - r.dcIn[c] + 0.995f * r.dcOut[c];
                r.dcIn[c] = in;
                r.dcOut[c] = out;
                const int32_t mixed = stereo[i * 2u + c] + static_cast<int32_t>(std::lround(out * 32767.0f));
                stereo[i * 2u + c] = static_cast<int16_t>(std::clamp<int32_t>(mixed, -32768, 32767));
            }
        }
    }
}
