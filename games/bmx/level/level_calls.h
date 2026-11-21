#pragma once
// What BMX XXX's level objects share of other modules (the sounds' words,
// the path men's message, the rider collision layer's node handle), and the
// one engine function they call with no readable body yet, by address.
// Scaffolding (address-call): objectLevelNodeName, a plain call once written.

#include "bmx/game/pickups.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/collision/dynamics.h"
#include "zge/math/types.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/level/level_calls.h.h"

namespace game::bmx
{
    // The game's sound events (soundEvent, bmx/sound/events.h): event 55 is
    // a level object's own sound, by what its third word asks.
    constexpr uint32_t kObjectSoundEvent = 55;
    constexpr uint32_t kObjectSoundStart = 0; // (position, 0, name, volume) -> the sound's handle
    constexpr uint32_t kObjectSoundStop = 1;  // (position, 1, handle, 0)
    constexpr uint32_t kObjectSoundMove = 2;  // (position, 2, handle, volume): the playing sound follows the object
    constexpr uint32_t kObjectSoundOnce = 3;  // (position, 3, name, volume): played once
    constexpr uint32_t kFullVolume = 0x7F;

    // The path men's message for what a rider near them did (the path
    // men's file is another group's: its name there is kRiderDidMessage).
    constexpr uint32_t kRiderDidMessage = 0x109;
    constexpr uint32_t kRiderDidTrick = 0, kRiderCrashed = 1; // its data
}

namespace game::bmx::calls
{
    // An object's level node's name (the engine's; 0 for an object with no node).
    inline ee::Ptr<const char> objectLevelNodeName(ee::Ptr<zge::SceneObject> object)
    {
        return ee::game<ee::Ptr<const char>>(sym::objectLevelNodeName, object);
    }
}

namespace game::bmx
{
    // The collision layer's handle of an object's level node (what showNode
    // and setNodeCollidable take).
    inline uint64_t objectNodeHandle(ee::Ptr<zge::SceneObject> object)
    {
        return rider::collision::nodeHandle(ee::Address(zge::objectLevelNode(object)));
    }
}
