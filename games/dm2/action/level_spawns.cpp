// The level's spawn points at its start (action.h): DM2's own code
// (action.c).

#include "action.h"
#include "../rider/physics/physics.h"
#include "../rider/rider.h"
#include "../rider/vector.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/scene/locators.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/dm2/action/level_spawns.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kPlayerName = sym::levelPlayerName; // "Player"
        constexpr uint32_t kSpawnKind = sym::levelSpawnKind;   // "Spawn"

        uint32_t hashOf(uint32_t text) { return zge::nameHash(ee::pointerAt<char>(text)); }
    }

    void probeSpawnGround()
    {
        ee::Local<ee::Float[4]> point;
        ee::Local<zge::LocatorList> spawns;
        ee::Local<rider::GroundHit> ground;
        const uint32_t player = hashOf(kPlayerName);
        zge::findLocators(spawns, hashOf(kSpawnKind), player);
        for (ee::Ptr<zge::Locator> spawn = spawns->head; spawn; spawn = spawn->next)
        {
            *rider::vec4(point) = *rider::vec4(rider::physics::locatorPoint(*spawn).position);
            rider::groundBelow(point, ground);
        }
        zge::freeLocators(spawns);
    }
    READABLE_FUNCTION(probeSpawnGround, Verified);
}
