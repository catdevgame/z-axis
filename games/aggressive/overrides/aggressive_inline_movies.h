#pragma once

class PS2Machine;

namespace aggressive_inline
{
    // The game's movie player on host decoding (unless ZX_MOVIES=hle);
    // true when installed.
    bool installNativeMovies(PS2Machine &runtime);
}
