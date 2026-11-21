// BMX XXX's attaches (attach.h): what the engine's carried objects call for
// an attach a rider carries: its set-up on the rider, and its two updates
// each frame. BMX XXX's own code.

#include "attach.h"
#include "attach_calls.h"

#include "bmx/pathman/pathman_calls.h"
#include "bmx/pathman/pathman_phrases.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/box.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/carried_objects.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/attach/attach_carried.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        // A swinging attach: three points on its holder's bone (two held:
        // the top and one below it; one free, pulled to the lower one), and
        // the attach's own bone turned along the line from the top to the
        // free one.
        constexpr ee::Float kSwingPointMass = 2.0f;
        constexpr ee::Float kSwingDrop = 2.0f;       // the lower held point under the top
        constexpr ee::Float kSwingFreeRise = 0.5f;   // the free point over the lower held one
        constexpr ee::Float kSwingSpaceStiffness = 10.0f;
        constexpr ee::Float kSwingLinkStiffness = 2.0f;
        constexpr ee::Float kSwingPull = 100.0f;
        constexpr ee::Float kSwingDamping = 1.0f;
        // How fast the attach's bone goes back to its rest each update.
        constexpr ee::Float kSwingRestBlend = 0.3f;
        constexpr ee::Float kSwingStep = 0.015f;
        constexpr uint32_t kSwingSubSteps = 4;

        // A boxed attach is kept inside its holder's box, less its own box
        // and this margin; the box's floor is this much lower.
        constexpr ee::Float kBoxMargin = 0.05f;
        constexpr ee::Float kBoxFloorDrop = 0.2f;
        // A random number's 31 bits as a fraction of 1.
        constexpr ee::Float kRandomUnit = 0x1p-31f;
        // The boxed attach's point is pulled to its place: the way there
        // (no more than 10) doubled onto its velocity.
        constexpr ee::Float kBoxFarSquared = 100.0f;
        constexpr ee::Float kBoxFarPull = 10.0f;
        constexpr ee::Float kBoxPullScale = 2.0f;

        // The figure of a rigged human attach rides this much over its holder,
        // and a figure with a rig of its own this much under its hips' point.
        constexpr ee::Float kFigureRise = 0.5f;
        constexpr ee::Float kFigureHipsDrop = 0.1f;
        constexpr ee::Float kFigureStep = 0.015f;
        constexpr uint32_t kFigureFreeSubSteps = 3;


        ee::Float randomBetween(ee::Float low, ee::Float high)
        {
            return ee::toFloat(static_cast<int32_t>(randomNumber() & 0x7FFFFFFF)) * kRandomUnit * (high - low) + low;
        }

        // The object's own bone: the bone of its model named as the object is.
        ee::Ptr<SceneBone> ownBone(ee::Ptr<SceneObject> object) { return findModelBone(object, objectNameHash(object)); }

        // The attach's bone eased back to its rest rotation.
        void easeBoneToRest(ee::Ptr<SceneObject> attach, ee::Ptr<Quaternion> blended, ee::Ptr<Quaternion> rest)
        {
            const ee::Ptr<Quaternion> rotation = boneRotation(findBoneChannel(attach->animator, objectNameHash(attach)));
            quaternionNlerp(blended.get(), rest.get(), rotation.get(), kSwingRestBlend);
            setBoneRotation(findBoneChannel(attach->animator, objectNameHash(attach)), blended);
        }

        // A count as the float the game compares it with (an unsigned
        // conversion: a count past 31 bits is halved, rounded to odd, and
        // doubled).
        ee::Float countAsFloat(int32_t count)
        {
            if (count >= 0)
                return ee::toFloat(count);
            const uint32_t bits = static_cast<uint32_t>(count);
            const ee::Float half = ee::toFloat(static_cast<int32_t>((bits >> 1) | (bits & 1)));
            return half + half;
        }
    }

    // setUpAttach (the carried object's set-up, once its carrier has it):
    // the attach takes its carry tag and is shown. With a holder among the
    // carrier's objects it is placed on it: a boxed attach somewhere inside
    // the holder's box, with a point that six planes keep in; any other on
    // the holder's bone, and when its data says so with an animator and the
    // points it swings on. A human attach then gets its figure's rig or
    // system. At the end everything is posed and updated three times, and a
    // human attach's figure starts an animation; a figure without a rig
    // starts settling.
    void setUpAttach(ee::Ptr<SceneObject> attach, ee::Float step)
    {
        ee::Local<Vec4> place;
        ee::Local<Quaternion> orientation;
        ee::Local<Vec4> point;
        ee::Local<Vec4> normal;
        ee::Local<Vec4> low;
        ee::Local<Vec4> high;
        const ee::Ptr<SceneObject> carrier(animalCarrier(attach));
        if (!carrier)
            return;
        zge::setCarriedAnimalTag(ee::Address(attach), attachCarryTag(attach));
        setSceneObjectShown(attach, 1);
        const ee::Ptr<SceneObject> holder = attachHolder(attach, carrier);
        if (holder && attachHolderTag(attach) != kCarryTagBoxed)
        {
            attach::clearVector(place);
            attach::clearVector(ee::Ptr<Vec4>(orientation.address()));
            orientation->w = 1.0f;
            const ee::Ptr<SceneBone> holderBone = attachHolderBone(attach, carrier);
            attachPlace(attach, place);
            setObjectOrientation(attach, orientation);
            setObjectPosition(attach, place);
            updatePose(holder);
            attachObject(attach, holder, holderBone);
            markPoseStale(attach);
            updatePose(attach);
            setAttachSecondSystem(attach, {});
            setAttachSwingSystem(attach, {});
            const ee::Ptr<SceneBone> bone = ownBone(attach);
            if (attachSwings(attach) != 0 || attachHasAnimator(attach) != 0)
            {
                attach::remakeObjectAnimator(attach);
                if (bone && attachSwings(attach) != 0)
                {
                    const ee::Ptr<AnimateSystem> system = newAnimateSystem(attach);
                    setAttachSwingSystem(attach, system);
                    *point = *place;
                    const ee::Ptr<AnimatePoint> top = attach::newObjectBonePoint(system, holder, holderBone, kPointHeld, point, 0, kSwingPointMass);
                    point->y = point->y - kSwingDrop;
                    const ee::Ptr<AnimatePoint> lower = attach::newObjectBonePoint(system, holder, holderBone, kPointHeld, point, 1, kSwingPointMass);
                    point->y = point->y + kSwingFreeRise;
                    const ee::Ptr<AnimatePoint> free = attach::newObjectBonePoint(system, holder, holderBone, kPointFree, point, 2, kSwingPointMass);
                    newAnimateBoneSpace(system, top, holderBone, kSwingSpaceStiffness);
                    newAnimateBoneSpace(system, lower, holderBone, kSwingSpaceStiffness);
                    newDistanceConstraint(system, top, free, kSwingLinkStiffness);
                    newAnimatePull(system, lower, free, kSwingPull, kSwingDamping);
                    attach::newRootedBonePivot(system, bone, top, free);
                    resetAnimatePoints(system);
                }
            }
            if (attachCarryTag(attach) == kCarryTagHuman)
            {
                if (attachFigureHasRig(attach) != 0)
                    buildFigureRig(attach);
                else
                    buildFigureSystem(attach);
            }
        }
        else if (holder)
        {
            attach::clearVector(point);
            attach::clearVector(normal);
            attach::clearVector(low);
            attach::clearVector(high);
            const ee::Ptr<Node> holderNode = objectLevelNode(holder);
            if (holderNode)
            {
                *low = holderNode->modelBounds().low;
                *high = holderNode->modelBounds().high;
                const ee::Ptr<Node> node = objectLevelNode(attach);
                if (node)
                {
                    const Box &own = node->modelBounds();
                    low->x = low->x - (own.low.x - kBoxMargin);
                    low->y = low->y - (own.low.y - kBoxMargin);
                    low->z = low->z - (own.low.z - kBoxMargin);
                    high->x = high->x - (own.high.x + kBoxMargin);
                    high->y = high->y - (own.high.y + kBoxMargin);
                    high->z = high->z - (own.high.z + kBoxMargin);
                    // A box too small for the attach: both sides at the high one.
                    if (high->x < low->x)
                        low->x = high->x;
                    if (high->y < low->y)
                        low->y = high->y;
                    if (high->z < low->z)
                        low->z = high->z;
                }
                const ee::Ptr<AnimateSystem> system = newAnimateSystem(holder);
                setAttachSecondSystem(attach, {});
                setAttachSwingSystem(attach, {});
                point->x = randomBetween(low->x, high->x);
                point->y = randomBetween(low->y, high->y);
                point->z = randomBetween(low->z, high->z);
                attach::newObjectBonePoint(system, attach, ownBone(holder), kPointFree, point, 0, kSwingPointMass);

                // The box's six sides, each a plane group of its own on
                // the holder's bone: the low sides face up the axis, the
                // high sides down it.
                ee::Ptr<AnimatePlaneGroup> side = addAnimatePlaneGroup(system);
                *point = kZero;
                *normal = kZero;
                point->x = low->x;
                normal->x = 1.0f;
                newAnimatePlane(side, ownBone(holder), point, normal);
                side = addAnimatePlaneGroup(system);
                *point = kZero;
                *normal = kZero;
                point->y = low->y - kBoxFloorDrop;
                normal->y = 1.0f;
                newAnimatePlane(side, ownBone(holder), point, normal);
                side = addAnimatePlaneGroup(system);
                *point = kZero;
                *normal = kZero;
                point->z = low->z;
                normal->z = 1.0f;
                newAnimatePlane(side, ownBone(holder), point, normal);
                side = addAnimatePlaneGroup(system);
                *point = kZero;
                *normal = kZero;
                point->x = high->x;
                normal->x = -1.0f;
                newAnimatePlane(side, ownBone(holder), point, normal);
                side = addAnimatePlaneGroup(system);
                *point = kZero;
                *normal = kZero;
                point->y = high->y;
                normal->y = -1.0f;
                newAnimatePlane(side, ownBone(holder), point, normal);
                side = addAnimatePlaneGroup(system);
                *point = kZero;
                *normal = kZero;
                point->z = high->z;
                normal->z = -1.0f;
                newAnimatePlane(side, ownBone(holder), point, normal);

                setAttachSwingSystem(attach, system);
                resetAnimatePoints(system);
                attachObject(attach, holder, ownBone(holder));
            }
        }

        if (attachHolder(attach, carrier))
            updatePose(attachHolder(attach, carrier));
        if (attachCarryTag(attach) == kCarryTagHuman && attachFigure(attachFigureName(attach)))
        {
            markPoseStale(attachFigure(attachFigureName(attach)));
            updatePose(attachFigure(attachFigureName(attach)));
        }
        updatePose(attach);
        updateAttachFigure(attach, step);
        updatePose(attach);
        updateAttachFigure(attach, step);
        updateAttachSwing(attach, 0.0f);
        updatePose(attach);
        updateAttachFigure(attach, step);
        if (attachCarryTag(attach) != kCarryTagHuman || !attachFigure(attachFigureName(attach)))
            return;
        const ee::Ptr<Motion> animation = findLibraryAnimation(pickFigureAnimation(attach));
        markPoseStale(attachFigure(attachFigureName(attach)));
        updatePose(attachFigure(attachFigureName(attach)));
        if (!animation)
            return;
        attach::nudgeAnimation(attachFigure(attachFigureName(attach)));
        markPoseStale(attachFigure(attachFigureName(attach)));
        updatePose(attachFigure(attachFigureName(attach)));
        const ee::Ptr<AnimateSystem> figureSystem = attachFigureSystem(attach);
        if (attachFigureHasRig(attach) == 0)
            figureSystem->setPivotsSkipped(kFigureSettled + 1);
    }
    READABLE_FUNCTION_WAITS(setUpAttach, Verified);

    // attachLetGo (the carried object's let-go call, detachAnimal's): nothing.
    void attachLetGo(ee::Ptr<zge::SceneObject> carried, ee::Float step) {}
    READABLE_FUNCTION(attachLetGo, Verified);

    // updateAttachSwing (the carried object's first update of a frame; the
    // frame's time is not used): the swing system moves four steps and the
    // attach's bone eases back to its rest (not a boxed attach's). Then the
    // figure of a human attach on a holder: a rigged figure is put over the
    // holder, turned as it is; its rig or system moves twice, a posed step
    // and three free ones each time; its next animation starts when due;
    // a figure without a rig starts settling again.
    void updateAttachSwing(ee::Ptr<SceneObject> attach, ee::Float step)
    {
        (void)step;
        ee::Local<Quaternion> blended;
        ee::Local<Quaternion> rest;
        ee::Local<Vec4> position;
        const ee::Ptr<AnimateSystem> swing = attachSwingSystem(attach);
        if (swing)
        {
            attach::clearVector(ee::Ptr<Vec4>(rest.address()));
            rest->w = 1.0f;
            if (zge::attachedParent(attach))
                updatePose(zge::attachedParent(attach));
            resetAnimatePoints(swing);
            for (uint32_t subStep = 0; subStep < kSwingSubSteps; ++subStep)
                attach::animate(swing, 1, swing->pivotsSkippedWord(), kSwingStep);
            if (attachHolderTag(attach) != kCarryTagBoxed)
                easeBoneToRest(attach, blended, rest);
            resetAnimatePoints(swing);
        }

        ee::Ptr<AnimateSystem> system = attachFigureSystem(attach);
        if (!system)
            return;
        const ee::Ptr<SceneObject> carrier(animalCarrier(attach));
        if (!carrier)
            return;
        const ee::Ptr<SceneObject> holder = attachHolder(attach, carrier);
        if (!holder)
            return;
        ee::Ptr<SceneObject> figure;
        if (attachFigureHasRig(attach) == 0)
        {
            figure = system->object;
        }
        else
        {
            system = attachFigureRig(attach)->system;
            figure = system->object;
            *position = *objectPosition(holder);
            position->z = position->z + kFigureRise;
            setObjectPosition(figure, position);
            setObjectOrientation(figure, objectOrientation(holder));
        }
        if (holder)
            updatePose(holder);
        markPoseStale(attach);
        updatePose(figure);
        for (uint32_t pass = 0; pass < 2; ++pass)
        {
            resetAnimatePoints(system);
            attach::animate(system, 1, system->pivotsSkippedWord(), kFigureStep);
            for (uint32_t subStep = 0; subStep < kFigureFreeSubSteps; ++subStep)
                attach::animate(system, 0, system->pivotsSkippedWord(), kFigureStep);
            resetAnimatePoints(system);
            if (pass == 0)
            {
                markPoseStale(figure);
                updatePose(figure);
            }
        }
        updateFigureAnimation(attach, 0.0f);
        markPoseStale(figure);
        updatePose(figure);
        if (attachFigureHasRig(attach) == 0)
            system->setPivotsSkipped(kFigureSettled + 1);
    }
    READABLE_FUNCTION_WAITS(updateAttachSwing, Verified);

    // updateAttachFigure (the carried object's second update of a frame):
    // the blinking attach counts the update; the holder of any but a human
    // attach is posed; the swing system moves a step: a hanging attach's
    // bone eases back to its rest, a boxed attach is put where its point
    // is, held inside the box's sides, and the point is pulled to its place
    // on the bone. A human attach says its "ride" phrase now and then, and
    // its figure moves: one with an animation gets the next when due; one
    // with a rig is put under its hips' point; one with a system moves a
    // step and settles, unless the rider's state holds it still. The
    // figure is shown in the attach's place. While an attach blinks, the
    // figure (or the attach without one) is shown or hidden as the blink
    // has it, whichever attach this is.
    // Note: a rigged figure whose rig has no system is read at address 0
    // for its object, as the game ships it.
    void updateAttachFigure(ee::Ptr<SceneObject> attach, ee::Float step)
    {
        ee::Local<Quaternion> blended;
        ee::Local<Quaternion> rest;
        ee::Local<Vec4> point;
        ee::Local<Vec4> move;
        ee::Local<Vec4> inBox;
        ee::Local<Vec4> none;
        uint32_t posed = 1;
        ee::Ptr<SceneObject> blinking = attach;
        if (g_attachBlink->on != 0 && g_attachBlink->attach == attach)
            g_attachBlink->frames += 1;
        if (attachCarryTag(attach) != kCarryTagHuman)
        {
            const ee::Ptr<SceneObject> holder = attachHolder(attach, ee::Ptr<SceneObject>(animalCarrier(attach)));
            if (holder)
            {
                updatePose(holder);
                markPoseStale(holder);
            }
        }

        const ee::Ptr<AnimateSystem> swing = attachSwingSystem(attach);
        if (swing)
        {
            attach::clearVector(ee::Ptr<Vec4>(rest.address()));
            rest->w = 1.0f;
            if (zge::attachedParent(attach))
                updatePose(zge::attachedParent(attach));
            attach::animate(swing, 1, swing->pivotsSkippedWord(), step);
            posed = 0;
            if (attachHolderTag(attach) != kCarryTagBoxed)
                easeBoneToRest(attach, blended, rest);
            if (attachHolderTag(attach) == kCarryTagBoxed)
            {
                attach::clearVector(none);
                const ee::Ptr<AnimatePoint> boxed = swing->points;
                objectBonePoint(swing->object, boxed->bone, none, point, 0);
                *inBox = boxed->position - *point;
                // Each side holds its own axis: a low side (its normal up
                // the axis) from below, a high side from above.
                for (ee::Ptr<AnimatePlaneGroup> side = swing->planeGroups; side; side = side->next)
                {
                    const ee::Ptr<AnimatePlane> plane = side->planes;
                    if (plane->normal.x != 0.0f)
                    {
                        if (0.0f < plane->normal.x ? inBox->x < plane->place.x : plane->place.x < inBox->x)
                            inBox->x = plane->place.x;
                    }
                    else if (plane->normal.y != 0.0f)
                    {
                        if (0.0f < plane->normal.y ? inBox->y < plane->place.y : plane->place.y < inBox->y)
                            inBox->y = plane->place.y;
                    }
                    else if (plane->normal.z != 0.0f)
                    {
                        if (0.0f < plane->normal.z ? inBox->z < plane->place.z : plane->place.z < inBox->z)
                            inBox->z = plane->place.z;
                    }
                }
                setObjectPosition(attach, inBox);
                const ee::Ptr<AnimatePoint> pulled = swing->points;
                objectBonePoint(swing->object, pulled->bone, &pulled->boneOffset, point, 0);
                *move = *point - swing->points->position;
                if (kBoxFarSquared < squaredLength(*move))
                {
                    *move = unit(*move);
                    *move = *move * kBoxFarPull;
                }
                *move = *move * kBoxPullScale;
                swing->points->velocity = swing->points->velocity + *move;
            }
        }

        const ee::Ptr<AnimateSystem> system = attachFigureSystem(attach);
        if (system)
        {
            sayRidePhrase(attachFigure(attachFigureName(attach)));
            const ee::Ptr<SceneObject> holder = attachHolder(attach, ee::Ptr<SceneObject>(animalCarrier(attach)));
            ee::Ptr<AnimateSystem> shown = system;
            if (pathman::objectHasAnimator(system->object) != 0)
            {
                updateFigureAnimation(attach, step);
            }
            else if (attachFigureHasRig(attach) != 0)
            {
                const ee::Ptr<BodyRig> rig = attachFigureRig(attach);
                if (holder)
                    updatePose(holder);
                shown = rig->system;
                if (shown)
                {
                    attach::animate(shown, 0, shown->pivotsSkippedWord(), step);
                    *point = rig->hipsBottom->position;
                    point->y = point->y - kFigureHipsDrop;
                    setObjectPosition(shown->object, point);
                }
            }
            else
            {
                const ee::Ptr<rider::Rider> rider(animalCarrier(attach));
                // (States 0x10, 0x11, 0xC and 0xD have no name in rider.h yet.)
                const bool heldStill = rider::physicsState(rider) == 0x10 || rider::physicsState(rider) == 0x11 ||
                                       rider::physicsState(rider) == 0xC || rider::physicsState(rider) == 0xD ||
                                       rider::physicsState(rider) == rider::kPhysicsBail;
                if (!heldStill)
                {
                    attach::animate(system, posed, system->pivotsSkippedWord(), step);
                    if (system->pivotsSkippedWord() == kFigureSettled)
                        system->setPivotsSkipped(0);
                    else if (system->pivotsSkippedWord() > kFigureSettled)
                        system->setPivotsSkipped(system->pivotsSkippedWord() - 1);
                }
            }
            blinking = shown->object;
            setSceneObjectShown(attach, 0);
            setSceneObjectShown(shown->object, 1);
        }

        if (g_attachBlink->on == 0)
            return;
        if (g_attachBlink->period < countAsFloat(g_attachBlink->frames))
        {
            const ee::Float shorter = g_attachBlink->period - kBlinkPeriodStep;
            g_attachBlink->frames = 0;
            g_attachBlink->shown = ~g_attachBlink->shown;
            g_attachBlink->period = shorter < kBlinkLeastPeriod ? kBlinkLeastPeriod : shorter;
        }
        setSceneObjectShown(blinking, g_attachBlink->shown);
    }
    READABLE_FUNCTION_WAITS(updateAttachFigure, Verified);
}
