#pragma once
// BMX XXX's records frame (bmx.c, records_frame.cpp): the records screen's
// four columns filled for a level. BMX XXX's own code.

#include "ee/ee.h"
#include "zge/zgui/widget.h"

#include <cstdint>

namespace game::bmx
{
    // A level's saved records written into the frame's four lists: each
    // record's name, the rider character, the player's name and the value.
    void fillRecordsFrame(ee::Ptr<zge::zgui::Node> frame, uint32_t level);
}
