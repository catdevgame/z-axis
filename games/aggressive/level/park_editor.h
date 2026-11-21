#pragma once

// The park editor's game side (park_editor.cpp): entering and leaving it, a
// saved park played, its natives.

#include "ee/plain.h"
#include "ee/readable.h"
#include "aggressive/level/park_buffer.h"

#include <cstdint>

namespace aggressive::level
{
    // (park_editor.cpp)
    void enterParkEditor(uint32_t theme, uint32_t piecesFile, uint32_t inGameEditor);
    uint32_t inParkEditor();
    void loadParkIntoEditor(uint32_t /*theme*/, uint32_t /*piecesFile*/, ee::Ptr<ParkBuffer> block);
    int32_t parkEditorTheme();
    void playSavedPark(uint32_t theme, uint32_t piecesFile, ee::Ptr<ParkBuffer> block);
    void registerParkEditorNatives();
    void shutdownParkEditor();
}
