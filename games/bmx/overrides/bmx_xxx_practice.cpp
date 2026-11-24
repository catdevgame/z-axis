// BMX XXX (SLUS-20415): Practice's restart point, the rider's
// (engine/overrides/zge_practice_rider.h): the rider stopped on the ground
// face nearest the point, heading as there, through the game's own respawn
// (riderReset), the action camera cut as the level exit's respawn cuts it
// (checkLevelExit: level 1's cut, then following).

#include "bmx_xxx_practice.h"
#include "sym/game_overrides/bmx_xxx_practice.cpp.h"

#include "overrides/zge_practice.h"
#include "overrides/zge_practice_rider.h"
#include "machine.h"

namespace bmx_xxx::practice
{
    namespace
    {
        namespace zp = zge_host::practice;

        constexpr uint32_t kCurrentPlayer = sym::bmx::g_currentPlayer; // byte
        constexpr uint32_t kPlayerCamera = sym::bmx::playerCamera;     // (player) -> its action camera
        constexpr uint32_t kSetCameraMode = sym::bmx::setCameraMode;   // (camera, level, mode)
        // games/bmx/game/level_exit.cpp: level 1's cut, then following.
        constexpr uint32_t kCameraCut = 3u;
        constexpr uint32_t kCameraFollow = 0u;

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        void cutCamera(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t player, uint32_t sp)
        {
            uint32_t camera = 0;
            if (!runtime->callGuestFunction(rdram, ctx, kPlayerCamera, {player}, sp, &camera) || !camera)
                return;
            runtime->callGuestFunction(rdram, ctx, kSetCameraMode, {camera, 1u, kCameraCut}, sp);
            runtime->callGuestFunction(rdram, ctx, kSetCameraMode, {camera, 1u, kCameraFollow}, sp);
        }

        // games/bmx/rider/rider.h Rider.
        const zp::RiderGame kRider{
            "bmx-xxx",
            0x2610u, // right, up, forward
            0x2650u, // position
            0x2BE0u, // drawnPosition
            0x2600u, // physicsState
            0x017Fu, // actionStates[0]
            0x1BE0u, // score (+0x1C0) .lineState (+0x1A20)
            sym::bmx::riderIndexOf,
            sym::bmx::riderOf,
            sym::bmx::riderReset,
            sym::bmx::trickLineEnd,
            &cutCamera,
        };
    }

    bool onVsync(uint8_t *, bool inLevel, bool pointPressed, bool restartPressed)
    {
        if (!inLevel)
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
