// ZX_DIFFCALL: generated calls of functions no recording reaches.
//
// Some registered functions have no recording: no route reaches them, so
// nothing compares their readable versions with the originals. This runs such
// a function from a real game state with generated arguments and keeps the
// call as a recording, which zx-fncheck then replays as any other. The tool
// on top (tools/diffcall.py) makes the arguments, drives the rounds and sorts
// the verdicts; a pass from it is its own lower evidence tier, never Verified.
//
//   ZX_DIFFCALL=HOOK@V1,V2,...   snapshots at the first call of HOOK at or
//                                  after vsync V1, V2 ...
//   ZX_DIFFCALL=HOOK@trigger     ... whenever the file DIR/trigger appears
//                                  (its first word names the snapshot); a
//                                  route waits on the game, then touches it
//   ZX_DIFFCALL_DIR=DIR          the folder (required)
//   ZX_DIFFCALL_BANK=1           the argument bank: every registered
//                                  function's table slot wrapped, keeping the
//                                  last distinct argument values each was
//                                  called with
//   ZX_DIFFCALL_WAIT=<s>         how long a snapshot waits for the tool
//                                  (default 3600), then the game goes on
//   ZX_DIFFCALL_CASE_MS=<ms>     a case's time limit (default 1000; after
//                                  three hangs a target's other cases of the
//                                  batch are skipped)
//   ZX_DIFFCALL_CASE_MB=<mb>     a case's memory above what it was forked
//                                  with (default 1024)
//   ZX_DIFFCALL_PATHS=1          the search's feedback: each returned case
//                                  reports the blocks its target's original
//                                  ran, and only the first cases of a set of
//                                  them keep their recordings
//   ZX_DIFFCALL_PATH_KEEP=<n>    how many (default 4), and 8 times that for
//                                  cases that read other values than every
//                                  kept case of the path
//
// The game's own copy stops a batch past 4 GB: own-memory.txt and own-memory.sample
// in the snapshot's folder say what it held (BUG in the log).
//
// At a snapshot, on the EE thread at HOOK's entry, the game stops and the
// folder DIR/snap-<name> gets ram.bin, scratch.bin, regs.txt, bank.tsv (with
// the bank on) and then the empty file `ready`. The tool writes cases into
// batch-<n>.cases (by a rename: complete when it appears), one a line:
//
//   ID TARGET A0,..,T3(hex) F12,..,F19(float bits, hex) POKES
//
// TARGET `~ADDRESS` makes the call an invocation on the thread (as ZX_GUEST_CALL's
// `~`): it may wait, and the child's copy of the game runs on meanwhile.
// TARGET `ADDRESS@ENTRY` records ADDRESS but calls ENTRY, a caller of it, which
// builds the arguments itself (sscanf's FILE for __svfscanf).
//
// POKES is `-` or a list of `address=hexbytes` and `address*count` (that many
// zero bytes, both hex), separated by commas: memory written in the case's
// copy of the game before the call (strings, out buffers, blocks). `^SITE=HEX`
// makes the call at SITE (the jal's address) in the recorded function return
// HEX in v0, `^SITE:f=HEX` those float bits in f0: the callee ran as usual, its
// result is an input of the recorded call, answered from the recording in the
// replay as any other.
// Each case runs in a child of fork(): the game's whole state, this thread
// only. The child installs the recorder for that one function, sets the
// registers, makes the call at sp - 0x8000 (the poke arena above it) and ends;
// nothing it does reaches the game, which goes on as if no case had run (the
// RAM's hash is compared across a batch and logged; a change is a bug here).
// A hang ends the case at its time limit, a host crash ends the child: the
// batch goes on. batch-<n>.out gets a line a case:
//
//   ID STATUS BYTES MS [BLOCKS]
//
// STATUS: kept (returned, recorded), notkept (returned, the recorder dropped
// it), fault (the function touched an address the hardware faults on, or an
// address error was signalled: an invalid input, not kept), noreturn (the call did not come back to its return address), waited (it
// waited: thrown to the scheduler), hang (the time limit), memory (the cap),
// signal<N> (the child died), skipped (three hangs of this target before it).
// BLOCKS (ZX_DIFFCALL_PATHS): the original's blocks the case ran, hex with
// commas. A case with a set new at this snapshot (or one that faulted) appends
// to batch-<n>.reads what the search reads from it: `#ID`, `S SP` (the call's
// stack pointer), then `R BLOCK VISIT ADDRESS SIZE VALUE` per read, `C SITE TARGET V0 F0BITS` per call, and for a
// fault `W BLOCK ORDINAL ADDRESS SIZE STORE`: the wild access, the case's first,
// which ended it, the ORDINALth load or store of its block (STORE 2: a call to
// no code, ORDINAL its site).
// The empty file batch-<n>.done follows. Recorded calls are appended to
// DIR/snap-<name>/rec/<address>.fnrec with the case's ID as their sequence number. The file `finish` in the snapshot's folder lets the
// game go on to its next snapshot.

#include "ee/diffcall.h"

#include "ee/fncheck.h"
#include "ee/fnrec_file.h"
#include "ee/readable.h"

#include "machine.h"
#include "ee_instructions.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#endif

namespace game::diffcall
{
    bool g_countWild = false;
    uint64_t g_wildAccesses = 0;
    static uint32_t g_firstWild = 0;

    namespace
    {
        // The case's child: the first wild access ends the case, telling the
        // search where (the fault's blocks, reads and the access).
        void (*g_onWild)(uint32_t address, uint32_t size, bool store) = nullptr;
        void (*g_onCallWild)(uint32_t site, uint32_t target) = nullptr;
        uint32_t g_blockLoads = 0, g_blockStores = 0; // memory operations of the block's visit
    }

    void noteAccess(uint32_t address, uint32_t size, bool store)
    {
        ++(store ? g_blockStores : g_blockLoads);
        const uint32_t top = address & 0xFE000000u;
        const bool ram = (top == 0u && address >= 0x00080000u) || top == 0x20000000u || top == 0x30000000u ||
                         top == 0x80000000u || top == 0xA0000000u;
        // (LQ and SQ clear the low bits on the hardware; generated code does not, so a
        // misaligned quadword is no reference: a real one is aligned.)
        const bool misaligned = size > 1u && (address & (size - 1u)) != 0u;
        if ((!ram && !ps2IsScratchpadAddress(address)) || misaligned)
        {
            if (g_wildAccesses++ == 0u)
                g_firstWild = address;
            if (g_onWild)
                g_onWild(address, size, store);
        }
    }

#if defined(_WIN32)
    // Generated calls run on the Mac and Linux: each case is a fork of the
    // game, which Windows has not (a recording replays anywhere).
    void noteBlock(uint32_t) {}
    void noteRead(uint32_t, uint32_t) {}
    void noteCall(uint32_t, uint32_t) {}
    void noteCallReturn(R5900Context *, uint32_t, uint32_t) {}
    void install(PS2Machine &, game::Game)
    {
        if (const char *spec = std::getenv("ZX_DIFFCALL"); spec && spec[0])
            std::fprintf(stderr, "[diffcall] ZX_DIFFCALL is not on Windows (each case is a fork of the game): run it on "
                                 "the Mac or Linux\n");
    }
#else
    namespace
    {
        namespace fs = std::filesystem;

        constexpr uint32_t kCallStackDrop = 0x8000u; // the poke arena is [sp - 0x8000, sp)
        constexpr int kIntArgs = 8;                  // a0-a3, t0-t3
        constexpr int kFloatArgs = 8;                // f12-f19
        constexpr size_t kFileHeaderBytes = 24u;     // fncheck::FileHeader
        constexpr size_t kSequenceOffset = 8u;       // a record: magic, flags, sequence

        // ---- a case's feedback (the child) ------------------------------------

        constexpr uint32_t kLoggedVisits = 2u;      // of a block
        constexpr uint32_t kLoggedReads = 32u;      // of a block's visit
        constexpr size_t kLoggedTotal = 4096u;      // reads, or calls, of a case
        struct ReadRow
        {
            uint32_t block = 0, visit = 0, address = 0, size = 0;
            uint64_t value = 0;
        };
        struct CallRow
        {
            uint32_t site = 0, target = 0, f0 = 0;
            uint64_t v0 = 0;
        };
        struct Force
        {
            uint32_t site = 0;
            bool isFloat = false;
            uint64_t value = 0;
        };
        std::vector<ReadRow> g_reads;
        std::vector<CallRow> g_callRows;
        std::vector<Force> g_forces; // the case's
        std::unordered_map<uint32_t, uint32_t> g_visits;
        uint8_t *g_caseRdram = nullptr;
        uint32_t g_block = 0, g_visit = 0, g_blockReads = 0;
        uint32_t g_caseSp = 0; // the stack pointer the case's call starts from

        void logBlock(uint32_t block)
        {
            g_block = block;
            g_visit = ++g_visits[block];
            g_blockReads = g_blockLoads = g_blockStores = 0;
        }

        void logRead(uint32_t address, uint32_t size)
        {
            if (g_visit > kLoggedVisits || g_blockReads >= kLoggedReads || g_reads.size() >= kLoggedTotal || !g_caseRdram)
                return;
            ++g_blockReads;
            ReadRow r;
            r.block = g_block;
            r.visit = g_visit;
            r.address = address;
            r.size = size;
            const uint32_t key = fncheck::memoryKey(address);
            if (key + std::min(size, 8u) <= fncheck::kMemoryKeys)
                std::memcpy(&r.value, fncheck::keyPointer(g_caseRdram, key), std::min(size, 8u));
            g_reads.push_back(r);
        }

        void logCallReturn(R5900Context *ctx, uint32_t returnAddress, uint32_t target)
        {
            const uint32_t site = returnAddress - 8u;
            for (const Force &f : g_forces)
                if (f.site == site)
                {
                    if (f.isFloat)
                        std::memcpy(&ctx->f[ee::reg::f0], &f.value, 4);
                    else
                        ee::setGpr64(ctx, ee::reg::v0, f.value);
                }
            if (g_callRows.size() >= kLoggedTotal)
                return;
            CallRow c;
            c.site = site;
            c.target = target;
            c.v0 = ee::gpr64(ctx, ee::reg::v0);
            std::memcpy(&c.f0, &ctx->f[ee::reg::f0], 4);
            g_callRows.push_back(c);
        }

        // ---- the argument bank -------------------------------------------------

        constexpr uint32_t kBankBase = (1u << 20), kBankEnd = (3u << 20);
        constexpr int kBankValues = 8;
        constexpr int kBankRegs = kIntArgs + 4; // a0-a3, t0-t3, f12-f15
        struct BankRow
        {
            PS2Machine::RecompiledFunction previous = nullptr;
            uint32_t calls = 0;
            uint8_t next[kBankRegs] = {};
            uint64_t values[kBankRegs][kBankValues] = {};
            uint32_t ticks[kBankRegs][kBankValues] = {};
        };
        std::vector<BankRow *> g_bank; // by (address - kBankBase) / 4
        bool g_bankOn = false;
        uint32_t g_bankTick = 1;

        inline void bankKeep(BankRow &row, int reg, uint64_t value)
        {
            uint64_t *values = row.values[reg];
            for (int i = 0; i < kBankValues; ++i)
                if (values[i] == value)
                {
                    row.ticks[reg][i] = g_bankTick;
                    return;
                }
            const int at = row.next[reg];
            row.next[reg] = static_cast<uint8_t>((at + 1) % kBankValues);
            values[at] = value;
            row.ticks[reg][at] = g_bankTick;
        }

        void bankEntry(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            BankRow &row = *g_bank[(ctx->pc - kBankBase) >> 2];
            if (g_bankOn)
            {
                ++row.calls;
                for (int i = 0; i < kIntArgs; ++i)
                    bankKeep(row, i, ee::gpr64(ctx, ee::reg::a0 + i));
                for (int i = 0; i < 4; ++i)
                {
                    uint32_t bits;
                    std::memcpy(&bits, &ctx->f[ee::reg::f12 + i], 4);
                    bankKeep(row, kIntArgs + i, bits);
                }
            }
            row.previous(rdram, ctx, runtime);
        }

        void installBank(PS2Machine &runtime, game::Game game)
        {
            g_bank.assign((kBankEnd - kBankBase) >> 2, nullptr);
            uint32_t wrapped = 0;
            for (game::readable::Function *f : game::readable::functions())
            {
                if (f->game != game || f->address < kBankBase || f->address >= kBankEnd || !runtime.hasFunction(f->address))
                    continue;
                // One row a slot, as the recorder keeps one: a game can register one address under two
                // names (BMX's currentAsset and assetCount). Wrapped again, the row's previous was
                // bankEntry itself, which called itself for ever: BMX hung at boot in that address's
                // first call.
                if (g_bank[(f->address - kBankBase) >> 2])
                    continue;
                auto *row = new BankRow;
                row->previous = runtime.lookupFunction(f->address);
                g_bank[(f->address - kBankBase) >> 2] = row;
                if (runtime.replaceFunction(f->address, &bankEntry))
                    ++wrapped;
            }
            g_bankOn = true;
            std::fprintf(stderr, "[diffcall] argument bank on %u functions\n", wrapped);
        }

        void writeBank(const fs::path &file)
        {
            std::ofstream out(file);
            char line[96];
            for (size_t i = 0; i < g_bank.size(); ++i)
            {
                const BankRow *row = g_bank[i];
                if (!row || !row->calls)
                    continue;
                const uint32_t address = kBankBase + static_cast<uint32_t>(i << 2);
                for (int reg = 0; reg < kBankRegs; ++reg)
                    for (int v = 0; v < kBankValues; ++v)
                        if (row->ticks[reg][v])
                        {
                            std::snprintf(line, sizeof(line), "%08x\t%d\t%llx\t%u\t%u\n", address, reg,
                                          static_cast<unsigned long long>(row->values[reg][v]), row->ticks[reg][v], row->calls);
                            out << line;
                        }
            }
        }

        // ---- cases -------------------------------------------------------------

        struct Case
        {
            std::string id;
            uint32_t target = 0;
            bool invoke = false; // an invocation on the thread: it may wait
            uint64_t a[kIntArgs] = {};
            uint32_t f[kFloatArgs] = {};
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pokes;
            std::vector<Force> forces;
            uint32_t entry = 0; // what the child calls: the target, or a caller of it
        };

        std::vector<std::string> split(const std::string &s, char by)
        {
            std::vector<std::string> out;
            size_t from = 0;
            while (from <= s.size())
            {
                size_t to = s.find(by, from);
                if (to == std::string::npos)
                    to = s.size();
                out.push_back(s.substr(from, to - from));
                from = to + 1;
            }
            return out;
        }

        bool parseCase(const std::string &line, Case &c)
        {
            std::stringstream words(line);
            std::string target, ints, floats, pokes;
            if (!(words >> c.id >> target >> ints >> floats >> pokes))
                return false;
            c.invoke = target[0] == '~';
            c.target = static_cast<uint32_t>(std::strtoul(target.c_str() + (c.invoke ? 1 : 0), nullptr, 16));
            c.entry = c.target;
            if (const size_t via = target.find('@'); via != std::string::npos)
                c.entry = static_cast<uint32_t>(std::strtoul(target.c_str() + via + 1, nullptr, 16));
            const std::vector<std::string> a = split(ints, ',');
            for (size_t i = 0; i < a.size() && i < kIntArgs; ++i)
                c.a[i] = std::strtoull(a[i].c_str(), nullptr, 16);
            const std::vector<std::string> f = split(floats, ',');
            for (size_t i = 0; i < f.size() && i < kFloatArgs; ++i)
                c.f[i] = static_cast<uint32_t>(std::strtoul(f[i].c_str(), nullptr, 16));
            if (pokes != "-")
                for (const std::string &p : split(pokes, ','))
                {
                    const size_t eq = p.find('=');
                    if (!p.empty() && p[0] == '^' && eq != std::string::npos)
                    {
                        Force f;
                        f.site = static_cast<uint32_t>(std::strtoul(p.c_str() + 1, nullptr, 16));
                        f.isFloat = p.find(":f") != std::string::npos;
                        f.value = std::strtoull(p.c_str() + eq + 1, nullptr, 16);
                        c.forces.push_back(f);
                        continue;
                    }
                    if (const size_t star = p.find('*'); star != std::string::npos)
                    {
                        c.pokes.emplace_back(static_cast<uint32_t>(std::strtoul(p.c_str(), nullptr, 16)),
                                             std::vector<uint8_t>(std::strtoul(p.c_str() + star + 1, nullptr, 16), 0u));
                        continue;
                    }
                    if (eq == std::string::npos)
                        return false;
                    std::vector<uint8_t> bytes;
                    for (size_t i = eq + 1; i + 1 < p.size(); i += 2)
                        bytes.push_back(static_cast<uint8_t>(std::strtoul(p.substr(i, 2).c_str(), nullptr, 16)));
                    c.pokes.emplace_back(static_cast<uint32_t>(std::strtoul(p.c_str(), nullptr, 16)), std::move(bytes));
                }
            return true;
        }

        uint64_t envNumber(const char *name, uint64_t fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? std::strtoull(v, nullptr, 0) : fallback;
        }

        game::Game g_game = game::Game::Ai;
        PS2Machine::RecompiledFunction g_hookPrevious = nullptr;
        uint32_t g_hookAddress = 0;
        std::vector<uint64_t> g_vsyncs; // the snapshots still to come
        bool g_triggerMode = false;
        fs::path g_dir;
        uint32_t g_snapshots = 0;

        constexpr const char *kFeedbackFile = "case.feedback";

        // First line the blocks, then the reads and calls (the header's format).
        void writeFeedback(const fs::path &file, const std::string &wild = {})
        {
            std::ofstream out(file);
            char line[128];
            bool first = true;
            std::vector<uint32_t> blocks;
            for (const auto &[block, visits] : g_visits)
                blocks.push_back(block);
            std::sort(blocks.begin(), blocks.end());
            for (const uint32_t block : blocks)
            {
                std::snprintf(line, sizeof(line), "%s%x", first ? "" : ",", block);
                out << line;
                first = false;
            }
            out << '\n' << wild;
            std::snprintf(line, sizeof(line), "S %x\n", g_caseSp);
            out << line;
            for (const ReadRow &r : g_reads)
            {
                std::snprintf(line, sizeof(line), "R %x %x %x %x %llx\n", r.block, r.visit, r.address, r.size,
                              static_cast<unsigned long long>(r.value));
                out << line;
            }
            for (const CallRow &c : g_callRows)
            {
                std::snprintf(line, sizeof(line), "C %x %x %llx %x\n", c.site, c.target, static_cast<unsigned long long>(c.v0), c.f0);
                out << line;
            }
        }

        // The child: never returns. Exit status 0 returned, 1 did not return,
        // 2 waited (a throw to the scheduler), 3 the function itself touched an
        // address the hardware would fault on (noteAccess).
        [[noreturn]] void runCaseChild(const Case &c, const fs::path &folder, uint8_t *rdram, R5900Context *ctx,
                                       PS2Machine *runtime, uint32_t limitMs)
        {
            g_bankOn = false;
            g_vsyncs.clear();
            g_triggerMode = false;
            const bool debug = envNumber("ZX_DIFFCALL_DEBUG", 0u) != 0u; // the child's output kept
            if (const int null = ::open("/dev/null", O_WRONLY); null >= 0 && !debug)
            {
                ::dup2(null, 1);
                ::dup2(null, 2);
            }
            // A hang ends here, whatever it hangs in (the parent also watches).
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            signal(SIGALRM, SIG_DFL);
            itimerval timer{};
            timer.it_value.tv_sec = limitMs / 1000u;
            timer.it_value.tv_usec = static_cast<int>(limitMs % 1000u) * 1000;
            setitimer(ITIMER_REAL, &timer, nullptr);

            char address[16];
            std::snprintf(address, sizeof(address), "0x%08x", c.target);
            const bool paths = envNumber("ZX_DIFFCALL_PATHS", 0u) != 0u;
            if (paths)
            {
                // Every call tried, the first with new blocks kept; the blocks told back.
                setenv("ZX_FNREC_NEW_BLOCKS", "2", 1);
                unsetenv("ZX_FNREC_COVERED");
            }
            setenv("ZX_FNREC", address, 1);
            setenv("ZX_FNREC_DIR", folder.c_str(), 1);
            setenv("ZX_FNREC_MAX_MB", "256", 1);
            unsetenv("ZX_FNREC_FROM");
            // (generated calls of one function, made here directly: what readable code calls past the recorder
            // does not matter to them)
            setenv("ZX_FNREC_ALLOW_READABLE", "1", 1);
            fncheck::installRecorder(*runtime, g_game);
            g_countWild = true;
            g_caseRdram = rdram;
            g_forces = c.forces;

            for (const auto &[at, bytes] : c.pokes)
                for (size_t i = 0; i < bytes.size(); ++i)
                    if (uint8_t *p = getMemPtr(rdram, at + static_cast<uint32_t>(i)))
                        *p = bytes[i];
            for (int i = 0; i < kIntArgs; ++i)
                ee::setGpr64(ctx, ee::reg::a0 + i, c.a[i]);
            for (int i = 0; i < kFloatArgs; ++i)
                std::memcpy(&ctx->f[ee::reg::f12 + i], &c.f[i], 4);
            const auto leave = [folder, debug, paths, id = c.id](int status)
            {
                g_countWild = false;
                if (debug)
                    std::fprintf(stderr, "[diffcall] case %s: status %d, %llu wild accesses, first 0x%x\n", id.c_str(), status,
                                 static_cast<unsigned long long>(g_wildAccesses), g_firstWild);
                if (g_wildAccesses)
                {
                    std::error_code ec;
                    fs::remove_all(folder, ec); // (not kept: an invalid input)
                    status = 3;
                }
                else if (paths && status == 0)
                    writeFeedback(folder / kFeedbackFile);
                _exit(status);
            };
            if (paths)
            {
                static fs::path s_folder;
                s_folder = folder;
                g_onWild = [](uint32_t address, uint32_t size, bool store)
                {
                    g_onWild = nullptr;
                    g_countWild = false;
                    char line[96];
                    std::snprintf(line, sizeof(line), "W %x %x %x %x %x\n", g_block, store ? g_blockStores : g_blockLoads, address, size,
                                  store ? 1 : 0);
                    std::error_code ec;
                    fs::remove_all(s_folder, ec);
                    fs::create_directories(s_folder, ec);
                    writeFeedback(s_folder / kFeedbackFile, line);
                    _exit(3);
                };
                g_onCallWild = [](uint32_t site, uint32_t target)
                {
                    g_onWild = nullptr;
                    g_onCallWild = nullptr;
                    g_countWild = false;
                    char line[96];
                    std::snprintf(line, sizeof(line), "W %x %x %x 0 2\n", g_block, site, target);
                    std::error_code ec;
                    fs::remove_all(s_folder, ec);
                    fs::create_directories(s_folder, ec);
                    writeFeedback(s_folder / kFeedbackFile, line);
                    _exit(3);
                };
            }
            int status = 0;
            try
            {
                if (c.invoke)
                {
                    // The scheduler's own kind of guest call, as ZX_GUEST_CALL's
                    // `~`: it may wait, and this copy of the game runs on meanwhile.
                    static auto s_leave = leave;
                    EeScheduler &scheduler = runtime->eeScheduler();
                    GuestInvocation invocation{};
                    invocation.kind = GuestInvocationKind::HleCall;
                    invocation.context = *ctx;
                    g_caseSp = scheduler.invocationStackTop() - 0x40u;
                    ee::setGpr64(&invocation.context, ee::reg::sp, g_caseSp);
                    ee::setGpr64(&invocation.context, ee::reg::ra, 0u);
                    invocation.context.pc = c.entry;
                    invocation.context.in_delay_slot = false;
                    invocation.onComplete = [](const R5900Context &, R5900Context &) { s_leave(0); };
                    ctx->pc = g_hookAddress;
                    scheduler.invokeCurrent(std::move(invocation));
                }
                const uint32_t sp = (static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)) & ~15u) - kCallStackDrop;
                g_caseSp = sp;
                status = runtime->callGuestFunction(rdram, ctx, c.entry, {}, sp, nullptr) ? 0 : 1;
            }
            catch (...)
            {
                status = 2; // (nothing a case throws may reach the batch's loop: that loop is the parent's)
            }
            leave(status);
            _exit(status);
        }

        constexpr uint32_t kStatusExl = 2u;
        constexpr uint64_t kOwnMemoryLimit = 4ull << 30;
        bool g_lastFaulted = false; // adoptRecording's last verdict
        // target:blocks: how many recordings that path keeps, and the paths that told their reads at
        // this snapshot (the search's seeds hold this state's addresses). A path keeps a few recordings
        // and more with other inputs: a difference may need one value on it, not another (a 64-bit
        // number on %ld's path, a table's entry that is not zero).
        constexpr uint64_t kOtherInputs = 8u; // of the plain limit
        std::unordered_map<std::string, uint64_t> g_paths;
        std::unordered_set<std::string> g_snapshotPaths;
        std::unordered_set<std::string> g_pathInputs; // target:blocks:the hash of a kept case's arguments and reads

        uint64_t memoryOf(pid_t pid)
        {
#if defined(__APPLE__)
            rusage_info_current info{};
            if (proc_pid_rusage(pid, RUSAGE_INFO_CURRENT, reinterpret_cast<rusage_info_t *>(&info)) == 0)
                return info.ri_phys_footprint;
#else
            (void)pid;
#endif
            return 0;
        }

        // The child's recording moved to rec/<address>.fnrec, the case's ID its
        // sequence number; the bytes moved, 0 if it kept none.
        uint64_t adoptRecording(const fs::path &temp, const fs::path &rec, uint32_t target, uint64_t sequence, bool keep)
        {
            g_lastFaulted = false;
            const std::string name = fncheck::recordingName(target, g_game);
            std::string bytes; // (the child's recording's plain bytes, compressed or not: fnrec_file.h)
            {
                fncheck::RecordingReader child;
                if (!child.open(temp / name))
                    return 0;
                bytes.assign(std::istreambuf_iterator<char>(child.stream()), std::istreambuf_iterator<char>());
            }
            std::error_code ec;
            fs::remove(temp / name, ec);
            if (bytes.size() <= kFileHeaderBytes + kSequenceOffset + 8u)
                return 0;
            // An address error signalled in the call (the runtime's checked
            // accesses: EXL set at the return): an invalid input, not kept.
            {
                std::istringstream record(bytes);
                record.seekg(static_cast<std::streamoff>(kFileHeaderBytes));
                fncheck::Record r;
                g_lastFaulted = !fncheck::readRecord(record, r, fncheck::kRecordVersion) ||
                                ((r.exit.cop0_status & ~r.entry.cop0_status) & kStatusExl) != 0u;
                // (The runtime takes a wild pointer into the BIOS for I/O: no registers there.)
                for (const fncheck::IoAccess &access : r.io)
                    if (access.address >= 0x20000000u)
                        g_lastFaulted = true;
                if (g_lastFaulted)
                    return 0;
            }
            if (!keep)
                return 0;
            std::memcpy(&bytes[kFileHeaderBytes + kSequenceOffset], &sequence, 8);
            const fs::path merged = rec / name;
            const bool fresh = !fs::exists(merged);
            // A file another recorder version began has records of its layout: these are not appended to it.
            if (!fresh)
            {
                fncheck::RecordingReader had;
                had.open(merged);
                fncheck::FileHeader header;
                if (!fncheck::readHeader(had.stream(), header, nullptr) || header.version != fncheck::kRecordVersion)
                {
                    std::fprintf(stderr, "[diffcall] %s: recorded by another recorder version, not appended to\n",
                                 merged.string().c_str());
                    return 0;
                }
            }
            fncheck::RecordingWriter out;
            if (fresh)
            {
                fncheck::FileHeader header;
                std::memcpy(&header, bytes.data(), kFileHeaderBytes);
                out.create(merged, header, fncheck::recorderCompresses());
            }
            else
                out.append(merged);
            out.writePlain(bytes.data() + kFileHeaderBytes, bytes.size() - kFileHeaderBytes);
            return bytes.size() - kFileHeaderBytes;
        }

        std::string runCase(const Case &c, uint64_t sequence, const fs::path &folder, uint8_t *rdram, R5900Context *ctx,
                            PS2Machine *runtime, uint32_t limitMs, uint64_t capBytes, std::ostream &reads)
        {
            const auto start = std::chrono::steady_clock::now();
            const fs::path temp = folder / "tmp", rec = folder / "rec";
            std::fflush(nullptr);
            const pid_t pid = fork();
            if (pid < 0)
                return "fork-failed 0 0";
            if (pid == 0)
                runCaseChild(c, temp, rdram, ctx, runtime, limitMs);
            int status = 0;
            bool done = false, overCap = false, overTime = false;
            uint64_t baseline = 0;
            for (uint32_t wait = 20; !done;)
            {
                const pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid)
                {
                    done = true;
                    break;
                }
                const uint64_t used = memoryOf(pid);
                if (!baseline)
                    baseline = used;
                if (capBytes && used > baseline + capBytes)
                    overCap = true;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                if (ms > static_cast<int64_t>(limitMs) + 2000)
                    overTime = true;
                if (overCap || overTime)
                {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    break;
                }
                usleep(wait);
                wait = wait < 2000u ? wait * 2u : wait;
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::string what;
            if (overCap)
                what = "memory";
            else if (overTime || (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM))
                what = "hang";
            else if (WIFSIGNALED(status))
                what = "signal" + std::to_string(WTERMSIG(status));
            else if (WEXITSTATUS(status) == 3)
                what = "fault";
            else if (WEXITSTATUS(status) == 2)
                what = "waited";
            else if (WEXITSTATUS(status) == 1)
                what = "noreturn";
            else
                what = "returned";
            // The search: the blocks the case ran, and whether their set is new.
            std::string blocks, details, pathKey, inputKey;
            bool newPath = true, newHere = true;
            if (std::ifstream feedback(temp / kFeedbackFile); feedback)
            {
                std::getline(feedback, blocks);
                details.assign(std::istreambuf_iterator<char>(feedback), std::istreambuf_iterator<char>());
                const std::string key = std::to_string(c.target) + ":" + blocks;
                const std::string arguments(reinterpret_cast<const char *>(c.a), sizeof(c.a));
                inputKey = key + ":" + std::to_string(std::hash<std::string>{}(arguments + details));
                const auto seen = g_paths.find(key);
                const uint64_t kept = seen == g_paths.end() ? 0u : seen->second, limit = envNumber("ZX_DIFFCALL_PATH_KEEP", 4u);
                newPath = kept < limit || (kept < limit * kOtherInputs && g_pathInputs.find(inputKey) == g_pathInputs.end());
                newHere = g_snapshotPaths.find(key) == g_snapshotPaths.end();
                feedback.close();
                std::error_code ec;
                fs::remove(temp / kFeedbackFile, ec);
                pathKey = key;
            }
            const uint64_t bytes = adoptRecording(temp, rec, c.target, sequence, newPath);
            if (what == "returned" && !g_lastFaulted && !pathKey.empty())
            {
                if (bytes)
                {
                    ++g_paths[pathKey];
                    g_pathInputs.insert(inputKey);
                }
                g_snapshotPaths.insert(pathKey);
            }
            if (what == "returned" && g_lastFaulted)
                what = "fault";
            else if (what == "returned")
                what = bytes ? "kept" : "notkept";
            if (what == "fault" || (newHere && (what == "kept" || what == "notkept")))
                reads << '#' << c.id << '\n' << details;
            char line[96];
            std::snprintf(line, sizeof(line), "%s %llu %lld", what.c_str(), static_cast<unsigned long long>(bytes),
                          static_cast<long long>(ms));
            return blocks.empty() ? std::string(line) : std::string(line) + " " + blocks;
        }

        uint64_t hashGuestMemory(const uint8_t *rdram)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            auto mix = [&](const uint8_t *p, size_t n)
            {
                for (size_t i = 0; i + 8 <= n; i += 8)
                {
                    uint64_t w;
                    std::memcpy(&w, p + i, 8);
                    h = (h ^ w) * 0x100000001B3ull;
                }
            };
            mix(rdram, PS2_RAM_SIZE);
            mix(ps2GetScratchpadHostPtr(), PS2_SCRATCHPAD_SIZE);
            return h;
        }

        void runBatch(const fs::path &folder, const fs::path &file, uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t limitMs = static_cast<uint32_t>(envNumber("ZX_DIFFCALL_CASE_MS", 1000u));
            const uint64_t capBytes = envNumber("ZX_DIFFCALL_CASE_MB", 1024u) << 20;
            const uint64_t before = hashGuestMemory(rdram);
            fs::path outName = file;
            outName.replace_extension(".out");
            std::ofstream out(outName);
            fs::path readsName = file;
            readsName.replace_extension(".reads");
            std::ofstream reads(readsName);
            std::ifstream in(file);
            std::string line;
            uint64_t count = 0;
            std::map<uint32_t, uint32_t> hangs;
            while (std::getline(in, line))
            {
                Case c;
                if (line.empty() || line[0] == '#' || !parseCase(line, c))
                    continue;
                const uint64_t sequence = std::strtoull(c.id.c_str(), nullptr, 0);
                if (hangs[c.target] >= 3u)
                {
                    out << c.id << " skipped 0 0" << std::endl;
                    continue;
                }
                const std::string result = runCase(c, sequence, folder, rdram, ctx, runtime, limitMs, capBytes, reads);
                if (++count % 16u == 0u && memoryOf(getpid()) > kOwnMemoryLimit)
                {
                    // (Seen once, a run memguard ended at 10 GB in the parent, 16 s into a batch, and
                    // never again: the game's copy stops here, leaving what it held for a look.)
                    std::fprintf(stderr, "[diffcall] BUG: the game's own memory passed its limit at case %s\n", c.id.c_str());
                    const std::string look = "vmmap -summary " + std::to_string(getpid()) + " > '" + (folder / "own-memory.txt").string() + "' 2>&1; sample " +
                                             std::to_string(getpid()) + " 1 -file '" + (folder / "own-memory.sample").string() + "' > /dev/null 2>&1";
                    if (std::system(look.c_str()) != 0)
                        std::fprintf(stderr, "[diffcall] the memory report failed\n");
                    out << c.id << ' ' << result << '\n';
                    break;
                }
                if (result.rfind("hang", 0) == 0)
                    ++hangs[c.target];
                out << c.id << ' ' << result << '\n';
                out.flush();
            }
            const uint64_t after = hashGuestMemory(rdram);
            if (before != after)
                std::fprintf(stderr, "[diffcall] BUG: the game's memory changed across a batch of %s\n", folder.c_str());
            out.close();
            fs::path doneName = file;
            doneName.replace_extension(".done");
            std::ofstream(doneName).put('\n');
            std::fprintf(stderr, "[diffcall] %s: %llu cases, memory %s\n", file.c_str(), static_cast<unsigned long long>(count),
                         before == after ? "unchanged" : "CHANGED");
        }

        void snapshot(const std::string &name, uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint64_t vsync = runtime->eeScheduler().currentVSyncTick();
            const fs::path folder = g_dir / ("snap-" + name);
            g_snapshotPaths.clear();
            fs::create_directories(folder / "rec");
            fs::create_directories(folder / "tmp");
            std::ofstream(folder / "ram.bin", std::ios::binary).write(reinterpret_cast<const char *>(rdram), PS2_RAM_SIZE);
            std::ofstream(folder / "scratch.bin", std::ios::binary)
                .write(reinterpret_cast<const char *>(ps2GetScratchpadHostPtr()), PS2_SCRATCHPAD_SIZE);
            {
                std::ofstream regs(folder / "regs.txt");
                regs << "vsync " << vsync << "\nthread " << runtime->eeScheduler().currentThreadId() << "\n";
                for (int i = 0; i < 32; ++i)
                {
                    char b[48];
                    std::snprintf(b, sizeof(b), "r%d %llx\n", i, static_cast<unsigned long long>(ee::gpr64(ctx, i)));
                    regs << b;
                }
            }
            if (!g_bank.empty())
                writeBank(folder / "bank.tsv");
            std::ofstream(folder / "ready").put('\n');
            std::fprintf(stderr, "[diffcall] snapshot %s at vsync %llu\n", name.c_str(), static_cast<unsigned long long>(vsync));

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(envNumber("ZX_DIFFCALL_WAIT", 3600u));
            for (uint32_t n = 0; std::chrono::steady_clock::now() < deadline;)
            {
                if (fs::exists(folder / "finish"))
                    break;
                const fs::path file = folder / ("batch-" + std::to_string(n) + ".cases");
                if (fs::exists(file))
                {
                    runBatch(folder, file, rdram, ctx, runtime);
                    ++n;
                    continue;
                }
                usleep(20000);
            }
            std::fprintf(stderr, "[diffcall] snapshot %s done\n", name.c_str());
        }

        void hookEntry(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint64_t vsync = runtime->eeScheduler().currentVSyncTick();
            g_bankTick = static_cast<uint32_t>(vsync) + 1u;
            if (g_triggerMode)
            {
                const fs::path trigger = g_dir / "trigger";
                std::error_code ec;
                if (fs::exists(trigger, ec))
                {
                    std::string name;
                    std::ifstream(trigger) >> name;
                    if (name.empty())
                        name = std::to_string(g_snapshots);
                    fs::remove(trigger, ec);
                    ++g_snapshots;
                    snapshot(name, rdram, ctx, runtime);
                }
            }
            else if (!g_vsyncs.empty() && vsync >= g_vsyncs.front())
            {
                g_vsyncs.erase(g_vsyncs.begin());
                snapshot(std::to_string(g_snapshots++), rdram, ctx, runtime);
            }
            ctx->pc = g_hookAddress;
            g_hookPrevious(rdram, ctx, runtime);
        }
    }

    void noteBlock(uint32_t block) { logBlock(block); }
    void noteRead(uint32_t address, uint32_t size) { logRead(address, size); }
    void noteCallReturn(R5900Context *ctx, uint32_t returnAddress, uint32_t target) { logCallReturn(ctx, returnAddress, target); }

    // A jump through a made-up pointer: no code there. The case ends as a wild access does, naming the call.
    void noteCall(uint32_t returnAddress, uint32_t target)
    {
        if (g_onCallWild && (target < (1u << 20) || target >= 0x02000000u))
            g_onCallWild(returnAddress - 8u, target);
    }

    void install(PS2Machine &runtime, game::Game game)
    {
        const char *spec = std::getenv("ZX_DIFFCALL");
        const char *dir = std::getenv("ZX_DIFFCALL_DIR");
        if (!spec || !*spec)
            return;
        if (!dir || !*dir)
        {
            std::fprintf(stderr, "[diffcall] ZX_DIFFCALL needs ZX_DIFFCALL_DIR\n");
            return;
        }
        g_game = game;
        g_dir = dir;
        const std::string all(spec);
        const size_t at = all.find('@');
        if (at == std::string::npos)
        {
            std::fprintf(stderr, "[diffcall] not HOOK@V1,V2,... or HOOK@trigger: %s\n", spec);
            return;
        }
        g_hookAddress = static_cast<uint32_t>(std::strtoul(all.c_str(), nullptr, 0));
        const std::string when = all.substr(at + 1);
        if (when == "trigger")
            g_triggerMode = true;
        else
            for (const std::string &v : split(when, ','))
                g_vsyncs.push_back(std::strtoull(v.c_str(), nullptr, 0));
        if (envNumber("ZX_DIFFCALL_BANK", 0u) != 0u)
            installBank(runtime, game);
        if (!runtime.hasFunction(g_hookAddress))
        {
            std::fprintf(stderr, "[diffcall] no function at 0x%x\n", g_hookAddress);
            return;
        }
        g_hookPrevious = runtime.lookupFunction(g_hookAddress);
        if (!runtime.replaceFunction(g_hookAddress, &hookEntry))
            return;
        std::fprintf(stderr, "[diffcall] snapshots at 0x%x: %s\n", g_hookAddress, when.c_str());
    }
#endif
}
