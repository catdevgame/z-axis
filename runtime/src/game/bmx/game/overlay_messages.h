#pragma once
// The overlay's messages as BMX XXX's game code sends them (game.c): a value
// for a player's display, an item for a player's reward display, the trick
// display's and the balance display's.

#include "bmx/rider/rider.h"
#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A value for a player's display: the health's.
    struct ValueMessage
    {
        uint8_t player;  // +0x00
        uint8_t flags;   // +0x01 0: the display moves towards the value
        uint8_t _02[2];
        ee::Float value; // +0x04
    };
    static_assert(offsetof(ValueMessage, value) == 0x04 && sizeof(ValueMessage) == 0x08);
    // An item for a player's reward display: its kind and its value.
    struct RewardItemMessage
    {
        uint8_t player;  // +0x00
        uint8_t kind;    // +0x01
        uint8_t _02[2];
        ee::Float value; // +0x04
    };
    static_assert(offsetof(RewardItemMessage, value) == 0x04 && sizeof(RewardItemMessage) == 0x08);
    // The reward display's item for a bail as it starts, its value the
    // bail's kind (DM2's kRewardFall).
    constexpr uint8_t kRewardItemBail = 0x0B;

    // What the trick display (kOverlayTrick) takes: the player, what to do,
    // and the score block whose lines it shows (Rider::score).
    struct TrickDisplayMessage
    {
        uint8_t player;  // +0x00
        uint8_t action;  // +0x01 TrickDisplayAction
        uint8_t _02[2];
        ee::Ptr<rider::ScoreBlock> score; // +0x04
    };
    static_assert(offsetof(TrickDisplayMessage, score) == 0x04 && sizeof(TrickDisplayMessage) == 0x08);
    // The trick display's actions (its handler's; DM2's).
    enum TrickDisplayAction : uint8_t
    {
        kTrickDisplayClear = 0,       // the display starts over ("Init")
        kTrickDisplayTrickBegun = 1,  // a trick of the current line began: the next name label fades in
        kTrickDisplayTrickText = 2,   // the line's text and points written
        kTrickDisplayNameFadeOut = 3, // the current name label fades out
        kTrickDisplayScoreFadeOut = 4, // the points fade out
        kTrickDisplayCrash = 5,       // the name and the points play "Crash"
    };

    // What the balance display (kOverlayBalance) takes: the player, what to
    // do (BalanceDisplayFlags) and the balance.
    struct BalanceMessage
    {
        uint8_t player;  // +0x00
        uint8_t pad01;   // padding: no reader (int25: the senders never set it; only whole-message copies move it)
        uint16_t flags;  // +0x02
        ee::Float value; // +0x04
    };
    static_assert(offsetof(BalanceMessage, flags) == 0x02 && offsetof(BalanceMessage, value) == 0x04 && sizeof(BalanceMessage) == 0x08);
    // BalanceMessage::flags, as the display's handler reads them: with none
    // the value moves; kBalanceShow fades the display in ("FadeIn", its
    // "Type" 1 with kBalanceWheelie, else 0; kBalanceTurnedRound turns its
    // indicator round), kBalanceHide fades it out ("FadeOut").
    constexpr uint16_t kBalanceShow = 0x40;
    constexpr uint16_t kBalanceHide = 0x80;
    constexpr uint16_t kBalanceWheelie = 0x100;
    constexpr uint16_t kBalanceTurnedRound = 0x800;
}
