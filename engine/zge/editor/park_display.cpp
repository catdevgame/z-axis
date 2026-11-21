// What the park editor shows of a piece and where its camera looks: a display
// wraps the node of a world object, a tower's display stretches the shared
// "Editobj_towerdef" model to the tower's height; a view is a saved camera place.

#include "zge/editor/park_display.h"
#include "zge/editor/game.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"
#include "aggressive/conversions.h"
#include "zge/editor/edit_records.h"
#include "zge/assets/geo.h"
#include "zge/scene/scene.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/park_display.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kSetNodeLocalMatrix = sym::setNodeLocalMatrix; // (node, matrix)
        constexpr uint32_t kTextTowerDef = sym::g_textEditobjTowerdef;       // "Editobj_towerdef"

        inline ee::Global<uint32_t, sym::g_towerDisplayNode> g_towerDisplayNode; // made once, hidden: the vertices towers copy

        // The node's geometry frame's collision mesh.
        ee::Ptr<zge::ZgePart> nodeMesh(uint32_t node)
        {
            return ee::Ptr<zge::Node>(node)->lods->part;
        }
    }

    void hideTowerDisplay()
    {
        const uint32_t node = *g_towerDisplayNode;
        if (node == 0)
            return;
        releaseNode(ee::Ptr<zge::Node>(node), 1);
        *g_towerDisplayNode = 0;
    }
    READABLE_FUNCTION_WAITS(hideTowerDisplay, Verified);

    namespace
    {
        namespace tail
        {
        int32_t setNodeLocalMatrix(uint32_t node, ee::Ptr<zge::Matrix> matrix) { return ee::tailCall<int32_t>(kSetNodeLocalMatrix, node, matrix); }
        }
    }

    int32_t setDisplayMatrix(uint32_t display, ee::Ptr<zge::Matrix> matrix)
    {
        return tail::setNodeLocalMatrix(ee::Ptr<EditDisplay>(display)->node, matrix);
    }
    READABLE_FUNCTION_WAITS(setDisplayMatrix, Verified);

    uint32_t newEditDisplay(uint32_t hash, uint32_t copyFlags)
    {
        const uint32_t display = editAllocate(static_cast<uint32_t>(sizeof(EditDisplay)));
        ee::Ptr<EditDisplay>(display)->node = newWorldObjectInstance(hash, copyFlags);
        return display;
    }
    READABLE_FUNCTION_WAITS(newEditDisplay, Verified);

    int32_t freeShownDisplay(uint32_t display)
    {
        releaseNode(ee::Ptr<zge::Node>(ee::Ptr<EditDisplay>(display)->node), 1);
        return game::tail(editFree, display);
    }
    READABLE_FUNCTION_WAITS(freeShownDisplay, Verified);

    int32_t freeEditDisplay(uint32_t display)
    {
        return game::tail(editFree, display);
    }
    READABLE_FUNCTION_WAITS(freeEditDisplay, Verified);

    // stretchTowerDisplay: a tower piece's display, stretched to the tower's height. The
    // shared "Editobj_towerdef" model (made once, hidden and not solid) marks the vertices
    // that stand at each height with that height in u; each of them goes into the tower's
    // own copy of the model, raised by the y of the piece's local matrix.
    int32_t stretchTowerDisplay(ee::Ptr<EditDisplay> display, ee::Ptr<zge::Matrix> pieceMatrix, uint32_t height)
    {
        if (*g_towerDisplayNode == 0)
        {
            *g_towerDisplayNode = newWorldObjectInstance(zge::nameHash(ee::Ptr<const char>(kTextTowerDef)), 0);
            zge::setNodeShown(ee::Ptr<zge::Node>(*g_towerDisplayNode), 0, 0);
            zge::setNodeCollides(ee::Ptr<zge::Node>(*g_towerDisplayNode), 0, 1);
        }
        const ee::Ptr<zge::ZgePart> templateMesh = nodeMesh(*g_towerDisplayNode);
        const ee::Ptr<zge::ZgePart> towerMesh = nodeMesh(display->node);
        ee::Local<zge::Float4> uv;
        ee::Local<zge::Vec4> position;
        for (uint32_t vertex = 0; vertex < towerMesh->vertexCount; ++vertex)
        {
            zge::vertexUv(templateMesh, vertex, uv);
            if (uv->x == unsignedToFloat(height))
            {
                zge::vertexPosition(templateMesh, vertex, position);
                position->y = position->y + pieceMatrix->translation().y;
                zge::setPartPosition(towerMesh, vertex, position);
            }
        }
        return refreshNodeBounds(ee::Ptr<zge::Node>(display->node));
    }
    READABLE_FUNCTION_WAITS(stretchTowerDisplay, Verified);

    uint32_t editDisplayNode(uint32_t display)
    {
        return ee::Ptr<EditDisplay>(display)->node;
    }
    READABLE_FUNCTION(editDisplayNode, Verified);

    void showEditDisplay(uint32_t display, uint32_t shown)
    {
        const uint32_t node = ee::Ptr<EditDisplay>(display)->node;
        if (node != 0)
            { game::tail(zge::setNodeShown, ee::Ptr<zge::Node>(node), shown, 1); return; }
    }
    READABLE_FUNCTION_WAITS(showEditDisplay, Verified);

    int32_t newEditView()
    {
        const uint32_t address = editAllocate(static_cast<uint32_t>(sizeof(EditView)));
        const ee::Ptr<EditView> view(address);
        view->position.x = view->position.y = view->position.z = ee::Float(0.0f);
        view->angles = zge::EulerAngles{};
        view->set = 0;
        return static_cast<int32_t>(address);
    }
    READABLE_FUNCTION_WAITS(newEditView, Verified);

    void freeEditView(uint32_t view)
    {
        game::tail(editFree, view);
    }
    READABLE_FUNCTION_WAITS(freeEditView, Verified);

    void saveEditView(uint32_t view, uint32_t catalog)
    {
        parkStreamWrite(catalog, view, 4);
        parkStreamWrite(catalog, view + 4, 4);
        parkStreamWrite(catalog, view + 8, 4);
        parkStreamWrite(catalog, view + offsetof(EditView, angles), 2);
        parkStreamWrite(catalog, view + offsetof(EditView, angles) + 2, 2);
        game::tail(parkStreamWrite, catalog, view + offsetof(EditView, angles) + 4, 2);
    }
    READABLE_FUNCTION(saveEditView, Verified);

    void loadEditView(uint32_t view, uint32_t catalog)
    {
        parkStreamRead(catalog, view, 4);
        parkStreamRead(catalog, view + 4, 4);
        parkStreamRead(catalog, view + 8, 4);
        parkStreamRead(catalog, view + offsetof(EditView, angles), 2);
        parkStreamRead(catalog, view + offsetof(EditView, angles) + 2, 2);
        game::tail(parkStreamRead, catalog, view + offsetof(EditView, angles) + 4, 2);
    }
    READABLE_FUNCTION(loadEditView, Verified);
}
