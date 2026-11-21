// The collection's front end: the games' sound banks read on the host and
// the menu's sound (collection/sound_bank.h, menu_audio.h), the pictures and
// movies it takes from each game's disc with every "not set up" case
// (front_art.h), and the Thor's bottom screen (menu_bottom.h). With
// ZX_TEST_DISCS (a folder of <id>/disc folders) the real banks' music too.
#include "MiniTest.h"

#include "collection/front_art.h"
#include "collection/game_order.h"
#include "collection/menu_audio.h"
#include "collection/menu_bottom.h"
#include "collection/menu_scene.h"
#include "collection/settings.h"
#include "collection/sound_bank.h"
#include "collection/vector_font.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace
{
    namespace col = zx::collection;
    namespace fs = std::filesystem;

    void put16(std::vector<uint8_t> &out, size_t at, uint16_t value)
    {
        out[at] = static_cast<uint8_t>(value);
        out[at + 1] = static_cast<uint8_t>(value >> 8);
    }

    void put32(std::vector<uint8_t> &out, size_t at, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            out[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // A PS-ADPCM wave of `blocks` blocks of 28 equal samples (shift 0,
    // filter 0, every nibble 1: 4096), the last one with `endFlags`.
    std::vector<uint8_t> wave(size_t blocks, uint8_t endFlags)
    {
        std::vector<uint8_t> out(blocks * 16, 0x11);
        for (size_t b = 0; b < blocks; ++b)
        {
            out[b * 16] = 0x00;
            out[b * 16 + 1] = 0x00;
        }
        out[1] = 0x04; // the loop starts at the first block
        out[(blocks - 1) * 16 + 1] = endFlags;
        return out;
    }

    // A bank of `layout` with one wave per sample: sample i plays wave i at
    // pitch 0x1000 with pitch base 0x800, volume `volumes[i]`, pan 0, and
    // the stock banks' envelope: attack 0, decay 0, sustain rate 0x7F (held),
    // release 0x0F, sustain level 0x0F.
    std::vector<uint8_t> bank(col::BankLayout layout, const std::vector<std::vector<uint8_t>> &waves,
                              const std::vector<uint8_t> &volumes)
    {
        const size_t n = waves.size();
        const size_t sampleBytes = layout == col::BankLayout::Zge3Dm2 ? 0x14 : 0x18;
        const size_t samples = 0x64, references = samples + n * sampleBytes, table = references + n * 0x10;
        size_t data = (table + n * 8 + 0x7FF) & ~size_t(0x7FF);
        std::vector<uint8_t> out(data, 0);
        std::memcpy(out.data(), "ZSNDPS2 ", 8);
        if (layout == col::BankLayout::Zge4)
        {
            put32(out, 0x10, static_cast<uint32_t>(n));
            put32(out, 0x18, static_cast<uint32_t>(samples));
            put32(out, 0x1C, static_cast<uint32_t>(n));
            put32(out, 0x24, static_cast<uint32_t>(references));
            put32(out, 0x28, static_cast<uint32_t>(n));
            put32(out, 0x30, static_cast<uint32_t>(table));
        }
        else
        {
            put32(out, 0x10, static_cast<uint32_t>(n));
            put32(out, 0x14, static_cast<uint32_t>(samples));
            put32(out, 0x18, static_cast<uint32_t>(n));
            put32(out, 0x1C, static_cast<uint32_t>(references));
            put32(out, 0x20, static_cast<uint32_t>(n));
            put32(out, 0x24, static_cast<uint32_t>(table));
        }
        const size_t rates = layout == col::BankLayout::Zge3Dm2 ? 6 : 7;
        for (size_t i = 0; i < n; ++i)
        {
            const size_t s = samples + i * sampleBytes;
            put16(out, s, static_cast<uint16_t>(i));
            put16(out, s + 2, 0x1000);
            out[s + 4] = volumes[i];
            out[s + rates] = 0x00;     // attack
            out[s + rates + 1] = 0x00; // decay
            out[s + rates + 2] = 0x7F; // sustain rate
            out[s + rates + 3] = 0x0F; // release
            out[s + rates + 4] = 0x0F; // sustain level
            put16(out, references + i * 0x10, static_cast<uint16_t>(i));
            put16(out, references + i * 0x10 + 2, 0x800);
            put32(out, table + i * 8, static_cast<uint32_t>(out.size()));
            put32(out, table + i * 8 + 4, static_cast<uint32_t>(waves[i].size()));
            out.insert(out.end(), waves[i].begin(), waves[i].end());
        }
        put32(out, 8, static_cast<uint32_t>(out.size()));
        put32(out, 0x0C, static_cast<uint32_t>(data));
        return out;
    }

    // A picture of one colour.
    col::Image picture(int width, int height, uint32_t colour)
    {
        col::Image image;
        image.width = width;
        image.height = height;
        image.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), colour);
        return image;
    }

    fs::path scratch(const std::string &name)
    {
        const fs::path dir = fs::temp_directory_path() /
                             ("zx-front-end-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        return dir;
    }

    void write(const fs::path &file, const std::vector<uint8_t> &bytes)
    {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // A disc folder with the game's two banks: the music (a looping wave)
    // and seven menu sounds of 2..8 blocks.
    fs::path soundDisc(const std::string &game, col::BankLayout layout)
    {
        const fs::path disc = scratch(game) / "disc";
        write(disc / "SYSTEM.CNF", {'B', 'O', 'O', 'T'});
        write(disc / "ASSETS/AUDIO/SFXMEN00.ZSD", bank(layout, {wave(64, 0x03)}, {127}));
        std::vector<std::vector<uint8_t>> sounds;
        for (size_t i = 0; i < 7; ++i)
            sounds.push_back(wave(2 + i, 0x07));
        write(disc / "ASSETS/AUDIO/SFXCOM.ZSD", bank(layout, sounds, std::vector<uint8_t>(7, 100)));
        return disc;
    }

    // A texture library entry: FORM { TXRC { TXRH (name at +0x14), TBMP or TZMP (an 8-bit BMP w x h, every
    // texel colour index 1 = B G R A `bgra`) } }.
    std::vector<uint8_t> entry(const std::string &name, const char *kind, uint32_t bgra, int w, int h)
    {
        const size_t stride = ((static_cast<size_t>(w) * 8 + 31) / 32) * 4;
        std::vector<uint8_t> bmp(14 + 40 + 1024 + stride * h, 1);
        std::memset(bmp.data(), 0, 14 + 40 + 1024);
        bmp[0] = 'B';
        bmp[1] = 'M';
        put32(bmp, 10, 14 + 40 + 1024);
        put32(bmp, 14, 40);
        put32(bmp, 18, static_cast<uint32_t>(w));
        put32(bmp, 22, static_cast<uint32_t>(h));
        put16(bmp, 28, 8);
        put32(bmp, 54 + 4, bgra);
        std::vector<uint8_t> header(0x14 + 0x20, 0);
        std::memcpy(header.data() + 0x14, name.data(), name.size());
        std::vector<uint8_t> body;
        const auto chunk = [&body](const char *tag, const std::vector<uint8_t> &data)
        {
            const size_t at = body.size();
            body.resize(at + 8);
            std::memcpy(body.data() + at, tag, 4);
            put32(body, at + 4, static_cast<uint32_t>(data.size()));
            body.insert(body.end(), data.begin(), data.end());
        };
        chunk("TXRH", header);
        chunk(kind, bmp);
        // FORM { TXRC { ... } }, as the stock libraries have it.
        std::vector<uint8_t> form(16);
        std::memcpy(form.data(), "FORM", 4);
        put32(form, 4, static_cast<uint32_t>(body.size() + 8));
        std::memcpy(form.data() + 8, "TXRC", 4);
        put32(form, 12, static_cast<uint32_t>(body.size()));
        form.insert(form.end(), body.begin(), body.end());
        return form;
    }

    uint64_t fnv(const std::vector<int16_t> &pcm)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const int16_t s : pcm)
            for (int i = 0; i < 2; ++i)
            {
                h ^= static_cast<uint8_t>(static_cast<uint16_t>(s) >> (8 * i));
                h *= 0x100000001b3ull;
            }
        return h;
    }
}

void register_front_end_tests()
{
    MiniTest::Case("FrontEndSound", [](TestCase &tc)
    {
        tc.Run("each layout's samples, pitch, volume and envelope", [](TestCase &t)
        {
            for (const col::BankLayout layout : {col::BankLayout::Zge3, col::BankLayout::Zge3Dm2, col::BankLayout::Zge4})
            {
                std::vector<col::BankSound> sounds;
                std::string error;
                t.IsTrue(col::readSoundBank(bank(layout, {wave(3, 0x07), wave(5, 0x03)}, {100, 127}), layout, sounds, &error),
                         "read: " + error);
                t.Equals(sounds.size(), size_t(2), "two samples");
                if (sounds.size() != 2)
                    continue;
                // 0x1000 x 0x800 >> 12 = 0x800 at 44.1 kHz: 22050 Hz, the SPU2's 0x800 * 44100 / 48000.
                t.Equals(sounds[0].voicePitch, uint16_t(0x800 * 44100 / 48000), "the driver's pitch at 48 kHz");
                t.IsTrue(sounds[0].rate() > 22040.0 && sounds[0].rate() < 22060.0, "22.05 kHz");
                t.Equals(sounds[0].volume, uint8_t(100), "volume");
                t.Equals(sounds[0].adsr1, uint16_t(0x000F), "attack, decay, sustain level");
                t.Equals(sounds[0].adsr2, uint16_t(1 << 14 | 0x7F << 6 | 0x0F), "sustain falling, rate, release");
                t.IsTrue(sounds[1].wave != nullptr, "wave decoded");
                if (sounds[1].wave)
                {
                    t.Equals(sounds[1].wave->pcm.size(), size_t(5 * 28), "every block up to the end flag");
                    t.Equals(sounds[1].wave->pcm[0], int16_t(4096), "the samples");
                    t.Equals(sounds[1].wave->loopPcm.size(), size_t(5 * 28), "it loops from its first block");
                }
            }
        });

        tc.Run("a damaged bank is refused or keeps its sound silent", [](TestCase &t)
        {
            std::vector<col::BankSound> sounds;
            std::string error;
            t.IsFalse(col::readSoundBank({'Z', 'S', 'N', 'D'}, col::BankLayout::Zge3, sounds, &error), "too short");
            t.IsFalse(col::readSoundBank(std::vector<uint8_t>(0x100, 0), col::BankLayout::Zge3, sounds, &error), "no magic");
            std::vector<uint8_t> file = bank(col::BankLayout::Zge3, {wave(3, 0x07)}, {127});
            put32(file, 0x10, 100000); // a sample count past the end
            t.IsFalse(col::readSoundBank(file, col::BankLayout::Zge3, sounds, &error), "a table past the end");
            file = bank(col::BankLayout::Zge3, {wave(3, 0x07)}, {127});
            put32(file, 0x24 + 0, static_cast<uint32_t>(file.size() - 4)); // the wave table past the end
            t.IsFalse(col::readSoundBank(file, col::BankLayout::Zge3, sounds, &error), "the wave table past the end");
            file = bank(col::BankLayout::Zge3, {wave(3, 0x07)}, {127});
            const size_t table = file[0x24] | file[0x25] << 8;
            put32(file, table + 4, 0x10000000); // the wave runs past the end
            t.IsTrue(col::readSoundBank(file, col::BankLayout::Zge3, sounds, &error), "the bank reads");
            t.IsTrue(sounds.size() == 1 && sounds[0].wave == nullptr, "the sample has no wave");
        });

        tc.Run("pan splits the volume as the engine does", [](TestCase &t)
        {
            col::BankSound s;
            s.volume = 100;
            uint16_t l = 0, r = 0;
            s.voiceVolumes(127, l, r);
            t.Equals(l, uint16_t(((127 * 100) >> 7) << 7), "centre left");
            t.Equals(r, l, "centre right");
            s.pan = 127;
            s.voiceVolumes(127, l, r);
            t.Equals(l, uint16_t(0), "hard right: no left");
            s.pan = -64;
            s.voiceVolumes(127, l, r);
            t.Equals(r, uint16_t((((99 * 63) >> 7) & 0xFF) << 7), "left of centre: the right side less");
        });

        tc.Run("the games' menu sounds", [](TestCase &t)
        {
            const col::MenuSoundSource *ai = col::menuSoundSource("ai");
            const col::MenuSoundSource *dm2 = col::menuSoundSource("dm2");
            const col::MenuSoundSource *bmx = col::menuSoundSource("bmx");
            t.IsTrue(ai && dm2 && bmx && !col::menuSoundSource("thps"), "three games");
            if (!ai || !dm2 || !bmx)
                return;
            t.Equals(ai->sounds[static_cast<size_t>(col::MenuSound::Move)], uint32_t(1), "AI moves with 1");
            t.Equals(ai->sounds[static_cast<size_t>(col::MenuSound::Cancel)], uint32_t(0), "AI cancels with 0");
            t.Equals(dm2->sounds[static_cast<size_t>(col::MenuSound::Cancel)], uint32_t(5), "DM2's front end cancels with 5");
            t.Equals(bmx->sounds[static_cast<size_t>(col::MenuSound::Blocked)], uint32_t(4), "BMX XXX's blocked is 4");
            t.Equals(ai->musicSample, uint32_t(0), "the front end bank's first sample is the music");
        });

        tc.Run("music plays, crossfades and stops; sounds play once", [](TestCase &t)
        {
            col::MenuAudio audio;
            std::string error;
            t.IsTrue(audio.load("ai", soundDisc("ai", col::BankLayout::Zge3), &error), "AI loads: " + error);
            t.IsTrue(audio.load("bmx", soundDisc("bmx", col::BankLayout::Zge4), &error), "BMX XXX loads: " + error);
            t.IsFalse(audio.load("dm2", scratch("none"), &error), "no banks: not loaded");
            t.IsTrue(error.find("SFXMEN00.ZSD") != std::string::npos, "says which file");
            t.IsFalse(audio.loaded("dm2"), "DM2 silent");

            std::vector<int16_t> out(2 * 4800);
            const auto peak = [&out]()
            {
                int p = 0;
                for (const int16_t s : out)
                    p = std::max(p, std::abs(static_cast<int>(s)));
                return p;
            };
            audio.render(out.data(), 4800);
            t.Equals(peak(), 0, "silent before any music");
            audio.playMusic("ai");
            audio.render(out.data(), 4800);
            const int fadingIn = peak();
            t.IsTrue(fadingIn > 0, "music starts");
            for (int i = 0; i < 5; ++i)
                audio.render(out.data(), 4800);
            t.IsTrue(peak() > fadingIn, "louder once faded in");
            for (int i = 0; i < 40; ++i)
                audio.render(out.data(), 4800);
            t.IsTrue(peak() > 0, "the loop goes on past the wave's end");
            audio.playMusic("ai");
            audio.render(out.data(), 4800);
            t.IsTrue(peak() > 0, "the same game again keeps playing");
            audio.playMusic("dm2");
            for (int i = 0; i < 6; ++i)
                audio.render(out.data(), 4800);
            t.Equals(peak(), 0, "a game not loaded: the music fades out to silence");
            audio.playMusic("");
            audio.play("ai", col::MenuSound::Select);
            audio.render(out.data(), 4800);
            t.IsTrue(peak() > 0, "a menu sound");
            audio.render(out.data(), 4800);
            t.Equals(peak(), 0, "played once, then silence");
            audio.play("dm2", col::MenuSound::Select);
            audio.render(out.data(), 4800);
            t.Equals(peak(), 0, "nothing from a game not loaded");
            audio.setLevels(0, 0);
            audio.playMusic("bmx");
            for (int i = 0; i < 6; ++i)
                audio.render(out.data(), 4800);
            t.Equals(peak(), 0, "music level 0 is silent");
        });

        tc.Run("the discs' front end music (with ZX_TEST_DISCS)", [](TestCase &t)
        {
            const char *root = std::getenv("ZX_TEST_DISCS");
            if (!root)
                return;
            // Decoded by hand from each disc's SFXMEN00.ZSD sample 0 (front1's
            // decode of the same blocks): samples, FNV-1a 64 of the PCM.
            struct Expected
            {
                const char *game;
                size_t samples;
                uint64_t hash;
            };
            static const Expected kExpected[] = {
                {"ai", 1452080, 0xe16b19ecabeeab31ull},
                {"dm2", 994476, 0xe04a430fd71f6b5cull},
                {"bmx", 705796, 0xde681307329ebe5aull},
            };
            col::MenuAudio audio;
            for (const Expected &e : kExpected)
            {
                const fs::path disc = fs::path(root) / e.game / "disc";
                if (!fs::exists(disc / "SYSTEM.CNF"))
                    continue;
                std::string error;
                t.IsTrue(audio.load(e.game, disc, &error), std::string(e.game) + " loads: " + error);
                const col::BankSound *music = audio.music(e.game);
                t.IsTrue(music && music->wave, std::string(e.game) + " has its music");
                if (!music || !music->wave)
                    continue;
                t.Equals(music->wave->pcm.size(), e.samples, std::string(e.game) + " music length");
                t.Equals(fnv(music->wave->pcm), e.hash, std::string(e.game) + " music samples");
                t.IsTrue(music->rate() > 22040.0 && music->rate() < 22060.0, std::string(e.game) + " at 22.05 kHz");
                t.IsFalse(music->wave->loopPcm.empty(), std::string(e.game) + " loops");
            }
        });
    });

    MiniTest::Case("FrontEndArt", [](TestCase &tc)
    {
        tc.Run("a library picture by name, with the PS2's alpha", [](TestCase &t)
        {
            std::vector<uint8_t> file = entry("other", "TBMP", 0x80102030u, 4, 2);
            const std::vector<uint8_t> second = entry("rider_Dave", "TZMP", 0x40102030u, 4, 2);
            file.insert(file.end(), second.begin(), second.end());
            col::Image picture;
            t.IsTrue(col::readLibraryPicture(file, "rider_Dave", picture), "found by name");
            t.Equals(picture.width, 4, "its width");
            t.Equals(picture.pixels[0] & 0x00FFFFFFu, col::rgba(0x10, 0x20, 0x30) & 0x00FFFFFFu, "its colour (B G R A in the palette)");
            t.Equals(picture.pixels[0] >> 24, uint32_t(0x80), "alpha 0x40 of 0x80: half");
            t.IsTrue(col::readLibraryPicture(file, "other", picture), "the first one too");
            t.Equals(picture.pixels[0] >> 24, uint32_t(0xFF), "TBMP is opaque");
            t.IsFalse(col::readLibraryPicture(file, "missing", picture), "a name it lacks");
            t.IsFalse(col::readLibraryPicture({1, 2, 3}, "rider_Dave", picture), "not a library");
        });

        tc.Run("a game not set up has nothing from its disc", [](TestCase &t)
        {
            for (const char *game : {"ai", "dm2", "bmx"})
            {
                const col::FrontGame none = col::loadFrontGame(game, scratch("empty") / "disc");
                t.IsFalse(none.installed, std::string(game) + " not installed");
                t.IsTrue(none.card.empty() && none.riders.empty() && none.loops.empty() && none.still.empty() && none.intro.empty(),
                         std::string(game) + ": no pictures, movies or intro");
                t.Equals(none.colour, col::gameColour(game), "its colour still");
                t.IsTrue(col::loadRosterHeads(game, scratch("empty") / "disc").empty(), "no roster");
            }
            t.IsFalse(col::loadFrontGame("ai", {}).installed, "no folder at all");
        });

        tc.Run("an installed game's files, and what it lacks", [](TestCase &t)
        {
            const fs::path disc = scratch("ai") / "disc";
            write(disc / "SYSTEM.CNF", {'B'});
            write(disc / "ASSETS/MOVIES/BACKGRND.PSM", {0});
            write(disc / "ASSETS/MOVIES/Z-AXIS.PSS", {0});
            std::vector<uint8_t> library = entry("cha_taig", "TZMP", 0x80FFFFFFu, 8, 16);
            const std::vector<uint8_t> jaren = entry("cha_jaren", "TZMP", 0x80FFFFFFu, 8, 16);
            library.insert(library.end(), jaren.begin(), jaren.end());
            write(disc / "ASSETS/UILIB.ZAL", library);
            write(disc / "ASSETS/LOADING/MAINLOAD.RAW", std::vector<uint8_t>(640 * 448 * 3, 0x40));
            const col::FrontGame ai = col::loadFrontGame("ai", disc);
            t.IsTrue(ai.installed, "installed");
            t.Equals(ai.card.width, 640, "its card: the loading screen");
            t.IsFalse(ai.cardFocus.empty(), "with the logo's part");
            t.Equals(ai.riders.size(), size_t(2), "the two cut-outs there");
            t.Equals(ai.missing.size(), size_t(8), "the other eight pros named as missing");
            t.Equals(ai.loops.size(), size_t(1), "its menu background");
            t.IsTrue(ai.loops.size() == 1 && ai.loops[0].filename() == "BACKGRND.PSM", "BACKGRND.PSM");
            t.IsTrue(ai.intro.movie.filename() == "Z-AXIS.PSS" && ai.intro.end > 5.0 && ai.intro.end < 6.0, "the logo's 5.5 s");
            t.IsTrue(ai.still.empty(), "no still with a loop");
        });

        tc.Run("BMX XXX: Xbox previews with the extras, else its still; never its own movies", [](TestCase &t)
        {
            const fs::path disc = scratch("bmx") / "disc";
            write(disc / "SYSTEM.CNF", {'B'});
            write(disc / "ASSETS/MOVIES/INTRO.PSS", {0});
            write(disc / "ASSETS/MOVIES/DM_01.PSS", {0});
            std::vector<uint8_t> library = entry("bg_MainMenu", "TZMP", 0x80FFFFFFu, 512, 256);
            const std::vector<uint8_t> dave = entry("rider_Dave", "TZMP", 0x80FFFFFFu, 8, 16);
            library.insert(library.end(), dave.begin(), dave.end());
            write(disc / "ASSETS/UILIB.ZAL", library);
            const col::FrontGame plain = col::loadFrontGame("bmx", disc);
            t.IsTrue(plain.loops.empty(), "no loop without the extras");
            t.Equals(plain.still.width, 512, "the main menu's background");
            t.IsTrue(plain.stillPart.y == 35 && plain.stillPart.y + plain.stillPart.height == 256 && plain.stillPart.width == 512,
                     "shown below its own MAIN MENU lettering (rows 18-34)");
            t.Equals(plain.riders.size(), size_t(1), "Dave");
            t.IsTrue(plain.intro.empty(), "no Z-Axis movie on the PS2 disc");

            const fs::path extras = scratch("releases");
            write(extras / "movies/ASSETS/MOVIES/MENU/NASA.PSM", {0});
            write(extras / "movies/ASSETS/MOVIES/MENU/DAM.PSM", {0});
            write(extras / "movies/ASSETS/MOVIES/ZAXIS.PSS", {0});
            const col::FrontGame xbox = col::loadFrontGame("bmx", disc, extras);
            t.Equals(xbox.loops.size(), size_t(2), "the previews there");
            t.IsTrue(xbox.loops.size() == 2 && xbox.loops[0].filename() == "DAM.PSM", "in their order");
            for (const fs::path &loop : xbox.loops)
                t.IsTrue(loop.filename() != "INTRO.PSS" && loop.filename() != "DM_01.PSS", "never the disc's movies");
            t.IsTrue(xbox.still.empty(), "no still with a loop");
            t.IsTrue(xbox.intro.movie.filename() == "ZAXIS.PSS" && xbox.intro.end == 0.0, "the Xbox logo, whole");
        });

        tc.Run("the intro takes the installed games' logos in turn", [](TestCase &t)
        {
            std::vector<col::FrontGame> games(3);
            games[0].installed = true;
            games[0].intro = {"a.pss", 0.0, 5.5};
            games[1].installed = false;
            games[1].intro = {"b.pss", 0.0, 3.3};
            games[2].installed = true;
            games[2].intro = {"c.pss", 0.0, 0.0};
            t.IsTrue(col::introFor(games, 0).movie == "a.pss", "first launch");
            t.IsTrue(col::introFor(games, 1).movie == "c.pss", "next launch, skipping a game not set up");
            t.IsTrue(col::introFor(games, 2).movie == "a.pss", "round again");
            t.IsTrue(col::introFor({}, 0).empty(), "none");
        });
    });

    MiniTest::Case("FrontEndOrder", [](TestCase &tc)
    {
        tc.Run("the games in release order: DM2, AI, BMX XXX", [](TestCase &t)
        {
            t.IsTrue(col::kGameOrder[0] == "dm2" && col::kGameOrder[1] == "ai" && col::kGameOrder[2] == "bmx", "the one list");
            t.Equals(col::gameRank("dm2"), size_t(0), "DM2 first");
            t.Equals(col::gameRank("bmx"), size_t(2), "BMX XXX last");
            t.Equals(col::gameRank("thps"), size_t(3), "an unknown game after them");
            // The build lists them ai, dm2, bmx (make-app.sh's APP_GAMES): shown dm2, ai, bmx.
            const std::vector<size_t> order = col::inGameOrder({"ai", "dm2", "bmx"});
            t.IsTrue(order == std::vector<size_t>({1, 0, 2}), "the build's list in the collection's order");
            t.IsTrue(col::inGameOrder({"x", "bmx", "y", "dm2"}) == std::vector<size_t>({3, 1, 0, 2}), "unknown ones after, in their order");
        });

        tc.Run("every front-end list follows it", [](TestCase &t)
        {
            // The cards (and so the Thor's bottom screen, which draws them in
            // the list's order) and the intro's turn.
            const fs::path root = scratch("order");
            for (const char *game : {"ai", "dm2", "bmx"})
                write(root / game / "disc" / "SYSTEM.CNF", {'B'});
            write(root / "dm2/disc/ASSETS/MOVIE/ZINTRO.PSS", {0});
            write(root / "ai/disc/ASSETS/MOVIES/Z-AXIS.PSS", {0});
            const std::vector<col::FrontGame> games = col::loadFrontGames([&root](std::string_view id) { return root / std::string(id) / "disc"; });
            t.Equals(games.size(), col::kGameOrder.size(), "every game");
            for (size_t i = 0; i < games.size() && i < col::kGameOrder.size(); ++i)
                t.IsTrue(games[i].id == col::kGameOrder[i], "card " + std::to_string(i) + " is " + std::string(col::kGameOrder[i]));
            t.IsTrue(col::introFor(games, 0).movie.filename() == "ZINTRO.PSS", "the first launch's logo is DM2's");
            t.IsTrue(col::introFor(games, 1).movie.filename() == "Z-AXIS.PSS", "then AI's");
            const col::BottomLayout l = col::bottomLayout(1240, 1080, games.size(), 0, true);
            t.IsTrue(l.cards.size() == 3 && l.cards[0].x < l.cards[1].x && l.cards[1].x < l.cards[2].x, "left to right");
            const col::BottomHit hit = col::bottomHit(l, l.cards[0].x + 4.0f, l.cards[0].y + 4.0f);
            t.IsTrue(hit.kind == col::BottomHit::Card && games[static_cast<size_t>(hit.index)].id == "dm2", "the left card is DM2's");
        });
    });

    MiniTest::Case("FrontEndScene", [](TestCase &tc)
    {

        tc.Run("the riders in pairs that take turns, a still panned, nothing for a game not set up", [](TestCase &t)
        {
            auto games = std::make_shared<std::vector<col::FrontGame>>(3);
            (*games)[0].installed = true; // riders, no movie, no still
            for (int i = 0; i < 5; ++i)
                (*games)[0].riders.push_back(picture(4, 8, col::rgba(static_cast<uint8_t>(i * 40), 0, 0)));
            (*games)[1].installed = true; // a still
            (*games)[1].still = picture(100, 50, col::rgba(0, 200, 0));
            col::MenuScene scene;
            scene.setGames(games);
            scene.show(0, 100.0);
            t.IsFalse(scene.update(100.0), "no movie: no picture to take");
            col::Backdrop b = scene.backdrop(100.0);
            t.IsTrue(b.picture == nullptr, "no movie or still: none");
            t.IsTrue(b.riders[0] == &(*games)[0].riders[0] && b.riders[1] == &(*games)[0].riders[1], "the first pair");
            t.IsTrue(b.riderOpacity == 1.0f, "at once");
            b = scene.backdrop(100.0 + col::MenuScene::kRiderTurn + col::MenuScene::kRiderFade * 2);
            t.IsTrue(b.riders[0] == &(*games)[0].riders[2] && b.riderOpacity == 1.0f, "then the next pair");
            b = scene.backdrop(100.0 + col::MenuScene::kRiderTurn + 0.1);
            t.IsTrue(b.riderOpacity > 0.0f && b.riderOpacity < 0.5f, "fading in");
            b = scene.backdrop(100.0 + 2 * col::MenuScene::kRiderTurn + 1.0);
            t.IsTrue(b.riders[0] == &(*games)[0].riders[4] && b.riders[1] == nullptr, "an odd one alone");
            b = scene.backdrop(100.0 + 3 * col::MenuScene::kRiderTurn + 1.0);
            t.IsTrue(b.riders[0] == &(*games)[0].riders[0], "round again");

            scene.show(1, 200.0);
            b = scene.backdrop(200.0);
            t.IsTrue(b.picture == &(*games)[1].still && b.part.width == 80 && b.part.x == 0, "the still, four fifths of it, from the left");
            b = scene.backdrop(200.0 + col::MenuScene::kStillPan / 2);
            t.IsTrue(b.part.x == 20, "across at half the pan");
            b = scene.backdrop(200.0 + col::MenuScene::kStillPan);
            t.IsTrue(b.part.x == 0, "and back");
            t.IsTrue(b.riders[0] == nullptr, "no riders");
            (*games)[1].stillPart = {10, 12, 50, 30};
            for (double at : {0.0, col::MenuScene::kStillPan / 4, col::MenuScene::kStillPan / 2, col::MenuScene::kStillPan * 0.9})
            {
                b = scene.backdrop(200.0 + at);
                t.IsTrue(b.part.width == 40 && b.part.y == 12 && b.part.height == 30 && b.part.x >= 10 && b.part.x + b.part.width <= 60,
                         "a still's part: four fifths of it, panned inside it");
            }
            b = scene.backdrop(200.0 + col::MenuScene::kStillPan / 2);
            t.IsTrue(b.part.x == 20, "across the part at half the pan");

            scene.show(2, 300.0);
            b = scene.backdrop(300.0);
            t.IsTrue(b.picture == nullptr && b.riders[0] == nullptr, "a game not set up: nothing from a disc");
            scene.show(-1, 300.0);
            t.Equals(scene.shown(), -1, "none shown");
        });

        tc.Run("a movie that is not there plays nothing", [](TestCase &t)
        {
            auto games = std::make_shared<std::vector<col::FrontGame>>(1);
            (*games)[0].installed = true;
            (*games)[0].loops = {"/nonexistent/BACKGRND.PSM"};
            col::MenuScene scene;
            scene.setGames(games);
            scene.show(0, 0.0);
            t.IsFalse(scene.update(0.0), "no picture");
            t.IsTrue(scene.backdrop(0.0).picture == nullptr, "none behind the menu");
            col::IntroMovie intro;
            t.IsFalse(intro.open({"/nonexistent/Z-AXIS.PSS", 0.0, 5.5}), "no intro from a missing movie");
            t.IsFalse(intro.open({}), "nor from none");
        });

        tc.Run("the cabinet draws each game's card, Set Up for one not set up, the stripes with no picture", [](TestCase &t)
        {
            std::vector<col::Tile> tiles(3);
            tiles[0].name = "Dave Mirra BMX 2";
            tiles[0].colour = col::gameColour("dm2");
            tiles[0].art = picture(64, 32, col::rgba(10, 10, 220));
            tiles[1].name = "Aggressive Inline";
            tiles[1].colour = col::gameColour("ai");
            tiles[1].installed = false;
            tiles[2].name = "BMX XXX";
            tiles[2].colour = col::gameColour("bmx");
            tiles[2].installed = false;
            col::BitmapFont font;
            col::Shell shell(3);
            shell.start();
            shell.choose(1);
            const col::Layout layout = col::layout(960, 720, 3);
            col::Canvas canvas(960, 720);
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "");
            const col::Rect &card = layout.tiles[1];
            const uint32_t inside = canvas.image().at(card.x + 3 * card.width / 4, card.y + card.height / 2);
            t.IsTrue((inside >> 16 & 0xFF) > (inside & 0xFF), "AI not set up: its card in its own blue");
            const uint32_t frame = canvas.image().at(card.x + card.width / 2, card.y - card.height / 10 + 1);
            t.IsTrue((frame >> 16 & 0xFF) > 150, "framed in its colour, grown");
            const uint32_t art = canvas.image().at(layout.tiles[0].x + layout.tiles[0].width / 2, layout.tiles[0].y + layout.tiles[0].height / 2);
            t.IsTrue((art >> 16 & 0xFF) > 60 && (art >> 16 & 0xFF) < 220, "DM2's picture on its card, dimmed when not chosen");

            // No game has a picture: the games' stripes (ours) behind the menu.
            tiles[0].art = {};
            tiles[0].installed = false;
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "");
            size_t tinted = 0;
            for (int x = 0; x < 960; x += 4)
            {
                const uint32_t p = canvas.image().at(x, 300);
                tinted += (p & 0xFF) != (p >> 16 & 0xFF);
            }
            t.IsTrue(tinted > 20, "coloured stripes across the middle");

            // A backdrop: the movie's picture behind, the riders, the launch and the fade.
            col::Image movie = picture(32, 24, col::rgba(200, 200, 0));
            col::Image rider = picture(10, 40, col::rgba(0, 255, 255));
            col::Backdrop backdrop;
            backdrop.picture = &movie;
            backdrop.riders[0] = &rider;
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "", {}, {}, col::HintDevice::Keyboard, backdrop);
            const uint32_t behind = canvas.image().at(480, 340);
            t.IsTrue((behind & 0xFF) > 60 && (behind >> 16 & 0xFF) < 40, "the movie behind, darkened");
            const col::Rect &room = layout.riderLeft;
            const uint32_t standing = canvas.image().at(room.x + room.width / 2, room.y + room.height - 4);
            t.IsTrue((standing >> 8 & 0xFF) > 200 && (standing & 0xFF) < 40, "the rider standing at the left");
            backdrop.black = 1.0f;
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "", {}, {}, col::HintDevice::Keyboard, backdrop);
            t.IsTrue((canvas.image().at(480, 340) & 0xFFFFFF) == 0, "faded to black");
            backdrop.black = 0.0f;
            backdrop.launch = 1.0f;
            tiles[1].art = picture(64, 32, col::rgba(250, 0, 0));
            tiles[1].installed = true;
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "", {}, {}, col::HintDevice::Keyboard, backdrop);
            const uint32_t grown = canvas.image().at(480, 360);
            t.IsTrue((grown & 0xFF) > 200 && (grown >> 8 & 0xFF) < 40, "launching: the chosen card over the screen");
        });

        tc.Run("the menu's settings: the intro's logo, the music, whose logo next", [](TestCase &t)
        {
            zx::app::Settings settings;
            t.IsTrue(settings.introLogo && settings.menuMusic && settings.introTurn == 0, "the logo and the music by default");
            settings.introLogo = false;
            settings.menuMusic = false;
            settings.introTurn = 7;
            const std::string text = zx::app::formatSettings(settings, zx::app::everyOptionProfile(), zx::app::SettingsScope::App);
            t.IsTrue(text.find("[menu]") != std::string::npos && text.find("intro_movie = off") != std::string::npos, "in the root file");
            const zx::app::Settings back = zx::app::parseSettings(text);
            t.IsTrue(!back.introLogo && !back.menuMusic && back.introTurn == 7, "read back");
            t.IsTrue(zx::app::appWideKey("menu", "intro_movie"), "the app's, not a game's");
            t.IsTrue(zx::app::formatSettings(settings, zx::app::everyOptionProfile(), zx::app::SettingsScope::Game).find("[menu]") == std::string::npos,
                     "not in a game's file");
        });

        tc.Run("the bottom screen: shown while the menu is up, taps on one target", [](TestCase &t)
        {
            col::BottomScreen screen;
            col::Canvas canvas(620, 540);
            t.IsFalse(screen.draw(canvas), "nothing shown: the game's panel draws");
            auto scene = std::make_shared<col::BottomScene>();
            auto games = std::make_shared<std::vector<col::FrontGame>>(3);
            for (size_t i = 0; i < 3; ++i)
                (*games)[i].colour = col::gameColour(col::kGameOrder[i]);
            scene->games = games;
            scene->names = {"Dave Mirra Freestyle BMX 2", "Aggressive Inline", "BMX XXX"};
            scene->chosen = 2;
            screen.show(scene);
            t.IsTrue(screen.draw(canvas), "drawn");
            const col::BottomLayout l = col::bottomLayout(620, 540, 3, 0, false);
            const col::Rect &card = l.cards[1];
            screen.press(card.x + 5.0f, card.y + 5.0f, 620, 540);
            screen.release(card.x + 9.0f, card.y + 9.0f, 620, 540);
            screen.press(card.x + 5.0f, card.y + 5.0f, 620, 540);
            screen.release(l.play.x + 5.0f, l.play.y + 5.0f, 620, 540);
            const std::vector<col::BottomHit> taps = screen.takeTaps();
            t.IsTrue(taps.size() == 1 && taps[0].kind == col::BottomHit::Card && taps[0].index == 1, "a tap on AI's card; a slide off is none");
            t.IsTrue(screen.takeTaps().empty(), "taken once");
            screen.show(nullptr);
            t.IsFalse(screen.draw(canvas), "a game runs: its panel again");
        });

        tc.Run("the bottom screen while a setup or update runs: its progress, no Play and no taps", [](TestCase &t)
        {
            col::BottomScreen screen;
            col::Canvas canvas(620, 540);
            auto scene = std::make_shared<col::BottomScene>();
            auto games = std::make_shared<std::vector<col::FrontGame>>(3);
            for (size_t i = 0; i < 3; ++i)
            {
                (*games)[i].installed = true;
                (*games)[i].colour = col::rgba(10, 10, 230);
            }
            scene->games = games;
            scene->names = {"Dave Mirra Freestyle BMX 2", "Aggressive Inline", "BMX XXX"};
            scene->lines = {"Done", "Building code 50%", "Waiting"};
            scene->chosen = 1;
            scene->busy = "Updating...";
            scene->busyGames = {0, 1, 2};
            scene->busyDone = {1.0f, 0.5f, 0.0f};
            screen.show(scene);
            t.IsTrue(screen.draw(canvas), "drawn");
            const col::BottomLayout l = col::bottomLayout(620, 540, 3, 0, false);
            const uint32_t play = canvas.image().at(l.play.x + l.play.width / 2, l.play.y + l.play.height / 2);
            t.IsTrue((play >> 16 & 0xFF) < 100, "no Play button in the game's colour");
            screen.press(l.play.x + 5.0f, l.play.y + 5.0f, 620, 540);
            screen.release(l.play.x + 5.0f, l.play.y + 5.0f, 620, 540);
            screen.press(l.cards[2].x + 5.0f, l.cards[2].y + 5.0f, 620, 540);
            screen.release(l.cards[2].x + 5.0f, l.cards[2].y + 5.0f, 620, 540);
            t.IsTrue(screen.takeTaps().empty(), "nothing to tap while it runs");
            scene = std::make_shared<col::BottomScene>(*scene);
            scene->busy.clear();
            screen.show(scene);
            screen.press(l.play.x + 5.0f, l.play.y + 5.0f, 620, 540);
            screen.release(l.play.x + 5.0f, l.play.y + 5.0f, 620, 540);
            const std::vector<col::BottomHit> taps = screen.takeTaps();
            t.IsTrue(taps.size() == 1 && taps[0].kind == col::BottomHit::Play, "once it ends, Play again");
        });
    });

    MiniTest::Case("FrontEndFont", [](TestCase &tc)
    {
        tc.Run("the shipped font draws as the menu's bitmap font", [](TestCase &t)
        {
            // collection/resources/fonts, in the tree the tests were built from (tests/CMakeLists.txt).
            const fs::path fontFile = fs::path(ZX_TEST_RESOURCES_DIR) / col::kShippedFontFile;
            std::ifstream in(fontFile, std::ios::binary);
            const std::vector<uint8_t> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            t.IsTrue(ttf.size() > 10000, "the TTF is in the tree");
            col::BitmapFont font;
            t.IsTrue(col::bakeFont(ttf, 32.0f, font), "baked");
            t.Equals(font.height, 32, "a line's height");
            for (char c = 0x20; c <= 0x7E; ++c)
                t.IsTrue(font.glyphs[static_cast<uint8_t>(c)].present, std::string("character ") + c);
            const int z = col::Canvas::textWidth(font, "Z-Axis Collection", 1.0f);
            t.IsTrue(z > 200 && z < 600, "a wide face: " + std::to_string(z) + " pixels");
            col::Canvas canvas(400, 60);
            canvas.clear(col::rgba(0, 0, 0));
            canvas.text(font, 4, 4, "Set Up", 1.0f, col::rgba(255, 255, 255));
            size_t lit = 0;
            for (const uint32_t p : canvas.image().pixels)
                lit += (p & 0xFF) > 128;
            t.IsTrue(lit > 100, "it writes");
            t.IsFalse(col::bakeFont({1, 2, 3, 4}, 32.0f, font), "not a font");
        });
    });

    MiniTest::Case("FrontEndBottom", [](TestCase &tc)
    {
        tc.Run("the layout fits the Thor and its targets fit a thumb", [](TestCase &t)
        {
            for (const bool freeplay : {true, false})
            {
                const col::BottomLayout l = col::bottomLayout(1240, 1080, 3, 20, freeplay);
                const col::Rect screen{0, 0, 1240, 1080};
                const auto inside = [&screen](const col::Rect &r)
                { return r.x >= screen.x && r.y >= screen.y && r.x + r.width <= screen.width && r.y + r.height <= screen.height; };
                t.Equals(l.cards.size(), size_t(3), "a card per game");
                t.Equals(l.buttons.size(), size_t(freeplay ? 4 : 3), "the pages' buttons");
                t.IsTrue(l.heads.size() <= 20 && !l.heads.empty(), "as many heads as fit");
                for (const col::Rect &r : l.cards)
                    t.IsTrue(inside(r) && r.width >= 120 && r.height >= 120, "card inside, thumb-sized");
                for (const col::Rect &r : l.buttons)
                    t.IsTrue(inside(r) && r.width >= 120 && r.height >= 120, "button inside, thumb-sized");
                for (const col::Rect &r : l.heads)
                    t.IsTrue(inside(r), "head inside");
                t.IsTrue(inside(l.play) && inside(l.hint) && inside(l.name), "play, hint and name inside");
                t.IsTrue(l.cards[0].y + l.cards[0].height < l.play.y && l.play.y + l.play.height < l.buttons[0].y &&
                             l.buttons[0].y + l.buttons[0].height < l.hint.y,
                         "cards, Play, buttons, hint top to bottom without overlap");
                t.IsTrue(l.kinds.front() == (freeplay ? col::BottomButton::CustomFreeplay : col::BottomButton::Settings), "first button");
            }
        });

        tc.Run("taps find their target", [](TestCase &t)
        {
            const col::BottomLayout l = col::bottomLayout(1240, 1080, 3, 7, true);
            const auto centre = [](const col::Rect &r) { return std::pair<float, float>(r.x + r.width / 2.0f, r.y + r.height / 2.0f); };
            auto [cx, cy] = centre(l.cards[2]);
            col::BottomHit hit = col::bottomHit(l, cx, cy);
            t.IsTrue(hit.kind == col::BottomHit::Card && hit.index == 2, "the third card");
            std::tie(cx, cy) = centre(l.play);
            t.IsTrue(col::bottomHit(l, cx, cy).kind == col::BottomHit::Play, "Play");
            std::tie(cx, cy) = centre(l.buttons[3]);
            hit = col::bottomHit(l, cx, cy);
            t.IsTrue(hit.kind == col::BottomHit::Button && hit.button == col::BottomButton::Quit, "Quit");
            t.IsTrue(col::bottomHit(l, 2.0f, 2.0f).kind == col::BottomHit::None, "nothing in the corner");
        });

        tc.Run("it draws a set-up game and one not set up", [](TestCase &t)
        {
            std::vector<col::FrontGame> games(2);
            games[0].installed = true;
            games[0].colour = col::gameColour("ai");
            games[0].card.width = 4;
            games[0].card.height = 2;
            games[0].card.pixels.assign(8, col::rgba(200, 10, 10));
            games[1].colour = col::gameColour("bmx");
            const col::BottomLayout l = col::bottomLayout(620, 540, 2, 0, false);
            col::Canvas canvas(620, 540);
            col::BottomView view;
            view.games = &games;
            view.names = {"Aggressive Inline", "BMX XXX"};
            view.chosen = 1;
            col::drawBottom(canvas, l, view, col::BitmapFont{});
            const col::Rect &play = l.play;
            const uint32_t centre = canvas.image().at(play.x + play.width / 2, play.y + play.height / 2);
            t.IsTrue((centre & 0xFF) > 150 && (centre >> 8 & 0xFF) < 80, "Play in BMX XXX's red");
            t.Equals(std::string(col::bottomPlayLabel(games[1])), std::string("Set Up"), "Set Up for a game not set up");
            t.Equals(col::bottomHelp(games[1]), std::string("Tap a game, then Set Up."), "the help follows the button");
            t.Equals(col::bottomHelp(games[0]), std::string("Tap a game, then Play."), "and for a game set up");
            const col::Rect &card = l.cards[0];
            const uint32_t art = canvas.image().at(card.x + card.width / 2, card.y + card.height / 2);
            t.IsTrue((art & 0xFF) > 60 && (art & 0xFF) < 160, "the set-up game's card, dimmed when not chosen");
        });

        tc.Run("the bottom line: one rule for every game, touch and the gamepad's button", [](TestCase &t)
        {
            std::vector<col::FrontGame> games(3);
            games[0].installed = games[1].installed = games[2].installed = true;
            for (const col::FrontGame &game : games)
            {
                t.Equals(col::bottomHint(game, std::nullopt), std::string("Tap a game, then Play."), "no gamepad: touch alone");
                t.Equals(col::bottomHint(game, col::HintDevice::Xbox), std::string("Tap a game, then Play.   A Play"),
                         "a gamepad: what both do");
            }
            col::FrontGame notSetUp;
            t.Equals(col::bottomHint(notSetUp, col::HintDevice::PlayStation), std::string("Tap a game, then Set Up.   Cross Set Up"),
                     "the gamepad's verb follows the button");
            t.Equals(col::bottomHint(games[0], col::HintDevice::Nintendo), std::string("Tap a game, then Play.   B Play"), "by position");
        });
    });
}
