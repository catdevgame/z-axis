#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Guest-state probes evaluated on the EE thread at every VBlank start, so a
// snapshot is one consistent guest moment (like a PCSX2 savestate, which is
// also taken at a vsync boundary).
//
// ZX_DUMP_WHEN="<label>:<cond>[&<cond>...],..." dumps EE RAM to
//   <ZX_DUMP_PREFIX or zx-dump>-<label>.ram when all conditions hold.
//   Entries fire once each and in order: an entry is only armed after the
//   previous one fired, so e.g. a level frame counter that also counted in
//   the front end can be used for in-level moments. A condition is
//   <addr><op><value> on the guest u32 at addr, op one of == != >= <= > <,
//   tick<op><value> on the runtime vsync tick, or script<op><value> on the
//   ZX_PAD_SCRIPT clock in vsyncs (runtime/ps2_pad_script.h; -1 before
//   the first pad read), which follows the recorded run once SYNC anchors
//   line it up.
// ZX_STATE_LOG="<addr>[,<addr>...]" prints one "[state]" line with the
//   vsync tick and every listed u32 whenever any of them changed. An entry
//   <addr>:<n> lists n consecutive words. No line means nothing changed, so
//   a compare carries the last line forward (tooling/checks/career_ab.py).
//   ZX_STATE_LOG_FILE=<path> also writes the lines there, where no other
//   thread's log text can splice into them.
// ZX_PAD_DUMP / ZX_PAD_LOG: the same, evaluated at every port-0 slot-0
//   scePadRead (after the pad data is written) instead of at VBlank: the
//   point where tools/pcsx2replay_hook.s records in PCSX2, so per-frame logs
//   of both line up. ZX_PAD_LOG prints a "[pad-state]" line on every read,
//   not only on changes.
// ZX_PACKET_LOG=<file> (abdiff2): what rendering is handed, per vsync, for an
//   A/B of two runs (tooling/checks/career_ab.py): a line
//       P <tick> <vif1 bytes> <hash> <gif bytes> <hash> <vif0 bytes> <hash> <gs bytes> <hash> <vf hash> <vu0 rest hash> <vif1 live hash>
//   with a hash of every byte the DMAC handed VIF1 (the stream as the game built
//   it, before native draws replace anything), GIF PATH3 and VIF0 since the last
//   vsync, of the GS privileged registers at the vsync (PMODE to BGCOLOR), and of
//   the running context's VU0 registers: vf0 to vf31, then ACC, Q, the integer
//   registers and the flags. VU1 memory, the other GS registers and VRAM are made
//   from those streams, so comparing the streams covers them; the VU0 hashes are
//   taken wherever the vsync lands, which can be inside a function. The vif1 live
//   hash leaves out the words no microprogram reads (ps2_state_probe.cpp, kDeadWords).
//   ZX_PACKET_DUMP=<from>:<to> also writes the raw streams of those ticks to
//   <file>.t<tick>.<stream>.
struct R5900Context;

namespace zx::packets
{
    enum Stream : int
    {
        kVif1,
        kGif,
        kVif0,
        kGsRegisters,
        kStreams,
    };
    extern bool g_on;
    void configureFromEnvironment();
    void add(Stream stream, const void *bytes, size_t size);
    void onVsync(uint64_t tick, const R5900Context *ctx);
}

class StateProbe
{
public:
    enum class Op
    {
        Eq,
        Ne,
        Ge,
        Le,
        Gt,
        Lt
    };

    enum class Source
    {
        GuestU32,
        Tick,
        Script
    };

    struct Condition
    {
        Source source = Source::GuestU32;
        uint32_t address = 0u;
        Op op = Op::Eq;
        uint64_t value = 0u;
    };

    struct DumpTrigger
    {
        std::string label;
        std::vector<Condition> conditions;
    };

    using DumpWriter = std::function<void(const std::string &label, uint64_t tick)>;

    // Returns false and leaves the probe unchanged on a malformed spec.
    bool parseDumpSpec(const std::string &spec, std::string *error = nullptr);
    bool parseLogSpec(const std::string &spec, std::string *error = nullptr);
    void configureFromEnvironment();
    // ZX_PAD_DUMP / ZX_PAD_LOG instead of the VBlank variables.
    void configurePadReadFromEnvironment();

    // Called at every VBlank start. Fires at most one dump trigger per call.
    void onVsync(const uint8_t *rdram, uint64_t tick, const DumpWriter &writeDump);
    // Called at every port-0 pad read: logs every call, dumps like onVsync.
    void onPadRead(const uint8_t *rdram, uint64_t tick, const DumpWriter &writeDump);

    // Writes all of EE RAM to <ZX_DUMP_PREFIX or zx-dump>-<label>.ram.
    static void writeRamDump(const uint8_t *rdram, const std::string &label, uint64_t tick);
    // Next to a dump: <prefix>-<label>.sp, the guest threads' stacks and sp at that moment.
    static void writeThreadStacks(const std::string &label, const std::string &text);
    const std::vector<uint32_t> &logAddresses() const { return m_logAddresses; }

    bool enabled() const { return !m_triggers.empty() || !m_logAddresses.empty(); }
    size_t nextTrigger() const { return m_next; }
    const std::vector<DumpTrigger> &triggers() const { return m_triggers; }

    static bool evaluate(const Condition &condition, const uint8_t *rdram, uint64_t tick);
    // "<addr><op><value>", "tick<op><value>" or "script<op><value>".
    static bool parseCondition(const std::string &text, Condition &out);

    // The pad script clock, published by the pad backend at every update.
    static void publishScriptTime(int64_t vsyncs);
    static int64_t scriptTime();

private:
    void configure(const char *dumpVariable, const char *logVariable);
    void probe(const uint8_t *rdram, uint64_t tick, const DumpWriter &writeDump, bool everyCall, const char *tag);

    std::vector<DumpTrigger> m_triggers;
    size_t m_next = 0u;
    std::vector<uint32_t> m_logAddresses;
    std::vector<uint32_t> m_lastLogged;
    bool m_logPrimed = false;
};
