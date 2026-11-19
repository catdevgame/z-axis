#pragma once

// The park editor's anchors and the base part of its tools (park_anchors.cpp).

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "zge/container/list.h"
#include "zge/math/angle.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/render/draw.h"
#include "zge/render/primitives.h"
#include <cstddef>
#include <cstring>
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/editor/edit_records.h"
#include "zge/editor/park_camera.h"

#include <cstdint>

namespace zge::editor
{
    // (park_anchors.cpp)
    uint32_t acceptEntryNext(uint32_t entry);
    uint32_t acceptEntryValue(uint32_t entry);
    uint32_t acceptListFirst(uint32_t list);
    int32_t activateEditTool(uint32_t tool, uint32_t ui);
    int32_t addAnchorAccept(uint32_t anchor, uint32_t piece);
    uint32_t anchorAcceptList(uint32_t anchor);
    int32_t anchorAccepts(uint32_t anchor, ee::Ptr<EditObjectPart> object);
    int32_t anchorAngles(uint32_t anchor);
    ee::Ptr<EditObjectPart> anchorAttachedObject(uint32_t anchor);
    int32_t anchorHoldsPiece(uint32_t anchor);
    int32_t anchorMatrix(uint32_t anchor);
    ee::Ptr<EditObjectPart> anchorOwner(uint32_t anchor);
    uint32_t anchorReserved(uint32_t anchor);
    int32_t anchorWorldPosition(uint32_t anchor);
    int32_t appendAnchor(uint32_t list, uint32_t anchor);
    int32_t attachToAnchor(uint32_t anchor, ee::Ptr<EditObjectPart> part);
    void bindEditTool(uint32_t address, ee::Ptr<EditToolSet> toolSet, uint32_t selection, ee::Ptr<EditCamera> camera);
    int32_t deactivateEditTool(uint32_t tool, uint32_t ui);
    int32_t deleteAnchor(uint32_t anchor);
    void deleteAnchorList(uint32_t list);
    ee::Ptr<EditObjectPart> detachFromAnchor(uint32_t anchor);
    int32_t drawAnchorMarker(ee::Ptr<zge::Vec4> position, uint32_t colour);
    int32_t drawEditBox(ee::Address address, uint32_t colour);
    int32_t drawEditBoxLines(ee::Address address, uint32_t colour);
    void drawEditTool(uint32_t tool);
    uint32_t firstAnchor(uint32_t list);
    int32_t freeAnchorList(uint32_t list);
    void headingStep(ee::Ptr<zge::Vec3> step, ee::Float heading);
    uint32_t newEditList();
    uint32_t newPieceAnchors(uint32_t definitions);
    uint32_t nextAnchor(uint32_t anchor);
    void registerEditAnchorClass();
    void registerEditToolClass();
    int32_t reserveAnchor(uint32_t anchor, ee::Ptr<EditObjectPart> part);
    int32_t rotateAnchor(uint32_t anchor, uint32_t turn);
    int32_t setAnchorAngles(uint32_t anchor, ee::Address angles);
    int32_t setAnchorPosition(uint32_t anchor, ee::Address position);
    void setEditToolMethods(ee::Ptr<EditToolPart> part, uint32_t methods);
    uint32_t setEditToolName(ee::Address part, uint32_t name);
    ee::Ptr<zge::Matrix> updateAnchorMatrix(uint32_t address);
    void updateEditTool(uint32_t tool, ee::Float seconds);

    struct EditToolMethods
    {
        uint32_t activate;    // +0x00
        ee::FunctionPtr<void(uint32_t, ee::Float)> update; // +0x04 (tool, seconds)
        ee::FunctionPtr<void(uint32_t)> draw;              // +0x08 (tool)
        uint32_t deactivate;  // +0x0C
    };

    // The "EditAnchor" part of an anchor.
    struct EditAnchorPart
    {
        uint8_t _00[8];
        uint32_t copied;      // +0x08 made from a definition, whose accept list it shares
        uint8_t _0c[4];
        zge::Vec4 position;   // +0x10 local
        zge::EulerAngles angles; // +0x20
        uint8_t _26[0xA];
        zge::Matrix matrix;   // +0x30 its row T, at +0x60, is the world position
        uint32_t accepts;     // +0x70 what may sit on it
        ee::Ptr<EditObjectPart> attached;    // +0x74 the part sitting on it
        ee::Ptr<EditObjectPart> owner;       // +0x78 the piece it is on
#if PS2X_GAME_BMX
        uint32_t nameHash;    // +0x7C the name of the locator it was made from, hashed (the rail places table finds anchors by it)
#endif
        uint32_t reserved;    // +0x7C held, not part of the park (BMX XXX: +0x80)
#if PS2X_GAME_BMX
        uint8_t _84[0xC];
#endif
    };
    static_assert(offsetof(EditAnchorPart, reserved) == game::readable::perGame(0x7Cu, 0x7Cu, 0x80u));
    static_assert(sizeof(EditAnchorPart) == game::readable::perGame(0x80u, 0x80u, 0x90u) && offsetof(EditAnchorPart, matrix) == 0x30 && offsetof(EditAnchorPart, accepts) == 0x70);

    // What an accept list holds: a piece's index in the catalog.
    struct AcceptEntry
    {
        uint32_t next;        // +0x00
        uint32_t _04;
        uint32_t piece;       // +0x08
    };
    static_assert(sizeof(AcceptEntry) == 12);

    // A vertex of the 2D/world primitives.
    struct Vertex
    {
        zge::Vec4 position;   // +0x00
        ee::Float u, v;       // +0x10
        uint8_t colour[4];    // +0x18
        uint32_t _1c;
    };
    static_assert(sizeof(Vertex) == 0x20 && offsetof(Vertex, colour) == 0x18);

    // What drawLines and drawPrimitiveQuads take: a material, then the
    // vertices one line or quad uses.
    struct LineBatch
    {
        uint32_t material;
        ee::Ptr<Vertex> ends[2];
    };
    struct QuadBatch
    {
        uint32_t material;
        ee::Ptr<Vertex> corners[4];
    };

    // (park_anchors.cpp, for the games' own parts)
    void editToolMessage();
    int32_t addAcceptEntry(uint32_t list, uint32_t piece);
    int32_t freeAcceptList(uint32_t list);
    uint32_t newAcceptList();
    int32_t setAnchorOwner(uint32_t anchor, ee::Ptr<EditObjectPart> owner);
    void setAnchorListOwner(uint32_t list, ee::Ptr<EditObjectPart> owner);
}
