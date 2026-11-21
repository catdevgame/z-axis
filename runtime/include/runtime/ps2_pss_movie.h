#pragma once

// Sony PSS / PSM movies read on the host: an MPEG-2 program stream with the
// picture in stream 0xE0 (MPEG-2 video, decoded with FFmpeg to the same
// pixels on every host; without it, black pictures as many as FFmpeg gives)
// and, in a PSS,
// the sound in private stream 1 (0xBD): after a 4-byte sub-stream header per
// packet, an "SShd" header (type, rate, channels, interleave) and an "SSbd"
// block of 16-bit little-endian PCM, the channels interleaved in blocks of
// `interleave` bytes. A PSM has no sound.
//
// FileVideo plays any movie file FFmpeg demuxes (the Xbox release's Bink
// movies), for the same player.

#include "runtime/ps2_game_files.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zx::movie
{
    // The payloads of one stream, in file order.
    class PssPackets
    {
    public:
        PssPackets(std::shared_ptr<zx::game_files::GameFile> file, uint8_t streamId);
        // The next payload of the stream (private stream 1: without its
        // 4-byte sub-stream header); false at the program end or the end of
        // the file.
        bool next(std::vector<uint8_t> &payload);
        void rewind();

    private:
        const uint8_t *at(uint64_t position, size_t size);

        std::shared_ptr<zx::game_files::GameFile> m_file;
        uint8_t m_streamId = 0u;
        uint64_t m_position = 0u;
        uint64_t m_bufferStart = 0u;
        std::vector<uint8_t> m_buffer;
    };

    struct Picture
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba; // width * height * 4
    };

    // Pictures of a movie, in display order.
    class VideoSource
    {
    public:
        virtual ~VideoSource() = default;
        // The next picture; false after the last one (or without a decoder).
        virtual bool next(Picture &picture) = 0;
        virtual void rewind() = 0;
        [[nodiscard]] virtual double framesPerSecond() const = 0;
    };

    // The pictures FFmpeg's MPEG-2 decoder gives for a video stream, counted
    // from the stream's headers alone (no picture is decoded): its sequence
    // header's size, and for each picture whether the decoder shows one, by
    // the decoder's own rules (FFmpeg 9.0's mpeg12dec.c and mpegvideo_dec.c):
    // a B-picture with no earlier reference in an open GOP is skipped, and an
    // I- or P-picture is shown one reference later (at the end of the stream
    // for the last), a B-picture at once. So a build without a decoder plays
    // each movie for as many pictures, and the game's timeline after it is
    // the same.
    class PssPictureCount
    {
    public:
        // The next bytes of the video stream, split anywhere.
        void feed(const uint8_t *data, size_t size);
        // The end of the stream: the last reference picture is shown.
        void finish();
        // Takes one picture the decoder would have shown by now.
        bool take();
        [[nodiscard]] int width() const noexcept { return m_width; }
        [[nodiscard]] int height() const noexcept { return m_height; }
        // From the sequence header's frame rate code; 0 before one.
        [[nodiscard]] double framesPerSecond() const noexcept { return m_fps; }

    private:
        enum class Reference : uint8_t { none, dummy, picture };
        // One start code's header at `p` (after 00 00 01, its code first),
        // `size` bytes of it held: false when the header needs more.
        bool header(const uint8_t *p, size_t size);
        void pictureStarts();

        std::vector<uint8_t> m_pending;
        int m_width = 0;
        int m_height = 0;
        double m_fps = 0.0;
        bool m_lowDelay = false;
        bool m_closedGop = false;
        bool m_sync = false;
        bool m_afterPicture = false; // a picture header and no slice yet
        bool m_firstField = false;   // the first of two field pictures
        uint8_t m_type = 0u;         // 1 I, 2 P, 3 B
        uint8_t m_structure = 3u;    // 3 a frame, 1 or 2 a field
        Reference m_last = Reference::none;
        Reference m_next = Reference::none;
        size_t m_shown = 0u;
    };

    // With the decoder, the pictures are decoded ahead of next() (two at
    // most) on a thread of the video's own, from its first next() on: the
    // MPEG-2 decode and the RGBA conversion are off the thread that plays
    // the movie (the EE thread), and next() gives the same pictures, and
    // framesPerSecond() the same rate, as decoding there.
    class PssVideo final : public VideoSource
    {
    public:
        // `decode` false: black pictures counted from the headers
        // (PssPictureCount), as a build without an MPEG-2 decoder always gives.
        explicit PssVideo(std::shared_ptr<zx::game_files::GameFile> file, bool decode = true);
        ~PssVideo() override;
        PssVideo(const PssVideo &) = delete;
        PssVideo &operator=(const PssVideo &) = delete;

        // The next picture in display order; false after the last one.
        // Without an MPEG-2 decoder each is black, at the stream's size, and
        // there are as many as a decoder gives.
        bool next(Picture &picture) override;
        void rewind() override;
        // From the sequence header (29.97 for every movie on this disc), as
        // read by the time the last picture next() gave was decoded.
        [[nodiscard]] double framesPerSecond() const override { return m_fps; }

    private:
        struct Decoder;
        // A picture decoded ahead, and the rate read by then.
        struct Decoded
        {
            Picture picture;
            double fps = 0.0;
        };
        static constexpr size_t kAhead = 2u;

        void openDecoder();
        void feed(const uint8_t *data, size_t size);
        void flush();
        bool decodeNext(Picture &picture);
        bool nextBlack(Picture &picture);
        void decodeAhead();
        void stopAhead();

        PssPackets m_packets;
        std::unique_ptr<Decoder> m_decoder;
        std::unique_ptr<PssPictureCount> m_count; // without a decoder
        std::deque<Picture> m_ready;
        double m_fps = 30000.0 / 1001.0;
        double m_decodedFps = 30000.0 / 1001.0; // the decoding thread's (feed)
        bool m_decode = true;
        bool m_flushed = false;

        // The decoding thread and what it hands over.
        std::thread m_aheadThread;
        std::mutex m_aheadMutex;
        std::condition_variable m_aheadCv;
        std::deque<Decoded> m_ahead;
        bool m_aheadEnded = false; // the last picture is in m_ahead
        double m_endFps = 0.0;     // the rate read by the end of the stream
        bool m_aheadStop = false;
    };

    // A movie file FFmpeg demuxes and decodes (needs libavformat in the
    // build; Bink for the Xbox movies). next() gives the file's pictures, or
    // with setScaledSize pictures of that size, and full() the picture at
    // the file's own size from the same call.
    class FileVideo final : public VideoSource
    {
    public:
        explicit FileVideo(std::filesystem::path path);
        ~FileVideo() override;
        FileVideo(const FileVideo &) = delete;
        FileVideo &operator=(const FileVideo &) = delete;

        [[nodiscard]] bool ok() const noexcept { return m_impl != nullptr; }
        [[nodiscard]] int width() const noexcept { return m_width; }
        [[nodiscard]] int height() const noexcept { return m_height; }
        void setScaledSize(int width, int height);
        bool next(Picture &picture) override;
        [[nodiscard]] const Picture &full() const noexcept { return m_full; }
        void rewind() override;
        [[nodiscard]] double framesPerSecond() const override { return m_fps; }

    private:
        struct Impl;
        bool open();
        std::filesystem::path m_path;
        std::unique_ptr<Impl> m_impl;
        Picture m_full;
        int m_width = 0, m_height = 0;
        int m_scaledWidth = 0, m_scaledHeight = 0;
        double m_fps = 30000.0 / 1001.0;
    };

    class PssAudio
    {
    public:
        static constexpr uint32_t kOutputRate = 48000u;

        explicit PssAudio(std::shared_ptr<zx::game_files::GameFile> file);

        // The stream has an "SShd" PCM header this reader understands.
        [[nodiscard]] bool hasSound();
        // Up to `frames` interleaved stereo frames at 48 kHz; fewer at the end.
        size_t read(int16_t *stereo, size_t frames);
        void rewind();

    private:
        bool parseHeader();
        bool decodeBlock();

        PssPackets m_packets;
        std::vector<uint8_t> m_raw; // payload bytes not used yet
        size_t m_rawOffset = 0u;
        bool m_headerTried = false;
        bool m_headerOk = false;
        uint32_t m_rate = 0u;
        uint32_t m_channels = 0u;
        uint32_t m_interleave = 0u;
        uint64_t m_dataLeft = 0u;
        bool m_streamEnded = false;
        std::vector<int16_t> m_samples; // decoded stereo frames at the source rate
        size_t m_sampleOffset = 0u;     // in frames
        double m_phase = 0.0;           // resampling position within m_samples
    };
}
