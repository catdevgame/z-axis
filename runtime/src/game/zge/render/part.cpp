// Parts (zglTerrainRender, partStream .. copyStripGroups; the original's GC and Xbox
// builds call them topologies: "Didn't find topology id:%d"): the pool of
// parts, their streams and the part a stream kind hangs on, a part's
// references and its release, the copy and the share of a stream (what
// clonePart, render/part_clone.cpp, does for each kind), a part's material
// sets, and the readers of one face's or vertex's data.
//
// A stream is held one of three ways (StreamHolder): a stream record
// (ZgeStream: the data after a 16-byte header with the parts' references),
// a PointerArray of records (the bone list, the material sets: the array's
// references in its +0x0C word) or a tag table (face tags, vertex tags, face
// edges: zge/assets/tag_table.h, its references at +0x02).
//
// Safe points: the originals' entry blocks (ee::block), the returns that
// start one (ee::afterCall) and branches back (ee::loopBack) where their
// code reaches them (docs/READABLE.md, "safepoints").

#include "part.h"

#include "zge/math/vec.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/container/array.h"
#include "zge/container/cursor_array.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/render/material.h"
#include "zge/render/terrain.h"
#include "zge/container/pointer_array.h"
#include "zge/render/fixed_point.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/part.cpp.h"

namespace zge
{

    namespace
    {
        // The parts' pool (104-byte records, releasePart when one is freed)
        // and the bytes of strip groups made so far.
        inline ee::Global<int32_t, sym::g_partsReady> g_partsReady;
        inline ee::Global<uint32_t, sym::g_partPool> g_partPool;
        inline ee::Global<uint32_t, sym::g_stripGroupBytes> g_stripGroupBytes;
        // The engine's 1/255 (a colour byte to 0 .. 1).
        inline ee::Global<ee::Float, sym::g_oneOver255> g_oneOver255;

        constexpr uint32_t kCreatePool = sym::createPool; // (record bytes, records a block, free callback) -> pool
        constexpr uint32_t kDestroyPool = sym::destroyPool; // (pool): its records freed (the callback first), then it

        // A part's render state (zge/render: ZgePart +0x50 .. +0x67).

        // Tag tables (zge/assets/tag_table.cpp) and material tables.

        // A box (zge/math/box.h) set to a point, grown to hold one.
        constexpr uint32_t kBoxFromPoint = sym::boxFromPoint; // (box, point)
        constexpr uint32_t kBoxAddPoint = sym::boxExtend;  // (box, point)

        // This file's functions as the originals call them, by address.
        constexpr uint32_t kPartStream = sym::partStream;
        constexpr uint32_t kReleaseStream = sym::releaseStream;
        constexpr uint32_t kReleasePart = sym::releasePart;
        constexpr uint32_t kNewStream = sym::newStream;

        // setPartStream's result: the original returns its jump table's
        // address (no caller reads it).
        constexpr int32_t kSetStreamTable = sym::setStreamTable;
        constexpr int32_t kCopyStreamTable = sym::copyStreamTable; // copyStream's
        // copyStream's and addStreamReference's result for a kind past the
        // last with a stream: the high half of their jump tables' address.
        constexpr int32_t kStreamTablesHigh = ee::high(sym::setStreamTable);

        // The walk of the pool of parts.
        uint32_t firstInPool() { return poolFirst(ee::Ptr<PoolList>(*g_partPool)); }
        uint32_t nextInPool(uint32_t link) { return poolNext(ee::Ptr<PoolLink>(link)); }

        // How a stream kind is held (see the top of the file).
        enum class StreamHolder
        {
            Record,
            Array,
            TagTable,
        };
        constexpr StreamHolder holderOf(StreamKind kind)
        {
            switch (kind)
            {
            case StreamKind::Bones:
            case StreamKind::MaterialSets:
                return StreamHolder::Array;
            case StreamKind::FaceTags:
            case StreamKind::VertexTags:
            case StreamKind::FaceEdges:
                return StreamHolder::TagTable;
            default:
                return StreamHolder::Record;
            }
        }

        // A stream field as the word it holds: one kind's stream is a record,
        // another's an array or a tag table, so the code that handles any
        // kind takes them all as one word.
        template <typename Field>
        uint32_t &fieldWord(Field &field)
        {
            static_assert(sizeof(Field) == sizeof(uint32_t));
            return reinterpret_cast<uint32_t &>(field);
        }

        // The part's field a stream kind hangs on.
        uint32_t &streamField(ZgePart &part, StreamKind kind)
        {
            switch (kind)
            {
            case StreamKind::Positions: return fieldWord(part.positions);
            case StreamKind::Faces: return fieldWord(part.faces);
            case StreamKind::FaceNormals: return fieldWord(part.faceNormals);
            case StreamKind::Normals: return fieldWord(part.normals);
            case StreamKind::Uvs: return fieldWord(part.uvs);
            case StreamKind::Colours: return fieldWord(part.colours);
            case StreamKind::Bones: return part.bones;
            case StreamKind::ShadowEdges: return fieldWord(part.shadowEdges);
            case StreamKind::SkinWeights: return fieldWord(part.skinWeights);
            case StreamKind::Groups: return fieldWord(part.groups);
            case StreamKind::FaceTags: return fieldWord(part.faceTags);
            case StreamKind::VertexTags: return fieldWord(part.vertexTags);
            case StreamKind::FaceEdges: return fieldWord(part.faceEdges);
            default: return fieldWord(part.materialSets);
            }
        }

        // Each kind's case in the originals' jump tables (one entry block
        // each; kinds 7 and 8 have their cases the other way round).
        constexpr uint32_t kPartStreamCase[kStreamKinds] = {
            points::partStream::next9, points::partStream::next11, points::partStream::next13, points::partStream::next15, points::partStream::next17, points::partStream::next19, points::partStream::next21,
            points::partStream::next25, points::partStream::next23, points::partStream::next27, points::partStream::next29, points::partStream::next31, points::partStream::next33, points::partStream::next35};
        constexpr uint32_t kSetPartStreamCase[kStreamKinds] = {
            points::setPartStream::next9, points::setPartStream::next11, points::setPartStream::next13, points::setPartStream::next15, points::setPartStream::next17, points::setPartStream::next19, points::setPartStream::next21,
            points::setPartStream::next25, points::setPartStream::next23, points::setPartStream::next27, points::setPartStream::next29, points::setPartStream::next31, points::setPartStream::next33, points::setPartStream::next35};
        constexpr uint32_t kCopyStreamCase[kStreamKinds] = {
            points::copyStream::next24, points::copyStream::notTaken4, points::copyStream::notTaken6, points::copyStream::notTaken8, points::copyStream::notTaken10, points::copyStream::notTaken12, points::copyStream::notTaken18,
            points::copyStream::notTaken16, points::copyStream::notTaken14, points::copyStream::notTaken23, points::copyStream::notTaken25, points::copyStream::notTaken26, points::copyStream::notTaken27, points::copyStream::notTaken29};

        // releasePart's order: by field, the skin weights (+0x20) before the
        // shadow edges (+0x2C), the groups (+0x24) after both.
        constexpr StreamKind kReleaseOrder[kStreamKinds] = {
            StreamKind::Positions, StreamKind::Faces, StreamKind::FaceNormals, StreamKind::Normals,
            StreamKind::Uvs, StreamKind::Colours, StreamKind::Bones, StreamKind::SkinWeights,
            StreamKind::ShadowEdges, StreamKind::Groups, StreamKind::FaceTags, StreamKind::VertexTags,
            StreamKind::FaceEdges, StreamKind::MaterialSets};

        // A vertex's entry in an indexed stream (normals, colours, UVs): the
        // index stored for it (8- or 16-bit), or the vertex itself when the
        // stream has no indices. The blocks the readers enter on the way: the
        // 8-bit index's, the join after an index, the one without indices
        // (0: none of its own) and the read's.
        uint32_t streamEntry(const ZgeStream &stream, uint32_t vertex)
        {
            uint32_t entry = vertex;
            if (stream.indices)
            {
                if (stream.wideIndices & 1u)
                    entry = stream.indices.cast<uint16_t>()[vertex];
                else
                    entry = stream.indices[vertex];
            }
            return entry;
        }

        // The walk of a PointerArray as the engine's code writes it inline
        // (the cursor moved on, -1 past the end), with the original's blocks:
        // the one past the end and the one after.
        uint32_t walkOn(PointerArray &array, uint32_t pastEndBlock, uint32_t nextBlock)
        {
            const int32_t cursor = array.cursor;
            if (cursor >= 0)
            {
                array.cursor = cursor + 1;
                if (cursor + 1 < static_cast<int32_t>(array.count))
                {
                    const uint32_t item = array.items[cursor + 1];
                    return item;
                }
            }
            array.cursor = -1;
            return 0;
        }
        // Its start: the cursor on the first item (-1 when empty), with the
        // empty array's block and the one after.
        uint32_t walkStart(PointerArray &array, uint32_t emptyBlock, uint32_t nextBlock)
        {
            uint32_t item = 0;
            if (array.count)
            {
                array.cursor = 0;
                item = array.items[0];
            }
            else
                array.cursor = -1;
            return item;
        }
    }

    // ---- streams ---------------------------------------------------------

    // partStream: the part's stream of a kind (what its field holds), 0
    // for a kind past the last.
    uint32_t partStream(ee::Ptr<ZgePart> part, StreamKind kind)
    {
        if (static_cast<uint32_t>(kind) < kStreamKinds)
            return streamField(*part, kind);
        return 0;
    }
    READABLE_FUNCTION(partStream, Verified);

    // setPartStream: the part's stream of a kind set (no reference taken).
    int32_t setPartStream(ee::Ptr<ZgePart> part, StreamKind kind, uint32_t stream)
    {
        if (static_cast<uint32_t>(kind) < kStreamKinds)
        {
            streamField(*part, kind) = stream;
            return kSetStreamTable;
        }
        return 0;
    }
    READABLE_FUNCTION(setPartStream, Verified);

    // addStreamReference: one more part using the part's stream of a kind; the
    // references it has then (a tag table: the table). Without the stream:
    // whether the kind is one.
    int32_t addStreamReference(ee::Ptr<ZgePart> part, StreamKind kind)
    {
        const uint32_t stream = partStream(part, kind);
        const bool known = static_cast<uint32_t>(kind) < kStreamKinds;
        if (!stream || !known)
            return stream ? kStreamTablesHigh : known;
        int32_t result = 0;
        switch (holderOf(kind))
        {
        case StreamHolder::Record:
        {
            ZgeStream &record = *ee::Ptr<ZgeStream>(stream);
            result = record.refCount + 1;
            record.refCount = static_cast<uint16_t>(result);
            break;
        }
        case StreamHolder::Array:
            result = addArrayReference(ee::Ptr<PointerArray>(stream));
            break;
        case StreamHolder::TagTable:
            addTagTableReference(ee::Ptr<TagTableHeader>(stream));
            result = static_cast<int32_t>(stream); // (what it returns: the table)
            break;
        }
        return result;
    }
    READABLE_FUNCTION(addStreamReference, Verified);

    // releaseStream: one part fewer using the part's stream of a kind; with
    // the last, the stream freed (an array's records too: bones freed,
    // material tables released) and the part's field cleared.
    void releaseStream(ee::Ptr<ZgePart> part, StreamKind kind)
    {
        namespace here = points::releaseStream;
        bool freed = false;
        const uint32_t stream = partStream(part, kind);
        if (!stream)
            return;
        if (static_cast<uint32_t>(kind) < kStreamKinds)
        {
            switch (holderOf(kind))
            {
            case StreamHolder::Record:
            {
                ZgeStream &record = *ee::Ptr<ZgeStream>(stream);
                record.refCount = static_cast<uint16_t>(record.refCount - 1);
                if (record.refCount == 0)
                {
                    freed = true;
                    heapFree(stream);
                }
                break;
            }
            case StreamHolder::Array:
            {
                if (releaseArrayReference(ee::Ptr<PointerArray>(stream)) != 0)
                    break;
                PointerArray &records = *ee::Ptr<PointerArray>(stream);
                // ZGE2 walks the records with arrayFirst and arrayNext; ZGE3 inlines the walk.
                uint32_t record;
                if constexpr (game::readable::kIsDm2)
                    record = arrayFirst(ee::Ptr<PointerArray>(stream));
                else
                    record = walkStart(records, here::taken, here::taken2);
                if (record)
                {
                    for (;;)
                    {
                        if (kind == StreamKind::Bones)
                            heapFree(record);
                        if (kind == StreamKind::MaterialSets)
                            releaseMaterialTable(ee::Ptr<MaterialTable>(record));
                        if constexpr (game::readable::kIsDm2)
                            record = arrayNext(ee::Ptr<PointerArray>(stream));
                        else
                            record = walkOn(records, here::join, here::taken4);
                        if (!record)
                            break;
                    }
                }
                freed = true;
                freePointerArray(ee::Ptr<PointerArray>(stream));
                break;
            }
            case StreamHolder::TagTable:
                freed = releaseTagTable(ee::Ptr<TagTableHeader>(stream)) == 0;
                break;
            }
        }
        if (freed)
            setPartStream(part, kind, 0);
    }
    READABLE_FUNCTION_WAITS(releaseStream, Verified);

    // shareStream (clonePart's for a stream it does not copy): the source
    // part's stream of a kind shared by the clone, with a reference (a tail
    // call; its result).
    int32_t shareStream(ee::Ptr<ZgePart> clone, ee::Ptr<ZgePart> source, StreamKind kind)
    {
        const uint32_t stream = partStream(source, kind);
        setPartStream(clone, kind, stream);
        return game::tail(addStreamReference, clone, kind);
    }
    READABLE_FUNCTION(shareStream, Verified);

    // ---- the parts' pool -------------------------------------------------

    // initParts: the parts' pool made (256 a block; ZGE4 512). 1.
    uint32_t initParts()
    {
        *g_partsReady = 1;
        *g_partPool = createPool(sizeof(ZgePart), game::readable::perGame(0x100u, 0x100u, 0x200u), kReleasePart);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initParts, Verified);

    // shutDownParts: the parts' pool destroyed, if it was made. 1.
    uint32_t shutDownParts()
    {
        if (*g_partsReady == 0)
            return 1;
        destroyPool(ee::Ptr<Pool>(*g_partPool));
        *g_partPool = 0;
        *g_partsReady = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownParts, Verified);

    // newPart: a new part, cleared, with one reference and its
    // streams still to build (kPartNew); 0 if the pool gave none.
    ee::Ptr<ZgePart> newPart()
    {
        const ee::Ptr<ZgePart> part(poolAlloc(ee::Ptr<Pool>(*g_partPool)));
        if (part)
        {
            memoryFill(part, 0, sizeof(ZgePart));
            part->header = 0;
            part->refCount = 1;
            part->flags = kPartNew;
            if constexpr (!game::readable::kIsDm2) // (ZGE2 has no render state to clear)
                clearRenderState(part);
        }
        return part;
    }
    READABLE_FUNCTION_WAITS(newPart, Verified);

    // releasePart (also the pool's free callback): one reference less;
    // with the last (or none left), its packets and every stream released,
    // the parts chained after it released the same way, and the record
    // handed back. 1.

    uint32_t releasePart(ee::Ptr<ZgePart> part)
    {
        part->refCount = static_cast<uint16_t>(part->refCount - 1);
        if (static_cast<int16_t>(part->refCount) > 0)
            return 1;
        releasePartPackets(part);
        for (const StreamKind kind : kReleaseOrder)
            releaseStream(part, kind);
        if (const ee::Ptr<ZgePart> next = part->next; next)
            releasePart(next);
        poolFree(ee::Ptr<Pool>(*g_partPool), part);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releasePart, Verified);

    // addPartReference: one more reference to the part; the count.
    int32_t addPartReference(ee::Ptr<ZgePart> part)
    {
        const int32_t references = part->refCount + 1;
        part->refCount = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(addPartReference, Verified);

    // dropPartReference: one reference less, without the release (TGPH gives a
    // rigid part's first reference up: what holds it takes its own).
    int32_t dropPartReference(ee::Ptr<ZgePart> part)
    {
        const int32_t references = part->refCount - 1;
        part->refCount = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(dropPartReference, Verified);

    // findPartById: the loaded part with this id (ZgePart::id, TGPH +0x10;
    // what TGCI names its collision part by), or 0. The GC and Xbox builds
    // print "Didn't find topology id:%d" for none.
    ee::Ptr<ZgePart> findPartById(uint32_t id)
    {
        uint32_t link = firstInPool();
        if (link)
        {
            for (;;)
            {
                const ee::Ptr<ZgePart> part = poolRecordAs<ZgePart>(link);
                if (part->id == id)
                    return part;
                link = nextInPool(link);
                if (!link)
                    break;
            }
        }
        return ee::Ptr<ZgePart>();
    }
    READABLE_FUNCTION(findPartById, Verified);

    // buildPartChainPackets: the packets of the part and of the parts chained after
    // it built where they are new. (The original's result is the last
    // build's, and with no part whatever its caller last had: no caller
    // reads it.)
    void buildPartChainPackets(ee::Ptr<ZgePart> part)
    {
        if (part)
        {
            for (;;)
            {
                buildChainPartPackets(part);
                part = part->next;
                if (!part)
                    break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(buildPartChainPackets, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void boxFromPoint(uint32_t box, ee::Ptr<ee::Float> point) { ee::game(kBoxFromPoint, box, point); }
        void boxExtend(uint32_t box, ee::Ptr<ee::Float> point) { ee::game(kBoxAddPoint, box, point); }
    }

    // partBounds: the box set to the part's first position and grown to
    // hold the others.
    void partBounds(ee::Ptr<ZgePart> part, uint32_t box)
    {
        ee::Local<ee::Float[3]> point;
        ee::Ptr<Float4> position = streamRecords<Float4>(part->positions);
        const auto takePoint = [&]() {
            point[0] = position->x;
            point[1] = position->y;
            point[2] = position->z;
            ++position;
        };
        takePoint();
        boxFromPoint(box, point);
        for (int32_t left = part->vertexCount - 1; left != 0; --left)
        {
            takePoint();
            boxExtend(box, point);
        }
    }
    READABLE_FUNCTION(partBounds, Verified);

    namespace
    {
        // A stream record's engine object type (ZgeStream::type): ZGE2's 6, ZGE3's 7.
        constexpr uint8_t kStreamObjectType = game::readable::kIsDm2 ? 6 : 7;
    }

    // newStream: a stream of a kind for the part. New (copyOf 0): a
    // record of `bytes` (twice that for the positions and face normals of
    // a skinned part with bones: a copy per pose) set on the part, its data
    // returned; a bone: a record of `bytes` added to the part's bone list.
    // A copy (copyOf non-zero): a copy of the part's stream record, not set
    // on any part, the record returned (a bone: 0). The tag tables' kinds
    // and those past them get an empty record.
    uint32_t newStream(ee::Ptr<ZgePart> part, StreamKind kind, uint32_t bytes, uint32_t copyOf)
    {
        uint32_t size = 0;
        // A new record's size, or the copied one's.
        const auto recordSize = [&](ee::Ptr<uint8_t> field) {
            return copyOf ? ZgeStream::of(field).bytes : bytes + sizeof(ZgeStream);
        };
        // Positions and face normals: one copy per pose for a skinned part.
        const auto perPoseSize = [&]() {
            size = bytes + sizeof(ZgeStream);
            if ((part->flags & kPartSkinned) && part->boneCount != 0)
                size += bytes;
        };
        if (static_cast<uint32_t>(kind) < 13u)
        {
            switch (kind)
            {
            case StreamKind::Positions:
                if (copyOf)
                    size = ZgeStream::of(part->positions).bytes;
                else
                    perPoseSize();
                break;
            case StreamKind::Faces:
                size = recordSize(part->faces);
                break;
            case StreamKind::FaceNormals:
                if (copyOf)
                    size = ZgeStream::of(part->faceNormals).bytes;
                else
                    perPoseSize();
                break;
            case StreamKind::Normals:
                size = recordSize(part->normals);
                break;
            case StreamKind::Uvs:
                size = recordSize(part->uvs);
                break;
            case StreamKind::Colours:
                size = recordSize(part->colours);
                break;
            case StreamKind::Groups:
                size = recordSize(part->groups);
                if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps no count of the strip groups' bytes)
                    *g_stripGroupBytes += size;
                break;
            case StreamKind::SkinWeights:
                size = recordSize(part->skinWeights);
                break;
            case StreamKind::ShadowEdges:
                size = recordSize(part->shadowEdges);
                break;
            case StreamKind::Bones:
            {
                if (copyOf)
                    return 0;
                if (part->bones == 0)
                    part->bones = ee::Address(newCursorArray(part->boneCount));
                const uint32_t bone = heapAllocate(bytes);
                if constexpr (game::readable::kIsBmx) // (ZGE4 appends the bone at the end)
                    pointerArrayPushBack(ee::Ptr<PointerArray>(part->bones), bone);
                else
                    pointerArrayAdd(ee::Ptr<PointerArray>(part->bones), bone);
                return bone;
            }
            default:
                break; // the tag tables' kinds: an empty record
            }
        }
        const uint32_t made = heapAllocate(size);
        ZgeStream &record = *ee::Ptr<ZgeStream>(made);
        record.bytes = size;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 leaves the indices as the heap gave them)
            record.indices = ee::Ptr<uint8_t>();
        if (copyOf)
            memoryCopy(made, partStream(part, kind), size);
        record.refCount = 1;
        record.type = kStreamObjectType;
        if (copyOf)
            return made;
        setPartStream(part, kind, made);
        return made + sizeof(ZgeStream);
    }
    READABLE_FUNCTION_WAITS(newStream, Verified);

    // findPartBoneRecord (ZGE4 only; mergeSceneMeshes'): the part's bone record (the Bones stream's list) of that pose
    // number, 0 if it has none.
    inline ee::Ptr<BoneRecord> findPartBoneRecord(ee::Ptr<ZgePart> part, uint32_t poseNumber)
    {
        const ee::Ptr<PointerArray> bones(partStream(part, StreamKind::Bones));
        if (!bones)
            return {};
        ee::Ptr<BoneRecord> record(walkFirst(*bones));
        while (record && record->poseIndex != poseNumber)
        {
            uint32_t next;
            walkNext(*bones, next);
            record = ee::Ptr<BoneRecord>(next);
        }
        return record;
    }
    READABLE_FUNCTION_ONLY(bmx, findPartBoneRecord);

    // copyPartBoneRecord (ZGE4 only; mergeSceneBone's): the bone record of that pose number from one part added to
    // another's bones, with its offset, numbered after the bones `to` has (nothing when `from` has no such record).
    inline void copyPartBoneRecord(ee::Ptr<ZgePart> to, ee::Ptr<ZgePart> from, uint32_t poseNumber)
    {
        const ee::Ptr<BoneRecord> source = findPartBoneRecord(from, poseNumber);
        if (!source)
            return;
        constexpr uint32_t kRecordBytes = 0x20; // ZGE4's bone record: its offset, size and pose number, padded
        const ee::Ptr<BoneRecord> record(newStream(to, StreamKind::Bones, kRecordBytes, 0));
        record->bytes = kRecordBytes;
        record->poseIndex = static_cast<uint16_t>(pointerArrayCount(ee::Ptr<PointerArray>(partStream(to, StreamKind::Bones))));
        record->offset = source->offset;
    }
    READABLE_FUNCTION_ONLY(bmx, copyPartBoneRecord);

    namespace
    {
        ee::Ptr<uint8_t> tableBytes(ee::Ptr<TagTableHeader> table) { return table.cast<uint8_t>(); }
    }

    // attachBuffer: a tag table set as the part's face tags, vertex tags or
    // face edges, with a reference; the references it has then. Another
    // kind: nothing set, and the result the kind the original compared with
    // last.
    int32_t attachBuffer(ee::Ptr<ZgePart> part, StreamKind kind, ee::Ptr<TagTableHeader> table)
    {
        switch (kind)
        {
        case StreamKind::VertexTags:
            part->vertexTags = tableBytes(table);
            break;
        case StreamKind::FaceTags:
            part->faceTags = tableBytes(table);
            break;
        case StreamKind::FaceEdges:
            part->faceEdges = tableBytes(table);
            break;
        default:
            if (static_cast<uint32_t>(kind) < static_cast<uint32_t>(StreamKind::FaceEdges))
                return static_cast<int32_t>(StreamKind::FaceTags);
            return static_cast<int32_t>(StreamKind::FaceEdges);
        }
        const int32_t references = table->references + 1;
        table->references = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(attachBuffer, Verified);

    // copyStream (clonePart's for a stream it copies): a copy of the
    // source part's stream of a kind set on the clone (a record copied, a
    // tag table cloned, the bone list's records and the material tables
    // copied into new arrays). Without the stream, whether the kind is one;
    // else setPartStream's result.
    int32_t copyStream(ee::Ptr<ZgePart> clone, ee::Ptr<ZgePart> source, StreamKind kind, uint32_t copyFlags)
    {
        namespace here = points::copyStream;
        const uint32_t stream = partStream(source, kind);
        const bool known = static_cast<uint32_t>(kind) < kStreamKinds;
        if (!stream || !known)
            return stream ? kStreamTablesHigh : known;
        ee::Address copy = 0u; // the copied stream, any kind's
        int32_t result = 0; // when there is nothing to set: the field read (0)
        switch (kind)
        {
        case StreamKind::Bones:
        {
            // A new list as large as the source's, a copy of each bone.
            PointerArray &bones = *ee::Ptr<PointerArray>(source->bones);
            const uint32_t capacity = cursorArrayCapacity(ee::Ptr<PointerArray>(source->bones));
            copy = newCursorArray(capacity);
            uint32_t bone = walkStart(bones, here::taken, here::taken2);
            result = -1;
            if (bone)
            {
                for (;;)
                {
                    const uint32_t bytes = ee::Ptr<BoneRecord>(bone)->bytes;
                    const uint32_t made = heapAllocate(bytes);
                    memoryCopy(made, bone, bytes);
                    pointerArrayAdd(ee::Ptr<PointerArray>(copy), made);
                    PointerArray &walked = *ee::Ptr<PointerArray>(source->bones);
                    bone = walkOn(walked, here::join, here::taken3);
                    if (!bone)
                        break;
                }
            }
            break;
        }
        case StreamKind::FaceTags:
        case StreamKind::VertexTags:
        case StreamKind::FaceEdges:
        {
            const uint32_t table = streamField(*source, kind);
            result = kCopyStreamTable; // for no table: the jump table's address
            if (table)
                result = static_cast<int32_t>(copy = copyTagTable(ee::Ptr<TagTableHeader>(table)));
            break;
        }
        case StreamKind::MaterialSets:
        {
            // A new array as large as the source's, a copy of each table.
            PointerArray &sets = *source->materialSets;
            uint32_t table = walkStart(sets, here::taken4, here::taken5);
            const uint32_t capacity = cursorArrayCapacity(source->materialSets);
            copy = newPointerArray(capacity);
            result = static_cast<int32_t>(copy);
            if (table)
            {
                for (;;)
                {
                    const ee::Ptr<MaterialTable> made = copyMaterialTable(ee::Ptr<MaterialTable>(table), copyFlags);
                    pointerArrayAdd(ee::Ptr<PointerArray>(copy), made);
                    PointerArray &walked = *source->materialSets;
                    table = walkOn(walked, here::join4, here::taken6);
                    if (!table)
                        break;
                }
            }
            break;
        }
        default:
            // A stream record: a copy of it (newStream's).
            if (streamField(*source, kind) == 0)
                break;
            result = static_cast<int32_t>(copy = newStream(source, kind, 0, 1));
            break;
        }
        if (copy)
            result = setPartStream(clone, kind, copy);
        return result;
    }

    // copyStreamForClone: ZGE2's copyStream: of the source part's streams
    // only those a clone changes are copied and set on the clone: positions,
    // colours, shadow edges and skin weights as record copies (newStream),
    // the bone list's records and the material tables into new arrays
    // (walked with arrayFirst and arrayNext), the tag tables cloned. The
    // other kinds the clone shares. (Its caller reads no result.)
    void copyStreamForClone(ee::Ptr<ZgePart> clone, ee::Ptr<ZgePart> source, StreamKind kind, uint32_t copyFlags)
    {
        ee::Address copy = 0u;
        switch (kind)
        {
        case StreamKind::Positions:
        case StreamKind::Colours:
        case StreamKind::ShadowEdges:
        case StreamKind::SkinWeights:
            if (streamField(*source, kind) != 0)
                copy = newStream(source, kind, 0, 1);
            break;
        case StreamKind::Bones:
        {
            // A new list as large as the source's, a copy of each bone.
            copy = newPointerArray(cursorArrayCapacity(ee::Ptr<PointerArray>(source->bones)));
            const ee::Ptr<PointerArray> bones(source->bones);
            for (uint32_t bone = arrayFirst(bones); bone; bone = arrayNext(bones))
            {
                const uint32_t bytes = ee::Ptr<BoneRecord>(bone)->bytes;
                const uint32_t made = heapAllocate(bytes);
                memoryCopy(made, bone, bytes);
                pointerArrayAdd(ee::Ptr<PointerArray>(copy), made);
            }
            break;
        }
        case StreamKind::FaceTags:
        case StreamKind::VertexTags:
        case StreamKind::FaceEdges:
            if (const uint32_t table = streamField(*source, kind))
                copy = copyTagTable(ee::Ptr<TagTableHeader>(table));
            break;
        case StreamKind::MaterialSets:
        {
            // A new array as large as the source's, a copy of each table.
            uint32_t table = arrayFirst(source->materialSets);
            copy = newPointerArray(cursorArrayCapacity(source->materialSets));
            for (; table; table = arrayNext(source->materialSets))
                pointerArrayAdd(ee::Ptr<PointerArray>(copy), copyMaterialTable(ee::Ptr<MaterialTable>(table), copyFlags));
            break;
        }
        default:
            return;
        }
        if (copy)
            setPartStream(clone, kind, copy);
    }
    READABLE_FUNCTION_WAITS_OWN(copyStream, Verified, dm2, copyStreamForClone);

    // partIgnoresStream: whether the part needs no stream of this kind: normals,
    // when it is not lit by them and its render type draws none.
    uint32_t partIgnoresStream(ee::Ptr<ZgePart> part, StreamKind kind)
    {
        if (kind != StreamKind::FaceNormals)
        {
            if (kind != StreamKind::Normals || (part->flags & kPartLitByNormals))
                return 0;
            if (renderIgnoresStream(part, StreamKind::Normals) != 0)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(partIgnoresStream, Verified);

    // ---- material sets ---------------------------------------------------

    // meshMaterials (kMeshMaterials): the part's material table of a set, 0
    // if it has none.
    ee::Ptr<MaterialTable> meshMaterials(ee::Ptr<ZgePart> part, uint32_t set)
    {
        if (part->materialSets)
            return ee::Ptr<MaterialTable>(pointerArrayAt(part->materialSets, set));
        return ee::Ptr<MaterialTable>();
    }
    READABLE_FUNCTION(meshMaterials, Verified);

    // attachMaterialTable: a material table added as the part's next material set
    // (the sets made for two); the first sets the part's material count.
    // The sets it has then (or, for the first, the count). ZGE2 only adds the
    // table: no count, no result.
    READABLE_RESULT_EXCEPT(dm2, int32_t) attachMaterialTable(ee::Ptr<ZgePart> part, ee::Ptr<MaterialTable> table)
    {
        if (!part->materialSets)
            part->materialSets = newPointerArray(2);
        if constexpr (game::readable::kIsDm2)
        {
            game::tail(pointerArrayAdd, part->materialSets, table);
            return READABLE_RETURN_EXCEPT(dm2, 0);
        }
        pointerArrayAdd(part->materialSets, table);
        int32_t result = pointerArrayCount(part->materialSets);
        if (result == 1)
        {
            result = materialCount(table);
            part->materialCount = static_cast<uint8_t>(result);
        }
        return READABLE_RETURN_EXCEPT(dm2, result);
    }
    READABLE_FUNCTION_WAITS(attachMaterialTable, Verified);

    // materialSetCount: how many material sets the part has.
    int32_t materialSetCount(ee::Ptr<ZgePart> part)
    {
        return pointerArrayCount(part->materialSets);
    }
    READABLE_FUNCTION(materialSetCount, Verified);

    // ---- skinned parts ---------------------------------------------------

    // attachSkinnedPart: a skinned part chained after its owner; when the owner
    // has bones and the part is skinned, the part shares the owner's bone
    // list (a tail call).
    int32_t attachSkinnedPart(ee::Ptr<ZgePart> owner, ee::Ptr<ZgePart> skinned)
    {
        skinned->next = owner->next;
        owner->next = skinned;
        if (owner->boneCount == 0)
            return 0;
        if (!(skinned->flags & kPartSkinned))
            return 0;
        const uint32_t bones = partStream(owner, StreamKind::Bones);
        addStreamReference(owner, StreamKind::Bones);
        return game::tail(setPartStream, skinned, StreamKind::Bones, bones);
    }
    READABLE_FUNCTION(attachSkinnedPart, Verified);

    // shareBones: the part's bone list given up for the source's, shared
    // (a tail call).
    int32_t shareBones(ee::Ptr<ZgePart> source, ee::Ptr<ZgePart> part)
    {
        releaseStream(part, StreamKind::Bones);
        addStreamReference(source, StreamKind::Bones);
        const uint32_t bones = partStream(source, StreamKind::Bones);
        return game::tail(setPartStream, part, StreamKind::Bones, bones);
    }
    READABLE_FUNCTION_WAITS(shareBones, Verified);

    // ---- one face or vertex ----------------------------------------------

    // faceMaterial (kFaceMaterial): the face's material index (its flags'
    // low byte, kFaceMaterial).
    uint32_t faceMaterial(ee::Ptr<ZgePart> part, uint32_t face)
    {
        const Face &record = streamRecords<Face>(part->faces)[face];
        return record.flags & 0xFFu; // the low byte
    }
    READABLE_FUNCTION(faceMaterial, Verified);

    // faceCornerIndex: where a face's corner is in the corners of every face
    // (three a face).
    int32_t faceCornerIndex(ee::Ptr<ZgePart> /*part*/, uint32_t face, uint32_t corner)
    {
        return static_cast<int32_t>(face * 3u + corner);
    }
    READABLE_FUNCTION(faceCornerIndex, Verified);

    // faceTriangle (nothing calls it directly): a face as a triangle:
    // its three corners' positions (the positions stream's quadwords, the
    // last corner first), its normal (the face normals stream, w 0) and two
    // bits of its flags into out's flags (the others kept). The result is
    // what the original computes last: the collides bit, shifted.
    uint64_t faceTriangle(ee::Ptr<ZgePart> part, uint32_t face, ee::Ptr<FaceTriangle> out)
    {
        const Face &record = streamRecords<Face>(part->faces)[face];
        const ee::Ptr<Float4> positions = streamRecords<Float4>(part->positions);
        for (int32_t corner = 2; corner >= 0; --corner)
            aligned(ee::Ptr<Float4>(&out->corners[corner])) = aligned(positions + record.vertex[corner]);
        const ZgeStream &normals = ZgeStream::of(part->faceNormals);
        const uint32_t entry = streamEntry(normals, face);
        const Vec3 &normal = streamRecords<Vec3>(part->faceNormals)[entry];
        out->normal.x = normal.x;
        out->normal.y = normal.y;
        out->normal.z = normal.z;
        out->normal.w = ee::Float(0.0f); // (the word 0)
        const uint64_t twoSided = (record.flags & kFaceTwoSided) ? kTriangleTwoSided : 0u;
        const uint64_t collides = (record.flags & kFaceCollides) ? kTriangleCollides : 0u;
        out->flags = (out->flags & ~kTriangleTwoSided) | twoSided;
        out->flags = (out->flags & ~kTriangleCollides) | collides;
        return collides;
    }
    READABLE_FUNCTION(faceTriangle, Verified);

    // vertexNormal: a vertex's normal (x, y, z, w = 1) into out; out.
    ee::Ptr<Float4> vertexNormal(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<Float4> out)
    {
        const ZgeStream &normals = ZgeStream::of(part->normals);
        const uint32_t entry = streamEntry(normals, vertex);
        const Vec3 &normal = streamRecords<Vec3>(part->normals)[entry];
        out->x = normal.x;
        out->y = normal.y;
        out->z = normal.z;
        out->w = ee::Float(1.0f);
        return out;
    }
    READABLE_FUNCTION(vertexNormal, Verified);

    // setNormalEntry: normal table entry `entry` set to x, y, z (the table
    // itself, whatever the indices); the entry's address less the header.
    ee::Ptr<uint8_t> setNormalEntry(ee::Ptr<ZgePart> part, uint32_t entry, ee::Ptr<ee::Float> normal)
    {
        Vec3 &to = streamRecords<Vec3>(part->normals)[entry];
        to.x = normal[0];
        to.y = normal[1];
        to.z = normal[2];
        return part->normals + entry * 12;
    }
    READABLE_FUNCTION(setNormalEntry, Verified);

    // vertexColour: a vertex's colour (RGBA8, as it is) into out; out.
    ee::Ptr<VertexColour> vertexColour(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<VertexColour> out)
    {
        const ZgeStream &colours = ZgeStream::of(part->colours);
        const uint32_t entry = streamEntry(colours, vertex);
        *out = streamRecords<VertexColour>(part->colours)[entry];
        return out;
    }
    READABLE_FUNCTION(vertexColour, Verified);

    // vertexColourFloats: a vertex's colour as four floats 0 .. 1 into out; out.
    ee::Ptr<Float4> vertexColourFloats(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<Float4> out)
    {
        const ZgeStream &colours = ZgeStream::of(part->colours);
        const uint32_t entry = streamEntry(colours, vertex);
        const uint32_t rgba = streamRecords<uint32_t>(part->colours)[entry];
        const ee::Float scale = *g_oneOver255;
        out->x = ee::toFloat(static_cast<int32_t>((rgba) & 0xFFu)) * scale;
        out->y = ee::toFloat(static_cast<int32_t>((rgba >> 8) & 0xFFu)) * scale;
        out->z = ee::toFloat(static_cast<int32_t>((rgba >> 16) & 0xFFu)) * scale;
        out->w = ee::toFloat(static_cast<int32_t>((rgba >> 24) & 0xFFu)) * scale;
        return out;
    }
    READABLE_FUNCTION(vertexColourFloats, Verified);

    // vertexUv: a vertex's texture coordinates (s16, 4096 = 1.0) as
    // floats into out (z = w = 1); out.
    ee::Ptr<Float4> vertexUv(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<Float4> out)
    {
        const ZgeStream &uvs = ZgeStream::of(part->uvs);
        const uint32_t entry = streamEntry(uvs, vertex);
        const ee::Ptr<int16_t> uv = streamRecords<int16_t>(part->uvs) + entry * 2;
        out->x = uvFromFixed(uv[0]);
        out->y = uvFromFixed(uv[1]);
        out->w = ee::Float(1.0f);
        out->z = ee::Float(1.0f);
        return out;
    }

    // partUv: ZGE2's vertexUv: vertex `index`'s texture coordinates, the UV stream's two floats (no index), into
    // out (z 0, w 1); out.
    ee::Ptr<Vec4> partUv(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Vec4> out)
    {
        const ee::Ptr<ee::Float> uv = streamRecords<ee::Float>(part->uvs) + (index << 1);
        out->x = uv[0];
        out->w = ee::Float(1.0f);
        out->y = uv[1];
        out->z = ee::Float(0.0f);
        return out;
    }
    READABLE_FUNCTION_OWN(vertexUv, Verified, dm2, partUv);

    // ---- every part ------------------------------------------------------

    // visitParts: every loaded part visited, nothing done with it (the
    // debug report the PS2 build compiled out); the walk's last result.
    int32_t visitParts()
    {
        uint32_t link = firstInPool();
        if (link)
        {
            for (;;)
            {
                poolRecord(link);
                link = nextInPool(link);
                if (!link)
                    break;
            }
        }
        return static_cast<int32_t>(link);
    }
    READABLE_FUNCTION(visitParts, Verified);

    namespace
    {
        int32_t visitPart(uint32_t visit, uint32_t part, uint32_t data) { return ee::call<int32_t>(visit, part, data); }
    }

    // forEachPart: visit(part, data) for every loaded part until one
    // returns 0.
    void forEachPart(uint32_t visit, uint32_t data)
    {
        for (uint32_t link = firstInPool(); link != 0;)
        {
            const uint32_t part = poolRecord(link);
            link = nextInPool(link);
            if (visitPart(visit, part, data) == 0)
                break;
        }
    }
    READABLE_FUNCTION(forEachPart, Verified);

    // ---- copies of a part's strips and faces (a merge's) ----

    // copyStripGroups: `count` strip groups copied from `from` to `to`, each
    // group's submesh (its first word's high byte) moved by submeshOffset
    // and each index by indexOffset (the whole halfword, its strip bits
    // too). Where the copies end.
    ee::Ptr<StripGroup> copyStripGroups(ee::Ptr<StripGroup> from, ee::Ptr<StripGroup> to, uint32_t count, uint32_t submeshOffset,
                                        uint32_t indexOffset)
    {
        for (uint32_t copied = 0; copied < count; ++copied)
        {
            const ee::Ptr<StripGroup> next = nextStripGroup(from);
            memoryCopy(to, from, ee::Address(next) - ee::Address(from));
            const ee::Ptr<StripGroup> group = to;
            group->submeshMode = static_cast<uint16_t>(group->submeshMode + (submeshOffset << 8));
            const ee::Ptr<uint16_t> indices(group->indices);
            for (uint32_t index = 0; index < group->count; ++index)
                indices[index] = static_cast<uint16_t>(indices[index] + indexOffset);
            from = next;
            to = nextStripGroup(to);
        }
        return to;
    }
    READABLE_FUNCTION(copyStripGroups, Verified);

    // copyFaces: `count` faces of `from` copied to `to` from its face
    // firstFace on, each face's material (its flags' low byte) moved by
    // materialOffset and its corners' vertices by vertexOffset.
    void copyFaces(ee::Ptr<ZgePart> from, ee::Ptr<ZgePart> to, uint32_t count, uint32_t firstFace, uint32_t materialOffset,
                   uint32_t vertexOffset)
    {
        ee::Local<Face> face;
        for (uint32_t copied = 0; copied < count; ++copied)
        {
            faceEntry(from, copied, face);
            // The material, the flags' low byte, moved alone.
            face->flags = static_cast<uint16_t>((face->flags & ~kFaceLowByte) | ((face->flags + materialOffset) & kFaceLowByte));
            // (Up to 15 corners: the record of a face is longer than Face says.)
            const ee::Ptr<uint16_t> vertices(face->vertex);
            for (uint32_t corner = 0; corner < ((face->flags & kFaceVertexCount) >> 8u); ++corner)
                vertices[corner] = static_cast<uint16_t>(vertices[corner] + vertexOffset);
            setFaceEntry(to, firstFace + copied, face);
        }
    }
    READABLE_FUNCTION(copyFaces, Verified);

}
