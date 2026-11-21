// DM2's park editor: the nodes the editor shows (a copy of a world object
// per placed object, rail and held object), their place, heading and
// colour, and the rectangles of cells the cursor and a placed object cover.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // A shown node of a copy of the world object of that id; 0 when the
    // world has none or the copy fails.
    ee::Ptr<zge::Node> newWorldObjectInstance(uint32_t nameHash);
    // The node released with its family and the pointer cleared; nothing
    // while the whole park is being cleared.
    void releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>> node);

    // A node's place: set (its matrix's translation, then the node told),
    // and read. Nothing for no node.
    void setNodePosition(ee::Ptr<zge::Node> node, ee::Ptr<const zge::Vec4> position);
    void nodePosition(ee::Ptr<zge::Node> node, ee::Ptr<zge::Vec4> position);
    // A node turned about the vertical to `degrees`, its place kept.
    void setNodeHeading(ee::Ptr<zge::Node> node, ee::Float degrees);
    // Called with a node and 2.0 (the held object) or 0.5 (a placed one):
    // the function is empty in this build.
    void unusedNodeSetting();

    // Every vertex of the node's mesh given the colour; the materials'
    // blend mode follows when the colour turns translucent or opaque.
    void setNodeColour(ee::Ptr<zge::Node> node, ee::Ptr<const zge::VertexColour> colour);
    int32_t setMeshBlendMode(ee::Ptr<zge::ZgePart> mesh, uint32_t mode);

    // The held object's node's box, 0 without one.
    ee::Ptr<zge::Box> heldObjectBox();

    // The cells the cursor covers (the selected rectangle) and its levels.
    void cursorCellBox(ee::Ptr<ParkEditor> editor, ee::Ptr<CellBox> box);
    // The cells a placed object covers, from its size and centre as turned.
    void placedObjectCellBox(ee::Ptr<PlacedObject> object, ee::Ptr<CellBox> box);

    // What a placed object takes of the park's capacity: its mesh's vertex
    // count and 200, 500 when its node has no mesh, 0 without a node.
    uint32_t placedObjectCost(ee::Ptr<PlacedObject> object);
    // Objects 148 .. 156 are the lights ("Too many Lights!" is their limit's
    // message).
    uint32_t editObjectIsLight(uint32_t object);
}
