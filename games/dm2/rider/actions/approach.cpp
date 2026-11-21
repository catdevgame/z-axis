// DM2's snap onto a grind, stall or lip target (physics states 3, 5 and 7,
// MIXING 14.1's table): the approach's gravity and move, and the bail
// body's release.
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x80082174 (approachMove),
// 0x80086F30 (bailBodyRelease); BMX XXX PS2 approachGravity,
// approachMove, bailBodyRelease.

#include "actions.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/collision/dynamics.h"
#include "sym/dm2/rider/actions/approach.cpp.h"

namespace game::dm2::rider
{
    namespace
    {
        // Gravity a frame on the snap's step (y grows downwards).
        inline ee::Global<ee::Float, sym::g_gravity> g_approachGravity; // 0.0055
        // The wheels' spin kept a frame of the snap.
        inline ee::Global<ee::Float, sym::g_approachSpinKept> g_approachSpinKept; // 0.9967
        // The rows turn this share of the way to the target's a frame.
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_alignShare; // 0.085

        // The bail's hands (bailHands): the bones' world points, each
        // against the faces near it (bailHandContact fills the hand's record).
        constexpr uint32_t kLeftHand = sym::leftHand;  // "LeftHand"
        constexpr uint32_t kRightHand = sym::rightHand; // "RightHand"
        inline ee::Global<zge::Vec4, sym::origin> g_origin; // (0, 0, 0, 0)

        uint32_t hashOf(uint32_t text) { return zge::nameHash(ee::pointerAt<char>(text)); }
    }

    // States 3, 5 and 7's forces: gravity on the snap's step (its y).
    // BMX XXX PS2 approachGravity.
    void approachGravity(Rider *rider)
    {
        rider->snapStep[1] = rider->snapStep[1] + *g_approachGravity;
    }
    READABLE_FUNCTION(approachGravity, Verified);

    // States 3, 5 and 7's move: the wheels spin down; the snap's step added
    // to the position and the drawn body for the snap's frames, then the
    // state after the approach (4 the grind, 6 the stall, 8 the lip).
    // Returns the position (or the state, as setPhysicsState's).
    // GameCube 0x80082174; BMX XXX PS2 approachMove.
    uint32_t approachMove(Rider *rider)
    {
        const ee::Float kept = *g_approachSpinKept;
        for (WheelPose &wheel : rider->wheelPoses)
            wheel.spin = ee::toInt(ee::toFloat(wheel.spin) * kept);
        if (--rider->snapFrames == 0)
        {
            // The original's tail call: setPhysicsState, and the state's
            // enter callback under it (grindEnter, grindOnto), run at this
            // function's own stack, its frame given back first (grindOnto's
            // velocity takes its w from a word there).
            const uint8_t state = physicsState(rider);
            return game::tail(setPhysicsState, rider, uint32_t(state) + 1u);
        }
        *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *vec4(rider->snapStep);
        *vec4(rider->position) = *vec4(rider->position) + *vec4(rider->snapStep);
        return ee::Address(rider->position);
    }
    READABLE_FUNCTION(approachMove, Verified);

    // States 3, 5 and 7's turn: the up and forward rows turned toward the
    // target's by a share a frame, the right row and forward made square
    // with them again. Returns the right row. GameCube 0x8007D1E0.
    uint32_t approachAlign(Rider *rider)
    {
        const ee::Float share = *g_alignShare;
        const zge::Matrix &target = *matrix(rider->snapRows);
        const zge::Vec4 &targetUp = target.row(1);
        const zge::Vec4 &targetForward = target.row(2);
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &forward = *vec4(rider->forward);
        forward.x = forward.x + (targetForward.x - forward.x) * share;
        up.x = up.x + (targetUp.x - up.x) * share;
        forward.y = forward.y + (targetForward.y - forward.y) * share;
        up.y = up.y + (targetUp.y - up.y) * share;
        forward.z = forward.z + (targetForward.z - forward.z) * share;
        up.z = up.z + (targetUp.z - up.z) * share;
        right = zge::cross(up, forward);
        right = zge::unit(right);
        forward = zge::cross(right, up);
        return ee::Address(rider->right); // (what the original leaves: the right row)
    }
    READABLE_FUNCTION(approachAlign, Verified);

    // The bail's hands against the ground, while the bail reaches with them:
    // each hand's bone in the world, then its contact (until one holds).
    // GameCube 0x800874B0; BMX XXX PS2 bailHands.
    void bailHands(Rider *rider)
    {
        ee::Local<zge::Vec4> leftHand;
        ee::Local<zge::Vec4> rightHand;
        if (!(rider->flags & kFlagBailHands))
        {
            rider->hands[1].holding = 0;
            rider->hands[0].holding = 0;
            return;
        }
        if (rider->hands[0].holding != 0 || rider->hands[1].holding != 0)
            return;
        const ee::Ptr<zge::SceneObject> model(rider->messenger);
        zge::objectBonePoint(model, zge::findModelBone(model, hashOf(kLeftHand)), g_origin.get(), leftHand, 0);
        bailHandContact(rider, &rider->hands[0], leftHand);
        zge::objectBonePoint(model, zge::findModelBone(model, hashOf(kRightHand)), g_origin.get(), rightHand, 0);
        bailHandContact(rider, &rider->hands[1], rightHand);
    }
    READABLE_FUNCTION(bailHands, Verified);

    // The bail's hands' bodies let go: the left's, then the right's (the
    // original's second is its tail call; disableContact keeps no frame, so
    // it does the same either way). GameCube 0x80086F30.
    void bailBodyRelease(Rider *rider)
    {
        zge::disableContact(ee::Ptr<zge::BodyContact>(rider->hands[0].body));
        game::tail(zge::disableContact, ee::Ptr<zge::BodyContact>(rider->hands[1].body));
    }
    READABLE_FUNCTION(bailBodyRelease, Verified);
}
