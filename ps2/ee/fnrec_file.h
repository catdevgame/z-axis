#pragma once

// A recording's file (fncheck.h: a header, then records) as it is kept on disk: plain, or compressed with zstd
// (third_party/cmake/zx_zstd.cmake, the pinned library). Every reader and writer of a .fnrec goes through here, so a
// recording reads the same in either form and the name stays <address>.fnrec.
//
// The compressed form is a zstd stream whose decompressed bytes are the plain file, byte for byte (`zstd -d` gives
// it back). It begins with a skippable frame (kPackedMagic) holding a PackedHead: the plain file's header and size,
// for the tools that only want those (tools/checks/fnrec_io.py, the verdict cache's key: by the plain size, so a
// file compressed in place keeps its key). Then frames of whole records, each with its content size and checksum:
//   - the recorder writes one frame per record (ZX_FNREC_PLAIN=1: plain files), compressed at kRecordLevel with
//     one context for every file, so its memory does not grow with the files, and a run ended by a kill loses at
//     most the record it was writing; its head's plain size is 0 (not known while it records);
//   - the converter (tools/checks/fnrec_pack.py) groups a plain file's records into frames of at least 64 KiB at
//     level 9, and names the plain size (1 MiB frames were 2% smaller, but a callee probe's lookup of one record
//     decompressed half a frame on average: AI's sample spent 5.6x as long in the probes).
// A reader keeps where each frame it passed begins (plain offset, file offset), so a seek decompresses from the
// nearest frame start before it: at most a frame's bytes for a seek within what it has read. A seek ahead passes
// whole frames by their headers (each names its plain size) without decompressing them, so a skim of a big
// recording (skimRecord, the callee index) reads about what it reads of a plain file.
//
// A plain file whose last record was cut short reads its whole records and stops, as before. A compressed one
// reads the same way, and says what is wrong (RecordingReader::problem): a frame cut short, a damaged frame (its
// checksum), or a stream shorter than its head names.

#include "fncheck.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <istream>
#include <memory>
#include <streambuf>
#include <string>
#include <vector>

namespace fncheck
{
    // The skippable frame that begins a compressed recording (zstd's skippable magics are 0x184D2A50 to 5F).
    constexpr uint32_t kPackedMagic = 0x184D2A50u;
    // A compressed recording's head (the skippable frame's content).
    struct PackedHead
    {
        char tag[4] = {'F', 'N', 'R', 'Z'};
        uint32_t version = 1;
        uint64_t plainBytes = 0; // the plain file's size, 0 if the writer did not know it (the recorder)
        FileHeader header;       // the plain file's header
    };
    static_assert(sizeof(PackedHead) == 40, "file layout");

    // The recorder's compression level, and the converter's.
    constexpr int kRecordLevel = 3;
    constexpr int kPackLevel = 9;

    // Where a frame of a compressed file begins: its first byte's offset in the plain bytes and in the file.
    struct FrameStart
    {
        uint64_t plain = 0, packed = 0;
    };

    // The plain bytes of a compressed recording as a stream buffer (RecordingReader's).
    class PackedReadBuffer : public std::streambuf
    {
    public:
        PackedReadBuffer();
        ~PackedReadBuffer() override;
        PackedReadBuffer(const PackedReadBuffer &) = delete;
        PackedReadBuffer &operator=(const PackedReadBuffer &) = delete;
        // The decompression context and the buffers, a thread's kept for its next reader (the callee probes open
        // a reader per record they look up).
        struct Context;

        // The file's bytes from `file` (open, at its start); `known`: frame starts a reader of the same file found
        // before.
        void begin(std::filebuf *file, uint64_t plainBytes, std::vector<FrameStart> known);
        const std::vector<FrameStart> &frames() const { return m_frames; }
        const std::string &problem() const { return m_problem; }

    protected:
        int_type underflow() override;
        pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override;
        pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;

    private:
        bool decode();                     // the next output into the buffer; false at the end
        bool restartAt(const FrameStart &frame);
        pos_type seekTo(uint64_t plain);
        uint64_t position() const { return m_outBase + static_cast<uint64_t>(gptr() - eback()); }
        void noteFrame(uint64_t plain, uint64_t packed);

        std::filebuf *m_file = nullptr;
        std::unique_ptr<Context> m_zstd;
        char *m_in = nullptr, *m_out = nullptr;
        size_t m_inSize = 0, m_outSize = 0;
        size_t m_inPos = 0, m_inEnd = 0;   // the unread part of m_in
        uint64_t m_inBase = 0;             // m_in's first byte's offset in the file
        uint64_t m_outBase = 0;            // m_out's first byte's offset in the plain bytes
        uint64_t m_plainBytes = 0;         // the head's (0: not known)
        bool m_inFrame = false;            // a frame begun and not yet ended
        bool m_atEnd = false;
        std::vector<FrameStart> m_frames;  // by plain offset
        std::string m_problem;
    };

    // A recording opened for reading, in either form: stream() is its plain bytes (a header, then records), with
    // tellg and seekg in plain offsets.
    class RecordingReader
    {
    public:
        RecordingReader();
        ~RecordingReader();
        RecordingReader(const RecordingReader &) = delete;
        RecordingReader &operator=(const RecordingReader &) = delete;

        // False if the file cannot be opened (stream() then fails as an unopened ifstream does). `known`: frame
        // starts found by an earlier reader of the file (frames()).
        bool open(const std::filesystem::path &path, std::vector<FrameStart> known = {});
        std::istream &stream() { return *m_stream; }
        bool compressed() const { return m_packed != nullptr; }
        // The compressed file's frame starts passed so far (empty for a plain file).
        std::vector<FrameStart> frames() const;
        // What is wrong with a compressed file's bytes read so far, or empty.
        std::string problem() const;
        // The file closed (stream() then fails, as a closed ifstream's).
        void close();

    private:
        std::unique_ptr<std::filebuf> m_file; // (opened once: the plain form reads straight from it)
        std::unique_ptr<PackedReadBuffer> m_packed;
        std::unique_ptr<std::istream> m_stream;
    };

    // True if the bytes begin a compressed recording (a zstd frame or skippable frame: the head is optional, so a
    // file compressed with the zstd tool reads too).
    bool isPacked(const void *bytes, size_t size);

    // A recording opened for writing, a record at a time, each written through to the file as it is kept.
    class RecordingWriter
    {
    public:
        RecordingWriter() = default;
        ~RecordingWriter();
        RecordingWriter(const RecordingWriter &) = delete;
        RecordingWriter &operator=(const RecordingWriter &) = delete;

        // A new file (an existing one is replaced) with `header`, compressed or plain. False if it cannot be made.
        bool create(const std::filesystem::path &path, const FileHeader &header, bool compressed);
        // An existing file, records added at its end in its own form. False if it cannot be opened or is not a
        // recording.
        bool append(const std::filesystem::path &path);
        // One record, written and flushed (compressed: one frame).
        bool write(const Record &record);
        // Records already in the plain layout (writeRecord's bytes), written as one piece (compressed: one frame).
        bool writePlain(const void *bytes, size_t size);
        void close();
        bool compressed() const { return m_compressed; }

    private:
        std::FILE *m_file = nullptr;
        bool m_compressed = false;
        std::vector<char> m_frame; // a compressed frame's bytes
        std::string m_plain;       // a record's plain bytes
    };

    // ZX_FNREC_PLAIN=1: the recorder writes plain files (compressed by default).
    bool recorderCompresses();
}
