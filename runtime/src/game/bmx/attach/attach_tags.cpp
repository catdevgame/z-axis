// BMX XXX's attaches (attach.h): what the level's data gives each one and
// what it keeps for itself, read from and written to the tag record of its
// scene object's component. BMX XXX's own code.

#include "attach.h"
#include "attach_calls.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/random.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/carried_objects.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/attach/attach_tags.cpp.h"

namespace game::bmx
{
    using namespace zge;

    // attachState: what a save keeps of the attach.
    void attachState(ee::Ptr<SceneObject> attach, ee::Ptr<AttachState> out)
    {
        out->carried = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagCarried));
        out->player = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagPlayer));
    }
    READABLE_FUNCTION(attachState, Verified);

    // setAttachState
    void setAttachState(ee::Ptr<SceneObject> attach, ee::Ptr<const AttachState> state)
    {
        setTagWord(objectComponent(attach), kAttachTagWords, kAttachTagCarried, state->carried);
        game::tail(setTagWord, objectComponent(attach), kAttachTagWords, kAttachTagPlayer, state->player);
    }
    READABLE_FUNCTION(setAttachState, Verified);

    // attachCarryTag: its tag among what the rider carries.
    uint32_t attachCarryTag(ee::Ptr<SceneObject> attach)
    {
        return tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagCarryTag);
    }
    READABLE_FUNCTION(attachCarryTag, Verified);

    // attachHolderTag: the tag of the carried object it hangs on.
    uint32_t attachHolderTag(ee::Ptr<SceneObject> attach)
    {
        return tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagHolderTag);
    }
    READABLE_FUNCTION(attachHolderTag, Verified);

    // attachHolder: the object it hangs on: the one object of its holder
    // tag the carrier carries; 0 when the carrier has none or several.
    ee::Ptr<SceneObject> attachHolder(ee::Ptr<SceneObject> attach, ee::Ptr<SceneObject> carrier)
    {
        ee::Local<uint32_t> holder;
        if (zge::carriedAnimalCountWithTag(ee::Address(carrier), attachHolderTag(attach)) != 1)
            return {};
        zge::listCarriedAnimalsWithTag(ee::Address(carrier), attachHolderTag(attach), holder);
        return ee::Ptr<SceneObject>(*holder);
    }
    READABLE_FUNCTION(attachHolder, Verified);

    // attachSwings
    uint32_t attachSwings(ee::Ptr<SceneObject> attach)
    {
        return tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagSwings);
    }
    READABLE_FUNCTION(attachSwings, Verified);

    // attachHasAnimator
    uint32_t attachHasAnimator(ee::Ptr<SceneObject> attach)
    {
        return tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagHasAnimator);
    }
    READABLE_FUNCTION(attachHasAnimator, Draft);

    // attachHolderBone: the holder's bone it hangs on; 0 when its data
    // names none or the carrier has no one holder.
    ee::Ptr<SceneBone> attachHolderBone(ee::Ptr<SceneObject> attach, ee::Ptr<SceneObject> carrier)
    {
        const uint32_t boneName = static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagHolderBone));
        if (boneName == 0)
            return {};
        if (zge::carriedAnimalCountWithTag(ee::Address(carrier), attachHolderTag(attach)) != 1)
            return {};
        return findModelBone(attachHolder(attach, carrier), boneName);
    }
    READABLE_FUNCTION(attachHolderBone, Verified);

    // carriedObjectWithTag: the one object of that tag the carrier carries;
    // 0 when it has none or several. (The attach is not used.)
    ee::Ptr<SceneObject> carriedObjectWithTag(ee::Ptr<SceneObject> attach, ee::Ptr<SceneObject> carrier, uint32_t tag)
    {
        (void)attach;
        ee::Local<uint32_t> carried;
        if (zge::carriedAnimalCountWithTag(ee::Address(carrier), tag) != 1)
            return {};
        zge::listCarriedAnimalsWithTag(ee::Address(carrier), tag, carried);
        return ee::Ptr<SceneObject>(*carried);
    }
    READABLE_FUNCTION(carriedObjectWithTag, Verified);

    // carriedObjectBone: the bone of that name on the carrier's one carried
    // object of that tag. With no attach given, 0.
    ee::Ptr<SceneBone> carriedObjectBone(ee::Ptr<SceneObject> attach, ee::Ptr<SceneObject> carrier, uint32_t tag, uint32_t boneName)
    {
        const ee::Ptr<SceneObject> carried = carriedObjectWithTag(attach, carrier, tag);
        if (!attach)
            return {};
        return findModelBone(carried, boneName);
    }
    READABLE_FUNCTION(carriedObjectBone, Verified);

    // attachPlace: where it sits on its holder's bone, into `out`.
    ee::Ptr<Vec4> attachPlace(ee::Ptr<SceneObject> attach, ee::Ptr<Vec4> out)
    {
        tagVector(objectComponent(attach), 0x40, kAttachTagPlace, out);
        return out;
    }
    READABLE_FUNCTION(attachPlace, Verified);

    // setAttachSecondSystem
    void setAttachSecondSystem(ee::Ptr<SceneObject> attach, ee::Ptr<AnimateSystem> system)
    {
        game::tail(setTagWord, objectComponent(attach), kAttachTagWords, kAttachTagSecondSystem, system);
    }
    READABLE_FUNCTION(setAttachSecondSystem, Verified);

    // setAttachSwingSystem
    void setAttachSwingSystem(ee::Ptr<SceneObject> attach, ee::Ptr<AnimateSystem> system)
    {
        game::tail(setTagWord, objectComponent(attach), kAttachTagWords, kAttachTagSwingSystem, system);
    }
    READABLE_FUNCTION(setAttachSwingSystem, Verified);

    // attachSecondSystem
    ee::Ptr<AnimateSystem> attachSecondSystem(ee::Ptr<SceneObject> attach)
    {
        return ee::Ptr<AnimateSystem>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagSecondSystem));
    }
    READABLE_FUNCTION(attachSecondSystem, Verified);

    // attachSwingSystem
    ee::Ptr<AnimateSystem> attachSwingSystem(ee::Ptr<SceneObject> attach)
    {
        return ee::Ptr<AnimateSystem>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagSwingSystem));
    }
    READABLE_FUNCTION(attachSwingSystem, Verified);

    // attachFigureName: the name hash of the figure's model.
    uint32_t attachFigureName(ee::Ptr<SceneObject> attach)
    {
        return static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagFigureName));
    }
    READABLE_FUNCTION(attachFigureName, Verified);

    // attachFigureSystem / attachFigureRig: the figure's animate system, or
    // its body rig when it has one (attachFigureHasRig): the same tag word
    // read as the one or the other, with a setter each.
    ee::Ptr<AnimateSystem> attachFigureSystem(ee::Ptr<SceneObject> attach)
    {
        return ee::Ptr<AnimateSystem>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagFigureRig));
    }
    READABLE_FUNCTION(attachFigureSystem, Verified);

    void setAttachFigureSystem(ee::Ptr<SceneObject> attach, ee::Ptr<AnimateSystem> system)
    {
        game::tail(setTagWord, objectComponent(attach), kAttachTagWords, kAttachTagFigureRig, system);
    }
    READABLE_FUNCTION(setAttachFigureSystem, Verified);

    uint32_t attachFigureHasRig(ee::Ptr<SceneObject> attach)
    {
        return tagByte(objectComponent(attach), kAttachTagBytes, kAttachTagFigureHasRig);
    }
    READABLE_FUNCTION(attachFigureHasRig, Verified);

    ee::Ptr<BodyRig> attachFigureRig(ee::Ptr<SceneObject> attach)
    {
        return ee::Ptr<BodyRig>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagFigureRig));
    }
    READABLE_FUNCTION(attachFigureRig, Draft);

    void setAttachFigureRig(ee::Ptr<SceneObject> attach, ee::Ptr<BodyRig> rig)
    {
        game::tail(setTagWord, objectComponent(attach), kAttachTagWords, kAttachTagFigureRig, rig);
    }
    READABLE_FUNCTION(setAttachFigureRig, Draft);

    namespace
    {
        uint32_t animationName(ee::Ptr<SceneObject> attach, uint32_t index)
        {
            return static_cast<uint32_t>(tagWord(objectComponent(attach), kAttachTagWords, kAttachTagAnimations + index * kAttachAnimationStride));
        }
        ee::Float animationWeight(ee::Ptr<SceneObject> attach, uint32_t index)
        {
            return tagFloat(objectComponent(attach), kAttachTagWords, kAttachTagAnimationWeights + index * kAttachAnimationStride);
        }
    }

    // pickFigureAnimation: the name hash of one of the figure's four
    // animations, picked at random by their weights (one of 256 steps over
    // the weights' sum): the fourth, third and second each take their share
    // from the top, the first takes the rest.
    uint32_t pickFigureAnimation(ee::Ptr<SceneObject> attach)
    {
        const ee::Float first = animationWeight(attach, 0);
        const ee::Float second = animationWeight(attach, 1);
        const ee::Float third = animationWeight(attach, 2);
        const ee::Float sum = first + second + third + animationWeight(attach, 3);
        const ee::Float chance = ee::toFloat(static_cast<int32_t>(randomNumber() & 0xFF)) / 255.0f;
        const uint32_t firstName = animationName(attach, 0);
        ee::Float left = chance * sum - animationWeight(attach, 3);
        if (left < 0.0f)
            return animationName(attach, 3);
        left = left - animationWeight(attach, 2);
        if (left < 0.0f)
            return animationName(attach, 2);
        left = left - animationWeight(attach, 1);
        if (left < 0.0f)
            return animationName(attach, 1);
        return firstName;
    }
    READABLE_FUNCTION(pickFigureAnimation, Verified);
}
