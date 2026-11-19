#pragma once
// BMX XXX's turn score records (game.c; score.cpp): the rider's score block,
// the part of the rider record game.c's scoring functions take the address
// of (Rider::score, rider.h's ScoreBlock: DM2's with room for 128 lines and
// 256 tricks).

#include "bmx/rider/rider.h"
#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    using rider::ScoreBlock;
    using rider::ScoreLine;
    using rider::ScoreTrick;

    // The rider's flags the scoring reads, named for what DM2's scoring
    // does with them (rider.h names the rest): the alignment turns the air
    // (its yaw is no spin), an action was taken in the air, and the air
    // began rolling backwards (a spin after it marks the trick).
    constexpr rider::RiderFlag kFlagAlignAbout{2, 0x40};
    constexpr rider::RiderFlag kFlagActed = rider::kFlagActed;
    constexpr rider::RiderFlag kFlagTakeoffBack = rider::kFlagTakeoffBack;

    // The state of the turn's current trick line (ScoreBlock::lineState;
    // DM2's).
    enum LineState : uint8_t
    {
        kLineNone = 0,     // nothing to score yet
        kLineMoveSeen = 1, // the rider has a move to score: a line opens when there is room
        kLineOpen = 2,     // a line is open and waits for its next trick
        kLineTrick = 3,    // the line's last trick is the rider's current one and follows it
        kLineSpin = 4,     // the trick is over; the air's spin still counts
    };

    // What the turn's tricks are counted by (ScoreBlock::cellTrickCounts,
    // DM2's kinds with BMX XXX's types), in each of the level's 16 cells and
    // in all (the 17th row).
    enum TrickCountKind : uint32_t
    {
        kCountAirTricks = 0,
        kCountModifiers = 1,
        kCountModifiedTricks = 2, // a modifier on a trick
        kCountGrinds = 3,
        kCountStallsAndTaps = 4,
        kCountManuals = 5,
        kCountBails = 6,
    };
    constexpr uint32_t kLevelCells = 16;

    // A new, empty line opened after the block's last.
    void openScoreLine(ee::Ptr<ScoreBlock> score);
    // A new, empty trick added at the end of the block's last line.
    void addScoreTrick(ee::Ptr<ScoreBlock> score);

    // A player's scoring for the frame (score_frame.cpp): the mode's own,
    // the score display, the air's height and the skid's length.
    void playerScoreFrame(uint32_t player);
    // The trick line's scoring (most modes' own): the current line's state
    // moved on by what the rider does, its tricks followed, the trick and
    // balance displays told.
    void trickScoreFrame(uint32_t player);
    // The run of lines ends, landed (1) or lost in a bail (0): its tricks
    // counted, the turn scored, the records checked, the lines cleared
    // (score_end.cpp).
    void trickLineEnd(uint32_t player, uint32_t landed);
    // Every trick and line of the player's turn scored, and the run's
    // points (score_points.cpp).
    void scoreLines(uint32_t player);
    // The wipeout's and the paintball's own scoring.
    void wipeoutScoreFrame(uint32_t player);
    void paintballScoreFrame(uint32_t player);
    // Whether the rider has a move to score: half a turn of spin or more
    // (not while the alignment turns it), an action, a trick or a trick
    // motion (DM2's).
    uint32_t riderHasMoveToScore(const rider::Rider *rider);
    // The crowd and the path men react to a line that ends: landed, a
    // cheer by its points over 4000; else a bail's groan.
    int32_t lineReaction(uint32_t landed, ee::Ptr<ScoreBlock> score);
    // A trick line's air ends, landed or in a bail (the air state's): a
    // landing's wheels' contacts, the jump's distance against its record,
    // the landing graded, the lines scored, the trick display told; then the
    // objectives (score.cpp).
    void trickLineEvent(uint32_t player, uint32_t landed);
}
