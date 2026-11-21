// The engine's profiler (zprofile.c; zge/debug/profiler.h lays out its state
// and bars): bars started and stopped round parts of a frame count EE
// cycles; the overlay task draws each bar's row (its peak, this frame's
// time, tick marks per frame of time, its label) and ends the bars' frames.
// The game makes two bars, the application's and the engine CPU's, each
// started and stopped by tasks at the frame's start and end.
//
// Original quirks kept: newAppProfileBar and newEngineProfileBar return 0
// when their bar exists already (the bar only when they made it).

#include "profiler.h"

#include "cycle_counter.h"
#include "profile_overlay.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/render/draw.h"
#include "zge/render/primitives.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/debug/profiler.cpp.h"

namespace zge
{
    namespace
    {
        using namespace profile_overlay;

        // The screen's width (the cameras' defaults, g_cameraDefaults + 0x0C).
        inline ee::Global<int32_t, sym::g_screenWidth> g_screenWidth;
        // %hi(g_profilerReady), kept in a saved register.

        // The tick marks' steps for a bar's scale under one frame, and the
        // tolerance of the whole-number test.
        inline ee::Global<ee::Float, sym::g_tickStepTenth> g_tickStepTenth;     // 0.1
        inline ee::Global<ee::Float, sym::g_tickStepHundredth> g_tickStepHundredth; // 0.01
        inline ee::Global<ee::Float, sym::g_tickStepThousandth> g_tickStepThousandth; // 0.001
        inline ee::Global<ee::Float, sym::g_tickTolerance> g_tickTolerance;     // 0.00005

        // Colours (R, G, B, A words): the peak's (grey, half alpha), the
        // ticks' at whole frames (white), tenths (red), hundredths (green)
        // and the others (blue), the bars' (green and the engine's).
        constexpr uint32_t kPeakColour = sym::peakColour;
        constexpr uint32_t kWholeTickColour = sym::wholeTickColour;
        constexpr uint32_t kTenthTickColour = sym::tenthTickColour;
        constexpr uint32_t kHundredthTickColour = sym::hundredthTickColour;
        constexpr uint32_t kOtherTickColour = sym::otherTickColour;
        constexpr uint32_t kAppBarColour = sym::appBarColour;
        constexpr uint32_t kAppBarLabel = sym::appBarLabel;    // "App"
        constexpr uint32_t kEngineBarColour = sym::engineBarColour;
        constexpr uint32_t kEngineBarLabel = sym::engineBarLabel; // "EngCPU"
        constexpr uint32_t kSourceFile = sym::sourceFile2;     // "../ngengine/ZGE3/zprofile.c" (the tasks' source)

        // The task priorities.
        constexpr uint32_t kDisplayTaskPriority = 258;
        // (ZGE2 starts and stops the application's bar a stage earlier: 261 and 262)
        constexpr uint32_t kAppStartPriority = game::readable::perGame(262u, 261u), kAppStopPriority = game::readable::perGame(263u, 262u);
        constexpr uint32_t kEngineRestartPriority = 256, kEngineStopPriority = 259;

        // (flags, triangles: a texture and three vertex pointers each, count, matrix)
        constexpr uint32_t kCreatePool = sym::createPool;       // (record bytes, records a block, free callback) -> pool
        constexpr uint32_t kDestroyPool = sym::destroyPool;
        constexpr uint32_t kPoolFree = sym::poolFree;
        constexpr uint32_t kPoolCount = sym::poolCount;        // (pool) -> records in use
        constexpr uint32_t kDrawProfileTicks = sym::drawProfileTicks;
        constexpr uint32_t kDrawProfilePeak = sym::drawProfilePeak;
        constexpr uint32_t kDrawProfileTime = sym::drawProfileTime;
        constexpr uint32_t kDrawProfileDisplay = sym::drawProfileDisplay;
        constexpr uint32_t kStartProfileBarTask = sym::startProfileBarTask;
        constexpr uint32_t kStopProfileBarTask = sym::stopProfileBarTask;
        constexpr uint32_t kRestartProfileBarTask = sym::restartProfileBarTask;
        constexpr uint32_t kFreeProfileBar = sym::freeProfileBar;
        constexpr uint32_t kEndProfileBarFrame = sym::endProfileBarFrame;
        constexpr uint32_t kResetProfilePeaks = sym::resetProfilePeaks;

        Profiler &profiler() { return *g_profiler; }
        ProfileBar &barAt(uint32_t bar) { return *ee::Ptr<ProfileBar>(bar); }

        // The bars' pool, walked by its links.
        uint32_t firstBarLink(uint32_t pool) { return poolFirst(ee::Ptr<PoolList>(pool)); }
        uint32_t nextBarLink(uint32_t link) { return poolNext(ee::Ptr<PoolLink>(link)); }
        uint32_t barOfLink(uint32_t link) { return poolRecord(link); }

        // A bar's length for its cycles: frames of time over its scale, at
        // most the display's width less the left end.
        ee::Float barWidth(ee::Float time, ee::Float scale)
        {
            ee::Float width = time * profiler().barLength / scale;
            const ee::Float screen = unsignedToFloat(static_cast<uint32_t>(*g_screenWidth));
            if (ee::toFloat(ee::toInt(screen - profiler().left)) <= width)
            {
                const ee::Float screenAgain = unsignedToFloat(static_cast<uint32_t>(*g_screenWidth));
                width = ee::toFloat(ee::toInt(screenAgain - profiler().left));
            }
            return width;
        }
        ee::Float barWidth(ee::Float time, const ProfileBar &bar) { return barWidth(time, bar.scale); }

        // A bar of a width and a height from the left end at y, in a colour:
        // the quad's four vertices (top left, bottom left, bottom right, top
        // right).
        void fillQuad(uint32_t quads, ee::Float y, ee::Float width, uint32_t height, ee::Ptr<uint8_t> colour)
        {
            OverlayVertex &topLeft = vertexOf(quads, 0);
            topLeft.x = profiler().left;
            topLeft.y = y;
            setColour(topLeft, colour);
            OverlayVertex &bottomLeft = vertexOf(quads, 1);
            bottomLeft.x = topLeft.x;
            const ee::Float h = unsignedToFloat(height);
            bottomLeft.y = vertexOf(quads, 0).y + h;
            setColour(bottomLeft, colour);
            OverlayVertex &bottomRight = vertexOf(quads, 2);
            bottomRight.x = vertexOf(quads, 0).x + width;
            bottomRight.y = vertexOf(quads, 1).y;
            setColour(bottomRight, colour);
            OverlayVertex &topRight = vertexOf(quads, 3);
            topRight.x = vertexOf(quads, 2).x;
            topRight.y = vertexOf(quads, 0).y;
            setColour(topRight, colour);
        }
    }

    // ---- drawing --------------------------------------------------------------------

    // profileBarWidth (ZGE4 only; its drawProfilePeak and drawProfileTime
    // call it): a bar's length for cycles at a scale, as ZGE3 works it out
    // inline: the cycles as frames of time, over the scale, at most the
    // display's width less the left end.
    inline ee::Float profileBarWidth(int32_t cycles, ee::Float scale)
    {
        const ee::Float frameRate = unsignedToFloat(static_cast<uint32_t>(profiler().layout.frameRate));
        return barWidth(cyclesToTime(cycles, frameRate), scale);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, profileBarWidth);

    // drawProfilePeak: a bar's peak as a grey quad at the row's top (after the
    // warm-up frames); 0 or the warm-up frames left.
    int32_t drawProfilePeak(ee::Ptr<ProfileBar> bar, ee::Address quads, ee::Float top)
    {
        const int32_t warmUp = profiler().warmUpFrames;
        if (warmUp != 0)
            return warmUp;
        ee::Float width;
        if constexpr (game::readable::kIsBmx)
            width = profileBarWidth(static_cast<int32_t>(bar->peakCycles), bar->scale);
        else
        {
            const ee::Float frameRate = unsignedToFloat(static_cast<uint32_t>(profiler().layout.frameRate));
            const ee::Float time = cyclesToTime(static_cast<int32_t>(bar->peakCycles), frameRate);
            width = barWidth(time, *bar);
        }
        fillQuad(quads, top, width, static_cast<uint32_t>(profiler().layout.barHeight), ee::Ptr<uint8_t>(kPeakColour));
        return game::tail(zge::drawPrimitiveQuads, kOverlayPrimitives, ee::Ptr<uint32_t>(quads), 1, 0u);
    }
    READABLE_FUNCTION_WAITS(drawProfilePeak, Verified);

    // drawProfileTime: a bar's time (the last frame's while it runs, else this
    // frame's so far) as a thinner quad in its colour, centred in the row.
    void drawProfileTime(ee::Ptr<ProfileBar> bar, ee::Address quads, ee::Float top)
    {
        uint32_t cycles;
        if ((bar->flags & kProfileBarRunning) != 0)
            cycles = bar->lastFrameCycles;
        else
            cycles = bar->cycles;
        const ee::Float frameRate = unsignedToFloat(static_cast<uint32_t>(profiler().layout.frameRate));
        const ee::Float time = cyclesToTime(static_cast<int32_t>(cycles), frameRate);
        const ee::Float width = barWidth(time, *bar);
        const ProfileLayout &layout = profiler().layout;
        const ee::Float y = top + ee::abs(ee::toFloat(layout.barHeight - layout.timeHeight) * ee::Float(0.5f));
        fillQuad(quads, y, width, static_cast<uint32_t>(layout.timeHeight), ee::Ptr<uint8_t>(bar->colour));
        game::tail(zge::drawPrimitiveQuads, kOverlayPrimitives, ee::Ptr<uint32_t>(quads), 1, 0u);
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION_WAITS(drawProfileTime, Verified)); // (BMX XXX's: games/bmx/debug/profiler.cpp)

    // drawProfileTickAt (ZGE4 only; its drawProfileTicks draws each tick
    // with it): a tick mark at whole screen units, a triangle from above
    // right and above left of x down to x, in a colour.
    inline int32_t drawProfileTickAt(ee::Address triangles, int32_t x, int32_t y, ee::Ptr<uint8_t> colour)
    {
        const ProfileLayout &layout = profiler().layout;
        const uint32_t halfWidth = static_cast<uint32_t>(layout.tickWidth) >> 1;
        OverlayVertex &aboveRight = vertexOf(triangles, 0);
        aboveRight.x = unsignedToFloat(static_cast<uint32_t>(x) + halfWidth);
        aboveRight.y = unsignedToFloat(static_cast<uint32_t>(y - layout.tickAbove));
        setColour(aboveRight, colour);
        OverlayVertex &aboveLeft = vertexOf(triangles, 1);
        aboveLeft.x = unsignedToFloat(static_cast<uint32_t>(x) - halfWidth);
        aboveLeft.y = unsignedToFloat(static_cast<uint32_t>(y - layout.tickAbove));
        setColour(aboveLeft, colour);
        OverlayVertex &point = vertexOf(triangles, 2);
        point.x = ee::toFloat(x);
        point.y = unsignedToFloat(static_cast<uint32_t>(y - layout.tickAbove + layout.tickBelow));
        setColour(point, colour);
        return game::tail(zge::drawPrimitiveStrips, kOverlayPrimitives, triangles, 1, 0u);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, drawProfileTickAt);

    // drawProfileTicks: tick marks along a bar's row, a triangle each, from 0
    // to its scale in steps of a frame (or a tenth, a hundredth, a
    // thousandth for bars under a frame long): white at whole frames, red
    // at tenths, green at hundredths, else blue.
    void drawProfileTicks(ee::Ptr<ProfileBar> bar, ee::Address triangles, ee::Float top)
    {
        const ee::Float scale = bar->scale;
        ee::Float tick(1.0f);
        if (!(tick <= scale))
        {
            if (*g_tickStepTenth < scale)
                tick = *g_tickStepTenth;
            else if (*g_tickStepHundredth < scale)
                tick = *g_tickStepHundredth;
            else
                tick = *g_tickStepThousandth;
        }
        ee::Float position = ee::Float(0.0f);
        ee::Float end = scale;
        if (position <= end)
        {
            for (;;)
            {
                const ee::Float tolerance = *g_tickTolerance;
                const ee::Float x = profiler().left + position * profiler().barLength / end;
                OverlayVertex &first = vertexOf(triangles, 0);
                if (isWhole(position, tolerance))
                    setColour(first, ee::Ptr<uint8_t>(kWholeTickColour));
                else
                {
                    if (isWhole(position * ee::Float(10.0f), tolerance))
                        setColour(first, ee::Ptr<uint8_t>(kTenthTickColour));
                    else
                    {
                        if (isWhole(position * ee::Float(100.0f), tolerance))
                            setColour(first, ee::Ptr<uint8_t>(kHundredthTickColour));
                        else
                            setColour(first, ee::Ptr<uint8_t>(kOtherTickColour));
                    }
                }
                std::memcpy(vertexOf(triangles, 1).colour, first.colour, 4);
                std::memcpy(vertexOf(triangles, 2).colour, first.colour, 4);
                const ProfileLayout &layout = profiler().layout;
                // Up right, up left, then down at x.
                const ee::Float halfRight = unsignedToFloat(static_cast<uint32_t>(layout.tickWidth) >> 1);
                vertexOf(triangles, 0).x = x + halfRight;
                const ee::Float above = unsignedToFloat(static_cast<uint32_t>(layout.tickAbove));
                vertexOf(triangles, 0).y = top - above;
                const ee::Float halfLeft = unsignedToFloat(static_cast<uint32_t>(layout.tickWidth) >> 1);
                vertexOf(triangles, 1).x = x - halfLeft;
                const ee::Float aboveAgain = unsignedToFloat(static_cast<uint32_t>(layout.tickAbove));
                vertexOf(triangles, 1).y = top - aboveAgain;
                vertexOf(triangles, 2).x = x;
                const ee::Float aboveOnce = unsignedToFloat(static_cast<uint32_t>(layout.tickAbove));
                const ee::Float aboveTop = top - aboveOnce;
                const ee::Float below = unsignedToFloat(static_cast<uint32_t>(layout.tickBelow));
                vertexOf(triangles, 2).y = aboveTop + below;
                position = position + tick;
                drawPrimitiveStrips(kOverlayPrimitives, triangles, 1, 0);
                end = bar->scale;
                if (!(position <= end))
                    break;
            }
        }
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION_WAITS(drawProfileTicks, Verified)); // (BMX XXX's: games/bmx/debug/profiler.cpp)

    void resetProfilePeaks(ee::Address bar);

    // drawProfileDisplay (the overlay task): the bars' full length from the
    // display's width, then each bar's row (label, ticks, peak, time; its
    // frame ended unless it runs or its owner ends it), the warm-up
    // counted down and the peaks reset every peakHoldFrames. 1.
    uint32_t drawProfileDisplay()
    {
        ee::Local<OverlayVertex[4]> vertices;
        ee::Local<OverlayQuadList> quadList;
        ee::Local<OverlayTriangleList> triangleList;
        if (profiler().displayMode == 0)
            return 1;
        // The primitive mode set, and the old one put back at the end: on the
        // PS2 primitiveRendererReady, a stub that reads no mode and returns 1.
        primitiveRendererReady();
        const int32_t inside = *g_screenWidth - (profiler().layout.margin << 1);
        ee::Float rowTop = profiler().top;
        const uint32_t pool = profiler().barPool;
        if constexpr (game::readable::kIsBmx) // (ZGE4 takes the width as a signed int and keeps the length's fraction)
            profiler().barLength = ee::toFloat(inside) - profiler().left;
        else
        {
            const ee::Float insideWidth = unsignedToFloat(static_cast<uint32_t>(inside));
            profiler().barLength = ee::toFloat(ee::toInt(insideWidth - profiler().left));
        }

        const ee::Ptr<OverlayVertex> corners = vertices;
        quadList->texture = 0;
        triangleList->texture = 0;
        for (int corner = 0; corner < 4; ++corner)
            quadList->vertices[corner] = corners + corner;
        for (int corner = 0; corner < 3; ++corner)
            triangleList->vertices[corner] = corners + corner;

        for (uint32_t link = firstBarLink(pool); link != 0; link = nextBarLink(link))
        {
            const ee::Ptr<ProfileBar> bar(barOfLink(link));
            drawProfileLabel(bar, rowTop);
            drawProfileTicks(bar, triangleList, rowTop);
            drawProfilePeak(bar, quadList, rowTop);
            drawProfileTime(bar, quadList, rowTop);
            if ((bar->flags & (kProfileBarRunning | kProfileBarOwnFrame)) == 0)
                endProfileBarFrame(bar);
            const ProfileLayout &layout = profiler().layout;
            rowTop = rowTop + unsignedToFloat(static_cast<uint32_t>(layout.barHeight + layout.rowGap));
        }
        Profiler &p = profiler();
        if (p.warmUpFrames != 0)
            p.warmUpFrames = p.warmUpFrames - 1;
        if (p.peakHoldFrames != 0)
        {
            p.peakHoldLeft = p.peakHoldLeft - 1;
            if (p.peakHoldLeft == -1)
            {
                resetProfilePeaks(0);
                p.peakHoldLeft = p.peakHoldFrames;
            }
        }
        primitiveRendererReady();
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawProfileDisplay, Verified);

    // ---- the bars' tasks ---------------------------------------------------------------

    void restartProfileBar(ee::Address bar);

    // startProfileBarTask, stopProfileBarTask, restartProfileBarTask: the tasks that start, stop
    // and restart a bar (their data). 1.
    uint32_t startProfileBarTask(uint32_t bar)
    {
        startProfileBar(bar);
        return 1;
    }
    READABLE_FUNCTION(startProfileBarTask, Verified);

    uint32_t stopProfileBarTask(uint32_t bar)
    {
        stopProfileBar(bar);
        return 1;
    }
    READABLE_FUNCTION(stopProfileBarTask, Verified);

    uint32_t restartProfileBarTask(uint32_t bar)
    {
        restartProfileBar(bar);
        return 1;
    }
    READABLE_FUNCTION(restartProfileBarTask, Verified);

    // ---- set-up ----------------------------------------------------------------------

    void setProfilePeakHold(int32_t frames);

    // initProfiler: the profiler cleared and its timers set up (0 if they
    // fail), the default layout, the bars' pool and the peak hold.
    int32_t initProfiler()
    {
        memoryFill(kProfilerAddress, 0, static_cast<uint32_t>(sizeof(Profiler)));
        if (initCycleTimers() == 0)
            return 0;
        Profiler &p = profiler();
        p.ready = 1;
        p.layout.timeHeight = 6;
        p.layout.tickWidth = 8;
        p.layout.tickBelow = 4;
        p.layout.peakHoldFrames = 120;
        p.layout.frameRate = 60;
        p.warmUpFrames = 10;
        p.displayMode = 1;
        p.layout.margin = 0x20;
        p.layout.barHeight = 8;
        p.layout.rowGap = game::readable::perGame(4, 2); // (ZGE2's rows are 2 apart)
        p.layout.tickAbove = 4;
        p.layout.barScale = 1;
        p.layout.originX = 0;
        p.layout.originY = 0;
        clearProfileLabelOffset(p);
        clearProfileDisplayFlags(p);
        p.left = ee::toFloat(0x20);
        p.top = ee::toFloat(0x20);
        p.defaultScale = ee::toFloat(1);
        const uint32_t pool = createPool(static_cast<uint32_t>(sizeof(ProfileBar)), 0, kFreeProfileBar);
        const int32_t peakHold = p.layout.peakHoldFrames;
        p.barPool = pool;
        setProfilePeakHold(peakHold);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initProfiler, Verified);

    // shutDownProfiler: the bars' tasks deleted, the pool destroyed, the timers
    // shut down (0 if that fails). 1.
    uint32_t shutDownProfiler()
    {
        if (profiler().ready != 0)
        {
            ee::Ptr<Task> Profiler::*const tasks[] = {&Profiler::displayTask, &Profiler::appStartTask, &Profiler::appStopTask,
                                                       &Profiler::engineRestartTask, &Profiler::engineStopTask};
            for (int i = 0; i < 5; ++i)
            {
                const ee::Ptr<Task> task = profiler().*tasks[i];
                if (task)
                    deleteTask(task);
            }
            destroyPool(ee::Ptr<Pool>(profiler().barPool));
            if (shutDownCycleTimers() == 0)
                return 0;
            profiler().ready = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownProfiler, Verified);

    // setProfileLayout (no caller): the layout copied in and the overlay's
    // corner and a new bar's scale from it; the bar scale.
    int32_t setProfileLayout(uint32_t layout)
    {
        Profiler &p = profiler();
        std::memcpy(&p.layout, ee::Ptr<uint8_t>(layout).get(), sizeof(ProfileLayout)); // it may sit at any alignment
        const int32_t margin = p.layout.margin;
        const int32_t originY = p.layout.originY;
        p.left = ee::toFloat(p.layout.originX + margin);
        p.top = ee::toFloat(originY + margin);
        const int32_t scale = p.layout.barScale;
        p.defaultScale = ee::toFloat(scale);
        return scale;
    }
    READABLE_FUNCTION(setProfileLayout, Verified);

    // newProfileBar: a bar from the pool, cleared (the overlay task made
    // with the first): flags (the display mask in the low byte,
    // kNewProfileBarOwnFrame), an id, its colour (R, G, B, A at colour) and
    // label, the default scale.
    uint32_t newProfileBar(uint32_t flags, uint32_t id, ee::Address colour, uint32_t label)
    {
        const uint32_t bar = poolAlloc(ee::Ptr<Pool>(profiler().barPool));
        memoryFill(bar, 0, static_cast<uint32_t>(sizeof(ProfileBar)));
        if (poolCount(ee::Ptr<Pool>(profiler().barPool)) == 1)
            profiler().displayTask = newTask(kDisplayTaskPriority, kDrawProfileDisplay, 0, 0, kSourceFile, game::readable::perGame(571u, 561u, 670u));
        ProfileBar &b = barAt(bar);
        if ((flags & kNewProfileBarOwnFrame) != 0)
            b.flags |= kProfileBarOwnFrame;
        std::memcpy(b.colour, ee::Ptr<uint8_t>(colour).get(), 4); // it may sit at any alignment
        b.id = id;
        b.label = label;
        b.peakCycles = 0;
        b.cycles = 0;
        b.scale = profiler().defaultScale;
        memoryFill(bar + static_cast<uint32_t>(offsetof(ProfileBar, startCount)), 0, 8);
        addProfileBarDisplayMask(b, flags & 0xFFu);
        return bar;
    }
    READABLE_FUNCTION_WAITS(newProfileBar, Verified);

    // newAppProfileBar: the application's bar (display mask 8, green, "App"),
    // started and stopped by tasks (262 and 263), its scale one frame. (ZGE2:
    // no display mask, tasks 261 and 262.)
    uint32_t newAppProfileBar()
    {
        if (profiler().appBar != 0)
            return 0;
        const uint32_t bar = newProfileBar(game::readable::perGame(8u, 0u), 1u, kAppBarColour, kAppBarLabel);
        profiler().appBar = bar;
        if (bar != 0)
        {
            profiler().appStartTask = newTask(kAppStartPriority, kStartProfileBarTask, bar, 1, kSourceFile, game::readable::perGame(625u, 603u));
            profiler().appStopTask =
                newTask(kAppStopPriority, kStopProfileBarTask, profiler().appBar, 1, kSourceFile, game::readable::perGame(629u, 607u));
            setProfileBarScale(profiler().appBar, ee::Float(1.0f));
        }
        const uint32_t made = profiler().appBar;
        return made;
    }
    READABLE_FUNCTION_WAITS(newAppProfileBar, Verified);

    // appProfileBar: the application's bar.
    uint32_t appProfileBar()
    {
        return profiler().appBar;
    }
    READABLE_FUNCTION(appProfileBar, Verified);

    // newEngineProfileBar: the engine CPU's bar (no display mask, "EngCPU"),
    // restarted and stopped by tasks (256 and 259).
    uint32_t newEngineProfileBar()
    {
        if (profiler().engineBar != 0)
            return 0;
        const uint32_t bar = newProfileBar(0u, 2u, kEngineBarColour, kEngineBarLabel);
        profiler().engineBar = bar;
        if (bar != 0)
        {
            profiler().engineRestartTask =
                newTask(kEngineRestartPriority, kRestartProfileBarTask, bar, 1, kSourceFile, game::readable::perGame(675u, 653u));
            profiler().engineStopTask =
                newTask(kEngineStopPriority, kStopProfileBarTask, profiler().engineBar, 1, kSourceFile, game::readable::perGame(679u, 657u));
        }
        const uint32_t made = profiler().engineBar;
        return made;
    }
    READABLE_FUNCTION_WAITS(newEngineProfileBar, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t poolFree(uint32_t pool, uint32_t record) { return ee::tailCall<int32_t>(kPoolFree, pool, record); }
        }
    }

    // freeProfileBar (the pool's free callback): a bar's display mask taken
    // out of the profiler's and the bar back to the pool.
    int32_t freeProfileBar(ee::Address bar)
    {
        const uint32_t ready = profiler().ready;
        if (ready != 0)
        {
            const uint32_t pool = profiler().barPool;
            removeProfileBarDisplayMask(barAt(bar));
            return tail::poolFree(pool, bar);
        }
        return static_cast<int32_t>(ready);
    }
    READABLE_FUNCTION(freeProfileBar, Verified);

    // setProfileDisplayMode: the overlay on (non-zero) or off.
    void setProfileDisplayMode(uint32_t mode)
    {
        profiler().displayMode = mode;
    }
    READABLE_FUNCTION(setProfileDisplayMode, Verified);

    // ---- timing --------------------------------------------------------------------

    // startProfileBar: a bar started (its display mask set, its cycle count
    // begun); 1 if it runs already, else 0 (and 0 for no bar).
    uint32_t startProfileBar(ee::Address bar)
    {
        if (bar == 0)
            return 0;
        ProfileBar &b = barAt(bar);
        if ((b.flags & kProfileBarRunning) != 0)
            return 1;
        const uint32_t mask = profileBarDisplayMask(b);
        b.flags |= kProfileBarRunning;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps no running bars' masks)
            setTimerMask(mask);
        startCycleCount(bar + static_cast<uint32_t>(offsetof(ProfileBar, startCount)));
        return 0;
    }
    READABLE_FUNCTION(startProfileBar, Verified);

    // stopProfileBar: a bar stopped: its cycles since the start added to this
    // frame's, its display mask cleared; this frame's cycles (1 for no bar).
    uint32_t stopProfileBar(ee::Address bar)
    {
        if (bar == 0)
            return 1;
        const uint32_t cycles = static_cast<uint32_t>(cyclesSince(bar + static_cast<uint32_t>(offsetof(ProfileBar, startCount))));
        ProfileBar &b = barAt(bar);
        const uint32_t mask = profileBarDisplayMask(b);
        b.cycles = b.cycles + cycles;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps no running bars' masks)
            clearTimerMask(mask);
        const uint32_t total = b.cycles;
        b.flags &= ~uint32_t{kProfileBarRunning};
        return total;
    }
    READABLE_FUNCTION(stopProfileBar, Verified);

    // endProfileBarFrame: a bar's frame ended: the peak raised to this frame's
    // cycles (after the warm-up), this frame's kept as the last, the count
    // cleared. memoryFill's result.
    int32_t endProfileBarFrame(ee::Address bar)
    {
        ProfileBar &b = barAt(bar);
        uint32_t cycles;
        if (profiler().warmUpFrames == 0)
        {
            const uint32_t peak = b.peakCycles;
            cycles = b.cycles;
            b.peakCycles = cycles >= peak ? cycles : peak;
        }
        else
            cycles = b.cycles;
        b.lastFrameCycles = cycles;
        b.cycles = 0;
        const int32_t cleared = static_cast<int32_t>(memoryFill(bar + static_cast<uint32_t>(offsetof(ProfileBar, startCount)), 0, 8));
        return endFrameColourChanges(bar, cleared); // (ZGE4 keeps the frame's colour changes too)
    }
    READABLE_FUNCTION(endProfileBarFrame, Verified);

    // restartProfileBar: a bar's frame ended and the bar started again.
    void restartProfileBar(ee::Address bar)
    {
        endProfileBarFrame(bar);
        game::tail(zge::startProfileBar, bar);
    }
    READABLE_FUNCTION(restartProfileBar, Verified);

    // profileBarPeakTime (no caller): a bar's peak in frames of time.
    ee::Float profileBarPeakTime(ee::Ptr<ProfileBar> bar)
    {
        const ee::Float frameRate = unsignedToFloat(static_cast<uint32_t>(profiler().layout.frameRate));
        return cyclesToTime(static_cast<int32_t>(bar->peakCycles), frameRate);
    }
    READABLE_FUNCTION(profileBarPeakTime, Verified);

    void resetProfilePeaks(ee::Address bar);

    // setProfilePeakHold: the peaks reset every frames frames (0: the peaks
    // reset now and never again).
    void setProfilePeakHold(int32_t frames)
    {
        profiler().peakHoldFrames = frames;
        if (frames == 0)
        {
            game::tail(zge::resetProfilePeaks, 0u);
            return;
        }
    }
    READABLE_FUNCTION(setProfilePeakHold, Verified);

    // resetProfilePeaks: a bar's peak cleared, or every bar's (0).
    void resetProfilePeaks(ee::Address bar)
    {
        if (bar != 0)
        {
            barAt(bar).peakCycles = 0;
            return;
        }
        for (uint32_t link = firstBarLink(profiler().barPool); link != 0; link = nextBarLink(link))
            barAt(barOfLink(link)).peakCycles = 0;
    }
    READABLE_FUNCTION(resetProfilePeaks, Verified);

    // setProfileBarScale: a bar's scale (the frames of time its length spans).
    void setProfileBarScale(ee::Address bar, ee::Float scale)
    {
        barAt(bar).scale = scale;
    }
    READABLE_FUNCTION(setProfileBarScale, Verified);

    // setProfileOrigin (no caller): the overlay's corner (x, y) plus the
    // margin; the margin (as the original leaves it: halved and with its
    // low bit when negative).
    int32_t setProfileOrigin(int32_t x, int32_t y)
    {
        const ee::Float fx = ee::toFloat(x);
        const int32_t margin = profiler().layout.margin;
        ee::Float left;
        if (margin >= 0)
            left = fx + ee::toFloat(margin);
        else
        {
            const ee::Float half = ee::toFloat(static_cast<int32_t>((static_cast<uint32_t>(margin) & 1u) | (static_cast<uint32_t>(margin) >> 1)));
            left = fx + (half + half);
        }
        profiler().left = left;
        const ee::Float fy = ee::toFloat(y);
        int32_t result = profiler().layout.margin;
        ee::Float top;
        if (result >= 0)
            top = fy + ee::toFloat(result);
        else
        {
            result = static_cast<int32_t>((static_cast<uint32_t>(result) & 1u) | (static_cast<uint32_t>(result) >> 1));
            const ee::Float half = ee::toFloat(result);
            top = fy + (half + half);
        }
        profiler().top = top;
        return result;
    }
    READABLE_FUNCTION(setProfileOrigin, Verified);
}
