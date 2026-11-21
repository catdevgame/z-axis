#pragma once
// DM2's level gaps (game.c): the named jumps of a level ("pretty much free
// gap", "eight-pack hop"), each from one tagged place to another. A trick
// line that goes from a gap's one end to its other takes the gap's points
// and shows its name.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    constexpr uint32_t kLevelGaps = 32;
    constexpr uint8_t kNoGap = 0xFF;

    // A gap of the level being played.
    struct LevelGap
    {
        uint32_t from;   // +0x00 the name hash of the faces' tag it starts on
        uint32_t to;     // +0x04 and of the tag it ends on
        uint32_t flags;  // +0x08 kGapOneWay
        uint32_t points; // +0x0C added to the score of the line that makes it
        char name[0x40]; // +0x10 shown with the line's tricks
    };
    static_assert(offsetof(LevelGap, name) == 0x10 && sizeof(LevelGap) == 0x50);
    constexpr uint32_t kGapOneWay = 0x1; // only from its start to its end (else either way)

    // The gaps of every built-in level counted (as the game starts).
    void countLevelGaps();
    // The level's gaps set up from its table (a built-in level), or counted
    // as they were set (the park editor's level). Always 1.
    uint32_t initLevelGaps();
    // The gaps' end as a level is left: nothing to give up (an empty
    // function, named by its place: it follows initLevelGaps in game.c and
    // the game's cleanup calls it where its set-up calls that).
    void cleanUpLevelGaps();
    // A gap of the level set (the park editor's gaps); returns the name's copy.
    uint32_t setLevelGap(uint32_t gap, const LevelGap *from);
    // The gap between two tags (name hashes), or kNoGap.
    uint32_t findGap(uint32_t from, uint32_t to);
    // A gap's name.
    ee::Ptr<char> gapName(uint32_t gap);
}
