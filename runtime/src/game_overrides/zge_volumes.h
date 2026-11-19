#pragma once

// The game's own volumes in the settings model, shared by the games built
// on the Z-Axis engine (zge_volumes.cpp). Each game keeps its options'
// volumes as bytes (in its save) and tells the sound with its sound event
// 0, soundEvent(0, kind, volume, ...), which also sets them on what plays.
// Those stay the only volumes: the settings model shows each as a row in
// percent (audio.music_volume, audio.effects_volume, audio.speech_volume),
// and a change made there is passed to the game's event 0 from a function
// of the game's that runs every frame on the EE, as its options menu does.
// Nothing is written until a row is changed.
//
// Aggressive Inline (aggressive_inline_sound.cpp), Dave Mirra BMX 2
// (dave_mirra_volumes.cpp) and BMX XXX (bmx_xxx_sound.cpp) each describe
// theirs in a Game record.

#include <cstdint>

class PS2Runtime;

namespace zge_host::volumes
{
    // One of the game's volumes.
    struct Volume
    {
        const char *key;   // the settings row ("audio.music_volume")
        const char *label; // "Music Volume"
        const char *help;
        uint32_t kind;     // soundEvent(0, kind, ...)'s kind
        // The byte the options menu shows and the save keeps, which the
        // menu writes before it sends the event (0: the event's own byte,
        // `heard`, is that).
        uint32_t saved;
        // The byte the event sets (0: the game has none but `saved`).
        uint32_t heard;
        // A word the options menu edits and copies into `saved` (its
        // script's variable), kept in step (0: none).
        uint32_t menuWord = 0;
    };

    struct Game
    {
        const char *tag;       // the log's ("[dave-mirra]")
        uint32_t frame;        // runs every frame on the EE: the change is passed from there
        uint32_t soundEvent;   // soundEvent(event, a, b, c, d)
        int fullVolume;        // the bytes' loudest (127, or 128)
        // The sound mode word, -1 while the game's sound is not up (the event
        // then does nothing, so only the bytes are written); 0: always up.
        uint32_t soundMode;
        const Volume *volumes;
        uint32_t count;
    };

    // Hooks game.frame and adds the rows (posted to the window thread).
    // False if the hook could not be installed.
    bool install(PS2Runtime &runtime, const Game &game);
}
