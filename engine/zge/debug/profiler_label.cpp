// The profiler's bar labels (zprofile.c; zge/debug/profiler.cpp has the rest
// of the profiler). drawProfileLabel prints a label as debugPrint's format.

#include "zge/debug/profiler.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstdint>
#include "sym/zge/debug/profiler_label.cpp.h"

namespace zge
{
    namespace // the game functions below call: ee/plain.h
    {
        constexpr uint32_t kDebugPrint = sym::debugPrint; // (x, y, format, ...) the debug text overlay

        // The debug text's coordinates: 16-bit, with bit 14 set [unknown:
        // what the printer reads it as].
        int32_t debugTextCoordinate(int32_t value)
        {
            return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(value) | 0x4000u));
        }

        namespace tail
        {
            void debugPrint(uint32_t x, uint32_t y, uint32_t format) { ee::tailCall(kDebugPrint, x, y, format); }
        }
    }

    // drawProfileLabel: a bar's label (if it has one) after the bar's full
    // length, by the debug text overlay; the caller's spare argument words are
    // handed on to the printer as they are (the original's tail jump).
    void drawProfileLabel(ee::Ptr<ProfileBar> bar, ee::Float top)
    {
        const uint32_t label = bar->label;
        if (label != 0)
        {
            // (ZGE2 prints the label at the screen's left edge; ZGE3 moved it after the bar)
            // (ZGE4 adds the profiler's label offset)
            const int32_t afterBar = ee::toInt(g_profiler->left + g_profiler->barLength + ee::Float(8.0f)) + profileLabelOffsetX(*g_profiler);
            const int32_t x = game::readable::kIsDm2 ? debugTextCoordinate(0) : debugTextCoordinate(afterBar);
            const int32_t y = debugTextCoordinate(ee::toInt(top) + profileLabelOffsetY(*g_profiler) - 3);
            tail::debugPrint(x, y, label);
            return;
        }
    }
    READABLE_FUNCTION(drawProfileLabel, Verified);
}
