// BMX XXX (SLUS-20415): the ZAudio sound driver on the host (zge_sound.cpp,
// the three games' one driver) with BMX's ZAUDIO.IRX.
//
// BMX's EE side is AI's code (ps2Chan.c, identical functions in the work
// database): soundRpc and bindSoundRpc, commands 0-15 with AI's numbers and
// payloads. BMX's ZAUDIO.IRX (119,779 bytes, with its symbol table and debug
// sections: built without optimisation) was decompiled (headless Ghidra on
// its image at base 0, named from its own symbols) and compared with AI's
// function by function: the command handler, sample memory (1 KB pages from
// 0x8000), channels, the stream table (0x90-byte entries), StrmAlloc /
// SetParam / StrmBuffer / Play / Stop / Pause, the refill
// (ZAudioStream_Upload, UploadStereoBuffer's loop flags), the 60 Hz update
// thread (KOFF read back, queued stream commands, the end by the voice's
// NAX, status by SIF DMA) are AI's: ZGE3's streams. What differs:
//  - the sample table (gSamples) has 256 entries (AI's 128);
//  - Alloc replies {handle, SPU address} and ChanSetParam takes the SPU
//    address to play from (AI's takes the sample handle): the EE keeps a
//    bank as one allocation and starts each sound at its offset in it;
//  - at a fade's end, flag 4 ends the stream (state 8) and flag 2 alone
//    leaves it in state 6 (AI's ZAUDIO: flag 2 ends it).
//
// And the game's volumes in the settings model (zge_volumes.cpp), as AI's:
// the same three bytes (g_masterVolume: effects, music, speech, 0-127) and
// sound event 0, passed from the ZAudio per-frame task (soundTask). BMX's
// options menu also keeps them in the saved game (SavedGame's
// channelVolumes, which its sliders show), written first, as the menu's
// bmxmenuscriptSet*Volume do (game/bmx/ui/menu_script.cpp). With the app's
// settings (PS2X_GAME_SETTINGS_MENUS=1).
#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_sound.cpp.h"

#include "runtime/ps2_boot_config.h"
#include "zge_sound.h"
#include "zge_volumes.h"

#include <cstddef>

namespace bmx_xxx::native_sound
{
    namespace
    {
        // Where BMX's IOP loads ZAUDIO.IRX (after the other seven modules);
        // the handles point into its gSamples and gStreams.
        constexpr uint32_t kZaudioBase = 0x00049900u;
        constexpr zge_host::sound::Game kSound{
            .rpc = sym::bmx::soundRpc,
            .bind = sym::bmx::bindSoundRpc,
            .replyBuffer = sym::bmx::soundReply, // soundRpc's RPC client's receive buffer
            .sampleTable = kZaudioBase + 0xC700u,
            .streamTable = kZaudioBase + 0x9258u,
            .streamStride = 0x90u,
            .sampleSlots = 256,
            .replyHasSpuAddress = true,
            .channelSample = zge_host::sound::ChannelSample::SpuAddress,
            .fadeEndStops = 4u,
            .fadeEndHolds = 2u,
            .rings = zge_host::sound::StreamRings::PerStream,
            // The soundtrack's twenty songs, MUSIC.ZSD's waves (ZGE4's bank
            // header: the count at +0x28, the table at +0x30); the song info
            // and the music menu name them by key.
            .songs = 20,
            .bankWaveCount = 0x28u,
            .bankWaves = 0x30u,
            .stringTableText = sym::bmx::stringTableText,
            .songKey = "KSTR_SONGINFO_%02u_",
        };

        // The saved game (game/bmx/profile/players.h SavedGame) as far as
        // its volumes, by the volume event's kind.
        struct SavedGameStart
        {
            uint8_t _00000[0x1F];
            uint8_t language;
            uint8_t channelVolumes[3];
        };
        static_assert(offsetof(SavedGameStart, channelVolumes) == 0x20);
        constexpr uint32_t kEffects = 0u, kMusic = 1u, kSpeech = 2u; // game/bmx/sound/sound_state.h Volume
        constexpr uint32_t savedVolume(uint32_t kind)
        {
            return sym::bmx::g_savedGame + offsetof(SavedGameStart, channelVolumes) + kind;
        }
        constexpr zge_host::volumes::Volume kVolumes[] = {
            {"audio.music_volume", "Music Volume", "The game's own music volume.", kMusic,
             savedVolume(kMusic), sym::bmx::g_masterVolume + kMusic},
            {"audio.effects_volume", "Effects Volume", "The game's own effects volume.", kEffects,
             savedVolume(kEffects), sym::bmx::g_masterVolume + kEffects},
            {"audio.speech_volume", "Speech Volume", "The game's own speech volume.", kSpeech,
             savedVolume(kSpeech), sym::bmx::g_masterVolume + kSpeech},
        };
        constexpr zge_host::volumes::Game kGameVolumes{
            .tag = "[bmx-xxx]",
            .frame = sym::bmx::soundTask,
            .soundEvent = sym::bmx::soundEvent,
            .fullVolume = 127,
            .soundMode = sym::bmx::g_gameMode, // -1 before the game runs: soundEvent does nothing
            .volumes = kVolumes,
            .count = 3u,
        };
    }
}

namespace bmx_xxx
{
    bool installNativeSound(PS2Runtime &runtime)
    {
        if (const char *on = ps2x::boot::text("PS2X_GAME_SETTINGS_MENUS"); on && on[0] == '1')
            zge_host::volumes::install(runtime, native_sound::kGameVolumes);
        return zge_host::sound::install(runtime, native_sound::kSound);
    }
}
