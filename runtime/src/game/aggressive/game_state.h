#pragma once

// The game's top level, as a modder finds it: GameState is the block of
// globals everything hangs from (what is being played, the cameras, the pads,
// the save data's unlocks and volumes). The game runs one TopState at a time
// (a level, the front end, a short wait between them); while a level runs, the
// Level object holds its players and the PlayMode it is in (skating, paused,
// a fly-through ...).

#include "aggressive/camera.h"
#include "aggressive/globals.h"
#include "aggressive/play_modes.h"
#include "aggressive/skater/skater.h"

#include "ee/callback.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/game_state.h.h"

namespace aggressive
{
    struct GameState;
    struct Level;

    // A row of the level's play mode table (modeRecords). The level is in
    // one at a time (Level::mode); enterGameMode changes it.
    struct PlayMode
    {
        int32_t id;                         // +0x00 a PlayModeId
        ee::Callback<int32_t()> enter;      // +0x04
        ee::Callback<int32_t()> exit;       // +0x08
        ee::Callback<int32_t(ee::Float)> update; // +0x0C a frame, seconds
        uint32_t data;                      // +0x10 0 in every row
    };
    static_assert(sizeof(PlayMode) == 0x14);

    enum PlayModeId : int32_t
    {
        kModeFreeCamera = 0,
        kModeMotionView = 1,
        kModePlaying = 2,
        kModePaused = 3,
        kModeCinematicCamera = 4,
        kModeFlyThru = 5,
        kModeDebugMenu = 6,
        kModeParkEditor = 7,
    };
    inline ee::Global<PlayMode[8], sym::modeRecords> g_playModes;

    // The front end's block (TopState::frontEnd while in the front end), 8
    // bytes from the heap. registerMenuFunctions clears the byte at +4 as
    // the front end starts; no reader of it or of the rest is known.
    struct FrontEndState
    {
        uint8_t _00[4];
        uint8_t clearedAtStart; // +0x04
        uint8_t _05[3];
    };
    static_assert(sizeof(FrontEndState) == 8);

    // A row of the game's top state table (topStates): the states are a level,
    // the front end and a wait of a few frames that leads to the next one.
    struct TopState
    {
        int32_t id;                                // +0x00 a TopStateId
        ee::Callback<int32_t(ee::Ptr<GameState>)> enter; // +0x04
        ee::Callback<int32_t()> exit;              // +0x08
        ee::Callback<uint64_t(ee::Float)> update;  // +0x0C a frame, seconds
        // +0x10 the state's own block on the heap: the Level in a level, a
        // FrontEndState in the front end.
        union
        {
            ee::Ptr<Level> level;
            ee::Ptr<FrontEndState> frontEnd;
        };
    };
    static_assert(sizeof(TopState) == 0x14);

    enum TopStateId : int32_t
    {
        kTopStateLevel = 0,
        kTopStateFrontEnd = 1,
        kTopStateWait = 2,
        kTopStateNone = 3, // no state: at the start, and as a level ends
    };
    inline ee::Global<TopState[3], sym::topStates> g_topStates;

    // The level being played, from the top state's block. A heap object;
    // its head reads as a doubleword in the original.
    struct Level
    {
        // +0x00 bit 2: the players are held (their update is skipped); bits
        // 8 .. 13 why play stopped (LevelStopReason); bits 14 .. 15 the
        // player that stopped it.
        uint16_t flags;
        uint8_t splitGame;       // +0x02 the split-screen game: a SplitGame
        // +0x03 read only when a timed end in the career finds it: kinds 3 and
        // 5 keep the stop reason already set, the others ask to retry.
        uint8_t timeUpKind;
        ee::Ptr<PlayMode> mode;  // +0x04 the mode it is in
        uint32_t timer;          // +0x08 the level's timer, 0 when none
        uint8_t _0c[4];
        skater::Player players[2]; // +0x10
    };
    static_assert(offsetof(Level, players) == 0x10);

    constexpr uint16_t kLevelPlayersHeld = 0x0004;
    constexpr uint16_t kLevelStopReasonBits = 0x3F00;

    // Level::flags & kLevelStopReasonBits.
    enum LevelStopReason : uint16_t
    {
        kStopRunOver = 0x0300,
        kStopRunResults = 0x0500,
        kStopRetry = 0x0800,
        kStopOpeningFlyThru = 0x0A00, // the level's opening fly-through: play follows it
        kStopFlyThru = 0x0B00,        // one from the pause menu: back there after it
        kStopOverlayText = 0x0C00,
        kStopOutOfJuice = 0x0E00, // the carton ran dry with Death by Juice on
    };

    // Level::splitGame, where a game's code tells them apart.
    enum SplitGame : uint8_t
    {
        kSplitGameCollect = 2,
        kSplitGameCards = 3,
        kSplitGameAnimalRescue = 4,
    };

    // The block of globals at the root of the game. Its head holds the debug
    // display's bits; past the pads it is the part of the save data that is
    // not per skater.
    struct GameState
    {
        uint8_t debugDisplay;       // +0x000 bits 0-1: the profiler's display
        uint8_t playerSkaters[2];   // +0x001 the skaters the players chose
        GameType type;              // +0x003
        int32_t level;              // +0x004 the level's number (14: none)
        int32_t topStateId;         // +0x008 a TopStateId
        ee::Ptr<TopState> topState; // +0x00C
        ee::Float frameTime;        // +0x010 this frame's seconds
        uint8_t _014[0x0C];
        FollowCamera cameras[2];    // +0x020 player two's in split screen
        skater::PadState pads[2];   // +0x120
        uint8_t _208[0x15];
        uint8_t musicVolume;        // +0x21D
        uint8_t soundVolume;        // +0x21E
        uint8_t _21f;
        uint16_t playlist;          // +0x220 a bit per track that may play
    };
    static_assert(offsetof(GameState, topState) == 0x0C && offsetof(GameState, cameras) == 0x20 && offsetof(GameState, pads) == 0x120);
    static_assert(offsetof(GameState, musicVolume) == 0x21D && offsetof(GameState, playlist) == 0x220);

    inline ee::Global<GameState, sym::g_debugDisplay> g_gameState;

    // The running top state's block: the level, in a level.
    inline ee::Ptr<Level> currentLevel()
    {
        return g_gameState->topState->level;
    }

    // Player `index` of a level (0, or 1 in split screen).
    inline ee::Ptr<skater::Player> levelPlayer(ee::Ptr<Level> level, int32_t index)
    {
        return ee::Ptr<skater::Player>(level->players) + index;
    }
    inline ee::Ptr<skater::Player> levelPlayer(int32_t index)
    {
        return levelPlayer(currentLevel(), index);
    }

    inline void setLevelStopReason(ee::Ptr<Level> level, uint16_t reason)
    {
        level->flags = static_cast<uint16_t>((level->flags & ~kLevelStopReasonBits) | reason);
    }

    inline bool isSplitScreen()
    {
        return g_gameState->type == GameType::SplitScreen;
    }

    // How many players the game has: the loops over them read the game type
    // again each time round, so this is asked each time too.
    inline int32_t playerCount()
    {
        return isSplitScreen() ? 2 : 1;
    }
}
