// DM2's limb chains (physics.c): the rider model's arms and legs set up as
// two-bone chains, so a hand or foot can be put on a point (the grips, the
// trick poses: aimBoneChain).

#include "rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "sym/dm2/rider/limb_chains.cpp.h"

namespace game::dm2::rider
{
    namespace
    {
        // A limb's bones by name: the upper and the lower one, and the bone
        // the chain ends at.
        struct LimbBoneNames
        {
            ee::Ptr<char> upper;
            ee::Ptr<char> lower;
            ee::Ptr<char> end;
        };
        // In Rider::limbTargets' order: the left arm ("LeftUpArm",
        // "LeftLowArm", "LeftHand"), the right arm, the left leg
        // ("LeftUpLeg", "LeftLowLeg", "LeftFoot"), the right leg.
        constexpr uint32_t kLimbs = 4;
        inline ee::Global<LimbBoneNames[kLimbs], sym::g_limbBoneNames> g_limbBoneNames;

        // Where one of the model's bones sits in its parent at rest.
        void bindPosition(ee::Ptr<zge::SceneObject> model, ee::Ptr<char> boneName, ee::Ptr<zge::Vec4> out)
        {
            zge::modelBoneBindPosition(model, zge::findModelBone(model, zge::nameHash(boneName)), out);
        }
    }

    // setUpLimbChains (rider): each limb's chain made over the rider model's
    // bones: the joint is where the lower bone sits in the upper one, the end
    // where the hand or foot sits in the lower one.
    void setUpLimbChains(Rider *rider)
    {
        const ee::Ptr<zge::SceneObject> model(rider->messenger);
        for (uint32_t limb = 0; limb < kLimbs; ++limb)
        {
            const LimbBoneNames &names = (*g_limbBoneNames)[limb];
            ee::Local<zge::Vec4> joint;
            ee::Local<zge::Vec4> end;
            bindPosition(model, names.lower, joint);
            bindPosition(model, names.end, end);
            zge::setUpBoneChain(&rider->limbTargets[limb], model, ee::Address(names.upper), joint, ee::Address(names.lower), end);
        }
    }
    READABLE_FUNCTION(setUpLimbChains, Verified);
}
