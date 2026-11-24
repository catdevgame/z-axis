// Practice's restart point for a rider (zge_practice_rider.h).

#include "zge_practice_rider.h"
#include "zge_practice.h"

#include "machine.h"
#include "runtime/session.h"

#include <cmath>
#include <cstring>
#include <iostream>

namespace zge_host::practice
{
    namespace
    {
        constexpr uint8_t kPhysicsGround = 0u;
        constexpr uint8_t kActionRiding = 0u;
        // A play frame's game time: the games run their play at 60 frames a
        // second (DM2's round.cpp kFramesPerSecond, BMX XXX's the same).
        constexpr float kFrameSeconds = 1.0f / 60.0f;

        struct Point
        {
            float rows[3][4] = {};
            float position[4] = {};
        };
        struct State
        {
            Point point;
        };
        State &state() { return zx::session::state<State>(); }

        uint32_t stackBelow(const R5900Context *ctx) { return (getRegU32(ctx, 29) - 0x200u) & ~0xFu; }

        uint32_t call(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t function,
                      std::initializer_list<uint32_t> args)
        {
            uint32_t v0 = 0;
            runtime->callGuestFunction(rdram, ctx, function, args, stackBelow(ctx), &v0);
            return v0;
        }

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        void copyIn(const uint8_t *rdram, uint32_t address, float *out, size_t count)
        {
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(out, p, count * sizeof(float));
        }

        void copyOut(uint8_t *rdram, uint32_t address, const float *values, size_t count)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, values, count * sizeof(float));
        }

        void logPlace(const RiderGame &game, const char *what, const float position[4], const float forward[4])
        {
            std::cout << "[" << game.log << "] practice " << what << ": position (" << position[0] << ", "
                      << position[1] << ", " << position[2] << ") forward (" << forward[0] << ", " << forward[1]
                      << ", " << forward[2] << ")" << std::endl;
        }

        void setPoint(const RiderGame &game, const uint8_t *rdram, uint32_t rider)
        {
            Practice &practice = current();
            if (readByte(rdram, rider + game.physicsState) != kPhysicsGround ||
                readByte(rdram, rider + game.actionState) != kActionRiding)
            {
                practice.pointRefused("A restart point needs the rider on the ground");
                return;
            }
            Point point;
            copyIn(rdram, rider + game.rows, &point.rows[0][0], 12);
            copyIn(rdram, rider + game.position, point.position, 4);
            state().point = point;
            logPlace(game, "point", point.position, point.rows[2]);
            practice.pointSet();
        }

        void returnToPoint(const RiderGame &game, uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime,
                           uint32_t player, uint32_t index, uint32_t rider)
        {
            const Point &point = state().point;
            if (readByte(rdram, rider + game.lineState) != 0u)
                call(rdram, ctx, runtime, game.trickLineEnd, {player, 0u});
            // The respawn looks for its face from the position, and keeps the
            // forward row (as spawnPlayer gives it its locator's).
            copyOut(rdram, rider + game.rows, &point.rows[0][0], 12);
            copyOut(rdram, rider + game.position, point.position, 4);
            copyOut(rdram, rider + game.drawnPosition, point.position, 4);
            call(rdram, ctx, runtime, game.riderReset, {index, 1u});
            if (game.cutCamera)
                game.cutCamera(rdram, ctx, runtime, player, stackBelow(ctx));
            float position[4] = {};
            float rows[3][4] = {};
            copyIn(rdram, rider + game.position, position, 4);
            copyIn(rdram, rider + game.rows, &rows[0][0], 12);
            logPlace(game, "back", position, rows[2]);
            const float dx = position[0] - point.position[0], dy = position[1] - point.position[1],
                        dz = position[2] - point.position[2];
            std::cout << "[" << game.log << "] practice: on the ground face " << std::sqrt(dx * dx + dy * dy + dz * dz)
                      << " units from the point, stopped" << std::endl;
            current().returned();
        }
    }

    bool riderPlayFrame(const RiderGame &game, uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime,
                        uint32_t player)
    {
        Practice &practice = current();
        const Request request = practice.take();
        if (request == Request::None)
        {
            practice.played(kFrameSeconds);
            return false;
        }
        const uint32_t index = call(rdram, ctx, runtime, game.riderIndexOf, {player}) & 0xFFu;
        const uint32_t rider = call(rdram, ctx, runtime, game.riderOf, {index});
        if (!rider)
            return false;
        if (request == Request::SetPoint)
        {
            setPoint(game, rdram, rider);
            practice.played(kFrameSeconds);
            return false;
        }
        returnToPoint(game, rdram, ctx, runtime, player, index, rider);
        return true;
    }

    void riderLeft()
    {
        if (current().hasPoint())
            current().leave();
    }
}
