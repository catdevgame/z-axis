// SPDX-License-Identifier: GPL-3.0+
//
// The SPU2, the PS2 sound processor the IOP emulator (iop/) drives, ported from
// PCSX2's SPU2 (ps2/spu2/README.md). This header is the only interface the IOP
// uses:
//  - 16-bit register access for 0x1F900000..0x1F9007FF (higher offsets inside
//    0x1F900000..0x1F90FFFF reach the raw register file as in PCSX2),
//  - the IOP DMA channel 4/7 registers (0x1F8010C0..CF, 0x1F801500..0F),
//  - time advance in IOP cycles (one 48 kHz sample per 768 cycles),
//  - interrupts raised for the IOP (9 = SPU2, 0x24 = DMA4, 0x28 = DMA7),
//  - mixed output as interleaved signed 16-bit stereo frames.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace zx
{
    namespace spu2
    {
        struct Spu2System;
    }

    class Spu2
    {
    public:
        static constexpr uint32_t RegisterBase = 0x1F900000u;
        static constexpr uint32_t RegisterEnd = 0x1F910000u;
        static constexpr uint32_t SampleRate = 48000u;
        static constexpr uint32_t CyclesPerSample = 768u;

        static constexpr int IrqSpu2 = 9;
        static constexpr int IrqDma4 = 0x24;
        static constexpr int IrqDma7 = 0x28;

        static constexpr uint32_t InterruptSpu2 = 1u;
        static constexpr uint32_t InterruptDma4 = 2u;
        static constexpr uint32_t InterruptDma7 = 4u;

        using OutputCallback = std::function<void(const int16_t *interleavedStereo, size_t frames)>;

        explicit Spu2(std::span<uint8_t> iopRam);
        ~Spu2();

        Spu2(const Spu2 &) = delete;
        Spu2 &operator=(const Spu2 &) = delete;

        void reset();

        // The IOP cycle counter the SPU2 follows. Register and DMA accesses
        // first bring the SPU2 up to this time, as PCSX2 does with TimeUpdate().
        void setClock(const uint64_t *iopCycles);
        void setOutput(OutputCallback output);
        // Event trace sink; ZX_TRACE_SPU2=<n> enables the first n events
        // (register writes, DMA starts/completions, IRQs).
        void setTrace(std::function<void(const char *)> trace);

        [[nodiscard]] static bool isRegisterAddress(uint32_t physical) noexcept
        {
            return physical >= RegisterBase && physical < RegisterEnd;
        }
        [[nodiscard]] static bool isDmaRegisterAddress(uint32_t physical) noexcept
        {
            return (physical >= 0x1F8010C0u && physical < 0x1F8010D0u) ||
                   (physical >= 0x1F801500u && physical < 0x1F801510u);
        }

        [[nodiscard]] uint16_t readRegister(uint32_t physical);
        void writeRegister(uint32_t physical, uint16_t value);

        // 32-bit aligned access to MADR/BCR/CHCR/TADR of DMA4 and DMA7.
        // Writing CHCR with bit 24 set starts the transfer.
        [[nodiscard]] uint32_t readDmaRegister(uint32_t physical);
        void writeDmaRegister(uint32_t physical, uint32_t value);

        // Advance to the clock (or to an explicit cycle).
        void advance();
        void advanceTo(uint64_t iopCycle);
        // IOP cycle by which advance() must next be called so samples, DMA
        // progress and interrupts are not late.
        [[nodiscard]] uint64_t nextEventCycle() const noexcept;
        // A DMA4/DMA7 transfer is in progress.
        [[nodiscard]] bool dmaActive() const noexcept;
        // Completes the running non-auto DMA4/DMA7 transfers at once (the
        // data lands, the completion interrupt is raised) without mixing
        // ahead of the clock. Returns true when one was running.
        bool finishDma();

        // Bitmask of InterruptSpu2/Dma4/Dma7 raised since the last call.
        [[nodiscard]] uint32_t takeInterrupts() noexcept;
        [[nodiscard]] bool hasPendingInterrupts() const noexcept;

        // Push any buffered output frames to the output callback.
        void flushOutput();

        // Diagnostics and tests.
        [[nodiscard]] uint64_t samplesMixed() const noexcept;
        [[nodiscard]] uint16_t readSpuRam(uint32_t halfwordAddress) const noexcept;
        void writeSpuRam(uint32_t halfwordAddress, uint16_t value) noexcept;
        [[nodiscard]] uint32_t activeVoiceMask(int core) const noexcept;
        [[nodiscard]] spu2::Spu2System &system() noexcept { return *m_system; }

        // Decodes one 16-byte SPU2 ADPCM block into 28 samples (exposed for tests).
        static void decodeAdpcmBlock(const uint8_t block[16], int16_t out[28], int32_t &prev1, int32_t &prev2);

    private:
        std::unique_ptr<spu2::Spu2System> m_system;
    };
}
