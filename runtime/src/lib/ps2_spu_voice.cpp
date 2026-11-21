// SPDX-License-Identifier: GPL-3.0+
//
// The ADPCM decoding, interpolation and envelope follow PCSX2's SPU2
// (Mixer.cpp, ADSR.cpp at commit 091ab687, as iop/'s port has them), so
// a native voice sounds and keys off like the emulated one.

#include "runtime/ps2_spu_voice.h"

#include "sound/spu_gauss_table.h"

#include <algorithm>
#include <cstring>

namespace zx::spu
{
    namespace
    {
        constexpr int32_t kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        constexpr int32_t kEnvelopeMax = 0x7FFF;
    }

    void decodeBlock(const uint8_t *block, int16_t *out, int32_t &prev1, int32_t &prev2)
    {
        const int shift = (block[0] & 0xF) + 16;
        // Filters 5-15 do not exist; PCSX2's table has zeros there.
        const int filter = block[0] >> 4 & 0xF;
        const int32_t f1 = filter <= 4 ? kFilter[filter][0] : 0;
        const int32_t f2 = filter <= 4 ? kFilter[filter][1] : 0;
        for (size_t i = 0; i < 14; ++i)
        {
            const int8_t byte = static_cast<int8_t>(block[2 + i]);
            const int32_t low = static_cast<int32_t>((static_cast<uint32_t>(static_cast<int32_t>(byte)) << 28) & 0xF0000000u);
            int32_t pcm = (low >> shift) + ((f1 * prev1 + f2 * prev2 + 32) >> 6);
            pcm = std::clamp<int32_t>(pcm, -0x8000, 0x7FFF);
            const int32_t high = static_cast<int32_t>((static_cast<uint32_t>(static_cast<int32_t>(byte)) << 24) & 0xF0000000u);
            int32_t pcm2 = (high >> shift) + ((f1 * pcm + f2 * prev1 + 32) >> 6);
            pcm2 = std::clamp<int32_t>(pcm2, -0x8000, 0x7FFF);
            out[i * 2] = static_cast<int16_t>(pcm);
            out[i * 2 + 1] = static_cast<int16_t>(pcm2);
            prev2 = pcm;
            prev1 = pcm2;
        }
    }

    std::shared_ptr<const DecodedSample> decodeSample(const uint8_t *adpcm, size_t bytes)
    {
        auto sample = std::make_shared<DecodedSample>();
        const size_t blocks = adpcm ? bytes / kBlockBytes : 0u;
        sample->pcm.reserve(blocks * kSamplesPerBlock);
        int32_t prev1 = 0;
        int32_t prev2 = 0;
        size_t loopStart = 0;
        size_t end = blocks;
        for (size_t b = 0; b < blocks; ++b)
        {
            const uint8_t *block = adpcm + b * kBlockBytes;
            // The SPU2 takes the loop start from the block header while it
            // plays that block (unless the loop address was written, which
            // the drivers this is for never do).
            if (block[1] & kFlagLoopStart)
                loopStart = b;
            const size_t at = sample->pcm.size();
            sample->pcm.resize(at + kSamplesPerBlock);
            decodeBlock(block, sample->pcm.data() + at, prev1, prev2);
            if (block[1] & kFlagLoopEnd)
            {
                end = b;
                break;
            }
        }
        sample->loopStartBlock = loopStart;
        if (end == blocks)
        {
            sample->endless = true;
            return sample;
        }
        if (adpcm[end * kBlockBytes + 1] & kFlagLoop)
        {
            sample->loopPcm.resize((end - loopStart + 1u) * kSamplesPerBlock);
            for (size_t b = loopStart; b <= end; ++b)
                decodeBlock(adpcm + b * kBlockBytes, sample->loopPcm.data() + (b - loopStart) * kSamplesPerBlock, prev1, prev2);
        }
        return sample;
    }

    // ---- envelope ----------------------------------------------------------------

    void Envelope::setRegisters(uint16_t adsr1, uint16_t adsr2)
    {
        const uint8_t sustainLevel = adsr1 & 0xFu;
        const uint8_t decayShift = (adsr1 >> 4) & 0xFu;
        const uint8_t attackStep = (adsr1 >> 8) & 0x3u;
        const uint8_t attackShift = (adsr1 >> 10) & 0x1Fu;
        const bool attackExp = (adsr1 >> 15) & 1u;
        const uint8_t releaseShift = adsr2 & 0x1Fu;
        const bool releaseExp = (adsr2 >> 5) & 1u;
        const uint8_t sustainStep = (adsr2 >> 6) & 0x3u;
        const uint8_t sustainShift = (adsr2 >> 8) & 0x1Fu;
        const bool sustainDecrease = (adsr2 >> 14) & 1u;
        const bool sustainExp = (adsr2 >> 15) & 1u;

        m_rates[Attack] = {false, attackExp, attackShift, static_cast<int8_t>(7 - attackStep), kEnvelopeMax};
        m_rates[Decay] = {true, true, decayShift, -8, (sustainLevel + 1) << 11};
        int8_t sustain = static_cast<int8_t>(7 - sustainStep);
        if (sustainDecrease)
            sustain = static_cast<int8_t>(~sustain);
        m_rates[Sustain] = {sustainDecrease, sustainExp, sustainShift, sustain, 0};
        m_rates[Release] = {true, releaseExp, releaseShift, -8, 0};
    }

    void Envelope::attack()
    {
        m_phase = Attack;
        m_counter = 0;
        m_value = 0;
    }

    void Envelope::release()
    {
        if (m_phase != Stopped)
        {
            m_phase = Release;
            m_counter = 0;
        }
    }

    void Envelope::stop()
    {
        m_phase = Stopped;
        m_value = 0;
    }

    bool Envelope::step()
    {
        const Rate &rate = m_rates[m_phase];
        uint32_t counterStep = 0x8000u >> std::max(0, rate.shift - 11);
        int32_t levelStep = rate.step * (1 << std::max(0, 11 - rate.shift));
        if (rate.exponential)
        {
            if (!rate.decrease && m_value > 0x6000)
                counterStep >>= 2;
            if (rate.decrease)
                levelStep = static_cast<int16_t>((levelStep * m_value) >> 15);
        }
        counterStep = std::max<uint32_t>(1u, counterStep);
        m_counter += counterStep;
        if (m_counter >= 0x8000u)
        {
            m_counter = 0;
            m_value = std::clamp<int32_t>(m_value + levelStep, 0, kEnvelopeMax);
        }
        if (m_phase == Sustain)
            return m_value != 0;
        if ((!rate.decrease && m_value >= rate.target) || (rate.decrease && m_value <= rate.target))
            m_phase = static_cast<Phase>(m_phase + 1);
        return m_phase <= Release;
    }

    // ---- sources and voices ------------------------------------------------------

    SampleSource::SampleSource(std::shared_ptr<const DecodedSample> sample)
        : m_sample(std::move(sample))
    {
    }

    bool SampleSource::nextBlock(int16_t *out)
    {
        if (!m_sample)
            return false;
        if (!m_looping)
        {
            if (m_position + kSamplesPerBlock <= m_sample->pcm.size())
            {
                std::memcpy(out, m_sample->pcm.data() + m_position, kSamplesPerBlock * sizeof(int16_t));
                m_position += kSamplesPerBlock;
                return true;
            }
            if (m_sample->endless)
            {
                std::fill(out, out + kSamplesPerBlock, int16_t{0});
                return true;
            }
            if (m_sample->loopPcm.empty())
                return false;
            m_looping = true;
            m_position = 0;
        }
        std::memcpy(out, m_sample->loopPcm.data() + m_position, kSamplesPerBlock * sizeof(int16_t));
        m_position += kSamplesPerBlock;
        if (m_position >= m_sample->loopPcm.size())
            m_position = 0;
        return true;
    }

    void Voice::setVolume(uint16_t left, uint16_t right) noexcept
    {
        m_volumeLeft = static_cast<int16_t>(static_cast<uint16_t>(left << 1));
        m_volumeRight = static_cast<int16_t>(static_cast<uint16_t>(right << 1));
    }

    void Voice::keyOn(std::unique_ptr<BlockSource> source)
    {
        m_source = std::move(source);
        m_fifo.fill(0);
        m_fifoRead = 0;
        m_fifoWrite = 0;
        m_counter = 0;
        m_sourceEnded = false;
        m_envelope.attack();
        m_blockPosition = 0;
        if (!m_source || !m_source->nextBlock(m_block.data()))
            stop();
    }

    void Voice::stop()
    {
        m_envelope.stop();
        m_sourceEnded = true;
    }

    // PCSX2 DecodeSamples: keeps about 12 samples queued, 4 at a time; at the
    // end of a block the next one is taken (or the voice stops) right away.
    void Voice::decode()
    {
        if (static_cast<int32_t>(m_fifoWrite - m_fifoRead) > 12)
            return;
        for (uint32_t i = 0; i < 4u; ++i)
            m_fifo[(m_fifoWrite + i) % 32u] = m_block[m_blockPosition + i];
        m_fifoWrite += 4u;
        m_blockPosition += 4u;
        if (m_blockPosition >= kSamplesPerBlock)
        {
            m_blockPosition = 0;
            if (!m_source || !m_source->nextBlock(m_block.data()))
                stop();
        }
    }

    void Voice::mix(int32_t &left, int32_t &right)
    {
        if (!active())
            return;
        decode();
        if (!active())
            return;
        const auto &taps = kGaussTable[(m_counter & 0x0FF0u) >> 4];
        int32_t value = 0;
        for (uint32_t i = 0; i < 4u; ++i)
            value += (taps[i] * m_fifo[(m_fifoRead + i) % 32u]) >> 15;
        if (!m_envelope.step())
            stop();
        value = (value * m_envelope.value()) >> 15;
        left += (value * m_volumeLeft) >> 15;
        right += (value * m_volumeRight) >> 15;
        m_counter += std::min<uint32_t>(m_pitch, 0x3FFFu);
        m_fifoRead += m_counter >> 12;
        m_counter &= 0xFFFu;
    }
}
