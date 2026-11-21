#include "fncheck.h"
#include "diffcall.h"

#include "fncheck_build.h"
#include "fncheck_stops.h"
#include "fnrec_file.h"
#include "runtime/state_probe.h"
#include "readable.h"

#include "game_overrides.h"
#include "machine.h"
#include "runtime/ee_scheduler.h"
#include "runtime/host_write.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace fncheck
{
    // ---- file format -------------------------------------------------------

    namespace
    {
        constexpr uint32_t kRecordMagic = 0x4C4C4143u; // "CALL"

        template <typename T>
        void put(std::ostream &out, const T &value)
        {
            out.write(reinterpret_cast<const char *>(&value), sizeof(T));
        }
        template <typename T>
        bool get(std::istream &in, T &value)
        {
            return static_cast<bool>(in.read(reinterpret_cast<char *>(&value), sizeof(T)));
        }
        // A context as the host lays it out, its tail padding (never written,
        // whatever the copy it came from held) zeroed, so that recordings of
        // the same calls are the same bytes.
        void putContext(std::ostream &out, const R5900Context &ctx)
        {
            constexpr size_t kUsed = offsetof(R5900Context, fcr31) + sizeof(uint32_t);
            alignas(R5900Context) unsigned char bytes[sizeof(R5900Context)];
            std::memcpy(bytes, &ctx, sizeof(bytes));
            std::memset(bytes + kUsed, 0, sizeof(bytes) - kUsed);
            out.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
        }
        void putLines(std::ostream &out, const std::vector<Line> &lines)
        {
            put(out, static_cast<uint32_t>(lines.size()));
            if (!lines.empty())
                out.write(reinterpret_cast<const char *>(lines.data()), static_cast<std::streamsize>(lines.size() * sizeof(Line)));
        }
        bool getLines(std::istream &in, std::vector<Line> &lines)
        {
            uint32_t n = 0;
            if (!get(in, n) || n > (1u << 24))
                return false;
            lines.resize(n);
            return n == 0 || static_cast<bool>(in.read(reinterpret_cast<char *>(lines.data()), static_cast<std::streamsize>(n * sizeof(Line))));
        }
    }

    void writeHeader(std::ostream &out, uint32_t address, game::Game game)
    {
        FileHeader header;
        header.address = address;
        header.game = static_cast<uint32_t>(game);
        put(out, header);
    }

    std::string recordingName(uint32_t address, game::Game game)
    {
        char name[48];
        if (game == game::Game::Ai)
            std::snprintf(name, sizeof(name), "%08x.fnrec", address);
        else
            std::snprintf(name, sizeof(name), "%s-%08x.fnrec", game::gameName(game), address);
        return name;
    }

    void writeRecord(std::ostream &out, const Record &record)
    {
        put(out, kRecordMagic);
        put(out, record.flags);
        put(out, record.sequence);
        put(out, record.vsync);
        put(out, record.deadLow);
        put(out, record.cycles);
        putContext(out, record.entry);
        putContext(out, record.exit);
        putLines(out, record.inputs);
        putLines(out, record.outputs);
        put(out, static_cast<uint32_t>(record.calls.size()));
        for (const Call &c : record.calls)
        {
            put(out, c.target);
            put(out, c.sp);
            put(out, c.flags);
            put(out, c.a);
            put(out, c.aHigh);
            put(out, c.f);
            putContext(out, c.after);
            putLines(out, c.effects);
            putLines(out, c.reads);
        }
        if (record.flags & kIoLog)
        {
            put(out, static_cast<uint32_t>(record.io.size()));
            if (!record.io.empty())
                out.write(reinterpret_cast<const char *>(record.io.data()),
                          static_cast<std::streamsize>(record.io.size() * sizeof(IoAccess)));
        }
    }

    bool readHeader(std::istream &in, FileHeader &header, std::string *error)
    {
        const FileHeader expected;
        if (!get(in, header) || std::memcmp(header.magic, expected.magic, sizeof(header.magic)) != 0)
        {
            if (error)
                *error = "not a fncheck recording";
            return false;
        }
        if (header.version < 1 || header.version > expected.version || header.contextSize != expected.contextSize)
        {
            if (error)
                *error = "recorded by another build kind (version or R5900Context size differs)";
            return false;
        }
        // Before version 6 the word was reserved (0, AI's).
        if (header.version < 6)
            header.game = 0;
        game::Game game;
        if (!game::gameFromNumber(header.game, game))
        {
            if (error)
                *error = "recorded in a game this build does not know (" + std::to_string(header.game) + ")";
            return false;
        }
        return true;
    }

    namespace
    {
        // Recordings made before memory keys (round 7) hold lines at the
        // addresses the code used: the uncached mirrors (0x20000000,
        // the uncached mirror) and the scratchpad. Those are folded
        // onto the keys, lines that meet merged (keys below kMemoryKeys are
        // left as they are).
        void foldOntoKeys(std::vector<Line> &lines)
        {
            bool folded = false;
            for (Line &l : lines)
                if (l.address >= kMemoryKeys)
                {
                    l.address = memoryKey(l.address);
                    folded = true;
                }
            if (!folded)
                return;
            std::map<uint32_t, Line> merged;
            for (const Line &l : lines)
            {
                Line &m = merged[l.address];
                m.address = l.address;
                for (uint32_t i = 0; i < 16u; ++i)
                    if (l.mask & (1u << i))
                        m.bytes[i] = l.bytes[i];
                m.mask = static_cast<uint16_t>(m.mask | l.mask);
            }
            lines.clear();
            for (const auto &[address, line] : merged)
                lines.push_back(line);
        }
    }

    namespace
    {
        // A recording made before drafter25 keeps the scratchpad accesses the
        // generated code made through runtime->LoadN / StoreN (a constant
        // address) in its I/O log: they become memory lines, as the recorder
        // now keeps them (fncheck.h ioLoad). A load's bytes are inputs unless
        // an I/O store of the call wrote them before (a line the recording
        // has already wins), a store's last bytes outputs (likewise).
        void scratchpadIoToMemory(Record &record)
        {
            if (!std::any_of(record.io.begin(), record.io.end(),
                             [](const IoAccess &a) { return ps2IsScratchpadAddress(a.address); }))
                return;
            std::map<uint32_t, Line> in, out;
            for (const Line &l : record.inputs)
                in[l.address] = l;
            for (const Line &l : record.outputs)
                out[l.address] = l;
            const std::map<uint32_t, Line> hadIn = in, hadOut = out;
            std::set<uint32_t> stored;
            std::vector<IoAccess> io;
            for (const IoAccess &a : record.io)
            {
                if (!ps2IsScratchpadAddress(a.address))
                {
                    io.push_back(a);
                    continue;
                }
                const uint64_t words[2] = {a.lo, a.hi};
                const uint8_t *bytes = reinterpret_cast<const uint8_t *>(words);
                const uint32_t key = memoryKey(a.address);
                for (uint32_t i = 0; i < a.size && i < 16u; ++i)
                {
                    const uint32_t k = key + i, lineKey = k & ~15u, bit = 1u << (k & 15u);
                    if (a.store)
                    {
                        stored.insert(k);
                        const auto had = hadOut.find(lineKey);
                        if (had != hadOut.end() && (had->second.mask & bit))
                            continue;
                        Line &l = out[lineKey];
                        l.address = lineKey;
                        l.bytes[k & 15u] = bytes[i];
                        l.mask = static_cast<uint16_t>(l.mask | bit);
                    }
                    else if (!stored.count(k))
                    {
                        Line &l = in[lineKey];
                        l.address = lineKey;
                        if (l.mask & bit)
                            continue;
                        l.bytes[k & 15u] = bytes[i];
                        l.mask = static_cast<uint16_t>(l.mask | bit);
                    }
                }
            }
            record.io = std::move(io);
            if (record.io.empty())
                record.flags &= ~kIoLog;
            record.inputs.clear();
            for (const auto &[k, l] : in)
                record.inputs.push_back(l);
            record.outputs.clear();
            for (const auto &[k, l] : out)
                record.outputs.push_back(l);
        }
    }

    bool readRecord(std::istream &in, Record &record, uint32_t version)
    {
        if (!readRecordAsWritten(in, record, version))
            return false;
        foldOntoKeys(record.inputs);
        foldOntoKeys(record.outputs);
        scratchpadIoToMemory(record);
        for (Call &c : record.calls)
        {
            foldOntoKeys(c.effects);
            foldOntoKeys(c.reads);
            // Before version 4 every tail call was charged a dispatch; in the
            // game a direct one (most of them) charges none.
            if (version < 4 && (c.flags & kTailCall))
            {
                c.flags |= kDirectTailCall;
                record.cycles -= std::min<uint64_t>(record.cycles, EeScheduler::kGuestDispatchCycles);
            }
        }
        return true;
    }

    bool readRecordAsWritten(std::istream &in, Record &record, uint32_t version)
    {
        uint32_t magic = 0;
        if (!get(in, magic) || magic != kRecordMagic)
            return false;
        uint32_t calls = 0;
        if (!get(in, record.flags) || !get(in, record.sequence) || !get(in, record.vsync) || !get(in, record.deadLow) ||
            (version >= 3 && !get(in, record.cycles)) ||
            !get(in, record.entry) || !get(in, record.exit) || !getLines(in, record.inputs) ||
            !getLines(in, record.outputs) || !get(in, calls) || calls > (1u << 20))
            return false;
        record.calls.resize(calls);
        for (Call &c : record.calls)
        {
            if (!get(in, c.target) || !get(in, c.sp) || (version >= 2 && !get(in, c.flags)) || !get(in, c.a) ||
                (version >= 5 && !get(in, c.aHigh)) || !get(in, c.f) || !get(in, c.after) ||
                !getLines(in, c.effects) || (version >= 7 && !getLines(in, c.reads)))
                return false;
            c.highKnown = version >= 5;
        }
        record.io.clear();
        if (version >= 5 && (record.flags & kIoLog))
        {
            uint32_t n = 0;
            if (!get(in, n) || n > (1u << 24))
                return false;
            record.io.resize(n);
            if (n && !in.read(reinterpret_cast<char *>(record.io.data()), static_cast<std::streamsize>(n * sizeof(IoAccess))))
                return false;
        }
        return true;
    }

    namespace
    {
        // Skips `bytes`: read through the stream's buffer when short (a seek
        // per call's few lines made indexing a big file a syscall and a
        // buffer refill each), a seek when long.
        bool skipBytes(std::istream &in, std::streamoff bytes)
        {
            if (bytes <= 0)
                return true;
            if (bytes < (std::streamoff(1) << 16))
                return static_cast<bool>(in.ignore(bytes)) && in.gcount() == bytes;
            return static_cast<bool>(in.seekg(bytes, std::ios::cur));
        }
        bool skipLines(std::istream &in)
        {
            uint32_t n = 0;
            if (!get(in, n) || n > (1u << 24))
                return false;
            return skipBytes(in, static_cast<std::streamoff>(n) * static_cast<std::streamoff>(sizeof(Line)));
        }
    }

    bool skimRecord(std::istream &in, RecordHead &head, uint32_t version)
    {
        uint32_t magic = 0, deadLow = 0, calls = 0;
        uint64_t cycles = 0;
        if (!get(in, magic) || magic != kRecordMagic)
            return false;
        if (!get(in, head.flags) || !get(in, head.sequence) || !get(in, head.vsync) || !get(in, deadLow) ||
            (version >= 3 && !get(in, cycles)) || !get(in, head.entry) || !get(in, head.exit) || !skipLines(in) ||
            !skipLines(in) || !get(in, calls) || calls > (1u << 20))
            return false;
        // A call: target, sp, flags (2 on), a, aHigh (5 on), f, the registers after, its effects, its frame reads (7 on).
        const std::streamoff callBytes = static_cast<std::streamoff>(
            sizeof(uint32_t) * 2u + (version >= 2 ? sizeof(uint32_t) : 0u) + sizeof(Call::a) +
            (version >= 5 ? sizeof(Call::aHigh) : 0u) + sizeof(Call::f) + sizeof(R5900Context));
        for (uint32_t i = 0; i < calls; ++i)
            if (!skipBytes(in, callBytes) || !skipLines(in) || (version >= 7 && !skipLines(in)))
                return false;
        if (version >= 5 && (head.flags & kIoLog))
        {
            uint32_t n = 0;
            if (!get(in, n) || n > (1u << 24))
                return false;
            if (!skipBytes(in, static_cast<std::streamoff>(n) * static_cast<std::streamoff>(sizeof(IoAccess))))
                return false;
        }
        return true;
    }

    // ---- traced calls ------------------------------------------------------

    CallRunner g_callRunner = nullptr;
    // ZX_FNREC_UNWOUND=1: a recorded function's callee that goes through the
    // scheduler with no wait (a checkpoint due in it, a jump or a chunk's hop
    // out of it, a tail call's target that does) keeps the call open, the
    // session parked at its return point (drafter24), and kept if nothing but
    // the thread's own code ran meanwhile. Off by default, so such a call is
    // not kept: with every BMX XXX function recorded on vr-dam-save, these
    // records failed the readable versions of two Verified functions
    // (addLevelObject, setupUiMaterial: a callee's argument, where the same
    // calls recorded without them pass), not understood yet (tracedrift1).
    bool g_unwindParking = false;
    // ZX_FNREC_MAX_MB (default 512; 0 none): a recording larger than this
    // (its calls, their effects, the I/O log, the lines it touched) is given
    // up and its memory freed (Recording::afterCall).
    uint64_t g_maxRecordBytes = 512ull << 20;

    namespace
    {
        struct Extent
        {
            uint32_t end;
            void (*traced)(uint8_t *, R5900Context *, PS2Machine *);
        };
        // Per game: the games' code shares addresses. `overlapping`: two of the game's extents share addresses (an
        // entry_ function registered inside its neighbour's extent: BMX XXX's riderless bike move inside
        // ghostRideMove's); `longest`: the longest extent, which bounds the search for the ones holding an address.
        struct Extents
        {
            std::map<uint32_t, Extent> byStart;
            bool overlapping = false;
            uint32_t longest = 0;
        };
        Extents &tracedExtentsOf(game::Game game)
        {
            static Extents extents[game::kGameCount];
            return extents[static_cast<size_t>(game) < game::kGameCount ? static_cast<size_t>(game) : 0u];
        }
        std::map<uint32_t, Extent> &tracedExtents(game::Game game) { return tracedExtentsOf(game).byStart; }
        game::Game g_tracedGame = game::Game::Ai;
        // The traced function whose extent holds both addresses, if any. Where extents overlap, two may hold them:
        // which one's traced code a jump table's jump goes on in cannot be told from the addresses, so the run stops
        // there rather than go on in either.
        const Extent *sameTracedFunction(uint32_t a, uint32_t b)
        {
            const Extents &set = tracedExtentsOf(g_tracedGame);
            const auto &extents = set.byStart;
            auto it = extents.upper_bound(a);
            if (it == extents.begin())
                return nullptr;
            --it;
            if (!set.overlapping)
            {
                if (a >= it->second.end || b < it->first || b >= it->second.end)
                    return nullptr;
                return &it->second;
            }
            const std::pair<const uint32_t, Extent> *found = nullptr;
            for (;; --it)
            {
                if (a - it->first >= set.longest)
                    break; // (this one and every earlier one ends before a)
                if (a < it->second.end && b >= it->first && b < it->second.end)
                {
                    if (found)
                    {
                        std::fprintf(stderr,
                                     "[fncheck] the %s traced functions at 0x%08x and 0x%08x both hold the jump from "
                                     "0x%08x to 0x%08x: their extents overlap, and which one's traced code goes on "
                                     "there is not known\n",
                                     game::gameName(g_tracedGame), it->first, found->first, a, b);
                        std::abort();
                    }
                    found = &*it;
                }
                if (it == extents.begin())
                    break;
            }
            return found ? &found->second : nullptr;
        }
    }

    TracedExtent::TracedExtent(uint32_t start, uint32_t end, void (*traced)(uint8_t *, R5900Context *, PS2Machine *),
                               game::Game game)
    {
        Extents &set = tracedExtentsOf(game);
        const auto [it, added] = set.byStart.insert_or_assign(start, Extent{end, traced});
        (void)added;
        set.longest = std::max(set.longest, end > start ? end - start : 0u);
        // Extents in start order overlap somewhere only if two neighbours do.
        if (it != set.byStart.begin() && std::prev(it)->second.end > start)
            set.overlapping = true;
        if (const auto next = std::next(it); next != set.byStart.end() && next->first < end)
            set.overlapping = true;
    }

    void setTracedGame(game::Game game) { g_tracedGame = game; }

    namespace
    {
        // A chunked function's chunks ([start, end) each), by game and the
        // function's address (its first chunk's), as its traced original runs
        // them in the game.
        std::map<std::pair<int, uint32_t>, std::vector<std::pair<uint32_t, uint32_t>>> g_tracedChunks;
    }

    bool tracedChunksKnown(uint32_t function) { return g_tracedChunks.count({static_cast<int>(g_tracedGame), function}) != 0u; }
    void noteTracedChunk(uint32_t function, uint32_t start, uint32_t end)
    {
        g_tracedChunks[{static_cast<int>(g_tracedGame), function}].emplace_back(start, end);
    }

    uint32_t tracedExtentEnd(uint32_t start)
    {
        const auto &extents = tracedExtents(g_tracedGame);
        const auto it = extents.find(start);
        return it == extents.end() ? 0u : it->second.end;
    }

    bool tracedDispatch(PS2Machine *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                        uint32_t fallthroughPc, int kind, const char *debugName)
    {
        const auto branchKind = static_cast<PS2Machine::GuestBranchKind>(kind);
        const bool isCall = branchKind == PS2Machine::GuestBranchKind::DirectCall ||
                            branchKind == PS2Machine::GuestBranchKind::IndirectCall;
        Tracer *tracer = t_tracer;
        if (tracer)
            tracer->cycles(EeScheduler::kGuestDispatchCycles);
        // A call's dispatch is a safe point: due, the frame unwinds and the
        // callee returns to the scheduler, which dispatches the return
        // address (zx-fncheck --safepoints: the call is answered as usual and
        // the function returns with pc there).
        const bool unwind = isCall && tracer && tracer->safePointDue(EeScheduler::kGuestDispatchCycles);
        if (isCall && tracer)
            tracer->beforeCall(rdram, ctx, targetPc);
        else if (tracer && branchKind == PS2Machine::GuestBranchKind::IndirectJump && targetPc != sourcePc &&
                 sameTracedFunction(sourcePc, targetPc))
        {
            // A jump table's jump within the traced function (a target its
            // switch does not hold): in the game the plain code's dispatch
            // (8 cycles, a safe point) leaves for the scheduler, which goes
            // on at the target through the table, and so does the recorded
            // call (the recorder parks it there: runSession); zx-fncheck's
            // replay goes on there, traced (the generated code returns after
            // the dispatch).
            if (tracer->inGame())
                return runtime->dispatchGuestBranch(rdram, ctx, targetPc, sourcePc, fallthroughPc, branchKind, debugName);
            ctx->pc = targetPc;
            sameTracedFunction(sourcePc, targetPc)->traced(rdram, ctx, runtime);
            return false;
        }
        else if (tracer && (branchKind == PS2Machine::GuestBranchKind::DirectJump ||
                            branchKind == PS2Machine::GuestBranchKind::IndirectJump))
        {
            // Leaving the function by a jump: a tail call, run to its return by
            // the recorder (Tracer::tailCall).
            tracer->beforeCall(rdram, ctx, targetPc);
            tracer->tailCall(false);
            return runtime->dispatchGuestBranch(rdram, ctx, targetPc, sourcePc, fallthroughPc, branchKind, debugName);
        }
        if (isCall && tracer && g_callRunner && g_callRunner(runtime, rdram, ctx, targetPc, fallthroughPc))
        {
            tracer->afterCall(rdram, ctx, true);
            return true;
        }
        // A callee that leaves by a jump (into a block the recompiler split
        // off) comes back here not returned, as in the game: the frame unwinds
        // to the scheduler, which runs it on and dispatches the return point
        // (the recorder parks the call there: runSession).
        const bool returned =
            runtime->dispatchGuestBranch(rdram, ctx, targetPc, sourcePc, fallthroughPc, branchKind, debugName);
        if (isCall && tracer)
            tracer->afterCall(rdram, ctx, returned);
        if (unwind && returned)
            return false; // pc is the return address
        return returned;
    }

    // ---- what callees write: page protection --------------------------------

    // A recorded call keeps what each of its callees changed in RAM. Copying
    // and comparing all of RAM around every callee call (64 MB, about 3 to 6
    // ms) made recording slow; instead EE RAM is made read-only for the call
    // (one mprotect / VirtualProtect of the whole range), the first write to
    // a page faults, the handler copies the page as it was and makes it
    // writable, and at the return only those pages are compared with their
    // copies. Calls nest (a recorded callee's own callees): an interval per
    // open call, LIFO; a page's copy, taken at its first write after the
    // newest interval began, is the "before" of every open interval that has
    // not seen the page yet. Writes by other threads (the IOP's, DMA) fault
    // the same way; writes the operating system makes (a file read straight
    // into RAM) would fail instead, so those readers call ps2BeforeHostWrite
    // first (runtime/host_write.h). The scratchpad (16 KB) and RAM's
    // bytes outside whole pages are copied and compared as before.
    // ZX_FNREC_EFFECTS=diff keeps the full RAM diff (the reference).
    namespace
    {
        class WriteTracker
        {
        public:
            static WriteTracker &instance()
            {
                static WriteTracker tracker;
                return tracker;
            }

            // Covers RAM at `ram` (PS2_RAM_SIZE bytes). False when page
            // protection is not available (the caller diffs RAM instead).
            bool attach(uint8_t *ram)
            {
                if (m_ram == ram)
                    return m_ok;
                if (m_depth != 0u)
                    return false; // another RAM while calls are open: not tracked
                release();
                m_ram = ram;
#if defined(_WIN32)
                SYSTEM_INFO info;
                GetSystemInfo(&info);
                m_pageSize = info.dwPageSize;
#else
                m_pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
                const uintptr_t begin = reinterpret_cast<uintptr_t>(ram);
                const uintptr_t end = begin + PS2_RAM_SIZE;
                const uintptr_t first = (begin + m_pageSize - 1u) / m_pageSize * m_pageSize;
                const uintptr_t last = end / m_pageSize * m_pageSize;
                if (m_pageSize == 0u || (m_pageSize & (m_pageSize - 1u)) || last <= first)
                    return m_ok = false;
                m_first = reinterpret_cast<uint8_t *>(first);
                m_pages = (last - first) / m_pageSize;
                m_lowEdge = static_cast<uint32_t>(first - begin);
                m_highEdge = static_cast<uint32_t>(end - last);
                m_writable.reset(new uint8_t[m_pages]);
                std::memset(m_writable.get(), 1, m_pages);
                // Copies of pages, reserved address space: four per page (open
                // intervals nest a few deep), committed as they are touched.
                m_slots = m_pages * 4u;
#if defined(_WIN32)
                m_pool = static_cast<uint8_t *>(
                    VirtualAlloc(nullptr, m_slots * m_pageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
                void *pool = mmap(nullptr, m_slots * m_pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                m_pool = pool == MAP_FAILED ? nullptr : static_cast<uint8_t *>(pool);
#endif
                if (!m_pool)
                    return m_ok = false;
                m_refs.reset(new uint16_t[m_slots]());
                m_free.reset(new uint32_t[m_slots]);
                for (size_t i = 0; i < m_slots; ++i)
                    m_free[i] = static_cast<uint32_t>(m_slots - 1u - i);
                m_freeCount = m_slots;
                if (!m_handler)
                    installHandler();
                m_handler = true;
                ps2HostWriteHookStorage().store(&hostWriteHook, std::memory_order_release);
                return m_ok = true;
            }

            // Another RAM (a unit test's): the old one's state let go.
            void release()
            {
                m_ok = false;
                if (m_pool)
                {
#if defined(_WIN32)
                    VirtualFree(m_pool, 0, MEM_RELEASE);
#else
                    munmap(m_pool, m_slots * m_pageSize);
#endif
                }
                m_pool = nullptr;
                m_frames.clear();
                m_ram = nullptr;
            }

            size_t depth() const { return m_depth; }

            // An interval begins (a callee is called): RAM read-only.
            void push(uint8_t *scratchpad)
            {
                if (m_depth == m_frames.size())
                    m_frames.emplace_back(std::make_unique<Frame>(m_pages, m_lowEdge + m_highEdge));
                Frame &f = *m_frames[m_depth];
                std::memset(f.has.get(), 0, m_pages);
                f.count = 0;
                f.overflow = false;
                if (m_lowEdge)
                    std::memcpy(f.edges.get(), m_ram, m_lowEdge);
                if (m_highEdge)
                    std::memcpy(f.edges.get() + m_lowEdge, m_ram + PS2_RAM_SIZE - m_highEdge, m_highEdge);
                f.scratchpad = scratchpad != nullptr;
                if (scratchpad)
                    std::memcpy(f.scratch, scratchpad, PS2_SCRATCHPAD_SIZE);
                Lock lock(m_lock);
                ++m_depth;
                protectAll(false);
            }

            // The innermost interval ends: `changed(key, before, now, size)`
            // for each stretch of memory that may have changed (the pages
            // written, the edges, the scratchpad), in no particular order.
            // False if a page's copy could not be kept (no slot free).
            template <typename F>
            bool pop(uint8_t *scratchpad, F &&changed)
            {
                if (m_depth == 0u)
                    return false;
                Frame &f = *m_frames[m_depth - 1u];
                for (size_t i = 0; i < f.count; ++i)
                {
                    const Entry &e = f.entries[i];
                    const uint8_t *now = m_first + e.page * m_pageSize;
                    changed(static_cast<uint32_t>(now - m_ram), m_pool + static_cast<size_t>(e.slot) * m_pageSize, now,
                            static_cast<uint32_t>(m_pageSize));
                }
                if (m_lowEdge)
                    changed(0u, f.edges.get(), m_ram, m_lowEdge);
                if (m_highEdge)
                    changed(PS2_RAM_SIZE - m_highEdge, f.edges.get() + m_lowEdge, m_ram + PS2_RAM_SIZE - m_highEdge,
                            m_highEdge);
                if (f.scratchpad && scratchpad)
                    changed(kScratchpadKey, f.scratch, scratchpad, PS2_SCRATCHPAD_SIZE);
                const bool ok = !f.overflow;
                discardTop();
                return ok;
            }

            // Intervals left open by a call that did not come back (a callee
            // unwound to the scheduler) are dropped.
            void unwindTo(size_t depth)
            {
                while (m_depth > depth)
                    discardTop();
            }

            // A fault at `address`: ours if it is a write to a protected page.
            bool fault(void *address)
            {
                uint8_t *p = static_cast<uint8_t *>(address);
                if (!m_ok || p < m_first || p >= m_first + m_pages * m_pageSize)
                    return false;
                Lock lock(m_lock);
                open(static_cast<size_t>(p - m_first) / m_pageSize);
                return true;
            }

            void beforeHostWrite(void *destination, size_t size)
            {
                uint8_t *p = static_cast<uint8_t *>(destination);
                if (!m_ok || size == 0u || p + size <= m_first || p >= m_first + m_pages * m_pageSize)
                    return;
                const size_t from = p < m_first ? 0u : static_cast<size_t>(p - m_first) / m_pageSize;
                const size_t to = std::min(m_pages - 1u, static_cast<size_t>(p + size - 1u - m_first) / m_pageSize);
                Lock lock(m_lock);
                for (size_t page = from; page <= to; ++page)
                    open(page);
            }

            uint64_t faults() const { return m_faults; }

        private:
            struct Entry
            {
                uint32_t page;
                uint32_t slot;
            };
            struct Frame
            {
                Frame(size_t pages, size_t edgeBytes)
                    : has(new uint8_t[pages]()), entries(new Entry[pages]), edges(new uint8_t[edgeBytes + 1u])
                {
                }
                std::unique_ptr<uint8_t[]> has; // the page's copy is taken
                std::unique_ptr<Entry[]> entries;
                size_t count = 0;
                bool overflow = false;
                std::unique_ptr<uint8_t[]> edges;
                bool scratchpad = false;
                uint8_t scratch[PS2_SCRATCHPAD_SIZE];
            };
            struct Lock
            {
                explicit Lock(std::atomic_flag &flag) : f(flag)
                {
                    while (f.test_and_set(std::memory_order_acquire))
                    {
                    }
                }
                ~Lock() { f.clear(std::memory_order_release); }
                std::atomic_flag &f;
            };

            // Under the lock (also from the fault handler: no allocation).
            void open(size_t page)
            {
                if (m_writable[page])
                    return; // another thread's fault opened it already
                ++m_faults;
                uint32_t slot = ~0u;
                for (size_t d = 0; d < m_depth; ++d)
                {
                    Frame &f = *m_frames[d];
                    if (f.has[page])
                        continue;
                    if (slot == ~0u)
                    {
                        if (m_freeCount == 0u)
                        {
                            f.overflow = true;
                            continue;
                        }
                        slot = m_free[--m_freeCount];
                        std::memcpy(m_pool + static_cast<size_t>(slot) * m_pageSize, m_first + page * m_pageSize,
                                    m_pageSize);
                    }
                    f.has[page] = 1;
                    f.entries[f.count++] = Entry{static_cast<uint32_t>(page), slot};
                    ++m_refs[slot];
                }
                protect(m_first + page * m_pageSize, m_pageSize, true);
                m_writable[page] = 1;
            }

            void discardTop()
            {
                Lock lock(m_lock);
                Frame &f = *m_frames[m_depth - 1u];
                for (size_t i = 0; i < f.count; ++i)
                    if (--m_refs[f.entries[i].slot] == 0u)
                        m_free[m_freeCount++] = f.entries[i].slot;
                f.count = 0;
                --m_depth;
                if (m_depth == 0u)
                    protectAll(true);
            }

            void protectAll(bool writable)
            {
                protect(m_first, m_pages * m_pageSize, writable);
                std::memset(m_writable.get(), writable ? 1 : 0, m_pages);
            }

            static void protect(uint8_t *p, size_t size, bool writable)
            {
#if defined(_WIN32)
                DWORD old = 0;
                VirtualProtect(p, size, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
#else
                mprotect(p, size, writable ? PROT_READ | PROT_WRITE : PROT_READ);
#endif
            }

            static void hostWriteHook(void *destination, size_t size)
            {
                instance().beforeHostWrite(destination, size);
            }

#if defined(_WIN32)
            static LONG CALLBACK handler(EXCEPTION_POINTERS *info)
            {
                const EXCEPTION_RECORD *e = info->ExceptionRecord;
                if (e->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->NumberParameters >= 2 &&
                    e->ExceptionInformation[0] == 1 &&
                    instance().fault(reinterpret_cast<void *>(e->ExceptionInformation[1])))
                    return EXCEPTION_CONTINUE_EXECUTION;
                return EXCEPTION_CONTINUE_SEARCH;
            }
            void installHandler() { AddVectoredExceptionHandler(1, &handler); }
#else
            static struct sigaction &previous(int signal)
            {
                static struct sigaction bus{}, segv{};
                return signal == SIGBUS ? bus : segv;
            }
            static void handler(int signal, siginfo_t *info, void *context)
            {
                if (instance().fault(info->si_addr))
                    return; // the write runs again, on a writable page
                struct sigaction &old = previous(signal);
                if (old.sa_flags & SA_SIGINFO)
                {
                    if (old.sa_sigaction)
                    {
                        old.sa_sigaction(signal, info, context);
                        return;
                    }
                }
                else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)
                {
                    old.sa_handler(signal);
                    return;
                }
                // Not ours: the default action, when the access runs again.
                sigaction(signal, &old, nullptr);
                if (old.sa_handler == SIG_IGN)
                    std::signal(signal, SIG_DFL);
            }
            void installHandler()
            {
                struct sigaction action{};
                action.sa_sigaction = &handler;
                action.sa_flags = SA_SIGINFO;
                sigemptyset(&action.sa_mask);
                sigaction(SIGBUS, &action, &previous(SIGBUS));
                sigaction(SIGSEGV, &action, &previous(SIGSEGV));
            }
#endif

            uint8_t *m_ram = nullptr;
            bool m_ok = false;
            bool m_handler = false;
            size_t m_pageSize = 0;
            uint8_t *m_first = nullptr;
            size_t m_pages = 0;
            uint32_t m_lowEdge = 0, m_highEdge = 0;
            std::unique_ptr<uint8_t[]> m_writable;
            uint8_t *m_pool = nullptr;
            size_t m_slots = 0;
            std::unique_ptr<uint16_t[]> m_refs;
            std::unique_ptr<uint32_t[]> m_free;
            size_t m_freeCount = 0;
            std::vector<std::unique_ptr<Frame>> m_frames;
            size_t m_depth = 0;
            std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
            uint64_t m_faults = 0;
        };

        // ZX_FNREC_EFFECTS: pages (the default) or diff.
        bool g_trackPages = true;
    }

    // ---- recording ---------------------------------------------------------

    namespace
    {
        uint32_t lineOf(uint32_t address) { return address & ~15u; }

        // Groups byte values into lines.
        void addByte(std::map<uint32_t, Line> &lines, uint32_t address, uint8_t value)
        {
            Line &line = lines[lineOf(address)];
            line.address = lineOf(address);
            const uint32_t i = address & 15u;
            line.mask = static_cast<uint16_t>(line.mask | (1u << i));
            line.bytes[i] = value;
        }

        std::vector<Line> flatten(const std::map<uint32_t, Line> &lines)
        {
            std::vector<Line> out;
            out.reserve(lines.size());
            for (const auto &[address, line] : lines)
                out.push_back(line);
            return out;
        }

        // One recorded call of a traced function.
        class Recording final : public Tracer
        {
        public:
            // `caller`: the recording whose open call this one is (the
            // function called it directly), which learns what this call reads
            // of its frame (forwardReads); null when another function made it.
            void begin(uint8_t *rdram, const R5900Context &entry, Recording *caller)
            {
                m_rdram = rdram;
                m_lines.clear();
                m_caller = caller;
                m_callReads.clear();
                m_record = Record{};
                m_record.entry = entry;
                m_entrySp = static_cast<uint32_t>(ee::gpr64(&entry, ee::reg::sp));
                m_minSp = m_entrySp;
                m_broken = false;
                m_unwoundOpen = m_dropping = false;
                m_tooBig = false;
                m_bytes = 0;
                m_lostPages = false;
                m_tail = false;
                m_callStack.clear();
                m_openCalls = 0;
                m_snapshotted = ~size_t(0);
            }

            // A wait: the one open call (the one that waited) and where the
            // thread comes back, its return point and sp.
            // A tail call that waits comes back at the jump's ra (the function's
            // own return, or an inner self-call's return point).
            bool parkable(uint32_t &pc, uint32_t &sp) const
            {
                if (m_broken || m_callStack.size() != 1u || m_openCalls != 1u)
                    return false;
                pc = m_tail ? m_tailReturn : m_callRa;
                sp = m_record.calls[m_callStack.back()].sp;
                return pc != 0u;
            }
            // The open call's memory before-image made whole (RAM and the
            // scratchpad as they were at the call), for afterCall's full diff
            // when the thread comes back: with page protection, the interval
            // (on top: `below` intervals under it) taken off the tracker and
            // the copies of the pages written so far put back over RAM's.
            bool park(uint8_t *rdram, size_t below) { return openCallImage(rdram, below) != nullptr; }
            // The open call's before-image: RAM and the scratchpad (memory keys)
            // as they were at the call, or null.
            const uint8_t *openCallImage(uint8_t *rdram, size_t below)
            {
                const size_t level = m_openCalls - 1u;
                WriteTracker &tracker = WriteTracker::instance();
                const bool tracked = g_trackPages && tracker.depth() == below + 1u;
                if (g_trackPages && !tracked && tracker.depth() != below)
                    return nullptr;
                if (!tracked)
                {
                    m_snapshotted = level; // (diff mode: beforeCall took the image)
                    return m_snapshots.size() > level ? m_snapshots[level].get() : nullptr;
                }
                if (m_snapshots.size() <= level)
                    m_snapshots.resize(level + 1u);
                if (!m_snapshots[level])
                    m_snapshots[level] = std::make_unique<uint8_t[]>(kMemoryKeys);
                uint8_t *image = m_snapshots[level].get();
                std::memcpy(image, rdram, PS2_RAM_SIZE);
                uint8_t *scratchpad = ps2GetScratchpadHostPtr();
                if (scratchpad)
                    std::memcpy(image + kScratchpadKey, scratchpad, PS2_SCRATCHPAD_SIZE);
                const bool kept = tracker.pop(scratchpad, [&](uint32_t key, const uint8_t *before, const uint8_t *, uint32_t size) {
                    std::memcpy(image + key, before, size);
                });
                if (!kept)
                    return nullptr;
                m_snapshotted = level;
                return image;
            }

            // A function recorded to a stop (fncheck_stops.h): the record ends
            // at its one open call, whose callee never comes back to it. The
            // call keeps its target, arguments and sp (kStopCall); the outputs
            // are RAM as it was at the call (the before-image: what the callee
            // did before it left is not the function's), the exit the
            // registers at the call. Null if the call cannot be told apart
            // (more than one open, the image not whole).
            Record *stopAtOpenCall(uint8_t *rdram, size_t below)
            {
                if (m_broken || m_tooBig || m_callStack.size() != 1u || m_openCalls != 1u)
                    return nullptr;
                const uint8_t *image = openCallImage(rdram, below);
                if (!image)
                    return nullptr;
                const size_t index = m_callStack.back();
                m_record.calls.resize(index + 1u);
                Call &call = m_record.calls[index];
                call.flags |= kStopCall;
                call.after = R5900Context{};
                call.effects.clear();
                m_record.flags |= kStopsAtCall;
                m_callStack.clear();
                m_openCalls = 0;
                m_snapshotted = ~size_t(0);
                R5900Context exit = m_stopContext;
                exit.pc = call.target;
                return &finish(exit, image);
            }

            // Addresses are kept as memory keys (fncheck.h): physical RAM (the
            // game reaches the same RAM through the uncached mirrors,
            // 0x20000000 and 0x30000000, too) and the scratchpad after it.
            void read(uint32_t address, uint32_t size) override
            {
                if (game::diffcall::g_countWild)
                {
                    game::diffcall::noteAccess(address, size);
                    game::diffcall::noteRead(address, size);
                }
                if (m_tooBig)
                    return;
                const uint32_t key = memoryKey(address);
                readKey(key, size);
                if (m_caller)
                    for (uint32_t i = 0; i < size; ++i)
                    {
                        LineState &line = m_lines[lineOf(key + i)];
                        const uint16_t bit = static_cast<uint16_t>(1u << ((key + i) & 15u));
                        if (!((line.own | line.callee) & bit))
                            line.readFirst = static_cast<uint16_t>(line.readFirst | bit);
                    }
            }

            // The open call is a direct call of `target` (not a jump out),
            // the callee entered with the call's sp.
            bool directCall(uint32_t target, uint32_t sp) const
            {
                if (m_tooBig || m_callStack.empty() || m_callStack.back() >= m_record.calls.size())
                    return false;
                const Call &call = m_record.calls[m_callStack.back()];
                return call.target == target && call.sp == sp && !(call.flags & kTailCall);
            }
            // ... and hands the callee a pointer into this function's own
            // frame, [sp at the call, sp at entry), or into the frame of a
            // recorded caller it was called by directly (a pointer handed
            // on), in an argument register: the callee may read what was left
            // there, so it is run traced for its reads (recorderEntry)
            // whether its call is kept or not.
            bool framePassed(const R5900Context &atEntry) const
            {
                if (!directCall(atEntry.pc, static_cast<uint32_t>(ee::gpr64(&atEntry, ee::reg::sp))))
                    return false;
                for (int i = 0; i < ee::kIntArgRegs; ++i)
                    if (inFrames(memoryKey(static_cast<uint32_t>(ee::gpr64(&atEntry, ee::reg::a0 + i)))))
                        return true;
                return false;
            }
            bool inFrames(uint32_t key) const
            {
                if (m_tooBig || m_callStack.empty() || m_callStack.back() >= m_record.calls.size())
                    return false;
                const uint32_t low = memoryKey(m_record.calls[m_callStack.back()].sp), high = memoryKey(m_entrySp);
                if (key >= low && key < high)
                    return true;
                return m_caller && key >= high && key - high < kStackWindow && m_caller->inFrames(key);
            }
            // A byte the open call's callee read before anything wrote it,
            // with its value at the call: one of this function's frame goes
            // into the call's reads (Call::reads); one above it, in the frame
            // of the function that called this one, goes on to that call's
            // recording, unless this function or a callee of it changed it.
            void calleeRead(uint32_t key, uint8_t value)
            {
                if (m_tooBig || m_callStack.empty() || m_callStack.back() >= m_record.calls.size())
                    return;
                const size_t index = m_callStack.back();
                const uint32_t low = memoryKey(m_record.calls[index].sp), high = memoryKey(m_entrySp);
                if (key >= low && key < high)
                {
                    std::map<uint32_t, Line> &lines = m_callReads[index];
                    const auto it = lines.find(lineOf(key));
                    if (it == lines.end() || !(it->second.mask & (1u << (key & 15u))))
                        addByte(lines, key, value);
                    return;
                }
                if (!m_caller || key < high || key - high >= kStackWindow)
                    return;
                const auto it = m_lines.find(lineOf(key));
                if (it != m_lines.end() && ((it->second.own | it->second.callee) & (1u << (key & 15u))))
                    return;
                m_caller->calleeRead(key, value);
            }
            // At the end of a call made directly by another recorded function:
            // what it read above its own frame (its caller's frame, through a
            // pointer it was given) before writing it, to the caller's open
            // call (calleeRead).
            void forwardReads()
            {
                if (!m_caller || m_tooBig)
                    return;
                const uint32_t high = memoryKey(m_entrySp);
                for (const auto &[address, line] : m_lines)
                    if (line.readFirst && address + 16u > high && (address >= high ? address - high : 0u) < kStackWindow)
                        for (uint32_t i = 0; i < 16u; ++i)
                            if ((line.readFirst & (1u << i)) && (line.captured & (1u << i)) && address + i >= high)
                                m_caller->calleeRead(address + i, line.entry[i]);
            }
            // Parked at a wait: its caller's recording does not outlive it.
            void detachCaller() { m_caller = nullptr; }

            void write(uint32_t address, uint32_t size) override
            {
                if (game::diffcall::g_countWild)
                    game::diffcall::noteAccess(address, size, true);
                if (m_tooBig)
                    return;
                const uint32_t key = memoryKey(address);
                readKey(key, size); // keep the bytes' values from before the write
                for (uint32_t i = 0; i < size; ++i)
                {
                    LineState &line = m_lines[lineOf(key + i)];
                    line.own = static_cast<uint16_t>(line.own | (1u << ((key + i) & 15u)));
                }
            }

            void special(uint32_t) override { m_record.flags |= kSpecialAccess; }
            void io(const IoAccess &access) override
            {
                if (m_tooBig)
                    return;
                m_record.flags |= kIoLog;
                m_record.io.push_back(access);
                m_bytes += sizeof(IoAccess);
            }
            void cycles(uint32_t count) override { m_record.cycles += count; }
            bool inGame() const override { return true; }

            void beforeCall(uint8_t *rdram, R5900Context *ctx, uint32_t target) override
            {
                if (m_tooBig)
                    return;
                m_bytes += sizeof(Call);
                Call call;
                call.target = target;
                call.sp = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp));
                for (int i = 0; i < ee::kIntArgRegs; ++i)
                {
                    call.a[i] = ee::gpr64(ctx, ee::reg::a0 + i);
                    call.aHigh[i] = ee::gprHigh64(ctx, ee::reg::a0 + i);
                }
                call.highKnown = true;
                for (int i = 0; i < ee::kFloatArgRegs; ++i)
                    call.f[i] = ctx->f[ee::reg::f12 + i];
                m_minSp = std::min(m_minSp, call.sp);
                m_callRa = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
                if (m_openCalls == 0u)
                    m_stopContext = *ctx; // (the registers a stop at this call keeps: stopAtOpenCall)
                if (game::diffcall::g_countWild)
                    game::diffcall::noteCall(m_callRa, target);
                m_record.calls.push_back(std::move(call));
                if (g_trackPages && WriteTracker::instance().attach(rdram))
                    WriteTracker::instance().push(ps2GetScratchpadHostPtr());
                else
                {
                    if (m_snapshots.size() <= m_openCalls)
                        m_snapshots.emplace_back(std::make_unique<uint8_t[]>(kMemoryKeys));
                    std::memcpy(m_snapshots[m_openCalls].get(), rdram, PS2_RAM_SIZE);
                    if (const uint8_t *scratchpad = ps2GetScratchpadHostPtr())
                        std::memcpy(m_snapshots[m_openCalls].get() + kScratchpadKey, scratchpad, PS2_SCRATCHPAD_SIZE);
                }
                m_callStack.push_back(m_record.calls.size() - 1u);
                ++m_openCalls;
            }

            void afterCall(uint8_t *rdram, R5900Context *ctx, bool returned) override
            {
                if (m_tooBig)
                    return;
                afterCallBody(rdram, ctx, returned);
                // A call that never ends (engineMain, parked and resumed
                // at every wait of its game loop: 3 GB in 3 minutes, paths6)
                // or makes too many calls: given up, its memory freed
                // (ZX_FNREC_MAX_MB, default 512; drafter24).
                if (g_maxRecordBytes && m_bytes + m_lines.size() * 64u > g_maxRecordBytes)
                    giveUpTooBig();
            }
            bool tooBig() const { return m_tooBig; }
            void countAtEntry(uint32_t count)
            {
                m_record.entry.cop0_count = count;
                m_record.flags |= kEntryCount;
            }
            void giveUpTooBig()
            {
                m_tooBig = m_broken = true;
                const R5900Context entry = m_record.entry;
                m_record = Record{};
                m_record.entry = entry;
                m_callReads.clear();
                std::unordered_map<uint32_t, LineState>().swap(m_lines);
            }
            void afterCallBody(uint8_t *rdram, R5900Context *ctx, bool returned)
            {
                if (m_callStack.empty())
                    return;
                // The function's one open call unwound to the scheduler (not a
                // wait: no throw): kept open, so the session can be parked at
                // its return point (runSession); dropUnwound ends it as before.
                // A tail call's target that left for the scheduler too: parked
                // at the jump's ra.
                if (!returned && !m_dropping && g_unwindParking && m_callStack.size() == 1u && m_openCalls == 1u &&
                    !m_broken)
                {
                    m_unwoundOpen = true;
                    return;
                }
                m_unwoundOpen = false;
                const size_t index = m_callStack.back();
                m_callStack.pop_back();
                --m_openCalls;
                // (A call parked at a wait has its whole before-image: park.)
                const bool imaged = m_snapshotted == m_openCalls;
                m_snapshotted = ~size_t(0);
                const bool tracked = !imaged && g_trackPages && WriteTracker::instance().attach(rdram);
                if (!returned)
                {
                    if (tracked)
                        WriteTracker::instance().pop(ps2GetScratchpadHostPtr(), [](uint32_t, const uint8_t *, const uint8_t *, uint32_t) {});
                    m_broken = true;
                    return;
                }
                Call &call = m_record.calls[index];
                if (game::diffcall::g_countWild)
                    game::diffcall::noteCallReturn(ctx, m_callRa, call.target); // (a case may answer another result)
                call.after = *ctx;
                // What the callee changed, outside its own (dead) frame.
                const uint32_t deadLow = call.sp >= kStackWindow ? call.sp - kStackWindow : 0u;
                std::map<uint32_t, Line> effects;
                auto changedByte = [&](uint32_t address, uint8_t value) {
                    if (address >= deadLow && address < call.sp)
                        return;
                    addByte(effects, address, value);
                    LineState &line = m_lines[lineOf(address)];
                    line.callee = static_cast<uint16_t>(line.callee | (1u << (address & 15u)));
                };
                if (tracked)
                {
                    // Only the pages written since the call began (and the
                    // scratchpad, RAM's bytes outside whole pages).
                    const bool kept = WriteTracker::instance().pop(
                        ps2GetScratchpadHostPtr(),
                        [&](uint32_t key, const uint8_t *before, const uint8_t *now, uint32_t size) {
                            for (uint32_t offset = 0; offset < size; offset += 8u)
                            {
                                const uint32_t n = std::min(8u, size - offset);
                                if (std::memcmp(before + offset, now + offset, n) == 0)
                                    continue;
                                for (uint32_t i = 0; i < n; ++i)
                                    if (before[offset + i] != now[offset + i])
                                        changedByte(key + offset + i, now[offset + i]);
                            }
                        });
                    if (!kept)
                        m_broken = m_lostPages = true; // a page's copy was lost: the effects are not known
                    call.effects = flatten(effects);
                    m_bytes += call.effects.size() * sizeof(Line);
                    return;
                }
                const uint8_t *before = m_snapshots[m_openCalls].get();
                const uint32_t keys = ps2GetScratchpadHostPtr() ? kMemoryKeys : PS2_RAM_SIZE;
                for (uint32_t offset = 0; offset < keys; offset += 8u)
                {
                    const uint8_t *now = keyPointer(rdram, offset);
                    uint64_t a, b;
                    std::memcpy(&a, before + offset, 8);
                    std::memcpy(&b, now, 8);
                    if (a == b)
                        continue;
                    for (uint32_t i = 0; i < 8u; ++i)
                    {
                        const uint32_t address = offset + i;
                        if (before[address] == now[i] || (address >= deadLow && address < call.sp))
                            continue;
                        addByte(effects, address, now[i]);
                        LineState &line = m_lines[lineOf(address)];
                        line.callee = static_cast<uint16_t>(line.callee | (1u << (address & 15u)));
                    }
                }
                call.effects = flatten(effects);
                m_bytes += call.effects.size() * sizeof(Line);
            }

            void tailCall(bool direct) override
            {
                if (!m_record.calls.empty())
                    m_record.calls.back().flags |= kTailCall | (direct ? kDirectTailCall : 0u);
                m_tail = true;
                m_tailReturn = m_callRa;
            }
            // A tail call is open: the recorder runs it, then afterCall.
            bool tailPending() const { return m_tail && !m_callStack.empty(); }
            // The open tail call is a C++ call of its target in the game's
            // code (PS2Machine::tailCall), not a dispatched jump.
            bool tailDirect() const
            {
                return tailPending() && (m_record.calls[m_callStack.back()].flags & kDirectTailCall);
            }
            // No call open and nothing given up: the function's own code left
            // for the scheduler (runSession parks it to go on there).
            bool betweenCalls() const { return m_callStack.empty() && m_openCalls == 0u && !m_broken; }
            // Where the open tail call's target returns to (ra at the jump):
            // the function's own return address, or, for the jump that ends
            // an inner call of the function itself, the return point of that
            // self-call.
            uint32_t tailReturn() const { return m_tailReturn; }

            bool broken() const { return m_broken; }
            bool unwoundOpen() const { return m_unwoundOpen; }
            // Not parked after all: the open call ended as not returned.
            void dropUnwound(uint8_t *rdram, R5900Context *ctx)
            {
                m_dropping = true;
                afterCall(rdram, ctx, false);
                m_dropping = false;
            }
            bool lostPages() const { return m_lostPages; }

            // The record, from the state at the return (outputs from `image`,
            // memory keys, when given: a stop's, stopAtOpenCall).
            Record &finish(const R5900Context &exit, const uint8_t *image = nullptr)
            {
                m_record.exit = exit;
                m_record.deadLow = m_minSp >= kStackWindow ? m_minSp - kStackWindow : 0u;
                std::map<uint32_t, Line> inputs, outputs;
                for (const auto &[address, line] : m_lines)
                {
                    for (uint32_t i = 0; i < 16u; ++i)
                    {
                        const uint16_t bit = static_cast<uint16_t>(1u << i);
                        const uint32_t byteAddress = address + i;
                        if (line.captured & bit)
                            addByte(inputs, byteAddress, line.entry[i]);
                        const bool dead = byteAddress >= m_record.deadLow && byteAddress < m_entrySp;
                        if ((line.own | line.callee) & bit && !dead)
                            addByte(outputs, byteAddress, image ? image[byteAddress] : *keyPointer(m_rdram, byteAddress));
                    }
                }
                m_record.inputs = flatten(inputs);
                m_record.outputs = flatten(outputs);
                for (const auto &[index, lines] : m_callReads)
                    if (index < m_record.calls.size())
                        m_record.calls[index].reads = flatten(lines);
                return m_record;
            }

        private:
            struct LineState
            {
                uint8_t entry[16] = {};
                uint16_t captured = 0; // entry value known
                uint16_t own = 0;      // written by the function
                uint16_t callee = 0;   // changed by a callee (its entry value no longer in RAM)
                uint16_t readFirst = 0; // read before the function or a callee wrote it (kept with a caller: forwardReads)
            };

            void readKey(uint32_t key, uint32_t size)
            {
                for (uint32_t line = lineOf(key); line <= lineOf(key + size - 1u); line += 16u)
                    touch(line);
            }

            // Bytes not captured yet still hold their entry value, unless a
            // callee changed them (those come from the callee's effects).
            void touch(uint32_t address)
            {
                LineState &line = m_lines[address];
                const uint16_t need = static_cast<uint16_t>(~(line.captured | line.callee));
                if (!need)
                    return;
                const uint8_t *live = keyPointer(m_rdram, address);
                for (uint32_t i = 0; i < 16u; ++i)
                    if (need & (1u << i))
                        line.entry[i] = live[i];
                line.captured = static_cast<uint16_t>(line.captured | need);
            }

            uint8_t *m_rdram = nullptr;
            std::unordered_map<uint32_t, LineState> m_lines;
            Recording *m_caller = nullptr;                          // begin
            std::map<size_t, std::map<uint32_t, Line>> m_callReads; // by call: Call::reads
            Record m_record;
            uint32_t m_entrySp = 0, m_minSp = 0;
            bool m_broken = false;
            bool m_unwoundOpen = false, m_dropping = false; // (afterCall, dropUnwound)
            bool m_tooBig = false;   // given up: past ZX_FNREC_MAX_MB (afterCall)
            uint64_t m_bytes = 0;    // the record's calls, effects and I/O log, about
            bool m_lostPages = false;
            bool m_tail = false;
            uint32_t m_callRa = 0, m_tailReturn = 0;
            R5900Context m_stopContext{}; // the registers at the outermost open call (beforeCall)
            std::vector<std::unique_ptr<uint8_t[]>> m_snapshots;
            std::vector<size_t> m_callStack;
            size_t m_openCalls = 0;
            size_t m_snapshotted = ~size_t(0); // the open call (level) whose before-image park made whole
        };

        struct Recorded
        {
            game::readable::Function *function = nullptr;
            PS2Machine::RecompiledFunction previous = nullptr; // what ran here before the recorder
            bool created = false; // its file was begun by this run (later records are appended)
            uint64_t calls = 0, kept = 0, skipped = 0, lastVsync = ~0ull;
            uint64_t leftElsewhere = 0, calleeUnwound = 0, lostPages = 0; // why calls were skipped
            // Parked calls not gone on with: the table has one of the recompiler's
            // entry blocks at the pc (the game runs the rest slice by slice, which
            // the traced original does not), or something else ran while it was
            // parked between calls or its callee went through the scheduler.
            uint64_t inEntryBlock = 0, interrupted = 0;
            uint64_t tooBig = 0; // given up past ZX_FNREC_MAX_MB
            // Recorded to a stop (fncheck_stops.h, ZX_FNREC_STOPS): its rule, and the calls kept at one.
            StopRule stop = StopRule::None;
            uint64_t stopped = 0;
            uint64_t selfCallsCut = 0; // of leftElsewhere: still in a call of itself after kMaxSelfCallResumes
            // Calls that waited (sceMcSync, a semaphore: the wait throws to the
            // scheduler through the recorder, so the call never comes back here)
            // and could not be parked (runSession): not kept, counted (paths1:
            // 1 of the 90 card functions kept before drafter23 parked them).
            uint64_t waited = 0;
            // Calls not kept that ran traced for their reads of a recorded
            // caller's frame (Session::keep false).
            uint64_t watched = 0;
            uint32_t firstElsewhere = 0;
            uint32_t thisVsync = 0, stride = 1;
            uint32_t max = 3000, perVsync = 64; // its caps (ZX_FNREC_MAX / _PER_VSYNC, or its line in @file)
            uint64_t strideBlock = 250;         // calls kept before the stride doubles
            // ZX_FNREC_NEW_BLOCKS: the original's block table, the blocks a
            // kept call (or the ZX_FNREC_COVERED seed) ran, and the calls
            // recorded and dropped because they ran none new.
            const fncheck::BlockTable *blocks = nullptr;
            std::vector<uint8_t> seen;
            uint64_t dull = 0, seeded = 0, added = 0;
            // Which of a vsync's calls are recorded: every `pickStep`th from
            // `pickPhase` (random), from the count the vsync before made, so
            // the calls spread over the frame's callers, not its first ones.
            uint32_t callsThisVsync = 0, pickStep = 1, pickPhase = 0;
            // Calls recorded in a row that ran no new block: every 16 of them
            // the function rests twice as long (vsyncs, at most kMaxRest)
            // before it is recorded again; a new block ends the rest, and so
            // does a vsync without a call (a new episode).
            uint32_t dullStreak = 0;
            uint64_t restUntil = 0;
            // ZX_FNREC_NEW_BLOCKS=3: the calls past the capped sample tried
            // (by the rules above, on their own vsync count) and the ones kept
            // for a new block; `kept` and the caps count the sample alone.
            uint64_t probeVsync = ~0ull, probesKept = 0;
            uint32_t probesThisVsync = 0;
        };

        // The blocks of one recorded call's original (ZX_FNREC_NEW_BLOCKS).
        class CallBlocks final : public fncheck::BlockSink
        {
        public:
            explicit CallBlocks(const fncheck::BlockTable *table) : m_table(table), m_hit(table ? table->count : 0u, 0u) {}
            void hit(const fncheck::BlockTable &table, uint32_t block) override
            {
                if (&table == m_table && block < m_hit.size())
                {
                    m_hit[block] = 1;
                    if (game::diffcall::g_countWild)
                        game::diffcall::noteBlock(table.starts[block]);
                }
            }
            const std::vector<uint8_t> &blocks() const { return m_hit; }

        private:
            const fncheck::BlockTable *m_table;
            std::vector<uint8_t> m_hit;
        };

        // The recordings' files, at most kOpenFiles open at once (Windows'
        // C runtime allows 512 open streams; a sweep records thousands of
        // functions): the least recently written is closed, and opened again
        // to append when its function keeps another call.
        class FileCache
        {
        public:
            static constexpr size_t kOpenFiles = 64;

            RecordingWriter &get(const std::filesystem::path &folder, Recorded &r)
            {
                auto it = m_open.find(r.function->address);
                if (it != m_open.end())
                {
                    m_order.splice(m_order.begin(), m_order, it->second.order);
                    return *it->second.file;
                }
                if (m_open.size() >= kOpenFiles)
                {
                    const uint32_t oldest = m_order.back();
                    m_order.pop_back();
                    m_open.erase(oldest);
                }
                const std::string name = recordingName(r.function->address, r.function->game);
                auto file = std::make_unique<RecordingWriter>();
                if (!r.created)
                {
                    std::filesystem::create_directories(folder);
                    FileHeader header;
                    header.address = r.function->address;
                    header.game = static_cast<uint32_t>(r.function->game);
                    file->create(folder / name, header, recorderCompresses());
                    r.created = true;
                }
                else
                    file->append(folder / name);
                m_order.push_front(r.function->address);
                Open &o = m_open[r.function->address];
                o.file = std::move(file);
                o.order = m_order.begin();
                return *o.file;
            }

        private:
            struct Open
            {
                std::unique_ptr<RecordingWriter> file;
                std::list<uint32_t>::iterator order;
            };
            std::unordered_map<uint32_t, Open> m_open;
            std::list<uint32_t> m_order; // most recently written first
        };

        struct RecorderState
        {
            std::filesystem::path folder;
            uint32_t max = 3000;
            uint32_t perVsync = 64;
            uint64_t fromVsync = 0; // ZX_FNREC_FROM: no calls kept before this vsync
            // ZX_FNREC_UNTIL_SCRIPT=<n>: no calls begun once the pad script's clock
            // (ZX_PAD_SCRIPT, in vsyncs) is at n: a route's recording ends at its
            // last input's guest moment on every host, not when the run is stopped.
            bool untilSet = false;
            int64_t untilScript = 0;
            uint32_t minCalls = 0;
            // ZX_FNREC_NEW_BLOCKS=1: a call is kept only if it ran a block of
            // its original that no kept call ran (nor the ZX_FNREC_COVERED
            // seed); the per-vsync cap counts the calls recorded, kept or not,
            // and there is no stride.
            bool newBlocks = false;
            // ZX_FNREC_NEW_BLOCKS=2: every call is tried (no rest after dull
            // calls, no per-vsync cap, no sampling within a vsync), for one hot
            // function's rare paths; still only the calls that run a new block
            // are kept (gamehand2).
            bool everyCall = false;
            // ZX_FNREC_NEW_BLOCKS=3: the capped sample as with the variable
            // unset (the same calls, kept whatever they ran), and also every
            // call past it that runs a block of its original no kept call ran
            // (nor the ZX_FNREC_COVERED seed), tried as with 1 (a few a
            // vsync, picked across the frame, resting after dull ones).
            bool alsoNewBlocks = false;
            uint32_t stopFrames = 1; // ZX_FNREC_STOP_FRAMES: the frame rule's vsyncs (fncheck_stops.h)
            std::map<uint32_t, std::unique_ptr<Recorded>> functions;
            std::vector<std::unique_ptr<Recording>> recordings; // one per nesting depth
            size_t depth = 0;
            FileCache files;
        };
        RecorderState *g_recorder = nullptr;

        // Every call until kStrideBlock are kept, then every 2nd, 4th ...; a
        // function with a small cap (a sweep's 200 or 50) doubles its stride
        // after an eighth of its cap, so its calls still spread over the run.
        constexpr uint64_t kStrideBlock = 250;
        uint64_t strideBlockFor(uint32_t max) { return std::min<uint64_t>(kStrideBlock, std::max<uint64_t>(8u, max / 8u)); }
        constexpr uint32_t kMaxResumes = kMaxSelfCallResumes;
        constexpr uint32_t kMaxStride = 64;
        constexpr uint64_t kMaxRest = 120; // vsyncs (ZX_FNREC_NEW_BLOCKS: two seconds)

        bool pastUntil()
        {
            return g_recorder->untilSet && StateProbe::scriptTime() >= g_recorder->untilScript;
        }

        bool wanted(Recorded &r, uint64_t vsync)
        {
            ++r.calls;
            if (pastUntil())
                return false;
            if (r.kept >= r.max || vsync < g_recorder->fromVsync)
                return false;
            if (vsync != r.lastVsync)
            {
                if (g_recorder->newBlocks && r.blocks)
                {
                    // Not called the vsync before (a state's callback, a
                    // move's code): a new episode, recorded from its start.
                    if (r.lastVsync == ~0ull || vsync > r.lastVsync + 1u)
                    {
                        r.dullStreak = 0;
                        r.restUntil = 0;
                    }
                    static uint64_t seed = 0x9E3779B97F4A7C15ull;
                    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                    r.pickStep = std::max(1u, r.callsThisVsync / r.perVsync);
                    r.pickPhase = static_cast<uint32_t>(seed >> 33) % r.pickStep;
                    r.callsThisVsync = 0;
                }
                r.lastVsync = vsync;
                r.thisVsync = 0;
            }
            if (g_recorder->newBlocks && r.blocks)
            {
                const uint32_t index = r.callsThisVsync++;
                if (g_recorder->everyCall)
                    return true;
                if (vsync < r.restUntil)
                    return false;
                if (r.thisVsync >= r.perVsync || index % r.pickStep != r.pickPhase)
                    return false;
                ++r.thisVsync; // counts the calls recorded: most are dropped
                return true;
            }
            if (r.thisVsync >= r.perVsync || (r.calls - 1u) % r.stride != 0u)
                return false;
            return true;
        }

        // ZX_FNREC_NEW_BLOCKS=3: whether a call the capped sample does not
        // keep is tried for a new block, by ZX_FNREC_NEW_BLOCKS=1's rules on
        // the calls past the sample (its own vsync count and random picks).
        bool probeWanted(Recorded &r, uint64_t vsync)
        {
            if (vsync < g_recorder->fromVsync || pastUntil())
                return false;
            if (vsync != r.probeVsync)
            {
                if (r.probeVsync == ~0ull || vsync > r.probeVsync + 1u)
                {
                    r.dullStreak = 0;
                    r.restUntil = 0;
                }
                static uint64_t seed = 0xD1B54A32D192ED03ull;
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                r.pickStep = std::max(1u, r.callsThisVsync / r.perVsync);
                r.pickPhase = static_cast<uint32_t>(seed >> 33) % r.pickStep;
                r.callsThisVsync = 0;
                r.probeVsync = vsync;
                r.probesThisVsync = 0;
            }
            const uint32_t index = r.callsThisVsync++;
            if (vsync < r.restUntil)
                return false;
            if (r.probesThisVsync >= r.perVsync || index % r.pickStep != r.pickPhase)
                return false;
            ++r.probesThisVsync;
            return true;
        }

        // A call of the function itself is, in generated code, a jump to the
        // function's start with ra at the return point (the self-call's jal
        // is not dispatched). The inner call's return, its jr ra or the jump
        // of its tail call, then leaves the C++ function with pc at that
        // return point, where the dispatcher goes on with the outer call (a
        // resume entry: every jal's return point is one). True if pc is such
        // a return point of fn: the instruction two before it is a jal to fn.
        bool selfCallReturn(const uint8_t *rdram, uint32_t function, uint32_t pc)
        {
            if (pc < function + 8u || pc - function >= kMaxFunctionSpan)
                return false;
            uint32_t word = 0;
            std::memcpy(&word, rdram + ((pc - 8u) & PS2_RAM_MASK), 4);
            return word == (0x0C000000u | ((function >> 2) & 0x03FFFFFFu));
        }

        // ---- recording across a wait (drafter23) ------------------------------
        //
        // A callee that waits (sceMcSync, a semaphore) throws to the scheduler
        // through the recorder, and the thread goes on later at the callee's
        // return point, dispatched through the table. The recording is parked
        // there: its open call's memory before-image kept whole (with page
        // protection, the copies of the pages written so far put back over
        // RAM), the return point's table slot taken; when the thread comes back
        // to that address with that sp, the call ends (its effects: what
        // changed while it waited, by anyone) and the traced function goes on
        // from there, to its return as usual, parked again at another wait.
        // At most kMaxParked at once (each holds a RAM image); others are
        // dropped as before (Recorded::waited). ZX_FNREC_WAITS=0: none.
        struct Session
        {
            Recorded *recorded = nullptr;
            // False: a call not kept, run traced only for what it reads of its
            // caller's frame (Recording::framePassed); never written or parked.
            bool keep = true;
            // ZX_FNREC_NEW_BLOCKS=3: a call past the capped sample, kept
            // only if it runs a new block (keepRecord).
            bool probe = false;
            std::unique_ptr<Recording> recording;
            std::unique_ptr<CallBlocks> blocks;
            uint32_t returnPc = 0, entrySp = 0;
            uint64_t vsync = 0, sequence = 0;
            // Parked: where the thread comes back.
            uint32_t resumePc = 0, resumeSp = 0;
            int thread = 0;
            uint64_t parkedAt = 0; // the vsync
            // How it goes on there: its open call ends (what changed meanwhile,
            // by anyone, is that call's effects), or, parked between calls (its
            // own code left for the scheduler: an inner self-call's return, a
            // jump within it), only if nothing ran meanwhile but the
            // scheduler's dispatch of that pc and no checkpoint came due since
            // its code began to run (`steps`: EeScheduler::scheduleSteps then),
            // else it is not kept.
            bool openCall = true;
            uint64_t steps = 0;
            // Parked at its open call whose callee went through the scheduler
            // with no wait (unwound): it goes on only if nothing but this
            // thread's code ran meanwhile (`interleaves`: EeScheduler::interleaves
            // when its code began to run), so that the call's effects are the
            // callee's own (other threads' and interrupts' writes are no call's:
            // the readable version that calls it natively would not make them).
            bool unwound = false;
            uint64_t interleaves = 0;
            uint32_t resumes = 0; // the outer call gone on after an inner self-call's return
        };
        // A record made (at the function's return, or at its stop): written,
        // unless ZX_FNREC_MIN_CALLS or ZX_FNREC_NEW_BLOCKS leaves it out.
        void keepRecord(Session &session, Record &record)
        {
            RecorderState &state = *g_recorder;
            Recorded &r = *session.recorded;
            // ZX_FNREC_MIN_CALLS=<n>: only calls that made at least n calls
            // (the rare paths of a function that mostly takes one).
            if (record.calls.size() < state.minCalls)
                return;
            if (state.newBlocks && r.blocks)
            {
                // Kept only if it ran a block no kept call (nor the seed) ran.
                const std::vector<uint8_t> &hit = session.blocks->blocks();
                uint32_t added = 0;
                for (size_t b = 0; b < hit.size(); ++b)
                    if (hit[b] && !r.seen[b])
                    {
                        r.seen[b] = 1;
                        ++added;
                    }
                if (!added)
                {
                    ++r.dull;
                    if (++r.dullStreak % 16u == 0u && !state.everyCall)
                        r.restUntil = session.vsync + std::min<uint64_t>(kMaxRest, 1ull << std::min(r.dullStreak / 16u, 16u));
                    return;
                }
                r.dullStreak = 0;
                r.restUntil = 0;
                r.added += added;
                std::cout << "[fncheck:newblocks] address=0x" << std::hex << r.function->address << std::dec
                          << " name=" << r.function->name << " vsync=" << session.vsync << " added=" << added
                          << " total=" << (r.seeded + r.added) << " of=" << r.blocks->count << std::endl;
            }
            else if (state.alsoNewBlocks && r.blocks && session.blocks)
            {
                // The sample's blocks are seen whatever they are; a probe is
                // kept only for a block no kept call (nor the seed) ran.
                const std::vector<uint8_t> &hit = session.blocks->blocks();
                uint32_t added = 0;
                for (size_t b = 0; b < hit.size(); ++b)
                    if (hit[b] && !r.seen[b])
                    {
                        r.seen[b] = 1;
                        ++added;
                    }
                if (session.probe)
                {
                    if (!added)
                    {
                        ++r.dull;
                        if (++r.dullStreak % 16u == 0u)
                            r.restUntil = session.vsync + std::min<uint64_t>(kMaxRest, 1ull << std::min(r.dullStreak / 16u, 16u));
                        return;
                    }
                    r.dullStreak = 0;
                    r.restUntil = 0;
                    r.added += added;
                    std::cout << "[fncheck:newblocks] address=0x" << std::hex << r.function->address << std::dec
                              << " name=" << r.function->name << " vsync=" << session.vsync << " added=" << added
                              << " total=" << std::count(r.seen.begin(), r.seen.end(), uint8_t{1})
                              << " of=" << r.blocks->count << " past the sample" << std::endl;
                }
            }
            record.sequence = session.sequence;
            record.vsync = session.vsync;
            state.files.get(state.folder, r).write(record); // (written through: runs are often ended by a kill)
            if (session.probe)
            {
                ++r.probesKept; // (the sample's caps and stride count the sample alone)
                return;
            }
            ++r.kept;
            ++r.thisVsync;
            if (!state.newBlocks && r.kept % r.strideBlock == 0u && r.stride < kMaxStride)
                r.stride *= 2u;
        }

        constexpr size_t kMaxParked = 8;
        constexpr uint64_t kStaleVsyncs = 300; // five seconds
        uint64_t g_staleCount = 0;
        std::vector<std::unique_ptr<Session>> g_parked;
        std::unordered_map<uint32_t, PS2Machine::RecompiledFunction> g_resumeSlots; // pc -> what ran there before
        bool g_acrossWaits = true;
        uint64_t g_parkedCount = 0, g_resumedCount = 0;
        uint64_t g_unwoundParked = 0; // of g_parkedCount: unwound at a safe point, no wait
        uint64_t g_betweenParked = 0; // of g_parkedCount: parked between calls
        // Why a wait could not be parked (the recorder's summary line).
        uint64_t g_notParked[6] = {}; // off or full, not one open call, no table entry, the RAM image, the tracker
        const char *const kNotParked[] = {"off or 8 parked", "not one open call (a wait in its own code)",
                                          "no table entry at the return point", "the RAM image", "the tracker's interval"};
        void recorderResume(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);

        // The function the table has at pc for the game to go on with (what ran
        // there before a parked session took the slot).
        PS2Machine::RecompiledFunction gameCodeAt(PS2Machine *runtime, uint32_t pc)
        {
            const auto it = g_resumeSlots.find(pc);
            return it != g_resumeSlots.end() ? it->second : runtime->tableEntry(pc);
        }

        bool inTracedChunks(uint32_t function, uint32_t pc)
        {
            const auto it = g_tracedChunks.find({static_cast<int>(g_tracedGame), function});
            if (it == g_tracedChunks.end())
                return false;
            for (const auto &[start, end] : it->second)
                if (pc >= start && pc < end)
                    return true;
            return false;
        }

        // Whether the game goes on at pc as the function's traced original does
        // from there: the table has the function itself at pc (its switch), or
        // pc is in one of its chunks (an entry_ of the recompiler's own, which
        // the traced original runs chunk by chunk as the game does); not one of
        // the recompiler's entry blocks of a whole function, which run the rest
        // slice by slice through the scheduler (each slice's dispatch a charge
        // and a safe point the traced original does not have).
        bool goesOnWhole(const Recorded &r, PS2Machine *runtime, uint32_t pc)
        {
            return (r.previous && gameCodeAt(runtime, pc) == r.previous) || inTracedChunks(r.function->address, pc);
        }

        // Runs a session (begun, or resumed where it was parked) to its return.
        // `session` owns it; a wait parks it (moved away). Traced code runs only
        // where the game's own code would: wherever the game's code leaves for
        // the scheduler (a callee or a jump that does not come back, an inner
        // self-call's return, a jump within the function), so does the session's,
        // and it is parked to go on when the scheduler comes back there, or not
        // kept. False if it ran no guest code (a resumed session that ended, or
        // was given up, at its resume point: the game's code there is still to
        // run).
        bool runSession(std::unique_ptr<Session> &session, uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime,
                        bool resumed)
        {
            RecorderState &state = *g_recorder;
            Recorded &r = *session->recorded;
            Recording &recording = *session->recording;
            const uint32_t returnPc = session->returnPc;
            const uint32_t entrySp = session->entrySp;
            // Back at the caller: the return address with the caller's sp (a
            // nested call of the function returns to the same address deeper).
            auto atReturn = [&] {
                return ctx->pc == returnPc && static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)) == entrySp;
            };
            bool ran = !resumed;
            {
                // Restored however the call ends (a wait throws to the scheduler):
                // the tracer, the depth, and the write tracker's intervals of
                // calls that did not come back; a wait parks the session.
                struct Scope
                {
                    Tracer *outer;
                    size_t &depth;
                    size_t intervals;
                    fncheck::BlockSink *outerSink;
                    std::unique_ptr<Session> &session;
                    uint8_t *rdram;
                    PS2Machine *runtime;
                    int exceptions = std::uncaught_exceptions();
                    ~Scope()
                    {
                        if (std::uncaught_exceptions() > exceptions && session->keep && !stop(true) && !park())
                        {
                            ++session->recorded->waited;
                            ++session->recorded->skipped;
                        }
                        t_tracer = outer;
                        --depth;
                        WriteTracker::instance().unwindTo(intervals);
                        fncheck::t_blockSink = outerSink;
                    }
                    // A function recorded to a stop (fncheck_stops.h): its open call
                    // ends the record (kept, not parked) when the thread ended in
                    // it (`threw`: the callee threw to the scheduler, and the
                    // thread that made the call is gone or dormant), or, by the
                    // frame rule, when it leaves for the scheduler once
                    // ZX_FNREC_STOP_FRAMES vsyncs have passed since the entry.
                    bool stop(bool threw)
                    {
                        Recorded &r = *session->recorded;
                        if (r.stop == StopRule::None || !session->keep)
                            return false;
                        const GuestThread *thread = runtime->eeScheduler().thread(session->thread);
                        const bool ended = threw && (!thread || thread->status == EeThreadStatus::Dormant);
                        const uint64_t now = runtime->eeScheduler().currentVSyncTick();
                        const bool late = r.stop == StopRule::Frames && now >= session->vsync + g_recorder->stopFrames;
                        if (!ended && !late)
                            return false;
                        Record *record = session->recording->stopAtOpenCall(rdram, intervals);
                        if (!record)
                            return false;
                        ++r.stopped;
                        keepRecord(*session, *record);
                        return true;
                    }
                    // Room for one more parked session (the oldest goes if it has
                    // waited kStaleVsyncs: its thread went on elsewhere or ended,
                    // counted as waited).
                    bool room(uint64_t now)
                    {
                        if (g_acrossWaits && g_parked.size() >= kMaxParked && g_parked.front()->parkedAt + kStaleVsyncs < now)
                        {
                            ++g_parked.front()->recorded->waited;
                            ++g_parked.front()->recorded->skipped;
                            g_parked.erase(g_parked.begin());
                            ++g_staleCount;
                        }
                        return g_acrossWaits && g_parked.size() < kMaxParked;
                    }
                    // Parked at pc (the thread comes back there with sp): the slot
                    // taken, the session moved away.
                    void parkAt(uint32_t pc, uint32_t sp, uint64_t now)
                    {
                        session->recording->detachCaller();
                        session->parkedAt = now;
                        session->resumePc = pc;
                        session->resumeSp = sp;
                        // (session->thread: the thread that made the call; the
                        // scheduler has none current while a wait throws.)
                        if (!g_resumeSlots.count(pc))
                        {
                            g_resumeSlots[pc] = runtime->lookupFunction(pc);
                            runtime->replaceFunction(pc, &recorderResume);
                        }
                        if (std::getenv("ZX_FNREC_WAITS_DEBUG"))
                            std::cerr << "[fncheck] parked " << session->recorded->function->name << " at 0x" << std::hex
                                      << pc << " sp 0x" << sp << std::dec << " thread " << session->thread
                                      << (session->openCall ? "" : " (between calls)") << std::endl;
                        g_parked.push_back(std::move(session));
                        ++g_parkedCount;
                    }
                    // At its open call's return point: the callee waits (what
                    // anyone changes meanwhile is the call's effects), or it went
                    // through the scheduler with no wait (`unwound`: only while
                    // nothing but this thread's code runs, from `interleaves`).
                    bool park(bool unwound = false, uint64_t interleaves = 0)
                    {
                        Recording &rec = *session->recording;
                        if (!session->keep)
                            return false;
                        uint32_t pc = 0, sp = 0;
                        const uint64_t now = runtime->eeScheduler().currentVSyncTick();
                        if (!room(now))
                            return ++g_notParked[0], false;
                        if (!rec.parkable(pc, sp))
                            return ++g_notParked[1], false;
                        if (!runtime->hasFunction(pc))
                            return ++g_notParked[2], false;
                        if (!rec.park(rdram, intervals))
                            return ++g_notParked[3], false;
                        session->openCall = true;
                        session->unwound = unwound;
                        session->interleaves = interleaves;
                        parkAt(pc, sp, now);
                        return true;
                    }
                    // Between calls, at the pc its own code left at (the
                    // function goes on there as a whole in the game, or it is
                    // not kept).
                    bool parkBetweenCalls(uint32_t pc, uint32_t sp, uint64_t steps)
                    {
                        Recorded &r = *session->recorded;
                        if (!session->keep || !session->recording->betweenCalls())
                            return false;
                        const uint64_t now = runtime->eeScheduler().currentVSyncTick();
                        if (!room(now))
                            return ++g_notParked[0], false;
                        if (!runtime->hasFunction(pc))
                            return ++g_notParked[2], false;
                        if (!goesOnWhole(r, runtime, pc))
                            return ++r.inEntryBlock, false;
                        session->openCall = false;
                        session->steps = steps;
                        parkAt(pc, sp, now);
                        ++g_betweenParked;
                        return true;
                    }
                } scope{t_tracer, state.depth, WriteTracker::instance().depth(), fncheck::t_blockSink, session, rdram,
                        runtime};
                t_tracer = &recording;
                if (session->blocks)
                    fncheck::t_blockSink = session->blocks.get();
                ++state.depth;
                // The schedule steps when the traced code last began to run: a
                // checkpoint that came due in it (its events handled before the
                // next dispatch, maybe writing RAM) counts against a park
                // between calls as anything else that runs meanwhile does.
                uint64_t segmentSteps = 0, segmentInterleaves = 0;
                auto runTraced = [&]
                {
                    ran = true;
                    segmentSteps = runtime->eeScheduler().scheduleSteps();
                    segmentInterleaves = runtime->eeScheduler().interleaves();
                    r.function->traced(rdram, ctx, runtime);
                };
                bool givenUp = false; // at the resume point (counted there)
                if (resumed && ((!session->openCall && runtime->eeScheduler().scheduleSteps() > session->steps + 1u) ||
                                (session->openCall && session->unwound &&
                                 runtime->eeScheduler().interleaves() != session->interleaves)))
                {
                    // Something else ran while it was parked between calls (an
                    // interrupt, another thread: what it changed is no call's),
                    // or while its callee went through the scheduler.
                    ++r.interrupted;
                    givenUp = true;
                }
                else if (resumed)
                {
                    if (session->openCall)
                        recording.afterCall(rdram, ctx, true); // the call that waited came back
                    if (!atReturn() && !recording.broken())
                    {
                        if (goesOnWhole(r, runtime, ctx->pc))
                            runTraced();
                        else
                        {
                            ++r.inEntryBlock;
                            givenUp = true;
                        }
                    }
                }
                else
                    runTraced();
                if (givenUp)
                {
                    ++r.skipped;
                    t_tracer = scope.outer;
                    return false;
                }
                if (recording.tailPending())
                {
                    // It left by a jump: the target runs as in the game (a direct
                    // tail call is a C++ call of the function the table has there,
                    // PS2Machine::tailCall; a dispatched jump has left for the
                    // scheduler already), untraced (it is a callee), and its
                    // effects are a call's. Back at the function's own return
                    // address, or, for a jump that ends an inner self-call, at
                    // that self-call's return point, the call ends; anywhere
                    // else it left for the scheduler, and the call is parked at
                    // the jump's ra (below).
                    const uint32_t back = recording.tailReturn();
                    const bool inner = back != returnPc && selfCallReturn(rdram, r.function->address, back);
                    t_tracer = scope.outer;
                    if (recording.tailDirect() && runtime->hasFunction(ctx->pc))
                        runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
                    recording.afterCall(rdram, ctx, atReturn() || (inner && ctx->pc == back));
                    t_tracer = &recording;
                }
                // Back in the scheduler without a throw: an open call was
                // unwound at a safe point (a checkpoint due in a callee; the
                // scheduler goes on with it later, and its return comes back
                // through the table at the return point). Parked as a call
                // that waits is (drafter24: renderFrame and its
                // caller renderCountedFrame, 352,552 calls on paths5's routes and none
                // kept: every call is unwound somewhere in its callees).
                if (recording.unwoundOpen())
                {
                    if (!atReturn() && session->keep && scope.stop(false))
                        return true;
                    if (!atReturn() && session->keep && scope.park(true, segmentInterleaves))
                    {
                        ++g_unwoundParked;
                        return true;
                    }
                    recording.dropUnwound(rdram, ctx);
                }
                // Its own code left for the scheduler with no call open: an
                // inner self-call returned (generated code runs it inline, and
                // its return leaves the C++ function at the return point), a
                // jump within it, a safe point of its own. The game goes on
                // there through the table, and so does the session if the
                // table has the function itself there (parkBetweenCalls).
                const uint32_t pc = ctx->pc;
                const bool selfReturn = selfCallReturn(rdram, r.function->address, pc);
                if (!atReturn() && (!selfReturn || session->resumes < kMaxResumes))
                {
                    if (selfReturn)
                        ++session->resumes;
                    if (scope.parkBetweenCalls(pc, static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)), segmentSteps))
                        return true;
                }
                t_tracer = scope.outer;
            }
            // Kept only if it ran to its return (in one go, or across waits).
            if (!atReturn() || recording.broken())
            {
                if (!session->keep)
                    return ran;
                ++r.skipped;
                if (recording.tooBig())
                    ++r.tooBig; // (wherever it was when given up)
                else if (!atReturn())
                {
                    static uint32_t s_shown = 0;
                    if (std::getenv("ZX_FNREC_WAITS_DEBUG") && s_shown++ < 8)
                        std::cerr << "[fncheck] " << r.function->name << " left elsewhere: pc 0x" << std::hex << ctx->pc
                                  << " sp 0x" << ee::gpr64(ctx, ee::reg::sp) << ", its return 0x" << returnPc << " sp 0x"
                                  << entrySp << " ra 0x" << ee::gpr64(ctx, ee::reg::ra) << std::dec << " thread "
                                  << runtime->eeScheduler().currentThreadId() << " (made the call: " << session->thread
                                  << ")" << std::endl;
                    if (r.leftElsewhere++ == 0u)
                        r.firstElsewhere = ctx->pc;
                    if (session->resumes == kMaxResumes && selfCallReturn(rdram, r.function->address, ctx->pc))
                        ++r.selfCallsCut;
                }
                else if (recording.lostPages())
                    ++r.lostPages;
                else
                    ++r.calleeUnwound;
                return ran;
            }
            if (resumed)
                ++g_resumedCount;
            // What it read of its caller's frame, to the caller's open call.
            recording.forwardReads();
            if (!session->keep)
            {
                ++r.watched;
                return ran;
            }
            keepRecord(*session, recording.finish(*ctx));
            return ran;
        }

        void recorderEntry(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const auto it = g_recorder->functions.find(ctx->pc);
            if (it == g_recorder->functions.end())
            {
                std::fprintf(stderr, "[fncheck] recorder entered at 0x%x\n", ctx->pc);
                return;
            }
            Recorded &r = *it->second;
            const uint64_t vsync = runtime->eeScheduler().currentVSyncTick();
            // A call made directly by a recorded call: its reads of the
            // caller's frame go to the caller's record (Call::reads), and a
            // call handed a pointer into that frame is run traced for them
            // even when it is not kept.
            Recording *caller = dynamic_cast<Recording *>(t_tracer);
            if (caller && !caller->directCall(ctx->pc, static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp))))
                caller = nullptr;
            bool keep = wanted(r, vsync);
            bool probe = false;
            if (!keep && g_recorder->alsoNewBlocks && r.blocks && probeWanted(r, vsync))
                keep = probe = true;
            if (!keep && !(caller && caller->framePassed(*ctx)))
            {
                r.previous(rdram, ctx, runtime);
                return;
            }
            RecorderState &state = *g_recorder;
            auto session = std::make_unique<Session>();
            session->recorded = &r;
            session->keep = keep;
            session->probe = probe;
            // One Recording per nesting depth, reused (its RAM images with it);
            // a parked one takes its own away.
            if (state.recordings.size() <= state.depth)
                state.recordings.resize(state.depth + 1u);
            if (!state.recordings[state.depth])
                state.recordings[state.depth] = std::make_unique<Recording>();
            session->recording = std::move(state.recordings[state.depth]);
            session->returnPc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
            session->entrySp = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp));
            session->vsync = vsync;
            session->sequence = r.calls - 1u;
            session->thread = runtime->eeScheduler().currentThreadId();
            session->recording->begin(rdram, *ctx, caller);
            session->recording->countAtEntry(runtime->readCop0Count());
            if ((state.newBlocks || (state.alsoNewBlocks && keep)) && r.blocks)
                session->blocks = std::make_unique<CallBlocks>(r.blocks);
            // Its Recording goes back to this depth when the session ends here
            // (not parked), however it ends.
            struct GiveBack
            {
                RecorderState &state;
                size_t depth;
                std::unique_ptr<Session> &session;
                ~GiveBack()
                {
                    if (session && session->recording && !state.recordings[depth])
                        state.recordings[depth] = std::move(session->recording);
                }
            } give{state, state.depth, session};
            runSession(session, rdram, ctx, runtime, false);
        }

        // The table slot of a parked session's return point: the session whose
        // thread comes back there with its sp goes on; anything else runs what
        // ran there before.
        void recorderResume(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t pc = ctx->pc;
            const uint32_t sp = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp));
            const int thread = runtime->eeScheduler().currentThreadId();
            for (size_t i = 0; i < g_parked.size(); ++i)
            {
                Session &p = *g_parked[i];
                if (p.resumePc != pc || p.resumeSp != sp || p.thread != thread)
                    continue;
                std::unique_ptr<Session> session = std::move(g_parked[i]);
                g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
                // Ended or given up right here: the game's code at pc runs now,
                // for this one dispatch of it.
                if (!runSession(session, rdram, ctx, runtime, true))
                    if (PS2Machine::RecompiledFunction game = gameCodeAt(runtime, pc))
                        game(rdram, ctx, runtime);
                return;
            }
            static uint32_t s_miss = 0;
            if (std::getenv("ZX_FNREC_WAITS_DEBUG") && s_miss++ < 16)
                for (const auto &p : g_parked)
                    std::cerr << "[fncheck] resume slot 0x" << std::hex << pc << " sp 0x" << sp << std::dec << " thread "
                              << thread << ": parked 0x" << std::hex << p->resumePc << " sp 0x" << p->resumeSp << std::dec
                              << " thread " << p->thread << std::endl;
            const auto it = g_resumeSlots.find(pc);
            if (it != g_resumeSlots.end() && it->second)
                it->second(rdram, ctx, runtime);
        }

        void reportRecorder(PS2Machine &)
        {
            if (!g_recorder)
                return;
            for (auto &[address, r] : g_recorder->functions)
                std::cout << "[fncheck] 0x" << std::hex << address << std::dec << " " << r->function->name << ": "
                          << r->kept << " calls recorded of " << r->calls << " (" << r->skipped
                          << " not kept: yielded; " << r->leftElsewhere << " left elsewhere, first at 0x" << std::hex
                          << r->firstElsewhere << std::dec
                          << (r->selfCallsCut ? " (" + std::to_string(r->selfCallsCut) + " of them in a call of itself past " +
                                                    std::to_string(kMaxResumes) + " returns)"
                                              : std::string())
                          << ", " << r->calleeUnwound
                          << " with a callee that did not return"
                          << (r->inEntryBlock ? ", " + std::to_string(r->inEntryBlock) + " parked where the game goes on in an entry block" : std::string())
                          << (r->interrupted ? ", " + std::to_string(r->interrupted) + " parked and interrupted (another thread or an interrupt ran meanwhile)" : std::string())
                          << (r->waited ? ", " + std::to_string(r->waited) + " that waited (unwound to the scheduler)" : std::string())
                          << (r->lostPages ? ", " + std::to_string(r->lostPages) + " with callee writes not kept" : std::string())
                          << (r->tooBig ? ", " + std::to_string(r->tooBig) + " given up past ZX_FNREC_MAX_MB" : std::string())
                          << (r->watched ? ", " + std::to_string(r->watched) + " more run for their caller's frame reads" : std::string())
                          << (r->stopped ? "; " + std::to_string(r->stopped) + " of the recorded kept to a stop" : std::string())
                          << (r->blocks && g_recorder->newBlocks
                                  ? "; " + std::to_string(r->dull) + " recorded calls dropped (no new block), blocks " +
                                        std::to_string(r->seeded) + " seeded + " + std::to_string(r->added) + " new of " +
                                        std::to_string(r->blocks->count)
                                  : std::string())
                          << (r->blocks && g_recorder->alsoNewBlocks
                                  ? "; past the sample " + std::to_string(r->probesKept) + " kept for a new block, " +
                                        std::to_string(r->dull) + " tried and dropped, blocks " + std::to_string(r->seeded) +
                                        " seeded + " + std::to_string(r->added) + " new past the sample of " +
                                        std::to_string(r->blocks->count)
                                  : std::string())
                          << ")" << std::endl;
            if (g_trackPages)
                std::cout << "[fncheck] callee writes by page protection: " << WriteTracker::instance().faults()
                          << " pages copied" << std::endl;
            if (g_parkedCount || g_notParked[0] || g_notParked[1] || g_notParked[2] || g_notParked[3])
            {
                std::cout << "[fncheck] calls parked at a wait: " << g_parkedCount << " (" << g_unwoundParked
                          << " unwound at a safe point, not a wait, " << g_betweenParked << " between calls), " << g_resumedCount
                          << " kept after it, " << g_parked.size() << " still waiting, " << g_staleCount
                          << " given up after " << kStaleVsyncs << " vsyncs; not parked:";
                for (int i = 0; i < 4; ++i)
                    std::cout << " " << g_notParked[i] << " " << kNotParked[i] << (i < 3 ? "," : "");
                std::cout << std::endl;
            }
        }

        uint32_t envNumber(const char *name, uint32_t fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : fallback;
        }
    }

    bool installRecorder(PS2Machine &runtime, game::Game game)
    {
        const char *spec = std::getenv("ZX_FNREC");
        if (!spec || !*spec)
            return true;
        // A recording is made with readable code off: on, readable functions
        // call each other directly, past the recorder (fncheck.h).
        {
            const char *readable = std::getenv("ZX_READABLE");
            const char *allow = std::getenv("ZX_FNREC_ALLOW_READABLE");
            if (!(readable && std::strcmp(readable, "off") == 0) && !(allow && std::strcmp(allow, "1") == 0))
            {
                std::cerr << "[fncheck] not recording: ZX_FNREC is set but ZX_READABLE is "
                          << (readable && *readable ? readable : "unset (the defaults: readable code on)")
                          << "; recordings are made with ZX_READABLE=off (readable functions that are on call each "
                             "other past the recorder), or ZX_FNREC_ALLOW_READABLE=1 for a deliberate test"
                          << std::endl;
                return false;
            }
        }
        const char *folder = std::getenv("ZX_FNREC_DIR");
        if (!folder || !*folder)
        {
            std::cerr << "[fncheck] ZX_FNREC needs ZX_FNREC_DIR" << std::endl;
            return true;
        }
        static RecorderState state;
        state.folder = folder;
        state.max = std::max(1u, envNumber("ZX_FNREC_MAX", 3000u));
        state.perVsync = std::max(1u, envNumber("ZX_FNREC_PER_VSYNC", 64u));
        state.fromVsync = envNumber("ZX_FNREC_FROM", 0u);
        state.untilSet = false;
        if (const char *until = std::getenv("ZX_FNREC_UNTIL_SCRIPT"); until && *until)
        {
            state.untilSet = true;
            state.untilScript = std::strtoll(until, nullptr, 0);
        }
        // ZX_FNREC_ATOMIC=1 ran each recorded call with the EE's safe points held
        // off: an interrupt, a vsync or a thread switch due in it came after it,
        // so a recorded run played differently from the unrecorded one
        // (tracedrift1: BMX XXX's route vr-dam-save, every function recorded,
        // apart from vsync 5 and never at GAME OVER). Gone: calls run as in the
        // game, and the ones a checkpoint interrupts are parked or not kept.
        if (envNumber("ZX_FNREC_ATOMIC", 0u) != 0u)
            std::cerr << "[fncheck] ZX_FNREC_ATOMIC is gone (it changed how the game ran): recorded calls run as in "
                         "the game"
                      << std::endl;
        state.minCalls = envNumber("ZX_FNREC_MIN_CALLS", 0u);
        const uint32_t newBlocksMode = envNumber("ZX_FNREC_NEW_BLOCKS", 0u);
        state.newBlocks = newBlocksMode == 1u || newBlocksMode == 2u;
        state.everyCall = newBlocksMode == 2u;
        state.alsoNewBlocks = newBlocksMode == 3u;
        // ZX_FNREC_COVERED=<file>: the blocks the corpus already runs, a
        // line a function: `ADDRESS BLOCK,BLOCK,...` (block start
        // addresses; fncheck_corpus.py coverage --covered-out writes it).
        std::map<uint32_t, std::vector<uint32_t>> covered;
        if (const char *seed = std::getenv("ZX_FNREC_COVERED"); (state.newBlocks || state.alsoNewBlocks) && seed && *seed)
        {
            std::ifstream in(seed);
            std::string line;
            while (std::getline(in, line))
            {
                std::stringstream words(line.substr(0, line.find('#')));
                std::string fn, list;
                if (!(words >> fn))
                    continue;
                words >> list;
                std::vector<uint32_t> &out = covered[static_cast<uint32_t>(std::strtoul(fn.c_str(), nullptr, 0))];
                std::stringstream items(list);
                std::string item;
                while (std::getline(items, item, ','))
                    if (!item.empty())
                        out.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0)));
            }
            if (!in.eof() && covered.empty())
                std::cerr << "[fncheck] ZX_FNREC_COVERED: cannot read " << seed << std::endl;
        }
        if (const char *effects = std::getenv("ZX_FNREC_EFFECTS"); effects && *effects)
            g_trackPages = std::string(effects) != "diff";
        // ZX_FNREC_WAITS=0: a call that waits is dropped (before drafter23).
        g_acrossWaits = envNumber("ZX_FNREC_WAITS", 1u) != 0u;
        g_unwindParking = envNumber("ZX_FNREC_UNWOUND", 0u) != 0u;
        g_maxRecordBytes = static_cast<uint64_t>(envNumber("ZX_FNREC_MAX_MB", 512u)) << 20;

        // ZX_FNREC=<items> or @<file>: items are names, addresses or `all`,
        // comma separated; a file has one per line (or commas), `#` starts a
        // comment, and a line may give the function its own caps:
        // `ADDRESS 200 8` (calls kept, at most per vsync).
        struct Chosen
        {
            game::readable::Function *function;
            uint32_t max, perVsync;
        };
        std::vector<Chosen> chosen;
        auto choose = [&](std::string item, uint32_t max, uint32_t perVsync) {
            item.erase(0, item.find_first_not_of(" \t\r"));
            item.erase(item.find_last_not_of(" \t\r") + 1u);
            if (item.empty())
                return;
            if (item == "all")
            {
                for (game::readable::Function *f : game::readable::functions())
                    if (f->game == game)
                        chosen.push_back({f, max, perVsync});
                return;
            }
            game::readable::Function *f = game::readable::find(item, game);
            if (f && f->game != game)
                std::cerr << "[fncheck] ZX_FNREC: " << item << " is " << game::gameName(f->game)
                          << "'s, the game running is " << game::gameName(game) << std::endl;
            else if (f)
                chosen.push_back({f, max, perVsync});
            else
                std::cerr << "[fncheck] ZX_FNREC: not a registered function"
                          << (game == game::Game::Ai ? "" : std::string(" of ") + game::gameName(game)) << ": " << item
                          << std::endl;
        };
        if (spec[0] == '@')
        {
            std::ifstream list(spec + 1);
            if (!list)
            {
                std::cerr << "[fncheck] ZX_FNREC: cannot read " << (spec + 1) << std::endl;
                return true;
            }
            std::string line;
            while (std::getline(list, line))
            {
                line = line.substr(0, line.find('#'));
                std::stringstream words(line);
                std::string first;
                std::vector<uint32_t> numbers;
                if (!(words >> first))
                    continue;
                std::string word;
                while (words >> word)
                    numbers.push_back(static_cast<uint32_t>(std::strtoul(word.c_str(), nullptr, 0)));
                const uint32_t max = numbers.size() > 0 && numbers[0] ? numbers[0] : state.max;
                const uint32_t perVsync = numbers.size() > 1 && numbers[1] ? numbers[1] : state.perVsync;
                std::stringstream items(first);
                std::string item;
                while (std::getline(items, item, ','))
                    choose(item, max, perVsync);
            }
        }
        else
        {
            std::stringstream items(spec);
            std::string item;
            while (std::getline(items, item, ','))
                choose(item, state.max, state.perVsync);
        }
        for (const Chosen &c : chosen)
        {
            game::readable::Function *f = c.function;
            if (state.functions.count(f->address))
                continue;
            if (!f->traced)
            {
                std::cerr << "[fncheck] " << f->name << ": this build has no traced original (configure with "
                          << "ZX_RUNNER_GENERATED_DIR)" << std::endl;
                continue;
            }
            auto r = std::make_unique<Recorded>();
            r->function = f;
            r->max = c.max;
            r->perVsync = c.perVsync;
            r->strideBlock = strideBlockFor(c.max);
            if (state.newBlocks || state.alsoNewBlocks)
            {
                r->blocks = fncheck::findBlocks(f->address, f->game);
                if (r->blocks)
                {
                    r->seen.assign(r->blocks->count, 0u);
                    const auto seed = covered.find(f->address);
                    if (seed != covered.end())
                        for (uint32_t a : seed->second)
                        {
                            const uint32_t *end = r->blocks->starts + r->blocks->count;
                            const uint32_t *at = std::lower_bound(r->blocks->starts, end, a);
                            if (at != end && *at == a && !r->seen[at - r->blocks->starts])
                            {
                                r->seen[at - r->blocks->starts] = 1;
                                ++r->seeded;
                            }
                        }
                }
            }
            r->previous = runtime.lookupFunction(f->address);
            if (!runtime.replaceFunction(f->address, &recorderEntry))
                continue;
            state.functions[f->address] = std::move(r);
        }
        // ZX_FNREC_STOPS=1: the stop table's functions (fncheck_stops.h)
        // recorded to a stop; ZX_FNREC_STOP=<names or addresses>: more, by
        // the frame rule (experiments).
        state.stopFrames = std::max(1u, envNumber("ZX_FNREC_STOP_FRAMES", 1u));
        {
            const bool table = envNumber("ZX_FNREC_STOPS", 0u) != 0u;
            std::set<std::string> extra;
            if (const char *more = std::getenv("ZX_FNREC_STOP"); more && *more)
            {
                std::stringstream items(more);
                std::string item;
                while (std::getline(items, item, ','))
                    if (!item.empty())
                        extra.insert(item);
            }
            std::string named;
            for (auto &[address, r] : state.functions)
            {
                char at[16];
                std::snprintf(at, sizeof(at), "0x%x", address);
                if (table)
                    for (const StopEntry &e : kStops)
                        if (e.game == r->function->game && std::strcmp(e.name, r->function->name) == 0)
                            r->stop = e.rule;
                if (r->stop == StopRule::None && (extra.count(r->function->name) || extra.count(at)))
                    r->stop = StopRule::Frames;
                if (r->stop != StopRule::None)
                    named += std::string(named.empty() ? "" : ", ") + r->function->name +
                             (r->stop == StopRule::Frames ? " (thread end, or a wait after " + std::to_string(state.stopFrames) + " vsyncs)"
                                                          : " (thread end)");
            }
            if (!named.empty())
                std::cout << "[fncheck] recorded to a stop: " << named << std::endl;
        }
        g_recorder = &state;
        setTracedGame(game);
        ps2_game_overrides::addShutdownHook(&reportRecorder);
        std::cout << "[fncheck] recording " << state.functions.size()
                  << (game == game::Game::Ai ? "" : std::string(" ") + game::gameName(game)) << " functions into " << state.folder.string()
                  << " (" << state.max << " calls each unless listed with their own; callee writes by "
                  << (g_trackPages ? "page protection" : "a full RAM diff")
                  << (state.newBlocks ? "; only calls that run a new block of the original kept" : "")
                  << (state.alsoNewBlocks ? "; the capped sample, and past it the calls that run a new block of the original" : "")
                  << ")" << std::endl;
        return true;
    }

    void Vu0Memory::upload(PS2Memory &memory, const uint8_t *stream, uint32_t bytes)
    {
        memory.initializeVu0Memory();
        memory.processVIF0Data(stream, bytes);
        m_code.assign(memory.getVU0Code(), memory.getVU0Code() + PS2_VU0_CODE_SIZE);
        m_data.assign(memory.getVU0Data(), memory.getVU0Data() + PS2_VU0_DATA_SIZE);
    }

    void Vu0Memory::restore(PS2Memory &memory) const
    {
        memory.setVu0Memory(m_code.data(), m_data.data());
    }
}
