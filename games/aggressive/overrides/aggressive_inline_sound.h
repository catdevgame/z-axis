#pragma once

class PS2Machine;

namespace aggressive_inline
{
    // The game's ZAudio sound driver on the host (unless ZX_SOUND=spu2);
    // true when installed (then ZAUDIO.IRX and LIBSD.IRX are not needed).
    bool installNativeSound(PS2Machine &runtime);
}
