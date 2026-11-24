// Practice's row in the collection (practice.h).

#include "collection/practice.h"

namespace zx::collection
{
    zx::settings::Setting practiceSetting(std::string_view gameId, std::function<int()> current,
                                          std::function<void(int)> apply)
    {
        zx::settings::Setting practice;
        practice.key = "game.practice";
        practice.label = "Practice";
        // (the practice_point key: F2 on a keyboard, L3+L2 on a gamepad: L3 is no game's button.) Aggressive Inline puts the
        // skater back moving as at the point; DM2's and BMX XXX's respawn puts the rider there stopped.
        const bool stopped = gameId != "ai";
#if defined(__ANDROID__)
        practice.help = stopped ? "L3+L2 sets a point. Restart: back there, stopped." : "L3+L2 sets a restart point. Restart goes back, timed.";
#else
        practice.help = stopped ? "F2 sets a point. Restart: back there, stopped." : "F2 sets a restart point. Restart goes back, timed.";
#endif
        practice.current = std::move(current);
        practice.apply = std::move(apply);
        return practice;
    }

    bool practiceOffered(std::string_view gameId)
    {
        // aggressive_inline_practice.cpp, dave_mirra_practice.cpp, bmx_xxx_practice.cpp.
        return gameId == "ai" || gameId == "dm2" || gameId == "bmx";
    }
}
