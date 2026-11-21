// The guest heap's high-water marks per level (ZX_TRACE_HEAP=1, developers; runtime/heap_trace.h).
#include "runtime/heap_trace.h"

#include "ps2_runtime.h"
#include "runtime/panel_snapshot.h"
#include "runtime/ps2_memory.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>

namespace zx::heap_trace
{
    namespace
    {
        constexpr uint64_t kNameEvery = 10; // vsyncs between two readings of the level's name

        // The level's name as the running game's panel capture has it.
        std::string levelName(const uint8_t *rdram)
        {
            static panel::Snapshot snapshot; // (large: one, on the EE thread alone)
            panel::captureSnapshot(rdram, PS2_LEGACY_RAM_SIZE, snapshot);
            if (snapshot.mode == panel::GameMode::NoLevel)
                return "no level";
            std::string name(snapshot.levelFile, strnlen(snapshot.levelFile, sizeof(snapshot.levelFile)));
            return name.empty() ? "level " + std::to_string(snapshot.level) : name;
        }
    }

    bool enabled()
    {
        const char *v = std::getenv("ZX_TRACE_HEAP");
        return v && v[0] && !(v[0] == '0' && v[1] == '\0');
    }

    void install(PS2Runtime &runtime)
    {
        if (!enabled())
            return;
        auto levels = std::make_shared<Levels>();
        runtime.addVSyncHook([&runtime, levels](uint8_t *rdram, uint64_t tick) {
            if (!rdram || tick % kNameEvery != 0u)
                return;
            bool ended = false;
            const std::string line = levels->note(levelName(rdram), tick, runtime.guestHeapUsage(), ended);
            if (ended)
                runtime.resetGuestHeapPeaks();
            if (!line.empty())
                std::cout << line << std::endl;
        });
        std::cout << "[heap] the guest heap's peaks per level (ZX_TRACE_HEAP)" << std::endl;
    }

    std::string Levels::note(const std::string &level, uint64_t tick, const GuestHeap::Usage &usage, bool &ended)
    {
        ended = false;
        if (!m_started)
        {
            m_started = true;
            m_level = level;
            m_from = m_lastReport = tick;
            return "";
        }
        if (level != m_level)
        {
            const std::string line = describe(m_level, "ended", usage, m_from, tick);
            ended = true;
            m_level = level;
            m_from = m_lastReport = tick;
            m_anyReported = false;
            return line;
        }
        if (tick - m_lastReport < kReportEvery)
            return "";
        m_lastReport = tick;
        if (m_anyReported && usage.mainPeak == m_reported.mainPeak && usage.mainInUse == m_reported.mainInUse &&
            usage.extendedPeak == m_reported.extendedPeak && usage.extendedInUse == m_reported.extendedInUse)
            return "";
        m_reported = usage;
        m_anyReported = true;
        return describe(m_level, "so far", usage, m_from, tick);
    }

    std::string Levels::describe(const std::string &level, const char *why, const GuestHeap::Usage &usage,
                                 uint64_t from, uint64_t to)
    {
        std::ostringstream o;
        o << "[heap] " << level << " (" << why << "): 32 MB heap peak " << usage.mainPeak << " bytes, "
          << usage.mainInUse << " in use; extended peak " << usage.extendedPeak << " bytes, " << usage.extendedInUse
          << " in use; ticks " << from << ".." << to;
        return o.str();
    }
}
