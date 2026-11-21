#pragma once

// fncheck: proving a readable function equal to the recompiled one.
//
// 1. Recording (in the game). ZX_FNREC=<functions> makes the runner
//    record calls of those functions into ZX_FNREC_DIR (one
//    <address>.fnrec file each; game data, so keep them in a scratch
//    folder). A recorded call runs the recompiled function built a second
//    time with every memory access traced (ps2/ee/fncheck_traced.h), and
//    keeps:
//      - the registers at entry and at return;
//      - its input: every byte it read, as it was at entry (16-byte lines
//        with a byte mask);
//      - its output: every byte it or its callees wrote, as it is at the
//        return (the stack below sp, dead after the return, left out);
//      - the calls it made: target, argument registers, the results (v0,
//        v1, f0), what the callee changed in RAM, and the registers it
//        returned with, and the bytes of the function's own frame the
//        callee read (Call::reads, below). What a callee changed is learnt by page protection
//        (round 14, fncheck.cpp WriteTracker: RAM read-only around the call,
//        each page copied at its first write, compared at the return; about
//        0.1 ms a call against the full RAM diff's 6 to 8 ms, which
//        ZX_FNREC_EFFECTS=diff still does).
//    A recorded run plays exactly as the unrecorded one (tracedrift1): the
//    traced code charges and checks what the plain code does, and wherever
//    the plain code leaves for the scheduler (a checkpoint due, a callee or
//    a jump that does not come back, an inner self-call's return), the
//    traced code does too. The call is then parked to go on when the
//    scheduler comes back there: at its open call's return point when the
//    callee waits (what changed meanwhile, by anyone, is that call's
//    effects; a callee that goes through the scheduler with no wait keeps
//    the call only with ZX_FNREC_UNWOUND=1, fncheck.cpp), or, with no call open, only
//    if nothing but that dispatch ran meanwhile and the table has the
//    function itself there (not one of the recompiler's entry blocks, which
//    run the rest slice by slice); else it is not kept. Calls that yield to
//    the scheduler halfway in their own code (an interrupt, a thread
//    switch) are not kept. Accesses to I/O registers (and the other
//    addresses the runtime handles, PS2Machine::isSpecialAddress) are kept
//    in order with their values (Record::io, round 12): the checker answers
//    the loads from them and compares the stores. A jump out of the
//    function (a tail call) is kept as its last call, run to the return by
//    the recorder; a call of the function itself, which generated code runs
//    inline, is followed through its return (kMaxSelfCallResumes below).
//
// 2. Checking (zx-fncheck, ps2/ee/fncheck_main.cpp). Every recorded call
//    is replayed from its entry state in an otherwise empty RAM, against
//    the traced original (which must give the recording back: a check of
//    the recording itself) and against the readable version, twice, with
//    different fill patterns in the memory the recording does not cover.
//    The readable version passes a call if, both times:
//      - its result register (by its C++ return type) is the recorded one;
//      - RAM afterwards is the recorded output, byte for byte, and nothing
//        else changed (memory outside the recording's pages is
//        inaccessible during the call);
//      - it made the same calls, in order, with the same arguments; they
//        are answered from the recording (results and RAM changes), so a
//        function is checked on its own;
//      - at each call, the memory the callee reads holds what the
//        original's did there, as far as a probe of the callee in the
//        original's replay reaches (fncheck_main.cpp probeCallee, the
//        result line's callee_read);
//      - with the recording folder's index of leftover reads
//        (--frame-reads-in), what it leaves in its own frame at the return is
//        the original's wherever a later recorded call reads it before
//        writing it (fncheck_main.cpp frameResidue, frame_residue_read);
//        elsewhere a difference there is a note (frame_residue_differs).
//    The two fill patterns catch reads of memory the original never read.
//    The EE cycles each version charges are compared too (reported, and a
//    failure with --cycles): emulated time decides when interrupts and
//    vsyncs fall, so readable code charges what the original's loops and
//    calls do (ee::chargeCycles).
//
// Environment (recording):
//   ZX_FNREC=all | <name or 0xaddress>,...   functions to record
//   ZX_FNREC=@<file>                         ... listed in a file, one a line (or commas; `#` a
//                                              comment), each line optionally with its own caps:
//                                              `ADDRESS 200 8` (calls kept, at most per vsync)
//   ZX_FNREC_EFFECTS=pages | diff            how callee writes are found (default pages)
//   ZX_FNREC_DIR=<folder>                    where (required)
//   ZX_FNREC_MAX=<n>                         calls per function (default 3000)
//   ZX_FNREC_PER_VSYNC=<n>                   at most n per function per vsync (default 64)
//   ZX_FNREC_FROM=<vsync>                    none kept before this vsync (default 0)
//   ZX_FNREC_UNWOUND=1                       keep a call whose callee goes through the scheduler with no
//                                              wait (parked at its return point while only its thread runs)
//   ZX_FNREC_UNTIL_SCRIPT=<n>                none begun once the pad script's clock is at n (vsyncs): a
//                                              route's recording ends at its last input on every host
//   (ZX_FNREC_ATOMIC, which ran recorded calls with EE safe points held off, is gone: a recorded
//    run plays as the unrecorded one, below)
//   ZX_FNREC_MIN_CALLS=<n>                   keep only calls that made at least n calls (rare paths)
//   ZX_FNREC_STOPS=1                         record the functions of the stop table (fncheck_stops.h)
//                                              to a stop: a call of one that does not return is kept up
//                                              to the callee that never comes back (kStopsAtCall)
//   ZX_FNREC_STOP=<name>,...                 more functions recorded to a stop, by the frame rule
//                                              (experiments only: the table is what routes use)
//   ZX_FNREC_STOP_FRAMES=<n>                 the frame rule's vsyncs (default 1)
//   ZX_FNREC_NEW_BLOCKS=1 | 2 | 3            1: only calls that run a block of the original no kept call
//                                              ran (nor ZX_FNREC_COVERED=<file>'s), a few a vsync; 2: every
//                                              call tried; 3: the capped sample as unset, and past it the calls
//                                              that run a new block
//   ZX_FNREC_ALLOW_READABLE=1                record with readable code on (a deliberate test only): without
//                                              it the recorder refuses unless ZX_READABLE=off
//   ZX_FNREC_PLAIN=1                         plain files (by default compressed with zstd, a frame a
//                                              record: fnrec_file.h, which every reader goes through)
// Games (ee/game_id.h): the recorder records the running game's registered
// functions (readable::install(runtime, game) installs it); an item is a
// name or address in that game (`dm2:ADDRESS` names a game explicitly,
// and must be the running one). A recording's header names its game
// (version 6 on; older ones are AI's), its file is <address>.fnrec for AI
// and <game>-<address>.fnrec for another (dm2-001a33f0.fnrec), and
// zx-fncheck replays it against that game's traced original.
//
// Spread: every call until 250 are kept (an eighth of a smaller cap, at
// least 8), then every 2nd, 4th ... 64th, so a long run is sampled
// throughout. Files are opened as calls are kept, at most 64 at once. Recordings are made with
// ZX_READABLE=off: readable functions that are on call each other directly, past the recorder, so a run with
// them on misses calls and records states the original does not make. installRecorder refuses (a message, no
// recorder) unless ZX_READABLE=off or ZX_FNREC_ALLOW_READABLE=1; the game's own install then ends the run.

#include "abi.h"
#include "fncheck_trace.h"
#include "game_id.h"

#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

class PS2Memory;
class PS2Machine;

namespace fncheck
{
    // Stack below the lowest sp seen (and below a callee's sp) that counts
    // as dead scratch: not output, not a callee effect.
    constexpr uint32_t kStackWindow = 0x10000u;

    // 16 bytes of EE memory (a key above); mask bit i covers bytes[i].
    struct Line
    {
        uint32_t address = 0;
        uint16_t mask = 0;
        uint16_t reserved = 0;
        uint8_t bytes[16] = {};
    };
    static_assert(sizeof(Line) == 24, "record layout");

    // A recorded function that calls itself: generated code makes the
    // self-call a jump to the function's start with ra at its return point,
    // so the inner call's return (jr ra, or the jump of its tail call) leaves
    // the C++ function there. The recorder and the checker go on with the
    // outer call at that point, traced, as the dispatcher goes on in the
    // game: the recording holds both calls' work and every call each makes
    // (setSkaterScore's rollover: two calls to the score counter). Bounds:
    // return points within kMaxFunctionSpan of the function's start, at
    // most kMaxSelfCallResumes per recorded call. Every inner call of a
    // recursion comes back through here once, however deep it went (BMX
    // XXX's splitColourBox: a call per colour box, up to 0x800 boxes), so
    // the bound only stops a runaway, as the recorder's tail-call hops'
    // does; 64 dropped every call of a recursion with more inner calls
    // (tools21).
    constexpr uint32_t kMaxFunctionSpan = 0x10000u;
    constexpr uint32_t kMaxSelfCallResumes = 1u << 20;

    enum CallFlags : uint32_t
    {
        kTailCall = 1u, // the function left by a jump (j / jr): the callee returns to its caller
        // ... a jump generated code makes as a direct C++ call (j to a known
        // function's start): no dispatch, so no EE cycles charged for it
        // (recordings of version 4 on; older ones charged it 8, and the
        // reader takes those off, as if every tail call were direct).
        kDirectTailCall = 2u,
        // The call the record stops at (a record with kStopsAtCall): the
        // callee never came back to the function (its thread ended in it, or
        // the recording stopped at this wait, ZX_FNREC_STOPS). Its target,
        // arguments and sp are kept, no results and no effects.
        kStopCall = 4u,
    };

    struct Call
    {
        uint32_t target = 0;
        uint32_t sp = 0;
        uint32_t flags = 0; // CallFlags (recordings of version 2 on)
        uint64_t a[ee::kIntArgRegs] = {};
        // The argument registers' upper 64 bits (recordings of version 5 on;
        // highKnown set), for calls that pass a quadword (ee::Qword).
        uint64_t aHigh[ee::kIntArgRegs] = {};
        bool highKnown = false;
        float f[ee::kFloatArgRegs] = {};
        // Registers when the callee returned (results in v0, v1, f0; the
        // rest is what the callee left in the caller-saved registers).
        R5900Context after{};
        std::vector<Line> effects;
        // The bytes of the calling function's own frame, [sp at the call, sp
        // at its entry), that the callee read before anything wrote them, as
        // they were at the call (recordings of version 7 on). A callee is
        // the caller's only reader of a record the caller builds on its stack
        // and hands it (BMX XXX's beginObjectiveBlock and registerBlockClass,
        // tools44): the call is answered from the recording, so these bytes
        // are the one sign of a wrong word there. Known for a callee the
        // recorder records (one it chose, ZX_FNREC): its traced original
        // runs at every call that passes it a pointer into the caller's
        // frame, recorded or not (Recording::framePassed), and the callees it
        // calls the same way add theirs (their reads of this frame through
        // the pointer it handed on). A callee it does not record, a library
        // call, and a pointer the caller leaves in memory: none, so the set
        // is a lower bound. zx-fncheck compares them at the readable
        // version's call (callee_read).
        std::vector<Line> reads;
    };

    enum RecordFlags : uint32_t
    {
        // Touched I/O registers, values not kept (recordings made before
        // round 12): not replayable. The scratchpad is recorded as memory.
        kSpecialAccess = 1u,
        // The record ends with its I/O accesses (Record::io). Readers made
        // before round 12 stop at such a record (as at a damaged one).
        kIoLog = 2u,
        // The entry context's cop0_count is COP0 Count at the call (the
        // recompiled code otherwise keeps only the last value it read
        // there); the replays start Count at it, so a function that reads
        // Count (mfc0 Count: readCycleCounter's timer) replays as recorded.
        // Recordings made before (drafter25) start Count at 0.
        kEntryCount = 4u,
        // The call does not return: the record ends at its last call, marked
        // kStopCall (ZX_FNREC_STOPS: a function that never returns, such as
        // a job that ends its own thread or a thread's endless loop). Its
        // outputs are RAM as it was at that call, its exit the registers at
        // the call. The checker replays it to that call, checks the call as
        // any other and stops there: the calls made, RAM and (the original's)
        // registers are compared at the stop; nothing is returned.
        kStopsAtCall = 8u,
    };

    struct Record
    {
        uint32_t flags = 0;
        uint64_t sequence = 0; // the call's number among the function's calls
        uint64_t vsync = 0;
        uint32_t deadLow = 0;  // [deadLow, entry sp) is dead stack
        // EE cycles the function's own code charged (32 per backward branch,
        // 8 per call it dispatched; recordings of version 3 on).
        uint64_t cycles = 0;
        R5900Context entry{};
        R5900Context exit{};
        std::vector<Line> inputs;
        std::vector<Line> outputs;
        std::vector<Call> calls;
        std::vector<IoAccess> io; // kIoLog
    };

    // The recording corpus's version (tools/readable_corpus.py: its folder
    // v<N>): records are written the same from file version 5 on (6 only
    // names the game in the header's word that was reserved, 7 adds what a
    // call's callee read of the caller's frame, which an older recording
    // just lacks), so version 5 to 7 recordings are one corpus.
    constexpr uint32_t kCorpusVersion = 5;

    // File: header, then records. Contexts are stored as the host lays
    // R5900Context out, so recordings belong to the build kind (OS,
    // compiler) that made them.
    // The version writeRecord writes: 7 adds each call's frame reads
    // (Call::reads) after its effects.
    constexpr uint32_t kRecordVersion = 7;

    struct FileHeader
    {
        char magic[8] = {'F', 'N', 'R', 'E', 'C', '1', 0, 0};
        // 6: the game (the word reserved until 5, always 0 there: AI).
        uint32_t version = kRecordVersion;
        uint32_t address = 0;
        uint32_t contextSize = sizeof(R5900Context);
        uint32_t game = 0; // game::Game's number (ee/game_id.h)
    };

    void writeHeader(std::ostream &out, uint32_t address, game::Game game = game::Game::Ai);
    // The recording's file name in its folder: <address>.fnrec (AI),
    // <game>-<address>.fnrec (another game).
    std::string recordingName(uint32_t address, game::Game game);
    void writeRecord(std::ostream &out, const Record &record);
    bool readHeader(std::istream &in, FileHeader &header, std::string *error);
    // False at the end of the file or on a damaged record. `version`: the
    // file's (FileHeader::version; records are the same from 5 to 6, and 7
    // adds Call::reads; the default is writeRecord's). Lines at an EE address that is not a
    // memory key (recordings made before the keys) are folded onto their
    // key; readRecordAsWritten leaves them as the file has them.
    bool readRecord(std::istream &in, Record &record, uint32_t version = kRecordVersion);
    bool readRecordAsWritten(std::istream &in, Record &record, uint32_t version = kRecordVersion);
    // A record's head (its flags, sequence, vsync and the registers at entry
    // and at the return), the rest skipped: for indexing a file's calls
    // without reading their memory (zx-fncheck's callee recordings). False
    // as readRecord.
    struct RecordHead
    {
        uint32_t flags = 0;
        uint64_t sequence = 0, vsync = 0;
        R5900Context entry{}, exit{};
    };
    bool skimRecord(std::istream &in, RecordHead &head, uint32_t version = kRecordVersion);

    // zx-fncheck's callee probes: a call (jal, jalr) of traced code is run
    // by this instead of dispatched when it returns true (ctx then at the
    // call's return point). Null in the game.
    using CallRunner = bool (*)(PS2Machine *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t target, uint32_t returnPc);
    extern CallRunner g_callRunner;

    // ZX_FNREC: wraps the chosen functions of `game` (the running game)
    // with the recorder. False when it refused: ZX_FNREC set but readable code on (above).
    bool installRecorder(PS2Machine &runtime, game::Game game = game::Game::Ai);

    // VU0's code memory in a replay: what the game had uploaded when the call was recorded, as a VCALLMS the
    // replayed code makes runs what is there (PS2Machine::executeVU0Microprogram). A game's own upload is the
    // VIF0 stream its DMA chain makes (each tag's VIF codes, then its data, as the DMAC hands VIF0 them;
    // tools/setup/vu0_upload.py writes it from the game's ELF into its generated code's folder,
    // vu0_upload.vif), built into zx-fncheck (game.cmake) and sent through the runtime's VIF0
    // (PS2Memory::processVIF0Data), as the game's boot sent it. BMX XXX's is initPlatform's (loadVu0Microprogram:
    // the sine and cosine program, at VU0's address 0, that angleSin and angleCos start). AI and DM2 upload none
    // and start none: their only VCALLMSR is sceDevVu0Exec, which nothing calls.
    struct Vu0Upload
    {
        game::Game game;
        const uint8_t *stream;
        uint32_t bytes;
    };
    // The uploads zx-fncheck was built with, a game at most once (none: empty).
    std::span<const Vu0Upload> vu0Uploads();

    // A game's VU0 memory after its upload, put back at the start of each replay (a microprogram an earlier
    // replay started can have changed VU0's data memory).
    class Vu0Memory
    {
    public:
        // VU0's memory zeroed, then `stream` sent through VIF0, and what that left kept.
        void upload(PS2Memory &memory, const uint8_t *stream, uint32_t bytes);
        // VU0's code and data memory as the upload left them.
        void restore(PS2Memory &memory) const;
        bool uploaded() const { return !m_code.empty(); }

    private:
        std::vector<uint8_t> m_code, m_data;
    };
}
