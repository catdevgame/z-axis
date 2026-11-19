// The Manual state's per-frame callbacks (skater.h StateRow::update 4 and
// 5): the balance meter, and the turn, jump and balance input.
//
// The manual's balance is Player::balance[1]: it drifts by a balance speed
// (+0x27C) that grows in the direction it already goes, faster the longer
// the manual lasts (+0x28C) and by the skater's manual stat (Manual Balance
// Factor, Skater / Manual Balance Add); the stick pushes it back (Manual
// Balance Adjust) once Manual Balance Delay (+0x264, set by the enter) has
// run out. Past 1 either way the manual ends.

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "moves.h"
#include "aggressive/hud/hud.h"
#include "sym/aggressive/skater/manual.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // The world's up (0, -1, 0, 0).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        constexpr uint32_t kPhysics = 3;
        constexpr uint32_t kGrindWalls = 14;
        constexpr uint32_t kManTurnRate = 21;          // Physics / Man Turn Rate
        constexpr uint32_t kManualBalanceAdjust = 20;  // Grind/Walls / Manual Balance Adjust
    }

    // manualBalance, Manual's fourth callback: the balance for a frame. After
    // Manual Balance Delay the drift speeds up away from the middle, the
    // meter moves and shows; the skater stands on the ground normal; past 1
    // the manual ends (Ground).
    void manualBalance(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Float &manualTime = player->manualTime;
        ee::Float &balanceDelay = player->balanceDelay;
        ee::Float &balanceSpeed = player->balanceSpeed;
        ee::Float &balance = player->balance[1];
        manualTime = manualTime + frameTime;
        player->shortTime = player->shortTime + frameTime;
        if (0.0f < balanceDelay)
            balanceDelay = balanceDelay - frameTime;
        else
        {
            // Away from the middle: the way the balance already drifts.
            const bool forward = 0.0f < balanceSpeed;
            const ee::Float factor = skaterStat(player, kStatManual, kStatManualFactor);
            if (forward)
                balanceSpeed = balanceSpeed + manualTime * factor;
            else
                balanceSpeed = balanceSpeed - manualTime * factor;
            balance = balance + balanceSpeed;
        }

        aggressive::hud::setBalance(player, balance);
        alignToGroundNormal(player);
        if (1.0f < ee::abs(balance))
            game::tail(aggressive::skater::setSkaterState, player, static_cast<uint32_t>(State::Ground));
    }
    READABLE_FUNCTION(manualBalance, Verified);

    // manualControls, Manual's fifth callback: the input. Jump: the skater's
    // jump speed straight up. Stick (Controls::stickX) turns by Man Turn
    // Rate, by the Fakie stat more when riding fakie. After Manual Balance
    // Delay, Controls::stickY pushes the balance (Manual Balance Adjust).
    void manualControls(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<zge::Vec4> jump;
        if (player->controls.actions & kActionJump)
        {
            const ee::Float jumpSpeed = skaterStat(player, kStatJump, 0);
            *jump = *g_worldUp * jumpSpeed;
            player->velocity = player->velocity + *jump;
            player->settleFrames = 0;
        }

        ee::Float &turnSpeed = player->turnRate;
        if (player->controls.stickX == 0.0f)
            turnSpeed = 0.0f;
        else if (player->fakie)
        {
            const ee::Ptr<TuningEntry> physics = (*g_tuningCategories)[kPhysics].entries;
            const ee::Float fakie = skaterStat(player, kStatFakie, 0);
            turnSpeed = player->controls.stickX * (physics[kManTurnRate].value * fakie);
        }
        else
            turnSpeed = player->controls.stickX * tuning(kPhysics, kManTurnRate);

        if (player->balanceDelay <= 0.0f)
        {
            const ee::Float push = player->controls.stickY;
            if (push != 0.0f)
            {
                player->balanceSpeed = player->balanceSpeed + frameTime * push * tuning(kGrindWalls, kManualBalanceAdjust);
            }
        }
    }
    READABLE_FUNCTION(manualControls, Verified);
}
