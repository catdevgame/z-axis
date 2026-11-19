// Tag tables (newBuffer makes one; zge/collision/collision.h TagTable):
// a 16-byte header (type 6, a reference count, the descriptor, the index
// count, the heap), an index per element, then at the next 16 bytes the
// records the indices pick. A part's face tags (TGFT) and face edges (TGFE)
// are tag tables, and so are tables the game makes for its own data; the
// game reads and writes the records through these accessors, byte and
// word at a time (the word ones through memcpy: records are not aligned).
//
// With PS2X_NATIVE_COLLISION (on by default; outside ps2xFnCheck) a part's face tags and
// edges are read from the host's mirror of the table
// (zge/native/collision_data.h TagMirror: the host part's table, made from
// the GEO file with the level setup's words) wherever an access falls in
// one; writes go to RAM and the mirror. The records handed back are the
// RAM table's either way. =verify compares every value read from a mirror
// with RAM.

#include "zge/memory/regions.h"
#include "zge/assets/tag_table.h"
#include "zge/assets/game_address.h"
#include "zge/collision/collision.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/alignment.h"
#include "zge/memory/pool.h"
#include "zge/native/collision_data.h"
#include "zge/native/host.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstring>
#include "sym/zge/assets/tag_table.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kMemcpy = sym::memoryCopy; // (dst, src, bytes)
        constexpr uint32_t kFree = sym::freeSized;   // (block, heap)
        constexpr uint64_t kMemcpyCallCycles = 16u;

        uint32_t descriptorIndexKind(uint32_t descriptor) { return descriptor & 0xFu; }
        uint32_t descriptorRecordBytes(uint32_t descriptor) { return (descriptor >> 4) & 0x3FFu; }
        uint32_t descriptorRecords(uint32_t descriptor) { return descriptor >> 14; }

        // The records follow the indices (indexBytes of them) at the next 16
        // bytes; the one `offset` bytes into them.
        ee::Ptr<TagRecord> recordAt(ee::Ptr<TagTableHeader> table, uint32_t indexBytes, uint32_t offset)
        {
            const ee::Ptr<uint8_t> records = tagTableIndices<uint8_t>(table) + roundUp16(indexBytes);
            return (records + offset).cast<TagRecord>();
        }

        // Where the records start: after the indices (indexBytes of them), at the next 16 bytes.
        ee::Ptr<uint8_t> recordsAfterIndices(ee::Ptr<TagTableHeader> table, uint32_t indexBytes)
        {
            return tagTableIndices<uint8_t>(table) + roundUp16(indexBytes);
        }
        // ZGE4's header word at +0x0C, the records' address (templates on the header: only ZGE4's code names it).
        template <typename Header>
        void keepRecords(ee::Ptr<Header> table, ee::Ptr<uint8_t> records)
        {
            table->records = records;
        }
        // ZGE4: record bytes `offset` into the records whose address the header keeps.
        template <typename Header>
        ee::Ptr<TagRecord> keptRecordAt(ee::Ptr<Header> table, uint32_t offset)
        {
            return (table->records + offset).template cast<TagRecord>();
        }

        // The mirror of a table, or of the one holding a field
        // (PS2X_NATIVE_COLLISION on, not under the checker).
        native::TagMirror *mirrorOf(ee::Ptr<TagTableHeader> table)
        {
            if (!native::hostExtrasOn() || !native::nativeCollisionEnabled())
                return nullptr;
            return native::tagMirror(table);
        }
        native::TagMirror *mirrorHolding(ee::Ptr<uint8_t> field, uint32_t bytes)
        {
            if (!native::hostExtrasOn() || !native::nativeCollisionEnabled())
                return nullptr;
            return native::tagMirrorHolding(field, bytes);
        }

        // The bytes `offset` into a record (a field: records are not aligned,
        // so the word ones go through memcpy).
        ee::Ptr<uint8_t> fieldOf(ee::Ptr<TagRecord> record, uint32_t offset)
        {
            return record.cast<uint8_t>() + offset;
        }

        template <typename T>
        T readMirror(native::TagMirror &mirror, ee::Ptr<uint8_t> field)
        {
            T value;
            std::memcpy(&value, mirror.at(field), sizeof(T));
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&value, &*field.cast<T>(), sizeof(T));
            native::countTagRead(true);
            return value;
        }

        // A field read in place: ZGE4 loads a record's unaligned word itself (tagWord, tagFloat), where ZGE2 and ZGE3
        // copy it out to the frame with memcpy.
        template <typename T>
        T readFieldInPlace(ee::Ptr<TagRecord> record, uint32_t offset)
        {
            const ee::Ptr<uint8_t> field = fieldOf(record, offset);
            if (native::TagMirror *mirror = mirrorHolding(field, sizeof(T)))
                return readMirror<T>(*mirror, field);
            T value;
            std::memcpy(&value, &*field, sizeof(T));
            return value;
        }

        // A field written in place: ZGE4 stores a record's unaligned word itself (setTagWord), where
        // ZGE2 and ZGE3 copy it in from the frame with memcpy.
        template <typename T>
        void writeFieldInPlace(ee::Ptr<TagRecord> record, uint32_t offset, T value)
        {
            const ee::Ptr<uint8_t> field = fieldOf(record, offset);
            std::memcpy(&*field, &value, sizeof(T));
            if (native::TagMirror *mirror = mirrorHolding(field, sizeof(T)))
                std::memcpy(mirror->at(field), &value, sizeof(T));
        }

        // The byte at record + offset, read and set: tagByte and setTagByte,
        // and inline in tagBit and setTagBitByte (the original calls neither).
        uint32_t readTagByte(ee::Ptr<TagRecord> record, uint32_t offset)
        {
            const ee::Ptr<uint8_t> field = fieldOf(record, offset);
            if (native::TagMirror *mirror = mirrorHolding(field, 1u))
                return readMirror<uint8_t>(*mirror, field);
            return *field;
        }
        void writeTagByte(ee::Ptr<TagRecord> record, uint32_t offset, uint32_t value)
        {
            const ee::Ptr<uint8_t> field = fieldOf(record, offset);
            *field = static_cast<uint8_t>(value);
            if (native::TagMirror *mirror = mirrorHolding(field, 1u))
                *mirror->at(field) = static_cast<uint8_t>(value);
        }
    }

    // tagRecordCount: how many records the table has.
    uint32_t tagRecordCount(ee::Ptr<TagTableHeader> table)
    {
        if (native::TagMirror *mirror = mirrorOf(table))
        {
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&mirror->descriptor, &table->descriptor, sizeof(uint32_t));
            return descriptorRecords(mirror->descriptor);
        }
        return descriptorRecords(table->descriptor);
    }
    READABLE_FUNCTION(tagRecordCount, Verified);

    // tagRecordAt: record `index` (not checked against the count).
    ee::Ptr<TagRecord> tagRecordAt(ee::Ptr<TagTableHeader> table, uint32_t index)
    {
        // ZGE4 keeps the records' address in the header (makeBuffer).
        if constexpr (game::readable::kIsBmx)
            return keptRecordAt(table, index * descriptorRecordBytes(table->descriptor));
        uint32_t descriptor, count;
        if (native::TagMirror *mirror = mirrorOf(table))
        {
            if (native::nativeCollisionVerify())
            {
                native::noteVerifiedRead(&mirror->descriptor, &table->descriptor, sizeof(uint32_t));
                native::noteVerifiedRead(&mirror->count, &table->count, sizeof(uint32_t));
            }
            descriptor = mirror->descriptor;
            count = mirror->count;
        }
        else
        {
            descriptor = table->descriptor;
            count = table->count;
        }
        // The indices take the width of their kind (g_tagIndexWidth) each.
        const uint32_t indexBytes = (*g_tagIndexWidth)[descriptorIndexKind(descriptor)] * count;
        return recordAt(table, indexBytes, index * descriptorRecordBytes(descriptor));
    }
    READABLE_FUNCTION(tagRecordAt, Verified);

    namespace
    {
        // An element's index: from the mirror when it holds it, else RAM.
        template <typename Index>
        uint32_t elementIndex(native::TagMirror *mirror, ee::Ptr<TagTableHeader> table, uint32_t element)
        {
            const ee::Ptr<Index> indices = tagTableIndices<Index>(table);
            const uint32_t offset = element * static_cast<uint32_t>(sizeof(Index));
            if (!mirror || !mirror->holdsAtOffset(offset, sizeof(Index)))
                return indices[element];
            Index value;
            std::memcpy(&value, mirror->atOffset(offset), sizeof(Index));
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&value, &indices[element], sizeof(Index));
            native::countTagRead(true);
            return value;
        }
    }

    // tagElementRecord: the record element `element` picks, or 0 when its index
    // is "none" (0xFF for u8 indices, 0xFFFF for u16) or the table's index
    // kind is neither (the element is not checked against the count).
    ee::Ptr<TagRecord> tagElementRecord(ee::Ptr<TagTableHeader> table, uint32_t element)
    {
        native::TagMirror *mirror = mirrorOf(table);
        uint32_t descriptor;
        if (mirror)
        {
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&mirror->descriptor, &table->descriptor, sizeof(uint32_t));
            descriptor = mirror->descriptor;
        }
        else
            descriptor = table->descriptor;

        uint32_t index;
        switch (descriptorIndexKind(descriptor))
        {
        case 0: // u8
            index = elementIndex<uint8_t>(mirror, table, element);
            if (index == 0xFFu)
                return {};
            break;
        case 1: // u16
            index = elementIndex<uint16_t>(mirror, table, element);
            if (index == 0xFFFFu)
                return {};
            break;
        default:
            return {};
        }
        if (!mirror && native::hostExtrasOn() && native::nativeCollisionEnabled())
            native::countTagRead(false);
        return tagRecordAt(table, index);
    }
    READABLE_FUNCTION(tagElementRecord, Verified);

    // tagByte: the byte at record + offset. (The second argument is
    // unused.)
    uint32_t tagByte(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset)
    {
        return readTagByte(record, offset);
    }
    READABLE_FUNCTION(tagByte, Verified);

    // setTagByte: sets the byte at record + offset.
    void setTagByte(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, uint32_t value)
    {
        writeTagByte(record, offset, value);
    }
    READABLE_FUNCTION(setTagByte, Verified);

    // tagWord: the word at record + offset, copied out to the frame
    // with memcpy (records are not aligned).
    int32_t tagWord(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset)
    {
        if constexpr (game::readable::kIsBmx)
            return readFieldInPlace<int32_t>(record, offset);
        ee::Local<uint32_t> word;
        const ee::Ptr<uint8_t> field = fieldOf(record, offset);
        if (native::TagMirror *mirror = mirrorHolding(field, 4u))
            *word = readMirror<uint32_t>(*mirror, field);
        else
            memoryCopy(word, field, 4u);
        return static_cast<int32_t>(*word);
    }
    READABLE_FUNCTION(tagWord, Verified);

    // tagFloat (kTagFloat): the float at record + offset, copied out to
    // the frame as tagWord does.
    ee::Float tagFloat(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset)
    {
        if constexpr (game::readable::kIsBmx)
            return readFieldInPlace<ee::Float>(record, offset);
        ee::Local<ee::Float> value;
        const ee::Ptr<uint8_t> field = fieldOf(record, offset);
        if (native::TagMirror *mirror = mirrorHolding(field, 4u))
            *value = readMirror<ee::Float>(*mirror, field);
        else
            memoryCopy(value, field, 4u);
        return *value;
    }
    READABLE_FUNCTION(tagFloat, Verified);

    // setTagWord: sets the word at record + offset (memcpy from the
    // frame); returns what memcpy does, the word's address.
    READABLE_RESULT_EXCEPT(bmx, uint32_t) setTagWord(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, ee::Address value)
    {
        if constexpr (game::readable::kIsBmx)
        {
            writeFieldInPlace<uint32_t>(record, offset, value);
            return READABLE_RETURN_EXCEPT(bmx, 0u);
        }
        ee::Local<uint32_t> src;
        *src = value;
        const ee::Ptr<uint8_t> field = fieldOf(record, offset);
        const uint32_t result = memoryCopy(field, src, 4u);
        if (native::TagMirror *mirror = mirrorHolding(field, 4u))
            std::memcpy(mirror->at(field), &value, sizeof(value));
        return READABLE_RETURN_EXCEPT(bmx, result);
    }
    READABLE_FUNCTION(setTagWord, Verified);

    // releaseTagTable: drops a reference; the last one frees the table (and
    // its mirror). Returns the table, or 0 once freed.
    ee::Ptr<TagTableHeader> releaseTagTable(ee::Ptr<TagTableHeader> table)
    {
        const uint16_t left = static_cast<uint16_t>(table->references - 1u);
        table->references = left;
        if (left != 0)
            return table;
        if (native::hostExtrasOn())
            native::forgetTagTable(table);
        // ZGE4 has no size in the header: the descriptor gives it again. ZGE2 frees the block whole.
        if constexpr (game::readable::kIsBmx)
            freeSized(table, tagDescriptorBytes(&table->descriptor));
        else if constexpr (game::readable::kIsDm2)
            heapFree(table);
        else
            freeSized(table, table->heap);
        return ee::Ptr<TagTableHeader>();
    }
    READABLE_FUNCTION(releaseTagTable, Verified);

    namespace
    {
        constexpr uint32_t kNewTagTable = sym::newBuffer;     // (descriptor *)
        constexpr uint32_t kInitTagTable = sym::makeBuffer;    // (table, descriptor *, bytes)
        constexpr uint32_t kTagTableBytes = sym::tagDescriptorBytes;   // (descriptor *)
        constexpr uint32_t kTagTableType = game::readable::perGame(6u, 5u, 6u); // the engine's object type (ZGE2's 5)

        // Bytes per index by the descriptor's kind.
        const ee::Ptr<uint32_t> g_tagIndexWidths(sym::g_tagIndexWidth);
        inline ee::Global<uint32_t, sym::g_tagTablesReady> g_tagTablesReady;
    }

    // tagDescriptorBytes: what a table of this descriptor (+0 its bits, +4 its
    // index count, as a table's header +4) takes: the header, the indices
    // rounded to 16 bytes, the records.
    uint32_t tagDescriptorBytes(ee::Address descriptor)
    {
        const ee::Ptr<uint32_t> words(descriptor);
        const uint32_t bits = words[0];
        const uint32_t indexBytes = words[1] * g_tagIndexWidths[descriptorIndexKind(bits)];
        return roundUp16(indexBytes) + descriptorRecords(bits) * descriptorRecordBytes(bits) + sizeof(TagTableHeader);
    }
    READABLE_FUNCTION(tagDescriptorBytes, Verified);

    // initTagTables.
    uint32_t initTagTables()
    {
        *g_tagTablesReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initTagTables, Verified);

    // shutdownTagTables.
    uint32_t shutdownTagTables()
    {
        if (*g_tagTablesReady == 0)
            return 1;
        *g_tagTablesReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownTagTables, Verified);

    // newBuffer: a table from the sized heap, one reference; its indices
    // and records are not cleared.
    ee::Ptr<TagTableHeader> newBuffer(ee::Address descriptor)
    {
        const uint32_t bytes = tagDescriptorBytes(descriptor);
        // (ZGE2 has no sized pools: the heap)
        const ee::Ptr<TagTableHeader> table(game::readable::kIsDm2 ? heapAllocate(bytes) : allocateSized(bytes));
        makeBuffer(table, descriptor, bytes);
        return table;
    }
    READABLE_FUNCTION_WAITS(newBuffer, Verified);

    // makeBuffer: a table made in `memory` (a block of its own, or a record's
    // buffer object), one reference.
    void makeBuffer(ee::Address memory, ee::Address descriptor, uint32_t bytes)
    {
        const ee::Ptr<TagTableHeader> table(memory);
        memoryFill(table, 0, sizeof(TagTableHeader));
        if constexpr (!game::readable::kIsBmx)
            table->heap = bytes;
        table->type = kTagTableType;
        table->references = 1;
        memoryCopy(&table->descriptor, descriptor, sizeof(TagTableHeader::descriptor) + sizeof(TagTableHeader::count));
        // ZGE4 keeps the records' address (after the indices, at the next 16 bytes) where the others keep the size.
        if constexpr (game::readable::kIsBmx)
            keepRecords(table, recordsAfterIndices(table, table->count * g_tagIndexWidths[descriptorIndexKind(table->descriptor)]));
    }
    READABLE_FUNCTION(makeBuffer, Verified);

    // copyTagTable.
    ee::Ptr<TagTableHeader> copyTagTable(ee::Ptr<TagTableHeader> table)
    {
        const ee::Ptr<uint32_t> descriptor = &table->descriptor;
        const uint32_t bytes = tagDescriptorBytes(descriptor);
        const ee::Ptr<TagTableHeader> copy = newBuffer(descriptor);
        memoryCopy(copy + 1, table + 1, bytes - sizeof(TagTableHeader));
        return copy;
    }
    READABLE_FUNCTION_WAITS(copyTagTable, Verified);

    // addTagTableReference: one reference more. Returns the table.
    ee::Ptr<TagTableHeader> addTagTableReference(ee::Ptr<TagTableHeader> table)
    {
        table->references += 1;
        return table;
    }
    READABLE_FUNCTION(addTagTableReference, Verified);

    // setTagShort .. setTagString (no caller in the ELF): the record's field at
    // `offset` written or read; the second argument, the value's kind (2 a
    // byte, 8 a word, as tagByte and tagWord are called), is unused.
    // setTagShort: the 16-bit value written.
    int32_t setTagShort(uint32_t record, uint32_t /*valueKind*/, uint32_t offset, uint32_t value)
    {
        ee::Local<uint16_t> valueSlot;
        *valueSlot = static_cast<uint16_t>(value);
        return memoryCopy(record + offset, valueSlot, 2);
    }
    READABLE_FUNCTION(setTagShort, Draft);

    // setTagWordFrom: 4 bytes copied in from source.
    int32_t setTagWordFrom(uint32_t record, uint32_t /*valueKind*/, uint32_t offset, uint32_t src)
    {
        return memoryCopy(record + offset, src, 4);
    }
    READABLE_FUNCTION(setTagWordFrom, Draft);

    // setTagVector: 12 bytes copied in from source. ZGE4 copies them in
    // place (unaligned loads and stores) and answers nothing.
    READABLE_RESULT_EXCEPT(bmx, int32_t) setTagVector(ee::Address record, uint32_t /*valueKind*/, uint32_t offset, ee::Address src)
    {
        if constexpr (game::readable::kIsBmx)
            std::memcpy(&*ee::Ptr<uint8_t>(record + offset), &*ee::Ptr<uint8_t>(src), sizeof(Vec3));
        else
            return READABLE_RETURN_EXCEPT(bmx, memoryCopy(record + offset, src, 12));
    }
    READABLE_FUNCTION(setTagVector, Draft);

    // tagString: the string copied out to dst.
    uint32_t tagString(ee::Address record, uint32_t /*valueKind*/, uint32_t offset, ee::Address dst)
    {
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 copies at most 0x40 bytes, the last a 0.
            constexpr uint32_t kTagStringBytes = 0x40u;
            stringCopyN(dst, record + offset, kTagStringBytes);
            const ee::Ptr<char> copy(dst);
            copy[kTagStringBytes - 1] = 0;
        }
        else
            stringCopy(dst, record + offset);
        return dst;
    }
    READABLE_FUNCTION(tagString, Draft);

    // copyTagWord (record, ..., offset, out): the 4 bytes at record + offset
    // copied to out (memoryCopy). Was game/aggressive's (camera.cpp); the
    // blocks say it is the engine's (layer2), DM2 (PS2) has it word for word.
    uint32_t copyTagWord(ee::Address record, uint32_t /*group*/, uint32_t offset, ee::Address dst)
    {
        zge::memoryCopy(dst, record + offset, 4);
        return dst;
    }

    // copyTagWordInPlace: ZGE4's copyTagWord: the same 4 bytes copied by the
    // function itself (an unaligned word loaded and stored), no memoryCopy.
    uint32_t copyTagWordInPlace(ee::Address record, uint32_t /*group*/, uint32_t offset, ee::Address dst)
    {
        std::memcpy(ee::at<uint8_t>(dst), ee::at<uint8_t>(record + offset), sizeof(uint32_t));
        return dst;
    }
    READABLE_FUNCTION_OWN(copyTagWord, Verified, bmx, copyTagWordInPlace);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        uint32_t stringCopy(uint32_t dst, uint32_t src) { return game::tail(zge::stringCopy, dst, src); }
        }
    }

    // setTagString: the string copied in from source.
    uint32_t setTagString(uint32_t record, uint32_t /*valueKind*/, uint32_t offset, uint32_t src)
    {
        return tail::stringCopy(record + offset, src);
    }
    READABLE_FUNCTION(setTagString, Draft);

    uint32_t tagStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(tagStubZero, Draft);

    void tagStubA()
    {
    }
    READABLE_FUNCTION(tagStubA, Draft);

    void tagStubB()
    {
    }
    READABLE_FUNCTION(tagStubB, Draft);

    void tagStubC()
    {
    }
    READABLE_FUNCTION(tagStubC, Draft);

    void tagStubD()
    {
    }
    READABLE_FUNCTION(tagStubD, Draft);

    void tagStubE()
    {
    }
    READABLE_FUNCTION(tagStubE, Draft);

    void tagStubF()
    {
    }
    READABLE_FUNCTION(tagStubF, Draft);

    // tagVector: the three words at record + offset copied out to dst (as
    // memcpy: records are not aligned), dst's fourth word 0. Returns dst.
    // ZGE4 copies them in place (unaligned loads and stores) where ZGE2 and
    // ZGE3 call memoryCopy.
    ee::Ptr<Vec4> tagVector(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, ee::Ptr<Vec4> dst)
    {
        const ee::Ptr<uint8_t> field = fieldOf(record, offset);
        if (native::TagMirror *mirror = mirrorHolding(field, sizeof(Vec3)))
            std::memcpy(&dst->xyz(), mirror->at(field), sizeof(Vec3));
        else if constexpr (game::readable::kIsBmx)
            std::memcpy(&dst->xyz(), &*field, sizeof(Vec3));
        else
            memoryCopy(dst, field, sizeof(Vec3));
        dst->w = ee::Float::fromBits(0);
        return dst;
    }
    READABLE_FUNCTION(tagVector, Verified);

    // setTagFloat: sets the float at record + offset (memcpy from the
    // frame, as setTagWord); returns what memcpy does, the float's address.
    // ZGE4 stores the float in place (an unaligned store) and returns nothing.
    READABLE_RESULT_EXCEPT(bmx, uint32_t) setTagFloat(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, ee::Float value)
    {
        if constexpr (game::readable::kIsBmx)
        {
            const ee::Ptr<uint8_t> field = fieldOf(record, offset);
            std::memcpy(&*field, &value, sizeof(ee::Float));
            if (native::TagMirror *mirror = mirrorHolding(field, sizeof(ee::Float)))
                std::memcpy(mirror->at(field), &value, sizeof(ee::Float));
        }
        else
        {
            ee::Local<ee::Float> src;
            *src = value;
            const ee::Ptr<uint8_t> field = fieldOf(record, offset);
            const uint32_t result = memoryCopy(field, src, sizeof(ee::Float));
            if (native::TagMirror *mirror = mirrorHolding(field, sizeof(ee::Float)))
                std::memcpy(mirror->at(field), &*src, sizeof(ee::Float));
            return READABLE_RETURN_EXCEPT(bmx, result);
        }
    }
    READABLE_FUNCTION(setTagFloat, Verified);

    // tagRecordBytes: the bytes of one of the table's records (its
    // descriptor's).
    uint32_t tagRecordBytes(ee::Ptr<TagTableHeader> table)
    {
        return descriptorRecordBytes(table->descriptor);
    }
    READABLE_FUNCTION(tagRecordBytes, Draft);

    // tagBit: bit `bit` of the bytes at record (bit 0 the first byte's
    // lowest). (The second argument is unused.)
    uint32_t tagBit(ee::Ptr<TagRecord> record, uint32_t, uint32_t bit)
    {
        return (readTagByte(record, bit >> 3) >> (bit & 7u)) & 1u;
    }
    READABLE_FUNCTION(tagBit, Draft);

    // setTagBitByte: the byte holding bit `bit` set to value: the whole
    // byte, not the bit (as the original).
    void setTagBitByte(ee::Ptr<TagRecord> record, uint32_t, uint32_t bit, uint32_t value)
    {
        writeTagByte(record, bit >> 3, value);
    }
    READABLE_FUNCTION(setTagBitByte, Draft);

    // tagShort: the 16-bit value at record + offset (memcpy to the frame).
    uint32_t tagShort(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset)
    {
        ee::Local<uint16_t> value;
        const ee::Ptr<uint8_t> field = fieldOf(record, offset);
        if (native::TagMirror *mirror = mirrorHolding(field, sizeof(uint16_t)))
            *value = readMirror<uint16_t>(*mirror, field);
        else
            memoryCopy(value, field, sizeof(uint16_t));
        return *value;
    }
    READABLE_FUNCTION(tagShort, Draft);
}
