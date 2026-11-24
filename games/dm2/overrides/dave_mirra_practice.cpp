// Dave Mirra Freestyle BMX 2 (SLUS-20159): Practice's restart point, the
// rider's (engine/overrides/zge_practice_rider.h): the rider stopped on the
// ground face nearest the point, heading as there, through the game's own
// respawn (riderReset), the camera cut as a turn's start cuts it (the
// before-turn mode, then following). Single player only: a multiplayer
// game's turns move between the players' riders.

#include "dave_mirra_practice.h"
#include "sym/game_overrides/dave_mirra_practice.cpp.h"

#include "overrides/zge_practice.h"
#include "overrides/zge_practice_rider.h"
#include "machine.h"

namespace dave_mirra::practice
{
    namespace
    {
        namespace zp = zge_host::practice;

        constexpr uint32_t kCurrentPlayer = sym::dm2::g_currentPlayer;           // byte
        constexpr uint32_t kMultiplayerPlayers = sym::dm2::g_multiplayerPlayers; // byte: the players (1 in single player)
        constexpr uint32_t kSetCameraMode = sym::dm2::setCameraMode;             // (player, level, mode)
        // games/dm2/camera/camera.h: level 0's modes; the turn's start
        // (round.cpp kCameraBeforeTurn) cuts from it to following.
        constexpr uint32_t kCameraBeforeTurn = 3u;
        constexpr uint32_t kCameraFollows = 0u;

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        void cutCamera(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t player, uint32_t sp)
        {
            runtime->callGuestFunction(rdram, ctx, kSetCameraMode, {player, 0u, kCameraBeforeTurn}, sp);
            runtime->callGuestFunction(rdram, ctx, kSetCameraMode, {player, 0u, kCameraFollows}, sp);
        }

        // games/dm2/rider/rider.h Rider.
        const zp::RiderGame kRider{
            "dave-mirra",
            0x1000u, // right, up, forward
            0x1040u, // position
            0x14F0u, // drawnPosition
            0x0FF0u, // physicsState
            0x0165u, // actionStates[0]
            0x083Cu, // score.lineState
            sym::dm2::riderIndexOf,
            sym::dm2::riderOf,
            sym::dm2::riderReset,
            sym::dm2::trickLineEnd,
            &cutCamera,
        };
    }

    bool onVsync(uint8_t *rdram, bool inLevel, bool pointPressed, bool restartPressed)
    {
        if (!inLevel || readByte(rdram, kMultiplayerPlayers) > 1u)
        {
            zp::riderLeft();
            return false;
        }
        return zp::current().onVsync(pointPressed, restartPressed);
    }

    bool playFrame(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        return zp::riderPlayFrame(kRider, rdram, ctx, runtime, readByte(rdram, kCurrentPlayer));
    }
}
