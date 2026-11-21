#pragma once

class PS2Runtime;

namespace aggressive_inline
{
    // The game's movie player on host decoding (unless ZX_MOVIES=hle);
    // true when installed.
    bool installNativeMovies(PS2Runtime &runtime);
}
