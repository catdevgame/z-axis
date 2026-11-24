// The ZAudio sound driver on the host, shared by the three games (see
// zge_sound.h). ZAUDIO.IRX was decompiled for the semantics (Aggressive
// Inline's and Dave Mirra BMX 2's from their images, BMX XXX's from its own,
// with its symbol table), the SPU2's voices are zge/native/spu_voice.h's.
//
// The commands (fn):
//   0 Init {status on, status address} / 1 Reset;
//   2 Alloc {size} / 3 Free {handle} / 4 Load {handle, offset, 1 KB}: sample
//     memory, 1 KB uploads of PS-ADPCM, decoded to PCM when a voice first
//     plays it;
//   5 ChanSetParam {channel, flags, sample, pitch, volumes, ADSR}, 6 KON /
//     7 KOFF {40-channel mask}: the voices as the SPU2 plays them;
//   8 StrmAlloc / 9 StrmFree, and 10 SetParam (pitch, loop count, source,
//     volume, fade), 11 Play, 12 Stop, 13 Pause, queued for the next tick as
//     ZAUDIO queues them: streams read through the game file layer (so the
//     mods folder applies) and decoded as they play;
//   14 / 15: status writes off / on (around movies).
// The status is written at every VBlank (guest time): one byte per channel
// at Init's address (4 envelope running, 1 silent), which drives the EE's
// channel state machine (soundTask: freeing, stealing), and 16 bytes per
// stream {state, 2, progress, total}, which the music polls (state 2,
// stopped, starts the next song). The sound is made in guest time (one
// 48 kHz frame per 6144 EE cycles) up to each command and VBlank, mixed into
// the effects (voices), music and speech (streams, by their file) buses and
// handed to the host's output with the movie sound on top (ps2_audio_mix).
#include "zge_sound.h"
#include "zge_host.h"
#include "zge/sound/driver_commands.h" // the commands and records as the engine sends them

#include "game_overrides.h"
#include "machine.h"
#include "runtime/ee_scheduler.h"
#include "runtime/audio_file.h"
#include "runtime/boot_config.h"
#include "runtime/frame_trace.h"
#include "runtime/game_files.h"
#include "runtime/session.h"
#include "runtime/zip_archive.h"
#include "zge/native/spu_voice.h"
#include "zge_sites.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace zge_host::sound
{
    namespace
    {
        namespace gf = zx::game_files;
        namespace spu = zx::spu;

        constexpr int kRegA0 = 4, kRegA1 = 5, kRegA2 = 6, kRegA3 = 7, kRegSp = 29, kRegRa = 31;

        constexpr uint64_t kEeCyclesPerFrame = EeScheduler::kEeClockHz / 48000u; // 6144
        constexpr size_t kChannels = 40;
        constexpr size_t kStreams = 4;
        constexpr uint32_t kSampleStride = 0x10u; // a sample table entry

        // ZAudio's commands and its records are the engine's own definition (zge/sound/driver_commands.h).
        using namespace zge::zaudio;

        // What a stream plays from (StrmAlloc's flags).
        enum class Source : uint32_t
        {
            None,    // no source (4): the EE feeds it
            Sectors, // disc sectors: offsets and lengths in 2 KB units
            Bytes,   // file bytes (8)
        };

        // ZAUDIO's SPU memory: 1 KB pages from 0x8000, first fit.
        constexpr uint32_t kPages = 0x7E0; // 2,016
        constexpr uint32_t kPageBytes = 0x400u;
        constexpr uint32_t kSpuSampleBase = 0x8000u;
        constexpr uint32_t kRingBytes = 0x10000u;      // a stream ring: two halves
        constexpr uint32_t kHalfBytes = kRingBytes / 2u; // a ring half (per channel)
        constexpr uint32_t kHalfBlocks = kHalfBytes / spu::kBlockBytes;
        constexpr uint32_t kSectorBytes = 0x800u;    // also the size of a stereo source's left/right pieces

        uint32_t rd32(uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void wr32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        void readGuest(uint8_t *rdram, uint32_t address, void *out, size_t size)
        {
            auto *bytes = static_cast<uint8_t *>(out);
            for (size_t i = 0; i < size; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, address + static_cast<uint32_t>(i));
                bytes[i] = p ? *p : 0u;
            }
        }

        void writeGuestString(uint8_t *rdram, uint32_t address, const std::string &text, size_t size)
        {
            for (size_t i = 0; i < size; ++i)
                if (uint8_t *p = getMemPtr(rdram, address + static_cast<uint32_t>(i)))
                    *p = i < text.size() ? static_cast<uint8_t>(text[i]) : 0u;
        }

        void returnTo(R5900Context *ctx, uint32_t value)
        {
            setReturnS32(ctx, static_cast<int32_t>(value));
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        bool iequals(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                              { return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y)); });
        }

        // One boot's switches, logs, files and bus gains.
        struct State
        {
            const Game *game = nullptr;
            bool trace = false;
            int traceLevel = 0;
            std::FILE *log = nullptr;     // ZX_SOUND_LOG
            std::FILE *memDump = nullptr; // ZX_SOUND_MEMDUMP: every allocation and free with the vsync tick, and
                                          // the table with each sample's bytes hashed every 300 ticks when it changed
            PS2Machine::RecompiledFunction originalRpc = nullptr; // (the log with ZAUDIO on the IOP)
            PS2Machine::RecompiledFunction originalPlaylistEntry = nullptr;
            PS2Machine::RecompiledFunction originalStringTableText = nullptr;
            // The custom songs' texts by their keys' nameHash (Game::songKey),
            // and where they are in guest memory once the game first asks.
            struct SongText
            {
                uint32_t wave = 0;
                bool title = false;
                std::string text;
                uint32_t address = 0;
            };
            std::map<uint32_t, SongText> songTexts;
            bool songTextsPlaced = false;
            bool addedSongTexts = false; // the added songs' are in songTexts
            uint32_t logStatusAddress = 0;                 // the channel status bytes (Init's), for the log
            std::array<uint8_t, kChannels> loggedStatus{}; // and those last logged
            std::unique_ptr<gf::GameFileSystem> files;
            std::array<std::atomic<float>, BusCount> busGain{1.0f, 1.0f, 1.0f};

            ~State()
            {
                for (std::FILE *file : {log, memDump})
                    if (file)
                        std::fclose(file);
            }
        };
        State &state() { return zx::session::state<State>(); }
        const Game &game() { return *state().game; }

        gf::GameFileSystem &files()
        {
            std::unique_ptr<gf::GameFileSystem> &fileSystem = state().files;
            if (!fileSystem)
            {
                const PS2Machine::IoPaths &paths = PS2Machine::getIoPaths();
                gf::GameFileRoots roots;
                if (!paths.cdFromImage)
                    roots.disc = paths.cdRoot;
                if (roots.disc.empty())
                    roots.image = PS2Machine::cdImageFileSystem();
                if (const char *mods = zx::boot::text("ZX_MODS_DIR"); mods && *mods)
                    roots.mods = gf::modsRoots(mods);
                fileSystem = std::make_unique<gf::GameFileSystem>(std::move(roots));
            }
            return *fileSystem;
        }

        // ---- what a stream's voices read -------------------------------------------------

        // A stream's ADPCM as its voices take it, block by block per channel
        // (block flags ignored: ZAUDIO rewrote them for its ring).
        class StreamBlocks
        {
        public:
            virtual ~StreamBlocks() = default;
            // The next 16 bytes of a channel; false at the end.
            virtual bool next(int channel, uint8_t *block) = 0;
            // Blocks channel 0 has taken since the last (re)start.
            [[nodiscard]] uint64_t blocks() const noexcept { return m_blocks; }

        protected:
            struct Queue
            {
                std::vector<uint8_t> bytes;
                size_t at = 0;
            };

            bool take(Queue &q, int channel, uint8_t *block)
            {
                std::memcpy(block, q.bytes.data() + q.at, spu::kBlockBytes);
                q.at += spu::kBlockBytes;
                if (channel == 0)
                    ++m_blocks;
                return true;
            }
            void dropTaken()
            {
                for (Queue &q : m_queues)
                {
                    q.bytes.erase(q.bytes.begin(), q.bytes.begin() + static_cast<std::ptrdiff_t>(q.at));
                    q.at = 0;
                }
            }

            std::array<Queue, 2> m_queues;
            uint64_t m_blocks = 0;
        };

        // ZGE3's: `bytes` from `start` in a game file read on as the voices
        // take it, 64 KB at a time, stereo as 0x800-byte left/right pieces;
        // at the end the stream says whether to go round again (its loop
        // count).
        class SourceBlocks final : public StreamBlocks
        {
        public:
            using LoopQuery = std::function<bool()>;

            SourceBlocks(std::shared_ptr<gf::GameFile> file, uint64_t start, uint64_t bytes, bool stereo, LoopQuery loop)
                : m_file(std::move(file)), m_start(start), m_bytes(bytes), m_stereo(stereo), m_loop(std::move(loop))
            {
            }

            bool next(int channel, uint8_t *block) override
            {
                Queue &q = m_queues[channel & 1];
                if (q.at + spu::kBlockBytes > q.bytes.size() && !refill())
                    return false;
                if (q.at + spu::kBlockBytes > q.bytes.size())
                    return false;
                return take(q, channel, block);
            }

        private:
            bool refill()
            {
                if (m_position >= m_bytes)
                {
                    if (m_ended || !m_loop || !m_loop())
                    {
                        m_ended = true;
                        return false;
                    }
                    m_position = 0;
                    m_blocks = 0;
                }
                const size_t want = static_cast<size_t>(std::min<uint64_t>(m_bytes - m_position, kRingBytes));
                std::vector<uint8_t> chunk(want, 0u);
                size_t got = 0u;
                {
                    zx::ftrace::Scope traceRead(zx::ftrace::kDiscRead, zx::ftrace::kDiscRead + 100, 1);
                    got = m_file ? m_file->read(m_start + m_position, chunk.data(), want) : 0u;
                }
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(), uint8_t{0}); // past the file: silence
                m_position += want;
                dropTaken();
                if (!m_stereo)
                {
                    m_queues[0].bytes.insert(m_queues[0].bytes.end(), chunk.begin(), chunk.end());
                    return true;
                }
                for (size_t at = 0; at < chunk.size(); at += kSectorBytes)
                {
                    const size_t n = std::min<size_t>(kSectorBytes, chunk.size() - at);
                    Queue &q = m_queues[(at / kSectorBytes) & 1u];
                    q.bytes.insert(q.bytes.end(), chunk.begin() + static_cast<std::ptrdiff_t>(at),
                                   chunk.begin() + static_cast<std::ptrdiff_t>(at + n));
                }
                return true;
            }

            std::shared_ptr<gf::GameFile> m_file;
            uint64_t m_start = 0;
            uint64_t m_bytes = 0;
            uint64_t m_position = 0;
            bool m_stereo = false;
            bool m_ended = false;
            LoopQuery m_loop;
        };

        // DM2's: what ZAUDIO read into its ring, piece by piece in the order
        // the voices play it: each piece 64 KB from `offset` in the source,
        // `valid` bytes of it from the file and zeros after. Stereo:
        // 0x800-byte left/right pieces; mono: the even ones (the odd ones go
        // to the right ring, which no voice plays).
        class RingPieces final : public StreamBlocks
        {
        public:
            RingPieces(std::shared_ptr<gf::GameFile> file, uint64_t start, bool stereo)
                : m_file(std::move(file)), m_start(start), m_stereo(stereo)
            {
            }

            void push(uint64_t offset, uint32_t valid) { m_pieces.push_back({offset, std::min(valid, kRingBytes)}); }

            bool next(int channel, uint8_t *block) override
            {
                Queue &q = m_queues[channel & 1];
                while (q.at + spu::kBlockBytes > q.bytes.size())
                    if (!load())
                        return false;
                return take(q, channel, block);
            }

        private:
            struct Piece
            {
                uint64_t offset;
                uint32_t valid;
            };

            bool load()
            {
                if (m_pieces.empty())
                    return false;
                const Piece piece = m_pieces.front();
                m_pieces.pop_front();
                std::vector<uint8_t> chunk(kRingBytes, 0u);
                if (m_file && piece.valid)
                    m_file->read(m_start + piece.offset, chunk.data(), piece.valid);
                dropTaken();
                for (size_t at = 0; at < chunk.size(); at += kSectorBytes)
                {
                    const size_t side = (at / kSectorBytes) & 1u;
                    if (!m_stereo && side)
                        continue;
                    Queue &q = m_queues[side];
                    q.bytes.insert(q.bytes.end(), chunk.begin() + static_cast<std::ptrdiff_t>(at),
                                   chunk.begin() + static_cast<std::ptrdiff_t>(at + kSectorBytes));
                }
                return true;
            }

            std::shared_ptr<gf::GameFile> m_file;
            uint64_t m_start = 0;
            bool m_stereo = false;
            std::deque<Piece> m_pieces;
        };

        // A stream channel as an SPU2 voice reads it: ADPCM decoded as it plays.
        class StreamChannel final : public spu::BlockSource
        {
        public:
            StreamChannel(std::shared_ptr<StreamBlocks> data, int channel) : m_data(std::move(data)), m_channel(channel) {}

            bool nextBlock(int16_t *out) override
            {
                uint8_t block[spu::kBlockBytes];
                if (!m_data || !m_data->next(m_channel, block))
                    return false;
                spu::decodeBlock(block, out, m_prev1, m_prev2);
                return true;
            }

        private:
            std::shared_ptr<StreamBlocks> m_data;
            int m_channel = 0;
            int32_t m_prev1 = 0;
            int32_t m_prev2 = 0;
        };

        // ---- a replacement's PCM --------------------------------------------------------

        // Both channels of a replacement (a song of the player's, a line from
        // another release), pulled in step as a stream's blocks are.
        class TrackData
        {
        public:
            TrackData(std::unique_ptr<zx::audio_file::Reader> reader, SourceBlocks::LoopQuery loop)
                : m_reader(std::move(reader)), m_loop(std::move(loop))
            {
            }

            bool next(int channel, int16_t *out)
            {
                Queue &q = m_queues[channel & 1];
                while (q.at + spu::kSamplesPerBlock > q.samples.size())
                    if (!refill())
                        return false;
                std::memcpy(out, q.samples.data() + q.at, spu::kSamplesPerBlock * sizeof(int16_t));
                q.at += spu::kSamplesPerBlock;
                if (channel == 0)
                    m_played += spu::kSamplesPerBlock;
                return true;
            }

            // How much of it has played, 0 .. 1.
            [[nodiscard]] double fraction() const
            {
                const uint64_t total = m_reader ? m_reader->frames() : 0u;
                return total ? std::min(1.0, static_cast<double>(m_played) / static_cast<double>(total)) : 1.0;
            }

        private:
            struct Queue
            {
                std::vector<int16_t> samples;
                size_t at = 0;
            };

            // The next frames onto both queues, as they come (a block may
            // span two reads, and the start again after the end when it
            // loops); at the very end the last part of a block is padded with
            // silence. False when nothing more comes.
            bool refill()
            {
                if (!m_reader || m_ended)
                    return false;
                constexpr size_t kFrames = 8192;
                std::vector<int16_t> frames(kFrames * 2u);
                size_t got = m_reader->read(frames.data(), kFrames);
                if (got == 0u && m_loop && m_loop())
                {
                    m_reader->rewind();
                    m_played = 0;
                    got = m_reader->read(frames.data(), kFrames);
                }
                bool more = got > 0u;
                for (int c = 0; c < 2; ++c)
                {
                    Queue &q = m_queues[c];
                    q.samples.erase(q.samples.begin(), q.samples.begin() + static_cast<std::ptrdiff_t>(q.at));
                    q.at = 0;
                    for (size_t i = 0; i < got; ++i)
                        q.samples.push_back(frames[i * 2u + static_cast<size_t>(c)]);
                    if (got == 0u && q.samples.size() % spu::kSamplesPerBlock != 0u)
                    {
                        q.samples.resize((q.samples.size() / spu::kSamplesPerBlock + 1u) * spu::kSamplesPerBlock, int16_t{0});
                        more = true;
                    }
                }
                if (got == 0u)
                    m_ended = true;
                return more;
            }

            std::unique_ptr<zx::audio_file::Reader> m_reader;
            SourceBlocks::LoopQuery m_loop;
            std::array<Queue, 2> m_queues;
            uint64_t m_played = 0;
            bool m_ended = false;
        };

        class TrackChannel final : public spu::BlockSource
        {
        public:
            TrackChannel(std::shared_ptr<TrackData> data, int channel) : m_data(std::move(data)), m_channel(channel) {}
            bool nextBlock(int16_t *out) override { return m_data && m_data->next(m_channel, out); }

        private:
            std::shared_ptr<TrackData> m_data;
            int m_channel = 0;
        };

        // ---- the custom soundtrack ------------------------------------------------------

        // A mod folder's (or zip's) Music folder: the files this build can
        // read, in name order (names case-insensitive).
        std::vector<std::filesystem::path> musicFolderFiles(const std::filesystem::path &root)
        {
            const auto readable = zx::audio_file::extensions();
            auto audio = [&](const std::string &name)
            {
                std::string ext = std::filesystem::path(name).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return std::find(readable.begin(), readable.end(), ext) != readable.end();
            };
            std::vector<std::filesystem::path> here;
            // A mod packed as a zip (runtime/zip_archive.h): its Music
            // folder's files, as paths through the zip.
            if (const zx::game_files::ZipPath zip = zx::game_files::zipPathOf(root); zip.zip)
            {
                const std::string music = zip.inside.empty() ? std::string("MUSIC") : zip.inside + "/MUSIC";
                for (const std::string &file : zip.zip->filesBelow(music))
                    if (file.find('/') == std::string::npos && audio(file))
                        here.push_back(root / "Music" / file);
                std::sort(here.begin(), here.end());
                return here;
            }
            std::error_code ec;
            std::filesystem::path dir;
            for (const auto &entry : std::filesystem::directory_iterator(root, ec))
                if (entry.is_directory(ec) && iequals(entry.path().filename().string(), "Music"))
                    dir = entry.path();
            if (dir.empty())
                return here;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
                if (entry.is_regular_file(ec) && audio(entry.path().filename().string()))
                    here.push_back(entry.path());
            std::sort(here.begin(), here.end());
            return here;
        }

        // The mod folders of this boot (ZX_MODS_DIR, game_files.h), in its order.
        std::vector<std::filesystem::path> modRoots()
        {
            const char *modsText = zx::boot::text("ZX_MODS_DIR");
            if (!modsText || !*modsText)
                return {};
            return zx::game_files::modsRoots(modsText);
        }

        // The song a Music folder's file replaces: its name's two-digit
        // number NN (not followed by a third digit) when it is one of the
        // game's songs (1 .. Game::songs), as MUSIC.ZSD wave NN-1; -1 for
        // any other file, which is a song the mod adds.
        int replacedWave(const std::filesystem::path &file)
        {
            const std::string name = file.filename().string();
            auto digit = [&name](size_t i) { return i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])); };
            if (!digit(0) || !digit(1) || digit(2))
                return -1;
            const int number = (name[0] - '0') * 10 + (name[1] - '0');
            return number >= 1 && static_cast<uint32_t>(number) <= game().songs ? number - 1 : -1;
        }

        // <mod>/Music/NN*.wav (.ogg/.flac/.mp3 with FFmpeg's libavformat)
        // for MUSIC.ZSD wave NN-1, names case-insensitive; the files this
        // build reads, in name order, the last mod folder's first
        // (ZX_MODS_DIR, game_files.h).
        std::vector<std::filesystem::path> customTrackFiles(uint32_t wave)
        {
            std::vector<std::filesystem::path> candidates;
            const std::vector<std::filesystem::path> mods = modRoots();
            for (auto root = mods.rbegin(); root != mods.rend(); ++root)
                for (const std::filesystem::path &file : musicFolderFiles(*root))
                    if (replacedWave(file) == static_cast<int>(wave))
                        candidates.push_back(file);
            return candidates;
        }

        std::string songNumber(uint32_t wave)
        {
            char text[8];
            std::snprintf(text, sizeof(text), "%02u", wave + 1u);
            return text;
        }

        std::unique_ptr<zx::audio_file::Reader> openCustomTrack(uint32_t wave, std::filesystem::path *opened = nullptr)
        {
            for (const auto &path : customTrackFiles(wave))
            {
                std::string error;
                if (auto track = zx::audio_file::open(path, &error))
                {
                    std::cout << "[sound] custom soundtrack: song " << songNumber(wave) << " from " << path.string() << " ("
                              << track->rate() << " Hz)" << std::endl;
                    if (opened)
                        *opened = path;
                    return track;
                }
                std::cerr << "[sound] cannot play " << path.string() << ": " << error << std::endl;
            }
            return nullptr;
        }

        // MUSIC.ZSD's wave table: (file offset, size) per wave.
        std::vector<std::pair<uint32_t, uint32_t>> musicWaves(gf::GameFile &file)
        {
            std::vector<std::pair<uint32_t, uint32_t>> waves;
            uint8_t header[0x40];
            if (file.read(0, header, sizeof(header)) != sizeof(header) || std::memcmp(header, "ZSNDPS2 ", 8) != 0)
                return waves;
            uint32_t count = 0, table = 0;
            std::memcpy(&count, header + game().bankWaveCount, 4);
            std::memcpy(&table, header + game().bankWaves, 4);
            if (count > 4096u)
                return waves;
            std::vector<uint8_t> entries(count * 8u);
            if (file.read(table, entries.data(), entries.size()) != entries.size())
                return waves;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t offset = 0, size = 0;
                std::memcpy(&offset, entries.data() + i * 8u, 4);
                std::memcpy(&size, entries.data() + i * 8u + 4u, 4);
                waves.emplace_back(offset, size);
            }
            return waves;
        }

        // The player's song for a music stream: the song whose wave starts
        // where the stream does.
        Replacement addedSong(const StreamSource &source);

        Replacement customSong(const StreamSource &source)
        {
            Replacement replacement = addedSong(source);
            if (replacement.reader || source.bus != Music || !source.stereo || !source.file || game().songs == 0u)
                return replacement;
            const auto waves = musicWaves(*source.file);
            for (uint32_t i = 0; i < std::min<uint32_t>(game().songs, static_cast<uint32_t>(waves.size())); ++i)
            {
                if (waves[i].first != source.start)
                    continue;
                replacement.reader = openCustomTrack(i);
                replacement.ownRate = true;
                break;
            }
            return replacement;
        }

        // ---- the songs a mod adds ---------------------------------------------------------

        // Every other file of the mods' Music folders (replacedWave -1: a
        // number past the game's songs, or none) is a song the game plays
        // after its own: MUSIC.ZSD is served with a wave more for each,
        // after its last (its tables' head, zx::game_files::registerHead;
        // the file is read through, never copied), sized as a PS2 music
        // wave of the song's length and placed past the file's end, so
        // nothing of the disc's is read for it. A stream of that wave plays
        // the file (addedSong). The game's soundtrack then counts them where
        // it counts MUSIC.ZSD's waves (Dave Mirra BMX 2, BMX XXX: they come
        // in their turn); Aggressive Inline's playlist is its 13 songs, and
        // zge_soundtrack.cpp plays them after those.
        constexpr const char kMusicBankPath[] = "ASSETS/AUDIO/MUSIC.ZSD";
        constexpr const char kAddedSongsOwner[] = "zge-sound-added-songs";

        struct AddedSongs
        {
            std::vector<AddedSong> songs; // in the mods' order (ZX_MODS_DIR), each folder's in name order
            uint32_t firstWave = 0;       // MUSIC.ZSD's wave of the first, once its head is made
            std::atomic<bool> made{false};
        };
        AddedSongs &addedSongList() { return zx::session::state<AddedSongs>(); }

        // The songs a mod adds, with their titles (the tags, else the file's
        // name) and lengths: a file this build cannot read is left out, and
        // said so.
        void findAddedSongs()
        {
            AddedSongs &added = addedSongList();
            added.songs.clear();
            for (const std::filesystem::path &root : modRoots())
                for (const std::filesystem::path &file : musicFolderFiles(root))
                {
                    if (replacedWave(file) >= 0)
                        continue;
                    std::string error;
                    const auto reader = zx::audio_file::open(file, &error);
                    if (!reader || reader->rate() == 0u || reader->frames() == 0u)
                    {
                        std::cerr << "[sound] added song " << file.string() << " left out: "
                                  << (reader ? std::string("its length is not known") : error) << std::endl;
                        continue;
                    }
                    AddedSong song;
                    song.file = file;
                    song.title = reader->tag("title");
                    if (song.title.empty())
                        song.title = zx::audio_file::nameFromFileName(file);
                    song.artist = reader->tag("artist");
                    song.album = reader->tag("album");
                    song.seconds = static_cast<double>(reader->frames()) / static_cast<double>(reader->rate());
                    added.songs.push_back(std::move(song));
                }
        }

        // A song's length as a PS2 music wave of it: stereo PS-ADPCM at
        // 48 kHz (16 bytes per 28 samples a channel), whole sectors.
        uint32_t ps2MusicBytes(double seconds)
        {
            const uint64_t samples = static_cast<uint64_t>(seconds * 48000.0);
            const uint64_t bytes = (samples + 27u) / 28u * 16u * 2u;
            return static_cast<uint32_t>(std::min<uint64_t>((bytes + kSectorBytes - 1u) / kSectorBytes * kSectorBytes, 0x7FFFF800u));
        }

        uint32_t headWord(const std::vector<uint8_t> &head, size_t at)
        {
            uint32_t value = 0;
            if (at + 4u <= head.size())
                std::memcpy(&value, head.data() + at, 4);
            return value;
        }
        void putHeadWord(std::vector<uint8_t> &head, size_t at, uint32_t value)
        {
            if (at + 4u <= head.size())
                std::memcpy(head.data() + at, &value, 4);
        }

        // Where the added waves go: past the file's end and past every wave
        // its tables already list (another head's added ones too), on a
        // sector.
        uint64_t addedWavesStart(uint64_t fileSize, const std::vector<uint8_t> &waveTable)
        {
            uint64_t end = fileSize;
            for (size_t at = 0; at + 8u <= waveTable.size(); at += 8u)
                end = std::max<uint64_t>(end, uint64_t(headWord(waveTable, at)) + headWord(waveTable, at + 4u));
            return (end + kSectorBytes - 1u) / kSectorBytes * kSectorBytes;
        }

        // The added waves' table entries ({offset, size} each), their
        // starts noted, from `at`; the end of the last.
        uint64_t appendAddedWaves(std::vector<uint8_t> &waveTable, uint64_t at)
        {
            for (AddedSong &song : addedSongList().songs)
            {
                const uint32_t size = ps2MusicBytes(song.seconds);
                song.start = at;
                const size_t entry = waveTable.size();
                waveTable.resize(entry + 8u);
                putHeadWord(waveTable, entry, static_cast<uint32_t>(at));
                putHeadWord(waveTable, entry + 4u, size);
                at += size;
            }
            return at;
        }

        // ZGE3's bank (AI, DM2): "ZSNDPS2 ", the file's size (+0x08), the
        // tables' end (+0x0C), then seven {count, offset} tables, the waves'
        // the third ({offset, size} a wave); the waves from their first's
        // offset on. Null when another table is not empty, the waves' is not
        // the last, or there is no room before the first wave.
        std::shared_ptr<const std::vector<uint8_t>> pairsHead(const std::vector<uint8_t> &head, uint64_t fileSize)
        {
            constexpr size_t kTables = 7, kWaveTable = 2, kHeadBytes = 0x48;
            AddedSongs &added = addedSongList();
            const uint32_t tableBytes = headWord(head, 0x0C);
            for (size_t t = 0; t < kTables; ++t)
                if (t != kWaveTable && headWord(head, 0x10 + 8 * t) != 0)
                    return nullptr;
            const uint32_t waves = headWord(head, 0x10 + 8 * kWaveTable), table = headWord(head, 0x14 + 8 * kWaveTable);
            if (table != kHeadBytes || tableBytes != table + 8u * waves || waves == 0u)
                return nullptr;
            const uint32_t firstWave = headWord(head, table);
            const uint32_t newTableBytes = tableBytes + 8u * static_cast<uint32_t>(added.songs.size());
            if (newTableBytes > firstWave || firstWave > head.size())
                return nullptr;
            std::vector<uint8_t> waveTable(head.begin() + table, head.begin() + tableBytes);
            const uint64_t end = appendAddedWaves(waveTable, addedWavesStart(fileSize, waveTable));
            std::vector<uint8_t> out(head.begin(), head.begin() + firstWave);
            std::copy(waveTable.begin(), waveTable.end(), out.begin() + table);
            for (size_t t = 0; t < kTables; ++t)
                if (t != kWaveTable && headWord(out, 0x14 + 8 * t) == tableBytes)
                    putHeadWord(out, 0x14 + 8 * t, newTableBytes); // (the empty tables after the waves' move on)
            putHeadWord(out, 0x10 + 8 * kWaveTable, waves + static_cast<uint32_t>(added.songs.size()));
            putHeadWord(out, 0x0C, newTableBytes);
            putHeadWord(out, 0x08, static_cast<uint32_t>(std::min<uint64_t>(end, 0xFFFFFFFFu)));
            added.firstWave = waves;
            return std::make_shared<const std::vector<uint8_t>>(std::move(out));
        }

        // ZGE4's bank (BMX XXX; engine/zge/sound/bank.h): "ZSNDPS2 ", the
        // file's size (+0x08), the tables' end (+0x0C), then seven {count,
        // hash index, table} tables (the hash index {hash, number} an entry),
        // the waves' the third; the tables from +0x64. The added waves get
        // hashes after the largest there. Null as pairsHead.
        std::shared_ptr<const std::vector<uint8_t>> triplesHead(const std::vector<uint8_t> &head, uint64_t fileSize)
        {
            constexpr size_t kTables = 7, kWaveTable = 2, kHeadBytes = 0x64;
            AddedSongs &added = addedSongList();
            const uint32_t tableBytes = headWord(head, 0x0C);
            uint32_t hashes[kTables], tables[kTables];
            for (size_t t = 0; t < kTables; ++t)
            {
                hashes[t] = headWord(head, 0x14 + 12 * t);
                tables[t] = headWord(head, 0x18 + 12 * t);
                if (t != kWaveTable && headWord(head, 0x10 + 12 * t) != 0)
                    return nullptr;
            }
            const uint32_t waves = headWord(head, 0x10 + 12 * kWaveTable);
            const uint32_t count = static_cast<uint32_t>(added.songs.size());
            if (hashes[kWaveTable] != kHeadBytes || tables[kWaveTable] != kHeadBytes + 8u * waves ||
                tableBytes != tables[kWaveTable] + 8u * waves || waves == 0u)
                return nullptr;
            const uint32_t firstWave = headWord(head, tables[kWaveTable]);
            const uint32_t newTableBytes = kHeadBytes + 16u * (waves + count);
            if (newTableBytes > firstWave || firstWave > head.size())
                return nullptr;
            std::vector<uint8_t> hashIndex(head.begin() + hashes[kWaveTable], head.begin() + tables[kWaveTable]);
            std::vector<uint8_t> waveTable(head.begin() + tables[kWaveTable], head.begin() + tableBytes);
            uint32_t hash = 0;
            for (size_t at = 0; at + 8u <= hashIndex.size(); at += 8u)
                hash = std::max(hash, headWord(hashIndex, at));
            const uint64_t end = appendAddedWaves(waveTable, addedWavesStart(fileSize, waveTable));
            for (uint32_t i = 0; i < count; ++i)
            {
                const size_t entry = hashIndex.size();
                hashIndex.resize(entry + 8u);
                putHeadWord(hashIndex, entry, hash + 1u + i);
                putHeadWord(hashIndex, entry + 4u, waves + i);
            }
            std::vector<uint8_t> out(head.begin(), head.begin() + kHeadBytes);
            out.insert(out.end(), hashIndex.begin(), hashIndex.end());
            out.insert(out.end(), waveTable.begin(), waveTable.end());
            for (size_t t = 0; t < kTables; ++t)
            {
                // (the empty tables before the waves' stay, those after move on)
                if (t == kWaveTable)
                {
                    putHeadWord(out, 0x18 + 12 * t, kHeadBytes + 8u * (waves + count));
                    continue;
                }
                putHeadWord(out, 0x14 + 12 * t, hashes[t] >= tableBytes ? newTableBytes : hashes[t]);
                putHeadWord(out, 0x18 + 12 * t, tables[t] >= tableBytes ? newTableBytes : tables[t]);
            }
            putHeadWord(out, 0x10 + 12 * kWaveTable, waves + count);
            putHeadWord(out, 0x0C, newTableBytes);
            putHeadWord(out, 0x08, static_cast<uint32_t>(std::min<uint64_t>(end, 0xFFFFFFFFu)));
            out.resize(firstWave, 0); // (up to the first wave: the bank's own padding)
            added.firstWave = waves;
            return std::make_shared<const std::vector<uint8_t>>(std::move(out));
        }

        // MUSIC.ZSD's head with the added songs' waves (null: the bank as it
        // is, said once).
        std::shared_ptr<const std::vector<uint8_t>> musicBankWithSongs(gf::GameFile &bank)
        {
            AddedSongs &added = addedSongList();
            std::vector<uint8_t> head(kSectorBytes);
            std::shared_ptr<const std::vector<uint8_t>> made;
            if (bank.read(0, head.data(), head.size()) == head.size() && std::memcmp(head.data(), "ZSNDPS2 ", 8) == 0)
                made = game().bankHeader == BankHeader::Triples ? triplesHead(head, bank.size()) : pairsHead(head, bank.size());
            if (!made)
            {
                std::cerr << "[sound] added songs: MUSIC.ZSD (" << bank.source() << ") has no room for them: not played" << std::endl;
                return nullptr;
            }
            for (size_t i = 0; i < added.songs.size(); ++i)
                added.songs[i].wave = added.firstWave + static_cast<uint32_t>(i);
            added.made = true;
            std::cout << "[sound] added songs: MUSIC.ZSD's " << added.firstWave << " waves and " << added.songs.size()
                      << " more (waves " << added.firstWave << "-" << added.firstWave + added.songs.size() - 1u << ")" << std::endl;
            for (const AddedSong &song : added.songs)
                std::cout << "[sound] added songs: wave " << song.wave << " \"" << song.title << "\" (" << song.file.filename().string()
                          << ", " << static_cast<int>(song.seconds) << " s)" << std::endl;
            return made;
        }

        // A music stream of an added song's wave: the song's file.
        Replacement addedSong(const StreamSource &source)
        {
            Replacement replacement;
            const AddedSongs &added = addedSongList();
            if (!added.made || source.bus != Music || !source.file)
                return replacement;
            for (const AddedSong &song : added.songs)
            {
                if (song.start != source.start)
                    continue;
                std::string error;
                replacement.reader = zx::audio_file::open(song.file, &error);
                replacement.ownRate = true;
                if (replacement.reader)
                    std::cout << "[sound] added song: wave " << song.wave << ", \"" << song.title << "\" from "
                              << song.file.string() << " (" << replacement.reader->rate() << " Hz)" << std::endl;
                else
                    std::cerr << "[sound] cannot play " << song.file.string() << ": " << error << std::endl;
                break;
            }
            return replacement;
        }

        // ---- the driver -----------------------------------------------------------------

        struct SampleSlot
        {
            bool used = false;
            uint32_t size = 0;
            uint32_t firstPage = 0;
            uint32_t pages = 0;
            uint32_t generation = 0;
            std::vector<uint8_t> adpcm;
            // Decoded from a start offset until its end flag; emptied when
            // uploads change the sample.
            std::map<uint32_t, std::shared_ptr<const spu::DecodedSample>> decoded;
        };

        struct Channel
        {
            spu::Voice voice;
            uint32_t sample = UINT32_MAX; // slot index of what ChanSetParam set
            uint32_t sampleOffset = 0;    // and the byte offset in it
            // What the voice was keyed on with: its memory, for the SPU2's
            // way of ending a voice whose sample was freed and overwritten.
            uint32_t playingSlot = UINT32_MAX;
            uint32_t playingGeneration = 0;
            uint32_t playingFirstPage = 0;
            uint32_t playingPages = 0;
        };

        // A ZAUDIO stream table entry.
        struct Stream
        {
            bool used = false;
            bool stereo = false;
            Source mode = Source::Sectors;
            uint32_t eeObject = 0;
            uint32_t statusAddress = 0;
            uint16_t pitch = 0;      // SPU2 pitch
            uint16_t trackPitch = 0; // a replacement's at its own rate; 0: the stream's pitch
            uint32_t loopCount = 1;  // plays before the end
            uint32_t loopsDone = 0;
            uint32_t halves = 0;     // ring halves crossed
            bool needFill = false;   // the ring to be filled at Play
            // Status words, and the state before a pause.
            uint32_t state = 0;
            uint32_t word54 = 0;
            uint32_t progress = 0;
            uint32_t total = 0;
            uint32_t pausedState = 0;
            // Source.
            bool sourceOpen = false;
            std::shared_ptr<gf::GameFile> file;
            uint64_t start = 0; // its first byte in the file
            Bus bus = Effects;
            // Playback.
            std::shared_ptr<StreamBlocks> data;
            std::shared_ptr<TrackData> track;
            uint64_t halvesSeen = 0;
            std::array<spu::Voice, 2> voices;
            std::array<uint16_t, 2> volLeft{};  // VOLL of the left/right voice
            std::array<uint16_t, 2> volRight{}; // VOLR
            // A fade.
            uint32_t fadeFlags = 0;
            int32_t fadeStep = 0;
            uint32_t fadeTicks = 0;
            bool stopAtHalf = false;
            // PerStream's: the source's size, the progress units (sectors or
            // bytes) a refill reads, whether it all fits the ring, the rings.
            uint64_t bytes = 0;
            uint32_t unitsPerHalf = 0;
            bool fits = false;
            std::array<int32_t, 2> ring{-1, -1};
            // Shared's: the source's length and what was read of it, in units.
            uint32_t length = 0;
            uint32_t read = 0;
        };

        struct QueuedCommand
        {
            uint32_t fn = 0;
            std::vector<uint8_t> payload;
        };

        bool playingOrPaused(const Stream &s)
        {
            return s.state == kStreamPlayingOdd || s.state == kStreamPlaying || s.state == kStreamEnding || s.state == kStreamPaused;
        }

        uint16_t playPitch(const Stream &s) { return s.track && s.trackPitch ? s.trackPitch : s.pitch; }

        class Driver;

        // A ZAUDIO version's streams (StreamRings).
        class StreamSteps
        {
        public:
            virtual ~StreamSteps() = default;
            // Init with the status on.
            virtual void init(Driver &driver) { (void)driver; }
            // StrmAlloc on a free entry.
            virtual void allocate(Driver &driver, Stream &s, bool stereo, Source mode, uint16_t pitch, uint32_t eeObject,
                                  uint32_t statusAddress) = 0;
            // StrmFree, after the voices were keyed off.
            virtual void release(Driver &driver, Stream &s) { (void)driver, (void)s; }
            // SetParam's source, once its file is open: the offset and length as sent.
            virtual void setSource(Stream &s, uint32_t offset, uint32_t length) = 0;
            virtual void play(Driver &driver, Stream &s) = 0;
            virtual void stop(Stream &s) = 0;
            // One tick, after the fade: ring halves crossed, refills, the end.
            virtual void tick(Driver &driver, Stream &s) = 0;
            // Whether the volume sets the left voice's left side only (and
            // the right voice's right side): what the volume command asked,
            // or a fade's (true for a stereo stream).
            virtual bool leftOnly(const Stream &s, bool asked) const
            {
                (void)s;
                return asked;
            }
        };

        class Driver
        {
        public:
            void configure(std::unique_ptr<StreamSteps> steps);
            void rpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
            void vsync(uint8_t *rdram, PS2Machine *runtime);

            // For the stream steps: sample memory and the voices on it.
            int32_t allocSlot(uint32_t size); // a table entry with its pages, as ZAUDIO's AllocSample
            void freeSlot(uint32_t slot);
            const SampleSlot &slot(size_t index) const { return m_samples[index]; }
            // New data in SPU memory: a voice still playing a freed sample
            // there reads it on the SPU2 and soon meets an end flag; here it
            // stops (unless it plays `slot` as it is now).
            void overwritten(uint32_t firstPage, uint32_t pages, uint32_t slot);
            void stopStream(Stream &s) { m_steps->stop(s); }
            // The stream's source from its start into its voices, or what
            // replaces it (Play).
            void keyOnSource(Stream &s, std::shared_ptr<StreamBlocks> data);
            // What replaces the stream's source (the custom soundtrack's song,
            // the game's own resolver's) made its track: false for none.
            bool replaceSource(Stream &s);
            void applyStreamVolume(Stream &s, uint16_t volume, bool leftOnly);
            size_t streamIndex(const Stream &s) const { return static_cast<size_t>(&s - m_streams.data()); }

        private:
            void render(PS2Machine *runtime);
            uint32_t execute(uint8_t *rdram, uint32_t fn, uint32_t send, uint32_t size);
            void reset();
            uint32_t findPages(uint32_t need) const; // first fit
            uint32_t allocSample(uint32_t size);
            void freeSample(uint32_t handle);
            void loadSample(uint8_t *rdram, uint32_t send);
            std::shared_ptr<const spu::DecodedSample> decoded(uint32_t slot, uint32_t offset);
            bool slotAt(uint32_t spuAddress, uint32_t &slot, uint32_t &offset) const;
            void setChannel(uint8_t *rdram, uint32_t send);
            void keyMask(uint8_t *rdram, uint32_t send, bool on);
            uint32_t allocStream(uint8_t *rdram, uint32_t send);
            void freeStream(uint32_t handle);
            Stream *stream(uint32_t handle);
            void runQueued(const QueuedCommand &command);
            void streamSetParam(Stream &s, const std::vector<uint8_t> &p);
            void streamPause(Stream &s, bool pause);
            void openSource(Stream &s, const std::string &path, uint32_t offset, uint32_t length);
            void fade(Stream &s);
            void writeStatus(uint8_t *rdram);
            void dumpMemory();

            std::unique_ptr<StreamSteps> m_steps;
            bool m_initialized = false;
            uint32_t m_replyAddress = 0; // Alloc's second reply word, where the game has it
            bool m_statusEnabled = true;
            uint32_t m_statusAddress = 0;
            uint64_t m_frames = 0; // 48 kHz frames made so far (guest time)
            bool m_clockStarted = false;
            std::vector<SampleSlot> m_samples;
            std::array<bool, kPages> m_pageUsed{};
            uint64_t m_tick = 0;     // the vsync tick (ZX_SOUND_MEMDUMP)
            bool m_memDirty = false; // (ZX_SOUND_MEMDUMP)
            uint32_t m_generation = 0;
            std::array<Channel, kChannels> m_channels;
            std::array<Stream, kStreams> m_streams;
            std::vector<QueuedCommand> m_queue;
            float m_dcIn[2] = {0.0f, 0.0f};
            float m_dcOut[2] = {0.0f, 0.0f};
            std::vector<int16_t> m_out;
        };

        // The boot's driver: a game booted again starts with every sample,
        // stream and channel free and its clock at 0, as in a process of its own.
        Driver &driver() { return zx::session::state<Driver>(); }

        // ---- ZGE3's streams (AI, BMX XXX) -------------------------------------------------

        class PerStreamRings final : public StreamSteps
        {
        public:
            void allocate(Driver &driver, Stream &s, bool stereo, Source mode, uint16_t pitch, uint32_t eeObject,
                          uint32_t statusAddress) override
            {
                // The status words and the state live on in ZAUDIO's table.
                const uint32_t streamState = s.state, word54 = s.word54, progress = s.progress, total = s.total;
                s = Stream{};
                s.state = streamState;
                s.word54 = word54;
                s.progress = progress;
                s.total = total;
                s.used = true;
                s.stereo = stereo;
                s.ring[0] = driver.allocSlot(kRingBytes);
                if (s.stereo)
                    s.ring[1] = driver.allocSlot(kRingBytes);
                s.mode = mode;
                s.pitch = pitch;
                s.eeObject = eeObject;
                s.statusAddress = statusAddress;
                s.volLeft = {0x3FFFu, 0u};
                s.volRight = {static_cast<uint16_t>(s.stereo ? 0u : 0x3FFFu), 0x3FFFu};
                for (size_t c = 0; c < 2; ++c)
                {
                    s.voices[c].setPitch(s.pitch);
                    s.voices[c].setVolume(s.volLeft[c], s.volRight[c]);
                    s.voices[c].setEnvelope(0x00FFu, 0x1FC0u);
                }
            }

            void release(Driver &driver, Stream &s) override
            {
                for (int32_t &ring : s.ring)
                {
                    if (ring >= 0)
                        driver.freeSlot(static_cast<uint32_t>(ring));
                    ring = -1;
                }
            }

            void setSource(Stream &s, uint32_t offset, uint32_t length) override
            {
                if (s.mode == Source::Sectors)
                {
                    // Whole sectors from the one holding `offset` (ZAUDIO
                    // reads the disc by sector).
                    s.start = static_cast<uint64_t>(offset / kSectorBytes) * kSectorBytes;
                    s.total = (length + kSectorBytes - 1u) / kSectorBytes;
                    s.bytes = static_cast<uint64_t>(s.total) * kSectorBytes;
                    s.unitsPerHalf = (s.stereo ? 2u * kHalfBytes : kHalfBytes) / kSectorBytes;
                }
                else
                {
                    s.start = offset;
                    s.total = length;
                    s.bytes = length;
                    s.unitsPerHalf = s.stereo ? 2u * kHalfBytes : kHalfBytes;
                }
                s.sourceOpen = true;
                s.progress = 0;
                s.halves = 0;
                s.fits = s.bytes <= 2u * (s.stereo ? 2u * kHalfBytes : kHalfBytes);
                s.needFill = false;
                s.state = kStreamStopped;
                s.word54 = 2;
            }

            void play(Driver &driver, Stream &s) override
            {
                for (spu::Voice &v : s.voices)
                    v.setPitch(playPitch(s));
                if (s.state == kStreamPaused)
                {
                    s.state = s.pausedState;
                    return;
                }
                s.state = (s.halves & 1u) ? kStreamPlayingOdd : kStreamPlaying;
                s.needFill = false;
                if (s.fits)
                    s.state = kStreamEnding;
                if (s.mode == Source::None || !s.sourceOpen)
                    return;
                for (int32_t ring : s.ring)
                    if (ring >= 0)
                        driver.overwritten(driver.slot(static_cast<size_t>(ring)).firstPage,
                                           driver.slot(static_cast<size_t>(ring)).pages, static_cast<uint32_t>(ring));
                s.halvesSeen = 0;
                s.loopsDone = 0;
                Stream *self = &s;
                auto loop = [self]()
                {
                    ++self->loopsDone;
                    self->halvesSeen = 0;
                    return self->loopsDone < self->loopCount;
                };
                driver.keyOnSource(s, std::make_shared<SourceBlocks>(s.file, s.start, s.bytes, s.stereo, loop));
            }

            // ZAUDIO's stop: both voices keyed off, the source back at its start.
            void stop(Stream &s) override
            {
                for (spu::Voice &v : s.voices)
                    v.keyOff();
                s.stopAtHalf = false;
                if (s.mode == Source::None)
                {
                    s.state = kStreamStopped;
                    return;
                }
                s.progress = 0;
                s.needFill = true;
                s.halves = 0;
                s.total = s.mode == Source::Sectors ? static_cast<uint32_t>((s.bytes + kSectorBytes - 1u) / kSectorBytes)
                                                    : static_cast<uint32_t>(s.bytes);
                s.state = kStreamStopped;
            }

            // The ring halves the voices crossed (state and progress as
            // ZAUDIO's refills leave them), and the end.
            void tick(Driver &driver, Stream &s) override
            {
                (void)driver;
                if (s.state != kStreamPlayingOdd && s.state != kStreamPlaying && s.state != kStreamEnding)
                    return;
                uint64_t halves = 0;
                if (s.track)
                {
                    // A replacement: the original's halves in step with how
                    // much of it has played.
                    const uint64_t totalHalves = (static_cast<uint64_t>(s.total) + s.unitsPerHalf - 1u) / std::max<uint32_t>(1u, s.unitsPerHalf);
                    halves = static_cast<uint64_t>(s.track->fraction() * static_cast<double>(totalHalves));
                }
                else if (s.data)
                {
                    halves = s.data->blocks() / kHalfBlocks;
                }
                while (s.halvesSeen < halves)
                {
                    ++s.halvesSeen;
                    if (s.stopAtHalf)
                    {
                        stop(s);
                        return;
                    }
                    if (s.state != kStreamEnding)
                        s.state = (s.halves & 1u) ? kStreamPlayingOdd : kStreamPlaying;
                    ++s.halves;
                    s.progress = static_cast<uint32_t>(std::min<uint64_t>(s.total, (s.halvesSeen + 2u) * s.unitsPerHalf));
                }
                const bool sounding = s.voices[0].active() || (s.stereo && s.voices[1].active());
                if (!sounding)
                    stop(s); // played to the end
            }
        };

        // ---- DM2's streams -----------------------------------------------------------------

        class SharedRings final : public StreamSteps
        {
        public:
            // The two stream rings, once (sample table entries 0 and 1 at the
            // first Init).
            void init(Driver &driver) override
            {
                if (m_ringsAllocated)
                    return;
                m_ringsAllocated = true;
                const int32_t left = driver.allocSlot(kRingBytes);
                driver.allocSlot(kRingBytes);
                m_ringFirstPage = left >= 0 ? driver.slot(static_cast<size_t>(left)).firstPage : 0u;
            }

            void allocate(Driver &driver, Stream &s, bool stereo, Source mode, uint16_t pitch, uint32_t eeObject,
                          uint32_t statusAddress) override
            {
                (void)driver;
                // The status words, the state and the ring halves live on in
                // ZAUDIO's table, and the voices; the alloc sets what it sets.
                Stream fresh{};
                fresh.state = s.state;
                fresh.word54 = s.word54;
                fresh.progress = s.progress;
                fresh.total = s.total;
                fresh.pausedState = s.pausedState;
                fresh.halves = s.halves;
                fresh.voices = std::move(s.voices);
                s = std::move(fresh);
                s.used = true;
                s.needFill = false;
                s.stereo = stereo;
                s.loopCount = 1;
                s.loopsDone = 0;
                s.mode = mode;
                s.pitch = pitch;
                s.eeObject = eeObject;
                s.statusAddress = statusAddress;
                s.volLeft = {0x3FFFu, 0u};
                s.volRight = {0u, 0x3FFFu};
                for (size_t c = 0; c < 2; ++c)
                {
                    s.voices[c].stop();
                    s.voices[c].setPitch(s.pitch);
                    s.voices[c].setVolume(s.volLeft[c], s.volRight[c]);
                    s.voices[c].setEnvelope(0x00FFu, 0x1FC0u);
                }
            }

            void setSource(Stream &s, uint32_t offset, uint32_t length) override
            {
                if (s.mode == Source::Sectors)
                {
                    s.start = static_cast<uint64_t>(offset / kSectorBytes) * kSectorBytes;
                    s.length = (length + kSectorBytes - 1u) / kSectorBytes;
                }
                else
                {
                    s.start = offset;
                    s.length = length;
                }
                s.sourceOpen = true;
                s.read = 0;
                s.needFill = true;
                s.progress = 0;
                s.halves = 0;
                s.total = s.length;
                s.state = kStreamStopped;
                s.word54 = 2;
            }

            // The first two pieces (if the source was just set or stopped),
            // and key on.
            void play(Driver &driver, Stream &s) override
            {
                for (spu::Voice &v : s.voices)
                    v.setPitch(playPitch(s));
                if (s.state != kStreamPaused)
                {
                    if (s.needFill && s.mode != Source::None && s.sourceOpen)
                    {
                        auto pieces = std::make_shared<RingPieces>(s.file, s.start, s.stereo);
                        pieces->push(0u, kRingBytes);
                        pieces->push(static_cast<uint64_t>(readUnits(s)) * unitBytes(s), kRingBytes);
                        s.data = pieces;
                        s.read = 2u * readUnits(s);
                        s.halvesSeen = 0;
                        s.loopsDone = 0;
                        driver.overwritten(m_ringFirstPage, 2u * kRingBytes / kPageBytes, UINT32_MAX);
                        if (driver.replaceSource(s))
                            for (spu::Voice &v : s.voices)
                                v.setPitch(playPitch(s));
                    }
                    s.needFill = false;
                    for (size_t c = 0; c < (s.stereo ? 2u : 1u); ++c)
                    {
                        if (s.mode != Source::None && s.track)
                            s.voices[c].keyOn(std::make_unique<TrackChannel>(s.track, static_cast<int>(c)));
                        else if (s.mode != Source::None && s.data)
                            s.voices[c].keyOn(std::make_unique<StreamChannel>(s.data, static_cast<int>(c)));
                        else
                            s.voices[c].keyOn(nullptr);
                    }
                }
                s.state = (s.halves & 1u) ? kStreamPlayingOdd : kStreamPlaying;
            }

            void stop(Stream &s) override
            {
                for (spu::Voice &v : s.voices)
                    v.keyOff();
                s.stopAtHalf = false;
                if (s.mode != Source::None)
                {
                    s.progress = 0;
                    s.read = 0;
                    s.needFill = true;
                    s.halves = 0;
                    s.total = s.length;
                }
                s.state = kStreamStopped;
            }

            // A half crossing, one a tick at most (as ZAUDIO sees the voice's
            // address); also while paused at its pitch.
            void tick(Driver &driver, Stream &s) override
            {
                (void)driver;
                if (s.track)
                {
                    replacementTick(s);
                    return;
                }
                if (!playingOrPaused(s) || !s.data)
                    return;
                const uint64_t halves = s.data->blocks() / kHalfBlocks;
                if (s.halvesSeen >= halves)
                    return;
                s.halvesSeen = halves;
                if (s.state == kStreamEnding)
                {
                    stop(s);
                    return;
                }
                if (s.state != kStreamNoRefill)
                    refill(s);
                ++s.halves;
            }

            bool leftOnly(const Stream &s, bool asked) const override
            {
                (void)s, (void)asked;
                return true;
            }

        private:
            static uint32_t unitBytes(const Stream &s) { return s.mode == Source::Sectors ? kSectorBytes : 1u; }
            // Every refill reads 64 KB: 0x20 sectors, or 64 KB of bytes.
            static uint32_t readUnits(const Stream &s) { return s.mode == Source::Sectors ? kRingBytes / kSectorBytes : kRingBytes; }

            // A replacement plays to its own end: the source's ring halves in
            // step with how much of it has played, the state and progress as
            // the refills would leave them, a fade's end at the next half.
            void replacementTick(Stream &s)
            {
                if (s.state != kStreamPlayingOdd && s.state != kStreamPlaying && s.state != kStreamEnding)
                    return;
                const uint64_t sourceBytes = static_cast<uint64_t>(s.length) * unitBytes(s);
                const uint64_t pieces = (sourceBytes + kRingBytes - 1u) / kRingBytes;
                const uint64_t halves = static_cast<uint64_t>(s.track->fraction() * static_cast<double>(pieces));
                while (s.halvesSeen < halves)
                {
                    ++s.halvesSeen;
                    if (s.state == kStreamEnding)
                    {
                        stop(s);
                        return;
                    }
                    s.state = (s.halves & 1u) ? kStreamPlayingOdd : kStreamPlaying;
                    ++s.halves;
                    s.read = static_cast<uint32_t>(std::min<uint64_t>(s.length, (s.halvesSeen + 2u) * readUnits(s)));
                    s.progress = s.read;
                    s.total = s.length;
                }
                const bool sounding = s.voices[0].active() || (s.stereo && s.voices[1].active());
                if (!sounding)
                    stop(s); // played to its end
            }

            // A ring half was played: ZAUDIO reads the next 64 KB into it.
            void refill(Stream &s)
            {
                auto *pieces = static_cast<RingPieces *>(s.data.get());
                if (s.mode == Source::None || !pieces)
                    return;
                const uint32_t unit = unitBytes(s);
                const uint32_t at = s.read;
                s.read += readUnits(s);
                s.state = (s.halves & 1u) ? kStreamPlayingOdd : kStreamPlaying;
                // The end: DM2's ZAUDIO compares with total - 1 sectors
                // (bytes: total - 0x800).
                uint32_t valid = kRingBytes;
                const bool end = s.mode == Source::Sectors ? s.length - 1u <= s.read
                                                           : static_cast<int64_t>(s.length) - kSectorBytes <= static_cast<int64_t>(s.read);
                if (end)
                {
                    const int64_t over = static_cast<int64_t>(s.read) - static_cast<int64_t>(s.length);
                    const int64_t keep = static_cast<int64_t>(kRingBytes) - over * unit;
                    valid = static_cast<uint32_t>(std::clamp<int64_t>(keep, 0, kRingBytes));
                    pieces->push(static_cast<uint64_t>(at) * unit, valid);
                    ++s.loopsDone;
                    if (s.loopsDone < s.loopCount)
                        s.read = 0;
                    else
                        s.state = kStreamEnding;
                }
                else
                {
                    pieces->push(static_cast<uint64_t>(at) * unit, valid);
                }
                s.progress = s.read;
                s.total = s.length;
            }

            bool m_ringsAllocated = false;
            uint32_t m_ringFirstPage = 0;
        };

        // ---- clock and mixing --------------------------------------------------------------

        void Driver::configure(std::unique_ptr<StreamSteps> steps)
        {
            m_steps = std::move(steps);
            m_samples.assign(game().sampleSlots, SampleSlot{});
        }

        void Driver::render(PS2Machine *runtime)
        {
            const uint64_t now = runtime->eeScheduler().eeCycle() / kEeCyclesPerFrame;
            if (!m_clockStarted)
            {
                m_clockStarted = true;
                m_frames = now;
            }
            if (now <= m_frames)
                return;
            // After a long stall (or a jump of the clock) make no more than
            // a second; the host output drops what it cannot use anyway.
            uint64_t todo = std::min<uint64_t>(now - m_frames, 48000u);
            m_frames = now;
            const State &st = state();
            const float gain[BusCount] = {st.busGain[Effects].load(std::memory_order_relaxed),
                                          st.busGain[Music].load(std::memory_order_relaxed),
                                          st.busGain[Speech].load(std::memory_order_relaxed)};
            constexpr size_t kChunk = 1024;
            while (todo > 0u)
            {
                const size_t frames = static_cast<size_t>(std::min<uint64_t>(todo, kChunk));
                todo -= frames;
                m_out.resize(frames * 2u);
                for (size_t f = 0; f < frames; ++f)
                {
                    int32_t bus[BusCount][2] = {};
                    for (Channel &c : m_channels)
                        c.voice.mix(bus[Effects][0], bus[Effects][1]);
                    for (Stream &s : m_streams)
                    {
                        if (!s.used)
                            continue;
                        for (spu::Voice &v : s.voices)
                            v.mix(bus[s.bus][0], bus[s.bus][1]);
                    }
                    for (int c = 0; c < 2; ++c)
                    {
                        float mixed = 0.0f;
                        for (int b = 0; b < BusCount; ++b)
                            mixed += static_cast<float>(bus[b][c]) * gain[b];
                        // The SPU2's output stage: 16-bit clamp, then its
                        // DC-blocking high-pass.
                        const float in = std::clamp(mixed, -32768.0f, 32767.0f) / 32767.0f;
                        const float out = in - m_dcIn[c] + 0.995f * m_dcOut[c];
                        m_dcIn[c] = in;
                        m_dcOut[c] = out;
                        m_out[f * 2u + static_cast<size_t>(c)] =
                            static_cast<int16_t>(std::clamp(out * 32767.0f, -32768.0f, 32767.0f));
                    }
                }
                runtime->submitNativeAudio(m_out.data(), frames);
            }
        }

        // ---- commands ----------------------------------------------------------------------

        void logCommand(uint8_t *rdram, PS2Machine *runtime, uint32_t fn, uint32_t send, uint32_t size);
        void logStatus(uint8_t *rdram, uint64_t tick);

        void Driver::rpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t fn = getRegU32(ctx, kRegA0);
            const uint32_t send = getRegU32(ctx, kRegA1);
            const uint32_t size = getRegU32(ctx, kRegA2);
            const bool wantReply = getRegU32(ctx, kRegA3) != 0u;
            logCommand(rdram, runtime, fn, send, size);
            render(runtime);
            const uint32_t result = execute(rdram, fn, send, size);
            if (state().traceLevel >= 2)
                std::cout << "[sound] fn " << fn << " size " << size << " -> 0x" << std::hex << result << std::dec << std::endl;
            const uint32_t reply = game().replyBuffer;
            if (game().replyHasSpuAddress && fn == kAllocateWave && result != 0u)
                m_replyAddress = kSpuSampleBase + m_samples[(result - game().sampleTable) / kSampleStride].firstPage * kPageBytes;
            if (wantReply)
            {
                wr32(rdram, reply, result);
                if (game().replyHasSpuAddress)
                    wr32(rdram, reply + 4u, m_replyAddress);
            }
            returnTo(ctx, wantReply ? reply : 0u);
        }

        uint32_t Driver::execute(uint8_t *rdram, uint32_t fn, uint32_t send, uint32_t size)
        {
            switch (fn)
            {
            case kInit:
            {
                DriverInit init{};
                readGuest(rdram, send, &init, sizeof(init));
                const bool statusOn = init.statusOn != 0u;
                if (game().rings == StreamRings::PerStream)
                    for (Channel &c : m_channels)
                        c.voice.stop();
                if (statusOn)
                {
                    m_statusAddress = init.voiceStatus;
                    m_initialized = true;
                    m_steps->init(*this);
                }
                // ZGE3's Init resets; DM2's does not.
                if (game().rings == StreamRings::PerStream)
                    reset();
                return 1;
            }
            case kReset:
                reset();
                return 0;
            case kAllocateWave:
                return allocSample(rd32(rdram, send));
            case kFreeWave:
                freeSample(rd32(rdram, send));
                return 1;
            case kSendWave:
                loadSample(rdram, send);
                return 0;
            case kSetVoice:
                setChannel(rdram, send);
                return 1;
            case kKeyOn:
            case kKeyOff:
                keyMask(rdram, send, fn == kKeyOn);
                return 1;
            case kOpenStream:
                return allocStream(rdram, send);
            case kCloseStream:
                freeStream(rd32(rdram, send));
                return 1;
            case kStreamRequest:
            case kPlayStream:
            case kStopStream:
            case kPauseStream:
            {
                QueuedCommand command;
                command.fn = fn;
                command.payload.resize(std::min<uint32_t>(size, 0x200u));
                readGuest(rdram, send, command.payload.data(), command.payload.size());
                m_queue.push_back(std::move(command));
                return 1;
            }
            case kStatusOff:
            case kStatusOn:
                m_statusEnabled = fn == kStatusOn;
                if (state().trace)
                    std::cout << "[sound] status writes " << (m_statusEnabled ? "on" : "off") << std::endl;
                return 1;
            default:
                std::cerr << "[sound] unknown ZAudio command " << fn << std::endl;
                return 0;
            }
        }

        // Reset: every sample freed (DM2's: its stream rings too), every
        // channel keyed off (DM2's: the streams' voices too), the stream
        // commands queued dropped.
        void Driver::reset()
        {
            for (SampleSlot &slot : m_samples)
                slot = SampleSlot{};
            m_pageUsed.fill(false);
            for (Channel &c : m_channels)
                c.voice.keyOff();
            if (game().rings == StreamRings::Shared)
                for (Stream &s : m_streams)
                    for (spu::Voice &v : s.voices)
                        v.keyOff();
            m_queue.clear();
        }

        // ---- sample memory -----------------------------------------------------------------

        uint32_t Driver::findPages(uint32_t need) const
        {
            for (uint32_t p = 0; p < kPages; ++p)
            {
                uint32_t run = 0;
                while (run < need && p + run < kPages && !m_pageUsed[p + run])
                    ++run;
                if (run >= need)
                    return p;
            }
            return UINT32_MAX;
        }

        int32_t Driver::allocSlot(uint32_t size)
        {
            // ZAUDIO's: the first free entry, the first run of pages that fits.
            size_t i = m_samples.size();
            for (size_t k = 0; k < m_samples.size(); ++k)
            {
                if (!m_samples[k].used)
                {
                    i = k;
                    break;
                }
            }
            if (i == m_samples.size())
            {
                std::cerr << "[sound] out of sample handles" << std::endl;
                return -1;
            }
            SampleSlot &slot = m_samples[i];
            const uint32_t need = (size + kPageBytes - 1u) / kPageBytes;
            const uint32_t first = findPages(need);
            if (first == UINT32_MAX)
            {
                std::cerr << "[sound] failed to alloc sample memory (" << size << " bytes)" << std::endl;
                return -1;
            }
            for (uint32_t p = first; p < first + need; ++p)
                m_pageUsed[p] = true;
            slot = SampleSlot{};
            slot.used = true;
            slot.size = size;
            slot.firstPage = first;
            slot.pages = need;
            slot.generation = ++m_generation;
            if (state().memDump)
            {
                std::fprintf(state().memDump, "%llu alloc %zu size %u pages %u+%u\n", static_cast<unsigned long long>(m_tick), i,
                             size, first, need);
                m_memDirty = true;
            }
            return static_cast<int32_t>(i);
        }

        uint64_t fnv(const std::vector<uint8_t> &bytes, size_t size)
        {
            uint64_t h = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < size && i < bytes.size(); ++i)
                h = (h ^ bytes[i]) * 0x100000001b3ull;
            return h;
        }

        void Driver::dumpMemory()
        {
            std::FILE *dump = state().memDump;
            std::fprintf(dump, "%llu table", static_cast<unsigned long long>(m_tick));
            for (size_t i = 0; i < m_samples.size(); ++i)
                if (m_samples[i].used)
                    std::fprintf(dump, " %zu:%u+%u:%u:%016llx", i, m_samples[i].firstPage, m_samples[i].pages, m_samples[i].size,
                                 static_cast<unsigned long long>(fnv(m_samples[i].adpcm, m_samples[i].size)));
            std::fputc('\n', dump);
            std::fflush(dump);
            m_memDirty = false;
        }

        void Driver::freeSlot(uint32_t slot)
        {
            SampleSlot &s = m_samples[slot];
            if (s.used && state().memDump)
            {
                std::fprintf(state().memDump, "%llu free %u hash %016llx\n", static_cast<unsigned long long>(m_tick), slot,
                             static_cast<unsigned long long>(fnv(s.adpcm, s.size)));
                m_memDirty = true;
            }
            if (s.used)
                for (uint32_t p = s.firstPage; p < s.firstPage + s.pages && p < kPages; ++p)
                    m_pageUsed[p] = false;
            // Voices still playing it keep their decoded copy until its
            // memory is written again (overwritten()).
            s = SampleSlot{};
        }

        void Driver::overwritten(uint32_t firstPage, uint32_t pages, uint32_t slot)
        {
            const uint32_t generation = slot < m_samples.size() ? m_samples[slot].generation : 0u;
            for (Channel &c : m_channels)
            {
                if (!c.voice.active() || (c.playingSlot == slot && c.playingGeneration == generation))
                    continue;
                if (firstPage < c.playingFirstPage + c.playingPages && c.playingFirstPage < firstPage + pages)
                    c.voice.stop();
            }
        }

        uint32_t Driver::allocSample(uint32_t size)
        {
            const int32_t slot = allocSlot(size);
            if (slot < 0)
                return 0;
            m_samples[static_cast<size_t>(slot)].adpcm.assign(size, 0u);
            if (state().traceLevel >= 2)
                std::cout << "[sound] sample " << slot << ": " << size << " bytes" << std::endl;
            return game().sampleTable + static_cast<uint32_t>(slot) * kSampleStride;
        }

        void Driver::freeSample(uint32_t handle)
        {
            const uint32_t slot = (handle - game().sampleTable) / kSampleStride;
            if (handle < game().sampleTable || slot >= m_samples.size())
            {
                std::cerr << "[sound] FreeSample: invalid sample handle 0x" << std::hex << handle << std::dec << std::endl;
                return;
            }
            freeSlot(slot);
        }

        void Driver::loadSample(uint8_t *rdram, uint32_t send)
        {
            constexpr uint32_t kSpuBytes = 0x200000u;
            const uint32_t handle = rd32(rdram, send + offsetof(WaveLoad, handle));
            const uint32_t offset = rd32(rdram, send + offsetof(WaveLoad, offset));
            const uint32_t slot = (handle - game().sampleTable) / kSampleStride;
            if (handle < game().sampleTable || slot >= m_samples.size() || !m_samples[slot].used || offset > kSpuBytes)
            {
                std::cerr << "[sound] Load: invalid sample handle 0x" << std::hex << handle << std::dec << std::endl;
                return;
            }
            SampleSlot &s = m_samples[slot];
            // A 1 KB transfer (the SPU2 memory past the allocation is the next
            // allocation's; keep what fits plus a little slack).
            const uint32_t end = offset + kPageBytes;
            if (s.adpcm.size() < end)
                s.adpcm.resize(end, 0u);
            static_assert(kPageBytes == kWaveLoadBytes);
            readGuest(rdram, send + offsetof(WaveLoad, data), s.adpcm.data() + offset, kWaveLoadBytes);
            s.decoded.clear();
            m_memDirty = state().memDump != nullptr;
            overwritten(s.firstPage + offset / kPageBytes, 1u, slot);
        }

        std::shared_ptr<const spu::DecodedSample> Driver::decoded(uint32_t slot, uint32_t offset)
        {
            if (slot >= m_samples.size() || !m_samples[slot].used)
                return nullptr;
            SampleSlot &s = m_samples[slot];
            if (offset > 0u && offset >= s.adpcm.size())
                return nullptr;
            std::shared_ptr<const spu::DecodedSample> &at = s.decoded[offset];
            if (!at)
                at = spu::decodeSample(s.adpcm.data() + offset, s.adpcm.size() - offset);
            return at;
        }

        // The allocation holding an SPU address, and the offset in it.
        bool Driver::slotAt(uint32_t spuAddress, uint32_t &slot, uint32_t &offset) const
        {
            for (size_t i = 0; i < m_samples.size(); ++i)
            {
                const SampleSlot &s = m_samples[i];
                const uint32_t start = kSpuSampleBase + s.firstPage * kPageBytes;
                if (s.used && spuAddress >= start && spuAddress < start + s.pages * kPageBytes)
                {
                    slot = static_cast<uint32_t>(i);
                    offset = spuAddress - start;
                    return true;
                }
            }
            return false;
        }

        // ---- channels ----------------------------------------------------------------------

        // kSetVoice: the voice's command as the engine keeps it.
        void Driver::setChannel(uint8_t *rdram, uint32_t send)
        {
            VoiceCommand command{};
            readGuest(rdram, send, &command, sizeof(command));
            const uint32_t channel = command.driverVoice, flags = command.changes, sample = command.wave, pitch = command.pitch;
            if (channel >= kChannels)
            {
                std::cerr << "[sound] ChanSetParam: invalid channel " << channel << std::endl;
                return;
            }
            Channel &c = m_channels[channel];
            if ((flags & kChangeWave) && game().channelSample == ChannelSample::Handle)
            {
                const uint32_t slot = (sample - game().sampleTable) / kSampleStride;
                if (sample < game().sampleTable || slot >= m_samples.size())
                {
                    std::cerr << "[sound] ChanSetParam: invalid sample handle 0x" << std::hex << sample << std::dec << std::endl;
                }
                else
                {
                    c.sample = slot;
                    c.sampleOffset = 0;
                }
            }
            else if (flags & kChangeWave)
            {
                uint32_t slot = 0, offset = 0;
                if (!slotAt(sample, slot, offset))
                {
                    std::cerr << "[sound] ChanSetParam: no sample at SPU address 0x" << std::hex << sample << std::dec << std::endl;
                    c.sample = UINT32_MAX;
                }
                else
                {
                    c.sample = slot;
                    c.sampleOffset = offset;
                }
            }
            // The pitch in 44.1 kHz units, the SPU2's at 48 kHz.
            if (flags & kChangePitch)
                c.voice.setPitch(static_cast<uint16_t>(pitch * 44100u / 48000u));
            if (flags & kChangeVolume)
                c.voice.setVolume(command.volumeLeft, command.volumeRight);
            if (flags & kChangeEnvelope)
                c.voice.setEnvelope(command.attackDecaySustain, command.sustainRelease);
        }

        void Driver::keyMask(uint8_t *rdram, uint32_t send, bool on)
        {
            uint64_t mask = 0;
            readGuest(rdram, send, &mask, kKeyMaskBytes);
            for (size_t i = 0; i < kChannels; ++i)
            {
                if (!(mask >> i & 1u))
                    continue;
                Channel &c = m_channels[i];
                if (!on)
                {
                    c.voice.keyOff();
                    continue;
                }
                auto sample = decoded(c.sample, c.sampleOffset);
                const bool playable = sample != nullptr;
                c.voice.keyOn(playable ? std::make_unique<spu::SampleSource>(std::move(sample)) : nullptr);
                if (playable)
                {
                    const SampleSlot &slot = m_samples[c.sample];
                    c.playingSlot = c.sample;
                    c.playingGeneration = slot.generation;
                    c.playingFirstPage = slot.firstPage;
                    c.playingPages = slot.pages;
                }
            }
        }

        // ---- streams -----------------------------------------------------------------------

        Stream *Driver::stream(uint32_t handle)
        {
            const uint32_t base = game().streamTable, stride = game().streamStride;
            const uint32_t index = (handle - base) / stride;
            if (handle < base || index >= kStreams || (handle - base) % stride != 0u)
                return nullptr;
            return &m_streams[index];
        }

        // kOpenStream: the engine's StreamOpen.
        uint32_t Driver::allocStream(uint8_t *rdram, uint32_t send)
        {
            for (size_t i = 0; i < kStreams; ++i)
            {
                Stream &s = m_streams[i];
                if (s.used)
                    continue;
                StreamOpen open{};
                readGuest(rdram, send, &open, sizeof(open));
                const Source mode = (open.flags & kOpenNoSource) ? Source::None : (open.flags & kOpenFileBytes) ? Source::Bytes : Source::Sectors;
                const uint16_t pitch = static_cast<uint16_t>(open.pitch * 44100u / 48000u);
                m_steps->allocate(*this, s, (open.flags & kOpenStereo) != 0u, mode, pitch, open.slot, open.state);
                if (state().trace)
                    std::cout << "[sound] stream " << i << " allocated (" << (s.stereo ? "stereo" : "mono") << ", pitch 0x"
                              << std::hex << s.pitch << std::dec << ")" << std::endl;
                return game().streamTable + static_cast<uint32_t>(i) * game().streamStride;
            }
            std::cerr << "[sound] StrmAlloc: no free stream" << std::endl;
            return 0;
        }

        void Driver::freeStream(uint32_t handle)
        {
            Stream *s = stream(handle);
            if (!s || !s->used)
            {
                std::cerr << "[sound] StrmFree: invalid stream handle 0x" << std::hex << handle << std::dec << std::endl;
                return;
            }
            for (spu::Voice &v : s->voices)
                v.keyOff();
            m_steps->release(*this, *s);
            s->used = false;
            s->sourceOpen = false;
            s->data.reset();
            s->track.reset();
            s->file.reset();
            if (state().trace)
                std::cout << "[sound] stream " << streamIndex(*s) << " freed" << std::endl;
        }

        void Driver::runQueued(const QueuedCommand &command)
        {
            uint32_t handle = 0;
            if (command.payload.size() >= 4u)
                std::memcpy(&handle, command.payload.data(), 4);
            Stream *s = stream(handle);
            if (!s || !s->used)
            {
                std::cerr << "[sound] stream command " << command.fn << ": invalid stream handle 0x" << std::hex << handle
                          << std::dec << std::endl;
                return;
            }
            switch (command.fn)
            {
            case kStreamRequest:
                streamSetParam(*s, command.payload);
                break;
            case kPlayStream:
                m_steps->play(*this, *s);
                break;
            case kStopStream:
                m_steps->stop(*s);
                break;
            case kPauseStream:
            {
                StreamPause pause{};
                std::memcpy(&pause, command.payload.data(), std::min(command.payload.size(), sizeof(pause)));
                streamPause(*s, pause.paused != 0u);
                break;
            }
            default:
                break;
            }
        }

        void Driver::streamPause(Stream &s, bool pause)
        {
            if (pause)
            {
                s.pausedState = s.state;
                s.state = kStreamPaused;
                for (spu::Voice &v : s.voices)
                    v.setPitch(0);
            }
            else
            {
                s.state = s.pausedState;
                for (spu::Voice &v : s.voices)
                    v.setPitch(playPitch(s));
            }
        }

        // The left voice's VOLL (and VOLR unless leftOnly), the right voice's
        // VOLR. ZGE3's volume command (fn 10 flag 8) passes its own flags'
        // bit 1 as leftOnly (ZAUDIO reads the command's flags byte there), so
        // after it a stereo stream's left channel plays on both sides, as on
        // the PS2.
        void Driver::applyStreamVolume(Stream &s, uint16_t volume, bool leftOnly)
        {
            s.volLeft = {volume, 0u};
            s.volRight = {static_cast<uint16_t>(leftOnly ? 0u : volume), volume};
            for (size_t c = 0; c < 2; ++c)
                s.voices[c].setVolume(s.volLeft[c], s.volRight[c]);
        }

        void Driver::openSource(Stream &s, const std::string &path, uint32_t offset, uint32_t length)
        {
            zx::ftrace::Scope traceOpen(zx::ftrace::kDiscOpen, zx::ftrace::kDiscOpen + 100, 1);
            s.sourceOpen = false;
            s.file = files().open(path);
            if (!s.file)
            {
                std::cerr << "[sound] stream: cannot open " << path << std::endl;
                return;
            }
            const std::string name = gf::normalizePath(path);
            s.bus = name.find("MUSIC") != std::string::npos ? Music : name.find("SPEECH") != std::string::npos ? Speech : Effects;
            m_steps->setSource(s, offset, length);
            if (state().trace)
                std::cout << "[sound] stream " << streamIndex(s) << " source " << name << " at 0x" << std::hex << s.start
                          << std::dec << std::endl;
        }

        bool Driver::replaceSource(Stream &s)
        {
            s.track.reset();
            const StreamSource source{s.bus, s.file, s.start, s.stereo};
            Replacement replacement = customSong(source);
            if (!replacement.reader && game().resolve)
                replacement = game().resolve(source);
            if (!replacement.reader)
                return false;
            const uint32_t rate = replacement.reader->rate();
            SourceBlocks::LoopQuery loop = [&s]()
            {
                ++s.loopsDone;
                s.halvesSeen = 0;
                return s.loopsDone < s.loopCount;
            };
            s.track = std::make_shared<TrackData>(std::move(replacement.reader), loop);
            s.trackPitch = replacement.ownRate
                               ? static_cast<uint16_t>(std::min<uint64_t>(0x3FFFu, (static_cast<uint64_t>(rate) << 12) / 48000u))
                               : 0u;
            return true;
        }

        // Play from the start: the source, or what replaces it.
        void Driver::keyOnSource(Stream &s, std::shared_ptr<StreamBlocks> data)
        {
            s.data.reset();
            if (replaceSource(s))
            {
                for (size_t c = 0; c < (s.stereo ? 2u : 1u); ++c)
                {
                    s.voices[c].setPitch(playPitch(s));
                    s.voices[c].keyOn(std::make_unique<TrackChannel>(s.track, static_cast<int>(c)));
                }
                return;
            }
            s.data = std::move(data);
            for (size_t c = 0; c < (s.stereo ? 2u : 1u); ++c)
            {
                s.voices[c].setPitch(s.pitch);
                s.voices[c].keyOn(std::make_unique<StreamChannel>(s.data, static_cast<int>(c)));
            }
        }

        // kStreamRequest: the engine's StreamRequest (the payload is read as far as it goes).
        void Driver::streamSetParam(Stream &s, const std::vector<uint8_t> &p)
        {
            StreamRequest request{};
            std::memcpy(&request, p.data(), std::min(p.size(), sizeof(request)));
            const uint32_t flags = request.flags;
            if (flags & kRequestPitch)
            {
                s.pitch = static_cast<uint16_t>(request.pitch * 44100u / 48000u);
                if (!s.track || !s.trackPitch)
                    for (spu::Voice &v : s.voices)
                        v.setPitch(s.pitch);
            }
            if (flags & kRequestLoops)
            {
                s.loopCount = request.loops;
                if (s.loopCount < s.loopsDone)
                    m_steps->stop(s);
            }
            if (flags & kRequestSource)
            {
                if (playingOrPaused(s))
                    m_steps->stop(s);
                std::string path;
                for (size_t i = 0; i < sizeof(request.path) && request.path[i]; ++i)
                    path.push_back(request.path[i]);
                if (s.mode != Source::None)
                    openSource(s, path, request.offset, request.length);
            }
            // (ZAUDIO reads the request's loops flag as the volume's left-only bit.)
            if (flags & kRequestVolume)
                applyStreamVolume(s, static_cast<uint16_t>(request.volume), m_steps->leftOnly(s, (flags & kRequestLoops) != 0u));
            if (flags & kRequestFade)
            {
                s.fadeFlags = request.fadeFlags | 1u;
                s.fadeTicks = std::max<uint32_t>(1u, request.fadeMilliseconds >> 4);
                s.fadeStep = (static_cast<int32_t>(request.fadeTarget) - static_cast<int32_t>(s.volLeft[0])) / static_cast<int32_t>(s.fadeTicks);
            }
        }

        // A fade's tick; ZAUDIO ends every fade at its (never set) target: 0,
        // then the end flags.
        void Driver::fade(Stream &s)
        {
            if (!(s.fadeFlags & 1u))
                return;
            const bool leftOnly = m_steps->leftOnly(s, s.stereo);
            applyStreamVolume(s, static_cast<uint16_t>(s.volLeft[0] + s.fadeStep), leftOnly);
            if (--s.fadeTicks != 0u)
                return;
            applyStreamVolume(s, 0u, leftOnly);
            if (playingOrPaused(s))
            {
                if (s.fadeFlags & game().fadeEndStops)
                {
                    s.state = kStreamEnding;
                    s.stopAtHalf = true;
                }
                else if (s.fadeFlags & game().fadeEndHolds)
                {
                    s.state = kStreamPaused;
                }
            }
            s.fadeFlags = 0;
        }

        void Driver::writeStatus(uint8_t *rdram)
        {
            if (!m_statusEnabled)
                return;
            for (Stream &s : m_streams)
            {
                if (!s.used || !s.statusAddress)
                    continue;
                wr32(rdram, s.statusAddress, s.state);
                wr32(rdram, s.statusAddress + 4u, s.word54);
                wr32(rdram, s.statusAddress + 8u, s.progress);
                wr32(rdram, s.statusAddress + 12u, s.total);
            }
            if (!m_initialized || !m_statusAddress)
                return;
            for (size_t i = 0; i < kChannels; ++i)
            {
                // ZAUDIO reports 3/2 for a channel keyed off since the last
                // tick from reading KOFF back, but the SPU2 reads KOFF as 0
                // (PCSX2; the game's channel state machine was tuned on
                // that), so only 4 (envelope running) and 1 (silent) occur.
                const uint8_t status = m_channels[i].voice.envelope() != 0 ? kVoiceStatusSounding : kVoiceStatusSilent;
                if (uint8_t *p = getMemPtr(rdram, m_statusAddress + static_cast<uint32_t>(i)))
                    *p = status;
            }
        }

        // ZAUDIO's 60 Hz thread, here at every VBlank: queued stream
        // commands, streams, status.
        void Driver::vsync(uint8_t *rdram, PS2Machine *runtime)
        {
            m_tick = runtime->eeScheduler().currentVSyncTick();
            if (state().memDump && m_memDirty && m_tick % 300u == 0u)
                dumpMemory();
            render(runtime);
            std::vector<QueuedCommand> queue;
            queue.swap(m_queue);
            for (const QueuedCommand &command : queue)
            {
                if (state().trace)
                {
                    uint32_t handle = 0, flags = 0;
                    if (command.payload.size() >= 8u)
                    {
                        std::memcpy(&handle, command.payload.data(), 4);
                        std::memcpy(&flags, command.payload.data() + 4, 4);
                    }
                    static const char *names[] = {"SetParam", "Play", "Stop", "Pause"};
                    std::cout << "[sound] stream " << (handle - game().streamTable) / game().streamStride << " "
                              << names[command.fn - kStreamRequest];
                    if (command.fn == kStreamRequest)
                        std::cout << " flags 0x" << std::hex << (flags & 0xFFFFu) << std::dec;
                    if (command.fn == kPauseStream)
                        std::cout << " " << flags;
                    std::cout << std::endl;
                }
                runQueued(command);
            }
            for (Stream &s : m_streams)
            {
                if (!s.used)
                    continue;
                const uint32_t before = s.state;
                fade(s);
                m_steps->tick(*this, s);
                if (state().trace && before != s.state)
                    std::cout << "[sound] stream " << streamIndex(s) << " state " << before << " -> " << s.state << " (progress 0x"
                              << std::hex << s.progress << " of 0x" << s.total << std::dec << ")" << std::endl;
            }
            writeStatus(rdram);
            logStatus(rdram, runtime->eeScheduler().currentVSyncTick());
        }

        // ---- the command log ---------------------------------------------------------------

        void logCommand(uint8_t *rdram, PS2Machine *runtime, uint32_t fn, uint32_t send, uint32_t size)
        {
            State &st = state();
            if (!st.log)
                return;
            if (fn == kInit && rd32(rdram, send + offsetof(DriverInit, statusOn)) != 0u)
                st.logStatusAddress = rd32(rdram, send + offsetof(DriverInit, voiceStatus));
            std::fprintf(st.log, "%llu fn %u size %u:", static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), fn, size);
            const uint32_t shown = fn == kSendWave ? 8u : std::min<uint32_t>(size, fn >= kStreamRequest ? 0x40u : sizeof(VoiceCommand));
            for (uint32_t i = 0; i < shown; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, send + i);
                std::fprintf(st.log, "%s%02x", (i % 4u) ? "" : " ", p ? *p : 0u);
            }
            if (fn == kStreamRequest && size >= sizeof(StreamRequest))
                std::fprintf(st.log, " | vol %08x fade %08x %08x %08x", rd32(rdram, send + offsetof(StreamRequest, volume)),
                             rd32(rdram, send + offsetof(StreamRequest, fadeTarget)), rd32(rdram, send + offsetof(StreamRequest, fadeMilliseconds)),
                             rd32(rdram, send + offsetof(StreamRequest, fadeFlags)));
            std::fputc('\n', st.log);
        }

        void logStatus(uint8_t *rdram, uint64_t tick)
        {
            State &st = state();
            if (!st.log || !st.logStatusAddress)
                return;
            std::array<uint8_t, kChannels> now{};
            readGuest(rdram, st.logStatusAddress, now.data(), now.size());
            if (now == st.loggedStatus)
                return;
            st.loggedStatus = now;
            std::fprintf(st.log, "%llu status ", static_cast<unsigned long long>(tick));
            for (uint8_t b : now)
                std::fputc('0' + (b & 7u), st.log);
            std::fputc('\n', st.log);
        }

        void loggingRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            logCommand(rdram, runtime, getRegU32(ctx, kRegA0), getRegU32(ctx, kRegA1), getRegU32(ctx, kRegA2));
            state().originalRpc(rdram, ctx, runtime);
        }

        // ---- the custom soundtrack's song titles --------------------------------------------

        // What the games' fonts and AI's caption script can show: printable
        // ASCII, no quotes or braces (AI sets its caption with SET Text {"..."}).
        std::string captionText(const std::string &text, size_t limit)
        {
            std::string out;
            for (unsigned char c : text)
            {
                if (c < 0x20 || c >= 0x7F)
                    continue;
                out += c == '"' ? '\'' : c == '{' ? '(' : c == '}' ? ')' : c == '\\' ? '/' : static_cast<char>(c);
                if (out.size() >= limit)
                    break;
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        }

        // setPlaylistEntry(soundtrack, index, title, artist): the game names
        // its soundtrack's songs (index = MUSIC.ZSD wave) once at its start;
        // a song of the player's is named instead by its file's tags, else its
        // file name without the number. The strings are handed to the game's
        // own function from the stack below the caller's (each game keeps
        // them its own way: a record's 0x40-byte fields, or the heap).
        void playlistEntry(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            constexpr size_t kTextBytes = 0x40;
            const uint32_t list = getRegU32(ctx, kRegA0);
            const uint32_t index = getRegU32(ctx, kRegA1);
            uint32_t title = getRegU32(ctx, kRegA2);
            uint32_t artist = getRegU32(ctx, kRegA3);
            const uint32_t strings = (getRegU32(ctx, kRegSp) - 2u * kTextBytes) & ~0xFu;
            const uint32_t sp = (strings - 0x100u) & ~0xFu;
            zge_host::sites::seen("sound.playlistEntry");
            std::filesystem::path path;
            if (list != 0u && list == rd32(rdram, game().gameSoundtrack) && index < game().songs)
            {
                if (auto track = openCustomTrack(index, &path))
                {
                    std::string name = captionText(track->tag("title"), kTextBytes - 4u);
                    const std::string by = captionText(track->tag("artist"), kTextBytes - 4u);
                    if (name.empty())
                        name = captionText(zx::audio_file::nameFromFileName(path), kTextBytes - 4u);
                    if (!name.empty() && name.find_first_not_of("0123456789") != std::string::npos)
                    {
                        writeGuestString(rdram, strings, name, kTextBytes);
                        writeGuestString(rdram, strings + kTextBytes, by, kTextBytes);
                        title = strings;
                        artist = strings + kTextBytes;
                        std::cout << "[sound] custom soundtrack: song " << songNumber(index) << " shown as \"" << name << "\""
                                  << (by.empty() ? "" : " by " + by) << std::endl;
                    }
                }
            }
            runtime->callGuestFunction(rdram, ctx, state().originalPlaylistEntry, game().setPlaylistEntry,
                                       {list, index, title, artist}, sp);
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        // The engine's nameHash of a string key: the ELF hash of its letters,
        // a-z as A-Z.
        uint32_t keyHash(const std::string &name)
        {
            uint32_t hash = 0;
            for (unsigned char c : name)
            {
                const uint32_t letter = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
                hash = (hash << 4) + letter;
                const uint32_t high = hash & 0xF0000000u;
                if (high)
                    hash ^= high >> 24;
                hash &= ~high;
            }
            return hash;
        }

        // A song's texts under the game's keys (Game::songKey) for its wave:
        // its title, artist and album, no label. A key whose text is empty
        // would stay "$KEY" on screen: a blank is a space.
        void addSongTexts(uint32_t wave, const std::string &title, const std::string &artist, const std::string &album)
        {
            State &st = state();
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix), game().songKey, wave);
            const struct
            {
                const char *field;
                std::string text;
            } fields[] = {
                {"SONGNAME", title},
                {"ARTIST", artist},
                {"ALBUM", album},
                {"LABEL", ""},
            };
            for (const auto &field : fields)
            {
                State::SongText &entry = st.songTexts[keyHash(std::string(prefix) + field.field)];
                entry.wave = wave;
                entry.title = std::strcmp(field.field, "SONGNAME") == 0;
                // (A text starting with '~' is expanded again by ZGE4.)
                entry.text = field.text.empty() || field.text[0] == '~' ? " " + field.text : field.text;
            }
        }

        // The texts of the songs of the player's under the game's keys
        // (Game::songKey): the title and artist from the file's tags (else
        // the title from its name), the album from its tag.
        void makeSongTexts()
        {
            constexpr size_t kTextLimit = 60;
            for (uint32_t wave = 0; wave < game().songs; ++wave)
            {
                std::filesystem::path path;
                auto track = openCustomTrack(wave, &path);
                if (!track)
                    continue;
                std::string title = captionText(track->tag("title"), kTextLimit);
                if (title.empty())
                    title = captionText(zx::audio_file::nameFromFileName(path), kTextLimit);
                if (title.empty() || title.find_first_not_of("0123456789") == std::string::npos)
                    continue;
                addSongTexts(wave, title, captionText(track->tag("artist"), kTextLimit), captionText(track->tag("album"), kTextLimit));
            }
        }

        // The added songs' texts, under their waves' keys once MUSIC.ZSD's
        // head has numbered them.
        void makeAddedSongTexts()
        {
            State &st = state();
            const AddedSongs &added = addedSongList();
            if (st.addedSongTexts || !added.made)
                return;
            st.addedSongTexts = true;
            constexpr size_t kTextLimit = 60;
            for (const AddedSong &song : added.songs)
            {
                std::string title = captionText(song.title, kTextLimit);
                if (title.empty())
                    title = "SONG " + std::to_string(song.wave + 1u);
                addSongTexts(song.wave, title, captionText(song.artist, kTextLimit), captionText(song.album, kTextLimit));
            }
        }

        // stringTableText(key): a custom song's text for its key, from guest
        // memory the first time it is asked for (the game's heap); every
        // other key the game's own table's.
        void songText(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            makeAddedSongTexts();
            const auto found = st.songTexts.find(getRegU32(ctx, kRegA0));
            if (found == st.songTexts.end())
            {
                st.originalStringTableText(rdram, ctx, runtime);
                return;
            }
            if (!st.songTextsPlaced)
            {
                st.songTextsPlaced = true;
                size_t bytes = 0;
                for (const auto &[key, entry] : st.songTexts)
                    bytes += entry.text.size() + 1u;
                uint32_t at = runtime->guestMalloc(static_cast<uint32_t>(bytes));
                for (auto &[key, entry] : st.songTexts)
                {
                    if (!at)
                        break;
                    entry.address = at;
                    writeGuestString(rdram, at, entry.text, entry.text.size() + 1u);
                    at += static_cast<uint32_t>(entry.text.size() + 1u);
                }
            }
            State::SongText &entry = found->second;
            if (!entry.address)
            {
                // (an added song's, numbered after the others were placed)
                entry.address = runtime->guestMalloc(static_cast<uint32_t>(entry.text.size() + 1u));
                if (entry.address)
                    writeGuestString(rdram, entry.address, entry.text, entry.text.size() + 1u);
            }
            if (!entry.address)
            {
                st.originalStringTableText(rdram, ctx, runtime);
                return;
            }
            zge_host::sites::seen("sound.songText");
            if (entry.title)
            {
                std::cout << "[sound] custom soundtrack: song " << songNumber(entry.wave) << " shown as \"" << entry.text << "\""
                          << std::endl;
                entry.title = false; // (said once)
            }
            returnTo(ctx, entry.address);
        }

        // ---- hooks -------------------------------------------------------------------------

        // bindSoundRpc: the IOP module answers at once.
        void bind(uint8_t *, R5900Context *ctx, PS2Machine *)
        {
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        void rpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            driver().rpc(rdram, ctx, runtime);
        }
    }

    std::vector<AddedSong> addedSongs()
    {
        const AddedSongs &added = addedSongList();
        return added.made ? added.songs : std::vector<AddedSong>{};
    }

    bool hasAddedSongs()
    {
        return !addedSongList().songs.empty();
    }

    bool install(PS2Machine &runtime, const Game &gameSound)
    {
        State &st = state();
        st.game = &gameSound;
        if (const char *log = std::getenv("ZX_SOUND_LOG"); log && *log)
        {
            st.log = std::fopen(log, "w");
            if (st.log)
                std::setvbuf(st.log, nullptr, _IOLBF, 0); // the runs end by kill
        }
        const char *mode = std::getenv("ZX_SOUND");
        // The game's own movie sound, when its movies run in their PS2 form
        // (ZX_MOVIES=hle, or their host form not installed:
        // zge_host::movieForm), goes to the IOP (sceSifSetDma, SDRDRV's
        // block transfer into the SPU2's core 1 input, which needs the SPU2
        // that ZAUDIO's Init set up): with the native driver nothing drains
        // the game's movie audio ring, the ring fills, the demux stops taking
        // the stream and the intro waits in sceMpegGetPicture. Then ZAUDIO
        // stays on the IOP.
        const bool ps2Movies = zge_host::movieForm() == zge_host::MovieForm::Ps2;
        if (zge_host::soundOnIop(mode, zge_host::movieForm()))
        {
            std::cout << "[sound] ZAUDIO on the IOP and the emulated SPU2 ("
                      << (ps2Movies ? "the movies' PS2 form needs it" : "ZX_SOUND=spu2") << ")" << std::endl;
            if (st.log)
            {
                st.originalRpc = runtime.lookupFunction(gameSound.rpc);
                if (st.originalRpc)
                    runtime.replaceFunction(gameSound.rpc, &loggingRpc);
                runtime.addVSyncHook([](uint8_t *rdram, uint64_t tick) { logStatus(rdram, tick); });
            }
            return false;
        }
        if (const char *trace = std::getenv("ZX_TRACE_SOUND"); trace && *trace)
        {
            st.traceLevel = std::atoi(trace);
            st.trace = st.traceLevel > 0;
        }
        if (!runtime.replaceFunction(gameSound.bind, &bind) || !runtime.replaceFunction(gameSound.rpc, &rpc))
        {
            std::cerr << "[sound] native sound hooks not installed" << std::endl;
            return false;
        }
        if (gameSound.rings == StreamRings::Shared)
            driver().configure(std::make_unique<SharedRings>());
        else
            driver().configure(std::make_unique<PerStreamRings>());
        if (const char *gains = std::getenv("ZX_SOUND_BUS_GAIN"); gains && *gains)
        {
            float e = 1.0f, m = 1.0f, sp = 1.0f;
            if (std::sscanf(gains, "%f,%f,%f", &e, &m, &sp) >= 1)
            {
                st.busGain[Effects] = e;
                st.busGain[Music] = m;
                st.busGain[Speech] = sp;
            }
        }
        // The songs a mod adds: MUSIC.ZSD served with their waves.
        findAddedSongs();
        const bool anyAdded = !addedSongList().songs.empty();
        if (anyAdded)
            gf::registerHead(kMusicBankPath, &musicBankWithSongs, kAddedSongsOwner);
        // Song titles: only when there is a song of the player's (else the
        // game runs untouched).
        bool anyCustom = false;
        for (uint32_t wave = 0; wave < gameSound.songs && !anyCustom; ++wave)
            anyCustom = !customTrackFiles(wave).empty();
        if (anyCustom && gameSound.setPlaylistEntry && gameSound.gameSoundtrack)
        {
            st.originalPlaylistEntry = runtime.lookupFunction(gameSound.setPlaylistEntry);
            if (!st.originalPlaylistEntry || !runtime.replaceFunction(gameSound.setPlaylistEntry, &playlistEntry))
                std::cerr << "[sound] custom soundtrack titles not installed" << std::endl;
        }
        if ((anyCustom || anyAdded) && gameSound.stringTableText && gameSound.songKey)
        {
            makeSongTexts();
            st.originalStringTableText = runtime.lookupFunction(gameSound.stringTableText);
            if ((!st.songTexts.empty() || anyAdded) &&
                (!st.originalStringTableText || !runtime.replaceFunction(gameSound.stringTableText, &songText)))
                std::cerr << "[sound] custom soundtrack titles not installed" << std::endl;
        }
        if (const char *dump = std::getenv("ZX_SOUND_MEMDUMP"); dump && *dump)
            st.memDump = std::fopen(dump, "w");
        runtime.setNativeAudio(true);
        runtime.addVSyncHook([&runtime](uint8_t *rdram, uint64_t) { driver().vsync(rdram, &runtime); });
        std::cout << "[sound] native ZAudio driver (ZX_SOUND=spu2 for ZAUDIO on the IOP)" << std::endl;
        return true;
    }
}
