// Two of the level's physics bodies touching (readable round gamehand3;
// over level/dynamics.h's structs since gamehand12): the contact sweep's
// handler for a pair of rigid bodies (setupLevelContacts sets it,
// level/dynamics_objects.cpp). A body asleep is woken when the other is
// awake, and both objects are told "Collide" with the bodies' relative
// motion over the step.

#include "aggressive/level/dynamics.h"
#include "aggressive/tuning.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/dynamics.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/bodies.cpp.h"

namespace aggressive::level
{
    namespace
    {
        constexpr uint32_t kCollideName = sym::collideName;   // "Collide"

        // What "Collide" carries.
        struct CollideData
        {
            zge::Vec4 _00;
            zge::Vec4 relativeVelocity; // +0x10 a's less b's, over the step
            zge::Vec4 _20;
        };
        static_assert(sizeof(CollideData) == 0x30);
    }

    namespace
    {
        // The engine's view of a level body (zge/collision/dynamics.h): the same record.
        ee::Ptr<zge::RigidBody> engineBody(ee::Ptr<RigidBody> body) { return body.cast<zge::RigidBody>(); }
        // And of its dynamics.
        ee::Ptr<zge::Dynamics> engineDynamics(ee::Ptr<Dynamics> dynamics) { return dynamics.cast<zge::Dynamics>(); }

        void sendCollide(uint32_t sender, uint32_t target, ee::Address data)
        {
            constexpr uint32_t kSynchronous = 1;
            zge::sendNewMessage(sender, target, zge::nameHash(ee::Ptr<const char>(kCollideName)), data, kSynchronous);
        }
    }

    // bodiesTouch: bodies `a` and `b` touching. Nothing for two parts of one
    // record, nor when both are asleep, nor when the engine says the contact
    // no longer holds. Otherwise each record asleep whose other is awake (or
    // has no record) is woken: its dynamics woken, its flag set, its body's
    // force made its weight, its rest time cleared, its frame handler
    // subscribed; then the engine keeps the contact and, when both
    // dynamics have scene objects, each is sent "Collide" with their relative
    // velocity over the step.
    void bodiesTouch(ee::Ptr<RigidBody> a, ee::Ptr<RigidBody> b)
    {
        ee::Local<CollideData> data;
        const ee::Ptr<DynamicsRecord> records[2] = {a->record, b->record};
        if (records[0] == records[1])
            return;
        const ee::Ptr<RigidBody> bodies[2] = {a, b};
        const ee::Ptr<Dynamics> dynamics[2] = {records[0]->dynamics, records[1]->dynamics};
        if (!((records[0]->flags | records[1]->flags) & kDynamicsAwake))
            return;
        if (zge::pairStillTouching(engineBody(a), engineBody(b)) == 0)
            return;

        for (int i = 0; i < 2; ++i)
        {
            const ee::Ptr<DynamicsRecord> record = records[i];
            const ee::Ptr<DynamicsRecord> other = records[1 - i];
            if (!record || (other && !(other->flags & kDynamicsAwake)) || (record->flags & kDynamicsAwake))
                continue;
            zge::wakeDynamics(ee::Ptr<zge::SceneObject>(record->sceneObject), engineDynamics(record->dynamics), 1);
            record->flags |= kDynamicsAwake;
            bodies[i]->force.x = 0.0f;
            bodies[i]->force.y = tuningValue(kTuningPhysics, 0 /* Gravity */) * bodies[i]->mass;
            bodies[i]->force.z = 0.0f;
            record->restTime = 0.0f;
            objectWoken(record);
        }

        const zge::Vec4 difference = a->velocity - b->velocity;
        zge::pairTouched(engineBody(a), engineBody(b));
        zge::memoryFill(data, 0, sizeof(CollideData));
        data->relativeVelocity = difference * (ee::Float(1.0f) / *g_stepRate);
        if (dynamics[0]->sceneObject == 0 || dynamics[1]->sceneObject == 0)
            return;
        sendCollide(dynamics[0]->sceneObject, dynamics[1]->sceneObject, data);
        sendCollide(dynamics[1]->sceneObject, dynamics[0]->sceneObject, data);
    }
    READABLE_FUNCTION_WAITS(bodiesTouch, Verified);
}
