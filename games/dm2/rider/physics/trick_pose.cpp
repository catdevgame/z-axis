// DM2's trick pose (physics.c; groundAfter's and airAfter's last call): a
// trick's bone poses, one bit a pose in the table g_trickFlags2 by the trick's
// motion (Rider::trickMotion): the bars turned or spun, the cranks turned,
// a hand or foot let go and moved, or a hand or foot put on a bike part,
// each blended in by the trick pose's share (Rider::topTrickBlend).
//
// Counterparts: DM2 GameCube 0x80085000;
// Xbox 0x04F020; BMX XXX PS2 poseRiderBones (medium).

#include "physics.h"

#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/physics/trick_pose.cpp.h"

namespace game::dm2::rider::physics
{
    namespace
    {
        // DM2's engine's call the poses make (its DM2 body is not written
        // yet): a model's parts' matrices from its bones.
        // Scaffolding (address-call): docs/SCAFFOLDING.md
        inline void poseModel(uint32_t model) { ee::game(sym::modelPose, model); }
        // As guest calls: the scales after them carry what they left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline ee::Ptr<zge::AnimationChannel> boneByCall(uint32_t model, uint32_t nameHash)
        {
            return ee::Ptr<zge::AnimationChannel>(ee::game<uint32_t>(sym::findAnimatorBone, model, nameHash));
        }
        inline void bonePointByCall(uint32_t model, ee::Ptr<zge::SceneBone> part, const zge::Vec4 &point, zge::Vec4 &out)
        {
            ee::game(sym::objectBonePoint, model, part, &point, &out, 0u);
        }
        // (0, 0, 0, 0): a part's own point.
        inline ee::Global<zge::Vec4, sym::origin> g_origin;

        // The poses a trick's motion has (one bit a pose; 0: none).
        inline ee::Global<uint32_t[256], sym::g_trickFlags2> g_trickPoses;
        constexpr uint32_t kPoseBits = 31;
        enum Pose : uint32_t
        {
            kPoseHeadLeft = 0x1u,      // the rider's head turned 45 degrees
            kPoseHeadRight = 0x2u,
            kPoseBarSpin = 0x4u,       // the bike's bars spun over the trick (0x8 the other way back)
            kPoseBarSpinBack = 0x8u,
            kPoseCrankSpin = 0x10u,    // the sprocket (the cranks) turned over the trick
            kPoseBarsTurnLeft = 0x20u, // the bike's bars turned by 0.375 of a turn times the share
            kPoseBarsTurnRight = 0x40u,
            kPoseHandOffFirst = 0x80u, // 0x80 .. 0x8000: a hand or foot off, moved (0x8000 from the body)
            kPoseHandFromBody = 0x8000u,
            kPoseOnBikeFirst = 0x10000u, // 0x10000 ..: a hand or foot put on a bike part
        };
        constexpr int32_t kHeadTurn = 0x2000;

        // The limbs (Rider::limbTargets) and the bones' name hashes
        // (rider_state.cpp's kBoneNames): the rider's hands and feet, the
        // bike's parts a limb is put on.
        inline ee::Global<uint32_t, sym::g_leftHandBoneHash> g_leftHandName;
        inline ee::Global<uint32_t, sym::g_rightHandBoneHash> g_rightHandName;
        inline ee::Global<uint32_t, sym::g_leftFootBoneHash> g_leftFootName;
        inline ee::Global<uint32_t, sym::g_rightFootBoneHash> g_rightFootName;
        inline ee::Global<uint32_t, sym::g_hipsDeckBoneHash> g_hipsDeckName;
        inline ee::Global<uint32_t, sym::g_handlebarsBoneHash> g_handlebarsName;
        inline ee::Global<uint32_t, sym::g_frontWheelBoneHash> g_frontWheelName;
        inline ee::Global<uint32_t, sym::g_backWheelBoneHash> g_backWheelName;
        enum Limb : uint32_t
        {
            kLeftHand = 0,
            kRightHand = 1,
            kLeftFoot = 2,
            kRightFoot = 3,
        };

        // The limbs' offsets off the bike's parts and their swing.
        inline ee::Global<ee::Float, sym::g_offsetA> g_offsetA;    // 0.1
        inline ee::Global<ee::Float, sym::g_offsetB> g_offsetB;    // -0.2
        inline ee::Global<ee::Float, sym::g_offsetC> g_offsetC;    // -0.23
        inline ee::Global<ee::Float, sym::g_offsetD> g_offsetD;    // 0.05
        inline ee::Global<ee::Float, sym::g_leftHandOut> g_leftHandOut;    // 0.6 (0x80)
        inline ee::Global<ee::Float, sym::g_rightHandOut> g_rightHandOut;   // -0.6 (0x100)
        inline ee::Global<ee::Float, sym::g_leftHandUp> g_leftHandUp;     // 0.55 (0x200)
        inline ee::Global<ee::Float, sym::g_rightHandUp> g_rightHandUp;    // -0.55 (0x400)
        inline ee::Global<ee::Float, sym::g_leftFootOut> g_leftFootOut;    // 0.7 (0x800)
        inline ee::Global<ee::Float, sym::g_rightFootOut> g_rightFootOut;   // -0.7 (0x1000)
        inline ee::Global<ee::Float, sym::g_leftFootBack> g_leftFootBack;   // -0.3 (0x2000)
        inline ee::Global<ee::Float, sym::g_rightFootBack> g_rightFootBack;  // 0.3 (0x4000)
        inline ee::Global<ee::Float, sym::g_fromBodyUp> g_fromBodyUp;   // -0.1
        inline ee::Global<ee::Float, sym::g_fromBodyBack> g_fromBodyBack; // -0.3
        inline ee::Global<ee::Float, sym::g_swingRate> g_swingRate;    // 1 / 15 of a turn a frame
        inline ee::Global<ee::Float, sym::g_swingX> g_swingX;       // 0.03
        inline ee::Global<ee::Float, sym::g_swingY> g_swingY;       // 0.02
        inline ee::Global<ee::Float, sym::g_onFrameX> g_onFrameX;     // -0.1 (the right hand on the front wheel)
        inline ee::Global<ee::Float, sym::g_onLeftPegX> g_onLeftPegX;   // 0.15 (the left foot on the back wheel)
        inline ee::Global<ee::Float, sym::g_onRightPegX> g_onRightPegX;  // -0.15 (the right foot on the back wheel)

        // The limb's bone turned back towards its rest by the share (its
        // quaternion's vector part scaled by 1 - share).
        void easeBone(Rider *rider, ee::Ptr<zge::AnimationChannel> bone, zge::Quaternion &q)
        {
            zge::quaternionCopy(&q, zge::boneRotation(bone));
            const ee::Float keep = ee::Float(1.0f) - rider->topTrickBlend;
            const ee::Float x = q.x * keep;
            const ee::Float y = q.y * keep;
            q.z = q.z * keep;
            q.x = x;
            q.y = y;
            zge::setBoneRotation(bone, &q);
        }
        // A bike bone's rotation set, the bike's parts' matrices updated.
        void setBikeBone(Rider *rider, ee::Ptr<zge::AnimationChannel> bone, zge::Quaternion &q)
        {
            zge::setBoneRotation(bone, &q);
            poseModel(rider->bikeModel);
        }
        // The bike's bars bone turned by `yaw`, tilted by the bars' tilt.
        void turnBars(Rider *rider, zge::Matrix &turn, zge::Quaternion &q, int32_t yaw)
        {
            zge::matrixIdentity(&turn);
            zge::matrixRotateY(&turn, &turn, yaw);
            zge::matrixRotateX(&turn, &turn, rider->barsTilt);
            zge::matrixToQuaternion(&q, &turn);
            setBikeBone(rider, rider->bikeBarsBone, q);
        }

        // A pose's code, by its bit.
        enum PoseKind
        {
            kKindHeadTurn,  // 0x1, 0x2
            kKindBarSpin,   // 0x4, 0x8
            kKindCrankSpin, // 0x10
            kKindBarsTurn,  // 0x20, 0x40
            kKindLimbOff,   // 0x80 .. 0x8000
            kKindOnBike,    // 0x10000 ..
        };
        PoseKind poseKind(uint32_t pose)
        {
            if (pose <= kPoseBarSpinBack)
                return pose < kPoseBarSpin ? kKindHeadTurn : kKindBarSpin;
            if (pose == kPoseCrankSpin)
                return kKindCrankSpin;
            if (pose < kPoseHandOffFirst)
                return kKindBarsTurn;
            return pose <= kPoseHandFromBody ? kKindLimbOff : kKindOnBike;
        }
    }

    // The trick's poses (with kFlagTopTrick, a trick on, and poses for its
    // motion), bit by bit. Only groundAfter, airAfter and poseBikeGround
    // call it, and none reads a result. GameCube 0x80085000.
    //
    // The scales carry what the last call left, as airGravity's do.
    void poseRiderBones(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;      // the rider's rotation for the limbs
        ee::Local<zge::Matrix> back;      // its inverse
        ee::Local<zge::Vec4> offset;      // the limb's offset
        ee::Local<zge::Vec4> swing;
        ee::Local<zge::Vec4> at;          // the bike part's point
        ee::Local<zge::Vec4> from;        // the limb's point
        ee::Local<zge::Quaternion> q;

        if ((rider->flags & kFlagTopTrick) == 0)
            return;
        const uint32_t poses = (*g_trickPoses)[rider->trickMotion];
        if (poses == 0)
            return;
        const ee::Float offsetA = *g_offsetA;
        const ee::Float offsetB = *g_offsetB;
        const ee::Float offsetC = *g_offsetC;
        const ee::Float offsetD = *g_offsetD;
        const ee::Ptr<zge::SceneObject> riderModel(rider->messenger);
        const ee::Ptr<zge::SceneObject> bikeModel(rider->bikeModel);

        // One pose (a single bit).
        const auto poseOne = [&](uint32_t pose)
        {
            switch (poseKind(pose))
            {
            case kKindHeadTurn:
                // The rider's head turned 45 degrees by the share.
                zge::matrixIdentity(turn);
                zge::matrixRotateY(turn, turn,
                                   static_cast<int16_t>(ee::toInt(ee::toFloat(pose == kPoseHeadLeft ? kHeadTurn : -kHeadTurn) *
                                                                  ee::Float(1.0f / 65536.0f) * rider->topTrickBlend * ee::Float(65536.0f))));
                zge::matrixToQuaternion(q, turn);
                zge::setBoneRotation(rider->headBone, q);
                return;
            case kKindBarSpin:
            {
                if (!(ee::Float(0.0f) < rider->topTrickBlend))
                    return;
                const uint16_t frames = rider->actionFrames[1];
                int16_t yaw = static_cast<int16_t>(
                    ee::toInt(ee::toFloat(int32_t(frames)) / ee::toFloat(int32_t(rider->topTrickLoopFrames)) * ee::Float(65536.0f)));
                if (pose == kPoseBarSpinBack && frames >= rider->topTrickLoopFrames)
                    yaw = static_cast<int16_t>(-yaw);
                turnBars(rider, *turn, *q, yaw);
                return;
            }
            case kKindCrankSpin:
            {
                if (!(ee::Float(0.0f) < rider->topTrickBlend))
                    return;
                const int16_t crank = static_cast<int16_t>(-ee::toInt(ee::toFloat(int32_t(rider->actionFrames[1])) /
                                                                      ee::toFloat(int32_t(rider->topTrickLoopFrames)) * ee::Float(65536.0f)));
                zge::matrixIdentity(turn);
                zge::matrixRotateX(turn, turn, crank);
                zge::matrixToQuaternion(q, turn);
                setBikeBone(rider, rider->sprocketBone, *q);
                return;
            }
            case kKindBarsTurn:
            {
                int16_t yaw = static_cast<int16_t>(ee::toInt(rider->topTrickBlend * ee::Float(0.375f) * ee::Float(65536.0f)));
                if (pose == kPoseBarsTurnRight)
                    yaw = static_cast<int16_t>(-yaw);
                turnBars(rider, *turn, *q, yaw);
                return;
            }
            case kKindLimbOff:
            {
                // A limb off the bike: its offset in the rider's frame (from
                // the body for 0x8000), swung over the trick, scaled by the
                // share, from the limb's point.
                zge::quaternionToMatrix(turn, zge::objectOrientation(riderModel));
                turn->row(3) = zge::kZero;
                *offset = zge::kZero;
                *swing = zge::kZero;
                uint32_t limb;
                uint32_t name;
                switch (pose)
                {
                case 0x80u:
                default:
                    limb = kLeftHand;
                    offset->x = *g_leftHandOut;
                    offset->y = 0.0f;
                    offset->z = 0.0f;
                    name = *g_leftHandName;
                    break;
                case 0x100u:
                    limb = kRightHand;
                    offset->x = *g_rightHandOut;
                    offset->y = 0.0f;
                    offset->z = 0.0f;
                    name = *g_rightHandName;
                    break;
                case 0x200u:
                    limb = kLeftHand;
                    offset->x = *g_leftHandUp;
                    offset->y = offsetA;
                    offset->z = offsetB;
                    name = *g_leftHandName;
                    break;
                case 0x400u:
                    limb = kRightHand;
                    offset->x = *g_rightHandUp;
                    offset->y = offsetA;
                    offset->z = offsetB;
                    name = *g_rightHandName;
                    break;
                case 0x800u:
                    limb = kLeftFoot;
                    offset->y = 0.0f;
                    offset->x = *g_leftFootOut;
                    offset->z = offsetB;
                    name = *g_leftFootName;
                    break;
                case 0x1000u:
                    limb = kRightFoot;
                    offset->y = 0.0f;
                    offset->x = *g_rightFootOut;
                    offset->z = offsetB;
                    name = *g_rightFootName;
                    break;
                case 0x2000u:
                    limb = kLeftFoot;
                    offset->x = *g_leftFootBack;
                    offset->y = *g_leftFootBack;
                    offset->z = offsetB;
                    name = *g_leftFootName;
                    break;
                case 0x4000u:
                    limb = kRightFoot;
                    offset->x = *g_rightFootBack;
                    offset->y = *g_rightFootBack;
                    offset->z = offsetB;
                    name = *g_rightFootName;
                    break;
                case kPoseHandFromBody:
                    limb = kRightHand;
                    name = *g_rightHandName;
                    break;
                }
                ee::Ptr<zge::SceneBone> part;
                ee::Ptr<zge::AnimationChannel> bone;
                if (pose != kPoseHandFromBody)
                {
                    part = zge::findModelBone(riderModel, name);
                    bone = boneByCall(rider->messenger, name);
                }
                else
                {
                    // The right hand from where it is to the body's centre,
                    // in the rider's frame, a little up and back.
                    part = zge::findModelBone(riderModel, *g_rightHandName);
                    bone = zge::findAnimatorBone(riderModel, *g_rightHandName);
                    *at = *vec4(rider->position) + *vec4(rider->rootOffset);
                    zge::objectBonePoint(riderModel, part, &*g_origin, from, 0u);
                    *offset = *at - *from;
                    zge::matrixTranspose3(back, turn);
                    back->row(3) = zge::kZero;
                    engine::transformPoint(&offset->x, back, &offset->x);
                    const ee::Float z = offset->z + *g_fromBodyBack;
                    offset->y = offset->y + *g_fromBodyUp;
                    offset->z = z;
                }
                *offset = zge::scaledCarrying(*offset, rider->topTrickBlend, zge::scaleLeftover(zge::LeftBy::lastCall));
                const int16_t phase = static_cast<int16_t>(ee::toInt(ee::toFloat(int32_t(rider->actionFrames[1])) * *g_swingRate * ee::Float(65536.0f)));
                if (pose != kPoseHandFromBody)
                {
                    swing->x = engine::cosTurn(phase) * *g_swingX;
                    const ee::Float s = engine::sinTurn(phase);
                    const ee::Float share = rider->topTrickBlend;
                    swing->y = s * *g_swingY;
                    *swing = zge::scaledCarrying(*swing, share, zge::scaleLeftover(zge::LeftBy::lastCall));
                    *offset = *offset + *swing;
                }
                engine::transformPoint(&offset->x, turn, &offset->x);
                zge::objectBonePoint(riderModel, part, &*g_origin, from, 0u);
                *offset = *offset + *from;
                zge::aimBoneChain(&rider->limbTargets[limb], offset);
                easeBone(rider, bone, *q);
                return;
            }
            case kKindOnBike:
                break;
            }

            // A limb on a bike part: from the limb's point towards the part's
            // point (the pose's offset off it) by the share.
            *offset = zge::kZero;
            uint32_t limb = 0;
            uint32_t partName;
            uint32_t limbName;
            const ee::Float *copied = nullptr; // a rider pose copied into the offset
            switch (pose)
            {
            case kPoseOnBikeFirst:
            default: // (1 << 26, 1 << 27)
                limb = kLeftHand;
                partName = *g_handlebarsName;
                limbName = *g_leftHandName;
                copied = rider->grips[0];
                break;
            case (1u << 17):
                limb = kRightHand;
                partName = *g_handlebarsName;
                limbName = *g_rightHandName;
                copied = rider->grips[1];
                break;
            case (1u << 18):
                limb = kLeftHand;
                partName = *g_handlebarsName;
                limbName = *g_leftHandName;
                copied = rider->grips[1];
                break;
            case (1u << 19):
                limb = kRightHand;
                partName = *g_handlebarsName;
                limbName = *g_rightHandName;
                copied = rider->grips[0];
                break;
            case (1u << 20):
                limb = kLeftHand;
                partName = *g_hipsDeckName;
                limbName = *g_leftHandName;
                copied = rider->seatGrip;
                break;
            case (1u << 21):
                limb = kRightHand;
                partName = *g_hipsDeckName;
                limbName = *g_rightHandName;
                copied = rider->seatGrip;
                break;
            case (1u << 22):
                limb = kLeftHand;
                partName = *g_hipsDeckName;
                limbName = *g_leftHandName;
                break;
            case (1u << 23):
                limb = kRightHand;
                partName = *g_hipsDeckName;
                limbName = *g_rightHandName;
                break;
            case (1u << 30):
                limb = kLeftFoot;
                partName = *g_hipsDeckName;
                limbName = *g_leftFootName;
                break;
            case (1u << 24):
                limb = kLeftHand;
                partName = *g_frontWheelName;
                limbName = *g_leftHandName;
                break;
            case (1u << 25):
                limb = kRightHand;
                partName = *g_frontWheelName;
                limbName = *g_rightHandName;
                break;
            case (1u << 28):
                limb = kLeftFoot;
                partName = *g_backWheelName;
                limbName = *g_leftFootName;
                break;
            case (1u << 29):
                limb = kRightFoot;
                partName = *g_backWheelName;
                limbName = *g_rightFootName;
                break;
            }
            const ee::Ptr<zge::SceneBone> part = zge::findModelBone(bikeModel, partName);
            const ee::Ptr<zge::SceneBone> limbPart = zge::findModelBone(riderModel, limbName);
            const ee::Ptr<zge::AnimationChannel> bone = zge::findAnimatorBone(riderModel, limbName);
            if (copied != nullptr)
                *offset = *vec4(copied);
            else
            {
                switch (pose)
                {
                case (1u << 22):
                case (1u << 23):
                case (1u << 30):
                    offset->x = ee::Float(0.0f);
                    offset->y = offsetC;
                    offset->z = offsetD;
                    break;
                case (1u << 24):
                    offset->x = offsetA;
                    break;
                case (1u << 25):
                    offset->x = *g_onFrameX;
                    break;
                case (1u << 28):
                    offset->x = *g_onLeftPegX;
                    break;
                case (1u << 29):
                    offset->x = *g_onRightPegX;
                    break;
                }
            }
            zge::objectBonePoint(bikeModel, part, offset, at, 0u);
            bonePointByCall(rider->messenger, limbPart, *g_origin, *from);
            *offset = *at - *from;
            *offset = zge::scaledCarrying(*offset, rider->topTrickBlend, zge::scaleLeftover(zge::LeftBy::lastCall));
            *from = *from + *offset;
            zge::aimBoneChain(&rider->limbTargets[limb], from);
            easeBone(rider, bone, *q);
        };

        for (uint32_t bit = 0; bit < kPoseBits; ++bit)
            if (const uint32_t pose = poses & (1u << bit); pose != 0)
                poseOne(pose);
    }
    READABLE_FUNCTION(poseRiderBones, Verified);
}
