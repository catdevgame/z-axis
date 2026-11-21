// BMX XXX's game set-up and end (game_setup.h): BMX XXX's own code (game.c,
// its game block), DM2's line (games/dm2/game/round.cpp) with BMX XXX's own
// steps: a copy of each rider's model kept for the game, the texture
// effects, and the mode set to none.

#include "game_setup.h"

#include "player_riders.h"
#include "records.h"
#include "round.h"

#include "bmx/objectives/objectives.h"
#include "bmx/profile/players.h"
#include "bmx/profile/records.h"
#include "bmx/replay/replays.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/platform/tasks.h"
#include "zge/render/texture_effects.h"
#include "zge/scene/model_records.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/game_setup.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The game's replays (round.cpp's): each player's turn, the last
        // turn's and the whole round's.
        inline ee::Global<uint8_t[kMaxPlayers], sym::g_playerReplays> g_playerReplays;
        inline ee::Global<uint8_t, sym::g_lastTurnReplay> g_lastTurnReplay;
        inline ee::Global<uint8_t, sym::g_roundReplay> g_roundReplay;
        // The players share this much replay data.
        constexpr int32_t kSharedReplayBytes = 0x1000;

        // Each player's rider model as the game started (a model record of
        // the engine's): gameCleanup puts it back on the rider.
        inline ee::Global<ee::Ptr<zge::ModelRecord>[kMaxPlayers], sym::g_keptRiderModels> g_keptRiderModels;

        // The level's extents in quarters (DM2's names).
        inline ee::Global<uint16_t, sym::g_levelQuarterX> g_levelQuarterX;
        inline ee::Global<uint16_t, sym::g_levelQuarterZ> g_levelQuarterZ;
        // A level extent in whole units, rounded up to the next four, in
        // quarters: as the game computes it, in 16 bits.
        uint16_t quarterOf(ee::Float extent)
        {
            return static_cast<uint16_t>(static_cast<uint16_t>(ee::toInt(extent) + 3) >> 2);
        }

        // The game task (gameSetup makes it; its frame is gameTaskFrame).
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_gameTask> g_gameTask;
        constexpr uint32_t kGameTaskStage = 1;
        constexpr uint32_t kGameSourcePath = sym::gameSourcePath; // "./src/com/action/game.c"
        constexpr uint32_t kGameSetupLine = 410;

        // "%s" and the HORSE word a game starts with ("BMX").
        constexpr uint32_t kTextFormat = sym::textFormat;
        constexpr uint32_t kDefaultHorseWord = sym::defaultHorseWord;

        // No game mode (a game's start sets one).
        constexpr uint8_t kNoGameMode = 0xFF;
        // The game's records kept from the saved ones: three entries of
        // each mode's every kind.
        constexpr uint32_t kGameModes = 20;
        constexpr uint32_t kKeptRecordEntries = 3;

        // RiderCharacter::flags cleared as a game is set up: the bike and
        // level won marks and bit 3 (what sets it is not read).
        constexpr uint16_t kCharacterFlag08 = 0x0008;
    }

    // unusedGameSetupHook (inferred name): the empty hook a game's set-up
    // starts with; always 1, which its caller does not read.
    int32_t unusedGameSetupHook()
    {
        return 1;
    }
    READABLE_FUNCTION(unusedGameSetupHook, Verified);

    // gameSetup: the game's set-up as a level starts. The round has no flags,
    // no phase and no mode, and its turn's end goes back to its start. A
    // session with one rider runs its turn from the start (mode 0); else each
    // player gets a replay of its share of the data (marked multiplayer with
    // two players or more), its character loses this game's won marks, the
    // saved game its movie won mark, and a copy of the rider's model is kept;
    // the round's replay is the one player's, or one of its own; the HORSE
    // word is "BMX". Then every mode's game records start from the saved
    // ones, the level's extents are taken in quarters, the objectives are
    // loaded, the texture effects started, every rider put away, and the game
    // task made.
    void gameSetup()
    {
        ee::Local<ReplaySetup> setup;
        unusedGameSetupHook();
        RoundState &round = *g_round;
        round.flags = 0;
        round.phase = kNoRoundPhase;
        *g_currentGameMode = kNoGameMode;
        round.phaseFramesLeft = 0;
        setTurnEndStage(0);

        if (sessionHasOneRider())
        {
            *g_currentGameMode = 0;
            round.phase = kTurnRunningPhase;
        }
        else
        {
            setup->format = 0;
            // (A session with no players would divide by 0 here, as the
            // original does: its divide traps.)
            setup->dataBytes = static_cast<uint32_t>(kSharedReplayBytes / static_cast<int32_t>(*g_playerCount));
            setup->flags = 0;
            if (*g_playerCount >= 2)
                setup->flags = kReplayMultiplayerSetUp;
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                const ee::Ptr<RiderCharacter> character = playerCharacter(player);
                setup->rider = riderIndexOf(player);
                setup->weapon = playerWeapon(player);
                (*g_playerReplays)[player] = newReplay(setup);
                character->flags = static_cast<uint16_t>(character->flags & ~kCharacterWonBike & ~kCharacterFlag08 & ~kCharacterWonLevel);
                g_savedGame->flags = static_cast<uint8_t>(g_savedGame->flags & ~kSavedMovieWon);
                (*g_keptRiderModels)[player] = zge::newModelRecord();
                zge::copyModelRecord((*g_keptRiderModels)[player], playerRider(player)->model);
            }
            if (*g_playerCount == 1)
            {
                *g_roundReplay = (*g_playerReplays)[0];
            }
            else
            {
                setup->weapon = 0;
                setup->rider = kReplayNoRider;
                *g_roundReplay = newReplay(setup);
            }
            *g_lastTurnReplay = (*g_playerReplays)[0];
            zge::formatString(sym::g_horseWord, kTextFormat, kDefaultHorseWord);
        }

        for (uint32_t mode = 0; mode < kGameModes; ++mode)
        {
            for (uint32_t kind = 0; kind < kRecordsPerSpan; ++kind)
            {
                for (uint32_t entry = 0; entry < kKeptRecordEntries; ++entry)
                {
                    const uint32_t key = recordKey(mode, kind, kSavedRecords, entry);
                    const ee::Ptr<GameRecord> record = gameRecord(key);
                    if (record)
                    {
                        const ee::Ptr<SavedRecord> saved = savedRecord(key);
                        record->marks = 0;
                        record->value = saved->value;
                    }
                }
            }
        }

        const rider::Bounds *const bounds = rider::levelBounds();
        *g_levelQuarterX = quarterOf(bounds->max[0] - bounds->min[0]);
        *g_levelQuarterZ = quarterOf(bounds->max[2] - bounds->min[2]);
        objectives::loadLevelObjectives();
        zge::initTextureFx();
        for (uint32_t player = 0; player < *g_playerCount; ++player)
            rider::riderReset(riderIndexOf(player), 0);
        *g_gameTask = zge::newTask(kGameTaskStage, sym::gameTaskFrame, 0, 1, kGameSourcePath, kGameSetupLine);
    }
    READABLE_FUNCTION(gameSetup, Verified);

    // gameCleanup: the game's end as a level is left. The game task ends, the
    // texture effects stop, the level's objectives and the objectives are
    // freed. Unless the session has one rider: the round's replay is freed
    // where it is not the one player's, and from the last player to the
    // first each rider gets the model it had as the game started, the copy
    // is freed and the player's replay too.
    void gameCleanup()
    {
        zge::deleteTask(*g_gameTask);
        zge::freeTextureEffects();
        objectives::freeLevelObjectives();
        zge::freeObjectives();
        if (sessionHasOneRider())
            return;
        if (*g_playerCount != 1)
            freeReplay(*g_roundReplay);
        for (int32_t player = static_cast<int32_t>(*g_playerCount) - 1; player >= 0; --player)
        {
            zge::copyModelRecord(playerRider(static_cast<uint32_t>(player))->model, (*g_keptRiderModels)[player]);
            zge::freeModelRecord((*g_keptRiderModels)[player]);
            freeReplay((*g_playerReplays)[player]);
        }
    }
    READABLE_FUNCTION(gameCleanup, Verified);
}
