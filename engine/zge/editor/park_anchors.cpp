// The park editor's tools (the base part every tool class has) and anchors. An
// anchor is an edit object a piece can be attached to: one sits on each cell,
// and on a piece each of its "EditAnchor" locators makes one, listing the pieces
// that may sit there. Anchors carry position and angles, work out their matrix
// through an owner's, and pass messages on to what they hold.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/angle.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/render/draw.h"
#include "zge/render/primitives.h"
#include "zge/editor/edit_core.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_messages.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/editor/park_anchors.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kMatrixTranslation = 3 * sizeof(zge::Vec4); // a matrix's fourth row
        constexpr uint32_t kListPush = sym::listPush;           // (list, node) to the front
        constexpr uint32_t kMovedMessage = sym::makeMovedMessage;

        constexpr uint32_t kConstructEditTool = sym::constructEditTool;
        constexpr uint32_t kDestructEditTool = sym::destructEditTool;
        constexpr uint32_t kEditToolMessage = sym::editToolMessage;
        constexpr uint32_t kConstructEditAnchor = sym::constructEditAnchor;
        constexpr uint32_t kDestructEditAnchor = sym::destructEditAnchor;
        constexpr uint32_t kEditAnchorMessage = sym::editAnchorMessage;

        constexpr uint32_t kEditToolName = sym::g_editToolName;
        constexpr uint32_t kEditToolBaseName = sym::g_editToolEditBaseClassName;
        constexpr uint32_t kEditAnchorName = sym::g_editAnchorName;
        constexpr uint32_t kEditAnchorBaseName = sym::g_editAnchorEditBaseClassName;

        constexpr ee::Float kCellMetres = 4.572f;
        constexpr ee::Float kDegreesToAngle = 182.04445f;     // 65536 / 360

        inline ee::Global<uint32_t, sym::g_uiDefaultMaterial> g_uiDefaultMaterial;

        // The next in a list whose first word links them.
        uint32_t linkAfter(uint32_t link)
        {
            return *ee::Ptr<uint32_t>(link);
        }

        ee::Ptr<EditAnchorPart> anchorPart(uint32_t anchor)
        {
            return ee::Ptr<EditAnchorPart>(anchor);
        }

        // The vertices on the stack take the colour word, solid, no UVs.
        void paintVertices(const ee::Ptr<Vertex> first, uint32_t colour, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const ee::Ptr<Vertex> vertex = first + i;
                std::memcpy(vertex->colour, &colour, sizeof(vertex->colour));
                vertex->colour[3] = 255;
                vertex->u = 0;
                vertex->v = 0;
            }
        }
    }

    void registerEditToolClass()
    {
        game::tail(registerEditClass, kEditToolName, static_cast<uint32_t>(sizeof(EditToolPart)), kEditToolBaseName, kConstructEditTool, kDestructEditTool, kEditToolMessage);
    }
    READABLE_FUNCTION(registerEditToolClass, Verified);

    namespace
    {
        // A tool's method, if it has one. `after` is the block the original joins
        // at. The tool and what it works with are named parameters though the
        // original only leaves them in place for the method: a function that hands
        // its arguments on through a pointer shows none of them, and a caller
        // reaching it as C++ passes what its signature says.
        int32_t callToolMethod(uint32_t tool, uint32_t ui, uint32_t method, uint32_t returnAddress, uint32_t after)
        {
            int32_t result = static_cast<int32_t>(method);
            if (method != 0)
                result = ee::call<int32_t>(method, tool, ui);
            return result;
        }
    }

    int32_t deactivateEditTool(uint32_t tool, uint32_t ui)
    {
        namespace here = points::deactivateEditTool;
        return callToolMethod(tool, ui, ee::Ptr<EditToolMethods>(ee::Ptr<EditToolPart>(tool)->methods)->deactivate, here::next7, here::done);
    }
    READABLE_FUNCTION(deactivateEditTool, Verified);

    int32_t activateEditTool(uint32_t tool, uint32_t ui)
    {
        namespace here = points::activateEditTool;
        return callToolMethod(tool, ui, ee::Ptr<EditToolMethods>(ee::Ptr<EditToolPart>(tool)->methods)->activate, here::next7, here::done);
    }
    READABLE_FUNCTION(activateEditTool, Verified);

    int32_t constructEditTool(uint32_t data)
    {
        const uint32_t address = editObjectPart(data, kEditToolName);
        const ee::Ptr<EditToolPart> part(address);
        part->word08 = 1;
        part->iconChar = 'A';
        part->editCamera = {};
        part->toolSet = 0;
        part->methods = 0;
        part->selection = 0;
        return static_cast<int32_t>(address);
    }
    READABLE_FUNCTION(constructEditTool, Verified);

    void destructEditTool()
    {
    }
    READABLE_FUNCTION(destructEditTool, Verified);

    void editToolMessage()
    {
    }
    READABLE_FUNCTION(editToolMessage, Verified);

    // updateEditTool: the method takes the frame's seconds, which the original leaves
    // in place for it, so they are a parameter here.
    void updateEditTool(uint32_t tool, ee::Float seconds)
    {
        const ee::Ptr<EditToolMethods> methods(ee::Ptr<EditToolPart>(tool)->methods);
        if (methods->update)
            methods->update(tool, seconds);
    }
    READABLE_FUNCTION(updateEditTool, Verified);

    void drawEditTool(uint32_t tool)
    {
        const ee::Ptr<EditToolMethods> methods(ee::Ptr<EditToolPart>(tool)->methods);
        if (methods->draw)
            methods->draw(tool);
    }
    READABLE_FUNCTION(drawEditTool, Verified);

    void setEditToolMethods(ee::Ptr<EditToolPart> part, uint32_t methods)
    {
        part->methods = methods;
    }
    READABLE_FUNCTION(setEditToolMethods, Verified);

    uint32_t setEditToolName(ee::Address part, uint32_t name)
    {
        return game::tail(zge::stringCopy, part + static_cast<uint32_t>(offsetof(EditToolPart, name)), name);
    }
    READABLE_FUNCTION(setEditToolName, Verified);

    void bindEditTool(uint32_t address, ee::Ptr<EditToolSet> toolSet, uint32_t selection, ee::Ptr<EditCamera> camera)
    {
        const ee::Ptr<EditToolPart> part(address);
        part->selection = selection;
        part->toolSet = toolSet;
        part->editCamera = camera;
    }
    READABLE_FUNCTION(bindEditTool, Verified);

    // headingStep: one cell along the axis a heading is nearest to: how the
    // tools' cursors move with the camera turned.
    void headingStep(ee::Ptr<zge::Vec3> step, ee::Float heading)
    {
        const ee::Float cell = kCellMetres;
        const zge::Angle angle = static_cast<zge::Angle>(static_cast<int16_t>(ee::toInt(heading * kDegreesToAngle)));
        const ee::Float cosine = zge::angleCos(angle);
        step->y = ee::Float(0.0f);
        step->x = cosine * cell;
        step->z = zge::angleSin(angle) * cell;
        if (ee::abs(step->z) < ee::abs(step->x))
            step->z = ee::Float(0.0f);
        else
            step->x = ee::Float(0.0f);
    }
    READABLE_FUNCTION(headingStep, Verified);

    uint32_t newEditList()
    {
        const uint32_t list = editAllocate(12u);
        zge::initList(list);
        return list;
    }
    READABLE_FUNCTION_WAITS(newEditList, Verified);

    // newPieceAnchors: a placed piece's own anchors, copies of its definition's.
    uint32_t newPieceAnchors(uint32_t definitions)
    {
        const uint32_t list = editAllocate(12u);
        zge::initList(list);
        for (uint32_t definition = linkAfter(definitions); definition != 0; definition = linkAfter(definition))
            zge::listPush(list, newEditObject(kEditAnchorName, definition));
        return list;
    }
    READABLE_FUNCTION_WAITS(newPieceAnchors, Verified);

    void deleteAnchorList(uint32_t list)
    {
        for (uint32_t anchor = linkAfter(list); anchor != 0; anchor = linkAfter(list))
        {
            zge::listRemove(ee::Ptr<zge::List>(list), ee::Ptr<zge::ListNode>(anchor));
            deleteEditObject(anchor);
        }
    }
    READABLE_FUNCTION_WAITS(deleteAnchorList, Verified);

    int32_t freeAnchorList(uint32_t list)
    {
        return game::tail(editFree, list);
    }
    READABLE_FUNCTION_WAITS(freeAnchorList, Verified);

    int32_t setAnchorOwner(uint32_t anchor, ee::Ptr<EditObjectPart> owner);

    void setAnchorListOwner(uint32_t list, ee::Ptr<EditObjectPart> owner)
    {
        for (uint32_t anchor = linkAfter(list); anchor != 0; anchor = linkAfter(anchor))
            setAnchorOwner(anchor, owner);
    }
    READABLE_FUNCTION_WAITS(setAnchorListOwner, Verified);

    namespace
    {
        namespace tail
        {
        int32_t listPush(uint32_t list, uint32_t node) { return ee::tailCall<int32_t>(kListPush, list, node); }
        }
    }

    int32_t appendAnchor(uint32_t list, uint32_t anchor)
    {
        return tail::listPush(list, anchor);
    }
    READABLE_FUNCTION(appendAnchor, Verified);

    uint32_t firstAnchor(uint32_t list)
    {
        return linkAfter(list);
    }
    READABLE_FUNCTION(firstAnchor, Verified);

    uint32_t nextAnchor(uint32_t anchor)
    {
        return linkAfter(anchor);
    }
    READABLE_FUNCTION(nextAnchor, Verified);

    void registerEditAnchorClass()
    {
        game::tail(registerEditClass, kEditAnchorName, static_cast<uint32_t>(sizeof(EditAnchorPart)), kEditAnchorBaseName, kConstructEditAnchor, kDestructEditAnchor, kEditAnchorMessage);
    }
    READABLE_FUNCTION(registerEditAnchorClass, Verified);

    uint32_t newAcceptList();

    // constructEditAnchor: a new anchor with its accept list and identity matrix, or a
    // copy of a definition sharing the definition's list.
    void constructEditAnchor(uint32_t data, uint32_t definition)
    {
        const uint32_t address = editObjectPart(data, kEditAnchorName);
        const ee::Ptr<EditAnchorPart> part(address);
        if (definition == 0)
        {
            part->copied = 0;
            const uint32_t accepts = newAcceptList();
            part->attached = 0;
            part->owner = 0;
            part->angles.z = 0;
            part->angles.y = 0;
            part->angles.x = 0;
            part->accepts = accepts;
            part->position.x = ee::Float(0.0f);
            part->position.z = ee::Float(0.0f);
            part->position.y = ee::Float(0.0f);
            game::tail(zge::matrixIdentity, ee::Ptr<zge::Matrix>(address + offsetof(EditAnchorPart, matrix)));
            return;
        }
        const ee::Ptr<EditAnchorPart> source(definition);
        part->copied = 1;
        part->accepts = source->accepts;
        part->position = source->position;
        part->angles = source->angles;
        part->matrix = source->matrix;
    }
    READABLE_FUNCTION_WAITS(constructEditAnchor, Verified);

    int32_t freeAcceptList(uint32_t list);

    int32_t destructEditAnchor(uint32_t data)
    {
        const ee::Ptr<EditAnchorPart> part(editObjectPart(data, kEditAnchorName));
        const int32_t result = static_cast<int32_t>(part->copied);
        if (result == 0)
            return game::tail(freeAcceptList, part->accepts);
        return result;
    }
    READABLE_FUNCTION_WAITS(destructEditAnchor, Verified);

    // editAnchorMessage: a message to an anchor goes on to what sits on it, unless
    // that is only reserved; then to the message's own handler.
    int32_t editAnchorMessage(uint32_t data, uint32_t message)
    {
        const uint32_t address = editObjectPart(data, kEditAnchorName);
        const ee::Ptr<EditAnchorPart> part(address);
        if (part->attached == 0 || part->reserved == 0)
        {
            if (part->attached != 0)
            {
                const uint32_t sender = messageSender(message);
                queueEditMessageTarget(sender, part->attached);
            }
        }
        return game::tail(runMessageAnchorHandler, message, address);
    }
    READABLE_FUNCTION_WAITS(editAnchorMessage, Verified);

    // setAnchorPosition: the new position, and every part told it moved.
    int32_t setAnchorPosition(uint32_t anchor, ee::Address position)
    {
        anchorPart(anchor)->position = *ee::Ptr<zge::Vec4>(position & ~0xFu);
        return game::tail(broadcastEditMessage, anchor, kMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(setAnchorPosition, Verified);

    int32_t setAnchorAngles(uint32_t anchor, ee::Address angles)
    {
        anchorPart(anchor)->angles = *ee::Ptr<const zge::EulerAngles>(angles);
        return game::tail(broadcastEditMessage, anchor, kMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(setAnchorAngles, Verified);

    uint32_t anchorAcceptList(uint32_t anchor)
    {
        return anchorPart(anchor)->accepts;
    }
    READABLE_FUNCTION(anchorAcceptList, Verified);

    uint32_t newAcceptList()
    {
        const uint32_t list = editAllocate(12u);
        zge::initList(list);
        return list;
    }
    READABLE_FUNCTION_WAITS(newAcceptList, Verified);

    int32_t freeAcceptList(uint32_t list)
    {
        for (uint32_t entry = linkAfter(list); entry != 0;)
        {
            const uint32_t next = ee::Ptr<AcceptEntry>(entry)->next;
            zge::listRemove(ee::Ptr<zge::List>(list), ee::Ptr<zge::ListNode>(entry));
            editFree(entry);
            entry = next;
        }
        return game::tail(editFree, list);
    }
    READABLE_FUNCTION_WAITS(freeAcceptList, Verified);

    int32_t addAcceptEntry(uint32_t list, uint32_t piece)
    {
        const uint32_t entry = editAllocate(static_cast<uint32_t>(sizeof(AcceptEntry)));
        ee::Ptr<AcceptEntry>(entry)->piece = piece;
        return tail::listPush(list, entry);
    }
    READABLE_FUNCTION_WAITS(addAcceptEntry, Verified);

    uint32_t acceptListFirst(uint32_t list)
    {
        return linkAfter(list);
    }
    READABLE_FUNCTION(acceptListFirst, Verified);

    uint32_t acceptEntryNext(uint32_t entry)
    {
        return ee::Ptr<AcceptEntry>(entry)->next;
    }
    READABLE_FUNCTION(acceptEntryNext, Verified);

    uint32_t acceptEntryValue(uint32_t entry)
    {
        return ee::Ptr<AcceptEntry>(entry)->piece;
    }
    READABLE_FUNCTION(acceptEntryValue, Verified);

    // attachToAnchor: a part put on an anchor, moved to it.
    int32_t attachToAnchor(uint32_t anchor, ee::Ptr<EditObjectPart> part)
    {
        anchorPart(anchor)->attached = part;
        setEditObjectParentAnchor(part, anchor);
        const int32_t result = broadcastEditMessage(part, kMovedMessage, 0);
        anchorPart(anchor)->reserved = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(attachToAnchor, Verified);

    int32_t reserveAnchor(uint32_t anchor, ee::Ptr<EditObjectPart> part)
    {
        anchorPart(anchor)->attached = part;
        anchorPart(anchor)->reserved = 1;
        return 1;
    }
    READABLE_FUNCTION(reserveAnchor, Verified);

    ee::Ptr<EditObjectPart> detachFromAnchor(uint32_t anchor)
    {
        const ee::Ptr<EditObjectPart> part = anchorPart(anchor)->attached;
        if (part)
            setEditObjectParentAnchor(part, 0);
        anchorPart(anchor)->attached = {};
        return part;
    }
    READABLE_FUNCTION(detachFromAnchor, Verified);

    int32_t setAnchorOwner(uint32_t anchor, ee::Ptr<EditObjectPart> owner)
    {
        anchorPart(anchor)->owner = owner;
        return game::tail(broadcastEditMessage, anchor, kMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(setAnchorOwner, Verified);

    ee::Ptr<EditObjectPart> anchorOwner(uint32_t anchor)
    {
        return anchorPart(anchor)->owner;
    }
    READABLE_FUNCTION(anchorOwner, Verified);

    ee::Ptr<EditObjectPart> anchorAttachedObject(uint32_t anchor)
    {
        return anchorPart(anchor)->attached;
    }
    READABLE_FUNCTION(anchorAttachedObject, Verified);

    // updateAnchorMatrix: the anchor's angles and position as a matrix, through the
    // owner's if it has one. Returns the anchor's matrix.
    ee::Ptr<zge::Matrix> updateAnchorMatrix(uint32_t address)
    {
        ee::Local<zge::Matrix> local;
        const ee::Ptr<EditAnchorPart> part(address);
        zge::matrixIdentity(local);
        zge::matrixRotateXYZ(local, local, &part->angles);
        local->translation() = part->position;
        if (part->owner != 0)
            return zge::matrixMultiply(&part->matrix, editObjectMatrix(part->owner), local);
        part->matrix = *local;
        return &part->matrix;
    }
    READABLE_FUNCTION(updateAnchorMatrix, Verified);

    int32_t deleteAnchor(uint32_t anchor)
    {
        return game::tail(deleteEditObject, anchor);
    }
    READABLE_FUNCTION_WAITS(deleteAnchor, Verified);

    // rotateAnchor: the anchor turned, and every part told it moved.
    int32_t rotateAnchor(uint32_t anchor, uint32_t turn)
    {
        const ee::Ptr<EditAnchorPart> part = anchorPart(anchor);
        const ee::Ptr<const zge::EulerAngles> by(turn);
        part->angles.x = static_cast<zge::Angle>(part->angles.x + by->x);
        part->angles.y = static_cast<zge::Angle>(part->angles.y + by->y);
        part->angles.z = static_cast<zge::Angle>(part->angles.z + by->z);
        return game::tail(broadcastEditMessage, anchor, kMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(rotateAnchor, Verified);

    int32_t anchorWorldPosition(uint32_t anchor)
    {
        return static_cast<int32_t>(anchor + offsetof(EditAnchorPart, matrix) + kMatrixTranslation);
    }
    READABLE_FUNCTION(anchorWorldPosition, Verified);

    int32_t anchorMatrix(uint32_t anchor)
    {
        return static_cast<int32_t>(anchor + offsetof(EditAnchorPart, matrix));
    }
    READABLE_FUNCTION(anchorMatrix, Verified);

    int32_t anchorAngles(uint32_t anchor)
    {
        return static_cast<int32_t>(anchor + offsetof(EditAnchorPart, angles));
    }
    READABLE_FUNCTION(anchorAngles, Verified);

    uint32_t anchorReserved(uint32_t anchor)
    {
        return anchorPart(anchor)->reserved;
    }
    READABLE_FUNCTION(anchorReserved, Verified);

    int32_t addAnchorAccept(uint32_t anchor, uint32_t piece)
    {
        return game::tail(addAcceptEntry, anchorPart(anchor)->accepts, piece);
    }
    READABLE_FUNCTION_WAITS(addAnchorAccept, Verified);

    // anchorAccepts: whether an object's piece is on the anchor's list.
    int32_t anchorAccepts(uint32_t anchor, ee::Ptr<EditObjectPart> object)
    {
        const uint32_t def = editObjectDef(object);
        const uint32_t piece = pieceDefIndex(def);
        for (uint32_t entry = linkAfter(anchorPart(anchor)->accepts); entry != 0;)
        {
            const ee::Ptr<AcceptEntry> current(entry);
            entry = current->next;
            if (current->piece == piece)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(anchorAccepts, Verified);

    int32_t anchorHoldsPiece(uint32_t anchor)
    {
        const ee::Ptr<EditAnchorPart> part = anchorPart(anchor);
        int32_t result = 0;
        if (part->attached != 0)
            result = part->reserved == 0 ? 1 : 0;
        return result;
    }
    READABLE_FUNCTION(anchorHoldsPiece, Verified);

    namespace
    {
        constexpr uint32_t kBoxCorners = 8;

        // Corners 0-3 are a box's top, high y; 4-7 the bottom, under them.
        void setBoxCorners(ee::Ptr<Vertex> corners, uint32_t address)
        {
            const ee::Ptr<zge::Box> box(address);
            const ee::Float lowX = box->low.x, lowY = box->low.y, lowZ = box->low.z;
            const ee::Float highX = box->high.x, highY = box->high.y, highZ = box->high.z;
            const auto setCorner = [&](uint32_t index, ee::Float x, ee::Float y, ee::Float z) {
                const ee::Ptr<Vertex> vertex = corners + index;
                vertex->position.x = x;
                vertex->position.y = y;
                vertex->position.z = z;
            };
            setCorner(0, highX, highY, highZ);
            setCorner(1, highX, highY, lowZ);
            setCorner(2, lowX, highY, lowZ);
            setCorner(3, lowX, highY, highZ);
            setCorner(4, highX, lowY, highZ);
            setCorner(5, highX, lowY, lowZ);
            setCorner(6, lowX, lowY, lowZ);
            setCorner(7, lowX, lowY, highZ);
        }

        // The twelve edges in the order both box functions draw them.
        constexpr uint8_t kBoxEdges[12][2] = {{0, 1}, {2, 1}, {2, 3}, {0, 3}, {4, 5}, {6, 5}, {6, 7}, {4, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    }

    namespace
    {
        int32_t drawQuads(ee::Ptr<QuadBatch> batch) { return zge::drawPrimitiveQuads(0, batch, 1, 0); }
    }

    // drawAnchorMarker: half a metre of line down from a point.
    int32_t drawAnchorMarker(ee::Ptr<zge::Vec4> position, uint32_t colour)
    {
        ee::Local<uint32_t> colourWord;
        ee::Local<Vertex[2]> ends;
        ee::Local<LineBatch> line;
        const ee::Ptr<Vertex> first = ends;
        *colourWord = colour;
        line->material = *g_uiDefaultMaterial;
        paintVertices(first, *colourWord, 2);
        ends[0].position = *position;
        ends[1].position = *position;
        ends[1].position.y = ends[1].position.y - ee::Float(0.5f);
        line->ends[0] = first;
        line->ends[1] = first + 1;
        return drawLines(0, ee::Address(line), 1, 0);
    }
    READABLE_FUNCTION_WAITS(drawAnchorMarker, Verified);

    // drawEditBox: a box's twelve edges solid, its six faces in the colour's own alpha.
    int32_t drawEditBox(ee::Address address, uint32_t colour)
    {
        ee::Local<uint32_t> colourWord;
        ee::Local<Vertex[kBoxCorners]> cornerVertices;
        ee::Local<LineBatch> line;
        ee::Local<QuadBatch> quad;
        const ee::Ptr<Vertex> corners = cornerVertices;
        const uint32_t material = *g_uiDefaultMaterial;
        quad->material = material;
        line->material = material;
        *colourWord = colour;
        paintVertices(corners, *colourWord, kBoxCorners);
        setBoxCorners(corners, address);
        for (uint32_t i = 0; i < 12; ++i)
        {
            line->ends[0] = corners + kBoxEdges[i][0];
            line->ends[1] = corners + kBoxEdges[i][1];
            drawLines(0, ee::Address(line), 1, 0);
        }

        // The sides, in the colour's alpha.
        for (uint32_t i = 0; i < kBoxCorners; ++i)
            corners[i].colour[3] = static_cast<uint8_t>(*colourWord >> 24);

        static constexpr uint8_t kFaces[][4] = {{5, 6, 7, 4}, {1, 2, 3, 0}, {1, 0, 4, 5}, {2, 1, 5, 6}, {3, 2, 6, 7}, {0, 3, 7, 4}};
        int32_t result = 0;
        for (const auto &face : kFaces)
        {
            for (uint32_t k = 0; k < 4; ++k)
                quad->corners[k] = corners + face[k];
            result = drawQuads(quad);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(drawEditBox, Verified);

    // drawEditBoxLines: the edges alone.
    int32_t drawEditBoxLines(ee::Address address, uint32_t colour)
    {
        ee::Local<uint32_t> colourWord;
        ee::Local<Vertex[kBoxCorners]> cornerVertices;
        ee::Local<LineBatch> line;
        ee::Local<QuadBatch> quad;
        const ee::Ptr<Vertex> corners = cornerVertices;
        const uint32_t material = *g_uiDefaultMaterial;
        quad->material = material; // as drawEditBox sets it up; no face is drawn here
        line->material = material;
        *colourWord = colour;
        paintVertices(corners, *colourWord, kBoxCorners);
        setBoxCorners(corners, address);
        int32_t result = 0;
        for (const auto &edge : kBoxEdges)
        {
            line->ends[0] = corners + edge[0];
            line->ends[1] = corners + edge[1];
            result = drawLines(0, ee::Address(line), 1, 0);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(drawEditBoxLines, Verified);
}
