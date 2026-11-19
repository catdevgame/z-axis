#pragma once

class PS2Runtime;

namespace aggressive_inline
{
    // The game's ZAudio sound driver on the host (unless PS2X_SOUND=spu2);
    // true when installed (then ZAUDIO.IRX and LIBSD.IRX are not needed).
    bool installNativeSound(PS2Runtime &runtime);
}
