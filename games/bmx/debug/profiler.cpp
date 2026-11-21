// BMX XXX's own drawing of a profile bar's row (ZGE4's zprofile.c; ZGE3's is zge/debug/profiler.cpp): the time
// in pieces, one per colour change of the frame, and the tick marks at whole screen units with the bar's two
// marked times after them. Each takes ZGE4's two steps of its own, profileBarWidth and drawProfileTickAt
// (zge/debug/profiler.cpp).

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/debug/profile_overlay.h"
#include "zge/debug/profiler.h"
#include "zge/render/primitives.h"

#include <cstdint>
#include "sym/bmx/debug/profiler.cpp.h"

namespace game::bmx
{
    namespace
    {
        using namespace zge::profile_overlay;

        // The tick marks' steps for a bar's scale under one frame, and the
        // tolerance of the whole-number test.
        inline ee::Global<ee::Float, sym::g_tickStepTenth> g_tickStepTenth;     // 0.1
        inline ee::Global<ee::Float, sym::g_tickStepHundredth> g_tickStepHundredth; // 0.01
        inline ee::Global<ee::Float, sym::g_tickStepThousandth> g_tickStepThousandth; // 0.001
        inline ee::Global<ee::Float, sym::g_tickTolerance> g_tickTolerance;     // 0.00005

        // The ticks' colours (R, G, B, A words): at whole frames (white), tenths (red), hundredths (green), the
        // others (blue), and ZGE4's marked times (yellow).
        constexpr uint32_t kWholeTickColour = sym::wholeTickColour;
        constexpr uint32_t kTenthTickColour = sym::tenthTickColour;
        constexpr uint32_t kHundredthTickColour = sym::hundredthTickColour;
        constexpr uint32_t kOtherTickColour = sym::otherTickColour;
        constexpr uint32_t kMarkedFrameTickColour = sym::markedFrameTickColour;

        zge::Profiler &profiler() { return *zge::g_profiler; }
    }

    // drawProfileTimeInColours: BMX XXX's drawProfileTime: a bar's time (the
    // last frame's while it runs, else this frame's so far) as a thinner quad
    // centred in the row, in pieces: from the left end to each colour change
    // of that frame and the last to the time, the first in the bar's colour
    // and each after in its change's.
    void drawProfileTimeInColours(ee::Ptr<zge::ProfileBar> bar, ee::Address quads, ee::Float top)
    {
        const bool lastFrame = (bar->flags & zge::kProfileBarRunning) != 0;
        const uint32_t cycles = lastFrame ? bar->lastFrameCycles : bar->cycles;
        zge::ProfileColourChange *changes = lastFrame ? bar->lastFrameColourChanges : bar->colourChanges;
        const uint32_t changeCount = lastFrame ? bar->lastFrameColourChangeCount : bar->colourChangeCount;
        const zge::ProfileLayout &layout = profiler().layout;
        const ee::Float y = top + ee::abs(ee::toFloat(layout.barHeight - layout.timeHeight) * ee::Float(0.5f));
        const ee::Float bottom = y + unsignedToFloat(static_cast<uint32_t>(layout.timeHeight));
        const ee::Float width = zge::profileBarWidth(static_cast<int32_t>(cycles), bar->scale);
        ee::Float left = profiler().left;
        for (uint32_t piece = 0;; ++piece)
        {
            const ee::Ptr<uint8_t> colour = piece == 0 ? ee::Ptr<uint8_t>(bar->colour) : ee::Ptr<uint8_t>(changes[piece - 1].colour);
            ee::Float right;
            if (piece == changeCount)
                right = profiler().left + width;
            else
            {
                const ee::Float changeWidth = zge::profileBarWidth(static_cast<int32_t>(changes[piece].cycles), bar->scale);
                right = profiler().left + changeWidth;
            }
            OverlayVertex &topLeft = vertexOf(quads, 0);
            topLeft.x = left;
            topLeft.y = y;
            setColour(topLeft, colour);
            OverlayVertex &bottomLeft = vertexOf(quads, 1);
            bottomLeft.x = left;
            bottomLeft.y = bottom;
            setColour(bottomLeft, colour);
            OverlayVertex &bottomRight = vertexOf(quads, 2);
            bottomRight.x = right;
            bottomRight.y = bottom;
            setColour(bottomRight, colour);
            OverlayVertex &topRight = vertexOf(quads, 3);
            topRight.x = right;
            topRight.y = y;
            setColour(topRight, colour);
            zge::drawPrimitiveQuads(kOverlayPrimitives, quads, 1, 0u);
            left = right;
            if (!(piece < changeCount))
                break;
        }
    }
    READABLE_FUNCTION_WAITS(drawProfileTimeInColours, Verified);

    // drawProfileTicksAndMarks: BMX XXX's drawProfileTicks: tick marks along a
    // bar's row from 0 to its scale in steps of a frame (or a tenth, a
    // hundredth, a thousandth for bars under a frame long), white at whole
    // frames, red at tenths, green at hundredths, else blue, each at whole
    // screen units; then a yellow one at each of the bar's marked times above 0.
    void drawProfileTicksAndMarks(ee::Ptr<zge::ProfileBar> bar, ee::Address triangles, ee::Float top)
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
                uint32_t colour;
                if (isWhole(position, tolerance))
                    colour = kWholeTickColour;
                else if (isWhole(position * ee::Float(10.0f), tolerance))
                    colour = kTenthTickColour;
                else if (isWhole(position * ee::Float(100.0f), tolerance))
                    colour = kHundredthTickColour;
                else
                    colour = kOtherTickColour;
                zge::drawProfileTickAt(triangles, ee::toInt(x), ee::toInt(top), ee::Ptr<uint8_t>(colour));
                position = position + tick;
                end = bar->scale;
                if (!(position <= end))
                    break;
            }
        }
        const int32_t row = ee::toInt(top);
        for (const ee::Float marked : bar->markedFrames)
        {
            if (ee::Float(0.0f) < marked)
            {
                const ee::Float x = profiler().left + marked * profiler().barLength / end;
                zge::drawProfileTickAt(triangles, ee::toInt(x), row, ee::Ptr<uint8_t>(kMarkedFrameTickColour));
                end = bar->scale;
            }
        }
    }
    READABLE_FUNCTION_WAITS(drawProfileTicksAndMarks, Verified);
}
