#pragma once

// The main menu's cheat codes (cheats.cpp): the script function behind the
// menu's key sniffer.

#include <cstdint>

namespace game::dm2
{
    // "bmxCheckCheat": the key the main menu's sniffer just read joins the
    // last 32 keys, and every code found among them is taken out and does
    // what it opens, with the UI's select sound.
    void bmxCheckCheat();
}
