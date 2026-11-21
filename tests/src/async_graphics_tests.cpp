#include "MiniTest.h"
#include "runtime/async_stage.h"
#include "runtime/memory.h"
#include "gs/gif_arbiter.h"

#include <array>
#include <functional>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


namespace
{
    constexpr uint32_t kVif1Ch = 0x10009000u;
    constexpr uint32_t kVif1Stat = 0x10003C00u;
    constexpr uint32_t kVif1Fbrst = 0x10003C10u;
    constexpr uint32_t kStream = 0x00030000u;

    uint32_t vifCmd(uint32_t opcode, uint32_t num, uint32_t imm, bool irq = false)
    {
        return (irq ? 0x80000000u : 0u) | (opcode << 24) | (num << 16) | (imm & 0xFFFFu);
    }

    // A VIF1 stream with an interrupt-bit VIFcode in the middle: unpack,
    // MARK, the interrupt NOP, then BASE/OFFSET, MSCAL and another unpack.
    std::vector<uint32_t> streamWithInterrupt()
    {
        std::vector<uint32_t> w;
        w.push_back(vifCmd(0x01u, 0u, 0x0101u));       // STCYCL 1,1
        w.push_back(vifCmd(0x6Cu, 2u, 0x0010u));       // UNPACK V4-32, 2 vectors at 0x10
        for (uint32_t i = 0; i < 8u; ++i)
            w.push_back(0x3F800000u + i);
        w.push_back(vifCmd(0x07u, 0u, 0x1234u));       // MARK
        w.push_back(vifCmd(0x00u, 0u, 0u, true));      // NOP with the interrupt bit
        w.push_back(vifCmd(0x03u, 0u, 0x0020u));       // BASE
        w.push_back(vifCmd(0x02u, 0u, 0x0040u));       // OFFSET
        w.push_back(vifCmd(0x14u, 0u, 0x0000u));       // MSCAL 0
        w.push_back(vifCmd(0x6Cu, 1u, 0x0030u));       // UNPACK V4-32, 1 vector at 0x30
        for (uint32_t i = 0; i < 4u; ++i)
            w.push_back(0x40000000u + i);
        while ((w.size() % 4u) != 0u)
            w.push_back(0u);
        return w;
    }

    struct Observed
    {
        std::array<uint32_t, 15> vif1Regs{};
        uint32_t gifStat = 0;
        uint32_t chcr = 0;
        std::vector<uint32_t> intc;
        std::vector<uint8_t> vu1Data;
        uint32_t mscals = 0;
    };

    void observe(PS2Memory &mem, Observed &out, const std::atomic<uint32_t> &mscals)
    {
        for (uint32_t i = 0; i < 15u; ++i)
            out.vif1Regs[i] = mem.readIORegister(kVif1Stat + i * 16u);
        out.gifStat = mem.readIORegister(0x10003020u);
        out.chcr = mem.readIORegister(kVif1Ch);
        out.intc = mem.consumePendingIntcCauses();
        mem.syncGraphics();
        out.vu1Data.assign(mem.getVU1Data(), mem.getVU1Data() + 0x400u);
        out.mscals = mscals.load();
    }

    // Kick the stream, observe; FBRST.STC, observe.
    void runScenario(bool async, Observed &stalled, Observed &resumed)
    {
        PS2Memory mem;
        mem.initialize();
        std::atomic<uint32_t> mscals{0};
        mem.setVu1MscalCallback([&](uint32_t, uint32_t, uint32_t)
                                { mscals.fetch_add(1u); });
        mem.setAsyncGraphics(async);
        const std::vector<uint32_t> words = streamWithInterrupt();
        std::memcpy(mem.getRDRAM() + kStream, words.data(), words.size() * 4u);
        mem.writeIORegister(kVif1Ch + 0x10u, kStream);
        mem.writeIORegister(kVif1Ch + 0x20u, static_cast<uint32_t>(words.size() / 4u));
        mem.writeIORegister(kVif1Ch, 0x100u);
        observe(mem, stalled, mscals);
        mem.writeIORegister(kVif1Fbrst, 0x8u);
        observe(mem, resumed, mscals);
        mem.stopAsyncGraphics();
    }

    // DMAtag: qwc, id, address (upper 32 bits of the low word).
    void putTag(PS2Memory &mem, uint32_t at, uint32_t qwc, uint32_t id, uint32_t addr, uint32_t w2 = 0u, uint32_t w3 = 0u)
    {
        const uint32_t tag[4] = {qwc | (id << 28), addr, w2, w3};
        std::memcpy(mem.getRDRAM() + at, tag, sizeof(tag));
    }

    void putWords(PS2Memory &mem, uint32_t at, const std::vector<uint32_t> &words)
    {
        std::memcpy(mem.getRDRAM() + at, words.data(), words.size() * 4u);
    }

    uint32_t vu1Word(PS2Memory &mem, uint32_t qw, uint32_t lane)
    {
        mem.syncGraphics();
        uint32_t v = 0;
        std::memcpy(&v, mem.getVU1Data() + qw * 16u + lane * 4u, 4u);
        return v;
    }

    // A chain like the game's mesh packets: a CNT whose payload ends in an
    // interrupt-bit NOP (the texture request), then a REF to a block the
    // interrupt handler patches (the material's TEX0), then END.
    constexpr uint32_t kChain = 0x00040000u;
    constexpr uint32_t kBlockA = 0x00041000u;
    constexpr uint32_t kBlockB = 0x00042000u;

    std::vector<uint32_t> unpackOne(uint32_t vuAddr, uint32_t value)
    {
        return {vifCmd(0x01u, 0u, 0x0101u), vifCmd(0x6Cu, 1u, vuAddr), value, value + 1u, value + 2u, value + 3u, 0u, 0u};
    }

    void buildInterruptChain(PS2Memory &mem)
    {
        putTag(mem, kChain, 1u, 1u, 0u);                                                          // CNT 1
        putWords(mem, kChain + 16u, {vifCmd(0x07u, 0u, 0x0001u), vifCmd(0x00u, 0u, 0u, true), 0u, 0u}); // MARK 1, NOP+i
        putTag(mem, kChain + 32u, 2u, 3u, kBlockA);                                                // REF 2 -> A
        putTag(mem, kChain + 48u, 1u, 7u, 0u);                                                     // END 1
        putWords(mem, kChain + 64u, {vifCmd(0x07u, 0u, 0x0002u), 0u, 0u, 0u});                      // MARK 2
        putWords(mem, kBlockA, unpackOne(0x40u, 0x1000u));
        putWords(mem, kBlockB, unpackOne(0x40u, 0x3000u));
    }

    void kickChain(PS2Memory &mem, uint32_t tadr, bool tte = false)
    {
        mem.writeIORegister(kVif1Ch + 0x30u, tadr);
        mem.writeIORegister(kVif1Ch + 0x20u, 0u);
        mem.writeIORegister(kVif1Ch, 0x100u | (1u << 2) | (tte ? 0x40u : 0u));
    }

    struct RefetchResult
    {
        bool stalled = false;
        uint32_t vu1 = 0;
        uint32_t mark = 0;
        bool busyAfter = true;
    };

    // Kick, let the "handler" patch memory while VIF1 is stalled, STC.
    template <typename Patch>
    RefetchResult runRefetch(bool async, Patch patch, bool tte = false)
    {
        PS2Memory mem;
        mem.initialize();
        mem.setAsyncGraphics(async);
        buildInterruptChain(mem);
        kickChain(mem, kChain, tte);
        RefetchResult r;
        r.stalled = mem.isVif1StalledOnInterrupt() && mem.consumePendingIntcCauses() == std::vector<uint32_t>{5u};
        patch(mem);
        mem.writeIORegister(kVif1Fbrst, 0x8u);
        r.vu1 = vu1Word(mem, 0x40u, 0u);
        r.mark = mem.readIORegister(0x10003C30u);
        r.busyAfter = (mem.readIORegister(kVif1Ch) & 0x100u) != 0u;
        mem.stopAsyncGraphics();
        return r;
    }
    // Native draws (PS2Memory::setVif1Substitute): two "nodes" as the game's
    // renderer builds them. Node A: a CALL (its payload: MARK, the texture
    // request NOP with the interrupt bit, MSCAL 4, MSCAL 0) of a material
    // chain (REF of a GS block the "handler" patches, CALL of a mesh packet:
    // MSCAL 2 and MSCNT, RET), RET. Node B: the same packet shape, not
    // substituted. Then END.
    constexpr uint32_t kNodeA = 0x00050000u;
    constexpr uint32_t kNodeB = 0x00050030u; // where node A's CALL returns
    constexpr uint32_t kEnd = 0x00050060u;
    constexpr uint32_t kMatChain = 0x00051000u;
    constexpr uint32_t kMesh = 0x00052000u;
    constexpr uint32_t kGsBlock = 0x00053000u;

    void buildNativeChain(PS2Memory &mem)
    {
        putTag(mem, kNodeA, 2u, 5u, kMatChain);
        putWords(mem, kNodeA + 16u, {vifCmd(0x07u, 0u, 1u), vifCmd(0x00u, 0u, 0u, true), vifCmd(0x14u, 0u, 4u),
                                     vifCmd(0x14u, 0u, 0u), 0u, 0u, 0u, 0u});
        putTag(mem, kNodeB, 2u, 5u, kMatChain);
        putWords(mem, kNodeB + 16u, {vifCmd(0x07u, 0u, 1u), 0u, vifCmd(0x14u, 0u, 4u), vifCmd(0x14u, 0u, 0u), 0u, 0u, 0u, 0u});
        putTag(mem, kEnd, 0u, 7u, 0u);
        putTag(mem, kMatChain, 2u, 3u, kGsBlock);
        putTag(mem, kMatChain + 16u, 0u, 5u, kMesh);
        putTag(mem, kMatChain + 32u, 0u, 6u, 0u);
        putWords(mem, kGsBlock, unpackOne(0x40u, 0x1000u));
        putTag(mem, kMesh, 1u, 6u, 0u);
        putWords(mem, kMesh + 16u, {vifCmd(0x14u, 0u, 2u), vifCmd(0x17u, 0u, 0u), 0u, 0u});
    }

    struct NativeRun
    {
        std::vector<std::string> seen; // GS side, in order
        uint32_t stat = 0, tops = 0;   // VIF1 registers after
        uint32_t blockAtBegin = 0;     // the GS block as begin() read it
        uint32_t vu1 = 0;
        PS2Memory::Vif1SubstituteStats stats;
    };

    NativeRun runNative(bool async, bool substitute, uint64_t tag)
    {
        PS2Memory mem;
        mem.initialize();
        mem.setAsyncGraphics(async);
        NativeRun r;
        std::mutex seenMutex;
        auto note = [&](const std::string &what)
        {
            std::lock_guard<std::mutex> lock(seenMutex);
            r.seen.push_back(what);
        };
        mem.setGsPacketSink([&](const uint8_t *data, uint32_t)
                            { note("vu1 " + std::to_string(data[0])); });
        GifArbiter arbiter([&](const uint8_t *data, uint32_t size)
                           { mem.deliverGifPacket(data, size); });
        mem.setGifArbiter(&arbiter);
        // A VU1 run "kicks" a packet naming its program.
        mem.setVu1MscalCallback([&](uint32_t pc, uint32_t, uint32_t)
                                {
                                    std::array<uint8_t, 16> packet{};
                                    packet[0] = static_cast<uint8_t>(pc / 8u);
                                    mem.submitGifPacket(GifPathId::Path1, packet.data(), 16u); });
        mem.setVu1MscntCallback([&](uint32_t, uint32_t)
                                {
                                    std::array<uint8_t, 16> packet{};
                                    packet[0] = 99;
                                    mem.submitGifPacket(GifPathId::Path1, packet.data(), 16u); });
        buildNativeChain(mem);
        if (substitute)
        {
            PS2Memory::Vif1Substitute sub;
            sub.tag = tag;
            sub.begin = [&]() -> std::function<void()>
            {
                std::memcpy(&r.blockAtBegin, mem.getRDRAM() + kGsBlock + 8u, 4u);
                return [&]()
                { note("native"); };
            };
            mem.setVif1Substitute(0x20000000u | kNodeA, std::move(sub)); // the EE's uncached mirror
        }
        kickChain(mem, kNodeA);
        mem.consumePendingIntcCauses();
        putWords(mem, kGsBlock, unpackOne(0x40u, 0x2000u)); // the "handler" patches the block
        mem.writeIORegister(kVif1Fbrst, 0x8u);
        mem.syncGraphics();
        r.stat = mem.readIORegister(kVif1Stat);
        r.tops = mem.readIORegister(0x10003CC0u);
        r.vu1 = vu1Word(mem, 0x40u, 0u);
        r.stats = mem.vif1SubstituteStats();
        mem.stopAsyncGraphics();
        return r;
    }
}

void register_async_graphics_tests()
{
    MiniTest::Case("AsyncGraphics", [](TestCase &tc)
    {
        tc.Run("async stage runs tasks in order and drain waits for all of them", [](TestCase &t)
        {
            Ps2AsyncStage stage("test-stage");
            std::vector<int> seen;
            bool ordered = true;
            for (int round = 0; round < 50; ++round)
            {
                for (int i = 0; i < 200; ++i)
                    stage.post([&seen, value = round * 200 + i]()
                               { seen.push_back(value); });
                stage.drain();
                t.IsTrue(stage.idle(), "stage should be idle after drain");
                ordered = ordered && static_cast<int>(seen.size()) == (round + 1) * 200;
            }
            for (size_t i = 0; i < seen.size(); ++i)
                ordered = ordered && seen[i] == static_cast<int>(i);
            t.IsTrue(ordered, "tasks should run exactly once, in posting order");
            stage.stop();
        });

        tc.Run("GS side-band notes run in stream order with GIF packets", [](TestCase &t)
        {
            for (const bool async : {false, true})
            {
                PS2Memory mem;
                mem.initialize();
                mem.setAsyncGraphics(async);
                std::vector<std::string> seen;
                std::mutex seenMutex;
                mem.setGsPacketSink([&](const uint8_t *data, uint32_t size)
                                    {
                                        std::lock_guard<std::mutex> lock(seenMutex);
                                        seen.push_back("packet " + std::to_string(data[0]) + " " + std::to_string(size)); });
                std::array<uint8_t, 16> packet{};
                bool ordered = true;
                for (int round = 0; round < 100; ++round)
                {
                    seen.clear();
                    packet[0] = 1;
                    mem.deliverGifPacket(packet.data(), 16u);
                    mem.postGsSideband([&]()
                                       {
                                           std::lock_guard<std::mutex> lock(seenMutex);
                                           seen.push_back("note"); });
                    packet[0] = 2;
                    mem.deliverGifPacket(packet.data(), 16u);
                    mem.syncGraphics();
                    ordered = ordered && seen == std::vector<std::string>{"packet 1 16", "note", "packet 2 16"};
                }
                t.IsTrue(ordered, std::string(async ? "async" : "sync") + ": packet, note, packet");
                mem.stopAsyncGraphics();
            }
        });

        tc.Run("async stage drain from the worker itself does not deadlock", [](TestCase &t)
        {
            Ps2AsyncStage stage("test-stage");
            std::atomic<bool> ran{false};
            stage.post([&]()
                       {
                           stage.drain();
                           ran = true; });
            stage.drain();
            t.IsTrue(ran.load(), "task that drains its own stage should complete");
        });

        tc.Run("async stage stop finishes queued tasks", [](TestCase &t)
        {
            std::atomic<int> count{0};
            {
                Ps2AsyncStage stage("test-stage");
                for (int i = 0; i < 1000; ++i)
                    stage.post([&]()
                               { count.fetch_add(1); });
            }
            t.Equals(count.load(), 1000, "destroying the stage should run every posted task");
        });

        tc.Run("async VIF1 with an interrupt-bit VIFcode matches synchronous VIF1", [](TestCase &t)
        {
            Observed syncStalled, syncResumed;
            runScenario(false, syncStalled, syncResumed);
            t.IsTrue(syncStalled.intc.size() == 1u && syncStalled.intc[0] == 5u, "sync: INTC VIF1 raised once");
            t.IsTrue((syncStalled.vif1Regs[0] & (1u << 11)) != 0u, "sync: STAT.INT set while stalled");
            t.Equals(syncStalled.vif1Regs[3], 0x1234u, "sync: MARK from the stream");
            t.Equals(syncStalled.mscals, 0u, "sync: MSCAL after the stall has not run");
            t.Equals(syncResumed.mscals, 1u, "sync: MSCAL runs after STC");

            // Repeat to give a race a chance to show.
            bool same = true;
            for (int round = 0; round < 200 && same; ++round)
            {
                Observed asyncStalled, asyncResumed;
                runScenario(true, asyncStalled, asyncResumed);
                same = asyncStalled.vif1Regs == syncStalled.vif1Regs && asyncStalled.gifStat == syncStalled.gifStat &&
                       asyncStalled.chcr == syncStalled.chcr && asyncStalled.intc == syncStalled.intc &&
                       asyncStalled.vu1Data == syncStalled.vu1Data && asyncStalled.mscals == syncStalled.mscals &&
                       asyncResumed.vif1Regs == syncResumed.vif1Regs && asyncResumed.gifStat == syncResumed.gifStat &&
                       asyncResumed.chcr == syncResumed.chcr && asyncResumed.intc == syncResumed.intc &&
                       asyncResumed.vu1Data == syncResumed.vu1Data && asyncResumed.mscals == syncResumed.mscals;
            }
            t.IsTrue(same, "async VIF1 registers (shadow reads), DMA state, INTC causes, VU1 memory and MSCALs should match synchronous VIF1");
        });

        tc.Run("VIF1 chain behind an interrupt stall is read after the handler ran", [](TestCase &t)
        {
            for (const bool async : {false, true})
            {
                const char *mode = async ? "async" : "sync";
                // The handler patches the REF'd block (the game's TEX0 in a material block).
                RefetchResult r = runRefetch(async, [](PS2Memory &mem)
                                             { putWords(mem, kBlockA + 8u, {0x2000u}); });
                t.IsTrue(r.stalled, std::string(mode) + ": VIF1 stalls on the interrupt-bit NOP");
                t.Equals(r.vu1, 0x2000u, std::string(mode) + ": REF data written by the handler reaches VU1");
                t.Equals(r.mark, 0x0002u, std::string(mode) + ": the chain runs to its END");
                t.IsFalse(r.busyAfter, std::string(mode) + ": VIF1 DMA completes after STC");

                // The handler rewrites a tag the DMAC has not read yet.
                r = runRefetch(async, [](PS2Memory &mem)
                               { putTag(mem, kChain + 32u, 2u, 3u, kBlockB); });
                t.Equals(r.vu1, 0x3000u, std::string(mode) + ": a tag rewritten by the handler is followed");

                // Nothing patched: the same data as before.
                r = runRefetch(async, [](PS2Memory &) {});
                t.Equals(r.vu1, 0x1000u, std::string(mode) + ": unpatched chain unchanged");
            }
        });

        tc.Run("VIF1 data after an interrupt stall in the same block is read after the handler", [](TestCase &t)
        {
            for (const bool async : {false, true})
            {
                PS2Memory mem;
                mem.initialize();
                mem.setAsyncGraphics(async);
                std::vector<uint32_t> words = {vifCmd(0x00u, 0u, 0u, true), 0u, 0u, 0u};
                const std::vector<uint32_t> tail = unpackOne(0x50u, 0x1000u);
                words.insert(words.end(), tail.begin(), tail.end());
                putWords(mem, kStream, words);
                mem.writeIORegister(kVif1Ch + 0x10u, kStream);
                mem.writeIORegister(kVif1Ch + 0x20u, static_cast<uint32_t>(words.size() / 4u));
                mem.writeIORegister(kVif1Ch, 0x100u);
                t.IsTrue(mem.isVif1StalledOnInterrupt(), "normal mode stalls on the interrupt bit");
                putWords(mem, kStream + 16u + 8u, {0x4000u});
                mem.writeIORegister(kVif1Fbrst, 0x8u);
                t.Equals(vu1Word(mem, 0x50u, 0u), 0x4000u, std::string(async ? "async" : "sync") + ": normal-mode data behind the stall is read after the handler");
                mem.stopAsyncGraphics();
            }
        });

        tc.Run("VIF1 interrupt stall in a tag's TTE words", [](TestCase &t)
        {
            for (const bool async : {false, true})
            {
                PS2Memory mem;
                mem.initialize();
                mem.setAsyncGraphics(async);
                // TTE: each tag sends its upper 64 bits first. The first tag's
                // words are MARK 1 and the interrupt NOP; its REF block follows.
                putTag(mem, kChain, 2u, 3u, kBlockA, vifCmd(0x07u, 0u, 0x0001u), vifCmd(0x00u, 0u, 0u, true));
                putTag(mem, kChain + 16u, 0u, 7u, 0u, vifCmd(0x07u, 0u, 0x0003u), 0u);
                putWords(mem, kBlockA, unpackOne(0x40u, 0x1000u));
                kickChain(mem, kChain, true);
                t.IsTrue(mem.isVif1StalledOnInterrupt(), "stall on the TTE interrupt NOP");
                t.Equals(mem.readIORegister(0x10003C30u), 0x0001u, "MARK before the stall");
                t.Equals(mem.readIORegister(kVif1Ch + 0x30u), kChain + 16u, "TADR at the next unread tag while stalled");
                putWords(mem, kBlockA + 8u, {0x5000u});
                mem.writeIORegister(kVif1Fbrst, 0x8u);
                const std::string mode = async ? "async" : "sync";
                t.Equals(vu1Word(mem, 0x40u, 0u), 0x5000u, mode + ": the tag's data is read after the handler");
                t.Equals(mem.readIORegister(0x10003C30u), 0x0003u, mode + ": the END tag's TTE words follow");
                mem.stopAsyncGraphics();
            }
        });

        tc.Run("ZX_VIF1_PREFETCH reads the whole VIF1 chain at the kick", [](TestCase &t)
        {
            PS2Memory::s_vif1PrefetchChain = true;
            RefetchResult r = runRefetch(true, [](PS2Memory &mem)
                                         { putWords(mem, kBlockA + 8u, {0x2000u}); });
            PS2Memory::s_vif1PrefetchChain = false;
            t.IsTrue(r.stalled, "stalls as before");
            t.Equals(r.vu1, 0x1000u, "data copied at the kick: the handler's write is not seen");
            t.Equals(r.mark, 0x0002u, "the chain runs to its END");
        });

        tc.Run("native draws: a substituted packet's VU1 runs are replaced in stream order", [](TestCase &t)
        {
            const uint64_t tagA = (static_cast<uint64_t>(kMatChain) << 32) | 0x50000000u;
            for (const bool async : {false, true})
            {
                const std::string mode = async ? "async: " : "sync: ";
                const NativeRun strict = runNative(async, false, tagA);
                const NativeRun native = runNative(async, true, tagA);
                t.Equals(strict.seen, std::vector<std::string>{"vu1 4", "vu1 0", "vu1 2", "vu1 99", "vu1 4", "vu1 0", "vu1 2", "vu1 99"},
                         mode + "strict: both nodes through VU1");
                t.Equals(native.seen, std::vector<std::string>{"native", "vu1 4", "vu1 0", "vu1 2", "vu1 99"},
                         mode + "node A native, in its place; node B through VU1");
                t.Equals(native.stat, strict.stat, mode + "VIF1_STAT (DBF) as without substitution");
                t.Equals(native.tops, strict.tops, mode + "VIF1_TOPS as without substitution");
                t.Equals(native.vu1, strict.vu1, mode + "UNPACKs still reach VU1 memory");
                t.Equals(native.blockAtBegin, 0x2000u, mode + "begin() reads RAM after the interrupt handler");
                t.Equals(native.stats.packets, uint64_t(1), mode + "one packet substituted");
                t.Equals(native.stats.runsSkipped, uint64_t(4), mode + "its four runs skipped");
            }
        });

        tc.Run("native draws: another packet at the registered address runs on VU1", [](TestCase &t)
        {
            const uint64_t otherTag = (static_cast<uint64_t>(0x00059000u) << 32) | 0x50000000u;
            const NativeRun r = runNative(true, true, otherTag);
            t.Equals(r.seen.size(), size_t(8), "both nodes through VU1");
            t.Equals(r.stats.mismatched, uint64_t(1), "counted as stale");
        });

        tc.Run("async VIF1 FIFO writes and VU1 memory reads see queued work", [](TestCase &t)
        {
            PS2Memory mem;
            mem.initialize();
            mem.setAsyncGraphics(true);
            // STCYCL, then UNPACK V4-8 (zero-extended) 1 vector at 0x5, through the FIFO.
            const uint32_t words[4] = {vifCmd(0x01u, 0u, 0x0101u), vifCmd(0x6Eu, 1u, 0x4005u), 0x04030201u, 0u};
            mem.write128(0x10005000u, _mm_loadu_si128(reinterpret_cast<const __m128i *>(words)));
            // VU1 data memory at 0x1100C000 + 0x5 * 16.
            t.Equals(mem.read32(0x1100C050u), 1u, "read of VU1 memory should wait for the unpack");
            t.Equals(mem.read32(0x1100C05Cu), 4u, "last lane of the unpacked vector");
            t.Equals(mem.readIORegister(0x10003C40u), 0x0101u, "CYCLE from the shadow");
            mem.stopAsyncGraphics();
        });
    });
}
