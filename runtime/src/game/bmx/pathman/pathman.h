#pragma once

// BMX XXX's path men (the level object types "PathMan" and "FastPathMan"):
// the people who walk a level's paths. Each keeps a record of its own beside
// its tag record: where it is on its path, the animation it plays, how near
// the riders are, and the objective it gives when it has one. BMX XXX's own
// code; no other build names its functions, so the names are from what each
// does and from the module's strings ("PathMan", "MOTIONLIST", ".phrase.%s",
// the phrases' names).

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "zge/math/types.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/follower.h"
#include "zge/scene/locators.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Objective; // (ZGE4's objectives: zge/scene/objectives.h)
}

namespace game::bmx
{
    // What a path man is doing: each state has its animation.
    enum PathManState : int32_t
    {
        kPathManNoState = 0,       // no animation found for the state asked for
        kPathManOnPath = 1,        // walks its path, playing a motion of the path's motion list
        kPathManDodge = 2,         // a rider comes at it: it gets out of the way
        kPathManDodgeMirrored = 3, // the same to the other side (the animation mirrored)
        kPathManAfterDodge = 4,    // what it does once it has dodged (looped)
        kPathManHit = 5,           // a rider reached it: it stops where it is
        kPathManAfterHit = 6,      // what it does once it has been hit (looped)
    };

    // The phrases a path man speaks, in the order of the game's table of
    // their names.
    enum PathManPhrase : uint32_t
    {
        kPhraseProximity = 0,     // "proximity"
        kPhraseHit = 1,           // "hit"
        kPhraseTrick = 2,         // "trick"
        kPhraseCrash = 3,         // "crash"
        kPhraseProximityPre = 4,  // "proximitypre"
        kPhraseProximityPost = 5, // "proximitypost"
        kPhrasePickup = 6,        // "pickup"
        kPhraseRide = 7,          // "ride"
        kPhraseSit = 8,           // "sit"
    };

    // Called with the path man at each step of its animation (the engine's
    // track blend hook, of which a path man's reads the object alone).
    using PathManPoseHook = ee::FunctionPtr<void(ee::Ptr<zge::SceneObject> pathMan)>;

    // Where a path man is on its path. A "PathMan" follows one of the
    // engine's paths at a speed; a "FastPathMan" one of its fast paths, by
    // time.
    struct PathManTravel
    {
        int32_t fast;         // +0x00 1: a "FastPathMan"
        // +0x04 its follower: a zge::Follower, or with `fast` a fast
        // follower (pathman_calls.h); 0 until it is put on a path.
        uint32_t follower;
        ee::Float topSpeed;   // +0x08 its speed at full pace
        ee::Float speedWanted; // +0x0C top speed x the animation's speed scale
    };
    static_assert(sizeof(PathManTravel) == 0x10);

    // The animation a path man plays, and how.
    struct PathManAnimation
    {
        int32_t state;                  // +0x00 PathManState
        ee::Ptr<zge::Motion> animation; // +0x04
        char name[0x40];                // +0x08 the animation's name (kPathManOnPath: from the motion list)
        uint32_t playMode;              // +0x48 playAnimation's mode
        PathManPoseHook poseHook;       // +0x4C the animation's blend hook: mirrors the pose when `mirrored`; 0 for a looped listed motion
        ee::Float speedScale;           // +0x50 scales the top speed while it plays
        int32_t paused;                 // +0x54 1: the animation is paused, and the path man stays where it is
        int32_t mirrored;               // +0x58
        ee::Ptr<zge::BoneMirror> boneMirror; // +0x5C the left and right bones the mirrored pose swaps
    };
    static_assert(sizeof(PathManAnimation) == 0x60 && offsetof(PathManAnimation, playMode) == 0x48 &&
                  offsetof(PathManAnimation, boneMirror) == 0x5C);

    constexpr uint32_t kObjectiveNameBytes = 0x40;

    struct PathManRecord
    {
        PathManTravel travel;            // +0x00
        PathManAnimation animation;      // +0x10
        // Squared distances to the nearest rider that matter to it:
        ee::Float noticeRangeSquared;    // +0x70 it offers its objective, and dodges a rider coming at it
        ee::Float awakeRangeSquared;     // +0x74 twice that: unseen and with no rider this near, it goes back to its path
        ee::Float touchRangeSquared;     // +0x78 a rider this near has reached it
        // The squared distance to the nearest rider this frame, to the
        // rider's physics position and to its drawn body.
        ee::Float riderDistanceSquared;      // +0x7C
        ee::Float drawnRiderDistanceSquared; // +0x80
        uint16_t phraseFrames;           // +0x84 frames until it may speak in passing again
        uint16_t riderNearFrames;        // +0x86 frames a rider has been within touchRangeSquared
        char objectiveName[kObjectiveNameBytes]; // +0x88 the objective it gives; empty: none
        ee::Ptr<zge::Objective> objective; // +0xC8 that objective, 0 when the level has none of the name
    };
    static_assert(sizeof(PathManRecord) == 0xCC && offsetof(PathManRecord, animation) == 0x10 &&
                  offsetof(PathManRecord, noticeRangeSquared) == 0x70 && offsetof(PathManRecord, phraseFrames) == 0x84 &&
                  offsetof(PathManRecord, objectiveName) == 0x88 && offsetof(PathManRecord, objective) == 0xC8);

    // The record's objective name, as game code takes a text.
    inline ee::Ptr<char> objectiveNameOf(ee::Ptr<PathManRecord> record)
    {
        return ee::Ptr<char>(record->objectiveName);
    }

    // One motion of a path's motion list (a "MOTIONLIST" point's tag record
    // holds eight of these; an empty name is no motion).
    struct ListedMotion
    {
        char name[0x40];  // +0x00
        uint8_t loops;    // +0x40
        uint8_t _41[3];
        ee::Float speedScale; // +0x44
    };
    static_assert(sizeof(ListedMotion) == 0x48 && offsetof(ListedMotion, speedScale) == 0x44);
    constexpr uint32_t kListedMotions = 8;

    // (pathman_record.cpp) The path man's record, made and set up the first
    // time it is asked for; its two parts; the record freed.
    ee::Ptr<PathManRecord> pathManRecord(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<PathManTravel> pathManTravel(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<PathManAnimation> pathManAnimation(ee::Ptr<zge::SceneObject> pathMan);
    void setUpPathManRecord(ee::Ptr<zge::SceneObject> pathMan, ee::Ptr<PathManRecord> record);
    void freePathManRecord(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<zge::BoneMirror> newPathManBoneMirror(ee::Ptr<zge::SceneObject> pathMan);

    // (pathman_animation.cpp)
    ee::Ptr<const char> pathManName(ee::Ptr<zge::SceneObject> pathMan);
    void mirrorPathManPose(ee::Ptr<zge::SceneObject> pathMan);
    void startListedMotion(ee::Ptr<zge::SceneObject> pathMan, ee::Ptr<zge::ScenePoint> motionList, uint32_t pick);
    void pickPathMotion(ee::Ptr<zge::SceneObject> pathMan);
    // The state entered and its animation started with that blend: the
    // animation, 0 when the state has none.
    ee::Ptr<zge::Motion> setPathManState(ee::Ptr<zge::SceneObject> pathMan, int32_t state, ee::Float blend);
    ee::Ptr<zge::Motion> nextPathManState(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t pathManAnimationEnded(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<zge::Motion> returnPathManToPath(ee::Ptr<zge::SceneObject> pathMan);
    ee::Ptr<zge::Motion> sendPathManBackToPath(ee::Ptr<zge::SceneObject> pathMan);

    // (pathman_riders.cpp)
    void watchRiders(ee::Ptr<zge::SceneObject> pathMan);
    void measureRiders(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t measureRider(uint32_t riderIndex, ee::Ptr<zge::SceneObject> pathMan);
    uint32_t measureDrawnRider(uint32_t riderIndex, ee::Ptr<zge::SceneObject> pathMan);
    uint32_t pathManRiderWithin(ee::Ptr<zge::SceneObject> pathMan, ee::Float rangeSquared);
    uint32_t pathManDrawnRiderWithin(ee::Ptr<zge::SceneObject> pathMan, ee::Float rangeSquared);

    // (pathman_travel.cpp)
    ee::Ptr<zge::Vec4> pathManPosition(ee::Ptr<zge::SceneObject> pathMan, ee::Ptr<zge::Vec4> out);
    ee::Ptr<zge::Vec4> pathManBodyFacing(ee::Ptr<zge::SceneObject> pathMan, ee::Ptr<zge::Vec4> out);
    ee::Ptr<zge::Vec4> pathManFacing(ee::Ptr<zge::SceneObject> pathMan, ee::Ptr<zge::Vec4> out);
    void putPathManOnPath(ee::Ptr<zge::SceneObject> pathMan, uint32_t pathKey);
    void putPathManOnFastPath(ee::Ptr<zge::SceneObject> pathMan, uint32_t pathId);
    void steerPathManSpeed(ee::Ptr<zge::SceneObject> pathMan);
    void walkPathManPath(ee::Ptr<zge::SceneObject> pathMan, ee::Float time);
    void walkPathManFastPath(ee::Ptr<zge::SceneObject> pathMan, ee::Float time);
    void movePathMan(ee::Ptr<zge::SceneObject> pathMan, uint32_t evenWhenStill, ee::Float time);

    // (pathman_decisions.cpp)
    void decidePathMan(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t pathManWasHit(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t riderMayTakeObjective(ee::Ptr<zge::SceneObject> pathMan);
    void givePathManObjective(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t riderActivatesPathMan(ee::Ptr<zge::SceneObject> pathMan, uint32_t notWhenWithin);
    uint32_t riderReachesPathMan(ee::Ptr<zge::SceneObject> pathMan);
    void pathManIsHit(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t riderComesAtPathMan(ee::Ptr<zge::SceneObject> pathMan);
    void pathManDodges(ee::Ptr<zge::SceneObject> pathMan, uint32_t side);
    uint32_t riderLingersByPathMan(ee::Ptr<zge::SceneObject> pathMan);
    void sayRiderLingers(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t pathManMayGreet(ee::Ptr<zge::SceneObject> pathMan);
    void pathManGreets(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t pathManOutOfPlay(ee::Ptr<zge::SceneObject> pathMan);

    // (pathman_object.cpp) The two object types and their messages.
    uint32_t registerPathManTypes();
    uint32_t subscribePathMan(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t initPathMan(ee::Ptr<zge::SceneObject> pathMan);
    uint32_t initFastPathMan(ee::Ptr<zge::SceneObject> pathMan);
    int32_t pathManRemoved(ee::Ptr<zge::EngineMessage> message);
    int32_t updatePathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t pathManVelocity(ee::Ptr<zge::EngineMessage> message);
    int32_t pathManSaveSize(ee::Ptr<zge::EngineMessage> message);
    int32_t savePathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t restorePathManAs(ee::Ptr<zge::EngineMessage> message, int32_t fast);
    int32_t restorePathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t restoreFastPathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t pausePathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t resumePathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t pathManTouched(ee::Ptr<zge::EngineMessage> message);
    int32_t enablePathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t pathManIsEnabled(ee::Ptr<zge::EngineMessage> message);
    int32_t disablePathMan(ee::Ptr<zge::EngineMessage> message);
    int32_t pathManRiderDid(ee::Ptr<zge::EngineMessage> message);
    void drawPathManDebug(ee::Ptr<zge::SceneObject> pathMan);
}
