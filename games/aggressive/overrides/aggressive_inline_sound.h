#pragma once

class PS2Machine;

namespace aggressive_inline
{
    // The game's ZAudio sound driver on the host (unless ZX_SOUND=spu2);
    // true when installed (then ZAUDIO.IRX and LIBSD.IRX are not needed).
    bool installNativeSound(PS2Machine &runtime);
    // The soundtrack's controls: next, previous, shuffle and the songs a mod
    // adds (aggressive_inline_soundtrack.cpp; engine/overrides/zge_soundtrack.h).
    bool installSoundtrack(PS2Machine &runtime);
}
