// The skater's per-frame physics: the state machine's callbacks, the
// respawn after a bail, the speed and the developers' state overlay.

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "zge/debug/debug.h"
#include "sce/libgcc.h"
#include "moves.h"
#include "aggressive/game_start.h"

#include <cstring>
#include "sym/aggressive/skater/update.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        const StateRow &stateRow(ee::Ptr<Player> player)
        {
            return (*g_states)[static_cast<uint8_t>(player->state)];
        }

        uint32_t sceneObjectName(uint32_t object)
        {
            return object ? object + kSceneObjectName : 0u;
        }

        uint64_t printfDouble(ee::Float value)
        {
            return sce::libgcc::fptodp(value);
        }

        namespace tail
        {
            // debugPrint is variadic: its tail call keeps the address form (game::tail takes no defaulted words).
            void debugPrint(uint32_t x, uint32_t y, uint32_t format, uint32_t first, uint64_t second) { ee::tailCall(kDebugPrint, x, y, format, first, second); }
        }

        // Toggle Debug's level 1 and 2 (DEVTOOLS.md): the physics state as text.
        void drawDebugText(ee::Ptr<Player> player)
        {
            const uint32_t objectName = sceneObjectName(player->object);
            const uint32_t lastObjectName = sceneObjectName(player->lastObject);
            zge::debugPrint(0, 0x0C, sym::g_textPhysicsStateSObjectS, stateRow(player).name, objectName);
            zge::debugPrint(0, 0x140, sym::g_textObjectSLastS, objectName, lastObjectName);
            const uint8_t substate = player->substate;
            const uint64_t balance0 = printfDouble(player->balance[0]);
            const uint8_t side = player->side;
            const uint64_t balance1 = printfDouble(player->balance[1]);
            zge::debugPrint(0, 0x18, sym::g_textSubstateDSideDBalance02fBalance12f, substate, side, balance0, balance1);
            const uint64_t shortTime = printfDouble(player->shortTime);
            tail::debugPrint(0, -1, sym::g_textShortDState2F, player->shortCount, shortTime);
        }
    }

    void skaterUpdate(ee::Ptr<Player> player, ee::Float frameTime)
    {
        *g_skaterFrameTime = frameTime;
        if (player->settleFrames < 20)
            ++player->settleFrames;
        player->stateTime = player->stateTime + frameTime;

        for (int i = 0; i < 4; ++i)
            if (const auto callback = stateRow(player).update[i])
                callback(player, frameTime);
        if (const auto callback = stateRow(player).update[4]; callback && !(player->flags & kDown))
            callback(player, frameTime);

        // Down after a bail: back at the respawn point once Respawn Time has passed.
        if (player->flags & kDown)
        {
            player->downTime = player->downTime + frameTime;
            const ee::Float respawnTime = (*g_tuningCategories)[0].entries[kTuningRespawnTime].value;
            if (respawnTime < player->downTime)
            {
                respawn(player, 0, 0);
                crashJuiceLoss(player);
            }
        }

        player->previousSpeed = player->speed;
        player->speed = zge::vec3Length(&player->velocity.xyz());
        aggressive::updateLevelRecord(kLevelRecordTopSpeed, *g_levelWord, player->name, player->speed);

        if (*g_debugDisplay & 3u)
            drawDebugText(player);
    }
    READABLE_FUNCTION(skaterUpdate, Verified);

    void alignToGroundNormal(ee::Ptr<Player> player)
    {
        const zge::Vec4 &normal = player->groundNormal;
        if (normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f)
            return;
        // Up becomes the normal (all four words: an LQ / SQ copy), then
        // forward and right are made square to it.
        player->axes.up = normal;
        game::tail(orthonormalize, &player->axes.forward, &player->axes.right, &player->axes.up);
    }
    READABLE_FUNCTION(alignToGroundNormal, Verified);
}
