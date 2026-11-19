#include "runtime/ps2_audio_file.h"

#include "runtime/ps2_game_files.h"
#include "runtime/ps2_zip_archive.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#if PS2X_HAS_AVFORMAT
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

namespace ps2x::audio_file
{
    namespace
    {
        int16_t toS16(float v)
        {
            return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        }

        // A file's bytes read in place, or a file read whole into memory
        // (one in a zip: runtime/ps2_zip_archive.h).
        class Source
        {
        public:
            explicit Source(std::FILE *file) : m_file(file) {}
            explicit Source(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)), m_inMemory(true) {}
            ~Source()
            {
                if (m_file)
                    std::fclose(m_file);
            }
            Source(const Source &) = delete;
            Source &operator=(const Source &) = delete;

            size_t read(void *out, size_t size, size_t count)
            {
                if (!m_inMemory)
                    return std::fread(out, size, count, m_file);
                if (size == 0u || m_at >= m_bytes.size())
                    return 0u;
                const size_t whole = std::min(count, (m_bytes.size() - m_at) / size);
                std::memcpy(out, m_bytes.data() + m_at, whole * size);
                m_at += whole * size;
                return whole;
            }
            void seek(uint64_t at)
            {
                if (m_inMemory)
                    m_at = static_cast<size_t>(std::min<uint64_t>(at, m_bytes.size()));
                else
                    std::fseek(m_file, static_cast<long>(at), SEEK_SET);
            }
            uint64_t tell() const { return m_inMemory ? m_at : static_cast<uint64_t>(std::ftell(m_file)); }
            const std::vector<uint8_t> &bytes() const { return m_bytes; }

        private:
            std::FILE *m_file = nullptr;
            std::vector<uint8_t> m_bytes;
            size_t m_at = 0;
            bool m_inMemory = false;
        };

        // RIFF WAVE: PCM 8/16/24/32-bit or 32-bit float, any channel count.
        class WavReader final : public Reader
        {
        public:
            static std::unique_ptr<Reader> open(std::unique_ptr<Source> source, std::string *error)
            {
                std::unique_ptr<WavReader> wav(new WavReader());
                wav->m_file = std::move(source);
                if (!wav->parse())
                {
                    if (error)
                        *error = "not a PCM or float RIFF WAVE file";
                    return nullptr;
                }
                return wav;
            }

            size_t read(int16_t *out, size_t frames) override
            {
                const size_t frameBytes = static_cast<size_t>(m_channels) * m_bytesPerSample;
                frames = static_cast<size_t>(std::min<uint64_t>(frames, m_frames - m_position));
                if (frames == 0u)
                    return 0;
                m_raw.resize(frames * frameBytes);
                m_file->seek(m_dataOffset + m_position * frameBytes);
                const size_t got = m_file->read(m_raw.data(), frameBytes, frames);
                for (size_t i = 0; i < got; ++i)
                {
                    const uint8_t *frame = m_raw.data() + i * frameBytes;
                    const int16_t l = sample(frame);
                    out[i * 2] = l;
                    out[i * 2 + 1] = m_channels > 1 ? sample(frame + m_bytesPerSample) : l;
                }
                m_position += got;
                return got;
            }

            bool rewind() override
            {
                m_position = 0;
                return true;
            }
            [[nodiscard]] uint32_t rate() const override { return m_rate; }
            [[nodiscard]] uint64_t frames() const override { return m_frames; }

        private:
            WavReader() = default;

            static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
            static uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

            bool parse()
            {
                uint8_t header[12];
                if (m_file->read(header, 1, 12) != 12 || std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4))
                    return false;
                bool haveFormat = false;
                for (;;)
                {
                    uint8_t chunk[8];
                    if (m_file->read(chunk, 1, 8) != 8)
                        return false;
                    const uint32_t size = le32(chunk + 4);
                    const uint64_t body = m_file->tell();
                    if (!std::memcmp(chunk, "fmt ", 4) && size >= 16)
                    {
                        uint8_t fmt[40] = {};
                        if (m_file->read(fmt, 1, std::min<uint32_t>(size, sizeof(fmt))) < 16)
                            return false;
                        uint16_t tag = le16(fmt);
                        m_channels = le16(fmt + 2);
                        m_rate = le32(fmt + 4);
                        m_bytesPerSample = le16(fmt + 14) / 8u;
                        if (tag == 0xFFFEu && size >= 26)
                            tag = le16(fmt + 24); // WAVE_FORMAT_EXTENSIBLE: the sub-format's first word
                        m_float = tag == 3u;
                        haveFormat = (tag == 1u || tag == 3u) && m_channels > 0 && m_rate > 0 &&
                                     m_bytesPerSample >= 1u && m_bytesPerSample <= 4u && (!m_float || m_bytesPerSample == 4u);
                    }
                    else if (!std::memcmp(chunk, "data", 4))
                    {
                        if (!haveFormat)
                            return false;
                        m_dataOffset = body;
                        m_frames = size / (static_cast<uint64_t>(m_channels) * m_bytesPerSample);
                        return m_frames > 0;
                    }
                    m_file->seek(body + size + (size & 1u));
                }
            }

            int16_t sample(const uint8_t *p) const
            {
                switch (m_bytesPerSample)
                {
                case 1:
                    return static_cast<int16_t>((static_cast<int>(p[0]) - 128) << 8);
                case 2:
                    return static_cast<int16_t>(le16(p));
                case 3:
                    return static_cast<int16_t>(le16(p + 1));
                default:
                    if (m_float)
                    {
                        float f;
                        std::memcpy(&f, p, sizeof(f));
                        return toS16(f);
                    }
                    return static_cast<int16_t>(le16(p + 2));
                }
            }

            std::unique_ptr<Source> m_file;
            std::vector<uint8_t> m_raw;
            uint64_t m_dataOffset = 0;
            uint64_t m_frames = 0;
            uint64_t m_position = 0;
            uint32_t m_rate = 0;
            uint16_t m_channels = 0;
            uint16_t m_bytesPerSample = 0;
            bool m_float = false;
        };

#if PS2X_HAS_AVFORMAT
        // Anything libavformat/libavcodec in the build can demux and decode.
        class FfmpegReader final : public Reader
        {
        public:
            // A file on the host by its path, or one read into memory
            // (`bytes` not null: read through an AVIOContext over them).
            static std::unique_ptr<Reader> open(const std::filesystem::path &path, std::unique_ptr<Source> bytes, std::string *error)
            {
                std::unique_ptr<FfmpegReader> reader(new FfmpegReader());
                reader->m_memory = std::move(bytes);
                const char *problem = reader->init(path);
                if (problem)
                {
                    if (error)
                        *error = problem;
                    return nullptr;
                }
                return reader;
            }

            ~FfmpegReader() override
            {
                av_frame_free(&m_frame);
                av_packet_free(&m_packet);
                avcodec_free_context(&m_codec);
                avformat_close_input(&m_format);
                if (m_io)
                {
                    av_freep(&m_io->buffer);
                    avio_context_free(&m_io);
                }
            }

            size_t read(int16_t *out, size_t frames) override
            {
                size_t produced = 0;
                while (produced < frames)
                {
                    if (m_pendingAt >= m_pending.size() / 2u)
                    {
                        if (!decode())
                            break;
                        continue;
                    }
                    const size_t n = std::min(frames - produced, m_pending.size() / 2u - m_pendingAt);
                    std::memcpy(out + produced * 2u, m_pending.data() + m_pendingAt * 2u, n * 2u * sizeof(int16_t));
                    m_pendingAt += n;
                    produced += n;
                }
                return produced;
            }

            bool rewind() override
            {
                if (av_seek_frame(m_format, m_stream, 0, AVSEEK_FLAG_BACKWARD) < 0 &&
                    avformat_seek_file(m_format, -1, INT64_MIN, 0, 0, 0) < 0)
                    return false;
                avcodec_flush_buffers(m_codec);
                m_pending.clear();
                m_pendingAt = 0;
                m_flushed = false;
                m_ended = false;
                return true;
            }

            [[nodiscard]] uint32_t rate() const override { return m_rate; }
            [[nodiscard]] uint64_t frames() const override { return m_frames; }

            [[nodiscard]] std::string tag(const std::string &key) const override
            {
                // The container's tags (MP3, FLAC), else the stream's (Ogg
                // keeps Vorbis comments there). av_dict_get ignores case.
                const AVDictionary *dicts[] = {m_format ? m_format->metadata : nullptr,
                                               m_format && m_stream >= 0 ? m_format->streams[m_stream]->metadata : nullptr};
                for (const AVDictionary *dict : dicts)
                    if (const AVDictionaryEntry *entry = av_dict_get(dict, key.c_str(), nullptr, 0); entry && entry->value && *entry->value)
                        return entry->value;
                return {};
            }

        private:
            FfmpegReader() = default;

            static int readMemory(void *opaque, uint8_t *out, int size)
            {
                auto *self = static_cast<FfmpegReader *>(opaque);
                const std::vector<uint8_t> &bytes = self->m_memory->bytes();
                if (self->m_memoryAt >= bytes.size())
                    return AVERROR_EOF;
                const size_t n = std::min(static_cast<size_t>(size), bytes.size() - self->m_memoryAt);
                std::memcpy(out, bytes.data() + self->m_memoryAt, n);
                self->m_memoryAt += n;
                return static_cast<int>(n);
            }
            static int64_t seekMemory(void *opaque, int64_t offset, int whence)
            {
                auto *self = static_cast<FfmpegReader *>(opaque);
                const int64_t size = static_cast<int64_t>(self->m_memory->bytes().size());
                if (whence & AVSEEK_SIZE)
                    return size;
                int64_t at = offset;
                if ((whence & 3) == SEEK_CUR)
                    at += static_cast<int64_t>(self->m_memoryAt);
                else if ((whence & 3) == SEEK_END)
                    at += size;
                if (at < 0 || at > size)
                    return -1;
                self->m_memoryAt = static_cast<size_t>(at);
                return at;
            }

            const char *init(const std::filesystem::path &path)
            {
                if (m_memory)
                {
                    constexpr int kBuffer = 64 * 1024;
                    auto *buffer = static_cast<unsigned char *>(av_malloc(kBuffer));
                    m_io = buffer ? avio_alloc_context(buffer, kBuffer, 0, this, &FfmpegReader::readMemory, nullptr, &FfmpegReader::seekMemory)
                                  : nullptr;
                    m_format = m_io ? avformat_alloc_context() : nullptr;
                    if (!m_format)
                    {
                        if (m_io)
                            av_freep(&m_io->buffer);
                        else
                            av_free(buffer);
                        avio_context_free(&m_io);
                        return "out of memory";
                    }
                    m_format->pb = m_io;
                }
                if (avformat_open_input(&m_format, m_memory ? path.filename().string().c_str() : path.string().c_str(), nullptr, nullptr) < 0)
                    return "not a format this build reads";
                if (avformat_find_stream_info(m_format, nullptr) < 0)
                    return "no stream information";
                const AVCodec *codec = nullptr;
                m_stream = av_find_best_stream(m_format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
                if (m_stream < 0 || !codec)
                    return "no audio this build can decode";
                m_codec = avcodec_alloc_context3(codec);
                if (!m_codec || avcodec_parameters_to_context(m_codec, m_format->streams[m_stream]->codecpar) < 0 ||
                    avcodec_open2(m_codec, codec, nullptr) < 0)
                    return "cannot open the audio decoder";
                m_rate = static_cast<uint32_t>(m_codec->sample_rate);
                if (m_rate == 0u)
                    return "no sample rate";
                const AVStream *stream = m_format->streams[m_stream];
                if (stream->duration > 0)
                    m_frames = static_cast<uint64_t>(static_cast<double>(stream->duration) * av_q2d(stream->time_base) * m_rate);
                else if (m_format->duration > 0)
                    m_frames = static_cast<uint64_t>(static_cast<double>(m_format->duration) / AV_TIME_BASE * m_rate);
                m_packet = av_packet_alloc();
                m_frame = av_frame_alloc();
                return (m_packet && m_frame) ? nullptr : "out of memory";
            }

            // One more decoded frame into m_pending; false at the end.
            bool decode()
            {
                for (;;)
                {
                    if (m_ended)
                        return false;
                    const int got = avcodec_receive_frame(m_codec, m_frame);
                    if (got == 0)
                    {
                        convert();
                        av_frame_unref(m_frame);
                        return true;
                    }
                    if (got == AVERROR_EOF)
                    {
                        m_ended = true;
                        return false;
                    }
                    if (got != AVERROR(EAGAIN))
                    {
                        m_ended = true;
                        return false;
                    }
                    if (m_flushed)
                    {
                        m_ended = true;
                        return false;
                    }
                    if (av_read_frame(m_format, m_packet) < 0)
                    {
                        avcodec_send_packet(m_codec, nullptr); // drain
                        m_flushed = true;
                        continue;
                    }
                    if (m_packet->stream_index == m_stream)
                        avcodec_send_packet(m_codec, m_packet);
                    av_packet_unref(m_packet);
                }
            }

            float at(int channel, int index) const
            {
                const auto format = static_cast<AVSampleFormat>(m_frame->format);
                const int channels = m_frame->ch_layout.nb_channels;
                const bool planar = av_sample_fmt_is_planar(format);
                const uint8_t *data = planar ? m_frame->extended_data[channel] : m_frame->extended_data[0];
                const int i = planar ? index : index * channels + channel;
                switch (av_get_packed_sample_fmt(format))
                {
                case AV_SAMPLE_FMT_U8:
                    return (data[i] - 128) / 128.0f;
                case AV_SAMPLE_FMT_S16:
                    return reinterpret_cast<const int16_t *>(data)[i] / 32768.0f;
                case AV_SAMPLE_FMT_S32:
                    return static_cast<float>(reinterpret_cast<const int32_t *>(data)[i] / 2147483648.0);
                case AV_SAMPLE_FMT_FLT:
                    return reinterpret_cast<const float *>(data)[i];
                case AV_SAMPLE_FMT_DBL:
                    return static_cast<float>(reinterpret_cast<const double *>(data)[i]);
                default:
                    return 0.0f;
                }
            }

            void convert()
            {
                const int channels = m_frame->ch_layout.nb_channels;
                const int count = m_frame->nb_samples;
                m_pending.resize(static_cast<size_t>(count) * 2u);
                m_pendingAt = 0;
                for (int i = 0; i < count; ++i)
                {
                    const float l = channels > 0 ? at(0, i) : 0.0f;
                    const float r = channels > 1 ? at(1, i) : l;
                    m_pending[static_cast<size_t>(i) * 2u] = toS16(l);
                    m_pending[static_cast<size_t>(i) * 2u + 1u] = toS16(r);
                }
            }

            std::unique_ptr<Source> m_memory; // a file read into memory, else null
            size_t m_memoryAt = 0;
            AVIOContext *m_io = nullptr;
            AVFormatContext *m_format = nullptr;
            AVCodecContext *m_codec = nullptr;
            AVPacket *m_packet = nullptr;
            AVFrame *m_frame = nullptr;
            int m_stream = -1;
            uint32_t m_rate = 0;
            uint64_t m_frames = 0;
            std::vector<int16_t> m_pending;
            size_t m_pendingAt = 0;
            bool m_flushed = false;
            bool m_ended = false;
        };
#endif

        std::string lowerExtension(const std::filesystem::path &path)
        {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }
    }

    std::unique_ptr<Reader> open(const std::filesystem::path &path, std::string *error)
    {
        // A file in a zip (a mod's: runtime/ps2_zip_archive.h) is read whole
        // into memory; one on the host is read in place.
        std::unique_ptr<Source> memory;
        if (const ps2x::game_files::ZipPath zip = ps2x::game_files::zipPathOf(path); zip.zip)
        {
            std::vector<uint8_t> bytes;
            if (!zip.zip->read(zip.inside, bytes))
            {
                if (error)
                    *error = "cannot read it from its zip";
                return nullptr;
            }
            memory = std::make_unique<Source>(std::move(bytes));
        }
        if (lowerExtension(path) == ".wav")
        {
            if (!memory)
            {
#if defined(_WIN32)
                std::FILE *file = ::_wfopen(path.c_str(), L"rb");
#else
                std::FILE *file = std::fopen(path.c_str(), "rb");
#endif
                if (!file)
                {
                    if (error)
                        *error = "cannot open";
                    return nullptr;
                }
                memory = std::make_unique<Source>(file);
            }
            return WavReader::open(std::move(memory), error);
        }
#if PS2X_HAS_AVFORMAT
        return FfmpegReader::open(path, std::move(memory), error);
#else
        if (error)
            *error = "only WAV in this build";
        return nullptr;
#endif
    }

    std::vector<std::string> extensions()
    {
#if PS2X_HAS_AVFORMAT
        return {".wav", ".ogg", ".opus", ".flac", ".mp3"};
#else
        return {".wav"};
#endif
    }

    std::string nameFromFileName(const std::filesystem::path &path)
    {
        std::string name = path.stem().string();
        size_t at = 0;
        while (at < name.size() && std::isdigit(static_cast<unsigned char>(name[at])))
            ++at;
        if (at > 0)
            while (at < name.size() && (name[at] == ' ' || name[at] == '-' || name[at] == '_' || name[at] == '.'))
                ++at;
        name = at < name.size() ? name.substr(at) : name;
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        return name;
    }
}
