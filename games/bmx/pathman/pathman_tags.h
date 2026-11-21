#pragma once

// BMX XXX's path men (the level object types "PathMan" and "FastPathMan"):
// what the level's data gives each one, read from its tag record
// (pathman_tags.cpp).

#include "pathman_calls.h"

#include "ee/ee.h"
#include "zge/scene/animator.h"
#include "zge/scene/follower.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    // Where a path man's tag record keeps these (words and floats share the
    // tag kind kPathManTagWords, pathman_random.h).
    constexpr uint32_t kPathManTagPath = 0x44;      // word: the key of the path it walks
    constexpr uint32_t kPathManTagTopSpeed = 0x48;  // float: its speed along the path at full pace
    constexpr uint32_t kPathManTagSpeedStep = 0x4C; // float: how much its speed may change in one update
    constexpr uint32_t kPathManTagBlendTime = 0x50; // float: the blend into a new animation
    constexpr uint32_t kPathManTagRecord = 0x40;    // word: its record (pathman.h), 0 until first asked for
    constexpr uint32_t kPathManTagStartFrames = 0x54; // float: where on its fast path it starts, in frames of time
    constexpr uint32_t kPathManTagNoticeRange = 0x64; // float: how near a rider must be for it to notice
    constexpr uint32_t kPathManTagTouchRange = 0x68;  // float: how near a rider has reached it
    constexpr uint32_t kPathManTagDodgeAnimation = 0x6C;      // word: name hashes of its states' animations, 0: none
    constexpr uint32_t kPathManTagHitAnimation = 0x70;
    constexpr uint32_t kPathManTagAfterHitAnimation = 0x74;
    constexpr uint32_t kPathManTagAfterDodgeAnimation = 0xBC;
    constexpr uint32_t kPathManTagObjectiveName = 0x78; // text: the objective it gives
    constexpr uint32_t kPathManTagMarker = 0xC0;    // word: the marker scene shown over it while its objective can be taken
    // Bytes and texts have tag kinds of their own.
    constexpr uint32_t kPathManTagBytes = 2;
    constexpr uint32_t kPathManTagTexts = 0x10;
    constexpr uint32_t kPathManTagActsUnseen = 0xB8; // byte: it goes on acting off screen and out of play
    constexpr uint32_t kPathManTagDisabled = 0xB9;  // byte: hidden and still (the level's disable)
    // A path's first point (and a fast path) names its motion list in its
    // own tags: the name hash of a "MOTIONLIST" point.
    constexpr uint32_t kPathTagMotionList = 0x38;
    // A frame of a fast path's time, in seconds.
    constexpr ee::Float kFastPathFramesASecond = 30.0f;

    // The key of the path the path man walks.
    int32_t pathManPath(ee::Ptr<zge::SceneObject> pathMan);
    // Its speed along the path at full pace (an animation state scales it).
    ee::Float pathManTopSpeed(ee::Ptr<zge::SceneObject> pathMan);
    // How much its speed may change in one update, towards the speed wanted.
    ee::Float pathManSpeedStep(ee::Ptr<zge::SceneObject> pathMan);
    // The blend time its animations start with.
    ee::Float pathManBlendTime(ee::Ptr<zge::SceneObject> pathMan);
    // Where on its fast path a "FastPathMan" starts, in seconds.
    ee::Float pathManStartTime(ee::Ptr<zge::SceneObject> pathMan);
    // How near a rider must be for it to notice, and to have reached it.
    ee::Float pathManNoticeRange(ee::Ptr<zge::SceneObject> pathMan);
    ee::Float pathManTouchRange(ee::Ptr<zge::SceneObject> pathMan);
    // The animations of its states, 0 where its data names none.
    ee::Ptr<zge::Motion> pathManDodgeAnimation(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<zge::Motion> pathManHitAnimation(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<zge::Motion> pathManAfterHitAnimation(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<zge::Motion> pathManAfterDodgeAnimation(ee::Ptr<zge::SceneObject> pathMan);
    // The name of the objective it gives, copied out (empty: none).
    uint32_t pathManObjectiveName(ee::Ptr<zge::SceneObject> pathMan, ee::Address out);
    // Whether it goes on acting where no one sees it: its animations then
    // run off screen too, and out of play it is not sent back to its path.
    uint32_t pathManActsUnseen(ee::Ptr<zge::SceneObject> pathMan);
    // The name hash of the motion list of a path, and of a fast path.
    int32_t pathMotionList(ee::Ptr<zge::PathRecord> path);
    int32_t fastPathMotionList(ee::Ptr<zge::FastPath> path);
}
