// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Ported from PCSX2 pcsx2/SPU2/spu2sys.cpp, RegTable.cpp and the register
// access / reset parts of spu2.cpp (commit 091ab687).
//
// Changes: state is per Spu2System instance; the 0x401-entry template table of
// register write handlers is replaced by an address decoder that calls the same
// handler bodies; the PS1-mode register file (WriteRegPS1/ReadRegPS1) and the
// "ps1driver" ADMAS hack are dropped; CounterUpdate (psxCounters[6]) is
// replaced by Spu2::nextEventCycle(), which the IOP scheduler honours.

#include "spu2_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2x::iop::detail::spu2
{
    Spu2System::Spu2System(std::span<u8> iopRam)
        : spu2regs(0x10000 / sizeof(s16)),
          spu2mem(0x200000 / sizeof(s16)),
          pcm_cache(pcm_BlockCount),
          IopRam(iopRam)
    {
        Cores[0].Sys = this;
        Cores[1].Sys = this;
        if (const char *dump = std::getenv("PS2X_SPU2_ADMA_DUMP"); dump && *dump)
            AdmaDump = std::fopen(dump, "wb");
        if (const char *dump = std::getenv("PS2X_SPU2_RAM_DUMP"); dump && *dump)
            RamDumpPrefix = dump;
        BuildRegTable();
        Reset();
    }

    // SPU2::InternalReset(false)
    void Spu2System::Reset()
    {
        std::fill(spu2regs.begin(), spu2regs.end(), s16{0});
        std::fill(spu2mem.begin(), spu2mem.end(), s16{0});
        // from BIOS reversal. Locks the voices so they don't run free.
        std::memset(spu2mem.data() + 0x2800, 7, 0x10);
        // Loop which gets left over by the BIOS, Megaman X7 relies on it being there.
        std::memset(spu2mem.data() + 0xe870, 7, 0x10);
        for (PcmCacheEntry &entry : pcm_cache)
            entry.Validated = false;

        DCFilterIn[0] = DCFilterIn[1] = 0.f;
        DCFilterOut[0] = DCFilterOut[1] = 0.f;

        Spdif = V_SPDIF{}; // Reset IRQ Status if it got set in a previously run game
        PlayMode = 0;
        Cycles = 0;
        has_to_call_irq_dma[0] = has_to_call_irq_dma[1] = false;
        Dma[0] = IopDmaChannel{};
        Dma[1] = IopDmaChannel{};
        PendingIrqs = 0;
        SamplesMixed = 0;
        OutBufFrames = 0;
        lClocks = Now();

        Cores[0].Init(0);
        Cores[1].Init(1);
    }

    void V_Core::Init(int index)
    {
        Mute = false;
        DMABits = 0;
        NoiseClk = 0;
        NoiseCnt = 0;
        NoiseOut = 0;
        AutoDMACtrl = 0;
        InputDataLeft = 0;
        InputPosWrite = 0x100;
        InputDataProgress = 0;
        InputDataTransferred = 0;
        AttrBit0 = 0;
        DmaMode = 0;
        DMAPtr = kNoDmaPtr;
        DMARPtr = kNoDmaPtr;
        ReadSize = 0;
        IsDMARead = false;
        KeyOn = 0;
        KeyOff = 0;
        Sys->OutPos = 0;

        const int c = static_cast<int>(Index = static_cast<u32>(index));

        Regs = V_CoreRegs{};
        Regs.STATX = 0;
        Regs.ATTR = 0;
        ExtVol = V_VolumeLR(0x7FFF);
        InpVol = V_VolumeLR(0x7FFF);
        FxVol = V_VolumeLR(0);

        MasterVol = V_VolumeSlideLR(0, 0);

        std::memset(&DryGate, -1, sizeof(DryGate));
        std::memset(&WetGate, -1, sizeof(WetGate));
        DryGate.ExtL = 0;
        DryGate.ExtR = 0;
        if (!c)
        {
            WetGate.ExtL = 0;
            WetGate.ExtR = 0;
        }

        Regs.MMIX = c ? 0xFFC : 0xFF0; // PS2 confirmed (f3c and f30 after BIOS ran, ffc and ff0 after sdinit)
        Regs.VMIXL = 0xFFFFFF;
        Regs.VMIXR = 0xFFFFFF;
        Regs.VMIXEL = 0xFFFFFF;
        Regs.VMIXER = 0xFFFFFF;
        EffectsStartA = c ? 0xFFFF8 : 0xEFFF8;
        EffectsEndA = c ? 0xFFFFF : 0xEFFFF;
        Revb = V_Reverb{};

        FxEnable = false;
        // F1 2005 is known to rely on an uninitialised IRQA being an address which will be hit.
        IRQA = 0x800;
        IRQEnable = false; // PS2 confirmed
        TSA = 0;
        ActiveTSA = 0;

        for (uint v = 0; v < NumVoices; ++v)
        {
            VoiceGates[v].DryL = -1;
            VoiceGates[v].DryR = -1;
            VoiceGates[v].WetL = -1;
            VoiceGates[v].WetR = -1;

            Voices[v] = V_Voice{};
            Voices[v].Volume = V_VolumeSlideLR(0, 0);

            Voices[v].ADSR.Counter = 0;
            Voices[v].ADSR.Value = 0;
            Voices[v].ADSR.Phase = 0;
            Voices[v].ADSR.UpdateCache();
            Voices[v].Pitch = 0x3FFF;
            Voices[v].NextA = 0x2801;
            Voices[v].StartA = 0x2800;
            Voices[v].LoopStartA = 0x2800;

            std::memset(Voices[v].DecodeFifo, 0, sizeof(Voices[v].DecodeFifo));
            Voices[v].DecPosRead = 0;
            Voices[v].DecPosWrite = 0;
        }

        DMAICounter = 0;
        AdmaInProgress = false;

        Regs.STATX = 0x80;
        Regs.ENDX = 0xffffff; // PS2 confirmed

        RevbSampleBufPos = 0;
        std::memset(RevbDownBuf, 0, sizeof(RevbDownBuf));
        std::memset(RevbUpBuf, 0, sizeof(RevbUpBuf));
    }

    void Spu2System::DumpState()
    {
        if (!RamDumpPrefix)
            return;
        std::string base(RamDumpPrefix);
        if (std::FILE *f = std::fopen((base + ".ram").c_str(), "wb"))
        {
            std::fwrite(spu2mem.data(), sizeof(s16), spu2mem.size(), f);
            std::fclose(f);
        }
        if (std::FILE *f = std::fopen((base + ".voices").c_str(), "w"))
        {
            std::fprintf(f, "t=%.3f\n", static_cast<double>(SamplesMixed) / 48000.0);
            for (int c = 0; c < 2; ++c)
            {
                for (uint v = 0; v < V_Core::NumVoices; ++v)
                {
                    const V_Voice &vc = Cores[c].Voices[v];
                    std::fprintf(f, "c%d v%02u phase=%d env=%05x volL=%05x volR=%05x pitch=%04x ssa=%05x lsa=%05x nax=%05x adsr=%08x\n",
                                 c, v, vc.ADSR.Phase, vc.ADSR.Value & 0xFFFFF, vc.Volume.Left.Value & 0xFFFFF,
                                 vc.Volume.Right.Value & 0xFFFFF, vc.Pitch, vc.StartA, vc.LoopStartA, vc.NextA, vc.ADSR.reg32);
                }
            }
            std::fclose(f);
        }
    }

    void V_Voice::Start()
    {
        if (StartA & 7)
        {
            StartA = (StartA + 0xFFFF8) + 0x8;
        }

        ADSR.Attack();
        LoopMode = 0;

        SP = 0;

        LoopFlags = 0;
        NextA = StartA | 1;
        Prev1 = 0;
        Prev2 = 0;

        SBuffer = nullptr;
        DecPosRead = 0;
        DecPosWrite = 0;
    }

    void V_Voice::Stop()
    {
        ADSR.Value = 0;
        ADSR.Phase = V_ADSR::PHASE_STOPPED;
    }

    void Spu2System::SetIrqCall(int core)
    {
        if (!(Spdif.Info & (4 << core)) && Cores[core].IRQEnable)
        {
            Spdif.Info |= static_cast<u16>(4 << core);
            spu2Irq();
        }
    }

    void Spu2System::SetIrqCallDMA(int core)
    {
        // reset by an irq disable/enable cycle, behaviour found by
        // test programs that bizarrely only fired one interrupt
        has_to_call_irq_dma[core] = true;
    }

    // writes a signed value to the SPU2 ram
    // Invalidates the ADPCM cache in the process.
    void Spu2System::spu2M_Write(u32 addr, s16 value)
    {
        // Make sure the cache is invalidated:
        // (note to self : addr address WORDs, not bytes)
        addr &= 0xfffff;
        if (addr >= static_cast<u32>(SPU2_DYN_MEMLINE))
        {
            const int cacheIdx = static_cast<int>(addr / pcm_WordsPerBlock);
            pcm_cache[static_cast<size_t>(cacheIdx)].Validated = false;
        }
        *GetMemPtr(addr) = value;
    }

    void Spu2System::CheckDMAProgress(int cid)
    {
        V_Core &core = Cores[cid];
        const int adma_cbit = 1 << cid;
        const u64 now = Now();

        if (core.DMAICounter > 0 && now > core.LastClock)
        {
            const u32 amt = static_cast<u32>(std::min<u64>(now - core.LastClock, static_cast<u32>(core.DMAICounter)));
            core.DMAICounter -= static_cast<s32>(amt);
            core.LastClock = now;

            if (!core.AdmaInProgress)
                Dma[cid].MADR += amt / 2;

            if (core.DMAICounter > 0)
                return;

            for (int i = 0; i < 2; i++)
            {
                if (has_to_call_irq_dma[i])
                {
                    has_to_call_irq_dma[i] = false;
                    if (!(Spdif.Info & (4 << i)) && Cores[i].IRQEnable)
                    {
                        Spdif.Info |= static_cast<u16>(4 << i);
                        spu2Irq();
                    }
                }
            }

            if (((core.AutoDMACtrl & adma_cbit) != adma_cbit) && core.ReadSize)
            {
                if (core.IsDMARead)
                    core.FinishDMAread();
                else
                    core.FinishDMAwrite();
            }

            if (core.DMAICounter <= 0)
            {
                Dma[cid].MADR = Dma[cid].TADR;
                if (cid == 0)
                    spu2DMA4Irq();
                else
                    spu2DMA7Irq();
            }
        }
    }

    void Spu2System::TimeUpdate(u64 cClocks)
    {
        if (cClocks < lClocks)
        {
            // The IOP clock restarted (emulator reset): resynchronise.
            lClocks = cClocks;
            return;
        }

        u64 dClocks = cClocks - lClocks;

        // If for some reason our clock value seems way off base, just mix out a
        // little bit, skip the rest, and hope the ship "rights" itself later on.
        if (dClocks > static_cast<u64>(TickInterval) * SanityInterval)
        {
            dClocks = static_cast<u64>(TickInterval) * SanityInterval;
            lClocks = cClocks - dClocks;
        }

        // Update Mixing Progress
        while (dClocks >= TickInterval)
        {
            dClocks -= TickInterval;
            lClocks += TickInterval;
            Cycles++;

            for (int c = 0; c < 2; c++)
            {
                if (Cores[c].KeyOff)
                {
                    StopVoices(c, Cores[c].KeyOff);
                    Cores[c].KeyOff = 0;
                }

                if (Cores[c].KeyOn)
                {
                    StartVoices(c, Cores[c].KeyOn);
                    Cores[c].KeyOn = 0;
                }
            }

            Mix();
        }

        CheckDMAProgress(0);
        CheckDMAProgress(1);
    }

    void Spu2System::UpdateSpdifMode()
    {
        if (Spdif.Out & 0x4) // use 24/32bit PCM data streaming
        {
            PlayMode = 8;
            return;
        }

        if (Spdif.Out & SPDIF_OUT_BYPASS)
        {
            PlayMode = 2;
            if (!(Spdif.Mode & SPDIF_MODE_BYPASS_BITSTREAM))
                PlayMode = 4; // bitstream bypass
        }
        else
        {
            PlayMode = 0; // normal processing
            if (Spdif.Out & SPDIF_OUT_PCM)
            {
                PlayMode = 1;
            }
        }
    }

    void Spu2System::StartVoices(int core, u32 value)
    {
        Cores[core].Regs.ENDX &= ~value;

        for (u8 vc = 0; vc < V_Core::NumVoices; vc++)
        {
            if (!((value >> vc) & 1))
                continue;

            Cores[core].Voices[vc].Start();
        }
    }

    void Spu2System::StopVoices(int core, u32 value)
    {
        for (u8 vc = 0; vc < V_Core::NumVoices; vc++)
        {
            if (!((value >> vc) & 1))
                continue;

            Cores[core].Voices[vc].ADSR.Release();
        }
    }

    // ------------------------------------------------------------------
    //  Register access (SPU2read / SPU2write / SPU2_FastWrite)
    // ------------------------------------------------------------------

    u16 Spu2System::Read(u32 rmem)
    {
        const u32 mem = rmem & 0xFFFF;

        // PCSX2 compares the masked address against 0x1f9001AC here, which never
        // matches, so reads of the 0x1AC data port fall through to the register
        // table. Kept as-is: libsd is known to work with this behaviour.
        TimeUpdate(Now());

        if (mem >= 0x800)
            return spu2Ru16(mem);
        return *(regtable[mem >> 1]);
    }

    void Spu2System::Write(u32 rmem, u16 value)
    {
        // Note: Reverb/Effects are very sensitive to having precise update timings.
        TimeUpdate(Now());
        FastWrite(rmem, value);
    }

    namespace
    {
        inline void SetHiWord(u32 &src, u16 value)
        {
            src = (src & 0x0000FFFFu) | (static_cast<u32>(value) << 16);
        }

        inline void SetLoWord(u32 &src, u16 value)
        {
            src = (src & 0xFFFF0000u) | value;
        }
    }

    void Spu2System::WriteVoiceParams(int core, int voice, int param, u16 value)
    {
        V_Voice &thisvoice = Cores[core].Voices[voice];

        switch (param)
        {
        case 0: // VOLL (Volume L)
        case 1: // VOLR (Volume R)
        {
            V_VolumeSlide &thisvol = (param == 0) ? thisvoice.Volume.Left : thisvoice.Volume.Right;
            thisvol.RegSet(value);
        }
        break;

        case 2:
            thisvoice.Pitch = value;
            break;

        case 3: // ADSR1 (Envelope)
            thisvoice.ADSR.regADSR1 = value;
            thisvoice.ADSR.UpdateCache();
            break;

        case 4: // ADSR2 (Envelope)
            thisvoice.ADSR.regADSR2 = value;
            thisvoice.ADSR.UpdateCache();
            break;

        // REG_VP_ENVX, REG_VP_VOLXL and REG_VP_VOLXR are all writable, only ENVX has any effect when written to.
        case 5:
            thisvoice.ADSR.Value = value;
            break;
        default:
            break;
        }
    }

    void Spu2System::WriteVoiceAddr(int core, int voice, int address, u16 value)
    {
        V_Voice &thisvoice = Cores[core].Voices[voice];

        switch (address)
        {
        case 0: // SSA (Waveform Start Addr) (hiword, 4 bits only)
            thisvoice.StartA = (static_cast<u32>(value & 0x0F) << 16) | (thisvoice.StartA & 0xFFF8);
            break;

        case 1: // SSA (loword)
            thisvoice.StartA = (thisvoice.StartA & 0x0F0000) | (value & 0xFFF8);
            break;

        case 2:
            thisvoice.LoopMode = 1;
            thisvoice.LoopStartA = (static_cast<u32>(value & 0x0F) << 16) | (thisvoice.LoopStartA & 0xFFF8);
            break;

        case 3:
            thisvoice.LoopMode = 1;
            thisvoice.LoopStartA = (thisvoice.LoopStartA & 0x0F0000) | (value & 0xFFF8);
            break;

        case 4:
            // NAX is confirmed to be writable on hardware (decoder will start decoding at new location).
            thisvoice.NextA = (static_cast<u32>(value & 0x0F) << 16) | (thisvoice.NextA & 0xFFF8) | 1;
            break;

        case 5:
            thisvoice.NextA = (thisvoice.NextA & 0x0F0000) | (value & 0xFFF8) | 1;
            break;
        default:
            break;
        }
    }

    void Spu2System::WriteCore(int core, u32 omem, u16 value)
    {
        V_Core &thiscore = Cores[core];

        // Games like to repeatedly write these regs over and over with the same value, hence
        // the shortcut that skips the bitloop if the values are equal.
        const auto setSomeBits = [&](u32 &reg, s32 V_VoiceGates::*mask, bool hiword)
        {
            const u32 result = reg;
            if (hiword)
                SetHiWord(reg, value);
            else
                SetLoWord(reg, value);
            if (result == reg)
                return;

            const uint start_bit = hiword ? 16 : 0;
            const uint end_bit = hiword ? 24 : 16;
            for (uint vc = start_bit, vx = 1; vc < end_bit; ++vc, vx <<= 1)
                thiscore.VoiceGates[vc].*mask = (value & vx) ? -1 : 0;
        };

        switch (omem)
        {
        case REG__1AC:
            // 0x1ac / 0x5ac : direct-write to DMA address : special register (undocumented)
            thiscore.ActiveTSA = thiscore.TSA;
            for (int i = 0; i < 2; i++)
            {
                if (Cores[i].IRQEnable && (Cores[i].IRQA == thiscore.ActiveTSA))
                {
                    SetIrqCall(i);
                }
            }
            thiscore.DmaWrite(value);
            break;

        case REG_C_ATTR:
        {
            const bool irqe = thiscore.IRQEnable;
            const bool oldFXenable = thiscore.FxEnable;
            const u8 oldDmaMode = thiscore.DmaMode;
            (void)oldFXenable;

            thiscore.AttrBit0 = (value >> 0) & 0x01;                    // 1 bit
            thiscore.DMABits = static_cast<s8>((value >> 1) & 0x07);    // 3 bits
            thiscore.DmaMode = (value >> 4) & 0x03;                     // 2 bit (not necessary, we get the direction from the iop)
            thiscore.IRQEnable = (value >> 6) & 0x01;                   // 1 bit
            thiscore.FxEnable = (value >> 7) & 0x01;                    // 1 bit
            thiscore.NoiseClk = (value >> 8) & 0x3f;                    // 6 bits
            thiscore.Mute = false;
            thiscore.Regs.ATTR = value & 0xffff;

            if (!thiscore.DmaMode && !(thiscore.Regs.STATX & 0x400))
                thiscore.Regs.STATX &= ~0x80;
            else if (!oldDmaMode && thiscore.DmaMode)
                thiscore.Regs.STATX |= 0x80;

            thiscore.ActiveTSA = thiscore.TSA;

            if (thiscore.IRQEnable != irqe)
            {
                if (!thiscore.IRQEnable)
                    Spdif.Info &= static_cast<u16>(~(4 << thiscore.Index));
            }
        }
        break;

        case REG_S_PMON:
            for (int vc = 1; vc < 16; ++vc)
                thiscore.Voices[vc].Modulated = (value >> vc) & 1;
            SetLoWord(thiscore.Regs.PMON, value);
            break;

        case (REG_S_PMON + 2):
            for (int vc = 0; vc < 8; ++vc)
                thiscore.Voices[vc + 16].Modulated = (value >> vc) & 1;
            SetHiWord(thiscore.Regs.PMON, value);
            break;

        case REG_S_NON:
            for (int vc = 0; vc < 16; ++vc)
                thiscore.Voices[vc].Noise = (value >> vc) & 1;
            SetLoWord(thiscore.Regs.NON, value);
            break;

        case (REG_S_NON + 2):
            for (int vc = 0; vc < 8; ++vc)
                thiscore.Voices[vc + 16].Noise = (value >> vc) & 1;
            SetHiWord(thiscore.Regs.NON, value);
            break;

        case REG_S_VMIXL:
            setSomeBits(thiscore.Regs.VMIXL, &V_VoiceGates::DryL, false);
            break;

        case (REG_S_VMIXL + 2):
            setSomeBits(thiscore.Regs.VMIXL, &V_VoiceGates::DryL, true);
            break;

        case REG_S_VMIXEL:
            setSomeBits(thiscore.Regs.VMIXEL, &V_VoiceGates::WetL, false);
            break;

        case (REG_S_VMIXEL + 2):
            setSomeBits(thiscore.Regs.VMIXEL, &V_VoiceGates::WetL, true);
            break;

        case REG_S_VMIXR:
            setSomeBits(thiscore.Regs.VMIXR, &V_VoiceGates::DryR, false);
            break;

        case (REG_S_VMIXR + 2):
            setSomeBits(thiscore.Regs.VMIXR, &V_VoiceGates::DryR, true);
            break;

        case REG_S_VMIXER:
            setSomeBits(thiscore.Regs.VMIXER, &V_VoiceGates::WetR, false);
            break;

        case (REG_S_VMIXER + 2):
            setSomeBits(thiscore.Regs.VMIXER, &V_VoiceGates::WetR, true);
            break;

        case REG_P_MMIX:
        {
            // Each MMIX gate is assigned either 0 or 0xffffffff depending on the status
            // of the MMIX bits.
            const int vx = value & ((core == 0) ? 0xFF0 : 0xFFF);
            thiscore.WetGate.ExtR = (vx & 0x001) ? -1 : 0;
            thiscore.WetGate.ExtL = (vx & 0x002) ? -1 : 0;
            thiscore.DryGate.ExtR = (vx & 0x004) ? -1 : 0;
            thiscore.DryGate.ExtL = (vx & 0x008) ? -1 : 0;
            thiscore.WetGate.InpR = (vx & 0x010) ? -1 : 0;
            thiscore.WetGate.InpL = (vx & 0x020) ? -1 : 0;
            thiscore.DryGate.InpR = (vx & 0x040) ? -1 : 0;
            thiscore.DryGate.InpL = (vx & 0x080) ? -1 : 0;
            thiscore.WetGate.SndR = (vx & 0x100) ? -1 : 0;
            thiscore.WetGate.SndL = (vx & 0x200) ? -1 : 0;
            thiscore.DryGate.SndR = (vx & 0x400) ? -1 : 0;
            thiscore.DryGate.SndL = (vx & 0x800) ? -1 : 0;
            thiscore.Regs.MMIX = value;
        }
        break;

        case REG_S_ENDX:
            thiscore.Regs.ENDX &= 0xff0000;
            break;

        case (REG_S_ENDX + 2):
            thiscore.Regs.ENDX &= 0xffff;
            break;

        case REG_S_ADMAS:
            // (PCSX2's ps1driver hack for value 0x7FFF is not ported: PS1 mode is unsupported.)
            thiscore.AutoDMACtrl = value;
            if (!(value & 0x3) && thiscore.AdmaInProgress)
            {
                // Kill the current transfer so it doesn't continue
                thiscore.AdmaInProgress = false;
                thiscore.InputDataLeft = 0;
                thiscore.DMAICounter = 0;
                thiscore.InputDataTransferred = 0;

                // Not accurate behaviour but shouldn't hurt for now (Prince of Persia Warrior Within).
                for (int i = 0; i < 0x200; i++)
                {
                    GetMemPtr(0x2000 + (thiscore.Index << 10))[i] = 0;
                    GetMemPtr(0x2200 + (thiscore.Index << 10))[i] = 0;
                }
            }
            break;

        default:
        {
            const u32 addr = omem | ((core == 1) ? 0x400 : 0);
            *(regtable[addr >> 1]) = value;
        }
        break;
        }
    }

    void Spu2System::WriteCoreExt(int core, u32 addr, u16 value)
    {
        V_Core &thiscore = Cores[core];

        switch (addr)
        {
        case REG_P_MVOLL:
        case REG_P_MVOLR:
        {
            V_VolumeSlide &thisvol = (addr == REG_P_MVOLL) ? thiscore.MasterVol.Left : thiscore.MasterVol.Right;
            thisvol.RegSet(value);
        }
        break;

        case REG_P_EVOLL:
            thiscore.FxVol.Left = SignExtend16(value);
            break;

        case REG_P_EVOLR:
            thiscore.FxVol.Right = SignExtend16(value);
            break;

        case REG_P_AVOLL:
            thiscore.ExtVol.Left = SignExtend16(value);
            break;

        case REG_P_AVOLR:
            thiscore.ExtVol.Right = SignExtend16(value);
            break;

        case REG_P_BVOLL:
            thiscore.InpVol.Left = SignExtend16(value);
            break;

        case REG_P_BVOLR:
            thiscore.InpVol.Right = SignExtend16(value);
            break;

        // MVOLX has been confirmed to not be allowed to be written to.
        case REG_P_MVOLXL:
        case REG_P_MVOLXR:
            break;

        default:
        {
            const u32 raddr = addr + ((core == 1) ? 0x28 : 0);
            *(regtable[raddr >> 1]) = value;
        }
        break;
        }
    }

    // Replaces PCSX2's tbl_reg_writes[] lookup: decodes (rmem & 0x7ff) to the
    // same handler the table would select.
    void Spu2System::FastWrite(u32 rmem, u16 value)
    {
        const u32 a = rmem & 0x7FE;

        if (a < 0x760)
        {
            const int core = (a & 0x400) ? 1 : 0;
            const u32 off = a & 0x3FF;

            if (off < 0x180)
            {
                WriteVoiceParams(core, static_cast<int>(off >> 4), static_cast<int>((off & 0xF) >> 1), value);
                return;
            }
            if (off >= REG_VA_SSA && off < REG_A_ESA)
            {
                const u32 rel = off - REG_VA_SSA;
                WriteVoiceAddr(core, static_cast<int>(rel / 12), static_cast<int>((rel % 12) >> 1), value);
                return;
            }
            if (off >= 0x346)
            {
                // REGRAW: 0x346..0x3FE (core 0) and 0x746..0x75E (core 1)
                *(regtable[a >> 1]) = value;
                return;
            }
            if (off == REG_A_EEA + 2)
                return; // RegWrite_Null
            if (off >= 0x1B2 && off < REG_VA_SSA)
            {
                *(regtable[a >> 1]) = value; // REGRAW 0x1B2..0x1BE
                return;
            }
            WriteCore(core, off, value);
            return;
        }

        if (a < 0x788)
        {
            WriteCoreExt(0, a, value);
            return;
        }
        if (a < 0x7B0)
        {
            WriteCoreExt(1, a - 0x28, value);
            return;
        }

        switch (a)
        {
        case SPDIF_OUT:
        case SPDIF_IRQINFO:
        case SPDIF_MODE:
        case SPDIF_MEDIA:
        case SPDIF_PROTECT:
            *(regtable[a >> 1]) = value;
            UpdateSpdifMode();
            break;
        default:
            *(regtable[a >> 1]) = value;
            break;
        }
    }

    // ------------------------------------------------------------------
    //  RegTable.cpp
    // ------------------------------------------------------------------
    void Spu2System::BuildRegTable()
    {
        const auto U16P = [](auto &x)
        { return reinterpret_cast<u16 *>(&x); };
        const auto U16P_HI = [](auto &x)
        { return reinterpret_cast<u16 *>(&x) + 1; };

        regtable.fill(nullptr);

        for (int c = 0; c < 2; ++c)
        {
            V_Core &core = Cores[c];
            const u32 base = c ? 0x400u : 0u;
            const auto set = [&](u32 addr, u16 *ptr)
            { regtable[(base + addr) >> 1] = ptr; };

            // Voice Params: 8 params, 24 voices = 0x180 bytes
            for (u32 v = 0; v < V_Core::NumVoices; ++v)
            {
                V_Voice &vc = core.Voices[v];
                const u32 vb = v * 16;
                set(vb + 0x0, U16P(vc.Volume.Left.Reg_VOL));
                set(vb + 0x2, U16P(vc.Volume.Right.Reg_VOL));
                set(vb + 0x4, U16P(vc.Pitch));
                set(vb + 0x6, U16P(vc.ADSR.regADSR1));
                set(vb + 0x8, U16P(vc.ADSR.regADSR2));
                set(vb + 0xA, U16P(vc.ADSR.Value));
                set(vb + 0xC, U16P(vc.Volume.Left.Value));
                set(vb + 0xE, U16P(vc.Volume.Right.Value));
            }

            set(0x180, U16P(core.Regs.PMON));
            set(0x182, U16P_HI(core.Regs.PMON));
            set(0x184, U16P(core.Regs.NON));
            set(0x186, U16P_HI(core.Regs.NON));
            set(0x188, U16P(core.Regs.VMIXL));
            set(0x18A, U16P_HI(core.Regs.VMIXL));
            set(0x18C, U16P(core.Regs.VMIXEL));
            set(0x18E, U16P_HI(core.Regs.VMIXEL));
            set(0x190, U16P(core.Regs.VMIXR));
            set(0x192, U16P_HI(core.Regs.VMIXR));
            set(0x194, U16P(core.Regs.VMIXER));
            set(0x196, U16P_HI(core.Regs.VMIXER));
            set(0x198, U16P(core.Regs.MMIX));
            set(0x19A, U16P(core.Regs.ATTR));
            set(0x19C, U16P_HI(core.IRQA));
            set(0x19E, U16P(core.IRQA));
            set(0x1A0, U16P(core.KeyOn));
            set(0x1A2, U16P_HI(core.KeyOn));
            set(0x1A4, U16P(core.KeyOff));
            set(0x1A6, U16P_HI(core.KeyOff));
            set(0x1A8, U16P_HI(core.TSA));
            set(0x1AA, U16P(core.TSA));
            // 0x1AC, 0x1AE, 0x1B2..0x1BE: raw
            set(0x1B0, U16P(core.AutoDMACtrl));

            // Voice Addresses
            for (u32 v = 0; v < V_Core::NumVoices; ++v)
            {
                V_Voice &vc = core.Voices[v];
                const u32 vb = 0x1C0 + v * 12;
                set(vb + 0x0, U16P_HI(vc.StartA));
                set(vb + 0x2, U16P(vc.StartA));
                set(vb + 0x4, U16P_HI(vc.LoopStartA));
                set(vb + 0x6, U16P(vc.LoopStartA));
                set(vb + 0x8, U16P_HI(vc.NextA));
                set(vb + 0xA, U16P(vc.NextA));
            }

            set(0x2E0, U16P_HI(core.EffectsStartA));
            set(0x2E2, U16P(core.EffectsStartA));

            u32 *const revb[] = {
                &core.Revb.APF1_SIZE, &core.Revb.APF2_SIZE, &core.Revb.SAME_L_DST, &core.Revb.SAME_R_DST,
                &core.Revb.COMB1_L_SRC, &core.Revb.COMB1_R_SRC, &core.Revb.COMB2_L_SRC, &core.Revb.COMB2_R_SRC,
                &core.Revb.SAME_L_SRC, &core.Revb.SAME_R_SRC, &core.Revb.DIFF_L_DST, &core.Revb.DIFF_R_DST,
                &core.Revb.COMB3_L_SRC, &core.Revb.COMB3_R_SRC, &core.Revb.COMB4_L_SRC, &core.Revb.COMB4_R_SRC,
                &core.Revb.DIFF_L_SRC, &core.Revb.DIFF_R_SRC, &core.Revb.APF1_L_DST, &core.Revb.APF1_R_DST,
                &core.Revb.APF2_L_DST, &core.Revb.APF2_R_DST};
            for (u32 i = 0; i < std::size(revb); ++i)
            {
                set(0x2E4 + i * 4, U16P_HI(*revb[i]));
                set(0x2E6 + i * 4, U16P(*revb[i]));
            }

            set(0x33C, U16P_HI(core.EffectsEndA));
            set(0x33E, U16P(core.EffectsEndA));
            set(0x340, U16P(core.Regs.ENDX));
            set(0x342, U16P_HI(core.Regs.ENDX));
            set(0x344, U16P(core.Regs.STATX));
        }

        // 0x760: weird area
        for (int c = 0; c < 2; ++c)
        {
            V_Core &core = Cores[c];
            const u32 b = 0x760u + static_cast<u32>(c) * 0x28u;
            regtable[(b + 0x00) >> 1] = U16P(core.MasterVol.Left.Reg_VOL);
            regtable[(b + 0x02) >> 1] = U16P(core.MasterVol.Right.Reg_VOL);
            regtable[(b + 0x04) >> 1] = U16P(core.FxVol.Left);
            regtable[(b + 0x06) >> 1] = U16P(core.FxVol.Right);
            regtable[(b + 0x08) >> 1] = U16P(core.ExtVol.Left);
            regtable[(b + 0x0A) >> 1] = U16P(core.ExtVol.Right);
            regtable[(b + 0x0C) >> 1] = U16P(core.InpVol.Left);
            regtable[(b + 0x0E) >> 1] = U16P(core.InpVol.Right);
            regtable[(b + 0x10) >> 1] = U16P(core.MasterVol.Left.Value);
            regtable[(b + 0x12) >> 1] = U16P(core.MasterVol.Right.Value);
            regtable[(b + 0x14) >> 1] = U16P(core.Revb.IIR_VOL);
            regtable[(b + 0x16) >> 1] = U16P(core.Revb.COMB1_VOL);
            regtable[(b + 0x18) >> 1] = U16P(core.Revb.COMB2_VOL);
            regtable[(b + 0x1A) >> 1] = U16P(core.Revb.COMB3_VOL);
            regtable[(b + 0x1C) >> 1] = U16P(core.Revb.COMB4_VOL);
            regtable[(b + 0x1E) >> 1] = U16P(core.Revb.WALL_VOL);
            regtable[(b + 0x20) >> 1] = U16P(core.Revb.APF1_VOL);
            regtable[(b + 0x22) >> 1] = U16P(core.Revb.APF2_VOL);
            regtable[(b + 0x24) >> 1] = U16P(core.Revb.IN_COEF_L);
            regtable[(b + 0x26) >> 1] = U16P(core.Revb.IN_COEF_R);
        }

        // SPDIF interface
        regtable[0x7C0 >> 1] = U16P(Spdif.Out);
        regtable[0x7C2 >> 1] = U16P(Spdif.Info);
        regtable[0x7C4 >> 1] = U16P(Spdif.Unknown1);
        regtable[0x7C6 >> 1] = U16P(Spdif.Mode);
        regtable[0x7C8 >> 1] = U16P(Spdif.Media);
        regtable[0x7CA >> 1] = U16P(Spdif.Unknown2);
        regtable[0x7CC >> 1] = U16P(Spdif.Protection);

        // Everything else maps onto the raw register file.
        for (u32 mem = 0; mem < 0x800; mem += 2)
        {
            if (!regtable[mem >> 1])
                regtable[mem >> 1] = &spu2Ru16(mem);
        }
        regtable[0x400] = nullptr;
    }
}
