// DM2's bail (physics state 2; MIXING 14.1's bail rows): the tumbling body
// handed to the engine's dynamics and the hands held where they touched.
//
// The bail's dynamics (DM2's engine, ZGE2): the rider's model becomes a
// chain of bodies (wakeDynamics sets a model's bodies going), each body a
// record with its position at +0xE0, velocity +0xF0, matrix +0x160, spin
// +0x1A0, gravity +0x2A0 and the next body at +0x408. The hands' records
// (rider +0x1470 left, +0x14B0 right; bailHandContact fills them): two
// points in a node's space (+0x00, +0x10), the node (+0x20), the hand's
// body (+0x28), the share between the points (+0x30), on the ground (+0x34).
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x8007C1F8 (bailEnter),
// 0x80086DF8 (bailHandsHeld); BMX XXX PS2 bailHandsHeld.

#include "actions.h"
#include "../collision/collision.h"
#include "../physics/physics.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "zge/collision/collision.h"
#include "zge/collision/dynamics.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/actions/bail.cpp.h"

namespace game::dm2::rider
{
    namespace
    {
        constexpr uint32_t kWorldMatrix = 1; // nodeMatrix's
        inline ee::Global<int32_t, sym::g_hipsBoneHash> g_rootBodyName; // the body the rider's turn is about (a name hash)
        inline ee::Global<ee::Float, sym::g_bodyLimit> g_wakeTurnShare; // the bone drive's turn share as the bail starts
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity; // 0.0055 a frame
        constexpr int32_t kFramesASecond = 60;               // the dynamics run in seconds
    }

    // Physics state 2's enter: the rider's and the bike's models become
    // the dynamics' bodies, going with the rider's velocity (and his spin
    // turned into each body's velocity about the root body), the bike's
    // spinning down. The hands let go unless the bail keeps them. Returns
    // the state asked for.
    uint32_t bailEnter(Rider *rider, uint32_t /*from*/, uint32_t state)
    {
        ee::Local<zge::Vec4> v;           // a body's offset from the root, then its velocity
        ee::Local<zge::Vec4> root;        // the root body's position
        ee::Local<zge::Matrix> rotation;  // the rider's turn
        ee::Local<zge::Matrix> inverse;   // and back
        ee::Local<zge::EulerAngles> spin; // the turn a frame (pitch, yaw rate, roll)
        ee::Local<uint32_t> asked;        // the state asked for (the original keeps it in its frame)
        *asked = state;
        // Every scale here carries what this function's caller left (the
        // calls leave it alone), as airGravity's does.
        const uint64_t carried = zge::scaleLeftover(zge::LeftBy::caller);
        const ee::Float perSecond = ee::toFloat(kFramesASecond);

        zge::quaternionToMatrix(rotation, zge::objectOrientation(ee::Ptr<zge::SceneObject>(rider->messenger)));
        physics::setOrigin(&rotation->row(3).x);
        zge::matrixTranspose3(inverse, rotation);
        physics::setOrigin(&inverse->row(3).x);
        spin->x = rider->pitch;
        spin->y = rider->yawRate;
        spin->z = rider->roll;
        zge::wakeDynamics(ee::Ptr<zge::SceneObject>(rider->messenger), ee::Ptr<zge::Dynamics>(rider->riderDynamics), 1);
        if (!(rider->flags & kFlagNoPlayer))
        {
            rider->hands[0].holding = 0;
            rider->hands[1].holding = 0;
            rider->flags &= ~kFlagBailHands;
            zge::disableContact(ee::Ptr<zge::BodyContact>(rider->hands[0].body));
            zge::disableContact(ee::Ptr<zge::BodyContact>(rider->hands[1].body));
        }

        // The root body's position (the first body named so; with none, the
        // quadword the original reads at a null body's position).
        const ee::Ptr<zge::Dynamics> riderBodies(rider->riderDynamics);
        const uint32_t rootName = uint32_t(*g_rootBodyName);
        ee::Ptr<zge::RigidBody> rootBody = riderBodies->bodies;
        while (rootBody && rootBody->nameHash != rootName)
            rootBody = rootBody->next;
        *root = rootBody->position;

        // The rider's bodies: the bone drive, gravity (in units a second),
        // and the velocity: the rider's, plus the turn about the root a
        // frame (in the body's space, by the spin), in units a second.
        for (ee::Ptr<zge::RigidBody> body = riderBodies->bodies; body; body = body->next)
        {
            zge::driveBodyFromBone(ee::Ptr<zge::Dynamics>(rider->riderDynamics), body, ee::Float(0.0f), ee::Float(0.0f), *g_wakeTurnShare);
            const ee::Float mass = body->mass;
            body->force.z = 0.0f;
            body->force.x = 0.0f;
            body->force.y = *g_gravity * perSecond * perSecond * mass;
            *v = body->position - *root;
            physics::engine::transformPoint(&v->x, inverse, &v->x);
            zge::vec3RotateEuler(&v->xyz(), &v->xyz(), spin);
            physics::engine::transformPoint(&v->x, rotation, &v->x);
            *v = *v + *root;
            *v = *v - body->position;
            *v = zge::scaledCarrying(*v, perSecond, carried);
            body->velocity = *v;
            *v = zge::scaledCarrying(*vec4(rider->velocity), perSecond, carried);
            body->velocity = body->velocity + *v;
        }

        // The bike's bodies: gravity, the rider's velocity with rising
        // halved (y is down), and the spin 5 x the forward row's unit (the
        // original works out -2 x the right row's first, then overwrites it).
        zge::wakeDynamics(ee::Ptr<zge::SceneObject>(rider->bikeModel), ee::Ptr<zge::Dynamics>(rider->bikeDynamics), 1);
        for (ee::Ptr<zge::RigidBody> body = ee::Ptr<zge::Dynamics>(rider->bikeDynamics)->bodies; body; body = body->next)
        {
            const ee::Float mass = body->mass;
            body->force.x = 0.0f;
            body->force.z = 0.0f;
            body->force.y = *g_gravity * perSecond * perSecond * mass;
            body->velocity = zge::scaledCarrying(*vec4(rider->velocity), perSecond, carried);
            if (body->velocity.y < body->force.x)
                body->velocity.y = body->velocity.y * ee::Float(0.5f);
            body->spin = zge::unit(body->rotation.row(0));
            body->spin = zge::scaledCarrying(body->spin, ee::Float(-2.0f), carried);
            body->spin = zge::unit(body->rotation.row(2));
            body->spin = zge::scaledCarrying(body->spin, ee::Float(5.0f), carried);
        }

        rider->rideDistance = 0;
        rider->groundHeight = ee::Float(0.0f);
        return *asked;
    }
    READABLE_FUNCTION(bailEnter, Verified);

    // The bail's hands held where they touched (each hand on the ground:
    // its body held at the point between its two, in the node's world
    // space). MIXING 14.1's bailBodyPose.
    void bailHandsHeld(Rider *rider)
    {
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> a;
        ee::Local<zge::Vec4> b;
        auto hold = [&](const BailHand &hand) {
            physics::engine::transformPoint(&a->x, zge::nodeMatrix(ee::Ptr<zge::Node>(hand.node), kWorldMatrix), hand.edgeStart);
            physics::engine::transformPoint(&b->x, zge::nodeMatrix(ee::Ptr<zge::Node>(hand.node), kWorldMatrix), hand.edgeEnd);
            // The scale carries what the transforms' calls left beside the
            // share (the caller's: they leave it alone), as airGravity's does.
            const uint64_t carried = zge::scaleLeftover(zge::LeftBy::lastCall);
            *point = *b - *a;
            *point = zge::scaledCarrying(*point, hand.share, carried);
            *point = *point + *a;
            const ee::Ptr<zge::BodyContact> body(hand.body);
            zge::setContactPoint(body, point);
            zge::enableContact(body); // the hand's body held where its contact point is
        };
        if (rider->hands[0].holding != 0)
            hold(rider->hands[0]);
        if (rider->hands[1].holding != 0)
            hold(rider->hands[1]);
    }
    READABLE_FUNCTION(sym::bailBodyPose, bailHandsHeld, Verified);

    namespace
    {
        // bailMove's tuning.
        inline ee::Global<ee::Float, sym::g_bailSpinKept> g_bailSpinKept;  // the wheels' spin kept a frame
        // The bone drive's turn share for a body touching the world's faces
        // (RigidBody::contactCount set), and for one that is not.
        inline ee::Global<ee::Float, sym::g_freeBodyLimit> g_touchingTurnShare;
        inline ee::Global<ee::Float, sym::g_heldBodyLimit> g_clearTurnShare;
        inline ee::Global<ee::Float, sym::g_riderStepTime> g_riderStepTime;  // the rider's bodies' time a step (two steps a frame)
        inline ee::Global<ee::Float, sym::g_bikeStepTime> g_bikeStepTime;
        inline ee::Global<ee::Float, sym::g_riderVelocityScale> g_riderVelocityScale; // units a second to a frame
        inline ee::Global<ee::Float, sym::g_bikeVelocityScale> g_bikeVelocityScale;
        inline ee::Global<int32_t, sym::g_riderDebugDraw> g_drawBodies; // set: both models' bodies drawn after the step
        inline ee::Global<int32_t, sym::g_hipsDeckBoneHash> g_bikeRootBodyName;
        constexpr uint8_t kContactWall = 9; // the rider's contact kind: a wall (the bail does not move him)
        constexpr uint32_t kBailSoundKind = 1;
        constexpr uint32_t kBailSoundSurface = 3;
        // The rolling sound: the voice played by a sound code at a position
        // and a rate (the game side's).
        inline constexpr ee::FunctionPtr<void(uint32_t voice, int32_t code, const ee::Float *position, int32_t rate)>
            updateRollSound{sym::updateRollSound};

        // The engine's dynamics, called where they are: the bodies drawn
        // (drawDynamicsObject).
        inline void drawBodies(ee::Ptr<zge::Dynamics> dynamics) { ee::game(sym::drawDynamicsObject, dynamics); }
        // The engine's vec3Negate, called as the original calls it (the scale
        // after it carries what this call left).
        inline void negateByCall(ee::Float *v) { ee::game(sym::vec3Negate, v, v); }

        // The first body named `name` of a dynamics, else none.
        ee::Ptr<zge::RigidBody> bodyNamed(ee::Ptr<zge::Dynamics> dynamics, uint32_t name)
        {
            ee::Ptr<zge::RigidBody> body = dynamics->bodies;
            while (body && body->nameHash != name)
                body = body->next;
            return body;
        }
        // A dynamics stepped through `seconds`, a step at a time.
        void stepBodies(const uint32_t &dynamics, ee::Float seconds)
        {
            ee::Float left = seconds;
            do
                left = left - zge::updateDynamics(ee::Ptr<zge::Dynamics>(dynamics), ee::Ptr<zge::Sweep>(), left); // the time it stepped
            while (ee::Float(0.0f) < left);
        }

        // Where the bike's root body goes (the original keeps these five
        // addresses in its frame): the wheels' velocities and the drawn
        // body's rows.
        struct BikeTargets
        {
            ee::Ptr<ee::Float> backWheel; // +0x00
            ee::Ptr<ee::Float> right;     // +0x04
            ee::Ptr<ee::Float> up;        // +0x08
            ee::Ptr<ee::Float> frontWheel; // +0x0C
            ee::Ptr<ee::Float> forward;   // +0x10
        };
        static_assert(sizeof(BikeTargets) == 0x14 && offsetof(BikeTargets, forward) == 0x10);
    }

    // Physics state 2's move: the wheels spin down; unless against a wall,
    // the rider's bodies get their bone drive (by whether they touch the
    // world), the hands are held where they touched, the dynamics step (the
    // rider's twice), and the rider follows his root body (rows from its
    // turn, up and forward turned round; velocity, position, speed) and the
    // bike its root body (the drawn rows, the wheels' velocity, the drawn
    // position). Then the rolling sound, as in the air. GameCube 0x80082634.
    void bailMove(Rider *rider)
    {
        ee::Local<BikeTargets> bike;
        const ee::Float spinKept = *g_bailSpinKept;
        for (WheelPose &wheel : rider->wheelPoses)
            wheel.spin = ee::toInt(ee::toFloat(wheel.spin) * spinKept);
        if (rider->contactKind != kContactWall)
        {
            bike->right = &rider->body[0];
            bike->up = &rider->body[4];
            bike->forward = &rider->body[8];
            bike->backWheel = rider->wheelPoses[0].velocity;
            bike->frontWheel = rider->wheelPoses[1].velocity;
            for (ee::Ptr<zge::RigidBody> body = ee::Ptr<zge::Dynamics>(rider->riderDynamics)->bodies; body; body = body->next)
            {
                const ee::Float share = body->contactCount != 0 ? *g_touchingTurnShare : *g_clearTurnShare;
                zge::driveBodyFromBone(ee::Ptr<zge::Dynamics>(rider->riderDynamics), body, ee::Float(0.0f), ee::Float(0.0f), share);
            }
            if (!(rider->flags & kFlagNoPlayer))
                bailHandsHeld(rider);
            for (uint32_t step = 0; step < 2; ++step)
                stepBodies(rider->riderDynamics, *g_riderStepTime);
            if (!(rider->flags & kFlagNoPlayer))
            {
                bailBodyRelease(rider);
                bailHands(rider);
            }
            stepBodies(rider->bikeDynamics, *g_bikeStepTime);
            if (*g_drawBodies != 0)
            {
                drawBodies(ee::Ptr<zge::Dynamics>(rider->riderDynamics));
                drawBodies(ee::Ptr<zge::Dynamics>(rider->bikeDynamics));
            }

            // The rider by his root body (with none, what the original reads
            // at a null body). The scales carry what the last call left, as
            // airGravity's do.
            const ee::Ptr<zge::RigidBody> root = bodyNamed(ee::Ptr<zge::Dynamics>(rider->riderDynamics), uint32_t(*g_rootBodyName));
            zge::quaternionToMatrix(matrix(rider->right), &root->orientation);
            zge::vec3Negate(&vec4(rider->up)->xyz(), &vec4(rider->up)->xyz());
            negateByCall(rider->forward);
            *vec4(rider->velocity) = zge::scaledCarrying(root->velocity, *g_riderVelocityScale, zge::scaleLeftover(zge::LeftBy::lastCall));
            *vec4(rider->position) = root->position;
            physics::setOrigin(rider->rootOffset);
            rider->speed = zge::vec3Length(&vec4(rider->velocity)->xyz());

            // The bike by its root body.
            const ee::Ptr<zge::RigidBody> bikeRoot = bodyNamed(ee::Ptr<zge::Dynamics>(rider->bikeDynamics), uint32_t(*g_bikeRootBodyName));
            zge::quaternionToMatrix(matrix(bike->right), &bikeRoot->orientation);
            zge::vec3Negate(&vec4(bike->up)->xyz(), &vec4(bike->up)->xyz());
            negateByCall(bike->forward);
            const uint64_t carried = zge::scaleLeftover(zge::LeftBy::lastCall);
            const ee::Float bikeScale = *g_bikeVelocityScale;
            *vec4(bike->backWheel) = zge::scaledCarrying(bikeRoot->velocity, bikeScale, carried);
            *vec4(bike->frontWheel) = zge::scaledCarrying(bikeRoot->velocity, bikeScale, carried);
            *vec4(rider->drawnPosition) = bikeRoot->position;
        }
        const int32_t sound = surfaceSoundFlags(0u, 0u, kBailSoundKind, kBailSoundSurface);
        updateRollSound.tail(rider->rollingVoice, sound, rider->drawnPosition, rider->wheelPoses[1].spin);
    }
    READABLE_FUNCTION(bailMove, Verified);

    namespace collision
    {
        uint64_t nodeHandle(uint64_t node); // round 2's (collision/faces.cpp, tags.cpp)
        uint32_t edgeTags(uint64_t node, int32_t face, uint32_t side);
        uint32_t tagGrindable(uint32_t record);
    }

    namespace
    {
        // The faces the bail's bodies touched this frame (the dynamics'
        // contact query; inferred), or none.
        inline const collision::QueryHits *bodyContacts() { return ee::Ptr<const collision::QueryHits>(zge::dynamicsDebugRecord()); }
        inline ee::Global<ee::Float, sym::g_handReach> g_handReach; // the squared distance a hand reaches to an edge
        constexpr uint32_t kCorners = 3;
    }

    // A bail's hand against the faces the bodies touched: of the faces'
    // grindable edges (an edge, a kerb, a ledge), the one nearest the hand
    // (`point`, in the world), if within reach, is held: the hand record
    // gets the edge's two vertices (in its node's space), the node, and the
    // share along the edge of the nearest point (bailHandsHeld holds the
    // hand there). The record's holding byte says one was found. The first
    // edge found out of reach clears it again and the next one starts over.
    // GameCube 0x80086F68; DM2 Xbox 0x050600.
    void bailHandContact(Rider * /*rider*/, BailHand *hand, const zge::Vec4 *point)
    {
        ee::Local<zge::Vec4> onEdge;          // the point on the edge nearest the hand
        ee::Local<zge::Vec4> toHand;          // from it to the hand
        ee::Local<zge::Vec4> bestPoint;       // the nearest of all
        ee::Local<zge::Vec4> bestStart;       // its edge (in the world)
        ee::Local<zge::Vec4> bestEnd;
        ee::Local<zge::Vec4[kCorners]> corners;     // the face's corners in the world
        ee::Local<ee::Ptr<uint16_t>> indices; // the face's vertex indices (faceVertices')
        ee::Float nearest = ee::Float(0.0f);  // the best edge's squared distance
        if (const collision::QueryHits *const hits = bodyContacts())
        {
            const uint32_t nodeCount = static_cast<uint32_t>(hits->nodeCount);
            for (uint32_t n = 0; n < nodeCount; ++n)
            {
                const uint32_t handle =
                    static_cast<uint32_t>(collision::nodeHandle(collision::widen(hits->nodes[n].node)));
                const collision::QueryNode &entry = hits->nodes[n];
                const uint32_t node = entry.node;
                const ee::Ptr<zge::ZgePart> part(entry.part);
                const uint32_t faceCount = static_cast<uint32_t>(entry.faceCount);
                for (uint32_t f = 0; f < faceCount; ++f)
                {
                    const int32_t face = hits->nodes[n].faces[f].face;
                    zge::faceVertices(part, face, indices);
                    // The face's corners in the world.
                    for (uint32_t c = 0; c < kCorners; ++c)
                    {
                        zge::vertexPosition(part, (*indices)[c], onEdge);
                        const ee::Ptr<zge::Matrix> matrix = zge::nodeMatrix(ee::Ptr<zge::Node>(node), kWorldMatrix);
                        physics::engine::transformPoint(&corners[c].x, matrix, &onEdge->x);
                    }
                    // Its grindable edges, each against the best so far.
                    for (uint32_t side = 0; side < kCorners; ++side)
                    {
                        const uint32_t tags = collision::edgeTags(collision::widen(handle), face, side & 0xFFu);
                        if (tags == 0 || collision::tagGrindable(tags) == 0)
                            continue;
                        const uint32_t next = (side + 1) % kCorners;
                        const zge::Vec4 &start = corners[side];
                        const zge::Vec4 &end = corners[next];
                        zge::closestOnSegment(onEdge, &start, &end, point);
                        const bool first = hand->holding == 0;
                        if (first)
                            hand->holding = 1;
                        else
                        {
                            *toHand = *onEdge - *point;
                            const ee::Float distance = zge::dot(*toHand, *toHand);
                            if (!(distance < nearest))
                                continue;
                            nearest = distance;
                        }
                        // The edge's vertices in the node's space, for bailHandsHeld.
                        hand->nodeHandle = handle;
                        hand->node = node;
                        zge::vertexPosition(part, (*indices)[side], vec4(hand->edgeStart));
                        zge::vertexPosition(part, (*indices)[next], vec4(hand->edgeEnd));
                        *toHand = *onEdge - *point;
                        if (first)
                        {
                            nearest = zge::dot(*toHand, *toHand);
                            if (*g_handReach < nearest)
                                hand->holding = 0; // out of reach: the next edge starts over
                        }
                        *bestPoint = *onEdge;
                        *bestStart = start;
                        *bestEnd = end;
                    }
                }
            }
        }
        if (hand->holding == 0)
            return;
        // The share along the edge: (best - start).(end - start) / |end - start|^2.
        *onEdge = *bestPoint - *bestStart;
        *toHand = *bestEnd - *bestStart;
        const ee::Float length = zge::dot(*toHand, *toHand);
        const ee::Float along = zge::dot(*toHand, *onEdge);
        hand->share = along / length;
    }
    READABLE_FUNCTION(bailHandContact, Verified);
}
