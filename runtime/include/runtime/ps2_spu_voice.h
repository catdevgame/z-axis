#pragma once

// A sound voice that plays like one of the SPU2's, for native sound drivers
// that replace a game's IOP sound module: PS-ADPCM samples decoded to PCM
// when they are loaded, the SPU2's pitch counter and 4-tap gaussian
// interpolation, its ADSR envelope (the rates and phases of PCSX2's
// ADSR.cpp) and its voice volumes, at 48 kHz. A driver keys voices on and
// off, sets their registers as the game's driver set the SPU2's, reads the
// envelope back (ENVX) and mixes them where it likes; no SPU RAM, DMA or
// IRQs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace zx::spu
{
    constexpr size_t kBlockBytes = 16;
    constexpr size_t kSamplesPerBlock = 28;

    // PS-ADPCM block flags (byte 1 of a block).
    constexpr uint8_t kFlagLoopEnd = 1u;   // end of the sample: jump to the loop start
    constexpr uint8_t kFlagLoop = 2u;      // with kFlagLoopEnd: keep playing (else the voice stops)
    constexpr uint8_t kFlagLoopStart = 4u; // the loop starts at this block

    // One 16-byte block to 28 samples; prev1/prev2 carry the filter history.
    void decodeBlock(const uint8_t *block, int16_t *out, int32_t &prev1, int32_t &prev2);

    // A sample as a voice keyed on at its first block plays it.
    struct DecodedSample
    {
        // The blocks up to and including the one with the end flag, decoded
        // in order from silence.
        std::vector<int16_t> pcm;
        // The loop (loop start block to the end block) decoded again with
        // the filter history the jump back brings; empty if it does not loop.
        std::vector<int16_t> loopPcm;
        // No end flag in the data: the SPU2 would go on into whatever follows
        // in its memory; here the voice plays silence until keyed off.
        bool endless = false;
        // Block index of the loop start in pcm (for tests and logs).
        size_t loopStartBlock = 0;
    };

    std::shared_ptr<const DecodedSample> decodeSample(const uint8_t *adpcm, size_t bytes);

    // The SPU2's ADSR envelope (PCSX2 ADSR.cpp: phases, rates, targets).
    class Envelope
    {
    public:
        enum Phase : uint8_t
        {
            Stopped = 0,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        void setRegisters(uint16_t adsr1, uint16_t adsr2);
        void attack();
        void release();
        void stop();
        // One sample; false when the envelope has ended (the voice stops).
        bool step();

        [[nodiscard]] int32_t value() const noexcept { return m_value; } // ENVX, 0..0x7FFF
        [[nodiscard]] Phase phase() const noexcept { return m_phase; }

    private:
        struct Rate
        {
            bool decrease = false;
            bool exponential = false;
            uint8_t shift = 0;
            int8_t step = 0;
            int32_t target = 0;
        };

        std::array<Rate, 5> m_rates{};
        uint32_t m_counter = 0;
        int32_t m_value = 0;
        Phase m_phase = Stopped;
    };

    // Where a voice gets its samples from, one block at a time.
    class BlockSource
    {
    public:
        virtual ~BlockSource() = default;
        // The next 28 samples. False when the voice stops here (the end of a
        // sample that does not loop, or of a stream).
        virtual bool nextBlock(int16_t *out) = 0;
    };

    // Plays a DecodedSample: pcm, then loopPcm over and over if it loops.
    class SampleSource final : public BlockSource
    {
    public:
        explicit SampleSource(std::shared_ptr<const DecodedSample> sample);
        bool nextBlock(int16_t *out) override;

    private:
        std::shared_ptr<const DecodedSample> m_sample;
        size_t m_position = 0;
        bool m_looping = false;
    };

    class Voice
    {
    public:
        // Register values as the SPU2 takes them: pitch 0x1000 = 48 kHz
        // (at most 0x3FFF), volumes as VOLL/VOLR without the sweep mode
        // (0x3FFF is full, negative inverts), ADSR1/ADSR2.
        void setPitch(uint16_t pitch) noexcept { m_pitch = pitch; }
        void setVolume(uint16_t left, uint16_t right) noexcept;
        void setEnvelope(uint16_t adsr1, uint16_t adsr2) { m_envelope.setRegisters(adsr1, adsr2); }

        // Key on: the source from its start, envelope from silence.
        void keyOn(std::unique_ptr<BlockSource> source);
        // Key off: the envelope's release.
        void keyOff() { m_envelope.release(); }
        // Silent at once (envelope 0), as the SPU2 at a sample's end.
        void stop();

        [[nodiscard]] bool active() const noexcept { return m_envelope.phase() != Envelope::Stopped; }
        [[nodiscard]] int32_t envelope() const noexcept { return m_envelope.value(); }
        [[nodiscard]] uint16_t pitch() const noexcept { return m_pitch; }

        // Adds one 48 kHz output sample (16-bit range per voice) to left/right.
        void mix(int32_t &left, int32_t &right);

    private:
        void decode();

        std::unique_ptr<BlockSource> m_source;
        Envelope m_envelope;
        std::array<int16_t, 32> m_fifo{};
        std::array<int16_t, kSamplesPerBlock> m_block{};
        uint32_t m_blockPosition = kSamplesPerBlock; // next sample of m_block to queue
        uint32_t m_fifoRead = 0;
        uint32_t m_fifoWrite = 0;
        uint32_t m_counter = 0; // pitch counter, 12-bit fraction
        uint16_t m_pitch = 0;
        int32_t m_volumeLeft = 0;
        int32_t m_volumeRight = 0;
        bool m_sourceEnded = false;
    };
}
