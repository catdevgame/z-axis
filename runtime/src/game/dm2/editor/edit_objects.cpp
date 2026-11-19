// DM2's park editor objects (edit_objects.h): the catalog of
// assets\editor\object.txt, the held object and the placed objects. DM2's
// own code.

#include "edit_objects.h"
#include "edit_core.h"
#include "rail_tool.h"
#include "park_grid.h"
#include "edit_pools.h"
#include "edit_text.h"
#include "edit_lights.h"
#include "edit_gui.h"
#include "edit_grid.h"
#include "edit_display.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"

#include <cstdint>
#include "sym/dm2/editor/edit_objects.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The catalog: the definitions by index, how many, the pool they come
        // from (an index in the editor's pool table), and the categories:
        // how many, their NAMEs and each one's objects in the file's order.
        inline ee::Global<ee::Ptr<ee::Ptr<EditObjectDef>>, sym::g_editObjects> g_editObjects;
        inline ee::Global<uint8_t, sym::g_editObjectCount> g_editObjectCount;
        inline ee::Global<int8_t, sym::g_editObjectPool> g_editObjectPool;
        inline ee::Global<uint8_t, sym::g_editCategoryCount> g_editCategoryCount;
        struct EditCategoryName
        {
            char text[0x32];
        };
        constexpr ee::Ptr<EditCategoryName> kCategoryNames{sym::g_editCategoryNames};
        constexpr ee::Ptr<zge::List> kCategoryObjects{sym::g_editCategoryObjects};
        // An object definition from its link on a category's list (0 at the list's end).
        ee::Ptr<EditObjectDef> objectDefOf(ee::Ptr<zge::ListNode> node) { return zge::recordOfNode<EditObjectDef>(node); }

        // The held object: whether it is shown, its node, its index in the
        // catalog and its quarter turns.
        inline ee::Global<uint8_t, sym::g_heldObjectShown> g_heldObjectShown;
        inline ee::Global<ee::Ptr<zge::Node>, sym::g_heldObjectNode> g_heldObjectNode;
        inline ee::Global<uint8_t, sym::g_heldObject> g_heldObject;
        inline ee::Global<uint8_t, sym::g_heldRotation> g_heldRotation;

        // Scaffolding (unnamed-field): the secret level the player has
        // reached, as the catalog reads it: an object whose SECRET is above
        // it is shown as the secret stand-in and cannot be placed. Who sets
        // it is not this file's.
        inline ee::Global<uint8_t, sym::g_secretLevelUnlocked> g_secretLevelUnlocked;
        inline ee::Global<ee::Float, sym::g_parkCapacityUsed> g_parkCapacityUsed;

        // object.txt's words.
        constexpr ee::Ptr<const char> kOpen{sym::objectFileOpen};             // "{"
        constexpr ee::Ptr<const char> kClose{sym::objectFileClose};           // "}"
        constexpr ee::Ptr<const char> kName{sym::objectFileName};             // "NAME"
        constexpr ee::Ptr<const char> kLights{sym::objectFileLights};         // "LIGHTS"
        constexpr ee::Ptr<const char> kEnd{sym::objectFileEnd};               // "END"
        constexpr ee::Ptr<const char> kCat{sym::objectFileCat};               // "CAT"
        constexpr ee::Ptr<const char> kHash{sym::objectFileHash};             // "HASH"
        constexpr ee::Ptr<const char> kNameSuffix{sym::objectFileNameSuffix}; // "_name"
        constexpr ee::Ptr<const char> kHelpSuffix{sym::objectFileHelpSuffix}; // "_help"
        constexpr ee::Ptr<const char> kType{sym::objectFileType};             // "TYPE"
        constexpr ee::Ptr<const char> kRail{sym::objectFileRail};             // "RAIL"
        constexpr ee::Ptr<const char> kX{sym::objectFileX};                   // "X"
        constexpr ee::Ptr<const char> kY{sym::objectFileY};                   // "Y"
        constexpr ee::Ptr<const char> kCx{sym::objectFileCx};                 // "CX"
        constexpr ee::Ptr<const char> kCy{sym::objectFileCy};                 // "CY"
        constexpr ee::Ptr<const char> kSecret{sym::objectFileSecret};         // "SECRET"
        constexpr ee::Ptr<const char> kCorners{sym::objectFileCorners};       // "CORNERS"
        constexpr ee::Ptr<const char> kEdges{sym::objectFileEdges};           // "EDGES"
        constexpr ee::Ptr<const char> kObject{sym::objectFileObject};         // "OBJECT"
        constexpr ee::Ptr<const char> kObjects{sym::objectFileObjects};       // "OBJECTS"
        constexpr ee::Ptr<const char> kCatagories{sym::objectFileCatagories}; // "CATAGORIES"
        constexpr ee::Ptr<const char> kRails{sym::objectFileRails};           // "RAILS"
        constexpr ee::Ptr<const char> kObjectFilePath{sym::objectFilePath};   // "assets\editor\object.txt"
        constexpr ee::Ptr<const char> kSecretObject{sym::secretObjectName};   // "Editobj_secret"
        constexpr ee::Ptr<const char> kSecretNameKey{sym::secretNameKey};     // "KSTR_PARKSECRETNAME"
        constexpr ee::Ptr<const char> kSecretHelpKey{sym::secretHelpKey};     // "KSTR_PARKSECRETHELP"

        constexpr uint32_t kTokenBytes = 0x100;
        constexpr uint32_t kEditObjectPoolRecords = 100;
        // A quarter turn, in the degrees setNodeHeading takes.
        constexpr ee::Float kQuarterTurn = 90.0f;

        // A record from the editor's pools (an index in its pool table).
        ee::Ptr<EditObjectDef> newEditObjectDef(int8_t pool) { return ee::Ptr<EditObjectDef>(editPoolAllocate(pool)); }
        // Scaffolding (unnamed-field): the piece category whose held object
        // is drawn opaque; which of object.txt's categories is the
        // fifteenth is not read.
        constexpr uint8_t kOpaqueCategory = 14;

        // Nodes of the editor's world objects.
        ee::Ptr<zge::Node> newWorldObjectNode(uint32_t nameHash) { return newWorldObjectInstance(nameHash); }

        // The cursor and the cells.
        void cellPosition(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row, ee::Ptr<zge::Vec4> out) { cellWorldPosition(editor, column, row, out); }
        // An empty function of the editor core.
        // The top header's Selection and the bottom header's Help texts from
        // the held object (edit_gui).
        // The cosine of the editor's frame count times `rate`.

        ee::Ptr<EditObjectDef> editObject(uint8_t index)
        {
            return (*g_editObjects)[index];
        }

        // An unsigned number as a float, the way the compiler wrote it.
        ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1) | (value >> 1)));
            return half + half;
        }

        // A pulsing colour byte: the editor's pulse scaled about a middle value.
        uint8_t pulseByte(ee::Float swing, ee::Float middle)
        {
            return static_cast<uint8_t>(ee::toInt(pulseCosine(0x200) * swing + middle));
        }

    }

    void loadEditObjects()
    {
        *g_editObjectPool = newEditPool(sizeof(EditObjectDef), kEditObjectPoolRecords);
        readEditObjectFile();
        *g_heldObjectNode = ee::Ptr<zge::Node>();
    }
    READABLE_FUNCTION(loadEditObjects, Verified);

    void freeEditObjects()
    {
        hideHeldObject();
        zge::heapFree(*g_editObjects);
        *g_editObjects = ee::Ptr<ee::Ptr<EditObjectDef>>();
    }
    READABLE_FUNCTION(freeEditObjects, Verified);

    void updateHeldObject(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;
        if (!*g_heldObjectShown || !*g_heldObjectNode || editor->tool == kEditToolTerrain)
            return;
        cursorWorldPosition(editor, position);
        setNodePosition(*g_heldObjectNode, position);
        if (heldObjectFits(editor))
            setCursorState(editor, kCursorFree);
        else
            setCursorState(editor, kCursorBlocked);
    }
    READABLE_FUNCTION(updateHeldObject, Verified);

    void showHeldObject(uint8_t object)
    {
        *g_heldObjectShown = 1;
        const ee::Ptr<zge::Node> node = newHeldObjectNode(object);
        *g_heldObject = object;
        *g_heldObjectNode = node;
        setNodeHeading(node, ee::toFloat(*g_heldRotation) * kQuarterTurn);
        // The original hands the node and 2.0 to an empty function.
        unusedNodeSetting();
        game::tail(showHeldObjectTexts);
    }
    READABLE_FUNCTION(showHeldObject, Verified);

    void hideHeldObject()
    {
        *g_heldObjectShown = 0;
        if (*g_heldObjectNode)
        {
            releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>>(sym::g_heldObjectNode));
            *g_heldObjectNode = ee::Ptr<zge::Node>();
        }
    }
    READABLE_FUNCTION(hideHeldObject, Verified);

    // A kept bug: the new node is asked for by the low byte of the object's
    // model id, not by the object's index. Nothing calls it.
    void holdObjectByModelByte(uint8_t object)
    {
        if (*g_heldObjectNode)
            releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>>(sym::g_heldObjectNode));
        const ee::Ptr<zge::Node> node = newHeldObjectNode(static_cast<uint8_t>(editObject(object)->model));
        *g_heldObject = object;
        *g_heldObjectNode = node;
    }
    READABLE_FUNCTION(holdObjectByModelByte, Draft);

    void rotateHeldObject()
    {
        *g_heldRotation = static_cast<uint8_t>((*g_heldRotation + 1) & 3);
        game::tail(setNodeHeading, *g_heldObjectNode, ee::toFloat(*g_heldRotation) * kQuarterTurn);
    }
    READABLE_FUNCTION(rotateHeldObject, Verified);

    // The next object of the held one's category, the first after the last.
    ee::Ptr<EditObjectDef> holdNextObject()
    {
        const ee::Ptr<EditObjectDef> held = editObject(*g_heldObject);
        ee::Ptr<EditObjectDef> next = objectDefOf(held->link.next);
        if (!next)
            next = objectDefOf(kCategoryObjects[held->category].head);
        holdObject(next->index);
        showHeldObjectTexts();
        return next;
    }
    READABLE_FUNCTION(holdNextObject, Verified);

    ee::Ptr<EditObjectDef> holdPreviousObject()
    {
        const ee::Ptr<EditObjectDef> held = editObject(*g_heldObject);
        ee::Ptr<EditObjectDef> previous = objectDefOf(held->link.prev);
        if (!previous)
            previous = objectDefOf(kCategoryObjects[held->category].tail);
        holdObject(previous->index);
        showHeldObjectTexts();
        return previous;
    }
    READABLE_FUNCTION(holdPreviousObject, Verified);

    // The first object of the category after the held one's, of the first
    // category after the last.
    void holdNextCategory()
    {
        uint32_t category = (editObject(*g_heldObject)->category + 1u) & 0xFF;
        if (!(category < *g_editCategoryCount))
            category = 0;
        holdObject(objectDefOf(kCategoryObjects[category].head)->index);
        game::tail(showHeldObjectTexts);
    }
    READABLE_FUNCTION(holdNextCategory, Draft);

    // `object` held, or the category's first object when it is 0; nothing
    // when the category is empty.
    void holdCategoryObject(uint8_t category, ee::Ptr<EditObjectDef> object)
    {
        if (!object)
            object = objectDefOf(kCategoryObjects[category].head);
        if (!object)
            return;
        holdObject(object->index);
        game::tail(showHeldObjectTexts);
    }
    READABLE_FUNCTION(holdCategoryObject, Verified);

    void placeHeldObject(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<uint8_t> column;
        ee::Local<uint8_t> row;
        ee::Local<ee::Ptr<PlacedObject>> placed;

        *placed = ee::Ptr<PlacedObject>();
        if (*g_secretLevelUnlocked < editObject(*g_heldObject)->secret)
            return;
        if (refuseWhenParkFull(*g_heldObject))
            return;
        if (!heldObjectFits(editor))
            return;
        cursorCell(editor, column, row);
        newPlacedObject(editor, placed, *column, *row, *g_heldObject, *g_heldRotation);
        cursorWorldPosition(editor, position);
        setNodePosition((*placed)->node, position);
        setNodeHeading((*placed)->node, ee::toFloat((*placed)->rotation) * kQuarterTurn);
        // The original hands the node and 0.5 to an empty function.
        unusedNodeSetting();
        addNodeLights((*placed)->node);
        lightNodeVertices((*placed)->node, 1);
        const uint32_t cost = placedObjectCost(*placed);
        *g_parkCapacityUsed = *g_parkCapacityUsed + unsignedToFloat(cost);
    }
    READABLE_FUNCTION(placeHeldObject, Verified);

    // The held object's colour: pulsing white where it may be placed, red
    // where it is refused; its alpha pulses too, except under choice 14.
    void colourHeldObject(ee::Ptr<ParkEditor> editor, uint8_t refused)
    {
        ee::Local<zge::VertexColour> colour;
        static_cast<void>(editor);
        if (refused)
        {
            colour->blue = 0x20;
            colour->red = 0xFF;
            colour->green = 0x20;
            if (currentToolCategory() == kOpaqueCategory)
                colour->alpha = 0xFF;
            else
                colour->alpha = pulseByte(40.0f, 100.0f);
            setNodeColour(*g_heldObjectNode, colour);
            return;
        }
        colour->red = pulseByte(60.0f, 190.0f);
        colour->green = pulseByte(60.0f, 190.0f);
        colour->blue = 0xFF;
        if (currentToolCategory() == kOpaqueCategory)
            colour->alpha = 0xFF;
        else
            colour->alpha = pulseByte(60.0f, 190.0f);
        setNodeColour(*g_heldObjectNode, colour);
    }
    READABLE_FUNCTION(colourHeldObject, Verified);

    // The object's node and record given up, its cells left as they are.
    void freePlacedObject(ee::Ptr<ee::Ptr<PlacedObject>> slot)
    {
        const ee::Ptr<PlacedObject> object = *slot;
        releaseEditNode(&object->node);
        zge::heapFree(object);
        *slot = ee::Ptr<PlacedObject>();
    }
    READABLE_FUNCTION(freePlacedObject, Verified);

    void removePlacedObject(ee::Ptr<ParkEditor> editor, ee::Ptr<ee::Ptr<PlacedObject>> slot)
    {
        ee::Local<CellBox> box;
        const ee::Ptr<PlacedObject> object = *slot;
        placedObjectCellBox(object, box);
        for (uint32_t row = static_cast<uint8_t>(box->firstRow); !(box->lastRow < static_cast<int32_t>(row)); row = (row + 1) & 0xFF)
        {
            for (uint32_t column = static_cast<uint8_t>(box->firstColumn); !(box->lastColumn < static_cast<int32_t>(column));
                 column = (column + 1) & 0xFF)
            {
                if (cellInPark(editor, static_cast<int8_t>(column), static_cast<int8_t>(row)))
                {
                    parkCell(editor, column, row)->object = ee::Ptr<PlacedObject>();
                    updateCellRails(editor, column, row);
                }
            }
        }
        removeNodeLights(object->node);
        releaseEditNode(&object->node);
        zge::heapFree(object);
        *slot = ee::Ptr<PlacedObject>();
    }
    READABLE_FUNCTION(removePlacedObject, Verified);

    void newPlacedObject(ee::Ptr<ParkEditor> editor, ee::Ptr<ee::Ptr<PlacedObject>> out, uint8_t column, uint8_t row,
                         uint8_t object, uint8_t rotation)
    {
        ee::Local<CellBox> box;
        const ee::Ptr<PlacedObject> placed(zge::heapAllocate(sizeof(PlacedObject)));
        *out = placed;
        placed->object = object;
        const ee::Ptr<zge::Node> node = newWorldObjectNode(editObject(object)->model);
        placed->rotation = rotation;
        placed->column = column;
        placed->row = row;
        placed->unused = 0;
        placed->rails = ee::Ptr<CellRails>();
        placed->flags = 0;
        placed->node = node;
        placedObjectCellBox(placed, box);
        for (uint32_t r = static_cast<uint8_t>(box->firstRow); !(box->lastRow < static_cast<int32_t>(r)); r = (r + 1) & 0xFF)
        {
            for (uint32_t c = static_cast<uint8_t>(box->firstColumn); !(box->lastColumn < static_cast<int32_t>(c)); c = (c + 1) & 0xFF)
            {
                if (cellInPark(editor, static_cast<int8_t>(c), static_cast<int8_t>(r)))
                {
                    parkCell(editor, c, r)->object = placed;
                    updateCellRails(editor, c, r);
                }
            }
        }
    }
    READABLE_FUNCTION(newPlacedObject, Verified);

    int32_t heldObjectSpanX()
    {
        return editObjectSpanX(*g_heldObject, *g_heldRotation);
    }
    READABLE_FUNCTION(heldObjectSpanX, Verified);

    int32_t heldObjectSpanY()
    {
        return editObjectSpanY(*g_heldObject, *g_heldRotation);
    }
    READABLE_FUNCTION(heldObjectSpanY, Verified);

    int32_t heldObjectCentreX()
    {
        return editObjectCentreX(*g_heldObject, *g_heldRotation);
    }
    READABLE_FUNCTION(heldObjectCentreX, Verified);

    int32_t heldObjectCentreY()
    {
        return editObjectCentreY(*g_heldObject, *g_heldRotation);
    }
    READABLE_FUNCTION(heldObjectCentreY, Verified);

    ee::Ptr<const char> heldObjectName()
    {
        return editObjectName(*g_heldObject);
    }
    READABLE_FUNCTION(heldObjectName, Verified);

    ee::Ptr<const char> heldObjectHelp()
    {
        return editObjectHelp(*g_heldObject);
    }
    READABLE_FUNCTION(heldObjectHelp, Verified);

    uint8_t heldObject()
    {
        return *g_heldObject;
    }
    READABLE_FUNCTION(heldObject, Draft);

    int32_t editObjectSpanX(uint8_t object, uint8_t rotation)
    {
        switch (rotation)
        {
        case 0: return editObject(object)->columns;
        case 1: return editObject(object)->rows;
        case 2: return static_cast<int8_t>(-editObject(object)->columns);
        case 3: return static_cast<int8_t>(-editObject(object)->rows);
        default: return 1;
        }
    }
    READABLE_FUNCTION(editObjectSpanX, Verified);

    int32_t editObjectSpanY(uint8_t object, uint8_t rotation)
    {
        switch (rotation)
        {
        case 0: return editObject(object)->rows;
        case 1: return static_cast<int8_t>(-editObject(object)->columns);
        case 2: return static_cast<int8_t>(-editObject(object)->rows);
        case 3: return editObject(object)->columns;
        default: return 1;
        }
    }
    READABLE_FUNCTION(editObjectSpanY, Verified);

    int32_t editObjectCentreX(uint8_t object, uint8_t rotation)
    {
        switch (rotation)
        {
        case 0: return editObject(object)->centreColumn;
        case 1: return editObject(object)->centreRow;
        case 2: return static_cast<int8_t>(-editObject(object)->centreColumn);
        case 3: return static_cast<int8_t>(-editObject(object)->centreRow);
        default: return 0;
        }
    }
    READABLE_FUNCTION(editObjectCentreX, Verified);

    int32_t editObjectCentreY(uint8_t object, uint8_t rotation)
    {
        switch (rotation)
        {
        case 0: return editObject(object)->centreRow;
        case 1: return static_cast<int8_t>(-editObject(object)->centreColumn);
        case 2: return static_cast<int8_t>(-editObject(object)->centreRow);
        case 3: return editObject(object)->centreColumn;
        default: return 0;
        }
    }
    READABLE_FUNCTION(editObjectCentreY, Verified);

    ee::Ptr<const char> editObjectName(uint8_t object)
    {
        const ee::Ptr<EditObjectDef> def = editObject(object);
        if (*g_secretLevelUnlocked < def->secret)
            return editorText(zge::nameHash(kSecretNameKey));
        return editorText(def->nameKey);
    }
    READABLE_FUNCTION(editObjectName, Verified);

    ee::Ptr<const char> editObjectHelp(uint8_t object)
    {
        const ee::Ptr<EditObjectDef> def = editObject(object);
        if (*g_secretLevelUnlocked < def->secret)
            return editorText(zge::nameHash(kSecretHelpKey));
        return editorText(def->helpKey);
    }
    READABLE_FUNCTION(editObjectHelp, Verified);

    uint8_t editObjectCategory(uint8_t object)
    {
        return editObject(object)->category;
    }
    READABLE_FUNCTION(editObjectCategory, Draft);

    uint8_t editObjectRailGroup(uint8_t object)
    {
        return editObject(object)->railGroup;
    }
    READABLE_FUNCTION(editObjectRailGroup, Verified);

    uint8_t editObjectCorner(uint8_t object, uint8_t corner)
    {
        return editObject(object)->corners[corner];
    }
    READABLE_FUNCTION(editObjectCorner, Verified);

    // The edge's bit of EDGES, in place (not 0 or 1).
    uint32_t editObjectEdge(uint8_t object, uint32_t edge)
    {
        return editObject(object)->edges & (1u << (edge & 31));
    }
    READABLE_FUNCTION(editObjectEdge, Draft);

    ee::Ptr<zge::Node> shownHeldObjectNode()
    {
        if (!*g_heldObjectShown)
            return ee::Ptr<zge::Node>();
        return *g_heldObjectNode;
    }
    READABLE_FUNCTION(shownHeldObjectNode, Draft);

    void movePlacedObjectToCell(ee::Ptr<ParkEditor> editor, ee::Ptr<PlacedObject> object)
    {
        ee::Local<zge::Vec4> position;
        cellPosition(editor, object->column, object->row, position);
        setNodePosition(object->node, position);
    }
    READABLE_FUNCTION(movePlacedObjectToCell, Verified);

    // The cursor's box stretched over every cell of the object; 1 when it grew.
    uint32_t stretchCursorOverObject(ee::Ptr<ParkEditor> editor, ee::Ptr<PlacedObject> object)
    {
        ee::Local<CellBox> box;
        uint32_t grew = 0;
        placedObjectCellBox(object, box);
        for (uint32_t column = static_cast<uint8_t>(box->firstColumn); !(box->lastColumn < static_cast<int32_t>(column));
             column = (column + 1) & 0xFF)
        {
            for (uint32_t row = static_cast<uint8_t>(box->firstRow); !(box->lastRow < static_cast<int32_t>(row)); row = (row + 1) & 0xFF)
            {
                if (stretchCursorToCell(editor, column, row))
                    grew = 1;
            }
        }
        return grew;
    }
    READABLE_FUNCTION(stretchCursorOverObject, Verified);

    // A node of the object's world model, or of the secret stand-in while the
    // object is still secret. The object becomes the held one's index.
    ee::Ptr<zge::Node> newHeldObjectNode(uint8_t object)
    {
        *g_heldObject = object;
        const ee::Ptr<EditObjectDef> def = editObject(object);
        if (*g_secretLevelUnlocked < def->secret)
            return newWorldObjectNode(zge::nameHash(kSecretObject));
        return newWorldObjectNode(def->model);
    }
    READABLE_FUNCTION(newHeldObjectNode, Verified);

    // Another object held in place of the one shown, turned as that one was.
    void holdObject(uint8_t object)
    {
        if (*g_heldObjectNode)
            releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>>(sym::g_heldObjectNode));
        const ee::Ptr<zge::Node> node = newHeldObjectNode(object);
        *g_heldObject = object;
        *g_heldObjectNode = node;
        setNodeHeading(node, ee::toFloat(*g_heldRotation) * kQuarterTurn);
        heldObjectChanged(*g_heldObject, *g_heldRotation);
        game::tail(showHeldObjectTexts);
    }
    READABLE_FUNCTION(holdObject, Verified);

    uint32_t heldObjectFits(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<CellBox> box;
        int32_t height = -1; // none seen yet
        cursorCellBox(editor, box);
        for (int32_t row = static_cast<int16_t>(box->firstRow); !(box->lastRow < row); row = static_cast<int16_t>(row + 1))
        {
            for (int32_t column = static_cast<int16_t>(box->firstColumn); !(box->lastColumn < column);
                 column = static_cast<int16_t>(column + 1))
            {
                if (!cellCanBeBuiltOn(editor, static_cast<int8_t>(column), static_cast<int8_t>(row)))
                    return 0;
                const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
                if (cell->object)
                    return 0;
                if (height == -1)
                    height = cell->level;
                else if (height != cell->level)
                    return 0;
                if (cellHasRail(editor, column & 0xFF, row & 0xFF))
                    return 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(heldObjectFits, Verified);

    // One CAT block: its NAME kept, and the category added to the editor
    // core's choices, flagged unless it is the lights'.
    void readCategoryDef(uint8_t index)
    {
        ee::Local<char[kTokenBytes]> token;
        zge::nextToken(token, kTokenBytes);
        if (zge::compareStrings(kOpen, token) != 0)
            return;
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kClose, token) != 0)
        {
            if (zge::compareStrings(kName, token) == 0)
            {
                zge::nextToken(token, kTokenBytes);
                const ee::Ptr<EditCategoryName> name = kCategoryNames + index;
                zge::stringCopy(name, token);
                zge::nextToken(token, kTokenBytes);
                const int32_t differs = zge::compareStrings(name, kLights);
                addEditToolEntry(kEditToolPiece, index, ee::Ptr<const char>(name->text), differs != 0);
            }
            else
            {
                zge::nextToken(token, kTokenBytes);
            }
        }
    }
    READABLE_FUNCTION(readCategoryDef, Verified);

    void readCategoryList()
    {
        ee::Local<char[kTokenBytes]> token;
        *g_editCategoryCount = 0;
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kEnd, token) != 0)
        {
            if (zge::compareStrings(kCat, token) == 0)
            {
                readCategoryDef(*g_editCategoryCount);
                *g_editCategoryCount = static_cast<uint8_t>(*g_editCategoryCount + 1);
            }
            zge::nextToken(token, kTokenBytes);
        }
    }
    READABLE_FUNCTION(readCategoryList, Verified);

    uint32_t findEditCategory(ee::Ptr<const char> name)
    {
        for (int32_t index = 0; index < *g_editCategoryCount; ++index)
        {
            if (zge::compareStrings(kCategoryNames + index, name) == 0)
                return static_cast<uint32_t>(index) & 0xFF;
        }
        return kNoEditCategory;
    }
    READABLE_FUNCTION(findEditCategory, Verified);

    // One OBJECT block into a new definition; 0 when no block follows.
    ee::Ptr<EditObjectDef> readEditObject()
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<char[kTokenBytes]> key;
        ee::Local<int32_t> number;

        zge::nextToken(token, kTokenBytes);
        if (zge::compareStrings(kOpen, token) != 0)
            return ee::Ptr<EditObjectDef>();
        const ee::Ptr<EditObjectDef> def = newEditObjectDef(*g_editObjectPool);
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kClose, token) != 0)
        {
            if (zge::compareStrings(kHash, token) == 0)
            {
                zge::nextToken(token, kTokenBytes);
                def->model = zge::nameHash(token);
                zge::stringCopy(key, token);
                zge::appendString(key, kNameSuffix);
                def->nameKey = zge::nameHash(key);
                zge::stringCopy(key, token);
                zge::appendString(key, kHelpSuffix);
                def->helpKey = zge::nameHash(key);
            }
            else if (zge::compareStrings(kCat, token) == 0)
            {
                zge::nextToken(token, kTokenBytes);
                def->category = static_cast<uint8_t>(findEditCategory(token));
            }
            else if (zge::compareStrings(kType, token) == 0)
            {
                // TYPE's word is read and not used: the place holds the
                // object's index once the list is built.
                zge::nextToken(token, kTokenBytes);
                def->index = 0;
            }
            else if (zge::compareStrings(kRail, token) == 0)
            {
                zge::nextToken(token, kTokenBytes);
                def->railGroup = railTypeIndex(zge::nameHash(token));
            }
            else if (zge::compareStrings(kX, token) == 0)
            {
                zge::readNumberToken(number);
                def->columns = static_cast<int8_t>(*number);
            }
            else if (zge::compareStrings(kY, token) == 0)
            {
                zge::readNumberToken(number);
                def->rows = static_cast<int8_t>(*number);
            }
            else if (zge::compareStrings(kCx, token) == 0)
            {
                zge::readNumberToken(number);
                def->centreColumn = static_cast<int8_t>(*number);
            }
            else if (zge::compareStrings(kCy, token) == 0)
            {
                zge::readNumberToken(number);
                def->centreRow = static_cast<int8_t>(*number);
            }
            else if (zge::compareStrings(kSecret, token) == 0)
            {
                zge::readNumberToken(number);
                def->secret = static_cast<int8_t>(*number);
            }
            else if (zge::compareStrings(kCorners, token) == 0)
            {
                for (int32_t corner = 0; corner < 4; ++corner)
                {
                    zge::readNumberToken(number);
                    def->corners[corner] = static_cast<uint8_t>(*number);
                }
            }
            else if (zge::compareStrings(kEdges, token) == 0)
            {
                for (uint32_t edge = 0; edge < 4; ++edge)
                {
                    zge::readNumberToken(number);
                    if (*number != 0)
                        def->edges = static_cast<uint8_t>(def->edges | (1u << edge));
                }
            }
            zge::nextToken(token, kTokenBytes);
        }
        return def;
    }
    READABLE_FUNCTION(readEditObject, Verified);

    // The OBJECTS block: the definitions numbered in the file's order, the
    // catalog's array made, and each one put on its category's list.
    void readEditObjectList()
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<zge::List> read;

        *g_editObjectCount = 0;
        zge::initList(read);
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kEnd, token) != 0)
        {
            if (zge::compareStrings(kObject, token) == 0)
            {
                const ee::Ptr<EditObjectDef> def = readEditObject();
                def->index = *g_editObjectCount;
                zge::listPush(read, def);
                *g_editObjectCount = static_cast<uint8_t>(*g_editObjectCount + 1);
            }
            zge::nextToken(token, kTokenBytes);
        }
        for (uint32_t category = 0; category < kEditCategoryMax; ++category)
            zge::initList(kCategoryObjects + category);
        *g_editObjects = ee::Ptr<ee::Ptr<EditObjectDef>>(zge::heapAllocate(static_cast<uint32_t>(*g_editObjectCount) << 2));
        int32_t index = 0;
        for (ee::Ptr<EditObjectDef> def = objectDefOf(read->head); def;)
        {
            def->index = static_cast<uint8_t>(index);
            (*g_editObjects)[index] = def;
            ++index;
            const ee::Ptr<EditObjectDef> next = objectDefOf(def->link.next);
            zge::listRemove(read, def);
            if (def->category != kNoEditCategory)
                zge::listPush(kCategoryObjects + def->category, def);
            def = next;
        }
    }
    READABLE_FUNCTION(readEditObjectList, Verified);

    // object.txt read whole into memory and parsed as a script: its OBJECTS,
    // CATAGORIES and RAILS blocks up to END. A kept bug: any other word ends
    // the reading at once, with the script, the file and the memory left open.
    void readEditObjectFile()
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<uint32_t> bytes;

        const ee::Ptr<zge::ZFile> file(zge::openFile(kObjectFilePath, 1, 0).value);
        zge::fileSize(file, bytes, 0);
        const uint32_t text = zge::heapAllocate(*bytes);
        zge::fileRead(file, text, *bytes, 0);
        zge::closeFile(file, 0);
        const ee::Ptr<zge::ZFile> script = zge::openMemoryFile(text, *bytes);
        zge::beginScript(zge::filePath(script));
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kEnd, token) != 0)
        {
            if (zge::compareStrings(kObjects, token) == 0)
                readEditObjectList();
            else if (zge::compareStrings(kCatagories, token) == 0)
                readCategoryList();
            else if (zge::compareStrings(kRails, token) == 0)
                readRailList();
            else
                return;
            zge::nextToken(token, kTokenBytes);
        }
        zge::endScript();
        zge::closeFile(script, 0);
        zge::heapFree(text);
    }
    READABLE_FUNCTION(readEditObjectFile, Verified);

    void savePlacedObject(ee::Address stream, ee::Ptr<PlacedObject> object)
    {
        parkStreamWrite(stream, &object->object, 1);
        parkStreamWrite(stream, &object->unused, 1);
        parkStreamWrite(stream, &object->rotation, 1);
        parkStreamWrite(stream, &object->column, 1);
        game::tail(parkStreamWrite, stream, &object->row, 1);
    }
    READABLE_FUNCTION(savePlacedObject, Verified);

    void loadPlacedObject(ee::Ptr<ParkEditor> editor, ee::Address stream, ee::Ptr<ee::Ptr<PlacedObject>> out)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<uint8_t> object;
        ee::Local<uint8_t> savedUnused; // PlacedObject::unused, read and dropped: a new object's is 0
        ee::Local<uint8_t> rotation;
        ee::Local<uint8_t> column;
        ee::Local<uint8_t> row;

        parkStreamRead(stream, object, 1);
        parkStreamRead(stream, savedUnused, 1);
        parkStreamRead(stream, rotation, 1);
        parkStreamRead(stream, column, 1);
        parkStreamRead(stream, row, 1);
        newPlacedObject(editor, out, *column, *row, *object, *rotation);
        cellPosition(editor, *column, *row, position);
        setNodePosition((*out)->node, position);
        setNodeHeading((*out)->node, ee::toFloat((*out)->rotation) * kQuarterTurn);
        addNodeLights((*out)->node);
        const uint32_t cost = placedObjectCost(*out);
        *g_parkCapacityUsed = *g_parkCapacityUsed + unsignedToFloat(cost);
    }
    READABLE_FUNCTION(loadPlacedObject, Verified);
}
