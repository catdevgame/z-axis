// The steps of the skater's controls update (skaterControlsUpdate,
// controls.cpp) that read the stick and keep the input history (readable
// round gamehand): the stick (or the d-pad) as stickX / stickY, the stick's
// eight directions and their history (what the tricks' patterns match,
// tricks.cpp), the two-button command and the Cess Slide button's window.
// Tuning entries by their names (aggressive/tuning.h).

#include "aggressive/objects.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"

#include "ee/readable.h"
#include "ee/plain.h"
#include "action_tricks.h"
#include "trick_objects.h"
#include "zge/text/cstring.h"
#include "aggressive/pads.h"
#include "sym/aggressive/skater/control_steps.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kGroundSlot = 0x8A4;      // Player: the ground object's slot
        constexpr uint32_t kTrickKeys = 0x5B8;       // Player: a byte the command clears with the history (inferred)
        constexpr uint32_t kObjectKindCessSlide = 9;

        // The game's pad bits (PadState).
        constexpr uint32_t kPadUp = 0x1;
        constexpr uint32_t kPadDown = 0x2;
        constexpr uint32_t kPadLeft = 0x4;
        constexpr uint32_t kPadRight = 0x8;
        constexpr uint32_t kPad100 = 0x100;
        constexpr uint32_t kPad200 = 0x200;
        constexpr uint32_t kPadCessSlide = 0x800;

        constexpr uint32_t kHistorySize = 8;
        // A diagonal entered within this many seconds of a straight
        // direction next to it is merged with it.
        inline ee::Global<ee::Float, sym::g_diagonalMergeTime> g_diagonalMergeTime; // 0.05

        ee::Float control(uint32_t entry)
        {
            return (*g_tuningCategories)[5].entries[entry].value; // Control / ...
        }
        ee::Float deadzone() { return control(0); }             // Controller Deadzone
        ee::Float commandDelayFrames() { return control(3); }   // # cmd delay frames
        ee::Float buttonTimeout() { return control(4); }        // Button Timeout
        ee::Float buttonReviveTimeout() { return control(5); }  // Button Revive Timeout
        ee::Float cessSlideTimeout() { return control(6); }     // Cessslide Button Timeout

        Controls &controlsOf(ee::Ptr<Player> player)
        {
            return player->controls;
        }
    }

    // stickDirection: the stick's direction (kDirNone, kDirUp .. kDirUpLeft) past half
    // way on either axis (y < 0 is up).
    uint32_t stickDirection(ee::Float x, ee::Float y)
    {
        const ee::Float half(0.5f);
        const ee::Float minusHalf(-0.5f);
        // (The middle case of each column returns from the block it is in.)
        const auto pick = [&](uint32_t down, uint32_t up, uint32_t level) {
            if (half < y)
                return down;
            if (y < minusHalf)
                return up;
            return level;
        };
        if (half < x)
            return pick(kDirDownRight, kDirUpRight, kDirRight);
        if (x < minusHalf)
            return pick(kDirDownLeft, kDirUpLeft, kDirLeft);
        return pick(kDirDown, kDirUp, kDirNone);
    }
    READABLE_FUNCTION(stickDirection, Verified);

    // readStick: the stick into stickX / stickY: an axis inside the
    // dead zone is 0; at exactly 0 the d-pad gives -1 or 1. stickFlags says
    // which came from the stick.
    void readStick(ee::Ptr<Controls> controls)
    {
        controls->stickFlags &= ~(kStickAnalog | kStickAnalogX | kStickAnalogY);
        const ee::Float x = aggressive::padAxis(0, controls->port);
        controls->stickX = x;
        if (x == ee::Float(0.0f))
        {
            if (padHeld(kPadLeft, controls->port))
                controls->stickX = -1.0f;
            else
            {
                if (padHeld(kPadRight, controls->port))
                    controls->stickX = 1.0f;
            }
        }
        else
        {
            if (ee::abs(x) < deadzone())
                controls->stickX = 0.0f;
            else
                controls->stickFlags |= kStickAnalog | kStickAnalogX;
        }

        const ee::Float y = aggressive::padAxis(1, controls->port);
        controls->stickY = y;
        if (y == ee::Float(0.0f))
        {
            if (!padHeld(kPadUp, controls->port))
            {
                if (padHeld(kPadDown, controls->port))
                    controls->stickY = 1.0f;
                return;
            }
            controls->stickY = -1.0f;
        }
        else
        {
            if (ee::abs(y) < deadzone())
                controls->stickY = 0.0f;
            else
                controls->stickFlags |= kStickAnalog | kStickAnalogY;
        }
    }
    READABLE_FUNCTION(readStick, Verified);

    // ensureGroundObject: on the ground (Ground, Stand) and not down, the skater
    // always has a current action object: the state's slot's, or a new one
    // put in the ground slot.
    void ensureGroundObject(ee::Ptr<Player> player)
    {
        if (player)
        {
            if (static_cast<uint8_t>(player->state) < 2u)
            {
                if (player->flags & kDown)
                    return;
                if (!skaterCurrentObject(player))
                {
                    selectStateObject(player);
                    if (!skaterCurrentObject(player))
                    {
                        const ee::Ptr<GameObject> object = newGroundObject();
                        putObjectInSlot(player->objectSlots[3], object);
                        selectStateObject(player);
                        game::tail(aggressive::skater::skaterCurrentObject, player);
                        return;
                    }
                }
            }
        }
    }
    READABLE_FUNCTION_WAITS(ensureGroundObject, Verified);

    // updateHistory: the history of the stick's directions, each frame:
    // entries age (those past Control / Button Timeout dropped), the
    // newest's hold time grows; a new direction is entered at the front
    // (8 at most). A diagonal right after a straight direction next to it
    // (within g_diagonalMergeTime) is merged: a diagonal newest entry
    // stays (the new direction is taken as it), a straight one (the odd
    // directions) is replaced by the new direction. A new direction entered while the newest is younger than
    // Button Revive Timeout also clears the entries made in the same
    // instant. While holdHistory is set only the change flag is kept.
    void updateHistory(ee::Ptr<Controls> controls, ee::Float frameTime)
    {
        Controls &c = *controls;
        uint32_t direction = stickDirection(c.stickX, c.stickY);

        if (c.historyCount != 0)
        {
            for (int8_t i = 0;;)
            {
                bool done = true;
                const ee::Float age = c.historyAge[i];
                if (buttonTimeout() < age)
                    c.historyCount = static_cast<uint8_t>(i);
                else
                {
                    c.historyAge[i] = age + frameTime;
                    c.historyHeld[i] = c.historyHeld[i] + frameTime;
                    i = static_cast<int8_t>(i + 1);
                    done = !(i < static_cast<int32_t>(c.historyCount));
                }
                if (done)
                    break;
            }
        }

        if (c.holdHistory)
        {
            if (!c.historyChanged)
                c.historyChanged = direction != c.history[0];
            return;
        }
        if (direction != kDirNone && c.historyCount != 0)
        {
            if (c.historyAge[0] < *g_diagonalMergeTime)
            {
                const int32_t newest = c.history[0];
                const int32_t to = static_cast<int32_t>(direction & 7u);
                const bool adjacent = (newest + 1) % 8 == to || (newest - 1) % 8 == to;
                if (adjacent)
                {
                    if ((newest & 1) == 0)
                        direction = static_cast<uint32_t>(newest);
                    else
                        c.history[0] = static_cast<uint8_t>(direction);
                }
            }
        }
        if (!c.historyChanged)
            c.historyChanged = direction != c.history[0];
        if (direction == kDirNone)
            return;

        if (c.historyChanged)
        {
            ee::Float newestAge = c.historyAge[0];
            if (newestAge < buttonReviveTimeout())
            {
                // The entries made in the same instant as the newest go
                // with it (their age set to 0).
                if (static_cast<int32_t>(c.historyCount) >= 2)
                {
                    for (int8_t i = 1;;)
                    {
                        bool done = true;
                        if (c.historyAge[i] == newestAge)
                        {
                            c.historyAge[i] = 0.0f;
                            i = static_cast<int8_t>(i + 1);
                            done = !(i < static_cast<int32_t>(c.historyCount));
                        }
                        if (done)
                            break;
                        newestAge = c.historyAge[0];
                    }
                }
                c.historyAge[0] = 0.0f;
            }
            const uint32_t count = c.historyCount;
            // Make room at the front (the oldest falls off a full history).
            int8_t i = static_cast<int8_t>(count >= kHistorySize ? count - 1 : count);
            if (i > 0)
            {
                for (;;)
                {
                    const int8_t from = static_cast<int8_t>(i - 1);
                    c.history[i] = c.history[from];
                    c.historyAge[i] = c.historyAge[from];
                    c.historyHeld[i] = c.historyHeld[from];
                    i = from;
                    if (!(i > 0))
                        break;
                }
            }
            c.history[0] = static_cast<uint8_t>(direction);
            c.historyAge[0] = 0.0f;
            c.historyHeld[0] = 0.0f;
            if (c.historyCount < kHistorySize)
                c.historyCount = static_cast<uint8_t>(c.historyCount + 1);
        }
        c.historyChanged = 0;
    }
    READABLE_FUNCTION(updateHistory, Verified);

    // twoButtonCommand: the two-button command: pad bits 0x100 and 0x200 each
    // pressed within Control / # cmd delay frames of the other. On the
    // ground it puts the current object in state (4, 0) and clears the
    // history. The windows count down a frame each update.
    void twoButtonCommand(ee::Ptr<Player> player)
    {
        Controls &c = controlsOf(player);
        const bool held100 = padHeld(kPad100, c.port) != 0;
        const bool pressed100 = padPressed(kPad100, c.port) != 0;
        const bool held200 = padHeld(kPad200, c.port) != 0;
        const bool pressed200 = padPressed(kPad200, c.port) != 0;
        if (player->flags & kDown)
            return;
        if (pressed100 && held100)
            c.window100 = static_cast<int8_t>(ee::toInt(commandDelayFrames()));
        if (pressed200)
        {
            if (held200)
                c.window200 = static_cast<int8_t>(ee::toInt(commandDelayFrames()));
        }
        if (c.window100 >= 0)
        {
            bool onGround = true;
            if (c.window200 >= 0)
            {
                onGround = player->state == State::Ground;
                if (onGround)
                {
                    if (!skaterCurrentObject(player))
                        selectStateObject(player);
                    setObjectState(player, 0, 4, 0);
                    c.window200 = -1;
                    c.window100 = -1;
                    player->controls.historyCount = 0;
                    zge::memoryFill(player->controls.history, 0, kHistorySize);
                }
            }
            if (c.window100 >= 0)
                c.window100 = static_cast<int8_t>(c.window100 - 1);
        }
        if (c.window200 >= 0)
            c.window200 = static_cast<int8_t>(c.window200 - 1);
    }
    READABLE_FUNCTION_WAITS(twoButtonCommand, Verified);

    // cessSlideButton: the Cess Slide button (pad bit 0x800): a press opens a
    // window of Control / Cessslide Button Timeout; on the ground within
    // it a Cess Slide starts (its window closed then, the object let go if
    // it did not start). Not while one is already the current object.
    void cessSlideButton(ee::Ptr<Player> player, ee::Float frameTime)
    {
        Controls &c = controlsOf(player);
        bool closeWindow = false;
        ee::Ptr<GameObject> slide;
        if (const ee::Ptr<GameObject> current = skaterCurrentObject(player);
            current && gameObjectKind(ee::Ptr<aggressive::GameObject>(current)) == kObjectKindCessSlide)
            closeWindow = true;
        else
        {
            bool opened = false;
            if (padHeld(kPadCessSlide, c.port))
                opened = padPressed(kPadCessSlide, c.port) != 0;
            if (opened)
                c.cessSlideTime = cessSlideTimeout();
            else
            {
                if (ee::Float(0.0f) < c.cessSlideTime)
                    c.cessSlideTime = c.cessSlideTime - frameTime;
                else
                    c.cessSlideTime = -1.0f;
            }
            if (player->state != State::Ground)
                return;
            const bool open = ee::Float(0.0f) < c.cessSlideTime;
            if (!open)
                return;
            slide = newCessSlide();
            closeWindow = startTrick(player, slide) != 0;
        }
        if (closeWindow)
            c.cessSlideTime = -1.0f;
        else
            releaseGameObject(slide);
    }
    READABLE_FUNCTION_WAITS(cessSlideButton, Verified);

    namespace
    {
        constexpr uint32_t kActionSlot = 0x898;            // Player: the action object's slot
        constexpr uint32_t kHistoryBytes = 0x570;          // Player: Controls::history (8 bytes)
        constexpr uint32_t kReleaseTimer = 0x550 + 0x70;   // Player: Controls +0x70, seconds the release button counts as down (-1: not)
        constexpr uint32_t kReleaseButton = 6;             // the layout's button
    }

    // trickRelease: the release button (the layout's button 6) in the air:
    // a press (or a press within the timer's second) makes the air trick
    // the recent directions match, put in the action slot (the history
    // cleared); without one the timer runs down from 1 s. Let go in the air, an action object that is a
    // named grab (airTrickIsGrab) is followed by a new unnamed grab (made
    // the state's object; air_tricks.cpp's newUnnamedGrab). A press let go
    // in the same frame clears the history.
    void trickRelease(ee::Ptr<Player> player, ee::Float frameTime)
    {
        const Controls &c = player->controls;
        ee::Float &timer = player->controls.releaseTimer;
        const auto button = [&] { return (*g_buttonLayouts)[c.layout].buttons[kReleaseButton]; };
        const auto inAir = [&] { return static_cast<uint32_t>(static_cast<uint8_t>(player->state)) - 2u < 3u; };
        const auto clearHistory = [&] {
            player->controls.historyCount = 0;
            zge::memoryFill(player->controls.history, 0, 8);
        };

        bool down = ee::Float(0.0f) <= timer;
        // (padHeld is read whenever the button is pressed, whatever the timer
        // says: the original's `or` is not a short circuit.)
        if (padPressed(button(), c.port))
            down = (padHeld(button(), c.port) != 0) || down;
        bool letGo = false;
        if (down)
        {
            enum
            {
                Stop,  // the timer to -1
                Keep,  // left as it is
                Reset, // to -1 (the air trick made)
            } timerTo = Stop;
            if (inAir())
            {
                const ee::Ptr<GameObject> airTrick = matchAirTrick(player);
                if (airTrick)
                {
                    clearHistory();
                    putObjectInSlot(player->objectSlots[0], airTrick);
                    selectStateObject(player);
                    letGo = padHeld(button(), c.port) == 0;
                    timerTo = Reset;
                }
                else
                {
                    const ee::Float left = timer;
                    if (left < ee::Float(0.0f))
                    {
                        timer = 1.0f;
                        timerTo = Keep;
                    }
                    else
                    {
                        const ee::Float now = left - frameTime;
                        timer = now;
                        timerTo = now < ee::Float(0.0f) ? Stop : Keep;
                    }
                }
            }
            if (timerTo != Keep)
                timer = -1.0f;
        }
        if (padPressed(button(), c.port))
            letGo = (padHeld(button(), c.port) == 0) || letGo;
        const bool release = letGo && inAir();
        if (release)
        {
            ee::Ptr<GameObject> object = objectInSlot(player->objectSlots[0]);
            if (!object)
                object = skaterCurrentObject(player);
            if (object)
            {
                if (airTrickIsGrab(object))
                {
                    const ee::Ptr<GameObject> grab = newUnnamedGrab();
                    if (grab)
                    {
                        putObjectInSlot(player->objectSlots[0], grab);
                        selectStateObject(player);
                    }
                }
            }
        }
        if (padPressed(button(), c.port) && !padHeld(button(), c.port))
            clearHistory();
    }
    READABLE_FUNCTION_WAITS(trickRelease, Verified);
}
