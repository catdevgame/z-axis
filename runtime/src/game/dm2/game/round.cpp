// DM2's game modes and rounds (round.h): the game's mode, the round being
// played, its phase and its flags. DM2's own code (game.c, its game block).

#include "round.h"
#include "competition.h"
#include "gaps.h"
#include "goals.h"
#include "rewards.h"
#include "npcs.h"
#include "players.h"
#include "records.h"
#include "score.h"
#include "dm2/sound/sound_mode.h"
#include "../action/saved_records.h"

#include "dm2/action/action.h"
#include "dm2/camera/camera.h"
#include "dm2/editor/editor_state.h"
#include "dm2/menu/menu_mode.h"
#include "dm2/overlay/overlay.h"
#include "dm2/replay/replays.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/rider.h"
#include "dm2/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/round.cpp.h"

namespace game::dm2::rider
{
    ee::Ptr<Rider> riderOf(uint32_t index); // rider_state.cpp
    int32_t riderFrame(uint32_t index);     // actions/frame.cpp
    Bounds *levelBounds();                  // contact.cpp
}
namespace game::dm2::rider::physics
{
    void riderReset(uint32_t index, uint32_t shown); // respawn.cpp
    collision::FaceRef respawnAtStart(const ee::Float *point, uint32_t flags);
    int32_t locatorMatrix(const ee::Float *point, uint32_t name, zge::Matrix *matrix);
}

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kMultiplayerModes = 15; // 0 (single player) .. 14

        // The game being played (multiplayerMode, singlePlayerMode).
        inline ee::Global<uint8_t, sym::g_multiplayerMode> g_multiplayerMode;
        inline ee::Global<uint8_t, sym::g_singlePlayerMode> g_singlePlayerMode;
        // The rounds of each multiplayer mode (single player: one).
        inline ee::Global<uint8_t[kMultiplayerModes], sym::g_roundsByMode> g_roundsByMode;
        constexpr uint8_t kCompetitionRuns = 2;

        // The round: its flags, its number, its phase and the player who
        // starts it.
        inline ee::Global<uint8_t, sym::g_roundFlags> g_roundFlags;
        inline ee::Global<uint8_t, sym::g_roundNumber> g_roundNumber;
        inline ee::Global<uint8_t, sym::g_roundPhase> g_roundPhase;
        inline ee::Global<uint8_t, sym::g_firstPlayer> g_firstPlayer;

        // The level being played; the park editor's is the one after the
        // built-in levels.
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
        constexpr uint8_t kEditorLevel = 8;

        // The game's replays (the recorder's slots): each player's turn, the
        // whole round's (all riders, for a multiplayer game's end),
        // and the last turn's (set when its time is up).
        inline ee::Global<uint8_t[2], sym::g_playerReplays> g_playerReplays;
        inline ee::Global<uint8_t, sym::g_roundReplay> g_roundReplay;
        inline ee::Global<uint8_t, sym::g_lastTurnReplay> g_lastTurnReplay;

        // A competition's rivals are drawn from the first fourteen characters.
        constexpr uint32_t kDrawnCharacters = 14;

        // The record kinds a game's start clears, and the record entries of each.
        constexpr uint32_t kClearedRecordKinds = 13;
        constexpr uint32_t kClearedRecordEntries = 3;
        // The single-player modes a record key can name (two bits: the park
        // test, the fifth mode, has no records of its own).
        constexpr uint32_t kRecordKeySingles = kRecordKeySingleMask + 1;

        // The game task (gameSetup makes it): its frame draws the goals'
        // markers; its source file and the line it is made at.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_gameTask> g_gameTask;
        constexpr uint32_t kGameTaskFrame = sym::gameTaskFrame;
        constexpr uint32_t kGameSourcePath = sym::gameSourcePath; // "./src/com/game.c"
        constexpr uint32_t kGameSetupLine = 428;

        // The level's quarters: its extent in x and in z over four, the cell
        // of a rider's position (the record check, checkGameRecord).
        inline ee::Global<int16_t, sym::g_levelQuarterX> g_levelQuarterX;
        inline ee::Global<int16_t, sym::g_levelQuarterZ> g_levelQuarterZ;
        constexpr int32_t kLevelCells = 4;

        // The HORSE word (multiplayer mode 3): "BMX" until a player sets it.
        constexpr uint32_t kHorseWordFormat = sym::horseWordFormat;   // "%s"
        constexpr uint32_t kDefaultHorseWord = sym::defaultHorseWord; // "BMX"

        // A multiplayer game's players share this much recording (ReplaySetup::dataBytes).
        constexpr uint32_t kSharedReplayBytes = 0x1000;
        // The multiplayer mode before a menu picks one.
        constexpr uint8_t kNoMultiplayerMode = 0xFF;

        // The locator a player's rider spawns at ("PlayerOne", a Spawn locator),
        // and the position a mode-4 turn draws at random instead (setRoundPhase).
        constexpr uint32_t kPlayerOneName = sym::playerOneName;
        inline ee::Global<ee::Float[4], sym::g_randomSpawnPosition> g_randomSpawnPosition;

        void clearGameScore(rider::Rider *rider)
        {
            rider->score.horseLetters = 0;
            rider->score.gameScore = 0;
        }

        // Whether `character` is one of the first `count` of the field.
        bool inField(const uint8_t *field, uint32_t count, uint8_t character)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (field[i] == character)
                    return true;
            }
            return false;
        }
    }

    // setRoundFlags: `flags` set (on) or cleared; returns `on`.
    uint32_t setRoundFlags(uint8_t flags, uint32_t on)
    {
        if (on != 0)
            *g_roundFlags |= flags;
        else
            *g_roundFlags &= static_cast<uint8_t>(~flags);
        return on;
    }
    READABLE_FUNCTION(setRoundFlags, Verified);

    // roundFlags: whether any of `flags` is set.
    bool roundFlags(uint8_t flags)
    {
        return (*g_roundFlags & flags) != 0;
    }
    READABLE_FUNCTION(roundFlags, Verified);

    uint8_t multiplayerMode()
    {
        return *g_multiplayerMode;
    }
    READABLE_FUNCTION(multiplayerMode, Verified);

    uint8_t setSinglePlayerMode(uint8_t mode)
    {
        *g_singlePlayerMode = mode;
        return mode;
    }
    READABLE_FUNCTION(setSinglePlayerMode, Verified);

    uint8_t singlePlayerMode()
    {
        return *g_singlePlayerMode;
    }
    READABLE_FUNCTION(singlePlayerMode, Verified);

    uint8_t roundPhase()
    {
        return *g_roundPhase;
    }
    READABLE_FUNCTION(roundPhase, Verified);

    // roundCount: a single-player competition has its two runs; every other
    // game the rounds of its multiplayer mode (one for single player).
    uint8_t roundCount()
    {
        if (multiplayerMode() == 0 && singlePlayerMode() == kCompetition)
            return kCompetitionRuns;
        return (*g_roundsByMode)[multiplayerMode()];
    }
    READABLE_FUNCTION(roundCount, Verified);

    uint8_t roundNumber()
    {
        return *g_roundNumber;
    }
    READABLE_FUNCTION(roundNumber, Verified);

    uint8_t firstPlayer()
    {
        return *g_firstPlayer;
    }
    READABLE_FUNCTION(firstPlayer, Verified);

    // shownReplay: the replay the game shows: a single-player game's own; in
    // a multiplayer game the whole round's at the game's end, else the last
    // turn's.
    uint8_t shownReplay()
    {
        const uint8_t own = (*g_playerReplays)[0];
        if (multiplayerMode() == 0)
            return own;
        if (roundPhase() == kGameOverPhase)
            return *g_roundReplay;
        return *g_lastTurnReplay;
    }
    READABLE_FUNCTION(shownReplay, Verified);

    // startGame: a new game of a multiplayer mode (0: the single-player mode
    // set): the round's state from the start, the first player up, every
    // player's game score and HORSE letters cleared, the game's records of the
    // mode cleared and every span's announcements of its own records, a
    // competition's field drawn, a multiplayer game tied, then the game's first
    // phase. Returns the mode.
    uint8_t startGame(uint32_t mode)
    {
        *g_roundFlags = 0;
        *g_roundPhase = kNoRoundPhase;
        *g_firstPlayer = 0;
        *g_multiplayerMode = static_cast<uint8_t>(mode);
        *g_roundNumber = 0;
        const uint8_t players = *g_multiplayerPlayers;
        *g_currentPlayer = firstPlayer();
        if (players == 0)
        {
            clearGameScore(rider::riderOf(*g_singlePlayerRider).get());
        }
        else
        {
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                clearGameScore(rider::riderOf(riderIndexOf(player)).get());
        }

        for (uint32_t kind = 0; kind < kClearedRecordKinds; ++kind)
        {
            for (uint32_t entry = 0; entry < kClearedRecordEntries; ++entry)
            {
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(mode, singlePlayerMode(), kind, kGameRecords, entry));
                if (record)
                {
                    record->marks = 0;
                    record->value = 0.0f;
                }
            }
        }
        for (uint32_t span = 0; span < kRecordSpans; ++span)
        {
            for (uint32_t entry = 0; entry < kClearedRecordEntries; ++entry)
            {
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(mode, singlePlayerMode(), 0, span, entry));
                if (record)
                    record->marks = 0;
            }
        }

        if (mode == 0 && singlePlayerMode() == kCompetition)
        {
            uint8_t *field = *g_competitionRiders;
            field[0] = (*g_playerSetups)[*g_currentPlayer].character;
            for (uint32_t i = 1; i < kCompetitionRiders; ++i)
            {
                uint8_t character;
                do
                    character = static_cast<uint8_t>(zge::randomNumber() % kDrawnCharacters);
                while (inField(field, i, character));
                field[i] = character;
            }
        }

        setRoundFlags(kTieFlag, mode != 0);
        setRoundPhase(kGameStartPhase);
        return *g_multiplayerMode;
    }
    READABLE_FUNCTION(startGame, Verified);

    // spawnPlayer: a player's rider put at the level's start for its turn. Its
    // turn's score records are emptied and its rows set upright at the
    // "PlayerOne" locator (a mode-4 game: at the position the turn drew), the
    // drawn body with them; then the rider is reset (riderReset) and, shown, a
    // single-player game's goals start over (a level-goals game's entity made
    // active) and a first rider frame runs. The overlay's tip, objective,
    // reward, fall and exit warning are cleared and its score shown as 0.
    void spawnPlayer(uint32_t player, uint32_t shown)
    {
        ee::Local<zge::Matrix> locator;
        ee::Local<uint32_t> noScore;
        ee::Local<uint8_t> noTip;
        ee::Local<uint8_t> noObjective;
        ee::Local<uint8_t> noReward;
        ee::Local<uint8_t> noFall;
        ee::Local<uint8_t> noExitWarning;

        const uint32_t riderIndex = riderIndexOf(player);
        rider::Rider *const rider = rider::riderOf(riderIndex).get();
        rider->score.recordBeaten = 0;
        rider->score.lineState = 0;
        rider->score.lineCount = 0;
        rider->score.trickCount = 0;
        rider->health = rider::kFullHealth;

        zge::Matrix *const rows = rider::matrix(rider->right);
        zge::matrixIdentity(rows);
        zge::vec3Negate(&rows->row(1).xyz(), &rows->row(1).xyz());
        *rider::vec4(rider->position) = zge::kZero;
        if (multiplayerMode() == kRandomSpawnMode)
        {
            *rider::vec4(rider->position) = *rider::vec4(*g_randomSpawnPosition);
        }
        else if (rider::physics::locatorMatrix(rider->position, zge::nameHash(ee::pointerAt<char>(kPlayerOneName)), locator) != 0)
        {
            *rider::vec4(rider->position) = locator->translation();
            zge::copyRotationAndTranslation(rows, locator);
        }
        *rider::vec4(rider->drawnPosition) = *rider::vec4(rider->position);

        rider::physics::riderReset(riderIndex, shown);
        restoreSavedLevelObjects();
        if (shown != 0)
        {
            if (multiplayerMode() == 0)
            {
                if (singlePlayerMode() == kLevelGoals)
                    restartLevelGoals(player);
                rider->score.goalFlags = 0;
                rider->score.gapsFound = 0;
                rider->score.respectEarned = 0;
            }
            rider::riderFrame(riderIndex);
        }

        noTip = kOverlayNone;
        overlayMessage(kOverlayTip, noTip);
        noObjective = kOverlayNone;
        overlayMessage(kOverlayObjective, noObjective);
        noFall = kOverlayNone;
        overlayMessage(kOverlayFall, noFall);
        noReward = kOverlayNone;
        overlayMessage(kOverlayReward, noReward);
        noExitWarning = kOverlayNone;
        overlayMessage(kOverlayExitWarning, noExitWarning);
        noScore = 0;
        overlayMessage(kOverlayScore, noScore);
    }
    READABLE_FUNCTION(spawnPlayer, Verified);

    namespace
    {
        // A quarter of a level extent, as the compiler divides a signed
        // integer by four (the result rounded towards zero).
        int16_t quarterOf(ee::Float extent)
        {
            return static_cast<int16_t>((ee::toInt(extent) + 3) / kLevelCells);
        }
    }

    // gameSetup: the game's set-up as a level starts (action.c's level start).
    // A single-player game runs its turn from the start; a multiplayer game
    // gets each player's replay (sharing 4 KB of recording; its first rider's
    // doubles as the round's when there is one player, else the round gets
    // its own over every rider), the HORSE word back to "BMX" and, unless a
    // replay is being viewed, no mode until the menus pick one. Every mode's
    // records are read from the saved ones, the level's quarters measured,
    // its gaps and goals set up, and the game task made.
    void gameSetup()
    {
        ee::Local<ReplaySetup> setup;
        const uint8_t players = *g_multiplayerPlayers;
        if (players == 0)
        {
            *g_roundPhase = kTurnRunningPhase;
            *g_multiplayerMode = 0;
            *g_singlePlayerMode = 0;
        }
        else
        {
            setup->dataBytes = kSharedReplayBytes / players; // (players is not 0 here: the original's zero check)
            setup->flags = players > 1 ? kReplayMultiplayerSetUp : 0;
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
            {
                setup->rider = riderIndexOf(player);
                (*g_playerReplays)[player] = newReplay(setup);
                playerCharacter(player)->flags &= ~kCharacterSetupBits;
            }
            if (*g_multiplayerPlayers == 1)
            {
                *g_roundReplay = (*g_playerReplays)[0];
            }
            else
            {
                setup->rider = kReplayNoRider;
                *g_roundReplay = newReplay(setup);
            }
            *g_lastTurnReplay = (*g_playerReplays)[0];
            zge::formatString(sym::g_horseWord, kHorseWordFormat, kDefaultHorseWord);
            if (menuMode() != kMenuViewReplay)
            {
                *g_singlePlayerMode = 0;
                *g_multiplayerMode = kNoMultiplayerMode;
            }
        }

        for (uint32_t mode = 0; mode < kMultiplayerModes; ++mode)
        {
            for (uint32_t single = 0; single < kRecordKeySingles; ++single)
            {
                for (uint32_t kind = 0; kind < kClearedRecordKinds; ++kind)
                {
                    for (uint32_t entry = 0; entry < kClearedRecordEntries; ++entry)
                    {
                        const uint32_t key = recordKey(mode, single, kind, kSavedRecords, entry);
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
        }

        const rider::Bounds *const bounds = rider::levelBounds();
        *g_levelQuarterX = quarterOf(bounds->max[0] - bounds->min[0]);
        *g_levelQuarterZ = quarterOf(bounds->max[2] - bounds->min[2]);
        initLevelGaps();
        initLevelGoals();
        *g_gameTask = zge::newTask(1, kGameTaskFrame, 0, 1, kGameSourcePath, kGameSetupLine);
    }
    READABLE_FUNCTION(gameSetup, Verified);

    // gameCleanup: the game's end as a level is left. The park editor's
    // level takes its parks' scores again; the game task ends; the goals'
    // material and the gaps are given up; the replays are freed (the
    // round's own only when there are two players or more: one player's
    // doubles as it).
    void gameCleanup()
    {
        if (*g_sessionLevel == kEditorLevel)
            takeEditorParkScoresAgain();
        zge::deleteTask(*g_gameTask);
        releaseGoalsMaterial();
        cleanUpLevelGaps();
        if (*g_multiplayerPlayers == 0)
            return;
        if (*g_multiplayerPlayers != 1)
            freeReplay(*g_roundReplay);
        for (int32_t player = *g_multiplayerPlayers - 1; player >= 0; --player)
            freeReplay((*g_playerReplays)[player]);
    }
    READABLE_FUNCTION(gameCleanup, Verified);

    // The HORSE word set (the menus' entry), and read; both return its text.
    ee::Ptr<char> setHorseWord(ee::Ptr<const char> word)
    {
        zge::formatString(sym::g_horseWord, kHorseWordFormat, word);
        return ee::Ptr<char>(sym::g_horseWord);
    }
    READABLE_FUNCTION(setHorseWord, Verified);

    ee::Ptr<char> horseWord()
    {
        return ee::Ptr<char>(sym::g_horseWord);
    }
    READABLE_FUNCTION(horseWord, Verified);

    namespace
    {
        // The phase's clock: the frames it has left (the timer overlay shows
        // it; the game's frame counts it down), and how far the running
        // turn's end has got (the game's frame steps it once the clock is out).
        inline ee::Global<uint32_t, sym::g_phaseFramesLeft> g_phaseFramesLeft;
        inline ee::Global<uint8_t, sym::g_turnEndStage> g_turnEndStage;
        constexpr uint32_t kTurnStartFrames = 600;
        constexpr uint32_t kExitFadeFrames = 30;
        // A turn's time by game, at 60 frames a second: the single-player
        // games take the seconds set for the turn (180 as the game starts).
        constexpr uint32_t kFramesPerSecond = 60;
        constexpr uint32_t kCompetitionRunFrames = 3600;
        constexpr uint32_t kTurnFrames = 1800;
        constexpr uint32_t kFreeRideFrames = 18000;
        constexpr uint32_t kRandomSpawnFrames = 900;
        inline ee::Global<uint32_t, sym::g_turnSeconds> g_turnSeconds;

        // Who won each round, and who leads the game (roundWinner).
        inline ee::Global<uint8_t[15], sym::g_roundWinners> g_roundWinners;
        inline ee::Global<uint8_t, sym::g_leader> g_leader;


        // The play phases the round sets (the sound's; AI's names them).
        constexpr uint32_t kPlayPhaseRiding = 0;
        constexpr uint32_t kPlayPhaseTimed = 1;
        constexpr uint32_t kPlayPhaseStopped = 4;

        // What the round asks of code not read yet: the start-face search's
        // flags for a drawn place (respawnAtStart; what they select is the
        // rider code's), and the camera mode a turn's start sets (camera.c's;
        // what it shows is not read).
        constexpr uint32_t kDrawnPlaceFlags = 3;
        constexpr uint32_t kCameraBeforeTurn = 3;

        // An unsigned word as a float (the compiled code's two ways).
        ee::Float unsignedToFloat(uint32_t w)
        {
            if (static_cast<int32_t>(w) >= 0)
                return ee::toFloat(static_cast<int32_t>(w));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((w & 1u) | (w >> 1)));
            return half + half;
        }

        // The key of one of the current game's records (the game's modes are
        // asked for again for every key).
        uint32_t currentRecordKey(uint32_t kind, uint32_t span)
        {
            const uint32_t mode = multiplayerMode();
            const uint32_t single = singlePlayerMode();
            return recordKey(mode, single, kind, span);
        }

        // Whether the game has a player `player`: a game that counts no
        // players still has its one rider.
        bool hasPlayer(uint32_t player)
        {
            const uint8_t players = *g_multiplayerPlayers;
            return players == 0 ? player == 0 : player < players;
        }

        // Whether a phase is `player`'s to act in: the current player's, or
        // everyone's where all ride at once.
        bool takesTurn(uint32_t player)
        {
            return player == *g_currentPlayer || multiplayerMode() == kAllAtOnceMode;
        }

        rider::Rider *playerRider(uint32_t player)
        {
            return rider::riderOf(riderIndexOf(player)).get();
        }

        // In a series and in the best-turn modes the field counts the rounds a
        // rider has won (in HORSE, the letters taken).
        uint32_t &roundsWon(rider::Rider *rider)
        {
            return rider->score.horseLetters;
        }

        // A value drawn between two bounds, in 256ths of the whole units between them.
        ee::Float drawBetween(ee::Float low, ee::Float high)
        {
            const uint32_t share = zge::randomNumber() & 0xFF;
            const uint32_t span = static_cast<uint32_t>(ee::toInt(high - low));
            return low + ee::toFloat(static_cast<int32_t>((share * span) >> 8));
        }

        // The camera taken off the riders to a level point, for a screen.
        void cameraToLevelPoint()
        {
            setCameraPlayer(0);
            setCameraMode(0, 0, kCameraLevelPoint);
        }

        // Whether the game set one of the level's saved records (its name is asked for then).
        bool savedRecordSet()
        {
            for (uint32_t kind = 0; kind < kRecordKinds; ++kind)
            {
                const ee::Ptr<GameRecord> record = gameRecord(currentRecordKey(kind, kSavedRecords));
                if (record && (record->marks & kRecordSet) != 0)
                    return true;
            }
            return false;
        }

        // The current game's own record: its mode's kind, over the game.
        ee::Ptr<GameRecord> gameBestRecord()
        {
            const uint32_t mode = multiplayerMode();
            const uint32_t kind = (*g_modeRecordKinds)[multiplayerMode()];
            const uint32_t single = singlePlayerMode();
            return gameRecord(recordKey(mode, single, kind, kGameRecords));
        }

        // The game's last round is marked as it ends.
        void markLastRound()
        {
            if (roundCount() != 0)
            {
                const uint32_t number = roundNumber();
                if (number == static_cast<uint32_t>(roundCount()) - 1)
                    setRoundFlags(kGameEndsFlag, 1);
            }
        }
    }

    // roundWinner: the player who won `round`; with fewer than two rounds
    // asked for, the game's leader.
    uint8_t roundWinner(uint32_t round, uint32_t rounds)
    {
        if (rounds < 2)
            return *g_leader;
        return (*g_roundWinners)[round];
    }
    READABLE_FUNCTION(roundWinner, Verified);

    void turnStartHook(uint32_t)
    {
    }
    READABLE_FUNCTION(turnStartHook, Verified);

    // setRoundPhase: the round's phase machine. Entering a phase does all the
    // phase is: see each case. Returns the phase the round is in afterwards.
    //
    // Before anything, a turn that is over going to the next turn's start
    // passes the turn on: the next player is up; and when the round is done
    // the first player is up again (in HORSE, unless the round was drawn, the
    // player after the round's winner becomes the first), and the round is
    // counted (modes 1 .. 4 do not count a drawn one).
    uint8_t setRoundPhase(uint32_t phase)
    {
        ee::Local<OverlayScore> score;
        ee::Local<ScreenFade> fade;
        ee::Local<OverlayTrick> trick;
        ee::Local<uint8_t> health; // the rider's, for the overlay

        if (*g_roundPhase == kTurnOverPhase && multiplayerMode() != kAllAtOnceMode && phase == kTurnStartPhase)
        {
            // (A game here has players: the original traps on none.)
            *g_currentPlayer = static_cast<uint8_t>(static_cast<uint8_t>(*g_currentPlayer + 1) % *g_multiplayerPlayers);
            if (roundFlags(kRoundDoneFlag))
            {
                if (multiplayerMode() == kHorseMode && !roundFlags(kRoundDrawnFlag))
                {
                    const uint8_t winner = roundWinner(roundNumber(), 2);
                    *g_firstPlayer = static_cast<uint8_t>(static_cast<uint8_t>(winner + 1) % *g_multiplayerPlayers);
                }
                *g_currentPlayer = firstPlayer();
                const uint32_t mode = multiplayerMode();
                if ((mode >= kFirstBestTurnMode && mode <= kLastBestTurnMode) || !roundFlags(kRoundDrawnFlag))
                    *g_roundNumber += 1;
            }
        }

        *g_phaseFramesLeft = 0;
        *g_roundPhase = static_cast<uint8_t>(phase);
        *g_turnEndStage = 0;

        switch (phase)
        {
        // The game's start: the instructions screen over a view of the
        // level. A free ride and a park test go straight to the first turn.
        case kGameStartPhase:
            if (multiplayerMode() != 0)
            {
                showGameScreen(kScreenInstructions);
                break;
            }
            if (singlePlayerMode() == kFreeRide || singlePlayerMode() == kParkTest)
                return setRoundPhase(kTurnStartPhase);
            setCameraMode(*g_currentPlayer, 0, kCameraLevelPoint);
            showGameScreen(kScreenInstructions);
            setPlayPhase(kPlayPhaseStopped);
            break;

        // A turn's start. The turn's records are cleared and the saved ones'
        // marks; with the first player up a round starts: its records are
        // cleared and every span's marks, a random-spawn game draws the
        // round's place (anywhere in the level's bounds a start face is found),
        // and the round is neither done nor drawn. The players whose turn it is
        // are spawned with their turn's counts cleared; the others' riders are
        // reset out of play. Then the screen before the turn (a level's
        // objectives, the multiplayer ready screen), or the turn at once.
        case kTurnStartPhase:
        {
            showGameScreen(kScreenNone);
            for (uint32_t kind = 0; kind < kRecordKinds; ++kind)
            {
                const ee::Ptr<GameRecord> turnRecord = gameRecord(currentRecordKey(kind, kTurnRecords));
                if (turnRecord)
                {
                    turnRecord->marks = 0;
                    turnRecord->value = 0.0f;
                }
                const ee::Ptr<GameRecord> saved = gameRecord(currentRecordKey(kind, kSavedRecords));
                if (saved)
                    saved->marks = 0;
            }

            const uint8_t current = *g_currentPlayer;
            if (current == firstPlayer())
            {
                setRoundFlags(kFirstTurnFlag, 1);
                for (uint32_t kind = 0; kind < kRecordKinds; ++kind)
                {
                    const ee::Ptr<GameRecord> roundRecord = gameRecord(currentRecordKey(kind, kRoundRecords));
                    if (roundRecord)
                    {
                        roundRecord->marks = 0;
                        roundRecord->value = 0.0f;
                    }
                }
                for (uint32_t span = 0; span < kRecordSpans; ++span)
                {
                    for (uint32_t kind = 0; kind < kRecordKinds; ++kind)
                    {
                        const ee::Ptr<GameRecord> record = gameRecord(currentRecordKey(kind, span));
                        if (record)
                            record->marks = 0;
                    }
                }
                if (multiplayerMode() == kRandomSpawnMode)
                {
                    const rider::Bounds *const bounds = rider::levelBounds();
                    ee::Float *const place = *g_randomSpawnPosition;
                    place[1] = 0.0f;
                    rider::collision::FaceRef face;
                    do
                    {
                        place[0] = drawBetween(bounds->min[0], bounds->max[0]);
                        place[2] = drawBetween(bounds->min[2], bounds->max[2]);
                        face = rider::physics::respawnAtStart(place, kDrawnPlaceFlags);
                    } while (face == 0);
                    rider::collision::faceCentre(face, place);
                }
                setRoundFlags(kRoundDoneFlag, 0);
                setRoundFlags(kRoundDrawnFlag, 0);
                *g_keptLineCount = 0;
            }
            else
            {
                setRoundFlags(kFirstTurnFlag, 0);
            }

            for (uint32_t player = 0; hasPlayer(player); ++player)
            {
                if (takesTurn(player))
                {
                    rider::Rider *const rider = playerRider(player);
                    spawnPlayer(player, 1);
                    turnStartHook(player);
                    if (*g_multiplayerPlayers != 0)
                    {
                        const PlayerSetup &setup = (*g_playerSetups)[player];
                        rider->controllerLayout = setup.controllerLayout;
                        zge::memoryCopy(rider->tricksOff, playerCharacter(player)->tricksOff,
                                        sizeof(RiderCharacter::tricksOff));
                        if (multiplayerMode() == 0 && (setup.flags & kPlayerTips) != 0)
                            rider->score.tipsLeft = rider::kAllTips;
                    }
                    zge::memoryFill(rider->score.trickRepeats, 0, sizeof(rider->score.trickRepeats));
                    score->score = 0.0f;
                    score->flags = kScoreAtOnce;
                    overlayMessage(kOverlayScore, score);
                    rider->score.turnScore = 0.0f;
                    zge::memoryFill(rider->score.cellTrickCounts, 0, sizeof(rider->score.cellTrickCounts));
                    health = rider->health;
                    overlayMessage(kOverlayEmpty, health);
                }
                else
                {
                    rider::physics::riderReset(riderIndexOf(player), 0);
                }
            }

            setCameraPlayer(*g_currentPlayer);
            setCameraMode(*g_currentPlayer, 0, kCameraBeforeTurn);
            *g_phaseFramesLeft = kTurnStartFrames;
            const uint32_t mode = multiplayerMode();
            if (mode == 0)
            {
                if (singlePlayerMode() == kLevelGoals)
                {
                    showGameScreen(kScreenObjectives);
                    break;
                }
                if (singlePlayerMode() == kFreeRide || singlePlayerMode() == kParkTest)
                    return setRoundPhase(kFreeTurnPhase);
                return setRoundPhase(kTurnRunningPhase);
            }
            if (mode == kAllAtOnceMode)
                return setRoundPhase(kTurnRunningPhase);
            showGameScreen(kScreenMultiplayerReady);
            setPlayPhase(kPlayPhaseRiding);
            break;
        }

        // A free ride's or park test's turn: each rider's replay starts
        // recording and the camera follows it. No clock.
        case kFreeTurnPhase:
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
            {
                if (takesTurn(player))
                {
                    trick->action = kTrickDisplayClear;
                    trick->turn = 0;
                    overlayMessage(kOverlayTrick, trick);
                    setReplayRider((*g_playerReplays)[player], riderIndexOf(player));
                    setReplayState((*g_playerReplays)[player], kReplayRecording);
                    setCameraPlayer(player);
                    setCameraMode(player, 0, kCameraFollows);
                }
            }
            setPlayPhase(kPlayPhaseRiding);
            break;

        // The turn itself: each rider's lines and scores by kind start empty,
        // its replay starts recording, the camera follows it, and the clock
        // is set for the game being played.
        case kTurnRunningPhase:
        {
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
            {
                if (takesTurn(player))
                {
                    rider::Rider *const rider = playerRider(player);
                    rider->score.lineState = 0;
                    rider->score.lineCount = 0;
                    rider->score.trickCount = 0;
                    rider->score.kindScores[0] = 0.0f;
                    rider->score.kindScores[1] = 0.0f;
                    rider->score.kindScores[2] = 0.0f;
                    trick->turn = ee::Address(&rider->score.turnScore);
                    trick->action = kTrickDisplayClear;
                    overlayMessage(kOverlayTrick, trick);
                    setReplayRider((*g_playerReplays)[player], riderIndexOf(player));
                    setReplayState((*g_playerReplays)[player], kReplayRecording);
                    // (The game's own record is looked up and not used.)
                    const uint32_t mode = multiplayerMode();
                    const uint32_t single = singlePlayerMode();
                    gameRecord(recordKey(mode, single, (*g_modeRecordKinds)[multiplayerMode()], kSavedRecords));
                    setCameraPlayer(player);
                    setCameraMode(player, 0, kCameraFollows);
                }
            }

            const uint32_t mode = multiplayerMode();
            if (mode == 1 || mode == kAllAtOnceMode)
            {
                // By the single-player game picked with it.
                const uint32_t single = singlePlayerMode();
                if (single == kCompetition)
                    *g_phaseFramesLeft = kCompetitionRunFrames;
                else if (single == kLevelGoals)
                    *g_phaseFramesLeft = kTurnFrames;
                else if (single == kFreeRide)
                    *g_phaseFramesLeft = kFreeRideFrames;
                else
                    *g_phaseFramesLeft = *g_turnSeconds * kFramesPerSecond;
            }
            else if (mode == 0)
            {
                *g_phaseFramesLeft = *g_turnSeconds * kFramesPerSecond;
                setPlayPhase(kPlayPhaseTimed);
            }
            else if (mode == kRandomSpawnMode)
            {
                *g_phaseFramesLeft = kRandomSpawnFrames;
            }
            else
            {
                *g_phaseFramesLeft = kTurnFrames;
            }
            setNpcPlay(kNpcPlayTurn);
            break;
        }

        // The rider rode into the level's exit: the screen fades to white
        // (in play; not in the other action modes), and 30 frames on the
        // turn is over.
        case kExitPhase:
            if (actionMode() == 0)
            {
                fade->frames = kExitFadeFrames;
                fade->blue = 0xFF;
                fade->flags = 0;
                fade->red = 0xFF;
                fade->green = 0xFF;
                overlayMessage(kOverlayFade, fade);
                showOverlayFrame(kOverlayFade, 1);
            }
            *g_phaseFramesLeft = kExitFadeFrames;
            break;

        // The turn is over (its time is up, or it was ended).
        case kTurnOverPhase:
        {
            // The NPCs and the replays stop, and the turn's replay becomes the last turn's.
            setNpcPlay(kNpcPlayStopped);
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
            {
                if (takesTurn(player))
                    setReplayState((*g_playerReplays)[player], kReplayIdle);
            }
            *g_lastTurnReplay = (*g_playerReplays)[*g_currentPlayer];

            // HORSE: the first player must score. With nothing, the round
            // is drawn and done, the next player becomes the first, and a
            // new round starts at once; else the round is theirs for now.
            if (multiplayerMode() == kHorseMode)
            {
                rider::Rider *const rider = playerRider(*g_currentPlayer);
                if (roundFlags(kFirstTurnFlag))
                {
                    if (rider->score.turnScore == ee::Float(0.0f))
                    {
                        // (A game here has players: the original traps on none.)
                        *g_firstPlayer = static_cast<uint8_t>((*g_firstPlayer + 1) % *g_multiplayerPlayers);
                        setRoundFlags(kRoundDrawnFlag, 1);
                        setRoundFlags(kRoundDoneFlag, 1);
                        return setRoundPhase(kTurnStartPhase);
                    }
                    (*g_roundWinners)[roundNumber()] = *g_currentPlayer;
                }
            }

            // The turn's score against the best-turn records (saved, the
            // game's, the round's), and a competition run judged. Not in a
            // free ride or a park test.
            if (multiplayerMode() != 0 || (singlePlayerMode() != kFreeRide && singlePlayerMode() != kParkTest))
            {
                for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                {
                    if (takesTurn(player))
                    {
                        rider::Rider *const rider = playerRider(player);
                        for (uint32_t span = kSavedRecords; span <= kRoundRecords; ++span)
                        {
                            const ee::Ptr<GameRecord> record = gameRecord(currentRecordKey(kTurnScoreRecord, span));
                            if (record && record->value < rider->score.turnScore)
                            {
                                record->value = rider->score.turnScore;
                                record->player = static_cast<uint8_t>(player);
                                record->marks |= kRecordSet;
                            }
                        }
                        judgeScore(player);
                    }
                }
            }

            // The game score takes the turn in: a single-player game's is
            // the turn's score; a best-turn mode keeps the better; modes 1, 4
            // and 14 add the turns up. A series and HORSE keep no score. Then
            // the game score against the game-score records.
            const uint32_t scoredMode = multiplayerMode();
            if (scoredMode != kSeriesMode && scoredMode != kHorseMode)
            {
                rider::Rider *const rider = playerRider(*g_currentPlayer);
                const uint32_t mode = multiplayerMode();
                if (mode == 0)
                {
                    rider->score.gameScore = sce::libgcc::fptoui(rider->score.turnScore);
                }
                else if (mode >= kFirstBestTurnMode && mode <= kLastBestTurnMode)
                {
                    if (unsignedToFloat(rider->score.gameScore) < rider->score.turnScore)
                        rider->score.gameScore = sce::libgcc::fptoui(rider->score.turnScore);
                }
                else
                {
                    for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                    {
                        if (takesTurn(player))
                        {
                            rider::Rider *const adding = playerRider(player);
                            adding->score.gameScore = sce::libgcc::fptoui(unsignedToFloat(adding->score.gameScore) + adding->score.turnScore);
                        }
                    }
                }

                if (multiplayerMode() != 0 || (singlePlayerMode() != kFreeRide && singlePlayerMode() != kParkTest))
                {
                    for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                    {
                        if (takesTurn(player))
                        {
                            rider::Rider *const scoring = playerRider(player);
                            for (uint32_t span = kSavedRecords; span <= kRoundRecords; ++span)
                            {
                                const ee::Ptr<GameRecord> record = gameRecord(currentRecordKey(kGameScoreRecord, span));
                                if (record && record->value < unsignedToFloat(scoring->score.gameScore))
                                {
                                    record->value = unsignedToFloat(scoring->score.gameScore);
                                    record->player = static_cast<uint8_t>(player);
                                    record->marks |= kRecordSet;
                                }
                            }
                        }
                    }
                }
            }

            // With the next player the first again, the round is done.
            // (A game here has players: the original traps on none.)
            const uint32_t nextPlayer = static_cast<uint32_t>(*g_currentPlayer + 1) % *g_multiplayerPlayers;
            if (nextPlayer == firstPlayer())
            {
                setRoundFlags(kRoundDoneFlag, 1);
                if (multiplayerMode() == kHorseMode)
                {
                    // Everyone who scored nothing takes a letter (then the
                    // round is not drawn); the leader is who has the
                    // fewest letters among those who scored. With all but
                    // one out of letters, the game ends.
                    uint32_t out = 0;
                    uint32_t fewest = static_cast<uint8_t>(zge::stringLength(sym::g_horseWord));
                    setRoundFlags(kRoundDrawnFlag, 1);
                    for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                    {
                        rider::Rider *const rider = playerRider(player);
                        if (rider->score.turnScore == ee::Float(0.0f))
                        {
                            const uint32_t letters = ++rider->score.horseLetters;
                            if (letters >= static_cast<uint32_t>(zge::stringLength(sym::g_horseWord)))
                                ++out;
                            setRoundFlags(kRoundDrawnFlag, 0);
                        }
                        else if (rider->score.horseLetters < fewest)
                        {
                            *g_leader = static_cast<uint8_t>(player);
                            fewest = static_cast<uint8_t>(rider->score.horseLetters);
                        }
                    }
                    setRoundFlags(kGameEndsFlag, out == static_cast<uint32_t>(*g_multiplayerPlayers) - 1 ? 1 : 0);
                }
                else
                {
                    // The round goes to who set its best turn, unless another
                    // player's turn equals it (a drawn round).
                    const ee::Ptr<GameRecord> best = gameRecord(currentRecordKey(kTurnScoreRecord, kRoundRecords));
                    if (best)
                    {
                        setRoundFlags(kRoundDrawnFlag, 0);
                        for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                        {
                            const rider::Rider *const rider = playerRider(player);
                            if (player != best->player && rider->score.turnScore == best->value)
                                setRoundFlags(kRoundDrawnFlag, 1);
                        }
                        if (!roundFlags(kRoundDrawnFlag))
                        {
                            roundsWon(playerRider(best->player)) += 1;
                            (*g_roundWinners)[roundNumber()] = best->player;
                        }
                    }

                    if (multiplayerMode() == kSeriesMode)
                    {
                        // A series: who has won the most rounds leads; a
                        // shared lead is a tie.
                        if (!roundFlags(kRoundDrawnFlag))
                        {
                            *g_leader = 0;
                            setRoundFlags(kTieFlag, 0);
                            uint32_t most = 0;
                            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                            {
                                rider::Rider *const rider = playerRider(player);
                                const uint32_t won = roundsWon(rider);
                                if (most < won)
                                {
                                    most = static_cast<uint8_t>(won);
                                    *g_leader = static_cast<uint8_t>(player);
                                    setRoundFlags(kTieFlag, 0);
                                }
                                else if (won == most)
                                {
                                    setRoundFlags(kTieFlag, 1);
                                }
                            }
                            markLastRound();
                        }
                    }
                    else
                    {
                        markLastRound();
                    }
                }

                // A multiplayer game keeps the replay of its best turn as the
                // round's: the record holder's replay and the round's change
                // places when the game's record was just set (or the first
                // round was drawn).
                if (multiplayerMode() != 0)
                {
                    const ee::Ptr<GameRecord> record = gameBestRecord();
                    if ((record->marks & kRecordSet) != 0 || (roundNumber() == 0 && roundFlags(kRoundDrawnFlag)))
                    {
                        const uint8_t holders = (*g_playerReplays)[record->player];
                        (*g_playerReplays)[record->player] = *g_roundReplay;
                        *g_roundReplay = holders;
                    }
                }
                roundRewards(*g_currentPlayer);
            }

            for (uint32_t player = 0; hasPlayer(player); ++player)
                rider::physics::riderReset(riderIndexOf(player), 0);

            // What follows. A saved record set in a game that goes on: its
            // name is asked for first.
            if (!roundFlags(kGameEndsFlag) && savedRecordSet())
            {
                showGameScreen(kScreenHighScore);
                cameraToLevelPoint();
                return *g_roundPhase;
            }
            const uint32_t mode = multiplayerMode();
            if (mode == kHorseMode)
            {
                if (roundFlags(kRoundDrawnFlag) || !roundFlags(kRoundDoneFlag))
                    return setRoundPhase(kTurnStartPhase);
            }
            else if (mode == 0)
            {
                if (roundFlags(kGameEndsFlag))
                    return setRoundPhase(kGameOverPhase);
                showGameScreen(kScreenLeaderboard);
                cameraToLevelPoint();
                break;
            }
            else if (mode >= kFirstBestTurnMode && mode <= kLastBestTurnMode)
            {
                if (roundFlags(kGameEndsFlag))
                    return setRoundPhase(kGameOverPhase);
                return setRoundPhase(kTurnStartPhase);
            }
            setCameraPlayer(*g_currentPlayer);
            setCameraMode(*g_currentPlayer, 0, kCameraLevelPoint);
            showGameScreen(kScreenMultiplayerTimeUp);
            break;
        }

        // The game's end: a tie when another player's game score equals the
        // game record's (not in a series, which settles its own); then the
        // screen: a saved record's name, a level's rewards, a competition's
        // leaderboard, or game over.
        case kGameOverPhase:
            if (multiplayerMode() != kSeriesMode)
            {
                const ee::Ptr<GameRecord> record = gameBestRecord();
                setRoundFlags(kTieFlag, 0);
                for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                {
                    const rider::Rider *const rider = playerRider(player);
                    if (player != record->player && rider->score.gameScore == sce::libgcc::fptoui(record->value))
                        setRoundFlags(kTieFlag, 1);
                }
            }
            if (savedRecordSet())
            {
                showGameScreen(kScreenHighScore);
                cameraToLevelPoint();
                return *g_roundPhase;
            }
            if (multiplayerMode() == 0)
            {
                playerRider(*g_currentPlayer);
                if (singlePlayerMode() == kLevelGoals)
                {
                    showGameScreen(kScreenReward);
                    cameraToLevelPoint();
                    return *g_roundPhase;
                }
                if (singlePlayerMode() == kCompetition)
                {
                    showGameScreen(kScreenLeaderboard);
                    cameraToLevelPoint();
                    return *g_roundPhase;
                }
            }
            showGameScreen(kScreenGameOver);
            break;

        default:
            break;
        }

        showOverlayFrame(kOverlayTimer, phase == kTurnRunningPhase);
        return *g_roundPhase;
    }
    READABLE_FUNCTION(setRoundPhase, Verified);

    // endTurnEarly: the turn ended before its time (the exit confirmation's
    // answer). While a turn runs, or once it is over, the current rider's
    // open trick line is ended, the turn is over and the play stopped (after
    // the turn, the camera first goes back to following the current player).
    // In any other phase the screen just closes.
    void endTurnEarly()
    {
        const uint32_t phase = roundPhase();
        if (phase < kFreeTurnPhase || phase == kExitPhase || phase > kGameOverPhase)
        {
            game::tail(showGameScreen, kScreenNone);
            return;
        }
        if (phase >= kTurnOverPhase)
        {
            setCameraPlayer(*g_currentPlayer);
            setCameraMode(*g_currentPlayer, 0, kCameraFollows);
        }
        if (playerRider(*g_currentPlayer)->score.lineState != 0)
            trickLineEnd(*g_currentPlayer, 0);
        setRoundPhase(kTurnOverPhase);
        setPlayPhase(kPlayPhaseStopped);
        game::tail(setGameMode, kSoundModeLevel);
    }
    READABLE_FUNCTION(endTurnEarly, Verified);
}
