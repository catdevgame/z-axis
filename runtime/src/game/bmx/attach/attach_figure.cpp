// BMX XXX's attaches (attach.h): a human attach's figure: the model object
// it shows, its animations, and the pose hook that lets the figure's body
// rig or animate system move with each step of an animation. BMX XXX's own
// code.

#include "attach.h"
#include "attach_calls.h"

#include "bmx/pathman/pathman_calls.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/carried_objects.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/attach/attach_figure.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        // The animator channel of the figure's "Hips" bone (the name's hash).
        constexpr uint32_t kHipsChannel = 0x4CE53;
        // The step of a rig's moves in the pose hook, and how many of them
        // come before the one that turns the bones.
        constexpr ee::Float kRigStep = 1.0f / 60.0f;
        constexpr uint32_t kRigSubSteps = 5;
        // The track's weight the engine calls a blend hook with once the
        // blend has ended.
        constexpr ee::Float kBlendEnded = -1.0f;


        // The rider's physics states in which the figure's rig is left alone.
        bool riderHoldsFigureStill(ee::Ptr<rider::Rider> rider)
        {
            // (States 0x10, 0x11, 0xC and 0xD have no name in rider.h yet.)
            return rider::physicsState(rider) == 0x10 || rider::physicsState(rider) == 0x11 || rider::physicsState(rider) == 0xC ||
                   rider::physicsState(rider) == 0xD || rider::physicsState(rider) == rider::kPhysicsBail;
        }
    }

    // figurePoseHook (the blend hook of the figure's animations; `rig` is
    // what updateFigureAnimation gave the animation: the figure's body rig,
    // or its animate system when it has no rig). While the figure is on its
    // holder, the pose is followed by the rig's moves: five steps of the
    // points alone and one that turns the bones, unless the rider's state
    // holds the figure still. A figure on nothing has its hips' translation
    // cleared after one step.
    // As the game ships it, the first case reads `rig` as an animate system
    // and the second as a body rig, whichever the figure has.
    void figurePoseHook(ee::Ptr<SceneObject> figure, uint32_t track, ee::Address rig, ee::Float weight)
    {
        (void)track;
        if (weight == kBlendEnded)
            return;
        if (!zge::attachedParent(figure))
        {
            ee::Local<Vec4> none;
            attach::clearVector(none);
            const ee::Ptr<AnimateSystem> system = ee::Ptr<BodyRig>(rig)->system;
            attach::animate(system, 1, system->pivotsSkippedWord(), kRigStep);
            setBoneTranslation(findBoneChannel(sceneObjectAnimator(figure), kHipsChannel), none);
            return;
        }
        const ee::Ptr<AnimateSystem> system(rig);
        const ee::Ptr<rider::Rider> rider(animalCarrier(zge::attachedParent(figure)));
        updatePose(zge::attachedParent(figure));
        markPoseStale(system->object);
        setAnimatorChanges(system->object, 3);
        updatePose(system->object);
        if (riderHoldsFigureStill(rider))
            return;
        resetAnimatePoints(system);
        for (uint32_t step = 0; step < kRigSubSteps; ++step)
            attach::animate(system, 0, 1, kRigStep);
        attach::animate(system, 1, system->pivotsSkippedWord(), kRigStep);
        if (system->pivotsSkippedWord() == kFigureSettled)
            system->setPivotsSkipped(0);
        else if (system->pivotsSkippedWord() > kFigureSettled)
            system->setPivotsSkipped(system->pivotsSkippedWord() - 1);
    }
    READABLE_FUNCTION_WAITS(figurePoseHook, Verified);

    // updateFigureAnimation: once the figure's animation is about to end
    // (within `step`), or it has none, the next one starts: one of the
    // attach's four, picked by their weights, with the pose hook. A name the
    // game has no animation for starts nothing.
    void updateFigureAnimation(ee::Ptr<SceneObject> attach, ee::Float step)
    {
        ee::Local<ee::Float> time;
        ee::Address rig;
        ee::Ptr<AnimateSystem> system;
        if (attachFigureHasRig(attach) != 0)
        {
            const ee::Ptr<BodyRig> bodyRig = attachFigureRig(attach);
            rig = bodyRig;
            system = bodyRig->system;
        }
        else
        {
            system = attachFigureSystem(attach);
            rig = system;
        }
        if (!system)
            return;
        const ee::Ptr<SceneObject> figure = system->object;
        bool ended = true;
        if (pathman::objectHasAnimator(figure) != 0)
        {
            animationTime(figure, time);
            ended = animationDone(figure, *time + step) != 0;
        }
        if (!ended)
            return;
        const ee::Ptr<Motion> animation = findLibraryAnimation(pickFigureAnimation(attach));
        if (!animation)
            return;
        playAnimationWithHooks(figure, animation, 0, zge::BlendHook(sym::figurePoseHook), rig, 0.0f);
    }
    READABLE_FUNCTION_WAITS(updateFigureAnimation, Verified);

    // attachFigure: the figure's object by its model's name: the scene's
    // object of that name, else the engine's model object of that name, else
    // a model object made from the pose of that name.
    ee::Ptr<SceneObject> attachFigure(uint32_t nameHash)
    {
        ee::Local<uint32_t> name;
        *name = nameHash;
        const ee::Ptr<ee::Ptr<SceneObject>> found = findObjects(name.address(), 1);
        if (found)
            return *found;
        const ee::Ptr<SceneObject> made = namedModelObject(nameHash, 1);
        if (made)
            return made;
        return zge::newModelObjectFromPose(nameHash, 0);
    }
    READABLE_FUNCTION_WAITS(attachFigure, Verified);
}
