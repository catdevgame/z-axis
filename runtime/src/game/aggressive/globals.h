#pragma once

// Aggressive Inline's game state: global variables at fixed addresses, as
// worked out in the bring-up (docs/BRINGUP.md, tools/logiccheck.py,
// tools/REFERENCE-RUN.md and the game overrides that use them).

#include "ee/ee.h"
#include "zge/math/types.h"
#include "sym/aggressive/globals.h.h"

namespace zge::zgui
{
    struct Node;
}

namespace aggressive
{
    // The game mode (index into the mode table modeRecords).
    enum class GameMode : uint32_t
    {
        ArtViewer = 0, // the debug menu's free camera
        FrontEnd = 1,
        Play = 2,      // in a level
        Paused = 3,    // pause menu or a message box (the camera keeps running)
        DebugMenu = 6, // the developers' menu (unused in the retail game)
    };
    inline ee::Global<GameMode, sym::g_gameMode> g_gameMode;

    // What is being played (the front end's choice).
    enum class GameType : uint8_t
    {
        Career = 1,
        FreeSkate = 2,
        TimedRun = 3,
        SplitScreen = 4,
    };
    inline ee::Global<GameType, sym::g_gameTypeByte> g_gameType;
    // The skaters the players chose (player one, two: the save's skater
    // records' index).
    inline ee::Global<uint8_t[2], sym::g_playerSkaters> g_playerSkaters;
    // The level (8: the Zoo, the level cut from the PS2 release).
    inline ee::Global<uint8_t, sym::g_levelNumber> g_levelIndex;

    // Main-loop frames since the level started (0 on its loading screen;
    // one per vsync in a level, as on the PS2).
    inline ee::Global<uint32_t, sym::g_levelFrame> g_levelFrame;
    inline ee::Global<uint32_t, sym::g_vsyncCount> g_vsyncCount;
    // Level time, float seconds.
    inline ee::Global<ee::Float, sym::g_levelTime> g_levelTime;

    // 1 while a scripted camera runs (Movie Lot fly-through, Tutorial lesson intros).
    inline ee::Global<uint32_t, sym::g_scriptedCamera> g_scriptedCamera;
    // The zgui panel on top (a heap object).
    inline ee::Global<uint32_t, sym::g_topPanel> g_topPanel;
    // zgui's root node (the UI's tree).
    inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
    // The goals of the level (records of 0x2C bytes) and how many there are.
    inline ee::Global<uint32_t, sym::g_objectives> g_objectives;
    inline ee::Global<uint32_t, sym::g_objectiveCount> g_objectiveCount;

    // The player's position (the skater's physics state).
    inline ee::Global<zge::Vec3, sym::g_playerPosition> g_playerPosition;

    // Unlocks (64-bit flags) and the save data's skater and level records.
    inline ee::Global<uint64_t, sym::g_unlockedSkaters> g_unlocks;
    inline ee::Global<uint8_t, sym::g_skaterRecords> g_skaterRecords;
    inline ee::Global<uint8_t, sym::g_levelRecords> g_levelRecords;

    // The language's string table file name ("assets\string.eus").
    inline ee::Global<char, sym::g_stringTableName> g_stringTableName;
}
