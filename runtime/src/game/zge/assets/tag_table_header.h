#pragma once

// A tag table's header, on its own: a material holds one (zge/render/material.h),
// and tag_table.h's accessors take it.

#include "ee/ee.h"

#include <cstdint>

namespace zge
{
    // A tag table's 16-byte header (collision.h TagTable, as the accessors
    // see it).
    struct TagTableHeader
    {
        uint8_t type;        // +0x00 6
        uint8_t _01;
        uint16_t references; // +0x02 addTagTableReference adds one, releaseTagTable takes one
        uint32_t descriptor; // +0x04 TagTable::descriptor
        uint32_t count;      // +0x08 elements
#if ZX_GAME_BMX
        union
        {
            // BMX +0x0C ZGE4: the records' address (makeBuffer works it out once, tagRecordAt reads it); it keeps no
            // size (releaseTagTable works it out from the descriptor again)
            ee::Ptr<uint8_t> records;
            // [unplaced: ZGE2's and ZGE3's name for the word, so the shared code compiles; no BMX function reads it]
            uint32_t heap;
        };
#else
        uint32_t heap;       // +0x0C the heap it came from (makeBuffer's bytes)
#endif
    };
    static_assert(sizeof(TagTableHeader) == 16);
    // ZGE4's records' address (TagTableHeader::records, BMX XXX only): where code that reads a table's first record
    // inline takes it from (BMX XXX's rider). ZGE2 and ZGE3 keep no such word, so this is 0 there.
    inline ee::Ptr<uint8_t> keptTagRecords(ee::Ptr<TagTableHeader> table)
    {
#if ZX_GAME_BMX
        return table->records;
#else
        (void)table;
        return ee::Ptr<uint8_t>();
#endif
    }
}
