// BMX XXX's rider frame (action.c): the direction taps (forgotten after 40 frames without one), out of the level's
// bounds back to the start, the action state's callbacks until one keeps its state, the motions stepped (the rider's
// and the bike's: started, timed, blended with the lean's, ended, the bones the physics reads, the eyelids' blink),
// the physics frame, the bail's body kept with the rider, the shooter's frame and the soft parts'. DM2's riderFrame
// (games/dm2/rider/actions/frame.cpp) with the motions in a function of their own.

#include "../physics/physics.h"
#include "../rider.h"
#include "../vector.h"

#include "bmx/level/level_collision.h"
#include "bmx/weapon/weapon.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "sym/bmx/rider/actions/frame.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The motion root's rotation as the quaternion it is.
        ee::Ptr<zge::Quaternion> motionRootRotation(Rider *rider) { return ee::Ptr<ee::Float>(rider->motionRootRotation).cast<zge::Quaternion>(); }
        // The bones' name hashes (rider_state.cpp's kBoneNames): the rider's "Hips", "Head" and "Eyelids", the
        // bike's "Hipsdeck", "Handlebars", "Sprocket", "FrontWheel" and "BackWheel".
        inline ee::Global<uint32_t, sym::g_hipsBoneHash> g_hipsName;
        inline ee::Global<uint32_t, sym::g_headBoneHash> g_headName;
        inline ee::Global<uint32_t, sym::g_eyelidsBoneHash> g_eyelidsName;
        inline ee::Global<uint32_t, sym::g_hipsDeckBoneHash> g_hipsDeckName;
        inline ee::Global<uint32_t, sym::g_handlebarsBoneHash> g_handlebarsName;
        inline ee::Global<uint32_t, sym::g_sprocketBoneHash> g_sprocketName;
        inline ee::Global<uint32_t, sym::g_frontWheelBoneHash> g_frontWheelName;
        inline ee::Global<uint32_t, sym::g_backWheelBoneHash> g_backWheelName;
        constexpr uint32_t kLeanChannels = 17;

        // The eyelids' steps (Rider::eyelidState): a blink shuts them over 15 frames and opens them over 6, then
        // they stay open 30 frames and up to 240 more (a random sixteenth of 256); the idle shuts them over 60
        // frames and keeps them shut, waking opens them over 15 (then open 15 frames before the next blink).
        enum EyelidStep : uint8_t
        {
            kEyelidsBlinkShutting = 0,
            kEyelidsBlinkOpening = 1,
            kEyelidsOpen = 2,
            kEyelidsIdle = 3, // shutting as the rider dozes off (coastState)
            kEyelidsAsleep = 4,
            kEyelidsWoken = 5, // opening (coastState)
        };
        constexpr uint16_t kBlinkShutFrames = 15;
        constexpr uint16_t kBlinkOpenFrames = 6;
        constexpr uint16_t kDozeFrames = 60;
        constexpr uint16_t kWakeFrames = 15;
        constexpr uint16_t kOpenLeast = 30;
        constexpr ee::Float kOpenSpread(16.0f);
        constexpr uint16_t kOpenAfterWaking = 15;
        // The bone's turn open (about its x axis, a share of a turn: -30 degrees); shut is none.
        inline ee::Global<ee::Float, sym::g_eyelidsOpenTurn> g_eyelidsOpenTurn;

        // The engine's lean blend: its quadword of scale words is what the original's call leaves in that
        // register, no value of the caller's, so the call keeps the address form and names the other words.
        // Scaffolding (address-call): docs/SCAFFOLDING.md
        void blendLean(ee::Ptr<zge::SceneObject> model, ee::Ptr<zge::Motion> motion, ee::Float time, ee::Float weight)
        {
            ee::game(sym::blendLayer, model, motion, 0u, 0u, time, weight);
        }

        // The eyelids' step this frame: how far shut they are.
        void stepEyelids(Rider *rider)
        {
            switch (rider->eyelidState)
            {
            case kEyelidsBlinkShutting:
            case kEyelidsIdle:
            {
                const uint16_t steps = rider->eyelidState == kEyelidsBlinkShutting ? kBlinkShutFrames : kDozeFrames;
                if (rider->eyelidFrames < steps)
                {
                    const ee::Float share = ee::toFloat(int32_t(rider->eyelidFrames)) * (ee::Float(1.0f) / ee::toFloat(int32_t(steps)));
                    ++rider->eyelidFrames;
                    rider->eyelidsShut = share;
                    return;
                }
                ++rider->eyelidState; // to kEyelidsBlinkOpening, kEyelidsAsleep
                rider->eyelidsShut = 1.0f;
                rider->eyelidFrames = 0;
                return;
            }
            case kEyelidsBlinkOpening:
            case kEyelidsWoken:
            {
                const uint16_t steps = rider->eyelidState == kEyelidsBlinkOpening ? kBlinkOpenFrames : kWakeFrames;
                if (rider->eyelidFrames < steps)
                {
                    const ee::Float share = ee::toFloat(int32_t(rider->eyelidFrames)) * (ee::Float(1.0f) / ee::toFloat(int32_t(steps)));
                    ++rider->eyelidFrames;
                    rider->eyelidsShut = ee::Float(1.0f) - share;
                    return;
                }
                if (rider->eyelidState == kEyelidsBlinkOpening)
                {
                    const int32_t spread = static_cast<int32_t>(zge::randomNumber() & 0xF);
                    rider->eyelidFrames = static_cast<uint16_t>(ee::toInt(ee::toFloat(spread) * kOpenSpread) + kOpenLeast);
                }
                else
                    rider->eyelidFrames = kOpenAfterWaking;
                rider->eyelidsShut = 0.0f;
                rider->eyelidState = kEyelidsOpen;
                return;
            }
            case kEyelidsOpen:
                // Open until the frames run out (past 0), then a blink.
                if (--rider->eyelidFrames == 0xFFFF)
                {
                    rider->eyelidFrames = 0;
                    rider->eyelidState = kEyelidsBlinkShutting;
                }
                rider->eyelidsShut = 0.0f;
                return;
            case kEyelidsAsleep:
                rider->eyelidsShut = 1.0f;
                return;
            default:
                return;
            }
        }
    }

    // stepRiderMotion (rider, seconds): the motions' step on both models (unless kMotionFlag4 holds them): the lean's
    // weights, a restart from the start (the roots back to none), the time set, the lean blended in (whichever side
    // is on), the end looked for a step ahead (else kFlagMotionRestarted after a restart), the pose mirrored with the
    // bike, both posed, the bones the physics reads found again, the clock stepped; then the eyelids' step turns their
    // bone. Returns 1 when the motion ended. DM2's riderFrame's motions, with BMX XXX's root rotation and eyelids.
    int32_t stepRiderMotion(Rider *rider, ee::Float seconds)
    {
        // The eyelids' turn and rotation and the sprocket's mirrored turn, in the order the original's frame keeps
        // them (the sprocket's after the eyelids', though it is used first).
        ee::Local<zge::Matrix> eyelidsTurn;
        ee::Local<zge::Quaternion> eyelidsRotation;
        ee::Local<zge::Quaternion> sprocketTurn;
        const ee::Ptr<zge::SceneObject> model = rider->messenger;
        const ee::Ptr<zge::SceneObject> bike = rider->bikeModel;
        if (rider->motionFlags & kMotionLean)
        {
            for (uint32_t i = 0; i < kLeanChannels; ++i)
            {
                rider->leanWeights[0][i].weight = rider->leanBlend[0];
                rider->leanWeights[1][i].weight = rider->leanBlend[1];
            }
        }
        if (!(rider->motionFlags & kMotionFlag4))
        {
            zge::Vec4 &root = *vec4(rider->motionRoot);
            zge::Vec4 &bikeRoot = *vec4(rider->drawnOffset);
            if (rider->motionFlags & kMotionRestart)
            {
                zge::startAnimation(model, rider->motion, 0.0f);
                zge::startAnimation(bike, rider->motion, 0.0f);
                root = zge::kZero;
                rider->motionRootRotation[0] = 0.0f;
                rider->motionRootRotation[1] = 0.0f;
                rider->motionRootRotation[2] = 0.0f;
                rider->motionRootRotation[3] = 0.0f;
                bikeRoot = zge::kZero;
                rider->motionTime = 0.0f;
            }
            // (Both of the original's paths, kMotionFlag2 set or not, make the same two calls.)
            zge::updateAnimation(model, 0u, rider->motionTime);
            zge::updateAnimation(bike, 0u, rider->motionTime);
            if (rider->motionFlags & kMotionLean)
            {
                if (ee::Float(0.0f) < rider->leanBlend[0])
                {
                    blendLean(model, rider->leanMotions[0], rider->motionTime, rider->leanBlend[0]);
                    blendLean(bike, rider->leanMotions[0], rider->motionTime, rider->leanBlend[0]);
                }
                else if (ee::Float(0.0f) < rider->leanBlend[1])
                {
                    blendLean(model, rider->leanMotions[1], rider->motionTime, rider->leanBlend[1]);
                    blendLean(bike, rider->leanMotions[1], rider->motionTime, rider->leanBlend[1]);
                }
            }
            if (zge::animationDone(model, rider->motionTime + seconds * rider->motionRate) != 0)
                setFlag(rider, kFlagMotionEnded);
            else
            {
                if (rider->motionFlags & kMotionRestart)
                    setFlag(rider, kFlagMotionRestarted);
                rider->motionFlags &= ~kMotionRestart;
                clearFlag(rider, kFlagMotionEnded);
            }
            if (hasFlag(rider, kFlagMirrored))
            {
                // Mirrored: both models' poses mirrored, the bike's sprocket turned (its quaternion's x and w
                // swapped, w negated).
                zge::mirrorPose(rider->riderMirror);
                zge::mirrorPose(rider->bikeMirror);
                zge::quaternionCopy(sprocketTurn, zge::boneRotation(rider->sprocketBone));
                const ee::Float w = -sprocketTurn->w;
                sprocketTurn->w = sprocketTurn->x;
                sprocketTurn->x = w;
                zge::setBoneRotation(rider->sprocketBone, sprocketTurn);
            }
            zge::updatePose(model);
            zge::updatePose(bike);
            // The bones the physics reads: the hips' place and turn, the bike's root, then the bones themselves.
            const ee::Ptr<zge::AnimationChannel> hips = zge::findAnimatorBone(model, *g_hipsName);
            root = *zge::boneTranslation(hips);
            zge::quaternionCopy(motionRootRotation(rider), zge::boneRotation(hips));
            bikeRoot = *zge::boneTranslation(zge::findAnimatorBone(bike, *g_hipsDeckName));
            rider->headBone = zge::findAnimatorBone(model, *g_headName);
            rider->bikeBarsBone = zge::findAnimatorBone(bike, *g_handlebarsName);
            rider->sprocketBone = zge::findAnimatorBone(bike, *g_sprocketName);
            rider->wheelPoses[0].bone = zge::findAnimatorBone(bike, *g_frontWheelName);
            rider->wheelPoses[1].bone = zge::findAnimatorBone(bike, *g_backWheelName);
            rider->motionFlags &= ~kMotionFlag2 & ~kMotionLean;
            zge::updatePose(model);
            rider->motionTime = rider->motionTime + rider->motionRate * seconds;
        }

        // The eyelids (a model without the bone has none).
        const ee::Ptr<zge::AnimationChannel> eyelids = zge::findAnimatorBone(model, *g_eyelidsName);
        if (eyelids)
        {
            stepEyelids(rider);
            zge::matrixIdentity(eyelidsTurn);
            const zge::Angle open = static_cast<zge::Angle>(ee::toInt((ee::Float(1.0f) - rider->eyelidsShut) * *g_eyelidsOpenTurn * ee::Float(65536.0f)));
            zge::matrixRotateX(eyelidsTurn, eyelidsTurn, open);
            zge::matrixToQuaternion(eyelidsRotation, eyelidsTurn);
            zge::setBoneRotation(eyelids, eyelidsRotation);
        }
        return hasFlag(rider, kFlagMotionEnded) ? 1 : 0;
    }
    READABLE_FUNCTION(stepRiderMotion, Verified);

    namespace
    {
        using ActionStateFn = ee::FunctionPtr<int32_t(Rider *rider)>;
        // The action states' callbacks (one a level-0 state; ActionState): non-zero when the rider stays in it.
        inline ee::Global<ActionStateFn[11], sym::actionStates> g_actionStates;
        // A direction by its button bits (kButtonUp .. kButtonRight).
        inline ee::Global<uint8_t[16], sym::g_directionByButtons> g_directionByButtons;
        // The frame count the taps' times are taken from (a byte of it).
        inline ee::Global<uint8_t, sym::g_gameFrames> g_gameFrames;
        constexpr uint8_t kMaxTaps = 4;
        constexpr uint16_t kTapsForgottenFrames = 40;
        // The motions' and the soft parts' step a frame, in seconds (a sixtieth).
        inline ee::Global<ee::Float, sym::g_riderFrameSeconds> g_riderFrameSeconds;
        constexpr ee::Float kFramesASecond(60.0f);

        // The engine's sweep update of the bail's body, as a guest call: the scale after it carries what it left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        namespace calls
        {
            inline void sweepUpdateBody(ee::Ptr<zge::Sweep> sweep, ee::Ptr<zge::RigidBody> body) { ee::game(sym::sweepUpdateBody, sweep, body); }
        }

        // The direction bits now (held) and changed, the stick past 0.5 on each axis counting as its direction (now
        // or a frame ago). DM2's.
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

        // The direction taps: a direction just pushed (only presses changed) is a new tap (the oldest of four
        // dropped); a change with a press among others replaces the last tap (or is the first); each with the frame
        // it came in. With none for 40 frames the taps are forgotten.
        void readTaps(Rider *rider)
        {
            uint32_t held = rider->input.held & kButtonDirections;
            uint32_t changed = rider->input.changed & kButtonDirections;
            stickBits(rider->input, held, changed);
            if (changed == 0)
            {
                if (++rider->tapIdleFrames > kTapsForgottenFrames)
                {
                    for (uint32_t i = 0; i < kMaxTaps; ++i)
                        rider->taps[i] = 0;
                    rider->tapCount = 0;
                    rider->tapIdleFrames = 0;
                }
                return;
            }
            if (changed == held)
            {
                if (rider->tapCount == kMaxTaps)
                {
                    for (uint32_t i = 0; i < uint32_t(rider->tapCount) - 1; ++i)
                    {
                        rider->taps[i] = rider->taps[i + 1];
                        rider->tapTimes[i] = rider->tapTimes[i + 1];
                    }
                    rider->tapCount = static_cast<uint8_t>(rider->tapCount - 1);
                }
                rider->taps[rider->tapCount] = (*g_directionByButtons)[held];
                rider->tapTimes[rider->tapCount] = *g_gameFrames;
                rider->tapIdleFrames = 0;
                ++rider->tapCount;
                return;
            }
            if (held & changed)
            {
                if (rider->tapCount != 0)
                {
                    rider->taps[rider->tapCount - 1] = (*g_directionByButtons)[held];
                    rider->tapTimes[rider->tapCount - 1] = *g_gameFrames;
                }
                else
                {
                    rider->taps[rider->tapCount] = (*g_directionByButtons)[held];
                    rider->tapTimes[rider->tapCount] = *g_gameFrames;
                    ++rider->tapCount;
                }
                rider->tapIdleFrames = 0;
            }
        }
    }

    // riderFrame (index): a rider's frame. Its models shown, the taps read, out of the level (a unit past its
    // bounds; not below) back to the start riding; the frame's flags cleared and the shooter kept from the buttons
    // and the crosshair while the camera follows the rider; the action state's callbacks until one keeps its state;
    // the motions stepped a sixtieth; the waits counted down; the physics frame; the bail's body kept with the drawn
    // rider (his body's rows and place, the front wheel's velocity, a second's, less a unit up, no spin) and the
    // shooter's frame while the camera follows him; the soft parts stepped (the original's tail call). DM2's
    // riderFrame, which returns the bail hands' flag (BMX XXX's bail asks for them itself).
    void riderFrame(uint32_t index)
    {
        Rider *const rider = riderOf(index & 0xFFu).get();
        setFlag(rider, kFlagRiderModelShown);
        setFlag(rider, kFlagBikeModelShown);
        if (hasFlag(rider, kFlagTapsOn))
            readTaps(rider);

        const Bounds *const bounds = levelBounds();
        const ee::Float *const at = rider->position;
        if (bounds->max[1] + ee::Float(1.0f) < at[1] || at[0] < bounds->min[0] - ee::Float(1.0f) ||
            bounds->max[0] + ee::Float(1.0f) < at[0] || at[2] < bounds->min[2] - ee::Float(1.0f) ||
            bounds->max[2] + ee::Float(1.0f) < at[2])
        {
            resetPhysics(rider, 0u);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
        }

        clearFlag(rider, kFlagActed);
        clearFlag(rider, kFlagCoastStarted);
        clearFlag(rider, kFlagGrindAsked);
        clearFlag(rider, kFlagBailHands);
        clearFlag(rider, kFlagObjectiveAsked);
        rider->pedalForce = 0.0f;
        if (!hasFlag(rider, kFlagNoPlayer))
            ee::Ptr<bmx::Shooter>(rider->shooter)->flags |= bmx::kShooterIgnoresButtons | bmx::kShooterNoCrosshair;
        while ((*g_actionStates)[rider->actionStates[kLevelAction]](rider) == 0)
        {
        }
        ++rider->actionFrames[kLevelAction];
        clearFlag(rider, kFlagMotionRestarted);
        const ee::Float seconds = *g_riderFrameSeconds;
        stepRiderMotion(rider, seconds);
        if (rider->voiceWait != 0)
            --rider->voiceWait;
        if (rider->bumpWait != 0)
            --rider->bumpWait;
        ++rider->balanceFrames;
        physics::physicsFrame(rider);

        if (!hasFlag(rider, kFlagNoPlayer))
        {
            const ee::Ptr<zge::RigidBody> body = rider->bailBody;
            body->rotation = *matrix(rider->body);
            body->position = *vec4(rider->drawnPosition);
            calls::sweepUpdateBody(levelSweep(), rider->bailBody);
            body->velocity = zge::scaledCarrying(*vec4(rider->wheelPoses[0].velocity), kFramesASecond, zge::scaleLeftover(zge::LeftBy::lastCall));
            body->velocity.y = body->velocity.y - ee::Float(1.0f);
            body->spin = zge::kZero;
            bmx::updateShooter(ee::Ptr<bmx::Shooter>(rider->shooter).get());
        }
        if (rider->softParts)
            game::tail(zge::stepSoftPartsRig, rider->softParts, seconds);
    }
    READABLE_FUNCTION(riderFrame, Verified);
}
