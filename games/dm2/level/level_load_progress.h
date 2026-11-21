#pragma once

// The load screen's progress bar (level_load_progress.cpp): how far the
// level's load has come, from the files the chunk reader goes through.

#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/level/level_load_progress.h.h"

namespace game::dm2
{
    // The parts of a level's load, each with how far it has come (0 .. 1):
    // the level's four files by their extension, the riders' motion files,
    // and four more by their name.
    enum LevelLoadPart : uint32_t
    {
        kLoadGeometry,  // .GEO
        kLoadTextures,  // .TXR
        kLoadData,      // .DAT
        kLoadInstances, // .INS
        kLoadMotions,   // MOTION.<number>: the files count down to 0
        kLoadNpc,       // NPC
        kLoadOverlay,   // OVERLAY
        kLoadFx,        // FX
        kLoadFinal,     // FINAL
        kLevelLoadParts,
    };
    inline ee::Global<ee::Float[kLevelLoadParts], sym::g_levelLoadParts> g_levelLoadParts;

    // The chunk reader's progress hook while a level loads (BmxStartGame
    // sets it): the part the file belongs to is brought up to date and the
    // bar drawn as wide as the parts' weighted sum. A file of no part draws
    // nothing.
    void levelLoadProgress(ee::Ptr<const char> path, uint32_t bytesDone, uint32_t totalBytes);
}
