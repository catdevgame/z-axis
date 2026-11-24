#pragma once

#include <cstdint>
#include <string>

class PS2Machine;

namespace aggressive_inline
{
    // Developer and mod tools (docs/DEVTOOLS.md in the bring-up workspace):
    // the language of the game's string table (ZX_LANGUAGE), the hidden
    // debug menu (ZX_DEV_MENU=1: the developer_menu key, F1 or Select+L3, in a level) and Mods/tuning.ini
    // overrides of the developer tuning table. Nothing is changed or hooked
    // unless one of them is asked for.
    void installDevTools(PS2Machine &runtime);

    // Guest address of a tuning table entry's value (float) by category and
    // name as the game spells them ("Camera", "FOV"), 0 if there is none.
    uint32_t tuningValueAddress(const uint8_t *rdram, const std::string &category, const std::string &name);
}
