#pragma once
// What each player picked (bmx.c's table of two): BMX XXX's own code. The
// table is profile/players.h's g_playerSetups; game/player_riders.h reads
// it by player.

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What each player picked: the rider character they ride (an index of
    // the characters' records), the controller configuration, the
    // vibration and the tips, and their name ("player" at the game's start).
    struct PlayerSetup
    {
        uint8_t flags;            // +0x00 kSetupVibration, kSetupTips
        uint8_t controllerLayout; // +0x01 the controller configuration the player's rider takes (the menus' "SelectedIndex")
        uint8_t cameraType;       // +0x02 the player's camera's type (the level's loading gives it to setCameraType); 0 at the game's start
        uint8_t character;        // +0x03
        uint8_t watches;          // +0x04 (inferred) 1: the player's camera takes mode 6 while another rides
        char name[0x20];          // +0x05
    };
    static_assert(offsetof(PlayerSetup, controllerLayout) == 0x01 && offsetof(PlayerSetup, character) == 0x03);
    static_assert(offsetof(PlayerSetup, watches) == 0x04 && offsetof(PlayerSetup, name) == 0x05 && sizeof(PlayerSetup) == 0x25);
    // PlayerSetup::flags. Bit 0: the pad vibrates (the menus'
    // "Vibration.isOn"; set at the game's start). Bit 1: the tips are on
    // (DM2's flag: a one-rider turn starts with every tip left); the level
    // reward that opens the second level clears it.
    constexpr uint8_t kSetupVibration = 0x01;
    constexpr uint8_t kSetupTips = 0x02;
}
