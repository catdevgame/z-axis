#include "Common.h"
#include "MemoryCard.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/play_recording.h"
#include "runtime/ps2_host_write.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace ps2_stubs
{
    namespace
    {
        // PS2X_TRACE_MC=1: one stderr line per libmc call and its result.
        bool mcTraceEnabled()
        {
            static const bool enabled = []
            {
                const char *value = std::getenv("PS2X_TRACE_MC");
                return value && value[0] && value[0] != '0';
            }();
            return enabled;
        }

#define MC_TRACE(x)                                       \
    do                                                    \
    {                                                     \
        if (mcTraceEnabled())                             \
        {                                                 \
            std::ostringstream _mc_trace;                 \
            _mc_trace << "[MC] " << x << '\n';            \
            std::cerr << _mc_trace.str();                 \
        }                                                 \
    } while (0)

        constexpr int32_t kMcCmdGetInfo = 0x01;
        constexpr int32_t kMcCmdOpen = 0x02;
        constexpr int32_t kMcCmdClose = 0x03;
        constexpr int32_t kMcCmdSeek = 0x04;
        constexpr int32_t kMcCmdRead = 0x05;
        constexpr int32_t kMcCmdWrite = 0x06;
        constexpr int32_t kMcCmdFlush = 0x0A;
        constexpr int32_t kMcCmdMkdir = 0x0B;
        constexpr int32_t kMcCmdChdir = 0x0C;
        constexpr int32_t kMcCmdGetDir = 0x0D;
        constexpr int32_t kMcCmdSetFileInfo = 0x0E;
        constexpr int32_t kMcCmdDelete = 0x0F;
        constexpr int32_t kMcCmdFormat = 0x10;
        constexpr int32_t kMcCmdUnformat = 0x11;
        constexpr int32_t kMcCmdGetEntSpace = 0x12;
        constexpr int32_t kMcCmdRename = 0x13;

        constexpr int32_t kMcResultSucceed = 0;
        constexpr int32_t kMcResultChangedCard = -1;
        constexpr int32_t kMcResultNoFormat = -2;
        constexpr int32_t kMcResultNoEntry = -4;
        constexpr int32_t kMcResultDeniedPermit = -5;
        constexpr int32_t kMcResultNotEmpty = -6;
        constexpr int32_t kMcResultUpLimitHandle = -7;
        // sceMcGetInfo: -10 and below mean no card in the slot (the game
        // treats anything outside -2..0 as "no card").
        constexpr int32_t kMcResultNoCard = -10;
        // Returned by a libmc call itself (not through sceMcSync) when the
        // previous command has not been reported by sceMcSync yet.
        constexpr int32_t kMcErrBusy = -200;

        constexpr int32_t kMcTypePs2 = 2;
        constexpr int32_t kMcFormatted = 1;
        constexpr int32_t kMcUnformatted = 0;
        // Allocatable 1 KB clusters of a formatted 8 MB card (superblock
        // alloc_end of mcman/PCSX2 formatted cards). The root directory takes
        // one, so an empty card reports 8134 free.
        constexpr int64_t kMcAllocatableClusters = 8135;
        constexpr int64_t kMcClusterBytes = 1024;
        constexpr int64_t kMcDirEntryBytes = 512;
        constexpr size_t kMcMaxPathLen = 1024;
        constexpr size_t kMcMaxOpenFiles = 32;

        constexpr uint16_t kMcAttrReadable = 0x0001;
        constexpr uint16_t kMcAttrWriteable = 0x0002;
        constexpr uint16_t kMcAttrExecutable = 0x0004;
        constexpr uint16_t kMcAttr0400 = 0x0400;
        constexpr uint16_t kMcAttrFile = 0x0010;
        constexpr uint16_t kMcAttrSubdir = 0x0020;
        constexpr uint16_t kMcAttrClosed = 0x0080;
        constexpr uint16_t kMcAttrExists = 0x8000;

        struct SceMcStDateTime
        {
            uint8_t Resv2 = 0;
            uint8_t Sec = 0;
            uint8_t Min = 0;
            uint8_t Hour = 0;
            uint8_t Day = 0;
            uint8_t Month = 0;
            uint16_t Year = 0;
        };

        struct SceMcTblGetDir
        {
            SceMcStDateTime _Create{};
            SceMcStDateTime _Modify{};
            uint32_t FileSizeByte = 0;
            uint16_t AttrFile = 0;
            uint16_t Reserve1 = 0;
            uint32_t Reserve2 = 0;
            uint32_t PdaAplNo = 0;
            char EntryName[32]{};
        };

        static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

        struct McOpenFile
        {
            FILE *file = nullptr;
            int32_t port = 0;
            std::filesystem::path hostPath;
        };

        struct McPortState
        {
            std::string currentDir = "/";
            bool present = true;
            bool formatted = true;
            // mcman reports a card it has not seen since sceMcInit (or since it
            // was inserted/formatted) once as "changed" (-1), then 0.
            bool known = false;
            // sceMcGetDir with mode != 0 continues the previous search
            // (mcman keeps its position): its path and the next entry.
            std::string dirSearchPath;
            size_t dirSearchNext = 0u;
        };

        std::mutex g_mcStateMutex;
        int32_t g_mcNextFd = 1;
        int32_t g_mcLastCmd = 0;
        bool g_mcCommandPending = false;
        int32_t g_mcLastResult = 0;
        std::unordered_map<int32_t, McOpenFile> g_mcFiles;
        std::array<McPortState, 2> g_mcPorts{};
        // Card presence, format and "seen" state live in mcman on the IOP:
        // sceMcInit (libmc) does not reset them. Set up once per power-on.
        bool g_mcSlotsConfigured = false;
        int32_t g_cvMcFileCursor = 0;

        constexpr int32_t kCvMcSaveFileBytes = 0x838;
        constexpr int32_t kCvMcConfigFileBytes = 0x34;
        constexpr int32_t kCvMcIconInfoBytes = 0x3C4;
        constexpr int32_t kCvMcIconFileBytes = 0xB3F8;

        constexpr int32_t cvMcKilobytes(int32_t bytes)
        {
            return (bytes + 1023) / 1024;
        }

        constexpr int32_t kCvMcRequiredFreeKb =
            cvMcKilobytes(kCvMcSaveFileBytes) * 15 +
            cvMcKilobytes(kCvMcConfigFileBytes) +
            cvMcKilobytes(kCvMcIconInfoBytes) +
            cvMcKilobytes(kCvMcIconFileBytes) + 11;

        bool isValidMcPortSlot(int32_t port, int32_t slot)
        {
            return port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()) && slot == 0;
        }

        std::filesystem::path getMcRootPath(int32_t port)
        {
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            std::filesystem::path root = paths.mcRoot;
            if (root.empty())
            {
                if (!paths.elfDirectory.empty())
                {
                    root = paths.elfDirectory / "mc0";
                }
                else
                {
                    std::error_code ec;
                    const std::filesystem::path cwd = std::filesystem::current_path(ec);
                    root = ec ? std::filesystem::path("mc0") : (cwd / "mc0");
                }
            }

            root = root.lexically_normal();
            if (port <= 0)
            {
                return root;
            }

            const std::filesystem::path parent = root.parent_path();
            const std::string leaf = root.filename().string();
            const std::string lowerLeaf = toLowerAscii(leaf);
            if (lowerLeaf == "mc0")
            {
                return (parent / "mc1").lexically_normal();
            }
            if (lowerLeaf == "slot1")
            {
                return (parent / (leaf.substr(0, 4) + "2")).lexically_normal();
            }
            if (leaf.empty())
            {
                return (root / "mc1").lexically_normal();
            }

            return (parent / (leaf + "_slot" + std::to_string(port))).lexically_normal();
        }

        void ensureMcRootExists(int32_t port)
        {
            std::error_code ec;
            std::filesystem::create_directories(getMcRootPath(port), ec);
        }

        std::vector<std::string> splitMcPathComponents(const std::string &value)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : value)
            {
                if (c == '/' || c == '\\')
                {
                    if (!current.empty())
                    {
                        parts.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }

            if (!current.empty())
            {
                parts.push_back(current);
            }

            return parts;
        }

        std::string joinMcPathComponents(const std::vector<std::string> &parts)
        {
            if (parts.empty())
            {
                return "/";
            }

            std::string joined = "/";
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i != 0u)
                {
                    joined.push_back('/');
                }
                joined.append(parts[i]);
            }
            return joined;
        }

        std::string normalizeGuestMcPathLocked(int32_t port, std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            const std::string lower = toLowerAscii(path);
            if (lower.rfind("mc0:", 0) == 0 || lower.rfind("mc1:", 0) == 0)
            {
                path = path.substr(4);
            }

            const bool absolute = !path.empty() && path.front() == '/';
            std::vector<std::string> parts;
            if (!absolute && port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()))
            {
                parts = splitMcPathComponents(g_mcPorts[static_cast<size_t>(port)].currentDir);
            }

            for (const std::string &part : splitMcPathComponents(path))
            {
                if (part.empty() || part == ".")
                {
                    continue;
                }

                if (part == "..")
                {
                    if (!parts.empty())
                    {
                        parts.pop_back();
                    }
                    continue;
                }

                parts.push_back(part);
            }

            return joinMcPathComponents(parts);
        }

        std::filesystem::path guestMcPathToHostPath(int32_t port, const std::string &guestPath)
        {
            std::filesystem::path resolved = getMcRootPath(port);
            if (guestPath.size() > 1u)
            {
                resolved /= std::filesystem::path(guestPath.substr(1));
            }
            return resolved.lexically_normal();
        }

        bool localtimeSafeMc(const std::time_t *value, std::tm *out)
        {
#ifdef _WIN32
            return localtime_s(out, value) == 0;
#else
            return localtime_r(value, out) != nullptr;
#endif
        }

        std::time_t fileTimeToTimeTMc(std::filesystem::file_time_type value)
        {
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(systemTime);
        }

        void writeMcCString(uint8_t *rdram, uint32_t addr, const std::string &value)
        {
            if (addr == 0u)
            {
                return;
            }

            uint8_t *dst = getMemPtr(rdram, addr);
            if (!dst)
            {
                return;
            }

            std::memcpy(dst, value.c_str(), value.size() + 1u);
        }

        void writeMcDateTime(SceMcStDateTime &out, std::time_t value)
        {
            // PS2X_MC_FIXED_TIME=1 (test only): every date on the card is one date. A host file's
            // date is the time of the run, and a game keeps what it lists: two runs of an A/B differ.
            static ps2x::boot::PerBoot<bool> fixedTime([]() { return ps2x::boot::text("PS2X_MC_FIXED_TIME") != nullptr; });
            // A play recording keeps the dates the game was given (a replay's card files are new).
            value = play_recording::read(play_recording::Channel::Date, value);
            if (fixedTime.get())
                value = 1000000000;
            std::tm tm{};
            if (!localtimeSafeMc(&value, &tm))
            {
                std::memset(&out, 0, sizeof(out));
                return;
            }

            out.Resv2 = 0;
            out.Sec = static_cast<uint8_t>(tm.tm_sec);
            out.Min = static_cast<uint8_t>(tm.tm_min);
            out.Hour = static_cast<uint8_t>(tm.tm_hour);
            out.Day = static_cast<uint8_t>(tm.tm_mday);
            out.Month = static_cast<uint8_t>(tm.tm_mon + 1);
            out.Year = static_cast<uint16_t>(tm.tm_year + 1900);
        }

        void fillMcDirTableEntry(SceMcTblGetDir &entry,
                                 const std::string &name,
                                 bool isDirectory,
                                 uint32_t sizeBytes,
                                 std::time_t modifiedTime)
        {
            std::memset(&entry, 0, sizeof(entry));
            writeMcDateTime(entry._Create, modifiedTime);
            writeMcDateTime(entry._Modify, modifiedTime);
            entry.FileSizeByte = isDirectory ? 0u : sizeBytes;
            // As mcman stores them: 0x8427 for directories, 0x8497 for files.
            entry.AttrFile = static_cast<uint16_t>(kMcAttrReadable |
                                                   kMcAttrWriteable |
                                                   kMcAttrExecutable |
                                                   kMcAttr0400 |
                                                   (isDirectory ? kMcAttrSubdir : (kMcAttrFile | kMcAttrClosed)) |
                                                   kMcAttrExists);
            std::strncpy(entry.EntryName, name.c_str(), sizeof(entry.EntryName) - 1u);
            entry.EntryName[sizeof(entry.EntryName) - 1u] = '\0';
        }

        bool wildcardMatch(const std::string &pattern, const std::string &value)
        {
            size_t patternPos = 0u;
            size_t valuePos = 0u;
            size_t starPos = std::string::npos;
            size_t matchPos = 0u;

            while (valuePos < value.size())
            {
                if (patternPos < pattern.size() &&
                    (pattern[patternPos] == '?' || pattern[patternPos] == value[valuePos]))
                {
                    ++patternPos;
                    ++valuePos;
                }
                else if (patternPos < pattern.size() && pattern[patternPos] == '*')
                {
                    starPos = patternPos++;
                    matchPos = valuePos;
                }
                else if (starPos != std::string::npos)
                {
                    patternPos = starPos + 1u;
                    valuePos = ++matchPos;
                }
                else
                {
                    return false;
                }
            }

            while (patternPos < pattern.size() && pattern[patternPos] == '*')
            {
                ++patternPos;
            }

            return patternPos == pattern.size();
        }

        // PS2X_MC_FAIL (paths9, test only, off unless set): "CMD:N:CODE[,CMD:N:CODE...]" makes the Nth libmc command CMD of
        // the run (1-based, both ports; "N+" the Nth and every later one) report CODE at sceMcSync instead of its own
        // result, as a card does that fails or is changed or pulled in the middle of a save (CODE a libmc result: -1 a
        // new card, -2 unformatted, -3 full, -4 no entry, -5 denied, -10 no card). CMD: GetInfo Open Close Seek Read
        // Write Flush Mkdir Chdir GetDir SetFileInfo Delete Format Unformat GetEntSpace Rename. Reaches the save
        // dialogs' error paths (docs/DEVTOOLS.md).
        struct McFault
        {
            int32_t cmd;
            uint64_t from;
            bool later;
            int32_t code;
        };

        const std::vector<McFault> &mcFaults()
        {
            static const std::vector<McFault> faults = []
            {
                std::vector<McFault> out;
                const char *value = std::getenv("PS2X_MC_FAIL");
                if (!value || !value[0])
                {
                    return out;
                }
                static const struct
                {
                    const char *name;
                    int32_t cmd;
                } names[] = {{"GetInfo", kMcCmdGetInfo}, {"Open", kMcCmdOpen}, {"Close", kMcCmdClose}, {"Seek", kMcCmdSeek},
                             {"Read", kMcCmdRead}, {"Write", kMcCmdWrite}, {"Flush", kMcCmdFlush}, {"Mkdir", kMcCmdMkdir},
                             {"Chdir", kMcCmdChdir}, {"GetDir", kMcCmdGetDir}, {"SetFileInfo", kMcCmdSetFileInfo},
                             {"Delete", kMcCmdDelete}, {"Format", kMcCmdFormat}, {"Unformat", kMcCmdUnformat},
                             {"GetEntSpace", kMcCmdGetEntSpace}, {"Rename", kMcCmdRename}};
                std::string text(value);
                size_t pos = 0;
                while (pos < text.size())
                {
                    size_t end = text.find(',', pos);
                    if (end == std::string::npos)
                    {
                        end = text.size();
                    }
                    const std::string item = text.substr(pos, end - pos);
                    pos = end + 1;
                    const size_t a = item.find(':');
                    const size_t b = a == std::string::npos ? a : item.find(':', a + 1);
                    if (b == std::string::npos)
                    {
                        continue;
                    }
                    McFault fault{0, std::strtoull(item.c_str() + a + 1, nullptr, 10), false,
                                  static_cast<int32_t>(std::strtol(item.c_str() + b + 1, nullptr, 10))};
                    fault.later = item.find('+', a) != std::string::npos && item.find('+', a) < b;
                    const std::string name = item.substr(0, a);
                    for (const auto &entry : names)
                    {
                        if (name == entry.name)
                        {
                            fault.cmd = entry.cmd;
                        }
                    }
                    if (fault.cmd != 0)
                    {
                        out.push_back(fault);
                    }
                }
                return out;
            }();
            return faults;
        }

        void setMcCommandResultLocked(int32_t cmd, int32_t result)
        {
            if (!mcFaults().empty())
            {
                static uint64_t counts[32] = {};
                const uint64_t count = ++counts[static_cast<uint32_t>(cmd) & 31u];
                for (const McFault &fault : mcFaults())
                {
                    if (fault.cmd == cmd && (count == fault.from || (fault.later && count > fault.from)))
                    {
                        std::cerr << "[MC] PS2X_MC_FAIL: command " << cmd << " number " << count << " reports " << fault.code
                                  << " (it would have reported " << result << ")\n";
                        result = fault.code;
                    }
                }
            }
            g_mcLastCmd = cmd;
            g_mcLastResult = result;
            g_mcCommandPending = true;
        }

        // 0 if the port has a formatted card, else the libmc result for it.
        int32_t mcCardStatusLocked(const McPortState &state)
        {
            if (!state.present)
            {
                return kMcResultNoCard;
            }
            return state.formatted ? kMcResultSucceed : kMcResultNoFormat;
        }

        int64_t mcClustersFor(int64_t bytes)
        {
            return (bytes + kMcClusterBytes - 1) / kMcClusterBytes;
        }

        // Clusters a directory takes on a real card: its entry table ("." and
        // ".." plus one 512-byte entry per child), every file's data and every
        // subdirectory, recursively.
        int64_t mcUsedClusters(const std::filesystem::path &dir)
        {
            int64_t children = 0;
            int64_t used = 0;
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied, ec))
            {
                ++children;
                std::error_code entryEc;
                if (entry.is_directory(entryEc))
                {
                    used += mcUsedClusters(entry.path());
                }
                else
                {
                    const uintmax_t size = entry.file_size(entryEc);
                    used += entryEc ? 0 : mcClustersFor(static_cast<int64_t>(size));
                }
            }
            return used + mcClustersFor((children + 2) * kMcDirEntryBytes);
        }

        int32_t mcFreeClusters(int32_t port)
        {
            const int64_t used = mcUsedClusters(getMcRootPath(port));
            return static_cast<int32_t>(std::max<int64_t>(0, kMcAllocatableClusters - used));
        }

        // PS2X_MC_SLOT1 / PS2X_MC_SLOT2 (memory card slot 1 = port 0):
        // "none" leaves the slot empty, "unformatted" inserts an unformatted
        // card (until the game formats it). Default: a formatted card.
        void configureMcPortFromEnvironment(int32_t port, McPortState &state)
        {
            const char *value = std::getenv(port == 0 ? "PS2X_MC_SLOT1" : "PS2X_MC_SLOT2");
            const std::string mode = value ? toLowerAscii(value) : std::string{};
            state.present = mode != "none";
            state.formatted = mode != "unformatted";
            state.known = false;
        }

        // Sony libmc (e.g. sceMcFlush at 0x21ED30 in SLUS-20327): every
        // command first polls the libmc semaphore and returns -200 without
        // doing anything if it is taken; the semaphore is taken by the
        // command and only released by the sceMcSync call that reports its
        // completion. Games rely on this: Aggressive Inline issues
        // sceMcRead (or sceMcWrite) and sceMcFlush back to back and then
        // syncs once, expecting the read/write byte count, not the flush's 0.
        bool rejectMcCommandIfBusy(R5900Context *ctx, const char *name)
        {
            bool busy = false;
            {
                std::lock_guard<std::mutex> lock(g_mcStateMutex);
                busy = g_mcCommandPending;
            }
            if (busy)
            {
                MC_TRACE(name << " rejected: previous command not synced (" << kMcErrBusy << ")");
                setReturnS32(ctx, kMcErrBusy);
            }
            return busy;
        }

        void closeMcFilesLocked()
        {
            for (auto &[fd, openFile] : g_mcFiles)
            {
                if (openFile.file)
                {
                    std::fclose(openFile.file);
                    openFile.file = nullptr;
                }
            }
            g_mcFiles.clear();
        }

        void closeMcFilesForPortLocked(int32_t port)
        {
            for (auto it = g_mcFiles.begin(); it != g_mcFiles.end();)
            {
                if (it->second.port == port)
                {
                    if (it->second.file)
                    {
                        std::fclose(it->second.file);
                    }
                    it = g_mcFiles.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        int32_t allocateMcFdLocked(FILE *file, int32_t port, const std::filesystem::path &hostPath)
        {
            if (!file)
            {
                return kMcResultDeniedPermit;
            }
            if (g_mcFiles.size() >= kMcMaxOpenFiles)
            {
                return kMcResultUpLimitHandle;
            }

            // PS2X_MC_FD_BASE0=1 (paths8, test only): real libmc numbers a file's
            // descriptor from 0, the lowest free one; the game's mc00 device layer
            // (FUN_00121138: an open's result 0 is success, any other number an
            // error) relies on it, and this runtime starts at 1, so with the
            // default numbering no mc00 open can succeed. Off: unchanged.
            static ps2x::boot::PerBoot<bool> zeroBased([]()
            {
                const char *value = ps2x::boot::text("PS2X_MC_FD_BASE0");
                return value && value[0] && value[0] != '0';
            });
            if (zeroBased.get())
            {
                for (int32_t fd = 0; fd < 0x10000; ++fd)
                {
                    if (g_mcFiles.find(fd) != g_mcFiles.end())
                    {
                        continue;
                    }
                    g_mcFiles.emplace(fd, McOpenFile{file, port, hostPath});
                    return fd;
                }
                return kMcResultUpLimitHandle;
            }

            for (int attempt = 0; attempt < 0x10000; ++attempt)
            {
                if (g_mcNextFd <= 0)
                {
                    g_mcNextFd = 1;
                }

                const int32_t fd = g_mcNextFd++;
                if (g_mcFiles.find(fd) != g_mcFiles.end())
                {
                    continue;
                }

                g_mcFiles.emplace(fd, McOpenFile{file, port, hostPath});
                return fd;
            }

            return kMcResultUpLimitHandle;
        }

        FILE *openMcHostFile(const std::filesystem::path &hostPath, uint32_t flags)
        {
            const uint32_t access = flags & PS2_FIO_O_RDWR;
            const bool read = (access == PS2_FIO_O_RDONLY) || (access == PS2_FIO_O_RDWR);
            const bool write = (access == PS2_FIO_O_WRONLY) || (access == PS2_FIO_O_RDWR);
            const bool append = (flags & PS2_FIO_O_APPEND) != 0u;
            const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
            const bool truncate = (flags & PS2_FIO_O_TRUNC) != 0u;

            std::error_code ec;
            const bool exists = std::filesystem::exists(hostPath, ec) && !ec;

            const char *mode = "rb";
            if (read && write)
            {
                if (append)
                {
                    mode = exists ? "a+b" : "w+b";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "w+b";
                }
                else
                {
                    mode = "r+b";
                }
            }
            else if (write)
            {
                if (append)
                {
                    mode = exists ? "ab" : "wb";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "wb";
                }
                else
                {
                    mode = "r+b";
                }
            }

            return std::fopen(hostPath.string().c_str(), mode);
        }
    }

    void resetMemoryCardSlots()
    {
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        g_mcSlotsConfigured = false;
    }

    int32_t takeMemoryCardResult()
    {
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        g_mcCommandPending = false;
        return g_mcLastResult;
    }

    std::filesystem::path memoryCardFolder(int32_t port)
    {
        return getMcRootPath(port);
    }

    MemoryCardDebugSnapshot getMemoryCardDebugSnapshot()
    {
        MemoryCardDebugSnapshot snapshot{};
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        snapshot.nextFd = g_mcNextFd;
        snapshot.lastCmd = g_mcLastCmd;
        snapshot.lastResult = g_mcLastResult;
        snapshot.cvFileCursor = g_cvMcFileCursor;

        for (size_t i = 0; i < g_mcPorts.size(); ++i)
        {
            snapshot.ports[i].port = static_cast<int32_t>(i);
            snapshot.ports[i].currentDir = g_mcPorts[i].currentDir;
            snapshot.ports[i].formatted = g_mcPorts[i].formatted;
            snapshot.ports[i].rootPath = getMcRootPath(static_cast<int32_t>(i));
        }

        snapshot.openFiles.reserve(g_mcFiles.size());
        for (const auto &[fd, file] : g_mcFiles)
        {
            MemoryCardDebugOpenFile row{};
            row.fd = fd;
            row.port = file.port;
            row.hostPath = file.hostPath;
            snapshot.openFiles.push_back(std::move(row));
        }
        std::sort(snapshot.openFiles.begin(), snapshot.openFiles.end(), [](const MemoryCardDebugOpenFile &a, const MemoryCardDebugOpenFile &b)
                  { return a.fd < b.fd; });
        return snapshot;
    }

    void sceMcChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Chdir"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t pathAddr = getRegU32(ctx, 6);
        const uint32_t currentDirAddr = getRegU32(ctx, 7);
        const std::string requestedDir =
            (pathAddr != 0u) ? readPs2CStringBounded(rdram, pathAddr, kMcMaxPathLen) : std::string{};

        std::string currentDir = "/";
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                currentDir = state.currentDir;
                if (const int32_t status = mcCardStatusLocked(state); status != kMcResultSucceed)
                {
                    result = status;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string resolvedDir =
                        requestedDir.empty() ? state.currentDir : normalizeGuestMcPathLocked(port, requestedDir);
                    const std::filesystem::path hostDir = guestMcPathToHostPath(port, resolvedDir);
                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        state.currentDir = resolvedDir;
                        currentDir = resolvedDir;
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdChdir, result);
        }

        MC_TRACE("Chdir port=" << port << " '" << requestedDir
                                       << "' -> result=" << result << " cwd='" << currentDir << "'");
        writeMcCString(rdram, currentDirAddr, currentDir);
        setReturnS32(ctx, 0);
    }

    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Close"))
        {
            return;
        }
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end())
            {
                if (!it->second.file || std::fclose(it->second.file) == 0)
                {
                    result = kMcResultSucceed;
                }
                g_mcFiles.erase(it);
            }
            setMcCommandResultLocked(kMcCmdClose, result);
        }
        MC_TRACE("Close fd=" << fd << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Delete"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (const int32_t status = mcCardStatusLocked(state); status != kMcResultSucceed)
                {
                    result = status;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    if (guestPath != "/")
                    {
                        const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                        std::error_code ec;
                        if (std::filesystem::exists(hostPath, ec) && !ec)
                        {
                            if (std::filesystem::is_directory(hostPath, ec) &&
                                !std::filesystem::is_empty(hostPath, ec))
                            {
                                result = kMcResultNotEmpty;
                            }
                            else if (std::filesystem::remove(hostPath, ec) && !ec)
                            {
                                result = kMcResultSucceed;
                            }
                            else
                            {
                                result = kMcResultDeniedPermit;
                            }
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdDelete, result);
        }
        MC_TRACE("Delete port=" << port << " '" << path << "' -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
            }
        }

        // libmc teardown is just a local state reset in this immediate runtime model.
        setReturnS32(ctx, 0);
    }

    void sceMcFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Flush"))
        {
            return;
        }
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                result = (std::fflush(it->second.file) == 0) ? kMcResultSucceed : kMcResultDeniedPermit;
            }
            setMcCommandResultLocked(kMcCmdFlush, result);
        }
        MC_TRACE("Flush fd=" << fd << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcFormat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Format"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot) && !g_mcPorts[static_cast<size_t>(port)].present)
            {
                result = kMcResultNoCard;
            }
            else if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = true;
                    state.known = true;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdFormat, result);
        }
        MC_TRACE("Format port=" << port << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "GetDir"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        std::string rawPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const int32_t mode = static_cast<int32_t>(getRegU32(ctx, 7));
        const int32_t maxEntries = static_cast<int32_t>(getRegU32(ctx, 8));
        const uint32_t tableAddr = getRegU32(ctx, 9);

        std::vector<SceMcTblGetDir> entries;
        size_t firstEntry = 0u;
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (const int32_t status = mcCardStatusLocked(state); status != kMcResultSucceed)
                {
                    result = status;
                }
                else
                {
                    ensureMcRootExists(port);
                    // mode 0 starts a search; any other mode continues the
                    // last one (games list a directory one entry at a time).
                    if (mode != 0)
                    {
                        rawPath = state.dirSearchPath;
                        firstEntry = state.dirSearchNext;
                    }
                    const std::string guestQuery =
                        normalizeGuestMcPathLocked(port, rawPath.empty() ? "." : rawPath);
                    const bool hasWildcard =
                        guestQuery.find('*') != std::string::npos || guestQuery.find('?') != std::string::npos;

                    const std::filesystem::path queryRel =
                        (guestQuery.size() > 1u) ? std::filesystem::path(guestQuery.substr(1)) : std::filesystem::path{};

                    std::filesystem::path parentRel;
                    std::string pattern;
                    if (hasWildcard)
                    {
                        parentRel = queryRel.parent_path();
                        pattern = queryRel.filename().string();
                    }
                    else
                    {
                        const std::filesystem::path queryHostPath = guestMcPathToHostPath(port, guestQuery);
                        std::error_code queryEc;
                        if (std::filesystem::exists(queryHostPath, queryEc) && !queryEc &&
                            std::filesystem::is_directory(queryHostPath, queryEc))
                        {
                            parentRel = queryRel;
                            pattern = "*";
                        }
                        else
                        {
                            parentRel = queryRel.parent_path();
                            pattern = queryRel.filename().string();
                        }
                    }

                    if (pattern.empty())
                    {
                        pattern = "*";
                    }

                    std::filesystem::path hostDir = getMcRootPath(port);
                    if (!parentRel.empty())
                    {
                        hostDir /= parentRel;
                    }
                    hostDir = hostDir.lexically_normal();

                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        const std::time_t now = std::time(nullptr);
                        auto appendSpecial = [&](const std::string &name)
                        {
                            if (!wildcardMatch(pattern, name))
                            {
                                return;
                            }
                            SceMcTblGetDir entry{};
                            fillMcDirTableEntry(entry, name, true, 0u, now);
                            entries.push_back(entry);
                        };

                        appendSpecial(".");
                        appendSpecial("..");

                        std::vector<std::filesystem::directory_entry> dirEntries;
                        for (const auto &entry : std::filesystem::directory_iterator(
                                 hostDir, std::filesystem::directory_options::skip_permission_denied, ec))
                        {
                            if (ec)
                            {
                                break;
                            }
                            dirEntries.push_back(entry);
                        }

                        std::sort(dirEntries.begin(), dirEntries.end(),
                                  [](const std::filesystem::directory_entry &lhs,
                                     const std::filesystem::directory_entry &rhs)
                                  {
                                      return toLowerAscii(lhs.path().filename().string()) <
                                             toLowerAscii(rhs.path().filename().string());
                                  });

                        for (const auto &entry : dirEntries)
                        {
                            const std::string name = entry.path().filename().string();
                            if (!wildcardMatch(pattern, name))
                            {
                                continue;
                            }

                            std::error_code entryEc;
                            const bool isDirectory = entry.is_directory(entryEc) && !entryEc;
                            const uint32_t sizeBytes =
                                isDirectory ? 0u : static_cast<uint32_t>(entry.file_size(entryEc));
                            entryEc.clear();
                            const std::time_t modifiedTime = fileTimeToTimeTMc(entry.last_write_time(entryEc));
                            SceMcTblGetDir tableEntry{};
                            fillMcDirTableEntry(tableEntry,
                                                name,
                                                isDirectory,
                                                sizeBytes,
                                                entryEc ? now : modifiedTime);
                            entries.push_back(tableEntry);
                        }

                        const size_t available = entries.size() > firstEntry ? entries.size() - firstEntry : 0u;
                        const size_t entryCount =
                            std::min(available, maxEntries > 0 ? static_cast<size_t>(maxEntries) : 0u);
                        state.dirSearchPath = rawPath;
                        state.dirSearchNext = firstEntry + entryCount;
                        if (entryCount == 0u || tableAddr == 0u)
                        {
                            result = static_cast<int32_t>(entryCount);
                        }
                        else if (uint8_t *dst = getMemPtr(rdram, tableAddr))
                        {
                            std::memcpy(dst, entries.data() + firstEntry, entryCount * sizeof(SceMcTblGetDir));
                            result = static_cast<int32_t>(entryCount);
                        }
                        else
                        {
                            result = kMcResultDeniedPermit;
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdGetDir, result);
        }
        MC_TRACE("GetDir port=" << port << " '" << rawPath << "' mode=" << mode
                                        << " maxent=" << maxEntries << " -> result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetEntSpace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "GetEntSpace"))
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            setMcCommandResultLocked(kMcCmdGetEntSpace, 1024);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "GetInfo"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t typePtr = getRegU32(ctx, 6);
        const uint32_t freePtr = getRegU32(ctx, 7);
        const uint32_t formatPtr = getRegU32(ctx, 8);

        int32_t cardType = 0;
        int32_t freeBlocks = 0;
        int32_t format = kMcUnformatted;
        int32_t result = kMcResultNoEntry;

        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.present)
                {
                    result = kMcResultNoCard;
                }
                else
                {
                    cardType = kMcTypePs2;
                    freeBlocks = state.formatted ? mcFreeClusters(port) : 0;
                    format = state.formatted ? kMcFormatted : kMcUnformatted;
                    // A newly seen card is reported once (-1 formatted, -2
                    // unformatted), then as unchanged (0) with *format telling
                    // the two apart. Aggressive Inline's "card is unformatted"
                    // dialog stays up only while GetInfo keeps returning 0
                    // (poll callback 0x11D058); a repeated -2 reads as another
                    // card change and aborts it, looping with an empty box.
                    if (state.known)
                    {
                        result = kMcResultSucceed;
                    }
                    else
                    {
                        result = state.formatted ? kMcResultChangedCard : kMcResultNoFormat;
                    }
                    state.known = true;
                }
            }

            setMcCommandResultLocked(kMcCmdGetInfo, result);
        }

        if (typePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, typePtr))
            {
                std::memcpy(out, &cardType, sizeof(cardType));
            }
        }
        if (freePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, freePtr))
            {
                std::memcpy(out, &freeBlocks, sizeof(freeBlocks));
            }
        }
        if (formatPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, formatPtr))
            {
                std::memcpy(out, &format, sizeof(format));
            }
        }

        MC_TRACE("GetInfo port=" << port << " type=" << cardType
                                         << " free=" << freeBlocks << " format=" << format
                                         << " result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (size_t port = 0; port < g_mcPorts.size(); ++port)
            {
                McPortState &state = g_mcPorts[port];
                state.currentDir = "/";
                if (!g_mcSlotsConfigured)
                {
                    configureMcPortFromEnvironment(static_cast<int32_t>(port), state);
                }
                if (state.present)
                {
                    ensureMcRootExists(static_cast<int32_t>(port));
                }
            }
            g_mcSlotsConfigured = true;
        }
        setReturnS32(ctx, 0);
    }

    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Mkdir"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (const int32_t status = mcCardStatusLocked(state); status != kMcResultSucceed)
                {
                    result = status;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = std::filesystem::is_directory(hostPath, ec) ? kMcResultSucceed : kMcResultDeniedPermit;
                    }
                    else if (std::filesystem::create_directory(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                    else
                    {
                        result = std::filesystem::exists(hostPath.parent_path(), ec) && !ec
                                     ? kMcResultDeniedPermit
                                     : kMcResultNoEntry;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdMkdir, result);
        }
        MC_TRACE("Mkdir port=" << port << " '" << path << "' -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Open"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const uint32_t flags = getRegU32(ctx, 7);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (const int32_t status = mcCardStatusLocked(state); status != kMcResultSucceed)
                {
                    result = status;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
                    const bool exists = std::filesystem::exists(hostPath, ec) && !ec;
                    if (guestPath == "/")
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (exists && std::filesystem::is_directory(hostPath, ec))
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (!exists && !create)
                    {
                        result = kMcResultNoEntry;
                    }
                    else if (!std::filesystem::exists(hostPath.parent_path(), ec) || ec)
                    {
                        result = kMcResultNoEntry;
                    }
                    else
                    {
                        FILE *file = openMcHostFile(hostPath, flags);
                        if (!file)
                        {
                            result = exists ? kMcResultDeniedPermit : kMcResultNoEntry;
                        }
                        else
                        {
                            result = allocateMcFdLocked(file, port, hostPath);
                            if (result < 0)
                            {
                                std::fclose(file);
                            }
                        }
                    }
                }
            }
            setMcCommandResultLocked(kMcCmdOpen, result);
        }
        MC_TRACE("Open port=" << port << " '" << path << "' flags=0x" << std::hex << flags << std::dec << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Read"))
        {
            return;
        }
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t dstAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        uint8_t *dst = (size > 0) ? getMemPtr(rdram, dstAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!dst)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                // (straight into EE RAM: a recorder's read-only pages opened first, or the read system call fails)
                ps2BeforeHostWrite(dst, static_cast<size_t>(size));
                const size_t bytesRead = std::fread(dst, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesRead);
                if (std::ferror(it->second.file))
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdRead, result);
        }
        MC_TRACE("Read fd=" << fd << " dst=0x" << std::hex << dstAddr << std::dec << " size=" << size << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Rename"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string oldPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const std::string newPath = readPs2CStringBounded(rdram, getRegU32(ctx, 7), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (const int32_t status = mcCardStatusLocked(state); status != kMcResultSucceed)
                {
                    result = status;
                }
                else
                {
                    const std::filesystem::path oldHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, oldPath));
                    const std::filesystem::path newHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, newPath));
                    std::error_code ec;
                    if (std::filesystem::exists(oldHostPath, ec) && !ec &&
                        std::filesystem::exists(newHostPath.parent_path(), ec) && !ec)
                    {
                        std::filesystem::rename(oldHostPath, newHostPath, ec);
                        result = ec ? kMcResultDeniedPermit : kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdRename, result);
        }
        MC_TRACE("Rename port=" << port << " '" << oldPath << "' -> '" << newPath << "' -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Seek"))
        {
            return;
        }
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t offset = static_cast<int32_t>(getRegU32(ctx, 5));
        const int32_t origin = static_cast<int32_t>(getRegU32(ctx, 6));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                int whence = SEEK_SET;
                if (origin == PS2_FIO_SEEK_CUR)
                {
                    whence = SEEK_CUR;
                }
                else if (origin == PS2_FIO_SEEK_END)
                {
                    whence = SEEK_END;
                }

                if (std::fseek(it->second.file, offset, whence) == 0)
                {
                    const long position = std::ftell(it->second.file);
                    result = (position >= 0) ? static_cast<int32_t>(position) : kMcResultDeniedPermit;
                }
                else
                {
                    result = kMcResultDeniedPermit;
                }
            }

            setMcCommandResultLocked(kMcCmdSeek, result);
        }
        MC_TRACE("Seek fd=" << fd << " offset=" << offset << " origin=" << origin << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "SetFileInfo"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (const int32_t status = mcCardStatusLocked(state); status != kMcResultSucceed)
                {
                    result = status;
                }
                else
                {
                    const std::filesystem::path hostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, path));
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdSetFileInfo, result);
        }
        MC_TRACE("SetFileInfo port=" << port << " '" << path << "' -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cmdPtr = getRegU32(ctx, 5);
        const uint32_t resultPtr = getRegU32(ctx, 6);
        int32_t cmd = 0;
        int32_t result = 0;
        bool hadPending = false;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            hadPending = g_mcCommandPending;
            g_mcCommandPending = false;
            cmd = g_mcLastCmd;
            result = g_mcLastResult;
        }

        // libmc semantics: -1 means no async operation was executing; games rely
        // on it to tell idle polling apart from command completion.
        if (!hadPending)
        {
            setReturnS32(ctx, -1);
            return;
        }

        MC_TRACE("Sync cmd=" << cmd << " result=" << result);

        if (cmdPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, cmdPtr))
            {
                std::memcpy(out, &cmd, sizeof(cmd));
            }
        }
        if (resultPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, resultPtr))
            {
                std::memcpy(out, &result, sizeof(result));
            }
        }

        // 1 = command finished in this runtime's immediate model.
        setReturnS32(ctx, 1);
    }

    void sceMcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Unformat"))
        {
            return;
        }
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot) && !g_mcPorts[static_cast<size_t>(port)].present)
            {
                result = kMcResultNoCard;
            }
            else if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = false;
                    state.known = false;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdUnformat, result);
        }
        MC_TRACE("Unformat port=" << port << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rejectMcCommandIfBusy(ctx, "Write"))
        {
            return;
        }
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        const uint8_t *src = (size > 0) ? getConstMemPtr(rdram, srcAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!src)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesWritten = std::fwrite(src, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesWritten);
                if (!std::ferror(it->second.file))
                {
                    std::fflush(it->second.file);
                }
                else
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdWrite, result);
        }
        MC_TRACE("Write fd=" << fd << " src=0x" << std::hex << srcAddr << std::dec << " size=" << size << " -> " << result);
        setReturnS32(ctx, 0);
    }

    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcConfigFileBytes);
    }

    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cvMcFileCursor);
    }

    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcRequiredFreeKb);
    }

    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconInfoBytes);
    }

    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconFileBytes);
    }

    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcSaveFileBytes);
    }

    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t strAddr = getRegU32(ctx, 4);
        const std::string value = readPs2CStringBounded(rdram, runtime, strAddr, 1024);
        setReturnU32(ctx, strAddr + static_cast<uint32_t>(value.size()));
    }

    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t delta = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor += delta;
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 1);
    }

    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 0);
    }

    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }
}
