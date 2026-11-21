// DM2's tips (game.c). A turn starts with every tip still to show
// (rider::kAllTips in the score block's tipsLeft); a tip shown is taken out,
// and with none left the player's tips go off.
//
// The tips come in two groups. Of the first twelve (the air's, and one for
// riding upright) only the lowest one still to show is looked at each
// frame. The other ten (the ground's, the bail's, the grind's) are each
// looked at in turn until one is on the screen.

#include "tips.h"
#include "players.h"

#include "dm2/overlay/overlay.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/tips.cpp.h"

namespace game::dm2
{
    namespace
    {
        // What the tip label takes and gives back as its state: the tip,
        // and whether the label is on the screen.
        struct TipLabel
        {
            uint8_t tip;
            uint8_t pad01; // padding: no reader (pad2: the senders never set it; only whole-message copies move it)
            uint16_t visible;
        };
        static_assert(offsetof(TipLabel, visible) == 2 && sizeof(TipLabel) == 4);

        constexpr uint32_t kAirTips = 12;
        constexpr uint32_t kTips = 22;
        // The tips with something of their own: one that also takes tip 12
        // out, two that stay to be shown again.
        constexpr uint32_t kTipStandingStill = 13, kTipTakesTip12 = 16, kTipSecondAction4 = 17;
        constexpr uint32_t kTip12 = 12;

        constexpr uint8_t kFirstSurfaceTrick = 0x33;
        constexpr uint8_t kContactGround = 1;       // Rider::contactKind
        constexpr uint32_t kFlagUpright = 0x2000000; // Rider::flags (rider/physics/ground_move.cpp's name)
        constexpr uint8_t kFirstUnfinishedTrickBail = 7; // Rider::bailKind 7 and 8: a trick not finished on landing

        bool rising(const rider::Rider *rider)
        {
            return rider->velocity[1] < 0.0f; // (y grows downwards)
        }
        uint8_t action(const rider::Rider *rider)
        {
            return rider::actionState(rider, rider::kLevelAction);
        }
        uint8_t secondAction(const rider::Rider *rider)
        {
            return rider::actionState(rider, 1);
        }

        // Whether an air tip's moment has come.
        bool airTipMoment(uint32_t tip, const rider::Rider *rider, const rider::ScoreBlock &score)
        {
            switch (tip)
            {
            case 0:
            case 2:
            case 6:
                // Going up in an air with nothing done yet this run.
                return action(rider) == rider::kActionAir && secondAction(rider) == 0 && score.lineCount == 0 && rising(rider);
            case 1:
                return action(rider) == rider::kActionAir && secondAction(rider) == 2;
            case 3:
            case 4:
                return action(rider) == rider::kActionAir && secondAction(rider) == 1;
            case 5:
                // In an air after an air trick.
                if (!(action(rider) == rider::kActionAir && secondAction(rider) == 0))
                    return false;
                if (score.lineCount == 0)
                    return false;
                return score.lines[score.lineCount - 1].tricks->trick < kFirstSurfaceTrick;
            case 7:
            case 8:
            case 9:
                return action(rider) == rider::kActionAir && secondAction(rider) == 0 && rising(rider);
            case 10:
                return action(rider) == rider::kActionAir && rising(rider);
            case 11:
                return action(rider) == rider::kActionRiding && (rider->flags & kFlagUpright) != 0;
            default:
                return false;
            }
        }

        // Whether one of the other tips' moment has come.
        bool groundTipMoment(uint32_t tip, const rider::Rider *rider)
        {
            switch (tip)
            {
            case 12:
                return action(rider) == rider::kActionRiding && secondAction(rider) == 3;
            case kTipStandingStill:
                return action(rider) == rider::kActionRiding && secondAction(rider) == 0 && rider->speed == 0.0f;
            case 14:
                return action(rider) == rider::kActionRiding && secondAction(rider) == 0 && !(rider->speed == 0.0f);
            case 15:
                return action(rider) == rider::kActionRiding && secondAction(rider) == 1;
            case kTipTakesTip12:
            case 19:
                // Leaving the ground upwards while still riding.
                return action(rider) == rider::kActionRiding && rider->contactKind == kContactGround && rising(rider);
            case kTipSecondAction4:
                return action(rider) == rider::kActionRiding && secondAction(rider) == 4;
            case 18:
                return action(rider) == rider::kActionBail &&
                       static_cast<uint32_t>(rider->bailKind - kFirstUnfinishedTrickBail) < 2;
            case 20:
                return action(rider) == rider::kActionGrind;
            case 21:
                return action(rider) == rider::kActionRiding && secondAction(rider) == 3 &&
                       rider->contactKind == kContactGround && rising(rider);
            default:
                return false;
            }
        }
    }

    void tipsFrame(uint32_t player)
    {
        ee::Local<TipLabel> label;
        PlayerSetup &setup = (*g_playerSetups)[player];
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        rider::ScoreBlock &score = rider->score;
        if ((setup.flags & kPlayerTips) == 0)
            return;

        overlayFrameState(kOverlayTip, label);
        if (label->visible == 0)
        {
            const uint32_t left = score.tipsLeft;
            for (uint32_t tip = 0; tip < kAirTips; ++tip)
            {
                if ((left >> tip & 1) == 0)
                    continue;
                label->tip = static_cast<uint8_t>(tip);
                if (airTipMoment(tip, rider, score))
                {
                    overlayMessage(kOverlayTip, label);
                    score.tipsLeft &= ~(1u << tip);
                }
                if (score.tipsLeft == 0)
                    setup.flags &= static_cast<uint8_t>(~kPlayerTips);
                break;
            }
        }

        overlayFrameState(kOverlayTip, label);
        if (label->visible != 0)
            return;
        for (uint32_t tip = kAirTips; tip < kTips; ++tip)
        {
            if ((score.tipsLeft >> tip & 1) == 0)
                continue;
            label->tip = static_cast<uint8_t>(tip);
            if (groundTipMoment(tip, rider))
            {
                overlayMessage(kOverlayTip, label);
                if (tip == kTipTakesTip12)
                    score.tipsLeft &= ~(1u << tip) & ~(1u << kTip12);
                else if (tip != kTipStandingStill && tip != kTipSecondAction4)
                    score.tipsLeft &= ~(1u << tip);
            }
            overlayFrameState(kOverlayTip, label);
            if (label->visible != 0)
            {
                if (score.tipsLeft == 0)
                    setup.flags &= static_cast<uint8_t>(~kPlayerTips);
                return;
            }
        }
    }
    READABLE_FUNCTION(tipsFrame, Verified);
}
