// DM2's rider frame (riderFrame; MIXING 14.1's frame row): the
// direction taps, out of the level's bounds back to the start, the action
// state's callbacks (g_actionStates) until one keeps its state, the motions (the
// rider's and the bike's: started, timed, blended with the lean's, ended),
// the bones the physics reads, the physics frame, the bail's body kept with
// the rider, and the bail's hands reach (the grind button in a bail).
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x8008A56C; DM2 Xbox 0x0631A0.

#include "../../level/level_collision.h"
#include "actions.h"
#include "../physics/physics.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/actions/frame.cpp.h"

namespace game::dm2::rider
{
    Bounds *levelBounds();

    namespace
    {
        // The action states' callbacks (one a level-0 state; ActionState):
        // non-zero when the rider stays in the state.
        inline ee::Global<ee::FunctionPtr<int32_t(Rider *rider)>[8], sym::actionStates> g_actionStates;
        // A direction by its button bits (kButtonUp .. kButtonRight).
        inline ee::Global<uint8_t[16], sym::g_directionByButtons> g_directionByButtons;
        constexpr uint8_t kMaxTaps = 4;
        // The riders' motions (a motion a motion index).
        inline ee::Ptr<zge::Motion> riderMotion(uint16_t index) { return ee::Ptr<ee::Ptr<zge::Motion>>(sym::riderMotions)[index]; }
        // The bones' name hashes (rider_state.cpp's kBoneNames): the rider's
        // "Hips" and "Head", the bike's "Hipsdeck", "Handlebars", "Sprocket",
        // "FrontWheel" and "BackWheel".
        inline ee::Global<int32_t, sym::g_hipsBoneHash> g_hipsName;
        inline ee::Global<int32_t, sym::g_headBoneHash> g_headName;
        inline ee::Global<int32_t, sym::g_hipsDeckBoneHash> g_hipsDeckName;
        inline ee::Global<int32_t, sym::g_handlebarsBoneHash> g_handlebarsName;
        inline ee::Global<int32_t, sym::g_sprocketBoneHash> g_sprocketName;
        inline ee::Global<int32_t, sym::g_frontWheelBoneHash> g_frontWheelName;
        inline ee::Global<int32_t, sym::g_backWheelBoneHash> g_backWheelName;
        // The motions' time a frame: the ended test looks this far ahead, the
        // clock steps this far (each by the motion's rate).
        inline ee::Global<ee::Float, sym::g_motionEndAhead> g_motionEndAhead;
        inline ee::Global<ee::Float, sym::g_motionStep> g_motionStep;
        constexpr uint32_t kMotionFlagTurnRound = 0x2u; // (as states.cpp's) cleared after the bones are read
        constexpr uint32_t kMoveGrind = 7;
        constexpr uint32_t kMoveHeld = 1;
        constexpr uint32_t kMotionFlagFrozen = 0x4u;   // the motions not stepped (inferred)
        constexpr uint32_t kMotionFlagLean = 0x10u;    // the lean blends on (grindLean)
        constexpr uint32_t kFlagGrindAsked = (1u << 20); // the grind button held (states.cpp's): cleared each frame
        constexpr uint32_t kFlagActedFrame = 0x100u;

        // The engine's motion calls on a model (its scene object).
        inline void startMotion(uint32_t model, ee::Ptr<zge::Motion> motion, ee::Float time)
        {
            zge::startAnimation(ee::Ptr<zge::SceneObject>(model), motion, time);
        }
        inline void setMotionTime(uint32_t model, ee::Float time) { zge::updateMotionTracks(ee::Ptr<zge::SceneObject>(model), time); }
        inline void blendMotion(uint32_t model, ee::Ptr<zge::Motion> motion, ee::Float time, ee::Float weight)
        {
            zge::blendMotionKeys(ee::Ptr<zge::SceneObject>(model), motion, time, weight);
        }
        // Non-zero when the model's motion is done by `time`.
        inline int32_t motionDone(uint32_t model, ee::Float time) { return zge::animationDone(ee::Ptr<zge::SceneObject>(model), time); }
        // Scaffolding (address-call): DM2's model pose has no readable body yet.
        // The model posed from its motions.
        inline void poseModel(uint32_t model) { ee::game(sym::modelPose, model); }
        // The body's box moved in the level's sweep, as a guest call: the
        // scale after it carries what it left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline void sweepUpdateByCall(ee::Ptr<zge::Sweep> sweep, ee::Ptr<zge::RigidBody> body) { ee::game(sym::sweepUpdateBody, sweep, body); }

        // A model's bone by its name's hash.
        ee::Ptr<zge::AnimationChannel> boneOf(uint32_t model, int32_t nameHash)
        {
            return zge::findAnimatorBone(ee::Ptr<zge::SceneObject>(model), nameHash);
        }

        // The direction bits now (held) and changed, the stick past 0.5 on
        // each axis counting as its direction (now or a frame ago).
        void stickBits(const ButtonRecord &in, uint32_t &held, uint32_t &changed)
        {
            const ee::Float x = in.sticks[0].x;
            if (!(ee::Float(0.5f) < ee::abs(x)))
            {
                const ee::Float before = in.previous[0].x;
                if (ee::Float(0.5f) < ee::abs(before))
                    changed |= before < ee::Float(0.0f) ? kButtonLeft : kButtonRight;
            }
            else
            {
                held |= x < ee::Float(0.0f) ? kButtonLeft : kButtonRight;
                if (ee::abs(in.previous[0].x) <= ee::Float(0.5f))
                    changed |= x < ee::Float(0.0f) ? kButtonLeft : kButtonRight;
            }
            const ee::Float y = in.sticks[0].y;
            if (!(ee::Float(0.5f) < ee::abs(y)))
            {
                const ee::Float before = in.previous[0].y;
                if (ee::Float(0.5f) < ee::abs(before))
                    changed |= before < ee::Float(0.0f) ? kButtonUp : kButtonDown;
            }
            else
            {
                held |= y < ee::Float(0.0f) ? kButtonUp : kButtonDown;
                if (ee::abs(in.previous[0].y) <= ee::Float(0.5f))
                    changed |= y < ee::Float(0.0f) ? kButtonUp : kButtonDown;
            }
        }
    }

    // A rider's frame (by its player index). Returns the rider's flags
    // with the bail's reach bit as set (or 0x7FFFFFFF when cleared).
    //
    // The bail body's scale carries what the last call left, as
    // airGravity's does.
    int32_t riderFrame(uint32_t player)
    {
        ee::Local<zge::Quaternion> sprocketTurn; // the sprocket's rotation, mirrored
        Rider *const rider = riderOf(player & 0xFFu).get();

        // The direction taps: a direction just pushed (only presses
        // changed) is a new tap (the oldest of four dropped); a change with
        // a press among others replaces the last tap.
        if (rider->flags & kFlagTapsOn)
        {
            uint32_t held = rider->input.held & kButtonDirections;
            uint32_t changed = rider->input.changed & kButtonDirections;
            stickBits(rider->input, held, changed);
            if (changed != 0)
            {
                const uint8_t direction = (*g_directionByButtons)[held];
                if (changed == held)
                {
                    uint32_t count = rider->tapCount;
                    if (count == kMaxTaps)
                    {
                        for (uint32_t i = 0;; ++i)
                        {
                            rider->taps[i] = rider->taps[i + 1];
                            if (!(i + 1 < uint32_t(rider->tapCount) - 1))
                                break;
                        }
                        count = rider->tapCount - 1;
                        rider->tapCount = static_cast<uint8_t>(count);
                    }
                    rider->tapCount = static_cast<uint8_t>(count + 1);
                    rider->taps[count & 0xFFu] = direction;
                }
                else if (held & changed)
                {
                    const uint8_t count = rider->tapCount;
                    if (count != 0)
                        rider->taps[rider->tapCount - 1] = direction;
                    else
                    {
                        rider->tapCount = static_cast<uint8_t>(count + 1);
                        rider->taps[0] = direction;
                    }
                }
            }
        }

        // Out of the level (a unit past its bounds; not below): back to the start.
        const Bounds *const bounds = levelBounds();
        const ee::Float *const at = rider->position;
        if (bounds->max[1] + ee::Float(1.0f) < at[1] || at[0] < bounds->min[0] - ee::Float(1.0f) ||
            bounds->max[0] + ee::Float(1.0f) < at[0] || at[2] < bounds->min[2] - ee::Float(1.0f) ||
            bounds->max[2] + ee::Float(1.0f) < at[2])
        {
            physics::resetPhysics(rider);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
        }

        // The action state's callbacks until one keeps its state.
        rider->flags &= ~kFlagActedFrame & ~kFlagGrindAsked;
        rider->pedalForce = ee::Float(0.0f);
        while ((*g_actionStates)[rider->actionStates[kLevelAction]](rider) == 0)
        {
        }
        const uint32_t motionFlags = rider->motionFlags | kMotionFlagTurnRound;
        ++levelFrames(rider, kLevelAction);
        rider->motionFlags = motionFlags;

        zge::Vec4 &root = *vec4(rider->motionRoot);
        zge::Vec4 &bikeRoot = *vec4(rider->drawnOffset);
        if (!(motionFlags & kMotionFlagFrozen))
        {
            // The lean blends' weights into the blend's 16 channels.
            if (motionFlags & kMotionFlagLean)
                for (uint32_t i = 0; i < 16; ++i)
                {
                    rider->leanWeights[0][i].weight = rider->leanBlend[0];
                    rider->leanWeights[1][i].weight = rider->leanBlend[1];
                }
            if (rider->motionFlags & kMotionRestart)
            {
                startMotion(rider->messenger, riderMotion(static_cast<uint16_t>(rider->motion)), ee::Float(0.0f));
                startMotion(rider->bikeModel, riderMotion(static_cast<uint16_t>(rider->motion)), ee::Float(0.0f));
                root = zge::kZero;
                bikeRoot = zge::kZero;
                rider->motionTime = ee::Float(0.0f);
            }
            // (Both of the original's paths make the same two calls.)
            setMotionTime(rider->messenger, rider->motionTime);
            setMotionTime(rider->bikeModel, rider->motionTime);
            if (rider->motionFlags & kMotionFlagLean)
            {
                const ee::Float left = rider->leanBlend[0];
                if (ee::Float(0.0f) < left)
                {
                    blendMotion(rider->messenger, riderMotion(rider->leanMotions[0]), rider->motionTime, left);
                    blendMotion(rider->bikeModel, riderMotion(rider->leanMotions[0]), rider->motionTime, rider->leanBlend[0]);
                }
                else
                {
                    const ee::Float right = rider->leanBlend[1];
                    if (ee::Float(0.0f) < right)
                    {
                        blendMotion(rider->messenger, riderMotion(rider->leanMotions[1]), rider->motionTime, right);
                        blendMotion(rider->bikeModel, riderMotion(rider->leanMotions[1]), rider->motionTime, rider->leanBlend[1]);
                    }
                }
            }
            if (motionDone(rider->messenger, rider->motionTime + rider->motionRate * *g_motionEndAhead) != 0)
                rider->flags |= kFlagMotionEnded;
            else
            {
                const uint32_t mf = rider->motionFlags;
                const uint32_t flags = rider->flags & ~kFlagMotionEnded;
                rider->motionFlags = mf & ~kMotionRestart;
                rider->flags = flags;
            }
            if (rider->flags & kFlagMirrored)
            {
                // Mirrored: both models' poses mirrored, the bike's sprocket
                // turned (its quaternion's x and w swapped, w negated).
                zge::mirrorPose(rider->riderMirror);
                zge::mirrorPose(rider->bikeMirror);
                zge::quaternionCopy(sprocketTurn, zge::boneRotation(rider->sprocketBone));
                const ee::Float w = -sprocketTurn->w;
                sprocketTurn->w = sprocketTurn->x;
                sprocketTurn->x = w;
                zge::setBoneRotation(rider->sprocketBone, sprocketTurn);
            }
            poseModel(rider->messenger);
            poseModel(rider->bikeModel);
            // The bones the physics reads.
            root = zge::quadwordOf(zge::boneTranslation(boneOf(rider->messenger, *g_hipsName)));
            bikeRoot = zge::quadwordOf(zge::boneTranslation(boneOf(rider->bikeModel, *g_hipsDeckName)));
            rider->headBone = boneOf(rider->messenger, *g_headName);
            rider->bikeBarsBone = boneOf(rider->bikeModel, *g_handlebarsName);
            rider->sprocketBone = boneOf(rider->bikeModel, *g_sprocketName);
            rider->wheelPoses[0].bone = boneOf(rider->bikeModel, *g_frontWheelName);
            rider->wheelPoses[1].bone = boneOf(rider->bikeModel, *g_backWheelName);
            const uint32_t model = rider->messenger;
            rider->motionFlags &= ~kMotionFlagTurnRound & ~kMotionFlagLean;
            poseModel(model);
            rider->motionTime = rider->motionTime + rider->motionRate * *g_motionStep;
        }

        if (rider->soundWait != 0)
            --rider->soundWait;
        ++rider->balanceFrames;
        physics::physicsFrame(rider);

        // The bail's body kept with the rider: his rows, position and
        // velocity (a second's worth, less a unit up), no spin.
        if (!(rider->flags & kFlagNoPlayer))
        {
            zge::RigidBody &body = *rider->bailBody;
            physics::engine::copyMatrix(&body.rotation, rider->right);
            body.position = *vec4(rider->position);
            sweepUpdateByCall(levelSweep(), rider->bailBody);
            body.velocity = zge::scaledCarrying(*vec4(rider->velocity), ee::Float(60.0f), zge::scaleLeftover(zge::LeftBy::lastCall));
            body.velocity.y = body.velocity.y - ee::Float(1.0f);
            body.spin = zge::kZero;
        }

        // In a bail the grind button reaches with the hands.
        if (actionState(rider, uint32_t(kLevelAction)) == kActionBail &&
            moveButtons(uint32_t(rider->controllerLayout), &rider->input, kMoveGrind, kMoveHeld) != 0)
        {
            const uint32_t flags = rider->flags | kFlagBailHands;
            rider->flags = flags;
            return static_cast<int32_t>(flags);
        }
        rider->flags &= 0x7FFFFFFFu;
        return 0x7FFFFFFF;
    }
    READABLE_FUNCTION(riderFrame, Verified);
}
