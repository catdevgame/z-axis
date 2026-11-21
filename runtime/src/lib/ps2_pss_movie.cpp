#include "runtime/ps2_pss_movie.h"

#if !defined(ZX_HAS_FFMPEG)
#define ZX_HAS_FFMPEG 1
#endif

#if ZX_HAS_FFMPEG
extern "C"
{
#if ZX_HAS_AVFORMAT
#include <libavformat/avformat.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}
#endif

#include <algorithm>
#include <cstring>
#include <iostream>

namespace zx::movie
{
    namespace
    {
        constexpr size_t kReadChunk = 64u * 1024u;
        constexpr uint8_t kPackStart = 0xBAu;
        constexpr uint8_t kProgramEnd = 0xB9u;
        constexpr uint8_t kSystemHeader = 0xBBu;
        constexpr uint8_t kPrivateStream1 = 0xBDu;

        uint32_t le32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        // An MPEG-2 sequence header's frame_rate_code (1..8) as frames per
        // second; 0 for another code.
        double frameRate(uint8_t code)
        {
            static constexpr double kRates[] = {0.0, 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0, 50.0, 60000.0 / 1001.0, 60.0};
            return code >= 1u && code <= 8u ? kRates[code] : 0.0;
        }

        // The video stream's start codes (after 00 00 01).
        constexpr uint8_t kPictureStart = 0x00u;
        constexpr uint8_t kSliceFirst = 0x01u, kSliceLast = 0xAFu;
        constexpr uint8_t kSequenceHeader = 0xB3u;
        constexpr uint8_t kExtensionStart = 0xB5u;
        constexpr uint8_t kGroupStart = 0xB8u;
        constexpr uint8_t kSequenceExtension = 0x1u, kPictureCodingExtension = 0x8u; // extension_start_code_identifier
        constexpr uint8_t kFramePicture = 3u; // picture_structure
        constexpr uint8_t kTypeI = 1u, kTypeP = 2u, kTypeB = 3u; // picture_coding_type
    }

    // ---- pictures counted from the headers ---------------------------------------

    void PssPictureCount::feed(const uint8_t *data, size_t size)
    {
        m_pending.insert(m_pending.end(), data, data + size);
        const uint8_t *p = m_pending.data();
        const size_t n = m_pending.size();
        size_t i = 0u;
        while (i + 3u < n)
        {
            if (p[i] == 0u && p[i + 1u] == 0u && p[i + 2u] == 1u)
            {
                if (!header(p + i + 3u, n - (i + 3u)))
                    break; // the rest of this header comes with the next bytes
                i += 4u;
                continue;
            }
            ++i;
        }
        // (what is left: a header not yet whole, or the last 3 bytes, which
        // can be the start of a start code)
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(i));
    }

    bool PssPictureCount::header(const uint8_t *p, size_t size)
    {
        const uint8_t code = p[0];
        if (code >= kSliceFirst && code <= kSliceLast)
        {
            if (m_afterPicture)
            {
                m_afterPicture = false;
                pictureStarts();
            }
            return true;
        }
        switch (code)
        {
        case kSequenceHeader: // horizontal_size 12, vertical_size 12, aspect 4, frame_rate_code 4
            if (size < 5u)
                return false;
            m_width = (p[1] << 4) | (p[2] >> 4);
            m_height = ((p[2] & 0x0F) << 8) | p[3];
            if (const double fps = frameRate(p[4] & 0x0Fu); fps > 0.0)
                m_fps = fps;
            m_structure = kFramePicture;
            m_firstField = false;
            m_sync = true;
            m_afterPicture = false;
            return true;
        case kGroupStart: // time_code 25, closed_gop 1
            if (size < 5u)
                return false;
            m_closedGop = (p[4] & 0x40u) != 0u;
            m_firstField = false;
            m_sync = true;
            m_afterPicture = false;
            return true;
        case kPictureStart: // temporal_reference 10, picture_coding_type 3
            if (size < 3u)
                return false;
            m_type = static_cast<uint8_t>((p[2] >> 3) & 7u);
            m_afterPicture = true;
            return true;
        case kExtensionStart:
        {
            if (size < 2u)
                return false;
            const uint8_t id = p[1] >> 4;
            if (id == kSequenceExtension)
            {
                // profile_and_level 8, progressive_sequence 1, chroma_format 2,
                // horizontal_size_extension 2, vertical_size_extension 2,
                // bit_rate_extension 12, marker 1, vbv_buffer_size_extension 8,
                // low_delay 1
                if (size < 7u)
                    return false;
                m_width |= (((p[2] & 1u) << 1) | (p[3] >> 7)) << 12;
                m_height |= ((p[3] >> 5) & 3u) << 12;
                m_lowDelay = (p[6] & 0x80u) != 0u;
            }
            else if (id == kPictureCodingExtension && m_afterPicture)
            {
                // f_code 16, intra_dc_precision 2, picture_structure 2
                if (size < 4u)
                    return false;
                m_structure = p[3] & 3u;
            }
            return true;
        }
        default:
            return true;
        }
    }

    // A picture's first slice: whether FFmpeg decodes it, and which picture
    // it shows (decode_chunks, ff_mpv_frame_start, slice_end).
    void PssPictureCount::pictureStarts()
    {
        if (m_type != kTypeI && m_type != kTypeP && m_type != kTypeB)
            return; // (a picture header FFmpeg refuses: its slices are not decoded)
        // FFmpeg's first_field: toggled by each field picture, false for a frame.
        const bool field = m_structure != kFramePicture;
        m_firstField = field && !m_firstField;
        if (m_last == Reference::none && m_type == kTypeB && !m_closedGop)
            return; // a B-picture of an open GOP with no reference before it
        if (m_type == kTypeI)
            m_sync = true;
        if (m_next == Reference::none && m_type == kTypeP && !m_sync)
            return;
        if (m_firstField || !field)
        {
            if (m_type != kTypeB)
            {
                m_last = m_next;
                m_next = Reference::picture;
            }
            if (m_last == Reference::none && m_type != kTypeI)
                m_last = Reference::dummy;
            if (m_next == Reference::none && m_type == kTypeB)
                m_next = Reference::dummy;
        }
        if (m_firstField)
            return; // a frame is shown after its second field
        if (m_type == kTypeB || m_lowDelay)
            ++m_shown; // this picture
        else if (m_last == Reference::picture)
            ++m_shown; // the reference before it (one picture of latency)
    }

    void PssPictureCount::finish()
    {
        m_pending.clear();
        if (!m_lowDelay && m_next != Reference::none)
            ++m_shown; // the last reference picture, when the decoder is drained
        m_next = Reference::none;
    }

    bool PssPictureCount::take()
    {
        if (m_shown == 0u)
            return false;
        --m_shown;
        return true;
    }

    // ---- packets ------------------------------------------------------------

    PssPackets::PssPackets(std::shared_ptr<zx::game_files::GameFile> file, uint8_t streamId)
        : m_file(std::move(file)), m_streamId(streamId)
    {
    }

    void PssPackets::rewind()
    {
        m_position = 0u;
        m_bufferStart = 0u;
        m_buffer.clear();
    }

    const uint8_t *PssPackets::at(uint64_t position, size_t size)
    {
        if (!m_file || position + size > m_file->size())
            return nullptr;
        if (position < m_bufferStart || position + size > m_bufferStart + m_buffer.size())
        {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(std::max(size, kReadChunk), m_file->size() - position));
            m_buffer.resize(want);
            m_buffer.resize(m_file->read(position, m_buffer.data(), want));
            m_bufferStart = position;
            if (m_buffer.size() < size)
                return nullptr;
        }
        return m_buffer.data() + (position - m_bufferStart);
    }

    bool PssPackets::next(std::vector<uint8_t> &payload)
    {
        for (;;)
        {
            const uint8_t *p = at(m_position, 4u);
            if (!p)
                return false;
            if (p[0] != 0u || p[1] != 0u || p[2] != 1u)
            {
                ++m_position; // resynchronise on the next start code
                continue;
            }
            const uint8_t code = p[3];
            if (code == kProgramEnd)
                return false;
            if (code == kPackStart)
            {
                const uint8_t *pack = at(m_position, 14u);
                if (!pack)
                    return false;
                m_position += (pack[4] >> 6) == 1u ? 14u + (pack[13] & 7u) : 12u;
                continue;
            }
            if (code < kSystemHeader)
            {
                ++m_position;
                continue;
            }
            const uint8_t *header = at(m_position, 6u);
            if (!header)
                return false;
            const size_t length = 6u + ((static_cast<size_t>(header[4]) << 8) | header[5]);
            const uint64_t packet = m_position;
            m_position += length;
            if (code != m_streamId)
                continue;
            const uint8_t *pes = at(packet, length);
            if (!pes)
                return false;
            size_t offset = 6u;
            if (length >= 9u && (pes[6] & 0xC0u) == 0x80u)
            {
                offset = 9u + pes[8]; // MPEG-2 PES header
            }
            else
            {
                while (offset < length && pes[offset] == 0xFFu)
                    ++offset;
                if (offset + 1u < length && (pes[offset] & 0xC0u) == 0x40u)
                    offset += 2u;
                if (offset < length && (pes[offset] & 0xF0u) == 0x20u)
                    offset += 5u;
                else if (offset < length && (pes[offset] & 0xF0u) == 0x30u)
                    offset += 10u;
                else if (offset < length && pes[offset] == 0x0Fu)
                    ++offset;
            }
            if (code == kPrivateStream1)
            {
                // Sub-stream header (0xFF 0xA0 0x00 0x00 on this disc).
                while (offset < length && pes[offset] == 0xFFu)
                    ++offset;
                offset += 3u;
            }
            if (offset >= length)
                continue;
            payload.assign(pes + offset, pes + length);
            return true;
        }
    }

    // ---- video ----------------------------------------------------------------

#if ZX_HAS_FFMPEG
    struct PssVideo::Decoder
    {
        AVCodecContext *context = nullptr;
        AVCodecParserContext *parser = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *frame = nullptr;
        SwsContext *scaler = nullptr;
        // What the parser is handed: the fed bytes, then
        // AV_INPUT_BUFFER_PADDING_SIZE zeros, as FFmpeg's API asks of every
        // input buffer (its parsers read past the end: ff_combine_frame
        // copies the padding along with a picture's last bytes).
        std::vector<uint8_t> input;
        int scalerWidth = 0;
        int scalerHeight = 0;
        int scalerFormat = -1;

        bool open()
        {
            av_log_set_level(AV_LOG_ERROR);
            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
            if (!codec)
                return false;
            context = avcodec_alloc_context3(codec);
            parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
            packet = av_packet_alloc();
            frame = av_frame_alloc();
            if (!context || !parser || !packet || !frame)
                return false;
            // The same pictures on every host: the C IDCT (aarch64's NEON one
            // rounds otherwise than C and x86's SSE2, from the first P-picture
            // on) and no approximate motion compensation.
            context->idct_algo = FF_IDCT_SIMPLE;
            context->flags |= AV_CODEC_FLAG_BITEXACT;
            return avcodec_open2(context, codec, nullptr) == 0;
        }

        void close()
        {
            if (scaler)
                sws_freeContext(scaler);
            if (parser)
                av_parser_close(parser);
            if (context)
                avcodec_free_context(&context);
            if (packet)
                av_packet_free(&packet);
            if (frame)
                av_frame_free(&frame);
            scaler = nullptr;
            parser = nullptr;
        }

        void receive(std::deque<Picture> &out)
        {
            while (avcodec_receive_frame(context, frame) == 0)
            {
                Picture picture;
                picture.width = frame->width;
                picture.height = frame->height;
                picture.rgba.resize(static_cast<size_t>(frame->width) * frame->height * 4u);
                if (!scaler || scalerWidth != frame->width || scalerHeight != frame->height || scalerFormat != frame->format)
                {
                    if (scaler)
                        sws_freeContext(scaler);
                    // (bit-exact: swscale's NEON and x86 converters round otherwise)
                    scaler = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                            frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR | SWS_BITEXACT | SWS_ACCURATE_RND,
                                            nullptr, nullptr, nullptr);
                    scalerWidth = frame->width;
                    scalerHeight = frame->height;
                    scalerFormat = frame->format;
                }
                if (scaler)
                {
                    uint8_t *planes[4] = {picture.rgba.data(), nullptr, nullptr, nullptr};
                    int strides[4] = {frame->width * 4, 0, 0, 0};
                    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                }
                out.push_back(std::move(picture));
            }
        }

        void send(const uint8_t *data, int size, std::deque<Picture> &out)
        {
            AVPacket *input = nullptr; // null: drain
            if (size > 0)
            {
                packet->data = const_cast<uint8_t *>(data);
                packet->size = size;
                input = packet;
            }
            if (avcodec_send_packet(context, input) == AVERROR(EAGAIN))
            {
                receive(out);
                avcodec_send_packet(context, input);
            }
            receive(out);
        }
    };
#else
    struct PssVideo::Decoder
    {
        bool open() { return false; }
        void close() {}
    };
#endif

    PssVideo::PssVideo(std::shared_ptr<zx::game_files::GameFile> file, bool decode)
        : m_packets(std::move(file), 0xE0u), m_decode(decode)
    {
        openDecoder();
        if (decode && !m_decoder)
            std::cerr << "[movie] no MPEG-2 decoder: black pictures, as many as a decoder gives" << std::endl;
    }

    void PssVideo::openDecoder()
    {
        m_decoder.reset();
        m_count.reset();
        if (m_decode)
        {
            m_decoder = std::make_unique<Decoder>();
            if (!m_decoder->open())
            {
                m_decoder->close();
                m_decoder.reset();
            }
        }
        if (!m_decoder)
            m_count = std::make_unique<PssPictureCount>();
    }

    PssVideo::~PssVideo()
    {
        stopAhead();
        if (m_decoder)
            m_decoder->close();
    }

    void PssVideo::feed(const uint8_t *data, size_t size)
    {
#if ZX_HAS_FFMPEG
        // Frame rate from the first sequence header.
        for (size_t i = 0; i + 8u <= size; ++i)
        {
            if (data[i] == 0u && data[i + 1u] == 0u && data[i + 2u] == 1u && data[i + 3u] == 0xB3u)
            {
        if (const double fps = frameRate(data[i + 7u] & 0x0Fu); fps > 0.0)
                    m_decodedFps = fps;
                break;
            }
        }
        std::vector<uint8_t> &input = m_decoder->input;
        input.assign(data, data + size);
        input.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0u);
        data = input.data();
        while (size > 0u)
        {
            uint8_t *out = nullptr;
            int outSize = 0;
            const int used = av_parser_parse2(m_decoder->parser, m_decoder->context, &out, &outSize, data,
                                              static_cast<int>(size), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0)
                return;
            data += used;
            size -= static_cast<size_t>(used);
            if (outSize > 0)
                m_decoder->send(out, outSize, m_ready);
            if (used == 0 && outSize == 0)
                break;
        }
#else
        (void)data;
        (void)size;
#endif
    }

    void PssVideo::flush()
    {
#if ZX_HAS_FFMPEG
        uint8_t *out = nullptr;
        int outSize = 0;
        av_parser_parse2(m_decoder->parser, m_decoder->context, &out, &outSize, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (outSize > 0)
            m_decoder->send(out, outSize, m_ready);
        m_decoder->send(nullptr, 0, m_ready);
#endif
    }

    bool PssVideo::next(Picture &picture)
    {
        if (!m_decoder)
            return nextBlack(picture);
        if (!m_aheadThread.joinable())
            m_aheadThread = std::thread([this]()
                                        { decodeAhead(); });
        std::unique_lock<std::mutex> lock(m_aheadMutex);
        m_aheadCv.wait(lock, [&]()
                       { return !m_ahead.empty() || m_aheadEnded; });
        if (m_ahead.empty())
        {
            m_fps = m_endFps;
            return false;
        }
        picture = std::move(m_ahead.front().picture);
        m_fps = m_ahead.front().fps;
        m_ahead.pop_front();
        lock.unlock();
        m_aheadCv.notify_all();
        return true;
    }

    // The decoding thread: up to kAhead pictures ahead of next().
    void PssVideo::decodeAhead()
    {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(m_aheadMutex);
                m_aheadCv.wait(lock, [&]()
                               { return m_aheadStop || m_ahead.size() < kAhead; });
                if (m_aheadStop)
                    return;
            }
            Decoded decoded;
            const bool more = decodeNext(decoded.picture);
            decoded.fps = m_decodedFps;
            {
                std::lock_guard<std::mutex> lock(m_aheadMutex);
                if (more)
                    m_ahead.push_back(std::move(decoded));
                else
                {
                    m_aheadEnded = true;
                    m_endFps = decoded.fps;
                }
            }
            m_aheadCv.notify_all();
            if (!more)
                return;
        }
    }

    void PssVideo::stopAhead()
    {
        if (!m_aheadThread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(m_aheadMutex);
            m_aheadStop = true;
        }
        m_aheadCv.notify_all();
        m_aheadThread.join();
        m_ahead.clear();
        m_aheadEnded = false;
        m_aheadStop = false;
    }

    // The next picture from the stream (on the decoding thread).
    bool PssVideo::decodeNext(Picture &picture)
    {
        std::vector<uint8_t> payload;
        while (m_ready.empty())
        {
            if (m_flushed)
                return false;
            if (!m_packets.next(payload))
            {
                flush();
                m_flushed = true;
                continue;
            }
            feed(payload.data(), payload.size());
        }
        picture = std::move(m_ready.front());
        m_ready.pop_front();
        return true;
    }

    bool PssVideo::nextBlack(Picture &picture)
    {
        std::vector<uint8_t> payload;
        while (!m_count->take())
        {
            if (m_flushed)
                return false;
            if (!m_packets.next(payload))
            {
                m_count->finish();
                m_flushed = true;
                continue;
            }
            m_count->feed(payload.data(), payload.size());
            if (m_count->framesPerSecond() > 0.0)
                m_fps = m_count->framesPerSecond();
        }
        picture.width = m_count->width();
        picture.height = m_count->height();
        picture.rgba.resize(static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) * 4u);
        for (size_t i = 0; i < picture.rgba.size(); i += 4u)
        {
            picture.rgba[i] = picture.rgba[i + 1u] = picture.rgba[i + 2u] = 0u;
            picture.rgba[i + 3u] = 0xFFu; // (opaque, as the decoder's RGBA)
        }
        return true;
    }

    void PssVideo::rewind()
    {
        stopAhead();
        m_decodedFps = m_fps;
        m_packets.rewind();
        m_ready.clear();
        m_flushed = false;
        // A fresh decoder (after a flush to the end FFmpeg's decoder takes no
        // more input until reopened) or count.
        if (m_decoder)
            m_decoder->close();
        openDecoder();
    }

    // ---- sound ----------------------------------------------------------------

    PssAudio::PssAudio(std::shared_ptr<zx::game_files::GameFile> file)
        : m_packets(std::move(file), kPrivateStream1)
    {
    }

    bool PssAudio::hasSound()
    {
        if (!m_headerTried)
        {
            m_headerTried = true;
            m_headerOk = parseHeader();
        }
        return m_headerOk;
    }

    void PssAudio::rewind()
    {
        m_packets.rewind();
        m_raw.clear();
        m_rawOffset = 0u;
        m_headerTried = false;
        m_headerOk = false;
        m_streamEnded = false;
        m_samples.clear();
        m_sampleOffset = 0u;
        m_phase = 0.0;
    }

    bool PssAudio::parseHeader()
    {
        // "SShd" size(=0x18) type rate channels interleave ... "SSbd" size.
        std::vector<uint8_t> payload;
        while (m_raw.size() < 0x28u)
        {
            if (!m_packets.next(payload))
                return false;
            m_raw.insert(m_raw.end(), payload.begin(), payload.end());
        }
        if (std::memcmp(m_raw.data(), "SShd", 4) != 0)
            return false;
        const uint32_t headerSize = le32(m_raw.data() + 4);
        const uint32_t type = le32(m_raw.data() + 8);
        m_rate = le32(m_raw.data() + 12);
        m_channels = le32(m_raw.data() + 16);
        m_interleave = le32(m_raw.data() + 20);
        const size_t body = 8u + headerSize;
        while (m_raw.size() < body + 8u)
        {
            if (!m_packets.next(payload))
                return false;
            m_raw.insert(m_raw.end(), payload.begin(), payload.end());
        }
        if (std::memcmp(m_raw.data() + body, "SSbd", 4) != 0)
            return false;
        m_dataLeft = le32(m_raw.data() + body + 4);
        m_rawOffset = body + 8u;
        // Type 1: 16-bit PCM (the only one on this disc; 0x10 would be ADPCM).
        if (type != 1u || m_rate == 0u || m_channels == 0u || m_channels > 2u || m_interleave == 0u ||
            (m_interleave & 1u) != 0u)
        {
            std::cerr << "[movie] sound format not supported (type " << type << ", " << m_rate << " Hz, "
                      << m_channels << " channels, interleave " << m_interleave << ")" << std::endl;
            return false;
        }
        return true;
    }

    // One interleave block per channel into m_samples (stereo frames).
    bool PssAudio::decodeBlock()
    {
        const size_t block = static_cast<size_t>(m_interleave) * m_channels;
        std::vector<uint8_t> payload;
        while (m_raw.size() - m_rawOffset < block)
        {
            if (m_streamEnded || !m_packets.next(payload))
            {
                m_streamEnded = true;
                return false;
            }
            if (m_rawOffset > 0u)
            {
                m_raw.erase(m_raw.begin(), m_raw.begin() + static_cast<std::ptrdiff_t>(m_rawOffset));
                m_rawOffset = 0u;
            }
            m_raw.insert(m_raw.end(), payload.begin(), payload.end());
        }
        if (m_dataLeft < block)
            return false;
        m_dataLeft -= block;
        const uint8_t *p = m_raw.data() + m_rawOffset;
        const size_t perChannel = m_interleave / 2u;
        if (m_sampleOffset > 0u)
        {
            m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(m_sampleOffset * 2u));
            m_sampleOffset = 0u;
        }
        const size_t base = m_samples.size();
        m_samples.resize(base + perChannel * 2u);
        for (size_t i = 0; i < perChannel; ++i)
        {
            const uint8_t *l = p + i * 2u;
            const uint8_t *r = m_channels == 2u ? p + m_interleave + i * 2u : l;
            m_samples[base + i * 2u] = static_cast<int16_t>(l[0] | (l[1] << 8));
            m_samples[base + i * 2u + 1u] = static_cast<int16_t>(r[0] | (r[1] << 8));
        }
        m_rawOffset += block;
        return true;
    }

    size_t PssAudio::read(int16_t *stereo, size_t frames)
    {
        if (!hasSound())
            return 0u;
        size_t written = 0u;
        if (m_rate == kOutputRate)
        {
            while (written < frames)
            {
                const size_t available = m_samples.size() / 2u - m_sampleOffset;
                if (available == 0u && !decodeBlock())
                    break;
                const size_t take = std::min(frames - written, m_samples.size() / 2u - m_sampleOffset);
                std::memcpy(stereo + written * 2u, m_samples.data() + m_sampleOffset * 2u, take * 4u);
                m_sampleOffset += take;
                written += take;
            }
            return written;
        }
        // Linear interpolation for other rates.
        const double step = static_cast<double>(m_rate) / kOutputRate;
        while (written < frames)
        {
            const size_t index = m_sampleOffset + static_cast<size_t>(m_phase);
            if (index + 1u >= m_samples.size() / 2u)
            {
                if (!decodeBlock())
                    break;
                continue;
            }
            const double t = m_phase - static_cast<size_t>(m_phase);
            for (size_t c = 0; c < 2u; ++c)
            {
                const double a = m_samples[index * 2u + c];
                const double b = m_samples[(index + 1u) * 2u + c];
                stereo[written * 2u + c] = static_cast<int16_t>(a + (b - a) * t);
            }
            ++written;
            m_phase += step;
            const size_t whole = static_cast<size_t>(m_phase);
            m_sampleOffset += whole;
            m_phase -= static_cast<double>(whole);
        }
        return written;
    }

    // ---- any file FFmpeg reads ---------------------------------------------------

#if ZX_HAS_FFMPEG && ZX_HAS_AVFORMAT
    struct FileVideo::Impl
    {
        AVFormatContext *format = nullptr;
        AVCodecContext *codec = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *frame = nullptr;
        SwsContext *toRgba = nullptr; // the file's format -> RGBA at its size
        SwsContext *toScaled = nullptr; // RGBA -> RGBA at the scaled size
        int stream = -1;
        bool drained = false;
        // A stream with no frame rate of its own (an XMV: each frame with its
        // own time, held for as long as it says): played at the PS2 movies'
        // 29.97 frames a second, each picture the frame its time shows.
        bool variableRate = false;
        AVRational timeBase{1, 1};
        double lastGap = 0.0;      // the time between the last two frames
        uint64_t given = 0;        // pictures given
        bool held = false;         // the frame on show is in `frame` (converted at each picture)
        AVFrame *nextFrame = nullptr; // the one after it, decoded ahead
        bool haveNext = false;

        ~Impl()
        {
            if (toRgba)
                sws_freeContext(toRgba);
            if (toScaled)
                sws_freeContext(toScaled);
            av_frame_free(&frame);
            av_frame_free(&nextFrame);
            av_packet_free(&packet);
            avcodec_free_context(&codec);
            avformat_close_input(&format);
        }

        // A frame's time in seconds.
        double secondsOf(const AVFrame *f) const
        {
            const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            return pts == AV_NOPTS_VALUE ? 0.0 : double(pts) * av_q2d(timeBase);
        }

        // The frame on show at the next picture's time in `frame` (a
        // variable-rate stream); false after the stream's end.
        bool advance(double fps)
        {
            const double at = double(given) / fps;
            if (!held)
            {
                if (!decode(frame))
                    return false;
                held = true;
                haveNext = decode(nextFrame);
            }
            while (haveNext && secondsOf(nextFrame) <= at)
            {
                lastGap = secondsOf(nextFrame) - secondsOf(frame);
                std::swap(frame, nextFrame);
                av_frame_unref(nextFrame);
                haveNext = decode(nextFrame);
            }
            // The last frame shows for its own duration (else as long as the one before it).
            const double last = frame->duration > 0 ? double(frame->duration) * av_q2d(timeBase) : lastGap;
            if (!haveNext && at >= secondsOf(frame) + last)
                return false;
            ++given;
            return true;
        }

        // The next decoded frame in `into`; false at the end.
        bool decode(AVFrame *into)
        {
            for (;;)
            {
                const int got = avcodec_receive_frame(codec, into);
                if (got == 0)
                    return true;
                if (got != AVERROR(EAGAIN) || drained)
                    return false;
                if (av_read_frame(format, packet) < 0)
                {
                    avcodec_send_packet(codec, nullptr);
                    drained = true;
                    continue;
                }
                if (packet->stream_index == stream)
                    avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
            }
        }
    };

    FileVideo::FileVideo(std::filesystem::path path) : m_path(std::move(path))
    {
        if (!open())
            m_impl.reset();
    }

    FileVideo::~FileVideo() = default;

    bool FileVideo::open()
    {
        av_log_set_level(AV_LOG_ERROR);
        m_impl = std::make_unique<Impl>();
        Impl &m = *m_impl;
        if (avformat_open_input(&m.format, m_path.string().c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(m.format, nullptr) < 0)
        {
            std::cerr << "[movie] cannot read " << m_path.string() << std::endl;
            return false;
        }
        const AVCodec *decoder = nullptr;
        m.stream = av_find_best_stream(m.format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (m.stream < 0 || !decoder)
        {
            std::cerr << "[movie] no video decoder for " << m_path.string() << std::endl;
            return false;
        }
        m.codec = avcodec_alloc_context3(decoder);
        if (!m.codec || avcodec_parameters_to_context(m.codec, m.format->streams[m.stream]->codecpar) < 0 ||
            avcodec_open2(m.codec, decoder, nullptr) < 0)
            return false;
        const AVStream *video = m.format->streams[m.stream];
        if (video->avg_frame_rate.num > 0 && video->avg_frame_rate.den > 0)
            m_fps = av_q2d(video->avg_frame_rate);
        else
        {
            // No rate of its own: each frame's time says how long it shows.
            m.variableRate = true;
            m.timeBase = video->time_base;
            m_fps = 30000.0 / 1001.0;
        }
        m_width = m.codec->width;
        m_height = m.codec->height;
        m.packet = av_packet_alloc();
        m.frame = av_frame_alloc();
        m.nextFrame = av_frame_alloc();
        return m.packet && m.frame && m.nextFrame && m_width > 0 && m_height > 0;
    }

    void FileVideo::setScaledSize(int width, int height)
    {
        m_scaledWidth = width;
        m_scaledHeight = height;
    }

    bool FileVideo::next(Picture &picture)
    {
        if (!m_impl)
            return false;
        Impl &m = *m_impl;
        if (m.variableRate ? !m.advance(m_fps) : !m.decode(m.frame))
            return false;
        const int w = m.frame->width, h = m.frame->height;
        m_full.width = w;
        m_full.height = h;
        m_full.rgba.resize(static_cast<size_t>(w) * h * 4u);
        m.toRgba = sws_getCachedContext(m.toRgba, w, h, static_cast<AVPixelFormat>(m.frame->format), w, h, AV_PIX_FMT_RGBA,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m.toRgba)
            return false;
        {
            uint8_t *planes[4] = {m_full.rgba.data(), nullptr, nullptr, nullptr};
            int strides[4] = {w * 4, 0, 0, 0};
            sws_scale(m.toRgba, m.frame->data, m.frame->linesize, 0, h, planes, strides);
        }
        if (!m.variableRate) // (a variable-rate stream's frame may show again)
            av_frame_unref(m.frame);
        if (m_scaledWidth <= 0 || m_scaledHeight <= 0 || (m_scaledWidth == w && m_scaledHeight == h))
        {
            picture = m_full;
            return true;
        }
        m.toScaled = sws_getCachedContext(m.toScaled, w, h, AV_PIX_FMT_RGBA, m_scaledWidth, m_scaledHeight, AV_PIX_FMT_RGBA,
                                          SWS_AREA, nullptr, nullptr, nullptr);
        if (!m.toScaled)
            return false;
        picture.width = m_scaledWidth;
        picture.height = m_scaledHeight;
        picture.rgba.resize(static_cast<size_t>(m_scaledWidth) * m_scaledHeight * 4u);
        const uint8_t *in[4] = {m_full.rgba.data(), nullptr, nullptr, nullptr};
        int inStrides[4] = {w * 4, 0, 0, 0};
        uint8_t *out[4] = {picture.rgba.data(), nullptr, nullptr, nullptr};
        int outStrides[4] = {m_scaledWidth * 4, 0, 0, 0};
        sws_scale(m.toScaled, in, inStrides, 0, h, out, outStrides);
        return true;
    }

    void FileVideo::rewind()
    {
        // Reopened: seeking Bink back to the start is not reliable.
        if (!open())
            m_impl.reset();
    }
#else
    struct FileVideo::Impl
    {
    };

    FileVideo::FileVideo(std::filesystem::path path) : m_path(std::move(path))
    {
        std::cerr << "[movie] this build reads no movie files other than PSS (no libavformat)" << std::endl;
    }

    FileVideo::~FileVideo() = default;
    bool FileVideo::open() { return false; }
    void FileVideo::setScaledSize(int width, int height)
    {
        m_scaledWidth = width;
        m_scaledHeight = height;
    }
    bool FileVideo::next(Picture &) { return false; }
    void FileVideo::rewind() {}
#endif
}
