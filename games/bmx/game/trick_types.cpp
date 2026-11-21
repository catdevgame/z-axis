// BMX XXX's tricks by their type (trick_types.h): bmx.c's.

#include "trick_types.h"

#include "ee/readable.h"

#include <cstdint>
#include "sym/bmx/game/trick_types.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The first trick of each type after the air's, and one past the
        // flatland's last.
        constexpr uint32_t kFirstGrind = 0x33, kFirstStall = 0x3C, kFirstTap = 0x46, kFirstManual = 0x48,
                           kFirstFlatland = 0x4A, kLastFlatland = 0x63;
    }

    // trickType: a trick's type by where it is in the table (kTrickNone
    // past the flatland's, kNoTrick among them).
    uint32_t trickType(uint32_t trick)
    {
        if (trick < kFirstGrind)
            return kTrickAir;
        if (trick < kFirstStall)
            return kTrickGrind;
        if (trick < kFirstTap)
            return kTrickStall;
        if (trick < kFirstManual)
            return kTrickTap;
        if (trick < kFirstFlatland)
            return kTrickManual;
        return trick <= kLastFlatland ? kTrickFlatland : kTrickNone;
    }
    READABLE_FUNCTION(trickType, Verified);
}
