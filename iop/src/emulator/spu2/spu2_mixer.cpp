// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Ported from PCSX2 pcsx2/SPU2/Mixer.cpp, ReadInput.cpp and the output part of
// spu2.cpp (commit 091ab687). Mixing logic is unchanged; the PCM24/CDDA HiFi
// input path, wave dumps, debug counters and the host AudioStream are
// dropped. Output goes to Spu2System::Output in chunks of interleaved s16.

#include "spu2_internal.h"
#include "spu2_interpolate_table.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail::spu2
{
    // LOOP/END sets the ENDX bit and sets NAX to LSA, and the voice is muted if LOOP is not set
    // LOOP seems to only have any effect on the block with LOOP/END set, where it prevents muting the voice
    // (the documented requirement that every block in a loop has the LOOP bit set is nonsense according to tests)
    // LOOP/START sets LSA to NAX unless LSA was written manually since sound generation started
    // (see LoopMode, the method by which this is achieved on the real SPU2 is unknown)
    constexpr u32 XAFLAG_LOOP_END = 1u << 0;
    constexpr u32 XAFLAG_LOOP = 1u << 1;
    constexpr u32 XAFLAG_LOOP_START = 1u << 2;

    namespace
    {
        const s32 tbl_XA_Factor[16][2] = {
            {0, 0},
            {60, 0},
            {115, -52},
            {98, -55},
            {122, -60}};

        // Decodes one 16-byte SPU2 ADPCM block (header word + 14 bytes) into 28 samples.
        inline void XA_decode_block(s16 *buffer, const s16 *block, s32 &prev1, s32 &prev2)
        {
            const s32 header = *block;
            const s32 shift = (header & 0xF) + 16;
            const int id = header >> 4 & 0xF;
            const s32 pred1 = tbl_XA_Factor[id][0];
            const s32 pred2 = tbl_XA_Factor[id][1];

            const s8 *blockbytes = reinterpret_cast<const s8 *>(&block[1]);
            const s8 *blockend = &blockbytes[13];

            for (; blockbytes <= blockend; ++blockbytes)
            {
                s32 data = (static_cast<s32>(*blockbytes) << 28) & static_cast<s32>(0xF0000000u);
                s32 pcm = (data >> shift) + (((pred1 * prev1) + (pred2 * prev2) + 32) >> 6);

                pcm = std::clamp<s32>(pcm, -0x8000, 0x7fff);
                *(buffer++) = static_cast<s16>(pcm);

                data = (static_cast<s32>(*blockbytes) << 24) & static_cast<s32>(0xF0000000u);
                s32 pcm2 = (data >> shift) + (((pred1 * pcm) + (pred2 * prev1) + 32) >> 6);

                pcm2 = std::clamp<s32>(pcm2, -0x8000, 0x7fff);
                *(buffer++) = static_cast<s16>(pcm2);

                prev2 = pcm;
                prev1 = pcm2;
            }
        }

        inline s32 ApplyVolume(s32 data, s32 volume)
        {
            return (volume * data) >> 15;
        }

        inline StereoOut32 ApplyVolume(const StereoOut32 &data, const V_VolumeLR &volume)
        {
            return StereoOut32(
                ApplyVolume(data.Left, volume.Left),
                ApplyVolume(data.Right, volume.Right));
        }

        inline StereoOut32 ApplyVolume(const StereoOut32 &data, const V_VolumeSlideLR &volume)
        {
            return StereoOut32(
                ApplyVolume(data.Left, volume.Left.Value),
                ApplyVolume(data.Right, volume.Right.Value));
        }

        struct Mixer
        {
            Spu2System &sys;

            void IncrementNextA(V_Core &thiscore, uint voiceidx)
            {
                V_Voice &vc(thiscore.Voices[voiceidx]);

                // Important!  Both cores signal IRQ when an address is read, regardless of
                // which core actually reads the address.
                for (int i = 0; i < 2; i++)
                {
                    if (sys.Cores[i].IRQEnable && (vc.NextA == sys.Cores[i].IRQA))
                        sys.SetIrqCall(i);
                }

                vc.NextA++;
                vc.NextA &= 0xFFFFF;
            }

            void GetNextDataBuffered(V_Core &thiscore, uint voiceidx)
            {
                V_Voice &vc(thiscore.Voices[voiceidx]);

                if (vc.SBuffer == nullptr)
                {
                    const int cacheIdx = (vc.NextA & 0xFFFF8) / pcm_WordsPerBlock;
                    PcmCacheEntry &cacheLine = sys.pcm_cache[cacheIdx];
                    vc.SBuffer = cacheLine.Sampledata;

                    if (cacheLine.Validated && vc.Prev1 == cacheLine.Prev1 && vc.Prev2 == cacheLine.Prev2)
                    {
                        // Cached block!  Read from the cache directly.
                        // Make sure to propagate the prev1/prev2 ADPCM:
                        vc.Prev1 = vc.SBuffer[27];
                        vc.Prev2 = vc.SBuffer[26];
                    }
                    else
                    {
                        // Only flag the cache if it's a non-dynamic memory range.
                        if (vc.NextA >= static_cast<u32>(SPU2_DYN_MEMLINE))
                        {
                            cacheLine.Validated = true;
                            cacheLine.Prev1 = vc.Prev1;
                            cacheLine.Prev2 = vc.Prev2;
                        }

                        const s16 *memptr = sys.GetMemPtr(vc.NextA & 0xFFFF8);
                        XA_decode_block(vc.SBuffer, memptr, vc.Prev1, vc.Prev2);
                    }
                }

                // Get the sample index for NextA, we have to subtract 1 to ignore the loop header
                const int sampleIdx = ((vc.NextA % pcm_WordsPerBlock) - 1) * 4;
                for (int i = 0; i < 4; i++)
                {
                    vc.DecodeFifo[(vc.DecPosWrite + i) % 32] = vc.SBuffer[sampleIdx + i];
                }
            }

            void UpdateBlockHeader(V_Core &thiscore, uint voiceidx)
            {
                V_Voice &vc(thiscore.Voices[voiceidx]);

                for (int i = 0; i < 2; i++)
                    if (sys.Cores[i].IRQEnable && sys.Cores[i].IRQA == (vc.NextA & 0xFFFF8))
                        sys.SetIrqCall(i);

                const s16 *memptr = sys.GetMemPtr(vc.NextA & 0xFFFF8);
                vc.LoopFlags = static_cast<s8>(*memptr >> 8); // grab loop flags from the upper byte.

                if ((vc.LoopFlags & XAFLAG_LOOP_START) && !vc.LoopMode)
                {
                    vc.LoopStartA = vc.NextA & 0xFFFF8;
                }
            }

            void DecodeSamples(uint coreidx, uint voiceidx)
            {
                V_Core &thiscore(sys.Cores[coreidx]);
                V_Voice &vc(thiscore.Voices[voiceidx]);

                // Update the block header on every audio frame
                UpdateBlockHeader(thiscore, voiceidx);

                // When a voice is started at 0 pitch, NAX quickly advances to SSA + 5
                // So that would mean the decode buffer holds around 12 samples
                if (static_cast<int>(vc.DecPosWrite - vc.DecPosRead) > 12)
                {
                    // Sufficient data buffered
                    return;
                }

                if (vc.ADSR.Phase > V_ADSR::PHASE_STOPPED)
                {
                    GetNextDataBuffered(thiscore, voiceidx);
                }

                vc.DecPosWrite += 4;

                IncrementNextA(thiscore, voiceidx);
                if ((vc.NextA & 7) == 0)
                {
                    if (vc.LoopFlags & XAFLAG_LOOP_END)
                    {
                        thiscore.Regs.ENDX |= (1 << voiceidx);
                        vc.NextA = vc.LoopStartA;
                        if (!(vc.LoopFlags & XAFLAG_LOOP))
                        {
                            vc.Stop();
                        }
                    }

                    IncrementNextA(thiscore, voiceidx);
                    vc.SBuffer = nullptr;
                }
            }

            void UpdatePitch(uint coreidx, uint voiceidx)
            {
                V_Voice &vc(sys.Cores[coreidx].Voices[voiceidx]);
                s32 pitch;

                if ((vc.Modulated == 0) || (voiceidx == 0))
                    pitch = vc.Pitch;
                else
                    pitch = std::clamp((vc.Pitch * (32768 + sys.Cores[coreidx].Voices[voiceidx - 1].OutX)) >> 15, 0, 0x3fff);

                pitch = std::min(pitch, 0x3FFF);
                vc.SP += pitch;
            }

            void CalculateADSR(V_Core &thiscore, uint voiceidx)
            {
                V_Voice &vc(thiscore.Voices[voiceidx]);

                if (vc.ADSR.Phase == V_ADSR::PHASE_STOPPED)
                {
                    vc.ADSR.Value = 0;
                    return;
                }

                if (!vc.ADSR.Calculate())
                {
                    vc.Stop();
                }
            }

            static void ConsumeSamples(V_Core &thiscore, uint voiceidx)
            {
                V_Voice &vc(thiscore.Voices[voiceidx]);

                const int consumed = vc.SP >> 12;
                vc.SP &= 0xfff;
                vc.DecPosRead += consumed;
            }

            static s32 GetVoiceValues(V_Core &thiscore, uint voiceidx)
            {
                V_Voice &vc(thiscore.Voices[voiceidx]);

                const int phase = (vc.SP & 0x0ff0) >> 4;
                s32 out = 0;
                out += (interpTable[phase][0] * vc.DecodeFifo[(vc.DecPosRead + 0) % 32]) >> 15;
                out += (interpTable[phase][1] * vc.DecodeFifo[(vc.DecPosRead + 1) % 32]) >> 15;
                out += (interpTable[phase][2] * vc.DecodeFifo[(vc.DecPosRead + 2) % 32]) >> 15;
                out += (interpTable[phase][3] * vc.DecodeFifo[(vc.DecPosRead + 3) % 32]) >> 15;

                return out;
            }

            // This is Dr. Hell's noise algorithm as implemented in pcsxr
            // Supposedly this is 100% accurate
            static void UpdateNoise(V_Core &thiscore)
            {
                static const uint8_t noise_add[64] = {
                    1, 0, 0, 1, 0, 1, 1, 0,
                    1, 0, 0, 1, 0, 1, 1, 0,
                    1, 0, 0, 1, 0, 1, 1, 0,
                    1, 0, 0, 1, 0, 1, 1, 0,
                    0, 1, 1, 0, 1, 0, 0, 1,
                    0, 1, 1, 0, 1, 0, 0, 1,
                    0, 1, 1, 0, 1, 0, 0, 1,
                    0, 1, 1, 0, 1, 0, 0, 1};

                static const uint16_t noise_freq_add[5] = {
                    0, 84, 140, 180, 210};

                u32 level = 0x8000 >> (thiscore.NoiseClk >> 2);
                level <<= 16;

                thiscore.NoiseCnt += 0x10000;

                thiscore.NoiseCnt += noise_freq_add[thiscore.NoiseClk & 3];
                if ((thiscore.NoiseCnt & 0xffff) >= noise_freq_add[4])
                {
                    thiscore.NoiseCnt += 0x10000;
                    thiscore.NoiseCnt -= noise_freq_add[thiscore.NoiseClk & 3];
                }

                if (thiscore.NoiseCnt >= level)
                {
                    while (thiscore.NoiseCnt >= level)
                        thiscore.NoiseCnt -= level;

                    thiscore.NoiseOut = (thiscore.NoiseOut << 1) | noise_add[(thiscore.NoiseOut >> 10) & 63];
                }
            }

            static s32 GetNoiseValues(V_Core &thiscore)
            {
                return static_cast<s16>(thiscore.NoiseOut);
            }

            // writes a signed value to the SPU2 ram
            // Performs no cache invalidation -- use only for dynamic memory ranges
            // of the SPU2 (between 0x0000 and SPU2_DYN_MEMLINE)
            void spu2M_WriteFast(u32 addr, s32 value)
            {
                for (int i = 0; i < 2; i++)
                {
                    if (sys.Cores[i].IRQEnable && sys.Cores[i].IRQA == addr)
                        sys.SetIrqCall(i);
                }
                *sys.GetMemPtr(addr) = static_cast<s16>(value);
            }

            StereoOut32 MixVoice(uint coreidx, uint voiceidx)
            {
                V_Core &thiscore(sys.Cores[coreidx]);
                V_Voice &vc(thiscore.Voices[voiceidx]);

                vc.Volume.Update();

                DecodeSamples(coreidx, voiceidx);

                StereoOut32 voiceOut(0, 0);
                s32 Value = 0;

                if (vc.ADSR.Phase > V_ADSR::PHASE_STOPPED)
                {
                    if (vc.Noise)
                        Value = GetNoiseValues(thiscore);
                    else
                        Value = GetVoiceValues(thiscore, voiceidx);

                    // Update and Apply ADSR  (applies to normal and noise sources)
                    CalculateADSR(thiscore, voiceidx);
                    Value = ApplyVolume(Value, vc.ADSR.Value);
                    vc.OutX = Value;

                    voiceOut = ApplyVolume(StereoOut32(Value, Value), vc.Volume);
                }

                // SPU2 Note: The spu2 continues to process voices for eternity, always, so we
                // have to run through all the motions of updating the voice regardless of it's
                // audible status.  Otherwise IRQs might not trigger and emulation might fail.
                UpdatePitch(coreidx, voiceidx);

                ConsumeSamples(thiscore, voiceidx);

                // Write-back of raw voice data (post ADSR applied)
                if (voiceidx == 1)
                    spu2M_WriteFast(((0 == coreidx) ? 0x400 : 0xc00) + sys.OutPos, Value);
                else if (voiceidx == 3)
                    spu2M_WriteFast(((0 == coreidx) ? 0x600 : 0xe00) + sys.OutPos, Value);

                return voiceOut;
            }

            void MixCoreVoices(VoiceMixSet &dest, const uint coreidx)
            {
                V_Core &thiscore(sys.Cores[coreidx]);

                for (uint voiceidx = 0; voiceidx < V_Core::NumVoices; ++voiceidx)
                {
                    const StereoOut32 VVal(MixVoice(coreidx, voiceidx));

                    // Note: Results from MixVoice are ranged at 16 bits.
                    dest.Dry.Left += VVal.Left & thiscore.VoiceGates[voiceidx].DryL;
                    dest.Dry.Right += VVal.Right & thiscore.VoiceGates[voiceidx].DryR;
                    dest.Wet.Left += VVal.Left & thiscore.VoiceGates[voiceidx].WetL;
                    dest.Wet.Right += VVal.Right & thiscore.VoiceGates[voiceidx].WetR;
                }
            }

            StereoOut32 MixCore(const uint coreidx, const VoiceMixSet &inVoices, const StereoOut32 &Input, const StereoOut32 &Ext)
            {
                V_Core &thiscore(sys.Cores[coreidx]);

                thiscore.MasterVol.Update();
                UpdateNoise(thiscore);

                // Saturate final result to standard 16 bit range.
                const VoiceMixSet Voices(clamp_mix(inVoices.Dry), clamp_mix(inVoices.Wet));

                // Write Mixed results To Output Area
                spu2M_WriteFast(((0 == thiscore.Index) ? 0x1000 : 0x1800) + sys.OutPos, Voices.Dry.Left);
                spu2M_WriteFast(((0 == thiscore.Index) ? 0x1200 : 0x1A00) + sys.OutPos, Voices.Dry.Right);
                spu2M_WriteFast(((0 == thiscore.Index) ? 0x1400 : 0x1C00) + sys.OutPos, Voices.Wet.Left);
                spu2M_WriteFast(((0 == thiscore.Index) ? 0x1600 : 0x1E00) + sys.OutPos, Voices.Wet.Right);

                // Mix in the Input data
                StereoOut32 TD(
                    Input.Left & thiscore.DryGate.InpL,
                    Input.Right & thiscore.DryGate.InpR);

                // Mix in the Voice data
                TD.Left += Voices.Dry.Left & thiscore.DryGate.SndL;
                TD.Right += Voices.Dry.Right & thiscore.DryGate.SndR;

                // Mix in the External (nothing/core0) data
                TD.Left += Ext.Left & thiscore.DryGate.ExtL;
                TD.Right += Ext.Right & thiscore.DryGate.ExtR;

                // Reverberation: Mix Input, Voice, and External data:
                StereoOut32 TW;
                TW.Left = Input.Left & thiscore.WetGate.InpL;
                TW.Right = Input.Right & thiscore.WetGate.InpR;

                TW.Left += Voices.Wet.Left & thiscore.WetGate.SndL;
                TW.Right += Voices.Wet.Right & thiscore.WetGate.SndR;
                TW.Left += Ext.Left & thiscore.WetGate.ExtL;
                TW.Right += Ext.Right & thiscore.WetGate.ExtR;

                const StereoOut32 RV = thiscore.DoReverb(TW);

                // Mix Dry + Wet
                // (master volume is applied later to the result of both outputs added together).
                return TD + ApplyVolume(RV, thiscore.FxVol);
            }
        };
    }

    void DecodeAdpcmBlock(const u8 block[16], s16 out[28], s32 &prev1, s32 &prev2)
    {
        s16 words[8];
        std::memcpy(words, block, sizeof(words));
        XA_decode_block(out, words, prev1, prev2);
    }

    // ReadInput.cpp: ADMA / sound data input for one core.
    StereoOut32 V_Core::ReadInput()
    {
        StereoOut32 retval;
        u16 ReadIndex = Sys->OutPos;

        for (int i = 0; i < 2; i++)
            if (Sys->Cores[i].IRQEnable && (0x2000 + (Index << 10) + ReadIndex) == (Sys->Cores[i].IRQA & 0xfffffdff))
                Sys->SetIrqCall(static_cast<int>(i));

        // PlayMode & 2 is Bypass Mode, so it doesn't go through the SPU
        if ((Index == 1) || !(Index == 0 && (Sys->PlayMode & 2) != 0))
        {
            retval = StereoOut32(
                static_cast<s32>(*Sys->GetMemPtr(0x2000 + (Index << 10) + ReadIndex)),
                static_cast<s32>(*Sys->GetMemPtr(0x2200 + (Index << 10) + ReadIndex)));
        }

        // Simulate MADR increase, GTA VC tracks the MADR address for calculating a certain point in the buffer
        if (InputDataTransferred)
        {
            const u32 amount = std::min(InputDataTransferred, static_cast<u32>(0x180));

            InputDataTransferred -= amount;
            MADR() += amount;
            // Because some games watch the MADR to see when it reaches the end we need to end the DMA here
            // Tom & Jerry War of the Whiskers is one such game, the music will skip
            if (!InputDataTransferred && !InputDataLeft)
            {
                if (Index == 0)
                    Sys->spu2DMA4Irq();
                else
                    Sys->spu2DMA7Irq();
            }
        }

        if (Sys->PlayMode == 2 && Index == 0) // Bitstream bypass refills twice as quickly (GTA VC)
            ReadIndex = (ReadIndex * 2) & 0x1FF;

        if (ReadIndex == 0x100 || ReadIndex == 0x0 || ReadIndex == 0x80 || ReadIndex == 0x180)
        {
            if (ReadIndex == 0x100)
                InputPosWrite = 0;
            else if (ReadIndex == 0)
                InputPosWrite = 0x100;

            if (InputDataLeft >= 0x100)
            {
                AutoDMAReadBuffer(0);
                AdmaInProgress = true;
                if (InputDataLeft < 0x100)
                {
                    InputDataLeft = 0;
                }
            }
            else if ((AutoDMACtrl & (Index + 1)))
                AutoDMACtrl |= ~3;
        }
        return retval;
    }

    void Spu2System::Mix()
    {
        Mixer mixer{*this};

        // Note: Playmode 4 is SPDIF, which overrides other inputs.
        const StereoOut32 InputData[2] = {
            // SPDIF is on Core 0 (no AC3 decoder; games usually provide a normal ADMA stream too)
            ApplyVolume(Cores[0].ReadInput(), Cores[0].InpVol),
            // CDDA is on Core 1:
            (PlayMode & 8) ? StereoOut32() : ApplyVolume(Cores[1].ReadInput(), Cores[1].InpVol)};

        VoiceMixSet VoiceData[2] = {{StereoOut32(), StereoOut32()}, {StereoOut32(), StereoOut32()}}; // mixed voice data for each core.

        mixer.MixCoreVoices(VoiceData[0], 0);
        mixer.MixCoreVoices(VoiceData[1], 1);

        StereoOut32 Ext(mixer.MixCore(0, VoiceData[0], InputData[0], StereoOut32()));

        if ((PlayMode & 4) || (Cores[0].Mute != 0))
            Ext = StereoOut32();
        else
        {
            Ext = ApplyVolume(clamp_mix(Ext), Cores[0].MasterVol);
        }

        // Commit Core 0 output to ram before mixing Core 1:
        mixer.spu2M_WriteFast(0x800 + OutPos, Ext.Left);
        mixer.spu2M_WriteFast(0xA00 + OutPos, Ext.Right);

        Ext = ApplyVolume(Ext, Cores[1].ExtVol);
        StereoOut32 Out(mixer.MixCore(1, VoiceData[1], InputData[1], Ext));

        if (PlayMode & 8)
        {
            // CDDA (PCM24 input on core 1) is not supported; output silence instead.
            Out = StereoOut32();
        }
        else
        {
            Out = ApplyVolume(clamp_mix(Out), Cores[1].MasterVol);
        }

        Output32(Out);

        // Update AutoDMA output positioning
        OutPos++;
        if (OutPos >= 0x200)
            OutPos = 0;
        ++SamplesMixed;
        if (RamDumpPrefix && SamplesMixed % (48000u * 5u) == 0u)
            DumpState();
    }

    // spu2.cpp: spu2Output() with its DC blocking filter, writing s16 frames.
    void Spu2System::Output32(StereoOut32 out)
    {
        float conv[2];

        conv[0] = static_cast<float>(clamp_mix(out.Left)) / INT16_MAX;
        conv[1] = static_cast<float>(clamp_mix(out.Right)) / INT16_MAX;

        // Some games pause voices with the volume left on leaving us with
        // significant DC offset, so we filter it out (e.x. SSX 3)
        // A simple DC blocking high-pass filter
        // Implementation from http://peabody.sapp.org/class/dmp2/lab/dcblock/
        float output[2];
        output[0] = (conv[0] - DCFilterIn[0] + ((0.995f * DCFilterOut[0])));
        output[1] = (conv[1] - DCFilterIn[1] + ((0.995f * DCFilterOut[1])));
        DCFilterIn[0] = conv[0];
        DCFilterIn[1] = conv[1];
        DCFilterOut[0] = output[0];
        DCFilterOut[1] = output[1];

        if (!Output)
            return;

        if (OutBuf.size() != OutChunkFrames * 2)
            OutBuf.assign(OutChunkFrames * 2, 0);
        for (int c = 0; c < 2; ++c)
        {
            const float scaled = output[c] * 32767.0f;
            const float clamped = scaled < -32768.0f ? -32768.0f : (scaled > 32767.0f ? 32767.0f : scaled);
            OutBuf[OutBufFrames * 2 + static_cast<size_t>(c)] = static_cast<s16>(clamped);
        }
        if (++OutBufFrames == OutChunkFrames)
            FlushOutput();
    }

    void Spu2System::FlushOutput()
    {
        if (OutBufFrames != 0 && Output)
            Output(OutBuf.data(), OutBufFrames);
        OutBufFrames = 0;
    }
}
