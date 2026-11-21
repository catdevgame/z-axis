// A game's sound bank on the host (collection/sound_bank.h).
#include "collection/sound_bank.h"

#include <cstring>

namespace zx::collection
{
    namespace
    {
        uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
        uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

        // Where the head keeps each table's count and offset.
        struct Head
        {
            size_t sampleCount, samples, referenceCount, references, waveCount, waves, sampleBytes;
            // Envelope rates' offsets in a sample.
            size_t attack, decay, sustainRate, release, sustainLevel;
        };

        constexpr Head kZge3{0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x18, 0x07, 0x08, 0x09, 0x0A, 0x0B};
        constexpr Head kZge3Dm2{0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x14, 0x06, 0x07, 0x08, 0x09, 0x0A};
        constexpr Head kZge4{0x10, 0x18, 0x1C, 0x24, 0x28, 0x30, 0x18, 0x07, 0x08, 0x09, 0x0A, 0x0B};
        constexpr size_t kHeadBytes = 0x34;
        constexpr size_t kReferenceBytes = 0x10, kWaveBytes = 8;

        // The SPU2 runs at 48 kHz; the driver's pitch counts 0x1000 for 44.1
        // kHz (game_overrides/zge_sound.cpp makes the same change).
        uint16_t voicePitch(uint32_t samplePitch, uint32_t pitchBase)
        {
            const uint32_t driverPitch = (samplePitch * pitchBase) >> 12 & 0xFFFFu;
            return static_cast<uint16_t>(driverPitch * 44100u / 48000u);
        }

        // The voice's registers as the games' driver sets them from a sample
        // (ZAUDIO.IRX's: engine/zge/sound/driver_commands.h has the engine's
        // side): a level 0..127 is the SPU2's volume 0x3F80 at 127; ADSR1 is
        // attack (7 bits), decay (4), sustain level (4); ADSR2 sustain rate (7
        // bits) and release (5), the sustain always going down linearly.
        uint16_t voiceVolume(uint32_t level) { return static_cast<uint16_t>(level << 7); }
        uint16_t attackDecaySustain(uint32_t attack, uint32_t decay, uint32_t sustainLevel)
        {
            return static_cast<uint16_t>((attack & 0x7Fu) << 8 | (decay & 0xFu) << 4 | (sustainLevel & 0xFu));
        }
        uint16_t sustainRelease(uint32_t sustainRate, uint32_t release)
        {
            constexpr uint16_t kSustainFalling = 1u << 14;
            return static_cast<uint16_t>((sustainRate & 0x7Fu) << 6 | (release & 0x1Fu) | kSustainFalling);
        }

        bool fail(std::string *error, const char *why)
        {
            if (error)
                *error = why;
            return false;
        }
    }

    bool bankLayoutOf(std::string_view game, BankLayout &out)
    {
        if (game == "ai")
            out = BankLayout::Zge3;
        else if (game == "dm2")
            out = BankLayout::Zge3Dm2;
        else if (game == "bmx")
            out = BankLayout::Zge4;
        else
            return false;
        return true;
    }

    void BankSound::voiceVolumes(uint32_t level, uint16_t &left, uint16_t &right) const
    {
        const uint32_t v = (level * volume) >> 7;
        uint32_t l = v, r = v;
        if (pan > 0)
            l = (v * static_cast<uint32_t>(127 - pan)) >> 7 & 0xFFu;
        else if (pan < 0)
            r = (v * static_cast<uint32_t>(pan + 127)) >> 7 & 0xFFu;
        left = voiceVolume(l);
        right = voiceVolume(r);
    }

    bool readSoundBank(const std::vector<uint8_t> &file, BankLayout layout, std::vector<BankSound> &out, std::string *error)
    {
        out.clear();
        const uint8_t *d = file.data();
        const size_t size = file.size();
        if (size < kHeadBytes || std::memcmp(d, "ZSNDPS2 ", 8) != 0)
            return fail(error, "not a PS2 sound bank");
        const Head &h = layout == BankLayout::Zge4 ? kZge4 : layout == BankLayout::Zge3Dm2 ? kZge3Dm2 : kZge3;
        const size_t samples = u32(d + h.samples), sampleCount = u32(d + h.sampleCount);
        const size_t references = u32(d + h.references), referenceCount = u32(d + h.referenceCount);
        const size_t waves = u32(d + h.waves), waveCount = u32(d + h.waveCount);
        const auto fits = [size](size_t at, size_t count, size_t bytes) { return count <= size / bytes && at <= size - count * bytes; };
        if (!fits(samples, sampleCount, h.sampleBytes) || !fits(references, referenceCount, kReferenceBytes) ||
            !fits(waves, waveCount, kWaveBytes))
            return fail(error, "a table runs past the end of the bank");
        out.reserve(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i)
        {
            const uint8_t *s = d + samples + i * h.sampleBytes;
            BankSound sound;
            const uint32_t reference = u16(s);
            const uint32_t pitch = u16(s + 2);
            sound.volume = s[4];
            sound.pan = static_cast<int8_t>(s[5]);
            sound.adsr1 = attackDecaySustain(s[h.attack], s[h.decay], s[h.sustainLevel]);
            sound.adsr2 = sustainRelease(s[h.sustainRate], s[h.release]);
            if (reference < referenceCount)
            {
                const uint8_t *r = d + references + reference * kReferenceBytes;
                const uint32_t wave = u16(r);
                sound.voicePitch = voicePitch(pitch, u16(r + 2));
                if (wave < waveCount)
                {
                    const uint8_t *w = d + waves + wave * kWaveBytes;
                    const size_t offset = u32(w), bytes = u32(w + 4);
                    if (offset <= size && bytes <= size - offset)
                        sound.wave = zx::spu::decodeSample(d + offset, bytes);
                }
            }
            out.push_back(std::move(sound));
        }
        return true;
    }

    const MenuSoundSource *menuSoundSource(std::string_view game)
    {
        // soundEvent's menu sounds in each game (Move, Change, Select, Cancel,
        // Blocked): games/aggressive/sound/events.cpp (the menu bank's 1, 2,
        // 3, 0, 6), games/dm2/sound/sound_events.cpp and games/bmx/sound/
        // events.cpp (0, 1, 2, then 5 for Cancel in the front end, 4).
        static const MenuSoundSource kAi{"ASSETS/AUDIO/SFXMEN00.ZSD", 0, "ASSETS/AUDIO/SFXCOM.ZSD", {1, 2, 3, 0, 6}};
        static const MenuSoundSource kDm2{"ASSETS/AUDIO/SFXMEN00.ZSD", 0, "ASSETS/AUDIO/SFXCOM.ZSD", {0, 1, 2, 5, 4}};
        static const MenuSoundSource kBmx{"ASSETS/AUDIO/SFXMEN00.ZSD", 0, "ASSETS/AUDIO/SFXCOM.ZSD", {0, 1, 2, 5, 4}};
        if (game == "ai")
            return &kAi;
        if (game == "dm2")
            return &kDm2;
        if (game == "bmx")
            return &kBmx;
        return nullptr;
    }
}
