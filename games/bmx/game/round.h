#pragma once
// BMX XXX's game modes and rounds (game.c, round.cpp): which mode is being
// played, and where the round it plays is.
//
// BMX XXX's line of DM2's game code (games/dm2/game/round.h), with the modes
// made one: a mode is an index of a table of mode records (g_gameModes:
// how many rounds it has, its flags), where DM2 keeps a multiplayer mode
// and a single-player mode apart. The round's state is one record
// (RoundState) where DM2 keeps bytes; it holds the game records too
// (records.h).
//
// Names come from the code's use and from DM2's readable pairs (no build
// names these functions); "(inferred)" marks a meaning read from use only.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/round.h.h"

namespace game::bmx
{
    // A game mode's record (0x14 bytes): its rounds, and its flags. The other
    // bytes are not read by anything written yet.
    struct GameMode
    {
        uint8_t rounds;   // +0x00 how many rounds the game has (roundCount)
        uint8_t scored;   // +0x01 (inferred) not 0: a turn's score is judged and its records are kept
        uint8_t scoring;  // +0x02 how a turn's score counts in the game (ModeScoring)
        uint8_t _03;
        uint32_t flags;   // +0x04 (kModeMultiplayer and the flags below it)
        ee::Float turnSeconds; // +0x08 a turn's time; 0: no clock
        ee::Float target;      // +0x0C kStripMode's: a record of the mode's own kind this high ends the turn (checkGameRecord)
        uint8_t _10[0x14 - 0x10];
    };
    static_assert(offsetof(GameMode, flags) == 0x04 && sizeof(GameMode) == 0x14);
    static_assert(offsetof(GameMode, scoring) == 0x02 && offsetof(GameMode, turnSeconds) == 0x08 && offsetof(GameMode, target) == 0x0C);
    // GameMode::flags: the players take turns (the whole round's replay is
    // shown at the game's end, the game starts tied): where DM2 tests for a
    // multiplayer mode.
    constexpr uint32_t kModeMultiplayer = 0x2;
    // The players ride one after another, a turn each; without it every
    // player of a multiplayer mode rides at once, on a part of the screen.
    constexpr uint32_t kModeTurns = 0x4;
    // (inferred) With kModeTurns: a round is over once every player has had
    // a turn (setRoundPhase tests the two together).
    constexpr uint32_t kModeRoundOfTurns = 0x8;
    // Who starts the next round: its winner, or the player after its winner.
    constexpr uint32_t kModeWinnerStarts = 0x80;
    constexpr uint32_t kModeAfterWinnerStarts = 0x100;
    // Bit 0 is not read by name yet: setRoundPhase goes from a turn's end to
    // the next turn at once only for a turn-taking multiplayer mode without it.
    constexpr uint32_t kModeTurnKindMask = 0x7;

    // GameMode::scoring: what a rider's game score is.
    enum ModeScoring : uint8_t
    {
        kScoreTurnsSum = 1,  // the sum of the turns' scores
        kScoreRoundsWon = 2, // the rounds won (Rider horseLetters counts them)
        kScoreBestTurn = 3,  // the best turn's score
    };

    // The mode being played: an index of g_gameModes (a byte), named by the
    // game's own mode names ("KSTRMP_MODENAME_MODE<n>"; the quotes are its
    // descriptions, "KSTRMP_DESC_MODE<n>"). Mode 1 is the competition: two
    // rounds, a field of riders drawn at the game's start.
    inline ee::Global<uint8_t, sym::g_currentGameMode> g_currentGameMode;
    constexpr uint8_t kChallengeMode = 0;   // CHALLENGE: the one-rider game of a level's goals
    constexpr uint8_t kCompetitionMode = 1; // COMPETITION
    constexpr uint8_t kSessionMode = 2;     // SESSION
    constexpr uint8_t kFreeRideMode = 3;    // FREE RIDE
    constexpr uint8_t kSkillzMode = 4;      // SKILLZ: "the highest score over a two minute run"
    constexpr uint8_t kHighFiveMode = 5;    // HIGH FIVE: "a best-of-five series ... the biggest single trick in 30 seconds"
    constexpr uint8_t kHorseMode = 6;       // B-M-X: the word game, a rider who scores nothing takes a letter
    constexpr uint8_t kRandomSpawnMode = 7; // RANDOM SPOT: each round starts at a place drawn at random (DM2's mode 4)
    constexpr uint8_t kWipeoutMode = 8;     // WIPEOUT: "the most bodily harm in a single crash"
    // 9 .. 16: the longest grind, highest wall tap, big air, huge combo,
    // super spin, furthest jump, longest manual and longest nose wheelie
    // contests ("three chances").
    constexpr uint8_t kPaintballMode = 17;  // PAINTBALL: "collect all the boomboxes before your opponent snipes you"
    // STRIP CHALLENGE: "each time you break the record, your opponent loses
    // an article of clothing": a record of the mode's own kind that reaches
    // GameMode::target decides the turn against the other player.
    constexpr uint8_t kStripMode = 18;
    constexpr uint8_t kGhostRideMode = 19;  // GHOSTRIDE DUEL: "the first to take out 10 pedestrians by ghost riding a bike"
    constexpr uint8_t kCompetitionRuns = 2;

    // The mode being played, and its record.
    uint8_t gameMode();
    ee::Ptr<GameMode> gameModeRecord();

    // A record (records.h): 8 bytes; the round state holds the spans.
    struct GameRecord
    {
        ee::Float value;
        uint8_t player;
        uint8_t marks; // kRecordSet, kRecordAnnounced (DM2's)
        uint8_t _06[2];
    };
    static_assert(offsetof(GameRecord, player) == 0x04 && offsetof(GameRecord, marks) == 0x05 && sizeof(GameRecord) == 0x08);
    constexpr uint32_t kRecordSpans = 4;
    constexpr uint32_t kRecordsPerSpan = 14;

    // The round being played: its flags, its phase, its number, the player
    // who starts it, and the game records of its four spans. The bytes not
    // named are not read by anything written yet.
    constexpr uint32_t kRoundWinners = 15;
    struct RoundState
    {
        uint32_t flags;       // +0x00 (setRoundFlags; a word where DM2 keeps a byte)
        uint32_t phaseFramesLeft; // +0x04 the phase's clock in frames (the timer display shows it; DM2 g_phaseFramesLeft)
        uint32_t turnEndFrames;   // +0x08 frames in the turn's end stage (cleared with it)
        uint8_t phase;        // +0x0C (setRoundPhase's; kGameOverPhase its last)
        uint8_t turnEndStage; // +0x0D how far the running turn's end is (setTurnEndStage; DM2 g_turnEndStage)
        uint8_t number;       // +0x0E from 0
        uint8_t firstPlayer;  // +0x0F
        uint8_t decidedPlayer; // +0x10 set with kTurnDecided: the player whose rider has kFlagDowned (mode 18's record check: the other player)
        uint8_t _11[3];
        GameRecord records[kRecordSpans][kRecordsPerSpan]; // +0x14
        uint8_t roundWinners[kRoundWinners]; // +0x1D4 the player who won each round (roundWinner)
        uint8_t leader;       // +0x1E3 the game's leader: HORSE's rider with the fewest letters, the most rounds won
        char horseWord[12];   // +0x1E4 the HORSE word (setHorseWord)
    };
    static_assert(offsetof(RoundState, phaseFramesLeft) == 0x04 && offsetof(RoundState, turnEndStage) == 0x0D);
    static_assert(offsetof(RoundState, decidedPlayer) == 0x10);
    static_assert(offsetof(RoundState, roundWinners) == 0x1D4 && offsetof(RoundState, leader) == 0x1E3);
    static_assert(offsetof(RoundState, horseWord) == 0x1E4);
    static_assert(offsetof(RoundState, phase) == 0x0C && offsetof(RoundState, number) == 0x0E);
    static_assert(offsetof(RoundState, firstPlayer) == 0x0F && offsetof(RoundState, records) == 0x14);
    inline ee::Global<RoundState, sym::g_round> g_round;

    // The round's phase: its last is the game's end (the whole round's replay
    // is shown then). No phase (0xFF) stands before a game's first.
    uint8_t roundPhase();
    constexpr uint8_t kGameOverPhase = 5;
    constexpr uint8_t kNoRoundPhase = 0xFF;
    // The phases before the last, DM2's without its free-ride and exit phases:
    // the game's start (the camera on the level, the first screen), a turn's
    // start (records cleared, riders put at the start), the turn running
    // (recorders started, the clock set) and the turn over (scores, records,
    // the round's winner).
    constexpr uint8_t kGameStartPhase = 1;
    constexpr uint8_t kTurnStartPhase = 2;
    constexpr uint8_t kTurnRunningPhase = 3;
    constexpr uint8_t kTurnOverPhase = 4;

    // A rider's health (Rider::health) at the spawn, and the most a health
    // pickup takes it to.
    constexpr uint8_t kFullHealth = 0xFF;

    // The round's flags (setRoundFlags, roundFlags), DM2's first five.
    constexpr uint32_t kGameEndsFlag = 0x01;    // this round is the game's last
    constexpr uint32_t kRoundDoneFlag = 0x02;   // every player has had the round's turn
    constexpr uint32_t kFirstTurnFlag = 0x04;   // the turn is the first player's
    constexpr uint32_t kTieFlag = 0x08;         // the game's lead is shared
    constexpr uint32_t kRoundDrawnFlag = 0x10;  // no one won the round: it does not count
    // BMX XXX's own, cleared as a turn starts and set by the modes' turn-end
    // frames (DM2 keeps the same steps in g_turnEndStage alone):
    constexpr uint32_t kTurnAtRestFlag = 0x20;   // the turn's line is in: tricks are no longer scored
    constexpr uint32_t kTimeUpSoundFlag = 0x40;  // the clock's last count has sounded
    constexpr uint32_t kLastLineFlag = 0x80;     // time is out and the open line rides on: no clock
    constexpr uint32_t kTurnEndedFlag = 0x100;   // the turn is over (the game's frame goes to kTurnOverPhase)
    // Set when a goal's reward is a movie (updateGoal): the game's frame
    // pauses for the movie's confirmation screen.
    constexpr uint32_t kMovieWonFlag = 0x200;

    // Whether the player rides in the turn being played: the current player
    // alone where the players of a multiplayer mode take turns, else everyone.
    bool playerRides(uint8_t player);

    // The round goes to a phase (the phase machine: see round.cpp); returns
    // the phase the round is in afterwards.
    uint8_t setRoundPhase(uint32_t phase);
    // A player's rider put at its start for a turn (shown: with its rows and
    // position set at the mode's locator, and a first rider frame run), its
    // weapon set up and its overlay displays cleared.
    void spawnPlayer(uint8_t player, uint32_t shown);
    // The running turn's end stage set, its frame count cleared.
    void setTurnEndStage(uint8_t stage);
    // How far a running turn's end has got (RoundState::turnEndStage; DM2's
    // stages, and one of BMX XXX's own).
    enum TurnEndStage : uint8_t
    {
        kTurnRiding = 0,    // until the time is out, or a line is begun
        kTurnLineBegun = 1, // the time is out (a one-line turn: its line is open)
        kTurnLastLine = 2,  // waiting for the open line to end with the rider riding
        kTurnAtRest = 3,    // the line is in: kTurnRestFrames more frames
        kTurnDecided = 4,   // (inferred) decided before its time (decidedPlayer): kTurnDecidedFrames more frames
        kTurnEnded = 5,
    };
    constexpr uint32_t kTurnRestFrames = 30;
    constexpr uint32_t kTurnDecidedFrames = 240;
    // A new game of a mode, from its first phase; returns the mode.
    uint8_t startGame(uint32_t mode);

    // The player who won a round; `rounds` below 2 asks for the game's leader
    // instead (the callers written so far pass 2).
    uint8_t roundWinner(uint32_t round, uint32_t rounds);

    // The HORSE word: setting and reading it.
    ee::Ptr<char> setHorseWord(ee::Ptr<const char> word);
    ee::Ptr<char> horseWord();

    // The rounds a game has: a competition's two runs, else its mode's.
    uint8_t roundCount();
    // The round being played, from 0.
    uint8_t roundNumber();
    // The player who starts each round.
    uint8_t firstPlayer();

    // The replay the game shows (a recorder slot).
    uint8_t shownReplay();

    // The round's flags: set or clear `flags` (returns `on`), and whether any
    // of `flags` is set.
    uint32_t setRoundFlags(uint32_t flags, uint32_t on);
    bool roundFlags(uint32_t flags);
}
