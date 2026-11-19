// A skater's model when a level starts (setupSkater): the node, then the
// bone physics its bones ask for, then its score, place and sweep box. The
// rigs go by bone names: the first of them found is what the model has.
//
//   Breast Lft            the bob (Player::bobAnimation, the Boob Tech and Mem
//                         Check tuning), and with the shirt handles the shirt
//                         over it (bodyAnimation)
//   Pt...ShirtHandle      a loose shirt (bodyAnimation)
//   Pt_Coattail_*         a coat's tails (bodyAnimation)
//   Ponytail Lft and Rt   two ponytails (upDownAnimation)
//   Ponytail_Single       one ponytail (upDownAnimation)
//   MargeTop, MargeBottom a tall hairdo (upDownAnimation)
//
// A rig is points held to bones or free, springs between them, pivots that
// turn the bones after the free points and planes that keep the points out
// of the body. sway.cpp keeps the bone controllers found here going.

#include "skater.h"

#include "aggressive/globals.h"
#include "aggressive/tuning.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/sweep.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "moves.h"
#include "zge/scene/scene_database.h"
#include "aggressive/level/level_collision.h"
#include "aggressive/fx/library.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/skater/rig.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kModelNameBytes = 0x19u;
        // The skaters' model names, 25 bytes each, by skater; "None" first.
        inline ee::Global<char[32][kModelNameBytes], sym::g_textNone> g_skaterModelNames;

        constexpr uint32_t kChest = sym::g_textChest;                // "Chest"
        constexpr uint32_t kBreastLeft = sym::g_textBreastLft;           // "Breast Lft"
        constexpr uint32_t kBreastRight = sym::g_textBreastRt;          // "Breast Rt"
        constexpr uint32_t kHips = sym::g_textHips;                 // "Hips"
        constexpr uint32_t kAbdomen = sym::g_textAbdomen;              // "Abdomen"
        constexpr uint32_t kHead = sym::g_textHead;                 // "Head"
        constexpr uint32_t kShirtLower = sym::g_textPtLowerShirtHandle;           // "PtLowerShirtHandle"
        constexpr uint32_t kShirtLeft = sym::g_textPtShirtHandleLeft;            // "PtShirtHandleLeft"
        constexpr uint32_t kShirtRight = sym::g_textPtShirtHandleRight;           // "PtShirtHandleRight"
        constexpr uint32_t kShirtUpper = sym::g_textPtUpperShirtHandle;           // "PtUpperShirtHandle"
        constexpr uint32_t kCoatLeftLower2 = sym::g_textPtCoattailLeftLower2;       // "Pt_Coattail_Left_Lower"
        constexpr uint32_t kCoatLeftUpper = sym::g_textPtCoattailLeftUpper;        // "Pt_Coattail_Left_upper"
        constexpr uint32_t kCoatRightUpper = sym::g_textPtCoattailRightUpper;       // "Pt_Coattail_Right_upper"
        constexpr uint32_t kCoatRightLower2 = sym::g_textPtCoattailRightLower2;      // "Pt_Coattail_Right_Lower"
        constexpr uint32_t kCoatLeftLower = sym::g_textPtCoattailLeftLower;        // "Pt_Coattail_Left_lower"
        constexpr uint32_t kCoatRightLower = sym::g_textPtCoattailRightLower;       // "Pt_Coattail_Right_lower"
        constexpr uint32_t kPonytailLeft1 = sym::g_textPonytailLft2;        // "Ponytail Lft 1"
        constexpr uint32_t kPonytailRight1 = sym::g_textPonytailRt;       // "Ponytail Rt 1"
        constexpr uint32_t kPonytailLeft2 = sym::g_textPonytailLft;        // "Ponytail Lft 2"
        constexpr uint32_t kPonytailRight2 = sym::g_textPonytailRt2;       // "Ponytail Rt 2"
        constexpr uint32_t kPonytailSingle = sym::g_textPonytailSingle;       // "Ponytail_Single"
        constexpr uint32_t kMargeTop = sym::g_textMargeTop;             // "MargeTop"
        constexpr uint32_t kMargeBottom = sym::g_textMargeBottom;          // "MargeBottom"

        // The BoobTech tuning entries the bob reads.
        constexpr uint32_t kDisplacementForce = 0, kDampingForce = 1;
        constexpr uint32_t kTutorialLevel = 7;

        // Where a rig's points are worked out: the vectors the system's
        // functions read their places and normals from.
        struct RigVectors
        {
            ee::Local<zge::Vec4> bodyPlace, boneFrom, boneTo, bodyNormal; // the bob and the shirt over it
            ee::Local<zge::Vec4> clothPlace, clothNormal;                 // a loose shirt, two ponytails
            ee::Local<zge::Vec4> tailPlace, tailNormal;                   // a coat, one ponytail
            ee::Local<zge::Vec4> hairPlace;                               // a tall hairdo
        };

        // A rig's work: the skater's bones by name, and the system's parts.
        struct Rig
        {
            ee::Ptr<Player> player;

            ee::Ptr<zge::SceneBone> bone(uint32_t name) const
            {
                const int32_t hash = zge::nameHash(ee::Ptr<const char>(name));
                return zge::findModelBone(player->node, hash);
            }
            // A point on a bone: held to it (1) or free.
            ee::Ptr<zge::AnimatePoint> point(ee::Ptr<zge::AnimateSystem> system, uint32_t name, uint32_t held, ee::Ptr<const zge::Vec4> place, uint32_t id, ee::Float mass) const
            {
                const ee::Ptr<zge::SceneBone> b = bone(name);
                return zge::newBoneAttachedPoint(system, b, held, place, id, mass);
            }
            void boneSpace(ee::Ptr<zge::AnimateSystem> system, ee::Ptr<zge::AnimatePoint> point, uint32_t name, ee::Float stiffness) const
            {
                const ee::Ptr<zge::SceneBone> b = bone(name);
                zge::newAnimateBoneSpace(system, point, b, stiffness);
            }
            void pivot(ee::Ptr<zge::AnimateSystem> system, uint32_t name, ee::Ptr<zge::AnimatePoint> from, ee::Ptr<zge::AnimatePoint> to) const
            {
                const ee::Ptr<zge::SceneBone> b = bone(name);
                zge::newBonePivot(system, b, from, to);
            }
            void constraint(ee::Ptr<zge::AnimateSystem> system, ee::Ptr<zge::AnimatePoint> point, ee::Ptr<zge::AnimatePoint> other, uint32_t name, ee::Float stiffness) const
            {
                const ee::Ptr<zge::SceneBone> b = bone(name);
                zge::newBoneConstraint(system, point, other, b, stiffness);
            }
            ee::Ptr<zge::AnimatePlane> plane(ee::Ptr<zge::AnimatePlaneGroup> group, uint32_t name, ee::Ptr<const zge::Vec4> place, ee::Ptr<const zge::Vec4> normal) const
            {
                const ee::Ptr<zge::SceneBone> b = bone(name);
                return zge::newAnimatePlane(group, b, place, normal);
            }
            // The bone controller the animator has for a bone.
            ee::Ptr<zge::AnimationChannel> channel(uint32_t name) const
            {
                const ee::Ptr<zge::Animator> animator = zge::sceneObjectAnimator(player->node);
                const int32_t hash = zge::nameHash(ee::Ptr<const char>(name));
                return zge::findBoneChannel(animator, hash);
            }
            ee::Ptr<zge::AnimateSystem> newSystem() const
            {
                return zge::newAnimateSystem(player->node);
            }
            void bonePointToWorld(uint32_t name, ee::Ptr<const zge::Vec4> place, ee::Ptr<zge::Vec4> out) const
            {
                const ee::Ptr<zge::SceneBone> b = bone(name);
                zge::objectBonePoint(player->node, b, place, out, 1);
            }
            // What is in `place`: a point at the first vector's offset from
            // the second bone's origin in the world.
            void boneToBone(uint32_t fromBone, uint32_t toBone, zge::Vec4 &place, zge::Vec4 &boneFrom, zge::Vec4 &boneTo) const
            {
                bonePointToWorld(fromBone, &place, &boneFrom);
                set(place, 0.0f, 0.0f, 0.0f);
                bonePointToWorld(toBone, &place, &boneTo);
                place = boneFrom - boneTo;
            }

            static void set(zge::Vec4 &vector, ee::Float x, ee::Float y, ee::Float z)
            {
                vector.x = x;
                vector.y = y;
                vector.z = z;
            }
            static void setUnit(zge::Vec4 &vector, ee::Float x, ee::Float y, ee::Float z)
            {
                set(vector, x, y, z);
                vector = zge::unit(vector);
            }
            // A point's pull down, as a multiple of its mass (y grows
            // downward): the last point of a chain hangs heavier.
            static void weighDown(ee::Ptr<zge::AnimatePoint> point, ee::Float factor)
            {
                point->force.y = point->mass * factor;
            }
            // The points this plane does not stop, up to three.
            static void letThrough(ee::Ptr<zge::AnimatePlane> plane, uint32_t count, ee::Ptr<zge::AnimatePoint> first = {}, ee::Ptr<zge::AnimatePoint> second = {}, ee::Ptr<zge::AnimatePoint> third = {})
            {
                plane->ignoredCount = count;
                if (count >= 1)
                    plane->ignored[0] = first;
                if (count >= 2)
                    plane->ignored[1] = second;
                if (count >= 3)
                    plane->ignored[2] = third;
            }

            void bobRig(RigVectors &v) const;
            void shirtOverBob(RigVectors &v) const;
            void shirtRig(RigVectors &v) const;
            void coatRig(RigVectors &v) const;
            void twoPonytails(RigVectors &v) const;
            void onePonytail(RigVectors &v) const;
            void margeHair(RigVectors &v) const;
        };

        // Each breast: a free point on springs to one held on the chest,
        // pulled to two more held on the chest by the BoobTech forces, the
        // breast's bone turned after it; two planes keep the points off the
        // body.
        void Rig::bobRig(RigVectors &v) const
        {
            const ee::Float damping = tuningValue(kTuningBoobTech, kDampingForce);
            const ee::Float displacement = tuningValue(kTuningBoobTech, kDisplacementForce);
            constexpr ee::Float kHeldMass{2.0f}, kFreeMass{1.0f}, kSide{0.09f}, kBack{-0.1f}, kFront{-0.8f};
            constexpr ee::Float kChestStiffness{4.0f}, kSpringStiffness{12.0f};
            zge::Vec4 &place = v.bodyPlace, &normal = v.bodyNormal;

            const ee::Ptr<zge::AnimateSystem> system = newSystem();
            player->bobAnimation = system;
            place = zge::kZero;
            set(place, kSide, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> leftHeld = point(system, kChest, 1, &place, 0, kHeldMass);
            set(place, -kSide, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> rightHeld = point(system, kChest, 1, &place, 1, kHeldMass);
            set(place, kSide, -0.0f, kBack);
            const ee::Ptr<zge::AnimatePoint> leftAnchor = point(system, kChest, 1, &place, 0, kHeldMass);
            set(place, -kSide, -0.0f, kBack);
            const ee::Ptr<zge::AnimatePoint> rightAnchor = point(system, kChest, 1, &place, 1, kHeldMass);
            set(place, kSide, -0.0f, kFront);
            boneToBone(kChest, kBreastLeft, place, v.boneFrom, v.boneTo);
            const ee::Ptr<zge::AnimatePoint> left = point(system, kBreastLeft, 0, &place, 0, kFreeMass);
            set(place, -kSide, -0.0f, kFront);
            boneToBone(kChest, kBreastRight, place, v.boneFrom, v.boneTo);
            const ee::Ptr<zge::AnimatePoint> right = point(system, kBreastRight, 0, &place, 1, kFreeMass);
            boneSpace(system, leftHeld, kChest, kChestStiffness);
            boneSpace(system, rightHeld, kChest, kChestStiffness);
            boneSpace(system, leftAnchor, kChest, kChestStiffness);
            boneSpace(system, rightAnchor, kChest, kChestStiffness);
            zge::newDistanceConstraint(system, leftHeld, left, kSpringStiffness);
            zge::newDistanceConstraint(system, rightHeld, right, kSpringStiffness);
            zge::newAnimatePull(system, left, leftAnchor, displacement, damping);
            zge::newAnimatePull(system, right, rightAnchor, displacement, damping);
            pivot(system, kBreastLeft, leftHeld, left);
            pivot(system, kBreastRight, rightHeld, right);
            zge::sortAnimatePivots(system);

            ee::Ptr<zge::AnimatePlaneGroup> group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, 0.0f, -0.6f);
            set(normal, 0.0f, 0.0f, -1.0f);
            plane(group, kChest, &place, &normal);
            group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, -0.2f, 0.0f);
            setUnit(normal, 0.0f, 1.0f, 0.5f);
            plane(group, kChest, &place, &normal);
            zge::resetAnimatePoints(system);

            player->bobBoneLeft = channel(kBreastLeft);
            player->bobBoneRight = channel(kBreastRight);
        }

        // The shirt over a bob: three held points and their free ones, springs
        // across them, and planes off the hips. The bob's place and normal
        // are still in place: place starts from what bobRig left in it.
        void Rig::shirtOverBob(RigVectors &v) const
        {
            constexpr ee::Float kMass{2.0f}, kHang{0.4f}, kWeight{10.0f};
            constexpr ee::Float kBoneStiffness{4.0f}, kSpringStiffness{12.0f}, kHipsStiffness{1.0f};
            zge::Vec4 &place = v.bodyPlace, &normal = v.bodyNormal;

            const ee::Ptr<zge::AnimateSystem> system = newSystem();
            player->bodyAnimation = system;
            place = zge::kZero;
            set(place, 0.0f, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> leftHeld = point(system, kShirtLeft, 1, &place, 0, kMass);
            set(place, 0.0f, kHang, -0.0f);
            boneToBone(kShirtLeft, kHips, place, v.boneFrom, v.boneTo);
            const ee::Ptr<zge::AnimatePoint> hipsLeft = point(system, kHips, 1, &place, 0, kMass);
            set(place, -0.0f, 0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> rightHeld = point(system, kShirtRight, 1, &place, 1, kMass);
            set(place, 0.0f, kHang, -0.0f);
            boneToBone(kShirtRight, kHips, place, v.boneFrom, v.boneTo);
            const ee::Ptr<zge::AnimatePoint> hipsRight = point(system, kHips, 1, &place, 1, kMass);
            set(place, -0.0f, 0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> lowerHeld = point(system, kShirtLower, 1, &place, 1, kMass);
            set(place, 0.0f, -0.4f, -0.0f);
            boneToBone(kShirtLower, kHips, place, v.boneFrom, v.boneTo);
            set(place, 0.0f, kHang, -0.0f);
            const ee::Ptr<zge::AnimatePoint> left = point(system, kShirtLeft, 0, &place, 0, kMass);
            set(place, -0.0f, kHang, -0.0f);
            const ee::Ptr<zge::AnimatePoint> right = point(system, kShirtRight, 0, &place, 1, kMass);
            set(place, -0.0f, kHang, -0.0f);
            const ee::Ptr<zge::AnimatePoint> lower = point(system, kShirtLower, 0, &place, 1, kMass);
            boneSpace(system, leftHeld, kShirtLeft, kBoneStiffness);
            boneSpace(system, rightHeld, kShirtRight, kBoneStiffness);
            boneSpace(system, lowerHeld, kShirtLower, kBoneStiffness);
            boneSpace(system, hipsLeft, kHips, kBoneStiffness);
            boneSpace(system, hipsRight, kHips, kBoneStiffness);
            zge::newDistanceConstraint(system, leftHeld, left, kSpringStiffness);
            zge::newDistanceConstraint(system, rightHeld, right, kSpringStiffness);
            zge::newDistanceConstraint(system, lowerHeld, lower, kSpringStiffness);
            zge::newDistanceConstraint(system, lower, left, kSpringStiffness);
            zge::newDistanceConstraint(system, lower, right, kSpringStiffness);
            zge::newDistanceConstraint(system, left, hipsLeft, kHipsStiffness);
            zge::newDistanceConstraint(system, right, hipsRight, kHipsStiffness);
            pivot(system, kShirtLeft, leftHeld, left);
            pivot(system, kShirtRight, rightHeld, right);
            pivot(system, kShirtLower, lowerHeld, lower);
            zge::sortAnimatePivots(system);
            weighDown(lower, kWeight);
            weighDown(left, kWeight);
            weighDown(right, kWeight);
            zge::resetAnimatePoints(system);

            ee::Ptr<zge::AnimatePlaneGroup> group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, 0.0f, 0.15f);
            setUnit(normal, 0.0f, 0.0f, 1.0f);
            letThrough(plane(group, kHips, &place, &normal), 2, left, right);
            set(place, 0.33f, 0.0f, 0.0f);
            setUnit(normal, 1.0f, 0.0f, 0.0f);
            letThrough(plane(group, kHips, &place, &normal), 2, lower, right);
            set(place, -0.33f, 0.0f, 0.0f);
            setUnit(normal, -1.0f, 0.0f, 0.0f);
            letThrough(plane(group, kHips, &place, &normal), 2, left, lower);
            group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, 0.0f, -0.03f);
            setUnit(normal, 0.0f, 0.0f, 1.0f);
            letThrough(plane(group, kHips, &place, &normal), 1, lower);
        }

        // A loose shirt: chains from the handles, held at the chest and hips.
        void Rig::shirtRig(RigVectors &v) const
        {
            constexpr ee::Float kHang{0.4f}, kMass{2.0f}, kBodyMass{8.0f}, kStiffness{4.0f};
            constexpr ee::Float kConstraint{0.2f}, kWaistStiffness{0.1f}, kWeight{10.0f};
            zge::Vec4 &place = v.clothPlace, &normal = v.clothNormal;

            const ee::Ptr<zge::AnimateSystem> system = newSystem();
            player->bodyAnimation = system;
            set(place, 0.0f, kHang, 0.0f);
            const ee::Ptr<zge::AnimatePoint> lower = point(system, kShirtLower, 0, &place, 0, kMass);
            set(place, 0.0f, 0.0f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> lowerRoot = point(system, kShirtLower, 0, &place, 0, kMass);
            weighDown(lower, kWeight);
            set(place, 0.0f, kHang, 0.0f);
            const ee::Ptr<zge::AnimatePoint> left = point(system, kShirtLeft, 0, &place, 1, kMass);
            set(place, 0.0f, kHang, 0.0f);
            const ee::Ptr<zge::AnimatePoint> right = point(system, kShirtRight, 0, &place, 2, kMass);
            set(place, 0.0f, 0.0f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> chest = point(system, kChest, 1, &place, 2, kBodyMass);
            set(place, 0.0f, 0.0f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> hips = point(system, kHips, 1, &place, 2, kBodyMass);
            ee::Ptr<zge::AnimatePoint> held = point(system, kShirtUpper, 1, &place, 0, kMass);
            boneSpace(system, held, kShirtUpper, kStiffness);
            zge::newDistanceConstraint(system, lowerRoot, held, kStiffness);
            pivot(system, kShirtUpper, held, lowerRoot);
            zge::newDistanceConstraint(system, lowerRoot, lower, kStiffness);
            pivot(system, kShirtLower, lowerRoot, lower);
            held = point(system, kShirtLeft, 1, &place, 0, kMass);
            boneSpace(system, held, kShirtLeft, kStiffness);
            zge::newDistanceConstraint(system, held, left, kStiffness);
            pivot(system, kShirtLeft, held, left);
            held = point(system, kShirtRight, 1, &place, 0, kMass);
            boneSpace(system, held, kShirtRight, kStiffness);
            pivot(system, kShirtRight, held, right);
            zge::newDistanceConstraint(system, held, right, kStiffness);
            zge::sortAnimatePivots(system);
            boneSpace(system, hips, kHips, kStiffness);
            boneSpace(system, chest, kChest, kStiffness);
            constraint(system, hips, left, kHips, kConstraint);
            constraint(system, hips, right, kHips, kConstraint);
            zge::newDistanceConstraint(system, hips, lower, kWaistStiffness);
            zge::newDistanceConstraint(system, lowerRoot, hips, kStiffness);
            zge::newDistanceConstraint(system, lower, left, kStiffness);
            zge::newDistanceConstraint(system, lower, right, kStiffness);
            if (player->bobAnimation != 0)
                return;

            // No bob under it: planes of its own.
            ee::Ptr<zge::AnimatePlaneGroup> group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, 0.0f, kWaistStiffness);
            setUnit(normal, 0.0f, 0.0f, 1.0f);
            letThrough(plane(group, kHips, &place, &normal), 3, left, right, lowerRoot);
            set(place, 0.25f, 0.0f, 0.0f);
            setUnit(normal, 1.0f, 0.0f, 0.6f);
            letThrough(plane(group, kHips, &place, &normal), 3, lower, right, lowerRoot);
            set(place, -0.25f, 0.0f, 0.0f);
            setUnit(normal, -1.0f, 0.0f, 0.6f);
            letThrough(plane(group, kHips, &place, &normal), 3, lower, left, lowerRoot);
            group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, -0.04f, 0.0f);
            setUnit(normal, 0.0f, 1.0f, -0.1f);
            letThrough(plane(group, kAbdomen, &place, &normal), 1, lowerRoot);
            group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, 0.0f, 0.01f);
            setUnit(normal, 0.0f, 0.0f, 1.0f);
            letThrough(plane(group, kChest, &place, &normal), 3, lower, left, right);
        }

        // A coat's two tails: each a chain of three, the last slowed.
        void Rig::coatRig(RigVectors &v) const
        {
            constexpr ee::Float kMass{2.0f}, kTailLength{0.17f}, kWeight{10.0f}, kStiffness{4.0f};
            zge::Vec4 &place = v.tailPlace, &normal = v.tailNormal;

            const ee::Ptr<zge::AnimateSystem> system = newSystem();
            player->bodyAnimation = system;
            set(place, 0.0f, 0.0f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> leftRoot = point(system, kCoatLeftUpper, 1, &place, 0, kMass);
            set(place, 0.0f, 0.0f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> leftMiddle = point(system, kCoatLeftLower, 0, &place, 0, kMass);
            set(place, 0.0f, kTailLength, 0.0f);
            const ee::Ptr<zge::AnimatePoint> leftTip = point(system, kCoatLeftLower, 0, &place, 0, kMass);
            weighDown(leftTip, kWeight);
            boneSpace(system, leftRoot, kCoatLeftUpper, kStiffness);
            zge::newDistanceConstraint(system, leftRoot, leftMiddle, kStiffness);
            zge::newDistanceConstraint(system, leftMiddle, leftTip, kStiffness);
            set(place, 0.0f, 0.0f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> rightRoot = point(system, kCoatRightUpper, 1, &place, 0, kMass);
            set(place, 0.0f, 0.0f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> rightMiddle = point(system, kCoatRightLower, 0, &place, 0, kMass);
            set(place, 0.0f, kTailLength, 0.0f);
            const ee::Ptr<zge::AnimatePoint> rightTip = point(system, kCoatRightLower, 0, &place, 0, kMass);
            weighDown(rightTip, kWeight);
            boneSpace(system, rightRoot, kCoatRightUpper, kStiffness);
            zge::newDistanceConstraint(system, rightRoot, rightMiddle, kStiffness);
            zge::newDistanceConstraint(system, rightMiddle, rightTip, kStiffness);
            pivot(system, kCoatLeftUpper, leftRoot, leftMiddle);
            pivot(system, kCoatLeftLower, leftMiddle, leftTip);
            pivot(system, kCoatRightUpper, rightRoot, rightMiddle);
            pivot(system, kCoatRightLower, rightMiddle, rightTip);
            const ee::Ptr<zge::AnimatePlaneGroup> group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, 0.0f, 0.11f);
            setUnit(normal, 0.0f, 0.0f, 1.0f);
            letThrough(plane(group, kHips, &place, &normal), 0);
        }

        // Two ponytails: each a held root and a heavy free tip; a plane
        // between them keeps each tip on its own side.
        void Rig::twoPonytails(RigVectors &v) const
        {
            constexpr ee::Float kMass{2.0f}, kLength{0.4f}, kWeight{10.0f}, kBoneStiffness{4.0f}, kSpringStiffness{12.0f};
            zge::Vec4 &place = v.clothPlace, &normal = v.clothNormal;

            const ee::Ptr<zge::AnimateSystem> system = newSystem();
            player->upDownAnimation = system;
            place = zge::kZero;
            set(place, 0.0f, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> leftRoot = point(system, kPonytailLeft1, 1, &place, 0, kMass);
            set(place, -0.0f, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> rightRoot = point(system, kPonytailRight1, 1, &place, 1, kMass);
            set(place, 0.0f, kLength, -0.0f);
            const ee::Ptr<zge::AnimatePoint> leftTip = point(system, kPonytailLeft1, 0, &place, 0, kMass);
            weighDown(leftTip, kWeight);
            set(place, -0.0f, kLength, -0.0f);
            const ee::Ptr<zge::AnimatePoint> rightTip = point(system, kPonytailRight1, 0, &place, 1, kMass);
            weighDown(rightTip, kWeight);
            boneSpace(system, leftRoot, kPonytailLeft2, kBoneStiffness);
            boneSpace(system, rightRoot, kPonytailRight2, kBoneStiffness);
            zge::newDistanceConstraint(system, leftRoot, leftTip, kSpringStiffness);
            zge::newDistanceConstraint(system, rightRoot, rightTip, kSpringStiffness);
            pivot(system, kPonytailLeft1, leftRoot, leftTip);
            pivot(system, kPonytailRight1, rightRoot, rightTip);
            zge::sortAnimatePivots(system);
            const ee::Ptr<zge::AnimatePlaneGroup> group = zge::addAnimatePlaneGroup(system);
            set(place, 0.0f, 0.0f, 0.0f);
            set(normal, 1.0f, 0.0f, 0.0f);
            letThrough(plane(group, kPonytailLeft2, &place, &normal), 1, rightTip);
            set(place, 0.0f, 0.0f, 0.0f);
            set(normal, -1.0f, 0.0f, 0.0f);
            letThrough(plane(group, kPonytailRight2, &place, &normal), 1, leftTip);
            zge::resetAnimatePoints(system);
            player->hairBone = 0;
        }

        // One ponytail: a held root and a heavy free tip.
        void Rig::onePonytail(RigVectors &v) const
        {
            constexpr ee::Float kMass{2.0f}, kLength{0.23f}, kWeight{10.0f}, kBoneStiffness{4.0f}, kSpringStiffness{12.0f};
            zge::Vec4 &place = v.tailPlace, &normal = v.tailNormal;

            const ee::Ptr<zge::AnimateSystem> system = newSystem();
            player->upDownAnimation = system;
            place = zge::kZero;
            set(place, 0.0f, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> root = point(system, kPonytailSingle, 1, &place, 0, kMass);
            set(place, 0.0f, kLength, 0.0f);
            const ee::Ptr<zge::AnimatePoint> tip = point(system, kPonytailSingle, 0, &place, 0, kMass);
            weighDown(tip, kWeight);
            boneSpace(system, root, kPonytailSingle, kBoneStiffness);
            zge::newDistanceConstraint(system, root, tip, kSpringStiffness);
            pivot(system, kPonytailSingle, root, tip);
            zge::sortAnimatePivots(system);
            const ee::Ptr<zge::AnimatePlaneGroup> group = zge::addAnimatePlaneGroup(system);
            set(normal, -0.0f, 0.0f, 1.0f);
            set(place, 0.0f, 0.0f, 0.0f);
            letThrough(plane(group, kPonytailSingle, &place, &normal), 0);
            zge::resetAnimatePoints(system);
            player->hairBone = channel(kPonytailSingle);
        }

        // A tall hairdo: the top's point and tip held by springs to points
        // on the head's bones, so it bobs but stays up.
        void Rig::margeHair(RigVectors &v) const
        {
            constexpr ee::Float kMass{2.0f}, kBoneStiffness{4.0f}, kSpringStiffness{12.0f};
            constexpr ee::Float kMiddleStrength{300.0f}, kMiddleDamping{0.03f}, kTipStrength{500.0f}, kTipDamping{20.0f};
            zge::Vec4 &place = v.hairPlace;

            const ee::Ptr<zge::AnimateSystem> system = newSystem();
            player->upDownAnimation = system;
            place = zge::kZero;
            set(place, 0.0f, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> base = point(system, kMargeBottom, 1, &place, 0, kMass);
            set(place, -0.0f, -0.0f, -0.0f);
            const ee::Ptr<zge::AnimatePoint> middle = point(system, kMargeTop, 0, &place, 1, kMass);
            const ee::Ptr<zge::AnimatePoint> middleHeld = point(system, kMargeTop, 1, &place, 1, kMass);
            set(place, 0.0f, -0.4f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> tip = point(system, kMargeTop, 0, &place, 0, kMass);
            set(place, 0.0f, -0.6f, 0.0f);
            const ee::Ptr<zge::AnimatePoint> tipHeld = point(system, kMargeTop, 1, &place, 1, kMass);
            boneSpace(system, base, kHead, kBoneStiffness);
            boneSpace(system, middleHeld, kHead, kBoneStiffness);
            boneSpace(system, tipHeld, kHead, kBoneStiffness);
            zge::newDistanceConstraint(system, middle, base, kSpringStiffness);
            zge::newDistanceConstraint(system, middle, tip, kSpringStiffness);
            zge::newAnimatePull(system, middleHeld, middle, kMiddleStrength, kMiddleDamping);
            zge::newAnimatePull(system, tipHeld, tip, kTipStrength, kTipDamping);
            pivot(system, kMargeBottom, base, middle);
            pivot(system, kMargeTop, middle, tip);
            zge::sortAnimatePivots(system);
            zge::resetAnimatePoints(system);
            player->hairBone = 0;
            player->hairBoneLower = channel(kMargeBottom);
        }
    }

    // setupSkater. A player with no skater (index 0: "None") has nothing.
    uint32_t setupSkater(ee::Ptr<Player> player)
    {
        ee::Local<zge::Quaternion> facing;
        ee::Local<zge::Vec4> halfSize, cornerBack;
        RigVectors vectors;
        if ((player->skaterIndex & 0x1Fu) == 0)
            return 1;
        const char *name = (*g_skaterModelNames)[player->skaterIndex & 0x1Fu];
        player->node = zge::newSceneModelByName(0, name);
        player->active = 1;
        zge::quaternionIdentity(&*facing);
        zge::setObjectOrientation(player->node, facing);
        zge::updatePose(player->node);
        player->flags |= kRespawnAtLocator;

        const Rig rig{player};

        const auto clearShirtBones = [&] {
            player->swayBoneLeft = 0;
            player->swayBoneRight = 0;
        };
        const auto noShirt = [&] {
            player->swayBoneCentre = 0;
            clearShirtBones();
        };
        const auto noBodyRig = [&] {
            player->swayBoneCentre = 0;
            player->swayBoneLeft = 0;
            player->swayBoneRight = 0;
            player->bodyAnimation = 0;
        };

        if (rig.channel(kBreastLeft) != 0)
        {
            rig.bobRig(vectors);
            player->swayBoneCentre = rig.channel(kShirtLower);
            player->swayBoneLeft = rig.channel(kShirtLeft);
            player->swayBoneRight = rig.channel(kShirtRight);
            if (player->swayBoneCentre == 0)
                noShirt();
            else if (player->swayBoneLeft == 0 || player->swayBoneRight == 0)
            {
                player->swayBoneCentre = 0;
                clearShirtBones();
            }
            else
                rig.shirtOverBob(vectors);
        }
        else
        {
            player->swayBoneCentre = rig.channel(kShirtLower);
            player->swayBoneLeft = rig.channel(kShirtLeft);
            player->swayBoneRight = rig.channel(kShirtRight);
            if (player->swayBoneCentre != 0 && player->swayBoneLeft != 0 && player->swayBoneRight != 0)
            {
                rig.shirtRig(vectors);
                zge::resetAnimatePoints(player->bodyAnimation);
            }
            else
            {
                if (rig.channel(kCoatLeftLower2) != 0 && rig.channel(kCoatLeftUpper) != 0 && rig.channel(kCoatRightUpper) != 0 &&
                    rig.channel(kCoatRightLower2) != 0)
                {
                    rig.coatRig(vectors);
                    noShirt();
                }
                else
                    noBodyRig();
            }
        }

        const auto noHairBones = [&] {  player->hairBoneLower = 0; };
        if (rig.channel(kPonytailLeft1) != 0 && rig.channel(kPonytailRight1) != 0)
        {
            rig.twoPonytails(vectors);
            noHairBones();
        }
        else
        {
            if (rig.channel(kPonytailSingle) != 0)
            {
                rig.onePonytail(vectors);
                noHairBones();
            }
            else
            {
                if (rig.channel(kMargeTop) != 0 && rig.channel(kMargeBottom) != 0)
                    rig.margeHair(vectors);
                else
                {
                    player->hairBone = 0;
                    noHairBones();
                }
            }
        }

        setupSkaterScore(player);
        player->flags |= kRespawnAtLocator;
        respawn(player, 1, 0);
        player->drawnPosition = player->position;
        Rig::set(*halfSize, 0.5f, 0.5f, 0.5f);
        zge::vec3Negate(&cornerBack->xyz(), &halfSize->xyz());
        const ee::Ptr<zge::Sweep> sweep = levelSweep();
        player->sweepBox = zge::sweepAddBody(sweep, 4, player, &cornerBack->x, &halfSize->x);
        if (*g_levelWord == kTutorialLevel)
        {
            aggressive::fx::makeSkaterReflection(player);
            aggressive::fx::reflectSkater(player);
        }
        levelUpEffect(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupSkater, Verified);
}
