// Unit tests for the PCSX2-derived SPU2 in iop/: ADPCM decoding, register
// round trips through the IOP memory map, DMA4 into SPU2 RAM, voice key-on
// output, the SPU2 IRQ and ADMA streaming.

#include "emulator/core/iop_memory.h"
#include "spu2/spu2.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop::detail;
    using zx::Spu2;

    constexpr uint32_t kSpu = 0xBF900000u;
    constexpr uint32_t kCore1 = 0x400u;
    constexpr uint32_t kDma4Madr = 0xBF8010C0u;
    constexpr uint32_t kDma4Bcr = 0xBF8010C4u;
    constexpr uint32_t kDma4Chcr = 0xBF8010C8u;

    bool expect(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    // Straight transcription of the PS1/PS2 ADPCM formula, independent of the
    // port: s = (nibble << 12) >> shift, plus the fixed-point predictor.
    void referenceDecode(const uint8_t block[16], int16_t out[28], int32_t &p1, int32_t &p2)
    {
        static const int32_t f[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        const int shift = block[0] & 0xF;
        const int filter = (block[0] >> 4) & 0xF;
        for (int i = 0; i < 28; ++i)
        {
            const uint8_t byte = block[2 + i / 2];
            const int nibble = (i & 1) ? (byte >> 4) : (byte & 0xF);
            const int32_t raw = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;
            int32_t s = raw + ((f[filter][0] * p1 + f[filter][1] * p2 + 32) >> 6);
            s = s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
            out[i] = static_cast<int16_t>(s);
            p2 = p1;
            p1 = s;
        }
    }

    bool testAdpcmKnownVectors()
    {
        bool ok = true;
        {
            // shift 0, filter 0: nibble n -> n << 12
            uint8_t block[16] = {0x00, 0x00, 0x21, 0xF8, 0x07, 0x00};
            int16_t out[28];
            int32_t p1 = 0, p2 = 0;
            Spu2::decodeAdpcmBlock(block, out, p1, p2);
            ok &= expect(out[0] == 4096 && out[1] == 8192, "ADPCM shift 0: 0x21 -> 4096, 8192");
            ok &= expect(out[2] == -32768 && out[3] == -4096, "ADPCM shift 0: 0xF8 -> -32768, -4096");
            ok &= expect(out[4] == 28672 && out[5] == 0, "ADPCM shift 0: 0x07 -> 28672, 0");
        }
        {
            // filter 1 (60/64 of the previous sample), shift 12, zero nibbles.
            uint8_t block[16] = {0x1C, 0x00};
            int16_t out[28];
            int32_t p1 = 64, p2 = 0;
            Spu2::decodeAdpcmBlock(block, out, p1, p2);
            ok &= expect(out[0] == 60 && out[1] == 56 && out[2] == 53, "ADPCM filter 1 decay 64 -> 60, 56, 53");
            ok &= expect(p1 == out[27] && p2 == out[26], "ADPCM decoder returns the last two samples as history");
        }
        {
            // filter 4 overflow must clamp to 32767
            uint8_t block[16] = {0x40, 0x00, 0x77};
            int16_t out[28];
            int32_t p1 = 32767, p2 = 0;
            Spu2::decodeAdpcmBlock(block, out, p1, p2);
            ok &= expect(out[0] == 32767 && out[1] == 32767, "ADPCM filter 4 clamps to 32767");
        }
        {
            std::mt19937 rng(1234);
            int32_t a1 = 0, a2 = 0, b1 = 0, b2 = 0;
            for (int n = 0; n < 2000 && ok; ++n)
            {
                uint8_t block[16];
                for (auto &b : block)
                    b = static_cast<uint8_t>(rng());
                block[0] = static_cast<uint8_t>((rng() % 5) << 4 | (rng() % 13));
                int16_t got[28], want[28];
                Spu2::decodeAdpcmBlock(block, got, a1, a2);
                referenceDecode(block, want, b1, b2);
                ok &= expect(std::memcmp(got, want, sizeof(got)) == 0, "ADPCM matches the reference formula on random blocks");
            }
        }
        return ok;
    }

    bool testRegisterRoundTrip()
    {
        IopMemory memory;
        uint64_t clock = 0;
        memory.spu2().setClock(&clock);
        bool ok = true;

        // STATX after reset: DMA ready bit
        ok &= expect((memory.read16(kSpu + 0x344) & 0x80) != 0, "STATX bit 7 set after reset");

        // voice 5 on core 1: pitch, ADSR1/2, volume
        const uint32_t v5 = kSpu + kCore1 + 5u * 16u;
        memory.write16(v5 + 0x4, 0x1234);
        memory.write16(v5 + 0x6, 0x80FF);
        memory.write16(v5 + 0x8, 0x1FC0);
        memory.write16(v5 + 0x0, 0x3FFF);
        ok &= expect(memory.read16(v5 + 0x4) == 0x1234, "voice pitch round trip");
        ok &= expect(memory.read16(v5 + 0x6) == 0x80FF, "voice ADSR1 round trip");
        ok &= expect(memory.read16(v5 + 0x8) == 0x1FC0, "voice ADSR2 round trip");
        ok &= expect(memory.read16(v5 + 0x0) == 0x3FFF, "voice VOLL round trip");
        // VOLXL (current volume) follows a non-sweep VOL write (value << 1)
        ok &= expect(memory.read16(v5 + 0xC) == 0x7FFE, "VOLXL reflects the written volume");

        // voice 3 on core 0: SSA hi/lo (hi is 4 bits, lo is 8-aligned)
        const uint32_t va3 = kSpu + 0x1C0u + 3u * 12u;
        memory.write16(va3 + 0x0, 0x0002);
        memory.write16(va3 + 0x2, 0x3457);
        ok &= expect(memory.read16(va3 + 0x0) == 0x0002 && memory.read16(va3 + 0x2) == 0x3450,
                     "SSA round trip with 8-halfword alignment");

        // core registers: TSA, IRQA, MMIX, ATTR, VMIXL (32-bit split), KON storage
        memory.write16(kSpu + 0x1A8, 0x0001);
        memory.write16(kSpu + 0x1AA, 0x2000);
        ok &= expect(memory.read16(kSpu + 0x1A8) == 0x0001 && memory.read16(kSpu + 0x1AA) == 0x2000, "TSA round trip");
        memory.write16(kSpu + kCore1 + 0x19C, 0x000F);
        memory.write16(kSpu + kCore1 + 0x19E, 0xFFF0);
        ok &= expect(memory.read16(kSpu + kCore1 + 0x19C) == 0x000F && memory.read16(kSpu + kCore1 + 0x19E) == 0xFFF0,
                     "IRQA round trip");
        memory.write16(kSpu + 0x198, 0x0FF0);
        ok &= expect(memory.read16(kSpu + 0x198) == 0x0FF0, "MMIX round trip");
        memory.write16(kSpu + 0x19A, 0x8040);
        ok &= expect(memory.read16(kSpu + 0x19A) == 0x8040, "ATTR round trip");
        memory.write32(kSpu + 0x188, 0x00ABCDEFu);
        ok &= expect(memory.read32(kSpu + 0x188) == 0x00ABCDEFu, "VMIXL 32-bit round trip");

        // master volume (core 1 "weird area") and SPDIF
        memory.write16(kSpu + 0x788, 0x3FFF);
        ok &= expect(memory.read16(kSpu + 0x788) == 0x3FFF, "core 1 MVOLL round trip");
        memory.write16(kSpu + 0x7C6, 0x0900);
        ok &= expect(memory.read16(kSpu + 0x7C6) == 0x0900, "SPDIF mode round trip");

        // byte access still reaches the right halfword
        ok &= expect(memory.read8(v5 + 0x4) == 0x34 && memory.read8(v5 + 0x5) == 0x12, "byte reads of a register");

        return ok;
    }

    // Upload `halfwords` from IOP RAM 0x10000 to SPU2 RAM `spuAddr` with DMA4.
    // Like libsd, the transfer uses 16-word (64-byte) blocks, padded with zeros.
    void dmaUpload(IopMemory &memory, uint64_t &clock, uint32_t spuAddr, std::vector<uint16_t> data)
    {
        data.resize((data.size() + 31u) & ~size_t{31u}, 0u);
        (void)memory.writeRam(0x10000u, data.data(), data.size() * 2u);
        memory.write16(kSpu + 0x1A8, static_cast<uint16_t>(spuAddr >> 16));
        memory.write16(kSpu + 0x1AA, static_cast<uint16_t>(spuAddr));
        memory.write16(kSpu + 0x19A, 0x0020); // ATTR: DMA write mode
        const uint32_t words = static_cast<uint32_t>(data.size() / 2u);
        memory.write32(kDma4Madr, 0x10000u);
        memory.write32(kDma4Bcr, ((words / 16u) << 16u) | 16u);
        memory.write32(kDma4Chcr, 0x01000201u);
        for (int i = 0; i < 200 && (memory.read32(kDma4Chcr) & 0x01000000u); ++i)
        {
            clock += 2048;
            memory.spu2().advance();
        }
    }

    bool testDmaUpload()
    {
        IopMemory memory;
        uint64_t clock = 0;
        memory.spu2().setClock(&clock);
        bool ok = true;

        std::vector<uint16_t> data(0x1000);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint16_t>(i * 7u + 3u);
        (void)memory.spu2().takeInterrupts();
        dmaUpload(memory, clock, 0x5000u, data);

        bool same = true;
        for (size_t i = 0; i < data.size(); ++i)
            same &= memory.spu2().readSpuRam(0x5000u + static_cast<uint32_t>(i)) == data[i];
        ok &= expect(same, "DMA4 copies IOP RAM into SPU2 RAM at TSA");
        ok &= expect((memory.read32(kDma4Chcr) & 0x01000000u) == 0, "DMA4 CHCR busy bit clears on completion");
        ok &= expect((memory.spu2().takeInterrupts() & Spu2::InterruptDma4) != 0, "DMA4 completion raises IOP interrupt 0x24");
        ok &= expect(memory.read32(kDma4Madr) == 0x10000u + 0x2000u, "MADR ends at the end of the transfer");
        ok &= expect(memory.read16(kSpu + 0x1AA) == static_cast<uint16_t>(0x5000u + 0x1000u), "TSA advanced past the transfer");

        // Read it back with SPU2 -> IOP DMA.
        (void)memory.zeroRam(0x20000u, 0x2000u);
        memory.write16(kSpu + 0x1A8, 0);
        memory.write16(kSpu + 0x1AA, 0x5000);
        memory.write16(kSpu + 0x19A, 0x0030); // DMA read mode
        memory.write32(kDma4Madr, 0x20000u);
        memory.write32(kDma4Bcr, ((0x800u / 16u) << 16u) | 16u);
        memory.write32(kDma4Chcr, 0x01000200u);
        for (int i = 0; i < 400 && (memory.read32(kDma4Chcr) & 0x01000000u); ++i)
        {
            clock += 2048;
            memory.spu2().advance();
        }
        std::vector<uint16_t> back(0x1000);
        (void)memory.readRam(0x20000u, back.data(), back.size() * 2u);
        ok &= expect(back == data, "DMA4 read copies SPU2 RAM back into IOP RAM");
        return ok;
    }

    // ADPCM block: header, flags, 14 data bytes.
    void appendBlock(std::vector<uint16_t> &out, uint8_t shiftFilter, uint8_t flags, uint8_t pattern)
    {
        uint8_t block[16] = {shiftFilter, flags};
        for (int i = 2; i < 16; ++i)
            block[i] = pattern;
        for (int i = 0; i < 16; i += 2)
            out.push_back(static_cast<uint16_t>(block[i] | (block[i + 1] << 8)));
    }

    double rms(const std::vector<int16_t> &frames)
    {
        double sum = 0;
        for (int16_t s : frames)
            sum += static_cast<double>(s) * s;
        return frames.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(frames.size()));
    }

    bool testVoiceKeyOnProducesSound()
    {
        IopMemory memory;
        uint64_t clock = 0;
        Spu2 &spu2 = memory.spu2();
        spu2.setClock(&clock);
        std::vector<int16_t> output;
        spu2.setOutput([&](const int16_t *frames, size_t count)
                       { output.insert(output.end(), frames, frames + count * 2); });
        bool ok = true;

        // A looping square-ish wave: +0x7 / -0x8 nibbles at shift 0.
        std::vector<uint16_t> wave;
        appendBlock(wave, 0x00, 0x04, 0x77); // loop start
        appendBlock(wave, 0x00, 0x02, 0x88);
        appendBlock(wave, 0x00, 0x03, 0x77); // loop end + repeat
        dmaUpload(memory, clock, 0x6000u, wave);

        // Core 1 must pass core 0 through (MMIX ext bits), set master volumes.
        memory.write16(kSpu + 0x198, 0x0FF0);
        memory.write16(kSpu + kCore1 + 0x198, 0x0FFC);
        memory.write16(kSpu + 0x760, 0x3FFF);
        memory.write16(kSpu + 0x762, 0x3FFF);
        memory.write16(kSpu + 0x788, 0x3FFF);
        memory.write16(kSpu + 0x78A, 0x3FFF);

        const uint32_t v0 = kSpu;
        memory.write16(v0 + 0x0, 0x3FFF);              // VOLL
        memory.write16(v0 + 0x2, 0x3FFF);              // VOLR
        memory.write16(v0 + 0x4, 0x1000);              // pitch 1.0 (48 kHz)
        memory.write16(v0 + 0x6, 0x000F);              // ADSR1: fastest attack, SL 15
        memory.write16(v0 + 0x8, 0x0000);              // ADSR2: hold sustain
        memory.write16(kSpu + 0x1C0, 0x0000);          // SSA hi
        memory.write16(kSpu + 0x1C2, 0x6000);          // SSA lo

        clock += 768 * 100;
        spu2.advance();
        const size_t silentFrames = output.size();
        const double silentRms = rms(output);
        ok &= expect(silentRms < 1.0, "no output before key on");

        memory.write16(kSpu + 0x1A0, 0x0001); // KON voice 0
        clock += 768 * 4800;                  // 100 ms
        spu2.advance();
        spu2.flushOutput();

        ok &= expect((spu2.activeVoiceMask(0) & 1u) != 0, "voice 0 is playing after KON");
        std::vector<int16_t> tail(output.begin() + static_cast<std::ptrdiff_t>(silentFrames), output.end());
        const double level = rms(tail);
        ok &= expect(tail.size() >= 2u * 4800u, "SPU2 produced 48 kHz frames for the elapsed time");
        ok &= expect(level > 1000.0, "voice key-on produces non-silent output");
        ok &= expect(spu2.samplesMixed() >= 4900u, "samples mixed follow IOP cycles / 768");

        // Key off with the fastest release: output decays to (DC-filtered) silence.
        memory.write16(v0 + 0x8, 0x0000);
        memory.write16(kSpu + 0x1A4, 0x0001); // KOFF
        output.clear();
        clock += 768 * 4800;
        spu2.advance();
        spu2.flushOutput();
        std::vector<int16_t> last(output.end() - 2 * 1000, output.end());
        ok &= expect(rms(last) < 50.0, "voice is silent after key off");
        ok &= expect((spu2.activeVoiceMask(0) & 1u) == 0, "voice 0 stopped after release");
        return ok;
    }

    bool testIrqOnVoiceAddress()
    {
        IopMemory memory;
        uint64_t clock = 0;
        Spu2 &spu2 = memory.spu2();
        spu2.setClock(&clock);
        bool ok = true;

        std::vector<uint16_t> wave;
        appendBlock(wave, 0x00, 0x00, 0x11);
        appendBlock(wave, 0x00, 0x01, 0x11); // end, no loop
        dmaUpload(memory, clock, 0x7000u, wave);
        (void)spu2.takeInterrupts();

        memory.write16(kSpu + 0x19C, 0x0000); // IRQA = 0x7008 (second block)
        memory.write16(kSpu + 0x19E, 0x7008);
        memory.write16(kSpu + 0x19A, 0x0040); // ATTR: IRQ enable
        memory.write16(kSpu + 0x4, 0x1000);
        memory.write16(kSpu + 0x6, 0x000F);
        memory.write16(kSpu + 0x1C2, 0x7000);
        memory.write16(kSpu + 0x1A0, 0x0001);
        clock += 768 * 200;
        spu2.advance();
        ok &= expect((spu2.takeInterrupts() & Spu2::InterruptSpu2) != 0, "voice reaching IRQA raises the SPU2 interrupt");
        ok &= expect((memory.read16(kSpu + 0x7C2) & 0x4) != 0, "SPDIF IRQINFO flags core 0");
        ok &= expect((memory.read32(kSpu + 0x340) & 1u) != 0, "ENDX set for the voice after its end block");

        // Disabling the IRQ in ATTR acknowledges it.
        memory.write16(kSpu + 0x19A, 0x0000);
        ok &= expect((memory.read16(kSpu + 0x7C2) & 0x4) == 0, "clearing ATTR IRQ enable clears IRQINFO");
        return ok;
    }

    bool testAdmaStreaming()
    {
        IopMemory memory;
        uint64_t clock = 0;
        Spu2 &spu2 = memory.spu2();
        spu2.setClock(&clock);
        std::vector<int16_t> output;
        spu2.setOutput([&](const int16_t *frames, size_t count)
                       { output.insert(output.end(), frames, frames + count * 2); });
        bool ok = true;

        // Route core 0 sound data input to the output, then core 0 through core 1.
        memory.write16(kSpu + 0x198, 0x00F0);
        memory.write16(kSpu + kCore1 + 0x198, 0x000C);
        memory.write16(kSpu + 0x760, 0x3FFF);
        memory.write16(kSpu + 0x762, 0x3FFF);
        memory.write16(kSpu + 0x788, 0x3FFF);
        memory.write16(kSpu + 0x78A, 0x3FFF);
        memory.write16(kSpu + 0x76C, 0x7FFF); // BVOLL core 0
        memory.write16(kSpu + 0x76E, 0x7FFF);

        // 4 blocks of 512 bytes: L(256 samples) then R(256 samples), a 750 Hz square wave.
        std::vector<uint16_t> pcm;
        for (int block = 0; block < 4; ++block)
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    pcm.push_back(static_cast<uint16_t>(((i / 32) & 1) ? 12000 : -12000));
        (void)memory.writeRam(0x30000u, pcm.data(), pcm.size() * 2u);
        (void)spu2.takeInterrupts();

        memory.write16(kSpu + 0x1B0, 0x0001); // ADMAS core 0
        memory.write16(kSpu + 0x19A, 0x0020);
        memory.write32(kDma4Madr, 0x30000u);
        memory.write32(kDma4Bcr, ((static_cast<uint32_t>(pcm.size()) / 2u / 16u) << 16u) | 16u);
        memory.write32(kDma4Chcr, 0x01000201u);

        bool irq = false;
        for (int i = 0; i < 2000 && !irq; ++i)
        {
            clock += 768;
            spu2.advance();
            irq = (spu2.takeInterrupts() & Spu2::InterruptDma4) != 0;
        }
        spu2.flushOutput();
        ok &= expect(irq, "ADMA transfer completes with DMA4 interrupt once consumed");
        ok &= expect(rms(output) > 3000.0, "ADMA input is audible in the mix");
        return ok;
    }
}

int main()
{
    bool ok = true;
    ok &= testAdpcmKnownVectors();
    ok &= testRegisterRoundTrip();
    ok &= testDmaUpload();
    ok &= testVoiceKeyOnProducesSound();
    ok &= testIrqOnVoiceAddress();
    ok &= testAdmaStreaming();
    if (!ok)
        return 1;
    std::cout << "ps2_iop_spu2_tests: all passed\n";
    return 0;
}
