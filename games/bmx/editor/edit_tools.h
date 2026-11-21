#pragma once

// The park editor's tool set (edit_tools.cpp): its tools, the one in use and
// those put aside, the editor's update and its end.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tools.h.

#include "bmx/editor/edit_records.h"
#include "bmx/sound/events.h"
#include "zge/editor/edit_tools.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    void toolSetUseUiCamera(ee::Ptr<EditToolSet> toolSet);
    void resetParkCost();

    // BMX XXX's tools play a sound where Aggressive Inline's play none. The numbers are its
    // sound events', named by what every tool of the editor does when it fires them: a move
    // of the cursor or the held piece (each tool's four moves); a piece placed or ground chosen;
    // a turn (the object, crane and start tools' rotate, the rail tool's change of anchor, the
    // terrain tool's reselect); a delete; another piece (next, previous, the rail preview
    // remade); a place, delete or turn refused.
    constexpr uint32_t kMoveSound = 0x17, kPlaceSound = 0x18, kRotateSound = 0x19, kDeleteSound = 0x1A, kChangePieceSound = 0x1C, kRefusedSound = 0x1D;
    // The game's soundEvent (bmx/sound/events.h) plays them.
    using game::bmx::soundEvent;
    // Scaffolding (address-call): soundEvent made as the caller's last step (the original's tail jump).
    namespace calls
    {
        uint32_t soundEventTail(uint32_t event, ee::Address a, ee::Address b, uint32_t c, uint32_t d);
    }
}

