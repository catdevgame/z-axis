#pragma once
// DM2's game modes and rounds (game.c): which game is being played, and where
// the round it plays is (round.cpp).
//
// A game is a single-player mode (multiplayerMode 0, with its
// singlePlayerMode) or one of the multiplayer modes (1 .. 14, the
// "$KSTRMP_INST_MODE%02d" instructions). A game is played in rounds: the
// players take their turns one after another (the current player), and a
// round ends once each has had one; how many rounds a game has is
// roundCount. Each turn runs through the round phases (setRoundPhase).
//
// Names come from the code's use and the strings its menus show (no build
// names these functions); "(inferred)" marks a meaning read from use only.

#include "ee/ee.h"

#include <cstdint>

namespace game::dm2
{
    // A single-player game (singlePlayerMode), by the instructions the pause
    // menu shows for it ("$KSTRSP_INST_LEV%02d", "_COMP_LEV%02d",
    // "_SESSION_LEV%02d", "_FREE", "_TEST").
    enum SinglePlayerMode : uint8_t
    {
        kLevelGoals = 0,   // a ProQuest level's goals
        kCompetition = 1,  // a level's competition (two runs)
        kSession = 2,      // a timed session on a level
        kFreeRide = 3,
        kParkTest = 4,     // the park editor's park, ridden
    };

    // The multiplayer mode: 0 for a single-player game.
    uint8_t multiplayerMode();
    // The single-player mode (a SinglePlayerMode), and setting it.
    uint8_t singlePlayerMode();
    uint8_t setSinglePlayerMode(uint8_t mode);

    // The rounds a game has: by multiplayer mode, a competition's two runs.
    uint8_t roundCount();
    // The round being played, from 0 ("$KSTR_ROUND %ld" shows it plus one).
    uint8_t roundNumber();
    // The player who starts each round (the current player goes back to them
    // after a round).
    uint8_t firstPlayer();
    // The round's phase (setRoundPhase sets it). A game starts at
    // kGameStartPhase; each turn then runs from kTurnStartPhase to
    // kTurnOverPhase, which goes on to the next turn or to the game's end.
    enum RoundPhase : uint8_t
    {
        kGameStartPhase = 1,   // the camera at a level point, the instructions screen
        kTurnStartPhase = 3,   // the turn's records cleared, its rider spawned, the objectives or ready screen
        kFreeTurnPhase = 4,    // a free ride's or park test's turn: the replays record, no clock
        kTurnRunningPhase = 5, // the turn: the replays record, the clock runs (the timer shows only here)
        kExitPhase = 6,        // the rider rode into the level's exit: a white fade of 30 frames, then the turn is over
        kTurnOverPhase = 7,    // time up: the scores, the records, the round's winner; what follows
        kGameOverPhase = 8,    // the game's end (the records, the game over menus)
        kNoRoundPhase = 0xFF,  // a game's start sets it before the first
    };
    uint8_t roundPhase();
    // A phase entered, with all it does; returns the phase the round is in
    // after it (a phase may go straight on to the next).
    uint8_t setRoundPhase(uint32_t phase);

    // The multiplayer modes the round's code tells apart (inferred from what
    // it does for each; the menus' "$KSTRMP_INST_MODE%02d" texts name them).
    constexpr uint8_t kSeriesMode = 2;       // rounds won decide the game ("$KSTR_SERIES_TIE" when they are level)
    constexpr uint8_t kHorseMode = 3;        // HORSE: match the first player's turn or take a letter
    constexpr uint8_t kRandomSpawnMode = 4;  // each round starts at a place drawn at random
    constexpr uint8_t kFirstBestTurnMode = 5, kLastBestTurnMode = 13; // the best single turn is the game score
    constexpr uint8_t kAllAtOnceMode = 14;   // every player rides every turn

    // The round's flags (setRoundFlags, roundFlags).
    constexpr uint8_t kGameEndsFlag = 0x01;   // this round is the game's last
    constexpr uint8_t kRoundDoneFlag = 0x02;  // every player has had the round's turn
    constexpr uint8_t kFirstTurnFlag = 0x04;  // the turn is the round's first (the first player's)
    constexpr uint8_t kTieFlag = 0x08;        // the game's lead is shared (the game over menus show
                                              // "$KSTR_SERIES_TIE" for it); a multiplayer game starts tied
    constexpr uint8_t kRoundDrawnFlag = 0x10; // no one won the round (HORSE: no one took a letter, or the
                                              // first player scored nothing): modes 1 .. 4 do not count it

    // The turn ended before its time (the exit confirmation).
    void endTurnEarly();

    // The player who won a round; with fewer than two rounds asked for, the
    // game's leader (HORSE: the fewest letters; a series: the most rounds).
    uint8_t roundWinner(uint32_t round, uint32_t rounds);
    // Called for each player whose turn starts; empty in this build (it sits
    // with the level goals' code).
    void turnStartHook(uint32_t player);

    // The replay the game shows (a recorder slot).
    uint8_t shownReplay();

    // A new game of a multiplayer mode (0: the single-player mode set), from
    // its first phase; returns the mode.
    uint8_t startGame(uint32_t mode);

    // The game's set-up as a level starts: its players' replays, its records
    // from the saved ones, the level's gaps and goals, the game task.
    void gameSetup();

    // The game's end as a level is left: the game task, the goals' material
    // and the replays given up.
    void gameCleanup();
    // The game's frame for the riders (game_frame.cpp): their scoring and
    // goals, the level's exit, the turn's end and the clock. Returns the
    // round's phase.
    uint8_t gameRiderFrame();

    // The HORSE word (multiplayer mode 3; "BMX" until set): setting and reading it.
    ee::Ptr<char> setHorseWord(ee::Ptr<const char> word);
    ee::Ptr<char> horseWord();

    // A player's rider put at the level's start for its turn (shown: reset as
    // shown, with its turn's goals and overlay labels cleared).
    void spawnPlayer(uint32_t player, uint32_t shown);

    // The round's flags: set or clear `flags` (returns `on`), and whether any
    // of `flags` is set.
    uint32_t setRoundFlags(uint8_t flags, uint32_t on);
    bool roundFlags(uint8_t flags);
}
