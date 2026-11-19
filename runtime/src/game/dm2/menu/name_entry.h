#pragma once

// The name entry's check (name_entry.cpp): a script function of the front
// end's set-up.

#include <cstdint>

namespace game::dm2
{
    // "bmxValidateName": CommonUI's tempInt set to 1 when its tempString
    // (the name being entered) has a character that is not a space, else 0.
    void bmxValidateName();
}
