// Ghost riding (ghost_rider.h): the track's file, the clock, the rider's parts noted and the ghost's draws.

#include "runtime/ghost_rider.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/panel_snapshot.h"
#include "runtime/play_recording.h"
#include "runtime/ps2_memory_map.h"
#include "runtime/session.h"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace zx::ghost_rider
{
    namespace
    {
        // The file: this line, then one record per stretch of play, each whole on its own (a file cut short reads
        // to its last whole one). A record's head, plain (little-endian): its frames' compressed size and their size
        // (u32 each), the level and the rider (u8 length, its characters, each), the frame count (u32); then its
        // frames compressed with zstd: per frame its part count (u16) and per part its vertex count (u32), bone count
        // (u16), the world's rows 0 .. 3 (xyz, 12 floats) and the bones' palette (12 floats a bone). The head alone
        // says what a track holds (trackStretches) without its frames read.
        constexpr char kMagic[] = "ZXGHOST2\n";
        constexpr size_t kMagicBytes = sizeof(kMagic) - 1;
        constexpr int kCompressionLevel = 3;
        // A frame with no part drawn in it shows the last one drawn at most this many frames before (a game that
        // drew its rider every other frame).
        constexpr size_t kHoldFrames = 8;
        // Frame interpolation's identity of a ghost's draw: the live draw's with this bit (a node's key is its
        // address times 256 and the view: 40 bits).
        constexpr uint64_t kGhostMotionBit = 1ull << 62;

        struct Writer
        {
            std::string bytes;
            template <typename T>
            void put(T value)
            {
                char raw[sizeof(T)];
                std::memcpy(raw, &value, sizeof(T));
                bytes.append(raw, sizeof(T));
            }
        };
        struct Reader
        {
            const std::string &bytes;
            size_t at = 0;
            bool ok = true;
            template <typename T>
            T get()
            {
                T value{};
                if (at + sizeof(T) > bytes.size())
                {
                    ok = false;
                    return value;
                }
                std::memcpy(&value, bytes.data() + at, sizeof(T));
                at += sizeof(T);
                return value;
            }
        };

        void putText(Writer &w, const std::string &text)
        {
            const size_t length = std::min<size_t>(text.size(), 255);
            w.put<uint8_t>(static_cast<uint8_t>(length));
            w.bytes.append(text, 0, length);
        }
        bool getText(Reader &r, std::string &out)
        {
            const uint8_t length = r.get<uint8_t>();
            if (!r.ok || r.at + length > r.bytes.size())
                return false;
            out.assign(r.bytes, r.at, length);
            r.at += length;
            return true;
        }

        std::string framesBytes(const Stretch &stretch)
        {
            Writer w;
            for (const Frame &frame : stretch.frames)
            {
                w.put<uint16_t>(static_cast<uint16_t>(frame.size()));
                for (const PartPose &part : frame)
                {
                    w.put<uint32_t>(part.vertices);
                    w.put<uint16_t>(static_cast<uint16_t>(part.bones));
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 3; ++c)
                            w.put<float>(part.world[r][c]);
                    for (size_t i = 0; i < size_t{part.bones} * 12u; ++i)
                        w.put<float>(i < part.palette.size() ? part.palette[i] : 0.0f);
                }
            }
            return std::move(w.bytes);
        }

        // A stretch's frames (`frames` of them, as its head says) from their bytes.
        bool readFrames(const std::string &bytes, uint32_t frames, Stretch &out)
        {
            Reader r{bytes};
            if (frames > bytes.size())
                return false;
            out.frames.resize(frames);
            for (Frame &frame : out.frames)
            {
                const uint16_t parts = r.get<uint16_t>();
                frame.resize(r.ok ? parts : 0u);
                for (PartPose &part : frame)
                {
                    part.vertices = r.get<uint32_t>();
                    part.bones = r.get<uint16_t>();
                    for (int row = 0; row < 4; ++row)
                    {
                        for (int c = 0; c < 3; ++c)
                            part.world[row][c] = r.get<float>();
                        part.world[row][3] = row == 3 ? 1.0f : 0.0f;
                    }
                    if (!r.ok || r.at + size_t{part.bones} * 12u * sizeof(float) > bytes.size())
                        return false;
                    part.palette.resize(size_t{part.bones} * 12u);
                    for (float &v : part.palette)
                        v = r.get<float>();
                }
                if (!r.ok)
                    return false;
            }
            return r.ok && r.at == bytes.size();
        }

        bool appendRecord(std::ofstream &out, const Stretch &stretch)
        {
            const std::string raw = framesBytes(stretch);
            std::string packed(ZSTD_compressBound(raw.size()), '\0');
            const size_t size = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), kCompressionLevel);
            if (ZSTD_isError(size))
                return false;
            Writer head;
            head.put<uint32_t>(static_cast<uint32_t>(size));
            head.put<uint32_t>(static_cast<uint32_t>(raw.size()));
            putText(head, stretch.level);
            putText(head, stretch.rider);
            head.put<uint32_t>(static_cast<uint32_t>(stretch.frames.size()));
            out.write(head.bytes.data(), static_cast<std::streamsize>(head.bytes.size()));
            out.write(packed.data(), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        }

        void fail(std::string *error, std::string why)
        {
            if (error)
                *error = std::move(why);
        }

        // ---- 4 x 4 row-vector matrices -------------------------------------------------------------------------

        void multiply(const float a[4][4], const float b[4][4], float out[4][4])
        {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }

        // The inverse of a 3 x 3 (false: none).
        bool inverse3(const float m[3][3], float out[3][3])
        {
            const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
            const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
            const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
            const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
            if (!(std::abs(det) > 1e-12f))
                return false;
            const float k = 1.0f / det;
            out[0][0] = c00 * k;
            out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
            out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
            out[1][0] = c01 * k;
            out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
            out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
            out[2][0] = c02 * k;
            out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
            out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
            return true;
        }

        // ---- the boot's state ----------------------------------------------------------------------------------

        struct Boot
        {
            bool on = false;
            RiderObjects rider = nullptr;
            uint32_t riderObjects[kMaxRiderObjects] = {};
            size_t riderObjectCount = 0;
            std::unique_ptr<panel::Snapshot> snapshot;

            // The clock: in a level (its first frame of play seen), its name, the frame of play.
            bool inLevel = false;
            bool started = false;
            bool playing = false;
            std::string level;
            uint64_t frame = 0;

            // The track made: where ("1": beside the recording, resolved as the first level begins), the stretch
            // being made.
            std::string trackAsked;
            std::filesystem::path trackFile;
            bool trackResolved = false;
            Stretch made;
            uint64_t stretchesWritten = 0;
            uint64_t partsNoted = 0, ghostsGiven = 0; // (for the log: the live rider's parts, the ghost's poses given out)

            // The ghost: its track, the level it rides in ("" any), the stretch it rides now.
            Track ghost;
            std::string ghostLevel;
            const Stretch *riding = nullptr;

            // This frame's draws: what was noted (a part drawn in two views is noted once), the ghosts given out
            // by part (their order among parts of the same counts), the last noted.
            uint64_t drawFrame = ~uint64_t{0};
            std::unordered_set<uint64_t> noted;
            std::unordered_map<uint64_t, uint32_t> sameParts;
            std::unordered_map<uint64_t, std::shared_ptr<const Ghost>> given;
        };
        Boot &boot() { return zx::session::state<Boot>(); }

        uint64_t drawKey(uint32_t node, uint32_t part) { return (static_cast<uint64_t>(node) << 32) | part; }
        uint64_t partKey(uint32_t vertices, uint32_t bones) { return (static_cast<uint64_t>(vertices) << 32) | bones; }

        // Where the track goes, once: the file asked for, or beside the recording made or replayed.
        void resolveTrackFile(Boot &b)
        {
            if (b.trackResolved)
                return;
            b.trackResolved = true;
            if (b.trackAsked != "1")
            {
                b.trackFile = b.trackAsked;
                return;
            }
            if (play_recording::mode() == play_recording::Mode::Record)
                b.trackFile = trackOf(play_recording::recordingPath());
            else if (const char *replay = zx::boot::text("ZX_REPLAY"); replay && replay[0])
            {
                const std::filesystem::path beside = trackOf(replay);
                std::error_code ec;
                if (!std::filesystem::exists(beside, ec))
                    b.trackFile = beside;
            }
            if (b.trackFile.empty())
                std::cout << "[ghost] no track made: no recording made or replayed without one" << std::endl;
        }

        // The stretch being made, written at its end (the level left, the boot's end).
        void endStretch(Boot &b)
        {
            const bool drawn = std::any_of(b.made.frames.begin(), b.made.frames.end(), [](const Frame &f) { return !f.empty(); });
            if (!b.trackAsked.empty() && drawn)
            {
                resolveTrackFile(b);
                std::string error;
                if (!b.trackFile.empty())
                {
                    if (appendStretch(b.trackFile, b.made, &error))
                    {
                        ++b.stretchesWritten;
                        std::cout << "[ghost] track " << b.trackFile.string() << ": " << b.made.level << ", "
                                  << b.made.frames.size() << " frames of play"
                                  << (b.made.rider.empty() ? std::string() : ", " + b.made.rider) << std::endl;
                    }
                    else
                        std::cout << "[ghost] track " << b.trackFile.string() << " not written: " << error << std::endl;
                }
            }
            b.made = {};
        }

        void beginLevel(Boot &b, const std::string &level)
        {
            b.inLevel = true;
            b.started = false;
            b.playing = false;
            b.level = level;
            b.frame = 0;
            b.drawFrame = ~uint64_t{0}; // (this frame's draws begin again)
            b.made = {};
            b.made.level = level;
            b.riding = nullptr;
            std::cout << "[ghost] level " << level << " begins" << std::endl;
            // (where its track goes, while the recording is open)
            if (!b.trackAsked.empty())
                resolveTrackFile(b);
            if (!b.ghost.stretches.empty() && (b.ghostLevel.empty() || b.ghostLevel == level))
                b.riding = stretchFor(b.ghost, level);
            if (b.riding)
                std::cout << "[ghost] riding against " << level << ": " << b.riding->frames.size() << " frames of play"
                          << (b.riding->rider.empty() ? std::string() : ", " + b.riding->rider) << std::endl;
        }

        // The level's first frame of play: who rides it, and so whether the ghost does (only its own rider's).
        void beginPlay(Boot &b, const std::string &rider)
        {
            b.made.rider = rider;
            if (b.riding && !b.riding->rider.empty() && !rider.empty() && b.riding->rider != rider)
            {
                std::cout << "[ghost] no ghost in " << b.level << ": this run's rider is " << rider << ", the ghost's "
                          << b.riding->rider << std::endl;
                b.riding = nullptr;
            }
        }

        void endLevel(Boot &b)
        {
            endStretch(b);
            b.inLevel = false;
            b.started = false;
            b.playing = false;
            b.riding = nullptr;
        }

        // At each VBlank start (EE thread): the rider's node, and the clock from the panel's snapshot.
        void onVsync(uint8_t *rdram, uint64_t)
        {
            Boot &b = boot();
            if (!b.on)
                return;
            b.riderObjectCount = b.rider ? std::min(b.rider(rdram, b.riderObjects), kMaxRiderObjects) : 0u;
            panel::captureSnapshot(rdram, PS2_RAM_SIZE, *b.snapshot);
            const panel::Snapshot &s = *b.snapshot;
            if (s.mode == panel::GameMode::NoLevel)
            {
                if (b.inLevel)
                    endLevel(b);
                return;
            }
            const std::string level(s.levelFile, strnlen(s.levelFile, sizeof(s.levelFile)));
            if (!b.inLevel || level != b.level)
            {
                if (b.inLevel)
                    endLevel(b);
                beginLevel(b, level);
            }
            b.playing = s.mode == panel::GameMode::Play;
            if (b.playing)
            {
                if (b.started)
                    ++b.frame;
                else
                {
                    b.started = true;
                    beginPlay(b, std::string(s.skaterName, strnlen(s.skaterName, sizeof(s.skaterName))));
                }
            }
        }

        void onStopped(PS2Machine &)
        {
            Boot &b = boot();
            if (b.inLevel)
                endLevel(b);
            std::cout << "[ghost] run stopped: " << b.partsNoted << " rider parts noted, " << b.ghostsGiven
                      << " ghost poses given to the draws, " << b.stretchesWritten << " stretches written" << std::endl;
        }

        // The ghost's frame now: the one of this frame of play, else the last drawn shortly before (null: none, or
        // past the stretch's end).
        const Frame *ghostFrame(const Boot &b)
        {
            if (!b.riding || b.frame >= b.riding->frames.size())
                return nullptr;
            for (size_t back = 0; back <= kHoldFrames && back <= b.frame; ++back)
            {
                const Frame &f = b.riding->frames[b.frame - back];
                if (!f.empty())
                    return &f;
            }
            return nullptr;
        }
    }

    std::filesystem::path trackOf(const std::filesystem::path &recording)
    {
        std::filesystem::path track = recording;
        track.replace_extension(".zghost");
        return track;
    }

    bool writeTrack(const std::filesystem::path &file, const Track &track, std::string *error)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            fail(error, "cannot write " + file.string());
            return false;
        }
        out.write(kMagic, static_cast<std::streamsize>(kMagicBytes));
        for (const Stretch &stretch : track.stretches)
            if (!appendRecord(out, stretch))
            {
                fail(error, "cannot write " + file.string());
                return false;
            }
        return true;
    }

    bool appendStretch(const std::filesystem::path &file, const Stretch &stretch, std::string *error)
    {
        std::error_code ec;
        const bool fresh = !std::filesystem::exists(file, ec) || std::filesystem::file_size(file, ec) == 0u;
        std::ofstream out(file, std::ios::binary | std::ios::app);
        if (!out)
        {
            fail(error, "cannot write " + file.string());
            return false;
        }
        if (fresh)
            out.write(kMagic, static_cast<std::streamsize>(kMagicBytes));
        if (!appendRecord(out, stretch))
        {
            fail(error, "cannot write " + file.string());
            return false;
        }
        return true;
    }

    namespace
    {
        // Each whole record of a track's file, in order: its head, and its compressed frames when `frames` (else
        // they are skipped, not read), with their size once read. `each` returns false to stop. False for a file
        // that is not a track.
        template <typename Each>
        bool eachRecord(const std::filesystem::path &file, std::string *error, bool frames, Each each)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                fail(error, "cannot read " + file.string());
                return false;
            }
            std::error_code ec;
            const uint64_t fileSize = std::filesystem::file_size(file, ec);
            std::string magic(kMagicBytes, '\0');
            if (ec || !in.read(magic.data(), static_cast<std::streamsize>(magic.size())) || magic != kMagic)
            {
                fail(error, file.string() + " is not a ghost track");
                return false;
            }
            // (a head is at most 4 + 4 + 256 + 256 + 4 bytes: read that much, or what is left, and parse it)
            constexpr size_t kHeadMost = 4 + 4 + 256 + 256 + 4;
            uint64_t at = kMagicBytes;
            while (at < fileSize)
            {
                std::string headBytes(static_cast<size_t>(std::min<uint64_t>(kHeadMost, fileSize - at)), '\0');
                in.seekg(static_cast<std::streamoff>(at));
                if (!in.read(headBytes.data(), static_cast<std::streamsize>(headBytes.size())))
                    break;
                Reader r{headBytes};
                const uint32_t packed = r.get<uint32_t>();
                const uint32_t size = r.get<uint32_t>();
                StretchSummary head;
                if (!r.ok || !getText(r, head.level) || !getText(r, head.rider))
                    break;
                head.frames = r.get<uint32_t>();
                if (!r.ok || at + r.at + packed > fileSize)
                    break; // (cut short: the whole stretches before)
                at += r.at;
                std::string body;
                if (frames)
                {
                    body.resize(packed);
                    in.seekg(static_cast<std::streamoff>(at));
                    if (!in.read(body.data(), static_cast<std::streamsize>(packed)))
                        break;
                }
                at += packed;
                if (!each(std::move(head), body, size))
                    break;
            }
            return true;
        }
    }

    bool readTrack(const std::filesystem::path &file, Track &out, std::string *error)
    {
        out = {};
        return eachRecord(file, error, true,
                          [&out](StretchSummary head, const std::string &packed, uint32_t size)
                          {
                              std::string raw(size, '\0');
                              const size_t got = ZSTD_decompress(raw.data(), raw.size(), packed.data(), packed.size());
                              Stretch stretch;
                              stretch.level = std::move(head.level);
                              stretch.rider = std::move(head.rider);
                              if (ZSTD_isError(got) || got != size || !readFrames(raw, head.frames, stretch))
                                  return false;
                              out.stretches.push_back(std::move(stretch));
                              return true;
                          });
    }

    std::vector<StretchSummary> trackStretches(const std::filesystem::path &file)
    {
        std::vector<StretchSummary> out;
        eachRecord(file, nullptr, false,
                   [&out](StretchSummary head, const std::string &, uint32_t)
                   {
                       out.push_back(std::move(head));
                       return true;
                   });
        return out;
    }

    const Stretch *stretchFor(const Track &track, const std::string &level)
    {
        const Stretch *best = nullptr;
        for (const Stretch &stretch : track.stretches)
            if (stretch.level == level && (!best || stretch.frames.size() > best->frames.size()))
                best = &stretch;
        return best;
    }

    void install(PS2Machine &runtime, RiderObjects rider)
    {
        Boot &b = boot();
        const char *track = zx::boot::text("ZX_GHOST_TRACK");
        const char *ghost = zx::boot::text("ZX_GHOST");
        if (track && track[0])
            b.trackAsked = track;
        if (ghost && ghost[0])
        {
            std::filesystem::path file = ghost;
            if (file.extension() != ".zghost")
                file = trackOf(file);
            std::string error;
            if (!readTrack(file, b.ghost, &error))
                std::cout << "[ghost] no ghost: " << error << std::endl;
            else
            {
                const char *level = zx::boot::text("ZX_GHOST_LEVEL");
                b.ghostLevel = level ? level : "";
                std::cout << "[ghost] track " << file.string() << ": " << b.ghost.stretches.size() << " stretches of play"
                          << (b.ghostLevel.empty() ? std::string() : ", riding in " + b.ghostLevel) << std::endl;
            }
        }
        if (b.trackAsked.empty() && b.ghost.stretches.empty())
            return;
        b.on = true;
        b.rider = rider;
        b.snapshot = std::make_unique<panel::Snapshot>();
        runtime.addVSyncHook(&onVsync);
        // (the stretch being made written when the run stops: a runner's single boot never ends its session)
        runtime.addStoppedHook(&onStopped);
        std::cout << "[ghost] on:" << (b.trackAsked.empty() ? "" : " a track made (" + b.trackAsked + ")")
                  << (b.ghost.stretches.empty() ? "" : " a ghost ridden") << std::endl;
    }

    bool active() { return boot().on; }

    const uint32_t *riderObjects(size_t &count)
    {
        const Boot &b = boot();
        count = b.riderObjectCount;
        return b.riderObjects;
    }

    std::shared_ptr<const Ghost> noteRiderPart(uint32_t node, uint32_t part, PartPose pose, bool lightsTurnedByWorld)
    {
        Boot &b = boot();
        if (!b.on || !b.inLevel || !b.started)
            return nullptr;
        if (b.drawFrame != b.frame)
        {
            b.drawFrame = b.frame;
            b.noted.clear();
            b.sameParts.clear();
            b.given.clear();
        }
        const uint64_t key = drawKey(node, part);
        if (!b.noted.insert(key).second)
            return ghostNoted(node, part); // (drawn again this frame: another view)
        std::shared_ptr<const Ghost> ghost;
        if (const Frame *frame = ghostFrame(b))
        {
            // The n-th part of these counts this frame takes the ghost's n-th.
            uint32_t &n = b.sameParts[partKey(pose.vertices, pose.bones)];
            uint32_t seen = 0;
            for (const PartPose &p : *frame)
                if (p.vertices == pose.vertices && p.bones == pose.bones && seen++ == n)
                {
                    ghost = std::make_shared<const Ghost>(Ghost{p, lightsTurnedByWorld});
                    break;
                }
            ++n;
        }
        b.given[key] = ghost;
        ++b.partsNoted;
        if (ghost)
            ++b.ghostsGiven;
        // ZX_TRACE_GHOST=<n> (developers): every n-th frame of play, where the live rider and its ghost are.
        static const uint64_t traceEvery = []() -> uint64_t
        {
            const char *v = std::getenv("ZX_TRACE_GHOST");
            return v ? std::strtoull(v, nullptr, 0) : 0u;
        }();
        if (traceEvery && b.frame % traceEvery == 0u)
        {
            std::cout << "[ghost] frame " << b.frame << " part " << pose.vertices << "/" << pose.bones << ": live at "
                      << pose.world[3][0] << " " << pose.world[3][1] << " " << pose.world[3][2];
            if (ghost)
                std::cout << ", ghost at " << ghost->pose.world[3][0] << " " << ghost->pose.world[3][1] << " "
                          << ghost->pose.world[3][2];
            else
                std::cout << ", no ghost";
            std::cout << std::endl;
        }
        if (!b.trackAsked.empty() && b.playing)
        {
            if (b.made.frames.size() <= b.frame)
                b.made.frames.resize(b.frame + 1u);
            b.made.frames[b.frame].push_back(std::move(pose));
        }
        return ghost;
    }

    std::shared_ptr<const Ghost> ghostNoted(uint32_t node, uint32_t part)
    {
        Boot &b = boot();
        if (!b.on)
            return nullptr;
        const auto found = b.given.find(drawKey(node, part));
        return found == b.given.end() ? nullptr : found->second;
    }

    GSNativeDraw ghostDraw(const GSNativeDraw &live, const Ghost &ghost, const float screen[4][4], const float clip[4][4])
    {
        GSNativeDraw d = live;
        const PartPose &pose = ghost.pose;
        multiply(pose.world, screen, d.transform);
        for (int r = 0; r < 4; ++r)
            d.transform[r][2] *= 16.0f; // FTOI4: the GS's Z
        multiply(pose.world, clip, d.clipTransform);
        auto skin = live.skin ? std::make_shared<GSNativeSkinning>(*live.skin) : std::make_shared<GSNativeSkinning>();
        std::memset(skin->palette, 0, sizeof(skin->palette));
        const uint32_t bones = std::min<uint32_t>(pose.bones, GSNativeSkinning::kPaletteRows / 4);
        for (uint32_t k = 0; k < bones && (k + 1u) * 12u <= pose.palette.size(); ++k)
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 3; ++c)
                    skin->palette[k * 4u + r][c] = pose.palette[k * 12u + r * 3u + c];
                skin->palette[k * 4u + r][3] = r == 3 ? 1.0f : 0.0f;
            }
        // Lights turned by the live node's world (lightDir = world x directions): turned by the ghost's instead.
        if (ghost.lightsTurnedByWorld)
        {
            float live3[3][3], undo[3][3];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    live3[r][c] = live.motion.world[r][c];
            if (inverse3(live3, undo))
            {
                float directions[3][3], turned[3][3];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        directions[r][c] = undo[r][0] * skin->lightDir[0][c] + undo[r][1] * skin->lightDir[1][c] +
                                           undo[r][2] * skin->lightDir[2][c];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        turned[r][c] = pose.world[r][0] * directions[0][c] + pose.world[r][1] * directions[1][c] +
                                       pose.world[r][2] * directions[2][c];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        skin->lightDir[r][c] = turned[r][c];
            }
        }
        d.skin = std::move(skin);
        d.motion.key = live.motion.key ? (live.motion.key | kGhostMotionBit) : 0u;
        std::memcpy(d.motion.world, pose.world, sizeof(d.motion.world));
        d.seeThrough = kSeeThrough;
        return d;
    }

    GSNativeDraw ofRun(const GSNativeDraw &ghost, const GSNativeDraw &liveRun)
    {
        GSNativeDraw d = ghost;
        d.first = liveRun.first;
        d.count = liveRun.count;
        d.uvTransform = liveRun.uvTransform;
        d.uvFromNormal = liveRun.uvFromNormal;
        std::memcpy(d.uv, liveRun.uv, sizeof(d.uv));
        d.alpha = liveRun.alpha;
        d.normalOffset = liveRun.normalOffset;
        return d;
    }
}
