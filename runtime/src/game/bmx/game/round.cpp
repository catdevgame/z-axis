// BMX XXX's game modes and rounds (round.h): the mode being played, the round
// being played, its phase and its flags. BMX XXX's own code (game.c, its game
// block), DM2's line (game/dm2/game/round.cpp) with the modes made one table.

#include "round.h"

#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "competition.h"
#include "spawn_points.h"
#include "npcs.h"
#include "player_riders.h"
#include "records.h"
#include "bmx/profile/records.h"
#include "score.h"
#include "turn_rewards.h"

#include "bmx/fx/library.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/displays.h"
#include "bmx/overlay/screens.h"
#include "bmx/replay/replays.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/weapon/player_weapon.h"
#include "bmx/weapon/weapon.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/round.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The game's replays (the recorder's slots), DM2's: each player's
        // turn, the last turn's (set when its time is up) and the whole
        // round's (all riders, for a multiplayer game's end).
        inline ee::Global<uint8_t[2], sym::g_playerReplays> g_playerReplays;
        inline ee::Global<uint8_t, sym::g_lastTurnReplay> g_lastTurnReplay;
        inline ee::Global<uint8_t, sym::g_roundReplay> g_roundReplay;
    }

    uint8_t gameMode()
    {
        return *g_currentGameMode;
    }
    READABLE_FUNCTION(gameMode, Verified);

    // gameModeRecord: the mode's record in the table.
    ee::Ptr<GameMode> gameModeRecord()
    {
        return ee::Ptr<GameMode>(sym::g_gameModes) + static_cast<ptrdiff_t>(gameMode());
    }
    READABLE_FUNCTION(gameModeRecord, Verified);

    // setRoundFlags: `flags` set (on) or cleared; returns `on`.
    uint32_t setRoundFlags(uint32_t flags, uint32_t on)
    {
        if (on != 0)
            g_round->flags |= flags;
        else
            g_round->flags &= ~flags;
        return on;
    }
    READABLE_FUNCTION(setRoundFlags, Verified);

    // roundFlags: whether any of `flags` is set.
    bool roundFlags(uint32_t flags)
    {
        return (g_round->flags & flags) != 0;
    }
    READABLE_FUNCTION(roundFlags, Verified);

    uint8_t roundPhase()
    {
        return g_round->phase;
    }
    READABLE_FUNCTION(roundPhase, Verified);

    // roundCount: the competition has its two runs; every other mode the
    // rounds its record gives.
    uint8_t roundCount()
    {
        if (gameMode() == kCompetitionMode)
            return kCompetitionRuns;
        return gameModeRecord()->rounds;
    }
    READABLE_FUNCTION(roundCount, Verified);

    uint8_t roundNumber()
    {
        return g_round->number;
    }
    READABLE_FUNCTION(roundNumber, Verified);

    uint8_t firstPlayer()
    {
        return g_round->firstPlayer;
    }
    READABLE_FUNCTION(firstPlayer, Verified);

    // shownReplay: the replay the game shows: a one-rider mode's own; in a
    // multiplayer mode the whole round's at the game's end, else the last
    // turn's.
    uint8_t shownReplay()
    {
        const uint8_t own = (*g_playerReplays)[0];
        if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
            return own;
        if (roundPhase() == kGameOverPhase)
            return *g_roundReplay;
        return *g_lastTurnReplay;
    }
    READABLE_FUNCTION(shownReplay, Verified);

    // ---- The round's phases (setRoundPhase) and a game's start ----------------

    namespace
    {
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        // A competition's field (kCompetitionRiders), the player's own first,
        // the others drawn from the first twelve.
        constexpr uint32_t kDrawnCharacters = 12;
        const ee::Ptr<uint8_t> kCompetitionField(sym::g_competitionRiders);

        // The record kinds, and the entries of each, that a game's start clears.
        constexpr uint32_t kClearedRecordEntries = 3;

        // The camera on the level (camera.c's), shown between turns; and the
        // player whose pad has the menus (the UI's "ModalPlayerNum" property;
        // -1: every player's), set with each game screen.
        inline ee::Global<ee::Ptr<ActionCamera>, sym::g_gameCamera> g_gameCamera;
        inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;
        constexpr int32_t kEveryPlayer = -1;

        // The place a round of kRandomSpawnMode starts at, and how many lines
        // HORSE's turns have to match (trickLineEnd keeps them), cleared as a
        // round starts.
        inline ee::Global<ee::Float[4], sym::g_randomSpawnPosition> g_randomSpawnPosition;
        inline ee::Global<uint8_t, sym::g_keptLineCount> g_keptLineCount;
        constexpr uint32_t kSpawnFaceKind = 3;

        // "%s": a text copied as it is (the HORSE word, a record's name).
        constexpr uint32_t kTextFormat = sym::textFormat;

        constexpr uint32_t kTurnStartFrames = 600;
        const ee::Float kFramesPerSecond = 60.0f;

        // The sound a turn's start makes.
        constexpr uint32_t kTurnStartSound = 0x47;

        // A display's message about a player: the score's value, or the
        // rider's scores for the trick display.
        struct PlayerMessage
        {
            uint8_t player;
            uint8_t shown;
            uint8_t _02[2];
            uint32_t value;
        };
        static_assert(sizeof(PlayerMessage) == 8);

        // The camera's viewport layouts and hearing by players on the screen
        // (camera.c's numbers): the whole screen, a half for each of two
        // players, a quarter for each of up to four.
        constexpr uint32_t kWholeScreenLayout = 0;
        constexpr uint32_t kFirstHalfLayout = 4;
        constexpr uint32_t kFirstQuarterLayout = 6;
        constexpr uint32_t kSplitHearing = 2;
        constexpr uint32_t kSniperCameraMode = 6;

        // The mode's own record of a span (the slot g_modeRecordSlots names).
        ee::Ptr<GameRecord> modeRecord(uint32_t span)
        {
            const uint32_t mode = gameMode();
            const uint32_t kind = ee::Ptr<uint8_t>(sym::g_modeRecordSlots)[gameMode()];
            return gameRecord(recordKey(mode, kind, span));
        }

        // The player after `player`, round the table.
        uint32_t playerAfter(uint32_t player)
        {
            return ((player + 1) & 0xFF) % *g_playerCount;
        }

        // Whether `character` is one of the first `count` of the field.
        bool inField(ee::Ptr<uint8_t> field, uint32_t count, uint32_t character)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (field[i] == character)
                    return true;
            }
            return false;
        }

        void clearGameScore(ee::Ptr<rider::Rider> rider)
        {
            rider->score.horseLetters = 0;
            rider->score.gameScore = 0;
        }

        // A rider who does not ride this turn, and its weapon, put away.
        void putRiderAway(uint32_t player)
        {
            rider::riderReset(riderIndexOf(player), 0);
            resetWeapon(playerWeapon(player), 0);
        }

        // A player's camera put away between turns.
        void hidePlayerCamera(uint32_t player)
        {
            showCamera(playerActionCamera(player), 0);
        }

        // A record beaten: the value, who, and the mark that it was set.
        void setRecord(ee::Ptr<GameRecord> record, ee::Float value, uint8_t player)
        {
            const uint8_t marks = record->marks;
            record->value = value;
            record->player = player;
            record->marks = marks | kRecordSet;
        }

        // Every record of the level set in this game goes to its saved
        // counterpart, with the character and the name of the player who set
        // it. Returns whether there was one.
        bool saveNewRecords()
        {
            bool any = false;
            for (uint32_t kind = 0; kind < kRecordsPerSpan; ++kind)
            {
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), kind, kSavedRecords));
                if (!record || (record->marks & kRecordSet) == 0)
                    continue;
                any = true;
                const ee::Ptr<SavedRecord> saved = savedRecord(recordKey(gameMode(), kind, kSavedRecords));
                saved->flags |= kSavedRecordNew;
                saved->value = record->value;
                saved->character = playerSetup(record->player)->character;
                saved->bike = playerCharacter(record->player)->bike;
                saved->bikeChoice = playerCharacter(record->player)->bikeChoice;
                zge::formatString(saved->name, kTextFormat, ee::Ptr<char>(playerSetup(record->player)->name));
            }
            return any;
        }

        // A round of kRandomSpawnMode starts where a random point of the
        // level's box has a face to spawn on: at that face's centre.
        void drawRandomSpawn(zge::Matrix *spawnMatrix)
        {
            const rider::Bounds *const bounds = rider::levelBounds();
            ee::Float *const position = *g_randomSpawnPosition;
            position[1] = 0.0f;
            rider::collision::FaceRef face;
            do
            {
                for (uint32_t axis = 0; axis <= 2; axis += 2)
                {
                    const uint32_t draw = zge::randomNumber();
                    const int32_t extent = ee::toInt(bounds->max[axis] - bounds->min[axis]);
                    const uint32_t part = static_cast<uint32_t>(static_cast<int32_t>(draw & 0xFF) * extent) >> 8;
                    position[axis] = bounds->min[axis] + ee::toFloat(static_cast<int32_t>(part));
                }
                face = rider::findSpawnFace(position, kSpawnFaceKind, spawnMatrix);
            } while (face == 0);
            rider::collision::faceCentre(face, position);
        }

        // A turn's start for a player who rides in it.
        void startPlayerTurn(uint32_t player, const GameMode *mode, PlayerMessage &scoreMessage)
        {
            const ee::Ptr<PlayerSetup> setup = playerSetup(player);
            if (gameMode() == kPaintballMode && player != *g_currentPlayer)
                setup->watches = 1;
            else
                setup->watches = 0;

            const ee::Ptr<rider::Rider> rider = playerRider(player);
            spawnPlayer(static_cast<uint8_t>(player), 1);
            if ((gameModeRecord()->flags & kModeMultiplayer) == 0 && *g_currentLevel < kLevels)
                objectives::resetTurnGoals(static_cast<uint8_t>(player));
            objectives::unusedTurnStartHook();
            if (!sessionHasOneRider())
            {
                rider->controllerLayout = setup->controllerLayout;
                if ((mode->flags & kModeMultiplayer) == 0 && (setup->flags & kSetupTips) != 0)
                    rider->score.tipsLeft = kAllTips;
            }
            zge::memoryFill(rider->score.trickRepeats, 0, sizeof(rider->score.trickRepeats));

            scoreMessage.player = static_cast<uint8_t>(player);
            scoreMessage.value = 0;
            scoreMessage.shown = 1;
            overlayMessage(kOverlayScore, &scoreMessage);
            rider->score.turnScore = 0.0f;
            for (uint32_t i = 0; i < sizeof(rider->score.turnBytesLoop); ++i)
                rider->score.turnBytesLoop[i] = 0;
            zge::memoryFill(rider->score.cellTrickCounts, 0, sizeof(rider->score.cellTrickCounts));
            zge::memoryFill(rider->score.turnBytes4, 0, sizeof(rider->score.turnBytes4));
            zge::memoryFill(rider->score.turnBytes2, 0, sizeof(rider->score.turnBytes2));
        }

        // The turn starts running for a player who rides in it: the line and
        // the scores by kind cleared, the replay recording the rider and the
        // weapon, the player's camera on its part of the screen.
        void runPlayerTurn(uint32_t player, const GameMode *mode, PlayerMessage &trickMessage)
        {
            const ee::Ptr<rider::Rider> rider = playerRider(player);
            rider->score.lineState = 0;
            rider->score.lineCount = 0;
            rider->score.trickCount = 0;
            rider->score.kindScores[0] = 0.0f;
            rider->score.kindScores[1] = 0.0f;
            rider->score.kindScores[2] = 0.0f;
            rider->score.kindScores[3] = 0.0f;
            trickMessage.value = ee::Address(&rider->score.turnScore);
            trickMessage.player = static_cast<uint8_t>(player);
            trickMessage.shown = 0;
            overlayMessage(kOverlayTrick, &trickMessage);

            const uint8_t *const replay = &(*g_playerReplays)[player];
            {
                const uint8_t slot = *replay;
                setReplayRider(slot, riderIndexOf(player));
            }
            {
                const uint8_t slot = *replay;
                setReplayWeapon(slot, playerWeapon(player));
            }
            setReplayState(*replay, kReplayRecording);
            modeRecord(kSavedRecords);

            const ee::Ptr<ActionCamera> camera = playerActionCamera(player);
            if ((mode->flags & (kModeMultiplayer | kModeTurns)) != kModeMultiplayer)
            {
                setCameraViewportLayout(camera, kWholeScreenLayout);
                setCameraHearing(camera, 0);
            }
            else if (*g_playerCount == 2)
            {
                setCameraViewportLayout(camera, player + kFirstHalfLayout);
                setCameraHearing(camera, player + kSplitHearing);
            }
            else if (*g_playerCount >= 3 && *g_playerCount < 5)
            {
                setCameraViewportLayout(camera, player + kFirstQuarterLayout);
                setCameraHearing(camera, (player & 2) >> 1 | kSplitHearing);
            }
            showCamera(camera, 1);
            setCameraMode(camera, 0, 0);
            if (playerSetup(player)->watches == 1)
            {
                // The sniper's camera, and the crosshairs laid out on it.
                setCameraMode(camera, 1, kSniperCameraMode);
                layoutWeaponCrosshairs(cameraScreenRectangle(camera));
            }
        }
    }

    namespace
    {
        // The level's locators the riders start at, by player ("PlayerOne",
        // "PlayerTwo"), and the paintball's two: the player who snipes
        // and the one who rides.
        const ee::Ptr<uint32_t> kSpawnLocatorNames(sym::g_spawnLocatorNames);
        constexpr uint32_t kSpawnLocators = 2;
        constexpr uint32_t kSniperLocatorName = sym::sniperShooterName;
        constexpr uint32_t kSniperTargetLocatorName = sym::sniperTargetName;

        // The saved game's flags (bmx.c's record; its second halfword). What
        // bit 0x10 says is not read yet: with it a spawned rider's shooter
        // owns weapon bit 4.
        // Scaffolding (raw offset): until bmx.c's SavedGame header is in this tree.
        const ee::Ptr<uint16_t> kSavedGameFlags(sym::g_savedGame + 2);
        constexpr uint16_t kSavedFlag10 = 0x10;

        // A spawn resets the rider's shooter (weapon.c's record of who holds
        // the weapons) with ShooterWeapons: the weapons owned, as bits, and
        // its flags. The values a spawn gives (weapon.c's bits, not read by name yet).
        constexpr uint32_t kOwnedBit4 = 4;
        constexpr uint8_t kShooterFlag4 = 4;
        constexpr uint8_t kShooterShownInGame = 3;

        // The overlay's displays a spawn clears (displays.h), and their
        // messages: each starts with the player; 0xFF (a word of ones for
        // the objective) is "nothing".
        constexpr uint8_t kOverlayNone = 0xFF;
        constexpr uint32_t kNoObjective = 0xFFFFFFFF;
        struct ItemMessage
        {
            uint8_t player;
            uint8_t item; // kOverlayNone: nothing shown
        };
        struct BalanceMessage
        {
            uint8_t player;
            uint8_t pad01; // padding: no reader (int25: the senders never set it; only whole-message copies move it)
            uint16_t value;
        };
        static_assert(offsetof(BalanceMessage, value) == 2);
        constexpr uint16_t kSpawnBalance = 0x20;
    }

    // spawnPlayer: a player's rider put at its start for a turn. Its trick
    // line is emptied. Shown, its rows are set upright (right and up turned
    // about) at the origin, then at the place the mode gives: the position a
    // random-spawn round drew, or the level's locator of the player (the
    // paintball has one for the player who snipes and one for the player
    // who rides), whose matrix the rows take when the level has it; the drawn
    // body goes there too. The rider is reset, its shooter and its weapon reset,
    // and, shown, a first rider frame runs. The overlay's hint, objective,
    // reward and exit warning are cleared, and the score, balance and health
    // displays told of the start.
    void spawnPlayer(uint8_t player, uint32_t shown)
    {
        ee::Local<zge::Matrix> locator;
        ee::Local<ShooterWeapons> shooterReset;
        ee::Local<PlayerMessage> score;
        ee::Local<ItemMessage> noHint;
        ee::Local<PlayerMessage> noObjective;
        ee::Local<ItemMessage> noReward;
        ee::Local<ItemMessage> noExitWarning;
        ee::Local<BalanceMessage> balance;
        ee::Local<PlayerMessage> health;
        ee::Local<char[0x20]> locatorName;
        ee::Local<uint32_t[kSpawnLocators]> playerLocatorNames;

        for (uint32_t i = 0; i < kSpawnLocators; ++i)
            playerLocatorNames[i] = kSpawnLocatorNames[i];

        const uint32_t riderIndex = riderIndexOf(player);
        rider::Rider *const rider = rider::riderOf(riderIndex).get();
        rider->score.trickCount = 0;
        rider->score.recordBeaten = 0;
        rider->score.lineState = 0;
        rider->score.lineCount = 0;
        rider->health = kFullHealth;

        if (shown != 0)
        {
            zge::Matrix *const rows = rider::matrix(rider->right);
            zge::matrixIdentity(rows);
            zge::vec3Negate(&rider::vec4(rider->up)->xyz(), &rider::vec4(rider->up)->xyz());
            zge::vec3Negate(&rider::vec4(rider->right)->xyz(), &rider::vec4(rider->right)->xyz());
            *rider::vec4(rider->position) = zge::kZero;
            if (gameMode() == kRandomSpawnMode)
            {
                *rider::vec4(rider->position) = *rider::vec4(*g_randomSpawnPosition);
            }
            else
            {
                if (gameMode() == kPaintballMode)
                {
                    if (playerSetup(player)->watches == 1)
                        zge::formatString(locatorName.address(), kTextFormat, ee::Ptr<const char>(kSniperLocatorName));
                    else
                        zge::formatString(locatorName.address(), kTextFormat, ee::Ptr<const char>(kSniperTargetLocatorName));
                }
                else
                {
                    zge::formatString(locatorName.address(), kTextFormat, ee::Ptr<const char>(playerLocatorNames[player]));
                }
                const uint32_t name = zge::nameHash(locatorName);
                if (locatorMatrix(rider->position, name, locator) == 1)
                {
                    *rider::vec4(rider->position) = locator->translation();
                    *rows = *locator;
                }
            }
            *rider::vec4(rider->drawnPosition) = *rider::vec4(rider->position);
        }

        rider::riderReset(riderIndex, shown);

        shooterReset->owned = 0;
        if ((*kSavedGameFlags & kSavedFlag10) != 0)
            shooterReset->owned = kOwnedBit4;
        shooterReset->flags = kShooterFlag4;
        if (shown != 0 && gameMode() != kChallengeMode)
            shooterReset->flags |= kShooterShownInGame;
        resetShooter(ee::Ptr<Shooter>(rider->shooter), shooterReset);
        resetWeapon(playerWeapon(player), shown);
        if (shown != 0)
            rider::riderFrame(riderIndex);

        noHint->player = player;
        noHint->item = kOverlayNone;
        overlayMessage(kOverlayHint, noHint);
        noObjective->player = player;
        noObjective->value = kNoObjective;
        overlayMessage(kOverlayObjective, noObjective);
        noReward->player = player;
        noReward->item = kOverlayNone;
        overlayMessage(kOverlayReward, noReward);
        noExitWarning->item = kOverlayNone;
        noExitWarning->player = player;
        overlayMessage(kOverlayExitWarning, noExitWarning);
        score->player = player;
        score->value = 0;
        overlayMessage(kOverlayScore, score);
        balance->player = player;
        balance->value = kSpawnBalance;
        overlayMessage(kOverlayBalance, balance);
        health->player = player;
        health->shown = 1;
        health->value = 0;
        overlayMessage(kOverlayHealth, health);
    }
    READABLE_FUNCTION(spawnPlayer, Verified);

    // playerRides: where the players of a multiplayer mode take turns, only
    // the current player rides.
    bool playerRides(uint8_t player)
    {
        const uint32_t turnTaking = kModeMultiplayer | kModeTurns;
        if ((gameModeRecord()->flags & turnTaking) != turnTaking)
            return true;
        return player == *g_currentPlayer;
    }
    READABLE_FUNCTION(playerRides, Verified);

    void setTurnEndStage(uint8_t stage)
    {
        g_round->turnEndStage = stage;
        g_round->turnEndFrames = 0;
    }
    READABLE_FUNCTION(setTurnEndStage, Verified);

    uint8_t roundWinner(uint32_t round, uint32_t rounds)
    {
        if (rounds < 2)
            return g_round->leader;
        return g_round->roundWinners[round];
    }
    READABLE_FUNCTION(roundWinner, Verified);

    // The HORSE word set (the menus' entry), and read; both return its text.
    ee::Ptr<char> setHorseWord(ee::Ptr<const char> word)
    {
        zge::formatString(sym::g_horseWord, kTextFormat, word);
        return ee::Ptr<char>(sym::g_horseWord);
    }
    READABLE_FUNCTION(setHorseWord, Draft);

    ee::Ptr<char> horseWord()
    {
        return ee::Ptr<char>(sym::g_horseWord);
    }
    READABLE_FUNCTION(horseWord, Draft);

    // setUpLevelPickups: each of the level's objects is visited by
    // setUpLevelPickup, which shows or hides the pickups (HEALTH, WEAPON,
    // SCORE, BIKE, SNIPER) for the mode being played.
    void setUpLevelPickups()
    {
        game::tail(zge::forEachObject, sym::setUpLevelPickup, 0u);
    }
    READABLE_FUNCTION(setUpLevelPickups, Verified);

    // setRoundPhase: the round's phase machine. A phase is asked for; what the
    // round was in decides what is closed first (a running turn's open trick
    // lines; the turn over's hand to the next player, and to the next round
    // once everyone has ridden), then the new phase is set up:
    //   game start    the camera on the level and the game's first screen;
    //   turn start    the turn's records cleared (the round's too when the
    //                 first player is up), the riders who ride put at the
    //                 start, 600 frames on the clock and the screen before
    //                 the turn, or straight on to the running turn;
    //   turn running  the replays recording, each rider's camera on its part
    //                 of the screen, the clock at the mode's time;
    //   turn over     the replays stopped, the scores and records taken, the
    //                 round's winner and the game's leader found, the riders
    //                 put away, then the next turn, the game's end or a screen;
    //   game over     the tie found, new records saved, the last screen.
    // The timer display shows only while a turn with a clock runs. Returns
    // the round's phase afterwards.
    uint8_t setRoundPhase(uint32_t phase)
    {
        ee::Local<PlayerMessage> scoreMessage;
        ee::Local<zge::Matrix> spawnMatrix;
        ee::Local<PlayerMessage> trickMessage;
        ee::Local<uint32_t> turnFrames;

        const GameMode *const mode = gameModeRecord().get();
        RoundState &round = *g_round;

        if (round.phase == kTurnRunningPhase && phase == kTurnOverPhase)
        {
            // Time is up: a line still open ends here, unscored.
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (playerRides(static_cast<uint8_t>(player)) && playerRider(player)->score.lineState != 0)
                    trickLineEnd(player & 0xFF, 0);
            }
        }
        else if (round.phase == kTurnOverPhase && phase == kTurnStartPhase)
        {
            *g_currentPlayer = static_cast<uint8_t>(playerAfter(*g_currentPlayer));
            if (roundFlags(kRoundDoneFlag))
            {
                // A new round: who starts it, and its number.
                if ((mode->flags & kModeTurns) != 0 && (mode->flags & (kModeWinnerStarts | kModeAfterWinnerStarts)) != 0 &&
                    !roundFlags(kRoundDrawnFlag))
                {
                    if ((mode->flags & kModeAfterWinnerStarts) != 0)
                        round.firstPlayer = static_cast<uint8_t>(playerAfter(roundWinner(roundNumber(), 2)));
                    else
                        round.firstPlayer = roundWinner(roundNumber(), 2);
                }
                *g_currentPlayer = firstPlayer();
                if (!(mode->scoring == kScoreRoundsWon && roundFlags(kRoundDrawnFlag)))
                    ++round.number;
            }
        }

        round.phaseFramesLeft = 0;
        round.phase = static_cast<uint8_t>(phase);
        setTurnEndStage(0);

        switch (phase)
        {
        case kGameStartPhase:
            showCamera(*g_gameCamera, 1);
            showGameScreen(kScreenInstructions);
            if ((mode->flags & kModeMultiplayer) != 0)
            {
                *g_modalPlayerNum = kEveryPlayer;
            }
            else
            {
                setPlayPhase(0);
                *g_modalPlayerNum = *g_currentPlayer;
            }
            break;

        case kTurnStartPhase:
        {
            showGameScreen(kScreenPlay);
            setRoundFlags(kTurnEndedFlag, 0);
            setRoundFlags(kTurnAtRestFlag, 0);
            setRoundFlags(kLastLineFlag, 0);
            setRoundFlags(kTimeUpSoundFlag, 0);
            for (uint32_t kind = 0; kind < kRecordsPerSpan; ++kind)
            {
                ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), kind, kTurnRecords));
                if (record)
                {
                    record->marks = 0;
                    record->value = 0.0f;
                }
                record = gameRecord(recordKey(gameMode(), kind, kSavedRecords));
                if (record)
                    record->marks = 0;
            }

            bool firstTurn = true;
            if ((mode->flags & kModeTurns) != 0)
            {
                const uint8_t current = *g_currentPlayer;
                firstTurn = current == firstPlayer();
            }
            if (firstTurn)
            {
                // The round starts here: its records, every span's marks.
                setRoundFlags(kFirstTurnFlag, 1);
                for (uint32_t kind = 0; kind < kRecordsPerSpan; ++kind)
                {
                    const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), kind, kRoundRecords));
                    if (record)
                    {
                        record->marks = 0;
                        record->value = 0.0f;
                    }
                }
                for (uint32_t span = 0; span < kRecordSpans; ++span)
                {
                    for (uint32_t kind = 0; kind < kRecordsPerSpan; ++kind)
                    {
                        const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), kind, span));
                        if (record)
                            record->marks = 0;
                    }
                }
                if (gameMode() == kRandomSpawnMode)
                    drawRandomSpawn(spawnMatrix);
                setRoundFlags(kRoundDoneFlag, 0);
                setRoundFlags(kRoundDrawnFlag, 0);
                *g_keptLineCount = 0;
            }
            else
            {
                setRoundFlags(kFirstTurnFlag, 0);
            }

            restoreLevel();
            setUpLevelPickups();
            clearEffects();
            soundEvent(kTurnStartSound, 0, 0, 0, 0);
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (playerRides(static_cast<uint8_t>(player)))
                    startPlayerTurn(player, mode, scoreMessage);
                else
                    putRiderAway(player);
                // Every player's camera is put away until the turn runs.
                hidePlayerCamera(player);
            }
            showCamera(*g_gameCamera, 1);
            round.phaseFramesLeft = kTurnStartFrames;

            if ((mode->flags & kModeMultiplayer) == 0)
            {
                if (gameMode() != kChallengeMode)
                    return setRoundPhase(kTurnRunningPhase);
                showGameScreen(kScreenObjectives);
            }
            else
            {
                if ((mode->flags & kModeTurns) == 0)
                    return setRoundPhase(kTurnRunningPhase);
                showGameScreen(kScreenNextPlayer);
            }
            *g_modalPlayerNum = *g_currentPlayer;
            break;
        }

        case kTurnRunningPhase:
            showCamera(*g_gameCamera, 0);
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (playerRides(static_cast<uint8_t>(player)))
                    runPlayerTurn(player, mode, trickMessage);
            }
            turnFrames = static_cast<uint32_t>(ee::toInt(mode->turnSeconds * kFramesPerSecond));
            round.phaseFramesLeft = turnFrames;
            overlayMessage(kOverlayTimer, turnFrames);
            if ((mode->flags & kModeMultiplayer) == 0)
                setPlayPhase(1);
            setNpcPlay(kNpcPlayTurn);
            break;

        case kTurnOverPhase:
        {
            if ((mode->flags & kModeMultiplayer) == 0)
                setPlayPhase(0);
            setNpcPlay(kNpcPlayStopped);
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (playerRides(static_cast<uint8_t>(player)))
                    setReplayState((*g_playerReplays)[player], kReplayIdle);
            }
            *g_lastTurnReplay = (*g_playerReplays)[*g_currentPlayer];

            if (gameMode() == kHorseMode)
            {
                const ee::Ptr<rider::Rider> rider = playerRider(*g_currentPlayer);
                if (roundFlags(kFirstTurnFlag))
                {
                    if (rider->score.turnScore == ee::Float(0.0f))
                    {
                        // The player who sets the round's trick scored
                        // nothing: the round is drawn and the next sets it.
                        round.firstPlayer = static_cast<uint8_t>(playerAfter(round.firstPlayer));
                        setRoundFlags(kRoundDrawnFlag, 1);
                        setRoundFlags(kRoundDoneFlag, 1);
                        return setRoundPhase(kTurnStartPhase);
                    }
                    round.roundWinners[roundNumber()] = *g_currentPlayer;
                }
            }

            // The turn's score against the mode's record of each span, and
            // the judges' score.
            if (mode->scored != 0)
            {
                for (uint8_t player = 0; player < *g_playerCount; ++player)
                {
                    if (!playerRides(player))
                        continue;
                    const ee::Ptr<rider::Rider> rider = playerRider(player);
                    for (uint32_t span = kSavedRecords; span <= kRoundRecords; ++span)
                    {
                        const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), 1, span));
                        if (record && record->value < rider->score.turnScore)
                            setRecord(record, rider->score.turnScore, player);
                    }
                    judgeScore(player);
                }
            }

            // The game score takes the turn's, and goes against the mode's
            // other record of each span.
            if (mode->scoring != kScoreRoundsWon)
            {
                for (uint8_t player = 0; player < *g_playerCount; ++player)
                {
                    if (!playerRides(player))
                        continue;
                    const ee::Ptr<rider::Rider> rider = playerRider(player);
                    if (mode->scoring == kScoreTurnsSum)
                    {
                        const ee::Float sum = ee::toFloat(static_cast<int32_t>(rider->score.gameScore)) + rider->score.turnScore;
                        rider->score.gameScore = static_cast<uint32_t>(ee::toInt(sum));
                    }
                    else if (mode->scoring == kScoreBestTurn)
                    {
                        const ee::Float turn = rider->score.turnScore;
                        if (zge::unsignedToFloat(rider->score.gameScore) < turn)
                            rider->score.gameScore = static_cast<uint32_t>(ee::toInt(turn));
                    }
                }
                if (mode->scored != 0)
                {
                    for (uint8_t player = 0; player < *g_playerCount; ++player)
                    {
                        if (!playerRides(player))
                            continue;
                        const ee::Ptr<rider::Rider> rider = playerRider(player);
                        for (uint32_t span = kSavedRecords; span <= kRoundRecords; ++span)
                        {
                            const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), 0, span));
                            if (record && record->value < zge::unsignedToFloat(rider->score.gameScore))
                                setRecord(record, zge::unsignedToFloat(rider->score.gameScore), player);
                        }
                    }
                }
            }

            // Is the round done? Always for one rider and for players riding
            // at once; where they take turns, once the next is the first again.
            bool roundDone = true;
            if ((mode->flags & kModeMultiplayer) != 0 && (mode->flags & (kModeTurns | kModeRoundOfTurns)) != 0)
            {
                const uint32_t next = playerAfter(*g_currentPlayer);
                roundDone = next == firstPlayer();
            }
            if (roundDone)
                setRoundFlags(kRoundDoneFlag, 1);

            if (roundFlags(kRoundDoneFlag))
            {
                if (gameMode() == kHorseMode)
                {
                    // Every rider who scored nothing takes a letter; the
                    // leader has the fewest. The game ends with all but one
                    // out; the round is drawn unless someone took a letter.
                    uint32_t ridersOut = 0;
                    uint8_t fewest = static_cast<uint8_t>(zge::stringLength(sym::g_horseWord));
                    setRoundFlags(kRoundDrawnFlag, 1);
                    for (uint8_t player = 0; player < *g_playerCount; ++player)
                    {
                        const ee::Ptr<rider::Rider> rider = playerRider(player);
                        if (rider->score.turnScore == ee::Float(0.0f))
                        {
                            const uint32_t letters = ++rider->score.horseLetters;
                            if (letters >= static_cast<uint32_t>(zge::stringLength(sym::g_horseWord)))
                                ++ridersOut;
                            setRoundFlags(kRoundDrawnFlag, 0);
                        }
                        else if (rider->score.horseLetters < fewest)
                        {
                            round.leader = player;
                            fewest = static_cast<uint8_t>(rider->score.horseLetters);
                        }
                    }
                    setRoundFlags(kGameEndsFlag, ridersOut == static_cast<uint32_t>(*g_playerCount) - 1 ? 1 : 0);
                }
                else
                {
                    // The round's winner set the round's record, unless
                    // another rider's turn equals it: then it is drawn.
                    const ee::Ptr<GameRecord> best = gameRecord(recordKey(gameMode(), 1, kRoundRecords));
                    if (best)
                    {
                        setRoundFlags(kRoundDrawnFlag, 0);
                        for (uint32_t player = 0; player < *g_playerCount; ++player)
                        {
                            const ee::Ptr<rider::Rider> rider = playerRider(player);
                            if (player != best->player && rider->score.turnScore == best->value)
                                setRoundFlags(kRoundDrawnFlag, 1);
                        }
                        if (!roundFlags(kRoundDrawnFlag))
                        {
                            ++playerRider(best->player)->score.horseLetters;
                            round.roundWinners[roundNumber()] = best->player;
                        }
                    }
                    if (mode->scoring == kScoreRoundsWon)
                    {
                        if (!roundFlags(kRoundDrawnFlag))
                        {
                            // The leader has won the most rounds; a tie
                            // when another has won as many.
                            round.leader = 0;
                            setRoundFlags(kTieFlag, 0);
                            uint8_t most = 0;
                            for (uint32_t player = 0; player < *g_playerCount; ++player)
                            {
                                const ee::Ptr<rider::Rider> rider = playerRider(player);
                                const uint32_t won = rider->score.horseLetters;
                                if (most < won)
                                {
                                    most = static_cast<uint8_t>(won);
                                    round.leader = static_cast<uint8_t>(player);
                                    setRoundFlags(kTieFlag, 0);
                                }
                                else if (won == most)
                                {
                                    setRoundFlags(kTieFlag, 1);
                                }
                            }
                            const uint8_t number = roundNumber();
                            if (number == roundCount() - 1)
                                setRoundFlags(kGameEndsFlag, 1);
                        }
                    }
                    else
                    {
                        const uint8_t number = roundNumber();
                        if (number == roundCount() - 1)
                            setRoundFlags(kGameEndsFlag, 1);
                    }
                }

                if ((mode->flags & kModeMultiplayer) != 0)
                {
                    // The round's replay is the game's best turn so far: the
                    // record holder's replay changes places with it when the
                    // record was set in this turn, and in the first round.
                    const ee::Ptr<GameRecord> best = modeRecord(kGameRecords);
                    if ((best->marks & kRecordSet) != 0 || roundNumber() == 0)
                    {
                        const uint8_t kept = *g_roundReplay;
                        *g_roundReplay = (*g_playerReplays)[best->player];
                        (*g_playerReplays)[best->player] = kept;
                    }
                }
                giveTurnRewards(*g_currentPlayer);
            }

            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                putRiderAway(player);
                hidePlayerCamera(player);
                rider::riderOf(riderIndexOf(player));
                objectives::clearPlayerObjective(static_cast<uint8_t>(player));
            }
            showCamera(*g_gameCamera, 1);

            if (!roundFlags(kGameEndsFlag) && saveNewRecords())
            {
                showGameScreen(kScreenNewRecords);
                *g_modalPlayerNum = kEveryPlayer;
                return round.phase;
            }
            if (roundFlags(kRoundDoneFlag))
            {
                if (roundFlags(kGameEndsFlag))
                    return setRoundPhase(kGameOverPhase);
                if (gameMode() == kCompetitionMode)
                {
                    showGameScreen(kScreenLeaderboard);
                    *g_modalPlayerNum = *g_currentPlayer;
                    return round.phase;
                }
                if ((mode->flags & kModeMultiplayer) != 0 && mode->scoring == kScoreRoundsWon && roundFlags(kRoundDrawnFlag))
                    return setRoundPhase(kTurnStartPhase);
            }
            if ((mode->flags & kModeTurnKindMask) == (kModeMultiplayer | kModeTurns))
                return setRoundPhase(kTurnStartPhase);
            showGameScreen(kScreenAfterTurn);
            *g_modalPlayerNum = *g_currentPlayer;
            break;
        }

        case kGameOverPhase:
        {
            round.firstPlayer = 0;
            *g_currentPlayer = firstPlayer();
            if (mode->scoring == kScoreTurnsSum || mode->scoring == kScoreBestTurn)
            {
                // A tie: another rider's game score is the record's.
                const ee::Ptr<GameRecord> best = modeRecord(kGameRecords);
                setRoundFlags(kTieFlag, 0);
                for (uint32_t player = 0; player < *g_playerCount; ++player)
                {
                    const ee::Ptr<rider::Rider> rider = playerRider(player);
                    if (player != best->player && rider->score.gameScore == sce::libgcc::fptoui(best->value))
                        setRoundFlags(kTieFlag, 1);
                }
            }
            if (saveNewRecords())
            {
                showGameScreen(kScreenNewRecords);
                *g_modalPlayerNum = kEveryPlayer;
                return round.phase;
            }
            if ((mode->flags & kModeMultiplayer) == 0)
            {
                rider::riderOf(riderIndexOf(*g_currentPlayer));
                if (gameMode() == kChallengeMode)
                {
                    showGameScreen(kScreenReward);
                    *g_modalPlayerNum = *g_currentPlayer;
                    return round.phase;
                }
                if (gameMode() == kCompetitionMode)
                {
                    showGameScreen(kScreenLeaderboard);
                    *g_modalPlayerNum = *g_currentPlayer;
                    return round.phase;
                }
            }
            showGameScreen(kScreenGameOver);
            *g_modalPlayerNum = *g_playerCount > 1 ? kEveryPlayer : static_cast<int32_t>(*g_currentPlayer);
            break;
        }

        default:
            break;
        }

        showOverlayDisplay(kOverlayTimer, phase == kTurnRunningPhase && mode->turnSeconds != ee::Float(0.0f) ? 1 : 0);
        return round.phase;
    }
    READABLE_FUNCTION(setRoundPhase, Verified);

    // startGame: a new game of a mode: the round's state from the start, the
    // first player up, the riders' game scores and HORSE letters cleared, the
    // game's records cleared and every span's marks of the first three kinds,
    // a competition's field drawn, a multiplayer game tied, then the game's
    // first phase. Returns the mode.
    uint8_t startGame(uint32_t mode)
    {
        RoundState &round = *g_round;
        round.flags = 0;
        round.phase = kNoRoundPhase;
        round.firstPlayer = 0;
        *g_currentGameMode = static_cast<uint8_t>(mode);
        round.number = 0;
        *g_currentPlayer = firstPlayer();
        if (sessionHasOneRider())
        {
            clearGameScore(playerRider(0));
        }
        else
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
                clearGameScore(playerRider(player));
        }

        for (uint32_t kind = 0; kind < kRecordsPerSpan; ++kind)
        {
            for (uint32_t entry = 0; entry < kClearedRecordEntries; ++entry)
            {
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(mode, kind, kGameRecords, entry));
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
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(mode, 0, span, entry));
                if (record)
                    record->marks = 0;
            }
        }

        if (mode == kCompetitionMode)
        {
            kCompetitionField[0] = playerSetup(*g_currentPlayer)->character;
            for (uint32_t i = 1; i < kCompetitionRiders; ++i)
            {
                uint32_t character;
                do
                    character = zge::randomNumber() % kDrawnCharacters;
                while (inField(kCompetitionField, i, character));
                kCompetitionField[i] = static_cast<uint8_t>(character);
            }
        }

        setRoundFlags(kTieFlag, (gameModeRecord()->flags & kModeMultiplayer) != 0 ? 1 : 0);
        setRoundPhase(kGameStartPhase);
        restoreLevel();
        if ((gameModeRecord()->flags & kModeMultiplayer) == 0 && *g_currentLevel < kLevels)
            objectives::resetTurnGoals(*g_currentPlayer);
        return *g_currentGameMode;
    }
    READABLE_FUNCTION(startGame, Verified);
}
