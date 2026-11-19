#pragma once
// What BMX XXX's level object types share (entities.cpp, entity_models.cpp).

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    // The kind in an object's name, in capitals ("pu_health01" gives "HEALTH"); one shared buffer.
    ee::Ptr<char> entityKindName(ee::Ptr<zge::SceneObject> object);
    // The object's sound for a rider's touch ("<name>.bump", ".tap", ".grind", ".crash") when the rider meets it
    // fast enough; 1 when a sound started.
    uint32_t playTouchSound(ee::Ptr<zge::EngineMessage> message, ee::Address name);

    // Every level object type the session's kind has registered with the engine (always 1), and the engine's
    // table of types cleared again.
    uint32_t registerLevelObjectTypes();
    int32_t unregisterLevelObjectTypes();
    // Every object of the name shown, or hidden (one that says it is not enabled stays hidden).
    void showObjectsNamed(uint32_t name, uint32_t shown);
    // Every object told to hold its animation, or to let it go on.
    void pauseObjectAnimations();
    void resumeObjectAnimations();
    // The level's random numbers: the caller's state stepped, and the step as a fraction 0 .. 1.
    ee::Float seededRandom(ee::Ptr<uint32_t> state);

    // The segment a follower's object takes at the end of its segment (`atStart`: the end it is at), by the
    // object's exit weights in its tag data at `exitTags`: the segment's name hash, or kNoSegmentExit.
    constexpr uint32_t kNoSegmentExit = 0xFFFFFFFFu;
    uint32_t pickSegmentExit(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::Follower> follower, uint32_t atStart, uint32_t exitTags);
    uint32_t pickExitByWeight(ee::Ptr<zge::SceneObject> object, uint32_t exitTags);
    // What the current player's rider did told to the path men near it.
    int32_t tellPathMenNearRider(uint32_t what);
    uint32_t tellPathManIfNear(ee::Ptr<zge::SceneObject> object, uint32_t what);
    // Every "PathMan" and "FastPathMan" sent its enabled state again.
    int32_t refreshPathMen();
    void entitiesFrameNoop();
    // (entity_models.cpp) The entity models' cache freed.
    void freeEntityModelCache();
}
