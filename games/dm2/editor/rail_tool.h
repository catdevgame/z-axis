// DM2's park editor: the rails. assets\editor\object.txt's RAILS block
// lists the rail TYPES (the looks a rail can have), the rail OBJECTS (one
// world object per type each) and the GROUPS (for a kind of ground or
// object, which rail object stands in each of the eight slots of a cell,
// turned how far and dropped how much). A cell, or the object placed on
// it, holds the rails put on it; the rail tool shows a preview of the next.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    constexpr uint32_t kRailSlotCount = 8; // the places a rail can take on one cell
    constexpr uint8_t kNoRailObject = 0xFE; // a group's slot that takes no rail ("NONE")
    constexpr uint32_t kGroundRailGroup = 1; // the group of a cell with no object on it

    // One rail on a cell.
    struct Rail
    {
        zge::ListNode link;       // +0x00 the cell's rails
        ee::Ptr<zge::Node> node;  // +0x08 its shown node
        uint8_t type;             // +0x0C the rail type it was placed with
        uint8_t slot;             // +0x0D its slot on the cell
        uint8_t _0e[2];
    };
    static_assert(sizeof(Rail) == 0x10 && offsetof(Rail, node) == 0x08 && offsetof(Rail, slot) == 0x0D);

    // The rails of one cell (or of the object placed on it).
    struct CellRails
    {
        uint8_t group;   // +0x00 the rail group they were made from
        uint8_t slots;   // +0x01 a bit per slot taken
        uint8_t _02[2];
        zge::List rails; // +0x04 Rail records
    };
    static_assert(sizeof(CellRails) == 0x10 && offsetof(CellRails, rails) == 0x04);

    // A rail OBJECT of the file: its name and, per rail type, the name of
    // the world object shown for it.
    struct RailObject
    {
        zge::ListNode link;                   // +0x00 the list the reader collects them on
        char name[0x1C];                      // +0x08
        ee::Ptr<ee::Ptr<const char>> models;  // +0x24 [the rail types' count]
    };
    static_assert(sizeof(RailObject) == 0x28 && offsetof(RailObject, models) == 0x24);

    // A GROUP of the file.
    struct RailGroupSlot
    {
        uint8_t _00[8];
        uint8_t object;       // +0x08 the rail object's index, kNoRailObject: none
        uint8_t quarterTurns; // +0x09 its heading, added to the placed object's
        uint8_t drop;         // +0x0A how far below the cell's ground it stands
        uint8_t _0b;
    };
    struct RailGroup
    {
        zge::ListNode link;                 // +0x00
        uint32_t name;                      // +0x08 its name's hash
        RailGroupSlot slots[kRailSlotCount]; // +0x0C
    };
    static_assert(sizeof(RailGroupSlot) == 0x0C && sizeof(RailGroup) == 0x6C);
    static_assert(offsetof(RailGroup, slots) + offsetof(RailGroupSlot, object) == 0x14);

    // The RAILS block read from the token reader's file, and its parts.
    void readRailList();
    void readRailTypes();
    void readRailObjects();
    void readRailGroups();

    // The two record pools made and the first rail type chosen.
    void initRails();
    // What the RAILS block's reader made, freed.
    void freeRailList();

    // Where a cell's rails hang: on its object if it has one, else on it.
    ee::Ptr<ee::Ptr<CellRails>> cellRailsSlot(ee::Ptr<ParkCell> cell);
    uint32_t cellHasRail(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row);
    ee::Ptr<CellRails> newCellRails();
    // Every rail of the cell released, with the cell's record.
    void removeCellRails(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row);

    // Whether the cell's rail group has a rail object for the slot; the
    // slot as the cell's object is turned is kept for the preview.
    uint32_t railSlotOpen(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row, uint32_t slot);

    // The rail tool: the next slot of the cursor's cell that takes a rail,
    // the next and the previous rail type, the type's name, a rail placed
    // at the cursor, the cursor's rails removed, the preview shown again or
    // released.
    void nextRailSlot(ee::Ptr<ParkEditor> editor);
    void nextRailType();
    void previousRailType();
    ee::Ptr<const char> railTypeText();
    void placeRailAtCursor(ee::Ptr<ParkEditor> editor);
    void removeRailsAtCursor(ee::Ptr<ParkEditor> editor);
    void markRailPreviewStale();
    void releaseRailPreview();

    void placeRail(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row, uint32_t type, uint32_t slot);
    void updateRailPreview(ee::Ptr<ParkEditor> editor);
    void updateCellRails(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row);
    // A cell's rails to and from the park's stream.
    void saveCellRails(ee::Ptr<ParkEditor> editor, uint32_t stream, uint32_t column, uint32_t row);
    void loadCellRails(ee::Ptr<ParkEditor> editor, uint32_t stream, uint32_t column, uint32_t row);

    // The file's names looked up: a rail object by name (kNoRailObject for
    // "NONE", the objects' count when there is none), a rail group by its
    // name's hash (the groups' count when there is none).
    uint32_t railObjectIndex(ee::Ptr<const char> name);
    uint32_t railTypeIndex(uint32_t nameHash);
}
