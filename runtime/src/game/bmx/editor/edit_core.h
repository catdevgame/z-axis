#pragma once

// The park editor's core (edit_core.cpp): building a park from saved data or
// opening it in the editor, saving and loading it, and the events the game
// sets a callback for (park_editor.cpp).
// BMX XXX's own part of it; what both games have is zge/editor/edit_core.h.

#include "ee/ee.h"
#include "zge/editor/edit_core.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "bmx/editor/edit_records.h"

#include <cstdint>


namespace zge::editor
{
    void buildParkLevel(uint32_t themeIndex, uint32_t data, uint32_t bytes);
    void editCoreUseUiCamera();
    void openParkEditor(uint32_t themeIndex, uint32_t uiLoaded);
    void placeEditPiece(uint32_t anchor, ee::Ptr<EditObjectPart> object);
    uint32_t registerEditCoreFunctions();
    ee::Ptr<EditObjectPart> findParkStartObject();
    void showEditCorePark();
}
