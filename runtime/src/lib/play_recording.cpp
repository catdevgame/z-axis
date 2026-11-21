// Play recordings (runtime/play_recording.h): the writer, the replay, the
// state checksums and the boot's header.
//
// Records (type byte, LEB128 length, payload):
//   1 header    text, "key=value" lines
//   2 file      name (LEB128 length + bytes), contents (LEB128 length + bytes)
//   3 input     channel, key, reads since the stream's previous record, vsync,
//               size, then a change mask (LEB128, bit i: byte i changed) and
//               the changed bytes; a mask of 0 with a size over 63 means the
//               whole value follows. A stream's first record is against zeros.
//   4 checksum  vsync, count, count u32 (little endian): EE RAM per megabyte
//   5 frame     vsync, frame us, game, VIF1, GS, host CPU us, flips, pacer drop us
//   6 event     vsync, kind (string), text (string)
//   7 end       vsync, why (string)
#include "runtime/play_recording.h"

#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#if __has_include("play_recording_commit.h")
#include "play_recording_commit.h" // ZX_BUILD_COMMIT, written by the build (cmake/BuildCommit.cmake)
#endif
#ifndef ZX_BUILD_COMMIT
#define ZX_BUILD_COMMIT "unknown"
#endif

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/sysctl.h>
#elif defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#if defined(_WIN32)
#define environ _environ
#else
extern char **environ;
#endif

namespace play_recording
{
    namespace
    {
        constexpr char kMagic[8] = {'Z', 'A', 'X', 'R', 'E', 'C', '1', '\n'};
        enum RecordType : uint8_t
        {
            kHeader = 1,
            kFile = 2,
            kInput = 3,
            kChecksum = 4,
            kFrame = 5,
            kEvent = 6,
            kEnd = 7,
        };
        constexpr uint32_t kChecksumBlock = 1u << 20; // a megabyte of EE RAM per checksum
        constexpr uint64_t kDefaultChecksumEvery = 60u; // vsyncs
        constexpr uint64_t kFlushEvery = 60u;          // vsyncs between writes to the file

        const char *channelName(Channel channel)
        {
            static const char *const names[] = {"pad0", "pad1", "port-connected", "hotkey", "mouse-look", "key-recent",
                                                "prompt-device", "prompt-generation", "free-input", "date",
                                                "view-aspect", "widescreen", "random", "draw-distance", "motion-blur",
                                                "native-draws", "setting"};
            const size_t i = static_cast<size_t>(channel);
            return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
        }

        void putVarint(std::string &out, uint64_t value)
        {
            while (value >= 0x80u)
            {
                out.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
                value >>= 7;
            }
            out.push_back(static_cast<char>(value));
        }

        void putString(std::string &out, const std::string &text)
        {
            putVarint(out, text.size());
            out += text;
        }

        void putRecord(std::string &out, RecordType type, const std::string &payload)
        {
            out.push_back(static_cast<char>(type));
            putVarint(out, payload.size());
            out += payload;
        }

        struct Cursor
        {
            const uint8_t *p = nullptr;
            const uint8_t *end = nullptr;
            bool ok = true;
            uint64_t varint()
            {
                uint64_t value = 0;
                for (int shift = 0; shift < 64; shift += 7)
                {
                    if (p >= end)
                    {
                        ok = false;
                        return 0;
                    }
                    const uint8_t byte = *p++;
                    value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
                    if ((byte & 0x80u) == 0u)
                        return value;
                }
                ok = false;
                return value;
            }
            std::string string()
            {
                const uint64_t size = varint();
                if (!ok || static_cast<uint64_t>(end - p) < size)
                {
                    ok = false;
                    return {};
                }
                std::string text(reinterpret_cast<const char *>(p), static_cast<size_t>(size));
                p += size;
                return text;
            }
            uint32_t u32()
            {
                if (end - p < 4)
                {
                    ok = false;
                    return 0;
                }
                const uint32_t value = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                p += 4;
                return value;
            }
        };

        // One megabyte's checksum: four independent multiply-xor lanes over
        // 64-bit words, folded to 32 bits.
        uint32_t blockChecksum(const uint8_t *bytes, size_t size)
        {
            uint64_t lane[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull,
                                0x27D4EB2F165667C5ull};
            constexpr uint64_t kMul = 0xFF51AFD7ED558CCDull;
            size_t i = 0;
            for (; i + 32 <= size; i += 32)
            {
                for (int l = 0; l < 4; ++l)
                {
                    uint64_t word;
                    std::memcpy(&word, bytes + i + l * 8, sizeof(word));
                    lane[l] = (lane[l] ^ word) * kMul;
                    lane[l] ^= lane[l] >> 29;
                }
            }
            uint64_t h = lane[0] ^ (lane[1] * 3u) ^ (lane[2] * 5u) ^ (lane[3] * 7u) ^ size;
            h ^= h >> 33;
            h *= 0xC4CEB9FE1A85EC53ull;
            h ^= h >> 33;
            return static_cast<uint32_t>(h ^ (h >> 32));
        }

        std::vector<uint32_t> ramChecksums(const uint8_t *rdram)
        {
            std::vector<uint32_t> sums(PS2_RAM_SIZE / kChecksumBlock);
            for (size_t b = 0; b < sums.size(); ++b)
                sums[b] = blockChecksum(rdram + b * kChecksumBlock, kChecksumBlock);
            return sums;
        }

        uint64_t fileHash(const uint8_t *bytes, size_t size)
        {
            uint64_t h = 0xCBF29CE484222325ull; // FNV-1a
            for (size_t i = 0; i < size; ++i)
            {
                h ^= bytes[i];
                h *= 0x100000001B3ull;
            }
            return h;
        }

        uint64_t fileHash(const std::string &bytes)
        {
            return fileHash(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
        }

        std::string hex64(uint64_t value)
        {
            char text[17];
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
            return text;
        }

        bool readWhole(const std::filesystem::path &path, std::string &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            std::ostringstream text;
            text << in.rdbuf();
            out = text.str();
            return true;
        }

        std::string deviceLines()
        {
            std::string lines;
#if defined(__APPLE__)
            char model[128] = {};
            size_t size = sizeof(model);
            if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0)
                lines += "device=Mac " + std::string(model) + "\n";
            char os[64] = {};
            size = sizeof(os);
            if (sysctlbyname("kern.osproductversion", os, &size, nullptr, 0) == 0)
                lines += "os=macOS " + std::string(os) + "\n";
            // Low Power Mode and the thermal state, from Foundation when it is loaded.
            if (Class info = objc_getClass("NSProcessInfo"))
            {
                id process = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(info, sel_registerName("processInfo"));
                if (process)
                {
                    const SEL lowPower = sel_registerName("isLowPowerModeEnabled");
                    if (class_respondsToSelector(info, lowPower))
                        lines += std::string("power=") +
                                 (reinterpret_cast<BOOL (*)(id, SEL)>(objc_msgSend)(process, lowPower) ? "low power mode"
                                                                                                          : "normal") +
                                 "\n";
                    const SEL thermal = sel_registerName("thermalState");
                    if (class_respondsToSelector(info, thermal))
                        lines += "thermal=" +
                                 std::to_string(reinterpret_cast<long (*)(id, SEL)>(objc_msgSend)(process, thermal)) +
                                 "\n";
                }
            }
#elif defined(__ANDROID__)
            auto property = [](const char *name)
            {
                char value[PROP_VALUE_MAX] = {};
                __system_property_get(name, value);
                return std::string(value);
            };
            lines += "device=" + property("ro.product.manufacturer") + " " + property("ro.product.model") + "\n";
            lines += "os=Android " + property("ro.build.version.release") + "\n";
            // What a performance mode changes, where it can be read: the big
            // cores' governor and top frequency, the battery.
            auto firstLine = [](const char *path)
            {
                std::ifstream in(path);
                std::string line;
                std::getline(in, line);
                return line;
            };
            for (int cpu = 7; cpu >= 0; --cpu)
            {
                const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/";
                const std::string governor = firstLine((base + "scaling_governor").c_str());
                if (governor.empty())
                    continue;
                lines += "power=cpu" + std::to_string(cpu) + " " + governor + " max " +
                         firstLine((base + "scaling_max_freq").c_str()) + " kHz\n";
                break;
            }
            const std::string battery = firstLine("/sys/class/power_supply/battery/status");
            if (!battery.empty())
                lines += "battery=" + battery + " " + firstLine("/sys/class/power_supply/battery/capacity") + "%\n";
#elif defined(_WIN32)
            lines += "device=Windows PC\n";
#else
            lines += "device=Linux\n";
#endif
            lines += "cores=" + std::to_string(std::thread::hardware_concurrency()) + "\n";
            return lines;
        }

        struct Stream
        {
            uint64_t reads = 0;
            uint64_t lastRecordRead = 0;
            std::vector<uint8_t> last;
            // Replay: the recorded changes and the next one.
            struct Change
            {
                uint64_t read = 0;
                uint64_t vsync = 0;
                std::vector<uint8_t> value;
            };
            std::vector<Change> changes;
            size_t next = 0;
            std::vector<uint8_t> value;
            bool warned = false;
        };

        struct SettingChange
        {
            uint64_t vsync = 0;
            std::string key;
            int value = 0;
        };

        struct State
        {
            std::mutex mutex;
            Mode mode = Mode::Off;
            uint64_t generation = ~uint64_t{0};
            const std::atomic<uint64_t> *vsyncTick = nullptr;
            // Record
            std::FILE *file = nullptr;
            std::filesystem::path path;
            std::string pending;
            uint64_t lastFlushVsync = 0;
            uint64_t checksumEvery = kDefaultChecksumEvery;
            uint64_t bytesWritten = 0;
            // The ELF as loaded (noteElfImage): its hash and size.
            bool elfNoted = false;
            uint64_t elfHash = 0;
            size_t elfSize = 0;
            // Both
            std::unordered_map<uint64_t, Stream> streams;
            // Replay
            std::map<uint64_t, std::vector<uint32_t>> checksums;
            std::vector<SettingChange> settingChanges;
            size_t nextSetting = 0;
            uint64_t endVsync = 0;
            bool ended = false;
            uint64_t stopVsync = 0;
            std::atomic<bool> stop{false};
            uint64_t matched = 0;
            uint64_t differed = 0;
            uint64_t firstDiffer = 0;
            uint64_t inputMisplaced = 0;
            uint64_t firstMisplacedVsync = 0;
            uint64_t lastVsync = 0;
        };

        State &state()
        {
            static State s;
            return s;
        }

        std::atomic<Mode> g_mode{Mode::Off};
        thread_local bool t_gameThread = false;

        // While recording, SIGTERM and SIGINT (a tool's stop, Ctrl-C) end the
        // run as a window close does, so the file gets its end record.
        volatile std::sig_atomic_t g_terminate = 0;
        using SignalHandler = void (*)(int);
        SignalHandler g_previousTerm = SIG_DFL;
        SignalHandler g_previousInt = SIG_DFL;
        bool g_handlersInstalled = false;
        // A second signal while the first is still unanswered (the host loop
        // itself stuck) ends the process as the signal's own default would.
        void onTerminate(int signal)
        {
            if (g_terminate)
            {
                std::signal(signal, SIG_DFL);
                std::raise(signal);
                return;
            }
            g_terminate = 1;
        }

        uint64_t streamKey(Channel channel, uint32_t key)
        {
            return (static_cast<uint64_t>(channel) << 32) | key;
        }

        uint64_t nowVsync(const State &s)
        {
            return s.vsyncTick ? s.vsyncTick->load(std::memory_order_acquire) : 0u;
        }

        void flushLocked(State &s)
        {
            if (!s.file || s.pending.empty())
                return;
            std::fwrite(s.pending.data(), 1, s.pending.size(), s.file);
            std::fflush(s.file);
            s.bytesWritten += s.pending.size();
            s.pending.clear();
        }

        // The saves as they are at boot: every file under the folders, by
        // name under a label ("card1/...", "saves/...").
        void copySaves(State &s, const std::string &label, const std::filesystem::path &dir, std::string &header)
        {
            std::error_code ec;
            if (dir.empty() || !std::filesystem::exists(dir, ec))
                return;
            if (std::filesystem::is_regular_file(dir, ec))
            {
                std::string bytes;
                if (!readWhole(dir, bytes))
                    return;
                header += "file=" + label + " " + std::to_string(bytes.size()) + " " + hex64(fileHash(bytes)) + "\n";
                std::string payload;
                putString(payload, label);
                putString(payload, bytes);
                putRecord(s.pending, kFile, payload);
                return;
            }
            std::vector<std::filesystem::path> files;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec))
                    files.push_back(it->path());
            std::sort(files.begin(), files.end());
            for (const auto &file : files)
            {
                std::string bytes;
                if (!readWhole(file, bytes))
                    continue;
                const std::string name = label + "/" + std::filesystem::relative(file, dir, ec).generic_string();
                header += "file=" + name + " " + std::to_string(bytes.size()) + " " + hex64(fileHash(bytes)) + "\n";
                std::string payload;
                putString(payload, name);
                putString(payload, bytes);
                putRecord(s.pending, kFile, payload);
            }
        }

        std::string stamp()
        {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &tm);
            return text;
        }

        void beginRecording(State &s, const BootInfo &info, const std::string &folder)
        {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            const std::string game = info.game.empty() ? "game" : info.game;
            std::filesystem::path path = std::filesystem::path(folder) / (game + "-" + stamp() + ".zrec");
            for (int n = 2; std::filesystem::exists(path, ec); ++n)
                path = std::filesystem::path(folder) / (game + "-" + stamp() + "-" + std::to_string(n) + ".zrec");
            s.file = std::fopen(path.string().c_str(), "wb");
            if (!s.file)
            {
                std::fprintf(stderr, "[record] cannot write %s: not recording\n", path.string().c_str());
                return;
            }
            s.path = path;
            std::fwrite(kMagic, 1, sizeof(kMagic), s.file);
            s.bytesWritten = sizeof(kMagic);

            std::string header;
            header += "commit=" ZX_BUILD_COMMIT "\n";
            header += "game=" + info.game + "\n";
            header += std::string("front_end=") + (info.fromApp ? "app" : "runner") + "\n";
            header += "started=" + stamp() + "\n";
            header += "elf=" + std::filesystem::path(info.elfPath).filename().string() + "\n";
            {
                std::string elf;
                if (s.elfNoted)
                    header += "elf_hash=" + hex64(s.elfHash) + " " + std::to_string(s.elfSize) + "\n";
                else if (readWhole(info.elfPath, elf))
                    header += "elf_hash=" + hex64(fileHash(elf)) + " " + std::to_string(elf.size()) + "\n";
            }
            header += "disc_root=" + info.discRoot.string() + "\n";
            header += "boot=power-on\n"; // vsync 0 of this boot: the replay starts from the same point
            header += "checksum_every=" + std::to_string(s.checksumEvery) + "\n";
            header += "checksum_block=" + std::to_string(kChecksumBlock) + "\n";
            header += deviceLines();
            // Everything a boot reads: the BootConfig's names as this boot
            // reads them, and the process's switches.
            for (const std::string &name : zx::BootConfig::names())
                if (const char *value = zx::boot::text(name.c_str()); value && value[0])
                    header += "boot." + name + "=" + value + "\n";
            for (char **env = environ; env && *env; ++env)
            {
                const std::string entry = *env;
                const size_t eq = entry.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string name = entry.substr(0, eq);
                if ((name.rfind("ZX_", 0) == 0 || name.rfind("ZAXIS_", 0) == 0) && !zx::BootConfig::carries(name))
                    header += "env." + entry + "\n";
            }
            // The saves at boot, copied whole: the replay starts from them.
            std::string files;
            copySaves(s, "card1", info.memcardSlot1, files);
            copySaves(s, "card2", info.memcardSlot2, files);
            if (const char *saves = zx::boot::text("ZX_SAVES_DIR"); saves && saves[0])
                copySaves(s, "saves", saves, files);
            if (const char *gameplay = zx::boot::text("ZX_GAMEPLAY_INI"); gameplay && gameplay[0])
                copySaves(s, "gameplay.ini", gameplay, files);
            header += files;
            std::string record;
            putRecord(record, kHeader, header);
            s.pending.insert(0, record); // the header first, then the files
            flushLocked(s);
            std::fprintf(stderr, "[record] recording to %s\n", path.string().c_str());
        }

        // Each whole record of a recording's bytes, in order (a file cut
        // short by a crash ends at its last whole record). False when the
        // bytes are not a recording.
        bool walkRecords(const std::string &bytes, const std::function<void(uint8_t type, Cursor &record)> &each)
        {
            if (bytes.size() < sizeof(kMagic) || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
                return false;
            Cursor c{reinterpret_cast<const uint8_t *>(bytes.data()) + sizeof(kMagic),
                     reinterpret_cast<const uint8_t *>(bytes.data()) + bytes.size()};
            while (c.p < c.end)
            {
                const uint8_t type = *c.p++;
                const uint64_t size = c.varint();
                if (!c.ok || static_cast<uint64_t>(c.end - c.p) < size)
                    break; // cut short: a crash or a kill
                Cursor record{c.p, c.p + size};
                c.p += size;
                each(type, record);
            }
            return true;
        }

        bool loadReplay(State &s, const std::string &file)
        {
            std::string bytes;
            uint64_t inputs = 0, lastVsync = 0;
            auto each = [&](uint8_t type, Cursor &r)
            {
                switch (type)
                {
                case kInput:
                {
                    const Channel channel = static_cast<Channel>(r.varint());
                    const uint32_t key = static_cast<uint32_t>(r.varint());
                    const uint64_t readsSince = r.varint();
                    const uint64_t vsync = r.varint();
                    const size_t valueSize = static_cast<size_t>(r.varint());
                    const uint64_t mask = r.varint();
                    if (!r.ok)
                        break;
                    Stream &stream = s.streams[streamKey(channel, key)];
                    std::vector<uint8_t> value =
                        stream.changes.empty() ? std::vector<uint8_t>(valueSize, 0u) : stream.changes.back().value;
                    value.resize(valueSize, 0u);
                    for (size_t i = 0; i < valueSize && r.ok; ++i)
                    {
                        const bool whole = mask == 0u && valueSize > 63u;
                        if (whole || (i < 64 && (mask >> i) & 1u))
                        {
                            if (r.p >= r.end)
                                r.ok = false;
                            else
                                value[i] = *r.p++;
                        }
                    }
                    const uint64_t read = stream.changes.empty() ? readsSince : stream.changes.back().read + readsSince;
                    stream.changes.push_back({read, vsync, std::move(value)});
                    ++inputs;
                    lastVsync = std::max(lastVsync, vsync);
                    break;
                }
                case kChecksum:
                {
                    const uint64_t vsync = r.varint();
                    const uint64_t count = r.varint();
                    std::vector<uint32_t> sums;
                    for (uint64_t i = 0; i < count && r.ok; ++i)
                        sums.push_back(r.u32());
                    if (r.ok)
                        s.checksums[vsync] = std::move(sums);
                    lastVsync = std::max(lastVsync, vsync);
                    break;
                }
                case kEvent:
                {
                    const uint64_t vsync = r.varint();
                    const std::string kind = r.string();
                    const std::string text = r.string();
                    if (r.ok && kind == "setting")
                    {
                        // "key=value": a change from the app's menus.
                        const size_t eq = text.find('=');
                        if (eq != std::string::npos)
                            s.settingChanges.push_back({vsync, text.substr(0, eq), std::atoi(text.c_str() + eq + 1)});
                    }
                    break;
                }
                case kFrame:
                    lastVsync = std::max(lastVsync, r.varint());
                    break;
                case kEnd:
                    s.endVsync = r.varint();
                    s.ended = true;
                    break;
                default:
                    break;
                }
            };
            if (!readWhole(file, bytes) || !walkRecords(bytes, each))
            {
                std::fprintf(stderr, "[replay] %s is not a recording\n", file.c_str());
                return false;
            }
            if (!s.ended)
                s.endVsync = lastVsync;
            std::fprintf(stderr, "[replay] %s: %llu inputs, %zu checksums, %zu setting changes, to vsync %llu (%s)\n",
                         file.c_str(), static_cast<unsigned long long>(inputs), s.checksums.size(),
                         s.settingChanges.size(), static_cast<unsigned long long>(s.endVsync),
                         s.ended ? "ended" : "no end record: cut short");
            return true;
        }
    }

    Mode mode() { return g_mode.load(std::memory_order_relaxed); }

    const char *buildCommit() { return ZX_BUILD_COMMIT; }

    void noteElfImage(const uint8_t *bytes, size_t size)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.elfHash = fileHash(bytes, size);
        s.elfSize = size;
        s.elfNoted = true;
    }

    void beginBoot(const BootInfo &info)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.streams.clear();
        s.checksums.clear();
        s.settingChanges.clear();
        s.nextSetting = 0;
        s.pending.clear();
        s.ended = false;
        s.endVsync = 0;
        s.stop.store(false);
        s.matched = s.differed = s.firstDiffer = s.inputMisplaced = s.firstMisplacedVsync = s.lastVsync = 0;
        s.lastFlushVsync = 0;
        s.vsyncTick = info.vsyncTick;
        s.mode = Mode::Off;
        const char *replay = zx::boot::text("ZX_REPLAY");
        const char *record = zx::boot::text("ZX_RECORD");
        if (const char *every = zx::boot::text("ZX_RECORD_CHECKSUMS"); every && every[0])
            s.checksumEvery = std::max<uint64_t>(1u, std::strtoull(every, nullptr, 0));
        else
            s.checksumEvery = kDefaultChecksumEvery;
        if (replay && replay[0])
        {
            if (loadReplay(s, replay))
            {
                s.mode = Mode::Replay;
                const char *stopAt = zx::boot::text("ZX_REPLAY_STOP");
                s.stopVsync = stopAt && stopAt[0] ? std::strtoull(stopAt, nullptr, 0) : s.endVsync;
                // A recording cut short (a crash, a hang the app was killed
                // in) can be replayed past its last vsync, its inputs held as
                // they were last read: what came next is what it lost.
                if (s.stopVsync == 0u || (s.ended && s.stopVsync > s.endVsync))
                    s.stopVsync = s.endVsync;
                std::fprintf(stderr, "[replay] replaying to vsync %llu%s\n", static_cast<unsigned long long>(s.stopVsync),
                             s.stopVsync > s.endVsync ? " (past the recording's last: its inputs held)" : "");
            }
        }
        // A replay can record too: the second run's own file, to compare.
        if (record && record[0])
        {
            beginRecording(s, info, record);
            if (s.file && s.mode == Mode::Off)
                s.mode = Mode::Record;
        }
        g_mode.store(s.mode, std::memory_order_release);
        g_terminate = 0;
        if (s.file && !g_handlersInstalled)
        {
            g_previousTerm = std::signal(SIGTERM, onTerminate);
            g_previousInt = std::signal(SIGINT, onTerminate);
            g_handlersInstalled = true;
        }
    }

    void endBoot(const char *why)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const uint64_t vsync = nowVsync(s);
        if (s.mode == Mode::Replay)
        {
            std::fprintf(stderr,
                         "[replay] summary: %llu checksums matched, %llu differed%s%s, %llu inputs at another vsync "
                         "than recorded%s, stopped at vsync %llu of %llu\n",
                         static_cast<unsigned long long>(s.matched), static_cast<unsigned long long>(s.differed),
                         s.differed ? " (first at vsync " : "",
                         s.differed ? (std::to_string(s.firstDiffer) + ")").c_str() : "",
                         static_cast<unsigned long long>(s.inputMisplaced),
                         s.inputMisplaced ? (" (first at vsync " + std::to_string(s.firstMisplacedVsync) + ")").c_str() : "",
                         static_cast<unsigned long long>(vsync), static_cast<unsigned long long>(s.endVsync));
        }
        if (s.file)
        {
            std::string payload;
            putVarint(payload, vsync);
            putString(payload, why ? why : "");
            putRecord(s.pending, kEnd, payload);
            flushLocked(s);
            std::fclose(s.file);
            s.file = nullptr;
            std::fprintf(stderr, "[record] %s: %llu bytes, to vsync %llu\n", s.path.string().c_str(),
                         static_cast<unsigned long long>(s.bytesWritten), static_cast<unsigned long long>(vsync));
        }
        s.mode = Mode::Off;
        g_mode.store(Mode::Off, std::memory_order_release);
        if (g_handlersInstalled)
        {
            std::signal(SIGTERM, g_previousTerm == SIG_ERR ? SIG_DFL : g_previousTerm);
            std::signal(SIGINT, g_previousInt == SIG_ERR ? SIG_DFL : g_previousInt);
            g_handlersInstalled = false;
        }
    }

    void setGameThread() { t_gameThread = true; }
    bool onGameThread() { return t_gameThread; }

    namespace
    {
        // One read of a stream (the caller holds the lock): replaying, `value`
        // becomes the recorded value of this read, at its recorded size when
        // `resizable` (a text), else in the live value's size; recording, a
        // change is written. A value of another size than the last is written
        // whole: the reader starts from the last value cut or extended to the
        // new size, so a byte left out would keep the old value.
        void readLocked(State &s, Channel channel, uint32_t key, std::vector<uint8_t> &value, bool resizable)
        {
            const uint64_t vsync = nowVsync(s);
            Stream &stream = s.streams[streamKey(channel, key)];
            const uint64_t read = stream.reads++;
            if (s.mode == Mode::Replay)
            {
                if (stream.changes.empty())
                {
                    if (!stream.warned)
                        std::fprintf(stderr, "[replay] %s %u: read at vsync %llu, never read in the recording (live value)\n",
                                     channelName(channel), key, static_cast<unsigned long long>(vsync));
                    stream.warned = true;
                }
                while (stream.next < stream.changes.size() && stream.changes[stream.next].read <= read)
                {
                    const Stream::Change &change = stream.changes[stream.next];
                    if (change.read == read && change.vsync != vsync)
                    {
                        if (s.inputMisplaced++ == 0u)
                        {
                            s.firstMisplacedVsync = vsync;
                            std::fprintf(stderr, "[replay] %s %u read %llu at vsync %llu, recorded at vsync %llu: the run differs\n",
                                         channelName(channel), key, static_cast<unsigned long long>(read),
                                         static_cast<unsigned long long>(vsync), static_cast<unsigned long long>(change.vsync));
                        }
                    }
                    stream.value = change.value;
                    ++stream.next;
                }
                if (!stream.value.empty())
                {
                    if (resizable)
                        value = stream.value;
                    else
                        std::memcpy(value.data(), stream.value.data(), std::min(value.size(), stream.value.size()));
                }
            }
            if (s.file)
            {
                // Recording (also a replay's own recording): a record when the value changed.
                const size_t size = value.size();
                const bool first = stream.last.empty() && read == 0u;
                const bool resized = !stream.last.empty() && stream.last.size() != size;
                if (stream.last.size() != size)
                    stream.last.assign(size, 0u);
                uint64_t mask = 0;
                bool changed = first || resized;
                for (size_t i = 0; i < size; ++i)
                    if (resized || value[i] != stream.last[i])
                    {
                        changed = true;
                        if (i < 64)
                            mask |= uint64_t{1} << i;
                    }
                if (changed)
                {
                    const bool whole = size > 63u;
                    std::string payload;
                    putVarint(payload, static_cast<uint64_t>(channel));
                    putVarint(payload, key);
                    putVarint(payload, read - stream.lastRecordRead);
                    putVarint(payload, vsync);
                    putVarint(payload, size);
                    putVarint(payload, whole ? 0u : mask);
                    for (size_t i = 0; i < size; ++i)
                        if (whole || ((mask >> i) & 1u))
                            payload.push_back(static_cast<char>(value[i]));
                    putRecord(s.pending, kInput, payload);
                    std::memcpy(stream.last.data(), value.data(), size);
                    stream.lastRecordRead = read;
                }
            }
        }
    }

    void read(Channel channel, uint32_t key, void *bytes, size_t size)
    {
        if (!t_gameThread || mode() == Mode::Off || size == 0u)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        uint8_t *live = static_cast<uint8_t *>(bytes);
        std::vector<uint8_t> value(live, live + size);
        readLocked(s, channel, key, value, false);
        std::memcpy(live, value.data(), size);
    }

    void readText(Channel channel, uint32_t key, std::string &text)
    {
        if (!t_gameThread || mode() == Mode::Off)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        // A terminator after the text: an empty text is a value too.
        std::vector<uint8_t> value(text.begin(), text.end());
        value.push_back(0u);
        readLocked(s, channel, key, value, true);
        text.assign(value.begin(), value.empty() ? value.end() : value.end() - 1);
    }

    void onVsync(const uint8_t *rdram, uint64_t tick)
    {
        if (mode() == Mode::Off || !rdram)
            return;
        State &s = state();
        std::vector<SettingChange> apply; // outside the lock: a change is recorded too
        std::unique_lock<std::mutex> lock(s.mutex);
        s.lastVsync = tick;
        if (s.file && tick % s.checksumEvery == 0u)
        {
            const std::vector<uint32_t> sums = ramChecksums(rdram);
            std::string payload;
            putVarint(payload, tick);
            putVarint(payload, sums.size());
            for (uint32_t sum : sums)
                for (int b = 0; b < 4; ++b)
                    payload.push_back(static_cast<char>((sum >> (8 * b)) & 0xFFu));
            putRecord(s.pending, kChecksum, payload);
        }
        if (s.mode == Mode::Replay)
        {
            if (const auto it = s.checksums.find(tick); it != s.checksums.end())
            {
                const std::vector<uint32_t> sums = ramChecksums(rdram);
                std::string differing;
                for (size_t b = 0; b < sums.size() && b < it->second.size(); ++b)
                    if (sums[b] != it->second[b])
                        differing += (differing.empty() ? "" : ",") + std::to_string(b);
                if (differing.empty() && sums.size() == it->second.size())
                {
                    ++s.matched;
                }
                else
                {
                    if (s.differed++ == 0u)
                        s.firstDiffer = tick;
                    if (s.differed <= 20u)
                        std::fprintf(stderr, "[replay] state at vsync %llu DIFFERS in MB %s\n",
                                     static_cast<unsigned long long>(tick), differing.c_str());
                }
                if ((s.matched + s.differed) % 60u == 0u)
                    std::fprintf(stderr, "[replay] state at vsync %llu: %llu matched, %llu differed\n",
                                 static_cast<unsigned long long>(tick), static_cast<unsigned long long>(s.matched),
                                 static_cast<unsigned long long>(s.differed));
            }
            // Changes from the app's menus, at the vsync they came.
            while (s.nextSetting < s.settingChanges.size() && s.settingChanges[s.nextSetting].vsync <= tick)
            {
                const SettingChange &change = s.settingChanges[s.nextSetting++];
                std::fprintf(stderr, "[replay] setting %s=%d from the app's menu at vsync %llu (recorded at %llu): "
                                     "the state may differ from here\n",
                             change.key.c_str(), change.value, static_cast<unsigned long long>(tick),
                             static_cast<unsigned long long>(change.vsync));
                apply.push_back(change);
            }
            if (tick >= s.stopVsync && !s.stop.load())
            {
                std::fprintf(stderr, "[replay] reached vsync %llu: stopping\n", static_cast<unsigned long long>(tick));
                s.stop.store(true);
            }
        }
        if (s.file && tick - s.lastFlushVsync >= kFlushEvery)
        {
            flushLocked(s);
            s.lastFlushVsync = tick;
        }
        lock.unlock();
        for (const SettingChange &change : apply)
            zx::settings::set(change.key, change.value);
    }

    bool stopRequested()
    {
        return (mode() == Mode::Replay && state().stop.load(std::memory_order_relaxed)) ||
               (mode() != Mode::Off && g_terminate != 0);
    }

    void onHostFrame(const HostFrame &frame)
    {
        if (mode() == Mode::Off)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.file)
            return;
        auto us = [](double ms) { return static_cast<uint64_t>(std::max(0.0, ms * 1000.0 + 0.5)); };
        std::string payload;
        putVarint(payload, frame.vsync);
        putVarint(payload, us(frame.frameMs));
        putVarint(payload, us(frame.gameCpuMs));
        putVarint(payload, us(frame.vif1CpuMs));
        putVarint(payload, us(frame.gsCpuMs));
        putVarint(payload, us(frame.hostCpuMs));
        putVarint(payload, frame.flips);
        putVarint(payload, us(frame.pacerDroppedMs));
        putRecord(s.pending, kFrame, payload);
    }

    void onSettingChanged(const std::string &key, int value, bool fromGame)
    {
        if (mode() == Mode::Off)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.file)
            return;
        std::string payload;
        putVarint(payload, nowVsync(s));
        putString(payload, fromGame ? "setting-from-game" : "setting");
        putString(payload, key + "=" + std::to_string(value));
        putRecord(s.pending, kEvent, payload);
    }

    std::filesystem::path recordingPath()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.file ? s.path : std::filesystem::path();
    }

    ReplayResults replayResults()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        ReplayResults results;
        results.checksumsMatched = s.matched;
        results.checksumsDiffered = s.differed;
        results.firstDifferingVsync = s.firstDiffer;
        results.inputsMisplaced = s.inputMisplaced;
        results.endVsync = s.endVsync;
        results.recordingEnded = s.ended;
        results.vsync = s.lastVsync;
        return results;
    }

    void noteEvent(const std::string &kind, const std::string &text)
    {
        if (mode() == Mode::Off)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.file)
            return;
        std::string payload;
        putVarint(payload, nowVsync(s));
        putString(payload, kind);
        putString(payload, text);
        putRecord(s.pending, kEvent, payload);
    }

    void noteError(const std::string &text)
    {
        if (mode() == Mode::Off)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.file)
            return;
        std::string payload;
        putVarint(payload, nowVsync(s));
        putString(payload, "error");
        putString(payload, text);
        putRecord(s.pending, kEvent, payload);
        flushLocked(s); // (the run may not get much further)
    }

    std::filesystem::path folderFor(const std::filesystem::path &supportDir) { return supportDir / "Recordings"; }

    size_t countIn(const std::filesystem::path &folder)
    {
        size_t count = 0;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
            if (it->path().extension() == ".zrec")
                ++count;
        return count;
    }
    std::string Summary::value(const std::string &key) const
    {
        for (const auto &[name, text] : header)
            if (name == key)
                return text;
        return {};
    }

    bool summarize(const std::filesystem::path &file, Summary &out)
    {
        out = {};
        std::string bytes;
        uint64_t lastVsync = 0;
        auto each = [&](uint8_t type, Cursor &r)
        {
            switch (type)
            {
            case kHeader:
            {
                const std::string text(reinterpret_cast<const char *>(r.p), static_cast<size_t>(r.end - r.p));
                std::istringstream lines(text);
                for (std::string line; std::getline(lines, line);)
                    if (const size_t eq = line.find('='); eq != std::string::npos)
                        out.header.emplace_back(line.substr(0, eq), line.substr(eq + 1));
                break;
            }
            case kFile:
                if (std::string name = r.string(); r.ok)
                    out.files.push_back(std::move(name));
                break;
            case kInput:
            {
                r.varint(); // channel
                r.varint(); // key
                r.varint(); // reads since
                const uint64_t vsync = r.varint();
                if (r.ok)
                    lastVsync = std::max(lastVsync, vsync);
                break;
            }
            case kChecksum:
            case kFrame:
                if (const uint64_t vsync = r.varint(); r.ok)
                    lastVsync = std::max(lastVsync, vsync);
                break;
            case kEvent:
            {
                Summary::Event event;
                event.vsync = r.varint();
                event.kind = r.string();
                event.text = r.string();
                if (r.ok)
                    out.events.push_back(std::move(event));
                break;
            }
            case kEnd:
                out.endVsync = r.varint();
                out.ended = true;
                break;
            default:
                break;
            }
        };
        if (!readWhole(file, bytes) || !walkRecords(bytes, each))
            return false;
        if (!out.ended)
            out.endVsync = lastVsync;
        return true;
    }

    bool readFiles(const std::filesystem::path &file,
                   const std::function<void(const std::string &name, const std::string &bytes)> &each)
    {
        std::string bytes;
        auto record = [&](uint8_t type, Cursor &r)
        {
            if (type != kFile)
                return;
            const std::string name = r.string();
            const std::string contents = r.string();
            if (r.ok)
                each(name, contents);
        };
        return readWhole(file, bytes) && walkRecords(bytes, record);
    }
}
