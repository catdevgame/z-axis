#pragma once

// Tag tables and their accessors (tag_table.cpp; the layout in
// zge/collision/collision.h TagTable): the game reads and writes a table's
// records only through these, a byte or a word at a time.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "zge/assets/tag_table_header.h"
#include "zge/collision/collision.h"

#include <cstdint>

namespace zge
{
    static_assert(sizeof(TagTableHeader) == sizeof(TagTable));

    // The indices that follow a table's header, one per element (u8 or u16
    // by the descriptor's index kind), each picking a record or "none".
    template <typename Index>
    ee::Ptr<Index> tagTableIndices(ee::Ptr<TagTableHeader> table)
    {
        return (table + 1).cast<Index>();
    }

    // A record of a tag table. Its layout is the table's (a face's
    // FaceTagRecord, an edge's EdgeRecord, a material's MaterialTags); the
    // game reads and writes one through tagByte, tagWord and their setters.
    struct TagRecord;
    // A record as its table's layout says it is.
    template <typename Record>
    ee::Ptr<Record> tagRecordAs(ee::Ptr<TagRecord> record)
    {
        return record.cast<Record>();
    }
    // A table's first record: the one right after the header (a table of one record keeps no indices).
    inline ee::Ptr<TagRecord> firstTagRecord(ee::Ptr<TagTableHeader> table)
    {
        return (table + 1).cast<TagRecord>();
    }
    // A record's field at `offset`, as its type (what tagWord and setTagVector
    // read and write there).
    template <typename Field>
    ee::Ptr<Field> tagFieldAs(ee::Ptr<TagRecord> record, uint32_t offset)
    {
        return (record.cast<uint8_t>() + offset).template cast<Field>();
    }

    uint32_t tagRecordCount(ee::Ptr<TagTableHeader> table);
    // Record `index` (objectComponent and levelObjectClassTag hand on the
    // first to their many callers).
    ee::Ptr<TagRecord> tagRecordAt(ee::Ptr<TagTableHeader> table, uint32_t index);
    // The record an element's index picks, or 0 (none).
    ee::Ptr<TagRecord> tagElementRecord(ee::Ptr<TagTableHeader> table, uint32_t element);
    // A byte or word of a record (the second argument is unused).
    uint32_t tagByte(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset);
    void setTagByte(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, uint32_t value);
    int32_t tagWord(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset);
    ee::Float tagFloat(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset);        // (kTagFloat)
    // (ZGE4 stores in place and answers nothing; ZGE2 and ZGE3 answer memcpy's result, the field's address)
    READABLE_RESULT_EXCEPT(bmx, uint32_t) setTagWord(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, ee::Address value);
    // A record's text copied out to dst (ZGE4: at most 0x40 bytes, ended with a 0).
    uint32_t tagString(ee::Address record, uint32_t, uint32_t offset, ee::Address dst);
    ee::Ptr<TagTableHeader> releaseTagTable(ee::Ptr<TagTableHeader> table);
}

namespace zge
{
    // (tag_table.cpp)
    uint32_t initTagTables();
    uint32_t shutdownTagTables();
    // The bytes a table of this descriptor (bits, then the index count) takes.
    uint32_t tagDescriptorBytes(ee::Address descriptor);
    ee::Ptr<TagTableHeader> newBuffer(ee::Address descriptor);
    void makeBuffer(ee::Address memory, ee::Address descriptor, uint32_t bytes);
    ee::Ptr<TagTableHeader> copyTagTable(ee::Ptr<TagTableHeader> table);
    ee::Ptr<TagTableHeader> addTagTableReference(ee::Ptr<TagTableHeader> table);
    uint32_t copyTagWord(ee::Address record, uint32_t group, uint32_t offset, ee::Address dst);
    ee::Ptr<Vec4> tagVector(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, ee::Ptr<Vec4> dst);
    // (ZGE4 copies in place and answers nothing)
    READABLE_RESULT_EXCEPT(bmx, int32_t) setTagVector(ee::Address record, uint32_t, uint32_t offset, ee::Address src);
    READABLE_RESULT_EXCEPT(bmx, uint32_t) setTagFloat(ee::Ptr<TagRecord> record, uint32_t, uint32_t offset, ee::Float value);
}
