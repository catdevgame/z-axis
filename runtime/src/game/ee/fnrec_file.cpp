#include "fnrec_file.h"

#define ZSTD_STATIC_LINKING_ONLY // (ZSTD_getFrameHeader: a frame's sizes without decompressing it)
#include <zstd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ostream>

namespace fncheck
{
    namespace
    {
        // The text of a cut compressed file's problem: the replay workers' job scripts look for it in a job's
        // ps2xFnCheck and stop a checker without it before a compressed corpus.
        constexpr char kCutInFrame[] = "cut short: the file ends inside a compressed frame";
        constexpr uint32_t kZstdFrameMagic = 0xFD2FB528u;
        constexpr uint32_t kSkippableMagicMask = 0xFFFFFFF0u;
        constexpr uint32_t kSkippableMagicBase = 0x184D2A50u;

        uint32_t littleWord(const unsigned char *p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        std::FILE *openFile(const std::filesystem::path &path, bool write, bool append)
        {
#if defined(_WIN32)
            return _wfopen(path.c_str(), write ? (append ? L"ab" : L"wb") : L"rb");
#else
            return std::fopen(path.c_str(), write ? (append ? "ab" : "wb") : "rb");
#endif
        }

        // The compressed head (its skippable frame) at the start of `bytes`, if it is there.
        bool readHead(const unsigned char *bytes, size_t size, PackedHead &head)
        {
            if (size < 8u + sizeof(PackedHead) || littleWord(bytes) != kPackedMagic || littleWord(bytes + 4) != sizeof(PackedHead))
                return false;
            std::memcpy(&head, bytes + 8, sizeof(PackedHead));
            return std::memcmp(head.tag, PackedHead{}.tag, sizeof(head.tag)) == 0;
        }

        // A reader's file: to a byte, and its next bytes.
        bool seekRead(std::filebuf *file, uint64_t offset)
        {
            const std::streampos to(static_cast<std::streamoff>(offset));
            return file->pubseekpos(to, std::ios::in) == to;
        }
        size_t readSome(std::filebuf *file, void *into, size_t bytes)
        {
            return static_cast<size_t>(file->sgetn(static_cast<char *>(into), static_cast<std::streamsize>(bytes)));
        }

        // A frame's plain size (its header's content size) and its size in the file (its header, its blocks' headers
        // and sizes, its checksum), read at `packed` without decompressing it. False when the frame does not name
        // its content size (a stream the zstd tool wrote from a pipe) or cannot be read.
        bool frameExtent(std::filebuf *file, uint64_t packed, uint64_t &plainBytes, uint64_t &packedBytes)
        {
            unsigned char head[ZSTD_FRAMEHEADERSIZE_MAX];
            if (!seekRead(file, packed))
                return false;
            const size_t got = readSome(file, head, sizeof(head));
            ZSTD_frameHeader frame;
            if (ZSTD_getFrameHeader(&frame, head, got) != 0)
                return false;
            if (frame.frameType == ZSTD_skippableFrame)
            {
                plainBytes = 0;
                packedBytes = frame.headerSize + frame.frameContentSize;
                return true;
            }
            if (frame.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN)
                return false;
            uint64_t at = packed + frame.headerSize;
            for (;;)
            {
                unsigned char block[3];
                if (!seekRead(file, at) || readSome(file, block, 3) != 3)
                    return false;
                const uint32_t word = uint32_t(block[0]) | (uint32_t(block[1]) << 8) | (uint32_t(block[2]) << 16);
                const uint32_t type = (word >> 1) & 3u, size = word >> 3;
                if (type == 3u) // (reserved: not a frame)
                    return false;
                at += 3u + (type == 1u ? 1u : size); // (an RLE block holds its one byte)
                if (word & 1u)
                    break;
            }
            plainBytes = frame.frameContentSize;
            packedBytes = at + (frame.checksumFlag ? 4u : 0u) - packed;
            return true;
        }

        // The recorder's (and diffcall's) compression, one per thread: its context, and the buffers of the record
        // being written, kept for the next (a frame is made and written whole before the next begins).
        struct WriteScratch
        {
            ZSTD_CCtx *context = nullptr;
            std::string plain;
            std::vector<char> frame;
            ~WriteScratch() { ZSTD_freeCCtx(context); }
        };
        WriteScratch &writeScratch()
        {
            thread_local WriteScratch scratch;
            if (!scratch.context)
            {
                scratch.context = ZSTD_createCCtx();
                ZSTD_CCtx_setParameter(scratch.context, ZSTD_c_compressionLevel, kRecordLevel);
                ZSTD_CCtx_setParameter(scratch.context, ZSTD_c_checksumFlag, 1);
                ZSTD_CCtx_setParameter(scratch.context, ZSTD_c_contentSizeFlag, 1);
            }
            return scratch;
        }

        // Appends what is written to a string (a record's plain bytes, writeRecord's).
        class StringSink : public std::streambuf
        {
        public:
            explicit StringSink(std::string &out) : m_out(out) {}

        protected:
            int_type overflow(int_type c) override
            {
                if (!traits_type::eq_int_type(c, traits_type::eof()))
                    m_out.push_back(traits_type::to_char_type(c));
                return traits_type::not_eof(c);
            }
            std::streamsize xsputn(const char *s, std::streamsize n) override
            {
                m_out.append(s, static_cast<size_t>(n));
                return n;
            }

        private:
            std::string &m_out;
        };
    }

    bool isPacked(const void *bytes, size_t size)
    {
        if (size < 4u)
            return false;
        const uint32_t magic = littleWord(static_cast<const unsigned char *>(bytes));
        return magic == kZstdFrameMagic || (magic & kSkippableMagicMask) == kSkippableMagicBase;
    }

    bool recorderCompresses()
    {
        const char *plain = std::getenv("PS2X_FNREC_PLAIN");
        return !(plain && std::strcmp(plain, "1") == 0);
    }

    // ---- reading ---------------------------------------------------------------

    struct PackedReadBuffer::Context
    {
        ZSTD_DCtx *stream = ZSTD_createDCtx();
        std::unique_ptr<char[]> in{new char[ZSTD_DStreamInSize()]}, out{new char[ZSTD_DStreamOutSize()]};
        ~Context() { ZSTD_freeDCtx(stream); }
    };

    namespace
    {
        // The thread's contexts no reader holds now.
        std::vector<std::unique_ptr<PackedReadBuffer::Context>> &spareContexts()
        {
            thread_local std::vector<std::unique_ptr<PackedReadBuffer::Context>> spare;
            return spare;
        }
    }

    PackedReadBuffer::PackedReadBuffer()
    {
        auto &spare = spareContexts();
        if (spare.empty())
            m_zstd = std::make_unique<Context>();
        else
        {
            m_zstd = std::move(spare.back());
            spare.pop_back();
        }
        m_in = m_zstd->in.get();
        m_out = m_zstd->out.get();
        m_inSize = ZSTD_DStreamInSize();
        m_outSize = ZSTD_DStreamOutSize();
        setg(m_out, m_out, m_out);
    }

    PackedReadBuffer::~PackedReadBuffer()
    {
        auto &spare = spareContexts();
        if (spare.size() < 4u)
            spare.push_back(std::move(m_zstd));
    }

    void PackedReadBuffer::begin(std::filebuf *file, uint64_t plainBytes, std::vector<FrameStart> known)
    {
        m_file = file;
        m_plainBytes = plainBytes;
        m_frames = std::move(known);
        if (m_frames.empty())
            m_frames.push_back({0, 0});
        m_inPos = m_inEnd = 0;
        m_inBase = m_outBase = 0;
        m_inFrame = m_atEnd = false;
        m_problem.clear();
        ZSTD_DCtx_reset(m_zstd->stream, ZSTD_reset_session_only);
        setg(m_out, m_out, m_out);
    }

    void PackedReadBuffer::noteFrame(uint64_t plain, uint64_t packed)
    {
        if (packed > m_frames.back().packed)
            m_frames.push_back({plain, packed});
    }

    bool PackedReadBuffer::decode()
    {
        m_outBase += static_cast<uint64_t>(egptr() - eback());
        setg(m_out, m_out, m_out);
        if (m_atEnd)
            return false;
        ZSTD_outBuffer out{m_out, m_outSize, 0};
        while (out.pos == 0)
        {
            if (m_inPos == m_inEnd)
            {
                m_inBase += m_inEnd;
                m_inPos = 0;
                m_inEnd = readSome(m_file, m_in, m_inSize);
                if (m_inEnd == 0)
                {
                    m_atEnd = true;
                    if (m_inFrame)
                        m_problem = std::string(kCutInFrame) + " (after " +
                                    std::to_string(m_outBase) + " plain bytes, at byte " + std::to_string(m_inBase) + ")";
                    else if (m_plainBytes && m_outBase != m_plainBytes)
                        m_problem = "cut short: it decompresses to " + std::to_string(m_outBase) + " bytes, its head names " +
                                    std::to_string(m_plainBytes);
                    return false;
                }
            }
            if (!m_inFrame)
                noteFrame(m_outBase, m_inBase + m_inPos);
            m_inFrame = true;
            ZSTD_inBuffer in{m_in, m_inEnd, m_inPos};
            const size_t left = ZSTD_decompressStream(m_zstd->stream, &out, &in);
            m_inPos = in.pos;
            if (ZSTD_isError(left))
            {
                m_atEnd = true;
                m_problem = std::string("damaged: ") + ZSTD_getErrorName(left) + " in the frame that begins at byte " +
                            std::to_string(m_frames.back().packed);
                return false;
            }
            if (left == 0)
                m_inFrame = false; // (the frame ended, its checksum checked)
        }
        setg(m_out, m_out, m_out + out.pos);
        return true;
    }

    PackedReadBuffer::int_type PackedReadBuffer::underflow()
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        return decode() ? traits_type::to_int_type(*gptr()) : traits_type::eof();
    }

    bool PackedReadBuffer::restartAt(const FrameStart &frame)
    {
        if (!seekRead(m_file, frame.packed))
            return false;
        m_inBase = frame.packed;
        m_inPos = m_inEnd = 0;
        m_outBase = frame.plain;
        m_inFrame = m_atEnd = false;
        ZSTD_DCtx_reset(m_zstd->stream, ZSTD_reset_session_only);
        setg(m_out, m_out, m_out);
        return true;
    }

    PackedReadBuffer::pos_type PackedReadBuffer::seekTo(uint64_t plain)
    {
        const pos_type failed = pos_type(off_type(-1));
        const uint64_t decoded = m_outBase + static_cast<uint64_t>(egptr() - eback());
        if (plain < m_outBase || plain > decoded)
        {
            // From the nearest frame start at or before it, when that is behind what is decoded or past it.
            const auto after = std::upper_bound(m_frames.begin(), m_frames.end(), plain,
                                                [](uint64_t p, const FrameStart &f) { return p < f.plain; });
            FrameStart nearest = *(after - 1); // (m_frames[0] is the file's start)
            // Whole frames that end at or before it are passed by their headers, not decompressed (a skim of a big
            // recording reads its records' heads only, as on a plain file).
            if (plain > decoded && !m_atEnd)
            {
                uint64_t plainBytes = 0, packedBytes = 0;
                while (frameExtent(m_file, nearest.packed, plainBytes, packedBytes) && nearest.plain + plainBytes <= plain &&
                       packedBytes > 0)
                {
                    nearest = {nearest.plain + plainBytes, nearest.packed + packedBytes};
                    noteFrame(nearest.plain, nearest.packed);
                }
                if (nearest.plain <= decoded && !seekRead(m_file, m_inBase + m_inEnd)) // (decoding goes on where it was)
                    return failed;
            }
            if ((plain < m_outBase || nearest.plain > decoded) && !restartAt(nearest))
                return failed;
        }
        for (;;)
        {
            const uint64_t end = m_outBase + static_cast<uint64_t>(egptr() - eback());
            if (plain <= end)
            {
                setg(eback(), eback() + (plain - m_outBase), egptr());
                return pos_type(off_type(plain));
            }
            setg(eback(), egptr(), egptr());
            if (!decode())
                return failed;
        }
    }

    PackedReadBuffer::pos_type PackedReadBuffer::seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
    {
        if (!(which & std::ios_base::in))
            return pos_type(off_type(-1));
        int64_t target = off;
        if (dir == std::ios_base::cur)
            target += static_cast<int64_t>(position());
        else if (dir == std::ios_base::end)
        {
            if (!m_plainBytes)
                return pos_type(off_type(-1));
            target += static_cast<int64_t>(m_plainBytes);
        }
        if (target < 0)
            return pos_type(off_type(-1));
        return seekTo(static_cast<uint64_t>(target));
    }

    PackedReadBuffer::pos_type PackedReadBuffer::seekpos(pos_type pos, std::ios_base::openmode which)
    {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }

    RecordingReader::RecordingReader() : m_stream(std::make_unique<std::istream>(nullptr)) {}

    RecordingReader::~RecordingReader()
    {
        close();
    }

    void RecordingReader::close()
    {
        m_stream = std::make_unique<std::istream>(nullptr);
        m_packed.reset();
        m_file.reset();
    }

    bool RecordingReader::open(const std::filesystem::path &path, std::vector<FrameStart> known)
    {
        close();
        m_file = std::make_unique<std::filebuf>();
        if (!m_file->open(path, std::ios::in | std::ios::binary))
        {
            m_file.reset();
            return false;
        }
        unsigned char start[8 + sizeof(PackedHead)] = {};
        const size_t got = readSome(m_file.get(), start, sizeof(start));
        if (!seekRead(m_file.get(), 0))
            return false;
        if (isPacked(start, got))
        {
            PackedHead head;
            const uint64_t plainBytes = readHead(start, got, head) ? head.plainBytes : 0u;
            m_packed = std::make_unique<PackedReadBuffer>();
            m_packed->begin(m_file.get(), plainBytes, std::move(known));
            m_stream = std::make_unique<std::istream>(m_packed.get());
        }
        else
            m_stream = std::make_unique<std::istream>(m_file.get());
        return true;
    }

    std::vector<FrameStart> RecordingReader::frames() const
    {
        return m_packed ? m_packed->frames() : std::vector<FrameStart>{};
    }

    std::string RecordingReader::problem() const
    {
        return m_packed ? m_packed->problem() : std::string();
    }

    // ---- writing ---------------------------------------------------------------

    RecordingWriter::~RecordingWriter()
    {
        close();
    }

    void RecordingWriter::close()
    {
        if (m_file)
            std::fclose(m_file);
        m_file = nullptr;
    }

    bool RecordingWriter::create(const std::filesystem::path &path, const FileHeader &header, bool compressed)
    {
        close();
        m_file = openFile(path, true, false);
        if (!m_file)
            return false;
        m_compressed = compressed;
        if (!compressed)
            return writePlain(&header, sizeof(header));
        const uint32_t skippable[2] = {kPackedMagic, static_cast<uint32_t>(sizeof(PackedHead))};
        PackedHead head;
        head.header = header;
        if (std::fwrite(skippable, sizeof(skippable), 1, m_file) != 1 || std::fwrite(&head, sizeof(head), 1, m_file) != 1)
            return false;
        return writePlain(&header, sizeof(header));
    }

    bool RecordingWriter::append(const std::filesystem::path &path)
    {
        close();
        std::FILE *had = openFile(path, false, false);
        if (!had)
            return false;
        unsigned char start[8] = {};
        const size_t got = std::fread(start, 1, sizeof(start), had);
        std::fclose(had);
        const FileHeader plain;
        m_compressed = isPacked(start, got);
        if (!m_compressed && (got < sizeof(start) || std::memcmp(start, plain.magic, 6) != 0))
            return false;
        m_file = openFile(path, true, true);
        return m_file != nullptr;
    }

    bool RecordingWriter::write(const Record &record)
    {
        WriteScratch &scratch = writeScratch();
        scratch.plain.clear();
        StringSink sink(scratch.plain);
        std::ostream out(&sink);
        writeRecord(out, record);
        return writePlain(scratch.plain.data(), scratch.plain.size());
    }

    bool RecordingWriter::writePlain(const void *bytes, size_t size)
    {
        if (!m_file)
            return false;
        bool ok = true;
        if (!m_compressed)
            ok = std::fwrite(bytes, 1, size, m_file) == size;
        else
        {
            WriteScratch &scratch = writeScratch();
            scratch.frame.resize(ZSTD_compressBound(size));
            const size_t made = ZSTD_compress2(scratch.context, scratch.frame.data(), scratch.frame.size(), bytes, size);
            ok = !ZSTD_isError(made) && std::fwrite(scratch.frame.data(), 1, made, m_file) == made;
        }
        return std::fflush(m_file) == 0 && ok; // (runs are often ended by a kill)
    }
}
