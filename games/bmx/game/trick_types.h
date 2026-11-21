#pragma once
// BMX XXX's tricks by their type (bmx.c; trick_types.cpp), and the trick table's
// flags the scoring reads. A trick is a byte (Rider::trick, kNoTrick none):
// the air's first, then the grinds, stalls, taps, manuals and the
// flatland's, in the order of the trick table.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>
#include "sym/bmx/game/trick_types.h.h"

namespace game::bmx
{
    // A trick's type (the scripts' TRICK_TYPE words; the objectives read them
    // too).
    enum TrickType : uint32_t
    {
        kTrickNone = 0, // no trick (kNoTrick), or one past the flatland's
        kTrickAir = 1,
        kTrickGrind = 2,
        kTrickStall = 3,
        kTrickTap = 4,
        kTrickManual = 5,
        kTrickFlatland = 6,
        kTrickTypes = 7, // how many (a points objective's: any trick counts)
    };
    uint32_t trickType(uint32_t trick);

    // The trick table's flags, a word a trick (bmx.c's table; the rider's
    // files read other bits of it).
    inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickFlags;
    // The low three bits: a turning trick's kind (0 none; DM2's).
    constexpr uint32_t kTrickTurningKind = 0x7;
    // A trick whose frames in the air's second action do not count as held
    // (DM2's; the flatland's: it loops once).
    constexpr uint32_t kTrickNotHeld = 0x20;
    // A grind or stall whose lean is a turn about the bike's right row
    // (rails.cpp), as a wheelie's: its balance shows on the wheelie's
    // display, turned round.
    constexpr uint32_t kTrickRowsByRight = 0x80;
    // A grind or stall with no balance to keep: the balance display is not
    // shown for it.
    constexpr uint32_t kTrickNoBalance = 0x200;
}
