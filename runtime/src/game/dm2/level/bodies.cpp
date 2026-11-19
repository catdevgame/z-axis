// Two bodies of the level's sweep touching (bodies.h). DM2's own code (its game block).

#include "bodies.h"
#include "dm2/game/goals.h"

#include "dynamics_objects.h"

#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/bodies.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The game's gravity, a frame; a body's weight is it over a second squared.
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        constexpr ee::Float kFramesASecond = 60.0f;
        // A step's seconds (1 / 60).
        inline ee::Global<ee::Float, sym::g_dynamicsStepSeconds> g_stepSeconds;

        bool isRiderBody(uint32_t owner) { return (owner & kRiderBodyTag) == kRiderBodyTag; }
    }

    void bodiesTouch(ee::Ptr<zge::RigidBody> a, ee::Ptr<zge::RigidBody> b)
    {
        ee::Local<zge::Vec4> difference;
        ee::Local<TouchData> data;
        bool told = false;      // something of this touch is worth telling
        int32_t riderSlot = -1; // which of the two is a rider's body
        const ee::Ptr<zge::RigidBody> bodies[2] = {a, b};
        if (a->record == b->record)
            return;

        uint32_t owners[2];
        ee::Ptr<DynamicsRecord> records[2];
        ee::Ptr<zge::Dynamics> dynamics[2];
        uint8_t riderIndex[2] = {};
        int riderBodies = 0;
        for (int i = 0; i < 2; ++i)
        {
            owners[i] = bodies[i]->record;
            if (isRiderBody(owners[i]))
            {
                told = true;
                ++riderBodies;
                riderSlot = i;
                riderIndex[i] = static_cast<uint8_t>(owners[i]);
                dynamics[i] = ee::Ptr<zge::Dynamics>(rider::riderOf(riderIndex[i])->riderDynamics);
                records[i] = {};
            }
            else
            {
                records[i] = ee::Ptr<DynamicsRecord>(owners[i]);
                dynamics[i] = records[i]->dynamics;
            }
        }
        if (riderBodies == 2)
            return;
        if (zge::pairStillTouching(a, b) == 0)
            return;

        for (int i = 0; i < 2; ++i)
        {
            const ee::Ptr<DynamicsRecord> record = records[i];
            const ee::Ptr<DynamicsRecord> other = records[1 - i];
            if (record)
            {
                if (other && !(other->flags & kDynamicsAwake))
                    continue;
                told = true;
                if (record->flags & kDynamicsAwake)
                    continue;
                zge::wakeDynamics(record->sceneObject, record->dynamics, 1);
                record->flags |= kDynamicsAwake;
                const ee::Float weight = *g_gravity * kFramesASecond * kFramesASecond;
                bodies[i]->force.x = 0.0f;
                bodies[i]->force.y = weight * bodies[i]->mass;
                bodies[i]->force.z = 0.0f;
                objectWoken(record);
            }
            else if (isRiderBody(owners[i]) && other)
            {
                if (!(rider::riderOf(riderIndex[i])->flags & rider::kFlagNoPlayer))
                    gameTouchHandler(rider::riderOf(riderIndex[i])->player, other->sceneObject);
            }
        }
        if (!told)
            return;

        *difference = a->velocity - b->velocity;
        zge::pairTouched(a, b);
        zge::memoryFill(data, 0, sizeof(TouchData));
        // Scaffolding (t0-leftover): the factor reaches VU0 through a register whose upper half the last call left.
        data->velocity = zge::scaledCarrying(*difference, *g_stepSeconds, zge::scaleLeftover(zge::LeftBy::lastCall));

        if (riderSlot >= 0)
        {
            // The rider keeps its body's share of the two masses of its speed over the ground.
            const ee::Float share = bodies[riderSlot]->mass / (a->mass + b->mass);
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndex[riderSlot]);
            const ee::Float x = rider->velocity[0] * share;
            const ee::Float z = rider->velocity[2] * share;
            rider->velocity[2] = z;
            rider->velocity[0] = x;
            const ee::Ptr<zge::SceneObject> object = dynamics[1 - riderSlot]->sceneObject;
            if (object)
                zge::sendNewMessage(rider->messenger, object, kTouchMessage, data, zge::kMessageNow);
            return;
        }
        if (!dynamics[0]->sceneObject || !dynamics[1]->sceneObject)
            return;
        zge::sendNewMessage(dynamics[0]->sceneObject, dynamics[1]->sceneObject, kTouchMessage, data, zge::kMessageNow);
        zge::sendNewMessage(dynamics[1]->sceneObject, dynamics[0]->sceneObject, kTouchMessage, data, zge::kMessageNow);
    }
    READABLE_FUNCTION_WAITS(bodiesTouch, Verified);
}
