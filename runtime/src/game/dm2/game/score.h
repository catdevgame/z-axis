#pragma once
// DM2's trick scoring (game.c): a turn's trick lines and the tricks chained
// under them, in the rider's score block (rider::ScoreBlock).

#include "dm2/rider/rider.h"
#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/game/score.h.h"

namespace game::dm2
{
    // The tricks by number (rider::Rider::trick): the air tricks below
    // kFirstGrind, then the grinds, the stalls and lip tricks, the held
    // tricks, the manual and the nose wheelie.
    constexpr uint8_t kFirstGrind = 0x33, kFirstStall = 0x3C, kFirstHeldTrick = 0x43;
    constexpr uint8_t kManual = 0x46, kNoseWheelie = 0x47, kTricksEnd = 0x48;

    // The kinds the turn's tricks are counted by (rider::ScoreBlock::cellTrickCounts).
    enum TrickCountKind : uint32_t
    {
        kCountAirTricks = 0,
        kCountModifiers = 1,
        kCountModifiedTricks = 2, // a modifier on a trick
        kCountGrinds = 3,
        kCountLipTricks = 4,      // the stalls and lip tricks
        kCountWheelies = 5,
        kCountBails = 6,
    };
    // The wheel contact kinds a landing tells apart (rider.h's contact
    // kinds): a rail's edge, an object, and three the goals and the line's
    // gap give up on.
    constexpr uint8_t kContactRailEdge = 5;
    constexpr uint8_t kContactKind6 = 6, kContactKind9 = 9, kContactKind10 = 10;
    // What a wheel's contact is reduced to beside its tag: an edge's tag
    // word, a face's tag word, an object's name hash.
    constexpr uint32_t kTaggedEdge = 5, kTaggedFace = 6, kTaggedObject = 8;

    // The level's cells (its quarters in x by its quarters in z) the
    // counts are kept in; the counts' last row is the whole level's.
    constexpr uint32_t kCells = 16;

    // The state of the turn's current trick line (rider::ScoreBlock::lineState).
    enum LineState : uint8_t
    {
        kLineNone = 0,     // nothing to score yet
        kLineMoveSeen = 1, // the rider has a move to score: a line opens when there is room
        kLineOpen = 2,     // a line is open and waits for its next trick
        kLineTrick = 3,    // the line's last trick is the rider's current one and follows it
        kLineSpin = 4,     // the trick is over; the air's spin still counts
    };

    // A rider's scoring for the frame: the current line's state moved on
    // by what the rider does, the tricks recorded, the score, the ruler
    // and the gauge sent to the overlay.
    void trickScoreFrame(uint32_t player);

    // A new, empty line opened at the end of the turn's lines: it is the
    // current one from here on.
    void openScoreLine(rider::ScoreBlock *score);
    // A new, empty trick taken from the turn's tricks and chained at the
    // end of the current line.
    void newScoreTrick(rider::ScoreBlock *score);
    // While the player's rider is in the air: when the face its back wheel
    // last touched and the face of its contact are the two ends of one of
    // the level's gaps, the turn's current line takes the gap (a line is
    // opened for it, and the trick display told, when the turn has none).
    void markAirGap(uint32_t player);
    // Whether the rider's air has something to score: a spin of half a
    // turn or more, an action taken in the air, or a trick. 1 or 0.
    uint32_t riderHasMoveToScore(const rider::Rider *rider);
    // What the rider's code tells the scoring as a rider lands (`landed`
    // not 0): the level distance between the wheels' two contacts goes to
    // the record check, the turn's current line takes how straight the
    // landing was and the gap between the contacts' tags, and the trick
    // display writes the line's text. Then the level's goals take it.
    void trickEvent(uint32_t player, uint32_t landed);
    // The turn's lines scored: each trick's points, each line's, the three
    // distance sums (grinds, wheelies, nose wheelies) and the points of the
    // whole run, which the trick display shows. A run that ends in a grind
    // or a wheelie sends its distance to the ruler.
    void scoreLines(rider::ScoreBlock *score);
    // The player's run ended. Landed: the turn's counts of each trick and
    // of tricks by kind in the level's cell, the character's landed moves
    // and found gaps, the turn's score by the game's mode (HORSE keeps the
    // first player's line or matches it), the trick records; lost in a
    // bail: the bail counted. Either way the run's lines are cleared.
    void trickLineEnd(uint32_t player, uint32_t landed);

    // HORSE's line to match: the first player's first line, kept as the
    // turn ends (score_end.cpp), and how many are kept (0 or 1; the count
    // sits after the one line).
    inline ee::Global<rider::ScoreLine[1], sym::g_keptLines> g_keptLines;
    inline ee::Global<uint8_t, sym::g_keptLineCount> g_keptLineCount;
}
