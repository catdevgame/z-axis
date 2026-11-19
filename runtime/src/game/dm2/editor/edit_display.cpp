// DM2's park editor: the shown nodes and the cell rectangles
// (edit_display.h). DM2's own code, after ./src/com/editor/editGui.c in its
// build.

#include "edit_display.h"
#include "edit_core.h"
#include "edit_objects.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/render/terrain.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/editor/edit_display.cpp.h"

namespace game::dm2
{
    namespace
    {
        // 1 while the whole park is being cleared: the nodes go with the
        // park then, and releaseEditNode leaves them alone.
        inline ee::Global<uint32_t, sym::g_parkClearing> g_parkClearing;
        // The held object: its node and its index among the editor's objects.
        inline ee::Global<ee::Ptr<zge::Node>, sym::g_heldObjectNode> g_heldObjectNode;
        inline ee::Global<uint8_t, sym::g_heldObject> g_heldObject;
        // 65536 / 360: a degree in the engine's angle units.
        inline ee::Global<ee::Float, sym::angleUnitsPerDegree> angleUnitsPerDegree;

        // The copy keeps its own vertices and colours (copyGeoObject's flags).
        constexpr uint32_t kNodeCopyFlags = 0x00044845u;

        constexpr uint32_t kLightObjectFirst = 148;
        constexpr uint32_t kLightObjectCount = 9;

        constexpr uint32_t kCostPerObject = 200;
        constexpr uint32_t kCostWithoutMesh = 500;

        constexpr uint8_t kOpaque = 0xFF;
    }

    int32_t setMeshBlendMode(ee::Ptr<zge::ZgePart> mesh, uint32_t mode)
    {
        for (uint32_t set = 0; set < uint32_t(zge::materialSetCount(mesh));)
        {
            const ee::Ptr<zge::MaterialTable> table = zge::meshMaterials(mesh, set);
            ++set;
            if (!table)
                continue;
            for (uint32_t i = 0; i < table->count; ++i)
                // (the engine's jump to setMaterialState: AI's name for the
                // same one-instruction function)
                zge::zgui::rebuildMaterialRegisters(table->materials[i], 0, zge::kStateBlendMode, mode);
        }
        return 1;
    }
    READABLE_FUNCTION(setMeshBlendMode, Verified);

    ee::Ptr<zge::Node> newWorldObjectInstance(uint32_t nameHash)
    {
        const ee::Ptr<zge::GeoObject> object(zge::findWorldObjectByHash(nameHash));
        if (!object)
            return ee::Ptr<zge::Node>();
        const ee::Ptr<zge::GeoObject> copy = zge::copyGeoObject(object, kNodeCopyFlags);
        if (!copy)
            return ee::Ptr<zge::Node>();
        const ee::Ptr<zge::Node> node(zge::newAppInstanceNode(copy, 1));
        zge::releaseGeoObject(copy);
        return node;
    }
    READABLE_FUNCTION(newWorldObjectInstance, Verified);

    void releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>> node)
    {
        if (*g_parkClearing == 0)
        {
            zge::releaseNode(*node, 1);
            *node = ee::Ptr<zge::Node>();
        }
    }
    READABLE_FUNCTION(releaseEditNode, Verified);

    void setNodePosition(ee::Ptr<zge::Node> node, ee::Ptr<const zge::Vec4> position)
    {
        if (!node)
            return;
        const ee::Ptr<zge::Matrix> matrix = zge::nodeMatrix(node, 0);
        matrix->translation() = *position;
        game::tail(zge::setNodeLocalMatrix, node, matrix);
    }
    READABLE_FUNCTION(setNodePosition, Verified);

    void unusedNodeSetting()
    {
    }
    READABLE_FUNCTION(unusedNodeSetting, Verified);

    void setNodeHeading(ee::Ptr<zge::Node> node, ee::Float degrees)
    {
        if (!node)
            return;
        ee::Local<zge::Matrix> turned;
        const ee::Ptr<zge::Matrix> matrix = zge::nodeMatrix(node, 0);
        zge::matrixIdentity(turned);
        zge::matrixRotateY(turned, turned, zge::Angle(ee::toInt(degrees * *angleUnitsPerDegree)));
        turned->translation() = matrix->translation();
        zge::setNodeLocalMatrix(node, turned);
    }
    READABLE_FUNCTION(setNodeHeading, Verified);

    void nodePosition(ee::Ptr<zge::Node> node, ee::Ptr<zge::Vec4> position)
    {
        if (!node)
            return;
        *position = zge::nodeMatrix(node, 0)->translation();
    }
    READABLE_FUNCTION(nodePosition, Verified);

    void setNodeColour(ee::Ptr<zge::Node> node, ee::Ptr<const zge::VertexColour> colour)
    {
        if (!hasCollisionFrame(node))
            return;
        const ee::Ptr<zge::ZgePart> mesh = node->lods->part;
        if (!mesh)
            return;
        ee::Local<zge::VertexColour> before; // the first vertex's colour as it was
        const uint32_t count = mesh->vertexCount;
        zge::partColour(mesh, 0, before);
        for (uint32_t vertex = 0; vertex < count; ++vertex)
            zge::setPartColour(mesh, vertex, colour);
        if (before->alpha < kOpaque && colour->alpha == kOpaque)
            zge::forEachObjectMesh(node, zge::MeshVisit(sym::setMeshBlendMode), 0);
        if (before->alpha == kOpaque && colour->alpha < kOpaque)
            zge::forEachObjectMesh(node, zge::MeshVisit(sym::setMeshBlendMode), 1);
    }
    READABLE_FUNCTION(setNodeColour, Verified);

    ee::Ptr<zge::Box> heldObjectBox()
    {
        if (!*g_heldObjectNode)
            return ee::Ptr<zge::Box>();
        return zge::nodeBox(*g_heldObjectNode);
    }
    READABLE_FUNCTION(heldObjectBox, Verified);

    void cursorCellBox(ee::Ptr<ParkEditor> editor, ee::Ptr<CellBox> box)
    {
        if (!editor)
            return;
        // The cursor's cell is asked for and not used.
        ee::Local<uint8_t> column;
        ee::Local<uint8_t> row;
        cursorCell(editor, column, row);

        const int32_t top = -int32_t(parkCell(editor, editor->cursorColumn, editor->cursorRow)->level);
        box->top = top;
        if (!*g_heldObjectNode)
        {
            box->bottom = top;
        }
        else
        {
            int32_t highest = int32_t(editObjectCorner(*g_heldObject, 0));
            const int32_t second = int32_t(editObjectCorner(*g_heldObject, 1));
            const int32_t third = int32_t(editObjectCorner(*g_heldObject, 2));
            const int32_t fourth = int32_t(editObjectCorner(*g_heldObject, 3));
            if (highest < second)
                highest = second;
            if (highest < third)
                highest = third;
            if (highest < fourth)
                highest = fourth;
            box->bottom = top - highest;
        }

        if (editor->selectionColumns > 0)
        {
            box->firstColumn = editor->selectionColumn - editor->selectionColumns + 1;
            box->lastColumn = editor->selectionColumn;
        }
        else
        {
            box->firstColumn = editor->selectionColumn;
            box->lastColumn = editor->selectionColumn - editor->selectionColumns - 1;
        }
        if (editor->selectionRows > 0)
        {
            box->firstRow = editor->selectionRow - editor->selectionRows + 1;
            box->lastRow = editor->selectionRow;
        }
        else
        {
            box->firstRow = editor->selectionRow;
            box->lastRow = editor->selectionRow - editor->selectionRows - 1;
        }
    }
    READABLE_FUNCTION(cursorCellBox, Verified);

    void placedObjectCellBox(ee::Ptr<PlacedObject> object, ee::Ptr<CellBox> box)
    {
        if (!object)
            return;
        const int32_t column = int32_t(object->column) - editObjectCentreX(object->object, object->rotation);
        const int32_t row = int32_t(object->row) - editObjectCentreY(object->object, object->rotation);
        // A size the turn makes negative reaches back from the object's cell.
        if (editObjectSpanX(object->object, object->rotation) < 0)
        {
            const int32_t span = editObjectSpanX(object->object, object->rotation);
            box->lastColumn = column;
            box->firstColumn = column + span + 1;
        }
        else
        {
            box->firstColumn = column;
            box->lastColumn = column + editObjectSpanX(object->object, object->rotation) - 1;
        }
        if (editObjectSpanY(object->object, object->rotation) < 0)
        {
            const int32_t span = editObjectSpanY(object->object, object->rotation);
            box->lastRow = row;
            box->firstRow = row + span + 1;
        }
        else
        {
            box->firstRow = row;
            box->lastRow = row + editObjectSpanY(object->object, object->rotation) - 1;
        }
    }
    READABLE_FUNCTION(placedObjectCellBox, Verified);

    uint32_t placedObjectCost(ee::Ptr<PlacedObject> object)
    {
        if (!object || !object->node)
            return 0;
        const ee::Ptr<zge::Node> node = object->node;
        if (!hasCollisionFrame(node))
            return kCostWithoutMesh;
        const ee::Ptr<zge::ZgePart> mesh = node->lods->part;
        if (!mesh)
            return kCostWithoutMesh;
        return mesh->vertexCount + kCostPerObject;
    }
    READABLE_FUNCTION(placedObjectCost, Verified);

    uint32_t editObjectIsLight(uint32_t object)
    {
        return object - kLightObjectFirst < kLightObjectCount;
    }
    READABLE_FUNCTION(editObjectIsLight, Verified);
}
