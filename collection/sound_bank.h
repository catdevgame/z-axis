#pragma once

// A game's sound bank read on the host for the collection's menu: the
// front end's music and the menus' move, change, select, cancel and blocked
// sounds, from the player's own disc (the games' banks, not copies).
//
// A bank (.ZSD, "ZSNDPS2 ") holds its tables first and its PS-ADPCM waves
// after them, every offset in the tables an offset in the file
// (engine/zge/sound/bank.h has the engine's view of it). Three tables matter
// here: the samples (which wave reference, the pitch, volume, pan and
// envelope rates), the wave references (which wave and its pitch base) and
// the waves (offset and size). The engine has two forms of the head:
//   Zge3 (Aggressive Inline, Dave Mirra BMX 2): +0x10 sample count, +0x14
//        samples, +0x18 reference count, +0x1C references, +0x20 wave count,
//        +0x24 waves;
//   Zge4 (BMX XXX): each table a count, a name-hash table and the table:
//        +0x10/+0x18 samples, +0x1C/+0x24 references, +0x28/+0x30 waves.
// A sample is 0x18 bytes, Dave Mirra BMX 2's 0x14 (no distance byte, so its
// envelope rates start a byte earlier).
//
// The sound is made as the games' own driver makes it: the voice's pitch is
// the sample's times the reference's pitch base (0x1000 = 44.1 kHz), its
// envelope the sample's rates in the SPU2's ADSR words, and it plays on an
// SPU2 voice (runtime/spu_voice.h) at 48 kHz.

#include "runtime/spu_voice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zx::collection
{
    enum class BankLayout
    {
        Zge3,       // Aggressive Inline
        Zge3Dm2,    // Dave Mirra BMX 2: Zge3 with 0x14-byte samples
        Zge4,       // BMX XXX
    };

    // The layout of a game's banks by its collection id ("ai", "dm2",
    // "bmx"); false for another id.
    bool bankLayoutOf(std::string_view game, BankLayout &out);

    // One sample of a bank, ready for an SPU2 voice.
    struct BankSound
    {
        uint16_t voicePitch = 0;  // the SPU2's pitch register (0x1000 = 48 kHz)
        uint8_t volume = 0;       // 0..127
        int8_t pan = 0;           // -127..127
        uint16_t adsr1 = 0, adsr2 = 0;
        std::shared_ptr<const zx::spu::DecodedSample> wave; // null: its wave is outside the file

        // The voice's volume registers for the sample at `level` (0..127 of
        // its own volume), split by its pan as the engine's panVolumes does.
        void voiceVolumes(uint32_t level, uint16_t &left, uint16_t &right) const;
        // Its rate in Hz (the pitch register at 48 kHz).
        double rate() const { return voicePitch * 48000.0 / 4096.0; }
    };

    // Every sample of the bank, in order. False with *error when the file
    // is not a bank of that layout or a table runs past its end; a sample
    // whose wave does not fit in the file is kept with a null wave.
    bool readSoundBank(const std::vector<uint8_t> &file, BankLayout layout, std::vector<BankSound> &out,
                       std::string *error = nullptr);

    // The menus' sounds, in the order zgui asks for them.
    enum class MenuSound : uint8_t
    {
        Move,
        Change,
        Select,
        Cancel,
        Blocked,
    };
    constexpr size_t kMenuSoundCount = 5;

    // Where a game keeps its front end's sound, in the games' audio folder
    // (ASSETS/AUDIO): the music is the front end bank's (SFXMEN00.ZSD) first
    // sample, which each game's setGameMode starts as the menu's music; the
    // menus' sounds are samples of the common bank (SFXCOM.ZSD), each game's
    // soundEvent picking them (in the front end).
    struct MenuSoundSource
    {
        const char *musicBank = "ASSETS/AUDIO/SFXMEN00.ZSD";
        uint32_t musicSample = 0;
        const char *soundBank = "ASSETS/AUDIO/SFXCOM.ZSD";
        std::array<uint32_t, kMenuSoundCount> sounds{}; // by MenuSound
    };
    // nullptr for a game the collection does not know.
    const MenuSoundSource *menuSoundSource(std::string_view game);
}
