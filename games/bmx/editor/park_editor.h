#pragma once

// The game's side of the park editor (park_editor.cpp).

#include "bmx/save/park_buffer.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace game::bmx::editor
{
    // (park_editor.cpp)
    uint32_t inParkEditor();
    void loadParkIntoEditor(uint32_t /*theme*/, uint32_t /*piecesFile*/, ee::Ptr<game::bmx::ParkBuffer> block);
    int32_t parkEditorTheme();
    uint32_t saveParkToCard();
}
