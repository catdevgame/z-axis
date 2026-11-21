#pragma once

// DM2's rider physics (physics.c, physicsInit .. hitDistance): the physics frame, the ground and the
// air, landing, the bike's pose and the respawn. The engine calls they make,
// with Aggressive Inline's counterparts where the rider map has one.
//
// Matrices are the engine's 4x4 (rows right, up, forward, then the
// position; ee::Float[16]); angles are 16-bit (0x10000 a turn).

#include "../collision/collision.h"
#include "../rider.h"

#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "zge/scene/locators.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/dm2/rider/physics/physics.h.h"

namespace zge
{
    struct Dynamics;
    struct RigidBody;
    struct FaceContact;
}

namespace game::dm2::rider::physics
{
    // The contact callbacks newRider gives the rider model's dynamics (the
    // bail's bodies) and the bike model's (body_contacts.cpp): a hard touch's
    // sound and dust, then the level's own contact handling's answer.
    uint32_t riderDynamicsContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact);
    uint32_t bikeDynamicsContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact);

    // The engine's trigonometry and vector routines (DM2's; AI's in
    // brackets).
    constexpr uint32_t kSin = sym::sinTurnOf;            // (angle) -> f0 (a table)
    constexpr uint32_t kAsin = sym::asinTurn;           // (f12) -> the angle (a table)
    constexpr uint32_t kCos = sym::cosTurnOf;            // (angle) -> f0 (a table)
    constexpr uint32_t kAcos = sym::angleAcos;           // (f12) -> the angle (AI's angleAcos)
    constexpr uint32_t kLengthSquared = sym::lengthSquared;  // (v*) -> |v.xyz|^2
    constexpr uint32_t kNegate = sym::vec3Negate;         // (out*, v*) (AI's vec3Negate)
    constexpr uint32_t kLength = sym::vec3Length;         // (v*) -> |v.xyz| (AI's vec3Length)
    constexpr uint32_t kMatrixCopy = sym::copyRotationAndTranslation;     // (out*, matrix*): rows and position
    constexpr uint32_t kMatrixProduct = sym::matrixProduct;  // (out*, a*, b*): out = a b (rows and position)
    constexpr uint32_t kRandom = sym::randomNumber;         // (AI's randomNumber)
    constexpr uint32_t kQuatFromMatrix = sym::matrixToQuaternion; // (quaternion*, matrix*) (AI's matrixToQuaternion)

    // Engine objects (models) and bones.
    constexpr uint32_t kSceneOf = sym::sceneOf;        // (object) -> its scene (AI's sceneOf)
    constexpr uint32_t kSetPosition = sym::setPosition;    // (object, position*)
    constexpr uint32_t kSetRotation = sym::setObjectRotationMatrix;    // (object, matrix*): the matrix's rotation as the object's (AI's setObjectRotationMatrix)
    constexpr uint32_t kSetLit = sym::setObjectCastsShadow;         // (object, on) (inferred; AI's setObjectCastsShadow)
    constexpr uint32_t kSceneSetTint = sym::setSceneRootColour;   // (scene, rgba*) (inferred; AI's setSceneRootColour)
    constexpr uint32_t kBoneSetRotation = sym::setBoneRotation; // (bone, quaternion*) (AI's setBoneRotation)

    // The surfaces by kind (a wheel's material sound, an edge's wall kind,
    // 0 .. 15): how a wheel grips, slips and slows rolling on it, and how much
    // a grind along it drags.
    struct Surface
    {
        ee::Float grip;
        ee::Float slip;
        ee::Float rollingLoss; // a rolling wheel's speed lost
        ee::Float grindDrag;   // the share of a grind's velocity lost a frame at full balance, 11 times it at none (grindSlope)
    };
    static_assert(sizeof(Surface) == 0x10 && offsetof(Surface, grindDrag) == 0xC);
    inline ee::Global<Surface[16], sym::surfaces> g_surfaces;

    // A scene point as DM2's engine (ZGE2) keeps it: findLocators matches its
    // kind and name; its position and rotation follow (AI's ZGE3 keeps them
    // first, zge::ScenePoint).
    struct ScenePoint
    {
        uint32_t kindHash;      // +0x00
        uint32_t nameHash;      // +0x04
        uint8_t _08[0x20 - 0x08];
        ee::Float position[4];  // +0x20
        zge::Quaternion rotation; // +0x30
    };
    static_assert(offsetof(ScenePoint, position) == 0x20 && offsetof(ScenePoint, rotation) == 0x30);
    // The scene point a locator of findLocators' list holds (zge::Locator).
    inline ScenePoint &locatorPoint(const zge::Locator &locator) { return *ee::Ptr<ScenePoint>(locator.point); }

    // ---- DM2's engine (ZGE2) as the physics calls it: DM2's own code (a
    // function AI has under the same name is not the same code there, or AI
    // has none), each called where it is, through its typed declaration.
    namespace engine
    {
        // out = the point through the matrix: its rotation, then its
        // translation (ZGE2's transformPoint; ZGE3 and ZGE4 inline it).
        inline void transformPoint(ee::Float *out, ee::Address matrix, const ee::Float *point)
        {
            ee::game(sym::dm2::transformPoint, out, matrix, point);
        }
        // out = a b (rows and position; matrices).
        inline void matrixProduct(ee::Address out, ee::Address a, ee::Address b) { ee::game(sym::matrixProduct, out, a, b); }
        // A whole matrix copied: rows and position.
        inline void copyMatrix(ee::Address out, ee::Address matrix) { ee::game(sym::copyRotationAndTranslation, out, matrix); }
        // The sine and cosine of a turn (0x10000 a turn), from the engine's table.
        inline ee::Float sinTurn(int32_t angle) { return ee::game<ee::Float>(sym::sinTurnOf, angle); }
        inline ee::Float cosTurn(int32_t angle) { return ee::game<ee::Float>(sym::cosTurnOf, angle); }
        // The turn whose sine is the value (held to -1 .. 1), from the table:
        // a 16-bit turn, sign-extended (zge::angleAcos is the cosine's).
        inline int32_t asinTurn(ee::Float sine) { return ee::game<int32_t>(sym::asinTurn, sine); }

        // An engine object's position.
        inline void setObjectPosition(uint32_t object, const ee::Float *position)
        {
            ee::game(sym::setPosition, object, position);
        }
    }

    // SQC2 vf0: the point (0, 0, 0, 1).
    inline void setOrigin(ee::Float *v)
    {
        v[0] = 0.0f;
        v[1] = 0.0f;
        v[2] = 0.0f;
        v[3] = 1.0f;
    }

    // The rider record's flags the physics reads (Rider::flags).
    constexpr uint32_t kFlagAlignAbout = 0x4000u; // airRotate: the alignment turns the rows by its angles directly (inferred)
}

namespace game::dm2::rider
{
    // Rounds 1 and 2's functions the physics calls (physics_state.cpp,
    // contact.cpp, rider_state.cpp).
    uint8_t physicsState(const Rider *rider);
    uint8_t setPhysicsState(Rider *rider, uint32_t state);
    Bounds *levelBounds();
    int32_t groundBelow(const ee::Float *point, GroundHit *hit);
    ee::Float planeDistance(const ee::Float *point, const ee::Float *plane);
    void setContact(Rider *rider, const void *contact, uint32_t kind);
    void setWheelContact(Rider *rider, uint32_t wheel, const void *contact, uint32_t kind, const ee::Float *point);
    int32_t surfaceSoundFlags(uint32_t flag20000, uint32_t flag10000, uint32_t kind, uint32_t surface);
    ee::Ptr<Rider> riderOf(uint32_t index);
    uint8_t actionState(const Rider *rider, uint32_t level);
    uint8_t setActionState(Rider *rider, uint32_t level, uint32_t state);

    // A game sound at a position (the sound events' soundEvent): the sound,
    // its code and volume (a fourth word, 0, the rider's calls all pass).
    inline void soundEvent(int32_t sound, const ee::Float *position, int32_t code, int32_t volume)
    {
        game::dm2::soundEvent(static_cast<uint32_t>(sound), position, static_cast<uint32_t>(code),
                              static_cast<uint32_t>(volume), 0);
    }
}

namespace game::dm2
{
    // A player's pad rumbles (input/pads.cpp): a grind's (motor 0, on), an
    // impact on metal's (motor 1, its strength).
    void rumble(uint32_t player, uint32_t motor, uint32_t level);
    // The game's effects at a point with a motion (fx/spawns.cpp): the "dust"
    // effect, and so many sparks.
    void dust(const ee::Float *position, const ee::Float *motion);
    void sparks(const ee::Float *position, const ee::Float *motion, int32_t count);
}

namespace game::dm2::rider::physics
{
    // Quaternions and an object's parts (DM2's engine; AI's in brackets).
    constexpr uint32_t kPartPoint = sym::objectBonePoint;    // (object, part, point*, out*, 0): a point in the part's space into the world (AI's objectBonePoint)
}

namespace game::dm2::rider::physics
{
    // (air.cpp, frame.cpp, ground.cpp, impact.cpp, landing.cpp, respawn.cpp, trick_pose.cpp)
    void airRotationTrack(const ee::Float *current, const ee::Float *previous, int32_t frames, int16_t *angles);
    collision::RayHit *groundAhead(Rider *rider, uint32_t wheelIndex, ee::Float *point, ee::Float *height, ee::Float *clearance);
    int32_t groundHit(Rider *rider, const collision::RayHit *hit);
    int32_t groundImpact(Rider *rider, uint32_t wheelIndex, uint32_t hit);
    int32_t groundProbe(Rider *rider, uint32_t wheelIndex, const ee::Float *from);
    int32_t landingTest(Rider *rider, const collision::RayHit *hit);
    void lipTest(Rider *rider, collision::EdgeRef edge, int32_t anyAngle);
    void physicsFrame(Rider *rider);
    void poseRiderBones(Rider *rider);
    void resetPhysics(Rider *rider);
    collision::FaceRef spawnAtLocator(const ee::Float *point, uint32_t flags);
    int32_t groundBelowSpawn(const ee::Float *point, GroundHit *hit);
    void rideTouchedObject(Rider *rider, ee::Float *moved);
}
