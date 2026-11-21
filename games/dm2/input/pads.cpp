// DM2's pads as the game keeps them (pads.h). DM2's own code (camera.c's
// block, with the rider's input helpers).

#include "pads.h"

#include "dm2/action/action.h"
#include "dm2/game/players.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/input/pads.h"

#include <cstdint>
#include "sym/dm2/input/pads.cpp.h"

namespace game::dm2
{
    using rider::PadInput;

    namespace
    {
        constexpr uint32_t kPorts = 2;
        inline ee::Global<PadInput[kPorts], sym::g_pads> g_pads;
        // Not 0 while something other than the play has the pads.
        inline ee::Global<uint8_t, sym::g_padsTaken> g_padsTaken;

        bool vibrationOn(uint32_t player)
        {
            return (ee::Ptr<PlayerSetup>(sym::g_playerSetups)[player].flags & kPlayerVibration) != 0;
        }

        // The small motor is on or off: any level asks for this.
        constexpr uint32_t kSmallMotorOn = 200;
        // A rumble lasts this long (milliseconds).
        constexpr uint32_t kRumbleTime = 500;
        constexpr uint32_t kRumbleTag = 2;


        ee::Ptr<zge::Pad> driverPad(const PadInput &pad) { return ee::Ptr<zge::Pad>(pad.pad); }
        bool connected(const PadInput &pad) { return (driverPad(pad)->flags & zge::kPadConnected) != 0; }

        void clearSticks(PadInput &pad)
        {
            for (uint32_t stick = 0; stick < 2; ++stick)
            {
                pad.buttons.sticks[stick].x = 0.0f;
                pad.buttons.sticks[stick].y = 0.0f;
                pad.buttons.previous[stick].x = 0.0f;
                pad.buttons.previous[stick].y = 0.0f;
            }
        }
    }

    void initPads()
    {
        for (PadInput &pad : *g_pads)
        {
            const ee::Ptr<zge::Pad> made = zge::newPad(0);
            pad.flags = 0;
            pad.pad = ee::Address(made);
            pad.ignored = 0;
            pad.repeatWait = 0;
            if (!(made->flags & zge::kPadConnected))
                pad.flags = rider::kPadMissing;
        }
        *g_padsTaken = 1;
    }
    READABLE_FUNCTION(initPads, Verified);

    void freePads()
    {
        for (PadInput &pad : *g_pads)
        {
            if (pad.pad)
                zge::freePad(driverPad(pad));
        }
    }
    READABLE_FUNCTION(freePads, Verified);

    void clearPads()
    {
        for (PadInput &pad : *g_pads)
        {
            pad.buttons.held = 0;
            pad.buttons.changed = 0;
            pad.repeatWait = 0;
            clearSticks(pad);
        }
    }
    READABLE_FUNCTION(clearPads, Draft);

    void takePads(uint32_t taken)
    {
        if (taken == *g_padsTaken)
            return;
        if (*g_padsTaken == 0)
        {
            for (PadInput &pad : *g_pads)
            {
                pad.heldAside = pad.buttons.held;
                if (!(pad.flags & rider::kPadMissing) && connected(pad))
                    pad.ignored = driverPad(pad)->buttons;
                else
                    pad.ignored = 0;
            }
        }
        if (taken == 0)
        {
            for (PadInput &pad : *g_pads)
            {
                const uint32_t held = pad.heldAside;
                pad.buttons.held = held;
                if (!(pad.flags & rider::kPadMissing) && connected(pad))
                    pad.ignored = driverPad(pad)->buttons & ~held;
                else
                    pad.ignored = 0;
            }
        }
        *g_padsTaken = static_cast<uint8_t>(taken);
    }
    READABLE_FUNCTION(takePads, Verified);

    void rumble(uint32_t player, uint32_t motor, uint32_t level)
    {
        if (actionMode() != 0)
            return;
        if (!vibrationOn(player))
            return;
        if (motor == 0 && level != 0)
            level = kSmallMotorOn;
        const uint32_t wanted = level << 8;
        const ee::Ptr<zge::Pad> pad = driverPad(ee::Ptr<PadInput>(sym::g_pads)[player]);
        const ee::Ptr<uint32_t> motors(&pad->smallMotor);
        if (motors[motor] < wanted)
        {
            motors[motor] = wanted;
            game::tail(zge::padVibrate, pad, pad->smallMotor, pad->bigMotor, kRumbleTime, kRumbleTag);
        }
    }
    READABLE_FUNCTION(rumble, Verified);
    namespace
    {
        // PadInput::flags: the pad came back this frame.
        constexpr uint16_t kPadCameBack = 0x2;
        // readPads' result bits: a pad was lost, a pad came back.
        constexpr uint32_t kAPadLost = 0x1, kAPadCameBack = 0x2;

        constexpr ee::Float kStickPushed = 0.5f;
        constexpr uint32_t kActionModeOne = 1;

        void clearButtons(PadInput &pad)
        {
            pad.buttons.held = 0;
            pad.buttons.changed = 0;
            pad.ignored = 0;
            clearSticks(pad);
        }

        // The left stick counts as the d-pad on one axis: past half way it
        // is held (and changed if it was not last frame); let go, it has
        // changed by the direction it had.
        void stickAsButtons(rider::ButtonRecord &buttons, ee::Float now, ee::Float before, uint32_t negative, uint32_t positive)
        {
            if (kStickPushed < ee::abs(now))
            {
                buttons.held |= now < 0.0f ? negative : positive;
                if (ee::abs(before) <= kStickPushed)
                    buttons.changed |= now < 0.0f ? negative : positive;
            }
            else if (kStickPushed < ee::abs(before))
                buttons.changed |= before < 0.0f ? negative : positive;
        }
    }

    // readPads: each port's pad read into its button record, once a frame.
    // A missing pad stays cleared until one is connected (it "came back");
    // a pad that goes away is cleared and marked missing. A connected pad's
    // changed buttons are those that differ from the held ones, leaving out
    // the ignored ones (which stop being ignored once let go); the sticks
    // move to "previous" and are read anew. In the front end, and in the
    // level's action mode 1, the left stick also counts as the d-pad.
    // Returns kAPadLost and kAPadCameBack bits.
    uint32_t readPads()
    {
        uint32_t result = 0;
        for (PadInput &pad : *g_pads)
        {
            const uint16_t before = pad.flags;
            pad.flags = before & static_cast<uint16_t>(~kPadCameBack);
            if (before & rider::kPadMissing)
            {
                if (!connected(pad))
                    clearButtons(pad);
                else
                {
                    pad.repeatWait = 0;
                    pad.flags = static_cast<uint16_t>((before & ~(rider::kPadMissing | kPadCameBack)) | kPadCameBack);
                    result |= kAPadCameBack;
                }
                if (pad.flags & rider::kPadMissing)
                    continue;
            }
            if (!connected(pad))
            {
                result |= kAPadLost;
                pad.flags |= rider::kPadMissing;
                clearButtons(pad);
                continue;
            }
            const uint32_t down = driverPad(pad)->buttons;
            rider::ButtonRecord &buttons = pad.buttons;
            const uint32_t changed = (down ^ buttons.held) & ~pad.ignored;
            buttons.held ^= changed;
            pad.ignored &= down;
            buttons.previous[0] = buttons.sticks[0];
            buttons.previous[1] = buttons.sticks[1];
            buttons.changed = changed;
            buttons.sticks[0].x = driverPad(pad)->levels[0];
            buttons.sticks[0].y = driverPad(pad)->levels[1];
            buttons.sticks[1].x = driverPad(pad)->levels[2];
            buttons.sticks[1].y = driverPad(pad)->levels[3];
            if (gameState() != kGameStateFrontEnd)
            {
                if (gameState() != kGameStateLevel || actionMode() != kActionModeOne)
                    continue;
            }
            stickAsButtons(buttons, buttons.sticks[0].x, buttons.previous[0].x, rider::kButtonLeft, rider::kButtonRight);
            stickAsButtons(buttons, buttons.sticks[0].y, buttons.previous[0].y, rider::kButtonUp, rider::kButtonDown);
        }
        return result;
    }
    READABLE_FUNCTION(readPads, Verified);
}
