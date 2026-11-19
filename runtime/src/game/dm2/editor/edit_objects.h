// DM2's park editor objects: the catalog read from assets\editor\object.txt
// (the OBJECT records and their CATAGORIES), the object held at the cursor,
// and the objects placed in the park's cells. DM2's own code, linked after
// ./src/com/editor/editGui.c.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // One OBJECT of object.txt (0x24 bytes of the catalog's pool). The file's
    // keywords name the fields: HASH (the world object's name; "<HASH>_name"
    // and "<HASH>_help" are its string table keys), CAT, RAIL, X and Y (the
    // cells it covers), CX and CY (where its origin's cell is within them),
    // SECRET, CORNERS and EDGES.
    struct EditObjectDef
    {
        zge::ListNode link;  // +0x00 its category's list
        uint32_t nameKey;    // +0x08 nameHash("<HASH>_name")
        uint32_t model;      // +0x0C nameHash(HASH): the world object's name hash (findWorldObjectByHash)
        uint32_t helpKey;    // +0x10 nameHash("<HASH>_help")
        uint8_t category;    // +0x14 CAT's index, kNoEditCategory when the name is unknown
        uint8_t index;       // +0x15 its place in the catalog
        uint8_t railGroup;       // +0x16 RAIL's rail type
        uint8_t edges;       // +0x17 EDGES: one bit for each of the four numbers that is not 0
        int8_t columns;      // +0x18 X
        int8_t rows;         // +0x19 Y
        int8_t centreColumn; // +0x1A CX
        int8_t centreRow;    // +0x1B CY
        int8_t secret;       // +0x1C SECRET: the secret level that shows it
        uint8_t corners[4];  // +0x1D CORNERS
        uint8_t _21[3];
    };
    static_assert(offsetof(EditObjectDef, category) == 0x14 && offsetof(EditObjectDef, corners) == 0x1D);
    static_assert(sizeof(EditObjectDef) == 0x24);

    constexpr uint32_t kEditCategoryMax = 25;
    constexpr uint8_t kNoEditCategory = 25;

    // The catalog read (the pool of definitions made first) and given up.
    void loadEditObjects();
    void freeEditObjects();

    // The catalog's file and its blocks.
    void readEditObjectFile();
    void readCategoryList();
    void readCategoryDef(uint8_t index);
    void readEditObjectList();
    ee::Ptr<EditObjectDef> readEditObject();
    // The category of that NAME, kNoEditCategory when there is none.
    uint32_t findEditCategory(ee::Ptr<const char> name);

    // An object's definition by its index in the catalog. The spans are the
    // cells it covers along the park's columns and rows when turned by
    // `rotation` quarter turns (negative: towards lower numbers); the offsets
    // are its origin's place within them, turned the same way.
    int32_t editObjectSpanX(uint8_t object, uint8_t rotation);
    int32_t editObjectSpanY(uint8_t object, uint8_t rotation);
    int32_t editObjectCentreX(uint8_t object, uint8_t rotation);
    int32_t editObjectCentreY(uint8_t object, uint8_t rotation);
    uint8_t editObjectCategory(uint8_t object);
    uint8_t editObjectRailGroup(uint8_t object);
    uint8_t editObjectCorner(uint8_t object, uint8_t corner);
    uint32_t editObjectEdge(uint8_t object, uint32_t edge);
    // The string table's texts of its name and help; a still secret object
    // has the secret's.
    ee::Ptr<const char> editObjectName(uint8_t object);
    ee::Ptr<const char> editObjectHelp(uint8_t object);

    // The held object: the one shown at the cursor, ready to be placed.
    uint8_t heldObject();
    ee::Ptr<zge::Node> shownHeldObjectNode();
    ee::Ptr<const char> heldObjectName();
    ee::Ptr<const char> heldObjectHelp();
    int32_t heldObjectSpanX();
    int32_t heldObjectSpanY();
    int32_t heldObjectCentreX();
    int32_t heldObjectCentreY();
    ee::Ptr<zge::Node> newHeldObjectNode(uint8_t object);
    void showHeldObject(uint8_t object);
    void hideHeldObject();
    void holdObject(uint8_t object);
    void holdObjectByModelByte(uint8_t object);
    void rotateHeldObject();
    ee::Ptr<EditObjectDef> holdNextObject();
    ee::Ptr<EditObjectDef> holdPreviousObject();
    void holdNextCategory();
    void holdCategoryObject(uint8_t category, ee::Ptr<EditObjectDef> object);
    // The held object follows the cursor, and the cursor's state says whether
    // it fits there: every cell under it can be built on, is empty, at one
    // height and has no rail.
    void updateHeldObject(ee::Ptr<ParkEditor> editor);
    uint32_t heldObjectFits(ee::Ptr<ParkEditor> editor);
    void colourHeldObject(ee::Ptr<ParkEditor> editor, uint8_t refused);
    void placeHeldObject(ee::Ptr<ParkEditor> editor);

    // Placed objects: made on the cells they cover, taken off them, moved to
    // their cell, and written to and read from a saved park.
    void newPlacedObject(ee::Ptr<ParkEditor> editor, ee::Ptr<ee::Ptr<PlacedObject>> out, uint8_t column, uint8_t row,
                         uint8_t object, uint8_t rotation);
    void removePlacedObject(ee::Ptr<ParkEditor> editor, ee::Ptr<ee::Ptr<PlacedObject>> slot);
    void freePlacedObject(ee::Ptr<ee::Ptr<PlacedObject>> slot);
    void movePlacedObjectToCell(ee::Ptr<ParkEditor> editor, ee::Ptr<PlacedObject> object);
    uint32_t stretchCursorOverObject(ee::Ptr<ParkEditor> editor, ee::Ptr<PlacedObject> object);
    void savePlacedObject(ee::Address stream, ee::Ptr<PlacedObject> object);
    void loadPlacedObject(ee::Ptr<ParkEditor> editor, ee::Address stream, ee::Ptr<ee::Ptr<PlacedObject>> out);
}
