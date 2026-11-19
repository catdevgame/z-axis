// The games the runtime runs (runtime/ps2_games.h).
#include "runtime/ps2_games.h"

#include <cctype>
#include <string>

namespace ps2x::games
{
    const std::vector<Game> &all()
    {
        static const std::vector<Game> games = {
            // Aggressive Inline: a wider or turned view lets more of its
            // effects spawn (qa3 F1: widescreen, field of view 90,
            // mouse-look). Its draw distance moves every camera's far clip
            // the game does not set again: the 2D camera's, all session, and
            // a new camera's until the level camera sets its 1000 (200 -> 500
            // at Far, with their matrices): the game's own state.
            {"ai", "SLUS_203.27", "Aggressive Inline", true, true},
            // Dave Mirra Freestyle BMX 2: its draw distance multiplies every
            // level camera's far clip, single player's too (Woodward Camp 187
            // -> 280 at Far): the game's own state.
            {"dm2", "SLUS_201.59", "Dave Mirra Freestyle BMX 2", false, true},
            // BMX XXX: its draw distance moves every level's far clip (the
            // level's Globals FarClip, single player's too) and with native
            // draws the models' levels of detail: the game's own state.
            {"bmx", "SLUS_204.15", "BMX XXX", false, true},
        };
        return games;
    }

    const Game &game(std::string_view idOrElf)
    {
        std::string lower(idOrElf), upper(idOrElf);
        for (char &c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char &c : upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (const Game &each : all())
            if (lower == each.id || upper == each.elf)
                return each;
        return all().front();
    }
}
