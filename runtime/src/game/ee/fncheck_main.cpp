// zx-fncheck: checks readable functions against the recompiled originals
// on recorded calls (the method is described in fncheck.h).
//
//   zx-fncheck [options] <recording.fnrec | folder>...
//     --only <name|0xaddress>   only these functions (repeatable; dm2:ADDRESS
//                               or dm2:name for another game's, ee/game_id.h)
//     --limit <n>               at most n calls per function
//     --show <n>                details of the first n differences (default 3)
//     --list                    the registered functions and their status
//     --cycles                  a readable version charging other EE cycles fails
//     --semantic                the semantic verdict, what merges are gated on (not
//                               with --cycles, --markers or --safepoints): a call
//                               passes on what it does: results, RAM, its calls
//                               and their arguments, args_left, entry_left, and the
//                               VU0 lanes later code reads (fncheck::g_vu0Lanes). Time,
//                               marks, interruption and dead stack do not count;
//                               the result line says mode=semantic
//     --no-callee-reads         no callee probes (probeCallee): the memory a callee
//                               reads at a call is not compared (callee_read)
//     --frame-reads-out <dir>   each original replay's leftover reads (the bytes of its
//                               own frame it reads before the call writes them, with
//                               their values) into the folder's index in dir (frameResidue)
//     --frame-reads-in <dir>    that index: residue a readable version leaves unlike the
//                               original that the folder's next leftover read of it reads
//                               fails (frame_residue_read)
//     --frame-reads-only        the originals' replays alone (--frame-reads-out implies it:
//                               the index's pass; there each call with leftover reads is
//                               replayed again with them changed, `acts` if what its caller
//                               sees changes, and a call handing its callee a pointer into
//                               its frame has the callee probed for the leftovers it reads)
//     --no-frame-residue        the frame at the return not compared (frame_residue_differs)
//     --markers                 a `[fncheck:markers]` line per file: the original's
//                               entry blocks and loop heads the replayed calls
//                               reach that the readable version never marks
//                               (ee::block, ee::loopBack; ee/markers.h)
//     --safepoints-paths <n>    with --safepoints: up to n calls past its first few, each
//                               the first to run its set of blocks in the file, get the
//                               pass too (generated recordings: rare paths come late)
//     --call-blocks             a `[fncheck:callblocks]` line per replayed call of
//                               the original: the blocks it ran (hex starts), for
//                               trimming recordings of calls that repeat a path
//     --call-timeout <s>        a replayed call (the original's or the readable
//                               version's) that has used s seconds of the process's
//                               CPU time (default 60; 0: none) ends the process as a
//                               fault does, `[fncheck:fault] ... phase="<phase>
//                               (timeout)"`, so a replay that never ends (paths3:
//                               dpdiv) is reported and the corpus run goes
//                               on. CPU time, not the clock (drafter24): a sleeping
//                               Mac (the lid closed) or a loaded one stops or slows
//                               the clock's worth of work, not a call's own
//     --max-mem-mb <mb>         the process's own memory (macOS phys_footprint,
//                               resident and compressed; Linux resident; Windows
//                               private bytes), polled every 20 ms: above mb
//                               (default 2048; 0: no cap) it ends as a fault,
//                               `phase="<phase> (memory: N MB)"`, whoever runs it
//                               (round drafter23: a replay that never ended grew
//                               to 113 GB and the Mac panicked)
//     --runaway <k>             a replayed call (either version) that charges more
//                               than k times the EE cycles the recording says the
//                               call charged in the game, plus 2^24, ends as a
//                               fault, `phase="<phase> (runaway: ...)"` (default
//                               64; 0: none): a loop that never ends is reported
//                               at once, not after the call timeout
//     --generate <fn> <n>       no recordings: n generated calls of a pure vector
//                               routine (kVectorRoutines below), the traced
//                               original against the readable version
//
// A recording names its game (fncheck::FileHeader::game, version 6 on; AI's
// before): it is replayed against that game's registration and traced
// original (DM2's built from its namespaced generated code, game.cmake).
//
// Each recording file gives one `[fncheck:result]` line for tools (the
// counts, the first failures, and the digests the result was made against:
// impl= the readable function's source, orig= the generated original,
// rec= the file's recorder version and corpus folder, checker= this
// executable) and, when the build has the original's block table, one
// `[fncheck:coverage]` line: the original's blocks and how many of the
// replayed calls ran each (path coverage; ee/fncheck_build.h). Notes on the
// result line (not failures): saves_differ / first_saves, the calls where the
// game's model gives a callee another value of a callee-saved register its
// prologue saves than the original held there (a missing or wrong
// ee::calleeSaves: the callee writes another value to its stack frame);
// held_differ / first_held, the same for the other callee-saved registers;
// vu0_left / first_vu0_left / vu0_lanes, the calls of a function not registered
// _VU0 that leave a VU0 vector register's lane or Q unlike the original, the
// first, and every lane seen (a stale lane is live: a later VMULx.xyz keeps
// it and an SQC2 stores it; fncheck_corpus.py vu0 lists them); vu0_live, those
// that differ in a lane something reads before setting it after the return
// (a failure under --semantic); vu0_call / first_vu0_call, the calls where a
// lane the callee reads differs at a call the function makes (a failure under
// --semantic too); callee_read / first_callee_read, the calls where a byte
// the original's callee read at a call (its traced original run from the
// original's state there, probeCallee) differs at the readable version's
// call (a failure), and callee_probes ... callee_buckets, what the probes
// saw (the calls looked at, how each ended, the bytes kept);
// callee_frame_read_calls / callee_frame_read_bytes, the calls whose
// recording (version 7 on) says what the callee read of the caller's frame
// and the bytes of it compared the same way (fncheck.h Call::reads);
// frame_residue_differs / residue_bytes / first_residue, the calls that leave
// the function's own frame at the return unlike the original (frameResidue; a
// note), frame_residue_read / first_residue_read, those a later function's
// leftover read finds (a failure; frame_residue_read_value, those where it reads
// a value, not zeros alone; frame_residue_masked / first_residue_masked, those
// read only by calls that do not act on it, a note; frame_index, the folder's index's bytes, -1
// without --frame-reads-in), leftover_reads, the bytes the originals read
// there unset (with --frame-reads-out; leftovers_acted / leftovers_masked, the
// calls that act on their own and those that do not); callee_sp_differs /
// first_callee_sp, the calls where a readable callee would get another sp than
// the original's call gave it (Script::compareCalleeSp; information).
//
// Exit status 0 when every replayed call of every readable function is
// identical to the recording, 1 otherwise.

#include "ee/fncheck.h"
#include "ee/fncheck_build.h"
#include "ee/fnrec_file.h"
#include "ee/fncheck_jumps.h"
#include "ee/fncheck_tables.h"
#include "ee/markers.h"
#include "ee/readable.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cinttypes>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "sym/ee/fncheck_main.cpp.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <execinfo.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif

// zx_runtime needs a function table; here it holds the replay stubs.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = PS2_RAM_SIZE;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = PS2_RAM_SIZE >> 2;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[PS2_RAM_SIZE >> 2] = {};

namespace
{
    using fncheck::Call;
    using fncheck::Line;
    using fncheck::Record;
    using game::readable::Function;

    // ---- EE RAM with only the recording's pages accessible ----------------

    // What the current replay is, for the fault report.
    const char *g_phase = "";
    const Function *g_function = nullptr;
    uint64_t g_sequence = 0;
    // The first difference the call being replayed has shown so far (a call
    // out of step: its answer's RAM is another call's), said with a fault.
    std::string g_firstProblem;
    // The recording being checked (the fault line names it for tools).
    std::string g_file;

    // The hash of a 16-byte line's address, for the maps a replay looks up on
    // every byte it reads or writes. std::hash<uint32_t> is the key itself in
    // libc++ and libstdc++, but FNV-1a in MSVC's library, which scatters
    // neighbouring lines over the table, a cache miss each (deskspeed: on the
    // desktop the asset loaders' big recordings took 5 to 6 times the Mac's
    // user time). Here neighbouring lines are in neighbouring buckets everywhere.
    struct LineHash
    {
        size_t operator()(uint32_t line) const noexcept { return line >> 4; }
    };

    // The replay memory: a guest map (runtime/guest_map.h, the checker's layout: the game's, with the scratchpad's
    // window at RAM's size too), so a recording's memory key (fncheck.h: RAM's offset, or the scratchpad's after
    // RAM) is an address from the base. A replay opens the key pages it needs there. The readable code reaches
    // guest memory at base + address (ee::at), which for RAM below its size is the key's own page; a page of any
    // other view (a mirror, the scratchpad's own addresses, an address past RAM a pointer wrapped to) is opened when
    // an access faults on it and its key's page is open (openOtherView), so the replay sees what the game would, and
    // closed again with the replay's pages.
    class GuestRam
    {
    public:
        bool allocate()
        {
#if defined(_WIN32)
            SYSTEM_INFO info;
            GetSystemInfo(&info);
            m_pageSize = info.dwPageSize;
#else
            m_pageSize = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
#endif
            m_open.assign(fncheck::kMemoryKeys / m_pageSize + 1u, 0u);
            return m_map.create(PS2_RAM_SIZE, guest_map::Layout::Checker, false);
        }

        uint8_t *base() const { return m_map.base(); }
        uint32_t pageSize() const { return m_pageSize; }
        // RAM, then the scratchpad (memory keys, fncheck.h).
        uint32_t pageOf(uint32_t key) const { return key / m_pageSize; }
        bool contains(const void *p) const { return m_map.contains(p); }

        void open(const std::set<uint32_t> &pages, uint8_t fill)
        {
            for (uint32_t page : pages)
            {
                protect(page, true);
                m_open[page] = 1u;
                std::memset(base() + static_cast<size_t>(page) * m_pageSize, fill, m_pageSize);
            }
        }
        void close(const std::set<uint32_t> &pages)
        {
            for (uint32_t page : pages)
            {
                protect(page, false);
                m_open[page] = 0u;
            }
            closeOtherViews();
        }

        // A fault at p (the fault handler's): in a view other than the keys' own, on a page whose key's page is
        // open, that page is opened too and the access goes on (true); else it is a fault (false).
        bool openOtherView(const void *p)
        {
            if (!m_map.contains(p))
                return false;
            const uint64_t at = static_cast<uint64_t>(static_cast<const uint8_t *>(p) - base());
            if (at < kKeysViews)
                return false;
            const uint32_t key = keyOf(at);
            if (key >= fncheck::kMemoryKeys || !m_open[key / m_pageSize])
                return false;
            m_map.protectAt(at & ~static_cast<uint64_t>(m_pageSize - 1u), m_pageSize, true);
            m_otherViewsOpen = true;
            return true;
        }

    private:
        // The keys' own views: RAM's first and the scratchpad's window at RAM's size.
        static constexpr uint64_t kKeysViews = static_cast<uint64_t>(PS2_RAM_SIZE) + guest_map::kScratchpadWindow;

        // The memory key a guest map address shows.
        static uint32_t keyOf(uint64_t at)
        {
            const uint32_t address = static_cast<uint32_t>(at); // (the guard past 4 GB: RAM's start)
            const uint32_t step = address & ~PS2_RAM_MASK, within = address & PS2_RAM_MASK;
            const bool window = step == guest_map::kScratchpadAddress || step == guest_map::kScratchpadAlias || step == PS2_RAM_SIZE;
            return window && within < guest_map::kScratchpadWindow ? fncheck::kScratchpadKey + within : within;
        }

        // A key page, in the keys' own view (the others follow on a fault).
        void protect(uint32_t page, bool access)
        {
            m_map.protectAt(static_cast<uint64_t>(page) * m_pageSize, m_pageSize, access);
        }
        void closeOtherViews()
        {
            if (!m_otherViewsOpen)
                return;
            for (const guest_map::View &view : m_map.views())
                if (view.address >= kKeysViews)
                    m_map.protectAt(view.address, view.bytes, false);
            m_otherViewsOpen = false;
        }

        guest_map::Map m_map;
        uint32_t m_pageSize = 4096;
        std::vector<uint8_t> m_open; // by key page: open
        bool m_otherViewsOpen = false;
    };
    GuestRam g_ram;
    // The pages the current replay has open (GuestRam::open), for reads that
    // must not fault (the stack-argument check).
    const std::set<uint32_t> *g_openPages = nullptr;
    // ... and the pages a callee probe opened besides (ProbeSandbox::open).
    const std::set<uint32_t> *g_probePages = nullptr;
    bool accessible(uint32_t address, uint32_t size)
    {
        for (uint32_t i = 0; i < size; ++i)
        {
            const uint32_t page = g_ram.pageOf(address + i);
            if ((!g_openPages || !g_openPages->count(page)) && (!g_probePages || !g_probePages->count(page)))
                return false;
        }
        return true;
    }

    void reportFault(const void *address)
    {
        const uint32_t ee = static_cast<uint32_t>(static_cast<const uint8_t *>(address) - g_ram.base());
        std::fprintf(stderr,
                     "\n[fncheck] %s, call #%" PRIu64 ", %s: touched EE 0x%08x, outside the memory the recording "
                     "covers\n",
                     g_function ? g_function->name : "?", g_sequence, g_phase, ee);
        if (!g_firstProblem.empty())
            std::fprintf(stderr, "[fncheck] before it: %s\n", g_firstProblem.c_str());
        std::fflush(stderr);
        std::printf("[fncheck:fault] address=0x%08x file=\"%s\" call=%" PRIu64 " phase=\"%s\" touched=0x%08x\n",
                    g_function ? g_function->address : 0u, g_file.c_str(), g_sequence, g_phase, ee);
        std::fflush(stdout);
    }

#if defined(_WIN32)
    LONG CALLBACK faultHandler(EXCEPTION_POINTERS *info)
    {
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            info->ExceptionRecord->NumberParameters >= 2)
        {
            const void *address = reinterpret_cast<const void *>(info->ExceptionRecord->ExceptionInformation[1]);
            if (g_ram.openOtherView(address))
                return EXCEPTION_CONTINUE_EXECUTION;
            if (g_ram.contains(address))
            {
                reportFault(address);
                ExitProcess(3);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void installFaultHandler() { AddVectoredExceptionHandler(1, faultHandler); }
#else
    // The replay with its leftovers changed (checkFile) may reach memory or
    // runtime code no recording covers (a pointer it read there): a fault or
    // an abort then ends that replay, not the process (POSIX; on Windows the
    // replay is not made and its reads count as acting).
    sigjmp_buf g_perturbJump;
    volatile sig_atomic_t g_perturbArmed = 0;
    void perturbAbort(int)
    {
        if (g_perturbArmed)
        {
            g_perturbArmed = 0;
            siglongjmp(g_perturbJump, 1);
        }
        std::signal(SIGABRT, SIG_DFL);
        std::raise(SIGABRT);
    }

    void faultHandler(int signal, siginfo_t *info, void *context)
    {
        if (g_ram.openOtherView(info->si_addr))
            return;
        if (g_perturbArmed)
        {
            g_perturbArmed = 0;
            siglongjmp(g_perturbJump, 1);
        }
#if defined(__APPLE__) && defined(__aarch64__)
        // (Where, for atos: the address without the image's slide.)
        std::fprintf(stderr, "[fncheck] fault: code 0x%llx, address %p\n",
                     static_cast<unsigned long long>(static_cast<ucontext_t *>(context)->uc_mcontext->__ss.__pc -
                                                     _dyld_get_image_vmaddr_slide(0)),
                     info->si_addr);
#else
        (void)context;
#endif
#if defined(__linux__)
        // ZX_FNCHECK_PROBES=1: where (a debugging aid; not signal-safe).
        if (const char *v = std::getenv("ZX_FNCHECK_PROBES"); v && *v == '1')
        {
            void *frames[32];
            const int n = backtrace(frames, 32);
            backtrace_symbols_fd(frames, n, 2);
        }
#endif
        if (g_ram.contains(info->si_addr))
        {
            reportFault(info->si_addr);
            _exit(3);
        }
        // Not guest memory: the version replaying crashed (a host index out of
        // RAM). A fault of this call too, so a tool can say which call it was.
        if (g_function && *g_phase)
        {
            std::printf("[fncheck:fault] address=0x%08x file=\"%s\" call=%" PRIu64 " phase=\"%s (host crash)\" touched=0x00000000\n",
                        g_function->address, g_file.c_str(), g_sequence, g_phase);
            std::fflush(stdout);
            _exit(3);
        }
        std::signal(signal, SIG_DFL);
        std::raise(signal);
    }
    void installFaultHandler()
    {
        struct sigaction action
        {
        };
        action.sa_sigaction = &faultHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGBUS, &action, nullptr);
    }
#endif

    // ---- the call timeout (--call-timeout), the memory cap (--max-mem-mb) -----

    std::atomic<int64_t> g_callDeadlineMs{0}; // process CPU time ms (cpuMs); 0: no call running
    int64_t g_callTimeoutMs = 60000;

    // --upper FILE (tools/diffcall.py upper): what later code reads past the
    // checks of a call's low words, by listing: the upper halves of a function's
    // argument registers it reads, of the registers callers read after it, and
    // the VU0 registers callers read after it. Differences go to the note
    // upper= / first_upper= (a generated call's evidence, not a recording's pass).
    struct UpperReads
    {
        uint32_t args = 0;    // argument registers (bit 0: a0)
        uint32_t returns = 0; // general registers
        uint32_t vu0 = 0;     // vf registers
    };
    std::unordered_map<uint32_t, UpperReads> g_upperReads;

    void loadUpperReads(const std::string &path)
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            unsigned address = 0, args = 0, returns = 0, vu0 = 0;
            if (std::sscanf(line.c_str(), "%x %x %x %x", &address, &args, &returns, &vu0) == 4)
                g_upperReads[address] = UpperReads{args, returns, vu0};
        }
    }
    uint64_t g_maxMemBytes = 2048ull << 20;
    bool g_callBlocks = false; // --call-blocks
    // Per file: every (callee, the original's call site) where a readable
    // version gave the callee another callee-saved register value than the
    // original held: the saved slots (ee::savedSlot) its prologue saves in
    // the low word, the others in the high word (drafter24: the result
    // line's saves_sites and held_sites, for the hands agents' lists).
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_saveSites;

    // The process's own memory in bytes: on macOS its phys_footprint (what
    // memguard.py and fncheck_corpus.py's cap read: resident and compressed),
    // on Linux its resident set, on Windows its private bytes; 0 if unknown.
    uint64_t ownMemory()
    {
#if defined(__APPLE__)
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return 0;
        return info.phys_footprint;
#elif defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc)))
            return 0;
        return pmc.PrivateUsage;
#else
        FILE *f = std::fopen("/proc/self/statm", "r");
        if (!f)
            return 0;
        unsigned long long size = 0, resident = 0;
        const int n = std::fscanf(f, "%llu %llu", &size, &resident);
        std::fclose(f);
        return n == 2 ? resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) : 0;
#endif
    }

    // Ends the process as a fault of the call being replayed, `why` added to
    // the phase (a timeout, the memory cap, a runaway): the corpus runner
    // records the file as a fault and goes on with the next one.
    [[noreturn]] void endAsFault(const char *why)
    {
        std::fprintf(stderr, "\n[fncheck] %s, call #%" PRIu64 ", %s: %s\n", g_function ? g_function->name : "?",
                     g_sequence, g_phase, why);
        std::fflush(stderr);
        std::printf("[fncheck:fault] address=0x%08x file=\"%s\" call=%" PRIu64 " phase=\"%s (%s)\" touched=0x00000000\n",
                    g_function ? g_function->address : 0u, g_file.c_str(), g_sequence, g_phase, why);
        std::fflush(stdout);
        std::_Exit(3);
    }
    // The process's CPU time in ms (user and system, every thread: the
    // watchdog's own is a few microseconds a second). The call deadline is on
    // it, not the clock: the batch-4 replay's five timeouts were the Mac's
    // clamshell sleep (15 minutes on the clock, chunks of 903 s), and a
    // loaded machine slows the clock's share of a call, not its CPU time.
    int64_t cpuMs()
    {
#if defined(_WIN32)
        FILETIME created, exited, kernel, user;
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return 0;
        const auto ticks = [](const FILETIME &f)
        { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        return (ticks(kernel) + ticks(user)) / 10000;
#else
        timespec ts{};
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
            return 0;
        return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
#endif
    }
    // One thread for both: every 20 ms the process's memory against
    // --max-mem-mb, every 250 ms the running call against --call-timeout.
    void startCallWatchdog()
    {
        if (g_callTimeoutMs <= 0 && g_maxMemBytes == 0)
            return;
        std::thread(
            []
            {
                for (uint32_t tick = 1;; ++tick)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if (g_maxMemBytes)
                    {
                        const uint64_t used = ownMemory();
                        if (used > g_maxMemBytes)
                        {
                            char b[96];
                            std::snprintf(b, sizeof(b), "memory: %llu MB, the cap %llu MB",
                                          static_cast<unsigned long long>(used >> 20),
                                          static_cast<unsigned long long>(g_maxMemBytes >> 20));
                            endAsFault(b);
                        }
                    }
                    if (g_callTimeoutMs <= 0 || tick % 12u)
                        continue;
                    const int64_t deadline = g_callDeadlineMs.load();
                    if (deadline == 0 || cpuMs() < deadline)
                        continue;
                    char b[64];
                    std::snprintf(b, sizeof(b), "timeout, still running after %lld s of CPU time",
                                  static_cast<long long>(g_callTimeoutMs / 1000));
                    endAsFault(b);
                }
            })
            .detach();
    }

    // --runaway: the EE cycles a replayed call may charge, from the recorded
    // call's (Record::cycles: its own code's 32 per branch back and 8 per
    // dispatch in the game): k times that, plus 2^24 (half a million branches
    // back) for slack and recordings without the count.
    uint64_t g_runawayFactor = 64;
    uint64_t g_cycleBudget = ~0ull;
    void setCycleBudget(const Record &r)
    {
        g_cycleBudget = g_runawayFactor ? r.cycles * g_runawayFactor + (1ull << 24) : ~0ull;
    }
    void checkBudget(uint64_t charged)
    {
        if (charged <= g_cycleBudget)
            return;
        char b[160];
        std::snprintf(b, sizeof(b), "runaway: %llu EE cycles charged, the recorded call's %llu (--runaway %llu)",
                      static_cast<unsigned long long>(charged),
                      static_cast<unsigned long long>((g_cycleBudget - (1ull << 24)) / (g_runawayFactor ? g_runawayFactor : 1)),
                      static_cast<unsigned long long>(g_runawayFactor));
        endAsFault(b);
    }
    struct CallDeadline
    {
        CallDeadline() { g_callDeadlineMs.store(g_callTimeoutMs > 0 ? cpuMs() + g_callTimeoutMs : 0); }
        ~CallDeadline() { g_callDeadlineMs.store(0); }
    };

    // ---- one replayed call -------------------------------------------------

    bool inDeadStack(const Record &r, uint32_t address)
    {
        const uint32_t sp = static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::sp));
        return address >= r.deadLow && address < sp;
    }

    void writeLine(uint8_t *ram, const Line &line)
    {
        for (uint32_t i = 0; i < 16u; ++i)
            if (line.mask & (1u << i))
                ram[line.address + i] = line.bytes[i];
    }

    uint32_t floatBits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    std::string hex(uint64_t v)
    {
        char b[24];
        std::snprintf(b, sizeof(b), "0x%" PRIx64, v);
        return b;
    }

    // Checks the original's reads against the recording.
    // --safepoints: each replayed call is run again, interrupted at its k-th
    // safe point (a branch back, a call's dispatch; k = 1 .. the points the
    // call passes), in both versions: the original unwinds there (its traced
    // code returns, as generated code does when a checkpoint is due) and goes
    // on through the scheduler's dispatches of the entry blocks (SliceEmu
    // below), the readable version goes on in the safe-point model of
    // ee/abi.h (ee/markers.h setSafePointHook). What each interruption adds
    // to the call's EE cycles must be the same in both.
    struct SafePointReplay
    {
        bool on = false;          // the readable hook is set
        uint32_t interruptAt = 0; // 0: none
        uint32_t count = 0;       // safe points passed in this replay
        bool interrupted = false, resumed = false;
        uint32_t kind = 0;        // the interrupted point's cycles (8: a call's dispatch, 32: a branch back)
        uint32_t maxPoints = 64, maxCalls = 3;
        uint32_t maxPathCalls = 0; // --safepoints-paths: calls past maxCalls that ran a set of blocks none before them did
    };
    SafePointReplay g_sp;
    bool safePointNext(uint32_t cycles)
    {
        ++g_sp.count;
        if (g_sp.interruptAt == 0 || g_sp.count != g_sp.interruptAt)
            return false;
        g_sp.interrupted = true;
        g_sp.kind = cycles;
        return true;
    }
    void resetSafePoints(uint32_t at)
    {
        g_sp.interruptAt = at;
        g_sp.count = 0;
        g_sp.interrupted = g_sp.resumed = false;
        g_sp.kind = 0;
    }

    // The original after an interruption: the scheduler dispatches the pc it
    // left at, through the function table (an entry block: slice by slice;
    // else the whole function). The traced function runs on from there (the
    // same code as the entry_ files: those are the function split at the
    // leaders); from its block markers this charges what the entry blocks
    // would: 8 for each slice the scheduler dispatches (one entered by a fall
    // through or a branch out of the slice: an entry block returns there), 32
    // only for a branch back to the slice's own start (an entry block's
    // other branches back return without a check); a slice left for an
    // address the table gives the whole function goes on as the whole.
    // A call of the function itself (the traced code runs it inline: a jump
    // to the start, no branch back's 32) from a slice is the slice's jal in
    // the game, dispatched (8) into the whole function, whose return comes
    // back to the slice in place (ee/abi.cpp enterSelfCall). From the whole
    // function, its return is the scheduler's dispatch of the return address
    // (not charged here, as the recording's own cycles leave it out; the
    // readable's leaveSelfCall the same): the entry block there, else the
    // whole function (drafter23).
    class SliceEmu final : public fncheck::BlockSink
    {
    public:
        void begin(const fncheck::BlockTable *table, const std::vector<uint32_t> &leaders, std::vector<uint32_t> slotEnds,
                   uint32_t end, uint32_t start, std::vector<uint32_t> tableJumps)
        {
            m_table = table;
            m_leaders = leaders;
            m_slotEnds = std::move(slotEnds);
            m_end = end;
            m_start = start;
            m_tableJumps = std::move(tableJumps);
            m_last = 0;
            m_active = m_blocks = m_pending = m_back = false;
            m_added = 0;
            m_selfCalls.clear();
        }
        void resume(uint32_t pc)
        {
            m_active = true;
            m_pending = false;
            m_back = true; // (the block resumed at is not a call of the function itself)
            m_last = 0;
            m_selfCalls.clear();
            m_blocks = isLeader(pc);
            if (m_blocks)
                enter(pc);
        }
        // An inner self-call returned to its return point pc.
        void selfReturn(uint32_t pc)
        {
            if (!m_active)
                return;
            const SelfCallFrom from = m_selfCalls.empty() ? SelfCallFrom{false, 0, 0} : m_selfCalls.back();
            if (!m_selfCalls.empty())
                m_selfCalls.pop_back();
            if (from.fromSlice)
            {
                // Back in the slice, in place.
                m_blocks = true;
                m_slice = from.slice;
                m_sliceEnd = from.sliceEnd;
                return;
            }
            m_blocks = isLeader(pc);
            if (m_blocks)
                enter(pc);
        }
        // Any branch back's 32 (in a slice or not): the next block entered
        // is not a call of the function itself.
        void sawBackBranch() { m_back = true; }
        // A branch back's 32 in a slice: held until the target is seen.
        bool holdBackBranch()
        {
            if (!m_active || !m_blocks)
                return false;
            m_pending = true;
            return true;
        }
        void hit(const fncheck::BlockTable &table, uint32_t block) override
        {
            if (!m_active || &table != m_table)
                return;
            const uint32_t start = table.starts[block];
            const bool back = m_back;
            m_back = false;
            // Left by a jump table's JR (fncheck_jumps.h): out of an entry
            // block, the slice's IndirectJump dispatch first (ee::tableJump).
            const bool viaTable = m_last && std::binary_search(m_tableJumps.begin(), m_tableJumps.end(), m_last);
            m_last = start;
            if (start == m_start && !back)
            {
                // A call of the function itself: from a slice, its jal's
                // dispatch into the whole function, back in the slice at the
                // return; from the whole function, inline.
                m_selfCalls.push_back(m_blocks ? SelfCallFrom{true, m_slice, m_sliceEnd} : SelfCallFrom{false, 0, 0});
                if (m_blocks)
                    m_added += 8;
                m_pending = false;
                m_blocks = false;
                return;
            }
            if (!m_blocks)
                return;
            if (m_pending)
            {
                m_pending = false;
                if (start == m_slice)
                {
                    m_added += 32; // in place
                    return;
                }
            }
            if (start == m_slice || (start > m_slice && start < m_sliceEnd))
                return;
            if (viaTable)
                m_added += 8; // dispatchGuestBranch's IndirectJump
            m_added += 8; // the scheduler's dispatch
            if (isLeader(start))
                enter(start);
            else
                m_blocks = false;
        }
        uint64_t takeAdded()
        {
            const uint64_t a = m_added;
            m_added = 0;
            return a;
        }
        void stop() { m_active = false; }

    private:
        bool isLeader(uint32_t a) const { return std::binary_search(m_leaders.begin(), m_leaders.end(), a); }
        void enter(uint32_t leader)
        {
            m_slice = leader;
            const auto it = std::upper_bound(m_leaders.begin(), m_leaders.end(), leader);
            m_sliceEnd = it == m_leaders.end() ? m_end : *it;
            // An entry block that ends on a delay slot which is an entry of its
            // own: the slot runs with it, and what follows is the scheduler's.
            const auto slot = std::upper_bound(m_slotEnds.begin(), m_slotEnds.end(), leader);
            if (slot != m_slotEnds.end() && *slot < m_sliceEnd)
                m_sliceEnd = *slot + 4u;
        }
        const fncheck::BlockTable *m_table = nullptr;
        std::vector<uint32_t> m_leaders;
        std::vector<uint32_t> m_slotEnds; // SafePointMark flags 4, sorted
        uint32_t m_end = 0, m_slice = 0, m_sliceEnd = 0, m_start = 0;
        struct SelfCallFrom
        {
            bool fromSlice;
            uint32_t slice, sliceEnd;
        };
        std::vector<SelfCallFrom> m_selfCalls; // the open self-calls, innermost last
        std::vector<uint32_t> m_tableJumps;    // blocks that end with a jump table's JR, sorted
        uint32_t m_last = 0;                   // the block entered last
        bool m_active = false, m_blocks = false, m_pending = false, m_back = false;
        uint64_t m_added = 0;
    };
    SliceEmu g_sliceEmu;

    class Checker final : public fncheck::Tracer
    {
    public:
        void reset(const Record &r)
        {
            m_record = &r;
            m_valid.clear();
            if (m_wrote.bucket_count() > 4096u)
                decltype(m_wrote)().swap(m_wrote);
            else
                m_wrote.clear();
            m_uncovered = 0;
            m_cycles = 0;
            for (const Line &l : r.inputs)
                m_valid[l.address] |= l.mask;
            m_leftovers.clear();
            m_calleeLeftovers.clear();
            if (m_leftSeen.bucket_count() > 4096u)
                decltype(m_leftSeen)().swap(m_leftSeen);
            else
                m_leftSeen.clear();
        }
        void markValid(const Line &l)
        {
            m_valid[l.address] |= l.mask;
            m_wrote[l.address] |= l.mask;
        }

        void read(uint32_t address, uint32_t size) override
        {
            address = fncheck::memoryKey(address); // the uncached mirrors are the same RAM
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint32_t a = address + i;
                if (inDeadStack(*m_record, a))
                {
                    if (m_keepLeftovers)
                        noteLeftover(a, m_leftovers);
                    continue;
                }
                const auto it = m_valid.find(a & ~15u);
                if (it == m_valid.end() || !(it->second & (1u << (a & 15u))))
                {
                    if (m_uncovered++ == 0)
                        m_firstUncovered = a;
                }
            }
        }
        void write(uint32_t address, uint32_t size) override
        {
            address = fncheck::memoryKey(address);
            for (uint32_t i = 0; i < size; ++i)
            {
                m_valid[(address + i) & ~15u] |= static_cast<uint16_t>(1u << ((address + i) & 15u));
                m_wrote[(address + i) & ~15u] |= static_cast<uint16_t>(1u << ((address + i) & 15u));
            }
        }
        void special(uint32_t) override {}
        void cycles(uint32_t count) override
        {
            if (count == 32u)
            {
                g_sliceEmu.sawBackBranch();
                if (g_sliceEmu.holdBackBranch())
                    return; // (SliceEmu charges it if the branch stays in its slice)
            }
            m_cycles += count;
            checkBudget(m_cycles);
        }
        bool safePointDue(uint32_t count) override { return g_sp.on && safePointNext(count); }
        void add(uint64_t count) { m_cycles += count; }
        uint64_t charged() const { return m_cycles + g_sliceEmu.takeAdded(); }
        void beforeCall(uint8_t *, R5900Context *, uint32_t) override {}
        void afterCall(uint8_t *, R5900Context *, bool) override {}

        uint64_t uncovered() const { return m_uncovered; }
        uint32_t firstUncovered() const { return m_firstUncovered; }
        // Written in this replay (by the original's code or a callee's
        // recorded effects).
        bool wroteKey(uint32_t key) const { return wroteMask(key & ~15u) & (1u << (key & 15u)); }
        // A line's bytes known (the recording's or written) and written, a bit each.
        uint16_t validMask(uint32_t line) const
        {
            const auto it = m_valid.find(line);
            return it == m_valid.end() ? 0u : it->second;
        }
        uint16_t wroteMask(uint32_t line) const
        {
            const auto it = m_wrote.find(line);
            return it == m_wrote.end() ? 0u : it->second;
        }
        // The bytes of its own (dead) stack the original read before this
        // call wrote them, in the order first read, with the value each held:
        // what an earlier function left there (frameResidue; kept while
        // keepLeftovers is on).
        void keepLeftovers(bool on) { m_keepLeftovers = on; }
        bool keepingLeftovers() const { return m_keepLeftovers; }
        // In the replayed call's own (dead) stack.
        bool inOwnFrame(uint32_t key) const { return m_record && key < fncheck::kScratchpadKey && inDeadStack(*m_record, key); }
        const std::vector<std::pair<uint32_t, uint8_t>> &leftovers() const { return m_leftovers; }
        // ... those a callee probe read there through a pointer the call
        // passed (probeCallee in the index's pass): the call's, as its own.
        // True if it is one (the probe goes on: the recording's byte).
        bool calleeLeftover(uint32_t key)
        {
            if (!m_keepLeftovers || !m_record || key >= fncheck::kScratchpadKey || !inDeadStack(*m_record, key) ||
                wroteKey(key) || !knownKey(key))
                return false;
            noteLeftover(key, m_calleeLeftovers);
            return true;
        }
        const std::vector<std::pair<uint32_t, uint8_t>> &calleeLeftovers() const { return m_calleeLeftovers; }
        // A byte the original wrote, or one the recording holds.
        bool known(uint32_t address) const
        {
            address = fncheck::memoryKey(address);
            const auto it = m_valid.find(address & ~15u);
            return it != m_valid.end() && (it->second & (1u << (address & 15u)));
        }
        // The same for a memory key (the scratchpad's included).
        bool knownKey(uint32_t key) const
        {
            const auto it = m_valid.find(key & ~15u);
            return it != m_valid.end() && (it->second & (1u << (key & 15u)));
        }
        // The original's value of it at this point of the call: known, and in
        // the function's own (dead) stack only if written in this call (by its
        // code or a callee's recorded effects): what the recording holds of
        // the stack below its sp at entry is what it read there unset, an
        // earlier call's leftovers, not its own.
        bool heldKey(uint32_t key) const
        {
            if (!knownKey(key))
                return false;
            if (!m_record || key >= fncheck::kScratchpadKey || !inDeadStack(*m_record, key))
                return true;
            return wroteKey(key);
        }
        // The lowest line of [low, high) the replay knows a byte of (high if none).
        uint32_t lowestKnownLine(uint32_t low, uint32_t high) const
        {
            uint32_t lowest = high;
            for (const auto &[line, mask] : m_valid)
                if (mask && line + 16u > low && line < high && line < lowest)
                    lowest = line;
            return lowest;
        }

    private:
        void noteLeftover(uint32_t key, std::vector<std::pair<uint32_t, uint8_t>> &into)
        {
            if (wroteKey(key))
                return;
            uint16_t &seen = m_leftSeen[key & ~15u];
            const uint16_t bit = static_cast<uint16_t>(1u << (key & 15u));
            if (seen & bit)
                return;
            seen = static_cast<uint16_t>(seen | bit);
            into.emplace_back(key, g_ram.base()[key]);
        }

        const Record *m_record = nullptr;
        std::unordered_map<uint32_t, uint16_t, LineHash> m_valid;
        std::unordered_map<uint32_t, uint16_t, LineHash> m_wrote; // written in this replay (heldKey)
        bool m_keepLeftovers = false;
        std::vector<std::pair<uint32_t, uint8_t>> m_leftovers, m_calleeLeftovers;
        std::unordered_map<uint32_t, uint16_t, LineHash> m_leftSeen;
        uint64_t m_uncovered = 0;
        uint32_t m_firstUncovered = 0;
        uint64_t m_cycles = 0;
    };
    Checker g_checker;

    // --markers' order check (gamehand4): per replayed call, the entry blocks
    // the original reaches in order (its block markers at leaders) and the
    // ee::block markers the readable version passes in order; the original's,
    // kept to the leaders the readable marks in that call, must be the same.
    std::unordered_set<uint32_t> g_leaderSet;
    std::vector<uint32_t> g_originalLeaders, g_readableMarks;
    // Bounded (round drafter23: a readable loop that never ended pushed marks
    // until the process held 113 GB): the original's at kMaxOrder (a call that
    // reaches more leaders is not order-checked), the readable's at one more
    // than the original's (more marks than that differ whatever they are).
    constexpr size_t kMaxOrder = size_t(1) << 22;
    bool g_orderOverflow = false;
    bool g_orderOn = false;

    // Path coverage: the blocks of the original (ee/fncheck_build.h) that the
    // replayed calls ran, as the number of calls that ran each. A call counts
    // when its replay gave the recording back.
    class Coverage final : public fncheck::BlockSink
    {
    public:
        void reset(const fncheck::BlockTable *table)
        {
            m_table = table;
            m_calls.assign(table ? table->count : 0u, 0u);
            m_inCall.assign(table ? table->count : 0u, 0u);
            m_touched.clear();
            m_counted = 0;
            m_backInCall.clear();
            m_backTaken.clear();
            m_last = kNone;
        }
        void beginCall()
        {
            for (uint32_t b : m_touched)
                m_inCall[b] = 0;
            m_touched.clear();
            m_backInCall.clear();
            m_last = kNone;
        }
        void endCall(bool count)
        {
            if (!count)
                return;
            ++m_counted;
            for (uint32_t b : m_touched)
                ++m_calls[b];
            m_backTaken.insert(m_backInCall.begin(), m_backInCall.end());
        }
        void hit(const fncheck::BlockTable &table, uint32_t block) override
        {
            if (&table != m_table || block >= m_inCall.size())
                return;
            // A block entered after one at or past its start: a branch back
            // to it was taken (a loop the call iterated).
            const uint32_t start = table.starts[block];
            if (g_orderOn && g_leaderSet.count(start))
            {
                if (g_originalLeaders.size() < kMaxOrder)
                    g_originalLeaders.push_back(start);
                else
                    g_orderOverflow = true;
            }
            if (m_last != kNone && start <= m_last)
                m_backInCall.insert(start);
            m_last = start;
            if (m_inCall[block])
                return;
            m_inCall[block] = 1;
            m_touched.push_back(block);
        }
        // Whether a counted call branched back to `address` (a block start).
        bool backTaken(uint32_t address) const { return m_backTaken.count(address) != 0; }
        const fncheck::BlockTable *table() const { return m_table; }
        // The blocks the last call ran (indices, in the order first reached).
        const std::vector<uint32_t> &lastCall() const { return m_touched; }
        uint64_t counted() const { return m_counted; }
        uint64_t calls(uint32_t block) const { return m_calls[block]; }

    private:
        const fncheck::BlockTable *m_table = nullptr;
        std::vector<uint64_t> m_calls;
        std::vector<uint8_t> m_inCall;
        std::vector<uint32_t> m_touched;
        uint64_t m_counted = 0;
        static constexpr uint32_t kNone = 0xFFFFFFFFu;
        uint32_t m_last = kNone;
        std::unordered_set<uint32_t> m_backInCall, m_backTaken;
    };
    Coverage g_coverage;

    // Callee stack arguments (fncheck_build.h): what the original left at
    // [sp + low, sp + high) when it made each call to such a callee (the bytes
    // it wrote or the recording holds), compared with what a readable version
    // leaves at the sp its callees get. A ninth argument and on: the
    // recording's calls keep only a0-a7 and f12-f19, and the stack below the
    // function's sp is dead after the return, so without this a readable
    // version that leaves out the store passes (applyShadowBuffer's calls of
    // screenTexturedRectangle and screenRectangle, round tested14).
    struct StackSnapshot
    {
        bool taken = false;
        uint32_t low = 0;
        std::vector<uint8_t> bytes, known;
    };
    std::vector<StackSnapshot> g_stackSnapshots; // by call index, the current record's
    // The live VU0 lanes the original's callee found (kLiveVu0Lanes), by call index, and COP0 status at the call.
    struct Vu0AtCall
    {
        bool taken = false;
        uint32_t lanes[32][4] = {};
        uint32_t cop0Status = 0;
    };
    std::vector<Vu0AtCall> g_vu0AtCalls;
    bool g_semantic = false; // --semantic
    game::Game g_game = game::Game::Ai;
    // What a function leaves in its own frame (frameResidue, before replay()).
    std::filesystem::path g_frameReadsOut, g_frameReadsIn; // --frame-reads-out DIR, --frame-reads-in DIR
    bool g_frameReadsOnly = false;                          // --frame-reads-only: the originals' replays alone
    bool g_frameResidueOn = true;                           // --no-frame-residue: off
    bool g_perturbing = false;                              // the original's replay with its leftovers changed (checkFile)

    const fncheck::StackArgs *stackArgsOf(uint32_t target)
    {
        static std::unordered_map<uint64_t, const fncheck::StackArgs *> map = []
        {
            std::unordered_map<uint64_t, const fncheck::StackArgs *> m;
            for (size_t i = 0; i < fncheck::g_stackArgCount; ++i)
                m[(uint64_t(fncheck::g_stackArgs[i].game) << 32) | fncheck::g_stackArgs[i].function] = &fncheck::g_stackArgs[i];
            return m;
        }();
        const auto it = map.find((uint64_t(static_cast<uint32_t>(g_game)) << 32) | (target & PS2_RAM_MASK));
        return it == map.end() ? nullptr : it->second;
    }

    // The registers some caller reads after calling fn (fncheck_tables.h
    // ResultReader): {gprs, fprs}, {0, 0} when none does.
    std::pair<uint32_t, uint32_t> resultReads(const Function &fn)
    {
        static std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> map = []
        {
            std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> m;
            for (size_t i = 0; i < fncheck::g_resultReaderCount; ++i)
                m[(uint64_t(fncheck::g_resultReaders[i].game) << 32) | fncheck::g_resultReaders[i].function] = {
                    fncheck::g_resultReaders[i].gprs, fncheck::g_resultReaders[i].fprs};
            return m;
        }();
        const auto it = map.find((uint64_t(static_cast<uint32_t>(fn.game)) << 32) | fn.address);
        return it == map.end() ? std::pair<uint32_t, uint32_t>{0u, 0u} : it->second;
    }

    // A function some caller reads f0 after (fncheck_tables.h).
    bool f0IsRead(const Function &fn) { return (resultReads(fn).second & 1u) != 0u; }

    // --markers: the markers the readable replays passed (ee/markers.h).
    bool g_markersMode = false;
    std::set<uint32_t> g_markedBlocks, g_markedLoops;
    void noteMarker(uint32_t address, ee::MarkerKind kind)
    {
        (kind == ee::MarkerKind::LoopBack ? g_markedLoops : g_markedBlocks).insert(address);
        if (g_orderOn && kind == ee::MarkerKind::Block)
            if (g_readableMarks.size() <= g_originalLeaders.size())
                g_readableMarks.push_back(address);
    }
    // The function's entry blocks (fncheck_tables.h SafePointMark): [first, last).
    std::pair<const fncheck::SafePointMark *, const fncheck::SafePointMark *> safePointMarks(uint8_t game, uint32_t fn)
    {
        const fncheck::SafePointMark *begin = fncheck::g_safePointMarks;
        const fncheck::SafePointMark *end = begin + fncheck::g_safePointMarkCount;
        auto less = [](const fncheck::SafePointMark &m, std::pair<uint8_t, uint32_t> k)
        { return std::make_pair(m.game, m.function) < k; };
        const fncheck::SafePointMark *first = std::lower_bound(begin, end, std::make_pair(game, fn), less);
        const fncheck::SafePointMark *last = first;
        while (last != end && last->game == game && last->function == fn)
            ++last;
        return {first, last};
    }

    // The VU0 lanes that matter at a function's entry and at its return, by
    // game (fncheck_tables.h Vu0Lanes, derived from each game's own code). Not
    // listed, nothing can read: what a function leaves there and the next one
    // sets first. The zero idiom VSUB x, x, x reads nothing (orthonormalize's
    // vf6.w), and neither does a QMFC2 of vf5's upper lanes that an MTC1 cuts.
    struct LiveLanes
    {
        uint64_t atEntry[2] = {}, atReturn[2] = {};
    };
    LiveLanes liveLanesOf(uint32_t function)
    {
        static std::unordered_map<uint64_t, LiveLanes> map = []
        {
            std::unordered_map<uint64_t, LiveLanes> m;
            for (size_t i = 0; i < fncheck::g_vu0LanesCount; ++i)
            {
                const fncheck::Vu0Lanes &row = fncheck::g_vu0Lanes[i];
                LiveLanes &lanes = m[(uint64_t(row.game) << 32) | row.function];
                std::memcpy(lanes.atEntry, row.atEntry, sizeof(row.atEntry));
                std::memcpy(lanes.atReturn, row.atReturn, sizeof(row.atReturn));
            }
            return m;
        }();
        const uint64_t game = uint64_t(static_cast<uint32_t>(g_game)) << 32;
        const auto own = map.find(game | (function & PS2_RAM_MASK));
        const auto indirect = map.find(game);
        LiveLanes lanes = own == map.end() ? LiveLanes{} : own->second;
        if (indirect != map.end())
            for (int i = 0; i < 2; ++i)
                lanes.atReturn[i] |= indirect->second.atReturn[i];
        return lanes;
    }
    bool laneIn(const uint64_t (&mask)[2], int reg, int lane)
    {
        return (mask[reg / 16] >> ((reg % 16) * 4 + lane)) & 1u;
    }

    // The argument registers a function reads before it sets them, its
    // callees' reads included (fncheck_tables.h; bits 0-7 a0-a3 and t0-t3,
    // 8-15 f12-f19), and apart those it only hands on unchanged to a callee
    // the scan cannot name (an indirect call or tail call, a kernel call:
    // an upper bound, a register such a callee may never read); 0 when the
    // scan did not list it.
    const char *const kArgName[] = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
    struct ArgUse
    {
        uint16_t reads = 0, handed = 0;
        bool known = false; // the scan has a row for the callee (one that reads no argument has none)
    };
    ArgUse argUseOf(uint32_t target)
    {
        static std::unordered_map<uint64_t, ArgUse> map = []
        {
            std::unordered_map<uint64_t, ArgUse> m;
            for (size_t i = 0; i < fncheck::g_argReaderCount; ++i)
                m[(uint64_t(fncheck::g_argReaders[i].game) << 32) | fncheck::g_argReaders[i].function] =
                    ArgUse{static_cast<uint16_t>(fncheck::g_argReaders[i].reads), static_cast<uint16_t>(fncheck::g_argReaders[i].handed), true};
            return m;
        }();
        const auto it = map.find((uint64_t(static_cast<uint32_t>(g_game)) << 32) | (target & PS2_RAM_MASK));
        return it == map.end() ? ArgUse{} : it->second;
    }

    // ---- what a callee reads (tools20) -------------------------------------
    //
    // A call is answered from the recording, so the memory the callee reads
    // at the call is otherwise never compared: a record the function builds
    // on its stack (dead after the return) or a field a later store or the
    // callee's own effects overwrite can be wrong in the readable version and
    // every replay still pass (bugs1: BMX XXX's findBodyFaceContacts handed
    // its contact callback 0 where the original hands the last face's
    // object). So, in the original's replay, each call's callee runs first as
    // its traced original, from the original's state at the call, in a
    // sandbox whose changes are undone after (probeCallee):
    //   - the callee's own recording of this very call, when the folder has
    //     one (a corpus route records many functions at once, nested calls
    //     included; CalleeRecordings): its inputs fill in the memory the
    //     replay does not hold, and its calls are answered from it, as the
    //     original's are, so the callee runs its whole path; each of its
    //     calls is probed in turn (below) before it is answered;
    //   - otherwise (and for those calls) a probe: the callee runs until its
    //     first byte the sandbox does not know, its first call, an I/O
    //     access or kCalleeProbeCycles.
    // Every byte read on the way that the replay knows of the original's
    // memory at the call (the recording's inputs, the original's writes,
    // earlier callees' effects) and that nothing in the sandbox wrote is kept
    // with its value: everything read before a stop is on the path the game
    // took, so the set is a lower bound of what the call tree read, never a
    // guess. At the readable version's call the same bytes must hold the
    // same values (Script::compareCalleeReads: callee_read, a failure). The
    // stack arguments window is stackArgsOf's check, and the stack below the
    // call's sp is the callee's own (a previous callee may have left anything
    // there in the game: not known).
    struct CalleeReads
    {
        bool taken = false;
        std::vector<uint32_t> keys;  // memory keys, ascending
        std::vector<uint8_t> values; // the original's, by key
        // What the call tree did on the way (ProbeTrace) and the registers at
        // the call, to run it again on the readable version's bytes
        // (calleeActsOn); kept when keys are.
        uint64_t trace = 0, events = 0;
        std::unique_ptr<R5900Context> atCall;
        std::vector<uint32_t> context; // the stack arguments it read (ProbeSandbox::context), ascending
        // The bytes of the caller's frame the callee read in the game, from
        // the recording (fncheck.h Call::reads, version 7 on), that the
        // original's replay holds at the call with that value: what the
        // original left there for it. Ascending; those the probe did not keep
        // are compared as they are (recordedReads).
        std::vector<uint32_t> recordedKeys;
        std::vector<uint8_t> recordedValues;
    };
    std::vector<CalleeReads> g_calleeReads; // by call index, the current record's
    bool g_calleeReadsOn = true;            // --no-callee-reads: off

    // How a call's callee was looked at (the result line's callee_ends).
    enum ProbeEnd : uint32_t
    {
        kProbeMatched,    // run on its own recording of this call to its return
        kProbeReturned,   // probed: the callee returned
        kProbeUnknown,    // probed: its first byte not known
        kProbeCall,       // probed: a call or jump it cannot follow (no traced original, too deep, the dispatcher's)
        kProbeIo,         // probed: an I/O register access
        kProbeCap,        // kCalleeProbeCycles, kCalleeProbeBytes kept, or the record's kCalleeRecordBytes
        kProbeNoOriginal, // no traced original of the callee in this build
        kProbeRuntime,    // probed: a function that runs the runtime's own code (untracedCode)
        kProbeEnds,
    };
    const char *const kProbeEndName[kProbeEnds] = {"matched", "returned", "unknown", "call", "io", "cap", "no_original",
                                                   "runtime"};

    // A function whose generated code goes into the runtime's own code (a
    // library function the runtime implements, a syscall), which touches
    // guest memory and the kernel's state past any tracer (fncheck_tables.h
    // UntracedCode): a probe never runs one.
    bool untracedCode(uint32_t function)
    {
        static const std::unordered_set<uint64_t> set = []
        {
            std::unordered_set<uint64_t> s;
            for (size_t i = 0; i < fncheck::g_untracedCodeCount; ++i)
                s.insert((uint64_t(fncheck::g_untracedCode[i].game) << 32) | fncheck::g_untracedCode[i].function);
            return s;
        }();
        return set.count((uint64_t(static_cast<uint32_t>(g_game)) << 32) | (function & PS2_RAM_MASK)) != 0;
    }
    constexpr uint64_t kCalleeProbeCycles = 1u << 20;
    constexpr size_t kCalleeProbeBytes = 1u << 16;
    // A record's probes keep at most this many bytes together (a function
    // that makes thousands of calls): past it a call is not probed (cap).
    constexpr size_t kCalleeRecordBytes = 1u << 23;
    size_t g_calleeRecordBytes = 0; // the current record's so far

    // Per file: calls looked at, how each ended, the bytes kept (log2
    // buckets: bucket 0 none, bucket b 2^(b-1) .. 2^b - 1).
    struct ProbeStats
    {
        uint64_t probes = 0, withBytes = 0, bytes = 0;
        // Calls whose callee has a recording in the folder; of those, the
        // ones where more than one record matched (probed instead), and the
        // matched runs that went another way than their record (a byte it
        // read the record does not hold, another call: kept up to there).
        uint64_t calleeRecorded = 0, ambiguous = 0, diverged = 0, inconsistent = 0;
        // Calls whose recording says what the callee read of the caller's
        // frame (Call::reads), and the bytes of it the original's replay
        // held as recorded (compared at the readable version's call).
        uint64_t frameReadCalls = 0, frameReadBytes = 0;
        uint64_t cycles = 0;     // EE cycles the probes and matched runs charged
        double seconds = 0;      // the time they took (the clock's)
        double indexSeconds = 0; // ... of it, indexing callee recordings (CalleeRecordings)
        uint64_t ends[kProbeEnds] = {};
        uint64_t buckets[18] = {};
    };
    ProbeStats g_probeStats;

    struct ProbeStop
    {
        ProbeEnd end;
    };

    // A record that stops at a call (fncheck::kStopsAtCall): the replay has
    // made that call, checked as any other, and goes no further (thrown from
    // the call's answer to replay()).
    struct StopReached
    {
    };

    // What a call tree does in the sandbox, as one hash: every access (its
    // kind, key and size), the bytes each write left, each call's target, and
    // the result registers of a callee that returned. Run again with the
    // readable version's bytes (calleeActsOn), the same hash over as many
    // events says those bytes changed nothing the callee did (a partial
    // store's read of the word it then rewrites, the bytes of a load that an
    // unaligned load masks off). `limit`: a run again stops after the first
    // run's count.
    struct ProbeTrace
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        uint64_t events = 0, limit = ~0ull;
        bool pending = false; // a write whose bytes are not mixed in yet
        uint32_t pendingKey = 0, pendingSize = 0;
    };
    ProbeTrace g_trace;
    bool accessible(uint32_t address, uint32_t size);
    void traceMix(uint64_t value)
    {
        g_trace.hash ^= value;
        g_trace.hash *= 0x100000001B3ull;
    }
    void traceFlushWrite()
    {
        if (!g_trace.pending)
            return;
        g_trace.pending = false;
        for (uint32_t i = 0; i < g_trace.pendingSize; ++i)
            if (accessible(g_trace.pendingKey + i, 1u))
                traceMix(g_ram.base()[g_trace.pendingKey + i]);
    }
    void traceEvent(uint64_t kind, uint64_t key, uint64_t size)
    {
        traceFlushWrite();
        if (++g_trace.events > g_trace.limit)
            throw ProbeStop{kProbeCap};
        traceMix((kind << 56) ^ (key << 8) ^ size);
    }

    class ProbeIo final : public fncheck::IoPort
    {
    public:
        void load(uint32_t, uint32_t, uint64_t &, uint64_t &) override { throw ProbeStop{kProbeIo}; }
        void store(uint32_t, uint32_t, uint64_t, uint64_t) override { throw ProbeStop{kProbeIo}; }
    };

    // The memory a call's probes run in: the replay's RAM with every change
    // logged (undone at the end, newest first), the bytes made known beyond
    // the replay's (the callee's recorded inputs, what the sandbox wrote),
    // pages opened for them, and the bytes kept.
    class ProbeSandbox
    {
    public:
        void begin(uint32_t sp, const fncheck::StackArgs *args)
        {
            m_sp = fncheck::memoryKey(sp);
            m_argsLow = args ? fncheck::memoryKey(sp + args->low) : 0u;
            m_argsHigh = args ? m_argsLow + (args->high - args->low) : 0u;
            m_undo.clear();
            emptied(m_changed);
            emptied(m_known);
            m_reads.clear();
            m_context.clear();
            m_opened.clear();
            g_probePages = &m_opened;
        }
        // RAM, the pages and the open set as they were.
        void end()
        {
            for (auto it = m_undo.rbegin(); it != m_undo.rend(); ++it)
                g_ram.base()[it->first] = it->second;
            m_undo.clear();
            g_ram.close(m_opened);
            m_opened.clear();
            g_probePages = nullptr;
        }
        // Pages a recorded callee touched that the replay does not hold.
        void open(const std::set<uint32_t> &pages)
        {
            std::set<uint32_t> added;
            for (uint32_t p : pages)
                if ((!g_openPages || !g_openPages->count(p)) && m_opened.insert(p).second)
                    added.insert(p);
            g_ram.open(added, 0u);
        }
        // The callee's own frame: below the call's sp.
        bool ownFrame(uint32_t k) const { return k < m_sp && k + fncheck::kStackWindow >= m_sp; }
        // The original's at the call: what the replay holds there; run again
        // (calleeActsOn), the bytes the first run kept.
        bool outer(uint32_t k) const
        {
            if (m_overlay)
                return std::binary_search(m_overlay->keys.begin(), m_overlay->keys.end(), k) ||
                       std::binary_search(m_overlay->context.begin(), m_overlay->context.end(), k);
            return !ownFrame(k) && g_checker.heldKey(k);
        }
        void overlay(const CalleeReads *reads) { m_overlay = reads; }
        bool rerun() const { return m_overlay != nullptr; }
        bool knows(uint32_t k) const { return accessible(k, 1u) && (m_known.count(k) || outer(k)); }
        bool changed(uint32_t k) const { return m_changed.count(k) != 0; }
        // A byte the call tree read: kept if it is the original's at the call.
        // False when the cap is reached.
        bool noteRead(uint32_t k)
        {
            if (changed(k) || !outer(k))
                return true;
            if (m_reads.size() + m_context.size() >= kCalleeProbeBytes)
                return false;
            // (The stack arguments: known to a run again, compared by stackArgsOf's check.)
            if (k >= m_argsLow && k < m_argsHigh)
                m_context.insert(k);
            else
                m_reads.emplace(k, g_ram.base()[k]);
            return true;
        }
        size_t mark() const { return m_undo.size(); }
        // An unordered set's clear() walks all its buckets, which never
        // shrink: after one big matched run every later call paid for them
        // (15 ms a probe). A big one is let go instead.
        static void emptied(std::unordered_set<uint32_t> &set)
        {
            if (set.bucket_count() > 1024u)
                std::unordered_set<uint32_t>().swap(set);
            else
                set.clear();
        }
        // A write about to happen (logged with the byte's value now); `keep`:
        // it stays known (a matched run's own, an answered call's), else it
        // is a probe's, undone at its end (revert).
        void noteWrite(uint32_t k, bool keep)
        {
            m_undo.emplace_back(k, g_ram.base()[k]);
            if (keep)
            {
                m_changed.insert(k);
                m_known.insert(k);
            }
        }
        void set(uint32_t k, uint8_t value)
        {
            noteWrite(k, true);
            g_ram.base()[k] = value;
        }
        // The recorded callee's input, where the replay holds nothing.
        void fill(uint32_t k, uint8_t value)
        {
            m_undo.emplace_back(k, g_ram.base()[k]);
            m_known.insert(k);
            g_ram.base()[k] = value;
        }
        void revert(size_t mark)
        {
            while (m_undo.size() > mark)
            {
                g_ram.base()[m_undo.back().first] = m_undo.back().second;
                m_undo.pop_back();
            }
        }
        const std::map<uint32_t, uint8_t> &reads() const { return m_reads; }
        const std::set<uint32_t> &context() const { return m_context; }
        void clear()
        {
            m_reads.clear();
            m_context.clear();
        }

    private:
        uint32_t m_sp = 0, m_argsLow = 0, m_argsHigh = 0; // memory keys
        std::vector<std::pair<uint32_t, uint8_t>> m_undo;
        std::unordered_set<uint32_t> m_changed, m_known;
        std::map<uint32_t, uint8_t> m_reads;
        std::set<uint32_t> m_context; // the replay's bytes it read that are not compared (stack arguments)
        std::set<uint32_t> m_opened; // pages opened for it (g_probePages)
        const CalleeReads *m_overlay = nullptr;
    };
    ProbeSandbox g_sandbox;
    PS2Runtime *g_replayRuntime = nullptr; // the replay's (calleeActsOn, from a readable version's call)
    // Each game's VU0 memory as its own upload left it (fncheck.h Vu0Upload; a game with none, zeroed), put back
    // at each replay's start; none at all when the build has no game's upload (VU0 has no memory then, and a
    // VCALLMS runs nothing).
    std::array<fncheck::Vu0Memory, game::kGameCount> g_vu0Memory;
    void uploadVu0Memory(PS2Runtime &runtime)
    {
        const std::span<const fncheck::Vu0Upload> uploads = fncheck::vu0Uploads();
        if (uploads.empty())
            return;
        for (uint32_t g = 0; g < game::kGameCount; ++g)
        {
            const auto own = std::find_if(uploads.begin(), uploads.end(),
                                          [g](const fncheck::Vu0Upload &u) { return static_cast<uint32_t>(u.game) == g; });
            if (own != uploads.end())
                g_vu0Memory[g].upload(runtime.memory(), own->stream, own->bytes);
            else
                g_vu0Memory[g].upload(runtime.memory(), nullptr, 0);
        }
    }
    bool g_probing = false; // a callee probe or matched run is running (probeCallee)
    size_t scriptNextIndex(); // the replay's next recorded call (Script, below)

    // The tracer of a callee run in the sandbox. A probe (`matched` false)
    // follows the calls it makes into their traced originals (probeRunCall,
    // at most kProbeDepth deep) and stops at its first unknown byte, a jump
    // out it cannot follow, an I/O access or the cap; its writes are its own
    // (undone when it ends). A matched run (its recording answers its calls,
    // probeStub) knows every byte it reads unless it went another way than
    // its record; its writes stay for the rest of the run.
    class ProbeTracer final : public fncheck::Tracer
    {
    public:
        void begin(bool matched)
        {
            m_matched = matched;
            m_cycles = 0;
            m_capped = false;
            m_stopKey = 0;
            m_written.clear();
        }
        void read(uint32_t address, uint32_t size) override
        {
            const uint32_t key = fncheck::memoryKey(address);
            traceEvent(1u, key, size);
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint32_t k = key + i;
                if (m_written.count(k))
                    continue; // (its own value)
                // (the index's pass: the caller's leftover, read through a
                // pointer into its frame; known, not compared)
                if (!g_sandbox.changed(k) && !g_sandbox.ownFrame(k) && g_checker.calleeLeftover(k))
                    continue;
                if (!g_sandbox.knows(k))
                {
                    m_stopKey = k;
                    throw ProbeStop{kProbeUnknown};
                }
                if (!g_sandbox.noteRead(k))
                    throw ProbeStop{kProbeCap};
            }
        }
        void write(uint32_t address, uint32_t size) override
        {
            const uint32_t key = fncheck::memoryKey(address);
            traceEvent(2u, key, size);
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint32_t k = key + i;
                if (!accessible(k, 1u))
                {
                    m_stopKey = k;
                    throw ProbeStop{kProbeUnknown}; // (a page the sandbox does not hold)
                }
                if (m_matched)
                    g_sandbox.noteWrite(k, true);
                else if (m_written.insert(k).second)
                    g_sandbox.noteWrite(k, false);
            }
            g_trace.pending = true;
            g_trace.pendingKey = key;
            g_trace.pendingSize = size;
        }
        void special(uint32_t) override { throw ProbeStop{kProbeIo}; }
        // A probe's calls are run by probeRunCall; a matched run's land in
        // probeStub (their table slots): only the one its record makes next.
        void beforeCall(uint8_t *, R5900Context *, uint32_t target) override;
        // A probe follows a jump made as a direct C++ call (its traced code
        // returns with pc at the target: probeFrom goes on there); one that
        // goes through the dispatcher it cannot follow.
        void tailCall(bool direct) override
        {
            if (!m_matched && !direct)
                throw ProbeStop{kProbeCall};
        }
        bool matched() const { return m_matched; }
        uint32_t depth = 0; // probe calls open (probeRunCall)
        void afterCall(uint8_t *, R5900Context *, bool) override {}
        void io(const fncheck::IoAccess &) override { throw ProbeStop{kProbeIo}; }
        void cycles(uint32_t count) override { m_cycles += count; }
        // Past the cap the traced code unwinds at its next safe point.
        bool safePointDue(uint32_t) override
        {
            if (m_cycles > kCalleeProbeCycles)
                m_capped = true;
            return m_capped;
        }
        bool capped() const { return m_capped; }
        uint32_t stopKey() const { return m_stopKey; }
        uint64_t charged() const { return m_cycles; }

    private:
        bool m_matched = false;
        uint64_t m_cycles = 0;
        bool m_capped = false;
        uint32_t m_stopKey = 0;
        std::unordered_set<uint32_t> m_written;
    };

    // ZX_FNCHECK_PROBES=1: a line per call looked at on stderr.
    bool showProbes()
    {
        static const bool show = [] { const char *v = std::getenv("ZX_FNCHECK_PROBES"); return v && *v == '1'; }();
        return show;
    }

    constexpr uint32_t kProbeDepth = 16;

    // A function left by a direct tail call (pc at another function's start,
    // not at `ra`): its target goes on, traced, as the game's would.
    void runTailCalls(R5900Context &ctx, uint32_t ra, PS2Runtime *runtime)
    {
        for (uint32_t hops = 0; ctx.pc != ra && hops < 64u; ++hops)
        {
            if (untracedCode(ctx.pc))
                throw ProbeStop{kProbeRuntime};
            const PS2Runtime::RecompiledFunction next = game::readable::tracedOriginal(ctx.pc & PS2_RAM_MASK, g_game);
            if (!next)
                return;
            next(g_ram.base(), &ctx, runtime);
        }
    }

    // fncheck::g_callRunner: a probe's call, run by the callee's traced
    // original with the probe's tracer (its reads are the call tree's).
    bool probeRunCall(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t target, uint32_t returnPc)
    {
        ProbeTracer *probe = dynamic_cast<ProbeTracer *>(fncheck::t_tracer);
        if (!probe || probe->matched())
            return false;
        if (probe->capped())
            throw ProbeStop{kProbeCap};
        if (untracedCode(target))
            throw ProbeStop{kProbeRuntime};
        const PS2Runtime::RecompiledFunction traced = game::readable::tracedOriginal(target & PS2_RAM_MASK, g_game);
        if (!traced || probe->depth >= kProbeDepth)
            throw ProbeStop{kProbeCall};
        ++probe->depth;
        ctx->pc = target;
        traced(rdram, ctx, runtime);
        runTailCalls(*ctx, returnPc, runtime);
        --probe->depth;
        if (ctx->pc != returnPc)
            throw ProbeStop{kProbeCall}; // (left some other way)
        return true;
    }

    // Runs the callee at ctx->pc as a probe (its writes undone after); how it ended.
    ProbeEnd probeFrom(const R5900Context &atCall, PS2Runtime *runtime)
    {
        const uint32_t target = atCall.pc;
        if (untracedCode(target))
            return kProbeRuntime;
        const PS2Runtime::RecompiledFunction traced = game::readable::tracedOriginal(target & PS2_RAM_MASK, g_game);
        if (!traced)
            return kProbeNoOriginal;
        ProbeTracer probe;
        probe.begin(false);
        static ProbeIo io;
        R5900Context ctx = atCall;
        const uint32_t ra = static_cast<uint32_t>(ee::gpr64(&atCall, ee::reg::ra));
        fncheck::Tracer *const outerTracer = fncheck::t_tracer;
        fncheck::IoPort *const outerIo = fncheck::t_ioPort;
        const size_t mark = g_sandbox.mark();
        ProbeEnd end = kProbeReturned;
        fncheck::t_tracer = &probe;
        fncheck::t_ioPort = &io;
        try
        {
            traced(g_ram.base(), &ctx, runtime);
            runTailCalls(ctx, ra, runtime);
            if (probe.capped())
                end = kProbeCap;
            else if (ctx.pc != ra)
                end = kProbeCall; // (left by a jump it cannot follow)
        }
        catch (const ProbeStop &stop)
        {
            end = stop.end;
        }
        fncheck::t_tracer = outerTracer;
        fncheck::t_ioPort = outerIo;
        traceFlushWrite();
        if (end == kProbeReturned)
        {
            traceMix(ee::gpr64(&ctx, ee::reg::v0));
            traceMix(ee::gpr64(&ctx, ee::reg::v1));
        }
        g_sandbox.revert(mark);
        g_probeStats.cycles += probe.charged();
        if (showProbes())
            std::fprintf(stderr, "[fncheck:probe]   call #%" PRIu64 " probe of 0x%x sp 0x%x: %s, stopped at 0x%x, pc 0x%x\n",
                         g_sequence, target, static_cast<uint32_t>(ee::gpr64(&atCall, ee::reg::sp)), kProbeEndName[end],
                         probe.stopKey(), ctx.pc);
        return end;
    }

    // A matched run's call: probed, then answered from the callee's record.
    struct MatchedRun
    {
        const Record *record = nullptr;
        size_t next = 0;
    };
    MatchedRun g_matchedRun;
    void ProbeTracer::beforeCall(uint8_t *, R5900Context *, uint32_t target)
    {
        traceEvent(3u, target, 0u);
        if (!m_matched)
            return; // (probeRunCall runs it)
        const MatchedRun &m = g_matchedRun;
        if (!m.record || m.next >= m.record->calls.size() || m.record->calls[m.next].target != target)
            throw ProbeStop{kProbeUnknown}; // (another call than its record's: went another way)
    }
    void probeStub(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        MatchedRun &m = g_matchedRun;
        const uint32_t ra = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        if (!m.record || m.next >= m.record->calls.size() || m.record->calls[m.next].target != ctx->pc)
            throw ProbeStop{kProbeUnknown}; // (another call than its record's: went another way)
        const Call &c = m.record->calls[m.next++];
        fncheck::Tracer *const runTracer = fncheck::t_tracer;
        probeFrom(*ctx, runtime);
        fncheck::t_tracer = runTracer;
        for (const Line &l : c.effects)
            for (uint32_t i = 0; i < 16u; ++i)
                if (l.mask & (1u << i))
                {
                    if (!accessible(l.address + i, 1u))
                        throw ProbeStop{kProbeUnknown};
                    g_sandbox.set(l.address + i, l.bytes[i]);
                }
        *ctx = c.after;
        ctx->pc = ra;
    }

    // The callee's own recording in the folder: a record of it made at this
    // very call holds every byte its own code read (its inputs) and answers
    // its calls. The call is told by the registers at it (all the general
    // registers, which the original's replay has exactly as the game had
    // them) and the vsync; a record that more than one call matches is not
    // used, and one whose inputs disagree with what the replay holds is
    // another call's. Indexed once per process from the records' heads
    // (fncheck::skimRecord).
    struct CalleeRecordings
    {
        struct Entry
        {
            uint64_t vsync = 0;
            uint64_t registers = 0; // registersKey of the entry context
            std::streamoff offset = 0;
            std::streamoff bytes = 0; // the record's size in the file
        };
        bool ok = false;
        uint32_t version = 0;
        std::string path;
        std::vector<Entry> entries; // by vsync
        std::vector<fncheck::FrameStart> frames; // a compressed file's, for the record's seek
    };
    std::unordered_map<std::string, std::unique_ptr<CalleeRecordings>> g_calleeRecordings;
    std::filesystem::path g_folder; // the folder of the file being checked
    uint64_t g_recordVsync = 0;     // the vsync of the record being replayed
    size_t g_calleeEntries = 0;     // entries indexed (all files): at most kCalleeEntries
    constexpr size_t kCalleeEntries = size_t(1) << 22;
    constexpr uint64_t kCalleeVsyncs = 16; // a callee's record begins at most this many vsyncs after its caller's
    // A callee record larger than this (in the file) is not run: it is probed
    // (a big one costs more to read and fill in than the replay of the call).
    constexpr std::streamoff kCalleeRecordInputs = std::streamoff(1) << 20;

    uint64_t registersKey(const R5900Context &ctx)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (int r = 1; r < 32; ++r)
        {
            h ^= ee::gpr64(&ctx, r);
            h *= 0x100000001B3ull;
        }
        return h;
    }

    const CalleeRecordings *calleeRecordingsOf(uint32_t target)
    {
        // (By target within the folder first: a path built for every call was
        // a good part of a probe that ends at once.)
        static std::filesystem::path s_folder;
        static game::Game s_game = game::Game::Ai;
        static std::unordered_map<uint32_t, const CalleeRecordings *> s_byTarget;
        if (s_folder != g_folder || s_game != g_game)
        {
            s_byTarget.clear();
            s_folder = g_folder;
            s_game = g_game;
        }
        if (const auto it = s_byTarget.find(target & PS2_RAM_MASK); it != s_byTarget.end())
            return it->second;
        const std::string path = (g_folder / fncheck::recordingName(target & PS2_RAM_MASK, g_game)).string();
        auto &slot = g_calleeRecordings[path];
        if (!slot)
            slot = std::make_unique<CalleeRecordings>();
        s_byTarget[target & PS2_RAM_MASK] = slot.get();
        if (!slot->path.empty())
            return slot.get(); // (indexed already)
        CalleeRecordings &cr = *slot;
        cr.path = path;
        fncheck::RecordingReader reader;
        reader.open(path);
        std::istream &in = reader.stream();
        fncheck::FileHeader header;
        if (!in || !fncheck::readHeader(in, header, nullptr) || header.game != static_cast<uint32_t>(g_game) ||
            header.address != (target & PS2_RAM_MASK))
            return slot.get();
        cr.version = header.version;
        const auto started = std::chrono::steady_clock::now();
        struct Timed
        {
            std::chrono::steady_clock::time_point started;
            ~Timed() { g_probeStats.indexSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }
        } timed{started};
        fncheck::RecordHead head;
        for (;;)
        {
            const std::streamoff at = in.tellg();
            if (g_calleeEntries >= kCalleeEntries || !fncheck::skimRecord(in, head, header.version))
                break;
            cr.entries.push_back({head.vsync, registersKey(head.entry), at, static_cast<std::streamoff>(in.tellg()) - at});
            ++g_calleeEntries;
        }
        std::stable_sort(cr.entries.begin(), cr.entries.end(),
                         [](const CalleeRecordings::Entry &a, const CalleeRecordings::Entry &b) { return a.vsync < b.vsync; });
        cr.ok = !cr.entries.empty();
        cr.frames = reader.frames();
        return slot.get();
    }

    // The callee's record of this call, if the folder has exactly one; `ambiguous` if more.
    bool calleeRecordOf(uint32_t target, const R5900Context &atCall, Record &record, bool &ambiguous)
    {
        ambiguous = false;
        const CalleeRecordings *cr = calleeRecordingsOf(target);
        if (!cr || !cr->ok)
            return false;
        ++g_probeStats.calleeRecorded;
        const uint64_t key = registersKey(atCall);
        const auto first = std::lower_bound(cr->entries.begin(), cr->entries.end(), g_recordVsync,
                                            [](const CalleeRecordings::Entry &e, uint64_t v) { return e.vsync < v; });
        const CalleeRecordings::Entry *found = nullptr;
        for (auto it = first; it != cr->entries.end() && it->vsync <= g_recordVsync + kCalleeVsyncs; ++it)
            if (it->registers == key)
            {
                if (found)
                {
                    ambiguous = true;
                    return false;
                }
                found = &*it;
            }
        if (!found || found->bytes > kCalleeRecordInputs)
            return false;
        fncheck::RecordingReader reader;
        reader.open(cr->path, cr->frames);
        std::istream &in = reader.stream();
        return in && in.seekg(found->offset) && fncheck::readRecord(in, record, cr->version);
    }

    // The callee run on its record (above), from the original's state at the
    // call; false if the record is not this call's (its inputs disagree with
    // the replay's memory), and `diverged` if it went another way part way.
    bool runMatched(const Record &record, const R5900Context &atCall, PS2Runtime *runtime, bool &diverged)
    {
        diverged = false;
        const uint32_t target = atCall.pc;
        const PS2Runtime::RecompiledFunction traced = game::readable::tracedOriginal(target & PS2_RAM_MASK, g_game);
        if (!traced || untracedCode(target))
            return false;
        // Its inputs where the replay holds the original's bytes must be
        // them; elsewhere they fill the sandbox in (their pages opened).
        std::set<uint32_t> pages;
        auto addPages = [&](const std::vector<Line> &lines)
        {
            for (const Line &l : lines)
                if (!accessible(l.address, 1u))
                    pages.insert(g_ram.pageOf(l.address));
        };
        addPages(record.inputs);
        addPages(record.outputs);
        for (const Call &c : record.calls)
            addPages(c.effects);
        // (Run again, the bytes it compares are the readable version's.)
        if (!g_sandbox.rerun())
            for (const Line &l : record.inputs)
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    const uint32_t k = l.address + i;
                    if ((l.mask & (1u << i)) && accessible(k, 1u) && g_sandbox.outer(k) && g_ram.base()[k] != l.bytes[i])
                        return false;
                }
        g_sandbox.open(pages);
        for (const Line &l : record.inputs)
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t k = l.address + i;
                if ((l.mask & (1u << i)) && !g_sandbox.outer(k))
                    g_sandbox.fill(k, l.bytes[i]);
            }
        // Its calls answered by probeStub (their table slots, put back after).
        std::vector<std::pair<uint32_t, PS2Runtime::RecompiledFunction>> slots;
        for (const Call &c : record.calls)
        {
            const uint32_t slot = (c.target & PS2_RAM_MASK) >> 2;
            slots.emplace_back(slot, g_ps2RecompiledFunctionTable[slot]);
            g_ps2RecompiledFunctionTable[slot] = &probeStub;
        }
        g_matchedRun = MatchedRun{&record, 0};
        ProbeTracer run;
        run.begin(true);
        static ProbeIo io;
        R5900Context ctx = atCall;
        const uint32_t ra = static_cast<uint32_t>(ee::gpr64(&atCall, ee::reg::ra));
        fncheck::Tracer *const outerTracer = fncheck::t_tracer;
        fncheck::IoPort *const outerIo = fncheck::t_ioPort;
        fncheck::t_tracer = &run;
        fncheck::t_ioPort = &io;
        try
        {
            traced(g_ram.base(), &ctx, runtime);
            // A tail call it left by: answered as the original's are.
            for (uint32_t hops = 0; ctx.pc != ra && hops < fncheck::kMaxSelfCallResumes; ++hops)
            {
                if (g_matchedRun.next >= record.calls.size() || record.calls[g_matchedRun.next].target != ctx.pc)
                    break;
                probeStub(g_ram.base(), &ctx, runtime);
            }
            diverged = ctx.pc != ra || run.capped() || g_matchedRun.next != record.calls.size();
        }
        catch (const ProbeStop &)
        {
            diverged = true;
        }
        fncheck::t_tracer = outerTracer;
        fncheck::t_ioPort = outerIo;
        traceFlushWrite();
        if (!diverged)
        {
            traceMix(ee::gpr64(&ctx, ee::reg::v0));
            traceMix(ee::gpr64(&ctx, ee::reg::v1));
        }
        g_probeStats.cycles += run.charged();
        for (auto it = slots.rbegin(); it != slots.rend(); ++it)
            g_ps2RecompiledFunctionTable[it->first] = it->second;
        g_matchedRun = MatchedRun{};
        if (showProbes())
            std::fprintf(stderr, "[fncheck:probe]   call #%" PRIu64 " matched run of 0x%x: %s, stopped at 0x%x, pc 0x%x\n",
                         g_sequence, target, diverged ? "diverged" : "to its return", run.stopKey(), ctx.pc);
        return true;
    }

    // One look at the callee of a call (above): its own record of the call
    // run, else a probe, in the sandbox (left as it was); its bytes in
    // g_sandbox.reads(), its trace in g_trace. `overlay`: run again on the
    // bytes a first run kept (calleeActsOn).
    ProbeEnd lookAtCallee(const R5900Context &atCall, PS2Runtime *runtime, const CalleeReads *overlay, uint64_t limit)
    {
        const uint32_t target = atCall.pc;
        const uint32_t sp = static_cast<uint32_t>(ee::gpr64(&atCall, ee::reg::sp));
        const uint32_t count = runtime->readCop0Count();
        fncheck::BlockSink *const outerSink = fncheck::t_blockSink;
        fncheck::t_blockSink = nullptr;
        g_sandbox.begin(sp, stackArgsOf(target));
        g_sandbox.overlay(overlay);
        g_trace = ProbeTrace{};
        g_trace.limit = limit;
        g_probing = true;
        // ZX_FNCHECK_PROBE_VERIFY=1: the open pages compared before and
        // after (what a probe changed that the sandbox did not undo).
        static const bool s_verify = [] { const char *v = std::getenv("ZX_FNCHECK_PROBE_VERIFY"); return v && *v == '1'; }();
        std::vector<uint8_t> before;
        if (s_verify && g_openPages)
            for (uint32_t page : *g_openPages)
                before.insert(before.end(), g_ram.base() + static_cast<size_t>(page) * g_ram.pageSize(),
                              g_ram.base() + static_cast<size_t>(page + 1) * g_ram.pageSize());
        const size_t scriptAt = scriptNextIndex();
        static Record callee;
        bool ambiguous = false, matched = false, diverged = false;
        if (!untracedCode(target) && calleeRecordOf(target, atCall, callee, ambiguous))
        {
            matched = runMatched(callee, atCall, runtime, diverged);
            if (!overlay)
                g_probeStats.inconsistent += matched ? 0u : 1u;
        }
        if (ambiguous && !overlay)
            ++g_probeStats.ambiguous;
        ProbeEnd end;
        if (matched)
        {
            end = kProbeMatched;
            if (!overlay)
                g_probeStats.diverged += diverged ? 1u : 0u;
        }
        else
            end = probeFrom(atCall, runtime);
        traceMix(end);
        g_sandbox.overlay(nullptr);
        g_sandbox.end();
        g_probing = false;
        if (s_verify && g_openPages)
        {
            size_t at = 0;
            for (uint32_t page : *g_openPages)
                for (uint32_t i = 0; i < g_ram.pageSize(); ++i, ++at)
                {
                    const uint32_t k = page * g_ram.pageSize() + i;
                    if (g_ram.base()[k] != before[at])
                    {
                        std::fprintf(stderr, "[fncheck:probe] call #%" PRIu64 " to 0x%x: left EE 0x%08x 0x%02x, was 0x%02x\n",
                                     g_sequence, target, k, g_ram.base()[k], before[at]);
                        g_ram.base()[k] = before[at];
                    }
                }
        }
        if (scriptNextIndex() != scriptAt)
            std::fprintf(stderr, "[fncheck:probe] call #%" PRIu64 " to 0x%x: the probe moved the replay's calls\n", g_sequence, target);
        fncheck::t_blockSink = outerSink;
        runtime->writeCop0Count(count);
        return end;
    }

    // The original's call `index` to atCall.pc: what its callee reads (above).
    void probeCallee(size_t index, const R5900Context &atCall, PS2Runtime *runtime)
    {
        if (g_calleeReads.size() <= index)
            g_calleeReads.resize(index + 1);
        CalleeReads &out = g_calleeReads[index];
        out = CalleeReads{};
        out.taken = true;
        ++g_probeStats.probes;
        const uint32_t target = atCall.pc;
        const uint32_t sp = static_cast<uint32_t>(ee::gpr64(&atCall, ee::reg::sp));
        ProbeEnd end = kProbeCap;
        const auto started = std::chrono::steady_clock::now();
        // (A library stub, a syscall: nothing to run, so no sandbox either;
        // copyTextureTiles makes 700,000 calls of one in a recorded call.)
        if (untracedCode(target))
        {
            end = kProbeRuntime;
            g_sandbox.clear();
        }
        else if (g_calleeRecordBytes < kCalleeRecordBytes)
            end = lookAtCallee(atCall, runtime, nullptr, ~0ull);
        else
            g_sandbox.clear(); // (nothing kept)
        g_probeStats.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const std::map<uint32_t, uint8_t> &reads = g_sandbox.reads();
        ++g_probeStats.ends[end];
        out.keys.reserve(reads.size());
        out.values.reserve(reads.size());
        for (const auto &[key, value] : reads)
        {
            out.keys.push_back(key);
            out.values.push_back(value);
        }
        if (!reads.empty())
        {
            out.trace = g_trace.hash;
            out.events = g_trace.events;
            out.atCall = std::make_unique<R5900Context>(atCall);
            out.context.assign(g_sandbox.context().begin(), g_sandbox.context().end());
        }
        g_probeStats.bytes += reads.size();
        g_calleeRecordBytes += reads.size() + (reads.empty() ? 0u : sizeof(R5900Context));
        g_probeStats.withBytes += reads.empty() ? 0u : 1u;
        size_t bucket = 0;
        while (bucket + 1 < std::size(g_probeStats.buckets) && (reads.size() >> bucket) != 0u)
            ++bucket;
        ++g_probeStats.buckets[bucket];
        if (showProbes())
        {
            std::string list;
            for (const auto &[key, value] : reads)
                if (list.size() < 400)
                {
                    char b[24];
                    std::snprintf(b, sizeof(b), " %x=%02x", key, value);
                    list += b;
                }
            std::fprintf(stderr, "[fncheck:probe] call #%" PRIu64 " index %zu to 0x%x sp 0x%x ra 0x%x: %s, %zu bytes:%s\n",
                         g_sequence, index, target, sp, static_cast<uint32_t>(ee::gpr64(&atCall, ee::reg::ra)),
                         kProbeEndName[end], reads.size(), list.c_str());
        }
    }

    // Some of the bytes a callee read differ at the readable version's call:
    // whether the callee does anything else with them. Its call tree is run
    // again from the original's registers at the call, knowing only the bytes
    // the first run kept (the readable version's values of them now in RAM),
    // for as many events as the first run had: the same trace says no.
    bool calleeActsOn(const CalleeReads &want, PS2Runtime *runtime)
    {
        if (!want.atCall)
            return true;
        lookAtCallee(*want.atCall, runtime, &want, want.events);
        return g_trace.hash != want.trace || g_trace.events != want.events;
    }

    // Answers calls from the recording, in order.
    class Script final : public ee::CallHandler
    {
    public:
        void reset(const Record &r, bool checkReads, uint32_t function)
        {
            m_record = &r;
            m_function = function & PS2_RAM_MASK;
            m_end = fncheck::tracedExtentEnd(m_function);
            m_next = 0;
            m_problem.clear();
            m_newProblem.clear();
            m_argsLeft.clear();
            m_handedLeft.clear();
            m_wide.clear();
            m_upper.clear();
            m_direct.clear();
            m_saves.clear();
            m_calleeSp.clear();
            m_held.clear();
            m_entryLeft.clear();
            m_staleLeft.clear();
            m_vu0Call.clear();
            m_calleeRead.clear();
            m_calleeMasked = 0;
            g_firstProblem.clear();
            m_checkReads = checkReads;
            m_reachedStop = false;
            m_cycles = 0;
            ee::keepLeft(r.entry); // before the first call: the caller's registers
            for (int i = 0; i < ee::kIntArgRegs; ++i)
            {
                m_left[i] = ee::gpr64(&r.entry, ee::reg::a0 + i);
                m_from[i] = Held::Entry;
            }
            for (int i = 0; i < ee::kFloatArgRegs; ++i)
            {
                m_leftF[i] = r.entry.f[ee::reg::f12 + i];
                m_from[ee::kIntArgRegs + i] = Held::Entry;
            }
            m_lastDirect = false;
            m_stale.clear();
        }
        void cycles(uint64_t count) override
        {
            m_cycles += count;
            checkBudget(m_cycles);
        }
        uint32_t interruptions() const override { return g_sp.interrupted ? 1u : 0u; }
        // Generated code runs a call of the function itself inline, so the recording has the calls inside it and none
        // for it: this call is one when it goes to the replayed function and the recording's next call does not.
        bool callsItself(uint32_t target) const override
        {
            if ((target & PS2_RAM_MASK) != m_function)
                return false;
            const Call *next = nextCall();
            return !next || (next->target & PS2_RAM_MASK) != m_function;
        }
        // Generated code runs a jump into the function's own extent as a goto (a tail call in the readable form: BMX's
        // ghostRideMove into the riderless bike's move), so the recording has the calls after it and none for it: this
        // tail call is one when it goes inside the replayed function and the recording's next call is not one of it.
        bool jumpsInside(uint32_t target) const override
        {
            const uint32_t t = target & PS2_RAM_MASK;
            if (t < m_function || t >= m_end)
                return false;
            const Call *next = nextCall();
            return !next || (next->target & PS2_RAM_MASK) != t;
        }
        uint64_t charged() const { return m_cycles; }

        // regs: the call as made; filled with the recorded results. Null
        // if the recording has no such call.
        const Call *answer(ee::CallRegs &regs)
        {
            const size_t index = m_next++;
            if (index >= m_record->calls.size())
            {
                fail("call " + std::to_string(index) + " to " + hex(regs.target) + ": not in the recording (" +
                     std::to_string(m_record->calls.size()) + " calls recorded)");
                return nullptr;
            }
            const Call &c = m_record->calls[index];
            if (c.target != regs.target)
                fail("call " + std::to_string(index) + " goes to " + hex(regs.target) + ", recorded " + hex(c.target));
            // The words compared are those the callee takes in this game, not all the readable call passes: a
            // shared caller passes its AI callee's words, and another version's callee there can take fewer (DM2's
            // cloneScene one where AI's takes two, DM2's addGifTag four where AI's takes six). A word this game's
            // callee neither reads nor hands on (its row of the argument scan, an upper bound in another game) is
            // whatever the original's caller left there, not an argument: not compared. AI's replay compares
            // every word passed, as before; a callee with no row too. A word the callee reads that the call does
            // not pass is args_left's (compareArgsLeft).
            const ArgUse use = g_game == game::Game::Ai ? ArgUse{} : argUseOf(regs.target);
            const auto takes = [&](int bit) { return !use.known || ((use.reads | use.handed) >> bit & 1u); };
            for (int i = 0; i < regs.intCount; ++i)
                if (!takes(i))
                    continue;
                else if (static_cast<uint32_t>(c.a[i]) != static_cast<uint32_t>(regs.a[i]))
                    fail("call " + std::to_string(index) + " to " + hex(c.target) + ": a" + std::to_string(i) + " = " +
                         hex(static_cast<uint32_t>(regs.a[i])) + ", recorded " + hex(static_cast<uint32_t>(c.a[i])));
                else if (c.a[i] != regs.a[i] && m_wide.empty() &&
                         c.a[i] != static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(c.a[i]))))
                    // The low words agree; the original passed a 64-bit value
                    // (not a sign-extended word) and the readable another upper
                    // half: a 64-bit value handled as 32 (drafter25: _d2b's
                    // double, litodp's compare). A note: a callee may use only
                    // the low word.
                    m_wide = "call " + std::to_string(index) + " to " + hex(c.target) + ": a" + std::to_string(i) +
                             " = " + hex(regs.a[i]) + ", recorded " + hex(c.a[i]);
            // The upper half of an argument register the callee reads, which a
            // readable call that does not pass a quadword does not set.
            if (const auto reads = g_upperReads.find(c.target); reads != g_upperReads.end() && m_upper.empty() && c.highKnown)
                for (int i = 0; i < regs.intCount; ++i)
                    if ((reads->second.args & (1u << i)) &&
                        ((regs.qwordArgs & (1u << i)) ? regs.aHigh[i] : 0u) != c.aHigh[i])
                    {
                        m_upper = "call " + std::to_string(index) + " to " + hex(c.target) + ": a" + std::to_string(i) +
                                  "'s upper half, which the callee reads, recorded " + hex(c.aHigh[i]);
                        break;
                    }
            // A quadword argument: all 128 bits (recordings of version 5 on).
            for (int i = 0; i < regs.intCount; ++i)
                if (takes(i) && (regs.qwordArgs & (1u << i)) && c.highKnown && (c.a[i] != regs.a[i] || c.aHigh[i] != regs.aHigh[i]))
                    fail("call " + std::to_string(index) + " to " + hex(c.target) + ": quadword a" + std::to_string(i) +
                         " differs from the recorded one");
            for (int i = 0; i < regs.floatCount; ++i)
                if (takes(8 + i) && std::memcmp(&c.f[i], &regs.f[i], 4) != 0)
                    fail("call " + std::to_string(index) + " to " + hex(c.target) + ": f" + std::to_string(12 + i) +
                         " = " + std::to_string(regs.f[i]) + ", recorded " + std::to_string(c.f[i]));
            if (c.flags & fncheck::kStopCall)
            {
                // The call the record stops at: checked as above, no results
                // (the callee never came back); the caller throws StopReached.
                m_reachedStop = true;
                return &c;
            }
            regs.v0 = ee::gpr64(&c.after, ee::reg::v0);
            regs.v1 = ee::gpr64(&c.after, ee::reg::v1);
            regs.f0 = c.after.f[ee::reg::f0];
            // An argument register the callee left as it got it holds what the
            // readable call had there, not the original's (a readable caller
            // that reads one after the call without passing it would read the
            // recording's value: a hole gamehand2 asked about). Passed: the
            // readable's argument; not passed: what the register held before
            // the call in the game (the last call's leftover, or the caller's
            // at entry: Script::reset starts the leftovers there).
            uint64_t before[ee::kIntArgRegs];
            float beforeF[ee::kFloatArgRegs];
            for (int i = 0; i < ee::kIntArgRegs; ++i)
                before[i] = ee::t_ee.left.low[ee::reg::a0 + i];
            for (int i = 0; i < ee::kFloatArgRegs; ++i)
                beforeF[i] = ee::t_ee.left.f[ee::reg::f12 + i];
            ee::keepLeft(c.after); // the callee's temporaries as recorded (ee::leftByCall)
            for (int i = 0; i < ee::kIntArgRegs; ++i)
                if (ee::gpr64(&c.after, ee::reg::a0 + i) == c.a[i])
                    ee::t_ee.left.low[ee::reg::a0 + i] = i < regs.intCount ? regs.a[i] : before[i];
            for (int i = 0; i < ee::kFloatArgRegs; ++i)
                if (std::memcmp(&c.after.f[ee::reg::f12 + i], &c.f[i], 4) == 0)
                    ee::t_ee.left.f[ee::reg::f12 + i] = i < regs.floatCount ? regs.f[i] : beforeF[i];
            // VU0 and COP0 status as the callee left them, as the game's calls keep them (the original's replay
            // takes the whole context after the call). Status: an address error the callee raised leaves EXL set
            // (port3: the original newCreatedRiderModel's stale name word in nameHash); interrupts it enabled or
            // disabled. The readable's status at the call is the original's (compareStatusAtCall).
            if (ee::Frame *frame = ee::t_ee.frame; frame && frame->ctx)
            {
                ee::Vu0State::of(c.after).into(*frame->ctx);
                frame->ctx->cop0_status = c.after.cop0_status;
            }
            for (const Line &l : c.effects)
            {
                writeLine(g_ram.base(), l);
                if (m_checkReads)
                    g_checker.markValid(l);
            }
            return &c;
        }

        void call(ee::CallRegs &regs) override
        {
            compareSaves(regs);
            compareStackArgs(regs);
            compareArgsLeft(regs);
            compareVu0AtCall(regs);
            compareStatusAtCall(regs);
            compareCalleeReads(regs);
            compareCalleeSp(regs);
            // --safepoints: the dispatch (charged by ee::call) is a safe point.
            const bool unwound = g_sp.on && !regs.tail && safePointNext(EeScheduler::kGuestDispatchCycles);
            if (g_sp.on)
                if (ee::Frame *frame = ee::t_ee.frame)
                    frame->callUnwound = false; // (each call's own, as callSuspending has it)
            if (const Call *c = answer(regs))
            {
                if (c->flags & fncheck::kStopCall)
                    throw StopReached{};
                keepHeld(regs, *c);
            }
            if (unwound)
            {
                // The callee returned through the scheduler, which dispatches
                // the return address: the frame goes on as the table has it
                // there (ee/suspend.cpp callSuspending).
                if (ee::Frame *frame = ee::t_ee.frame)
                {
                    frame->mode = ee::kWhole;
                    frame->paid = false;
                    frame->callUnwound = true;
                }
                m_cycles += EeScheduler::kGuestDispatchCycles;
            }
        }

        // Where an argument register's value comes from at this point of the
        // readable function, in the game. Entry: its caller's, as the context
        // had it (only so if a table call entered it: a direct caller,
        // game::call, sets no register); Passed: an ee::call set it; Callee: a
        // guest callee left it (the fiber's context keeps what a real guest
        // call leaves); Direct: a readable callee called directly changed it
        // in the recording, which in the game only its own guest calls do.
        enum class Held : uint8_t
        {
            Entry,
            Passed,
            Callee,
            Direct,
        };
        void keepHeld(const ee::CallRegs &regs, const Call &c)
        {
            m_lastDirect = regs.direct;
            for (int i = 0; i < ee::kIntArgRegs; ++i)
            {
                if (!regs.direct && i < regs.intCount)
                {
                    m_left[i] = regs.a[i];
                    m_from[i] = Held::Passed;
                }
                const uint64_t after = ee::gpr64(&c.after, ee::reg::a0 + i);
                if (after != c.a[i])
                {
                    m_left[i] = after;
                    m_from[i] = regs.direct ? Held::Direct : Held::Callee;
                }
            }
            for (int i = 0; i < ee::kFloatArgRegs; ++i)
            {
                if (!regs.direct && i < regs.floatCount)
                {
                    m_leftF[i] = regs.f[i];
                    m_from[ee::kIntArgRegs + i] = Held::Passed;
                }
                if (std::memcmp(&c.after.f[ee::reg::f12 + i], &c.f[i], 4) != 0)
                {
                    m_leftF[i] = c.after.f[ee::reg::f12 + i];
                    m_from[ee::kIntArgRegs + i] = regs.direct ? Held::Direct : Held::Callee;
                }
            }
        }

        // An argument register the callee reads (or hands on) that the
        // readable call does not pass (rider2a: a readable call that leaves
        // out arguments its callee reads went unnoticed): the callee gets
        // what the context holds there (Held). Noted when that differs from
        // what the original passed, or can't be told (Direct): args_left, a
        // failure (argscan1; readable_args_dead.tsv takes out a read that is
        // not one).
        // Counted apart for a readable callee called directly (game::call,
        // CallRegs::direct: direct_left): its signature is all its readable
        // callers give it, so a register the original reads or hands on
        // reaches it as whatever the context holds (zgehand10: freeLoadedFile
        // without the a0 it hands on to the heap's free, a level-load hang
        // only logiccheck caught). A register with the original's value that
        // is the function's own entry value left in place: right here, and
        // right in the game while a table call enters the function, but not
        // from a direct caller (entry_left: gamehand25's activateEditTool,
        // which left a0 and a1 for a tool method, hung the EE at tick 4480
        // with every replay passing). A failure for a function some game::call
        // names; readable_args_dead.tsv takes out a read that is not one.
        // A register the callee only hands on to a callee the scan cannot
        // name, an indirect call no recording saw, a kernel call (handed_left;
        // readable_indirect_calls.tsv names the rest): noted, an upper bound.
        void compareArgsLeft(const ee::CallRegs &regs)
        {
            const size_t index = m_next;
            if (index >= m_record->calls.size())
                return;
            const Call &c = m_record->calls[index];
            if (c.target != regs.target)
                return;
            const ArgUse use = argUseOf(regs.target);
            std::string entry;
            const std::string readLeft = firstLeft(regs, c, use.reads, index, entry);
            if (m_staleLeft.empty())
                m_staleLeft = firstStale(regs, c, use.reads, index);
            std::string unusedEntry;
            const std::string handedLeft = readLeft.empty() ? firstLeft(regs, c, use.handed & ~use.reads, index, unusedEntry) : std::string();
            if (m_argsLeft.empty())
                m_argsLeft = readLeft;
            if (m_handedLeft.empty())
                m_handedLeft = handedLeft;
            if (m_entryLeft.empty())
                m_entryLeft = entry;
            if (regs.direct && m_direct.empty())
                m_direct = readLeft.empty() ? handedLeft : readLeft;
        }

        // The first register of `reads` the call leaves out, with another
        // value than the original passed; `entry`, the first left in place
        // from the function's entry with the original's value.
        std::string firstLeft(const ee::CallRegs &regs, const Call &c, uint16_t reads, size_t index, std::string &entry) const
        {
            if (!(reads >> regs.intCount & 0xFFu) && !(reads >> (8 + regs.floatCount) & 0xFFu))
                return {};
            std::string found;
            const auto note = [&](const std::string &reg, const std::string &left, const std::string &original, bool same, Held held)
            {
                const std::string head = "call " + std::to_string(index) + " to " + hex(c.target) + ": " + reg + " not passed ";
                if (held == Held::Direct && found.empty())
                    found = head + "(the callee reads it or hands it on): what a readable callee called directly left (recorded " +
                            left + "), the original's " + original;
                else if (!same && found.empty())
                    found = head + "(the callee reads it or hands it on): left " + left + ", the original's " + original;
                else if (same && held == Held::Entry && entry.empty())
                    entry = head + "(the callee reads it or hands it on): the function's own entry value, left in place";
            };
            for (int i = regs.intCount; i < ee::kIntArgRegs; ++i)
                if (reads & (1u << i))
                    note(kArgName[i], hex(static_cast<uint32_t>(m_left[i])), hex(static_cast<uint32_t>(c.a[i])),
                         static_cast<uint32_t>(m_left[i]) == static_cast<uint32_t>(c.a[i]), m_from[i]);
            for (int i = regs.floatCount; i < ee::kFloatArgRegs; ++i)
                if (reads & (1u << (8 + i)))
                    note("f" + std::to_string(12 + i), std::to_string(m_leftF[i]), std::to_string(c.f[i]),
                         std::memcmp(&m_leftF[i], &c.f[i], 4) == 0, m_from[ee::kIntArgRegs + i]);
            return found;
        }

        // An argument the callee reads with the value ee::leftByCall gave in
        // its register straight after a direct call (stale_left, a note: by
        // value, so a constant can match, and the direct callee's last guest
        // call may leave just that): the recording's value here, in the game
        // whatever that guest call left.
        std::string firstStale(const ee::CallRegs &regs, const Call &c, uint16_t reads, size_t index) const
        {
            for (const auto &[arg, bits] : m_stale)
            {
                const bool isFloat = arg >= ee::kIntArgRegs;
                const int i = isFloat ? arg - ee::kIntArgRegs : arg;
                if (i >= (isFloat ? regs.floatCount : regs.intCount) || !(reads & (1u << (isFloat ? 8 + i : i))))
                    continue;
                if ((isFloat ? floatBits(regs.f[i]) : static_cast<uint32_t>(regs.a[i])) == bits)
                    return "call " + std::to_string(index) + " to " + hex(c.target) + ": " +
                           (isFloat ? "f" + std::to_string(12 + i) : std::string(kArgName[i])) +
                           " is what a readable callee called directly left (ee::leftByCall), and the callee reads it";
            }
            return {};
        }

        // ee::callerRegister, ee::callerFloat, ee::leftByCall (ee/abi.h).
        void registerRead(int r, bool left) override
        {
            const bool isFloat = r >= 32;
            const int number = isFloat ? r - 32 : r;
            // Callee-saved registers have their model (Frame::view).
            if (isFloat ? number >= 20 : (ee::savedSlot(r) >= 0 || r == ee::reg::sp || r == 0))
                return;
            const std::string name = isFloat ? "f" + std::to_string(number) : "r" + std::to_string(number);
            if (left)
            {
                // After a direct call t_ee.left is what the callee's last guest
                // call left, not what the recording says it left: a callee
                // that reads this value (compareArgsLeft) may get another one.
                const int held = isFloat ? (number >= 12 ? ee::kIntArgRegs + number - 12 : -1)
                                         : (r >= ee::reg::a0 && r < ee::reg::a0 + ee::kIntArgRegs ? r - ee::reg::a0 : -1);
                const uint32_t bits = isFloat ? floatBits(ee::t_ee.left.f[number]) : static_cast<uint32_t>(ee::t_ee.left.low[r]);
                if (m_lastDirect && held >= 0 && bits != 0u && m_stale.size() < 16)
                    m_stale.emplace_back(held, bits);
                return;
            }
            const int arg = isFloat ? (number >= 12 ? ee::kIntArgRegs + number - 12 : -1)
                                    : (r >= ee::reg::a0 && r < ee::reg::a0 + ee::kIntArgRegs ? r - ee::reg::a0 : -1);
            // Only one the original's code needs: a register handed to a
            // variadic callee whose format does not take it, or a result
            // handed back unchanged (readable_args.py looks at those).
            if (arg >= 0 && m_from[arg] == Held::Entry && m_entryLeft.empty() && g_function &&
                (argUseOf(g_function->address).reads & (1u << arg)))
                m_entryLeft = "ee::caller" + std::string(isFloat ? "Float" : "Register") + "(" + name + ") reads the function's entry value";
        }
        const std::string &argsLeft() const { return m_argsLeft; }
        const std::string &handedLeft() const { return m_handedLeft; }
        const std::string &entryLeft() const { return m_entryLeft; }
        const std::string &staleLeft() const { return m_staleLeft; }
        const std::string &wide() const { return m_wide; }
        const std::string &upper() const { return m_upper; }
        const std::string &directLeft() const { return m_direct; }

        // The callee-saved registers at a readable version's call (not a tail
        // call): what the game's model gives the callee (ee::calleeSaves's
        // published value, else the function's entry value: ee::applySaves)
        // against what the original held there (the recorded call's
        // registers after it: the callee keeps them). saves: a register the
        // callee's prologue saves (the frame table), so in the game it writes
        // another value to its stack; held: any other (a deeper callee may
        // save it). ra only when published (a guest callee's is the runtime's
        // return slot). Notes, not failures; tools give the hands agents the
        // registers and values to publish.
        void compareSaves(const ee::CallRegs &regs)
        {
            ee::Frame *frame = ee::t_ee.frame;
            if (!frame)
                return;
            const ee::SavedRegs next = frame->next;
            frame->next.mask = 0; // (used up by the call, as applySaves does)
            const size_t index = m_next;
            if (regs.tail || index >= m_record->calls.size() || !frame->ctx)
                return;
            const Call &c = m_record->calls[index];
            if (c.target != regs.target)
                return;
            uint32_t saved = 0, wide = 0;
            if (const ee::FrameRow *row = ee::frameLayout(static_cast<uint32_t>(g_game), c.target & PS2_RAM_MASK))
            {
                const ee::EntrySave *s = ee::frameSaves(*row);
                for (uint32_t i = 0; i < row->count; ++i)
                    if (const int slot = ee::savedSlot(s[i].reg); slot >= 0)
                    {
                        saved |= 1u << slot;
                        if (s[i].bytes == 8)
                            wide |= 1u << slot;
                    }
            }
            const uint32_t viewMask = ee::framesOn() ? frame->view.mask : 0u;
            for (int slot = 0; slot < ee::kSavedSlots; ++slot)
            {
                const int r = ee::savedSlotReg(slot);
                const uint32_t bit = 1u << slot;
                if (r == 28 || (r == ee::reg::ra && !(next.mask & bit))) // (gp: the same everywhere)
                    continue;
                uint64_t model, original;
                if (next.mask & bit)
                    model = next.value[slot];
                else if (viewMask & bit)
                    model = frame->view.value[slot];
                else if (r >= 32)
                    model = floatBits(frame->ctx->f[r - 32]);
                else
                    model = ee::gpr64(frame->ctx, r);
                original = r >= 32 ? floatBits(c.after.f[r - 32]) : ee::gpr64(&c.after, r);
                // A 4-byte save (and an FPR) keeps the low word only.
                const bool differs = (saved & bit) && !(wide & bit) ? static_cast<uint32_t>(model) != static_cast<uint32_t>(original)
                                                                     : (r >= 32 ? static_cast<uint32_t>(model) != static_cast<uint32_t>(original)
                                                                                : model != original);
                if (!differs)
                    continue;
                // (target, the original's call site: the return address the
                // recorded call came back to, less 8) -> the registers
                const uint32_t site = static_cast<uint32_t>(ee::gpr64(&c.after, ee::reg::ra)) - 8u;
                g_saveSites[{c.target, site}] |= (saved & bit) ? bit : static_cast<uint64_t>(bit) << 32;
                std::string &first = (saved & bit) ? m_saves : m_held;
                if (first.empty())
                    first = "call " + std::to_string(index) + " to " + hex(c.target) + ": " + savedName(r) + " " +
                            ((next.mask & bit) ? "published " : "not published, ") + hex(model) + ", the original's " +
                            hex(original);
            }
        }
        static std::string savedName(int r)
        {
            static const char *const names[] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};
            if (r >= 16 && r <= 23)
                return names[r - 16];
            if (r == ee::reg::ra)
                return "ra";
            if (r == 30)
                return "fp";
            if (r >= 32)
                return "f" + std::to_string(r - 32);
            return "r" + std::to_string(r);
        }
        // The first difference of each kind in this replay.
        const std::string &savesDiffer() const { return m_saves; }
        const std::string &calleeSpDiffers() const { return m_calleeSp; }
        const std::string &heldDiffer() const { return m_held; }

        // A readable version's call (not a tail call) of a callee that reads
        // stack arguments: the bytes the original left there, at the sp the
        // callee gets (abi.cpp: below the innermost StackFrame, at least
        // kCalleeStackGap below sp).
        // The sp a readable callee would be entered with when this readable
        // version calls it directly (abi.cpp callGuest, enterDirectFrame: the
        // caller's fnSp for a tail call, else its calleeSp) against the sp of
        // the original's call (Call::sp): another one puts the callee's frame,
        // and what it leaves there, elsewhere than the original's (cov4's
        // __udivdi3 under timeMilliseconds). Information (callee_sp_differs).
        void compareCalleeSp(const ee::CallRegs &regs)
        {
            const size_t index = m_next;
            if (!m_calleeSp.empty() || index >= m_record->calls.size())
                return;
            const Call &c = m_record->calls[index];
            const Function *callee = game::readable::find(c.target & PS2_RAM_MASK, g_game);
            const ee::Frame *frame = ee::t_ee.frame;
            if (!callee || !callee->readable || !frame || c.target != regs.target)
                return;
            const uint32_t sp = regs.tail && frame->fnSp ? frame->fnSp : frame->calleeSp;
            if (sp == c.sp)
                return;
            char b[200];
            std::snprintf(b, sizeof(b), "call %zu to %s 0x%08x: sp 0x%08x, the original's 0x%08x%s", index, callee->name,
                          c.target, sp, c.sp, regs.tail ? " (a tail call)" : "");
            m_calleeSp = b;
        }

        void compareStackArgs(const ee::CallRegs &regs)
        {
            const size_t index = m_next;
            if (regs.tail || index >= g_stackSnapshots.size() || !g_stackSnapshots[index].taken)
                return;
            const ee::Frame *frame = ee::t_ee.frame;
            if (!frame)
                return;
            const StackSnapshot &snap = g_stackSnapshots[index];
            const uint32_t sp = frame->calleeSp; // (abi.cpp: StackFrame's, else kCalleeStackGap below sp)
            for (size_t i = 0; i < snap.bytes.size(); ++i)
            {
                if (!snap.known[i])
                    continue;
                const uint32_t a = sp + snap.low + static_cast<uint32_t>(i);
                const bool ok = accessible(fncheck::memoryKey(a), 1u);
                const uint8_t got = ok ? g_ram.base()[fncheck::memoryKey(a)] : 0u;
                if (!ok || got != snap.bytes[i])
                {
                    if (m_newProblem.empty())
                        m_newProblem = "call " + std::to_string(index) + " to " + hex(regs.target) +
                                       ": stack argument byte at sp+" + hex(snap.low + i) + " = " +
                                       (ok ? hex(got) : std::string("(not accessible)")) + ", the original's " +
                                       hex(snap.bytes[i]);
                    return;
                }
            }
        }
        // The VU0 lanes the callee reads before it sets them, against the
        // original's at its call: unlike them the callee works on another
        // value (vu0_call; a failure under --semantic, the recording answers
        // the call, so nothing else would show it).
        void compareVu0AtCall(const ee::CallRegs &regs)
        {
            const size_t index = m_next;
            const ee::Frame *frame = ee::t_ee.frame;
            if (!m_vu0Call.empty() || !frame || !frame->ctx || index >= g_vu0AtCalls.size() || !g_vu0AtCalls[index].taken)
                return;
            const LiveLanes live = liveLanesOf(regs.target);
            for (int i = 1; i < 32; ++i)
                for (int lane = 0; lane < 4; ++lane)
                    if (laneIn(live.atEntry, i, lane))
                    {
                        uint32_t have[4];
                        std::memcpy(have, &frame->ctx->vu0_vf[i], sizeof(have));
                        const uint32_t original = g_vu0AtCalls[index].lanes[i][lane];
                        if (have[lane] != original)
                        {
                            m_vu0Call = "call " + std::to_string(index) + " to " + hex(regs.target) + ": vf" + std::to_string(i) +
                                        "." + "xyzw"[lane] + " " + hex(have[lane]) + ", the original's " + hex(original);
                            return;
                        }
                    }
        }
        const std::string &vu0AtCall() const { return m_vu0Call; }
        // COP0 status at the call against the original's: the answer sets the callee's status after it (answer),
        // so a status the readable got wrong before a call (EI, DI) shows here, not at the return.
        void compareStatusAtCall(const ee::CallRegs &regs)
        {
            const size_t index = m_next;
            const ee::Frame *frame = ee::t_ee.frame;
            if (!frame || !frame->ctx || index >= g_vu0AtCalls.size() || !g_vu0AtCalls[index].taken)
                return;
            if (frame->ctx->cop0_status != g_vu0AtCalls[index].cop0Status)
                fail("call " + std::to_string(index) + " to " + hex(regs.target) + ": COP0 status " +
                     hex(frame->ctx->cop0_status) + ", the original's " + hex(g_vu0AtCalls[index].cop0Status));
        }

        // The bytes the original's callee read at this call (probeCallee),
        // against the readable version's memory at its call: unlike them the
        // callee works on another value (callee_read; a failure: the
        // recording answers the call, and the memory may be dead or
        // overwritten by the return).
        void compareCalleeReads(const ee::CallRegs &regs)
        {
            const size_t index = m_next;
            if (!m_calleeRead.empty() || index >= g_calleeReads.size() || !g_calleeReads[index].taken ||
                index >= m_record->calls.size() || m_record->calls[index].target != regs.target)
                return;
            const CalleeReads &want = g_calleeReads[index];
            // The word holding a byte, the readable's and the original's
            // (bytes of it not known to have been read: '?').
            auto differs = [&](uint32_t key, const std::vector<uint32_t> &keys, const std::vector<uint8_t> &values,
                               const char *how)
            {
                const uint32_t word = key & ~3u;
                std::string have, original;
                for (uint32_t b = 4; b-- > 0;)
                {
                    char text[4];
                    const uint32_t k = word + b;
                    if (accessible(k, 1u))
                        std::snprintf(text, sizeof(text), "%02x", g_ram.base()[k]);
                    else
                        std::snprintf(text, sizeof(text), "--");
                    have += text;
                    const auto at = std::lower_bound(keys.begin(), keys.end(), k);
                    if (at != keys.end() && *at == k)
                        std::snprintf(text, sizeof(text), "%02x", values[static_cast<size_t>(at - keys.begin())]);
                    else
                        std::snprintf(text, sizeof(text), "??");
                    original += text;
                }
                const bool scratch = key >= fncheck::kScratchpadKey;
                char where[48];
                std::snprintf(where, sizeof(where), scratch ? "scratchpad+0x%x" : "EE 0x%08x",
                              scratch ? word - fncheck::kScratchpadKey : word);
                const Function *callee = game::readable::find(regs.target & PS2_RAM_MASK, g_game);
                m_calleeRead = "call " + std::to_string(index) + " to " + hex(regs.target) +
                               (callee ? " " + std::string(callee->name) : std::string()) + ": the callee reads " + where +
                               " = 0x" + have + ", the original's 0x" + original + " (byte " + hex(key - word) + how + ")";
            };
            for (size_t i = 0; i < want.keys.size(); ++i)
            {
                const uint32_t key = want.keys[i];
                const bool ok = accessible(key, 1u);
                if (ok && g_ram.base()[key] == want.values[i])
                    continue;
                // Read but not acted on (the same trace on the readable
                // version's bytes): a note, callee_masked.
                if (!calleeActsOn(want, g_replayRuntime))
                {
                    ++m_calleeMasked;
                    break;
                }
                differs(key, want.keys, want.values, "");
                return;
            }
            // What the recording says the callee read of the caller's frame
            // in the game (Call::reads), past the probe's bytes: the call is
            // answered from the recording, so a wrong word the function left
            // there for its callee shows nowhere else (tools44: BMX XXX's
            // beginObjectiveBlock, a block class's handler in the wrong slot
            // of the record registerBlockClass copies). Whether the callee
            // acts on it cannot be run here: read, it counts.
            for (size_t i = 0; i < want.recordedKeys.size(); ++i)
            {
                const uint32_t key = want.recordedKeys[i];
                if ((accessible(key, 1u) && g_ram.base()[key] == want.recordedValues[i]) ||
                    std::binary_search(want.keys.begin(), want.keys.end(), key))
                    continue;
                differs(key, want.recordedKeys, want.recordedValues, ", in the caller's frame, as the recording has the callee read it");
                return;
            }
        }
        const std::string &calleeRead() const { return m_calleeRead; }
        uint64_t calleeMasked() const { return m_calleeMasked; }

        // The first difference of the checks added in round `coverage`.
        const std::string &newProblem() const { return m_newProblem; }
        size_t nextIndex() const { return m_next; }
        bool reachedStop() const { return m_reachedStop; }

        void finish()
        {
            if (m_next < m_record->calls.size())
                fail("made " + std::to_string(m_next) + " calls, recorded " + std::to_string(m_record->calls.size()) +
                     " (next: " + hex(m_record->calls[m_next].target) + ")");
        }

        const std::string &problem() const { return m_problem; }

        const Call *nextCall() const { return m_next < m_record->calls.size() ? &m_record->calls[m_next] : nullptr; }

        bool nextIsTailCallTo(uint32_t target) const
        {
            return m_next < m_record->calls.size() && m_record->calls[m_next].target == target &&
                   (m_record->calls[m_next].flags & fncheck::kTailCall);
        }

    private:
        void fail(const std::string &why)
        {
            if (m_problem.empty())
                m_problem = g_firstProblem = why;
        }

        const Record *m_record = nullptr;
        uint32_t m_function = 0; // the function replayed, masked
        uint32_t m_end = 0;      // the end of its traced extent (0: none)
        size_t m_next = 0;
        std::string m_problem;
        std::string m_newProblem;
        std::string m_argsLeft;
        std::string m_handedLeft; // the first a callee only hands on to an unnamed callee (a note)
        std::string m_entryLeft;  // the first entry value it relies on being in the context
        std::string m_staleLeft;  // the first leftover of a direct callee a callee reads
        std::string m_vu0Call;    // the first live VU0 lane unlike the original's at a call
        std::string m_calleeRead; // the first byte a callee reads unlike the original's at a call
        uint64_t m_calleeMasked = 0; // calls whose callee read such bytes and did nothing else with them
        uint64_t m_left[ee::kIntArgRegs] = {};
        float m_leftF[ee::kFloatArgRegs] = {};
        Held m_from[ee::kIntArgRegs + ee::kFloatArgRegs] = {};
        bool m_lastDirect = false;
        // ee::leftByCall values read after a direct call: (argument register, value)
        std::vector<std::pair<int, uint32_t>> m_stale;
        std::string m_wide; // the first call argument whose upper half differs (a note)
        std::string m_upper; // ... one the callee reads (--upper)
        std::string m_direct; // the first args_left of a direct call (a note)
        std::string m_saves, m_held, m_calleeSp;
        bool m_checkReads = false;
        bool m_reachedStop = false; // the record's stop call made (kStopCall)
        uint64_t m_cycles = 0;
    };
    Script g_script;
    size_t scriptNextIndex() { return g_script.nextIndex(); }

    // A readable replay's safe point (ee/markers.h): interrupted, the frame
    // goes on as resumed by the scheduler's dispatch (8, as the original's).
    bool readableSafePoint(uint32_t cycles)
    {
        if (!safePointNext(cycles))
            return false;
        g_script.cycles(EeScheduler::kGuestDispatchCycles);
        return true;
    }

    // I/O registers: the recording's accesses, in order (Record::io): loads
    // answered with the recorded value, stores compared with it.
    class IoScript final : public fncheck::IoPort
    {
    public:
        void reset(const Record &r)
        {
            m_record = &r;
            m_next = 0;
            m_problem.clear();
        }
        void load(uint32_t address, uint32_t size, uint64_t &lo, uint64_t &hi) override
        {
            lo = hi = 0;
            if (const fncheck::IoAccess *a = next(address, size, false))
            {
                lo = a->lo;
                hi = a->hi;
            }
        }
        void store(uint32_t address, uint32_t size, uint64_t lo, uint64_t hi) override
        {
            const fncheck::IoAccess *a = next(address, size, true);
            if (a && (a->lo != lo || a->hi != hi))
                fail("I/O access " + std::to_string(m_next - 1) + ": stores " + hex(lo) + " to " + hex(address) +
                     ", recorded " + hex(a->lo));
        }
        void finish()
        {
            if (m_next < m_record->io.size())
                fail("made " + std::to_string(m_next) + " I/O accesses, recorded " + std::to_string(m_record->io.size()) +
                     " (next: " + std::string(m_record->io[m_next].store ? "a store to " : "a load of ") +
                     hex(m_record->io[m_next].address) + ")");
        }
        const std::string &problem() const { return m_problem; }

    private:
        const fncheck::IoAccess *next(uint32_t address, uint32_t size, bool store)
        {
            const size_t index = m_next++;
            const auto what = [&](bool st, uint32_t a, uint32_t n)
            { return std::string(st ? "a store to " : "a load of ") + hex(a) + " (" + std::to_string(n) + " bytes)"; };
            if (index >= m_record->io.size())
            {
                fail("I/O access " + std::to_string(index) + ", " + what(store, address, size) +
                     ": not in the recording (" + std::to_string(m_record->io.size()) + " recorded)");
                return nullptr;
            }
            const fncheck::IoAccess &a = m_record->io[index];
            if (a.address != address || a.size != size || (a.store != 0) != store)
            {
                fail("I/O access " + std::to_string(index) + " is " + what(store, address, size) + ", recorded " +
                     what(a.store != 0, a.address, a.size));
                return nullptr;
            }
            return &a;
        }
        void fail(const std::string &why)
        {
            if (m_problem.empty())
                m_problem = why;
        }

        const Record *m_record = nullptr;
        size_t m_next = 0;
        std::string m_problem;
    };
    IoScript g_io;

    // The recompiled original's calls land here (every recorded target is
    // mapped to it): the recording's answer, with the full register state
    // the callee returned with.
    // A call that hands its callee a pointer into the replayed call's own
    // frame (an argument register).
    // The recording's frame reads of a call (Call::reads) where the
    // original's replay holds the bytes with the recorded values: the
    // original's frame as the callee found it in the game. (A byte the
    // replay does not hold, or holds otherwise, is not the original's work
    // in this call: a leftover of an earlier one.)
    void recordedReads(CalleeReads &out, const Call &call)
    {
        out.recordedKeys.clear();
        out.recordedValues.clear();
        for (const Line &l : call.reads)
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t k = l.address + i;
                if ((l.mask & (1u << i)) && accessible(k, 1u) && g_checker.heldKey(k) && g_ram.base()[k] == l.bytes[i])
                {
                    out.recordedKeys.push_back(k);
                    out.recordedValues.push_back(l.bytes[i]);
                }
            }
        g_probeStats.frameReadCalls += call.reads.empty() ? 0u : 1u;
        g_probeStats.frameReadBytes += out.recordedKeys.size();
    }

    bool passesFramePointer(const R5900Context &ctx)
    {
        for (int i = 0; i < ee::kIntArgRegs; ++i)
            if (g_checker.inOwnFrame(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::a0 + i))))
                return true;
        return false;
    }

    void replayStub(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        // A callee probe never reaches the recording's answers (they are the
        // replayed function's): one that would, stops there.
        if (g_probing)
        {
            if (showProbes())
                std::fprintf(stderr, "[fncheck:probe]   a probe reached the replay's call answer at 0x%x\n", ctx->pc);
            throw ProbeStop{kProbeCall};
        }
        ee::CallRegs regs;
        regs.target = ctx->pc;
        regs.intCount = ee::kIntArgRegs;
        regs.floatCount = ee::kFloatArgRegs;
        for (int i = 0; i < ee::kIntArgRegs; ++i)
            regs.a[i] = ee::gpr64(ctx, ee::reg::a0 + i);
        for (int i = 0; i < ee::kFloatArgRegs; ++i)
            regs.f[i] = ctx->f[ee::reg::f12 + i];
        const uint32_t ra = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        // The original's call of a callee that reads stack arguments: what it
        // left there (not for a tail call: those arguments are its caller's).
        const size_t index = g_script.nextIndex();
        const Call *next = g_script.nextCall();
        if (const fncheck::StackArgs *args = stackArgsOf(regs.target);
            args && next && !(next->flags & fncheck::kTailCall))
        {
            if (g_stackSnapshots.size() <= index)
                g_stackSnapshots.resize(index + 1);
            StackSnapshot &snap = g_stackSnapshots[index];
            snap.taken = true;
            snap.low = args->low;
            snap.bytes.assign(args->high - args->low, 0u);
            snap.known.assign(args->high - args->low, 0u);
            const uint32_t sp = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp));
            for (uint32_t i = 0; i < args->high - args->low; ++i)
            {
                const uint32_t a = fncheck::memoryKey(sp + args->low + i);
                if (accessible(a, 1u) && g_checker.known(a))
                {
                    snap.bytes[i] = g_ram.base()[a];
                    snap.known[i] = 1u;
                }
            }
        }
        const size_t callIndex = g_script.nextIndex();
        if (callIndex >= g_vu0AtCalls.size())
            g_vu0AtCalls.resize(callIndex + 1);
        Vu0AtCall &vu0 = g_vu0AtCalls[callIndex];
        vu0.taken = true;
        for (int i = 0; i < 32; ++i)
            std::memcpy(vu0.lanes[i], &ctx->vu0_vf[i], sizeof(vu0.lanes[i]));
        vu0.cop0Status = ctx->cop0_status;
        // What the callee reads at this call, for a function with a readable
        // version to compare (not in a --safepoints pass: its interrupted
        // replays are the original's again).
        if (g_calleeReadsOn && !g_sp.on && !g_frameReadsOnly && g_function && g_function->readable && next &&
            next->target == regs.target)
        {
            probeCallee(callIndex, *ctx, runtime);
            recordedReads(g_calleeReads[callIndex], *next);
        }
        // The index's pass: a call handed a pointer into the frame, for the
        // caller's leftovers its callee reads there (Checker::calleeLeftover).
        else if (g_frameReadsOnly && !g_perturbing && g_checker.keepingLeftovers() && next && next->target == regs.target &&
                 passesFramePointer(*ctx))
            probeCallee(callIndex, *ctx, runtime);
        if (const Call *c = g_script.answer(regs))
        {
            // The record's stop: ctx stays as at the call (the recording's exit).
            if (c->flags & fncheck::kStopCall)
                throw StopReached{};
            *ctx = c->after;
        }
        ctx->pc = ra;
    }

    enum class Who
    {
        Original,
        Readable,
    };

    struct Outcome
    {
        bool ok = true;
        std::string why;
        // The checks added in round `coverage` (f0 of an integer function,
        // callee stack arguments), kept apart so that the earlier verdicts
        // stay readable from the counts (new_bad).
        bool newOk = true;
        std::string newWhy;
        // A callee-read argument register the readable call left out, with
        // another value than the original passed (Script::compareArgsLeft).
        std::string argsLeft;
        std::string handedLeft; // the same for a register the callee only hands on (a note)
        // A register the function relies on finding in the context as its
        // caller left it (Script::compareArgsLeft): a failure if a game::call
        // names the function, else a note.
        std::string entryLeft;
        std::string staleLeft; // a direct callee's leftover a callee reads (a note)
        std::string vu0Call;   // a lane the callee reads unlike the original's at a call
        std::string calleeRead; // a byte the callee reads unlike the original's at a call (a failure)
        uint64_t calleeMasked = 0; // calls whose callee read such bytes and did nothing else with them (a note)
        // A register a caller reads after the call (ResultReader), other than
        // the result, that the original sets and the readable leaves with
        // another value (regs_left; a note).
        std::string regsLeft;
        std::string upper; // what --upper names, differing
        // A 64-bit value handled as 32: a call argument, or a 32-bit result,
        // whose low word is the original's and whose upper half is not (the
        // original's is not the sign extension of its low word; wide; a note).
        std::string wide;
        std::string directLeft; // args_left of a game::call (a note)
        // A callee-saved register at a call that the model gives the callee
        // with another value than the original held (Script::compareSaves):
        // saves, one the callee's prologue saves; held, any other.
        std::string saves, held;
        std::string calleeSp; // a readable callee's sp unlike the original's call (compareCalleeSp; information)
        // VU0 left unlike the original by a function not registered _VU0 (a
        // note): the first difference, and the lanes that differ (bit 0 vf0's
        // x .. bit 3 its w, per register; q: Q).
        std::string vu0Left;
        uint32_t vu0Lanes[32] = {};
        bool vu0Q = false;
        std::string vu0Live; // the first of those a later function reads (Vu0Lanes::atReturn)
        bool deadV0Differs = false;
        bool deadF0Differs = false;
        uint64_t cycles = 0; // charged by the version replayed
        // What it leaves in its own frame unlike the original (frameResidue):
        // the bytes, the first, and the first a later function reads (a failure).
        uint64_t residueBytes = 0;
        std::string residue, residueRead;
        bool residueReadOfValue = false; // ... of a value, not zeros alone
        std::string residueMasked;       // read only by calls that do not act on it (a note)
    };

    // The recorder goes on with a recorded call at the return points of the
    // function's calls of itself (fncheck.cpp selfCallReturn: it reads the
    // jal in the game's code, which a replay does not have). A recording
    // holds only calls that went on at such points, so a replay that leaves
    // the function at an address inside it goes on there too; anything else
    // shows as a difference.
    constexpr uint32_t kMaxResumes = fncheck::kMaxSelfCallResumes;
    bool mayBeSelfCallReturn(const Function &fn, uint32_t pc)
    {
        return pc >= fn.address + 8u && pc - fn.address < fncheck::kMaxFunctionSpan && (pc & 3u) == 0u;
    }

    std::set<uint32_t> pagesOf(const Record &r)
    {
        std::set<uint32_t> pages;
        // (lines come in address order: most share the last one's page)
        uint32_t last = UINT32_MAX;
        auto add = [&](uint32_t address)
        {
            const uint32_t page = g_ram.pageOf(address);
            if (page != last)
                pages.insert(last = page);
        };
        for (const Line &l : r.inputs)
            add(l.address);
        for (const Line &l : r.outputs)
            add(l.address);
        for (const Call &c : r.calls)
            for (const Line &l : c.effects)
                add(l.address);
        const uint32_t sp = static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::sp));
        for (uint32_t a = r.deadLow & ~(g_ram.pageSize() - 1u); a < sp; a += g_ram.pageSize())
            add(a);
        return pages;
    }

    bool sameGpr(const R5900Context &a, const R5900Context &b, int r)
    {
        return std::memcmp(&a.r[r], &b.r[r], sizeof(a.r[r])) == 0;
    }

    // g_saveSites as `callee@site:s0+s1 ...` (held: the high word's), at
    // most 64 sites.
    std::string saveSites(bool held)
    {
        static const char *const names[] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};
        std::string out;
        size_t n = 0, more = 0;
        for (const auto &[key, masks] : g_saveSites)
        {
            const uint32_t mask = held ? static_cast<uint32_t>(masks >> 32) : static_cast<uint32_t>(masks);
            if (!mask)
                continue;
            if (n++ >= 64)
            {
                ++more;
                continue;
            }
            char b[40];
            std::snprintf(b, sizeof(b), "%s0x%x@0x%x:", out.empty() ? "" : " ", key.first, key.second);
            out += b;
            bool firstReg = true;
            for (int slot = 0; slot < ee::kSavedSlots; ++slot)
            {
                if (!(mask & (1u << slot)))
                    continue;
                const int r = ee::savedSlotReg(slot);
                out += firstReg ? "" : "+";
                firstReg = false;
                if (r >= 16 && r <= 23)
                    out += names[r - 16];
                else if (r == 30)
                    out += "fp";
                else if (r == ee::reg::ra)
                    out += "ra";
                else if (r >= 32)
                    out += "f" + std::to_string(r - 32);
                else
                    out += "r" + std::to_string(r);
            }
        }
        if (more)
            out += " +" + std::to_string(more) + " more";
        return out;
    }

    // ---- what a function leaves in its own frame -----------------------------
    //
    // The original's working values in its own stack frame stay there after it
    // returns, and a later function that reads a local it never set reads them:
    // closestPointOnTriangle's edges (frame 0x00 .. 0x4F) came back as the w of
    // BMX XXX's camera matrix row through setCameraMatrix (tools22), DM2's
    // closestPointOnSegment, waterDrawCheck and drawLensFlare's vectors as heap
    // words (held3). The RAM compare at the return leaves the frame out (dead
    // stack), so this compares it on its own:
    //   - after the original's replay: its frame at the return, [deadLow, entry
    //     sp): what its code or its callees' recorded effects wrote there, and
    //     elsewhere what it held at entry (the recording's bytes) or nothing
    //     known (the fill);
    //   - after each readable replay, the same bytes: one that differs is
    //     residue the readable version leaves unlike the original
    //     (frame_residue_differs, a note: most of it is never read);
    //   - --frame-reads-out DIR: each original replay's leftover reads, the
    //     bytes of its own frame it read before the call wrote them, with the
    //     values they held (an earlier function's residue), one line per call in
    //     DIR/<corpus>__<folder>.<pid>.reads (the folder's index);
    //   - --frame-reads-in DIR: that index. A differing byte the original wrote
    //     or held, that the folder's first leftover read of it after the call
    //     (by vsync, then the EE cycle at entry, fncheck::kEntryCount) reads with
    //     the original's value, is residue a later function reads
    //     (frame_residue_read, a failure).
    // The reader is only seen where it is recorded, and only in its recorded
    // calls.
    struct FrameAtReturn
    {
        bool taken = false;
        uint32_t deadLow = 0, low = 0, sp = 0; // [low, sp) known in part; [deadLow, low) not at all
        std::vector<uint8_t> bytes;            // the original's, from low
        std::vector<uint8_t> kind;             // ResidueKind of each
    };
    enum ResidueKind : uint8_t
    {
        kNotKnown = 0, // neither wrote nor read in the call: the entry's leftover, not recorded
        kWritten = 1,  // the original's code or a callee's recorded effect wrote it in the call
        kEntry = 2,    // the recording's (the line was touched): the entry's leftover, known
    };
    FrameAtReturn g_frameAtReturn;

    struct ResidueByte
    {
        uint32_t address = 0;
        uint8_t original = 0, readable = 0;
        ResidueKind kind = kNotKnown;
    };

    void takeFrameAtReturn(const Record &r)
    {
        FrameAtReturn &f = g_frameAtReturn;
        f.taken = true;
        f.deadLow = r.deadLow;
        f.sp = static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::sp));
        f.low = std::max(f.deadLow, g_checker.lowestKnownLine(f.deadLow, f.sp));
        const uint8_t *ram = g_ram.base();
        f.bytes.assign(ram + f.low, ram + f.sp);
        f.kind.assign(f.sp - f.low, kNotKnown);
        for (uint32_t line = f.low & ~15u; line < f.sp; line += 16u)
        {
            const uint16_t valid = g_checker.validMask(line), wrote = g_checker.wroteMask(line);
            if (!valid && !wrote)
                continue;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t a = line + i;
                if (a < f.low || a >= f.sp)
                    continue;
                if (wrote & (1u << i))
                    f.kind[a - f.low] = kWritten;
                else if (valid & (1u << i))
                    f.kind[a - f.low] = kEntry;
            }
        }
    }

    // The readable replay's frame against the original's at the return (its
    // fill where nothing is known).
    std::vector<ResidueByte> frameResidue(uint8_t fill)
    {
        std::vector<ResidueByte> out;
        const FrameAtReturn &f = g_frameAtReturn;
        if (!f.taken)
            return out;
        const uint8_t *ram = g_ram.base();
        uint64_t pattern;
        std::memset(&pattern, fill, sizeof(pattern));
        uint32_t a = f.deadLow;
        for (; a < f.low; ++a)
        {
            if ((a & 7u) == 0u && a + 8u <= f.low)
            {
                uint64_t w;
                std::memcpy(&w, ram + a, 8);
                if (w == pattern)
                {
                    a += 7u;
                    continue;
                }
            }
            if (ram[a] != fill)
                out.push_back({a, fill, ram[a], kNotKnown});
        }
        for (; a < f.sp; ++a)
        {
            const ResidueKind kind = static_cast<ResidueKind>(f.kind[a - f.low]);
            const uint8_t want = kind == kNotKnown ? fill : f.bytes[a - f.low];
            if (ram[a] != want)
                out.push_back({a, want, ram[a], kind});
        }
        return out;
    }

    // ---- the folder's leftover reads (--frame-reads-out, --frame-reads-in) ----

    // <corpus>__<folder>: a recording folder's name in the index.
    std::string frameIndexName(const std::filesystem::path &folder)
    {
        const std::filesystem::path full = std::filesystem::absolute(folder);
        return full.parent_path().filename().string() + "__" + full.filename().string();
    }

    // A call's leftover reads into the folder's index: one line each for its
    // own (`a`: changed, they change what its caller sees, the replay with them
    // changed in checkFile;
    // `m`: they do not) and its callees' through a pointer into its frame
    // (`c`). game function sequence vsync count|- sp kind, then address:bytes.
    void writeLeftoverReads(const Function &fn, const Record &r, const std::vector<std::pair<uint32_t, uint8_t>> &own,
                            bool acts, const std::vector<std::pair<uint32_t, uint8_t>> &callees)
    {
        static std::ofstream s_out;
        static std::string s_name;
        if (own.empty() && callees.empty())
            return;
        const std::string name = frameIndexName(g_folder);
        if (name != s_name || !s_out.is_open())
        {
            if (s_out.is_open())
                s_out.close();
#if defined(_WIN32)
            const unsigned long pid = GetCurrentProcessId();
#else
            const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
            std::error_code ec;
            std::filesystem::create_directories(g_frameReadsOut, ec);
            s_out.open(g_frameReadsOut / (name + "." + std::to_string(pid) + ".reads"), std::ios::app);
            s_name = name;
        }
        char head[160];
        std::snprintf(head, sizeof(head), "%s %08x %" PRIu64 " %" PRIu64 " ", game::gameName(g_game), fn.address,
                      r.sequence, r.vsync);
        std::string prefix = head;
        if (r.flags & fncheck::kEntryCount)
            prefix += std::to_string(r.entry.cop0_count);
        else
            prefix += "-";
        char b[16];
        std::snprintf(b, sizeof(b), " %08x", static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::sp)));
        prefix += b;
        auto put = [&](const std::vector<std::pair<uint32_t, uint8_t>> &reads, const char *kind)
        {
            if (reads.empty())
                return;
            std::vector<std::pair<uint32_t, uint8_t>> sorted(reads);
            std::sort(sorted.begin(), sorted.end());
            std::string line = prefix + " " + kind;
            for (size_t i = 0; i < sorted.size(); ++i)
            {
                if (i == 0 || sorted[i].first != sorted[i - 1].first + 1u)
                {
                    std::snprintf(b, sizeof(b), " %08x:", sorted[i].first);
                    line += b;
                }
                std::snprintf(b, sizeof(b), "%02x", sorted[i].second);
                line += b;
            }
            s_out << line << '\n';
        };
        put(own, acts ? "a" : "m");
        put(callees, "c");
        s_out.flush();
    }

    // The original replayed again with every leftover of its frame changed
    // (each byte of the recording below its sp at entry turned over): a run
    // that leaves its caller what the recording says (RAM outside its frame,
    // v0, v1, f0, the registers it keeps, its calls and their arguments) does
    // not act on its leftovers; one that does not, or goes off the recording
    // (a byte it cannot reach, its cycle budget), does.
    struct PerturbStop
    {
    };
    class PerturbTracer final : public fncheck::Tracer
    {
    public:
        void begin() { m_cycles = 0; }
        void read(uint32_t address, uint32_t size) override
        {
            if (!accessible(fncheck::memoryKey(address), size))
                throw PerturbStop{};
        }
        void write(uint32_t address, uint32_t size) override
        {
            if (!accessible(fncheck::memoryKey(address), size))
                throw PerturbStop{};
        }
        void special(uint32_t) override {}
        void beforeCall(uint8_t *, R5900Context *, uint32_t) override {}
        void afterCall(uint8_t *, R5900Context *, bool) override {}
        void cycles(uint32_t count) override
        {
            m_cycles += count;
            if (m_cycles > g_cycleBudget)
                throw PerturbStop{};
        }

    private:
        uint64_t m_cycles = 0;
    };
    PerturbTracer g_perturbTracer;

    void changeLeftovers(const Record &r)
    {
        const uint32_t sp = static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::sp));
        uint8_t *ram = g_ram.base();
        for (const Line &l : r.inputs)
            for (uint32_t i = 0; i < 16u; ++i)
                if ((l.mask & (1u << i)) && l.address + i >= r.deadLow && l.address + i < sp)
                    ram[l.address + i] = static_cast<uint8_t>(~ram[l.address + i]);
    }

    // One leftover read of the index: a byte a recorded call read in its own
    // frame before writing it, and the value it found.
    struct LeftoverRead
    {
        uint64_t vsync = 0;
        uint64_t sequence = 0;
        uint32_t count = 0;
        uint32_t function = 0;
        bool timed = false;
        // The call acts on its leftovers (its own: it changes what its caller
        // sees with them changed; a callee's through a pointer: taken to).
        bool acts = true;
        bool callee = false;
        uint8_t value = 0;
    };
    struct FrameIndex
    {
        std::string name;
        bool loaded = false;
        uint64_t reads = 0;
        std::unordered_map<uint32_t, std::vector<LeftoverRead>> at; // by address, in time order
    };
    FrameIndex g_frameIndex;

    bool readBefore(const LeftoverRead &x, const LeftoverRead &y)
    {
        if (x.vsync != y.vsync)
            return x.vsync < y.vsync;
        return x.count < y.count;
    }

    // The index of the folder of the file being checked (once per folder).
    void loadFrameIndex()
    {
        const std::string name = frameIndexName(g_folder);
        if (g_frameIndex.loaded && g_frameIndex.name == name)
            return;
        g_frameIndex = FrameIndex{};
        g_frameIndex.name = name;
        g_frameIndex.loaded = true;
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(g_frameReadsIn, ec))
        {
            const std::string file = e.path().filename().string();
            if (file.size() < name.size() + 7u || file.compare(0, name.size() + 1u, name + ".") != 0 ||
                e.path().extension() != ".reads")
                continue;
            std::ifstream in(e.path());
            std::string text;
            while (std::getline(in, text))
            {
                char gameName[16], count[24], kind[4];
                unsigned function = 0, sp = 0;
                unsigned long long sequence = 0, vsync = 0;
                int used = 0;
                if (std::sscanf(text.c_str(), "%15s %x %llu %llu %23s %x %3s%n", gameName, &function, &sequence, &vsync, count,
                                &sp, kind, &used) < 7)
                    continue;
                LeftoverRead read;
                read.acts = kind[0] != 'm';
                read.callee = kind[0] == 'c';
                read.function = function;
                read.sequence = sequence;
                read.vsync = vsync;
                read.timed = count[0] != '-';
                read.count = read.timed ? static_cast<uint32_t>(std::strtoul(count, nullptr, 10)) : 0u;
                const char *p = text.c_str() + used;
                while (*p == ' ')
                {
                    char *end = nullptr;
                    uint32_t address = static_cast<uint32_t>(std::strtoul(p + 1, &end, 16));
                    if (!end || *end != ':')
                        break;
                    p = end + 1;
                    while (std::isxdigit(static_cast<unsigned char>(p[0])) && std::isxdigit(static_cast<unsigned char>(p[1])))
                    {
                        const char pair[3] = {p[0], p[1], 0};
                        read.value = static_cast<uint8_t>(std::strtoul(pair, nullptr, 16));
                        g_frameIndex.at[address++].push_back(read);
                        ++g_frameIndex.reads;
                        p += 2;
                    }
                }
            }
        }
        for (auto &[address, reads] : g_frameIndex.at)
            std::stable_sort(reads.begin(), reads.end(), readBefore);
    }

    // The folder's first leftover read of `address` after the call `r` of the
    // function being checked began, or null. With `original`: past reads of that
    // value by calls that do not act on it (a read leaves the byte as it was),
    // to the first that acts on it or finds another value; else the first that
    // does not act.
    const LeftoverRead *firstReadAfter(const Record &r, uint32_t address, const uint8_t *original = nullptr)
    {
        const auto it = g_frameIndex.at.find(address);
        if (it == g_frameIndex.at.end())
            return nullptr;
        LeftoverRead self;
        self.vsync = r.vsync;
        self.count = (r.flags & fncheck::kEntryCount) ? r.entry.cop0_count : 0u;
        const bool timed = (r.flags & fncheck::kEntryCount) != 0;
        const auto &reads = it->second;
        // (untimed: from the first read of the call's vsync on, the call's own
        // reads left out; their order within the vsync is not known)
        auto at = timed ? std::upper_bound(reads.begin(), reads.end(), self, readBefore)
                        : std::lower_bound(reads.begin(), reads.end(), self,
                                           [](const LeftoverRead &x, const LeftoverRead &y) { return x.vsync < y.vsync; });
        const LeftoverRead *masked = nullptr;
        for (; at != reads.end(); ++at)
        {
            if (at->function == g_function->address && at->sequence == r.sequence)
                continue;
            if (timed && at->timed && at->vsync == r.vsync && at->count == self.count)
                continue;
            if (!original || at->value != *original || at->acts)
                return masked && (!original || at->value != *original) ? masked : &*at;
            if (!masked)
                masked = &*at;
        }
        return masked;
    }

    // The byte and its word (little-endian, as the EE reads it): the
    // readable version's and the original's (its fill where nothing is known).
    std::string describeResidue(const Record &r, const ResidueByte &b, uint8_t fill)
    {
        static const char *const kinds[] = {"the entry's leftover, not recorded", "the original wrote it",
                                            "the entry's leftover"};
        const uint32_t sp = static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::sp));
        const FrameAtReturn &f = g_frameAtReturn;
        const uint32_t word = b.address & ~3u;
        uint32_t readable = 0, original = 0;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const uint32_t a = word + i;
            const uint8_t mine = g_ram.base()[a];
            const uint8_t theirs = a >= f.low && a < f.sp && f.kind[a - f.low] != kNotKnown ? f.bytes[a - f.low] : fill;
            readable |= uint32_t(mine) << (8u * i);
            original |= uint32_t(theirs) << (8u * i);
        }
        char text[240];
        std::snprintf(text, sizeof(text),
                      "call #%" PRIu64 ": EE 0x%08x (sp-0x%x) = 0x%08x, the original's 0x%08x (byte %u: %s)", r.sequence,
                      word, sp - word, readable, original, b.address - word, kinds[b.kind]);
        return text;
    }

    // The readable replay's residue (frameResidue) into its outcome: noted, and
    // a failure where the index shows a later function reading it.
    void compareFrameAtReturn(const Record &r, uint8_t fill, Outcome &out)
    {
        const std::vector<ResidueByte> residue = frameResidue(fill);
        out.residueBytes = residue.size();
        if (residue.empty())
            return;
        const ResidueByte *shown = &residue.front();
        for (const ResidueByte &b : residue)
            if (b.kind == kWritten)
            {
                shown = &b;
                break;
            }
        out.residue = describeResidue(r, *shown, fill);
        if (g_frameReadsIn.empty())
            return;
        // A read of the original's value: of a value (some byte of the word the
        // reader read is not 0), or of zeros alone (which another write left as
        // well, as likely: noted apart, still a failure).
        const FrameAtReturn &f = g_frameAtReturn;
        auto originalAt = [&](uint32_t a, uint8_t &value)
        {
            if (a < f.low || a >= f.sp || f.kind[a - f.low] == kNotKnown)
                return false;
            value = f.bytes[a - f.low];
            return true;
        };
        for (const ResidueByte &b : residue)
        {
            if (b.kind == kNotKnown)
                continue;
            const LeftoverRead *read = firstReadAfter(r, b.address, &b.original);
            if (!read || read->value != b.original)
                continue;
            if (!read->acts)
            {
                // (read, and nothing its caller sees changes with it: a note)
                if (out.residueMasked.empty())
                {
                    const Function *reader = game::readable::find(read->function, g_game);
                    char text[160];
                    std::snprintf(text, sizeof(text), "; read by %s 0x%08x call #%" PRIu64 ", which does not act on it",
                                  reader ? reader->name : "?", read->function, read->sequence);
                    out.residueMasked = describeResidue(r, b, fill) + text;
                }
                continue;
            }
            bool ofValue = b.original != 0;
            for (uint32_t a = b.address & ~3u; a < (b.address & ~3u) + 4u && !ofValue; ++a)
            {
                uint8_t original = 0;
                const bool known = a != b.address && originalAt(a, original);
                const LeftoverRead *other = known ? firstReadAfter(r, a, &original) : nullptr;
                ofValue = other && other->function == read->function && other->sequence == read->sequence &&
                          other->value == original && original != 0;
            }
            if (!out.residueRead.empty() && (out.residueReadOfValue || !ofValue))
                continue;
            const Function *reader = game::readable::find(read->function, g_game);
            char text[200];
            std::snprintf(text, sizeof(text), "; read by %s 0x%08x call #%" PRIu64 "%s (vsync %" PRIu64 "%s%s)",
                          reader ? reader->name : "?", read->function, read->sequence,
                          read->callee ? " through its callee" : "", read->vsync,
                          (r.flags & fncheck::kEntryCount) && read->timed ? "" : ", order in the vsync not known",
                          ofValue ? "" : ", zeros");
            out.residueRead = describeResidue(r, b, fill) + text;
            out.residueReadOfValue = ofValue;
            if (ofValue)
                break;
        }
        if (!out.residueRead.empty() && out.newOk)
        {
            out.newOk = false;
            out.newWhy = "leaves its frame unlike the original where a later function reads it: " + out.residueRead;
        }
    }

    Outcome replay(const Function &fn, const Record &r, Who who, uint8_t fill, PS2Runtime &runtime)
    {
        const CallDeadline deadline;
        Outcome out;
        g_replayRuntime = &runtime;
        const std::set<uint32_t> pages = pagesOf(r);
        g_ram.open(pages, fill);
        g_openPages = &pages;
        uint8_t *ram = g_ram.base();
        for (const Line &l : r.inputs)
            writeLine(ram, l);
        if (g_perturbing)
            changeLeftovers(r);
        for (const Call &c : r.calls)
            g_ps2RecompiledFunctionTable[(c.target & PS2_RAM_MASK) >> 2] = &replayStub;

        R5900Context ctx = r.entry;
        ctx.pc = fn.address;
        if (const fncheck::Vu0Memory &vu0 = g_vu0Memory[static_cast<uint32_t>(fn.game)]; vu0.uploaded())
            vu0.restore(runtime.memory());
        setCycleBudget(r);
        // COP0 Count at every replay from the game's at the call (a recording
        // that keeps it, fncheck::kEntryCount), else from 0: a function that
        // reads it (mfc0 Count: readCycleCounter's timer) then sees the same value
        // however many EE cycles the process has charged before, so its replay
        // and first difference are the same from run to run.
        runtime.writeCop0Count((r.flags & fncheck::kEntryCount) ? r.entry.cop0_count : 0u);
        const uint32_t ra = static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::ra));
        g_script.reset(r, who == Who::Original, fn.address);
        g_io.reset(r);
        fncheck::t_ioPort = &g_io;
        volatile bool stopped = false; // (a replay with its leftovers changed that went off the recording)
        if (who == Who::Original && g_perturbing)
        {
            g_phase = "recompiled original (leftovers changed)";
            g_perturbTracer.begin();
#if defined(_WIN32)
            stopped = true; // (no way back from a fault here: counted as acting)
#else
            struct sigaction abortAction
            {
            }, previousAbort{};
            abortAction.sa_handler = &perturbAbort;
            sigemptyset(&abortAction.sa_mask);
            sigaction(SIGABRT, &abortAction, &previousAbort);
            if (sigsetjmp(g_perturbJump, 1) == 0)
            {
                g_perturbArmed = 1;
                try
                {
                    fncheck::t_tracer = &g_perturbTracer;
                    fn.traced(ram, &ctx, &runtime);
                    for (uint32_t resumes = 0; ctx.pc != ra && resumes < kMaxResumes; ++resumes)
                    {
                        if (g_script.nextIsTailCallTo(ctx.pc))
                        {
                            const uint32_t recordedReturn = g_script.nextCall()->after.pc;
                            replayStub(ram, &ctx, &runtime);
                            if (recordedReturn == ra)
                                ctx.pc = ra;
                        }
                        if (ctx.pc == ra || !mayBeSelfCallReturn(fn, ctx.pc))
                            break;
                        fn.traced(ram, &ctx, &runtime);
                    }
                }
                catch (const PerturbStop &)
                {
                    stopped = true;
                }
                catch (const StopReached &)
                {
                }
                g_perturbArmed = 0;
            }
            else
                stopped = true;
            sigaction(SIGABRT, &previousAbort, nullptr);
#endif
            fncheck::t_tracer = nullptr;
        }
        else if (who == Who::Original)
        {
            g_phase = "recompiled original";
            g_checker.reset(r);
            g_checker.keepLeftovers(!g_frameReadsOut.empty() && !g_sp.on);
            g_stackSnapshots.clear();
            g_vu0AtCalls.clear();
            if (!g_sp.on)
            {
                g_calleeReads.clear();
                g_calleeRecordBytes = 0;
                g_recordVsync = r.vsync;
            }
            if (g_sp.interruptAt)
                fncheck::t_blockSink = &g_sliceEmu; // (not counted as coverage)
            else
            {
                g_coverage.beginCall();
                fncheck::t_blockSink = &g_coverage;
                g_originalLeaders.clear();
                g_orderOverflow = false;
            }
            fncheck::t_tracer = &g_checker;
            try
            {
                fn.traced(ram, &ctx, &runtime);
                fncheck::t_tracer = nullptr;
                for (uint32_t resumes = 0; ctx.pc != ra && resumes < kMaxResumes; ++resumes)
                {
                    // It left by a jump: the recording's tail call answers it, and
                    // returns to the jump's ra: the function's own return address,
                    // or for the jump that ends an inner self-call, its return
                    // point.
                    if (g_script.nextIsTailCallTo(ctx.pc))
                    {
                        const uint32_t recordedReturn = g_script.nextCall()->after.pc;
                        replayStub(ram, &ctx, &runtime);
                        // Recordings made before the recorder followed self-calls
                        // ran an inner call's tail call on to the function's own
                        // return: the recorded call ends the function there.
                        if (recordedReturn == ra)
                            ctx.pc = ra;
                    }
                    // An inner self-call returned (generated code runs a call of
                    // the function itself inline, fncheck.cpp selfCallReturn): the
                    // outer call goes on there, as the recorder went on.
                    // --safepoints: interrupted, the scheduler dispatches the pc
                    // it left at (8), through the table (SliceEmu).
                    const bool resume = g_sp.interrupted && !g_sp.resumed && ctx.pc != ra;
                    if (resume)
                    {
                        g_sp.resumed = true;
                        g_checker.add(EeScheduler::kGuestDispatchCycles);
                        g_sliceEmu.resume(ctx.pc);
                    }
                    if (ctx.pc == ra || (!resume && !mayBeSelfCallReturn(fn, ctx.pc)))
                        break;
                    if (!resume)
                        g_sliceEmu.selfReturn(ctx.pc);
                    fncheck::t_tracer = &g_checker;
                    fn.traced(ram, &ctx, &runtime);
                    fncheck::t_tracer = nullptr;
                }
            }
            catch (const StopReached &)
            {
                // (a record that stops at a call: ctx as at that call)
                fncheck::t_tracer = nullptr;
            }
            fncheck::t_blockSink = nullptr;
            g_sliceEmu.stop();
        }
        else
        {
            g_phase = fill == 0xA5u ? "readable version" : "readable version (second fill)";
            g_readableMarks.clear();
            ee::t_ee.callHandler = &g_script;
            ee::Frame *const outerFrame = ee::t_ee.frame;
            try
            {
                fn.readable(ram, &ctx, &runtime);
            }
            catch (const StopReached &)
            {
                // Its stop call made: the frames it was in are left as a throw
                // leaves them (ee::entryDirect puts its outer frame back only
                // at a return).
                ee::t_ee.frame = outerFrame;
                ee::t_ee.tailNext = false;
                ee::t_ee.directArguments = nullptr;
                ee::t_ee.selfCallFrame = false;
            }
            ee::t_ee.callHandler = nullptr;
        }
        fncheck::t_ioPort = nullptr;
        g_script.finish();
        g_io.finish();
        out.cycles = who == Who::Original ? g_checker.charged() : g_script.charged();

        auto fail = [&](const std::string &why)
        {
            if (out.ok)
                out.why = why;
            out.ok = false;
        };
        // A record that stops at a call (fncheck::kStopsAtCall): judged at that
        // call. It must be reached (the calls before it made, as recorded, then
        // it); nothing returns, so the readable version's results and the
        // registers it keeps are not compared, nor what it leaves in its frame.
        const bool stopRecord = (r.flags & fncheck::kStopsAtCall) != 0u;
        if (stopped)
            fail("went off the recording");
        if (!stopRecord && ctx.pc != ra)
            fail("did not return (pc " + hex(ctx.pc) + ")");
        if (!g_script.problem().empty())
            fail(g_script.problem());
        if (!g_io.problem().empty())
            fail(g_io.problem());
        if (stopRecord && !g_script.reachedStop())
            fail(ctx.pc == ra ? "returned before its stop (the recording's call " +
                                    std::to_string(r.calls.empty() ? 0u : r.calls.size() - 1u) + " to " +
                                    hex(r.calls.empty() ? 0u : r.calls.back().target) + ", which never came back)"
                              : "did not reach its stop (pc " + hex(ctx.pc) + ")");
        if (who == Who::Readable)
        {
            out.argsLeft = g_script.argsLeft();
            out.handedLeft = g_script.handedLeft();
            out.entryLeft = g_script.entryLeft();
            out.staleLeft = g_script.staleLeft();
            out.vu0Call = g_script.vu0AtCall();
            out.calleeRead = g_script.calleeRead();
            out.calleeMasked = g_script.calleeMasked();
            out.wide = g_script.wide();
            out.upper = g_script.upper();
            out.directLeft = g_script.directLeft();
            out.saves = g_script.savesDiffer();
            out.calleeSp = g_script.calleeSpDiffers();
            out.held = g_script.heldDiffer();
        }
        if (who == Who::Readable && !g_script.newProblem().empty())
        {
            out.newOk = false;
            out.newWhy = g_script.newProblem();
        }
        if (who == Who::Readable && out.newOk && !out.calleeRead.empty())
        {
            out.newOk = false;
            out.newWhy = "memory a callee reads unlike the original's at the call: " + out.calleeRead;
        }
        if (who == Who::Readable && out.newOk && !out.argsLeft.empty())
        {
            out.newOk = false;
            out.newWhy = out.argsLeft;
        }
        if (who == Who::Readable && out.newOk && !out.entryLeft.empty() && game::readable::calledDirectly(fn.host))
        {
            out.newOk = false;
            out.newWhy = "called directly (game::call), and " + out.entryLeft;
        }
        if (who == Who::Original && !g_perturbing && g_checker.uncovered())
            fail(std::to_string(g_checker.uncovered()) + " bytes read that the recording does not hold (first " +
                 hex(g_checker.firstUncovered()) + ")");

        // Result registers.
        const R5900Context &want = r.exit;
        // Interrupts enabled or not (EI, DI).
        if (ctx.cop0_status != want.cop0_status)
            fail("COP0 status " + hex(ctx.cop0_status) + ", recorded " + hex(want.cop0_status));
        if (who == Who::Original && g_perturbing)
        {
            // What its caller sees: the results and the registers it keeps.
            static constexpr int kSeen[] = {2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
            for (int i : kSeen)
                if (!sameGpr(ctx, want, i))
                    fail("r" + std::to_string(i) + " differs");
            if (std::memcmp(&ctx.f[0], &want.f[0], 4) != 0)
                fail("f0 differs");
        }
        else if (who == Who::Original)
        {
            // The same code from the same state: every register.
            for (int i = 1; i < 32; ++i)
                if (!sameGpr(ctx, want, i))
                    fail("r" + std::to_string(i) + " = " + hex(ee::gpr64(&ctx, i)) + ", recorded " + hex(ee::gpr64(&want, i)));
            for (int i = 0; i < 32; ++i)
                if (std::memcmp(&ctx.f[i], &want.f[i], 4) != 0)
                    fail("f" + std::to_string(i) + " differs");
            if (ctx.hi != want.hi || ctx.lo != want.lo)
                fail("hi/lo differ");
            if (std::memcmp(ctx.vu0_vf, want.vu0_vf, sizeof(ctx.vu0_vf)) != 0)
                fail("VU0 registers differ");
        }
        else if (!stopRecord)
        {
            const uint64_t v0 = ee::gpr64(&ctx, ee::reg::v0), wantV0 = ee::gpr64(&want, ee::reg::v0);
            switch (fn.returnKind)
            {
            case ee::ReturnKind::Void:
                // v0 of a void function some caller reads (the original sets
                // it: a callee's result handed on, or its own): a failure of
                // the coverage round's kind, else a note (dead_v0).
                if (v0 != wantV0 && (resultReads(fn).first & (1u << ee::reg::v0)) &&
                    wantV0 != ee::gpr64(&r.entry, ee::reg::v0))
                {
                    if (out.newOk)
                    {
                        out.newOk = false;
                        out.newWhy = "v0 left = " + hex(v0) + ", the original's " + hex(wantV0) +
                                     ": declared void, a caller reads v0";
                    }
                }
                else
                    out.deadV0Differs = v0 != wantV0;
                break;
            case ee::ReturnKind::Int32:
            case ee::ReturnKind::Pointer:
                if (static_cast<uint32_t>(v0) != static_cast<uint32_t>(wantV0))
                    fail("v0 = " + hex(static_cast<uint32_t>(v0)) + ", recorded " + hex(static_cast<uint32_t>(wantV0)));
                else if (v0 != wantV0 && out.wide.empty() &&
                         wantV0 != static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(wantV0))))
                    out.wide = "v0 = " + hex(v0) + ", recorded " + hex(wantV0) + " (a 32-bit result)";
                break;
            case ee::ReturnKind::Int64:
                if (v0 != wantV0)
                    fail("v0 = " + hex(v0) + ", recorded " + hex(wantV0));
                break;
            case ee::ReturnKind::Float:
                if (std::memcmp(&ctx.f[0], &want.f[0], 4) != 0)
                    fail("f0 = " + std::to_string(ctx.f[0]) + ", recorded " + std::to_string(want.f[0]));
                break;
            }
            // f0 of a function that does not return a float, when the
            // original's f0 at the return is not the one it was called with
            // (its code or a callee's set it): the int_f0 class (round
            // tested14: a draft left f0 stale and its caller read it). A
            // failure for an integer or pointer function some caller reads
            // f0 after (the generated code's call sites, fncheck_build.h);
            // otherwise noted (dead_f0), as v0 of a void function is. (Every
            // integer function: 1,052 functions over the corpus, 168 of them
            // Verified, use f0 as scratch that no caller reads.)
            if (fn.returnKind != ee::ReturnKind::Float && std::memcmp(&want.f[0], &r.entry.f[0], 4) != 0 &&
                std::memcmp(&ctx.f[0], &want.f[0], 4) != 0)
            {
                if (fn.returnKind == ee::ReturnKind::Void || !f0IsRead(fn))
                    out.deadF0Differs = true;
                else if (out.newOk)
                {
                    char b[160];
                    std::snprintf(b, sizeof(b), "f0 left = %g (0x%08x), the original's %g (0x%08x): returns an integer, "
                                  "a caller may read f0", ctx.f[0], floatBits(ctx.f[0]), want.f[0],
                                  floatBits(want.f[0]));
                    out.newOk = false;
                    out.newWhy = b;
                }
            }
            // The other registers callers read after the call (v1, a0-a3,
            // t0-t9, at; f1-f19): noted when the original set one and the
            // readable leaves another value.
            {
                const auto [gprs, fprs] = resultReads(fn);
                for (int i = 1; i < 32 && out.regsLeft.empty(); ++i)
                    if (i != ee::reg::v0 && (gprs & (1u << i)) && ee::gpr64(&want, i) != ee::gpr64(&r.entry, i) &&
                        ee::gpr64(&ctx, i) != ee::gpr64(&want, i))
                        out.regsLeft = "r" + std::to_string(i) + " left = " + hex(ee::gpr64(&ctx, i)) +
                                       ", the original's " + hex(ee::gpr64(&want, i));
                for (int i = 1; i < 20 && out.regsLeft.empty(); ++i)
                    if ((fprs & (1u << i)) && std::memcmp(&want.f[i], &r.entry.f[i], 4) != 0 &&
                        std::memcmp(&ctx.f[i], &want.f[i], 4) != 0)
                        out.regsLeft = "f" + std::to_string(i) + " left = " + std::to_string(ctx.f[i]) +
                                       ", the original's " + std::to_string(want.f[i]);
            }
            // What it leaves in VU0, for a function whose callers read it.
            if (fn.leavesVu0)
            {
                for (int i = 1; i < 32; ++i)
                    if (std::memcmp(&ctx.vu0_vf[i], &want.vu0_vf[i], sizeof(ctx.vu0_vf[i])) != 0)
                        fail("VU0 vf" + std::to_string(i) + " differs");
                if (std::memcmp(&ctx.vu0_q, &want.vu0_q, sizeof(ctx.vu0_q)) != 0)
                    fail("VU0 Q differs");
                if (ctx.vu0_mac_flags != want.vu0_mac_flags)
                    fail("VU0 MAC flags " + hex(ctx.vu0_mac_flags) + ", recorded " + hex(want.vu0_mac_flags));
                if (ctx.vu0_status != want.vu0_status)
                    fail("VU0 status " + hex(ctx.vu0_status) + ", recorded " + hex(want.vu0_status));
            }
            else if (who == Who::Readable)
            {
                const LiveLanes live = liveLanesOf(fn.address);
                for (int i = 1; i < 32; ++i)
                {
                    uint32_t have[4], original[4];
                    std::memcpy(have, &ctx.vu0_vf[i], sizeof(have));
                    std::memcpy(original, &want.vu0_vf[i], sizeof(original));
                    for (int lane = 0; lane < 4; ++lane)
                        if (have[lane] != original[lane])
                        {
                            out.vu0Lanes[i] |= 1u << lane;
                            const std::string text = "vf" + std::to_string(i) + "." + "xyzw"[lane] + " left = " + hex(have[lane]) +
                                                     ", the original's " + hex(original[lane]);
                            if (out.vu0Left.empty())
                                out.vu0Left = text;
                            if (out.vu0Live.empty() && laneIn(live.atReturn, i, lane))
                                out.vu0Live = text;
                        }
                }
                // (--semantic: a later function reads it, so it fails as a
                // wrong result does.)
                if (g_semantic && !out.vu0Live.empty() && out.newOk)
                {
                    out.newOk = false;
                    out.newWhy = "VU0 left unlike the original in a live lane: " + out.vu0Live;
                }
                if (g_semantic && !out.vu0Call.empty() && out.newOk)
                {
                    out.newOk = false;
                    out.newWhy = "VU0 unlike the original in a lane the callee reads: " + out.vu0Call;
                }
                out.vu0Q = std::memcmp(&ctx.vu0_q, &want.vu0_q, sizeof(ctx.vu0_q)) != 0;
                if (out.vu0Q && out.vu0Left.empty())
                    out.vu0Left = "Q left = " + std::to_string(ctx.vu0_q) + ", the original's " + std::to_string(want.vu0_q);
            }
            // --upper: what the listing says later code reads of the registers left.
            if (const auto reads = g_upperReads.find(fn.address); reads != g_upperReads.end() && out.upper.empty())
            {
                for (int i = 1; i < 32 && out.upper.empty(); ++i)
                    if ((reads->second.returns & (1u << i)) && ee::gprHigh64(&want, i) != ee::gprHigh64(&r.entry, i) &&
                        ee::gprHigh64(&ctx, i) != ee::gprHigh64(&want, i))
                        out.upper = "r" + std::to_string(i) + "'s upper half left = " + hex(ee::gprHigh64(&ctx, i)) +
                                    ", the original's " + hex(ee::gprHigh64(&want, i));
                for (int i = 1; i < 32 && out.upper.empty() && !fn.leavesVu0; ++i)
                    if ((reads->second.vu0 & (1u << i)) && std::memcmp(&want.vu0_vf[i], &r.entry.vu0_vf[i], 16) != 0 &&
                        std::memcmp(&ctx.vu0_vf[i], &want.vu0_vf[i], 16) != 0)
                        out.upper = "VU0 vf" + std::to_string(i) + " left differs from the original's, a caller reads it";
            }
            // What callers keep in registers across a call.
            static constexpr int kPreserved[] = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
            for (int i : kPreserved)
                if (ee::gpr64(&ctx, i) != ee::gpr64(&r.entry, i))
                    fail("r" + std::to_string(i) + " not preserved");
        }

        // Memory: the recorded output, and nothing else changed.
        // By line, not maps: a replay walks every line of its pages, and with
        // --safepoints a big recording replays thousands of times (a map's nodes
        // were deskspeed's other half on Windows, whose heap is the slow one).
        // Filled and emptied here, nothing between runs or returns.
        static std::vector<const Line *> outputAt(fncheck::kMemoryKeys >> 4), inputAt(fncheck::kMemoryKeys >> 4);
        for (const Line &l : r.outputs)
            outputAt[l.address >> 4] = &l;
        for (const Line &l : r.inputs)
            inputAt[l.address >> 4] = &l;
        uint64_t wrong = 0;
        uint32_t firstWrong = 0;
        uint8_t got = 0, expected = 0;
        for (uint32_t page : pages)
        {
            const uint32_t begin = page * g_ram.pageSize();
            for (uint32_t line = begin; line < begin + g_ram.pageSize(); line += 16u)
            {
                const Line *const o = outputAt[line >> 4];
                const Line *const in = inputAt[line >> 4];
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    const uint32_t a = line + i;
                    if (inDeadStack(r, a))
                        continue;
                    uint8_t e = fill;
                    if (in && (in->mask & (1u << i)))
                        e = in->bytes[i];
                    if (o && (o->mask & (1u << i)))
                        e = o->bytes[i];
                    if (ram[a] != e && wrong++ == 0)
                    {
                        firstWrong = a;
                        got = ram[a];
                        expected = e;
                    }
                }
            }
        }
        for (const Line &l : r.outputs)
            outputAt[l.address >> 4] = nullptr;
        for (const Line &l : r.inputs)
            inputAt[l.address >> 4] = nullptr;
        if (wrong)
        {
            char b[160];
            std::snprintf(b, sizeof(b), "%" PRIu64 " bytes of RAM differ (first EE 0x%08x: 0x%02x, recorded 0x%02x)",
                          wrong, firstWrong, got, expected);
            fail(b);
        }
        // (A stop record has no return, so no frame left at one.)
        if (!stopRecord && !g_sp.on && who == Who::Original && !g_perturbing)
        {
            if (g_frameResidueOn && !g_frameReadsOnly && fn.readable)
                takeFrameAtReturn(r);
        }
        else if (!stopRecord && !g_sp.on && who == Who::Readable)
            compareFrameAtReturn(r, fill, out);
        g_ram.close(pages);
        g_openPages = nullptr;
        return out;
    }

    // ---- per function ------------------------------------------------------

    struct Options
    {
        std::set<uint64_t> only; // (game << 32) | address
        uint64_t limit = ~0ull;
        int show = 3;
        bool cycles = false; // --cycles: a cycle difference fails the call
        bool semantic = false; // --semantic: live VU0 fails the call; no time, marks or interruption
        bool safepoints = false; // --safepoints [points [calls]]
        bool selfTestStops = false; // --self-test-stops: stop records judged (selfTestStops)
        std::string generate; // --generate fn n
        uint64_t generated = 0;
    };

    // ---- digests (fncheck_build.h) -------------------------------------------

    const char *readableDigest(const Function &fn)
    {
        for (size_t i = 0; i < fncheck::g_readableDigestCount; ++i)
            if (fncheck::g_readableDigests[i].function == fn.address &&
                fncheck::g_readableDigests[i].game == static_cast<uint8_t>(fn.game))
                return fncheck::g_readableDigests[i].digest;
        return "-";
    }

    std::filesystem::path executablePath()
    {
#if defined(_WIN32)
        wchar_t buffer[32768];
        const DWORD n = GetModuleFileNameW(nullptr, buffer, 32768);
        return std::filesystem::path(std::wstring(buffer, n));
#elif defined(__APPLE__)
        char buffer[4096];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0)
            return std::filesystem::path(buffer);
        return {};
#else
        std::error_code ec;
        return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    }

    // This executable's identity: a 64-bit hash of its bytes (words mixed in
    // order, then the length), computed once.
    const std::string &checkerDigest()
    {
        static const std::string digest = []
        {
            std::ifstream in(executablePath(), std::ios::binary);
            if (!in)
                return std::string("-");
            uint64_t h = 0x243F6A8885A308D3ull, length = 0;
            std::vector<char> buffer(1u << 20);
            while (in)
            {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const size_t n = static_cast<size_t>(in.gcount());
                size_t i = 0;
                for (; i + 8 <= n; i += 8)
                {
                    uint64_t w;
                    std::memcpy(&w, buffer.data() + i, 8);
                    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
                    h ^= h >> 29;
                }
                for (; i < n; ++i)
                    h = (h ^ static_cast<uint8_t>(buffer[i])) * 0x100000001B3ull;
                length += n;
            }
            h = (h ^ length) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 32;
            char b[24];
            std::snprintf(b, sizeof(b), "%016" PRIx64, h);
            return std::string(b);
        }();
        return digest;
    }

    // The recording's recorder version and corpus folder: <file version>:<the
    // folder's parent>/<the folder> (6:v5/lc).
    std::string recordingDigest(const std::filesystem::path &path, uint32_t version)
    {
        const std::filesystem::path folder = std::filesystem::absolute(path).parent_path();
        return std::to_string(version) + ":" + folder.parent_path().filename().string() + "/" + folder.filename().string();
    }

    struct SafePointStats
    {
        uint32_t calls = 0, points = 0, differ = 0, countDiffer = 0, originalBad = 0;
        std::string first;
    };

    // One recorded call interrupted at each of its safe points in turn, in
    // both versions: what the interruption adds to the call's cycles (from
    // the uninterrupted replay's) must be the same; and both must pass the
    // same safe points (their counts: a hand-written function's
    // ee::chargeLoop, a missing marker).
    void safePointPass(const Function &fn, const Record &r, PS2Runtime &runtime, SafePointStats &st)
    {
        auto note = [&](const std::string &why)
        {
            if (st.first.empty())
                st.first = "call #" + std::to_string(r.sequence) + ": " + why;
        };
        ++st.calls;
        resetSafePoints(0);
        const Outcome o0 = replay(fn, r, Who::Original, 0xA5u, runtime);
        const uint32_t originalPoints = g_sp.count;
        resetSafePoints(0);
        const Outcome r0 = replay(fn, r, Who::Readable, 0xA5u, runtime);
        const uint32_t readablePoints = g_sp.count;
        if (!o0.ok || !r0.ok)
            return;
        if (originalPoints != readablePoints)
        {
            ++st.countDiffer;
            note("safe points: the original passes " + std::to_string(originalPoints) + ", the readable " +
                 std::to_string(readablePoints));
        }
        const uint32_t n = std::min(std::min(originalPoints, readablePoints), g_sp.maxPoints);
        for (uint32_t k = 1; k <= n; ++k)
        {
            resetSafePoints(k);
            const Outcome o = replay(fn, r, Who::Original, 0xA5u, runtime);
            const uint32_t kindO = g_sp.kind;
            resetSafePoints(k);
            const Outcome a = replay(fn, r, Who::Readable, 0xA5u, runtime);
            const uint32_t kindR = g_sp.kind;
            resetSafePoints(0);
            ++st.points;
            if (!o.ok)
            {
                ++st.originalBad;
                note("point " + std::to_string(k) + ": the interrupted original does not replay: " + o.why);
                continue;
            }
            const int64_t addO = static_cast<int64_t>(o.cycles) - static_cast<int64_t>(o0.cycles);
            const int64_t addR = static_cast<int64_t>(a.cycles) - static_cast<int64_t>(r0.cycles);
            if (!a.ok || kindO != kindR || addO != addR)
            {
                ++st.differ;
                note("point " + std::to_string(k) + " (" + (kindO == 8 ? "a call" : "a branch back") +
                     (kindR != kindO ? std::string(", the readable's ") + (kindR == 8 ? "a call" : "a branch back")
                                     : std::string()) +
                     "): " + (!a.ok ? "the readable fails: " + a.why
                                    : "adds " + std::to_string(addR) + " cycles, the original " + std::to_string(addO)));
            }
        }
    }

    bool checkFile(const std::filesystem::path &path, const Options &options, PS2Runtime &runtime, bool &anyFailure)
    {
        fncheck::RecordingReader reader; // (plain or compressed: ee/fnrec_file.h)
        reader.open(path);
        std::istream &in = reader.stream();
        fncheck::FileHeader header;
        std::string error;
        if (!in || !fncheck::readHeader(in, header, &error))
        {
            std::cerr << path.string() << ": " << (error.empty() ? "cannot read" : error) << std::endl;
            anyFailure = true;
            return false;
        }
        const game::Game game = static_cast<game::Game>(header.game); // (readHeader checked it)
        const std::string gamePrefix = game == game::Game::Ai ? std::string() : std::string(game::gameName(game)) + " ";
        if (!options.only.empty() && !options.only.count((uint64_t(header.game) << 32) | header.address))
            return true;
        Function *fn = game::readable::find(header.address, game);
        if (!fn)
        {
            std::printf("%s0x%08x: no registered function (%s)\n", gamePrefix.c_str(), header.address,
                        path.filename().string().c_str());
            return true;
        }
        // The traced original's jump tables are looked up in its game.
        fncheck::setTracedGame(game);
        g_game = game;
        const fncheck::BlockTable *blocks = fn->traced ? fncheck::findBlocks(fn->address, game) : nullptr;
        g_coverage.reset(blocks);
        g_markedBlocks.clear();
        g_markedLoops.clear();
        g_saveSites.clear();
        g_function = fn;
        g_file = path.string();
        g_folder = path.parent_path();
        if (!g_frameReadsIn.empty())
            loadFrameIndex();
        std::printf("%s0x%08x %s (%s)\n", gamePrefix.c_str(), fn->address, fn->name,
                    game::readable::statusName(fn->status));

        uint64_t cases = 0, special = 0, originalOk = 0, originalBad = 0, readableOk = 0, readableBad = 0,
                 fillDependent = 0, skipped = 0, deadV0 = 0, recordedWithCycles = 0, originalCycles = 0,
                 readableCycles = 0, cyclesDiffer = 0, newBad = 0, deadF0 = 0, argsLeft = 0, regsLeft = 0, wide = 0, upper = 0, directLeft = 0,
                 savesDiffer = 0, heldDiffer = 0, handedLeft = 0, vu0Left = 0, entryLeft = 0, staleLeft = 0, vu0Live = 0,
                 vu0Call = 0, calleeRead = 0, calleeMasked = 0, residueDiffer = 0, residueBytes = 0, residueRead = 0,
                 residueReadValue = 0, residueMasked = 0, calleeSpDiffer = 0, leftoverReads = 0, leftoversActed = 0, leftoversMasked = 0;
        std::string firstCalleeRead, firstResidue, firstResidueRead, firstResidueMasked, firstCalleeSp;
        g_probeStats = ProbeStats{};
        uint32_t vu0Lanes[32] = {};
        bool vu0Q = false;
        std::string firstVu0Left, firstVu0Live, firstVu0Call;
        std::string firstEntryLeft, firstStaleLeft, firstHandedLeft, firstArgsLeft, firstRegsLeft, firstSaves, firstHeld, firstWide, firstDirectLeft, firstUpper;
        uint32_t spCalls = 0, spPathCalls = 0;
        std::unordered_set<std::string> spPaths;
        SafePointStats spStats;
        uint32_t orderBad = 0;
        std::string firstOrder;
        g_orderOn = g_markersMode && blocks && fn->readable;
        g_leaderSet.clear();
        if (g_orderOn)
        {
            const auto [first, last] = safePointMarks(static_cast<uint8_t>(game), fn->address);
            for (const fncheck::SafePointMark *m = first; m != last; ++m)
                if (m->flags & 1u)
                    g_leaderSet.insert(m->address);
        }
        if (options.safepoints && blocks)
        {
            std::vector<uint32_t> leaders;
            const auto [first, last] = safePointMarks(static_cast<uint8_t>(game), fn->address);
            std::vector<uint32_t> slotEnds;
            for (const fncheck::SafePointMark *m = first; m != last; ++m)
                ((m->flags & 1u) ? leaders : slotEnds).push_back(m->address);
            std::sort(leaders.begin(), leaders.end());
            std::sort(slotEnds.begin(), slotEnds.end());
            std::vector<uint32_t> tableJumps;
            for (size_t i = 0; i < fncheck::g_tableJumpCount; ++i)
                if (fncheck::g_tableJumps[i].game == static_cast<uint8_t>(game) &&
                    fncheck::g_tableJumps[i].function == fn->address)
                    tableJumps.push_back(fncheck::g_tableJumps[i].block);
            std::sort(tableJumps.begin(), tableJumps.end());
            g_sliceEmu.begin(blocks, leaders, std::move(slotEnds), fn->address + fncheck::kMaxFunctionSpan, fn->address, std::move(tableJumps));
        }
        int shown = 0;
        uint64_t stops = 0; // records that stop at a call (fncheck::kStopsAtCall)
        std::string firstOriginal, firstReadable; // the first failure of each, for the result line
        Record record;
        while (cases + special < options.limit && fncheck::readRecord(in, record, header.version))
        {
            // I/O accesses without their values (recordings made before round
            // 12) cannot be replayed.
            if ((record.flags & fncheck::kSpecialAccess) && !(record.flags & fncheck::kIoLog))
            {
                ++special;
                continue;
            }
            ++cases;
            const bool stopRecord = (record.flags & fncheck::kStopsAtCall) != 0u;
            stops += stopRecord ? 1u : 0u;
            g_sequence = record.sequence;
            g_frameAtReturn.taken = false;
            bool replayable = true;
            bool newPath = false; // (--safepoints-paths)
            if (fn->traced)
            {
                const Outcome o = replay(*fn, record, Who::Original, 0xA5u, runtime);
                g_coverage.endCall(o.ok);
                if (!g_frameReadsOut.empty() && o.ok && !stopRecord) // (a stop record leaves no frame at a return)
                {
                    // The index's pass: its leftover reads, and whether it acts
                    // on its own (replayed again with them changed).
                    const std::vector<std::pair<uint32_t, uint8_t>> own = g_checker.leftovers(),
                                                                    callees = g_checker.calleeLeftovers();
                    leftoverReads += own.size() + callees.size();
                    bool acts = false;
                    if (!own.empty())
                    {
                        g_perturbing = true;
                        acts = !replay(*fn, record, Who::Original, 0xA5u, runtime).ok;
                        g_perturbing = false;
                        leftoversActed += acts ? 1u : 0u;
                        leftoversMasked += acts ? 0u : 1u;
                    }
                    writeLeftoverReads(*fn, record, own, acts, callees);
                }
                if (g_sp.maxPathCalls && g_coverage.table())
                {
                    std::vector<uint32_t> run(g_coverage.lastCall());
                    std::sort(run.begin(), run.end());
                    newPath = spPaths.insert(std::string(reinterpret_cast<const char *>(run.data()), run.size() * sizeof(uint32_t))).second;
                }
                if (g_callBlocks && g_coverage.table())
                {
                    // --call-blocks: the blocks this call of the original ran
                    // (calls that ran no new block repeat an earlier one's path).
                    std::vector<uint32_t> run(g_coverage.lastCall());
                    std::sort(run.begin(), run.end());
                    std::string list;
                    for (uint32_t b : run)
                    {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%s%x", list.empty() ? "" : ",", g_coverage.table()->starts[b]);
                        list += text;
                    }
                    std::printf("[fncheck:callblocks] address=0x%08x file=\"%s\" call=%" PRIu64 " ok=%d blocks=%s\n",
                                fn->address, path.string().c_str(), record.sequence, o.ok ? 1 : 0,
                                list.empty() ? "-" : list.c_str());
                }
                if (o.ok)
                    ++originalOk;
                else
                {
                    ++originalBad;
                    replayable = false;
                    if (firstOriginal.empty())
                        firstOriginal = o.why;
                    if (shown++ < options.show)
                        std::printf("    call #%" PRIu64 " (vsync %" PRIu64 "): the original does not replay: %s\n",
                                    record.sequence, record.vsync, o.why.c_str());
                }
            }
            if (!fn->readable || g_frameReadsOnly)
                continue;
            if (!replayable)
            {
                ++skipped;
                continue;
            }
            Outcome a = replay(*fn, record, Who::Readable, 0xA5u, runtime);
            if (g_orderOn && a.ok && fn->traced && !g_orderOverflow)
            {
                const std::unordered_set<uint32_t> marked(g_readableMarks.begin(), g_readableMarks.end());
                std::vector<uint32_t> want;
                for (uint32_t l : g_originalLeaders)
                    if (marked.count(l))
                        want.push_back(l);
                if (want != g_readableMarks)
                {
                    ++orderBad;
                    if (firstOrder.empty())
                    {
                        size_t i = 0;
                        while (i < want.size() && i < g_readableMarks.size() && want[i] == g_readableMarks[i])
                            ++i;
                        firstOrder = "call #" + std::to_string(record.sequence) + ": marker " + std::to_string(i) +
                                     " is " + (i < g_readableMarks.size() ? hex(g_readableMarks[i]) : std::string("none")) +
                                     ", the original reaches " + (i < want.size() ? hex(want[i]) : std::string("none"));
                    }
                }
            }
            if (header.version >= 3)
            {
                ++recordedWithCycles;
                originalCycles += record.cycles;
                readableCycles += a.cycles;
                if (a.cycles != record.cycles)
                {
                    ++cyclesDiffer;
                    if (options.cycles && a.ok)
                    {
                        a.ok = false;
                        a.why = "charges " + std::to_string(a.cycles) + " EE cycles, the original " +
                                std::to_string(record.cycles);
                    }
                }
            }
            Outcome b;
            if (a.ok)
                b = replay(*fn, record, Who::Readable, 0x5Au, runtime);
            // (A call past the first few replays only if it goes another way: a difference at an
            // interruption sits on a path, and a file's first calls are not its rare ones.)
            const bool spFirst = spCalls < g_sp.maxCalls, spPath = newPath && spPathCalls < g_sp.maxPathCalls;
            if (options.safepoints && a.ok && fn->traced && !stopRecord && (spFirst || spPath))
            {
                ++(spFirst ? spCalls : spPathCalls);
                safePointPass(*fn, record, runtime, spStats);
            }
            deadV0 += a.deadV0Differs ? 1u : 0u;
            deadF0 += a.deadF0Differs ? 1u : 0u;
            if (!a.argsLeft.empty())
            {
                ++argsLeft;
                if (firstArgsLeft.empty())
                    firstArgsLeft = a.argsLeft;
            }
            if (!a.entryLeft.empty())
            {
                ++entryLeft;
                if (firstEntryLeft.empty())
                    firstEntryLeft = a.entryLeft;
            }
            if (!a.staleLeft.empty())
            {
                ++staleLeft;
                if (firstStaleLeft.empty())
                    firstStaleLeft = a.staleLeft;
            }
            if (!a.upper.empty())
            {
                ++upper;
                if (firstUpper.empty())
                    firstUpper = a.upper;
            }
            if (!a.handedLeft.empty())
            {
                ++handedLeft;
                if (firstHandedLeft.empty())
                    firstHandedLeft = a.handedLeft;
            }
            if (!a.regsLeft.empty())
            {
                ++regsLeft;
                if (firstRegsLeft.empty())
                    firstRegsLeft = a.regsLeft;
            }
            if (!a.wide.empty())
            {
                ++wide;
                if (firstWide.empty())
                    firstWide = a.wide;
            }
            if (!a.directLeft.empty())
            {
                ++directLeft;
                if (firstDirectLeft.empty())
                    firstDirectLeft = a.directLeft;
            }
            if (!a.vu0Left.empty())
            {
                ++vu0Left;
                if (firstVu0Left.empty())
                    firstVu0Left = a.vu0Left;
                for (int i = 0; i < 32; ++i)
                    vu0Lanes[i] |= a.vu0Lanes[i];
                vu0Q = vu0Q || a.vu0Q;
            }
            if (!a.vu0Live.empty())
            {
                ++vu0Live;
                if (firstVu0Live.empty())
                    firstVu0Live = a.vu0Live;
            }
            if (!a.vu0Call.empty())
            {
                ++vu0Call;
                if (firstVu0Call.empty())
                    firstVu0Call = a.vu0Call;
            }
            calleeMasked += a.calleeMasked;
            if (!a.calleeRead.empty() || (a.ok && !b.calleeRead.empty()))
            {
                ++calleeRead;
                if (firstCalleeRead.empty())
                    firstCalleeRead = !a.calleeRead.empty() ? a.calleeRead : "with another fill: " + b.calleeRead;
            }
            if (a.residueBytes || (a.ok && b.residueBytes))
            {
                ++residueDiffer;
                residueBytes += a.residueBytes ? a.residueBytes : b.residueBytes;
                if (firstResidue.empty())
                    firstResidue = a.residueBytes ? a.residue : "with another fill: " + b.residue;
            }
            if (a.residueRead.empty() && !(a.ok && !b.residueRead.empty()) &&
                (!a.residueMasked.empty() || (a.ok && !b.residueMasked.empty())))
            {
                ++residueMasked;
                if (firstResidueMasked.empty())
                    firstResidueMasked = !a.residueMasked.empty() ? a.residueMasked : "with another fill: " + b.residueMasked;
            }
            if (!a.residueRead.empty() || (a.ok && !b.residueRead.empty()))
            {
                const bool ofValue = a.residueReadOfValue || (a.ok && b.residueReadOfValue);
                ++residueRead;
                residueReadValue += ofValue ? 1u : 0u;
                // (the first read of a value, else the first of zeros)
                if (firstResidueRead.empty() || (ofValue && residueReadValue == 1u))
                    firstResidueRead = !a.residueRead.empty() && (a.residueReadOfValue || !ofValue)
                                           ? a.residueRead
                                           : "with another fill: " + b.residueRead;
            }
            if (!a.calleeSp.empty())
            {
                ++calleeSpDiffer;
                if (firstCalleeSp.empty())
                    firstCalleeSp = a.calleeSp;
            }
            if (!a.saves.empty())
            {
                ++savesDiffer;
                if (firstSaves.empty())
                    firstSaves = a.saves;
            }
            if (!a.held.empty())
            {
                ++heldDiffer;
                if (firstHeld.empty())
                    firstHeld = a.held;
            }
            // The checks added in round `coverage`, on both fills.
            const bool newOk = a.newOk && (!a.ok || b.newOk);
            const std::string newWhy = !a.newOk ? a.newWhy : (a.ok && !b.newOk ? "with another fill: " + b.newWhy : "");
            if (a.ok && b.ok && newOk)
                ++readableOk;
            else if (a.ok && b.ok)
            {
                // Only a new check differs: the earlier checks passed it.
                ++readableBad;
                ++newBad;
                if (firstReadable.empty())
                    firstReadable = newWhy;
                if (shown++ < options.show)
                    std::printf("    call #%" PRIu64 " (vsync %" PRIu64 "): %s\n", record.sequence, record.vsync,
                                newWhy.c_str());
            }
            else
            {
                ++readableBad;
                if (a.ok)
                    ++fillDependent;
                if (firstReadable.empty())
                    firstReadable = a.ok ? "with another fill: " + b.why : a.why;
                if (shown++ < options.show)
                    std::printf("    call #%" PRIu64 " (vsync %" PRIu64 "): %s%s\n", record.sequence, record.vsync,
                                a.ok ? "with another fill: " : "", (a.ok ? b.why : a.why).c_str());
            }
        }
        if (fn->traced)
            std::printf("    original replay: %" PRIu64 " of %" PRIu64 " identical to the recording\n", originalOk, cases);
        else
            std::printf("    (this build has no traced original: the recording itself is not checked)\n");
        if (special)
            std::printf("    %" PRIu64 " calls touched I/O registers, values not recorded: not replayed\n", special);
        if (fn->readable)
        {
            std::printf("    readable:        %" PRIu64 " of %" PRIu64 " identical", readableOk, cases - skipped);
            if (readableBad)
                std::printf(", %" PRIu64 " differ%s", readableBad,
                            fillDependent ? (" (" + std::to_string(fillDependent) + " read memory the original did not)").c_str() : "");
            std::printf("\n");
            if (recordedWithCycles)
                std::printf("    EE cycles:       %s (original %.1f a call, readable %.1f)\n",
                            cyclesDiffer ? (std::to_string(cyclesDiffer) + " of " + std::to_string(recordedWithCycles) +
                                            " calls charge differently").c_str()
                                         : "the same on every call",
                            double(originalCycles) / double(recordedWithCycles),
                            double(readableCycles) / double(recordedWithCycles));
            if (deadV0)
                std::printf("    note: v0 differs in %" PRIu64 " calls; the function is declared void (check that no caller "
                            "uses it)\n",
                            deadV0);
            if (deadF0)
                std::printf("    note: f0 differs in %" PRIu64 " calls; no caller reads it after the call (or the function "
                            "is declared void)\n",
                            deadF0);
            if (argsLeft)
                std::printf("    note: %" PRIu64 " calls leave out an argument register the callee reads, with another value "
                            "than the original's (first: %s)\n",
                            argsLeft, firstArgsLeft.c_str());
            if (regsLeft)
                std::printf("    note: %" PRIu64 " calls leave a register a caller reads after the call with another value "
                            "than the original's (first: %s)\n",
                            regsLeft, firstRegsLeft.c_str());
            if (wide)
                std::printf("    note: %" PRIu64 " calls hand on a 64-bit value with another upper half than the "
                            "original's (first: %s)\n",
                            wide, firstWide.c_str());
            if (savesDiffer)
            {
                std::printf("    note: %" PRIu64 " calls give a callee another value of a register its prologue saves than "
                            "the original held there (ee::calleeSaves; first: %s)\n",
                            savesDiffer, firstSaves.c_str());
                // (callee@the original's jal: registers; readable_saves.py
                // writes the drafter's ee::calleeSaves line for each)
                std::printf("          at: %s\n", saveSites(false).c_str());
            }
            if (heldDiffer)
                std::printf("    note: %" PRIu64 " calls give a callee another value of a callee-saved register it does not "
                            "save itself (first: %s)\n",
                            heldDiffer, firstHeld.c_str());
            if (vu0Left)
                std::printf("    note: %" PRIu64 " calls leave VU0 unlike the original (first: %s)\n", vu0Left,
                            firstVu0Left.c_str());
            if (residueDiffer)
                std::printf("    note: %" PRIu64 " calls leave their frame unlike the original (%" PRIu64
                            " bytes; first: %s)\n",
                            residueDiffer, residueBytes, firstResidue.c_str());
            if (residueRead)
                std::printf("    %" PRIu64 " calls leave their frame unlike the original where a later function reads "
                            "it (first: %s)\n",
                            residueRead, firstResidueRead.c_str());
            if (residueMasked)
                std::printf("    note: %" PRIu64 " calls leave their frame unlike the original where a later function reads "
                            "it and does not act on it (first: %s)\n",
                            residueMasked, firstResidueMasked.c_str());
        }
        if (blocks)
        {
            uint32_t hit = 0;
            for (uint32_t i = 0; i < blocks->count; ++i)
                hit += g_coverage.calls(i) ? 1u : 0u;
            std::printf("    coverage:        %u of %u blocks of the original run by the %" PRIu64 " calls it replayed\n", hit,
                        blocks->count, g_coverage.counted());
        }
        // One line per file for tools (readable_sweep.py): the counts and the
        // first failure, quoted.
        auto vu0LaneList = [](const uint32_t (&lanes)[32], bool q) {
            std::string list;
            for (int i = 1; i < 32; ++i)
            {
                if (!lanes[i])
                    continue;
                list += (list.empty() ? "vf" : ",vf") + std::to_string(i) + ".";
                for (int lane = 0; lane < 4; ++lane)
                    if (lanes[i] & (1u << lane))
                        list += "xyzw"[lane];
            }
            if (q)
                list += list.empty() ? "Q" : ",Q";
            return list;
        };
        auto quoted = [](const std::string &s) {
            std::string q = "\"";
            for (char c : s)
                q += (c == '"' || c == '\\') ? '\'' : (c == '\n' ? ' ' : c);
            return q + "\"";
        };
        // new_bad: calls only the checks added in round `coverage` fail (in
        // readable_bad too); dead_f0: f0 differs where no caller reads it (or
        // the function is void), as dead_v0. The digests: impl
        // (the readable function's source, "-" without one), orig (the
        // generated original), rec (recorder version and corpus folder),
        // checker (this executable).
        // What the callee probes saw (probeCallee), on the result line: the
        // calls looked at, how each ended, the bytes kept (log2 buckets,
        // bucket b holding 2^(b-1) .. 2^b - 1 bytes; bucket 0 none), and of
        // the calls whose callee the folder has a recording of, the ones it
        // did not serve.
        std::string probeFields;
        {
            std::string ends, buckets;
            for (uint32_t e = 0; e < kProbeEnds; ++e)
                ends += (e ? "," : "") + std::string(kProbeEndName[e]) + ":" + std::to_string(g_probeStats.ends[e]);
            for (size_t i = 0; i < std::size(g_probeStats.buckets); ++i)
                buckets += (i ? "," : "") + std::to_string(g_probeStats.buckets[i]);
            probeFields = " callee_probes=" + std::to_string(g_probeStats.probes) + " callee_with_bytes=" +
                          std::to_string(g_probeStats.withBytes) + " callee_bytes=" + std::to_string(g_probeStats.bytes) +
                          " callee_ends=" + ends + " callee_recorded=" + std::to_string(g_probeStats.calleeRecorded) +
                          " callee_inconsistent=" + std::to_string(g_probeStats.inconsistent) + " callee_ambiguous=" +
                          std::to_string(g_probeStats.ambiguous) + " callee_diverged=" + std::to_string(g_probeStats.diverged) +
                          " callee_buckets=" + buckets + " callee_masked=" + std::to_string(calleeMasked) +
                          " callee_cycles=" + std::to_string(g_probeStats.cycles) +
                          " callee_ms=" + std::to_string(static_cast<uint64_t>(g_probeStats.seconds * 1000.0)) +
                          " callee_index_ms=" + std::to_string(static_cast<uint64_t>(g_probeStats.indexSeconds * 1000.0)) +
                          " callee_frame_read_calls=" + std::to_string(g_probeStats.frameReadCalls) +
                          " callee_frame_read_bytes=" + std::to_string(g_probeStats.frameReadBytes);
        }
        // Records that stop at a call (judged at that call, not at a return): a line of their own.
        if (stops)
            std::printf("[fncheck:stops] address=0x%08x game=%s file=%s records=%" PRIu64 "\n", fn->address,
                        game::gameName(game), path.filename().string().c_str(), stops);
        std::printf("[fncheck:result] address=0x%08x game=%s file=%s recorded=%" PRIu64 " original_ok=%" PRIu64
                    " original_bad=%" PRIu64 " readable=%d readable_ok=%" PRIu64 " readable_bad=%" PRIu64
                    " skipped=%" PRIu64 " cycles_differ=%" PRIu64 " special=%" PRIu64 " dead_v0=%" PRIu64 " first_original=%s"
                    " first_readable=%s new_bad=%" PRIu64 " dead_f0=%" PRIu64 " status=%s impl=%s orig=%s rec=%s checker=%s"
                    " args_left=%" PRIu64 " first_args_left=%s regs_left=%" PRIu64 " first_regs_left=%s"
                    " saves_differ=%" PRIu64 " first_saves=%s held_differ=%" PRIu64 " first_held=%s"
                    " saves_sites=%s held_sites=%s wide=%" PRIu64 " first_wide=%s direct_left=%" PRIu64
                    " first_direct_left=%s handed_left=%" PRIu64 " first_handed_left=%s vu0_left=%" PRIu64
                    " first_vu0_left=%s vu0_lanes=%s entry_left=%" PRIu64 " first_entry_left=%s direct_called=%d"
                    " stale_left=%" PRIu64 " first_stale_left=%s vu0_live=%" PRIu64 " first_vu0_live=%s vu0_call=%" PRIu64
                    " first_vu0_call=%s callee_read=%" PRIu64 " first_callee_read=%s%s frame_residue_differs=%" PRIu64
                    " residue_bytes=%" PRIu64 " first_residue=%s frame_residue_read=%" PRIu64 " frame_residue_read_value=%" PRIu64
                    " first_residue_read=%s frame_residue_masked=%" PRIu64 " first_residue_masked=%s leftovers_acted=%" PRIu64
                    " leftovers_masked=%" PRIu64
                    " leftover_reads=%" PRIu64 " frame_index=%lld callee_sp_differs=%" PRIu64 " first_callee_sp=%s mode=%s\n",
                    fn->address, game::gameName(game), quoted(path.string()).c_str(), cases, originalOk, originalBad, fn->readable ? 1 : 0,
                    readableOk, readableBad, skipped, cyclesDiffer, special, deadV0, quoted(firstOriginal).c_str(),
                    quoted(firstReadable).c_str(), newBad, deadF0, game::readable::statusName(fn->status),
                    fn->readable ? readableDigest(*fn) : "-", blocks ? blocks->original : "-",
                    quoted(recordingDigest(path, header.version)).c_str(), checkerDigest().c_str(), argsLeft,
                    quoted(firstArgsLeft).c_str(), regsLeft, quoted(firstRegsLeft).c_str(), savesDiffer,
                    quoted(firstSaves).c_str(), heldDiffer, quoted(firstHeld).c_str(), quoted(saveSites(false)).c_str(),
                    quoted(saveSites(true)).c_str(), wide, quoted(firstWide).c_str(), directLeft,
                    quoted(firstDirectLeft).c_str(), handedLeft, quoted(firstHandedLeft).c_str(), vu0Left,
                    quoted(firstVu0Left).c_str(), quoted(vu0LaneList(vu0Lanes, vu0Q)).c_str(), entryLeft,
                    quoted(firstEntryLeft).c_str(), game::readable::calledDirectly(fn->host) ? 1 : 0, staleLeft,
                    quoted(firstStaleLeft).c_str(), vu0Live, quoted(firstVu0Live).c_str(), vu0Call, quoted(firstVu0Call).c_str(),
                    calleeRead, quoted(firstCalleeRead).c_str(), probeFields.c_str(), residueDiffer, residueBytes,
                    quoted(firstResidue).c_str(), residueRead, residueReadValue, quoted(firstResidueRead).c_str(), residueMasked,
                    quoted(firstResidueMasked).c_str(), leftoversActed, leftoversMasked, leftoverReads,
                    g_frameReadsIn.empty() ? -1ll : static_cast<long long>(g_frameIndex.reads), calleeSpDiffer,
                    quoted(firstCalleeSp).c_str(), options.semantic ? "semantic" : options.cycles ? "cycles" : "plain");
        if (!g_upperReads.empty())
            std::printf("[fncheck:upper] address=0x%08x game=%s file=%s upper=%" PRIu64 " first_upper=%s\n", fn->address,
                        game::gameName(game), quoted(path.string()).c_str(), upper, quoted(firstUpper).c_str());
        if (blocks)
        {
            // The blocks in order, and the ones run with the number of calls
            // that ran each.
            std::string list, hits;
            for (uint32_t i = 0; i < blocks->count; ++i)
            {
                char b[40];
                std::snprintf(b, sizeof(b), "%s0x%x", i ? "," : "", blocks->starts[i]);
                list += b;
                if (const uint64_t n = g_coverage.calls(i))
                {
                    std::snprintf(b, sizeof(b), "%s0x%x:%" PRIu64, hits.empty() ? "" : ",", blocks->starts[i], n);
                    hits += b;
                }
            }
            std::printf("[fncheck:coverage] address=0x%08x game=%s name=%s status=%s file=%s calls=%" PRIu64
                        " blocks=%s hits=%s\n",
                        fn->address, game::gameName(game), fn->name, game::readable::statusName(fn->status),
                        quoted(path.string()).c_str(), g_coverage.counted(), list.empty() ? "-" : list.c_str(),
                        hits.empty() ? "-" : hits.c_str());
        }
        if (options.safepoints && fn->readable && fn->traced)
            std::printf("[fncheck:safepoints] address=0x%08x game=%s name=%s status=%s file=%s calls=%u points=%u "
                        "differ=%u count_differ=%u original_bad=%u first=%s\n",
                        fn->address, game::gameName(game), fn->name, game::readable::statusName(fn->status),
                        quoted(path.string()).c_str(), spStats.calls, spStats.points, spStats.differ,
                        spStats.countDiffer, spStats.originalBad,
                        quoted(spStats.first.empty() ? std::string("-") : spStats.first).c_str());
        if (g_markersMode && blocks && fn->readable)
        {
            // An entry block the original reached (the block holding it ran
            // in a replayed call: a call's return address lies inside a
            // block) that no readable replay marked.
            auto reached = [&](uint32_t address)
            {
                uint32_t holder = blocks->count;
                for (uint32_t i = 0; i < blocks->count; ++i)
                    if (blocks->starts[i] <= address)
                        holder = i;
                return holder < blocks->count && g_coverage.calls(holder) != 0;
            };
            std::string blocksLeft, loopsLeft;
            uint32_t entries = 0, reachedCount = 0;
            const auto [first, last] = safePointMarks(static_cast<uint8_t>(game), fn->address);
            for (const fncheck::SafePointMark *m = first; m != last; ++m)
            {
                if (!(m->flags & 1u))
                    continue; // (a slice's end, SliceEmu's)
                ++entries;
                if (!reached(m->address))
                    continue;
                ++reachedCount;
                char b[24];
                std::snprintf(b, sizeof(b), "0x%x", m->address);
                if (!g_markedBlocks.count(m->address))
                    blocksLeft += (blocksLeft.empty() ? "" : ",") + std::string(b);
                // A loop head counts only when a replayed call took the
                // branch back to it.
                if ((m->flags & 2u) && g_coverage.backTaken(m->address) && !g_markedLoops.count(m->address))
                    loopsLeft += (loopsLeft.empty() ? "" : ",") + std::string(b);
            }
            auto list = [](const std::set<uint32_t> &s)
            {
                std::string out;
                for (uint32_t a : s)
                {
                    char b[24];
                    std::snprintf(b, sizeof(b), "%s0x%x", out.empty() ? "" : ",", a);
                    out += b;
                }
                return out.empty() ? std::string("-") : out;
            };
            // (the marked ones too: fncheck_corpus.py markers sums the files)
            std::printf("[fncheck:markers] address=0x%08x game=%s name=%s status=%s file=%s entry_blocks=%u reached=%u "
                        "marked_blocks=%zu marked_loops=%zu unmarked_blocks=%s unmarked_loops=%s marked_blocks_at=%s "
                        "marked_loops_at=%s order_bad=%u first_order=%s\n",
                        fn->address, game::gameName(game), fn->name, game::readable::statusName(fn->status),
                        quoted(path.string()).c_str(), entries, reachedCount, g_markedBlocks.size(),
                        g_markedLoops.size(), blocksLeft.empty() ? "-" : blocksLeft.c_str(),
                        loopsLeft.empty() ? "-" : loopsLeft.c_str(),
                        list(std::set<uint32_t>(g_markedBlocks.begin(), g_markedBlocks.end())).c_str(),
                        list(std::set<uint32_t>(g_markedLoops.begin(), g_markedLoops.end())).c_str(), orderBad,
                        quoted(firstOrder.empty() ? std::string("-") : firstOrder).c_str());
        }
        g_markedBlocks.clear();
        g_markedLoops.clear();
        // A compressed file that ends inside a frame, is damaged or is shorter than its head says: said, and a
        // failure (a plain file cut short by a kill reads its whole records and stops, as before).
        if (const std::string problem = reader.problem(); !problem.empty())
        {
            std::fprintf(stderr, "%s: %s\n", path.string().c_str(), problem.c_str());
            anyFailure = true;
        }
        std::fflush(stdout);
        if (originalBad || readableBad || cases == 0)
            anyFailure = anyFailure || originalBad || readableBad;
        return true;
    }


    // ---- generated calls of pure vector routines ----------------------------

    // Routines that read only vectors their pointer arguments give and
    // write only through output pointers (no calls, no globals): checked on
    // generated inputs, so every path is taken, not only the ones a
    // recorded run reached. Arguments in order a0, a1, a2, a3, t0, t1.
    enum class Arg
    {
        Vector,         // a Vec4 to read
        UnitOrNull,     // a unit direction, or 0
        OutVector,      // a Vec4 to write
        OutOrNull,      // a Vec4 to write, or 0
        OutFloatOrNull, // a float to write, or 0
    };
    struct VectorRoutine
    {
        uint32_t address;
        std::vector<Arg> args;
    };
    const VectorRoutine kVectorRoutines[] = {
        // closestPointOnSegment (a, b, direction or 0, point, out)
        {sym::closestPointOnSegment, {Arg::Vector, Arg::Vector, Arg::UnitOrNull, Arg::Vector, Arg::OutVector}},
        // closestPointOnTriangle (point, v0, v1, v2, out or 0, squared or 0)
        {sym::closestPointOnTriangle, {Arg::Vector, Arg::Vector, Arg::Vector, Arg::Vector, Arg::OutOrNull, Arg::OutFloatOrNull}},
    };

    struct Rng
    {
        uint64_t s;
        uint32_t next()
        {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return static_cast<uint32_t>(s >> 11);
        }
        float uniform(float lo, float hi) { return lo + (hi - lo) * (next() & 0xFFFFFF) / float(0x1000000); }
        bool chance(int percent) { return next() % 100u < static_cast<uint32_t>(percent); }
    };

    bool generate(const std::string &name, uint64_t count, PS2Runtime &runtime)
    {
        const Function *fn = game::readable::find(name);
        const VectorRoutine *routine = nullptr;
        for (const VectorRoutine &r : kVectorRoutines)
            if (fn && r.address == fn->address)
                routine = &r;
        if (!fn || !routine || !fn->readable || !fn->traced)
        {
            std::cerr << "--generate: " << name << " is not a vector routine with a readable version and a traced original"
                      << std::endl;
            return false;
        }
        // Data (a vector every 0x40 bytes), then the stack.
        constexpr uint32_t kData = (2u << 20), kStackTop = (3u << 20), kReturn = 0x00123450u; // not an address
        std::set<uint32_t> pages = {kData / g_ram.pageSize()};
        for (uint32_t a = kStackTop - 0x4000u; a < kStackTop; a += g_ram.pageSize())
            pages.insert(a / g_ram.pageSize());
        uint8_t *ram = g_ram.base();
        // Path coverage of the generated calls (a call counts when the
        // readable version gave the original's result back), printed as a
        // replayed file's: file="generate:<n>" (round covrec).
        const fncheck::BlockTable *blocks = fncheck::findBlocks(fn->address, fn->game);
        g_coverage.reset(blocks);
        Rng rng{0x9E3779B97F4A7C15ull ^ fn->address};
        uint64_t same = 0, shown = 0;
        std::map<std::string, uint64_t> failures;
        for (uint64_t call = 0; call < count; ++call)
        {
            // The inputs: a shape around the origin, points near and far,
            // with degenerate cases (repeated or lined-up vertices, a point on
            // a vertex), and the odd w.
            float vec[4][4];
            const float scale = rng.chance(20) ? 1000.0f : rng.chance(30) ? 0.01f : 10.0f;
            for (auto &v : vec)
            {
                for (int i = 0; i < 3; ++i)
                    v[i] = rng.uniform(-scale, scale);
                v[3] = rng.chance(70) ? 1.0f : rng.uniform(-2.0f, 2.0f);
            }
            if (rng.chance(10))
                std::memcpy(vec[2], vec[1], sizeof(vec[1])); // two vertices the same
            if (rng.chance(10))
                for (int i = 0; i < 3; ++i)
                    vec[3][i] = vec[1][i] + (vec[2][i] - vec[1][i]) * 2.0f; // lined up
            if (rng.chance(5))
                std::memcpy(vec[0], vec[1 + rng.next() % 3], sizeof(vec[0])); // the point on a vertex
            if (rng.chance(5))
                for (int i = 0; i < 3; ++i)
                    vec[0][i] = 0.0f;

            R5900Context entry{};
            int vector = 0;
            uint32_t outputs[2] = {0, 0};
            static constexpr int kArgRegs[] = {4, 5, 6, 7, 8, 9};
            std::vector<std::pair<uint32_t, std::array<uint8_t, 16>>> inputs;
            for (size_t i = 0; i < routine->args.size(); ++i)
            {
                const uint32_t at = kData + static_cast<uint32_t>(i) * 0x40u;
                uint32_t value = at;
                std::array<uint8_t, 16> bytes{};
                switch (routine->args[i])
                {
                case Arg::Vector:
                    std::memcpy(bytes.data(), vec[vector++ % 4], 16);
                    inputs.emplace_back(at, bytes);
                    break;
                case Arg::UnitOrNull:
                    if (rng.chance(50))
                        value = 0;
                    else
                    {
                        float d[4] = {rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1), 0.0f};
                        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        for (int k = 0; k < 3 && l > 0.0f; ++k)
                            d[k] /= l;
                        d[3] = rng.chance(50) ? 0.0f : rng.uniform(-1, 1);
                        std::memcpy(bytes.data(), d, 16);
                        inputs.emplace_back(at, bytes);
                    }
                    break;
                case Arg::OutVector:
                    outputs[0] = at;
                    break;
                case Arg::OutOrNull:
                    value = rng.chance(25) ? 0u : at;
                    outputs[0] = value;
                    break;
                case Arg::OutFloatOrNull:
                    value = rng.chance(25) ? 0u : at;
                    outputs[1] = value;
                    break;
                }
                ee::setGpr64(&entry, kArgRegs[i], value);
            }
            ee::setGpr64(&entry, ee::reg::sp, kStackTop - 0x100u);
            ee::setGpr64(&entry, ee::reg::ra, kReturn);
            // VU0 as some earlier code left it.
            for (int r = 1; r < 32; ++r)
                entry.vu0_vf[r] = _mm_setr_ps(rng.uniform(-5, 5), rng.uniform(-5, 5), rng.uniform(-5, 5), rng.uniform(-5, 5));
            entry.vu0_vf[0] = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
            entry.vu0_q = rng.uniform(-3, 3);
            entry.vu0_status = static_cast<uint16_t>(rng.next() & 0x3CFu);
            entry.vu0_mac_flags = rng.next() & 0xFFFFu;

            struct Result
            {
                R5900Context ctx;
                std::vector<uint8_t> data;
            };
            auto run = [&](bool original)
            {
                g_ram.open(pages, 0xA5u);
                for (const auto &[at, bytes] : inputs)
                    std::memcpy(ram + at, bytes.data(), 16);
                Result r{entry, {}};
                r.ctx.pc = fn->address;
                if (original)
                {
                    g_coverage.beginCall();
                    fncheck::t_blockSink = &g_coverage;
                    fn->traced(ram, &r.ctx, &runtime);
                    fncheck::t_blockSink = nullptr;
                }
                else
                    fn->readable(ram, &r.ctx, &runtime);
                r.data.assign(ram + kData, ram + kData + 0x200u);
                g_ram.close(pages);
                return r;
            };
            const Result want = run(true), got = run(false);
            std::string why;
            if (got.ctx.pc != kReturn || want.ctx.pc != kReturn)
                why = "did not return";
            else if (got.data != want.data)
                why = "memory written";
            else
            {
                for (int r = 1; r < 32 && why.empty(); ++r)
                    if (std::memcmp(&got.ctx.vu0_vf[r], &want.ctx.vu0_vf[r], 16) != 0)
                        why = "VU0 vf" + std::to_string(r);
                if (why.empty() && std::memcmp(&got.ctx.vu0_q, &want.ctx.vu0_q, 4) != 0)
                    why = "VU0 Q";
                if (why.empty() && got.ctx.vu0_mac_flags != want.ctx.vu0_mac_flags)
                    why = "VU0 MAC flags";
                if (why.empty() && got.ctx.vu0_status != want.ctx.vu0_status)
                    why = "VU0 status";
                for (int i : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
                    if (why.empty() && ee::gpr64(&got.ctx, i) != ee::gpr64(&entry, i))
                        why = "r" + std::to_string(i) + " not preserved";
            }
            g_coverage.endCall(why.empty());
            if (why.empty())
            {
                ++same;
                continue;
            }
            ++failures[why];
            if (shown++ < 3)
            {
                std::cout << "  call " << call << ": " << why << "; inputs";
                for (const auto &[at, bytes] : inputs)
                {
                    float f[4];
                    std::memcpy(f, bytes.data(), 16);
                    std::cout << " (" << f[0] << ", " << f[1] << ", " << f[2] << ", " << f[3] << ")";
                }
                std::cout << " out " << hex(outputs[0]) << " squared " << hex(outputs[1]) << std::endl;
            }
        }
        std::cout << hex(fn->address) << " " << fn->name << ": " << same << " of " << count
                  << " generated calls identical (memory written, VU0 registers, Q, MAC and status flags)" << std::endl;
        for (const auto &[why, n] : failures)
            std::cout << "  " << n << " differ: " << why << std::endl;
        if (blocks)
        {
            std::string list, hits;
            for (uint32_t i = 0; i < blocks->count; ++i)
            {
                char b[40];
                std::snprintf(b, sizeof(b), "%s0x%x", i ? "," : "", blocks->starts[i]);
                list += b;
                if (const uint64_t n = g_coverage.calls(i))
                {
                    std::snprintf(b, sizeof(b), "%s0x%x:%" PRIu64, hits.empty() ? "" : ",", blocks->starts[i], n);
                    hits += b;
                }
            }
            std::cout << std::flush;
            std::printf("[fncheck:coverage] address=0x%08x game=%s name=%s status=%s file=\"generate:%" PRIu64
                        "\" calls=%" PRIu64 " blocks=%s hits=%s\n",
                        fn->address, game::gameName(fn->game), fn->name, game::readable::statusName(fn->status), count,
                        g_coverage.counted(), list.empty() ? "-" : list.c_str(), hits.empty() ? "-" : hits.c_str());
            std::fflush(stdout);
        }
        return same == count;
    }

    // ---- --self-test-stops: records that stop at a call ----------------------
    //
    // A function made for the test (it is no game's): it writes p + 1 at 0x5000,
    // calls 0x6000 with p, writes the result at 0x5004 and calls 0x6100 with
    // it, which never comes back (a stop record, fncheck::kStopsAtCall). Its
    // traced original is written as fncheck_traced.h traces generated code; the
    // record is the recorder's form of one call (p 7, the first callee's
    // result 20). The good readable version passes; each broken one fails.
    namespace stoptest
    {
        constexpr uint32_t kFunction = 0x00004C00u, kFirstReturn = 0x00004C10u, kStopReturn = 0x00004C20u;
        constexpr uint32_t kFirst = 0x00006000u, kStop = 0x00006100u, kOtherStop = 0x00006200u;
        constexpr uint32_t kOut = 0x00005000u, kCallerReturn = 0x00004F00u, kSp = 0x01F00000u;
        constexpr uint32_t kArgument = 7u, kFirstResult = 20u;

        void traced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            auto word = [&](uint32_t address) -> uint32_t & { return *reinterpret_cast<uint32_t *>(rdram + address); };
            constexpr int kCall = static_cast<int>(PS2Runtime::GuestBranchKind::DirectCall);
            const uint32_t p = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0));
            fncheck::traceWrite(kOut, 4u);
            word(kOut) = p + 1u;
            ee::setGpr64(ctx, ee::reg::ra, kFirstReturn);
            if (!fncheck::tracedDispatch(runtime, rdram, ctx, kFirst, kFunction + 8u, kFirstReturn, kCall, ""))
                return;
            const uint32_t result = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::v0));
            fncheck::traceWrite(kOut + 4u, 4u);
            word(kOut + 4u) = result;
            ee::setGpr64(ctx, ee::reg::a0, result);
            ee::setGpr64(ctx, ee::reg::ra, kStopReturn);
            if (!fncheck::tracedDispatch(runtime, rdram, ctx, kStop, kFunction + 0x18u, kStopReturn, kCall, ""))
                return;
            ctx->pc = kCallerReturn; // (never in the game: the stop's callee does not come back)
        }

        int32_t good(uint32_t p)
        {
            *ee::at<uint32_t>(kOut) = p + 1u;
            const int32_t result = ee::call<int32_t>(kFirst, p);
            *ee::at<int32_t>(kOut + 4u) = result;
            return ee::call<int32_t>(kStop, result);
        }
        int32_t oneCallFewer(uint32_t p)
        {
            *ee::at<uint32_t>(kOut) = p + 1u;
            *ee::at<int32_t>(kOut + 4u) = static_cast<int32_t>(kFirstResult);
            return ee::call<int32_t>(kStop, static_cast<int32_t>(kFirstResult));
        }
        int32_t otherStop(uint32_t p)
        {
            *ee::at<uint32_t>(kOut) = p + 1u;
            const int32_t result = ee::call<int32_t>(kFirst, p);
            *ee::at<int32_t>(kOut + 4u) = result;
            return ee::call<int32_t>(kOtherStop, result);
        }
        int32_t extraByte(uint32_t p)
        {
            *ee::at<uint32_t>(kOut) = p + 1u;
            const int32_t result = ee::call<int32_t>(kFirst, p);
            *ee::at<int32_t>(kOut + 4u) = result;
            *ee::at<uint8_t>(kOut + 8u) = 1u;
            return ee::call<int32_t>(kStop, result);
        }
        int32_t returnsInstead(uint32_t p)
        {
            *ee::at<uint32_t>(kOut) = p + 1u;
            const int32_t result = ee::call<int32_t>(kFirst, p);
            *ee::at<int32_t>(kOut + 4u) = result;
            return result;
        }

        Record record()
        {
            Record r;
            r.flags = fncheck::kStopsAtCall;
            ee::setGpr64(&r.entry, ee::reg::a0, kArgument);
            ee::setGpr64(&r.entry, ee::reg::sp, kSp);
            ee::setGpr64(&r.entry, ee::reg::ra, kCallerReturn);
            r.entry.pc = kFunction;
            r.deadLow = kSp - fncheck::kStackWindow;
            Line in;
            in.address = kOut;
            in.mask = 0x00FFu; // (the words written, as they were: the recorder keeps a written byte's old value)
            r.inputs.push_back(in);
            Line out = in;
            const uint32_t first = kArgument + 1u, second = kFirstResult;
            std::memcpy(out.bytes, &first, 4);
            std::memcpy(out.bytes + 4, &second, 4);
            r.outputs.push_back(out);
            Call call;
            call.target = kFirst;
            call.sp = kSp;
            call.a[0] = kArgument;
            call.highKnown = true;
            call.after = r.entry;
            ee::setGpr64(&call.after, ee::reg::v0, kFirstResult);
            ee::setGpr64(&call.after, ee::reg::ra, kFirstReturn);
            call.after.pc = kFirstReturn;
            r.calls.push_back(call);
            Call stop;
            stop.target = kStop;
            stop.sp = kSp;
            stop.flags = fncheck::kStopCall;
            stop.a[0] = kFirstResult;
            stop.highKnown = true;
            r.calls.push_back(stop);
            r.exit = call.after; // the registers at the stop call
            ee::setGpr64(&r.exit, ee::reg::a0, kFirstResult);
            ee::setGpr64(&r.exit, ee::reg::ra, kStopReturn);
            r.exit.pc = kStop;
            r.cycles = 2u * EeScheduler::kGuestDispatchCycles;
            return r;
        }
    }

    // 0 if the good version passes and every broken one fails, as must be.
    int selfTestStops(PS2Runtime &runtime)
    {
        using namespace stoptest;
        g_calleeReadsOn = false; // (a callee probe needs the game's traced originals)
        g_game = game::Game::Ai;
        fncheck::setTracedGame(game::Game::Ai);
        const Record r = record();
        Function fn;
        fn.address = kFunction;
        fn.name = "stopSelfTest";
        fn.status = game::readable::Status::Draft;
        fn.returnKind = ee::ReturnKind::Int32;
        fn.traced = &traced;
        g_function = &fn;
        int wrong = 0;
        auto expect = [&](const char *what, bool wantOk, const Outcome &o)
        {
            const bool right = o.ok == wantOk;
            wrong += right ? 0 : 1;
            std::printf("stop self-test: %-48s %s%s%s\n", what, o.ok ? "passes" : "fails", right ? "" : "  <- WRONG",
                        o.ok ? "" : (": " + o.why).c_str());
        };
        expect("the traced original", true, replay(fn, r, Who::Original, 0xA5u, runtime));
        Record bent = r;
        ee::setGpr64(&bent.exit, ee::reg::a0, kFirstResult + 1u);
        expect("the traced original, a0 at the stop not recorded", false, replay(fn, bent, Who::Original, 0xA5u, runtime));
        struct Case
        {
            const char *what;
            PS2Runtime::RecompiledFunction readable;
            bool ok;
        };
        const Case cases[] = {
            {"the readable version", &ee::entry<&good>, true},
            {"one call fewer before the stop", &ee::entry<&oneCallFewer>, false},
            {"another stop call", &ee::entry<&otherStop>, false},
            {"one byte of RAM more", &ee::entry<&extraByte>, false},
            {"returns instead of reaching the stop", &ee::entry<&returnsInstead>, false},
        };
        for (const Case &c : cases)
        {
            fn.readable = c.readable;
            expect(c.what, c.ok, replay(fn, r, Who::Readable, 0xA5u, runtime));
            if (c.ok)
                expect("(with the second fill)", true, replay(fn, r, Who::Readable, 0x5Au, runtime));
        }
        g_function = nullptr;
        std::printf("stop self-test: %s\n", wrong ? "FAILED" : "ok");
        return wrong ? 1 : 0;
    }

    void list()
    {
        for (const Function *f : game::readable::functions())
            std::printf("%s0x%08x  %-9s %-40s %s%s%s\n",
                        f->game == game::Game::Ai ? "" : (std::string(game::gameName(f->game)) + ":").c_str(), f->address,
                        game::readable::statusName(f->status), f->name,
                        f->file, f->waits ? "  (callees can wait: not switchable)" : "",
                        f->traced ? "" : "  (no traced original in this build)");
        // Registrations whose C++ functions the linker gave one address
        // (identical code folded: lld-link's full ICF), which game::call cannot
        // tell apart. None with /OPT:SAFEICF.
        size_t shared = 0;
        for (const Function *f : game::readable::functions())
            if (f->host)
                if (const Function *first = game::readable::findHost(f->host); first && first != f)
                {
                    ++shared;
                    std::printf("shares its host code: 0x%08x %s with 0x%08x %s\n", f->address, f->name, first->address,
                                first->name);
                }
        std::printf("%zu registrations share their host code with another\n", shared);
    }
}

int main(int argc, char **argv)
{
    Options options;
    std::vector<std::filesystem::path> inputs;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--only" && i + 1 < argc)
        {
            const Function *f = game::readable::find(std::string(argv[++i]));
            if (!f)
            {
                std::cerr << "unknown function " << argv[i] << std::endl;
                return 2;
            }
            options.only.insert((uint64_t(f->game) << 32) | f->address);
        }
        else if (arg == "--limit" && i + 1 < argc)
            options.limit = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--semantic")
            options.semantic = g_semantic = true;
        else if (arg == "--cycles")
            options.cycles = true;
        else if (arg == "--no-callee-reads")
            g_calleeReadsOn = false;
        else if (arg == "--frame-reads-out" && i + 1 < argc)
        {
            g_frameReadsOut = argv[++i];
            g_frameReadsOnly = true; // (the index's pass replays the originals alone)
        }
        else if (arg == "--frame-reads-in" && i + 1 < argc)
            g_frameReadsIn = argv[++i];
        else if (arg == "--frame-reads-only")
            g_frameReadsOnly = true;
        else if (arg == "--no-frame-residue")
            g_frameResidueOn = false;
        else if (arg == "--safepoints")
        {
            options.safepoints = true;
            g_sp.on = true;
            ee::setSafePointHook(&readableSafePoint);
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])))
                g_sp.maxPoints = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])))
                g_sp.maxCalls = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        }
        else if (arg == "--safepoints-paths" && i + 1 < argc)
            g_sp.maxPathCalls = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--upper" && i + 1 < argc)
            loadUpperReads(argv[++i]);
        else if (arg == "--call-blocks")
            g_callBlocks = true;
        else if (arg == "--call-timeout" && i + 1 < argc)
            g_callTimeoutMs = std::atoll(argv[++i]) * 1000;
        else if (arg == "--max-mem-mb" && i + 1 < argc)
            g_maxMemBytes = std::strtoull(argv[++i], nullptr, 0) << 20;
        else if (arg == "--runaway" && i + 1 < argc)
            g_runawayFactor = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--markers")
        {
            g_markersMode = true;
            ee::setMarkerHook(&noteMarker);
        }
        else if (arg == "--show" && i + 1 < argc)
            options.show = std::atoi(argv[++i]);
        else if (arg == "--generate" && i + 2 < argc)
        {
            options.generate = argv[++i];
            options.generated = std::strtoull(argv[++i], nullptr, 0);
        }
        else if (arg == "--self-test-stops")
            options.selfTestStops = true;
        else if (arg == "--list")
        {
            for (Function *f : game::readable::functions())
                f->traced = game::readable::tracedOriginal(f->address, f->game);
            list();
            return 0;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "usage: zx-fncheck [--only fn] [--limit n] [--show n] [--list] [--cycles] [--semantic] [--no-callee-reads] [--frame-reads-out dir] [--frame-reads-in dir] [--frame-reads-only] [--no-frame-residue] [--markers] [--safepoints [p [c]]] [--safepoints-paths n] "
                         "[--call-blocks] [--call-timeout s] [--max-mem-mb mb] [--runaway k] [--generate fn n] [--self-test-stops] <recording|folder>..."
                      << std::endl;
            return 2;
        }
        else
            inputs.emplace_back(arg);
    }
    if (options.semantic && (options.cycles || options.safepoints || g_markersMode))
    {
        std::cerr << "--semantic is the verdict without --cycles, --markers and --safepoints" << std::endl;
        return 2;
    }
    std::vector<std::filesystem::path> files;
    for (const auto &p : inputs)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec))
        {
            for (const auto &e : std::filesystem::directory_iterator(p, ec))
                if (e.path().extension() == ".fnrec")
                    files.push_back(e.path());
        }
        else
            files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    if (files.empty() && options.generate.empty() && !options.selfTestStops)
    {
        std::cerr << "no recordings given" << std::endl;
        return 2;
    }

    if (!g_ram.allocate())
    {
        std::cerr << "cannot reserve EE RAM" << std::endl;
        return 2;
    }
    installFaultHandler();
    startCallWatchdog();
    fncheck::g_callRunner = &probeRunCall;
    ee::g_ram = g_ram.base();
    for (Function *f : game::readable::functions())
        f->traced = game::readable::tracedOriginal(f->address, f->game);

    PS2Runtime runtime;
    // The scratchpad lives after RAM in the replay memory (memory keys).
    ps2SetScratchpadHostPtr(g_ram.base() + fncheck::kScratchpadKey);
    uploadVu0Memory(runtime);
    if (options.selfTestStops)
        return selfTestStops(runtime);
    bool anyFailure = false;
    if (!options.generate.empty() && !generate(options.generate, options.generated, runtime))
        anyFailure = true;
    for (const auto &file : files)
        checkFile(file, options, runtime, anyFailure);
    return anyFailure ? 1 : 0;
}
