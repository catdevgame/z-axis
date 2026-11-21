// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Ported from PCSX2 pcsx2/SPU2/Dma.cpp, the DMA4/7 callbacks in spu2.cpp and
// the SPU2 part of pcsx2/IopDma.cpp (commit 091ab687).
//
// Changes: DMA logs are dropped; DMAPtr/DMARPtr are IOP RAM byte addresses
// (copies wrap at 2 MiB instead of trusting host pointers); the IOP DMA
// channel registers (HW_DMA4/7_MADR, BCR, CHCR, TADR) live in Spu2System and
// completion raises IOP interrupt 0x24/0x28 through PendingIrqs.

#include "spu2_internal.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

namespace zx::spu2
{
    static constexpr int CYCLES_PER_WORD = 24;

    u32 &V_Core::MADR()
    {
        return Sys->Dma[Index].MADR;
    }

    u32 &V_Core::TADR()
    {
        return Sys->Dma[Index].TADR;
    }

    u16 V_Core::DmaRead()
    {
        const u16 ret = static_cast<u16>(Sys->spu2M_Read(ActiveTSA));
        ++ActiveTSA;
        ActiveTSA &= 0xfffff;
        TSA = ActiveTSA;
        return ret;
    }

    void V_Core::DmaWrite(u16 value)
    {
        Sys->spu2M_Write(ActiveTSA, value);
        ++ActiveTSA;
        ActiveTSA &= 0xfffff;
        TSA = ActiveTSA;
    }

    void Spu2System::CopyFromIop(u32 iopAddr, s16 *dst, u32 halfwords) const
    {
        u8 *out = reinterpret_cast<u8 *>(dst);
        const size_t bytes = static_cast<size_t>(halfwords) * 2u;
        u32 src = iopAddr & IopRamMask;
        size_t done = 0;
        while (done < bytes)
        {
            const size_t chunk = std::min(bytes - done, static_cast<size_t>(IopRam.size() - src));
            std::memcpy(out + done, IopRam.data() + src, chunk);
            done += chunk;
            src = 0;
        }
    }

    void Spu2System::CopyToIop(u32 iopAddr, const s16 *src, u32 halfwords)
    {
        const u8 *in = reinterpret_cast<const u8 *>(src);
        const size_t bytes = static_cast<size_t>(halfwords) * 2u;
        u32 dst = iopAddr & IopRamMask;
        size_t done = 0;
        while (done < bytes)
        {
            const size_t chunk = std::min(bytes - done, static_cast<size_t>(IopRam.size() - dst));
            std::memcpy(IopRam.data() + dst, in + done, chunk);
            done += chunk;
            dst = 0;
        }
    }

    void V_Core::AutoDMAReadBuffer(int mode) // mode: 0= split stereo; 1 = do not split stereo
    {
        u32 spos = InputPosWrite & 0x100; // Starting position passed by TSA
        bool leftbuffer = !(InputPosWrite & 0x80);

        if (InputPosWrite == 0xFFFF) // Data request not made yet
            return;

        AutoDMACtrl &= 0x3;

        int size = static_cast<int>(std::min(InputDataLeft, static_cast<u32>(0x200)));
        if (!leftbuffer)
            size = std::min(size, 0x100);

        if (DMAPtr == kNoDmaPtr)
        {
            DMAPtr = MADR();
            InputDataProgress = 0;
        }

        if (mode)
        {
            // size is in bytes here, as in PCSX2 (memcpy of 'size' bytes)
            Sys->CopyFromIop(DMAPtr + InputDataProgress * 2, Sys->GetMemPtr(0x2000 + (Index << 10) + spos), static_cast<u32>(size) / 2);
            MADR() += size;
            InputDataLeft -= 0x200;
            InputDataProgress += 0x200;
        }
        else
        {
            while (size)
            {
                if (!leftbuffer)
                    spos |= 0x200;
                else
                    spos &= ~0x200u;

                Sys->CopyFromIop(DMAPtr + InputDataProgress * 2, Sys->GetMemPtr(0x2000 + (Index << 10) + spos), 0x100);
                if (Sys->AdmaDump)
                    std::fwrite(Sys->GetMemPtr(0x2000 + (Index << 10) + spos), 2, 0x100, Sys->AdmaDump);
                InputDataTransferred += 0x200;
                InputDataLeft -= 0x100;
                InputDataProgress += 0x100;
                leftbuffer = !leftbuffer;
                size -= 0x100;
                InputPosWrite += 0x80;
            }
        }
        if (!(InputPosWrite & 0x80))
            InputPosWrite = 0xFFFF;
    }

    void V_Core::StartADMAWrite(u32 sz)
    {
        const u32 size = sz;

        Sys->TimeUpdate(Sys->Now());

        InputDataProgress = 0;
        TADR() = MADR() + (size << 1);
        if ((AutoDMACtrl & (Index + 1)) == 0)
        {
            ActiveTSA = 0x2000 + (Index << 10);
            DMAICounter = static_cast<s32>(size * CYCLES_PER_WORD);
            LastClock = Sys->Now();
        }
        else if (size >= 256)
        {
            InputDataLeft = size;
            if (InputPosWrite != 0xFFFF)
            {
                AutoDMAReadBuffer(0);
            }
            AdmaInProgress = true;
        }
        else
        {
            InputDataLeft = 0;
            DMAICounter = static_cast<s32>(size * CYCLES_PER_WORD);
            LastClock = Sys->Now();
        }
    }

    void V_Core::PlainDMAWrite(u32 size)
    {
        Sys->TimeUpdate(Sys->Now());

        ReadSize = size;
        IsDMARead = false;
        DMAICounter = 0;
        LastClock = Sys->Now();
        Regs.STATX &= ~0x80;
        Regs.STATX |= 0x400;
        TADR() = MADR() + (size << 1);

        FinishDMAwrite();
    }

    void V_Core::FinishDMAwrite()
    {
        if (DMAPtr == kNoDmaPtr)
        {
            DMAPtr = MADR();
        }

        DMAICounter = static_cast<s32>(ReadSize);

        u32 buff1end = ActiveTSA + std::min(ReadSize, static_cast<u32>(0x100) + static_cast<u32>(std::abs(DMAICounter / CYCLES_PER_WORD)));
        u32 buff2end = 0;
        if (buff1end > 0x100000)
        {
            buff2end = buff1end - 0x100000;
            buff1end = 0x100000;
        }

        const int cacheIdxStart = static_cast<int>(ActiveTSA / pcm_WordsPerBlock);
        const int cacheIdxEnd = static_cast<int>((buff1end + pcm_WordsPerBlock - 1) / pcm_WordsPerBlock);
        for (int idx = cacheIdxStart; idx < cacheIdxEnd && idx < pcm_BlockCount; ++idx)
            Sys->pcm_cache[static_cast<size_t>(idx)].Validated = false;

        // Write the first part of the buffer (up to the end of SPU2 RAM)
        const u32 buff1size = (buff1end - ActiveTSA);
        Sys->CopyFromIop(DMAPtr, Sys->GetMemPtr(ActiveTSA), buff1size);

        u32 TDA;

        if (buff2end > 0)
        {
            // second branch needs copied: wrap to the start of SPU2 RAM
            const u32 start = ActiveTSA;
            TDA = buff1end;

            DMAPtr += (TDA - ActiveTSA) * 2;
            ReadSize -= TDA - ActiveTSA;
            ActiveTSA = 0;
            for (int idx = 0; idx < static_cast<int>((buff2end + pcm_WordsPerBlock - 1) / pcm_WordsPerBlock) && idx < pcm_BlockCount; ++idx)
                Sys->pcm_cache[static_cast<size_t>(idx)].Validated = false;
            Sys->CopyFromIop(DMAPtr, Sys->GetMemPtr(0), buff2end);
            TDA = (buff2end) & 0xfffff;

            // Flag interrupt?  If IRQA occurs between start and dest, flag it.
            // Important: Test both core IRQ settings for either DMA!
            for (int i = 0; i < 2; i++)
            {
                if (Sys->Cores[i].IRQEnable && (Sys->Cores[i].IRQA > start || Sys->Cores[i].IRQA < TDA))
                {
                    Sys->SetIrqCallDMA(i);
                }
            }
        }
        else
        {
            // Buffer doesn't wrap/overflow!
            TDA = buff1end;

            for (int i = 0; i < 2; i++)
            {
                if (Sys->Cores[i].IRQEnable && (Sys->Cores[i].IRQA > ActiveTSA && Sys->Cores[i].IRQA < TDA))
                {
                    Sys->SetIrqCallDMA(i);
                }
            }
        }

        DMAPtr += (TDA - ActiveTSA) * 2;
        ReadSize -= TDA - ActiveTSA;

        DMAICounter = (DMAICounter - static_cast<s32>(ReadSize)) * CYCLES_PER_WORD;

        ActiveTSA = TDA;
        ActiveTSA &= 0xfffff;
        TSA = ActiveTSA;
    }

    void V_Core::FinishDMAread()
    {
        u32 buff1end = ActiveTSA + std::min(ReadSize, static_cast<u32>(0x100) + static_cast<u32>(std::abs(DMAICounter / CYCLES_PER_WORD)));
        u32 buff2end = 0;

        if (buff1end > 0x100000)
        {
            buff2end = buff1end - 0x100000;
            buff1end = 0x100000;
        }

        if (DMAPtr == kNoDmaPtr)
        {
            DMAPtr = MADR();
        }

        const u32 buff1size = (buff1end - ActiveTSA);
        Sys->CopyToIop(DMARPtr, Sys->GetMemPtr(ActiveTSA), buff1size);
        u32 TDA;

        if (buff2end > 0)
        {
            const u32 start = ActiveTSA;
            TDA = buff1end;

            DMARPtr += (TDA - ActiveTSA) * 2;
            ReadSize -= TDA - ActiveTSA;
            ActiveTSA = 0;

            Sys->CopyToIop(DMARPtr, Sys->GetMemPtr(0), buff2end);

            TDA = (buff2end) & 0xfffff;

            for (int i = 0; i < 2; i++)
            {
                if (Sys->Cores[i].IRQEnable && (Sys->Cores[i].IRQA > start || Sys->Cores[i].IRQA < TDA))
                {
                    Sys->SetIrqCallDMA(i);
                }
            }
        }
        else
        {
            TDA = buff1end;

            for (int i = 0; i < 2; i++)
            {
                if (Sys->Cores[i].IRQEnable && (Sys->Cores[i].IRQA > ActiveTSA && Sys->Cores[i].IRQA < TDA))
                {
                    Sys->SetIrqCallDMA(i);
                }
            }
        }

        DMARPtr += (TDA - ActiveTSA) * 2;
        ReadSize -= TDA - ActiveTSA;

        if (ReadSize)
            DMAICounter = static_cast<s32>(std::min(ReadSize, static_cast<u32>(0x100)) * CYCLES_PER_WORD);
        else
            DMAICounter = CYCLES_PER_WORD;

        ActiveTSA = TDA;
        ActiveTSA &= 0xfffff;
        TSA = ActiveTSA;
    }

    void V_Core::DoDMAread(u32 iopAddr, u32 size)
    {
        Sys->TimeUpdate(Sys->Now());

        DMARPtr = iopAddr;
        ActiveTSA = TSA & 0xfffff;
        ReadSize = size;
        IsDMARead = true;
        LastClock = Sys->Now();
        DMAICounter = static_cast<s32>(std::min(ReadSize, static_cast<u32>(0x100)) * CYCLES_PER_WORD);
        Regs.STATX &= ~0x80;
        Regs.STATX |= 0x400;
        TADR() = MADR() + (size << 1);
    }

    void V_Core::DoDMAwrite(u32 iopAddr, u32 size)
    {
        DMAPtr = iopAddr;

        if (size < 2)
        {
            Regs.STATX &= ~0x80;
            DMAICounter = 1 * CYCLES_PER_WORD;
            LastClock = Sys->Now();
            return;
        }

        ActiveTSA = TSA & 0xfffff;

        const bool adma_enable = ((AutoDMACtrl & (Index + 1)) == (Index + 1));

        if (adma_enable)
        {
            StartADMAWrite(size);
        }
        else
        {
            PlainDMAWrite(size);
            Regs.STATX &= ~0x80;
            Regs.STATX |= 0x400;
        }
    }

    // ------------------------------------------------------------------
    //  IOP side (PCSX2 IopDma.cpp + SPU2interruptDMA4/7 from spu2.cpp)
    // ------------------------------------------------------------------

    void Spu2System::InterruptDMA(int core)
    {
        if (Cores[core].DmaMode)
            Cores[core].Regs.STATX |= 0x80;
        Cores[core].Regs.STATX &= ~0x400;
        Cores[core].TSA = Cores[core].ActiveTSA;
    }

    void Spu2System::TraceEvent(const char *fmt, ...)
    {
        if (TraceBudget <= 0 || !Trace)
            return;
        --TraceBudget;
        char text[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        char line[300];
        // Prefix with emulated time in seconds (samples mixed / 48 kHz).
        std::snprintf(line, sizeof(line), "%s @%.3f", text, static_cast<double>(SamplesMixed) / 48000.0);
        Trace(line);
    }

    void Spu2System::spu2DMA4Irq()
    {
        TraceEvent("[spu2] DMA4 done madr=%06x tsa=%05x", Dma[0].MADR, Cores[0].TSA);
        InterruptDMA(0);
        if (Dma[0].CHCR & 0x01000000)
        {
            Dma[0].CHCR &= ~0x01000000u;
            PendingIrqs |= 2u; // IOP interrupt 0x24 (DMA4)
        }
    }

    void Spu2System::spu2DMA7Irq()
    {
        TraceEvent("[spu2] DMA7 done madr=%06x tsa=%05x", Dma[1].MADR, Cores[1].TSA);
        InterruptDMA(1);
        if (Dma[1].CHCR & 0x01000000)
        {
            Dma[1].CHCR &= ~0x01000000u;
            PendingIrqs |= 4u; // IOP interrupt 0x28 (DMA7)
        }
    }

    void Spu2System::spu2Irq()
    {
        TraceEvent("[spu2] IRQ info=%04x irqa0=%05x irqa1=%05x", Spdif.Info, Cores[0].IRQA, Cores[1].IRQA);
        PendingIrqs |= 1u; // IOP interrupt 9 (SPU2)
    }

    // psxDmaGeneric(): BCR = block count (high) x block size in words (low).
    void Spu2System::StartDma(int core)
    {
        IopDmaChannel &ch = Dma[core];
        const u32 size = (ch.BCR >> 16) * (ch.BCR & 0xFFFF);
        const u32 madr = ch.MADR & IopRamMask;

        TimeUpdate(Now());
        TraceEvent("[spu2] DMA%d start chcr=%08x madr=%06x bcr=%08x tsa=%05x admas=%x", core ? 7 : 4, ch.CHCR, madr, ch.BCR,
                   Cores[core].TSA, Cores[core].AutoDMACtrl);

        if (ch.CHCR & 1u)
        {
            // IOP RAM -> SPU2 (PCSX2 handles CHCR 0x01000201)
            Cores[core].DoDMAwrite(madr, size * 2);
        }
        else
        {
            // SPU2 -> IOP RAM (CHCR 0x01000200)
            Cores[core].DoDMAread(madr, size * 2);
        }
    }
}
