// The skater's controls: the pad read into action bits and input timers
// each frame, before the physics.

#include "skater.h"

#include "aggressive/objects.h"
#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/debug/debug.h"
#include "action_tricks.h"
#include "moves.h"
#include "trick_objects.h"
#include "zge/text/cstring.h"
#include "sym/aggressive/skater/controls.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // Game functions the controls update calls, in its order.
        constexpr uint32_t kTrickInput = sym::jumpButton;        // (player, actions*)
        // (player, move) -> the state entered or -1: the action button's
        // move (10) asked of the skater's state (states.cpp requestMove).
        constexpr uint32_t kRequestMove = sym::requestMove;
        constexpr uint32_t kObjectKind = sym::gameObjectKind;        // (object) -> its kind
        constexpr uint32_t kObjectState = sym::gameObjectState;       // (object) -> its state block
        constexpr uint32_t kObjectMessage = sym::sendObjectMessage;     // (object, message, 0, out*)
        constexpr uint32_t kControlsStep13 = sym::manualInput;    // (player)

        constexpr uint32_t kObjectKindPole = 8;
        constexpr uint32_t kObjectKindVehicle = 3; // (inferred: skitching)
        constexpr uint32_t kPadButton100 = 0x100;
        constexpr uint32_t kPadButton200 = 0x200;

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        // Tuning entries (aggressive/tuning.h; DEVTOOLS.md).
        ee::Float wallRideTimeout() { return tuning(14, 42); } // Grind/Walls / Wall Ride Timeout
        ee::Float stallTimeout() { return tuning(14, 18); }    // Grind/Walls / Stall Timeout
        ee::Float poleGrabTimeout() { return tuning(15, 12); } // Action / Pole Grab Timeout
        ee::Float bowlJumpOutTime() { return tuning(3, 39); }  // Physics / Bowl Jumpout Time

        bool isAirState(State state)
        {
            return static_cast<uint32_t>(state) - static_cast<uint32_t>(State::Air) < 3u; // Air, Air QPipe, Air Spine
        }
        bool isPoleState(State state)
        {
            return state >= State::VerticalPole && state <= State::HorizontalPole;
        }
    }

    int32_t padHeld(uint32_t buttons, uint32_t port)
    {
        return ((*g_pads)[port & 0xFFu].held & buttons) != 0u;
    }
    READABLE_FUNCTION(padHeld, Verified);

    int32_t padPressed(uint32_t buttons, uint32_t port)
    {
        return ((*g_pads)[port & 0xFFu].pressed & buttons) != 0u;
    }
    READABLE_FUNCTION(padPressed, Verified);

    void jumpButton(ee::Ptr<Player> player, ee::Ptr<uint32_t> actions);

    void manualInput(ee::Ptr<Player> player);

    int32_t skaterControlsUpdate(ee::Ptr<Player> player, ee::Float frameTime)
    {
        // The actions are built in the original's stack frame: a callee
        // (kTrickInput) adds to them through their address.
        ee::Local<uint32_t> actions;
        ee::Local<uint32_t> result;
        const ee::Ptr<Controls> controls = &player->controls;
        // Input windows the states read, each counting up to its limit.
        if (controls->wallRideTime < wallRideTimeout())
            controls->wallRideTime = controls->wallRideTime + frameTime;
        if (controls->stallTime < stallTimeout())
            controls->stallTime = controls->stallTime + frameTime;
        if (controls->poleGrabTime < poleGrabTimeout())
            controls->poleGrabTime = controls->poleGrabTime + frameTime;

        readStick(controls);
        ensureGroundObject(player);
        const uint32_t kept = controls->keptWord;
        *actions = 0;
        const auto held = [&](uint32_t buttons)
        {
            return padHeld(buttons, controls->port) != 0;
        };
        const auto pressed = [&](uint32_t buttons)
        {
            return padPressed(buttons, controls->port) != 0;
        };
        const auto layoutButton = [&](int i) { return (*g_buttonLayouts)[controls->layout].buttons[i]; };

        if (held(layoutButton(0)) || held(layoutButton(1)) || controls->stickY < 0.0f)
            *actions |= kActionPush;
        if (held(layoutButton(2)) || 0.0f < controls->stickY)
            *actions |= kActionBrake;
        if (held(layoutButton(3)))
            controls->releasedTime = 0.0f;
        else
            controls->releasedTime = ee::min(controls->releasedTime + frameTime, 100.0f);

        // Jumping out of a bowl: the stick held long enough.
        if (stickDirection(controls->stickX, controls->stickY) == 1)
        {
            controls->bowlJumpOutTime = controls->bowlJumpOutTime + frameTime;
            if (bowlJumpOutTime() < controls->bowlJumpOutTime)
                *actions |= kActionBowlJumpOut;
        }
        else
        {
            controls->bowlJumpOutTime = 0.0f;
            *actions &= ~kActionBowlJumpOut;
        }

        trickButtonsUpdate(player);
        jumpButton(player, actions);

        // Poles: grab one while the button is held; a press while on one
        // tells the pole.
        if (held(layoutButton(4)))
        {
            if (freeToAct(player))
            {
                *actions |= kActionPoleGrab;
                requestMove(player, kMoveAction);
            }
        }
        else
        {
            if (pressed(layoutButton(4)) && isPoleState(player->state))
            {
                const ee::Ptr<GameObject> pole = skaterCurrentObject(player);
                if (pole && static_cast<int32_t>(gameObjectKind(pole)) == kObjectKindPole)
                {
                    *result = 0;
                    sendObjectMessage(pole, *g_msgButtonReleased, 0, result);
                }
            }
        }

        twoButtonCommand(player);
        cessSlideButton(player, frameTime);

        if (pressed(kPadButton200) || pressed(kPadButton100))
        {
            // In the air (or on a moving object that is not yet going).
            bool airborne = false;
            const ee::Ptr<GameObject> object = skaterCurrentObject(player);
            if (object)
            {
                if (static_cast<int32_t>(gameObjectKind(object)) == kObjectKindVehicle)
                {
                    const ee::Ptr<uint32_t> state(gameObjectState(object));
                    airborne = state[1] < 2u && state[0] == 0u;
                }
            }
            else
                airborne = isAirState(player->state);
            if (airborne)
                selectStateObject(player);
        }
        if (held(kPadButton100))
            *actions |= kActionButton100;
        if (held(kPadButton200))
            *actions |= kActionButton200;

        updateCurrentObject(player, frameTime);
        controls->keptWord = kept;
        controls->actions = *actions;
        updateHistory(controls, frameTime);
        trickRelease(player, frameTime);
        manualInput(player);

        // Toggle Debug: the input history along the screen.
        if (*g_debugDisplay & 3u)
        {
            // (The counter is a signed char, as in the game.)
            if (controls->historyCount)
                for (int8_t i = 0;;)
                {
                    zge::debugPrint(i * 10, 0x96, sym::g_textD, controls->history[i]);
                    i = static_cast<int8_t>(i + 1);
                    if (!(i < static_cast<int32_t>(controls->historyCount)))
                        break;
                }
        }
        return 1;
    }
    READABLE_FUNCTION(skaterControlsUpdate, Verified);

    namespace
    {
        constexpr uint32_t kObjectKindRideable = 1;       // (inferred) an object the jump button puts in its state 2
        constexpr uint32_t kObjectKindNoJump = 9;         // standing on one, the jump button does nothing
        constexpr uint32_t kActionJumpHeld = 0x4;
        constexpr uint32_t kJumpRearmed = 0x2;            // Controls::stickFlags: the jump button let go since the last wall plant
    }

    // jumpButton (player, actions *): the jump button (the layout's button
    // 3). Held with the stick down (the newest history entry) past Grind/Walls
    // / Wall Ride Timeout, once per press: a wall plant (kActionWallRide, the
    // timer back to 0); otherwise the press re-arms it. When the button
    // changes (padPressed): nothing on an object of kind 9; let go, a jump
    // (kActionJump) with the newest direction kept at +0x6D and the history
    // emptied; held, kActionJumpHeld, and an object of kind 1 not yet in its
    // state 2 is put there (2, 1) unless the stick points down.
    void jumpButton(ee::Ptr<Player> player, ee::Ptr<uint32_t> actions)
    {
        const ee::Ptr<Controls> controls = &player->controls;
        const auto button = [&] { return (*g_buttonLayouts)[controls->layout].buttons[3]; };
        const ee::Ptr<GameObject> object = skaterCurrentObject(player);
        bool rearm = true;
        if (padHeld(button(), controls->port))
        {
            if (controls->history[0] == kDirDown)
            {
                rearm = false;
                if (wallRideTimeout() < controls->wallRideTime && (controls->stickFlags & kJumpRearmed))
                {
                    *actions = *actions | kActionWallRide;
                    controls->wallRideTime = 0.0f;
                }
            }
        }
        if (rearm)
            controls->stickFlags = controls->stickFlags | kJumpRearmed;
        if (!padPressed(button(), controls->port))
            return;
        if (object && gameObjectKind(ee::Ptr<GameObject>(object)) == kObjectKindNoJump)
            return;
        if (!padHeld(button(), controls->port))
        {
            const uint32_t before = *actions;
            *actions = before | kActionJump;
            const uint8_t last = controls->historyCount ? controls->history[0] : 0;
            controls->lastDirection = last;
            controls->historyCount = 0;
            zge::memoryFill(controls->history, 0, 8);
        }
        else
        {
            if (object && gameObjectKind(ee::Ptr<GameObject>(object)) == kObjectKindRideable)
            {
                const ee::Ptr<uint32_t> state(gameObjectState(ee::Ptr<GameObject>(object)));
                if (*state != 2)
                {
                    if (stickDirection(controls->stickX, controls->stickY) != kDirDown)
                        setObjectState(player, object, 2, 1);
                }
            }
            *actions = *actions | kActionJumpHeld;
        }
    }
    READABLE_FUNCTION(jumpButton, Verified);

    namespace
    {
        constexpr uint32_t kObjectKindNoManual = 4;
        constexpr uint32_t kManualStick = 0x10;          // Controls::stickFlags: the stick asks for a manual
        ee::Float groundManualTimeout() { return tuning(5, 8); } // Control / Ground Manual Timeout
    }

    // manualInput (player): a manual from the ground. On the Ground state,
    // not down, the current object (if any; not one of kind 4) agreeing to be
    // cut (its answer to the interrupt message), a tap of pad button 1 or 2
    // (pressed, not held) or the stick's manual flag: the manual the recent
    // input makes within Control / Ground Manual Timeout (matchManualTrick)
    // starts, the history emptied first.
    void manualInput(ee::Ptr<Player> player)
    {
        ee::Local<uint32_t> result;
        const ee::Ptr<GameObject> object = skaterCurrentObject(player);
        if (player->state != State::Ground)
            return;
        if (player->flags & kDown)
            return;
        bool wanted = true;
        if (object)
        {
            if (gameObjectKind(ee::Ptr<GameObject>(object)) == kObjectKindNoManual)
                wanted = false;
            else
            {
                *result = 0;
                sendObjectMessage(ee::Ptr<GameObject>(object), *g_msgAreYouFinished, player, result);
                wanted = *result != 0;
            }
        }
        if (wanted)
        {
            bool tapped = padPressed(1, 0) && !padHeld(1, 0);
            if (!tapped)
            {
                tapped = padPressed(2, 0) && !padHeld(2, 0);
                if (!tapped)
                    tapped = (player->controls.stickFlags & kManualStick) != 0;
            }
            if (tapped)
            {
                if (const ee::Ptr<GameObject> trick = matchManualTrick(player, groundManualTimeout()))
                {
                    player->controls.historyCount = 0;
                    zge::memoryFill(player->controls.history, 0, 8);
                    startTrick(player, trick);
                }
            }
        }
    }
    READABLE_FUNCTION_WAITS(manualInput, Verified);
}
