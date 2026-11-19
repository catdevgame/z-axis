#pragma once
// BMX XXX's level pickups (game.c, pickups.cpp): which of the level's pickup
// and checkpoint objects a turn has. BMX XXX's own code.
//
// Names come from the code's use and the game's strings (no build names
// these functions); "(inferred)" marks a meaning read from use only.

#include "bmx/weapon/weapon.h"
#include "ee/ee.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/pickups.h.h"

namespace game::bmx
{
    // A name from its `skip`th character on with every '_' left out, in the
    // game's one buffer for it (the next call overwrites it).
    ee::Ptr<char> nameWithoutUnderscores(ee::Ptr<const char> name, uint32_t skip);

    // The visitor setUpLevelPickups gives every object of the level: a
    // pickup or a checkpoint is enabled or disabled for the mode being
    // played. Returns 1 (the walk goes on).
    uint32_t setUpLevelPickup(ee::Ptr<zge::SceneObject> object);

    // A rider's activation of a level object (its message): what a pickup
    // gives (health, a weapon, points, a bike part, a paintball pickup) or
    // the van checkpoint's end of the challenge's turn. 1 when the object
    // was the objectives', a pickup or the van; else 0.
    uint32_t riderActivatedObject(ee::Ptr<zge::EngineMessage> activation);

    // Whether a projectile's event on a rider counts for `player`'s weapon
    // (riderHitByProjectile asks first; `unused` is its projectile, not
    // read): 1 or 0 (inferred name).
    uint32_t riderHitAllowed(uint8_t player, uint32_t unused, const ProjectileMessage *message);
}
