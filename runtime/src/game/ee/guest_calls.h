#pragma once

// ZX_GUEST_CALL (ee/guest_calls.cpp): guest functions called at a chosen
// vsync before a hooked function, for recording paths no input reaches.

class PS2Runtime;

namespace game::guest_calls
{
    // Reads ZX_GUEST_CALL and hooks its functions (after the readable
    // functions and the recorder are installed); nothing when it is unset.
    void install(PS2Runtime &runtime);
}
