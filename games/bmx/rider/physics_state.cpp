// BMX XXX's physics task and states: the task registered at start, the
// rider's physics state and its change through the exit and enter tables,
// an impulse, the ground's enter and two exits. DM2's (games/dm2/rider/physics_state.cpp);
// BMX XXX's engine task call is its engine's newTask.

#include "rider.h"

#include "bmx/fx/marker.h"
#include "bmx/game/player_riders.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/tasks.h"
#include "zge/render/primitives.h"
#include "sym/bmx/rider/physics_state.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The physics task's callback (DM2's rider map names its
        // counterpart debugDrawRider).
        constexpr uint32_t kPhysicsTaskFrame = sym::debugDrawRider;
        constexpr uint32_t kTaskStage = 1;   // the stage it runs in
        constexpr uint32_t kTaskAtEnd = 1;   // added at the stage's end
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_physicsTask> g_physicsTask;
        // Frames the physics task has run.
        inline ee::Global<uint32_t, sym::g_physicsTaskFrames> g_physicsTaskFrames;
        // "./src/com/action/physics.c", physicsInit's line.
        constexpr uint32_t kPhysicsFile = sym::physicsFile;
        constexpr uint32_t kPhysicsInitLine = 457;

        // A physics state's exit (rider, from, to), 0 for none, and enter
        // (rider, from, to) -> the state entered.
        using StateExit = ee::FunctionPtr<void(Rider *, uint32_t, uint32_t)>;
        using StateEnter = ee::FunctionPtr<int32_t(Rider *, uint32_t, uint32_t)>;
        inline ee::Global<StateExit[kPhysicsStates], sym::g_physicsExits> g_physicsExits;
        inline ee::Global<StateEnter[kPhysicsStates], sym::g_physicsEnters> g_physicsEnters;
    }

    // Registers the physics task (and its frame counter from 0).
    void physicsInit()
    {
        *g_physicsTask = zge::newTask(kTaskStage, kPhysicsTaskFrame, 0u, kTaskAtEnd, kPhysicsFile, kPhysicsInitLine);
        *g_physicsTaskFrames = 0;
    }
    READABLE_FUNCTION(physicsInit, Verified);

    // The physics task deleted (the level's end). deleteTask's result (1).
    uint32_t deletePhysicsTask()
    {
        return game::tail(zge::deleteTask, *g_physicsTask);
    }
    READABLE_FUNCTION(deletePhysicsTask, Verified);

    namespace
    {
        inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;
        inline ee::Global<int32_t, sym::g_drawBodies> g_drawBodies; // set: the riders' debug lines and points drawn
        // The debug line (a line of two vertices) and its vertices.
        inline ee::Global<zge::PrimitiveLine, sym::g_riderDebugLine> g_debugLine;
        inline ee::Global<zge::PrimitiveVertex[2], sym::g_riderDebugLineVertices> g_debugLineVertices;
        constexpr uint32_t kDebugLineFlags = 2; // drawLines' primitive flags for them
        // The wheels' rows are drawn 0.3 long; the points 0.1 across.
        inline ee::Global<ee::Float, sym::g_debugRowLength> g_debugRowLength;
        inline ee::Global<ee::Float, sym::g_debugPointSize> g_debugPointSize;
        constexpr uint8_t kNoRider = 0xFF;
        void setColour(uint8_t *rgba, uint8_t r, uint8_t g, uint8_t b)
        {
            rgba[0] = r;
            rgba[1] = g;
            rgba[2] = b;
            rgba[3] = 0xFF;
        }
    }

    // debugDrawRider: the physics task's frame (DM2's rider map's name): with g_drawBodies set, each shown
    // player's rider drawn for debugging: from each wheel's point on the body its rows (on the ground) or a line
    // down its height (off it), in orange; points at the position (cyan), at the position plus the root offset
    // (orange) and at the drawn body's (green). Then the task's frame count. 1.
    uint32_t debugDrawRider()
    {
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> marker;
        for (uint32_t player = 0; *g_playerCount != 0 ? player < *g_playerCount : player == 0; ++player)
        {
            const uint8_t index = riderIndexOf(player);
            if (index == kNoRider)
                continue;
            Rider *const rider = riderOf(index).get();
            if (!hasFlag(rider, kFlagShown) || *g_drawBodies == 0)
                continue;
            zge::PrimitiveVertex &from = (*g_debugLineVertices)[0];
            zge::PrimitiveVertex &to = (*g_debugLineVertices)[1];
            const ee::Float length = *g_debugRowLength;
            to.rgba[3] = 0xFF;
            setColour(from.rgba, 0xB0, 0x50, 0x10);
            to.rgba[0] = 0xB0;
            to.rgba[1] = 0x50;
            to.rgba[2] = 0x10;
            for (uint32_t wheel = 0; wheel < 2; ++wheel)
            {
                const WheelPose &pose = rider->wheelPoses[wheel];
                *point = *vec4(wheel ? rider->backWheelPoint : rider->frontWheelPoint);
                *point = zge::transformed(*matrix(rider->body), *point);
                *point = *point + *vec4(rider->drawnPosition);
                zge::quadwordOf(ee::Ptr<zge::PrimitiveVertex>(&from)) = *point; // the vertex's position quadword
                if (pose.ground != kWheelOffGround)
                {
                    for (int row = 2; row >= 0; --row)
                    {
                        to.x = from.x + pose.matrix[row * 4 + 0] * length;
                        to.y = from.y + pose.matrix[row * 4 + 1] * length;
                        to.z = from.z + pose.matrix[row * 4 + 2] * length;
                        zge::drawLines(kDebugLineFlags, ee::Address(g_debugLine.get()), 1u, 0u);
                    }
                }
                else
                {
                    const ee::Float height = wheel ? rider->backWheelHeight : rider->frontWheelHeight;
                    to.x = point->x + -rider->body[4] * height;
                    to.y = point->y + -rider->body[5] * height;
                    to.z = point->z + -rider->body[6] * height;
                    zge::drawLines(kDebugLineFlags, ee::Address(g_debugLine.get()), 1u, 0u);
                }
            }
            setColour(from.rgba, 0x20, 0xB0, 0xB0);
            drawSpinningMarker(ee::Ptr<zge::Vec4>(vec4(rider->position)), ee::Ptr<uint8_t>(from.rgba), *g_debugPointSize);
            setColour(from.rgba, 0xB0, 0x50, 0x10);
            *marker = *vec4(rider->position) + *vec4(rider->rootOffset);
            drawSpinningMarker(marker, ee::Ptr<uint8_t>(from.rgba), *g_debugPointSize);
            *marker = *vec4(rider->drawnPosition);
            setColour(from.rgba, 0x30, 0xB0, 0x20);
            drawSpinningMarker(marker, ee::Ptr<uint8_t>(from.rgba), *g_debugPointSize);
        }
        *g_physicsTaskFrames = *g_physicsTaskFrames + 1;
        return 1;
    }
    READABLE_FUNCTION(debugDrawRider, Verified);

    // wheelieEnter (rider, from, to): physics states 9 and 10's enter (the nose wheelie, the manual): the rider's
    // place moved onto the balancing wheel's point (in the drawn body's space), the motion's root offset from it
    // (the root less the bike's, y and z the other way, the wheel's point taken off, in the drawn body's rows).
    // Returns the state. DM2's does nothing.
    uint32_t wheelieEnter(Rider *rider, uint32_t from, uint32_t to)
    {
        (void)from;
        ee::Local<zge::Vec4> point;
        ee::Float *const wheelPoint = to == kPhysicsManual ? rider->backWheelPoint : rider->frontWheelPoint;
        *point = *vec4(wheelPoint);
        *point = zge::transformed(*matrix(rider->body), *point);
        *point = *point + *vec4(rider->drawnPosition);
        *vec4(rider->position) = *point;
        zge::Vec4 &offset = *vec4(rider->rootOffset);
        offset = *vec4(rider->motionRoot) - *vec4(rider->drawnOffset);
        rider->rootOffset[2] = -rider->rootOffset[2];
        rider->rootOffset[1] = -rider->rootOffset[1];
        zge::vec3Negate(&point->xyz(), &vec4(wheelPoint)->xyz());
        offset = offset + *point;
        offset = zge::transformed(*matrix(rider->body), offset);
        return to;
    }
    READABLE_FUNCTION(wheelieEnter, Verified);

    uint8_t physicsState(const Rider *rider)
    {
        return rider->physicsState;
    }
    READABLE_FUNCTION(physicsState, Verified);

    // Changes the physics state: the old state's exit, then the new one's
    // enter, whose result is the state the rider is in (an enter can go on
    // to another). Returns it. Every call, a change or not, forgets the
    // object touched and clears fakie, kFlagBarsHeld and kFlagBalanceLeanOff.
    uint8_t setPhysicsState(Rider *rider, uint32_t state)
    {
        rider->touchedObject = 0;
        clearFlag(rider, kFlagFakie);
        clearFlag(rider, kFlagBarsHeld);
        clearFlag(rider, kFlagBalanceLeanOff);
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

    // State 0's enter: off a grind, the drawn body takes the rows (a whole
    // matrix; DM2 copies its rotation and position), and the rider rides
    // backwards (fakie) when moving against the forward row; the air's frames
    // and angles cleared.
    uint32_t groundEnter(Rider *rider, uint32_t from, uint32_t to)
    {
        if (from == kPhysicsGrind)
        {
            *matrix(rider->body) = *matrix(rider->right);
            if (zge::dot(*vec4(rider->forward), *vec4(rider->velocity)) < 0.0f)
                setFlag(rider, kFlagFakie);
            else
                clearFlag(rider, kFlagFakie);
        }
        rider->groundHeight = 0;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        return to;
    }
    READABLE_FUNCTION(groundEnter, Verified);

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
    READABLE_FUNCTION(bailExit, Verified);
}
