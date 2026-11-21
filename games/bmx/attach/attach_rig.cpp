// BMX XXX's attaches (attach.h): the body rig of a human attach's figure:
// the figure becomes one of the rider's carried objects, gets a whole-body
// rig, a plane to sit on and its hands' grips. BMX XXX's own code.

#include "attach.h"
#include "attach_calls.h"

#include "bmx/pathman/pathman_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/animator.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/carried_objects.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/bmx/attach/attach_rig.cpp.h"

namespace zge
{
    int32_t resetAnimatorPose(ee::Ptr<SceneObject> object); // (animation.cpp; in no header)
}

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        // The holder's bone the figure sits on.
        constexpr uint32_t kSeatBoneName = sym::hipsdeckBoneName; // "Hipsdeck"
        // The seat's plane: this far up the bone, facing down its y axis.
        constexpr ee::Float kSeatRise = 0.1f;

        // One attach (its object's name hash; which is not read) gets the
        // rig without elbows and knees, half the solver's rounds, and its
        // hips' point out of the level's way.
        constexpr uint32_t kJointlessAttachName = 0x4834C57;
        constexpr ee::Float kJointlessRoundsShare = 0.5f;

        // A hand's grip: a held point on the gripped bone, heavy, and the
        // hand's point of the rig tied to it.
        constexpr ee::Float kGripMass = 20.0f;
        constexpr ee::Float kGripSpaceStiffness = 10.0f;
        constexpr ee::Float kGripStiffness = 100.0f;
        constexpr uint32_t kGripPointId = 4;

        // The rig's points by their ids, the hashes of their bones' names.
        constexpr uint32_t kChestPoint = zge::nameHashOf("Chest");
        constexpr uint32_t kHipsBottomPoint = 0x770576D;
        constexpr uint32_t kLeftShoulderPoint = 0xA41B1D2;
        constexpr uint32_t kLeftElbowPoint = 0xB8A57D7;
        constexpr uint32_t kLeftHandPoint = 0x9B8C124;
        constexpr uint32_t kLeftHipPoint = zge::nameHashOf("LeftHip");
        constexpr uint32_t kLeftFootPoint = 0x9B8B744;
        constexpr uint32_t kRightShoulderPoint = 0xB93D182;
        constexpr uint32_t kRightElbowPoint = 0xD8D6CF7;
        constexpr uint32_t kRightHandPoint = 0xBD894F4;
        constexpr uint32_t kRightHipPoint = 0xDBD8980;
        constexpr uint32_t kRightFootPoint = 0xBD90694;

        // The figure's bones the animate system of a figure without a rig
        // is built on, by name.
        constexpr uint32_t kLeftUpArm = sym::leftUpArmBoneName;     // "LeftUpArm"
        constexpr uint32_t kLeftLowArm = sym::leftLowArmBoneName;   // "LeftLowArm"
        constexpr uint32_t kLeftHand = sym::leftHandBoneName;       // "LeftHand"
        constexpr uint32_t kRightUpArm = sym::rightUpArmBoneName;   // "RightUpArm"
        constexpr uint32_t kRightLowArm = sym::rightLowArmBoneName; // "RightLowArm"
        constexpr uint32_t kRightHand = sym::rightHandBoneName;     // "RightHand"
        constexpr uint32_t kChest = sym::chestBoneName;             // "Chest"
        constexpr uint32_t kMidTorso = sym::midTorsoBoneName;       // "_MidTorso"
        constexpr uint32_t kLeftUpLeg = sym::leftUpLegBoneName;     // "LeftUpLeg"
        constexpr uint32_t kLeftLowLeg = sym::leftLowLegBoneName;   // "LeftLowLeg"
        constexpr uint32_t kLeftFoot = sym::leftFootBoneName;       // "LeftFoot"
        constexpr uint32_t kHips = sym::hipsBoneName;               // "Hips"
        constexpr uint32_t kRightUpLeg = sym::rightUpLegBoneName;   // "RightUpLeg"
        constexpr uint32_t kRightLowLeg = sym::rightLowLegBoneName; // "RightLowLeg"
        constexpr uint32_t kRightFoot = sym::rightFootBoneName;     // "RightFoot"

        ee::Ptr<SceneBone> namedBone(ee::Ptr<SceneObject> object, uint32_t name)
        {
            return findModelBone(object, nameHash(ee::Ptr<const char>(name).get()));
        }

        // The ids of the system's arm points (a leg's and the torso's have 0).
        enum : uint32_t
        {
            kLeftUpArmPoint = 1,
            kLeftLowArmPoint = 2,
            kLeftHandPointOfSystem = 3,
            kLeftGripPoint = 4,
            kRightUpArmPoint = 5,
            kRightLowArmPoint = 6,
            kRightHandPointOfSystem = 7,
            kRightGripPoint = 8,
        };

        // The system's own points weigh this; a limb's links are this stiff.
        constexpr ee::Float kLimbPointMass = 0.5f;
        constexpr ee::Float kRightUpLegMass = 0.3f;
        constexpr ee::Float kArmLinkStiffness = 10.0f;
        constexpr ee::Float kLegLinkStiffness = 20.0f;
        constexpr ee::Float kLeftHandGripMass = 10.0f;
        constexpr ee::Float kLimbGripMass = 2.0f;
        constexpr ee::Float kTorsoSpaceStiffness = 20.0f;
        constexpr ee::Float kTorsoLinkStiffness = 30.0f;
        constexpr ee::Float kShoulderLinkStiffness = 20.0f;
        constexpr ee::Float kShouldersLinkStiffness = 100.0f;
        constexpr ee::Float kElbowsLinkStiffness = 1.0f;

        // The point keeps its x and z where the level is met.
        void keepSideways(ee::Ptr<AnimatePoint> point)
        {
            point->setAxisKept(0, 1);
            point->setAxisKept(2, 1);
        }
    }

    // buildFigureRig: the figure of a human attach with a body rig. The
    // figure loses its animator, is carried by the rider under the figure's
    // tag, and stands at the origin, at rest; the attach itself is hidden.
    // It gets a whole-body rig (kept in the attach's tag record), a plane
    // on the holder's "Hipsdeck" bone to sit on, and for each hand whose
    // data names a bone of a carried object, a grip there. Then, by point:
    // the chest, shoulders, elbows, hips and feet keep their x and z where
    // they meet the level; so does a hand, which with a grip does not
    // collide at all; the hips' bottom keeps x and z, or for the one
    // jointless attach does not collide; every other point does not collide.
    void buildFigureRig(ee::Ptr<SceneObject> attach)
    {
        ee::Local<Vec4> origin;
        ee::Local<Vec4> seatNormal;
        ee::Local<Vec4> seatPlace;
        ee::Local<Quaternion> upright;
        ee::Local<Vec4> leftPlace;
        ee::Local<Vec4> rightPlace;
        const ee::Ptr<SceneObject> carrier(animalCarrier(attach));
        attach::clearVector(origin);
        attach::clearVector(seatNormal);
        attach::clearVector(seatPlace);
        attach::clearVector(ee::Ptr<Vec4>(upright.address()));
        upright->w = 1.0f;
        const ee::Ptr<SceneObject> figure = attachFigure(attachFigureName(attach));
        if (pathman::objectHasAnimator(figure) != 0)
            pathman::freeAnimatorOf(figure);
        zge::attachCarriedObject(carrier, figure, {}, {}, {}, {});
        zge::setCarriedAnimalTag(ee::Address(figure), kCarryTagFigure);
        setSceneObjectShown(attach, 0);
        setObjectPosition(figure, origin);
        setObjectOrientation(figure, upright);
        resetAnimatorPose(figure);
        updatePose(figure);
        const ee::Ptr<SceneObject> holder = attachHolder(attach, carrier);
        // (Looked up as the game does; neither bone is used.)
        attachHolderBone(attach, carrier);
        findModelBone(attach, objectNameHash(attach));

        ee::Ptr<BodyRig> rig;
        if (objectNameHash(attach) == kJointlessAttachName)
        {
            rig = newWholeBodyRigWithoutJoints(figure);
            rig->system->setRoundsScale(kJointlessRoundsShare);
        }
        else
        {
            rig = newWholeBodyRig(figure);
        }
        setAttachFigureRig(attach, rig);
        const ee::Ptr<AnimateSystem> system = rig->system;

        const ee::Ptr<AnimatePlaneGroup> seat = addAnimatePlaneGroup(system);
        seatNormal->y = -1.0f;
        seatPlace->y = kSeatRise;
        if (findModelBone(holder, nameHash(ee::Ptr<const char>(kSeatBoneName).get())))
        {
            const ee::Ptr<AnimatePlane> plane =
                newAnimatePlane(seat, findModelBone(holder, nameHash(ee::Ptr<const char>(kSeatBoneName).get())), seatPlace, seatNormal);
            plane->setObject(holder);
        }

        const uint32_t leftTag = tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagLeftHand.carryTag);
        const uint32_t leftName = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagLeftHand.boneName));
        const ee::Ptr<SceneObject> leftObject = carriedObjectWithTag(attach, carrier, leftTag);
        const ee::Ptr<SceneBone> leftBone = carriedObjectBone(attach, carrier, leftTag, leftName);
        if (leftBone)
        {
            tagVector(objectComponent(attach), 0x40, kAttachTagLeftHand.place, leftPlace);
            const ee::Ptr<AnimatePoint> grip = attach::newObjectBonePoint(system, leftObject, leftBone, kPointHeld, leftPlace, kGripPointId, kGripMass);
            newAnimateBoneSpace(system, grip, leftBone, kGripSpaceStiffness);
            newOwnBoneConstraint(system, grip, rig->leftHand, kGripStiffness);
        }
        const uint32_t rightTag = tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagRightHand.carryTag);
        const uint32_t rightName = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagRightHand.boneName));
        const ee::Ptr<SceneObject> rightObject = carriedObjectWithTag(attach, carrier, rightTag);
        const ee::Ptr<SceneBone> rightBone = carriedObjectBone(attach, carrier, rightTag, rightName);
        if (rightBone)
        {
            tagVector(objectComponent(attach), 0x40, kAttachTagRightHand.place, rightPlace);
            const ee::Ptr<AnimatePoint> grip = attach::newObjectBonePoint(system, rightObject, rightBone, kPointHeld, rightPlace, kGripPointId, kGripMass);
            newAnimateBoneSpace(system, grip, rightBone, kGripSpaceStiffness);
            newOwnBoneConstraint(system, grip, rig->rightHand, kGripStiffness);
        }

        sortAnimatePivots(system);
        resetAnimatePoints(system);
        updateFigureAnimation(attach, 0.0f);

        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
        {
            switch (point->id)
            {
            case kChestPoint:
            case kLeftShoulderPoint:
            case kLeftElbowPoint:
            case kLeftHipPoint:
            case kLeftFootPoint:
            case kRightShoulderPoint:
            case kRightElbowPoint:
            case kRightHipPoint:
            case kRightFootPoint:
                keepSideways(point);
                break;
            case kLeftHandPoint:
                if (leftBone)
                    point->setCollisionOff(1);
                keepSideways(point);
                break;
            case kRightHandPoint:
                if (rightBone)
                    point->setCollisionOff(1);
                keepSideways(point);
                break;
            case kHipsBottomPoint:
                if (objectNameHash(attach) == kJointlessAttachName)
                    point->setCollisionOff(1);
                else
                    keepSideways(point);
                break;
            default:
                point->setCollisionOff(1);
                break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(buildFigureRig, Verified);

    // buildFigureSystem: the figure of a human attach without a body rig.
    // The figure is carried by the rider under the figure's tag, loses its
    // animator and stands at rest; the attach itself is hidden. It gets an
    // animate system of its own (kept in the attach's tag record) with, for
    // each hand and foot whose data names a bone of a carried object, the
    // limb's three points, a grip on that bone tied to the limb's end, and
    // pivots that turn the limb's two bones. With an arm it gets the
    // torso: a chest point and a held mid-torso point, linked to each
    // shoulder, and pivots for both bones (from both shoulders when it has
    // both). At the end the figure is attached to the attach's own bone,
    // everything is posed, and the system starts settling.
    void buildFigureSystem(ee::Ptr<SceneObject> attach)
    {
        ee::Local<Vec4> origin;
        ee::Local<Quaternion> upright;
        ee::Local<Vec4> leftArmPlace;
        ee::Local<Vec4> rightArmPlace;
        ee::Local<Vec4> torsoPlace;
        ee::Local<Vec4> leftLegPlace;
        ee::Local<Vec4> rightLegPlace;
        // A carried object a hand grips that is not the holder (the right
        // hand's when both are): it is posed with the rest at the end.
        ee::Ptr<SceneObject> gripped;
        const ee::Ptr<SceneObject> carrier(animalCarrier(attach));
        attach::clearVector(origin);
        attach::clearVector(ee::Ptr<Vec4>(upright.address()));
        upright->w = 1.0f;
        const ee::Ptr<SceneObject> figure = attachFigure(attachFigureName(attach));
        zge::attachCarriedObject(carrier, figure, {}, {}, {}, {});
        zge::setCarriedAnimalTag(ee::Address(figure), kCarryTagFigure);
        setSceneObjectShown(attach, 0);
        if (pathman::objectHasAnimator(figure) != 0)
            pathman::freeAnimatorOf(figure);
        resetAnimatorPose(figure);
        setObjectPosition(figure, origin);
        setObjectOrientation(attach, upright);
        setObjectOrientation(figure, upright);
        updatePose(figure);
        updatePose(attach);
        ee::Ptr<AnimateSystem> system = newAnimateSystem(figure);
        const ee::Ptr<SceneObject> holder = attachHolder(attach, carrier);
        // (Looked up as the game does; the holder's bone is not used.)
        attachHolderBone(attach, carrier);
        const ee::Ptr<SceneBone> ownBone = findModelBone(attach, objectNameHash(attach));

        const uint32_t leftTag = tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagLeftHand.carryTag);
        const uint32_t leftName = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagLeftHand.boneName));
        const ee::Ptr<SceneObject> leftObject = carriedObjectWithTag(attach, carrier, leftTag);
        const ee::Ptr<SceneBone> leftBone = carriedObjectBone(attach, carrier, leftTag, leftName);
        if (leftBone)
        {
            if (!system)
                system = newAnimateSystem(figure);
            gripped = leftObject == holder ? ee::Ptr<SceneObject>() : leftObject;
            *leftArmPlace = kZero;
            const ee::Ptr<AnimatePoint> upArm =
                newBoneAttachedPoint(system, namedBone(figure, kLeftUpArm), kPointFree, leftArmPlace, kLeftUpArmPoint, kLimbPointMass);
            const ee::Ptr<AnimatePoint> lowArm =
                newBoneAttachedPoint(system, namedBone(figure, kLeftLowArm), kPointFree, leftArmPlace, kLeftLowArmPoint, kLimbPointMass);
            const ee::Ptr<AnimatePoint> hand =
                newBoneAttachedPoint(system, namedBone(figure, kLeftHand), kPointFree, leftArmPlace, kLeftHandPointOfSystem, kLimbPointMass);
            tagVector(objectComponent(attach), 0x40, kAttachTagLeftHand.place, leftArmPlace);
            const ee::Ptr<AnimatePoint> grip =
                attach::newObjectBonePoint(system, leftObject, leftBone, kPointHeld, leftArmPlace, kLeftGripPoint, kLeftHandGripMass);
            newAnimateBoneSpace(system, grip, leftBone, kGripSpaceStiffness);
            newDistanceConstraint(system, upArm, lowArm, kArmLinkStiffness);
            newDistanceConstraint(system, lowArm, hand, kArmLinkStiffness);
            newOwnBoneConstraint(system, grip, hand, kGripStiffness);
            attach::newRootedBonePivot(system, namedBone(figure, kLeftLowArm), lowArm, hand);
            attach::newRootedBonePivot(system, namedBone(figure, kLeftUpArm), upArm, lowArm);
        }

        const uint32_t rightTag = tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagRightHand.carryTag);
        const uint32_t rightName = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagRightHand.boneName));
        const ee::Ptr<SceneObject> rightObject = carriedObjectWithTag(attach, carrier, rightTag);
        const ee::Ptr<SceneBone> rightBone = carriedObjectBone(attach, carrier, rightTag, rightName);
        if (rightBone)
        {
            if (!system)
                system = newAnimateSystem(figure);
            if (rightObject != holder)
                gripped = rightObject;
            *rightArmPlace = kZero;
            const ee::Ptr<AnimatePoint> upArm =
                newBoneAttachedPoint(system, namedBone(figure, kRightUpArm), kPointFree, rightArmPlace, kRightUpArmPoint, kLimbPointMass);
            const ee::Ptr<AnimatePoint> lowArm =
                newBoneAttachedPoint(system, namedBone(figure, kRightLowArm), kPointFree, rightArmPlace, kRightLowArmPoint, kLimbPointMass);
            const ee::Ptr<AnimatePoint> hand =
                newBoneAttachedPoint(system, namedBone(figure, kRightHand), kPointFree, rightArmPlace, kRightHandPointOfSystem, kLimbPointMass);
            tagVector(objectComponent(attach), 0x40, kAttachTagRightHand.place, rightArmPlace);
            const ee::Ptr<AnimatePoint> grip =
                attach::newObjectBonePoint(system, rightObject, rightBone, kPointHeld, rightArmPlace, kRightGripPoint, kLimbGripMass);
            newAnimateBoneSpace(system, grip, rightBone, kGripSpaceStiffness);
            newDistanceConstraint(system, upArm, lowArm, kArmLinkStiffness);
            newDistanceConstraint(system, lowArm, hand, kArmLinkStiffness);
            newOwnBoneConstraint(system, grip, hand, kGripStiffness);
            attach::newRootedBonePivot(system, namedBone(figure, kRightLowArm), lowArm, hand);
            attach::newRootedBonePivot(system, namedBone(figure, kRightUpArm), upArm, lowArm);
        }

        if (leftBone || rightBone)
        {
            attach::clearVector(torsoPlace);
            ee::Ptr<AnimatePoint> leftUpArm;
            ee::Ptr<AnimatePoint> rightUpArm;
            ee::Ptr<AnimatePoint> leftLowArm;
            ee::Ptr<AnimatePoint> rightLowArm;
            for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
            {
                if (point->id == kLeftUpArmPoint)
                    leftUpArm = point;
                if (point->id == kRightUpArmPoint)
                    rightUpArm = point;
                if (point->id == kLeftLowArmPoint)
                    leftLowArm = point;
                if (point->id == kRightLowArmPoint)
                    rightLowArm = point;
            }
            const ee::Ptr<AnimatePoint> chest = newBoneAttachedPoint(system, namedBone(figure, kChest), kPointFree, torsoPlace, 0, kLimbPointMass);
            const ee::Ptr<AnimatePoint> midTorso = newBoneAttachedPoint(system, namedBone(figure, kMidTorso), kPointHeld, torsoPlace, 0, kLimbPointMass);
            newAnimateBoneSpace(system, midTorso, namedBone(figure, kMidTorso), kTorsoSpaceStiffness);
            newDistanceConstraint(system, midTorso, chest, kTorsoLinkStiffness);
            if (leftUpArm)
            {
                newDistanceConstraint(system, chest, leftUpArm, kShoulderLinkStiffness);
                newDistanceConstraint(system, midTorso, leftUpArm, kShoulderLinkStiffness);
            }
            if (rightUpArm)
            {
                newDistanceConstraint(system, chest, rightUpArm, kShoulderLinkStiffness);
                newDistanceConstraint(system, midTorso, rightUpArm, kShoulderLinkStiffness);
            }
            if (leftUpArm && rightUpArm)
            {
                attach::clearVector(leftLegPlace);
                newDistanceConstraint(system, leftUpArm, rightUpArm, kShouldersLinkStiffness);
                attach::newFramePivot(system, namedBone(figure, kChest), rightUpArm, leftUpArm, chest, 1);
                attach::newFramePivot(system, namedBone(figure, kMidTorso), rightUpArm, leftUpArm, chest, 1);
                // (Left in the two vectors; nothing reads them after.)
                torsoPlace->z = -0.2f;
                leftLegPlace->z = -1.0f;
                leftLegPlace->x = 0.0f;
                newDistanceConstraint(system, leftLowArm, rightLowArm, kElbowsLinkStiffness);
            }
            else if (leftUpArm)
            {
                attach::newRootedBonePivot(system, namedBone(figure, kChest), chest, leftUpArm);
                attach::newRootedBonePivot(system, namedBone(figure, kMidTorso), midTorso, leftUpArm);
            }
            else
            {
                attach::newRootedBonePivot(system, namedBone(figure, kChest), chest, rightUpArm);
                attach::newRootedBonePivot(system, namedBone(figure, kMidTorso), midTorso, rightUpArm);
            }
        }

        const uint32_t leftFootTag = tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagLeftFoot.carryTag);
        const uint32_t leftFootName = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagLeftFoot.boneName));
        const ee::Ptr<SceneObject> leftFootObject = carriedObjectWithTag(attach, carrier, leftFootTag);
        const ee::Ptr<SceneBone> leftFootBone = carriedObjectBone(attach, carrier, leftFootTag, leftFootName);
        if (leftFootBone)
        {
            if (!system)
                system = newAnimateSystem(figure);
            *leftLegPlace = kZero;
            const ee::Ptr<AnimatePoint> upLeg = newBoneAttachedPoint(system, namedBone(figure, kLeftUpLeg), kPointHeld, leftLegPlace, 0, kLimbPointMass);
            const ee::Ptr<AnimatePoint> lowLeg = newBoneAttachedPoint(system, namedBone(figure, kLeftLowLeg), kPointFree, leftLegPlace, 0, kLimbPointMass);
            const ee::Ptr<AnimatePoint> foot = newBoneAttachedPoint(system, namedBone(figure, kLeftFoot), kPointFree, leftLegPlace, 0, kLimbPointMass);
            tagVector(objectComponent(attach), 0x40, kAttachTagLeftFoot.place, leftLegPlace);
            const ee::Ptr<AnimatePoint> grip = attach::newObjectBonePoint(system, leftFootObject, leftFootBone, kPointHeld, leftLegPlace, 0, kLimbGripMass);
            newAnimateBoneSpace(system, upLeg, namedBone(figure, kHips), kGripSpaceStiffness);
            newAnimateBoneSpace(system, grip, leftFootBone, kGripSpaceStiffness);
            newDistanceConstraint(system, upLeg, lowLeg, kLegLinkStiffness);
            newDistanceConstraint(system, lowLeg, foot, kLegLinkStiffness);
            newOwnBoneConstraint(system, grip, foot, kGripStiffness);
            attach::newRootedBonePivot(system, namedBone(figure, kLeftUpLeg), upLeg, lowLeg);
            attach::newRootedBonePivot(system, namedBone(figure, kLeftLowLeg), lowLeg, foot);
        }

        const uint32_t rightFootTag = tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagRightFoot.carryTag);
        const uint32_t rightFootName = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagRightFoot.boneName));
        const ee::Ptr<SceneObject> rightFootObject = carriedObjectWithTag(attach, carrier, rightFootTag);
        const ee::Ptr<SceneBone> rightFootBone = carriedObjectBone(attach, carrier, rightFootTag, rightFootName);
        if (rightFootBone)
        {
            if (!system)
                system = newAnimateSystem(figure);
            *rightLegPlace = kZero;
            const ee::Ptr<AnimatePoint> upLeg = newBoneAttachedPoint(system, namedBone(figure, kRightUpLeg), kPointHeld, rightLegPlace, 0, kRightUpLegMass);
            const ee::Ptr<AnimatePoint> lowLeg = newBoneAttachedPoint(system, namedBone(figure, kRightLowLeg), kPointFree, rightLegPlace, 0, kLimbPointMass);
            const ee::Ptr<AnimatePoint> foot = newBoneAttachedPoint(system, namedBone(figure, kRightFoot), kPointFree, rightLegPlace, 0, kLimbPointMass);
            tagVector(objectComponent(attach), 0x40, kAttachTagRightFoot.place, rightLegPlace);
            const ee::Ptr<AnimatePoint> grip = attach::newObjectBonePoint(system, rightFootObject, rightFootBone, kPointHeld, rightLegPlace, 0, kLimbGripMass);
            newAnimateBoneSpace(system, upLeg, namedBone(figure, kHips), kGripSpaceStiffness);
            newAnimateBoneSpace(system, grip, rightFootBone, kGripSpaceStiffness);
            newDistanceConstraint(system, upLeg, lowLeg, kLegLinkStiffness);
            newDistanceConstraint(system, lowLeg, foot, kLegLinkStiffness);
            newOwnBoneConstraint(system, grip, foot, kGripStiffness);
            attach::newRootedBonePivot(system, namedBone(figure, kRightUpLeg), upLeg, lowLeg);
            attach::newRootedBonePivot(system, namedBone(figure, kRightLowLeg), lowLeg, foot);
        }

        setAttachFigureSystem(attach, system);
        origin->x = 0.0f;
        origin->y = 0.0f;
        origin->z = 0.0f;
        setObjectPosition(figure, origin);
        setObjectOrientation(figure, upright);
        attachObject(figure, attach, ownBone);
        // The pivots do not turn the bones while they are put in order.
        system->setPivotsSkipped(1);
        sortAnimatePivots(system);
        if (gripped)
        {
            markPoseStale(gripped);
            updatePose(gripped);
        }
        markPoseStale(holder);
        markPoseStale(attach);
        markPoseStale(figure);
        updatePose(holder);
        updatePose(attach);
        updatePose(figure);
        system->setPivotsSkipped(kFigureSettled + 1);
        resetAnimatePoints(system);
        updateFigureAnimation(attach, 0.0f);
        attach::nudgeAnimation(figure);
        resetAnimatePoints(system);
    }
    READABLE_FUNCTION_WAITS(buildFigureSystem, Verified);
}
