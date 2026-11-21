#pragma once

// The animate system's records, in EE RAM (zge/scene/animate_system.cpp; its
// set-up is aggressive/skater/rig.cpp).

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "zge/math/types.h"
#include "zge/math/quaternion.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct SceneObject;
    struct SceneBone;
    struct AnimationChannel;

    // A mass. Held points (rig.cpp's second argument) are the bones' and
    // only hand their places to the constraints.
    enum AnimatePointKind : int32_t
    {
        kPointFree = 0,
        kPointHeld = 1,
    };
    // ZGE4's (BMX XXX) point has 16 bytes more at the front, how the level's collision treats it (ZGE4's
    // collidePoint reads them; no function read so far writes them, so they stay 0 from newBoneAttachedPoint's
    // fill), and keeps the object it animates before its next link (newBoneAttachedPoint); each offset below is
    // read in BMX XXX's own code (startPointSolve, applyPullToPoint, solveDistance, resetAnimatePoints).
    struct AnimatePoint
    {
#if ZX_GAME_BMX
        uint32_t collisionOff;   // BMX +0x00 not 0: collidePoint leaves the point where it is
        uint32_t axisKept[3];    // BMX +0x04 x, y, z: not 0, a hit with the level leaves that axis as it is
#endif
        int32_t kind;            // +0x00 (BMX +0x10)
        uint32_t _04;            // +0x04
        ee::Float mass;          // +0x08 (BMX +0x18)
        ee::Float inverseMass;   // +0x0C (BMX +0x1C) 0 for no mass
        Vec4 position;           // +0x10 (BMX +0x20)
        Vec4 _20;                // +0x20
        Vec4 velocity;           // +0x30 (BMX +0x40)
        Vec4 force;              // +0x40 (BMX +0x50) rig.cpp's weights: mass times the fall
        Vec4 solved;             // +0x50 (BMX +0x60) this round's place
        Vec4 solvedSum;          // +0x60 (BMX +0x70) the places asked for, by weight
        ee::Float solvedWeight;  // +0x70 (BMX +0x80)
        uint32_t _74[3];         // +0x74
        Vec4 lastPosition;       // +0x80 (BMX +0x90)
        Vec4 boneOffset;         // +0x90 (BMX +0xA0) where it starts, in the bone
        uint32_t id;             // +0xA0 (BMX +0xB0) what the ZPDN data names the point by (findZpdnPoint);
                                 // rig.cpp gives 0, 1 or 2, and nothing in AI looks those up
        ee::Ptr<SceneBone> bone; // +0xA4 (BMX +0xB4)
#if ZX_GAME_BMX
        ee::Ptr<SceneObject> object; // BMX +0xB8 the system's object (newBoneAttachedPoint; constraints copy it)
#endif
        ee::Ptr<AnimatePoint> next; // +0xA8 (BMX +0xBC)
#if !ZX_GAME_BMX
        uint32_t _ac;            // +0xAC
#endif
        // The object the point animates: ZGE4 reads the point's own (resetAnimatePoints), ZGE3 the system's.
        template <typename SystemObject>
        ee::Ptr<SceneObject> objectOr(SystemObject systemObject) const
        {
#if ZX_GAME_BMX
            (void)systemObject;
            return object;
#else
            return systemObject;
#endif
        }
        // ZGE4's own object of the point (0 in ZGE2 and ZGE3, which keep none: their steps read the system's).
        ee::Ptr<SceneObject> ownObject() const
        {
#if ZX_GAME_BMX
            return object;
#else
            return ee::Ptr<SceneObject>();
#endif
        }
        // ZGE4's collision words (collidePoint); ZGE2 and ZGE3 have none, so a point always collides there and
        // keeps no axis.
        bool collides() const
        {
#if ZX_GAME_BMX
            return collisionOff == 0;
#else
            return true;
#endif
        }
        bool keepsAxis(uint32_t axis) const
        {
#if ZX_GAME_BMX
            return axisKept[axis] != 0;
#else
            (void)axis;
            return false;
#endif
        }
        void setCollisionOff([[maybe_unused]] uint32_t off)
        {
#if ZX_GAME_BMX
            collisionOff = off;
#endif
        }
        void setAxisKept([[maybe_unused]] uint32_t axis, [[maybe_unused]] uint32_t kept)
        {
#if ZX_GAME_BMX
            axisKept[axis] = kept;
#endif
        }
        // The object the point animates, kept by ZGE4 only (newBoneAttachedPoint stores the system's).
        void setObject(ee::Ptr<SceneObject> systemObject)
        {
#if ZX_GAME_BMX
            object = systemObject;
#else
            (void)systemObject;
#endif
        }
    };
    static_assert(sizeof(AnimatePoint) == game::readable::perGame(0xB0u, 0xB0u, 0xC0u) &&
                  offsetof(AnimatePoint, solvedSum) == game::readable::perGame(0x60u, 0x60u, 0x70u));
    static_assert(offsetof(AnimatePoint, lastPosition) == game::readable::perGame(0x80u, 0x80u, 0x90u) &&
                  offsetof(AnimatePoint, next) == game::readable::perGame(0xA8u, 0xA8u, 0xBCu));
#if ZX_GAME_BMX
    static_assert(offsetof(AnimatePoint, axisKept) == 0x04 && offsetof(AnimatePoint, object) == 0xB8);
#endif
    static_assert(offsetof(AnimatePoint, kind) == game::readable::perGame(0x00u, 0x00u, 0x10u) &&
                  offsetof(AnimatePoint, position) == game::readable::perGame(0x10u, 0x10u, 0x20u));

    // A rule on one or two points (0x30 bytes), on the system's list.
    enum AnimateConstraintKind : int32_t
    {
        kConstraintDistance = 0,  // first and second as far apart as they started
        kConstraintAnchor = 1,    // first at offset (the dead newAnimateAnchor's)
        kConstraintBonePoint = 2, // first at offset in bone
        kConstraintBoneOffset = 3, // second at first + offset turned by bone
    };
    struct AnimateConstraint
    {
        int32_t kind;              // +0x00
        ee::Ptr<AnimatePoint> first;  // +0x04
        ee::Ptr<AnimatePoint> second; // +0x08
        ee::Float stiffness;       // +0x0C the weight of what it asks
        Vec4 offset;               // +0x10
        ee::Ptr<SceneBone> bone;   // +0x20
#if ZX_GAME_BMX
        ee::Ptr<SceneObject> object; // BMX +0x24 the first point's object (newBoneConstraint)
#endif
        ee::Float restLength;      // +0x24 (BMX +0x28)
        ee::Ptr<AnimateConstraint> next; // +0x28 (BMX +0x2C)
#if !ZX_GAME_BMX
        uint32_t _2c;              // +0x2C
#endif
        // The object the constraint belongs to, kept by ZGE4 only: its first point's (newBoneConstraint,
        // newAnimateBoneSpace).
        void setObjectOf(const AnimatePoint &point)
        {
#if ZX_GAME_BMX
            object = point.object;
#else
            (void)point;
#endif
        }
        // ZGE4's own object of the constraint (solveBonePoint places it on that; 0 in ZGE2 and ZGE3, which
        // keep none: their steps read the system's).
        ee::Ptr<SceneObject> ownObject() const
        {
#if ZX_GAME_BMX
            return object;
#else
            return ee::Ptr<SceneObject>();
#endif
        }
    };
    static_assert(sizeof(AnimateConstraint) == 0x30 && offsetof(AnimateConstraint, restLength) == game::readable::perGame(0x24u, 0x24u, 0x28u));

    // A side of the shape a group of planes makes (0x70 bytes): in the
    // bone at set-up, in the world after updateAnimatePlane.
    // ZGE4's (BMX XXX) plane (0xA0 bytes) keeps its object first (updateAnimatePlane sets it from the system
    // when 0) and ignores up to 15 points (newAnimatePlane, pushPointOutOfPlanes).
    struct AnimatePlane
    {
#if ZX_GAME_BMX
        ee::Ptr<SceneObject> object; // BMX +0x00
        ee::Ptr<SceneBone> bone;     // BMX +0x04
        uint32_t _08[2];
#else
        ee::Ptr<SceneBone> bone; // +0x00
        uint32_t _04[3];        // +0x04
#endif
        Vec4 place;             // +0x10
        Vec4 normal;            // +0x20
        Vec4 worldPlace;        // +0x30
        Vec4 worldNormal;       // +0x40
        ee::Float worldDistance; // +0x50 of the plane, along the normal
        ee::Ptr<AnimatePoint> ignored[game::readable::perGame(5, 5, 15)]; // +0x54 points this plane does not stop (rig.cpp's keepOut)
        uint32_t ignoredCount;  // +0x68 (BMX +0x90)
        ee::Ptr<AnimatePlane> next; // +0x6C (BMX +0x94)
#if ZX_GAME_BMX
        uint32_t _98[2];
#endif
        // The object the plane is placed on: ZGE4 keeps it in the plane, taking the system's the first time
        // (updateAnimatePlane, which reads the system only then); ZGE3 reads the system's each time.
        template <typename SystemObject>
        ee::Ptr<SceneObject> objectOrTake(SystemObject systemObject)
        {
#if ZX_GAME_BMX
            if (!object)
                object = systemObject();
            return object;
#else
            return systemObject();
#endif
        }
        // ZGE4's own object of the plane, for the code that makes one on another object's bone.
        void setObject([[maybe_unused]] ee::Ptr<SceneObject> planeObject)
        {
#if ZX_GAME_BMX
            object = planeObject;
#endif
        }
    };
    static_assert(sizeof(AnimatePlane) == game::readable::perGame(0x70u, 0x70u, 0xA0u) &&
                  offsetof(AnimatePlane, ignoredCount) == game::readable::perGame(0x68u, 0x68u, 0x90u));
    static_assert(offsetof(AnimatePlane, worldNormal) == 0x40 && offsetof(AnimatePlane, bone) == game::readable::perGame(0x00u, 0x00u, 0x04u));

    struct AnimatePlaneGroup
    {
        ee::Ptr<AnimatePlane> planes;    // +0x00
        ee::Ptr<AnimatePlaneGroup> next; // +0x04
    };

    // A sphere on a bone that points stay out of (0x40 bytes); only the
    // dead newAnimateDriver makes them, so the game's lists are empty.
    struct AnimateSphere
    {
        ee::Ptr<SceneBone> bone; // +0x00
        uint32_t _04[3];     // +0x04
        Vec4 centre;         // +0x10 in the world
        Vec4 boneCentre;     // +0x20
        ee::Float radius;    // +0x30
        ee::Ptr<AnimateSphere> next; // +0x34
        uint32_t _38[2];     // +0x38
    };
    static_assert(sizeof(AnimateSphere) == 0x40 && offsetof(AnimateSphere, next) == 0x34);

    // A spring with damping between two points (0x14 bytes): a force,
    // where a constraint is a place.
    struct AnimatePull
    {
        ee::Ptr<AnimatePoint> first;  // +0x00
        ee::Ptr<AnimatePoint> second; // +0x04
        ee::Float strength;  // +0x08
        ee::Float damping;   // +0x0C
        ee::Ptr<AnimatePull> next; // +0x10
    };
    static_assert(sizeof(AnimatePull) == 0x14);

    // What ZGE4's alignBonesToPoints does with a pivot (AnimatePivot::kind, BMX XXX only; the whole word, so a
    // frame kind with negated axes does nothing there).
    enum AnimatePivotKind : uint32_t
    {
        kPivotFollowLine = 0,   // the bone turned to follow from -> to (newBonePivot; ZGE3's only kind)
        kPivotFrameFirst = 1,   // 1 to 6: the bone set to the frame of its three points, in the kind's axis order
        kPivotFrameLast = 6,    //   (pivotFrame), turned by the pivot's frame offset
        kPivotFollowParent = 7, // the bone keeps its parent's turn
    };
    // The frame kinds' axes negated (the kind word's top byte; pivotFrame).
    enum PivotFrameNegation : uint32_t
    {
        kNegateAlong = 1u << 24,  // from -> to
        kNegateNormal = 2u << 24, // along x across
        kNegateAcross = 4u << 24, // from -> the third point, square to along
        kPivotKindMask = 0x00FFFFFFu,
    };

    // A bone turned to follow the line from one point to another (0x50
    // bytes).
    // ZGE4's (BMX XXX) pivot (0x60 bytes): two words before the points and one after them, a vector after the
    // rotation; the offsets as BMX XXX's own pivot makers write them (newBonePivot's counterpart:
    // from +0x08, to +0x0C, bone +0x14, parentBone +0x18, channel +0x1C, restDirection +0x20, next +0x54) and
    // its alignBonesToPoints and sortAnimatePivots read them (rotation +0x30, parent +0x50, 0x60 bytes swapped).
    struct AnimatePivot
    {
#if ZX_GAME_BMX
        uint32_t kind;              // BMX +0x00 AnimatePivotKind, a frame kind's PivotFrameNegation in its top byte
        uint32_t rootBone;          // BMX +0x04 not 0: the bone has no parent bone (newBonePivot's counterpart sets
                                    // 1), so alignBonesToPoints takes its parent frame from the object
#endif
        ee::Ptr<AnimatePoint> from; // +0x00 (BMX +0x08)
        ee::Ptr<AnimatePoint> to;   // +0x04 (BMX +0x0C)
#if ZX_GAME_BMX
        ee::Ptr<AnimatePoint> third; // BMX +0x10 a frame kind's third point (pivotFrame)
#endif
        ee::Ptr<SceneBone> bone;    // +0x08 (BMX +0x14)
        ee::Ptr<SceneBone> parentBone; // +0x0C (BMX +0x18)
        ee::Ptr<AnimationChannel> channel; // +0x10 (BMX +0x1C) the bone's in the object's animator
#if !ZX_GAME_BMX
        uint32_t _14[3];            // +0x14
#endif
        Vec4 restDirection;         // +0x20 from to to, in the parent bone's axes
        Quaternion rotation;        // +0x30 of the bone, in the world
#if ZX_GAME_BMX
        Quaternion frameOffset;     // BMX +0x40 a frame kind's: the bone's turn from its frame when made (the frame
                                    // maker's inverse frame times the bone's rotation)
#endif
        ee::Ptr<AnimatePivot> parent; // +0x40 (BMX +0x50) the pivot before it on parentBone
        ee::Ptr<AnimatePivot> next; // +0x44 (BMX +0x54)
        uint32_t _48[2];            // +0x48 (BMX +0x58)

        // ZGE4's pivot words (alignBonesToPoints, pivotFrame). ZGE2 and ZGE3 have none: every pivot follows its
        // line under its parent bone there.
        uint32_t kindWord() const
        {
#if ZX_GAME_BMX
            return kind;
#else
            return kPivotFollowLine;
#endif
        }
        bool onRootBone() const
        {
#if ZX_GAME_BMX
            return rootBone != 0;
#else
            return false;
#endif
        }
        ee::Ptr<AnimatePoint> thirdPoint() const
        {
#if ZX_GAME_BMX
            return third;
#else
            return ee::Ptr<AnimatePoint>();
#endif
        }
        Quaternion *frameOffsetOf()
        {
#if ZX_GAME_BMX
            return &frameOffset;
#else
            return nullptr;
#endif
        }
    };
    static_assert(sizeof(AnimatePivot) == game::readable::perGame(0x50u, 0x50u, 0x60u) &&
                  offsetof(AnimatePivot, next) == game::readable::perGame(0x44u, 0x44u, 0x54u));
    static_assert(offsetof(AnimatePivot, bone) == game::readable::perGame(0x08u, 0x08u, 0x14u) &&
                  offsetof(AnimatePivot, from) == game::readable::perGame(0x00u, 0x00u, 0x08u));
#if ZX_GAME_BMX
    static_assert(offsetof(AnimatePivot, rootBone) == 0x04 && offsetof(AnimatePivot, third) == 0x10 &&
                  offsetof(AnimatePivot, frameOffset) == 0x40);
#endif
    static_assert(offsetof(AnimatePivot, channel) == game::readable::perGame(0x10u, 0x10u, 0x1Cu) &&
                  offsetof(AnimatePivot, restDirection) == 0x20 && offsetof(AnimatePivot, rotation) == 0x30 &&
                  offsetof(AnimatePivot, parent) == game::readable::perGame(0x40u, 0x40u, 0x50u));
    constexpr uint32_t kPivotCopyBytes = game::readable::perGame(0x4Cu, 0x4Cu, 0x60u); // what sortAnimatePivots swaps

    // ZGE4's (BMX XXX) system (0x38 bytes): a word before the counts, the share of the rounds it gets (1.0
    // when made, newAnimateSystem; ZGE4's animate scales the rounds by it), whether its points collide with the
    // level, the object at +0x30 and the next system of a chain; the lists as read in freeAnimateSystem,
    // newAnimatePull, addAnimatePlaneGroup, newDistanceConstraint, newBoneAttachedPoint, the pivot count as
    // newBonePivot's counterpart counts it.
    struct AnimateSystem
    {
#if ZX_GAME_BMX
        uint32_t pivotsSkipped;              // BMX +0x00 a word for the system's owner: BMX XXX's game keeps there
                                             // what it hands animate as its third word (not 0: the pivots leave
                                             // their bones); no engine function read reads it
#endif
        uint32_t pointCount;                 // +0x00 (BMX +0x04)
        ee::Ptr<AnimatePoint> points;        // +0x04 (BMX +0x08)
        int32_t constraintCount;             // +0x08 (BMX +0x0C)
        ee::Ptr<AnimateConstraint> constraints; // +0x0C (BMX +0x10) the newest first
        uint32_t pivotCount;                 // +0x10 (BMX +0x14)
#if ZX_GAME_BMX
        ee::Float roundsScale;               // BMX +0x18 the share of g_animateRounds animate gives it
#endif
        // ZGE4's words, for the code that owns a system: ZGE2's and ZGE3's systems have none.
        uint32_t pivotsSkippedWord() const
        {
#if ZX_GAME_BMX
            return pivotsSkipped;
#else
            return 0;
#endif
        }
        void setPivotsSkipped([[maybe_unused]] uint32_t word)
        {
#if ZX_GAME_BMX
            pivotsSkipped = word;
#endif
        }
        void setRoundsScale([[maybe_unused]] ee::Float scale)
        {
#if ZX_GAME_BMX
            roundsScale = scale;
#endif
        }
        // ZGE4 gives a new system all the rounds (newAnimateSystem: its rounds scale 1.0).
        void setUnit()
        {
#if ZX_GAME_BMX
            roundsScale = ee::Float(1.0f);
#endif
        }
        // The rounds ZGE4's animate gives the constraints: the engine's count times the system's share,
        // truncated. ZGE2 and ZGE3 give the count.
        uint32_t roundsOf(uint32_t rounds) const
        {
#if ZX_GAME_BMX
            return static_cast<uint32_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(rounds)) * roundsScale));
#else
            return rounds;
#endif
        }
        ee::Ptr<AnimatePivot> pivots;        // +0x14 (BMX +0x1C) parents first (sortAnimatePivots)
        ee::Ptr<AnimatePlaneGroup> planeGroups; // +0x18 (BMX +0x20)
        ee::Ptr<AnimateSphere> spheres;      // +0x1C (BMX +0x24)
        ee::Ptr<AnimatePull> pulls;          // +0x20 (BMX +0x28)
#if ZX_GAME_BMX
        uint32_t collidesWithLevel;          // BMX +0x2C not 0: collidePoint keeps the free points out of the level
                                             // too (setAnimateSystemCollision; one of BMX XXX's rigs sets 1)
#endif
        ee::Ptr<SceneObject> object;         // +0x24 (BMX +0x30)
#if ZX_GAME_BMX
        ee::Ptr<AnimateSystem> next;         // BMX +0x34 the next system on the chain (resetAnimatePoints, animate,
                                             // drawAnimateSystem walk it)
#endif
        // ZGE4's chain and level collision (animate); ZGE2 and ZGE3 have neither: no next system, no collision.
        ee::Ptr<AnimateSystem> nextInChain() const
        {
#if ZX_GAME_BMX
            return next;
#else
            return ee::Ptr<AnimateSystem>();
#endif
        }
        uint32_t levelCollision() const
        {
#if ZX_GAME_BMX
            return collidesWithLevel;
#else
            return 0;
#endif
        }
        void setLevelCollision(uint32_t on)
        {
#if ZX_GAME_BMX
            collidesWithLevel = on;
#else
            (void)on;
#endif
        }
    };
    static_assert(sizeof(AnimateSystem) == game::readable::perGame(0x28u, 0x28u, 0x38u) &&
                  offsetof(AnimateSystem, object) == game::readable::perGame(0x24u, 0x24u, 0x30u));
#if ZX_GAME_BMX
    static_assert(offsetof(AnimateSystem, roundsScale) == 0x18 && offsetof(AnimateSystem, collidesWithLevel) == 0x2C &&
                  offsetof(AnimateSystem, next) == 0x34);
#endif
    static_assert(offsetof(AnimateSystem, pivots) == game::readable::perGame(0x14u, 0x14u, 0x1Cu) &&
                  offsetof(AnimateSystem, pulls) == game::readable::perGame(0x20u, 0x20u, 0x28u) &&
                  offsetof(AnimateSystem, pivotCount) == game::readable::perGame(0x10u, 0x10u, 0x14u));

    // ZGE4's body rig (BMX XXX only; 0x2C bytes from g_bodyRigPool): an animate system over a model's own body,
    // with the points of its hands and feet for whoever holds or places them. Three builders fill one: two of the
    // whole body with every point free (one without elbows and knees), and the limbs' (buildLimbRig), which
    // newLimbRig makes. The fields are as the builders write them at their ends.
    struct BodyRig
    {
        uint32_t _00;                      // +0x00 [unplaced: every builder clears it (memoryFill of 4 bytes)]
        ee::Ptr<AnimateSystem> system;     // +0x04
        ee::Ptr<AnimatePoint> leftHand;    // +0x08 the "LeftHand" bone's point
        ee::Ptr<AnimatePoint> rightHand;   // +0x0C "RightHand"
        uint32_t _10[2];                   // +0x10 [unplaced: no builder writes them]
        ee::Ptr<AnimatePoint> leftFoot;    // +0x18 "LeftFoot"
        ee::Ptr<AnimatePoint> rightFoot;   // +0x1C "RightFoot"
        ee::Ptr<AnimatePoint> hipsBottom;  // +0x20 the whole-body rigs' "HipsBottom" point (on the "Hips" bone); the
                                           // limbs' rig has none (0)
        uint32_t stepping;                 // +0x24 0: stepBodyRig does nothing (no builder writes it)
        uint32_t rotationsKept;            // +0x28 0: stepBodyRig clears the pivots' bone rotations before it animates
    };
    static_assert(sizeof(BodyRig) == 0x2C && offsetof(BodyRig, leftFoot) == 0x18 && offsetof(BodyRig, hipsBottom) == 0x20);

    // ZGE4's soft parts rig (BMX XXX only; 0xB0 bytes from g_softPartsRigPool): the animate systems over the parts
    // of a model that swing on it, each made when the model has that part's bones and 0 otherwise. The bones each
    // builder looks up by name: the shirt's "ShirtUpper", "ShirtLower", "ShirtLeft", "ShirtRight" (with "Chest",
    // "_MidTorso", "Hips"); the hair's "PigtailLeft", "PigtailRight", "PonytailTop", "PonytailBottom", "MargeTop",
    // "MargeBottom" (with "Head", "Chest"); the breasts' "BreastLeft", "BreastRight" (with "Chest"); the skirt's
    // "SkirtMiddle", "SkirtLeft", "SkirtRight" (with "Hips"); the necklace's "NecklaceTop", "NecklaceBottom" (with
    // "Chest"). The offsets are as the builders, the step and the reset write and read them.
    struct SoftPartsRig
    {
        int32_t settleSteps;                 // +0x00 not 0: the steps still to settle in. Such a step moves the points
                                             // and leaves the bones as they are (animate's third word), puts the
                                             // points back on their bones and counts one down
        ee::Ptr<AnimationChannel> breastLeftChannel;  // +0x04 the "BreastLeft" bone's channel
        ee::Ptr<AnimationChannel> breastRightChannel; // +0x08 "BreastRight"
        uint32_t _0C[13];                    // +0x0C [unplaced: the breasts' builder writes +0x0C..+0x20, the shirt's
                                             // +0x30..+0x38]
        Vec4 shirtPlaces[3];                 // +0x40 where shirtPoints[1], [2] and [0] were in the world at the last
                                             // reset, in that order
        ee::Ptr<AnimatePoint> shirtPoints[3]; // +0x70 three of the shirt system's points
        // The shirt's flutter (stepSoftPartsRig): each turn of the phase draws a new gust.
        ee::Float shirtGust;                 // +0x7C this turn's strength, 0.5 to 1 (a random draw)
        ee::Float shirtRate;                 // +0x80 how fast the phase turns as the shirt's points move
        ee::Float shirtPhase;                // +0x84 radians, 0 to a turn (the shirt's builder clears it)
        ee::Float shirtPush;                 // +0x88 how far a step can push the three points down
        uint32_t _8C;                        // +0x8C [unplaced]
        ee::Ptr<AnimateSystem> breasts;      // +0x90
        ee::Ptr<AnimateSystem> hair;         // +0x94
        ee::Ptr<AnimateSystem> shirt;        // +0x98
        ee::Ptr<AnimateSystem> skirt;        // +0x9C
        ee::Ptr<AnimateSystem> necklace;     // +0xA0
        uint32_t _A4[3];                     // +0xA4 [unplaced: nothing read writes them]
    };
    static_assert(sizeof(SoftPartsRig) == 0xB0 && offsetof(SoftPartsRig, shirtPlaces) == 0x40 && offsetof(SoftPartsRig, shirtPoints) == 0x70 &&
                  offsetof(SoftPartsRig, breasts) == 0x90 && offsetof(SoftPartsRig, necklace) == 0xA0);
}

namespace zge
{
    struct ZpdnRecord; // a ZPDN chunk's system (animate_system.cpp)

    // (animate_system.cpp)
    void freeAnimateSystem(ee::Ptr<AnimateSystem> system);
    ee::Ptr<AnimatePlaneGroup> addAnimatePlaneGroup(ee::Ptr<AnimateSystem> system);
    ee::Ptr<AnimatePlane> newAnimatePlane(ee::Ptr<AnimatePlaneGroup> group, ee::Ptr<SceneBone> bone, ee::Ptr<const Vec4> place, ee::Ptr<const Vec4> normal);
    ee::Ptr<AnimatePivot> newBonePivot(ee::Ptr<AnimateSystem> system, ee::Ptr<SceneBone> bone, ee::Ptr<AnimatePoint> from, ee::Ptr<AnimatePoint> to);
    ee::Ptr<AnimatePull> newAnimatePull(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first, ee::Ptr<AnimatePoint> second, ee::Float strength, ee::Float damping);
    ee::Ptr<AnimateSystem> newAnimateSystem(ee::Ptr<SceneObject> object);
    ee::Ptr<AnimateConstraint> newBoneConstraint(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first, ee::Ptr<AnimatePoint> second, ee::Ptr<SceneBone> bone, ee::Float stiffness);
    uint32_t newAnimateBoneSpace(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> point, ee::Ptr<SceneBone> bone, ee::Float stiffness);
    ee::Ptr<AnimateConstraint> newDistanceConstraint(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first, ee::Ptr<AnimatePoint> second, ee::Float stiffness);
    ee::Ptr<AnimatePoint> newBoneAttachedPoint(ee::Ptr<AnimateSystem> system, ee::Ptr<SceneBone> bone, int32_t kind, ee::Ptr<const Vec4> place, uint32_t id, ee::Float mass);
    // (ZGE4 takes a third word: not 0, the points move and the pivots leave their bones as they are.)
    void animate(ee::Ptr<AnimateSystem> system, uint32_t updateObjectPose READABLE_PARAM_ONLY(bmx, uint32_t pivotsSkipped), ee::Float step);
    void resetAnimatePoints(ee::Ptr<AnimateSystem> system);
    void sortAnimatePivots(ee::Ptr<AnimateSystem> system);
    ee::Ptr<BodyRig> newLimbRig(ee::Ptr<SceneObject> object); // ZGE4 only
    // ZGE4 only: the soft parts rig (SoftPartsRig) and the body rig's siblings.
    void clearPivotRotations(ee::Ptr<AnimateSystem> system);
    void resetSoftPartsRig(ee::Ptr<SoftPartsRig> rig);
    void stepSoftPartsRig(ee::Ptr<SoftPartsRig> rig, ee::Float seconds);
    void clearSoftPartsRotations(ee::Ptr<SoftPartsRig> rig);
    void settleSoftPartsRig(ee::Ptr<SoftPartsRig> rig, int32_t steps);
    void freeSoftPartsRig(ee::Ptr<SoftPartsRig> rig);
    void freeBodyRig(ee::Ptr<BodyRig> rig);
    ee::Ptr<AnimateConstraint> newOwnBoneConstraint(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first, ee::Ptr<AnimatePoint> second, ee::Float stiffness);
    void stepBodyRig(ee::Ptr<BodyRig> rig, ee::Float seconds);
    ee::Ptr<BodyRig> newWholeBodyRig(ee::Ptr<SceneObject> object);
    ee::Ptr<BodyRig> newWholeBodyRigWithoutJoints(ee::Ptr<SceneObject> object);
    ee::Ptr<SoftPartsRig> newSoftPartsRig(ee::Ptr<SceneObject> object);
    void drawAnimateSystem(ee::Ptr<AnimateSystem> system);
    void resetBodyRig(ee::Ptr<BodyRig> rig);
    void freeZpdnRecordLists(ee::Ptr<ZpdnRecord> record);
}
