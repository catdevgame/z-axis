// A scene point's tags handed to a node: BMX XXX's own code (its game
// block), word for word Aggressive Inline's (game/aggressive/pickups.cpp
// setNodeTagTable, which its NPC spawn calls).

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/locators.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/bmx/level/point_tags.cpp.h"

namespace game::bmx
{
    namespace
    {
        // What newBuffer is asked for: a tag table of `records` records of
        // `recordBytes` each.
        struct TagTableRequest
        {
            uint32_t kind : 4;
            uint32_t recordBytes : 10;
            uint32_t records : 18;
            uint32_t reserved;
        };
        static_assert(sizeof(TagTableRequest) == 8);
        constexpr uint32_t kTagTableKind = 3;
    }

    // setNodeTagTable: the point's tags copied into a buffer the node holds.
    void setNodeTagTable(ee::Ptr<zge::ScenePoint> point, ee::Ptr<zge::Node> rootNode)
    {
        ee::Local<TagTableRequest> request;
        request->kind = kTagTableKind;
        request->recordBytes = point->tagBytes;
        request->records = 1;
        request->reserved = 0;
        const ee::Ptr<zge::TagTableHeader> table = zge::newBuffer(request);
        zge::memoryCopy(table + 1, point->tags, point->tagBytes);
        zge::setNodeTags(rootNode, table);
        zge::releaseTagTable(table);
    }
    READABLE_FUNCTION_WAITS(setNodeTagTable, Verified);
}
