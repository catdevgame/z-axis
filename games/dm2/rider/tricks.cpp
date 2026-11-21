// DM2's trick lookups: the trick a rider's direction taps (or the stick,
// when there are none) ask for, in the grind, stall, lip and air tables
// (physics.c's end).
//
// Counterparts: DM2 GameCube and Xbox
// (high), BMX XXX PS2 (lipTrick, tapTrick and trickFromTaps high).

#include "rider.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "sym/dm2/rider/tricks.cpp.h"

namespace game::dm2::rider
{
    uint8_t stickDirection(const Rider *rider); // input.cpp

    namespace
    {
        // The tricks by the last two taps (previous, last; row 0: one tap,
        // or the stick), Direction by Direction.
        inline ee::Global<uint8_t[9][9], sym::g_grindTricks> g_grindTricks;
        inline ee::Global<uint8_t[9][9], sym::g_stallTricks> g_stallTricks;
        // A lip's trick by one direction (0x45: the lip trick at a quarter
        // pipe's top; setActionState kActionLipTrick).
        inline ee::Global<uint8_t[9], sym::g_lipTricks> g_lipTricks;
        // The air tricks by direction: row 0 one direction, row p two (p,
        // then the last); column d - 1 for direction d.
        inline ee::Global<uint8_t[9][8], sym::g_airTricks> g_airTricks;
        // A tap's trick code (0x57 and 0x58: the rider's own two tricks).
        inline ee::Global<uint8_t[9], sym::g_tapTricks> g_tapTricks;
        constexpr uint8_t kOwnTrick = 0x57;
        // The trick of two tap codes (previous, last).
        inline ee::Global<uint8_t[86][86], sym::g_trickCombinations> g_trickCombinations;

        // An air trick's tap sequence: `length` taps (up to three) ending
        // with the rider's last ones.
        struct TapSequence
        {
            uint8_t length;
            uint8_t taps[3];
            uint8_t trick;
            uint8_t motion; // into Rider::trickMotion (inferred)
        };
        static_assert(sizeof(TapSequence) == 6);
        constexpr uint32_t kSequenceGroups = 21;
        inline ee::Global<TapSequence[kSequenceGroups][4], sym::g_tapSequences> g_tapSequences;

        bool trickOff(const Rider *rider, uint8_t trick)
        {
            return (rider->tricksOff[trick >> 3] >> (trick & 7u)) & 1u;
        }

        // The grind and stall lookups: the last two taps' trick (if the
        // rider can do it), else the last tap's, else the stick's.
        uint8_t trickByTaps(const Rider *rider, const uint8_t (&table)[9][9])
        {
            const uint8_t count = rider->tapCount;
            uint8_t trick = kNoTrick;
            if (count >= 2)
            {
                trick = table[rider->taps[count - 2]][rider->taps[count - 1]];
                if (trickOff(rider, trick))
                    trick = kNoTrick;
            }
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
            if (trickOff(rider, sequence.trick))
                return false;
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

    // A lip's trick: the last tap's direction, or the stick's (the air
    // state asks for a lip or stall with it; kNoTrick: none).
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
    // kNoTrick for none or one the rider cannot do.
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
            if (trickOff(rider, trick))
                trick = kNoTrick;
        }
        if (trick != kNoTrick)
            return trick;
        trick = airTricks[rider->taps[rider->tapCount - 1] - 1];
        if (trickOff(rider, trick))
            trick = kNoTrick;
        return trick;
    }
    READABLE_FUNCTION(sym::airTrickOneDirection, airTrickByTaps, Verified);

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
}
