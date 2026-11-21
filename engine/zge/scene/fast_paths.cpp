// ZGE4's fast paths and fast followers (fast_paths.h; BMX XXX only). It sits
// after the engine's followers in the link order and keeps their list code
// (follower.cpp: newFollowerRecord, unlinkFollower, the two links,
// addFollowerToList) for its own records, by time where they go by
// distance.

#include "fast_paths.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/tag_table.h"
#include "zge/assets/tag_table_header.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/fast_paths.cpp.h"

namespace zge
{
    namespace
    {
        // The level's paths and their keys (two blocks, the sizes the ZFPH
        // chunk gives), and how many of each the ZFPD chunks have filled.
        inline ee::Global<uint32_t, sym::bmx::g_fastPathCount> g_fastPathCount;
        inline ee::Global<ee::Ptr<FastPath>, sym::bmx::g_fastPaths> g_fastPaths;
        inline ee::Global<ee::Ptr<FastPathKey>, sym::bmx::g_fastPathKeys> g_fastPathKeys;
        inline ee::Global<uint32_t, sym::bmx::g_fastPathKeysRead> g_fastPathKeysRead;
        inline ee::Global<uint32_t, sym::bmx::g_fastPathsRead> g_fastPathsRead;
        // The followers' pool, while a follower lives.
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_fastFollowerPool> g_fastFollowerPool;
        constexpr uint32_t kFastFollowersABlock = 0x30;
        // The words of a key as the ZFPD chunk has them.
        constexpr uint32_t kKeyWords = sizeof(FastPathKey) / 4;

        // What newBuffer is told about the ZFPT chunk's tag table: one
        // record of the chunk's size (geometry_nodes.cpp's tag chunks').
        constexpr uint32_t kTagWordIndices = 3;
        constexpr uint32_t kTagBytesShift = 4;
        constexpr uint32_t kTagBytesMask = 0x3FFu;
        constexpr uint32_t kTagOneRecord = 1u << 14;
        struct TagTableRequest
        {
            uint32_t bits;
            uint32_t indexCount;
        };

        // A follower's share of the way from its key to the next.
        ee::Float shareToNextKey(ee::Ptr<FastFollower> follower, const FastPathKey &key, const FastPathKey &next)
        {
            return (follower->time - key.time) / (next.time - key.time);
        }
    }

    // readFastPathHeaderChunk (ZFPH): the number of paths and of keys in
    // all, a cleared block of each (none when 0), and none read yet. The
    // keys' block is cleared only as far as the number of paths' worth of
    // keys: the original's slip, kept (each key is read in whole from its
    // ZFPD chunk anyway).
    inline uint32_t readFastPathHeaderChunk(ChunkReader *reader)
    {
        ee::Local<uint32_t> keyCount;
        readChunkU32(reader, &*g_fastPathCount);
        readChunkU32(reader, keyCount);
        if (*g_fastPathCount != 0)
        {
            *g_fastPaths = ee::Ptr<FastPath>(heapAllocate(*g_fastPathCount * sizeof(FastPath)));
            memoryFill(*g_fastPaths, 0, *g_fastPathCount * sizeof(FastPath));
        }
        else
        {
            *g_fastPaths = ee::Ptr<FastPath>();
        }
        if (*keyCount != 0)
        {
            *g_fastPathKeys = ee::Ptr<FastPathKey>(heapAllocate(*keyCount * sizeof(FastPathKey)));
            memoryFill(*g_fastPathKeys, 0, *g_fastPathCount * sizeof(FastPathKey));
        }
        else
        {
            *g_fastPathKeys = ee::Ptr<FastPathKey>();
        }
        *g_fastPathKeysRead = 0;
        *g_fastPathsRead = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readFastPathHeaderChunk);

    namespace
    {
        // The keys' block read word by word: a key's place as words, and the key the words make.
        ee::Ptr<uint32_t> keyWords(ee::Ptr<FastPathKey> key) { return key.cast<uint32_t>(); }
        ee::Ptr<FastPathKey> keyAtWords(ee::Ptr<uint32_t> words) { return words.cast<FastPathKey>(); }
    }

    // readFastPathChunk (ZFPD): the next path: its id, its number of keys
    // and the keys, word by word, into the next places of the keys' block.
    inline uint32_t readFastPathChunk(ChunkReader *reader)
    {
        const ee::Ptr<FastPath> path = *g_fastPaths + *g_fastPathsRead;
        ee::Ptr<uint32_t> word = keyWords(*g_fastPathKeys + *g_fastPathKeysRead);
        readChunkU32(reader, &path->id);
        readChunkU32(reader, &path->keyCount);
        path->keys = keyAtWords(word);
        for (uint32_t key = 0; key < path->keyCount; ++key, word += kKeyWords)
            for (uint32_t i = 0; i < kKeyWords; ++i)
                readChunkWord(reader, word + i);
        *g_fastPathKeysRead = *g_fastPathKeysRead + path->keyCount;
        *g_fastPathsRead = *g_fastPathsRead + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readFastPathChunk);

    // readFastPathTagsChunk (ZFPT): the last path read gets the chunk as its
    // tag table (none when the table is not made).
    inline uint32_t readFastPathTagsChunk(ChunkReader *reader)
    {
        ee::Local<TagTableRequest> request;
        const ee::Ptr<FastPath> path = *g_fastPaths + (*g_fastPathsRead - 1);
        const uint32_t bytes = chunkDataSize(reader);
        request->bits = kTagWordIndices | (bytes & kTagBytesMask) << kTagBytesShift | kTagOneRecord;
        request->indexCount = 0;
        const ee::Ptr<TagTableHeader> table = newBuffer(request);
        if (table)
        {
            readChunk(reader, table + 1, bytes);
            path->tags = table;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readFastPathTagsChunk);

    // initFastPaths: no paths yet, and the ZFPH, ZFPD and ZFPT chunks handed
    // to their readers.
    inline void initFastPaths()
    {
        *g_fastPathCount = 0;
        *g_fastPaths = ee::Ptr<FastPath>();
        *g_fastPathKeys = ee::Ptr<FastPathKey>();
        addChunkHandler(chunkTag("ZFPH"), sym::bmx::readFastPathHeaderChunk);
        addChunkHandler(chunkTag("ZFPD"), sym::bmx::readFastPathChunk);
        addChunkHandler(chunkTag("ZFPT"), sym::bmx::readFastPathTagsChunk);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, initFastPaths);

    // freeFastPaths: each path read lets its tag table go, and both blocks
    // are freed.
    inline void freeFastPaths()
    {
        if (*g_fastPaths)
        {
            for (uint32_t path = 0; path < *g_fastPathsRead; ++path)
                releaseTagTable((*g_fastPaths + path)->tags);
            heapFree(*g_fastPaths);
            *g_fastPaths = ee::Ptr<FastPath>();
        }
        if (*g_fastPathKeys)
        {
            heapFree(*g_fastPathKeys);
            *g_fastPathKeys = ee::Ptr<FastPathKey>();
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeFastPaths);

    // shutDownFastPaths: the paths freed.
    inline void shutDownFastPaths() { freeFastPaths(); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, shutDownFastPaths);

    // findFastPath: the path of the id among the ZFPH chunk's number of
    // them, 0 when none has it.
    inline ee::Ptr<FastPath> findFastPath(uint32_t id)
    {
        for (uint32_t path = 0; path < *g_fastPathCount; ++path)
            if ((*g_fastPaths + path)->id == id)
                return *g_fastPaths + path;
        return ee::Ptr<FastPath>();
    }
    READABLE_FUNCTION_ONLY(bmx, findFastPath);

    inline uint32_t fastPathId(ee::Ptr<FastPath> path) { return path->id; }
    READABLE_FUNCTION_ONLY(bmx, fastPathId);

    // fastPathEndTime: its last key's time.
    inline ee::Float fastPathEndTime(ee::Ptr<FastPath> path) { return path->keys[path->keyCount - 1].time; }
    READABLE_FUNCTION_ONLY(bmx, fastPathEndTime);

    // fastPathTags: the first record of its tag table.
    inline ee::Ptr<TagRecord> fastPathTags(ee::Ptr<FastPath> path) { return firstTagRecord(path->tags); }
    READABLE_FUNCTION_ONLY(bmx, fastPathTags);

    // newFastFollowerRecord: the pool made with the first follower.
    inline uint32_t newFastFollowerRecord()
    {
        if (!*g_fastFollowerPool)
            *g_fastFollowerPool = ee::Ptr<Pool>(createPool(sizeof(FastFollower), kFastFollowersABlock, 0));
        return poolAlloc(*g_fastFollowerPool);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newFastFollowerRecord);

    // freeFastFollowerRecord: the record back, and the pool gone with the
    // last one.
    inline void freeFastFollowerRecord(ee::Ptr<FastFollower> follower)
    {
        poolFree(*g_fastFollowerPool, follower);
        if (poolCount(*g_fastFollowerPool) == 0)
        {
            destroyPool(*g_fastFollowerPool);
            *g_fastFollowerPool = ee::Ptr<Pool>();
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeFastFollowerRecord);

    // unlinkFastFollower: off its path's list.
    inline void unlinkFastFollower(ee::Ptr<FastFollower> follower)
    {
        const ee::Ptr<FastFollower> earlier = follower->earlier;
        const ee::Ptr<FastFollower> later = follower->later;
        if (earlier)
            earlier->later = later;
        else
            follower->path->firstFollower = later;
        if (later)
            later->earlier = earlier;
        else
            follower->path->lastFollower = earlier;
        follower->earlier = ee::Ptr<FastFollower>();
        follower->later = ee::Ptr<FastFollower>();
    }
    READABLE_FUNCTION_ONLY(bmx, unlinkFastFollower);

    // linkFastFollowerBeforeNext: on the list just after `earlier`.
    inline void linkFastFollowerBeforeNext(ee::Ptr<FastFollower> follower, ee::Ptr<FastFollower> earlier)
    {
        const ee::Ptr<FastFollower> later = earlier->later;
        earlier->later = follower;
        if (later)
            later->earlier = follower;
        else
            follower->path->lastFollower = follower;
        follower->later = later;
        follower->earlier = earlier;
    }
    READABLE_FUNCTION_ONLY(bmx, linkFastFollowerBeforeNext);

    // linkFastFollowerAfterPrevious: on the list just before `later`.
    inline void linkFastFollowerAfterPrevious(ee::Ptr<FastFollower> follower, ee::Ptr<FastFollower> later)
    {
        const ee::Ptr<FastFollower> earlier = later->earlier;
        later->earlier = follower;
        if (earlier)
            earlier->later = follower;
        else
            follower->path->firstFollower = follower;
        follower->later = later;
        follower->earlier = earlier;
    }
    READABLE_FUNCTION_ONLY(bmx, linkFastFollowerAfterPrevious);

    // addFastFollowerToList: a new follower on its path's list by its time:
    // first when earlier than the first, else after the first follower from
    // the start that is not earlier than it (or the last): as the engine's
    // addFollowerToList, one place later than its time would put it.
    inline void addFastFollowerToList(ee::Ptr<FastFollower> follower)
    {
        const ee::Ptr<FastPath> path = follower->path;
        ee::Ptr<FastFollower> other = path->firstFollower;
        if (!other)
        {
            path->firstFollower = follower;
            path->lastFollower = follower;
            follower->earlier = ee::Ptr<FastFollower>();
            follower->later = ee::Ptr<FastFollower>();
            return;
        }
        const ee::Float time = follower->time;
        if (time < other->time)
        {
            game::tail(linkFastFollowerAfterPrevious, follower, other);
            return;
        }
        while (other->later && other->time < time)
            other = other->later;
        game::tail(linkFastFollowerBeforeNext, follower, other);
    }
    READABLE_FUNCTION_ONLY(bmx, addFastFollowerToList);

    // deleteFastFollower: off the list, and its record back.
    inline void deleteFastFollower(ee::Ptr<FastFollower> follower)
    {
        unlinkFastFollower(follower);
        game::tail(freeFastFollowerRecord, follower);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, deleteFastFollower);

    // fastFollowerPosition: its position, between its key's and the next's
    // by its share of the time between them; its placed time is its time
    // now.
    inline void fastFollowerPosition(ee::Ptr<FastFollower> follower, ee::Ptr<Vec4> out)
    {
        ee::Local<Vec4> from;
        ee::Local<Vec4> to;
        const ee::Ptr<FastPathKey> key = follower->path->keys + follower->key;
        from->xyz() = key[0].position;
        to->xyz() = key[1].position;
        vec3Lerp(&out->xyz(), &from->xyz(), &to->xyz(), shareToNextKey(follower, key[0], key[1]));
        follower->placedTime = follower->time;
    }
    READABLE_FUNCTION_ONLY(bmx, fastFollowerPosition);

    // fastFollowerOrientation: its orientation, between its key's and the
    // next's the same way.
    inline void fastFollowerOrientation(ee::Ptr<FastFollower> follower, ee::Ptr<Quaternion> out)
    {
        ee::Local<Quaternion> from;
        ee::Local<Quaternion> to;
        const ee::Ptr<FastPathKey> key = follower->path->keys + follower->key;
        *from = key[0].orientation;
        *to = key[1].orientation;
        quaternionNlerp(out, from, to, shareToNextKey(follower, key[0], key[1]));
    }
    READABLE_FUNCTION_ONLY(bmx, fastFollowerOrientation);

    // fastFollowerVelocity: the way from its key's position to the next's,
    // times its rate over the time between them. The w is the difference of
    // the two words its frame held past each copied position (the original
    // takes the whole quadwords), unscaled.
    inline void fastFollowerVelocity(ee::Ptr<FastFollower> follower, ee::Ptr<Vec4> out)
    {
        ee::Local<Vec4> from;
        ee::Local<Vec4> to;
        const ee::Ptr<FastPathKey> key = follower->path->keys + follower->key;
        from->xyz() = key[0].position;
        to->xyz() = key[1].position;
        const Vec4 way = *to - *from;
        *out = way;
        const ee::Float share = fastFollowerRate(follower) / (key[1].time - key[0].time);
        const Vec4 scaled = *out * share;
        *out = Vec4{scaled.x, scaled.y, scaled.z, way.w};
    }
    READABLE_FUNCTION_ONLY(bmx, fastFollowerVelocity);

    inline uint32_t fastFollowerEndMode(ee::Ptr<FastFollower> follower) { return follower->endMode; }
    READABLE_FUNCTION_ONLY(bmx, fastFollowerEndMode);
    inline void setFastFollowerEndMode(ee::Ptr<FastFollower> follower, uint32_t endMode) { follower->endMode = endMode; }
    READABLE_FUNCTION_ONLY(bmx, setFastFollowerEndMode);
    inline ee::Float fastFollowerTime(ee::Ptr<FastFollower> follower) { return follower->time; }
    READABLE_FUNCTION_ONLY(bmx, fastFollowerTime);

    // setFastFollowerTime: a new time, and its key found again: at the
    // time 0 the first, else back from the one it is at while that one is
    // later, then on while the next is not later than the time.
    inline void setFastFollowerTime(ee::Ptr<FastFollower> follower, ee::Float time)
    {
        if (follower->time == time)
            return;
        const ee::Ptr<FastPath> path = follower->path;
        const ee::Ptr<FastPathKey> keys = path->keys;
        // (a double compare, as the source's 0.0)
        if (sce::libgcc::dpcmp(sce::libgcc::fptodp(time), 0) == 0)
            follower->key = 0;
        uint32_t key = follower->key;
        if (key != 0 && !(keys[key].time <= time))
        {
            do
            {
                --key;
                follower->key = key;
            } while (key != 0 && !(keys[key].time <= time));
        }
        while (key < path->keyCount - 1 && !(time <= keys[key + 1].time))
        {
            ++key;
            follower->key = key;
        }
        follower->time = time;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setFastFollowerTime);

    inline ee::Float fastFollowerRate(ee::Ptr<FastFollower> follower) { return follower->rate; }
    READABLE_FUNCTION_ONLY(bmx, fastFollowerRate);
    inline void setFastFollowerRate(ee::Ptr<FastFollower> follower, ee::Float rate) { follower->rate = rate; }
    READABLE_FUNCTION_ONLY(bmx, setFastFollowerRate);
    inline ee::Ptr<FastPath> fastFollowerPath(ee::Ptr<FastFollower> follower) { return follower->path; }
    READABLE_FUNCTION_ONLY(bmx, fastFollowerPath);
    inline void setFastFollowerPath(ee::Ptr<FastFollower> follower, ee::Ptr<FastPath> path) { follower->path = path; }
    READABLE_FUNCTION_ONLY(bmx, setFastFollowerPath);

    // newFastFollower: a cleared follower on the path at the time, on the
    // path's list, with the rate and the end mode; 0 when no record is
    // left. Its time is set before setFastFollowerTime is asked for the same
    // time, which then changes nothing: the follower starts at the first key
    // whatever the time (the original's order, kept).
    inline ee::Ptr<FastFollower> newFastFollower(ee::Ptr<FastPath> path, uint32_t endMode, ee::Float rate, ee::Float time)
    {
        const ee::Ptr<FastFollower> follower(newFastFollowerRecord());
        if (!follower)
            return follower;
        memoryFill(follower, 0, sizeof(FastFollower));
        follower->path = path;
        follower->time = time;
        addFastFollowerToList(follower);
        setFastFollowerTime(follower, follower->time);
        setFastFollowerRate(follower, rate);
        setFastFollowerEndMode(follower, endMode);
        follower->placedTime = follower->time;
        return follower;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newFastFollower);
}
