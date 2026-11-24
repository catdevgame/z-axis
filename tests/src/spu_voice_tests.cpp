#include "MiniTest.h"
#include "runtime/audio_file.h"
#include "zge/native/spu_voice.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <cstdint>
#include <memory>
#include <vector>

using namespace zx;

namespace
{
    // A block of 28 equal samples: shift 0, filter 0, every nibble `nibble`.
    std::vector<uint8_t> block(uint8_t flags, uint8_t nibble)
    {
        std::vector<uint8_t> b(16, static_cast<uint8_t>(nibble | nibble << 4));
        b[0] = 0x00u;
        b[1] = flags;
        return b;
    }

    std::vector<uint8_t> sample(std::initializer_list<uint8_t> flags)
    {
        std::vector<uint8_t> out;
        for (uint8_t f : flags)
        {
            const auto b = block(f, 1u);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    }
}

void register_spu_voice_tests()
{
    MiniTest::Case("SpuVoice", [](TestCase &tc)
    {
        tc.Run("ADPCM block decode", [](TestCase &t)
        {
            uint8_t b[16] = {0x00, 0x00, 0x1F};
            int16_t out[28] = {};
            int32_t p1 = 0, p2 = 0;
            spu::decodeBlock(b, out, p1, p2);
            t.Equals(out[0], int16_t(-4096), "low nibble first (F = -1 << 12)");
            t.Equals(out[1], int16_t(4096), "then the high nibble");
            t.Equals(out[2], int16_t(0), "zero data");
            // Filter 1 (60/64 of the previous sample) carries the history on.
            uint8_t f[16] = {0x10, 0x00};
            int16_t next[28] = {};
            p1 = 4096;
            p2 = 0;
            spu::decodeBlock(f, next, p1, p2);
            t.Equals(next[0], int16_t((60 * 4096 + 32) >> 6), "prediction from the history");
            // Filters 5-15 do not exist: no prediction.
            uint8_t g[16] = {0x70, 0x00};
            p1 = 4096;
            spu::decodeBlock(g, next, p1, p2);
            t.Equals(next[0], int16_t(0), "unknown filter predicts nothing");
        });

        tc.Run("samples: loops, one-shots, no end", [](TestCase &t)
        {
            auto loop = sample({6, 2, 3, 0});
            auto decoded = spu::decodeSample(loop.data(), loop.size());
            t.Equals(decoded->pcm.size(), size_t(84), "up to the end block");
            t.Equals(decoded->loopPcm.size(), size_t(84), "loop from the start flag to the end");
            t.IsFalse(decoded->endless, "has an end");

            auto later = sample({0, 6, 3});
            decoded = spu::decodeSample(later.data(), later.size());
            t.Equals(decoded->loopStartBlock, size_t(1), "loop start where the flag is");
            t.Equals(decoded->loopPcm.size(), size_t(56), "two blocks loop");

            auto once = sample({0, 0, 1});
            decoded = spu::decodeSample(once.data(), once.size());
            t.IsTrue(decoded->loopPcm.empty(), "end without loop: no loop");

            auto open = sample({0, 0});
            decoded = spu::decodeSample(open.data(), open.size());
            t.IsTrue(decoded->endless, "no end flag: plays on");
        });

        tc.Run("envelope", [](TestCase &t)
        {
            spu::Envelope env;
            env.setRegisters(0x00FFu, 0x1FC0u); // ZAUDIO's stream voices: instant attack, hold, instant release
            env.attack();
            for (int i = 0; i < 8; ++i)
                env.step();
            t.Equals(env.value(), int32_t(0x7FFF), "full after a few samples");
            for (int i = 0; i < 48000; ++i)
                env.step();
            t.Equals(env.value(), int32_t(0x7FFF), "held");
            env.release();
            bool ended = false;
            for (int i = 0; i < 64 && !ended; ++i)
                ended = !env.step();
            t.IsTrue(ended, "release rate 0 ends at once");

            spu::Envelope slow;
            slow.setRegisters(0x7F00u, 0x0000u); // slowest exponential-free attack
            slow.attack();
            for (int i = 0; i < 1000; ++i)
                slow.step();
            t.IsTrue(slow.value() < 0x100, "slow attack still low");
        });

        tc.Run("audio files: WAV", [](TestCase &t)
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto path = std::filesystem::temp_directory_path() / ("zx-wav-" + std::to_string(now) + ".wav");
            {
                // Mono 24-bit, 22050 Hz, three samples.
                const uint8_t wav[] = {'R', 'I', 'F', 'F', 45, 0, 0, 0, 'W', 'A', 'V', 'E',
                                       'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0x22, 0x56, 0, 0, 0x66, 0x02, 0x01, 0, 3, 0, 24, 0,
                                       'd', 'a', 't', 'a', 9, 0, 0, 0, 0x00, 0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x7F, 0x00};
                std::ofstream out(path, std::ios::binary);
                out.write(reinterpret_cast<const char *>(wav), sizeof(wav));
            }
            auto reader = audio_file::open(path);
            t.IsTrue(reader != nullptr, "opens");
            if (reader)
            {
                t.Equals(reader->rate(), uint32_t(22050), "rate");
                t.Equals(reader->frames(), uint64_t(3), "frames");
                int16_t out[8] = {};
                t.Equals(reader->read(out, 4), size_t(3), "all frames");
                t.Equals(out[0], int16_t(0x1000), "24-bit to 16-bit");
                t.Equals(out[1], int16_t(0x1000), "mono on both sides");
                t.Equals(out[2], int16_t(-0x1000), "negative");
                t.Equals(out[4], int16_t(0x007F), "third");
                t.Equals(reader->read(out, 4), size_t(0), "end");
                t.IsTrue(reader->rewind() && reader->read(out, 1) == 1u, "rewind");
            }
            reader.reset(); // Windows cannot remove an open file
            std::filesystem::remove(path);
            t.IsTrue(audio_file::open(path) == nullptr, "missing file");
        });

        tc.Run("audio files: song name from the file name", [](TestCase &t)
        {
            t.Equals(audio_file::nameFromFileName("Music/01 My Song.ogg"), std::string("My Song"), "number and space");
            t.Equals(audio_file::nameFromFileName("13 - Last One.flac"), std::string("Last One"), "number and dash");
            t.Equals(audio_file::nameFromFileName("02_thing.mp3"), std::string("thing"), "underscore");
            t.Equals(audio_file::nameFromFileName("Song 2.wav"), std::string("Song 2"), "no leading number");
            t.Equals(audio_file::nameFromFileName("05.wav"), std::string("05"), "only a number");
        });

        tc.Run("voice plays a one-shot to its end", [](TestCase &t)
        {
            auto once = sample({0, 0, 1});
            spu::Voice voice;
            voice.setPitch(0x1000);
            voice.setVolume(0x3FFFu, 0x3FFFu);
            voice.setEnvelope(0x00FFu, 0x1FC0u);
            voice.keyOn(std::make_unique<spu::SampleSource>(spu::decodeSample(once.data(), once.size())));
            int samples = 0;
            int32_t peak = 0;
            while (voice.active() && samples < 1000)
            {
                int32_t l = 0, r = 0;
                voice.mix(l, r);
                peak = std::max(peak, l);
                ++samples;
            }
            t.IsFalse(voice.active(), "stopped at the end flag");
            t.Equals(voice.envelope(), int32_t(0), "silent");
            t.IsTrue(samples > 60 && samples <= 84, "after about the sample's length (the SPU2 stops while decoding ahead)");
            t.IsTrue(peak > 3000, "audible on the way");

            auto loop = sample({6, 3});
            voice.keyOn(std::make_unique<spu::SampleSource>(spu::decodeSample(loop.data(), loop.size())));
            for (int i = 0; i < 4800; ++i)
            {
                int32_t l = 0, r = 0;
                voice.mix(l, r);
            }
            t.IsTrue(voice.active(), "a loop plays on");
            voice.keyOff();
            for (int i = 0; i < 64; ++i)
            {
                int32_t l = 0, r = 0;
                voice.mix(l, r);
            }
            t.IsFalse(voice.active(), "until keyed off");
        });
    });
}
