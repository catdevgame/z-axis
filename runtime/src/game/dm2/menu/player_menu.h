#pragma once

// The front end's rider choice ("MainMenu.SelectPlayer", player_menu.cpp):
// the script functions behind it.

#include "../game/players.h"

#include "ee/plain.h"

#include <cstdint>

namespace game::dm2
{
    // "bmxSetupUnLockPlayer": how far the rider choice goes and which riders
    // its last two places show, by which of the two hidden riders (the 15th
    // and 16th characters) are still locked.
    void bmxSetupUnLockPlayer();

    // A character's record by its number (the rider choice's value, and the
    // menu's CurrentRider variables).
    ee::Ptr<RiderCharacter> riderCharacter(uint32_t character);

    // The rider choice shows eight stats a character, each a bar of as many
    // points as this.
    constexpr uint32_t kRiderStats = 8;
    uint8_t riderStatBars(uint32_t character, uint32_t stat);

    // The character's respect written into `text` ("Respect: 12,345/40,000")
    // and shown by the rider choice and the bike choice.
    void showRespect(ee::Ptr<char> text, uint32_t respect);

    // "bmxSetupPlayerStat": the rider choice's frame for the character it is
    // on: the eight stat bars, the sponsors' logos, whether the competition
    // can be entered, and the character's respect.
    void bmxSetupPlayerStat();
}
