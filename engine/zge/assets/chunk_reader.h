#pragma once

// The ZGE3 engine's chunk files: GEO, TXR, ZIT, fonts and every other
// Z-Axis asset is an IFF-like tree of chunks,
//
//     char tag[4]; u32 size (little-endian, the body's bytes); body
//
// padded to 4 bytes. One generic parser walks
// them: a ChunkReader keeps a stack of ChunkFrames (one per chunk entered),
// parseChunks reads the children of the current chunk and calls the
// handler registered for each tag (addChunkHandler; the GEO ones in
// zge/assets/geo.h). A handler gets the reader and the loader's context;
// it reads the chunk's body with readChunk / readChunkData, or enters it
// (enterChunk, then parseChunks for its children, then leaveChunk).
//
// The reader works on a file handle (fileRead reads, fileSeek
// seeks); each frame keeps its own logical position, and leaving a chunk
// seeks the file back to the parent's.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "sym/zge/assets/chunk_reader.h.h"

namespace zge
{
    struct ZFile;

    // A chunk tag as the game compares it: the four characters read as a
    // little-endian u32 ("TGEO" = 0x4F454754).
    constexpr uint32_t chunkTag(const char (&name)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) |
               static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 8 |
               static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 16 |
               static_cast<uint32_t>(static_cast<uint8_t>(name[3])) << 24;
    }
    static_assert(chunkTag("TGEO") == 0x4F454754u);

    // The 8 bytes in front of every chunk, on disc and in the reader.
    struct ChunkHeader
    {
        uint32_t tag;  // chunkTag
        uint32_t size; // bytes of the body (the header not included)
    };
    static_assert(sizeof(ChunkHeader) == 8);

    // One level of the reader's chunk stack (0x20 bytes).
    struct ChunkFrame
    {
        ChunkHeader header;     // +0x00 the chunk this frame walks
        uint32_t start;         // +0x08 file offset of its header
        uint32_t end;           // +0x0C file offset past its body (start + 8 + size)
        uint32_t position;      // +0x10 file offset this frame has read up to
        ChunkHeader current;    // +0x14 the child chunk being handled (what handlers read)
        uint32_t currentStart;  // +0x1C file offset of the child's header
    };
    static_assert(sizeof(ChunkFrame) == 0x20);
    static_assert(offsetof(ChunkFrame, position) == 0x10 && offsetof(ChunkFrame, current) == 0x14 &&
                  offsetof(ChunkFrame, currentStart) == 0x1C);

    // The reader a loader opens on a file (openChunkFile: 340 bytes, a
    // record of the parser's reader pool).
    struct ChunkReader
    {
        ee::Ptr<ZFile> file;        // +0x00 fileRead / fileSeek
        // +0x04 called after each chunk a handler took, (reader, context)
        // (setAfterChunkHook; 0: none).
        uint32_t afterChunk;
#if ZX_GAME_BMX
        // BMX +0x08: where the file stood when the reader was opened (openChunkFile asks filePosition): ZGE4's
        // seekChunk seeks from there, so a chunk file can sit inside a bigger one
        uint32_t openPosition;
#endif
        uint32_t totalBytes;        // +0x08 (BMX +0x0C) the whole load's size, for the progress hook
        uint32_t baseOffset;        // +0x0C (BMX +0x10) where this file starts in the whole load (a chunk read into memory: its outer file's position)
        char name[0x40];            // +0x10 (BMX +0x14) the file's name (handed to the progress hook)
        ee::Ptr<ChunkFrame> frame;  // +0x50 (BMX +0x54) the innermost chunk entered; null before the first
        ChunkFrame frames[8];       // +0x54 (BMX +0x58)
    };
    // (BMX XXX's offsets as its openChunkFile, closeChunkFile, loadObjectChunks and initChunkParser's pool use them)
    static_assert(offsetof(ChunkReader, totalBytes) == game::readable::perGame(0x08u, 0x08u, 0x0Cu) &&
                  offsetof(ChunkReader, baseOffset) == game::readable::perGame(0x0Cu, 0x0Cu, 0x10u) &&
                  offsetof(ChunkReader, name) == game::readable::perGame(0x10u, 0x10u, 0x14u) &&
                  offsetof(ChunkReader, frame) == game::readable::perGame(0x50u, 0x50u, 0x54u) &&
                  offsetof(ChunkReader, frames) == game::readable::perGame(0x54u, 0x54u, 0x58u) &&
                  sizeof(ChunkReader) == game::readable::perGame(340u, 340u, 0x158u));

    // The file offset a chunk offset is from: ZGE4's reader keeps where its file stood when opened, ZGE2's and ZGE3's
    // offsets are from the file's start.
    inline uint32_t chunkFileOrigin(const ChunkReader &reader)
    {
#if ZX_GAME_BMX
        return reader.openPosition;
#else
        (void)reader;
        return 0;
#endif
    }

    // A chunk handler's record (16 bytes, a record of the parser's handler
    // pool): the handlers are a list, the newest first.
    struct ChunkHandlerRecord
    {
        uint32_t tag;                           // +0x00 chunkTag
        uint32_t handler;                       // +0x04 (reader, context) -> non-zero to go on: a game function
        ee::Ptr<ChunkHandlerRecord> next;       // +0x08
        uint32_t unused;                        // +0x0C written 0, read by nothing
    };
    static_assert(sizeof(ChunkHandlerRecord) == 0x10);

    // The progress hook (ChunkParser::progressHook): how far a load is. BMX XXX's takes the context set with it too.
#if ZX_GAME_BMX
    using ChunkProgressHook = ee::FunctionPtr<void(const char *name, uint32_t bytesDone, uint32_t totalBytes, uint32_t context)>;
#else
    using ChunkProgressHook = ee::FunctionPtr<void(const char *name, uint32_t bytesDone, uint32_t totalBytes)>;
#endif

    // The parser's globals (g_chunkParser, 40 bytes; ziff.c).
    struct ChunkParser
    {
        uint32_t ready;             // +0x00 set by initChunkParser
        uint32_t readerPool;        // +0x04 ChunkReader records, 16 a block
        uint32_t handlerPool;       // +0x08 ChunkHandlerRecord records, 64 a block
        ee::Ptr<ChunkHandlerRecord> handlers; // +0x0C
        uint32_t traceEnabled;      // +0x10 non-zero: traceChunk walks every chunk entered (ZGE4: flags, kParserStepPastUnregistered)
        ChunkHeader rootHeader;     // +0x14 the file's top chunk, for the first enterChunk
        uint32_t rootPosition;      // +0x1C file offset past the top chunk's header
        uint32_t lastProgress;      // +0x20 when the progress hook was last called (collisionClock)
        // +0x24 called at most every 0x33 ticks while chunks are parsed and
        // when a reader is closed (0: none). BMX XXX's takes the context set
        // with it too.
        ChunkProgressHook progressHook;
#if ZX_GAME_BMX
        uint32_t progressContext;   // BMX +0x28 the progress hook's last argument (setChunkProgressHook's second)
#endif
    };
    static_assert(offsetof(ChunkParser, rootHeader) == 0x14 && offsetof(ChunkParser, rootPosition) == 0x1C &&
                  offsetof(ChunkParser, progressHook) == 0x24);
    static_assert(sizeof(ChunkParser) == game::readable::perGame(40u, 40u, 44u));
    inline ee::Global<ChunkParser, sym::g_chunkParser> g_chunkParser;
    // ZGE4's parser flag: a child no handler is registered for is stepped past by dispatchChunk itself (nextChunk).
    constexpr uint32_t kParserStepPastUnregistered = 0x4u;
    // Whether traceChunk walks each chunk entered: ZGE2 and ZGE3 trace on any setting but 0, ZGE4 on flag bit 0.
    inline bool chunkTraceOn(const ChunkParser &parser)
    {
        return (parser.traceEnabled & game::readable::perGame(~0u, ~0u, 0x1u)) != 0;
    }

    // ZGE4's reader keeps where its file stood when it was opened (chunkFileOrigin).
    inline void keepChunkFileOrigin(ChunkReader &reader, uint32_t position)
    {
#if ZX_GAME_BMX
        reader.openPosition = position;
#else
        (void)reader;
        (void)position;
#endif
    }

    // The progress hook called as each game's takes it: BMX XXX's with the context set with it.
    inline void callProgressHook(const ChunkParser &parser, const char *name, uint32_t bytesDone, uint32_t totalBytes)
    {
#if ZX_GAME_BMX
        parser.progressHook(name, bytesDone, totalBytes, parser.progressContext);
#else
        parser.progressHook(name, bytesDone, totalBytes);
#endif
    }

    // The progress hook's context set (ZGE4's: ZGE2 and ZGE3 keep none).
    inline void setProgressContext(ChunkParser &parser, uint32_t context)
    {
#if ZX_GAME_BMX
        parser.progressContext = context;
#else
        (void)parser;
        (void)context;
#endif
    }

    // Where seekChunk measures from.
    enum class ChunkSeek : int32_t
    {
        FromBody = 0,    // the current frame's body start
        FromCurrent = 1, // the current frame's position
    };

    // A chunk handler: reads its chunk, returns non-zero to go on.
    using ChunkHandler = int32_t (*)(ChunkReader *reader, void *context);

    // chunkDataSize: the body size of the chunk being handled.
    uint32_t chunkDataSize(ChunkReader *reader);
    // chunkBytesRead: bytes of it consumed so far, its header included.
    uint32_t chunkBytesRead(ChunkReader *reader);
    // readChunkHeader: reads the next child's header at the frame's position.
    int32_t readChunkHeader(ChunkReader *reader);
    // readChunk: reads up to maxBytes of the chunk's body that are left;
    // returns the bytes asked for (at most what is left).
    uint32_t readChunk(ChunkReader *reader, ee::Address dst, uint32_t maxBytes);
    // readChunkData: reads the whole body.
    uint32_t readChunkData(ChunkReader *reader, ee::Address dst);
    // readChunkU32: reads a u32.
    uint32_t readChunkU32(ChunkReader *reader, ee::Address dst);
    // seekChunk: moves the current frame (and the file) to an offset;
    // -1 when that is the frame's end, else the new position.
    int32_t seekChunk(ChunkReader *reader, ChunkSeek whence, uint32_t offset);
    // enterChunk: enters the chunk being handled (a new frame) and reads
    // its header again. Always 1.
    int32_t enterChunk(ChunkReader *reader);
    // leaveChunk: leaves it, back to the parent's position; 0 if the
    // parent has nothing left.
    int32_t leaveChunk(ChunkReader *reader);
}

namespace zge
{
    // (chunk_reader.cpp)
    int32_t initChunkParser();
    uint32_t shutDownChunkParser();
    // chunkProgressHook: the progress hook set (ChunkParser::progressHook).
    ChunkProgressHook chunkProgressHook();
    ee::Ptr<ChunkReader> openChunkFile(ee::Ptr<ZFile> file, uint32_t formTag, uint32_t bytes);
    int32_t closeChunkFile(ChunkReader *reader);
    int32_t parseChunks(ChunkReader *reader, ee::Address context);
    // The progress hook set (always 1); ZGE4's takes the context its hook is called with too.
    uint32_t setChunkProgressHook(ChunkProgressHook hook READABLE_PARAM_ONLY(bmx, uint32_t context));
    // Tracing set: the old setting.
    uint32_t setChunkTrace(uint32_t enabled);
    int32_t readChunkFile(ee::Address path, ee::Address context, uint32_t formTag);
    int32_t includeChunkFile(ChunkReader *reader, ee::Address context);
    int32_t loadObjectChunks(ChunkReader *reader, ee::Address context);
    ee::Ptr<ChunkHandlerRecord> addChunkHandler(uint32_t tag, uint32_t handler);
    int32_t readChunkWord(ChunkReader *reader, ee::Address dst);
    int32_t nextChunk(ChunkReader *reader);
    int32_t dispatchChunk(ChunkReader *reader, ee::Address context);
    uint32_t registerLightChunkHandlers();
    uint32_t shutdownLitoChunks();
    uint32_t shutdownChunkHandlers();
    void registerWsmChunkHandlers();
}
