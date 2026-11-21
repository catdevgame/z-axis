// BMX XXX's players in a level (player_riders.h): the rider, weapon and
// camera of each player. BMX XXX's own code (action.c).

#include "player_riders.h"

#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/player_riders.cpp.h"

namespace game::bmx
{
    namespace
    {
        // Each player's rider, weapon and camera, and the ones every player
        // shares in a session with one rider.
        const ee::Ptr<uint8_t> kPlayerRiders(sym::g_playerRiders);
        inline ee::Global<uint8_t, sym::g_singlePlayerRider> g_singlePlayerRider;
        const ee::Ptr<uint32_t> kPlayerWeapons(sym::g_playerWeapons);
        inline ee::Global<uint32_t, sym::g_singlePlayerWeapon> g_singlePlayerWeapon;
        const ee::Ptr<uint32_t> kPlayerCameras(sym::g_playerCameras);
    }

    uint8_t riderIndexOf(uint32_t player)
    {
        if (!sessionHasOneRider())
            return kPlayerRiders[player];
        return *g_singlePlayerRider;
    }
    READABLE_FUNCTION(riderIndexOf, Verified);

    uint32_t playerWeapon(uint32_t player)
    {
        if (!sessionHasOneRider())
            return kPlayerWeapons[player];
        return *g_singlePlayerWeapon;
    }
    READABLE_FUNCTION(playerWeapon, Verified);

    // playerCamera: the shared camera is the first player's.
    uint32_t playerCamera(uint32_t player)
    {
        if (!sessionHasOneRider())
            return kPlayerCameras[player];
        return kPlayerCameras[0];
    }
    READABLE_FUNCTION(playerCamera, Verified);
}
