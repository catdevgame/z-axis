#pragma once

// The engine's profiler (zprofile.c; zge/debug/profiler.cpp): bars that
// time parts of a frame in EE cycles, drawn as an overlay each frame (the
// display mode on): per bar the time this frame, the peak and tick marks
// per frame of time, with its label.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/debug/profiler.h.h"

namespace zge
{
    enum ProfileBarFlags : uint32_t
    {
        kProfileBarRunning = 1u << 0,  // started and not stopped
        kProfileBarOwnFrame = 1u << 1, // its owner ends its frame (the display does not)
    };

    // newProfileBar's flags: the display flags' mask in the low byte, and
    // this. ZGE3 added the display masks: ZGE2's flags are this bit alone.
    constexpr uint32_t kNewProfileBarOwnFrame = game::readable::perGame(0x10000u, 1u);

    // ZGE4's mark on a bar's row (BMX XXX's drawProfileTime): from the time
    // of cycles on, the row is drawn in this colour.
    struct ProfileColourChange
    {
        uint32_t cycles;            // +0x00 where the colour changes (in the bar's cycles)
        uint8_t colour[4];          // +0x04 R, G, B, A from there on
    };
    static_assert(sizeof(ProfileColourChange) == 8);
    constexpr uint32_t kProfileColourChanges = 10; // (BMX XXX's endProfileBarFrame copies 0x50 bytes of them)

    // A bar (0x30 bytes, from the profiler's pool; ZGE2's 0x28, with no
    // display mask: DM2's newProfileBar, startProfileBar, stopProfileBar;
    // ZGE4's 0xE0, with the last frame's cycles after its colour changes:
    // BMX XXX's newProfileBar, endProfileBarFrame, drawProfileTime).
    struct ProfileBar
    {
        uint32_t flags;             // +0x00 ProfileBarFlags (the low word of a 64-bit bit-field word)
        uint32_t id;                // +0x04 1: the application's, 2: the engine CPU's
        uint32_t label;             // +0x08 its name (a C string), drawn after the bar
        uint32_t cycles;            // +0x0C counted this frame
#if !ZX_GAME_BMX
        uint32_t lastFrameCycles;   // +0x10 the last frame's (ZGE4's is at +0x84)
#endif
        uint32_t peakCycles;        // +0x14 (BMX +0x10) the most a frame took (reset by resetProfilePeaks)
        ee::Float scale;            // +0x18 (BMX +0x14) the frames of time the bar's length spans
#if ZX_GAME_BMX
        // +0x18 two times (in frames) BMX XXX's drawProfileTicks marks on the
        // row with a tick of their own when above 0 (nothing in BMX XXX sets them).
        ee::Float markedFrames[2];
#endif
        uint8_t colour[4];          // +0x1C (BMX +0x20) R, G, B, A
#if !ZX_GAME_DM2
        uint32_t displayMask;       // +0x20 (BMX +0x24) display flags set while it runs (setDisplayFlags)
#endif
#if !ZX_GAME_DM2 && !ZX_GAME_BMX
        uint32_t _24;
#endif
        uint64_t startCount;        // +0x28 (DM2 +0x20) the cycle count when it was started
#if ZX_GAME_BMX
        // ZGE4's colour changes this frame and the last frame's (kept by
        // endProfileBarFrame, drawn while the bar runs). Nothing in BMX
        // XXX adds one: the counts stay 0.
        uint32_t colourChangeCount;                                   // +0x30
        ProfileColourChange colourChanges[kProfileColourChanges];     // +0x34
        uint32_t lastFrameCycles;                                     // +0x84
        uint32_t lastFrameColourChangeCount;                          // +0x88
        ProfileColourChange lastFrameColourChanges[kProfileColourChanges]; // +0x8C
#endif
    };
    static_assert(offsetof(ProfileBar, peakCycles) == game::readable::perGame(0x14u, 0x14u, 0x10u) &&
                  offsetof(ProfileBar, scale) == game::readable::perGame(0x18u, 0x18u, 0x14u) &&
                  offsetof(ProfileBar, colour) == game::readable::perGame(0x1Cu, 0x1Cu, 0x20u) &&
                  offsetof(ProfileBar, lastFrameCycles) == game::readable::perGame(0x10u, 0x10u, 0x84u) &&
                  offsetof(ProfileBar, startCount) == game::readable::perGame(0x28u, 0x20u, 0x28u) &&
                  sizeof(ProfileBar) == game::readable::perGame(0x30u, 0x28u, 0xE0u));
#if !ZX_GAME_DM2
    static_assert(offsetof(ProfileBar, displayMask) == game::readable::perGame(0x20u, 0x20u, 0x24u));
#endif
#if ZX_GAME_BMX
    static_assert(offsetof(ProfileBar, markedFrames) == 0x18 && offsetof(ProfileBar, colourChangeCount) == 0x30 &&
                  offsetof(ProfileBar, colourChanges) == 0x34 && offsetof(ProfileBar, lastFrameColourChangeCount) == 0x88 &&
                  offsetof(ProfileBar, lastFrameColourChanges) == 0x8C);
#endif

    // The overlay's layout in pixels (setProfileLayout copies all of it).
    struct ProfileLayout
    {
        int32_t margin;             // +0x00 32
        int32_t originX, originY;   // +0x04 the overlay's corner less the margin
        int32_t barHeight;          // +0x0C 8
        int32_t timeHeight;         // +0x10 6: the time bar inside the peak's
        int32_t rowGap;             // +0x14 4
        int32_t tickAbove;          // +0x18 4: a tick's height above the row
        int32_t tickWidth;          // +0x1C 8
        int32_t tickBelow;          // +0x20 4: below the row's top
        int32_t barScale;           // +0x24 1: a new bar's scale (frames)
        int32_t peakHoldFrames;     // +0x28 120: the peaks are reset this often
        int32_t frameRate;          // +0x2C 60: frames a second (cycles to time)
    };
    static_assert(sizeof(ProfileLayout) == 0x30);

    struct Task; // (zge/platform/tasks.h)

    // The profiler's state (g_profilerReady, 0x78 bytes).
    struct Profiler
    {
        uint32_t ready;             // +0x00
        uint32_t displayMode;       // +0x04 0: no overlay
        uint32_t barPool;           // +0x08 the bars' pool
        ee::Ptr<Task> displayTask;  // +0x0C the task that draws the overlay (made with the first bar)
        int32_t warmUpFrames;       // +0x10 10: frames before the peaks count and are drawn
        int32_t peakHoldLeft;       // +0x14 frames to the next reset of the peaks
        int32_t peakHoldFrames;     // +0x18 0: the peaks are never reset
        ee::Float left;             // +0x1C the bars' left end
        ee::Float top;              // +0x20 the first row's top
        ee::Float defaultScale;     // +0x24 a new bar's scale
        ee::Float barLength;        // +0x28 a bar's full length (the display's width less the margins)
        uint32_t appBar;            // +0x2C the application's bar and its start and stop tasks
        ee::Ptr<Task> appStartTask;
        ee::Ptr<Task> appStopTask;
        uint32_t engineBar;         // +0x38 the engine CPU's bar and its restart and stop tasks
        ee::Ptr<Task> engineRestartTask;
        ee::Ptr<Task> engineStopTask;
        ProfileLayout layout;       // +0x44
#if ZX_GAME_BMX
        // +0x74 ZGE4 adds these to a label's position (BMX XXX's
        // drawProfileLabel; initProfiler zeroes them and nothing sets them).
        int32_t labelOffsetX, labelOffsetY;
#endif
#if !ZX_GAME_DM2
        uint32_t displayFlags;      // +0x74 (BMX +0x7C) the bars' display masks together (ZGE2 has none: 0x74 bytes)
#endif
    };
    static_assert(offsetof(Profiler, warmUpFrames) == 0x10 && offsetof(Profiler, left) == 0x1C &&
                  offsetof(Profiler, barLength) == 0x28 && offsetof(Profiler, appBar) == 0x2C &&
                  offsetof(Profiler, engineBar) == 0x38 && offsetof(Profiler, layout) == 0x44 &&
                  sizeof(Profiler) == game::readable::perGame(0x78u, 0x74u, 0x80u));
#if !ZX_GAME_DM2
    static_assert(offsetof(Profiler, displayFlags) == game::readable::perGame(0x74u, 0x74u, 0x7Cu));
#endif
#if ZX_GAME_BMX
    static_assert(offsetof(Profiler, labelOffsetX) == 0x74 && offsetof(Profiler, labelOffsetY) == 0x78);
#endif

    constexpr uint32_t kProfilerAddress = sym::g_profilerReady;
    inline ee::Global<Profiler, kProfilerAddress> g_profiler;

    // A bar's display mask (newProfileBar's flags' low byte; setTimerMask
    // while it runs), and the profiler's display flags it is part of. ZGE3
    // added them: in ZGE2 a bar has no mask, and these read 0 and keep nothing.
    inline uint32_t profileBarDisplayMask(const ProfileBar &bar)
    {
#if !ZX_GAME_DM2
        return bar.displayMask;
#else
        (void)bar;
        return 0;
#endif
    }
    inline void addProfileBarDisplayMask(ProfileBar &bar, uint32_t mask)
    {
#if !ZX_GAME_DM2
        bar.displayMask = mask;
        g_profiler->displayFlags |= mask;
#else
        (void)bar, (void)mask;
#endif
    }
    inline void removeProfileBarDisplayMask(const ProfileBar &bar)
    {
#if !ZX_GAME_DM2
        g_profiler->displayFlags &= ~bar.displayMask;
#else
        (void)bar;
#endif
    }
    inline void clearProfileDisplayFlags(Profiler &profiler)
    {
#if !ZX_GAME_DM2
        profiler.displayFlags = 0;
#else
        (void)profiler;
#endif
    }

    // A bar's frame ended in ZGE4: its colour changes kept as the last
    // frame's and cleared; memoryCopy's result. ZGE2 and ZGE3 have none:
    // the result handed in.
    inline int32_t endFrameColourChanges(ee::Address bar, int32_t result)
    {
#if ZX_GAME_BMX
        ProfileBar &b = *ee::Ptr<ProfileBar>(bar);
        b.lastFrameColourChangeCount = b.colourChangeCount;
        result = static_cast<int32_t>(memoryCopy(bar + static_cast<uint32_t>(offsetof(ProfileBar, lastFrameColourChanges)),
                                                 bar + static_cast<uint32_t>(offsetof(ProfileBar, colourChanges)),
                                                 static_cast<uint32_t>(sizeof(b.colourChanges))));
        b.colourChangeCount = 0;
#else
        (void)bar;
#endif
        return result;
    }

    // ZGE4's two steps of its own drawing (profiler.cpp; BMX XXX's drawProfileTime and drawProfileTicks in
    // games/bmx/debug/profiler.cpp call them): a bar's length for cycles at a scale, and a tick mark at whole
    // screen units.
    ee::Float profileBarWidth(int32_t cycles, ee::Float scale);
    int32_t drawProfileTickAt(ee::Address triangles, int32_t x, int32_t y, ee::Ptr<uint8_t> colour);

    // ZGE4's offset of the bars' labels (Profiler::labelOffsetX, Y): 0 in
    // ZGE2 and ZGE3, which have none.
    inline void clearProfileLabelOffset(Profiler &profiler)
    {
#if ZX_GAME_BMX
        profiler.labelOffsetX = 0;
        profiler.labelOffsetY = 0;
#else
        (void)profiler;
#endif
    }
    inline int32_t profileLabelOffsetX(const Profiler &profiler)
    {
#if ZX_GAME_BMX
        return profiler.labelOffsetX;
#else
        (void)profiler;
        return 0;
#endif
    }
    inline int32_t profileLabelOffsetY(const Profiler &profiler)
    {
#if ZX_GAME_BMX
        return profiler.labelOffsetY;
#else
        (void)profiler;
        return 0;
#endif
    }
}

namespace zge
{
    // (profiler.cpp)
    int32_t initProfiler();
    uint32_t shutDownProfiler();
    uint32_t newProfileBar(uint32_t flags, uint32_t id, ee::Address colour, uint32_t label);
    uint32_t appProfileBar();
    int32_t freeProfileBar(ee::Address bar);
    void setProfileDisplayMode(uint32_t mode);
    uint32_t startProfileBar(ee::Address bar);
    uint32_t stopProfileBar(ee::Address bar);
    int32_t endProfileBarFrame(ee::Address bar);
    void setProfileBarScale(ee::Address bar, ee::Float scale);
}

namespace zge
{
    // (profiler_label.cpp) a bar's label after its row (debugPrint's format).
    void drawProfileLabel(ee::Ptr<ProfileBar> bar, ee::Float top);
}
