// Dave Mirra Freestyle BMX 2 (SLUS-20159): the game's volumes in the
// settings model, as Aggressive Inline's (aggressive_inline_sound.cpp, "the
// game's volumes in the settings model").
//
// The game keeps two volumes in its save, its options menu's SFX and music
// (g_sfxVolumeSetting, g_musicVolumeSetting: 0 .. 128), and tells the sound
// with soundEvent(0, kind, volume) (game/dm2/sound/sound_events.cpp: kind 0
// the effects, 1 the music, which also sets them on what plays), as the
// options menu's BmxUpdateVolumeInfo does (game/dm2/sound/menu_sounds.cpp).
// zge_volumes.cpp shows them as audio.effects_volume and audio.music_volume
// (percent) and passes a change made there the same way, from the sound's
// frame (musicFrame, every frame of every game state): the Audio page's
// variables first (Root.CurrentSFXVolume and CurrentMusicVolume,
// g_currentSfxVolume and g_currentMusicVolume, which frontEndEnter copies
// from the settings and the page shows and edits), then the saved bytes.
// DM2 has no speech volume of its own (no row). With the app's settings
// (PS2X_GAME_SETTINGS_MENUS=1) and the native sound driver; nothing is
// written until a row is changed.

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_volumes.cpp.h"

#include "runtime/ps2_boot_config.h"
#include "zge_volumes.h"

namespace dave_mirra::volumes
{
    namespace
    {
        constexpr zge_host::volumes::Volume kVolumes[] = {
            {"audio.music_volume", "Music Volume", "The game's own music volume.", 1u, sym::dm2::g_musicVolumeSetting,
             0u, sym::dm2::g_currentMusicVolume},
            {"audio.effects_volume", "Effects Volume", "The game's own effects volume.", 0u, sym::dm2::g_sfxVolumeSetting,
             0u, sym::dm2::g_currentSfxVolume},
        };
        constexpr zge_host::volumes::Game kGameVolumes{
            .tag = "[dave-mirra]",
            .frame = sym::dm2::musicFrame,
            .soundEvent = sym::dm2::soundEvent,
            .fullVolume = 128,
            .soundMode = 0u,
            .volumes = kVolumes,
            .count = 2u,
        };
    }
}

namespace dave_mirra
{
    void installGameVolumes(PS2Runtime &runtime)
    {
        if (const char *on = ps2x::boot::text("PS2X_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        zge_host::volumes::install(runtime, volumes::kGameVolumes);
    }
}
