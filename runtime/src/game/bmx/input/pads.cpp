// BMX XXX's pads as the game keeps them (pads.h). BMX XXX's own code
// (camera.c's block, with the rider's input helpers), DM2's line
// (game/dm2/input/pads.cpp): what BMX XXX adds is the four face buttons'
// levels, read in the order the player's controller configuration gives.

#include "pads.h"

#include "bmx/action/camera_state.h"
#include "bmx/game/player_riders.h"
#include "bmx/rider/rider.h"
#include "bmx/states/game_state.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/input/pads.h"

#include <cstdint>
#include "sym/bmx/input/pads.cpp.h"

namespace game::bmx
{
    using rider::PadInput;

    namespace
    {
        constexpr uint32_t kPorts = 2;
        inline ee::Global<PadInput[kPorts], sym::g_pads> g_pads;
        // Not 0 while something other than the play has the pads.
        inline ee::Global<uint8_t, sym::g_padsTaken> g_padsTaken;

        // For each controller configuration, the pad levels PadInput's four
        // face levels are read from: cross, square, circle, triangle for the
        // first; cross, circle, square, triangle; cross, square, triangle,
        // circle.
        struct FaceLevelOrder
        {
            uint8_t levels[rider::kPadFaceLevels];
        };
        const ee::Ptr<FaceLevelOrder> kFaceLevelOrders(sym::g_faceLevelOrders);

        // PadInput::flags: the pad came back this frame.
        constexpr uint16_t kPadCameBack = 0x2;
        // readPads' result bits: a pad was lost, a pad came back.
        constexpr uint32_t kAPadLost = 0x1, kAPadCameBack = 0x2;

        constexpr ee::Float kStickPushed = 0.5f;

        // The left stick also counts as the d-pad in the front end
        // (kGameStateFrontEnd) and in a level showing a game screen.

        bool connected(const PadInput &pad) { return (pad.pad->flags & zge::kPadConnected) != 0; }

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

    void initPads()
    {
        for (PadInput &pad : *g_pads)
        {
            const ee::Ptr<zge::Pad> made = zge::newPad(0);
            pad.flags = 0;
            pad.pad = made;
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
                zge::freePad(pad.pad);
        }
    }
    READABLE_FUNCTION(freePads, Verified);

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
                    pad.ignored = pad.pad->buttons;
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
                    pad.ignored = pad.pad->buttons & ~held;
                else
                    pad.ignored = 0;
            }
        }
        *g_padsTaken = static_cast<uint8_t>(taken);
    }
    READABLE_FUNCTION(takePads, Verified);

    // clearPad: a port's record emptied (buttons, sticks, face levels, now
    // and a frame ago), and its pad's rumble stopped if one is connected.
    // [name inferred: DM2's clearPads does this to every port]
    void clearPad(ee::Ptr<PadInput> pad)
    {
        pad->buttons.held = 0;
        pad->buttons.changed = 0;
        pad->repeatWait = 0;
        for (uint32_t stick = 0; stick < 2; ++stick)
        {
            pad->buttons.sticks[stick].x = 0.0f;
            pad->buttons.sticks[stick].y = 0.0f;
            pad->buttons.previous[stick].x = 0.0f;
            pad->buttons.previous[stick].y = 0.0f;
        }
        for (uint32_t level = 0; level < rider::kPadFaceLevels; ++level)
        {
            pad->faceLevels[level] = 0.0f;
            pad->previousFaceLevels[level] = 0.0f;
        }
        if (connected(*pad))
            game::tail(zge::padVibrate, pad->pad, 0, 0, 0, 0);
    }
    READABLE_FUNCTION(clearPad, Verified);

    // readPads: each port's pad read into its record, once a frame. A
    // missing pad stays cleared until one is connected (it "came back"); a
    // pad that goes away is cleared and marked missing. A connected pad's
    // changed buttons are those that differ from the held ones, leaving out
    // the ignored ones (which stop being ignored once let go); the sticks
    // and the face levels move to "previous" and are read anew, the face
    // levels in the order of the port's player's controller configuration.
    // In game state 0, and in a level showing a game screen, the left stick
    // also counts as the d-pad. Returns kAPadLost and kAPadCameBack bits.
    uint32_t readPads()
    {
        uint32_t result = 0;
        for (uint32_t port = 0; port < kPorts; ++port)
        {
            const ee::Ptr<PadInput> pad(&(*g_pads)[port]);
            const uint16_t before = pad->flags;
            pad->flags = before & static_cast<uint16_t>(~kPadCameBack);
            if (before & rider::kPadMissing)
            {
                if (!connected(*pad))
                    clearPad(pad);
                else
                {
                    pad->repeatWait = 0;
                    pad->flags = static_cast<uint16_t>((before & ~(rider::kPadMissing | kPadCameBack)) | kPadCameBack);
                    result |= kAPadCameBack;
                }
                if (pad->flags & rider::kPadMissing)
                    continue;
            }
            if (!connected(*pad))
            {
                result |= kAPadLost;
                pad->flags |= rider::kPadMissing;
                clearPad(pad);
                continue;
            }
            const ee::Ptr<zge::Pad> driver = pad->pad;
            const uint32_t down = driver->buttons;
            rider::ButtonRecord &buttons = pad->buttons;
            const uint32_t changed = (down ^ buttons.held) & ~pad->ignored;
            buttons.changed = changed;
            buttons.held ^= changed;
            pad->ignored &= down;
            buttons.previous[0] = buttons.sticks[0];
            buttons.previous[1] = buttons.sticks[1];
            buttons.sticks[0].x = driver->levels[zge::kLevelLeftX];
            buttons.sticks[0].y = driver->levels[zge::kLevelLeftY];
            buttons.sticks[1].x = driver->levels[zge::kLevelRightX];
            buttons.sticks[1].y = driver->levels[zge::kLevelRightY];
            const FaceLevelOrder &order = kFaceLevelOrders[playerSetup(port)->controllerLayout];
            for (uint32_t level = 0; level < rider::kPadFaceLevels; ++level)
            {
                pad->previousFaceLevels[level] = pad->faceLevels[level];
                pad->faceLevels[level] = pad->pad->levels[order.levels[level]];
            }
            if (gameState() != kGameStateFrontEnd)
            {
                if (gameState() != kGameStateLevel || cameraState() != kCameraStateScreen)
                    continue;
            }
            stickAsButtons(buttons, buttons.sticks[0].x, buttons.previous[0].x, rider::kButtonLeft, rider::kButtonRight);
            stickAsButtons(buttons, buttons.sticks[0].y, buttons.previous[0].y, rider::kButtonUp, rider::kButtonDown);
        }
        return result;
    }
    READABLE_FUNCTION(readPads, Verified);

    int32_t anyButtonPressed()
    {
        for (uint32_t port = 0; port < kPorts; ++port)
        {
            const rider::ButtonRecord &buttons = rider::padInput(port)->buttons;
            if ((buttons.held & buttons.changed & ~rider::kButtonDirections) != 0)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(anyButtonPressed, Verified);
}
