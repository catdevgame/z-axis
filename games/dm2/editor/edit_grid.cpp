// DM2's park editor grid positions and pulse (edit_grid.h): DM2's own code.

#include "edit_grid.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"

#include <cstdint>
#include "sym/dm2/editor/edit_grid.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<int32_t, sym::g_editPulseFrames> g_editPulseFrames;


        // Scaffolding (address-call): ZGE2's cosine of a turn (65536 to the
        // circle, a table) has no shared declaration.
        ee::Float cosTurnOf(int32_t angle)
        {
            return ee::game<ee::Float>(sym::cosTurnOf, angle);
        }
    }

    void cellWorldPosition(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row, ee::Ptr<zge::Vec4> position)
    {
        position->y = -ee::toFloat(parkCell(editor, column, row)->level);
        position->x = zge::unsignedToFloat(column) * kCellSize - ee::toFloat(editor->columns - 1) * kHalfCell;
        position->z = zge::unsignedToFloat(row) * kCellSize - ee::toFloat(editor->rows - 1) * kHalfCell;
    }
    READABLE_FUNCTION(cellWorldPosition, Verified);

    void stepPulse()
    {
        *g_editPulseFrames = *g_editPulseFrames + 1;
    }
    READABLE_FUNCTION(stepPulse, Verified);

    ee::Float pulseCosine(int32_t rate)
    {
        return cosTurnOf(static_cast<int16_t>(*g_editPulseFrames * rate));
    }
    READABLE_FUNCTION(pulseCosine, Verified);
}
