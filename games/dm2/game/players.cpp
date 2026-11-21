// DM2's players (players.h). DM2's own code (game.c's block; the riders'
// indices at the level's start).

#include "players.h"

#include "dm2/menu/menu_mode.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/players.cpp.h"

namespace game::dm2
{

    // riderIndexOf: a watched replay's rider, a single-player game's, or the
    // multiplayer player's own.
    uint8_t riderIndexOf(uint32_t player)
    {
        if (menuMode() == kMenuViewReplay)
            return *g_replayViewRider;
        if (*g_multiplayerPlayers == 0)
            return *g_singlePlayerRider;
        return ee::Ptr<uint8_t>(sym::g_playerRiders)[player];
    }
    READABLE_FUNCTION(riderIndexOf, Verified);

    // playerCharacter: the record of the character the player picked.
    ee::Ptr<RiderCharacter> playerCharacter(uint32_t player)
    {
        const uint8_t character = (ee::Ptr<PlayerSetup>(sym::g_playerSetups) + static_cast<ptrdiff_t>(player))->character;
        return ee::Ptr<RiderCharacter>(sym::g_riderCharacters) + static_cast<ptrdiff_t>(character);
    }
    READABLE_FUNCTION(playerCharacter, Verified);

    // playerLevelGoals: that character's record of a level's goals.
    ee::Ptr<LevelGoals> playerLevelGoals(uint32_t player, uint32_t level)
    {
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);
        return ee::Ptr<LevelGoals>(&character->levels[0]) + static_cast<ptrdiff_t>(level);
    }
    READABLE_FUNCTION(playerLevelGoals, Verified);
}
