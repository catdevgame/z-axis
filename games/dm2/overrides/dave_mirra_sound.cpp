// Dave Mirra Freestyle BMX 2 (SLUS-20159): the ZAudio sound driver on the
// host (zge_sound.cpp, the three games' one driver) with DM2's older
// ZAUDIO.IRX (the same module name and version string as AI's, 81 % of the
// code; decompiled for the port).
//
// EE side: AI's code (soundRpc, bindSoundRpc, soundReply); commands 0-15
// have AI's numbers and payloads (Load: {handle, offset, 1 KB}). DM2's
// ZAUDIO's sample half is AI's (128 samples, Alloc replies the handle,
// ChanSetParam takes it; a fade with flag 2 ends its stream), but for Init,
// which does not reset, and Reset, which also keys off the streams' voices.
// Its streams are the older design (zge_sound.h, StreamRings::Shared):
//  - two 64 KB SPU rings, allocated once at the first Init (sample table
//    entries 0 and 1) and shared by the four streams (core 1 voices 16-23);
//    StrmAlloc allocates nothing; Reset frees every page, rings included;
//  - Play (when the source was just set or stopped) reads two 64 KB pieces
//    of the source into ring halves 0 and 1 (0x800-byte left/right
//    pieces), without moving the progress; Play on a paused stream only
//    restores the pitch and sets the state 4/3 from the half count;
//  - every refill reads 64 KB (0x20 sectors) whatever the channel count;
//    the end is "sectors read >= total - 1": the rest of that piece is
//    zeroed, then either the source starts again (loops left) or the state
//    becomes 8; a stream in state 8 is keyed off at the next half crossing,
//    so the last piece is not heard (as on the PS2);
//  - the volume command sets VOLL of the left voice and VOLR of the right
//    one (the other side 0), a fade the same; a fade ends at volume 0;
//  - a mono stream plays on the left only and gets every other 0x800-byte
//    piece (the other half goes to the missing right ring).
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_sound.cpp.h"

#include "overrides/zge_sound.h"

namespace dave_mirra::native_sound
{
    namespace
    {
        // Where DM2's IOP loads ZAUDIO.IRX when SDRDRV is not loaded (0x15C00
        // with it); the handles point into its sample and stream tables.
        constexpr uint32_t kZaudioBase = 0x00014B00u;
        constexpr zge_host::sound::Game kSound{
            .rpc = sym::dm2::soundRpc,
            .bind = sym::dm2::bindSoundRpc,
            .replyBuffer = sym::dm2::soundReply,
            .sampleTable = kZaudioBase + 0x9180u,
            .streamTable = kZaudioBase + 0x5CE0u,
            .streamStride = 0x80u,
            .sampleSlots = 128,
            .replyHasSpuAddress = false,
            .channelSample = zge_host::sound::ChannelSample::Handle,
            .fadeEndStops = 2u,
            .fadeEndHolds = 0u,
            .rings = zge_host::sound::StreamRings::Shared,
            // The soundtrack's ten songs, MUSIC.ZSD's waves; the song info
            // and the music menu name them by key.
            .songs = 10,
            .bankWaveCount = 0x20u,
            .bankWaves = 0x24u,
            .stringTableText = sym::dm2::stringTableText,
            .songKey = "KSTR_SONGINFO_%u_",
        };
    }
}

namespace dave_mirra
{
    bool installNativeSound(PS2Machine &runtime)
    {
        const bool native = zge_host::sound::install(runtime, native_sound::kSound);
        // The soundtrack's controls, with either driver.
        installSoundtrack(runtime);
        return native;
    }
}
