// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// SPU2 state, ported from PCSX2 pcsx2/SPU2/defs.h and regs.h
// (commit 091ab68708a555198d80a7216965481c7ddd0d98).
//
// Changes from PCSX2:
//  - The global state (Cores, Spdif, spu2regs, _spu2mem, the ADPCM cache,
//    OutPos, Cycles, PlayMode, lClocks, ...) lives in one Spu2System instance
//    so several IOP emulators can coexist (unit tests).
//  - Savestate, debug UI, wave-dump, PS1-mode and multi-ISA code are dropped.
//  - DMA source/target pointers into IOP RAM are byte addresses with
//    wrap-around instead of raw host pointers.
//  - The IOP DMA controller glue (PCSX2 IopDma.cpp, HW_DMA4/7_* registers,
//    psxCounters[6]) is part of Spu2System.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <vector>

namespace zx::spu2
{
    using s8 = int8_t;
    using u8 = uint8_t;
    using s16 = int16_t;
    using u16 = uint16_t;
    using s32 = int32_t;
    using u32 = uint32_t;
    using s64 = int64_t;
    using u64 = uint64_t;
    using uint = unsigned int;

    // ------------------------------------------------------------------
    //  regs.h
    // ------------------------------------------------------------------
    constexpr u32 REG_S_PMON = 0x0180;
    constexpr u32 REG_S_NON = 0x0184;
    constexpr u32 REG_S_VMIXL = 0x0188;
    constexpr u32 REG_S_VMIXEL = 0x018C;
    constexpr u32 REG_S_VMIXR = 0x0190;
    constexpr u32 REG_S_VMIXER = 0x0194;
    constexpr u32 REG_P_MMIX = 0x0198;
    constexpr u32 REG_C_ATTR = 0x019A;
    constexpr u32 REG_A_IRQA = 0x019C;
    constexpr u32 REG_S_KON = 0x01A0;
    constexpr u32 REG_S_KOFF = 0x01A4;
    constexpr u32 REG_A_TSA = 0x01A8;
    constexpr u32 REG__1AC = 0x01AC;
    constexpr u32 REG_S_ADMAS = 0x01B0;
    constexpr u32 REG_VA_SSA = 0x01C0;
    constexpr u32 REG_A_ESA = 0x02E0;
    constexpr u32 REG_A_EEA = 0x033C;
    constexpr u32 REG_S_ENDX = 0x0340;
    constexpr u32 REG_P_STATX = 0x0344;

    constexpr u32 REG_P_MVOLL = 0x0760;
    constexpr u32 REG_P_MVOLR = 0x0762;
    constexpr u32 REG_P_EVOLL = 0x0764;
    constexpr u32 REG_P_EVOLR = 0x0766;
    constexpr u32 REG_P_AVOLL = 0x0768;
    constexpr u32 REG_P_AVOLR = 0x076A;
    constexpr u32 REG_P_BVOLL = 0x076C;
    constexpr u32 REG_P_BVOLR = 0x076E;
    constexpr u32 REG_P_MVOLXL = 0x0770;
    constexpr u32 REG_P_MVOLXR = 0x0772;

    constexpr u32 SPDIF_OUT = 0x07C0;
    constexpr u32 SPDIF_IRQINFO = 0x07C2;
    constexpr u32 SPDIF_MODE = 0x07C6;
    constexpr u32 SPDIF_MEDIA = 0x07C8;
    constexpr u32 SPDIF_PROTECT = 0x07CC;

    constexpr u16 SPDIF_OUT_PCM = 0x0020;
    constexpr u16 SPDIF_OUT_BYPASS = 0x0100;
    constexpr u16 SPDIF_MODE_BYPASS_BITSTREAM = 0x0002;

    // ------------------------------------------------------------------
    //  defs.h
    // ------------------------------------------------------------------
    struct StereoOut32
    {
        s32 Left = 0;
        s32 Right = 0;

        StereoOut32() = default;
        StereoOut32(s32 left, s32 right)
            : Left(left), Right(right)
        {
        }

        StereoOut32 operator+(const StereoOut32 &right) const
        {
            return StereoOut32(Left + right.Left, Right + right.Right);
        }
    };

    inline s16 SignExtend16(u16 v)
    {
        return static_cast<s16>(v);
    }

    inline s32 clamp_mix(s32 x)
    {
        return x < -0x8000 ? -0x8000 : (x > 0x7fff ? 0x7fff : x);
    }

    inline StereoOut32 clamp_mix(StereoOut32 sample)
    {
        return StereoOut32(clamp_mix(sample.Left), clamp_mix(sample.Right));
    }

    struct V_VolumeLR
    {
        s32 Left = 0;
        s32 Right = 0;

        V_VolumeLR() = default;
        explicit V_VolumeLR(s32 both)
            : Left(both), Right(both)
        {
        }
    };

    struct V_VolumeSlide
    {
        // Holds the "original" value of the volume for this voice, prior to slides.
        union
        {
            u16 Reg_VOL;
            struct
            {
                u16 Step : 2;
                u16 Shift : 5;
                u16 : 5;
                u16 Phase : 1;
                u16 Decr : 1;
                u16 Exp : 1;
                u16 Enable : 1;
            };
        };

        u32 Counter = 0;
        s32 Value = 0;

        V_VolumeSlide()
            : Reg_VOL(0)
        {
        }
        V_VolumeSlide(s16 regval, s32 fullvol)
            : Reg_VOL(static_cast<u16>(regval)), Counter(0), Value(fullvol)
        {
        }

        void Update();
        void RegSet(u16 src);
    };

    struct V_VolumeSlideLR
    {
        V_VolumeSlide Left;
        V_VolumeSlide Right;

        V_VolumeSlideLR() = default;
        V_VolumeSlideLR(s16 regval, s32 bothval)
            : Left(regval, bothval), Right(regval, bothval)
        {
        }

        void Update()
        {
            Left.Update();
            Right.Update();
        }
    };

    struct V_ADSR
    {
        union
        {
            u32 reg32;

            struct
            {
                u16 regADSR1;
                u16 regADSR2;
            };

            struct
            {
                u32 SustainLevel : 4;
                u32 DecayShift : 4;
                u32 AttackStep : 2;
                u32 AttackShift : 5;
                u32 AttackMode : 1;
                u32 ReleaseShift : 5;
                u32 ReleaseMode : 1;
                u32 SustainStep : 2;
                u32 SustainShift : 5;
                u32 : 1;
                u32 SustainDir : 1;
                u32 SustainMode : 1;
            };
        };

        static constexpr int ADSR_PHASES = 5;

        static constexpr int PHASE_STOPPED = 0;
        static constexpr int PHASE_ATTACK = 1;
        static constexpr int PHASE_DECAY = 2;
        static constexpr int PHASE_SUSTAIN = 3;
        static constexpr int PHASE_RELEASE = 4;

        struct CachedADSR
        {
            bool Decr = false;
            bool Exp = false;
            u8 Shift = 0;
            s8 Step = 0;
            s32 Target = 0;
        };

        std::array<CachedADSR, ADSR_PHASES> CachedPhases{};

        u32 Counter = 0;
        s32 Value = 0; // Ranges from 0 to 0x7fff (signed values are clamped to 0) [Reg_ENVX]
        u8 Phase = 0;  // monitors current phase of ADSR envelope

        V_ADSR()
            : reg32(0)
        {
        }

        void UpdateCache();
        bool Calculate();
        void Attack();
        void Release();
    };

    struct V_Voice
    {
        V_VolumeSlideLR Volume;

        // Envelope
        V_ADSR ADSR;
        // Pitch (also Reg_PITCH)
        u16 Pitch = 0;
        // Loop Start address (also Reg_LSAH/L)
        u32 LoopStartA = 0;
        // Sound Start address (also Reg_SSAH/L)
        u32 StartA = 0;
        // Next Read Data address (also Reg_NAXH/L)
        u32 NextA = 0;
        // Voice Decoding State
        s32 Prev1 = 0;
        s32 Prev2 = 0;

        // Pitch Modulated by previous voice
        bool Modulated = false;
        // Source (Wave/Noise)
        bool Noise = false;

        s8 LoopMode = 0;
        s8 LoopFlags = 0;

        // Sample pointer (19:12 bit fixed point)
        s32 SP = 0;

        // Last outputted audio value, used for voice modulation.
        s32 OutX = 0;

        // SBuffer points directly to an ADPCM cache entry.
        s16 *SBuffer = nullptr;

        // Each voice has a buffer of decoded samples
        s32 DecodeFifo[32]{};
        u32 DecPosWrite = 0;
        u32 DecPosRead = 0;

        void Start();
        void Stop();
    };

    struct V_Reverb
    {
        s16 IN_COEF_L = 0;
        s16 IN_COEF_R = 0;

        u32 APF1_SIZE = 0;
        u32 APF2_SIZE = 0;

        s16 APF1_VOL = 0;
        s16 APF2_VOL = 0;

        u32 SAME_L_SRC = 0;
        u32 SAME_R_SRC = 0;
        u32 DIFF_L_SRC = 0;
        u32 DIFF_R_SRC = 0;
        u32 SAME_L_DST = 0;
        u32 SAME_R_DST = 0;
        u32 DIFF_L_DST = 0;
        u32 DIFF_R_DST = 0;

        s16 IIR_VOL = 0;
        s16 WALL_VOL = 0;

        u32 COMB1_L_SRC = 0;
        u32 COMB1_R_SRC = 0;
        u32 COMB2_L_SRC = 0;
        u32 COMB2_R_SRC = 0;
        u32 COMB3_L_SRC = 0;
        u32 COMB3_R_SRC = 0;
        u32 COMB4_L_SRC = 0;
        u32 COMB4_R_SRC = 0;

        s16 COMB1_VOL = 0;
        s16 COMB2_VOL = 0;
        s16 COMB3_VOL = 0;
        s16 COMB4_VOL = 0;

        u32 APF1_L_DST = 0;
        u32 APF1_R_DST = 0;
        u32 APF2_L_DST = 0;
        u32 APF2_R_DST = 0;
    };

    struct V_SPDIF
    {
        u16 Out = 0;
        u16 Info = 0;
        u16 Unknown1 = 0;
        u16 Mode = 0;
        u16 Media = 0;
        u16 Unknown2 = 0;
        u16 Protection = 0;
    };

    struct V_CoreRegs
    {
        u32 PMON = 0;
        u32 NON = 0;
        u32 VMIXL = 0;
        u32 VMIXR = 0;
        u32 VMIXEL = 0;
        u32 VMIXER = 0;
        u32 ENDX = 0;

        u16 MMIX = 0;
        u16 STATX = 0;
        u16 ATTR = 0;
        u16 _1AC = 0;
    };

    struct V_VoiceGates
    {
        s32 DryL = 0; // 'AND Gate' for Direct Output to Left Channel
        s32 DryR = 0; // 'AND Gate' for Direct Output for Right Channel
        s32 WetL = 0; // 'AND Gate' for Effect Output for Left Channel
        s32 WetR = 0; // 'AND Gate' for Effect Output for Right Channel
    };

    struct V_CoreGates
    {
        s32 InpL = 0; // Sound Data Input to Direct Output (Left)
        s32 InpR = 0; // Sound Data Input to Direct Output (Right)
        s32 SndL = 0; // Voice Data to Direct Output (Left)
        s32 SndR = 0; // Voice Data to Direct Output (Right)
        s32 ExtL = 0; // External Input to Direct Output (Left)
        s32 ExtR = 0; // External Input to Direct Output (Right)
    };

    struct VoiceMixSet
    {
        StereoOut32 Dry, Wet;

        VoiceMixSet() = default;
        VoiceMixSet(const StereoOut32 &dry, const StereoOut32 &wet)
            : Dry(dry), Wet(wet)
        {
        }
    };

    // Sentinel for "no DMA source pointer yet" (PCSX2 used nullptr).
    constexpr u32 kNoDmaPtr = 0xFFFFFFFFu;

    struct Spu2System;

    struct V_Core
    {
        static constexpr uint NumVoices = 24;

        Spu2System *Sys = nullptr;
        u32 Index = 0; // Core index identifier.

        V_VoiceGates VoiceGates[NumVoices]{};
        V_CoreGates DryGate;
        V_CoreGates WetGate;

        V_VolumeSlideLR MasterVol; // Master Volume
        V_VolumeLR ExtVol;         // Volume for External Data Input
        V_VolumeLR InpVol;         // Volume for Sound Data Input
        V_VolumeLR FxVol;          // Volume for Output from Effects

        V_Voice Voices[NumVoices];

        u32 IRQA = 0;      // Interrupt Address
        u32 TSA = 0;       // DMA Transfer Start Address
        u32 ActiveTSA = 0; // Active DMA TSA - Required for NFL 2k5 which overwrites it mid transfer

        bool IRQEnable = false; // Interrupt Enable
        bool FxEnable = false;  // Effect Enable
        bool Mute = false;      // Mute
        bool AdmaInProgress = false;

        s8 DMABits = 0;            // DMA related?
        u8 NoiseClk = 0;           // Noise Clock
        u32 NoiseCnt = 0;          // Noise Counter
        u32 NoiseOut = 0;          // Noise Output
        u16 AutoDMACtrl = 0;       // AutoDMA Status
        s32 DMAICounter = 0;       // DMA Interrupt Counter
        u64 LastClock = 0;         // DMA Interrupt Clock Cycle Counter
        u32 InputDataLeft = 0;     // Input Buffer
        u32 InputDataTransferred = 0; // Used for simulating MADR increase (GTA VC)
        u32 InputPosWrite = 0;
        u32 InputDataProgress = 0;

        V_Reverb Revb; // Reverb Registers

        s16 RevbDownBuf[2][64 * 2]{}; // Downsample buffer for reverb, one for each channel
        s16 RevbUpBuf[2][64 * 2]{};   // Upsample buffer for reverb, one for each channel
        u32 RevbSampleBufPos = 0;
        u32 EffectsStartA = 0;
        u32 EffectsEndA = 0;

        V_CoreRegs Regs; // Registers

        u8 AttrBit0 = 0;
        u8 DmaMode = 0;

        // IOP RAM byte addresses (PCSX2: u16* DMAPtr / DMARPtr)
        u32 DMAPtr = kNoDmaPtr;
        u32 DMARPtr = kNoDmaPtr; // Mem pointer for DMA Reads
        u32 ReadSize = 0;
        bool IsDMARead = false;

        u32 KeyOn = 0;
        u32 KeyOff = 0;

        void Init(int index);

        // Mixer
        StereoOut32 DoReverb(StereoOut32 Input);
        s32 RevbGetIndexer(s32 offset);
        StereoOut32 ReadInput();

        // DMA
        u16 DmaRead();
        void DmaWrite(u16 value);
        void DoDMAwrite(u32 iopAddr, u32 size);
        void DoDMAread(u32 iopAddr, u32 size);
        void FinishDMAread();
        void AutoDMAReadBuffer(int mode);
        void StartADMAWrite(u32 size);
        void PlainDMAWrite(u32 size);
        void FinishDMAwrite();

        u32 &MADR();
        u32 &TADR();
    };

    // ADPCM decoder cache (see PCSX2 defs.h).
    constexpr s32 SPU2_DYN_MEMLINE = 0x2800;
    constexpr int pcm_WordsPerBlock = 8;
    constexpr int pcm_BlockCount = 0x100000 / pcm_WordsPerBlock;
    constexpr int pcm_DecodedSamplesPerBlock = 28;

    struct PcmCacheEntry
    {
        bool Validated = false;
        s16 Sampledata[pcm_DecodedSamplesPerBlock]{};
        s32 Prev1 = 0;
        s32 Prev2 = 0;
    };

    // IOP DMA channel registers for DMA4 (core 0) and DMA7 (core 1).
    struct IopDmaChannel
    {
        u32 MADR = 0;
        u32 BCR = 0;
        u32 CHCR = 0;
        u32 TADR = 0;
    };

    struct Spu2System
    {
        static constexpr uint TickInterval = 768;
        static constexpr int SanityInterval = 4800;
        static constexpr u32 IopRamMask = 0x1FFFFFu;

        explicit Spu2System(std::span<u8> iopRam);

        // State formerly global in PCSX2.
        V_Core Cores[2];
        V_SPDIF Spdif;
        std::vector<s16> spu2regs;  // 0x10000 bytes
        std::vector<s16> spu2mem;   // 2 MiB
        std::vector<PcmCacheEntry> pcm_cache;
        std::array<u16 *, 0x401> regtable{};
        u16 OutPos = 0;
        u32 Cycles = 0;
        int PlayMode = 0;
        u64 lClocks = 0;
        bool has_to_call_irq_dma[2] = {false, false};
        float DCFilterIn[2] = {0.f, 0.f};
        float DCFilterOut[2] = {0.f, 0.f};

        // IOP side.
        std::span<u8> IopRam;
        IopDmaChannel Dma[2];
        const u64 *Clock = nullptr;
        u32 PendingIrqs = 0;
        u64 SamplesMixed = 0;

        // Optional raw dump of ADMA input blocks (ZX_SPU2_ADMA_DUMP=<path>).
        std::FILE *AdmaDump = nullptr;
        // Optional SPU2 RAM + voice state snapshot every 5 emulated seconds
        // (ZX_SPU2_RAM_DUMP=<prefix>: <prefix>.ram, <prefix>.voices).
        const char *RamDumpPrefix = nullptr;
        void DumpState();

        // Optional trace (ZX_TRACE_SPU2=<n>: first n events).
        std::function<void(const char *)> Trace;
        long TraceBudget = 0;
        void TraceEvent(const char *fmt, ...);

        // Host output (interleaved stereo s16).
        std::function<void(const s16 *, size_t)> Output;
        std::vector<s16> OutBuf;
        size_t OutBufFrames = 0;
        static constexpr size_t OutChunkFrames = 256;

        u64 Now() const { return Clock ? *Clock : lClocks; }

        void Reset();
        void BuildRegTable();

        // spu2sys
        s16 *GetMemPtr(u32 addr) { return spu2mem.data() + (addr & 0xfffff); }
        s16 spu2M_Read(u32 addr) { return *GetMemPtr(addr & 0xfffff); }
        void spu2M_Write(u32 addr, s16 value);
        void spu2M_Write(u32 addr, u16 value) { spu2M_Write(addr, static_cast<s16>(value)); }
        u16 &spu2Ru16(u32 mem) { return reinterpret_cast<u16 *>(spu2regs.data())[(mem & 0x1fff) >> 1]; }

        void SetIrqCall(int core);
        void SetIrqCallDMA(int core);
        void StartVoices(int core, u32 value);
        void StopVoices(int core, u32 value);
        void UpdateSpdifMode();
        void TimeUpdate(u64 cClocks);
        void CheckDMAProgress(int cid);

        u16 Read(u32 rmem);
        void Write(u32 rmem, u16 value);
        void FastWrite(u32 rmem, u16 value);
        void WriteCore(int core, u32 omem, u16 value);
        void WriteCoreExt(int core, u32 addr, u16 value);
        void WriteVoiceParams(int core, int voice, int param, u16 value);
        void WriteVoiceAddr(int core, int voice, int address, u16 value);

        // IopDma glue
        void spu2Irq();
        void spu2DMA4Irq();
        void spu2DMA7Irq();
        void InterruptDMA(int core);
        void StartDma(int core);

        // IOP RAM <-> SPU2 RAM copies (halfword counts, IOP address wraps).
        void CopyFromIop(u32 iopAddr, s16 *dst, u32 halfwords) const;
        void CopyToIop(u32 iopAddr, const s16 *src, u32 halfwords);

        // Mixer
        void Mix();
        void Output32(StereoOut32 out);
        void FlushOutput();
    };

    // Decodes one 16-byte ADPCM block into 28 samples (Mixer.cpp XA_decode_block).
    void DecodeAdpcmBlock(const u8 block[16], s16 out[28], s32 &prev1, s32 &prev2);

    // Reverb resampling filters (ReverbResample.cpp, reference versions).
    s32 ReverbDownsample(V_Core &core, bool right);
    StereoOut32 ReverbUpsample(V_Core &core);
}
