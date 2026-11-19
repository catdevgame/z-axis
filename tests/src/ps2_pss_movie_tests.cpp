#include "MiniTest.h"
#include "runtime/ps2_audio_mix.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_pss_movie.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ps2x;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("ps2x-pss-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    using Bytes = std::vector<uint8_t>;

    void put32(Bytes &out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    void packHeader(Bytes &out)
    {
        // MPEG-2 pack header, no stuffing.
        const uint8_t pack[14] = {0, 0, 1, 0xBA, 0x44, 0, 4, 0, 4, 1, 0, 0xB2, 0x6F, 0xF8};
        out.insert(out.end(), pack, pack + 14);
    }

    // A PES packet with an MPEG-2 header: 5 bytes of PTS plus `stuffing`.
    void pes(Bytes &out, uint8_t id, const Bytes &payload, size_t stuffing = 5)
    {
        const size_t length = 3u + 5u + stuffing + payload.size();
        out.insert(out.end(), {0, 0, 1, id, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)});
        out.insert(out.end(), {0x81, 0x80, static_cast<uint8_t>(5u + stuffing), 0x21, 0, 1, 0x21, 0x8D});
        out.insert(out.end(), stuffing, 0xFFu);
        out.insert(out.end(), payload.begin(), payload.end());
    }

    // Private stream 1 payload with the disc's sub-stream header.
    Bytes sound(const Bytes &data)
    {
        Bytes out = {0xFF, 0xA0, 0x00, 0x00};
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }

    // A video stream for PssPictureCount: a 640x448 sequence (29.97, not low
    // delay), one GOP, and a frame picture of each type in `types` (I, P or
    // B) with one slice.
    Bytes videoStream(bool closedGop, const std::string &types)
    {
        Bytes es = {0, 0, 1, 0xB3, 0x28, 0x01, 0xC0, 0x24, 0xFF, 0xFF, 0xE0, 0x00};
        es.insert(es.end(), {0, 0, 1, 0xB5, 0x14, 0x8A, 0x00, 0x01, 0x00, 0x00});
        es.insert(es.end(), {0, 0, 1, 0xB8, 0x00, 0x08, 0x00, static_cast<uint8_t>(closedGop ? 0x40 : 0x00)});
        for (const char type : types)
        {
            const uint8_t code = type == 'I' ? 1 : type == 'P' ? 2 : 3;
            es.insert(es.end(), {0, 0, 1, 0x00, 0x00, static_cast<uint8_t>(code << 3), 0xFF, 0xF8});
            es.insert(es.end(), {0, 0, 1, 0xB5, 0x8F, 0xFF, 0xF3, 0x03, 0x00});
            es.insert(es.end(), {0, 0, 1, 0x01, 0x12, 0x34, 0x56});
        }
        es.insert(es.end(), {0, 0, 1, 0xB7});
        return es;
    }

    // The pictures PssPictureCount gives for `es`, fed `chunk` bytes at a time.
    size_t countPictures(const Bytes &es, size_t chunk, movie::PssPictureCount &count)
    {
        size_t shown = 0;
        for (size_t at = 0; at < es.size(); at += chunk)
        {
            count.feed(es.data() + at, std::min(chunk, es.size() - at));
            while (count.take())
                ++shown;
        }
        count.finish();
        while (count.take())
            ++shown;
        return shown;
    }
}

void register_ps2_pss_movie_tests()
{
    MiniTest::Case("PssMovie", [](TestCase &tc)
    {
        tc.Run("packets and PCM sound", [](TestCase &t)
        {
            // 2 channels, interleave 4 bytes (2 samples per channel per block).
            Bytes header;
            header.insert(header.end(), {'S', 'S', 'h', 'd'});
            put32(header, 0x18);
            put32(header, 1);     // PCM
            put32(header, 48000); // rate
            put32(header, 2);     // channels
            put32(header, 4);     // interleave
            put32(header, 0xFFFFFFFFu);
            put32(header, 0xFFFFFFFFu);
            header.insert(header.end(), {'S', 'S', 'b', 'd'});
            put32(header, 16); // two blocks
            // Block 1: L = 1, 2; R = -1, -2. Block 2: L = 3, 4; R = -3, -4.
            const int16_t samples[] = {1, 2, -1, -2, 3, 4, -3, -4};
            Bytes pcm(reinterpret_cast<const uint8_t *>(samples), reinterpret_cast<const uint8_t *>(samples) + sizeof(samples));

            Bytes file;
            packHeader(file);
            pes(file, 0xE0, {0, 0, 1, 0xB3, 0x20, 0x01, 0x00, 0x14});
            pes(file, 0xBD, sound(header));
            packHeader(file);
            pes(file, 0xBD, sound(Bytes(pcm.begin(), pcm.begin() + 6)));
            pes(file, 0xE0, {1, 2, 3});
            pes(file, 0xBD, sound(Bytes(pcm.begin() + 6, pcm.end())), 2);
            file.insert(file.end(), {0, 0, 1, 0xB9});

            TempDir dir;
            {
                std::ofstream out(dir.path / "MOVIE.PSS", std::ios::binary);
                out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
            }
            game_files::GameFileSystem files({dir.path, nullptr, {}});
            auto movie = files.open("movie.pss");
            t.IsTrue(movie != nullptr, "file opens");
            if (!movie)
                return;

            movie::PssPackets video(movie, 0xE0);
            Bytes payload;
            t.IsTrue(video.next(payload) && payload.size() == 8u && payload[3] == 0xB3u, "first video payload");
            t.IsTrue(video.next(payload) && payload == Bytes({1, 2, 3}), "second video payload");
            t.IsFalse(video.next(payload), "program end");
            video.rewind();
            t.IsTrue(video.next(payload) && payload.size() == 8u, "again after rewind");

            movie::PssAudio audio(movie);
            t.IsTrue(audio.hasSound(), "SShd header read");
            int16_t out[16] = {};
            const size_t got = audio.read(out, 8);
            t.Equals(got, size_t(4), "four stereo frames");
            const int16_t expected[] = {1, -1, 2, -2, 3, -3, 4, -4};
            t.IsTrue(std::memcmp(out, expected, sizeof(expected)) == 0, "channels de-interleaved in order");
            t.Equals(audio.read(out, 8), size_t(0), "nothing after the end");
            audio.rewind();
            t.Equals(audio.read(out, 2), size_t(2), "again after rewind");
            t.Equals(out[0], int16_t(1), "from the start");
        });

        tc.Run("audio mix input", [](TestCase &t)
        {
            int16_t frames[4] = {100, -100, 32000, -32000};
            ps2x::audio_mix::mix(frames, 2);
            t.Equals(frames[0], int16_t(100), "no source: unchanged");
            const int id = ps2x::audio_mix::addSource([](int32_t *mix, size_t count)
            {
                for (size_t i = 0; i < count * 2u; ++i)
                    mix[i] += i % 2u ? -1000 : 1000;
            });
            ps2x::audio_mix::mix(frames, 2);
            t.Equals(frames[0], int16_t(1100), "added");
            t.Equals(frames[1], int16_t(-1100), "added (right)");
            // The second frame: the SPU2's DC-blocking high-pass has started
            // to take the constant out (1000 * 0.995); both still clip.
            t.Equals(frames[2], int16_t(32767), "saturated high");
            t.Equals(frames[3], int16_t(-32768), "saturated low");
            int16_t later[2] = {0, 0};
            ps2x::audio_mix::mix(later, 1);
            t.Equals(later[0], int16_t(990), "a constant decays (1000 * 0.995^2)");
            ps2x::audio_mix::removeSource(id);
            t.IsFalse(ps2x::audio_mix::hasSources(), "removed");
        });
        tc.Run("pictures counted from the headers", [](TestCase &t)
        {
            // Open GOP: the two B-pictures before the first P have no
            // reference before them and FFmpeg skips them; I is shown when P
            // starts, P at the end.
            for (const size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(4096)})
            {
                movie::PssPictureCount count;
                t.Equals(countPictures(videoStream(false, "IBBPBB"), chunk, count), size_t(4),
                         "open GOP: 4 of 6 pictures, " + std::to_string(chunk) + " bytes at a time");
                t.Equals(count.width(), 640, "width");
                t.Equals(count.height(), 448, "height");
                t.IsTrue(count.framesPerSecond() > 29.96 && count.framesPerSecond() < 29.98, "29.97 frames per second");
            }
            movie::PssPictureCount closed;
            t.Equals(countPictures(videoStream(true, "IBBPBB"), 5, closed), size_t(6), "closed GOP: every picture");
            movie::PssPictureCount references;
            t.Equals(countPictures(videoStream(false, "IPPP"), 2, references), size_t(4), "I and P only: every picture");
        });

        tc.Run("black pictures as many as decoded (with PS2X_TEST_MOVIES)", [](TestCase &t)
        {
            // Every PSS and PSM under the folder (the discs' movies): the
            // decoder's pictures against the count from the headers.
            const char *root = std::getenv("PS2X_TEST_MOVIES");
            if (!root)
                return;
            std::vector<std::filesystem::path> movies;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                if (entry.is_regular_file() && (ext == ".PSS" || ext == ".PSM"))
                    movies.push_back(entry.path());
            }
            std::sort(movies.begin(), movies.end());
            t.IsTrue(!movies.empty(), "movies found");
            for (const auto &path : movies)
            {
                game_files::GameFileSystem files({path.parent_path(), nullptr, {}});
                auto file = files.open(path.filename().string());
                t.IsTrue(file != nullptr, path.string() + " opens");
                if (!file)
                    continue;
                movie::PssVideo decoded(file, true), black(file, false);
                movie::Picture a, b;
                size_t pictures = 0, blacks = 0;
                bool sameSize = true;
                uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a over the decoded pictures, for comparing builds
                for (;;)
                {
                    const bool gotA = decoded.next(a), gotB = black.next(b);
                    if (gotA)
                        for (const uint8_t byte : a.rgba)
                            hash = (hash ^ byte) * 0x100000001B3ull;
                    pictures += gotA;
                    blacks += gotB;
                    if (gotA && gotB && (a.width != b.width || a.height != b.height))
                        sameSize = false;
                    if (!gotA && !gotB)
                        break;
                }
                std::cout << "  " << path.string() << ": " << pictures << " decoded, " << blacks << " counted, " << b.width << "x" << b.height
                          << ", " << black.framesPerSecond() << " fps, pictures hash " << std::hex << hash << std::dec << std::endl;
                t.Equals(blacks, pictures, path.string() + ": as many pictures");
                t.IsTrue(sameSize, path.string() + ": the same size");
                t.Equals(black.framesPerSecond(), decoded.framesPerSecond(), path.string() + ": the same rate");
            }
        });
    });
}
