// BMX XXX's rider input: the pads' button records, the move table's tests
// and the stick's direction (the input helpers, and the tap helpers of
// physics.c's trick code). DM2's (game/dm2/rider/input.cpp), over BMX XXX's
// records: a pad is 0x58 bytes, a layout 25 moves, and the tap flags are
// bits of a byte.

#include "rider.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "sym/bmx/rider/input.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The move table: for each controller configuration, 25 moves of
        // three button tests.
        inline ee::Global<MoveLayout[3], sym::g_moveLayouts> g_moveLayouts;
        // The ports' pads.
        inline ee::Global<PadInput[2], sym::g_pads> g_pads;
        // inputStick's result (one for every caller: the stick, the d-pad on
        // the left one).
        inline ee::Global<Stick, sym::g_stick> g_stick;
        // A direction by its button bits (kButtonUp .. kButtonRight).
        inline ee::Global<uint8_t[16], sym::g_directionByButtons> g_directionByButtons;

        constexpr ee::Float kStickPushed = 0.5f;

        // The direction bits of a stick past half way, added to `bits`.
        uint32_t stickBits(Stick stick, uint32_t bits)
        {
            if (kStickPushed < ee::abs(stick.x))
                bits |= stick.x < 0.0f ? kButtonLeft : kButtonRight;
            if (kStickPushed < ee::abs(stick.y))
                bits |= stick.y < 0.0f ? kButtonUp : kButtonDown;
            return bits;
        }
    }

    // Whether `buttons` pass the test `mode` (ButtonMode) in a button record.
    // The left stick counts as the d-pad: past half way it is held, and it
    // has changed when it went past half way or back since the last frame.
    int32_t buttonTest(const ButtonRecord *record, uint32_t buttons, uint32_t mode)
    {
        const ee::Float x = record->sticks[0].x;
        uint32_t held = record->held;
        uint32_t changed = record->changed;
        // x: pushed now (held; changed if it was not), or let go (changed by
        // the direction it had).
        if (kStickPushed < ee::abs(x))
        {
            held |= x < 0.0f ? kButtonLeft : kButtonRight;
            if (ee::abs(record->previous[0].x) <= kStickPushed)
            {
                changed |= x < 0.0f ? kButtonLeft : kButtonRight;
            }
        }
        else
        {
            const ee::Float before = record->previous[0].x;
            if (kStickPushed < ee::abs(before))
            {
                changed |= before < 0.0f ? kButtonLeft : kButtonRight;
            }
        }
        const ee::Float y = record->sticks[0].y;
        if (kStickPushed < ee::abs(y))
        {
            held |= y < 0.0f ? kButtonUp : kButtonDown;
            if (ee::abs(record->previous[0].y) <= kStickPushed)
            {
                changed |= y < 0.0f ? kButtonUp : kButtonDown;
            }
        }
        else
        {
            const ee::Float before = record->previous[0].y;
            if (kStickPushed < ee::abs(before))
            {
                changed |= before < 0.0f ? kButtonUp : kButtonDown;
            }
        }

        switch (mode)
        {
        case kAnyPressed:
            return (held & changed & buttons) != 0u;
        case kAllDownOnePressed:
            return (held & buttons) == buttons && (changed & buttons) != 0u;
        case kAllDown:
            return (held & buttons) == buttons;
        case kAnyDown:
            return (held & buttons) != 0u;
        case kAllUpOneReleased:
            return (~held & buttons) == buttons && (changed & buttons) != 0u;
        case kAnyReleased:
            return (~held & changed & buttons) != 0u;
        case kAlways:
            return 1;
        case kAlways + 1:
            return 0;
        default:
            return 0;
        }
    }
    READABLE_FUNCTION(buttonTest, Verified);

    // A port's pad (its block of the pad records).
    PadInput *padInput(uint32_t port)
    {
        return &(*g_pads)[port];
    }
    READABLE_FUNCTION(padInput, Verified);

    // buttonTest on a port's pad.
    int32_t padButtons(uint32_t port, uint32_t buttons, uint32_t mode)
    {
        return buttonTest(&(*g_pads)[port].buttons, buttons, mode);
    }
    READABLE_FUNCTION(padButtons, Verified);

    // A move's test `which` (0 .. 2) in the move table's controller
    // configuration `layout`, on a button record (the rider's).
    int32_t moveButtons(uint32_t layout, const ButtonRecord *record, uint32_t move, uint32_t which)
    {
        const MoveTest &test = (*g_moveLayouts)[layout].moves[move].tests[which];
        return buttonTest(record, test.buttons, test.mode);
    }
    READABLE_FUNCTION(moveButtons, Verified);

    // A stick of a button record (0 left, 1 right) into the shared result;
    // the left one is the d-pad's (+-1) where a direction is held.
    Stick *inputStick(const ButtonRecord *record, uint32_t which)
    {
        Stick &stick = *g_stick;
        stick = record->sticks[which];
        if (which == 0)
        {
            if (record->held & kButtonLeft)
                stick.x = -1.0f;
            else if (record->held & kButtonRight)
                stick.x = 1.0f;
            if (record->held & kButtonUp)
                stick.y = -1.0f;
            else if (record->held & kButtonDown)
                stick.y = 1.0f;
        }
        return &stick;
    }
    READABLE_FUNCTION(inputStick, Verified);

    // The direction the rider holds: the d-pad and the left stick past half
    // way.
    uint8_t stickDirection(const Rider *rider)
    {
        return (*g_directionByButtons)[stickBits(rider->input.sticks[0], rider->input.held) & kButtonDirections];
    }
    READABLE_FUNCTION(stickDirection, Verified);

    // The direction taps cleared. With `keepStick`, taps go on: the
    // direction held now is the first tap, unless taps were on already.
    void clearTaps(Rider *rider, uint32_t keepStick)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            rider->taps[i] = 0;
        }
        rider->tapCount = 0;
        if (keepStick)
        {
            if (!hasFlag(rider, kFlagTapsOn))
            {
                const uint32_t bits = stickBits(rider->input.sticks[0], rider->input.held);
                const uint32_t direction = bits & kButtonDirections;
                if (direction)
                    rider->taps[rider->tapCount++] = (*g_directionByButtons)[direction];
            }
            setFlag(rider, kFlagTapsOn);
        }
        else
        {
            clearFlag(rider, kFlagTapsOn);
        }
        clearFlag(rider, kFlagTrickAWaiting);
        clearFlag(rider, kFlagTrickBWaiting);
    }
    READABLE_FUNCTION(clearTaps, Verified);
}
