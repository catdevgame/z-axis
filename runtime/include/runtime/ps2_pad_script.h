#pragma once

#include "runtime/ps2_state_probe.h"

#include <cstdint>
#include <string>
#include <vector>

// ZX_PAD_SCRIPT="<t>:<BUTTON>[:<dur>],..." presses port-0 buttons at fixed
// script times (default duration 0.25 s), on top of real input.
// "<t>:LX=<v>[:<dur>]" (also LY, RX, RY) holds a stick axis at v, from -1
// (left/up) to 1 (right/down), overriding real input while active.
// Times and durations are seconds, or vsyncs with a "v" suffix ("2397v").
// ZX_PAD_SCRIPT=@<file> reads the script from a file (a long one).
//
// "<t>:SYNC=<cond>[&<cond>]" anchors the script to the game: the condition
// (StateProbe syntax, e.g. 0x276FE4==3) is the event that happened at script
// time t in the recorded run. The event is the condition becoming true: it
// must be false at least once after the previous anchor. Anchors are taken
// in order. If the event happens earlier, the script clock jumps forward to
// t; if the clock reaches t first, it stops at t until the event happens. This replays closed-loop
// recordings (waits on game state) when the native timing differs, e.g.
// across loading screens.
//
// "<t>:WAIT=<cond>[&<cond>]" only holds: when the clock reaches t and the
// condition is false, it stands at t until the condition is true. It never
// moves the clock forward, so the recorded timing is kept when the game is
// on time or early.
class PadScript
{
public:
    static constexpr double kVsyncsPerSecond = 59.94;
    static constexpr uint8_t kStickCenter = 0x80;

    enum StickAxis : int
    {
        kAxisNone = -1,
        kAxisRx = 0,
        kAxisRy = 1,
        kAxisLx = 2,
        kAxisLy = 3
    };

    struct Press
    {
        double start = 0.0; // vsyncs
        double end = 0.0;
        uint16_t mask = 0u;
        int axis = kAxisNone;
        uint8_t axisValue = kStickCenter;
    };

    struct Sync
    {
        double time = 0.0; // vsyncs
        std::vector<StateProbe::Condition> conditions;
        bool holdOnly = false; // WAIT
    };

    struct SyncEvent
    {
        size_t index = 0u;
        double scriptTime = 0.0; // the anchor time t
        double shift = 0.0;      // clock change at this anchor, in vsyncs: > 0 jumped forward
                                 // (event came early), < 0 stood at t (event came late)
        bool waited = false;
    };

    // Parses the spec; unknown entries are reported through warn (if set)
    // and skipped.
    void parse(const std::string &spec, std::vector<std::string> *warnings = nullptr);
    bool empty() const { return m_presses.empty() && m_syncs.empty(); }

    // Advances the clock to clockVsyncs (a monotonic vsync count: emulated
    // vsyncs, or wall seconds * 59.94) and evaluates anchors against rdram.
    // The first call is the script origin. Returns the pressed buttons and
    // writes active stick axes to sticks[0..3] (RX, RY, LX, LY).
    uint16_t update(double clockVsyncs, uint64_t tick, const uint8_t *rdram, uint8_t *sticks,
                    SyncEvent *event = nullptr);
    // Same moment as the last update(), without evaluating anchors (for
    // other ports reading the same frame).
    uint16_t current(uint8_t *sticks) const;

    double scriptTime() const { return m_scriptTime; }
    size_t nextSync() const { return m_nextSync; }
    const std::vector<Press> &presses() const { return m_presses; }
    const std::vector<Sync> &syncs() const { return m_syncs; }

    static uint16_t buttonMask(const std::string &name);

private:
    std::vector<Press> m_presses;
    std::vector<Sync> m_syncs;
    bool m_started = false;
    double m_origin = 0.0;
    double m_offset = 0.0;
    double m_offsetAtArm = 0.0;
    double m_scriptTime = 0.0;
    size_t m_nextSync = 0u;
    bool m_waiting = false;
    bool m_sawFalse = false;
};
