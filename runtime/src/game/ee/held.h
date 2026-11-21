#pragma once

// The callee-saved registers the original holds at each of its calls.
//
// The original keeps values in s0-s7, fp and f20-f31 across its calls (a
// pointer it walks, a count, a float it loaded), and every callee's prologue
// saves the ones it uses into its own frame. A readable function keeps those
// values in C++ locals, so without more its callees would save its entry
// values instead: other words in the dead stack, which the game later copies
// into records (DM2's editor records, verify3; BMX XXX's front end, verify2).
//
// tools/readable_held_gen.py writes a table at build time from the original's
// code: per function and call site, the value each of those registers holds at
// the jal, as an expression over what the runtime has at the call (Node, below),
// one per call made before it (Case: the original's call site of the previous
// call, or the entry). The runtime follows the readable function's calls through
// the original's sites (the readable makes the original's calls in its order:
// zx-fncheck requires it), works the values out at each call, and gives them to
// the callee as the original's caller would (Frame::next, as ee::calleeSaves
// publishes them: abi.cpp), with the jal's return address. A value the table
// cannot give (it depends on how often a loop with no call in it ran, or on an
// operation not modelled: readable_held_unknown.tsv) is the function's entry
// value, as without the table. Game code has no part in it.

#include <cstdint>

namespace ee
{
    struct Frame;
    struct FrameRow;
    struct CallRegs;
    struct ThreadState;
}

namespace ee::held
{
    struct State;
    namespace program
    {
        struct Context;
        struct Worked;
        // A program generated as code (held_program.h): false when a part of it is not known.
        using Code = bool (*)(const Context &, uint64_t &);
        // A case's values as code: each one worked out (Worked), a kChecked one only when `confirmed`.
        using CaseValuesCode = void (*)(const Context &, bool confirmed, Worked &);
        // A case's checks as code: false when the call does not fit them; `confirmed` when one was worked out and
        // held; a kind 18 check only when `several`.
        using CaseChecksCode = bool (*)(const Context &, bool several, bool &confirmed);
    }

    // ---- the table (readable_held_gen.py writes it) ------------------------

    enum Op : uint8_t
    {
        kConst = 1,  // imm
        kEntry,      // the function's entry value of register `sub` (GPR, or 32 + FPR)
        kPrevious,   // what the previous call got in saved slot `sub` (ee::savedSlot)
        kResult,     // the previous call's v0 (sub 0), v1 (1) or f0's bits (2)
        kArgument,   // this call's integer argument `sub` (a0-a3, t0-t3)
        kFloatArgument, // its float argument `sub` (f12-f19), as bits
        kTarget,     // the address it calls (a jalr's register)
        kLoad,       // memory at the address popped + imm, as LoadKind `sub` loads it
        kAlu,        // the original's operation `sub` (AluOp) on the values popped
        kEntryLoad,  // the function's entry load `sub` (Function::firstEntryLoad): memory as the
                     // original read it before any store or call, read when the function is entered
        kReturnLoad, // return load `sub` of the site of the last call (Site::firstReturnLoad): memory as
                     // the original read it after that call, before any store, read as the call returns
        kSkipIf,     // a select the branches make, worked out lazily: pops its condition; the taken arm (the
                     // next imm - 1 nodes, then a kSkip) when it is not 0 (sub 0) or is 0 (sub 1), else the
                     // other arm after it, so a load the original made on the other path only is not made
        kSkip,       // the end of a select's taken arm: the next imm nodes (the other arm) skipped
        // A loop with no call in it (a list walked to the node that passes a test, a count stepped to a bound):
        // the header's values are carried, `sub` of them, popped as their initial values; then the program (the
        // next imm nodes) is run each iteration: its leaves in order, each kLeaf(kind, condition length)
        // [condition] kLeafEnd(0, values length) [values]; the first leaf whose condition is not 0 is taken: kind
        // 1 (the call's path) pushes its one value as the loop's result, kind 0 (a back edge) makes its values the
        // next carried ones. kCarried pushes carried value `sub`. No leaf taken, or kLoopIterations passed: not
        // known. readable_held_gen.py loop_program.
        kLoop,
        kCarried,
        kLeaf,
        kLeafEnd,
    };
    constexpr int kCarriedValues = 16;
    constexpr uint32_t kLoopIterations = 1u << 16;
    enum LoadKind : uint8_t { kLb, kLbu, kLh, kLhu, kLw, kLwu, kLd, kLwc1, kLq };
    // (readable_held_gen.py ALU_LIST, in its order; operands popped: one for
    // the forms with an immediate (imm) and the one-operand FPU forms, two,
    // three for movn and movz: source, condition, the old value)
    enum AluOp : uint8_t
    {
        kAddu, kDaddu, kSubu, kDsubu, kAnd, kOr, kXor, kNor, kSlt, kSltu, kSllv, kSrlv, kSrav, kDsllv, kDsrlv,
        kDsrav, kMovn, kMovz, kAddiu, kDaddiu, kAndi, kOri, kXori, kSlti, kSltiu, kSll, kSrl, kSra, kDsll, kDsrl,
        kDsra, kDsll32, kDsrl32, kDsra32, kAddS, kSubS, kMulS, kDivS, kMaxS, kMinS, kNegS, kAbsS, kSqrtS, kCvtSW,
        kCvtWS, kMtc1, kMfc1,
        // The multiply/divide unit's LO and HI of mult, multu, div, divu (and pipeline 1's mult1 .. divu1,
        // the same numbers) on two operands, as the recompiler's code works them out (a divide by 0 too).
        kMultLo, kMultHi, kMultuLo, kMultuHi, kDivLo, kDivHi, kDivuLo, kDivuHi,
        // The FPU's condition of c.eq.s, c.lt.s, c.le.s on two floats' bits: 1 or 0 (what bc1t/bc1f branch on).
        kCEqS, kCLtS, kCLeS, kAluCount,
    };

    // (imm: 32 bits, sign-extended: the original's constants are lui/ori
    // words, an FPR's bits are their low word.)
    struct Node
    {
        uint8_t op;
        uint8_t sub;
        int32_t imm;
    };
    // A value: a saved slot (or, as a check, what it tests: 0-7 the integer
    // argument, 8-15 the float one, 16 the target, 17 the case's path condition, which must hold, 18 a
    // condition the path from the previous call met, which must hold and proves nothing) and its expression, nodes
    // [first, first + count) in postfix order; count 0: not known. A slot with
    // kChecked is a value the site alone does not prove (the cases of the site
    // give it differently, so it holds only if the call came after the case's
    // previous call): given only when one of the case's checks was worked out
    // and held, else the entry value.
    // program: the same expression generated as code (Table::programs, readable_held_code.py: the same value,
    // known and not known where the nodes are), 0 for none (a leaf, or one the nodes alone give).
    constexpr uint8_t kChecked = 0x80u;
    struct Value
    {
        uint8_t slot;
        uint16_t count;
        uint32_t first;
        uint32_t program = 0;
    };
    // The values at a site after one previous call: prev, its site (the jal's
    // address), 0 for the entry, kLost after a call the runtime could not place
    // (then only what every case agrees on). The slots it does not list hold
    // what the previous call got (kLost: not known). Checks: the call's
    // arguments, where another site could be taken for this one, or where a
    // kChecked value needs them (then a single case is not taken when they fail).
    constexpr uint32_t kFromEntry = 0u;
    constexpr uint32_t kLost = 0xFFFFFFFFu;
    // valuesCode, checksCode: its values and its checks as code (Table::caseValues, caseChecks: what the table's
    // values and checks give, readable_held_code.py), 0 for none (the table's rows are worked through).
    struct Case
    {
        uint32_t prev;
        uint32_t firstValue, firstCheck;
        uint16_t valueCount, checkCount;
        uint32_t valuesCode = 0, checksCode = 0;
    };
    // A call site of the original: the jal's (or jalr's) address and its
    // target, 0 for a jalr. A function's sites are sorted by target (jalrs
    // first), then address.
    // Its return loads (Table::returnLoads [first, first + count)): what the original
    // loads after this call returns and before it stores outside its frame, at an
    // address worked out from what the runtime has then (BMX XXX's saveObjectState
    // keeps *a1 in s0, then stores through it). The runtime reads them when the
    // call returns, so the readable function's later stores do not make them
    // stale. At most kReturnLoads; in a replay none is read, as entry loads.
    constexpr int kReturnLoads = 8;
    struct Site
    {
        uint32_t site, target;
        uint32_t firstCase, caseCount;
        uint32_t firstReturnLoad = 0, returnLoadCount = 0;
    };
    enum FunctionFlags : uint32_t
    {
        kReadsEntryArguments = 1u, // some value needs the entry's argument registers
        kReadsResults = 2u,        // some value needs a call's results
        kReadsReturnLoads = 4u,    // some site has return loads
    };
    // Entry loads: memory the original reads before its first store outside its
    // frame and its first call, at an address worked out from the entry (a byte it
    // keeps in a register across a loop that stores elsewhere: DM2's showGameScreen).
    // The runtime reads them as the function is entered, so no later store of the
    // readable function's makes them stale. At most kEntryLoads per function. In
    // a replay none is read (a recording does not say which bytes it holds): not
    // known, so the entry value. ZX_READABLE_HELD_ENTRY_LOADS=0: none read.
    constexpr int kEntryLoads = 8;
    struct Function
    {
        uint32_t game, address; // ee/game_id.h number, the function's start
        uint32_t firstSite, siteCount;
        uint32_t flags;
        uint32_t firstEntryLoad = 0, entryLoadCount = 0; // Table::entryLoads (Value: slot 0, the load's nodes)
    };
    struct Table
    {
        const Function *functions = nullptr;
        uint32_t count = 0;
        const Site *sites = nullptr;
        const Case *cases = nullptr;
        const Value *values = nullptr;
        const Value *checks = nullptr;
        const Node *nodes = nullptr;
        const Value *entryLoads = nullptr;
        const Value *returnLoads = nullptr;
        const program::Code *programs = nullptr; // (Value::program; [0] unused)
        const program::CaseValuesCode *caseValues = nullptr; // (Case::valuesCode; [0] unused)
        const program::CaseChecksCode *caseChecks = nullptr; // (Case::checksCode; [0] unused)
    };
    // The generated table registers itself at static initialisation (a build
    // without it, the unit tests, has none: every call gives entry values).
    struct Registration
    {
        explicit Registration(const Table &table);
    };
    // The function at `address` of game `game` in the table, or null.
    const Function *find(uint32_t game, uint32_t address);

    // ---- the running function's place in the original ---------------------

    constexpr int kSlots = 23; // ee::kSavedSlots
    constexpr int kLocalRanges = 8;
    // The sites the last call can have been (several when the call's arguments
    // do not tell two apart: the values then are what they all agree on).
    constexpr int kPlaces = 4;
    struct State
    {
        const Function *function = nullptr; // null: no table row (nothing to do)
        uint32_t flags = 0;                 // the function's (Function::flags), when it has one
        uint32_t prev[kPlaces];             // the original's sites of the last call made, or kFromEntry
        uint8_t places = 0;                 // how many; 0: lost (a call not placed)
        uint32_t known = 0;                 // slots of `value` known
        uint32_t entry = 0;                 // of those, the ones still at the entry value (the entry model's)
        uint64_t value[kSlots];             // what the last call got (an FPR's bits)
        uint16_t arguments = 0;             // bits 0-7: a0-t3 at entry known, 8-15: f12-f19
        uint8_t results = 0;                // bits 0-2: v0, v1, f0 of the last call known
        uint64_t argument[8];
        uint32_t floatArgument[8];
        uint64_t v0, v1;
        uint32_t f0;
        uint8_t entryLoads = 0;             // bits: entryLoad[i] known (read at the entry)
        uint64_t entryLoad[kEntryLoads];
        const Site *lastSite = nullptr;     // the last call's site, when it is one (its return loads)
        uint32_t after = 0;                 // the calls that can come next, from one place or none (held.cpp After)
        // The function's ee::Local objects made so far, as frame offsets [from, to) (ee::frameLocal): a load from
        // its own frame reads what the original holds there only inside one of them (the readable keeps the
        // original's spills in C++; a Local sits at the original's address and the body fills it as the
        // original does). At most kLocalRanges; a load in the frame outside them is not known.
        uint8_t localRanges = 0;
        uint16_t localFrom[kLocalRanges], localTo[kLocalRanges];
        uint8_t returnLoads = 0;            // bits: returnLoad[i] known (read as that call returned)
        uint64_t returnLoad[kReturnLoads];
        State() {} // (not zeroed: the masks say what holds)
    };

    // The table's row of a frame-table function (null: none, or the table off),
    // through the thread's cache of the last ones asked for.
    const Function *rowOf(ThreadState &thread, const FrameRow *layout);
    // Entering a readable function with a row in `frame` (ee::enterFrame) on
    // `thread`: its state, from the entry, and its entry loads read.
    // `arguments`: the call's (a readable callee called directly), else the
    // context's argument registers (a call through the table).
    void enter(Frame &frame, const Function *row, const CallRegs *arguments, const ThreadState &thread);
    // The arguments of the readable callee about to be entered directly
    // (game::call, game::tailCall), for its enter(), are ee::ThreadState's
    // directArguments (ee/abi.h); null after.
    // A self-call: the inner call's arguments are not known (nor its entry loads).
    void forgetArguments(Frame &frame);
    // An ee::Local of the running function made at frame offset `offset`, `bytes` long (ee::frameLocal).
    void noteLocal(Frame &frame, uint32_t offset, uint32_t bytes);

    // Before a call (not a tail call) of the running function: the values the
    // original holds there into frame.next (what ee::calleeSaves published is
    // kept), with the jal's return address.
    void beforeCall(Frame &frame, const CallRegs &regs);
    // After it: its results (`known`: bits 0-2, v0, v1, f0).
    void afterCall(Frame &frame, uint64_t v0, uint64_t v1, uint32_t f0, uint8_t known);
    // Whether afterCall has anything to do in `state` (a function that reads a call's results or return loads).
    inline bool readsAfterCall(const State &state)
    {
        return state.function && (state.flags & (kReadsResults | kReadsReturnLoads));
    }
    // The same from a caller that has this thread's state (ee::t_ee) already.
    void beforeCall(Frame &frame, const CallRegs &regs, ThreadState &thread);
    void afterCall(Frame &frame, uint64_t v0, uint64_t v1, uint32_t f0, uint8_t known, const ThreadState &thread);

    // Scaffolding (held-place-as): docs/SCAFFOLDING.md. A call a host form makes in place of one of the original's:
    // a call of `target` in a function of game `game`
    // whose original has no site of it is placed at a site of `standsFor` (its case, its values), so the calls after
    // it get what the original holds there. render_path = records: the draw's DMA tag begun where the part's packet
    // was called (zge/host/class_draws.cpp); without it the place is lost there and the class renderer's next callee
    // (requestTextures) saves entry values into its frame, stack the game copies later (BMX XXX's health bar shards'
    // texture coordinates). Set before the game runs; a pair set twice is kept once.
    void placeAs(uint32_t game, uint32_t target, uint32_t standsFor);

    // ZX_READABLE_HELD (default 1): 0 leaves the table out (every call gives
    // the entry values, the model before tools18). on(): the table on in this
    // boot (wanted(), less lean's held part: ee::bootSwitches, read once per
    // boot); wanted(): the environment's and the frame table's part alone.
    bool on();
    bool wanted();

    // Evaluates one expression (the unit tests' view): false if a part of it
    // is not known. loads: memory is read only for the case the call is known
    // to be (the original made those loads on every path there; another case's
    // may be anywhere, and zx-fncheck's RAM holds only what the recording read),
    // and for a case's checks, which only choose among the cases: read through
    // the bounds check, never recorded as an access. unreadable (if given): set
    // when a load could not be read (an unmapped address): that case does not
    // match (ee/held.cpp matches). In a replay no check load is read (zx-fncheck
    // does not say what its recording holds) and such a check is not known, as
    // before: ruling those cases out left another site's case alone, whose value
    // loads then read where that site's base points (AI's drawScene, 0x178).
    // `replaying`: under zx-fncheck's call handler (its check loads are not read).
    bool evaluate(const Frame &frame, const State &state, const CallRegs &regs, const Node *nodes, uint32_t count,
                  uint64_t &out, bool loads = true, bool *unreadable = nullptr, bool replaying = false);
}
