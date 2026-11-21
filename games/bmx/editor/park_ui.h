#pragma once

// The park editor's interface (park_ui.cpp): its buttons, pad, tool panel and
// help pages.
// BMX XXX's own part of it; what both games have is zge/editor/park_ui.h.

#include "bmx/editor/edit_records.h"
#include "zge/editor/park_ui.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    void editRefused();
    void freeEditUi(uint32_t address);
    int32_t setPowerLevel(ee::Float level);
    int32_t updateEditFlash(ee::Float seconds);
}

