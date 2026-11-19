// DM2's physics states (physics.c): the physics task, the
// state change through the exit and enter tables, an impulse, and the
// enters and exits that do little.
//
// Counterparts: DM2 GameCube (physicsInit and the state changes), BMX XXX PS2.

#include "rider.h"
#include "physics/physics.h"
#include "../game/players.h"
#include "../fx/debug_draw.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/render/primitives.h"
#include "sym/dm2/rider/physics_state.cpp.h"

namespace zge
{
    // (primitives.cpp, in no header)
    int32_t drawLines(uint32_t primitiveFlags, uint32_t vertices, uint32_t vertexCount, uint32_t vertexMatrix);
}

namespace game::dm2::rider
{
    namespace
    {
        constexpr uint32_t kPhysicsTaskFrame = sym::debugDrawRider; // the task's callback (the rider map's debugDrawRider)

        inline ee::Global<ee::Ptr<zge::Task>, sym::g_physicsTask> g_physicsTask;
        // The rider records by index (rider_state.cpp's).
        inline ee::Global<ee::Ptr<Rider>[16], sym::g_riders> g_riders;
        // Frames the physics task has run (dm2logic's level anchor).
        inline ee::Global<uint32_t, sym::g_physicsTaskFrames> g_physicsTaskFrames;
        // "./src/com/physics.c", physicsInit's line.
        constexpr uint32_t kPhysicsFile = sym::physicsFile;
        constexpr uint32_t kPhysicsInitLine = 333;

        // A physics state's exit (rider, from, to), 0 for none, and enter
        // (rider, from, to) -> the state entered.
        using StateExit = ee::FunctionPtr<void(Rider *, uint32_t, uint32_t)>;
        using StateEnter = ee::FunctionPtr<int32_t(Rider *, uint32_t, uint32_t)>;
        inline ee::Global<StateExit[11], sym::g_physicsExits> g_physicsExits;
        inline ee::Global<StateEnter[11], sym::g_physicsEnters> g_physicsEnters;
    }

    // Registers the physics task (and its frame counter from 0).
    void physicsInit()
    {
        *g_physicsTask = zge::newTask(1, kPhysicsTaskFrame, 0, 1, kPhysicsFile, kPhysicsInitLine);
        *g_physicsTaskFrames = 0;
    }
    READABLE_FUNCTION(physicsInit, Verified);

    // The physics task deleted (the level's end). deleteTask's result (1).
    uint32_t deletePhysicsTask()
    {
        return game::tail(zge::deleteTask, *g_physicsTask);
    }
    READABLE_FUNCTION(deletePhysicsTask, Verified);

    // The level's end for the physics: every rider still in a record freed,
    // then the physics task deleted (whose result, 1, it answers).
    uint32_t physicsShutdown()
    {
        for (uint32_t index = 0; index < 16; ++index)
        {
            if ((*g_riders)[index])
                freeRider(index & 0xFFu);
        }
        return game::tail(deletePhysicsTask);
    }
    READABLE_FUNCTION(physicsShutdown, Verified);

    uint8_t physicsState(const Rider *rider)
    {
        return rider->physicsState;
    }
    READABLE_FUNCTION(physicsState, Verified);

    // Changes the physics state: the old state's exit, then the new one's
    // enter, whose result is the state the rider is in (an enter can go on
    // to another). Returns it.
    uint8_t setPhysicsState(Rider *rider, uint32_t state)
    {
        rider->touchedObject = 0;
        rider->flags &= ~kFlagFakie;
        const uint8_t from = physicsState(rider);
        if (from != state)
        {
            // Leaving no state (0xFF) runs the bail's exit.
            if (from == kPhysicsNone)
                (*g_physicsExits)[kPhysicsBail](rider, kPhysicsNone, state);
            else if (const StateExit exit = (*g_physicsExits)[from])
                exit(rider, from, state);
            rider->physicsState = static_cast<uint8_t>((*g_physicsEnters)[state](rider, from, state));
        }
        return rider->physicsState;
    }
    READABLE_FUNCTION(setPhysicsState, Verified);

    // velocity += impulse, and the speed again: the hop off the ground, a
    // rail, a lip or a wall.
    void addImpulse(Rider *rider, const ee::Float *impulse)
    {
        zge::Vec4 &velocity = *vec4(rider->velocity);
        velocity = velocity + *vec4(impulse);
        rider->speed = zge::vec3Length(&velocity.xyz());
    }
    READABLE_FUNCTION(addImpulse, Verified);

    // State 4's exit: the air's angles cleared. Returns the state left.
    uint32_t grindExit(Rider *rider, uint32_t from, uint32_t /*to*/)
    {
        rider->roll = 0;
        rider->pitch = 0;
        rider->yawRate = 0;
        return from;
    }
    READABLE_FUNCTION(grindExit, Verified);

    // State 2's exit (and leaving no state): nothing. Returns the state left.
    uint32_t bailExit(Rider * /*rider*/, uint32_t from, uint32_t /*to*/)
    {
        return from;
    }
    READABLE_FUNCTION(bailExit, Draft);

    // State 0's enter: off a grind, riding backwards (fakie) when moving
    // against the bike's forward row; the air's frames and angles cleared.
    uint32_t groundEnter(Rider *rider, uint32_t from, uint32_t to)
    {
        if (from == kPhysicsGrind)
        {
            zge::copyMatrixRotationAndTranslation(matrix(rider->body), matrix(rider->right)); // the body from the rows
            if (zge::dot(*vec4(rider->forward), *vec4(rider->velocity)) < 0.0f)
                rider->flags |= kFlagFakie;
            else
                rider->flags &= ~kFlagFakie;
        }
        rider->groundHeight = 0;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        return to;
    }
    READABLE_FUNCTION(groundEnter, Verified);

    // States 9 and 10's enter (the nose wheelie, the manual): nothing.
    uint32_t wheelieEnter(Rider * /*rider*/, uint32_t /*from*/, uint32_t to)
    {
        return to;
    }
    READABLE_FUNCTION(wheelieEnter, Verified);

    namespace
    {
        // The riders' debug drawing (with the debug setting g_bodiesSettle
        // on): a line of two vertices and its record, the wheels' lines 0.3
        // long, the markers (debugDrawAxes) 0.1.
        inline ee::Global<int32_t, sym::g_riderDebugDraw> g_debugDraw;
        inline ee::Global<zge::PrimitiveVertex[2], sym::riderDebugLineVertices> g_debugLineVertices;
        inline ee::Global<zge::PrimitiveLine, sym::riderDebugLine> g_debugLine;
        inline ee::Global<ee::Float, sym::riderDebugAxesSize> g_debugAxesSize;
        inline ee::Global<ee::Float, sym::riderDebugWheelLine> g_debugWheelLine;
        constexpr uint32_t kDrawLineKind = 2;
        constexpr uint8_t kNoRiderIndex = 0xFF;
        // The colours: the wheels' lines and the root's marker orange, the
        // position's marker blue.
        constexpr uint8_t kOrange[4] = {0xB0, 0x50, 0x10, 0xFF};
        constexpr uint8_t kBlue[4] = {0x20, 0x20, 0xB0, 0xFF};
        void setColour(uint8_t (&rgba)[4], const uint8_t (&colour)[4])
        {
            for (int i = 0; i < 4; ++i)
                rgba[i] = colour[i];
        }
    }

    // debugDrawRider: the physics task's frame (its callback): with the
    // debug setting on, each shown player's rider drawn: each wheel a line
    // from its place down the body's up row by its height (off the
    // ground), else three along its matrix's rows; a marker at the physics
    // position and one at the motion's root. Then the task's frames counted.
    // Always 1.
    uint32_t debugDrawRider()
    {
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> root;
        zge::PrimitiveVertex &from = (*g_debugLineVertices)[0];
        zge::PrimitiveVertex &to = (*g_debugLineVertices)[1];
        for (uint32_t player = 0; *g_multiplayerPlayers == 0 ? player == 0 : player < *g_multiplayerPlayers; ++player)
        {
            const uint8_t index = riderIndexOf(player);
            if (index == kNoRiderIndex)
                continue;
            Rider *const rider = riderOf(index).get();
            if (!(rider->flags & kFlagShown) || *g_debugDraw == 0)
                continue;
            setColour(to.rgba, kOrange);
            setColour(from.rgba, kOrange);
            const ee::Float reach = *g_debugWheelLine;
            for (uint32_t wheel = 0; wheel < 2; ++wheel)
            {
                const ee::Float *const wheelPoint = wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint;
                const ee::Float wheelHeight = wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight;
                const WheelPose &pose = rider->wheelPoses[wheel];
                *point = *vec4(wheelPoint);
                physics::engine::transformPoint(&point->x, rider->body, &point->x);
                *point = *point + *vec4(rider->drawnPosition);
                *vec4(&from.x) = *point;
                const auto line = [&](const ee::Float *row, ee::Float length) {
                    to.x = from.x + row[0] * length;
                    to.y = from.y + row[1] * length;
                    to.z = from.z + row[2] * length;
                    zge::drawLines(kDrawLineKind, ee::Address(&*g_debugLine), 1, 0);
                };
                if (pose.ground == kWheelOffGround)
                {
                    to.x = point->x + -rider->body[4] * wheelHeight;
                    to.y = point->y + -rider->body[5] * wheelHeight;
                    to.z = point->z + -rider->body[6] * wheelHeight;
                    zge::drawLines(kDrawLineKind, ee::Address(&*g_debugLine), 1, 0);
                }
                else
                {
                    line(&pose.matrix[8], reach);
                    line(&pose.matrix[4], reach);
                    line(&pose.matrix[0], reach);
                }
            }
            setColour(from.rgba, kBlue);
            debugDrawAxes(ee::Ptr<zge::Vec4>(vec4(rider->position)), from.rgba, *g_debugAxesSize);
            setColour(from.rgba, kOrange);
            *root = *vec4(rider->position) + *vec4(rider->rootOffset);
            debugDrawAxes(root, from.rgba, *g_debugAxesSize);
        }
        ++*g_physicsTaskFrames;
        return 1;
    }
    READABLE_FUNCTION_WAITS(debugDrawRider, Verified);
}
