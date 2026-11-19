// DM2's park editor: the editor's texts.
#pragma once

#include <cstdint>

namespace game::dm2
{
    // The text of the string table's entry `key` (a name's hash), "" if
    // there is none: the engine's stringTableText, as the editor calls it.
    const char *editorText(uint32_t key);
}
