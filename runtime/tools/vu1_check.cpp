// zx-vu1check: replays a VU1 run corpus recorded with ZX_VU1_RECORD
// through VU1Interpreter and compares every run bit for bit with the
// recording: registers and flags out, all 16 KiB of data memory out, cycle
// count, and every PATH1 packet.
//
//   zx-vu1check <corpus.vu1r>... [options]
//
// Options:
//   --stop          stop at the first run that differs
//   --quiet         print only the summary
//   --time          time the replay (interpreter only, thread CPU time, best
//                   of --repeat)
//   --repeat <n>    replay each corpus n times (timing)
//   --profile       per start PC: runs, cycles, packets and replay time
//   --trace <seq>   print pc, cycle and VI registers of every pair of run <seq>
//   --dump <seq> <prefix>  write <prefix>.code/.data-in/.data-out/.regs-in
//                  (vudis.py / ZX_VU1_DUMP layout) and <prefix>.state-in /
//                  .state-out (StateImage) for run <seq>
//   --poke <qword> <lane> <hex>  every run starts with that lane of that data
//                  qword (0 x .. 3 w) holding the value, and the recorded data
//                  out is expected to hold it too: "does anything read this
//                  word?" answered by everything a run produces
//   --poke-was <hex>  the --poke before it only where the lane holds this value
//   --reads <qword>  count the loads (LQ, LQI, LQD, ILW, ILWR) of that data
//                  qword by program counter and lanes
//   --chain         the corpus as one sequence: each run starts from what the
//                  replay of the run before left (VU registers, and every data
//                  qword the VIF did not write between the two runs: those
//                  the recording's data in holds unlike the run before's data
//                  out), not from the recording; a run's PATH1 packets, cycles,
//                  registers and data out are each compared with the
//                  recording. Without pokes every run must match (the model of
//                  the sequence holds); with them it answers "does anything
//                  read these lanes, in this run or any later one?" by what
//                  the VU sends the GS (PATH1, as the GS takes it: a PACKED
//                  qword's bits its register does not read are left out, its
//                  bytes counted apart) and by the runs' control flow
//                  (cycles), and counts the runs whose registers or data still
//                  carry the poked values.
//   --poke-range <first> <last> <lane> <hex>  with --chain: that lane of every
//                  data qword in [first, last] the VIF wrote before the run
//                  holds the value (as --poke, only where the VIF wrote it)
//   --poke-pc <pc>  with --chain: the pokes only before runs that start there
//   --module <path>  load a game module (runtime/game_module.h: a shared
//                  library, as the app makes at first run) and run its VU1
//                  microprograms compiled ahead of time, which replace the
//                  checker's own for the same micro memory: the player's code
//   --poke-top      with --chain: the --poke-range before it counts its qwords
//                  from the run's TOP (VIF1 TOPS: a double-buffered block)
//   --poke-if <qword> <lane> <hex>  with --chain: the --poke-range before it
//                  only where that word (from TOP too with --poke-top) holds
//                  the value (a block told by a word of its own)
// Exit status 0 when every run matches.
#include "runtime/game_module.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_record.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "runtime/ps2_thread_cpu.h"

#include <map>
#include <string>
#include <vector>

namespace
{
    // Thread CPU time: steadier than wall time on a busy machine. Windows
    // counts thread time in 15.6 ms ticks, too coarse per run: wall time.
    double cpuSeconds()
    {
#if defined(_WIN32)
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#else
        return zx::currentThreadCpuSeconds();
#endif
    }

    struct PcStats
    {
        uint64_t runs = 0;
        uint64_t cycles = 0;
        uint64_t packets = 0;
        double seconds = 0.0;
    };

    void writeFile(const std::string &path, const void *data, size_t size)
    {
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(data, 1, size, f);
            std::fclose(f);
        }
    }

    struct Poke
    {
        uint32_t qword = 0;
        uint32_t lane = 0;
        uint32_t value = 0;
        bool onlyIf = false;
        uint32_t was = 0;
        uint32_t last = 0;     // --poke-range: the last qword (qword the first)
        bool range = false;
        bool fromTop = false;  // --poke-top: qword and last are offsets from the run's TOP
        bool checked = false;  // --poke-if: only where word (ifQword, ifLane) holds ifValue
        uint32_t ifQword = 0, ifLane = 0, ifValue = 0;
    };

    // The bits of a PACKED GIF qword the GS takes for register descriptor `reg` (the GS manual's PACKED formats):
    // per 32-bit word of the qword, a mask. RGBAQ the bytes R, G, B, A at bits 0, 32, 64, 96; ST S, T, Q; UV U and V
    // (14 bits); XYZF2/3 X, Y (16 bits), Z (24 at 68), F (8 at 100), ADC (bit 111); XYZ2/3 X, Y, Z (32 at 64), ADC;
    // FOG F; A+D the data and the address byte; NOP nothing; the rest (PRIM, TEX0, CLAMP, the reserved one) whole.
    void packedMask(uint32_t reg, uint32_t mask[4])
    {
        static const uint32_t kWhole[4] = {~0u, ~0u, ~0u, ~0u};
        const uint32_t *m = kWhole;
        static const uint32_t kRgbaq[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
        static const uint32_t kSt[4] = {~0u, ~0u, ~0u, 0u};
        static const uint32_t kUv[4] = {0x3FFFu, 0x3FFFu, 0u, 0u};
        static const uint32_t kXyzf[4] = {0xFFFFu, 0xFFFFu, 0x0FFFFFF0u, 0x8FF0u};
        static const uint32_t kXyz[4] = {0xFFFFu, 0xFFFFu, ~0u, 0x8000u};
        static const uint32_t kFog[4] = {0u, 0u, 0u, 0xFF0u};
        static const uint32_t kAd[4] = {~0u, ~0u, 0xFFu, 0u};
        static const uint32_t kNop[4] = {0u, 0u, 0u, 0u};
        switch (reg)
        {
        case 0x1: m = kRgbaq; break;
        case 0x2: m = kSt; break;
        case 0x3: m = kUv; break;
        case 0x4: case 0xC: m = kXyzf; break;
        case 0x5: case 0xD: m = kXyz; break;
        case 0xA: m = kFog; break;
        case 0xE: m = kAd; break;
        case 0xF: m = kNop; break;
        default: break;
        }
        std::memcpy(mask, m, sizeof(uint32_t) * 4u);
    }

    // Two PATH1 packets the same in what the GS takes from them: the GIF tags whole, REGLIST and IMAGE data whole,
    // PACKED data by its register's bits (packedMask). Unequal sizes, or a packet that does not parse, compare whole.
    bool sameForGs(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
    {
        if (a.size() != b.size() || a.size() % 16u)
            return a == b;
        auto word = [](const std::vector<uint8_t> &v, size_t at) { uint32_t w = 0; std::memcpy(&w, v.data() + at, 4u); return w; };
        size_t at = 0;
        while (at + 16u <= a.size())
        {
            if (std::memcmp(a.data() + at, b.data() + at, 16u) != 0)
                return false; // (the tag)
            const uint64_t low = static_cast<uint64_t>(word(a, at)) | static_cast<uint64_t>(word(a, at + 4u)) << 32;
            const uint64_t regs = static_cast<uint64_t>(word(a, at + 8u)) | static_cast<uint64_t>(word(a, at + 12u)) << 32;
            const uint32_t nloop = static_cast<uint32_t>(low & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>(low >> 58) & 3u;
            const uint32_t nreg = (static_cast<uint32_t>(low >> 60) & 15u) ? static_cast<uint32_t>(low >> 60) & 15u : 16u;
            at += 16u;
            size_t bytes = flg == 0u ? size_t(nloop) * nreg * 16u
                         : flg == 1u ? ((size_t(nloop) * nreg + 1u) / 2u) * 16u
                                     : size_t(nloop) * 16u;
            if (at + bytes > a.size())
                return a == b;
            if (flg != 0u)
            {
                if (std::memcmp(a.data() + at, b.data() + at, bytes) != 0)
                    return false;
            }
            else
            {
                for (uint32_t loop = 0; loop < nloop; ++loop)
                    for (uint32_t r = 0; r < nreg; ++r, at += 16u)
                    {
                        uint32_t mask[4];
                        packedMask(static_cast<uint32_t>(regs >> (4u * r)) & 15u, mask);
                        for (uint32_t w = 0; w < 4u; ++w)
                            if ((word(a, at + 4u * w) ^ word(b, at + 4u * w)) & mask[w])
                                return false;
                    }
                bytes = 0;
            }
            at += bytes;
            if (low & 0x8000u) // EOP
                break;
        }
        return std::memcmp(a.data() + at, b.data() + at, a.size() - at) == 0;
    }

    void printPacketDifference(const std::vector<uint8_t> &want, const std::vector<uint8_t> &got, size_t index)
    {
        size_t at = 0;
        while (at < want.size() && at < got.size() && want[at] == got[at])
            ++at;
        std::printf(" packet %zu (%zu bytes, replay %zu) first differs at byte 0x%zx", index, want.size(), got.size(), at);
    }

    // The VU data load the lower instruction of a pair does now, as VI
    // stands before the pair: true, its qword and its dest lanes (8 x .. 1 w).
    bool lowerLoad(uint32_t lower, const VU1State &state, uint32_t &qword, uint32_t &lanes)
    {
        const uint32_t op = lower >> 25;
        const uint32_t is = (lower >> 11) & 31u;
        lanes = (lower >> 21) & 15u;
        const auto vi = [&state](uint32_t reg)
        { return static_cast<uint32_t>(static_cast<uint16_t>(state.vi[reg & 15u])); };
        if (op == 0x00 || op == 0x04) // LQ, ILW
        {
            const int32_t imm = static_cast<int32_t>(lower << 21) >> 21;
            qword = (vi(is) + imm) & 0x3FFu;
            return true;
        }
        if (op != 0x40 || (lower & 0x3Cu) != 0x3Cu)
            return false;
        const uint32_t ext = (((lower >> 6) & 0x1Fu) << 2) | (lower & 3u);
        if (ext == 0x34 || ext == 0x3E) // LQI, ILWR
            qword = vi(is) & 0x3FFu;
        else if (ext == 0x36) // LQD
            qword = (vi(is) - 1u) & 0x3FFu;
        else
            return false;
        return true;
    }
}

int main(int argc, char **argv)
{
    std::vector<std::string> paths;
    bool stopFirst = false;
    bool quiet = false;
    bool timing = false;
    bool profile = false;
    int repeat = 1;
    long dumpSeq = -1;
    long traceSeq = -1;
    std::string dumpPrefix;
    std::vector<Poke> pokes;
    long readsQword = -1;
    uint64_t poked = 0;
    bool chain = false;
    long pokePc = -1;
    for (int index = 1; index < argc; ++index)
    {
        const std::string arg = argv[index];
        if (arg == "--stop")
            stopFirst = true;
        else if (arg == "--quiet")
            quiet = true;
        else if (arg == "--time")
            timing = true;
        else if (arg == "--profile")
            profile = true;
        else if (arg == "--repeat" && index + 1 < argc)
            repeat = std::max(1, std::atoi(argv[++index]));
        else if (arg == "--trace" && index + 1 < argc)
            traceSeq = std::strtol(argv[++index], nullptr, 0);
        else if (arg == "--dump" && index + 2 < argc)
        {
            dumpSeq = std::strtol(argv[++index], nullptr, 0);
            dumpPrefix = argv[++index];
        }
        else if (arg == "--poke" && index + 3 < argc)
        {
            Poke poke;
            poke.qword = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0));
            poke.lane = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0)) & 3u;
            poke.value = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 16));
            pokes.push_back(poke);
        }
        else if (arg == "--poke-was" && index + 1 < argc && !pokes.empty())
        {
            pokes.back().onlyIf = true;
            pokes.back().was = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 16));
        }
        else if (arg == "--reads" && index + 1 < argc)
            readsQword = std::strtol(argv[++index], nullptr, 0);
        else if (arg == "--chain")
            chain = true;
        else if (arg == "--poke-range" && index + 4 < argc)
        {
            Poke poke;
            poke.range = true;
            poke.qword = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0)) & 0x3FFu;
            poke.last = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0)) & 0x3FFu;
            poke.lane = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0)) & 3u;
            poke.value = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 16));
            pokes.push_back(poke);
        }
        else if (arg == "--poke-top" && !pokes.empty() && pokes.back().range)
            pokes.back().fromTop = true;
        else if (arg == "--poke-if" && index + 3 < argc && !pokes.empty() && pokes.back().range)
        {
            Poke &poke = pokes.back();
            poke.checked = true;
            poke.ifQword = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0)) & 0x3FFu;
            poke.ifLane = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 0)) & 3u;
            poke.ifValue = static_cast<uint32_t>(std::strtoul(argv[++index], nullptr, 16));
        }
        else if (arg == "--poke-pc" && index + 1 < argc)
            pokePc = std::strtol(argv[++index], nullptr, 0);
        else if (arg == "--module" && index + 1 < argc)
        {
            std::string error;
            if (!zx::game_module::load(argv[++index], error))
            {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 2;
            }
        }
        else
            paths.push_back(arg);
    }
    if (paths.empty())
    {
        std::fprintf(stderr, "usage: zx-vu1check <corpus.vu1r>... [--stop] [--quiet] [--time] [--repeat n] "
                             "[--profile] [--dump seq prefix] [--poke qword lane hex] [--reads qword] [--chain "
                             "[--poke-range first last lane hex [--poke-top] [--poke-if qword lane hex]] [--poke-pc pc]] "
                             "[--module path]\n");
        return 2;
    }
    if (!chain && (pokePc >= 0 || std::any_of(pokes.begin(), pokes.end(), [](const Poke &p) { return p.range; })))
    {
        std::fprintf(stderr, "--poke-range and --poke-pc are --chain's\n");
        return 2;
    }
    // --chain's counts: runs whose PATH1 packets, cycles, registers (and flags) or data out differ from the recording.
    uint64_t chainPackets = 0, chainCycles = 0, chainState = 0, chainData = 0, chainBytes = 0;

    PS2Memory memory;
    if (!memory.initialize())
    {
        std::fprintf(stderr, "cannot initialize PS2Memory\n");
        return 2;
    }
    GS gs;
    uint8_t *code = memory.getVU1Code();
    uint8_t *data = memory.getVU1Data();

    std::map<std::pair<uint32_t, uint32_t>, uint64_t> reads; // (pc, dest lanes) -> loads
    uint64_t totalRuns = 0;
    uint64_t badRuns = 0;
    uint64_t totalCycles = 0;
    uint64_t totalPairs = 0;
    double bestSeconds = 0.0;
    std::map<uint32_t, PcStats> stats;

    for (int pass = 0; pass < repeat; ++pass)
    {
        double passSeconds = 0.0;
        for (const std::string &path : paths)
        {
            ps2vu1rec::Reader reader;
            if (!reader.open(path))
            {
                std::fprintf(stderr, "%s: not a VU1 corpus\n", path.c_str());
                return 2;
            }
            VU1Interpreter vu1(VU1Interpreter::Unit::VU1);
            std::vector<std::vector<uint8_t>> packets;
            vu1.setPacketObserver([&packets](const uint8_t *bytes, uint32_t size)
                                  { packets.emplace_back(bytes, bytes + size); },
                                  false);
            std::vector<uint8_t> runCode, dataIn, dataOut;
            std::vector<uint8_t> lastDataOut; // (--chain: the recording's data out of the run before)
            ps2vu1rec::Vu1RecordedRun run;
            bool continuing = false;
            bool chainStart = true;
            while (reader.next(run, runCode, dataIn, dataOut))
            {
                if (std::memcmp(code, runCode.data(), ps2vu1rec::kCodeSize) != 0)
                {
                    std::memcpy(code, runCode.data(), ps2vu1rec::kCodeSize);
                    memory.markVU1CodeModified();
                }
                for (const Poke &poke : pokes)
                {
                    if (chain)
                        break;
                    for (std::vector<uint8_t> *image : {&dataIn, &dataOut})
                    {
                        uint8_t *word = image->data() + poke.qword * 16u + poke.lane * 4u;
                        uint32_t now = 0;
                        std::memcpy(&now, word, 4u);
                        if (!poke.onlyIf || now == poke.was)
                        {
                            std::memcpy(word, &poke.value, 4u);
                            ++poked;
                        }
                    }
                }
                if (!chain || chainStart)
                    std::memcpy(data, dataIn.data(), ps2vu1rec::kDataSize);
                std::vector<bool> written(ps2vu1rec::kDataSize / 16u, chainStart);
                if (chain && !chainStart)
                {
                    // What the VIF wrote since the run before: those qwords as the recording has them; the rest as
                    // the replay left them.
                    for (uint32_t q = 0; q < ps2vu1rec::kDataSize / 16u; ++q)
                    {
                        if (std::memcmp(dataIn.data() + q * 16u, lastDataOut.data() + q * 16u, 16u) != 0)
                        {
                            std::memcpy(data + q * 16u, dataIn.data() + q * 16u, 16u);
                            written[q] = true;
                        }
                    }
                }
                if (chain && (pokePc < 0 || (run.kind == 0u && run.startPC == static_cast<uint32_t>(pokePc))))
                {
                    for (const Poke &poke : pokes)
                    {
                        if (poke.checked)
                        {
                            uint32_t held = 0;
                            std::memcpy(&held, data + (((poke.fromTop ? poke.ifQword + run.top : poke.ifQword) & 0x3FFu) * 16u) + poke.ifLane * 4u, 4u);
                            if (held != poke.ifValue)
                                continue;
                        }
                        for (uint32_t at = poke.qword; at <= (poke.range ? poke.last : poke.qword); ++at)
                        {
                            const uint32_t q = (poke.fromTop ? at + run.top : at) & 0x3FFu;
                            if (poke.range && !written[q])
                                continue;
                            uint8_t *word = data + q * 16u + poke.lane * 4u;
                            uint32_t now = 0;
                            std::memcpy(&now, word, 4u);
                            if (!poke.onlyIf || now == poke.was)
                            {
                                std::memcpy(word, &poke.value, 4u);
                                ++poked;
                            }
                        }
                    }
                }
                lastDataOut = dataOut;
                const bool firstRun = chainStart;
                chainStart = false;

                if (chain ? firstRun : (!continuing || run.startedIdle))
                {
                    VU1State state = vu1.state();
                    ps2vu1rec::applyState(run.stateIn, state);
                    vu1.restoreState(state);
                }
                if (chain && !firstRun)
                    vu1.state().cycles = run.stateIn.cycles; // (the VU's counter between runs, as recorded: no data reads it)
                vu1.state().dBitEnabled = run.stateIn.dBitEnabled != 0u;
                vu1.state().tBitEnabled = run.stateIn.tBitEnabled != 0u;
                packets.clear();

                if (static_cast<long>(run.sequence) == traceSeq && pass == 0)
                    vu1.setPairTrace([](uint32_t pc, const VU1State &state)
                                     {
                                         std::printf("pc %04x cycle %llu vi", pc, static_cast<unsigned long long>(state.cycles));
                                         for (int reg = 1; reg < 16; ++reg)
                                             std::printf(" %x", static_cast<unsigned>(state.vi[reg] & 0xFFFF));
                                         std::printf(" clip %06x mac %04x\n", state.clip, state.mac);
                                     });
                else if (readsQword >= 0 && pass == 0)
                    vu1.setPairTrace([&](uint32_t pc, const VU1State &state)
                                     {
                                         uint32_t lower = 0, qword = 0, lanes = 0;
                                         std::memcpy(&lower, code + (pc & 0x3FF8u), 4u);
                                         if (lowerLoad(lower, state, qword, lanes) && qword == static_cast<uint32_t>(readsQword))
                                             ++reads[{pc, lanes}];
                                     });
                else
                    vu1.setPairTrace(nullptr);
                const uint64_t pairsBefore = vu1.pairsExecuted();
                const double start = cpuSeconds();
                if (run.kind == 0u)
                    vu1.execute(code, ps2vu1rec::kCodeSize, data, ps2vu1rec::kDataSize, gs, &memory,
                                run.startPC, run.top, run.itop, run.maxCycles);
                else
                    vu1.resume(code, ps2vu1rec::kCodeSize, data, ps2vu1rec::kDataSize, gs, &memory,
                               run.top, run.itop, run.maxCycles);
                const double seconds = cpuSeconds() - start;
                passSeconds += seconds;
                continuing = !vu1.pipelinesIdle();

                if (pass != 0)
                    continue;

                ++totalRuns;
                totalPairs += vu1.pairsExecuted() - pairsBefore;
                totalCycles += run.stateOut.cycles - run.stateIn.cycles;
                PcStats &pc = stats[run.kind == 0u ? run.startPC : 0x80000000u | run.startPC];
                ++pc.runs;
                pc.cycles += run.stateOut.cycles - run.stateIn.cycles;
                pc.packets += run.packets.size();
                pc.seconds += seconds;

                std::string diff;
                const ps2vu1rec::StateImage out = ps2vu1rec::captureState(vu1.state());
                bool same = ps2vu1rec::compareStates(run.stateOut, out, &diff);
                if (std::memcmp(data, dataOut.data(), ps2vu1rec::kDataSize) != 0)
                {
                    same = false;
                    uint32_t differing = 0;
                    uint32_t first = 0;
                    for (uint32_t q = 0; q < ps2vu1rec::kDataSize / 16u; ++q)
                    {
                        if (std::memcmp(data + q * 16u, dataOut.data() + q * 16u, 16u) != 0)
                        {
                            if (differing++ == 0)
                                first = q;
                        }
                    }
                    diff += " data: " + std::to_string(differing) + " qwords differ, first at qword " +
                            std::to_string(first);
                }
                if (packets.size() != run.packets.size())
                {
                    same = false;
                    diff += " packets: want " + std::to_string(run.packets.size()) + " got " +
                            std::to_string(packets.size());
                }
                else
                {
                    for (size_t p = 0; p < packets.size(); ++p)
                    {
                        if (packets[p] != run.packets[p])
                        {
                            same = false;
                            diff += " packet " + std::to_string(p) + " differs";
                            break;
                        }
                    }
                }

                if (chain)
                {
                    // Each part apart: what the GS is sent, the control flow (cycles), the rest of the state, data.
                    // (what the GS takes from them: a PACKED qword's bits its register does not read are no output)
                    bool packetsSame = packets.size() == run.packets.size();
                    for (size_t p = 0; packetsSame && p < packets.size(); ++p)
                        packetsSame = sameForGs(packets[p], run.packets[p]);
                    chainBytes += packets == run.packets ? 0u : 1u;
                    const bool cyclesSame = out.cycles == run.stateOut.cycles;
                    ps2vu1rec::StateImage equalCycles = out;
                    equalCycles.cycles = run.stateOut.cycles;
                    std::string stateDiff;
                    const bool stateSame = ps2vu1rec::compareStates(run.stateOut, equalCycles, &stateDiff);
                    const bool dataSame = std::memcmp(data, dataOut.data(), ps2vu1rec::kDataSize) == 0;
                    chainPackets += packetsSame ? 0u : 1u;
                    chainCycles += cyclesSame ? 0u : 1u;
                    chainState += stateSame ? 0u : 1u;
                    chainData += dataSame ? 0u : 1u;
                    if (!packetsSame || !cyclesSame)
                    {
                        ++badRuns;
                        if (!quiet)
                        {
                            std::printf("CHAIN %s run %llu (%s pc=0x%x vsync=%llu):", path.c_str(),
                                        static_cast<unsigned long long>(run.sequence), run.kind == 0u ? "MSCAL" : "MSCNT",
                                        run.startPC, static_cast<unsigned long long>(run.vsync));
                            if (!cyclesSame)
                                std::printf(" cycles want %llu got %llu", static_cast<unsigned long long>(run.stateOut.cycles - run.stateIn.cycles),
                                            static_cast<unsigned long long>(out.cycles - run.stateIn.cycles));
                            if (packets.size() != run.packets.size())
                                std::printf(" packets: want %zu got %zu", run.packets.size(), packets.size());
                            else
                                for (size_t p = 0; p < packets.size(); ++p)
                                    if (packets[p] != run.packets[p])
                                    {
                                        printPacketDifference(run.packets[p], packets[p], p);
                                        break;
                                    }
                            std::printf("\n");
                        }
                        if (stopFirst)
                            return 1;
                    }
                    else if (pokes.empty() && (!stateSame || !dataSame))
                    {
                        ++badRuns; // (without pokes the sequence's model must hold whole)
                        if (!quiet)
                            std::printf("CHAIN %s run %llu (pc=0x%x): the model of the sequence fails:%s%s\n", path.c_str(),
                                    static_cast<unsigned long long>(run.sequence), run.startPC, stateDiff.c_str(),
                                    dataSame ? "" : " data");
                    }
                    continue;
                }
                if (static_cast<long>(run.sequence) == dumpSeq)
                {
                    writeFile(dumpPrefix + ".code", runCode.data(), runCode.size());
                    writeFile(dumpPrefix + ".data-in", dataIn.data(), dataIn.size());
                    writeFile(dumpPrefix + ".data-out", dataOut.data(), dataOut.size());
                    std::vector<uint8_t> regs(32u * 16u + 16u * 4u + 12u);
                    std::memcpy(regs.data(), run.stateIn.vf, sizeof(run.stateIn.vf));
                    std::memcpy(regs.data() + sizeof(run.stateIn.vf), run.stateIn.vi, sizeof(run.stateIn.vi));
                    const uint32_t extra[3] = {run.startPC, run.top, run.itop};
                    std::memcpy(regs.data() + sizeof(run.stateIn.vf) + sizeof(run.stateIn.vi), extra, sizeof(extra));
                    writeFile(dumpPrefix + ".regs-in", regs.data(), regs.size());
                    writeFile(dumpPrefix + ".state-in", &run.stateIn, sizeof(run.stateIn));
                    writeFile(dumpPrefix + ".state-out", &run.stateOut, sizeof(run.stateOut));
                    std::vector<uint8_t> allPackets;
                    for (const std::vector<uint8_t> &packet : run.packets)
                        allPackets.insert(allPackets.end(), packet.begin(), packet.end());
                    writeFile(dumpPrefix + ".gif", allPackets.data(), allPackets.size());
                    std::printf("dumped run %ld (%s pc=0x%x) to %s.*\n", dumpSeq, run.kind == 0u ? "MSCAL" : "MSCNT",
                                run.startPC, dumpPrefix.c_str());
                }

                if (!same)
                {
                    ++badRuns;
                    if (!quiet)
                        std::printf("MISMATCH %s run %llu (%s pc=0x%x vsync=%llu):%s\n", path.c_str(),
                                    static_cast<unsigned long long>(run.sequence),
                                    run.kind == 0u ? "MSCAL" : "MSCNT", run.startPC,
                                    static_cast<unsigned long long>(run.vsync), diff.c_str());
                    if (stopFirst)
                        return 1;
                }
            }
        }
        if (pass == 0 || passSeconds < bestSeconds)
            bestSeconds = passSeconds;
    }

    std::printf("%llu runs, %llu mismatching, %llu VU cycles, %llu instruction pairs\n",
                static_cast<unsigned long long>(totalRuns), static_cast<unsigned long long>(badRuns),
                static_cast<unsigned long long>(totalCycles), static_cast<unsigned long long>(totalPairs));
    if (timing)
        std::printf("replay %.3f ms (best of %d), %.1f ns per VU cycle\n", bestSeconds * 1e3, repeat,
                    totalCycles ? bestSeconds * 1e9 / static_cast<double>(totalCycles) : 0.0);
    if (chain)
        std::printf("chain: %llu runs differ in PATH1 packets as the GS takes them (%llu in their bytes), %llu in cycles, "
                    "%llu in registers or flags, %llu in data out; %llu lanes poked\n",
                    static_cast<unsigned long long>(chainPackets), static_cast<unsigned long long>(chainBytes),
                    static_cast<unsigned long long>(chainCycles),
                    static_cast<unsigned long long>(chainState), static_cast<unsigned long long>(chainData),
                    static_cast<unsigned long long>(poked));
    else if (!pokes.empty())
        std::printf("poked %llu images (a run's data in, data out)\n", static_cast<unsigned long long>(poked));
    if (readsQword >= 0)
    {
        uint64_t total = 0;
        for (const auto &[where, count] : reads)
        {
            std::printf("reads of qword 0x%lx: pc 0x%04x lanes %c%c%c%c: %llu\n", readsQword, where.first,
                        (where.second & 8u) ? 'x' : '-', (where.second & 4u) ? 'y' : '-', (where.second & 2u) ? 'z' : '-',
                        (where.second & 1u) ? 'w' : '-', static_cast<unsigned long long>(count));
            total += count;
        }
        std::printf("reads of qword 0x%lx: %llu\n", readsQword, static_cast<unsigned long long>(total));
    }
    if (profile)
    {
        std::vector<std::pair<uint32_t, PcStats>> sorted(stats.begin(), stats.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b)
                  { return a.second.cycles > b.second.cycles; });
        std::printf("%-12s %8s %12s %10s %10s\n", "start", "runs", "cycles", "packets", "ms");
        for (const auto &[pc, s] : sorted)
            std::printf("%s0x%-8x %8llu %12llu %10llu %10.3f\n", (pc & 0x80000000u) ? "C" : " ", pc & 0x7FFFFFFFu,
                        static_cast<unsigned long long>(s.runs), static_cast<unsigned long long>(s.cycles),
                        static_cast<unsigned long long>(s.packets), s.seconds * 1e3);
    }
    return badRuns == 0 ? 0 : 1;
}
