// The library ports (runtime/src/game/sce) against the recompiled code they port. Libraries are never
// rewritten: each port follows its pinned source (newlib 1.8.2, fdlibm, libgcc), and what proves it is that it
// does what the game's own code does. Play reaches few of these functions, so these tests call them, on inputs
// that cover their source's branches and edge values, against AI's recompiled originals (library_ports.tsv;
// tests/CMakeLists.txt builds each from AI's generated code with fncheck's tracing and block markers). Both run
// from the same memory, AI's ELF loaded and the case's inputs written, and are compared on the result and on
// every byte of RAM afterwards, the stack below the call's sp left out (dead once it returns).
//
// While an original runs, every port's table slot is hooked, so each port it calls on the way is compared too,
// from the state the original reached there (strtod's calls of _multadd, vfprintf's of _dtoa_r). Callees run as
// their originals in both runs, so each port is checked on its own, as zx-fncheck checks a recorded call. The
// system, the heap and fflush under the library are the test's own (write, read, malloc and kin), with their
// state in guest memory, so that both runs see the same. Coverage is the original's basic blocks (its block
// markers) that the compared calls ran.
//
// DM2's and BMX XXX's unrecorded library functions are this code (game/<game>/libraries.tsv: AI's, masked words
// equal; DM2's _vfprintf_r is the same source with other inlining).
#include "MiniTest.h"

#include "ee/abi.h"
#include "ee/fncheck_build.h"
#include "ee/readable.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "sce/libc.h"
#include "sce/libgcc.h"
#include "sce/libm.h"
#include "sce/newlib.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// A registration's file is named from the repository's top, whatever folder the checkout is in (ee/readable.h).
static_assert(std::string_view(game::readable::sourcePath<"/srv/build/z-axis/wt/lane/runtime/src/game/zge/a.cpp">) ==
              "runtime/src/game/zge/a.cpp");
static_assert(std::string_view(game::readable::sourcePath<"C:\\w\\wt\\slot1\\runtime\\src\\game\\b.h">) ==
              "runtime/src/game/b.h");
static_assert(std::string_view(game::readable::sourcePath<"../../runtime/src/game/sce/libm.cpp">) ==
              "runtime/src/game/sce/libm.cpp");
static_assert(std::string_view(game::readable::sourcePath<"/elsewhere/c.cpp">) == "c.cpp");

namespace sce::libc
{
    // (no header declares these three)
    int32_t vfprintf(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments);
    ee::Ptr<const char> __sccl(ee::Ptr<char> tab, ee::Ptr<const char> fmt);
    uint64_t _strtod_r(ee::Ptr<Reent> reent, ee::Ptr<const char> s00, ee::Ptr<ee::Ptr<const char>> se);
}

namespace
{
    // A function of each library source, so that its object is linked: the test library is an archive, and the
    // linker leaves out an object nothing names, and with it the ports it registers.
    const void *const kLibrarySources[] = {
        reinterpret_cast<const void *>(&sce::libm::isinf),          reinterpret_cast<const void *>(&sce::libc::_Balloc),
        reinterpret_cast<const void *>(&sce::libc::_strtod_r),      reinterpret_cast<const void *>(&sce::libc::vfprintf),
        reinterpret_cast<const void *>(&sce::libc::__sccl),         reinterpret_cast<const void *>(&sce::libc::ungetc),
        reinterpret_cast<const void *>(&sce::libc::__malloc_lock),  reinterpret_cast<const void *>(&sce::libc::strrchr),
        reinterpret_cast<const void *>(&sce::libgcc::__fixdfdi),
    };

    enum class LibraryRole
    {
        port,
        callee,
        system,
        data,
    };
    struct LibrarySymbol
    {
        const char *name;
        uint32_t address;
        LibraryRole role;
        const char *library; // a port's source
    };
#if __has_include("library_ports_table.h")
#include "library_ports_table.h"
#else
    inline constexpr LibrarySymbol kLibrarySymbols[] = {{"", 0u, LibraryRole::data, ""}}; // (a build without the table)
#endif

    using sce::libc::File;
    using sce::libc::Reent;
    using sce::libc::Bigint;
    using sce::libc::IoRequest;
    using sce::libc::IoVector;

    const LibrarySymbol *symbolNamed(const std::string &name)
    {
        for (const LibrarySymbol &s : kLibrarySymbols)
            if (name == s.name)
                return &s;
        return nullptr;
    }
    uint32_t addressOf(const char *name)
    {
        const LibrarySymbol *s = symbolNamed(name);
        if (!s)
        {
            std::fprintf(stderr, "[library] %s is not in library_ports.tsv\n", name);
            std::abort();
        }
        return s->address;
    }

    // Where a case's things go in guest memory (AI's ELF ends below 0x00300000).
    namespace layout
    {
        constexpr uint32_t kReturn = 0x00000100u;          // a call the test makes returns here: no code
        constexpr uint32_t kCompared = PS2_LEGACY_RAM_SIZE; // the memory compared after each call
        constexpr uint32_t kLowEnd = 0x00400000u;          // the ELF and what follows it, compared after each call
        constexpr uint32_t kInputs = 0x01800000u;          // a case's strings, records and buffers
        constexpr uint32_t kInputsEnd = 0x01900000u;
        constexpr uint32_t kWriteLog = 0x01900000u;        // what write and fflush got: its length, then the bytes
        constexpr uint32_t kWriteLogEnd = 0x01980000u;
        constexpr uint32_t kReadFile = 0x01980000u;        // what read gives: its size, the place, then the bytes
        constexpr uint32_t kReadFileEnd = 0x019F0000u;
        constexpr uint32_t kHeapNext = 0x019FFFF0u;        // the test heap's next free byte
        constexpr uint32_t kHeap = 0x01A00000u;
        constexpr uint32_t kHeapEnd = 0x01E00000u;
        constexpr uint32_t kStackTop = 0x01F00000u;        // sp at a call the test makes
        constexpr uint32_t kStackRoom = 0x00080000u;       // the stack below it a call may use
        constexpr uint32_t kDeadStack = 0x00010000u;       // below a call's sp: its callees' frames, dead after it
    }

    // The test's system: the descriptors it knows.
    constexpr int32_t kConsole = 1;     // a terminal (write appends to the log)
    constexpr int32_t kDiskFile = 3;    // a regular file (read gives the read file, seeks work)
    constexpr int32_t kBrokenFile = 9;  // every call on it fails with EIO
    constexpr int32_t kEio = 5, kEbadf = 9;

    struct PortStats
    {
        const LibrarySymbol *symbol = nullptr;
        uint64_t calls = 0, same = 0, inPlace = 0; // compared calls, identical ones, those reached inside an original
        std::vector<std::string> differences;     // the first few
        std::set<uint32_t> blocks;                // the original's blocks the compared calls ran
        uint32_t blockCount = 0;
        uint32_t caseCalls = 0;                   // compared inside originals in the running case (capped)
    };

    class Library;
    Library *g_library = nullptr;

    void portHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void systemWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void systemRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void systemClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void systemSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void systemStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void systemIsTerminal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void heapAllocate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void heapFree(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void heapAllocateZeroed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void heapReallocate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void flushFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // The original's blocks the running compared call runs (the innermost one).
    struct CoverageSink : fncheck::BlockSink
    {
        uint32_t function = 0;
        PortStats *stats = nullptr;
        void hit(const fncheck::BlockTable &table, uint32_t block) override
        {
            if (!stats || table.function != function || table.game != game::Game::Ai)
                return;
            stats->blocks.insert(block);
            stats->blockCount = table.count;
        }
    };

    class Library
    {
    public:
        static constexpr uint32_t kInPlacePerCase = 8;  // calls of a port compared inside one case's originals
        static constexpr uint32_t kInPlaceTotal = 200;  // ... and over all cases

        bool ready = false;
        std::string why; // why not
        PS2Runtime runtime;
        uint8_t *ram = nullptr;
        std::vector<uint8_t> image; // AI's ELF loaded: the memory every case starts from
        uint32_t gp = 0;
        std::map<uint32_t, PortStats> ports;
        std::string label; // the running case
        int depth = 0;     // compared calls running
        int readableRuns = 0;
        bool hooks = true;
        std::vector<std::vector<uint8_t>> snapshots;
        uint32_t elfEnd = 0;
        std::vector<std::string> caseProblems; // memory a case changed outside what a call may change
        CoverageSink sink;
        uint32_t inputs = layout::kInputs;

        Library()
        {
            if (!runtime.memory().initialize())
            {
                why = "no guest memory";
                return;
            }
            ram = runtime.memory().getRDRAM();
            for (const void *source : kLibrarySources)
                if (!source)
                    why = "a library source is not linked";
#ifndef ZX_LIBRARY_ORIGINALS
            why = "this build has no recompiled originals (no AI generated code: ZX_RUNNER_GENERATED_DIR)";
            return;
#else
            std::string elf;
            if (const char *path = std::getenv("ZX_TEST_AI_ELF"); path && path[0])
                elf = path;
#ifdef ZX_TEST_AI_ELF
            if (elf.empty())
                elf = ZX_TEST_AI_ELF;
#endif
            if (elf.empty())
            {
                why = "no AI ELF (ZX_TEST_AI_ELF)";
                return;
            }
            if (!loadElf(elf))
                return;
            for (const LibrarySymbol &s : kLibrarySymbols)
            {
                if (s.role == LibraryRole::port)
                {
                    PortStats &p = ports[s.address];
                    p.symbol = &s;
                    const game::readable::Function *f = game::readable::find(s.address);
                    if (!f || !f->readable)
                    {
                        why = std::string("no readable port registered for ") + s.name;
                        return;
                    }
                }
                if ((s.role == LibraryRole::port || s.role == LibraryRole::callee) &&
                    !game::readable::tracedOriginal(s.address))
                {
                    why = std::string("no recompiled original built for ") + s.name;
                    return;
                }
            }
            gp = crt0Gp();
            if (!gp)
            {
                why = "crt0's gp not found in the ELF's start";
                return;
            }
            snapshots.assign(4, std::vector<uint8_t>(layout::kCompared));
            ready = true;
#endif
        }

        bool loadElf(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> elf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto u32 = [&](size_t at) { uint32_t v = 0; if (at + 4 <= elf.size()) std::memcpy(&v, &elf[at], 4); return v; };
            auto u16 = [&](size_t at) { uint16_t v = 0; if (at + 2 <= elf.size()) std::memcpy(&v, &elf[at], 2); return v; };
            if (elf.size() < 0x34 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0)
            {
                why = "not an ELF: " + path;
                return false;
            }
            image.assign(layout::kCompared, 0);
            const uint32_t phoff = u32(0x1C);
            const uint16_t phentsize = u16(0x2A), phnum = u16(0x2C);
            for (uint32_t i = 0; i < phnum; ++i)
            {
                const size_t h = phoff + static_cast<size_t>(i) * phentsize;
                if (u32(h) != 1u) // PT_LOAD
                    continue;
                const uint32_t offset = u32(h + 4), vaddr = u32(h + 8), filesz = u32(h + 16), memsz = u32(h + 20);
                if (vaddr + memsz > layout::kInputs || offset + filesz > elf.size())
                {
                    why = "an unexpected segment in " + path;
                    return false;
                }
                std::memcpy(&image[vaddr], &elf[offset], filesz);
                elfEnd = std::max(elfEnd, (vaddr + memsz + 15u) & ~15u);
            }
            return true;
        }

        uint32_t word(uint32_t at) const
        {
            uint32_t v;
            std::memcpy(&v, ram + at, 4);
            return v;
        }
        void setWord(uint32_t at, uint32_t v) { std::memcpy(ram + at, &v, 4); }

        // gp as crt0 sets it: start's lui and addiu into a register, then daddu gp, it, zero.
        uint32_t crt0Gp() const
        {
            const uint32_t start = addressOf("start");
            uint32_t regs[32] = {};
            for (uint32_t i = 0; i < 64; ++i)
            {
                uint32_t insn;
                std::memcpy(&insn, &image[start + 4u * i], 4);
                const uint32_t op = insn >> 26, rs = (insn >> 21) & 31u, rt = (insn >> 16) & 31u, rd = (insn >> 11) & 31u;
                if (op == 0x0Fu) // lui
                    regs[rt] = insn << 16;
                else if (op == 0x09u) // addiu
                    regs[rt] = regs[rs] + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(insn & 0xFFFFu)));
                else if (op == 0 && (insn & 0x3Fu) == 0x2Du && rd == 28u && rt == 0u) // daddu gp, rs, zero
                    return regs[rs];
            }
            return 0;
        }

        // A case starts from the ELF's memory, the test system's files empty and its heap unused, every
        // function of library_ports.tsv in the table (ports hooked).
        void fresh(const std::string &caseLabel)
        {
            if (!label.empty())
                checkOutside();
            label = caseLabel;
            std::memcpy(ram, image.data(), layout::kCompared);
            std::memset(ram + layout::kCompared, 0, PS2_RAM_SIZE - layout::kCompared);
            setWord(layout::kHeapNext, layout::kHeap);
            setWord(layout::kWriteLog, 0);
            setWord(layout::kReadFile, 0);
            setWord(layout::kReadFile + 4, 0);
            inputs = layout::kInputs;
            ee::g_ram = ram;
            for (auto &[address, p] : ports)
                p.caseCalls = 0;
            for (const LibrarySymbol &s : kLibrarySymbols)
            {
                if (s.role == LibraryRole::port)
                    runtime.replaceFunction(s.address, &portHook);
                else if (s.role == LibraryRole::callee)
                    runtime.replaceFunction(s.address, game::readable::tracedOriginal(s.address));
            }
            const std::pair<const char *, PS2Runtime::RecompiledFunction> system[] = {
                {"write", &systemWrite},        {"read", &systemRead},          {"_close", &systemClose},
                {"_lseek", &systemSeek},        {"_fstat", &systemStatus},      {"isatty", &systemIsTerminal},
                {"_malloc_r", &heapAllocate},   {"_free_r", &heapFree},         {"_calloc_r", &heapAllocateZeroed},
                {"_realloc_r", &heapReallocate}, {"fflush", &flushFile},
            };
            for (const auto &[name, fn] : system)
                runtime.replaceFunction(addressOf(name), fn);
            // (no resume slots, ee::installSuspendableCalls: the ports whose callees can wait then run their calls
            // in place, as nothing here waits; this suite runs before the ones that install them)
        }

        // ---- the memory a call may change ----

        // The ELF's, the case's inputs, the test system's files, the heap so far and the stack: a call is compared
        // on these, and the rest of memory has to stay as the case found it (checked when the case ends).
        struct Range
        {
            uint32_t from, to;
        };
        uint32_t heapTop() const { return std::clamp(word(layout::kHeapNext), layout::kHeap, layout::kHeapEnd); }
        uint32_t logEnd() const
        {
            return std::min(layout::kWriteLog + 4u + word(layout::kWriteLog), layout::kWriteLogEnd);
        }
        std::vector<Range> live(uint32_t heap, uint32_t log) const
        {
            const uint32_t readEnd = std::min(layout::kReadFile + 8u + word(layout::kReadFile), layout::kReadFileEnd);
            return {{0u, std::max(elfEnd, layout::kLowEnd)},
                    {layout::kInputs, layout::kInputsEnd},
                    {layout::kWriteLog, log},
                    {layout::kReadFile, std::max(readEnd, layout::kReadFile + 8u)},
                    {layout::kHeapNext, layout::kHeapNext + 4u},
                    {layout::kHeap, heap},
                    {layout::kStackTop - layout::kStackRoom, layout::kStackTop + 0x1000u}};
        }
        void save(std::vector<uint8_t> &to, const std::vector<Range> &ranges) const
        {
            for (const Range &r : ranges)
                std::memcpy(&to[r.from], ram + r.from, r.to - r.from);
        }
        void load(const std::vector<uint8_t> &from, const std::vector<Range> &ranges)
        {
            for (const Range &r : ranges)
                std::memcpy(ram + r.from, &from[r.from], r.to - r.from);
        }
        void clear(uint32_t from, uint32_t to)
        {
            if (to > from)
                std::memset(ram + from, 0, to - from);
        }
        // The first byte outside the live memory unlike the case's start (0: none), and those put back.
        uint32_t outsideChange() const
        {
            const std::vector<Range> ranges = live(heapTop(), logEnd());
            uint32_t at = 0;
            for (size_t i = 0; i <= ranges.size(); ++i)
            {
                const uint32_t to = i < ranges.size() ? ranges[i].from : layout::kCompared;
                if (to > at && std::memcmp(ram + at, &image[at], to - at) != 0)
                {
                    while (ram[at] == image[at])
                        ++at;
                    return at;
                }
                if (i < ranges.size())
                    at = std::max(at, ranges[i].to);
            }
            return 0;
        }
        void restoreOutside()
        {
            const std::vector<Range> ranges = live(heapTop(), logEnd());
            uint32_t at = 0;
            for (size_t i = 0; i <= ranges.size(); ++i)
            {
                const uint32_t to = i < ranges.size() ? ranges[i].from : layout::kCompared;
                if (to > at)
                    std::memcpy(ram + at, &image[at], to - at);
                if (i < ranges.size())
                    at = std::max(at, ranges[i].to);
            }
        }
        // Outside the live memory everything is as the ELF and the case's start left it.
        void checkOutside()
        {
            const std::vector<Range> ranges = live(heapTop(), logEnd());
            uint32_t at = 0;
            for (size_t i = 0; i <= ranges.size(); ++i)
            {
                const uint32_t to = i < ranges.size() ? ranges[i].from : layout::kCompared;
                if (to > at && std::memcmp(ram + at, &image[at], to - at) != 0)
                {
                    uint32_t b = at;
                    while (b < to && ram[b] == image[b])
                        ++b;
                    if (caseProblems.size() < 8)
                        caseProblems.push_back(label + ": memory no call may change changed, the first at " + hex(b));
                    break;
                }
                if (i < ranges.size())
                    at = std::max(at, ranges[i].to);
            }
        }

        // ---- a case's inputs ----

        uint32_t put(const void *bytes, uint32_t size, uint32_t align = 8)
        {
            inputs = (inputs + align - 1u) & ~(align - 1u);
            const uint32_t at = inputs;
            if (at + size > layout::kInputsEnd)
            {
                std::fprintf(stderr, "[library] a case's inputs pass their room\n");
                std::abort();
            }
            if (size)
                std::memcpy(ram + at, bytes, size);
            inputs += size;
            return at;
        }
        uint32_t zeros(uint32_t size, uint32_t align = 16)
        {
            std::vector<uint8_t> z(size, 0);
            return put(z.data(), size, align);
        }
        uint32_t text(const std::string &s) { return put(s.c_str(), static_cast<uint32_t>(s.size() + 1), 1); }
        uint32_t words(std::initializer_list<uint32_t> w)
        {
            std::vector<uint32_t> v(w);
            return put(v.data(), static_cast<uint32_t>(v.size() * 4), 8);
        }
        uint32_t reent() const { return word(addressOf("g_impurePtr")); }

        // ---- running ----

        R5900Context context(std::initializer_list<uint64_t> gprs, std::initializer_list<float> fprs) const
        {
            R5900Context ctx;
            for (int r = 1; r < 32; ++r)
                ee::setGpr64(&ctx, r, 0x5A5A5A5A00000000ull + static_cast<uint64_t>(r) * 0x01010101ull);
            for (int f = 0; f < 32; ++f)
                ctx.f[f] = static_cast<float>(f) + 0.25f;
            int r = 4; // a0 .. a7
            for (uint64_t v : gprs)
                ee::setGpr64(&ctx, r++, v);
            int f = 12; // f12 .. f19
            for (float v : fprs)
                ctx.f[f++] = v;
            ee::setGpr64(&ctx, 28, gp);
            ee::setGpr64(&ctx, ee::reg::sp, layout::kStackTop);
            ee::setGpr64(&ctx, ee::reg::ra, layout::kReturn);
            ctx.pc = 0;
            return ctx;
        }

        // Runs fn from ctx (its pc set) until it returns to ra, following tail jumps through the table.
        bool runTo(PS2Runtime::RecompiledFunction fn, R5900Context &ctx, uint32_t ra)
        {
            fn(ram, &ctx, &runtime);
            for (uint32_t hops = 0; ctx.pc != ra && hops < (1u << 24) && runtime.hasFunction(ctx.pc); ++hops)
                runtime.lookupFunction(ctx.pc)(ram, &ctx, &runtime);
            return ctx.pc == ra;
        }

        // A host function run as a guest call (PS2Runtime::callGuestFunction): EE safe points are held off
        // meanwhile, so nothing unwinds to a scheduler.
        static inline std::function<void()> s_held;
        static void heldBody(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_held();
            ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        }
        void held(const std::function<void()> &body)
        {
            s_held = body;
            R5900Context scratch;
            (void)runtime.callGuestFunction(ram, &scratch, &heldBody, 0u, {}, layout::kStackTop);
            s_held = nullptr;
        }

        // The port at ctx->pc, its original and then the port from the same memory, compared; the original's
        // result and memory stay.
        void compare(R5900Context *ctx, uint32_t address, bool inPlace)
        {
            PortStats &p = ports.at(address);
            // ZX_LIBRARY_TRACE=1: each compared call on stderr as it starts
            static const bool trace = [] { const char *v = std::getenv("ZX_LIBRARY_TRACE"); return v && v[0] == '1'; }();
            if (trace)
            {
                std::fprintf(stderr, "[library] %s%s: %s (%s)\n", depth ? "  " : "", label.c_str(), p.symbol->name,
                             arguments(*ctx).c_str());
                std::fflush(stderr);
            }
            const game::readable::Function *f = game::readable::find(address);
            const R5900Context entry = *ctx;
            const uint32_t ra = static_cast<uint32_t>(ee::gpr64(&entry, ee::reg::ra));
            const uint32_t sp = static_cast<uint32_t>(ee::gpr64(&entry, ee::reg::sp));
            std::vector<uint8_t> &before = snapshots[2 * depth], &after = snapshots[2 * depth + 1];
            const uint32_t heapBefore = heapTop(), logBefore = logEnd();
            const std::vector<Range> was = live(heapBefore, logBefore);
            save(before, was);

            ++depth;
            const uint32_t outerFunction = sink.function;
            PortStats *const outerStats = sink.stats;
            fncheck::BlockSink *const outerSink = fncheck::t_blockSink;
            sink.function = address;
            sink.stats = &p;
            fncheck::t_blockSink = &sink;
            R5900Context original = entry;
            const bool originalReturned = runTo(game::readable::tracedOriginal(address), original, ra);
            fncheck::t_blockSink = outerSink;
            sink.function = outerFunction;
            sink.stats = outerStats;
            if (depth == 1)
                if (const uint32_t at = outsideChange())
                {
                    if (caseProblems.size() < 8)
                        caseProblems.push_back(label + ": " + p.symbol->name + "'s original wrote memory no call may change, at " +
                                               hex(at) + " (" + arguments(entry) + ")");
                    restoreOutside();
                }
            const uint32_t heapAfter = std::max(heapTop(), heapBefore), logAfter = std::max(logEnd(), logBefore);
            const std::vector<Range> ranges = live(heapAfter, logAfter);
            save(after, ranges);

            load(before, was);
            clear(heapBefore, heapAfter);
            clear(logBefore, logAfter);
            if (trace)
            {
                std::fprintf(stderr, "[library] %s  the original %s, v0 %s; the port\n", depth > 1 ? "  " : "",
                             originalReturned ? "returned" : "did not return", hex(ee::gpr64(&original, ee::reg::v0)).c_str());
                std::fflush(stderr);
            }
            R5900Context port = entry;
            ++readableRuns;
            const bool portReturned = runTo(f->readable, port, ra);
            --readableRuns;
            --depth;

            std::string difference;
            if (!originalReturned)
                difference = "the original did not return";
            else if (!portReturned)
                difference = "the port did not return";
            else
                difference = differs(f->returnKind, original, port, after, sp, ranges);
            ++p.calls;
            if (inPlace)
                ++p.inPlace;
            if (difference.empty())
                ++p.same;
            else if (p.differences.size() < 4)
                p.differences.push_back(label + ": " + difference + " (" + arguments(entry) + ")");
            if (depth == 0)
                if (const uint32_t at = outsideChange())
                {
                    if (caseProblems.size() < 8)
                        caseProblems.push_back(label + ": " + p.symbol->name + "'s port wrote memory no call may change, at " +
                                               hex(at) + " (" + arguments(entry) + ")");
                    restoreOutside();
                }
            const uint32_t heapPort = heapTop(), logPort = logEnd();
            load(after, ranges);
            clear(heapAfter, heapPort);
            clear(logAfter, logPort);
            *ctx = original;
        }

        static std::string hex(uint64_t v)
        {
            char b[24];
            std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
            return b;
        }
        static std::string arguments(const R5900Context &ctx)
        {
            std::string s = "a0-a3 ";
            for (int r = 4; r < 8; ++r)
                s += (r > 4 ? " " : "") + hex(ee::gpr64(&ctx, r));
            s += ", f12 " + hex(std::bit_cast<uint32_t>(ctx.f[12])) + " f13 " + hex(std::bit_cast<uint32_t>(ctx.f[13]));
            return s;
        }

        std::string differs(ee::ReturnKind kind, const R5900Context &original, const R5900Context &port,
                            const std::vector<uint8_t> &after, uint32_t sp, const std::vector<Range> &ranges) const
        {
            const uint64_t ov0 = ee::gpr64(&original, ee::reg::v0), pv0 = ee::gpr64(&port, ee::reg::v0);
            switch (kind)
            {
            case ee::ReturnKind::Void:
                break;
            case ee::ReturnKind::Int32:
            case ee::ReturnKind::Pointer:
                if (static_cast<uint32_t>(ov0) != static_cast<uint32_t>(pv0))
                    return "result " + hex(static_cast<uint32_t>(pv0)) + ", the original's " + hex(static_cast<uint32_t>(ov0));
                break;
            case ee::ReturnKind::Int64:
                if (ov0 != pv0)
                    return "result " + hex(pv0) + ", the original's " + hex(ov0);
                break;
            case ee::ReturnKind::Float:
                if (std::bit_cast<uint32_t>(original.f[0]) != std::bit_cast<uint32_t>(port.f[0]))
                    return "result " + hex(std::bit_cast<uint32_t>(port.f[0])) + ", the original's " +
                           hex(std::bit_cast<uint32_t>(original.f[0]));
                break;
            }
            const uint32_t deadLow = sp - layout::kDeadStack;
            uint32_t first = 0, count = 0;
            for (const Range &r : ranges)
            {
                for (uint32_t at = r.from; at < r.to; at += 4096u)
                {
                    const uint32_t end = std::min(at + 4096u, r.to);
                    if (std::memcmp(ram + at, &after[at], end - at) == 0)
                        continue;
                    for (uint32_t b = at; b < end; ++b)
                    {
                        if (ram[b] == after[b] || (b >= deadLow && b < sp))
                            continue;
                        if (count++ == 0)
                            first = b;
                    }
                }
            }
            if (count)
            {
                const uint32_t w = first & ~3u;
                uint32_t mine, theirs;
                std::memcpy(&mine, ram + w, 4);
                std::memcpy(&theirs, &after[w], 4);
                return std::to_string(count) + " bytes of memory differ, the first at " + hex(first) + ": word " + hex(mine) +
                       ", the original's " + hex(theirs);
            }
            return "";
        }

        // A call the test makes: the port and its original on these arguments.
        uint64_t call(const char *name, std::initializer_list<uint64_t> gprs = {}, std::initializer_list<float> fprs = {})
        {
            const uint32_t address = addressOf(name);
            R5900Context ctx = context(gprs, fprs);
            ctx.pc = address;
            held([&] { compare(&ctx, address, false); });
            lastFloat = ctx.f[0];
            return ee::gpr64(&ctx, ee::reg::v0);
        }
        float lastFloat = 0.0f;

        // A function's original alone, no port compared (a case's setup: a Bigint to work on).
        uint64_t original(const char *name, std::initializer_list<uint64_t> gprs = {}, std::initializer_list<float> fprs = {})
        {
            const uint32_t address = addressOf(name);
            R5900Context ctx = context(gprs, fprs);
            ctx.pc = address;
            const bool before = hooks;
            hooks = false;
            held([&] { (void)runTo(game::readable::tracedOriginal(address), ctx, layout::kReturn); });
            hooks = before;
            return ee::gpr64(&ctx, ee::reg::v0);
        }
    };

    void portHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Library &lib = *g_library;
        const uint32_t address = ctx->pc;
        PortStats &p = lib.ports.at(address);
        static const bool trace = [] { const char *v = std::getenv("ZX_LIBRARY_TRACE"); return v && v[0] == '1' && v[1] == '+'; }();
        if (trace) // ZX_LIBRARY_TRACE=1+: every call of a port too, as the table gets it
            std::fprintf(stderr, "[library]       %s %s %s\n", lib.readableRuns ? "port's" : "original's", p.symbol->name,
                         Library::arguments(*ctx).c_str());
        if (lib.hooks && lib.depth == 1 && lib.readableRuns == 0 && p.caseCalls < Library::kInPlacePerCase &&
            p.inPlace < Library::kInPlaceTotal)
        {
            ++p.caseCalls;
            lib.compare(ctx, address, true);
            return;
        }
        game::readable::tracedOriginal(address)(rdram, ctx, runtime);
    }

    // ---- the test's system, heap and fflush (both runs call them alike) ----

    uint32_t arg(R5900Context *ctx, int i) { return static_cast<uint32_t>(ee::gpr64(ctx, 4 + i)); }
    void returnWith(R5900Context *ctx, int64_t v)
    {
        ee::setGpr64(ctx, ee::reg::v0, static_cast<uint64_t>(v));
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void failWith(R5900Context *ctx, int32_t error)
    {
        g_library->setWord(addressOf("g_errno"), static_cast<uint32_t>(error));
        returnWith(ctx, -1);
    }
    bool knownDescriptor(int32_t fd) { return fd == 0 || fd == kConsole || fd == 2 || fd == kDiskFile; }

    void appendToLog(uint8_t *rdram, uint32_t from, uint32_t bytes)
    {
        Library &lib = *g_library;
        const uint32_t length = lib.word(layout::kWriteLog);
        const uint32_t room = layout::kWriteLogEnd - (layout::kWriteLog + 4u + length);
        const uint32_t n = std::min(bytes, room);
        std::memmove(rdram + layout::kWriteLog + 4u + length, rdram + (from & PS2_RAM_MASK), n);
        lib.setWord(layout::kWriteLog, length + n);
    }
    // write(fd, buffer, count): the bytes to the log.
    void systemWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const int32_t fd = static_cast<int32_t>(arg(ctx, 0));
        const int32_t count = static_cast<int32_t>(arg(ctx, 2));
        if (!knownDescriptor(fd) || count < 0)
            return failWith(ctx, fd == kBrokenFile ? kEio : kEbadf);
        appendToLog(rdram, arg(ctx, 1), static_cast<uint32_t>(count));
        returnWith(ctx, count);
    }
    // read(fd, buffer, count): the read file's next bytes.
    void systemRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        Library &lib = *g_library;
        const int32_t fd = static_cast<int32_t>(arg(ctx, 0));
        const int32_t count = static_cast<int32_t>(arg(ctx, 2));
        if (!knownDescriptor(fd) || count < 0)
            return failWith(ctx, fd == kBrokenFile ? kEio : kEbadf);
        const uint32_t size = lib.word(layout::kReadFile), place = lib.word(layout::kReadFile + 4);
        const uint32_t n = std::min(static_cast<uint32_t>(count), size - std::min(size, place));
        std::memmove(rdram + (arg(ctx, 1) & PS2_RAM_MASK), rdram + layout::kReadFile + 8u + place, n);
        lib.setWord(layout::kReadFile + 4, place + n);
        returnWith(ctx, n);
    }
    void systemClose(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const int32_t fd = static_cast<int32_t>(arg(ctx, 0));
        if (!knownDescriptor(fd))
            return failWith(ctx, fd == kBrokenFile ? kEio : kEbadf);
        returnWith(ctx, 0);
    }
    // lseek(fd, offset, whence): the disk file's place moves; a terminal cannot seek.
    void systemSeek(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        Library &lib = *g_library;
        const int32_t fd = static_cast<int32_t>(arg(ctx, 0));
        const int32_t offset = static_cast<int32_t>(arg(ctx, 1)), whence = static_cast<int32_t>(arg(ctx, 2));
        if (fd != kDiskFile)
            return failWith(ctx, fd == kBrokenFile ? kEio : 29); // ESPIPE
        const int32_t size = static_cast<int32_t>(lib.word(layout::kReadFile));
        const int32_t place = static_cast<int32_t>(lib.word(layout::kReadFile + 4));
        const int32_t to = whence == 0 ? offset : whence == 1 ? place + offset : size + offset;
        if (whence < 0 || whence > 2 || to < 0)
            return failWith(ctx, 22); // EINVAL
        lib.setWord(layout::kReadFile + 4, static_cast<uint32_t>(to));
        returnWith(ctx, to);
    }
    // fstat(fd, status): a terminal, or the disk file (512-byte blocks).
    void systemStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const int32_t fd = static_cast<int32_t>(arg(ctx, 0));
        const uint32_t status = arg(ctx, 1) & PS2_RAM_MASK;
        if (!knownDescriptor(fd))
            return failWith(ctx, fd == kBrokenFile ? kEio : kEbadf);
        std::memset(rdram + status, 0, sizeof(sce::libc::FileStatus));
        const uint32_t mode = fd == kDiskFile ? sce::libc::kRegularFile | 0644u : sce::libc::kCharacterDevice | 0620u;
        std::memcpy(rdram + status + offsetof(sce::libc::FileStatus, mode), &mode, 4);
        const int64_t blockSize = fd == kDiskFile ? 512 : 0;
        std::memcpy(rdram + status + offsetof(sce::libc::FileStatus, blockSize), &blockSize, 8);
        returnWith(ctx, 0);
    }
    void systemIsTerminal(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const int32_t fd = static_cast<int32_t>(arg(ctx, 0));
        returnWith(ctx, fd >= 0 && fd <= 2 ? 1 : 0);
    }

    // The heap: blocks one after another, each with its size in the word before it; free keeps them.
    uint32_t allocate(uint32_t bytes)
    {
        Library &lib = *g_library;
        const uint32_t next = lib.word(layout::kHeapNext);
        const uint32_t at = (next + 8u + 7u) & ~7u;
        if (bytes > layout::kHeapEnd - at)
            return 0;
        lib.setWord(at - 4u, bytes);
        lib.setWord(layout::kHeapNext, at + ((bytes + 7u) & ~7u));
        return at;
    }
    void heapAllocate(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnWith(ctx, allocate(arg(ctx, 1))); }
    void heapFree(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnWith(ctx, 0); }
    void heapAllocateZeroed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint64_t bytes = static_cast<uint64_t>(arg(ctx, 1)) * arg(ctx, 2);
        const uint32_t at = bytes > layout::kHeapEnd ? 0u : allocate(static_cast<uint32_t>(bytes));
        if (at)
            std::memset(rdram + at, 0, static_cast<size_t>(bytes));
        returnWith(ctx, at);
    }
    void heapReallocate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t old = arg(ctx, 1), bytes = arg(ctx, 2);
        const uint32_t at = allocate(bytes);
        if (at && old)
            std::memmove(rdram + at, rdram + old, std::min(bytes, g_library->word(old - 4u)));
        returnWith(ctx, at);
    }
    // fflush(fp): a writing FILE's buffered bytes to the log (newlib's __sflush, its write always whole).
    void flushFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        Library &lib = *g_library;
        const uint32_t fp = arg(ctx, 0);
        if (!fp)
            return returnWith(ctx, 0);
        int16_t flags;
        std::memcpy(&flags, rdram + fp + offsetof(File, flags), 2);
        const uint32_t base = lib.word(fp + offsetof(File, bufferBase));
        if ((flags & sce::libc::kCanWrite) == 0 || !base)
            return returnWith(ctx, 0);
        const uint32_t position = lib.word(fp + offsetof(File, position));
        appendToLog(rdram, base, position - base);
        lib.setWord(fp + offsetof(File, position), base);
        const int32_t size = static_cast<int32_t>(lib.word(fp + offsetof(File, bufferSize)));
        lib.setWord(fp + offsetof(File, writeSpace),
                    (flags & (sce::libc::kLineBuffered | sce::libc::kUnbuffered)) ? 0u : static_cast<uint32_t>(size));
        returnWith(ctx, 0);
    }

    Library &library()
    {
        static std::unique_ptr<Library> lib;
        if (!lib)
        {
            lib = std::make_unique<Library>();
            g_library = lib.get();
        }
        return *lib;
    }

    // Every readable function goes through the table while the tests run (game::call takes an enabled
    // one directly), so a port's callees are their originals.
    struct NothingEnabled
    {
        std::vector<std::pair<game::readable::Function *, bool>> saved;
        NothingEnabled()
        {
            for (game::readable::Function *f : game::readable::functions())
            {
                saved.emplace_back(f, f->enabled);
                f->enabled = false;
            }
        }
        ~NothingEnabled()
        {
            for (auto &[f, on] : saved)
                f->enabled = on;
        }
    };

    // ---- values ----

    uint64_t dbl(double d) { return std::bit_cast<uint64_t>(d); }
    uint64_t s64(int64_t v) { return static_cast<uint64_t>(v); }
    uint64_t s32(int32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(v)); }
    float flt(uint32_t bits) { return std::bit_cast<float>(bits); }

    const std::vector<double> &interestingDoubles()
    {
        static const std::vector<double> v = [] {
            std::vector<double> d = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, -1.5, 2.5, 9.5, 99.5, 0.1, 0.2, 0.3, 1.0 / 3.0,
                                     2.0 / 3.0, 3.14159265358979323846, 2.718281828459045, 123.456, -987654321.123,
                                     1e-5, 1e-10, 1e10, 1e15, 1e16, 1e17, 1e21, 1e22, 1e23, 1e100, 1e-100, 1e300, -1e300,
                                     1e308, 1.7976931348623157e308, 2.2250738585072014e-308, 2.2250738585072009e-308,
                                     4.9406564584124654e-324, 1e-320, 4294967295.0, 4294967296.0, 4294967295.5,
                                     2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0, 9007199254740992.0,
                                     9007199254740993.0, 18446744073709551615.0, 9223372036854775807.0,
                                     -9223372036854775808.0, 1.8e19, 999999.9999, 0.000123, 6.02214076e23, 5e-324};
            d.push_back(std::numeric_limits<double>::infinity());
            d.push_back(-std::numeric_limits<double>::infinity());
            d.push_back(std::bit_cast<double>(0x7FF8000000000000ull));
            d.push_back(std::bit_cast<double>(0xFFF8000000000001ull));
            d.push_back(std::bit_cast<double>(0x7FF0000000000001ull));
            d.push_back(std::bit_cast<double>(0x000FFFFFFFFFFFFFull));
            d.push_back(std::bit_cast<double>(0x8000000000000001ull));
            return d;
        }();
        return v;
    }
    const std::vector<uint32_t> &interestingFloats()
    {
        static const std::vector<uint32_t> v = {
            0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF000000u, 0x3FC00000u, 0xBFC00000u,
            0x40300000u, 0xC0300000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u, 0x4AFFFFFFu, 0x4B000000u,
            0x4B000001u, 0x7149F2CAu, 0xF149F2CAu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFC00001u,
            0x47F12065u, 0xB8D1B717u, 0x3F7FFFEFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3E99999Au, 0x41200000u, 0x461C3C00u,
            0xC61C3C00u, 0x3F490FDBu, 0x3F490FDCu, 0x4016CBE4u, 0x40490FDBu, 0x42C80000u, 0xC2C80000u, 0x461C4000u,
            0x43490F80u, 0x43490FDBu, 0x47C35000u, 0x501502F9u, 0x60AD78ECu, 0xD0000000u, 0x5F000000u, 0x4E6E6B28u};
        return v;
    }

    // ---- reports ----

    // Checks the ports of `library` (all with "" ): each compared, none differing. Prints each one's calls
    // and coverage.
    void report(TestCase &t, const std::string &library, bool final)
    {
        Library &lib = *g_library;
        uint32_t ports = 0, blocks = 0, hit = 0;
        for (auto &[address, p] : lib.ports)
        {
            if (!library.empty() && library != p.symbol->library)
                continue;
            ++ports;
            blocks += p.blockCount;
            hit += static_cast<uint32_t>(p.blocks.size());
            std::printf("\n    [library] %-22s %-20s %6llu calls (%llu inside originals), %6llu identical, blocks %u/%u",
                        p.symbol->library, p.symbol->name, static_cast<unsigned long long>(p.calls),
                        static_cast<unsigned long long>(p.inPlace), static_cast<unsigned long long>(p.same),
                        static_cast<uint32_t>(p.blocks.size()), p.blockCount);
            for (const std::string &d : p.differences)
                t.Fail(std::string(p.symbol->name) + ": " + d);
            if (final && p.calls == 0)
                t.Fail(std::string(p.symbol->name) + ": never compared");
        }
        if (final)
        {
            lib.checkOutside();
            for (const std::string &problem : lib.caseProblems)
                t.Fail(problem);
        }
        std::printf("\n    [library] %s: %u ports, blocks %u/%u\n", library.empty() ? "all" : library.c_str(), ports, hit, blocks);
    }

    bool start(TestCase &t)
    {
        Library &lib = library();
        if (!lib.ready)
        {
            std::printf("(not run: %s) ", lib.why.c_str());
            return false;
        }
        (void)t;
        return true;
    }
}

// ---- the cases ----

namespace
{
    namespace libc = sce::libc;

    void set16(Library &lib, uint32_t at, int16_t v) { std::memcpy(lib.ram + at, &v, 2); }
    uint32_t va(Library &lib, std::initializer_list<uint64_t> slots)
    {
        std::vector<uint64_t> v(slots);
        return lib.put(v.data(), static_cast<uint32_t>(v.size() * 8), 8);
    }

    // A FILE as sprintf makes one: writing into `bytes` of memory.
    uint32_t stringFile(Library &lib, uint32_t bytes)
    {
        const uint32_t buffer = lib.zeros(bytes + 1u);
        const uint32_t fp = lib.zeros(sizeof(File));
        set16(lib, fp + offsetof(File, flags), static_cast<int16_t>(libc::kIsString | libc::kCanWrite));
        set16(lib, fp + offsetof(File, descriptor), -1);
        lib.setWord(fp + offsetof(File, position), buffer);
        lib.setWord(fp + offsetof(File, bufferBase), buffer);
        lib.setWord(fp + offsetof(File, bufferSize), bytes);
        lib.setWord(fp + offsetof(File, writeSpace), bytes);
        lib.setWord(fp + offsetof(File, reent), lib.reent());
        return fp;
    }
    // A FILE on a descriptor with newlib's four functions, not set up yet (no buffer) unless `buffer`.
    uint32_t descriptorFile(Library &lib, int32_t fd, int flags, uint32_t buffer = 0, uint32_t bytes = 0)
    {
        const uint32_t fp = lib.zeros(sizeof(File));
        set16(lib, fp + offsetof(File, flags), static_cast<int16_t>(flags));
        set16(lib, fp + offsetof(File, descriptor), static_cast<int16_t>(fd));
        lib.setWord(fp + offsetof(File, cookie), fp);
        lib.setWord(fp + offsetof(File, readFunction), addressOf("__sread"));
        lib.setWord(fp + offsetof(File, writeFunction), addressOf("__swrite"));
        lib.setWord(fp + offsetof(File, seekFunction), addressOf("__sseek"));
        lib.setWord(fp + offsetof(File, closeFunction), addressOf("__sclose"));
        lib.setWord(fp + offsetof(File, reent), lib.reent());
        if (buffer)
        {
            lib.setWord(fp + offsetof(File, bufferBase), buffer);
            lib.setWord(fp + offsetof(File, position), buffer);
            lib.setWord(fp + offsetof(File, bufferSize), bytes);
        }
        return fp;
    }
    void readFile(Library &lib, const std::string &bytes)
    {
        if (bytes.size() > layout::kReadFileEnd - layout::kReadFile - 8u)
            std::abort();
        lib.setWord(layout::kReadFile, static_cast<uint32_t>(bytes.size()));
        lib.setWord(layout::kReadFile + 4, 0);
        std::memcpy(lib.ram + layout::kReadFile + 8, bytes.data(), bytes.size());
    }
    // A Bigint of the decimal digits (_s2b's original, from the first nine as y9), or of a small value.
    uint32_t bigint(Library &lib, const std::string &digits)
    {
        if (digits.size() <= 9)
            return static_cast<uint32_t>(lib.original("_i2b", {lib.reent(), s32(static_cast<int32_t>(std::stoul(digits)))}));
        const uint32_t s = lib.text(digits);
        const uint32_t y9 = static_cast<uint32_t>(std::stoul(digits.substr(0, 9)));
        const int32_t n = static_cast<int32_t>(digits.size());
        return static_cast<uint32_t>(lib.original("_s2b", {lib.reent(), s, s32(n), s32(n), y9}));
    }

    const char *const kNumbers[] = {"0", "1", "7", "4294967295", "1000000000", "123456789012345678901234567890",
                                    "340282366920938463463374607431768211456", "99999999999999999999999999999999999999999",
                                    "18446744073709551616", "10000000000000000000000000000000000000000000000000001",
                                    "2147483647", "65536", "31415926535897932384626433832795028841971693993751"};

    void libmCases(Library &lib)
    {
        lib.fresh("floorf");
        for (uint32_t x : interestingFloats())
            lib.call("floorf", {}, {flt(x)});
        for (float x : {0.25f, -0.25f, 0.999999f, -0.999999f, 3.5f, -3.5f, 1048575.5f, -1048575.5f, 8388607.5f, -8388607.5f,
                        65536.75f, -65536.75f, 1e-30f, -1e-30f})
            lib.call("floorf", {}, {x});

        lib.fresh("copysignf");
        for (uint32_t x : {0x00000000u, 0x80000000u, 0x3F800000u, 0xFF800000u, 0x7FC00000u, 0x00000001u})
            for (uint32_t y : {0x00000000u, 0x80000000u, 0xBF800000u, 0x7F800000u, 0xFFC00000u})
                lib.call("copysignf", {}, {flt(x), flt(y)});

        lib.fresh("scalbnf");
        for (uint32_t x : {0x00000000u, 0x80000000u, 0x3F800000u, 0xBFC00000u, 0x00000003u, 0x80000003u, 0x00400000u,
                           0x7F800000u, 0x7FC00000u, 0x7E967699u, 0x00A6F4E8u, 0x3F7FFFFFu})
            for (int32_t n : {0, 1, -1, 10, -10, 23, -23, 126, -126, 127, 128, 149, -149, 150, -150, 200, -200, 253, 254,
                              255, -255, 50000, 50001, -50000, -50001, 1000000, -1000000, std::numeric_limits<int32_t>::max(),
                              std::numeric_limits<int32_t>::min()})
                lib.call("scalbnf", {s32(n)}, {flt(x)});

        lib.fresh("isinf, isnan");
        for (double d : interestingDoubles())
        {
            lib.call("isinf", {dbl(d)});
            lib.call("isnan", {dbl(d)});
        }
        for (uint64_t bits : {0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF0000000000001ull, 0x7FF0000100000000ull,
                              0x7FEFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0x0000000000000001ull, 0x8000000000000000ull,
                              0x7FF00000FFFFFFFFull, 0x00000000FFFFFFFFull})
        {
            lib.call("isinf", {bits});
            lib.call("isnan", {bits});
        }

        lib.fresh("matherr");
        for (double d : {0.0, 1.0, -2.5, std::numeric_limits<double>::infinity(), std::bit_cast<double>(0x7FF8000000000000ull)})
        {
            // struct exception: type, name, arg1, arg2, retval, err
            const uint32_t e = lib.zeros(0x28, 8);
            lib.setWord(e, 1);
            std::memcpy(lib.ram + e + 8, &d, 8);
            std::memcpy(lib.ram + e + 16, &d, 8);
            lib.call("matherr", {e});
        }

        // __ieee754_rem_pio2f: every range of x, the large ones through __kernel_rem_pio2f (and its floorf
        // and scalbnf) inside the original.
        std::vector<uint32_t> xs = interestingFloats();
        for (int n = 1; n <= 40; ++n)
        {
            const float near = static_cast<float>(n) * 1.5707963267948966f;
            xs.push_back(std::bit_cast<uint32_t>(near));
            xs.push_back(std::bit_cast<uint32_t>(near) + 1u);
            xs.push_back(std::bit_cast<uint32_t>(near) - 1u);
            xs.push_back(std::bit_cast<uint32_t>(-near));
        }
        for (uint32_t e = 0x43000000u; e <= 0x7F000000u; e += 0x00800000u)
        {
            xs.push_back(e | 0x00123457u);
            xs.push_back(e | 0x00654321u | 0x80000000u);
        }
        uint32_t i = 0;
        for (uint32_t x : xs)
        {
            if (i++ % 24 == 0)
                lib.fresh("__ieee754_rem_pio2f");
            const uint32_t y = lib.zeros(8, 8);
            lib.call("__ieee754_rem_pio2f", {y}, {flt(x)});
        }
    }

    // ---- string/*.c with the EE's quadword step, and fp-bit's unpack_d ----

    // A text `offset` bytes past a 16-byte boundary, the bytes after its null `fill` (what a
    // block step reads past the end).
    uint32_t textAt(Library &lib, const std::string &s, uint32_t offset, uint8_t fill = 0x5A)
    {
        std::vector<uint8_t> bytes(offset + s.size() + 1 + 48, fill);
        std::memcpy(bytes.data() + offset, s.c_str(), s.size() + 1);
        return lib.put(bytes.data(), static_cast<uint32_t>(bytes.size()), 16) + offset;
    }
    // `bytes` of `fill` at `offset` past a 16-byte boundary: a destination.
    uint32_t bufferAt(Library &lib, uint32_t bytes, uint32_t offset, uint8_t fill = 0x77)
    {
        std::vector<uint8_t> b(offset + bytes + 48, fill);
        return lib.put(b.data(), static_cast<uint32_t>(b.size()), 16) + offset;
    }

    // Lengths about the block sizes (8, 16, 32), and the offsets that pick each path: 16-aligned
    // (the quadword step), 8-aligned (the long loop), unaligned (bytes).
    const uint32_t kLengths[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 33, 47, 48, 63, 64, 65, 100};
    const uint32_t kOffsets[] = {0, 8, 4, 1, 15};
    std::string textOf(uint32_t length, char first = 'a')
    {
        std::string s;
        for (uint32_t i = 0; i < length; ++i)
            s += static_cast<char>(first + i % 26);
        return s;
    }

    void stringCases(Library &lib)
    {
        lib.fresh("strlen");
        for (uint32_t length : kLengths)
            for (uint32_t offset : kOffsets)
                lib.call("strlen", {textAt(lib, textOf(length), offset)});

        lib.fresh("strcpy, strcat");
        for (uint32_t length : kLengths)
            for (auto [to, from] : std::initializer_list<std::pair<uint32_t, uint32_t>>{{0, 0}, {8, 8}, {0, 8}, {8, 0}, {1, 1}, {0, 1}, {4, 12}})
            {
                const uint32_t src = textAt(lib, textOf(length), from);
                lib.call("strcpy", {bufferAt(lib, length + 1, to), src});
                const uint32_t dst = textAt(lib, textOf(length % 19, 'A'), to, 0x33);
                lib.call("strcat", {dst, src});
            }

        lib.fresh("strcmp");
        for (uint32_t length : kLengths)
            for (auto [a, b] : std::initializer_list<std::pair<uint32_t, uint32_t>>{{0, 0}, {8, 8}, {0, 8}, {1, 1}, {3, 0}})
            {
                const std::string s = textOf(length);
                lib.call("strcmp", {textAt(lib, s, a), textAt(lib, s, b)});              // equal
                lib.call("strcmp", {textAt(lib, s, a), textAt(lib, s + "z", b)});        // a prefix
                lib.call("strcmp", {textAt(lib, s + "z", a), textAt(lib, s, b)});
                for (uint32_t at : {0u, length / 2, length ? length - 1 : 0u})
                {
                    if (at >= length)
                        continue;
                    std::string t = s;
                    t[at] = static_cast<char>(0x80 | t[at]); // a high byte: the unsigned difference
                    lib.call("strcmp", {textAt(lib, s, a), textAt(lib, t, b)});
                    lib.call("strcmp", {textAt(lib, t, a), textAt(lib, s, b)});
                }
            }

        lib.fresh("strncmp");
        for (uint32_t length : {0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u, 48u, 64u})
            for (auto [a, b] : std::initializer_list<std::pair<uint32_t, uint32_t>>{{0, 0}, {8, 8}, {0, 8}, {1, 1}})
            {
                const std::string s = textOf(length);
                std::string t = s + "zz";
                if (length > 3)
                    t[length - 3] = '#';
                for (uint32_t n : {0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 24u, 31u, 32u, 33u, 48u, 100u})
                {
                    lib.call("strncmp", {textAt(lib, s, a), textAt(lib, s, b), n});
                    lib.call("strncmp", {textAt(lib, s, a), textAt(lib, t, b), n});
                    lib.call("strncmp", {textAt(lib, t + "\xC0", a), textAt(lib, t + "\x40", b), n});
                }
            }

        lib.fresh("strncpy");
        for (uint32_t length : {0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 23u, 24u, 31u, 32u, 33u, 40u, 64u})
            for (auto [to, from] : std::initializer_list<std::pair<uint32_t, uint32_t>>{{0, 0}, {8, 8}, {0, 8}, {1, 1}})
                for (uint32_t count : {0u, 1u, 7u, 8u, 15u, 16u, 17u, 24u, 31u, 32u, 33u, 40u, 48u, 70u})
                    lib.call("strncpy", {bufferAt(lib, count + 1, to), textAt(lib, textOf(length), from), count});

        lib.fresh("strchr");
        for (uint32_t length : kLengths)
            for (uint32_t offset : {0u, 8u, 1u})
            {
                std::string s = textOf(length);
                if (length > 2)
                    s[length - 2] = '\xE9';
                const uint32_t text = textAt(lib, s, offset);
                for (int32_t c : std::initializer_list<int32_t>{0, 'a', 'b', 'z', 'q', '#', 0xE9, -0x17, 0x100 + 'c', 0x5A})
                    lib.call("strchr", {text, s32(c)});
            }

        lib.fresh("memchr");
        for (uint32_t length : kLengths)
            for (uint32_t offset : {0u, 8u, 1u})
            {
                std::string s = textOf(length);
                if (length > 4)
                {
                    s[length / 2] = '\0';
                    s[length - 3] = '\xF1';
                }
                const uint32_t bytes = textAt(lib, s, offset);
                for (int32_t c : std::initializer_list<int32_t>{0, 'a', 'c', 0xF1, -0x0F, 0x5A, 0x100 + 'b'})
                    for (uint32_t n : {0u, 1u, length / 2, length, length + 1})
                        lib.call("memchr", {bytes, s32(c), n});
            }

        lib.fresh("memmove");
        for (uint32_t length : {0u, 1u, 7u, 8u, 15u, 16u, 31u, 32u, 33u, 40u, 63u, 64u, 65u, 100u})
            for (int32_t shift : {-33, -17, -16, -8, -1, 0, 1, 8, 16, 17, 33, 200})
                for (uint32_t offset : {0u, 8u, 3u})
                {
                    const uint32_t area = bufferAt(lib, 2 * length + 512, offset);
                    for (uint32_t i = 0; i < 2 * length + 512; ++i)
                        lib.ram[area + i] = static_cast<uint8_t>(i * 7 + 1);
                    const uint32_t src = area + 240;
                    lib.call("memmove", {src + static_cast<uint32_t>(shift), src, length});
                }
    }

    // fp-bit's unpack_d (dp-bit and fp-bit): zero, the denormals the EE build reads as zero, numbers,
    // infinities, quiet and signalling NaNs.
    void unpackCases(Library &lib)
    {
        lib.fresh("__unpack_d, __unpack_f");
        for (uint64_t bits : {0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
                              0x3FF0000000000000ull, 0xC004000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x0010000000000000ull,
                              0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull,
                              0xFFF4000000000000ull, 0x7FF7FFFFFFFFFFFFull, 0x400921FB54442D18ull})
        {
            const uint32_t source = lib.put(&bits, 8, 8);
            lib.call("__unpack_d", {source, lib.zeros(0x18, 8)});
        }
        for (uint32_t bits : {0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x3F800000u, 0xC0200000u, 0x7F7FFFFFu,
                              0x00800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0x7F900000u, 0xFF8FFFFFu, 0x40490FDBu})
        {
            const uint32_t source = lib.put(&bits, 4, 4);
            lib.call("__unpack_f", {source, lib.zeros(0x10, 8)});
        }
    }

    void libgccCases(Library &lib)
    {
        unpackCases(lib);
        lib.fresh("double to integer");
        for (double d : interestingDoubles())
        {
            lib.call("__fixdfdi", {dbl(d)});
            lib.call("__fixunsdfdi", {dbl(d)});
            lib.call("dptoli", {dbl(d)});
            lib.call("dptoul", {dbl(d)});
        }
        for (double d : {1.8446744073709552e19, 1.8446744073709550e19, 9.2233720368547758e18, 9.2233720368547748e18,
                         -9.2233720368547758e18, -9.2233720368547779e18, 4294967296.5, 4294967297.0, -4294967296.0,
                         2147483647.5, 2147483648.5, -2147483648.5, 0.9999999999999999, -0.9999999999999999, 1e-310})
        {
            lib.call("__fixdfdi", {dbl(d)});
            lib.call("__fixunsdfdi", {dbl(d)});
            lib.call("dptoli", {dbl(d)});
            lib.call("dptoul", {dbl(d)});
        }
        lib.fresh("__floatdidf");
        for (int64_t v : {int64_t(0), int64_t(1), int64_t(-1), int64_t(2), int64_t(-2), int64_t(255), int64_t(1) << 31,
                          -(int64_t(1) << 31), (int64_t(1) << 32) - 1, int64_t(1) << 32, (int64_t(1) << 53) - 1,
                          int64_t(1) << 53, (int64_t(1) << 53) + 1, (int64_t(1) << 54) + 3, int64_t(1) << 62,
                          -(int64_t(1) << 62) - 3, int64_t(0x0123456789ABCDEF), -int64_t(0x0123456789ABCDEF),
                          std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(),
                          std::numeric_limits<int64_t>::min() + 1, int64_t(0x7FFFFFFF00000001), int64_t(1000000007)})
            lib.call("__floatdidf", {s64(v)});
        // __udivmoddi4's branches: a one-word divisor above and below the high word, the top bit set (no
        // normalization), a two-word divisor, and zero (the source's deliberate 1 / d0, which the R5900's
        // divu answers with 0xFFFFFFFF and no trap: cov4 found the profiler dividing by a zero elapsed time).
        lib.fresh("__udivdi3, __umoddi3");
        const uint64_t kDivOperands[] = {0, 1, 2, 3, 7, 10, 255, 256, 65535, 65536, 300000, 0x7FFFFFFF, 0x80000000,
                                         0xFFFFFFFF, 0x100000000ull, 0x100000001ull, 0x1FFFFFFFFull, 0x3B9ACA00,
                                         0x12345678ull, 0x123456789ull, 0x0123456789ABCDEFull, 0x8000000000000000ull,
                                         0xFFFFFFFF00000000ull, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull,
                                         0xDEADBEEF00001234ull, 0x00000000927C0000ull, 0xFFFFFFFF927C0000ull,
                                         0x000493E000000000ull, 0x00000000000493E0ull};
        for (uint64_t n : kDivOperands)
            for (uint64_t d : kDivOperands)
            {
                lib.call("__udivdi3", {n, d});
                lib.call("__umoddi3", {n, d});
            }
    }

    const char *const kNumberTexts[] = {
        "", " ", "0", "-0", "+0", "1", "-1", "1.5", ".5", "5.", "-.5", "1e10", "1E-10", "1e", "1e+", "1e-", "1.2.3",
        "abc", "  42abc", "\t\n+17", "inf", "nan", "0x1p3", "00000000000000001", "0.0000000000000000000000000000000001234",
        "123456789012345678901234567890", "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
        "1e309", "-1e400", "2.2250738585072014e-308", "2.2250738585072011e-308", "4.9e-324", "2.4703282292062327e-324",
        "2.4703282292062328e-324", "1e-400", "3.14159265358979323846264338327950288", "1e23", "8.98846567431158e307",
        "9007199254740993", "9007199254740992.5", "0.1", "0.2", "0.3", "123.456e-7", "1e0000000000000001",
        "1e-99999999999", "1e99999999999", "1234567890123456789e-30", "+.e5", "-.", "6.02214076e23", "1.0000000000000002",
        "0.9999999999999999", "2.5e-5", "1e22", "1e-22", "5e-324", "1.00000000000000011102230246251565404236316680908203125",
        "0.500000000000000166533453693773481063544750213623046875", "7.2057594037927933e16", "4.35679E-10",
        "123456789e-300", "1e-307", "1e-306", "9.99999999999999999999999999e-1", "999999999999999999999999999999e-29"};

    void libcCases(Library &lib)
    {
        lib.fresh("__errno, localeconv");
        lib.call("__errno");
        lib.call("localeconv");
        lib.call("_localeconv_r", {lib.reent()});
        lib.call("_localeconv_r", {0});

        lib.fresh("calloc");
        for (auto [n, size] : std::initializer_list<std::pair<uint32_t, uint32_t>>{
                 {0, 0}, {1, 1}, {3, 7}, {10, 100}, {0, 5}, {5, 0}, {0x10000, 0x10000}, {1, 0x3FFFFF}, {0x10, 0x10000000}})
            lib.call("calloc", {n, size});

        lib.fresh("strrchr");
        const std::string longText(300, 'q');
        for (const std::string &text : {std::string(""), std::string("a"), std::string("abc"), std::string("hello world"),
                                        std::string("\xff\xfe\x80"), longText + "x" + longText})
        {
            const uint32_t s = lib.text(text);
            for (int32_t c : std::initializer_list<int32_t>{0, 'a', 'c', 'o', 'l', 'x', 'q', 'z', 0x100 + 'o', -1, 0xFF, 0x80, -0x80})
                lib.call("strrchr", {s, s32(c)});
        }

        uint32_t i = 0;
        for (const char *text : kNumberTexts)
        {
            if (i++ % 8 == 0)
                lib.fresh("strtod, atof");
            const uint32_t s = lib.text(text);
            const uint32_t end = lib.zeros(8, 8);
            lib.call("strtod", {s, end});
            lib.call("strtod", {s, 0});
            lib.call("atof", {s});
        }

        lib.fresh("strtoul");
        for (const char *text : {"", "0", "123", "  -123", "+42", "0x1F", "0X1f", "1f", "0x", "077", "08", "z", "Z", "4294967295",
                                 "4294967296", "18446744073709551615", "18446744073709551616", "99999999999999999999",
                                 "1011", "777", "\t\n 12abc", "-0", "- 5", "0xg", "  ", "-18446744073709551615"})
        {
            const uint32_t s = lib.text(text);
            const uint32_t end = lib.zeros(8, 8);
            for (int32_t base : {0, 2, 8, 10, 16, 36, 1, 37, -1})
            {
                lib.call("strtoul", {s, end, s32(base)});
                lib.call("_strtoul_r", {lib.reent(), s, 0, s32(base)});
            }
        }
        stringCases(lib);
    }

    void mprecCases(Library &lib)
    {
        lib.fresh("_hi0bits, _lo0bits");
        for (uint32_t x : {0u, 1u, 2u, 3u, 5u, 0x80u, 0x100u, 0x8000u, 0xFFFFu, 0x10000u, 0x00800000u, 0x00FFFFFFu, 0x01000000u,
                           0x0FFFFFFFu, 0x10000000u, 0x20000000u, 0x3FFFFFFFu, 0x40000000u, 0x7FFFFFFFu, 0x80000000u,
                           0xFFFFFFFFu, 0x00010000u, 0x00000006u, 0x12345678u})
        {
            lib.call("_hi0bits", {s32(static_cast<int32_t>(x))}); // (a 32-bit value: sign-extended in its register)
            lib.call("_lo0bits", {lib.words({x})});
        }

        lib.fresh("_Balloc, _Bfree");
        std::vector<uint32_t> blocks;
        for (int32_t k : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 1, 3})
            blocks.push_back(static_cast<uint32_t>(lib.call("_Balloc", {lib.reent(), s32(k)})));
        lib.call("_Bfree", {lib.reent(), 0});
        for (uint32_t b : blocks)
            lib.call("_Bfree", {lib.reent(), b});
        for (int32_t k : {1, 3, 9, 0, 2})
            lib.call("_Balloc", {lib.reent(), s32(k)});

        lib.fresh("_i2b, _multadd");
        for (int32_t v : {0, 1, 12345, -1, 0x7FFFFFFF})
            lib.call("_i2b", {lib.reent(), s32(v)});
        for (const char *n : kNumbers)
        {
            for (auto [m, a] : std::initializer_list<std::pair<int32_t, int32_t>>{
                     {10, 0}, {10, 7}, {1, 0}, {0, 5}, {0x7FFFFFFF, 0x7FFFFFFF}, {-1, -1}, {65536, 1}, {1000000007, 12345}})
            {
                const uint32_t b = bigint(lib, n);
                lib.call("_multadd", {lib.reent(), b, s32(m), s32(a)});
            }
        }

        lib.fresh("_multiply, __mcmp, __mdiff, _ratio");
        for (const char *x : kNumbers)
        {
            for (const char *y : kNumbers)
            {
                const uint32_t a = bigint(lib, x), b = bigint(lib, y);
                lib.call("__mcmp", {a, b});
                lib.call("_multiply", {lib.reent(), a, b});
                lib.call("__mdiff", {lib.reent(), a, b});
                if (std::string(y) != "0")
                    lib.call("_ratio", {a, b});
            }
            lib.fresh("_multiply, __mcmp, __mdiff, _ratio");
        }

        lib.fresh("_pow5mult, _lshift, _b2d");
        for (const char *n : kNumbers)
        {
            for (int32_t k : {0, 1, 2, 3, 4, 5, 7, 8, 13, 16, 25, 33, 50, 100})
                lib.call("_pow5mult", {lib.reent(), bigint(lib, n), s32(k)});
            for (int32_t k : {0, 1, 5, 31, 32, 33, 63, 64, 95, 200})
                lib.call("_lshift", {lib.reent(), bigint(lib, n), s32(k)});
            const uint32_t e = lib.zeros(8, 8);
            lib.call("_b2d", {bigint(lib, n), e});
            lib.fresh("_pow5mult, _lshift, _b2d");
        }

        lib.fresh("_ulp, _d2b");
        for (double d : interestingDoubles())
        {
            lib.call("_ulp", {dbl(d)});
            if (!std::isfinite(d))
                continue;
            const uint32_t e = lib.zeros(8, 8), bits = lib.zeros(8, 8);
            lib.call("_d2b", {lib.reent(), dbl(d), e, bits});
        }

        lib.fresh("_s2b");
        for (auto [digits, nd0] : std::initializer_list<std::pair<const char *, int32_t>>{
                 {"1234567890", 10}, {"123456789.0123", 9}, {"1234567891234.5678", 13}, {"12345.67890123456789", 5},
                 {"123456789012345678901234567890", 30}, {"9.99999999999999999999", 1}, {"1234567.8", 7}})
        {
            std::string all(digits);
            const uint32_t s = lib.text(all);
            std::string plain;
            for (char c : all)
                if (c != '.')
                    plain += c;
            const uint32_t y9 = static_cast<uint32_t>(std::stoul(plain.substr(0, 9)));
            lib.call("_s2b", {lib.reent(), s, s32(nd0), s32(static_cast<int32_t>(plain.size())), y9});
        }
    }

    void dtoaCases(Library &lib)
    {
        uint32_t i = 0;
        for (double d : interestingDoubles())
        {
            if (i++ % 3 == 0)
                lib.fresh("_dtoa_r");
            for (int32_t mode : {0, 1, 2, 3, 4, 5})
            {
                for (int32_t ndigits : {-3, 0, 1, 5, 17, 25})
                {
                    if ((mode == 0 || mode == 1) && ndigits != 0)
                        continue;
                    // Mode 5 asks for digits up to a place: none at or past the number's own first (ilim, ndigits
                    // + k + 1, under 1) sends newlib's _pow5mult a negative power, which it shifts forever, the
                    // original as the port (newlib 1.8.2's callers ask mode 5 for no such place).
                    const int32_t k = d == 0.0 || !std::isfinite(d) ? 0 : static_cast<int32_t>(std::floor(std::log10(std::fabs(d))));
                    if (mode == 5 && std::isfinite(d) && d != 0.0 && ndigits + k + 1 < 2)
                        continue;
                    const uint32_t decpt = lib.zeros(8, 8), sign = lib.zeros(8, 8), rve = lib.zeros(8, 8);
                    lib.call("_dtoa_r", {lib.reent(), dbl(d), s32(mode), s32(ndigits), decpt, sign, ndigits == 5 ? 0u : rve});
                }
            }
        }
        lib.fresh("_strtod_r");
        for (const char *text : kNumberTexts)
        {
            const uint32_t s = lib.text(text);
            const uint32_t end = lib.zeros(8, 8);
            lib.call("_strtod_r", {lib.reent(), s, end});
        }
    }

    // A format and its arguments (each a 64-bit slot), printed through vfprintf.
    struct Printed
    {
        const char *format;
        std::vector<uint64_t> arguments;
    };
    std::vector<Printed> printedCases()
    {
        std::vector<Printed> v = {
            {"plain text, no conversion", {}},
            {"", {}},
            {"%d %i %u %x %X %o %c %%", {s32(-42), s32(42), s32(-1), 0xBEEFu, 0xBEEFu, 8u, 'z'}},
            {"%ld %lu %lx %qd %qu %hd %hu", {s64(-1234567890123ll), s64(-1), 0xFEDCBA9876543210ull, s64(-77), 77u, s32(-70000), 70000u}},
            {"%-5d|%05d|%+d|% d|%#x|%#o|%.3d|%10.4s|%-10s|", {s32(7), s32(-7), s32(7), s32(7), 0u, 8u, s32(5), 0, 0}},
            {"%*d|%-*d|%.*d|%*.*s", {s32(6), s32(1), s32(-6), s32(2), s32(4), s32(3), s32(8), s32(2), 0}},
            {"%s|%5s|%.0s|%c|%5c|%-5c|", {0, 0, 0, 'a', 'b', 'c'}},
            {"%p %n", {0x00123456u, 0}},
            {"%d %d %d %d %d %d %d %d %d %d %d %d", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
            {"%d", {s32(std::numeric_limits<int32_t>::min())}},
            {"%qd %qx %#qo", {s64(std::numeric_limits<int64_t>::min()), s64(-1), s64(-1)}},
            {"%z %y %5", {}},
        };
        for (const char *f : {"%f", "%e", "%g", "%E", "%G", "%.0f", "%.0e", "%.0g", "%#.0f", "%#.0e", "%#g", "%.1f", "%.10f",
                              "%.17g", "%.30f", "%20.6e", "%-20.6g|", "%+f", "% e", "%010.3f", "%Lf", "%.40e", "%#.3g"})
            for (double d : interestingDoubles())
                v.push_back({f, {dbl(d)}});
        return v;
    }

    void printfCases(Library &lib)
    {
        uint32_t i = 0;
        for (const Printed &p : printedCases())
        {
            if (i++ % 12 == 0)
                lib.fresh("vfprintf");
            std::vector<uint64_t> slots = p.arguments;
            // a %s or %n slot left 0 points at a text or a count
            std::string f(p.format);
            for (size_t at = 0, slot = 0; at < f.size(); ++at)
            {
                if (f[at] != '%')
                    continue;
                size_t c = at + 1;
                while (c < f.size() && std::strchr("-+ #0123456789.*lqhL", f[c]))
                {
                    if (f[c] == '*' && slot < slots.size())
                        ++slot;
                    ++c;
                }
                if (c >= f.size())
                    break;
                if (f[c] == '%')
                {
                    at = c;
                    continue;
                }
                if (slot < slots.size() && (f[c] == 's' || f[c] == 'n') && slots[slot] == 0)
                    slots[slot] = f[c] == 's' ? lib.text("a string") : lib.zeros(8, 8);
                ++slot;
                at = c;
            }
            const uint32_t format = lib.text(p.format);
            std::vector<uint64_t> padded = slots;
            padded.resize(padded.size() + 4, 0);
            const uint32_t arguments = lib.put(padded.data(), static_cast<uint32_t>(padded.size() * 8), 8);
            lib.call("vfprintf", {stringFile(lib, 4096), format, arguments});
        }

        // Other FILEs: a small string buffer, unbuffered and line-buffered terminals, a disk file, one not set up
        // for writing, a broken one; the output long enough for several gathers.
        const std::string longFormat = "%s %d %s %f %s %e %s %g %s %x %s %c %s %o %s %u|%400d|";
        lib.fresh("vfprintf to files");
        for (int which = 0; which < 10; ++which)
        {
            uint32_t fp = 0;
            switch (which)
            {
            case 9: fp = stringFile(lib, 256); lib.setWord(fp + offsetof(File, reent), 0); break; // no _reent yet
            case 0: fp = stringFile(lib, 16); break;
            case 1: fp = descriptorFile(lib, kConsole, libc::kCanWrite | libc::kUnbuffered); break;
            case 2: fp = descriptorFile(lib, kConsole, libc::kCanWrite | libc::kLineBuffered); break;
            case 3: fp = descriptorFile(lib, kDiskFile, libc::kCanWrite); break;
            case 4: fp = descriptorFile(lib, kDiskFile, libc::kCanRead); break;
            case 5: fp = descriptorFile(lib, kBrokenFile, libc::kCanWrite | libc::kUnbuffered); break;
            case 6: fp = descriptorFile(lib, kBrokenFile, libc::kCanWrite, lib.zeros(64), 64); break;
            case 7: fp = descriptorFile(lib, kDiskFile, libc::kReadWrite | libc::kCanRead); break;
            case 8: fp = descriptorFile(lib, kConsole, libc::kCanWrite, lib.zeros(32), 32); break;
            }
            const uint32_t word = lib.text("word");
            const uint32_t arguments = va(lib, {word, s32(-5), word, dbl(3.25), word, dbl(-1e-7), word, dbl(1e300), word, 0xABCu,
                                                word, 'q', word, 64u, word, 99u, s32(12), 0, 0, 0});
            lib.call("vfprintf", {fp, lib.text(longFormat), arguments});
            lib.call("vfprintf", {fp, lib.text("line one\nline two\n%d\n"), va(lib, {s32(3), 0})});
        }

        lib.fresh("cvt, exponent");
        for (double d : interestingDoubles())
        {
            if (!std::isfinite(d))
                continue;
            for (int32_t ch : {'f', 'e', 'g', 'E', 'G'})
                for (int32_t ndigits : {0, 1, 6, 17})
                    for (int32_t flags : {0, 1})
                    {
                        const uint32_t sign = lib.zeros(8, 8), decpt = lib.zeros(8, 8), length = lib.zeros(8, 8);
                        lib.call("cvt", {lib.reent(), dbl(d), s32(ndigits), s32(flags), sign, decpt, s32(ch), length});
                    }
            lib.fresh("cvt, exponent");
        }
        for (int32_t e : {0, 1, 5, 9, 10, -1, -5, -10, 99, 100, -100, 307, -308, -323, 1000})
            for (int32_t ch : {'e', 'E'})
                lib.call("exponent", {lib.zeros(16), s32(e), s32(ch)});

        lib.fresh("__sprint, __sbprintf");
        for (int residual : {0, 5, 40})
        {
            const uint32_t fp = stringFile(lib, 64);
            const uint32_t bytes = lib.text("0123456789012345678901234567890123456789");
            const uint32_t vectors = lib.words({bytes, static_cast<uint32_t>(std::min(residual, 20)), bytes,
                                                static_cast<uint32_t>(std::max(residual - 20, 0))});
            const uint32_t request = lib.words({vectors, residual ? 2u : 0u, static_cast<uint32_t>(residual)});
            lib.call("__sprint", {fp, request});
        }
        for (int32_t fd : {kConsole, kBrokenFile})
        {
            const uint32_t fp = descriptorFile(lib, fd, libc::kCanWrite | libc::kUnbuffered);
            lib.call("__sbprintf", {fp, lib.text("%s=%d %f\n"), va(lib, {lib.text("x"), s32(17), dbl(2.5), 0})});
        }
    }

    void scanfCases(Library &lib)
    {
        lib.fresh("__sccl");
        for (const char *format : {"abc]", "^abc]", "]abc]", "^]abc]", "a-z]", "z-a]", "a-]", "-a]", "", "^", "0-9A-Fa-f]x",
                                   "a-c-e]", "\xff]", "^\x01-\xfe]", "]", "^]", "a-a]", "--]", "a]b]", "^-]"})
        {
            const uint32_t table = lib.zeros(256);
            lib.call("__sccl", {table, lib.text(format)});
        }
    }

    void stdioCases(Library &lib)
    {
        lib.fresh("_write_r, _read_r, _close_r, _lseek_r, _fstat_r");
        readFile(lib, "hello\nworld\n");
        const uint32_t buffer = lib.zeros(64);
        const uint32_t status = lib.zeros(sizeof(libc::FileStatus), 8);
        for (int32_t fd : {kConsole, kDiskFile, kBrokenFile, -1, 0, 2})
        {
            lib.call("_write_r", {lib.reent(), s32(fd), lib.text("bytes"), 5});
            lib.call("_write_r", {lib.reent(), s32(fd), buffer, 0});
            lib.call("_read_r", {lib.reent(), s32(fd), buffer, 4});
            lib.call("_read_r", {lib.reent(), s32(fd), buffer, 64});
            lib.call("_lseek_r", {lib.reent(), s32(fd), 2, 0});
            lib.call("_lseek_r", {lib.reent(), s32(fd), s32(-3), 2});
            lib.call("_lseek_r", {lib.reent(), s32(fd), 1, 7});
            lib.call("_fstat_r", {lib.reent(), s32(fd), status});
            lib.call("_close_r", {lib.reent(), s32(fd)});
        }

        lib.fresh("__sread, __swrite, __sseek, __sclose");
        readFile(lib, "0123456789abcdef");
        for (int32_t fd : {kDiskFile, kConsole, kBrokenFile})
        {
            for (int flags : {libc::kCanRead, libc::kCanWrite | libc::kAppend, libc::kCanWrite | libc::kOffsetCorrect,
                              libc::kReadWrite | libc::kOffsetCorrect})
            {
                const uint32_t fp = descriptorFile(lib, fd, flags);
                lib.call("__sread", {fp, buffer, 6});
                lib.call("__swrite", {fp, lib.text("written"), 7});
                lib.call("__sseek", {fp, 3, 0});
                lib.call("__sseek", {fp, s32(-100), 1});
                lib.call("__sread", {fp, buffer, 100});
                lib.call("__sclose", {fp});
            }
        }

        lib.fresh("__smakebuf, __swsetup");
        for (int32_t fd : {kConsole, kDiskFile, kBrokenFile, -1})
        {
            for (int flags : {0, libc::kCanWrite, libc::kUnbuffered | libc::kCanWrite, libc::kCanRead, libc::kReadWrite,
                              libc::kReadWrite | libc::kCanRead | libc::kAtEof, libc::kLineBuffered | libc::kCanWrite})
            {
                lib.call("__smakebuf", {descriptorFile(lib, fd, flags)});
                lib.call("__swsetup", {descriptorFile(lib, fd, flags)});
            }
            // a reading FILE with ungetc's buffer of its own, turned to writing
            const uint32_t fp = descriptorFile(lib, fd, libc::kReadWrite | libc::kCanRead, lib.zeros(64), 64);
            lib.setWord(fp + offsetof(File, ungetBase), lib.zeros(64));
            lib.setWord(fp + offsetof(File, ungetSize), 64);
            lib.call("__swsetup", {fp});
            // one with a seek function of its own (no seek optimisation)
            const uint32_t own = descriptorFile(lib, fd, libc::kCanWrite);
            lib.setWord(own + offsetof(File, seekFunction), addressOf("_lseek_r"));
            lib.call("__smakebuf", {own});
        }
        // the heap full: __smakebuf goes unbuffered
        lib.setWord(layout::kHeapNext, layout::kHeapEnd - 16u);
        lib.call("__smakebuf", {descriptorFile(lib, kDiskFile, libc::kCanWrite)});
        lib.call("__swsetup", {descriptorFile(lib, kConsole, libc::kCanWrite)});

        lib.fresh("__sfvwrite");
        const std::string piece(300, 'p');
        for (int which = 0; which < 10; ++which)
        {
            uint32_t fp = 0;
            switch (which)
            {
            case 0: fp = stringFile(lib, 1000); break;
            case 1: fp = stringFile(lib, 10); break;
            case 2: fp = descriptorFile(lib, kConsole, libc::kCanWrite | libc::kUnbuffered); break;
            case 3: fp = descriptorFile(lib, kConsole, libc::kCanWrite | libc::kLineBuffered, lib.zeros(64), 64); break;
            case 4: fp = descriptorFile(lib, kDiskFile, libc::kCanWrite, lib.zeros(128), 128); break;
            case 5: fp = descriptorFile(lib, kDiskFile, libc::kCanRead); break;
            case 6: fp = descriptorFile(lib, kBrokenFile, libc::kCanWrite | libc::kUnbuffered); break;
            case 7: fp = descriptorFile(lib, kBrokenFile, libc::kCanWrite, lib.zeros(16), 16); break;
            case 8: fp = descriptorFile(lib, kConsole, libc::kCanWrite | libc::kLineBuffered, lib.zeros(16), 16); break;
            case 9: fp = descriptorFile(lib, kDiskFile, 0); break;
            }
            if (which >= 3 && which != 5 && which != 9)
            {
                const uint32_t base = lib.word(fp + offsetof(File, bufferBase));
                const int32_t size = static_cast<int32_t>(lib.word(fp + offsetof(File, bufferSize)));
                lib.setWord(fp + offsetof(File, writeSpace), (which == 3 || which == 8) ? 0u : static_cast<uint32_t>(size));
                lib.setWord(fp + offsetof(File, lineBufferSize), (which == 3 || which == 8) ? static_cast<uint32_t>(-size) : 0u);
                (void)base;
            }
            const uint32_t p = lib.text(piece), nl = lib.text("a\nb\n"), one = lib.text("x");
            for (auto pieces : {std::vector<std::pair<uint32_t, uint32_t>>{},
                                std::vector<std::pair<uint32_t, uint32_t>>{{one, 1}},
                                std::vector<std::pair<uint32_t, uint32_t>>{{nl, 4}, {one, 0}, {p, 300}},
                                std::vector<std::pair<uint32_t, uint32_t>>{{p, 7}, {nl, 2}, {p, 50}, {nl, 4}, {one, 1}}})
            {
                std::vector<uint32_t> words;
                uint32_t total = 0;
                for (auto [base, length] : pieces)
                {
                    words.push_back(base);
                    words.push_back(length);
                    total += length;
                }
                words.push_back(0);
                words.push_back(0);
                const uint32_t vectors = lib.put(words.data(), static_cast<uint32_t>(words.size() * 4), 8);
                const uint32_t request = lib.words({vectors, static_cast<uint32_t>(pieces.size()), total});
                lib.call("__sfvwrite", {fp, request});
            }
        }

        lib.fresh("ungetc, __submore");
        for (int flags : {libc::kCanRead, libc::kReadWrite, libc::kReadWrite | libc::kCanWrite, 0, libc::kCanWrite})
        {
            const uint32_t base = lib.text("abcdefgh");
            const uint32_t fp = descriptorFile(lib, kDiskFile, flags, base, 8);
            lib.setWord(fp + offsetof(File, position), base + 3u);
            lib.setWord(fp + offsetof(File, readSpace), 5);
            for (int32_t c : std::initializer_list<int32_t>{'c', 'b', 'x', -1, 'y', 0x1FF, 'z', 'w', 'v', 'u', 't'})
                lib.call("ungetc", {s32(c), fp});
        }
        {
            // ungetc past its three bytes, then past __submore's first buffer
            const uint32_t fp = descriptorFile(lib, kDiskFile, libc::kCanRead, lib.text("q"), 1);
            for (int n = 0; n < 1040; ++n)
            {
                if (n < 8 || (n > 1020 && n < 1032) || n % 128 == 0)
                    lib.call("ungetc", {s32('0' + n % 10), fp});
                else
                    lib.original("ungetc", {s32('0' + n % 10), fp});
            }
            const uint32_t again = descriptorFile(lib, kDiskFile, libc::kCanRead, lib.text("q"), 1);
            lib.setWord(again + offsetof(File, ungetBase), again + offsetof(File, ungetBytes));
            lib.setWord(again + offsetof(File, ungetSize), 3);
            lib.call("__submore", {again});
            lib.call("__submore", {again});
        }

        lib.fresh("lflush, _fwalk, _cleanup_r");
        for (int flags : {0, libc::kLineBuffered, libc::kCanWrite, libc::kLineBuffered | libc::kCanWrite})
            lib.call("lflush", {descriptorFile(lib, kConsole, flags, lib.zeros(16), 16)});
        lib.call("_fwalk", {lib.reent(), addressOf("lflush")});
        lib.call("_cleanup_r", {lib.reent()});
        lib.original("__sinit", {lib.reent()});
        lib.call("_fwalk", {lib.reent(), addressOf("lflush")});
        lib.call("_fwalk", {lib.reent(), addressOf("__sclose")});
        lib.call("_cleanup_r", {lib.reent()});
    }

    void mallocCases(Library &lib)
    {
        lib.fresh("__malloc_lock, __malloc_unlock");
        for (uint32_t reent : {lib.reent(), 0u})
        {
            lib.call("__malloc_lock", {reent});
            lib.call("__malloc_unlock", {reent});
        }
    }
}

void register_ps2_library_tests()
{
    MiniTest::Case("Library ports", [](TestCase &tc)
    {
        const std::pair<const char *, void (*)(Library &)> libraries[] = {
            {"sce/libm.cpp", &libmCases},     {"sce/libgcc.cpp", &libgccCases},     {"sce/libc.cpp", &libcCases},
            {"sce/mprec.cpp", &mprecCases},   {"sce/libc_strtod.cpp", &dtoaCases}, {"sce/libc_printf.cpp", &printfCases},
            {"sce/libc_scanf.cpp", &scanfCases}, {"sce/libc_stdio.cpp", &stdioCases}, {"sce/libc_malloc.cpp", &mallocCases},
        };
        int order = 1;
        for (const auto &[name, cases] : libraries)
        {
            const std::string library = name;
            const auto run = cases;
            tc.Run(std::to_string(order++) + " library ports of " + library + " against AI's recompiled originals", [library, run](TestCase &t)
            {
                if (!start(t))
                    return;
                NothingEnabled off;
                const auto began = std::chrono::steady_clock::now();
                run(*g_library);
                report(t, library, false);
                std::printf("    [library] %s: %.1f s\n", library.c_str(),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
            });
        }
        tc.Run("9z library ports: every port compared, identical to its original on every call", [](TestCase &t)
        {
            if (!start(t))
                return;
            report(t, "", true);
        });
    });
}
