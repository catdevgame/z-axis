#pragma once

// The park editor's interface (park_ui.cpp): its buttons, pad, tool panel and
// help pages.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_ui.h.

#include "aggressive/level/edit_records.h"
#include "zge/editor/park_ui.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    void disableUiButton(uint32_t ui, uint32_t button);
    void editRefused();
    void enableUiButton(uint32_t ui, uint32_t button);
    void freeEditUi(uint32_t address);
    int32_t playInGameEditPauseScript(uint32_t ui);
    int32_t setPowerLevel(ee::Float level);
    int32_t updateEditFlash(ee::Float seconds);
}

