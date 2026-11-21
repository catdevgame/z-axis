// Two-bone chains (an arm or a leg: an upper bone, a lower bone and the
// chain's end) pointed at a target by turning both bones, as inverse
// kinematics does it: the joint between the bones is placed by a few rounds
// of relaxation so that both bones keep their lengths, then the chain is
// swung to point its end at the target. Dave Mirra BMX 2 sets its rider's
// hands and feet with its own copy of aimBoneChain; nothing in Aggressive
// Inline uses these.

#include "bone_chain.h"

#include "zge/math/angle.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/debug/debug.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/bone_chain.cpp.h"

namespace zge
{

    namespace
    {
        // A chain whose end is this close to its start (squared) is left as it is.
        constexpr ee::Float kChainTooShort = 0.01f;
        // Two directions nearer parallel than this (their cross product's
        // squared length) need no turn.
        constexpr ee::Float kNotQuiteParallel = 0.001f;
        // The rounds that place the joint.
        constexpr uint32_t kJointRounds = 8;
        constexpr ee::Float kHalf = 0.5f;
        // addDebugLine's flags for the chain's lines (bit 0: not one
        // debugDrawFlags keeps).
        constexpr uint32_t kChainLineFlags = 1;

        uint32_t nameHashOf(uint32_t name) { return nameHash(ee::Ptr<const char>(name)); }

        // The joint (from the chain's start) moved to lie upperLength from
        // the start and lowerLength from the end: each round puts it halfway
        // between the place at the right distance from the start and the
        // place at the right distance from the end.
        void placeJoint(const BoneChain &chain, Vec4 &joint, const Vec4 &end, Vec4 &fromStart, Vec4 &fromEnd)
        {
            for (uint32_t round = 0; round < kJointRounds; ++round)
            {
                fromStart = unit(joint);
                fromStart = fromStart * chain.upperLength;
                fromEnd = joint - end;
                fromEnd = unit(fromEnd);
                fromEnd = fromEnd * chain.lowerLength;
                fromEnd = fromEnd + end;
                joint = fromStart + fromEnd;
                joint = joint * kHalf;
            }
        }

        // The turn that takes the unit direction `from` to `to` (none when
        // they are all but parallel).
        void turnBetween(const ee::Local<Quaternion> &turn, const ee::Local<Vec4> &axis, const Vec4 &from, const Vec4 &to)
        {
            *axis = cross(from, to);
            if (kNotQuiteParallel < dot(*axis, *axis))
            {
                const ee::Float sine = vec3Length(&axis->xyz());
                const ee::Float cosine = dot(from, to);
                const Angle angle = static_cast<short>(angleAtan2(sine, cosine));
                *axis = unit(*axis);
                quaternionFromAxisAngle(turn, &axis->xyz(), angle);
            }
            else
                quaternionIdentity(turn);
        }
    }

    // setUpBoneChain: the chain over the object's two bones of these names,
    // with the joint in the upper bone's space and the end in the lower
    // one's (each the aligned quadword it is in), their lengths, the upper
    // bone's parent and the two bones' animator channels.
    void setUpBoneChain(ee::Ptr<BoneChain> chain, ee::Ptr<SceneObject> object, uint32_t upperName, ee::Ptr<Vec4> joint,
                        uint32_t lowerName, ee::Ptr<Vec4> end)
    {
        chain->upperBone = ee::Ptr<SceneBone>(findModelBone(object, nameHashOf(upperName)));
        chain->lowerBone = ee::Ptr<SceneBone>(findModelBone(object, nameHashOf(lowerName)));
        chain->parentBone = boneParent(object->scene, chain->upperBone);
        chain->object = object;
        aligned(ee::Ptr<Vec4>(&chain->joint)) = quadwordOf(joint);
        aligned(ee::Ptr<Vec4>(&chain->end)) = quadwordOf(end);
        chain->upperLength = vec3Length(&chain->joint.xyz());
        chain->lowerLength = vec3Length(&chain->end.xyz());
        chain->upperChannel = findBoneChannel(object->animator, nameHashOf(upperName));
        chain->lowerChannel = findBoneChannel(object->animator, nameHashOf(lowerName));
    }
    READABLE_FUNCTION(setUpBoneChain, Draft);

    // drawBoneChainAim (debug): the chain as it stands (red) and the line
    // from its start to the target (green), then the joint placed as
    // aimBoneChain places it, with the end at the target's distance along
    // its present direction (blue). No bone is turned.
    void drawBoneChainAim(ee::Ptr<BoneChain> chain, ee::Ptr<Vec4> target)
    {
        ee::Local<Vec4> start;
        ee::Local<Vec4> joint;
        ee::Local<Vec4> end;
        ee::Local<Vec4> toTarget;
        ee::Local<Vec4> oldEnd;
        ee::Local<Vec4> oldJoint;
        ee::Local<Vec4> origin;
        ee::Local<Vec4> fromStart;
        ee::Local<Vec4> fromEnd;
        ee::Local<uint32_t> red;
        ee::Local<uint32_t> green;
        ee::Local<uint32_t> blue;
        ee::Local<Vec4> newJoint;
        ee::Local<Vec4> newEnd;
        *red = *ee::Ptr<uint32_t>(sym::g_boneChainDebugRed);
        *green = *ee::Ptr<uint32_t>(sym::g_boneChainDebugGreen);
        *blue = *ee::Ptr<uint32_t>(sym::g_boneChainDebugBlue);
        updatePose(chain->object);
        *origin = kZero;
        objectBonePoint(chain->object, chain->upperBone, origin, start, kBoneInWorld);
        objectBonePoint(chain->object, chain->upperBone, &chain->joint, joint, kBoneInWorld);
        objectBonePoint(chain->object, chain->lowerBone, &chain->end, end, kBoneInWorld);
        addDebugLine(start, joint, red, kChainLineFlags);
        addDebugLine(end, joint, red, kChainLineFlags);
        addDebugLine(start, target, green, kChainLineFlags);
        *joint = *joint - *start;
        *end = *end - *start;
        *toTarget = *target - *start;
        if (dot(*end, *end) < kChainTooShort)
            return;
        *oldEnd = *end;     // (kept, never read)
        *oldJoint = *joint; // (kept, never read)
        *end = unit(*end);
        *end = *end * vec3Length(&toTarget->xyz());
        placeJoint(*chain, joint, end, fromStart, fromEnd);
        *newJoint = *start + *joint;
        *newEnd = *start + *end;
        addDebugLine(start, newJoint, blue, kChainLineFlags);
        addDebugLine(newEnd, newJoint, blue, kChainLineFlags);
    }
    READABLE_FUNCTION_WAITS(drawBoneChainAim, Draft);

    // aimBoneChain: the chain's two bones turned (through their animator
    // channels) so that its end points at the target. The end is taken to
    // the target's distance along its present direction and the joint placed
    // to keep both bones' lengths (placeJoint); the upper bone is turned
    // from its old joint to the new one and then swung from the end's
    // direction to the target's, the lower bone from its old direction to
    // its new one. Each rotation is given in its parent's space (the upper
    // bone's in its parent bone's, the lower bone's in the upper bone's
    // before the swing). Nothing is turned when the chain's end is all but
    // at its start.
    void aimBoneChain(ee::Ptr<BoneChain> chain, ee::Ptr<Vec4> target)
    {
        ee::Local<Vec4> start;
        ee::Local<Vec4> joint;
        ee::Local<Vec4> end;
        ee::Local<Vec4> toTarget;
        ee::Local<Vec4> endDirection;
        ee::Local<Vec4> oldEnd;
        ee::Local<Vec4> oldJoint;
        ee::Local<Vec4> origin;
        ee::Local<Vec4> from;
        ee::Local<Vec4> to;
        ee::Local<Vec4> axis;
        ee::Local<Quaternion> turn;
        ee::Local<Quaternion> parentRotation;
        ee::Local<Quaternion> upperRotation;
        ee::Local<Quaternion> lowerRotation;
        ee::Local<Quaternion> inverse;
        ee::Local<Matrix> matrix;
        updatePose(chain->object);
        *origin = kZero;
        objectBonePoint(chain->object, chain->upperBone, origin, start, kBoneInWorld);
        objectBonePoint(chain->object, chain->upperBone, &chain->joint, joint, kBoneInWorld);
        objectBonePoint(chain->object, chain->lowerBone, &chain->end, end, kBoneInWorld);
        *joint = *joint - *start;
        *end = *end - *start;
        *toTarget = *target - *start;
        if (dot(*end, *end) < kChainTooShort)
            return;
        *oldEnd = *end;
        *oldJoint = *joint;
        *end = unit(*end);
        *end = *end * vec3Length(&toTarget->xyz());
        placeJoint(*chain, joint, end, from, to);

        objectBoneMatrix(chain->object, chain->upperBone, matrix, kBoneInWorld);
        matrixToQuaternion(upperRotation, matrix);
        objectBoneMatrix(chain->object, chain->lowerBone, matrix, kBoneInWorld);
        matrixToQuaternion(lowerRotation, matrix);
        objectBoneMatrix(chain->object, chain->parentBone, matrix, kBoneInWorld);
        matrixToQuaternion(parentRotation, matrix);

        // The upper bone: from the old joint to the new.
        *from = unit(*oldJoint);
        *to = unit(*joint);
        turnBetween(turn, axis, *from, *to);
        quaternionMultiply(upperRotation, turn, upperRotation);
        // Then the chain swung from its end's direction to the target's.
        *toTarget = unit(*toTarget);
        *endDirection = unit(*end);
        turnBetween(turn, axis, *endDirection, *toTarget);
        quaternionMultiply(turn, turn, upperRotation);
        quaternionInverse(inverse, parentRotation);
        quaternionMultiply(turn, inverse, turn);
        setBoneRotation(chain->upperChannel, turn);

        // The lower bone: from the old joint-to-end direction to the new.
        *end = *end - *joint;
        *oldEnd = *oldEnd - *oldJoint;
        *end = unit(*end);
        *oldEnd = unit(*oldEnd);
        turnBetween(turn, axis, *oldEnd, *end);
        quaternionMultiply(turn, turn, lowerRotation);
        quaternionInverse(inverse, upperRotation);
        quaternionMultiply(turn, inverse, turn);
        setBoneRotation(chain->lowerChannel, turn);
    }
    READABLE_FUNCTION_WAITS(aimBoneChain, Draft);
}
