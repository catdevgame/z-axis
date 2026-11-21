#pragma once
// Two-bone chains (an arm or a leg: an upper bone, a lower bone and the
// chain's end) pointed at a target by turning both bones (bone_chain.cpp).
// The chain's record is its caller's (BMX XXX's rider keeps one for each
// limb).

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct AnimationChannel;

    // A two-bone chain (0x40 bytes, the caller's).
    struct BoneChain
    {
        Vec4 joint;                // +0x00 where the lower bone starts (the elbow), in the upper bone's space
        Vec4 end;                  // +0x10 the chain's end (the hand), in the lower bone's space
        ee::Float upperLength;     // +0x20 joint's length
        ee::Float lowerLength;     // +0x24 end's length
        ee::Ptr<AnimationChannel> upperChannel; // +0x28 the object's animator's channels for the two bones
        ee::Ptr<AnimationChannel> lowerChannel; // +0x2C
        ee::Ptr<SceneBone> upperBone;  // +0x30
        ee::Ptr<SceneBone> lowerBone;  // +0x34
        ee::Ptr<SceneBone> parentBone; // +0x38 the upper bone's parent
        ee::Ptr<SceneObject> object;   // +0x3C
    };
    static_assert(offsetof(BoneChain, upperLength) == 0x20 && offsetof(BoneChain, upperChannel) == 0x28);
    static_assert(offsetof(BoneChain, upperBone) == 0x30 && offsetof(BoneChain, object) == 0x3C && sizeof(BoneChain) == 0x40);

    // The chain over the object's two bones of these names (the names'
    // addresses), with the joint in the upper bone's space and the end in
    // the lower one's.
    void setUpBoneChain(ee::Ptr<BoneChain> chain, ee::Ptr<SceneObject> object, uint32_t upperName, ee::Ptr<Vec4> joint,
                        uint32_t lowerName, ee::Ptr<Vec4> end);
    // The chain's two bones turned so that its end points at the target.
    void aimBoneChain(ee::Ptr<BoneChain> chain, ee::Ptr<Vec4> target);
}
