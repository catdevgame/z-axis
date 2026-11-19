#pragma once

// The guest heap's high-water marks per level (developers; PS2X_TRACE_HEAP=1, off by default).
//
// Every malloc of the game and the runtime's own guest allocations go to one heap (runtime/guest_heap.h): the
// 32 MB heap first, extended memory when it has no room or is asked for. The heap counts the bytes in use in
// each and the most each has held (GuestHeap::Usage). With PS2X_TRACE_HEAP set (not "0") a VSync hook names the
// level being played every 10 vsyncs from the running game's panel capture (runtime/panel_snapshot.h: the level
// folder, "level N" without one, "no level" in the front end and while a level loads) and logs, when that name
// changes, the stretch that ended with its peaks, then starts the peaks again at what is in use; and every 600
// vsyncs the stretch so far when its numbers moved:
//
//   [heap] woodward (ended): 32 MB heap peak 23456768 bytes, 21000000 in use; extended peak 0 bytes, 0 in use; ticks 1200..5400
//
// (tools/playbot/memuse.py's heap() reads these.) Off, nothing is installed: no hook, no capture, no log. The
// counting in the heap is a few additions per allocation and changes no block; the hook reads the guest's RAM
// and writes nothing, so the game runs the same either way.

#include "runtime/guest_heap.h"

#include <cstdint>
#include <string>

class PS2Runtime;

namespace ps2x::heap_trace
{
    // PS2X_TRACE_HEAP set and not "0".
    bool enabled();
    // When enabled: the VSync hook above, on `runtime`. Otherwise nothing.
    void install(PS2Runtime &runtime);

    // The accounting, apart from the hook so it can be tested: the stretch of one level name.
    class Levels
    {
    public:
        // The level name at `tick`, with the heap's usage then. A line to log ("" none): the stretch that ended
        // when the name changed (`ended` true: then the caller resets the heap's peaks), or every
        // kReportEvery vsyncs the stretch so far if its usage moved since its last line.
        std::string note(const std::string &level, uint64_t tick, const GuestHeap::Usage &usage, bool &ended);

        static constexpr uint64_t kReportEvery = 600;

        // "[heap] woodward (ended): 32 MB heap peak ... ticks 1200..5400" (`why`: "ended", "so far")
        static std::string describe(const std::string &level, const char *why, const GuestHeap::Usage &usage,
                                    uint64_t from, uint64_t to);

    private:
        std::string m_level;
        bool m_started = false;
        uint64_t m_from = 0;
        uint64_t m_lastReport = 0;
        GuestHeap::Usage m_reported{};
        bool m_anyReported = false;
    };
}
