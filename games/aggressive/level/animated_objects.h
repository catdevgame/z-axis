#pragma once

// Animated level objects ("AnimObj" and "AnimPropDef", script/anim_objects.cpp
// attaches them): what one is playing is kept in its component's tags, so a
// level's save can put it back (level/animated_objects.cpp).

#include "ee/ee.h"
#include "zge/scene/scene.h"
#include "ee/plain.h"

#include <cstdint>

namespace aggressive::level
{
    // The tag accessors' second argument (zge/assets/tag_table.h) for a word
    // and for a byte.
    constexpr uint32_t kTagWords = 8, kTagBytes = 2;

    // An animated object's component tags.
    constexpr uint32_t kTagAttachedX = 0x04, kTagAttachedY = 0x08, kTagAttachedZ = 0x0C; // its position when attached, as whole numbers
    constexpr uint32_t kTagVelocityX = 0x10, kTagVelocityY = 0x14, kTagVelocityZ = 0x18; // floats: its answer to the velocity message
    constexpr uint32_t kTagAnimationName = 28;    // the name hash of the animation it plays
    constexpr uint32_t kTagAnimationMode = 0x20;  // playAnimation's mode (bit 0 loops)
    constexpr uint32_t kTagAnimationPlaying = 33; // 1 once an animation was played

    // A saved animated object, as saveAnimObjectState writes it: found again
    // by its name.
    struct AnimObjectState
    {
        uint32_t name;         // +0x00 the object's name hash
        uint32_t animation;    // +0x04 kTagAnimationName
        ee::Float time;        // +0x08 the animation's time
        uint32_t mode : 4;     // +0x0C kTagAnimationMode
        uint32_t paused : 1;
        uint32_t playing : 1;  // kTagAnimationPlaying
        uint32_t : 26;         // never written: the save keeps what the stack held
    };
    static_assert(sizeof(AnimObjectState) == 0x10);
}

namespace aggressive::level
{
    // (animated_objects.cpp)
    int32_t animObjectStateBytes();
    int32_t playObjectAnimation(ee::Ptr<zge::SceneObject> sceneObject, ee::Address name, uint32_t mode, ee::Float blend);
    int32_t restoreAnimObjectStates(ee::Ptr<AnimObjectState> states, uint32_t bytes);
    void saveAnimObjectStates(ee::Address states);
}
