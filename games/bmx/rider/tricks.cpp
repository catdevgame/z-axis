// BMX XXX's trick lookups by the rider's direction taps: a grind's, a
// stall's, a lip's and an air trick, a tap's trick code and the trick of
// the last two taps. DM2's (games/dm2/rider/tricks.cpp); the tables are BMX
// XXX's own, and no lookup asks whether the rider may do the trick (DM2
// skips the tricks its rider has off).

#include "rider.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "sym/bmx/rider/tricks.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The tricks by the last two taps (previous, last; row 0: one tap,
        // or the stick), Direction by Direction.
        inline ee::Global<uint8_t[9][9], sym::g_grindTricks> g_grindTricks;
        inline ee::Global<uint8_t[9][9], sym::g_stallTricks> g_stallTricks;
        // The air tricks by direction: row 0 one direction, row p two (p,
        // then the last); column d - 1 for direction d.
        inline ee::Global<uint8_t[9][8], sym::g_airTricks> g_airTricks;
        // The flatland's tricks the same way, one table a trick button (A, B).
        inline ee::Global<uint8_t[2][9][8], sym::g_flatlandTricks> g_flatlandTricks;
        // An air trick's tap sequence: `length` taps (up to three) ending
        // with the rider's last ones.
        struct TapSequence
        {
            uint8_t length;
            uint8_t taps[3];
            uint8_t trick;
            uint8_t motion; // into Rider::trickMotion
        };
        static_assert(sizeof(TapSequence) == 6);
        constexpr uint32_t kSequenceGroups = 17; // DM2 21
        inline ee::Global<TapSequence[kSequenceGroups][4], sym::g_tapSequences> g_tapSequences;

        // The grind and stall lookups: the last two taps' trick, else the
        // last tap's, else the stick's.
        uint8_t trickByTaps(const Rider *rider, const uint8_t (&table)[9][9])
        {
            const uint8_t count = rider->tapCount;
            uint8_t trick = kNoTrick;
            if (count >= 2)
                trick = table[rider->taps[count - 2]][rider->taps[count - 1]];
            if (trick != kNoTrick)
                return trick;
            if (count != 0)
                trick = table[0][rider->taps[rider->tapCount - 1]];
            if (trick != kNoTrick)
                return trick;
            return table[0][stickDirection(rider)];
        }

        // Whether the rider's last taps are `sequence`'s (airTrickByTaps'
        // inner loop).
        bool tapsEnd(const Rider *rider, uint8_t count, const TapSequence &sequence)
        {
            if (sequence.length == 0)
                return false;
            if (count < sequence.length)
                return false;
            const uint8_t *tap = &rider->taps[rider->tapCount - sequence.length];
            const uint8_t *wanted = sequence.taps;
            uint32_t matched = 0;
            if (tap[0] == wanted[0])
            {
                matched = 1;
                while (matched < sequence.length && tap[matched] == wanted[matched])
                    ++matched;
            }
            return matched == sequence.length;
        }

        // A lip's trick by one direction.
        inline ee::Global<uint8_t[9], sym::g_lipTricks> g_lipTricks;
        // A tap's trick code (0x57 and 0x58: the rider's own two tricks).
        inline ee::Global<uint8_t[9], sym::g_tapTricks> g_tapTricks;
        constexpr uint8_t kOwnTrick = 0x57;
        // The trick of two tap codes (previous, last).
        inline ee::Global<uint8_t[86][86], sym::g_trickCombinations> g_trickCombinations;
    }

    // The grind trick of the rider's taps (or the stick).
    uint8_t grindTrick(const Rider *rider)
    {
        return trickByTaps(rider, *g_grindTricks);
    }
    READABLE_FUNCTION(grindTrick, Verified);

    // The stall trick of the rider's taps (or the stick).
    uint8_t stallTrick(const Rider *rider)
    {
        return trickByTaps(rider, *g_stallTricks);
    }
    READABLE_FUNCTION(stallTrick, Verified);

    // A lip's trick: the last tap's direction, or the stick's (kNoTrick:
    // none).
    uint8_t lipTrick(const Rider *rider)
    {
        const uint8_t count = rider->tapCount;
        if (count != 0)
            return (*g_lipTricks)[rider->taps[count - 1]];
        return (*g_lipTricks)[stickDirection(rider)];
    }
    READABLE_FUNCTION(lipTrick, Verified);

    // The air trick of the rider's taps: a tap sequence's (setting the
    // trick's motion), else the last two directions', else the last one's;
    // kNoTrick for none.
    uint8_t airTrickByTaps(Rider *rider)
    {
        const uint8_t count = rider->tapCount;
        uint8_t trick = kNoTrick;
        if (count == 0)
            return trick;
        for (uint32_t group = 0; group < kSequenceGroups && trick == kNoTrick; ++group)
        {
            const TapSequence *sequences = (*g_tapSequences)[group];
            for (uint32_t entry = 0; entry < 4; ++entry)
            {
                if (tapsEnd(rider, count, sequences[entry]))
                {
                    trick = sequences[entry].trick;
                    rider->trickMotion = sequences[entry].motion;
                    break;
                }
            }
        }

        const uint8_t *airTricks = &(*g_airTricks)[0][0];
        if (count >= 2)
        {
            if (trick != kNoTrick)
                return trick;
            const uint8_t *end = &rider->taps[rider->tapCount];
            trick = airTricks[end[-2] * 8 + end[-1] - 1];
        }
        if (trick != kNoTrick)
            return trick;
        return airTricks[rider->taps[rider->tapCount - 1] - 1];
    }
    READABLE_FUNCTION(airTrickByTaps, Verified);

    // A tap's trick code; the rider's own tricks for 0x57 and 0x58.
    uint8_t tapTrick(const Rider *rider, uint32_t tap)
    {
        const uint8_t code = (&(*g_tapTricks)[0])[tap];
        if (code < kOwnTrick || code == kNoTrick)
            return code;
        return rider->ownTricks[code - kOwnTrick];
    }
    READABLE_FUNCTION(tapTrick, Verified);

    // The trick of the rider's taps: the combination of the last two taps'
    // codes, else the last tap's.
    uint8_t trickFromTaps(const Rider *rider)
    {
        if (rider->tapCount == 0)
            return (*g_tapTricks)[0];
        if (rider->tapCount >= 2)
        {
            const uint8_t last = tapTrick(rider, rider->taps[rider->tapCount - 1]);
            const uint8_t previous = tapTrick(rider, rider->taps[rider->tapCount - 2]);
            const uint8_t trick = (&(*g_trickCombinations)[0][0])[previous * 86 + last];
            if (trick != kNoTrick)
                return trick;
        }
        return tapTrick(rider, rider->taps[rider->tapCount - 1]);
    }
    READABLE_FUNCTION(trickFromTaps, Verified);

    // The flatland trick of the rider's taps for a trick button (0 A, 1
    // B): the last two directions', else the last one's; kNoTrick with no
    // taps.
    uint8_t flatlandTrickByTaps(const Rider *rider, uint32_t button)
    {
        const uint8_t count = rider->tapCount;
        if (count == 0)
            return kNoTrick;
        const uint8_t *tricks = &(*g_flatlandTricks)[button][0][0];
        uint8_t trick = kNoTrick;
        if (count >= 2)
            trick = tricks[rider->taps[count - 2] * 8 + rider->taps[count - 1] - 1];
        if (trick != kNoTrick)
            return trick;
        return tricks[rider->taps[rider->tapCount - 1] - 1];
    }
    READABLE_FUNCTION(flatlandTrickByTaps, Verified);

    // The frames between two of the stick's taps (their places in the
    // taps' list), from the taps' times: bytes that wrap.
    uint32_t tapFramesApart(const Rider *rider, uint32_t later, uint32_t earlier)
    {
        uint32_t time = rider->tapTimes[later];
        const uint32_t from = rider->tapTimes[earlier];
        if (time < from)
            time += 0x100;
        return time - from;
    }
    READABLE_FUNCTION(tapFramesApart, Verified);
}
