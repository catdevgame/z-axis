#pragma once

// The save system's screens (save_menus.cpp).

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace game::bmx
{
    // (save_menus.cpp)
    int32_t showParkFileMenu(uint32_t up);
    int32_t showSaveSlotMenu(ee::Ptr<uint32_t> labels);
    int32_t showSaveChoice(ee::Ptr<const char> text, int32_t selected, int32_t allowCancel);
    int32_t showSaveMessage(uint32_t text);
}
