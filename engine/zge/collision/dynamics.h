#pragma once

// The engine's rigid body dynamics (block boxClosestPoint; zge/collision/dynamics.cpp
// and dynamics_solver.cpp): boxes that collide as boxes (separating axes),
// bodies against the world's faces, and the constraint solver of a dynamics
// object's tree of bodies and links. aggressive/level/dynamics.h has the same
// body as the game sees it.

#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/collision/sweep.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/dynamics.h.h"

namespace zge
{
    // A rigid body (0x3E0 bytes). Fields marked [inferred] are named from
    // how the contact code uses them only.
    struct RigidBody
    {
        uint32_t shape;              // +0x000 0: a box
        uint32_t nameHash;           // +0x004
        ee::Float mass;              // +0x008
        uint32_t _00c;               // +0x00C
        Matrix inertia;              // +0x010
        Matrix inverseInertia;       // +0x050
        Matrix worldInertia;         // +0x090 the solver's mass matrix takes its rows
        Vec4 boxSize;                // +0x0D0
        Vec4 position;               // +0x0E0
        Vec4 velocity;               // +0x0F0
        Quaternion orientation;      // +0x100
        Quaternion rest;             // +0x110
        Matrix worldInverseInertia;  // +0x120
        Matrix rotation;             // +0x160 its rows are the box's axes
        Vec4 spin;                   // +0x1A0
#if !ZX_GAME_BMX
        // What the step starts from (saveBodyState keeps it, restoreBodyState puts it back; a contact's push moves
        // pushedPosition with position). ZGE4 keeps none of it: BMX XXX's record is 0x2F0 bytes, and these stand past
        // its end (below), for the steps that compile away there.
        Vec4 pushedPosition;         // +0x1B0 [inferred] moved with position when contacts push it out
        Vec4 lastPosition;           // +0x1C0
        Vec4 lastVelocity;           // +0x1D0
        Vec4 lastSpin;               // +0x1E0
        Quaternion lastOrientation;  // +0x1F0
        uint8_t _200[0x20];          // +0x200
        Matrix lastInverseInertia;   // +0x220
        Vec4 lastForce;              // +0x260
        Vec4 lastTorque;             // +0x270
        uint8_t _280[0x20];          // +0x280
#endif
        Vec4 force;                  // +0x2A0 (BMX +0x1B0) this step's
        Vec4 solvedForce;            // +0x2B0 (BMX +0x1C0) [inferred] what the solver adds
        Vec4 torque;                 // +0x2C0 (BMX +0x1D0)
        Vec4 solvedTorque;           // +0x2D0 (BMX +0x1E0) [inferred]
        Vec4 springSpin;             // +0x2E0 (BMX +0x1F0) the spin the dead springBodyToBoneTurn's pushes add up; cleared before each solve, the dead addToSolvedForces adds it to solvedTorque
#if !ZX_GAME_BMX
        // (ZGE4 reads the file's spheres and keeps none: past the end in BMX XXX, as the last state.)
        uint32_t sphereCount;        // +0x2F0 the spheres the file gives it
        ee::Ptr<struct BodySphere> spheres; // +0x2F4 only the free and the dead drawDynamicsBody read them
#endif
        uint32_t contactCount;       // +0x2F8 (BMX +0x200) its faces touched last step: 1, else 0
        ee::Ptr<struct SweepBox> sweepEntry; // +0x2FC (BMX +0x204)
        uint8_t _300[game::readable::perGame(0x30, 0x30, 0x28)]; // +0x300 (BMX +0x208, 8 bytes fewer)
        Vec4 contactPush;            // +0x330 (BMX +0x230) [inferred] what the faces push it out by
        uint8_t _340[0x20];          // +0x340 (BMX +0x240)
#if ZX_GAME_DM2
        uint32_t ignoredCount;       // DM2 +0x360 bodies it does not collide with (pairStillTouching)
        ee::Ptr<RigidBody> ignored[10]; // DM2 +0x364
        // Whether `other` is on its ignore list.
        bool ignores(ee::Ptr<RigidBody> other) const
        {
            for (uint32_t i = 0; i < ignoredCount; ++i)
                if (ignored[i] == other)
                    return true;
            return false;
        }
        // `other` put on its ignore list (ZGE2's joint readers: a joint's two bodies ignore each other). As the
        // original: no room check, and a null body writes its list at address 0x360.
        void addIgnored(ee::Ptr<RigidBody> other) { ignored[ignoredCount] = other; ignoredCount = ignoredCount + 1; }
        // Its ignore list emptied (DM2's newRider, for the bail body it makes).
        void clearIgnored() { ignoredCount = 0; }
#else
        // (AI's and BMX XXX's bodies keep no ignore list: pairStillTouching's DM2 step asks.)
        bool ignores(ee::Ptr<RigidBody>) const { return false; }
        void addIgnored(ee::Ptr<RigidBody>) {}
        void clearIgnored() {}
#endif
        ee::Ptr<struct SceneBone> bone; // +0x360 (DM2 +0x38C, BMX +0x260) the object's model bone it follows
        ee::Ptr<struct AnimationChannel> boneChannel; // +0x364 (DM2 +0x390, BMX +0x264) the animator's channel for that bone, 0: none
#if ZX_GAME_DM2
        uint32_t _394;               // DM2 +0x394
#endif
        uint8_t _368[0x08];          // +0x368 (DM2 +0x398)
        Vec4 bindPosition;           // +0x370 (DM2 +0x3A0)
        uint32_t boneDriven;         // +0x380 (DM2 +0x3B0)
        ee::Ptr<RigidBody> parentBody; // +0x384 (DM2 +0x3B4) its bone's nearest parent with a body
        ee::Ptr<struct BodyContact> contacts; // +0x388 (DM2 +0x3B8) what holds it (see BodyContact)
        uint32_t linkKind3Mark;      // +0x38C (DM2 +0x3BC) set to 1 by a kind 3 link (the dead readDynamicsHinge); nothing reads it
        ee::Float boneSpring;        // +0x390 (DM2 +0x3C0) [inferred] the dead springBodyToBoneTurn's pull toward the bone's turn (driveBodyFromBone sets it)
        ee::Float boneDamping;       // +0x394 (DM2 +0x3C4) [inferred] the same's damping of the bodies' velocities (driveBodyFromBone sets it)
        uint32_t followsParentTurn;  // +0x398 (DM2 +0x3C8) 1: integrateBodyRotation turns it with its parent each step (set only by driveBodyFromBone, which nothing calls)
        uint8_t boneTurn[0x10];      // +0x39C (DM2 +0x3CC) a quaternion: its turn on the parent's
        uint32_t solvedTurn[4];      // +0x3AC (DM2 +0x3DC) a quaternion: integrateBodyRotation's turn, cleared before each solve
        uint32_t _3bc;               // +0x3BC (DM2 +0x3EC)
        Vec4 springImpulse;          // +0x3C0 (DM2 +0x3F0) the impulses of the dead springBodyToBoneTurn; cleared before each solve, the dead addToSolvedForces adds it to solvedForce
        ee::Float turnShare;         // +0x3D0 (DM2 +0x400) how far it follows the solved turn
        ee::Ptr<struct BodyExtents> halfExtents; // +0x3D4 (DM2 +0x404)
        ee::Ptr<RigidBody> next;     // +0x3D8 (DM2 +0x408) the next body of its dynamics
#if !ZX_GAME_BMX
        uint32_t record;             // +0x3DC (DM2 +0x40C) the game's
#else
        // BMX +0x2DC (the fields from bone on are AI's less 0x100 in BMX XXX). ZGE4's own: 1, a face touches the body from
        // either side (findBodyFaceContacts turns its normal to the body; boxClosestPoint takes the corners past the
        // plane, the nearest when none is, not those on it). The game sets it, through ZGE4's setter.
        uint32_t twoSidedFaces;
        uint32_t record; uint8_t _2e4[0x0C]; // BMX +0x2E0 the game's (BMX XXX's createDynamicsObject sets it); +0x2E4 [no function read so far]
        // AI's last state and spheres (above), past BMX XXX's 0x2F0-byte record.
        Vec4 pushedPosition;
        Vec4 lastPosition;
        Vec4 lastVelocity;
        Vec4 lastSpin;
        Quaternion lastOrientation;
        Matrix lastInverseInertia;
        Vec4 lastForce;
        Vec4 lastTorque;
        uint32_t sphereCount;
        ee::Ptr<struct BodySphere> spheres;
#endif

        // Whether faces touch it from either side (ZGE4 only).
#if ZX_GAME_BMX
        bool hasTwoSidedFaces() const { return twoSidedFaces != 0; }
        void setTwoSidedFaces(uint32_t twoSided) { twoSidedFaces = twoSided; }
#else
        bool hasTwoSidedFaces() const { return false; }
        void setTwoSidedFaces(uint32_t) {}
#endif

        // boneTurn and solvedTurn as the quaternion functions take them (they are not 16-byte aligned, as a
        // Quaternion is in C++).
        ee::Ptr<Quaternion> boneTurnQuaternion() { return ee::Ptr<uint8_t>(boneTurn).cast<Quaternion>(); }
        ee::Ptr<Quaternion> solvedTurnQuaternion() { return ee::Ptr<uint32_t>(solvedTurn).cast<Quaternion>(); }
    };
    // A body's record as the game allocates it (BMX XXX's sizeof has AI's fields past the record).
    constexpr uint32_t kRigidBodyBytes = game::readable::perGame(0x3E0u, 0x410u, 0x2F0u);
    static_assert(game::readable::kIsBmx || sizeof(RigidBody) == kRigidBodyBytes);
    static_assert(offsetof(RigidBody, sweepEntry) == game::readable::perGame(0x2FCu, 0x2FCu, 0x204u) && offsetof(RigidBody, springSpin) == game::readable::perGame(0x2E0u, 0x2E0u, 0x1F0u) &&
                  offsetof(RigidBody, bone) == game::readable::perGame(0x360u, 0x38Cu, 0x260u) && offsetof(RigidBody, parentBody) == game::readable::perGame(0x384u, 0x3B4u, 0x284u) &&
                  offsetof(RigidBody, contacts) == game::readable::perGame(0x388u, 0x3B8u, 0x288u) &&
                  offsetof(RigidBody, followsParentTurn) == game::readable::perGame(0x398u, 0x3C8u, 0x298u) &&
                  offsetof(RigidBody, solvedTurn) == game::readable::perGame(0x3ACu, 0x3DCu, 0x2ACu) && offsetof(RigidBody, springImpulse) == game::readable::perGame(0x3C0u, 0x3F0u, 0x2C0u) &&
                  offsetof(RigidBody, boneSpring) == game::readable::perGame(0x390u, 0x3C0u, 0x290u) && offsetof(RigidBody, boneDamping) == game::readable::perGame(0x394u, 0x3C4u, 0x294u));
    static_assert(offsetof(RigidBody, mass) == 0x008 && offsetof(RigidBody, position) == 0x0E0 &&
                  offsetof(RigidBody, velocity) == 0x0F0 && offsetof(RigidBody, rotation) == 0x160 &&
                  offsetof(RigidBody, worldInverseInertia) == 0x120 && offsetof(RigidBody, spin) == 0x1A0 &&
                  (game::readable::kIsBmx || offsetof(RigidBody, pushedPosition) == 0x1B0) && offsetof(RigidBody, contactCount) == game::readable::perGame(0x2F8u, 0x2F8u, 0x200u) &&
                  offsetof(RigidBody, contactPush) == game::readable::perGame(0x330u, 0x330u, 0x230u) && offsetof(RigidBody, solvedForce) == game::readable::perGame(0x2B0u, 0x2B0u, 0x1C0u) &&
                  offsetof(RigidBody, solvedTorque) == game::readable::perGame(0x2D0u, 0x2D0u, 0x1E0u) && offsetof(RigidBody, turnShare) == game::readable::perGame(0x3D0u, 0x400u, 0x2D0u) &&
                  offsetof(RigidBody, halfExtents) == game::readable::perGame(0x3D4u, 0x404u, 0x2D4u) && offsetof(RigidBody, next) == game::readable::perGame(0x3D8u, 0x408u, 0x2D8u));
#if ZX_GAME_BMX
    static_assert(offsetof(RigidBody, twoSidedFaces) == 0x2DC);
#else
    static_assert(offsetof(RigidBody, record) == game::readable::perGame(0x3DCu, 0x40Cu, 0x2DCu));
#endif

    // A sphere of a body's chunk (0x30 bytes; DM2's 0x40, its flag first: ZGE2 puts a record's words before its
    // vectors, ZGE3 after them, here and in BodyExtents and DynamicsForce).
#if ZX_GAME_DM2
    struct BodySphere
    {
        uint32_t aroundOrigin; // DM2 +0x00 1: the body's origin is inside it
        uint8_t _04[0x1C];     // DM2 +0x04
        Vec4 centre;           // DM2 +0x20 w unset
        ee::Float radius;      // DM2 +0x30
        uint8_t _34[0x0C];     // DM2 +0x34
    };
#else
    struct BodySphere
    {
        uint8_t _00[0x10];     // +0x00
        Vec4 centre;           // +0x10 w unset
        uint32_t aroundOrigin; // +0x20 1: the body's origin is inside it
        ee::Float radius;      // +0x24
        uint8_t _28[0x08];     // +0x28
    };
#endif
    static_assert(offsetof(BodySphere, aroundOrigin) == game::readable::perGame(0x20u, 0x00u, 0x20u) &&
                  offsetof(BodySphere, centre) == game::readable::perGame(0x10u, 0x20u, 0x10u) &&
                  offsetof(BodySphere, radius) == game::readable::perGame(0x24u, 0x30u, 0x24u) &&
                  sizeof(BodySphere) == game::readable::perGame(0x30u, 0x40u, 0x30u));

    // The other record of a box body: half extents and their length (0x20
    // bytes, RigidBody::halfExtents; DM2's length first).
#if ZX_GAME_DM2
    struct BodyExtents
    {
        ee::Float length; // DM2 +0x00
        uint32_t _04[3];  // DM2 +0x04
        Vec4 half;        // DM2 +0x10 w unset
    };
#else
    struct BodyExtents
    {
        Vec4 half;       // +0x00 w unset
        ee::Float length; // +0x10
        uint32_t _14[3]; // +0x14
    };
#endif
    static_assert(sizeof(BodyExtents) == 0x20 && offsetof(BodyExtents, half) == game::readable::perGame(0x00u, 0x10u, 0x00u) &&
                  offsetof(BodyExtents, length) == game::readable::perGame(0x10u, 0x00u, 0x10u));

    // A point of a body held at a place (0x14 bytes, on the body's list), made by newBodyContactRecord: AI calls it
    // nowhere; DM2's newRider holds the bail body's hands with it; ZGE4's game makes them.
    enum BodyContactKind : uint32_t
    {
        kBodyContactHold = 5,
        kBodyContactSlackHold = 6, // ZGE4: a hold that pulls only past the detail's slack (resolveBodyContactList's unnamed callee)
    };
    struct BodyContactDetail
    {
        ee::Ptr<RigidBody> body; // +0x00
        ee::Float slack;         // +0x04 ZGE4: how far the point may stray from its target before the hold pulls (newBodyContactRecord's last argument; AI and ZGE2 leave it 0)
        uint8_t _08[0x08];       // +0x08
        Vec4 target;             // +0x10 where the point is held
        Vec4 anchor;             // +0x20 the point, in the body's frame
    };
    static_assert(offsetof(BodyContactDetail, anchor) == 0x20 && sizeof(BodyContactDetail) == 0x30);
    struct BodyContact
    {
        uint32_t kind;                    // +0x00
        ee::Ptr<RigidBody> other;         // +0x04
        ee::Float strength;               // +0x08 1 on, 0 off
        ee::Ptr<BodyContactDetail> detail; // +0x0C
        ee::Ptr<BodyContact> next;        // +0x10
    };
    static_assert(offsetof(BodyContact, next) == 0x10 && sizeof(BodyContact) == 0x14);

    // A matrix of the solver, up to 6 by 6: rows and columns, then its
    // floats, rows of six whatever its size.
    struct SolverMatrix
    {
        uint32_t rows;        // +0x00
        uint32_t columns;     // +0x04
        ee::Float data[6][6]; // +0x08
    };
    static_assert(offsetof(SolverMatrix, data) == 0x08 && sizeof(SolverMatrix) == 0x98);

    // A joint of two bodies (the DCON chunk's; kind 0 has the first 0x30
    // bytes, the kinds the game's files don't use have more).
    struct BodyConstraint
    {
        ee::Ptr<RigidBody> bodyA;  // +0x00
        ee::Ptr<RigidBody> bodyB;  // +0x04
        ee::Float anchorLengthA;   // +0x08
        ee::Float anchorLengthB;   // +0x0C
        Vec4 anchorA;              // +0x10 in bodyA's frame
        Vec4 anchorB;              // +0x20 in bodyB's frame
        Vec4 axis;                 // +0x30
    };
    static_assert(offsetof(BodyConstraint, anchorB) == 0x20 && sizeof(BodyConstraint) == 0x40);

    // The records of DCON's kinds 3 and 4 (readDynamicsHinge,
    // readDynamicsBodyPair). The game's files have neither kind, and nothing
    // reads these records once they are made.
    struct BodyHinge
    {
        uint32_t fileWord;       // +0x00 the record's second word in the file; nothing reads it
        uint32_t _04;            // +0x04
        ee::Ptr<RigidBody> body; // +0x08 the body named, 0 if none has the name
        uint8_t _0c[0x24];       // +0x0C
    };
    static_assert(offsetof(BodyHinge, body) == 0x08 && sizeof(BodyHinge) == 0x30);
    struct BodyPair
    {
        ee::Ptr<RigidBody> first;  // +0x00
        ee::Ptr<RigidBody> second; // +0x04
    };
    static_assert(sizeof(BodyPair) == 0x08);

    // A link of the solver's tree. Only kind 0 has rows (addContactRows).
    struct DynamicsLink
    {
        uint32_t kind;                     // +0x00
        uint32_t rigidBody;                // +0x04 a word of the file; nothing reads it
        ee::Float fileLength;              // +0x08 the file's, in tenths of the dynamics' unit; nothing reads it
        ee::Ptr<BodyConstraint> constraint; // +0x0C
        ee::Ptr<DynamicsLink> next;        // +0x10
    };
    static_assert(offsetof(DynamicsLink, constraint) == 0x0C && offsetof(DynamicsLink, next) == 0x10 && sizeof(DynamicsLink) == 0x14);

    // A body's or a link's place in the solver's tree (0x3B0 bytes).
    enum SolverNodeKind : uint32_t
    {
        kSolverNodeBody = 0,
        kSolverNodeFixed = 2,
    };
    // One of a node's matrices.
    using SolverNodeMatrix = SolverMatrix;
    struct SolverNode
    {
        uint32_t kind;                   // +0x000
        uint32_t index;                  // +0x004
        SolverNodeMatrix mass;           // +0x008 the 6 by 6 mass matrix, then the articulated mass
        SolverNodeMatrix inverted;       // +0x0A0 its inverse
        SolverNodeMatrix linkMatrix;     // +0x138 what takes its motion to the joint's
        SolverNodeMatrix linkTransposed; // +0x1D0
        SolverNodeMatrix solved;         // +0x268 what it comes to: six rows of one column
        SolverNodeMatrix force;          // +0x300
        ee::Ptr<SolverNode> parent;      // +0x398
        ee::Ptr<SolverNode> firstChild;  // +0x39C
        ee::Ptr<SolverNode> nextSibling; // +0x3A0
        uint32_t childCount;             // +0x3A4
        uint32_t isLink;                 // +0x3A8
        union
        {
            ee::Ptr<RigidBody> body;     // +0x3AC a body's node
            ee::Ptr<DynamicsLink> link;  // a link's node
        };
    };
    static_assert(offsetof(SolverNode, inverted) == 0xA0 && offsetof(SolverNode, linkMatrix) == 0x138 && offsetof(SolverNode, linkTransposed) == 0x1D0 &&
                  offsetof(SolverNode, solved) == 0x268 && offsetof(SolverNode, force) == 0x300 && offsetof(SolverNode, firstChild) == 0x39C &&
                  offsetof(SolverNode, body) == 0x3AC && sizeof(SolverNode) == 0x3B0);

    // A dynamics object's tree of nodes in the orders the solver walks.
    struct ConstraintSystem
    {
        ee::Ptr<uint8_t> bodyBlocks; // +0x00 0xEC0 bytes per body
        ee::Ptr<SolverNode> nodes; // +0x04 bodies, then links
        ee::Ptr<SolverNode> root;  // +0x08
        ee::Ptr<ee::Ptr<SolverNode>> reversedOrder; // +0x0C leaves first
        ee::Ptr<ee::Ptr<SolverNode>> order;         // +0x10 from the root
        uint32_t bodyCount;        // +0x14
        uint32_t linkCount;        // +0x18
    };
    static_assert(offsetof(ConstraintSystem, order) == 0x10 && offsetof(ConstraintSystem, linkCount) == 0x18);

    // What the dynamics' contact callback is shown of a face.
    struct FaceContact
    {
        ee::Ptr<HitNode> object; // +0x00 0 when it is told that the search is over
        uint32_t face;           // +0x04
        uint32_t _08[2];         // +0x08
        Vec4 normal;             // +0x10
        Vec4 vertices[3];        // +0x20
    };
    static_assert(sizeof(FaceContact) == 0x50);
    struct Dynamics;
    using ContactCallback = ee::FunctionPtr<int32_t(ee::Ptr<Dynamics>, ee::Ptr<RigidBody>, ee::Ptr<FaceContact>)>;

    // The dynamics of one object: a definition from the file, or its copy.
    struct Dynamics
    {
        uint32_t flags;            // +0x00 wakeDynamics' last argument: bit 0, the object follows its first body
        uint32_t nameHash;         // +0x04
#if ZX_GAME_DM2
        union
        {
            ee::Float restitution; // DM2 +0x08
            ee::Float contactDamping;
        };
#else
        ee::Float restitution;     // +0x08 how much of a contact's speed a body keeps
        ee::Float contactDamping;  // +0x0C how much of the speed along a face it loses
#endif
        ee::Ptr<RigidBody> bodies; // +0x10 (DM2 +0x0C)
        ee::Ptr<DynamicsLink> links; // +0x14 (DM2 +0x10) with any it is solved; alone, each body steps by itself
        ConstraintSystem system;   // +0x18 (DM2 +0x14)
        ContactCallback contactCallback; // +0x34 (DM2 +0x30) asked about every face a body touches: 0 to ignore it
#if !ZX_GAME_DM2
        uint32_t owner;            // +0x38 (DM2 has none) the game's word for its contact callback (BMX XXX's rider
                                   //       dynamics: the Rider, newRider)
#endif
        ee::Ptr<struct SceneObject> sceneObject; // +0x3C (DM2 +0x34) the object it moves
    };
    static_assert(offsetof(Dynamics, system) == game::readable::perGame(0x18u, 0x14u, 0x18u) && offsetof(Dynamics, contactCallback) == game::readable::perGame(0x34u, 0x30u, 0x34u) &&
                  offsetof(Dynamics, sceneObject) == game::readable::perGame(0x3Cu, 0x34u, 0x3Cu) && sizeof(Dynamics) == game::readable::perGame(0x40u, 0x38u, 0x40u));

    // A box in the world on its own: the contact tests of anything against a
    // body (bodyBox and the game's kinds' fill it).
    struct OrientedBox
    {
        Vec4 centre;            // +0x00
        Vec4 halfExtents;       // +0x10
        Quaternion orientation; // +0x20
    };
    static_assert(sizeof(OrientedBox) == 0x30);

    // The origin as a point.
    inline constexpr Vec4 kOrigin{0.0f, 0.0f, 0.0f, 1.0f};

    // An axis of the box tests and the two intervals along it, kept while a
    // pair's contact is worked out.
    struct SeparatingAxis
    {
        ee::Float centreA, radiusA; // +0x00
        ee::Float centreB, radiusB; // +0x08
        Vec4 axis;                  // +0x10
        ee::Float length;           // +0x20 before it was made a unit
        uint32_t _24[3];            // +0x24
    };
    static_assert(sizeof(SeparatingAxis) == 0x30 && offsetof(SeparatingAxis, axis) == 0x10);
    constexpr uint32_t kSeparatingAxisCount = 15;
    inline ee::Global<SeparatingAxis[kSeparatingAxisCount], sym::g_separatingAxes> g_separatingAxes;

    // The pool of the definitions read from the files.
    inline ee::Global<uint32_t, sym::g_dynamicsDefinitionPool> g_dynamicsDefinitionPool;
    constexpr uint32_t kDynamicsBytes = sizeof(Dynamics);

    // The dynamics in hand (the definition being read, the one being stepped).
    inline ee::Global<ee::Ptr<Dynamics>, sym::g_currentDynamics> g_currentDynamics;
    // Set for the step if a body is near anything to touch.
    inline ee::Global<uint32_t, sym::g_collideWithFaces> g_collideWithFaces;
    // The objects bodies collide with this step: what collisionQuery found.
#if ZX_GAME_DM2
    // ZGE2's contact objects: its query's count and array of HitNodes (HitNodeTable), the objects in place.
    inline ee::Global<HitNodeTable, sym::g_contactObjects> g_contactObjects;
    inline ee::Ptr<HitNode> contactObject(uint32_t index) { return g_contactObjects->nodes + index; }
    // (as ZGE3's results record, for the shared code that does not run in DM2)
    inline ee::Ptr<QueryResults> contactObjectResults() { return ee::Ptr<QueryResults>(sym::g_contactObjects); }
#else
    inline ee::Global<QueryResults, sym::g_contactObjects> g_contactObjects;
    inline ee::Ptr<HitNode> contactObject(uint32_t index) { return g_contactObjects->nodes[index]; }
    inline ee::Ptr<QueryResults> contactObjectResults() { return g_contactObjects.get(); }
#endif

    // A face of a contact object in the world, kept for the step
    // (transformContactFaces).
    struct WorldFace
    {
        Vec4 vertices[3];
        Vec4 normal;
    };
    static_assert(sizeof(WorldFace) == 0x40);
    constexpr uint32_t kWorldFaceCount = 40;
    inline ee::Global<WorldFace[kWorldFaceCount], sym::g_worldFaces> g_worldFaces;

    // What a body last touched (dynamicsContactObject returns it).
    inline ee::Global<ee::Ptr<HitNode>, sym::g_contactObject> g_contactObject;

    // How long a step of the dynamics is.
    inline ee::Global<ee::Float, sym::g_dynamicsStepSeconds> g_dynamicsStepSeconds;

    void boxClosestPoint(ee::Ptr<RigidBody> body, const Vec4 *normal, Vec4 *out, uint32_t cornersKnown, ee::Float distance);
    // (dynamics.cpp) The box's point on the plane of the face it touches (findBodyFaceContacts' step on its own).
    void boxPointOnFacePlane(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> out);
    // (dynamics.cpp) A box body's half extents copied out, and set.
    ee::Ptr<Vec4> boxHalfExtents(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> out);
    void setBoxHalfExtents(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> half);
    // (dynamics.cpp, ZGE4 only) Whether faces touch the body from either side.
    void setBodyTwoSidedFaces(ee::Ptr<RigidBody> body, uint32_t twoSided);
    void projectTriangleOnAxis(const Vec4 *triangle, const Vec4 *axis, ee::Float *centreOut, ee::Float *extentOut);
    uint32_t boxesOverlap(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second);
    uint32_t orientedBoxesOverlap(ee::Ptr<OrientedBox> first, ee::Ptr<OrientedBox> second);
    int32_t intervalsSeparate(ee::Float centreA, ee::Float radiusA, ee::Float centreB, ee::Float radiusB);
    int32_t classifyFaceNormal(const Vec4 *velocity, const Vec4 *normal);
    void pointVelocity(ee::Ptr<RigidBody> body, const Vec4 *point, Vec4 *out);
    void projectBoxOnAxis(ee::Ptr<RigidBody> body, const Vec4 *axis, ee::Float *centreOut, ee::Float *radiusOut);

    int32_t findBodyFaceContacts(ee::Ptr<RigidBody> body, Vec4 *pointOut, Vec4 *normalOut, Vec4 *pushOut);
    void updateWorldInertia(ee::Ptr<RigidBody> body);
    void bodyFromBone(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body, Vec4 *positionOut, Matrix *rotationOut, Quaternion *orientationOut);
    void integrateBody(ee::Ptr<RigidBody> body);
    void applyBodiesToObjects(ee::Ptr<Dynamics> dynamics);
    // What is asked about each face a body touches.
    void setDynamicsContactHandler(ee::Ptr<Dynamics> dynamics, ContactCallback handler);
    void freeDynamicsBodies(ee::Ptr<Dynamics> dynamics);
    void buildBodyHierarchy(ee::Ptr<Dynamics> dynamics);

    // A force of the dynamics (dead: nothing makes one; dynamics_load.cpp): its direction, its
    // strength in the direction's w, the direction's length (first in DM2's, as in its BodyExtents).
#if ZX_GAME_DM2
    struct DynamicsForce
    {
        ee::Float length; // DM2 +0x00
        uint32_t _04[3];
        Vec4 direction;   // DM2 +0x10 w: the strength (setForceStrength)
    };
#else
    struct DynamicsForce
    {
        Vec4 direction;   // +0x00 w: the strength (setForceStrength)
        ee::Float length; // +0x10
        uint32_t _14[3];
    };
#endif
    static_assert(sizeof(DynamicsForce) == 0x20 && offsetof(DynamicsForce, direction) == game::readable::perGame(0x00u, 0x10u, 0x00u));
    // A force's strength (its direction's w; the second word is unread) (dynamics_load.cpp).
    void setForceStrength(ee::Ptr<DynamicsForce> force, uint32_t unread, ee::Float strength);
    // A dynamics' contact damping. ZGE has no setter of its own for it: BMX XXX's bail calls setForceStrength's code
    // on the dynamics, whose word at a force's strength (+0x0C in ZGE3's and ZGE4's layouts) is the damping.
    inline void setDynamicsContactDamping(ee::Ptr<Dynamics> dynamics, ee::Float damping)
    {
        setForceStrength(ee::Ptr<DynamicsForce>(dynamics.address), 0, damping);
    }

    // A body's matrix copied into another. ZGE3 copies the four quadwords inline; ZGE2 calls its copy routine, which
    // copies the rotation's and the translation's x, y and z and leaves the w column (DM2's own row,
    // copyRotationAndTranslation; its rider's code had named it rowsFromMatrix). `asTail`: the original's tail call.
    inline void copyBodyMatrix(Matrix *out, const Matrix *m, bool asTail = false)
    {
        if constexpr (game::readable::kIsDm2)
        {
            if (asTail)
                ee::tailCall<void>(sym::dm2::copyRotationAndTranslation, ee::Ptr<Matrix>(out), ee::Ptr<const Matrix>(m));
            else
                ee::game(sym::dm2::copyRotationAndTranslation, ee::Ptr<Matrix>(out), ee::Ptr<const Matrix>(m));
        }
        else
            *out = *m;
    }

    // What a body's turning takes of an impulse along `direction` at `arm`
    // from its centre: d . ((I^-1 (r x d)) x r).
    inline ee::Float turningShare(const Vec4 &arm, const Vec4 &direction, const RigidBody &body)
    {
        const Vec4 turned = cross(rotated(body.worldInverseInertia, cross(arm, direction)), arm);
        return dot(turned, direction);
    }

    // An impulse at a point `arm` from the body's centre.
    inline void applyImpulse(RigidBody &body, const Vec4 &impulse, const Vec4 &arm)
    {
        const Vec4 change = impulse * (ee::Float(1.0f) / body.mass);
        body.velocity = body.velocity + change;
        const Vec4 spinChange = rotated(body.worldInverseInertia, cross(arm, impulse));
        body.spin = body.spin + spinChange;
    }
}

namespace zge
{
    // (dynamics.cpp)
    ee::Ptr<HitNode> dynamicsContactObject();
    void resolveBodyContacts(ee::Ptr<RigidBody> body);
    void resolveBodyContactsFromDynamics(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body); // ZGE4's
    void resolveBodyContactList(ee::Ptr<RigidBody> body);
    void orientedBoxContact(ee::Ptr<OrientedBox> first, ee::Ptr<OrientedBox> second, Vec4 *pointOut, Vec4 *normalOut, ee::Float *depthOut);
    uint32_t pairStillTouching(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second);
    void pairTouched(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second);
}

namespace zge
{
    // (dynamics_body.cpp)
    void contactImpulse(ee::Ptr<RigidBody> body, const Vec4 *point, const Vec4 *normal, const Vec4 *otherVelocity);
    void drawDynamicsBody(ee::Ptr<RigidBody> body);
    void driveBodyFromBone(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body, ee::Float spring, ee::Float damping, ee::Float turnShare);
}

namespace zge
{
    // (dynamics_load.cpp)
    void freeDynamicsRecord(ee::Ptr<Dynamics> dynamics);
    // (ZGE4 only) A dynamics copy from the definitions' pool freed with all it owns.
    void freeDynamicsCopy(ee::Ptr<Dynamics> dynamics);
    // (ZGE4 only) A copy of a definition from the definitions' pool with its first body alone, no links and an
    // empty system (0 when that body is not a box but has half extents). The first word is not read.
    ee::Ptr<Dynamics> copyDynamicsFirstBody(uint32_t object, ee::Ptr<Dynamics> definition);
    void setDynamicsRestitution(ee::Ptr<Dynamics> dynamics, ee::Float restitution);
    ee::Ptr<Dynamics> cloneDynamicsDefinition(ee::Ptr<Node> node, uint32_t definitionName);
    // A copy of a definition (its first argument not read; dynamics_load.cpp).
    ee::Ptr<Dynamics> copyDynamicsDefinition(uint32_t node, ee::Ptr<Dynamics> definition);
    void setContactPoint(ee::Ptr<BodyContact> contact, ee::Ptr<Vec4> target);
    void enableContact(ee::Ptr<BodyContact> contact);
    void disableContact(ee::Ptr<BodyContact> contact);
}

namespace zge
{
    // (dynamics_solver.cpp)
    void solveDynamicsIsland(ee::Ptr<Dynamics> dynamics);
    uint32_t registerDynamicsChunkHandlers();
}

namespace zge
{
    // (dynamics_update.cpp)
    uint32_t shutdownDynamicsPool();
    uint32_t resetDynamicsDefinitions();
    ee::Ptr<Dynamics> loadDynamicsDefinition(ee::Ptr<ChunkReader> reader);
    void wakeDynamics(ee::Ptr<SceneObject> object, ee::Ptr<Dynamics> dynamics, uint32_t flags);
    void sweepUpdateBody(ee::Ptr<Sweep> sweep, ee::Ptr<RigidBody> body);
    // A held point taken off the body's list and freed with its detail
    // (dynamics_load.cpp).
    int32_t removeBodyContact(ee::Ptr<RigidBody> body, ee::Ptr<BodyContact> contact);
    ee::Float updateDynamics(ee::Ptr<Dynamics> dynamics, ee::Ptr<Sweep> sweep, ee::Float seconds);
    // dynamicsDebugRecord: the contact query's results when debug drawing is on, else 0 (dynamics_load.cpp).
    uint32_t dynamicsDebugRecord();
    void drawDynamicsObject(ee::Ptr<Dynamics> dynamics);
}

namespace zge
{
    // (solver_matrix.cpp)
    ee::Ptr<SolverMatrix> clearSolverMatrix(ee::Ptr<SolverMatrix> matrix);
    ee::Ptr<SolverMatrix> invertSolverMatrix3or6(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> matrix);
    ee::Ptr<SolverMatrix> copySolverMatrix(ee::Ptr<SolverMatrix> out, ee::Ptr<const SolverMatrix> matrix);
    ee::Ptr<SolverMatrix> multiplySolverMatrices(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> a, ee::Ptr<SolverMatrix> b);
    void transposeSolverMatrix(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> matrix);
    ee::Ptr<SolverMatrix> addSolverMatrices(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> a, ee::Ptr<SolverMatrix> b);
    ee::Ptr<SolverMatrix> subtractSolverMatrices(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> a, ee::Ptr<SolverMatrix> b);
}

namespace zge
{
    // (body_push.cpp)
    uint32_t bodyBox(ee::Ptr<RigidBody> body, ee::Ptr<OrientedBox> box);
    void pushBodyFromPoint(ee::Ptr<OrientedBox> /*otherBox*/, ee::Ptr<RigidBody> body, ee::Ptr<Vec4> velocity, ee::Ptr<Vec4> point, ee::Float mass);

    // (dynamics_load.cpp) newBodyContactRecord: a held point of `body` (holding `other` at `target`); newDynamicsForce:
    // a force along `direction`, 0 when the heap has no room.
    ee::Ptr<BodyContact> newBodyContactRecord(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> offset, ee::Ptr<Vec4> target,
                                              ee::Ptr<RigidBody> other);
    // ZGE4's newBodyContactRecord (its own body): the held point with the slack the hold allows it before it pulls.
    ee::Ptr<BodyContact> newBodyContactRecordWithSlack(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> offset, ee::Ptr<Vec4> target,
                                                       ee::Ptr<RigidBody> other, ee::Float slack);
    ee::Ptr<DynamicsForce> newDynamicsForce(ee::Ptr<Vec4> direction);
}
