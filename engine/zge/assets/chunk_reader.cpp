// The chunk files' parser (ziff.c; chunk_reader.h): a chunk file opened
// (its top chunk checked against the form's tag and entered), its
// children parsed with the handler registered for each tag, the reading
// of a chunk's body, the frame stack entered and left, a whole file read
// into memory first when it fits, a chunk read into memory and parsed as a
// file of its own, and the progress hook told as the load goes on.
//

#include "chunk_reader.h"
#include "file.h"
#include "game_address.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/container/cursor_array.h"
#include "zge/container/list.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/render/light_objects.h"
#include "zge/render/view_regions.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"
#include "zge/collision/collision.h"
#include "zge/scene/camera.h"
#include "zge/scene/visibility_regions.h"
#include "sym/zge/assets/chunk_reader.cpp.h"

#include <cstring>

namespace zge
{
    uint32_t chunkDataSize(ChunkReader *reader)
    {
        return reader->frame->current.size;
    }
    READABLE_FUNCTION(chunkDataSize, Verified);

    uint32_t chunkBytesRead(ChunkReader *reader)
    {
        const ChunkFrame *frame = reader->frame;
        return frame->position - frame->currentStart;
    }
    READABLE_FUNCTION(chunkBytesRead, Verified);

    int32_t readChunkHeader(ChunkReader *reader)
    {
        ChunkFrame *frame = reader->frame;
        frame->currentStart = frame->position;
        const int32_t got = fileRead(reader->file, &reader->frame->current, static_cast<uint32_t>(sizeof(ChunkHeader)), 0);
        reader->frame->position += static_cast<uint32_t>(got);
        return got;
    }
    READABLE_FUNCTION_WAITS(readChunkHeader, Verified);

    uint32_t readChunk(ChunkReader *reader, ee::Address dst, uint32_t maxBytes)
    {
        // What is left of the body: its size less what was read since the
        // header (chunkBytesRead counts the 8 header bytes).
        const uint32_t size = chunkDataSize(reader);
        const uint32_t consumed = chunkBytesRead(reader);
        const uint32_t left = size - (consumed - static_cast<uint32_t>(sizeof(ChunkHeader)));
        const uint32_t bytes = left < maxBytes ? left : maxBytes;
        const int32_t got = fileRead(reader->file, dst, bytes, 0);
        reader->frame->position += static_cast<uint32_t>(got);
        // The bytes asked for, even if the file gave fewer.
        return bytes;
    }
    READABLE_FUNCTION_WAITS(readChunk, Verified);

    uint32_t readChunkData(ChunkReader *reader, ee::Address dst)
    {
        const uint32_t size = chunkDataSize(reader);
        return readChunk(reader, dst, size);
    }
    READABLE_FUNCTION_WAITS(readChunkData, Verified);

    uint32_t readChunkU32(ChunkReader *reader, ee::Address dst)
    {
        return readChunk(reader, dst, 4u);
    }
    READABLE_FUNCTION_WAITS(readChunkU32, Verified);

    int32_t seekChunk(ChunkReader *reader, ChunkSeek whence, uint32_t offset)
    {
        uint32_t target = 0; // any other whence: offset 0 (an end-relative seek was never written)
        if (whence == ChunkSeek::FromBody)
            target = reader->frame->start + (offset + static_cast<uint32_t>(sizeof(ChunkHeader)));
        else if (whence == ChunkSeek::FromCurrent)
            target = reader->frame->position + offset;
        fileSeek(reader->file, chunkFileOrigin(*reader) + target, 0, 0);
        ChunkFrame *frame = reader->frame;
        frame->position = target;
        return target != frame->end ? static_cast<int32_t>(target) : -1;
    }
    READABLE_FUNCTION_WAITS(seekChunk, Verified);

    int32_t traceChunk(ChunkReader *reader);

    int32_t enterChunk(ChunkReader *reader)
    {
        // Push a frame for the chunk being handled: its header is the
        // parent's current child (or, for the file's first chunk, the one
        // the parser was given), and it starts 8 bytes before the parent's
        // position, which is past that header.
        ChunkFrame *parent = reader->frame;
        if (!parent)
        {
            reader->frame = ee::Ptr<ChunkFrame>(&reader->frames[0]);
            reader->frames[0].header = g_chunkParser->rootHeader;
            reader->frame->start = g_chunkParser->rootPosition - static_cast<uint32_t>(sizeof(ChunkHeader));
        }
        else
        {
            ChunkFrame *frame = parent + 1;
            reader->frame = ee::Ptr<ChunkFrame>(frame);
            frame->header = parent->current;
            reader->frame->start = parent->position - static_cast<uint32_t>(sizeof(ChunkHeader));
        }
        ChunkFrame *frame = reader->frame;
        frame->end = frame->start + frame->header.size + static_cast<uint32_t>(sizeof(ChunkHeader));
        frame->position = frame->start;
        frame->currentStart = frame->position;
        if (static_cast<int32_t>(frame->currentStart) < 0)
        {
            // No file offset to read from: take the header as known.
            frame->current = frame->header;
            frame->position += static_cast<uint32_t>(sizeof(ChunkHeader));
        }
        else
        {
            // Read the chunk's own header again; the frame's position ends
            // at its body.
            readChunkHeader(reader);
        }
        if (chunkTraceOn(*g_chunkParser))
            traceChunk(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterChunk, Verified);

    int32_t leaveChunk(ChunkReader *reader)
    {
        reader->frame = reader->frame - 1;
        // Put the file back where the parent was.
        return seekChunk(reader, ChunkSeek::FromCurrent, 0u) != -1;
    }
    READABLE_FUNCTION_WAITS(leaveChunk, Verified);

    // ---- the parser ----------------------------------------------------------

    namespace
    {
        // A form tag that is not checked (the whole file one chunk).
        constexpr uint32_t kAnyForm = 0xFFFFFFFFu;
        constexpr uint32_t kZlzoForm = chunkTag("ZLZO"); // (ZGE4: a file openChunkFile takes as any form)
        // How often parseChunks tells the progress hook, in collisionClock ticks.
        constexpr uint32_t kProgressTicks = 0x33u;
        // readChunkFile reads a file into memory when it leaves this much free.
        constexpr uint32_t kSpareMemory = 0x400u;
        constexpr uint32_t kZge2MemoryFileBytes = 0x20000u; // (ZGE2: readChunkFile reads a smaller file into the heap)
        constexpr uint32_t kHandlersPerBlock = game::readable::perGame(0x40u, 0x40u, 0x60u);
        constexpr uint32_t kReadersPerBlock = 0x10;
        // The longest include path, its directory and name.
        constexpr size_t kIncludePathBytes = 0xC0;

        int32_t callChunkHandler(uint32_t handler, ChunkReader *reader, uint32_t context) { return ee::game<int32_t>(handler, reader, context); }
        void callAfterChunkHook(uint32_t hook, ChunkReader *reader, uint32_t context) { ee::game(hook, reader, context); }
        // poolFree, whose result closeChunkFile passes on.
        int32_t freeChunkReader(uint32_t pool, ChunkReader *reader) { return ee::game<int32_t>(sym::poolFree, pool, reader); }
    }

    // setChunkProgressHook, chunkProgressHook: the progress hook set (always 1), read. ZGE4's takes the context
    // its hook is called with too.
    uint32_t setChunkProgressHook(ChunkProgressHook hook READABLE_PARAM_ONLY(bmx, uint32_t context = 0))
    {
        g_chunkParser->progressHook = hook;
        if constexpr (game::readable::kIsBmx)
            setProgressContext(*g_chunkParser, context);
        return 1;
    }
    READABLE_FUNCTION(setChunkProgressHook, Verified);

    ChunkProgressHook chunkProgressHook() { return g_chunkParser->progressHook; }
    READABLE_FUNCTION(chunkProgressHook, Verified);

    // findChunkHandler: the record of the handler registered for a tag (in a
    // parser's list, or the global one for 0), or 0.
    ee::Ptr<ChunkHandlerRecord> findChunkHandler(uint32_t tag, ee::Ptr<ChunkParser> parser)
    {
        ee::Ptr<ChunkHandlerRecord> handler = parser ? parser->handlers : g_chunkParser->handlers;
        for (; handler; handler = handler->next)
            if (handler->tag == tag)
                return handler;
        return ee::Ptr<ChunkHandlerRecord>();
    }
    READABLE_FUNCTION(findChunkHandler, Verified);

    // chunkAtEnd: whether the current chunk has no child left.
    int32_t chunkAtEnd(ChunkReader *reader)
    {
        const ChunkFrame *frame = reader->frame;
        return static_cast<int32_t>(static_cast<int32_t>(frame->position) < static_cast<int32_t>(frame->end)) ^ 1;
    }
    READABLE_FUNCTION(chunkAtEnd, Verified);

    // initChunkParser: the parser set up (its pools). Always 1.
    int32_t initChunkParser()
    {
        memoryFill(g_chunkParser.get(), 0, sizeof(ChunkParser));
        ChunkParser &parser = *g_chunkParser;
        parser.handlerPool = createPool(sizeof(ChunkHandlerRecord), kHandlersPerBlock, 0);
        parser.readerPool = createPool(sizeof(ChunkReader), kReadersPerBlock, 0);
        parser.ready = 1;
        parser.progressHook = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initChunkParser, Verified);

    // shutDownChunkParser: the parser shut down. Always 1.
    uint32_t shutDownChunkParser()
    {
        ChunkParser &parser = *g_chunkParser;
        destroyPool(ee::Ptr<Pool>(parser.handlerPool));
        destroyPool(ee::Ptr<Pool>(parser.readerPool));
        parser.ready = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownChunkParser, Verified);

    // setChunkTrace: tracing set (the old setting).
    uint32_t setChunkTrace(uint32_t enabled)
    {
        const uint32_t previous = g_chunkParser->traceEnabled;
        g_chunkParser->traceEnabled = enabled;
        return previous;
    }
    READABLE_FUNCTION(setChunkTrace, Verified);

    // openChunkFile: a reader on a file, its top chunk entered: read from
    // the file and checked against the form's tag, or (kAnyForm) the
    // whole file as one chunk of `bytes` (0: the file's size). A form that
    // does not match frees the reader, which the original then fills in
    // and returns all the same.
    ee::Ptr<ChunkReader> openChunkFile(ee::Ptr<ZFile> file, uint32_t formTag, uint32_t bytes)
    {
        ee::Local<uint32_t> size;
        ChunkParser &parser = *g_chunkParser;
        const ee::Ptr<ChunkReader> reader(poolAlloc(ee::Ptr<Pool>(parser.readerPool)));
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4: the reader's offsets are from where the file stands now.
            ee::Local<uint32_t> position;
            filePosition(file, position, 0);
            keepChunkFileOrigin(*reader, *position);
        }
        if (formTag == kAnyForm)
        {
            size = bytes;
            if (bytes == 0)
                fileSize(file, size, 0);
            parser.rootHeader.size = *size;
            parser.rootHeader.tag = formTag;
            parser.rootPosition = 0;
        }
        else
        {
            fileRead(file, &parser.rootHeader, sizeof(ChunkHeader), 0);
            parser.rootPosition = sizeof(ChunkHeader);
            // (ZGE4 takes a ZLZO file whatever form was asked for.)
            if (parser.rootHeader.tag != formTag && !(game::readable::kIsBmx && parser.rootHeader.tag == kZlzoForm))
                poolFree(ee::Ptr<Pool>(parser.readerPool), reader);
        }
        reader->file = file;
        reader->frame = ee::Ptr<ChunkFrame>();
        reader->afterChunk = 0;
        reader->baseOffset = 0;
        reader->totalBytes = parser.rootHeader.size;
        stringCopy(reader->name, filePath(file));
        enterChunk(reader);
        if (chunkTraceOn(parser))
            traceChunk(reader);
        return reader;
    }
    READABLE_FUNCTION_WAITS(openChunkFile, Verified);

    namespace
    {
        // The progress hook told how far the load is.
        void tellProgressHook(ChunkReader *reader)
        {
            ee::Local<uint32_t> position;
            ChunkParser &parser = *g_chunkParser;
            parser.lastProgress = collisionClock();
            filePosition(reader->file, position, 0);
            callProgressHook(parser, reader->name, reader->baseOffset + *position, reader->totalBytes);
        }
    }

    // closeChunkFile: a reader closed: the progress hook told how far the
    // load is, then the record freed.
    int32_t closeChunkFile(ChunkReader *reader)
    {
        ChunkParser &parser = *g_chunkParser;
        if (parser.progressHook)
            tellProgressHook(reader);
        return freeChunkReader(parser.readerPool, reader);
    }
    READABLE_FUNCTION_WAITS(closeChunkFile, Verified);

    // parseChunks: the current chunk's children parsed: each dispatched to
    // its handler, until one says stop (the result 0) or none is left (1).
    // The progress hook is told every kProgressTicks.
    int32_t parseChunks(ChunkReader *reader, ee::Address context)
    {
        if (seekChunk(reader, ChunkSeek::FromBody, 0u) == -1)
            return 1;
        readChunkHeader(reader);
        int32_t verdict = 1;
        bool finished = false;
        while (!finished)
        {
            if (dispatchChunk(reader, context) == 0)
            {
                verdict = 0;
                finished = true;
            }
            else if (nextChunk(reader) == -1)
                finished = true;

            ChunkParser &parser = *g_chunkParser;
            if (parser.progressHook && collisionClock() - parser.lastProgress >= kProgressTicks)
                tellProgressHook(reader);
        }
        return verdict;
    }
    READABLE_FUNCTION_WAITS(parseChunks, Verified);

    // setAfterChunkHook: the hook called after each chunk a handler took (always
    // 1).
    uint32_t setAfterChunkHook(ChunkReader *reader, uint32_t hook)
    {
        reader->afterChunk = hook;
        return 1;
    }
    READABLE_FUNCTION(setAfterChunkHook, Verified);

    // readChunkFile: a chunk file read: opened, its form checked, then read
    // into memory whole when it fits (with kSpareMemory to spare) and parsed
    // there, else parsed from the file. The handlers' verdict (0: not opened).
    int32_t readChunkFile(ee::Address path, ee::Address context, uint32_t formTag)
    {
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        if (!file)
            return 0;
        g_chunkParser->lastProgress = 0;
        const ee::Ptr<ChunkReader> reader = openChunkFile(file, formTag, 0);
        if (!reader)
        {
            closeFile(file, 0);
            return 0;
        }
        // ZGE2 reads a file under 128 KB into the heap; ZGE3 one that leaves kSpareMemory of main memory free.
        bool intoMemory;
        uint32_t size;
        if constexpr (game::readable::kIsDm2)
        {
            size = g_chunkParser->rootHeader.size;
            intoMemory = size < kZge2MemoryFileBytes;
        }
        else
        {
            const uint32_t freeBytes = mainRegionFreeBytes();
            size = g_chunkParser->rootHeader.size;
            intoMemory = size < freeBytes - kSpareMemory;
        }
        const uint32_t bytes = size + sizeof(ChunkHeader);
        if (intoMemory)
        {
            const uint32_t memory = game::readable::kIsDm2 ? heapAllocate(bytes) : allocateMemory(bytes);
            fileSeek(file, 0, kSeekFromStart, 0);
            fileRead(file, memory, bytes, 0);
            closeFile(file, 0);
            const ee::Ptr<ZFile> memoryFile = openMemoryFile(memory, bytes);
            closeChunkFile(reader);
            const ee::Ptr<ChunkReader> memoryReader = openChunkFile(memoryFile, formTag, 0);
            stringCopy(memoryReader->name, path);
            const int32_t verdict = parseChunks(memoryReader, context);
            closeChunkFile(memoryReader);
            closeFile(memoryFile, 0);
            if constexpr (game::readable::kIsDm2)
                heapFree(memory);
            else
                freeMemory(memory);
            return verdict;
        }
        const int32_t verdict = parseChunks(reader, context);
        closeChunkFile(reader);
        closeFile(file, 0);
        return verdict;
    }
    READABLE_FUNCTION_WAITS(readChunkFile, Verified);

    // reportIncludedFile (ZGE4 only): what an included file took of the heap
    // and its size, for the debug output: the GameCube build prints them
    // ("initial memory usage: %u KB", "file size: %u KB"); in the PS2 build
    // the prints are gone and what is left asks the heap's free bytes, opens
    // the file, asks its size and closes it, the answers unused (the file is
    // closed whether or not it opened).
    inline void reportIncludedFile(ee::Address path, uint32_t /*freeBefore*/)
    {
        freeHeapBytes();
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        ee::Local<uint32_t> fileBytes;
        fileSize(file, fileBytes, 0);
        closeFile(file, 0);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, reportIncludedFile);

    // includeChunkFile: a chunk handler: the chunk names another chunk file
    // (FORM), read from the reader's file's directory with the same
    // context. 1 when it was read. ZGE4 reports the file when the parser
    // asks for it (kParserReportIncludes), with the heap's free bytes from
    // before it was read.
    int32_t includeChunkFile(ChunkReader *reader, ee::Address context)
    {
        ee::Local<char[kIncludePathBytes]> path;
        const uint32_t nameBytes = chunkDataSize(reader);
        uint32_t freeBefore = 0;
        if constexpr (game::readable::kIsBmx)
        {
            if (chunkIncludesReported(*g_chunkParser))
                freeBefore = freeHeapBytes();
        }
        const uint32_t name = heapAllocate(nameBytes);
        readChunkData(reader, name);
        pathDirectory(reader->name, path);
        appendString(path, name);
        const int32_t read = readChunkFile(path, context, chunkTag("FORM"));
        heapFree(name);
        if (read == 0)
            return 0;
        if constexpr (game::readable::kIsBmx)
        {
            if (chunkIncludesReported(*g_chunkParser))
                reportIncludedFile(path, freeBefore);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(includeChunkFile, Verified);

    // loadObjectChunks: a chunk's own chunks parsed with the context: entered
    // in place when the file is in memory already, else the chunk's body
    // read into memory and parsed as a file of its own (its progress kept
    // in the outer file's). The handlers' verdict (1 for an empty chunk).
    int32_t loadObjectChunks(ChunkReader *reader, ee::Address context)
    {
        if (isMemoryFile(reader->file) != 0)
        {
            if (enterChunk(reader) == 0)
                return 1;
            const int32_t verdict = parseChunks(reader, context);
            leaveChunk(reader);
            return verdict;
        }
        const uint32_t bytes = chunkDataSize(reader);
        if (bytes == 0)
            return 1;
        const uint32_t data = heapAllocate(bytes);
        readChunkData(reader, data);
        const ee::Ptr<ZFile> file = openMemoryFile(data, bytes);
        const ee::Ptr<ChunkReader> inner = openChunkFile(file, kAnyForm, bytes);
        inner->totalBytes = reader->totalBytes;
        filePosition(reader->file, &inner->baseOffset, 0);
        stringCopy(inner->name, reader->name);
        const int32_t verdict = parseChunks(inner, context);
        closeChunkFile(inner);
        closeFile(file, 0);
        heapFree(data);
        return verdict;
    }
    READABLE_FUNCTION_WAITS(loadObjectChunks, Verified);

    uint32_t currentChunkId(ChunkReader *reader);

    // traceChunk: the chunk entered traced: each child's tag (the
    // messages are gone in this build), the file left at the last.
    int32_t traceChunk(ChunkReader *reader)
    {
        seekChunk(reader, ChunkSeek::FromBody, 0u);
        readChunkHeader(reader);
        for (;;)
        {
            const int32_t atEnd = chunkAtEnd(reader);
            if (atEnd != 0)
                return atEnd;
            currentChunkId(reader);
            nextChunk(reader);
        }
    }
    READABLE_FUNCTION_WAITS(traceChunk, Verified);

    // addChunkHandler: a handler registered for a tag (once: a tag registered
    // already keeps its handler); the tag's record.
    ee::Ptr<ChunkHandlerRecord> addChunkHandler(uint32_t tag, uint32_t handler)
    {
        const ee::Ptr<ChunkHandlerRecord> registered = findChunkHandler(tag, 0);
        if (registered)
            return registered;
        ChunkParser &parser = *g_chunkParser;
        const ee::Ptr<ChunkHandlerRecord> record(poolAlloc(ee::Ptr<Pool>(parser.handlerPool)));
        record->tag = tag;
        record->handler = handler;
        record->unused = 0;
        record->next = parser.handlers;
        parser.handlers = record;
        return record;
    }
    READABLE_FUNCTION_WAITS(addChunkHandler, Verified);

    // currentChunkId: the tag of the child being handled.
    uint32_t currentChunkId(ChunkReader *reader) { return reader->frame->current.tag; }
    READABLE_FUNCTION(currentChunkId, Verified);

    // readChunkWord: a word of the chunk's body (readChunk's result); the
    // same code as readChunkU32.
    int32_t readChunkWord(ChunkReader *reader, ee::Address dst)
    {
        return static_cast<int32_t>(readChunk(reader, dst, 4u));
    }
    READABLE_FUNCTION_WAITS(readChunkWord, Verified);

    // nextChunk: on to the next child: past the one being handled, its
    // header read; the frame's position, or -1 at the end.
    int32_t nextChunk(ChunkReader *reader)
    {
        const ChunkFrame *frame = reader->frame;
        if (seekChunk(reader, ChunkSeek::FromBody, frame->currentStart + frame->current.size - frame->start) == -1)
            return -1;
        readChunkHeader(reader);
        const int32_t position = static_cast<int32_t>(reader->frame->position);
        return position;
    }
    READABLE_FUNCTION_WAITS(nextChunk, Verified);

    // dispatchChunk: the child being handled given to its tag's handler
    // (unregistered tags are skipped), then the reader's after-chunk hook.
    // 0 when the handler says stop.
    int32_t dispatchChunk(ChunkReader *reader, ee::Address context)
    {
        if (chunkAtEnd(reader) != 0)
            return 1;
        const ee::Ptr<ChunkHandlerRecord> handler = findChunkHandler(currentChunkId(reader), 0);
        if (handler)
        {
            if (callChunkHandler(handler->handler, reader, context) == 0)
                return 0;
            if (reader->afterChunk != 0)
                callAfterChunkHook(reader->afterChunk, reader, context);
        }
        else if constexpr (game::readable::kIsBmx)
        {
            if (g_chunkParser->traceEnabled & kParserStepPastUnregistered)
                nextChunk(reader);
        }
        return 1;
    }
    READABLE_FUNCTION(dispatchChunk, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_litoChunksReady> g_litoChunksReady;
        inline ee::Global<ee::Ptr<Pool>, sym::g_litoChunkPool> g_litoChunkPool; // LitoRecords
        inline ee::Global<List, sym::g_litoRecords> g_litoRecords;
        inline ee::Global<int32_t, sym::g_regionsReady> g_regionsReady;
        inline ee::Global<int32_t, sym::g_regions> g_regions; // g_regions ((anonymous)::g_regions)
        inline ee::Global<int32_t, sym::g_zpdnChunkHandlersRegistered> g_zpdnChunkHandlersRegistered;
        inline ee::Global<int32_t, sym::g_zpdnChunkPool> g_zpdnChunkPool;
        inline ee::Global<int32_t, sym::g_ffdChunkHandlerReady> g_ffdChunkHandlerReady;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_emitterDefinitions> g_emitterDefinitions;
        inline ee::Global<int32_t, sym::g_emitterListReady> g_emitterListReady;
    }

    int32_t readZlitChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t context)
    {
        return loadObjectChunks(chunkReader, context);
    }
    READABLE_FUNCTION_WAITS(readZlitChunk, Verified);

    uint32_t readLithChunk()
    {
        return 1;
    }
    READABLE_FUNCTION(readLithChunk, Draft);

    // A LITO chunk's body: one of the level's lights.
    struct LitoChunk
    {
        uint32_t hash; // +0x00
        // +0x04 bits 0-9 the light's type, 10-25 the object groups it
        // lights, 26 a flag the light does not keep.
        uint32_t typeAndGroups;
        Vec3 position;            // +0x08
        Vec3 direction;           // +0x14
        ee::Float reach;          // +0x20
        ee::Float unreadFloats[3]; // +0x24
        uint8_t colour[4];        // +0x30
    };
    static_assert(offsetof(LitoChunk, position) == 0x08 && offsetof(LitoChunk, reach) == 0x20);
    static_assert(sizeof(LitoChunk) == 0x34);

    // readLitoChunk: a light from the pool, filled in from the chunk and put
    // on the lights' list, at its front when it is a directional light. 1.
    int32_t readLitoChunk(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<LitoChunk> body;
        readChunk(reader, &*body, sizeof(LitoChunk));
        // The engine unpacks the packed word's fields one by one and stores
        // the word back where it read it. Its flag (bit 26) takes bit 11,
        // one of the group bits, not its own (the GameCube build does the
        // same): the light keeps no flag, so this changes nothing but the
        // word left on the stack, kept as the original stores it.
        const uint32_t packed = body->typeAndGroups;
        body->typeAndGroups = (packed & ~(1u << 26)) | ((packed >> 11) & 1u) << 26;

        const ee::Ptr<LitoRecord> light(poolAlloc(*g_litoChunkPool));
        light->hash = body->hash;
        light->type = body->typeAndGroups & 0x3FFu;
        std::memcpy(light->colour, body->colour, sizeof(light->colour));
        light->position.xyz() = body->position;
        light->direction.xyz() = body->direction;
        light->reach = body->reach;
        light->unreadFloats[0] = body->unreadFloats[0];
        light->unreadFloats[1] = body->unreadFloats[1];
        light->unreadFloats[2] = body->unreadFloats[2];
        light->groups = static_cast<uint16_t>(body->typeAndGroups >> 10);
        if (light->type == kDirectionalLight)
            listPushFront(&*g_litoRecords, &light->link);
        else
            listPush(&*g_litoRecords, &light->link);
        return 1;
    }

    // dropLitoChunk: ZGE2's readLitoChunk: the light's 0x34 bytes read and dropped (ZGE2 keeps no lights from the
    // file, and no pool for them).
    int32_t dropLitoChunk(ee::Ptr<ChunkReader> reader)
    {
        constexpr uint32_t kZge2LitoBytes = 0x34u;
        ee::Local<uint8_t[kZge2LitoBytes]> body;
        readChunk(reader, body, kZge2LitoBytes);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(readLitoChunk, Verified, dm2, dropLitoChunk);

    uint32_t registerLightChunkHandlers()
    {
        *g_litoChunksReady = 1;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps no lights: dropLitoChunk)
            *g_litoChunkPool = ee::Ptr<Pool>(createPool(0x60, 8, 0));
        initList(&*g_litoRecords);
        addChunkHandler(0x54494C5Au, sym::readZlitChunk);
        addChunkHandler(0x4854494Cu, sym::readLithChunk);
        addChunkHandler(0x4F54494Cu, sym::readLitoChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerLightChunkHandlers, Verified);

    uint32_t shutdownLitoChunks()
    {
        if (*g_litoChunksReady != 0)
        {
            freeLitoRecords();
            if constexpr (!game::readable::kIsDm2) // (ZGE2's light chunks keep no pool)
                destroyPool(*g_litoChunkPool);
            *g_litoChunksReady = 0;
            return 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownLitoChunks, Verified);

    int32_t readZpvsChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t context)
    {
        return loadObjectChunks(chunkReader, context);
    }
    READABLE_FUNCTION_WAITS(readZpvsChunk, Verified);

    uint32_t readPvshChunk()
    {
        return 1;
    }
    READABLE_FUNCTION(readPvshChunk, Draft);

    // A PVSO chunk's body before its list of hidden regions. The older body
    // (AI's multiplayer Cannery, and the only one DM2 reads) has no flag
    // word: its list starts where it would be.
    struct PvsoHeader
    {
        uint32_t mesh;        // +0x00
        Vec3 boxLow;          // +0x04
        Vec3 boxHigh;         // +0x10
        uint32_t hiddenCount; // +0x1C
        uint32_t flag;        // +0x20
    };
    static_assert(offsetof(PvsoHeader, hiddenCount) == 0x1C && sizeof(PvsoHeader) == 0x24);
    constexpr uint32_t kOlderPvsoHeaderBytes = offsetof(PvsoHeader, flag); // (the body without the flag word)

    // readPvsoChunk: a view region from the chunk (zge/render/view_regions.h),
    // its list of hidden regions read as mesh hashes. A body with no room
    // for the flag word is the older chunk: the word read as the flag is the
    // list's first hash, and the region's flag is 1. Always 1.
    int32_t readPvsoChunk(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<PvsoHeader> header;
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 reads only the older body, the hidden regions' list straight after the box.
            readChunk(reader, &*header, kOlderPvsoHeaderBytes);
            const ee::Ptr<Region> region = newRegion(header->hiddenCount);
            region->mesh = header->mesh;
            region->box.low.xyz() = header->boxLow;
            region->box.low.w = ee::Float(1.0f);
            region->box.high.xyz() = header->boxHigh;
            region->box.high.w = ee::Float(1.0f);
            ee::Ptr<uint32_t> hidden = region->hidden;
            for (uint32_t index = 0; index < region->hiddenCount; ++index)
            {
                readChunkU32(reader, hidden.get());
                ++hidden;
            }
            return 1;
        }
        const uint32_t bodySize = chunkDataSize(reader);
        readChunk(reader, &*header, sizeof(PvsoHeader));
        const bool hasFlag = bodySize == header->hiddenCount * 4u + sizeof(PvsoHeader);
        const ee::Ptr<Region> region = newRegion(header->hiddenCount);
        ee::Ptr<uint32_t> hidden = region->hidden;
        region->hiddenCount = static_cast<uint16_t>(header->hiddenCount);
        uint32_t left = region->hiddenCount;
        region->mesh = header->mesh;
        region->box.low.xyz() = header->boxLow;
        region->box.low.w = ee::Float(1.0f);
        region->box.high.xyz() = header->boxHigh;
        region->box.high.w = ee::Float(1.0f);
        region->chunkFlag = static_cast<uint16_t>(header->flag);
        if (!hasFlag)
        {
            *hidden = header->flag;
            region->chunkFlag = 1;
            ++hidden;
            left -= 1;
        }
        while (left-- != 0)
        {
            readChunkU32(reader, hidden.get());
            ++hidden;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readPvsoChunk, Verified);

    uint32_t registerPvsChunkHandlers()
    {
        *g_regionsReady = 1;
        const int32_t poolResult = createPool(sizeof(Region), 0x40, 0); // 0x40 regions a block
        *g_regions = poolResult;
        addChunkHandler(0x5356505Au, sym::readZpvsChunk);
        addChunkHandler(0x48535650u, sym::readPvshChunk);
        addChunkHandler(0x4F535650u, sym::readPvsoChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerPvsChunkHandlers, Verified);

    uint32_t registerZpdnChunkHandlers()
    {
        *g_zpdnChunkHandlersRegistered = 1;
        addChunkHandler(0x4E44505Au, sym::readZpdnChunk);
        addChunkHandler(0x4644505Au, sym::readZpdfChunk);
        addChunkHandler(0x5450505Au, sym::readZpptChunk);
        addChunkHandler(0x4D4C505Au, sym::readZplmChunk);
        addChunkHandler(0x5443505Au, sym::readZpctChunk);
        addChunkHandler(0x4C56505Au, sym::readZpvlChunk);
        *g_zpdnChunkPool = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerZpdnChunkHandlers, Draft);

    // G_ffdChunkHandlerReady cleared, 1; shutdownEngineSystems.
    uint32_t shutdownChunkHandlers()
    {
        if (*g_ffdChunkHandlerReady == 0)
            return 1;
        *g_ffdChunkHandlerReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownChunkHandlers, Verified);

    int32_t readWsmcChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t context)
    {
        return loadObjectChunks(chunkReader, context);
    }
    READABLE_FUNCTION_WAITS(readWsmcChunk, Draft);

    namespace
    {
        // A WSMO chunk (BMX XXX's levels have them; AI's have none): a word,
        // a kind, and the kind's numbers. Nothing in AI reads the records
        // made of them, so only their layout is known.
        struct WsmoChunk
        {
            uint32_t word;      // +0x00
            uint32_t kind;      // +0x04 0, 1 or 2
            union
            {
                uint32_t value;        // +0x08 kind 0
                ee::Float numbers[4];  // kind 1: x, y, z; kind 2: four
            };
            ee::Float corners[4][3]; // +0x18 kind 2: four points, x, y and z
        };
        static_assert(sizeof(WsmoChunk) == 0x48);
        struct WsmoRecord
        {
            uint32_t word;      // +0x00 the chunk's
            uint32_t kind;      // +0x04
            uint32_t _08[2];    // +0x08 not set
            union
            {
                uint32_t value; // +0x10 kind 0
                Vec4 numbers;   // kind 1: its point (w 1); kind 2: its four
            };
            Vec4 corners[4];    // +0x20 kind 2: its points (w 1)
        };
        static_assert(sizeof(WsmoRecord) == 0x60);
        constexpr float kWsmoVectorScale = 9.81f;
        enum : uint32_t
        {
            kWsmoValue = 0,
            kWsmoPoint = 1,
            kWsmoCorners = 2,
        };
    }

    // readWsmoChunk: a WSMO chunk read into a record of its own (not
    // cleared: what a kind does not set stays as the heap had it), added
    // to g_emitterDefinitions; another kind than 0 to 2 gets only the word
    // and the kind. 1.
    uint32_t readWsmoChunk(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<WsmoChunk> chunk;
        const ee::Ptr<WsmoRecord> record(heapAllocate(sizeof(WsmoRecord)));
        readChunk(reader, &*chunk, sizeof(WsmoChunk));
        record->word = chunk->word;
        record->kind = chunk->kind;
        switch (chunk->kind)
        {
        case kWsmoValue:
            record->value = chunk->value;
            break;
        case kWsmoPoint:
            record->numbers.x = chunk->numbers[0];
            record->numbers.y = chunk->numbers[1];
            record->numbers.w = ee::Float(1.0f);
            record->numbers.z = chunk->numbers[2];
            if constexpr (game::readable::kIsBmx) // (ZGE4 scales the kind 1 vector by 9.81, g's value)
                record->numbers = record->numbers * ee::Float(kWsmoVectorScale);
            break;
        case kWsmoCorners:
            record->numbers.x = chunk->numbers[0];
            record->numbers.y = chunk->numbers[1];
            record->numbers.z = chunk->numbers[2];
            record->numbers.w = chunk->numbers[3];
            for (uint32_t i = 0; i < 4; ++i)
            {
                record->corners[i].x = chunk->corners[i][0];
                record->corners[i].y = chunk->corners[i][1];
                record->corners[i].w = ee::Float(1.0f);
                record->corners[i].z = chunk->corners[i][2];
            }
            break;
        default:
            break;
        }
        pointerArrayPushBack(*g_emitterDefinitions, record);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readWsmoChunk, Draft);

    void registerWsmChunkHandlers()
    {
        *g_emitterListReady = 1;
        *g_emitterDefinitions = newCursorArray(4);
        addChunkHandler(0x434D5357u, sym::readWsmcChunk);
        addChunkHandler(0x4F4D5357u, sym::readWsmoChunk);
    }
    READABLE_FUNCTION_WAITS(registerWsmChunkHandlers, Verified);
}
