#pragma once

// Functions of the engine (ZGE4) the attaches call that cannot be called by
// name on this line yet: the callee has no readable body in BMX XXX, takes
// other words in ZGE4 than the engine header says, or is declared in no
// header. Each is called where it is, by address. The names are the engine
// lane's (its working names where it says so).
// Scaffolding (address-call): a plain call once the callee is written.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable_waits.h"
#include "zge/math/types.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/attach/attach_calls.h.h"

namespace game::bmx::attach
{
    // --- Scene objects ---

    // The object let go of what it is attached to (the object it hangs from: zge::attachedParent).
    inline void detachObject(ee::Ptr<zge::SceneObject> object) { ee::game<void>(sym::detachObject, object); }

    // The object given a new animator record, the old one's matching bones copied into it.
    inline void remakeObjectAnimator(ee::Ptr<zge::SceneObject> object) { ee::game<void>(sym::remakeObjectAnimator, object); }

    // The object's animation moved on a little and blended (working name).
    inline void nudgeAnimation(ee::Ptr<zge::SceneObject> object) { ee::game<void>(sym::nudgeAnimation, object); }

    // --- Carried objects ---
    // The attaches call ZGE4's carried objects (zge/scene/carried_objects.h) by name: attachCarriedObject (ZGE4's
    // attachCarriedAnimal, with the carried object's calls), the carry messages with a word and the carried
    // objects' tags.

    // The C library's memset on a vector (by address, as the game calls it).
    inline void clearVector(ee::Ptr<zge::Vec4> vector) { ee::game<void>(sym::memset, vector, 0, static_cast<uint32_t>(sizeof(zge::Vec4))); }

    // --- The animate system (ZGE4's own makers) ---

    // ZGE4's animate: a step of every system on the chain from this one; the
    // first is posed before it when updateObjectPose, and the pivots leave
    // the bones alone when pivotsSkipped is not 0. (The engine's function,
    // which takes the third word in BMX XXX only.)
    inline void animate(ee::Ptr<zge::AnimateSystem> system, uint32_t updateObjectPose, uint32_t pivotsSkipped, ee::Float step)
    {
        static_cast<void>(pivotsSkipped);
        zge::animate(system, updateObjectPose READABLE_PARAM_ONLY(bmx, pivotsSkipped), step);
    }

    // A pivot that turns `bone` along the line from one point to the other
    // (ZGE4's newBonePivot; working name).
    inline ee::Ptr<zge::AnimatePivot> newRootedBonePivot(ee::Ptr<zge::AnimateSystem> system, ee::Ptr<zge::SceneBone> bone,
                                                         ee::Ptr<zge::AnimatePoint> from, ee::Ptr<zge::AnimatePoint> to)
    {
        return ee::game<ee::Ptr<zge::AnimatePivot>>(sym::newRootedBonePivot, system, bone, from, to);
    }
    // A pivot of a frame kind: the bone's frame from three points.
    inline ee::Ptr<zge::AnimatePivot> newFramePivot(ee::Ptr<zge::AnimateSystem> system, ee::Ptr<zge::SceneBone> bone,
                                                    ee::Ptr<zge::AnimatePoint> from, ee::Ptr<zge::AnimatePoint> to,
                                                    ee::Ptr<zge::AnimatePoint> third, uint32_t kind)
    {
        return ee::game<ee::Ptr<zge::AnimatePivot>>(sym::newFramePivot, system, bone, from, to, third, kind);
    }
    // A point on a bone of another object than the system's.
    inline ee::Ptr<zge::AnimatePoint> newObjectBonePoint(ee::Ptr<zge::AnimateSystem> system, ee::Ptr<zge::SceneObject> object,
                                                         ee::Ptr<zge::SceneBone> bone, int32_t kind, ee::Ptr<const zge::Vec4> place,
                                                         uint32_t id, ee::Float mass)
    {
        return ee::game<ee::Ptr<zge::AnimatePoint>>(sym::newObjectBonePoint, system, object, bone, kind, place, id, mass);
    }
}
