// DM2's park editor: the tags of a piece's collision mesh as the editor
// reads and changes them (the level's tag tables: a record for each face
// and for each face's three edges), and the tags the editor's gaps and
// lights go by.
#pragma once

#include "ee/ee.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/node.h"

#include <cstdint>

namespace game::dm2
{
    // The hashes of the names the editor's gap faces are tagged with:
    // "EDITGAP", and a table of "EDITGAP1" .. "EDITGAP4" (entry 0 is never
    // set; 1: EDITGAP2, 2: EDITGAP1, 3: EDITGAP4, 4: EDITGAP3).
    constexpr uint32_t kEditGapTagCount = 5;
    void setUpEditGapTags();
    void clearEditGapTags();
    uint32_t editGapTag();
    uint32_t editGapTagOf(uint32_t index);

    // The tag record of an edge (three to a face) and of a face of the
    // node's collision mesh, or 0.
    ee::Ptr<zge::TagRecord> editEdgeTagRecord(ee::Ptr<zge::Node> node, uint32_t edge);
    ee::Ptr<zge::TagRecord> editFaceTagRecord(ee::Ptr<zge::Node> node, uint32_t face);

    // Whether an edge can be ground; one edge's flag set or cleared (0: the
    // edge has no record); the flag of every edge of a group (0: no mesh).
    uint32_t editEdgeGrindable(ee::Ptr<zge::Node> node, uint32_t edge);
    uint32_t setEditEdgeGrindable(ee::Ptr<zge::Node> node, uint32_t edge, uint8_t on);
    uint32_t setEditEdgesGrindable(ee::Ptr<zge::Node> node, uint8_t group, uint8_t on);

    // A face's tag (the hash the goals and the editor match), 0 without a
    // record; one face's tag set (0: no record); every face tagged `from`
    // tagged `to` (0: no mesh).
    int32_t editFaceTag(ee::Ptr<zge::Node> node, uint32_t face);
    uint32_t setEditFaceTag(ee::Ptr<zge::Node> node, uint32_t face, uint32_t tag);
    uint32_t replaceEditFaceTags(ee::Ptr<zge::Node> node, int32_t from, uint32_t to);

    // The faces tagged "EDITLIGHT", in order, and how many: whether any.
    uint32_t findEditLightFaces(ee::Ptr<zge::Node> node, ee::Ptr<uint32_t> faces, ee::Ptr<uint32_t> count);
}
