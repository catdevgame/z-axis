// DM2's park editor: the rails (rail_tool.h). DM2's own code, after
// ./src/com/editor/editGui.c in its build.

#include "rail_tool.h"
#include "edit_text.h"
#include "edit_core.h"
#include "edit_pools.h"
#include "edit_objects.h"
#include "edit_grid.h"
#include "edit_gui.h"

#include "edit_display.h"
#include "editor_state.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/dm2/editor/rail_tool.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The editor's pools (by their index) of CellRails and Rail records.
        inline ee::Global<int8_t, sym::g_cellRailsPool> g_cellRailsPool;
        inline ee::Global<int8_t, sym::g_railPool> g_railPool;
        // The RAILS block as read: how many TYPES, OBJECTS and GROUPS, the
        // objects and groups by index, and the types' names (hashes).
        inline ee::Global<uint8_t, sym::g_railTypeCount> g_railTypeCount;
        inline ee::Global<uint8_t, sym::g_railObjectCount> g_railObjectCount;
        inline ee::Global<uint8_t, sym::g_railGroupCount> g_railGroupCount;
        inline ee::Global<ee::Ptr<ee::Ptr<RailObject>>, sym::g_railObjects> g_railObjects;
        inline ee::Global<ee::Ptr<ee::Ptr<RailGroup>>, sym::g_railGroups> g_railGroups;
        constexpr ee::Ptr<uint32_t> kRailTypeNames{sym::g_railTypeNames};
        // The rails placed in the park.
        inline ee::Global<int32_t, sym::g_railCount> g_railCount;
        // The rail tool: the preview's node, the slot and the type the next
        // rail gets, the slot as the cell's object is turned, and whether
        // the preview has to be made again.
        inline ee::Global<ee::Ptr<zge::Node>, sym::g_railPreviewNode> g_railPreviewNode;
        inline ee::Global<uint8_t, sym::g_railSlot> g_railSlot;
        inline ee::Global<uint8_t, sym::g_railGroupSlot> g_railGroupSlot;
        inline ee::Global<uint8_t, sym::g_railPreviewStale> g_railPreviewStale;
        inline ee::Global<uint8_t, sym::g_railType> g_railType;

        // A slot of a cell as its object's quarter turns move it: [turns][slot].
        constexpr ee::Ptr<uint8_t> kRailSlotByRotation{sym::railSlotByRotation};

        constexpr ee::Ptr<const char> kTextNone{sym::textNone}; // "NONE"

        constexpr uint32_t kRailPoolRecords = 100;
        // The tool gives up looking for a slot that takes a rail after this many.
        constexpr uint32_t kSlotTries = 16;

        // Where a cell's rails hang: on the object standing on it, or on the
        // cell itself (cellRailsSlot's answer, for the functions the
        // original wrote it into).
        ee::Ptr<ee::Ptr<CellRails>> railsSlotOf(ee::Ptr<ParkCell> cell)
        {
            if (cell->object)
                return &cell->object->rails;
            return &cell->rails;
        }

        // A cell's rails walked: the first Rail record of the list, the one after a rail (0 at the end).
        ee::Ptr<Rail> railOf(ee::Ptr<zge::ListNode> node) { return zge::recordOfNode<Rail>(node); }
        ee::Ptr<Rail> firstRail(ee::Ptr<CellRails> rails) { return railOf(rails->rails.head); }
        ee::Ptr<Rail> nextRail(ee::Ptr<Rail> rail) { return railOf(rail->link.next); }

        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        inline ee::Global<uint32_t, sym::g_railLimit> g_railLimit;
        inline ee::Global<ee::Ptr<ParkCell>, sym::g_railPreviewCell> g_railPreviewCell;

        constexpr ee::Ptr<const char> kStopFrameTitlePath{sym::textStopFrameTitlePath}; // "ParkEditor.StopFrame.Title"
        constexpr ee::Ptr<const char> kStopFramePath{sym::textStopFramePath};           // "ParkEditor.StopFrame"
        constexpr ee::Ptr<const char> kTextText{sym::textRailText};                     // "Text"
        constexpr ee::Ptr<const char> kTextTooManyRails{sym::textTooManyRails};         // "Too many Rails!"
        constexpr ee::Ptr<const char> kTextInit{sym::textInit};                         // "Init"
        // What zgui's PLAY sends the root to run an ACTION.
        constexpr uint32_t kPlayMessage = 0x8000;

        // What the rail tool tells the cursor: a rail fits here, this slot
        // takes none, the object under the cursor takes no rails at all.
        constexpr uint32_t kCursorRailFits = 0;
        constexpr uint32_t kCursorSlotTakesNone = 1;
        constexpr uint32_t kCursorObjectTakesNone = 2;

        constexpr ee::Float kQuarterTurnDegrees = 90.0f;

        uint8_t slotAsTurned(uint32_t turns, uint32_t slot)
        {
            return kRailSlotByRotation[turns * kRailSlotCount + slot];
        }
        RailGroupSlot &groupSlot(uint32_t group, uint32_t slot)
        {
            return (*g_railGroups)[group]->slots[slot];
        }
        // The world object's name a rail of the group's slot shows for a type.
        ee::Ptr<const char> railModel(uint32_t group, uint32_t slot, uint32_t type)
        {
            return (*g_railObjects)[groupSlot(group, slot).object]->models[type];
        }
    }

    // A rail of `type` put in `slot` of the cell, unless the park has its
    // limit of rails (the stop frame says so), the cell's group has no rail
    // for the slot, or the slot is taken.
    void placeRail(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row, uint32_t type, uint32_t slot)
    {
        column &= 0xFF;
        row &= 0xFF;
        type &= 0xFF;
        slot &= 0xFF;
        const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
        uint32_t turns = 0;
        if (!(uint32_t(*g_railCount) < *g_railLimit))
        {
            const ee::Ptr<zge::zgui::Node> title = zge::zgui::findNode(*g_uiRoot, kStopFrameTitlePath, 0);
            zge::zgui::setVariable(title, kTextText, kTextTooManyRails);
            const ee::Ptr<zge::zgui::Node> frame = zge::zgui::findNode(*g_uiRoot, kStopFramePath, 0);
            const ee::Ptr<zge::zgui::ListedAction> init = widgetAction(frame, kTextInit);
            zge::sendNewMessage(0, *g_uiRoot, kPlayMessage, init, 1);
            markStopFrameOpen();
            return;
        }

        ee::Ptr<ee::Ptr<CellRails>> holder = &cell->rails;
        if (cell->object)
        {
            const uint32_t group = editObjectRailGroup(cell->object->object);
            ee::Ptr<PlacedObject> object = cell->object;
            if (object->rails && object->rails->group != group)
            {
                updateCellRails(editor, column, row);
                object = cell->object;
            }
            turns = object->rotation;
            if (groupSlot(group, slotAsTurned(turns, slot)).object == kNoRailObject)
                return;
            holder = &object->rails;
        }
        ee::Ptr<CellRails> rails = *holder;
        if (!rails)
        {
            rails = newCellRails();
            *holder = rails;
            if (cell->object)
                rails->group = uint8_t(editObjectRailGroup(cell->object->object));
        }
        if ((rails->slots >> (slot & 31)) & 1)
            return;
        rails->slots = uint8_t(rails->slots | (1u << (slot & 31)));
        const uint32_t turned = slotAsTurned(turns, slot);

        const ee::Ptr<Rail> rail(editPoolAllocate(*g_railPool));
        rail->slot = uint8_t(slot);
        rail->type = uint8_t(type);
        rail->node = newWorldObjectInstance(zge::nameHash(railModel(rails->group, turned, type)));
        ++*g_railCount;
        zge::listPush(&rails->rails, rail);
        setNodeHeading(rail->node, ee::toFloat(int32_t(groupSlot(rails->group, turned).quarterTurns + turns)) * kQuarterTurnDegrees);

        ee::Local<zge::Vec4> position;
        cellWorldPosition(editor, column, row, position);
        position->y = position->y - ee::toFloat(int32_t(groupSlot(rails->group, turned).drop));
        setNodePosition(rail->node, position);
    }
    READABLE_FUNCTION(placeRail, Verified);

    // The rail tool's preview: made again when the cursor's cell or the
    // tool's choice changed. On an object the tool first moves on to a slot
    // the object's group has a rail for.
    void updateRailPreview(ee::Ptr<ParkEditor> editor)
    {
        uint32_t turns = 0;
        uint32_t group = kGroundRailGroup;
        const ee::Ptr<ParkCell> cell = parkCell(editor, editor->cursorColumn, editor->cursorRow);
        if (!*g_railPreviewStale && cell == *g_railPreviewCell)
            return;
        if (*g_railPreviewNode)
            releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>>(sym::g_railPreviewNode));
        if (cell->object)
        {
            group = editObjectRailGroup(cell->object->object);
            turns = cell->object->rotation;
            for (uint32_t tries = 1; !railSlotOpen(editor, cell->column, cell->row, *g_railSlot) && tries < kSlotTries; ++tries)
            {
                ++*g_railSlot;
                if (*g_railSlot >= kRailSlotCount)
                    *g_railSlot = 0;
            }
        }
        *g_railGroupSlot = slotAsTurned(turns, *g_railSlot);
        if (groupSlot(group, *g_railGroupSlot).object != kNoRailObject)
        {
            setCursorState(editor, kCursorRailFits);
            *g_railPreviewNode = newWorldObjectInstance(zge::nameHash(railModel(group, *g_railGroupSlot, *g_railType)));
            setNodeHeading(*g_railPreviewNode, ee::toFloat(int32_t(groupSlot(group, *g_railGroupSlot).quarterTurns + turns)) * kQuarterTurnDegrees);
        }
        else
        {
            setCursorState(editor, group == 0 ? kCursorObjectTakesNone : kCursorSlotTakesNone);
            *g_railPreviewNode = ee::Ptr<zge::Node>();
        }
        *g_railPreviewStale = 0;
        *g_railPreviewCell = ee::Ptr<ParkCell>();
        if (cell)
        {
            ee::Local<zge::Vec4> position;
            cursorWorldPosition(editor, position);
            if (cell->object)
                group = editObjectRailGroup(cell->object->object);
            position->y = position->y - ee::toFloat(int32_t(groupSlot(group, *g_railGroupSlot).drop));
            setNodePosition(*g_railPreviewNode, position);
            *g_railPreviewCell = cell;
        }
    }
    READABLE_FUNCTION(updateRailPreview, Verified);

    // The cell's rails after its object changed: put back in place, and,
    // when the object's rail group is another, shown again. Two things the
    // original does are kept: a rail shown again takes its world object from
    // the slot the rail before it had (slot 0 for the first), and from the
    // group the rails had before, which is only replaced at the end.
    void updateCellRails(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row)
    {
        ee::Local<zge::Vec4> position;
        // The original keeps the row and the object's turns in its frame.
        ee::Local<uint32_t> keptRow;
        ee::Local<uint32_t> turns;
        *keptRow = row & 0xFF;
        column &= 0xFF;
        const ee::Ptr<ParkCell> cell = parkCell(editor, column, *keptRow);
        *turns = 0;
        uint32_t turned = 0;
        uint32_t group = kGroundRailGroup;
        if (cell->object)
        {
            group = editObjectRailGroup(cell->object->object);
            *turns = cell->object->rotation;
        }
        const ee::Ptr<CellRails> rails = cell->object ? cell->object->rails : cell->rails;
        if (!rails)
            return;
        for (ee::Ptr<Rail> rail = firstRail(rails); rail;)
        {
            const ee::Ptr<Rail> next = nextRail(rail);
            if (group != rails->group)
            {
                releaseEditNode(&rail->node);
                rail->node = newWorldObjectInstance(zge::nameHash(railModel(rails->group, turned, rail->type)));
                turned = slotAsTurned(*turns, rail->slot);
                setNodeHeading(rail->node, ee::toFloat(int32_t(groupSlot(rails->group, turned).quarterTurns + *turns)) * kQuarterTurnDegrees);
            }
            else
            {
                turned = slotAsTurned(*turns, rail->slot);
            }
            cellWorldPosition(editor, column, *keptRow, position);
            position->y = position->y - ee::toFloat(int32_t(groupSlot(rails->group, turned).drop));
            setNodePosition(rail->node, position);
            rail = next;
        }
        rails->group = uint8_t(group);
    }
    READABLE_FUNCTION(updateCellRails, Verified);

    // A cell's rails written to the park's stream: a byte that says whether
    // there are any, then the group, the slots taken and each rail's type
    // and slot. A cell an object covers from another column and another row
    // writes none (the object's own cell does); the original's test lets a
    // cell in the object's column or row write them too, and that is kept.
    void saveCellRails(ee::Ptr<ParkEditor> editor, uint32_t stream, uint32_t column, uint32_t row)
    {
        column &= 0xFF;
        row &= 0xFF;
        const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
        ee::Local<uint8_t> hasRails;
        const ee::Ptr<CellRails> rails = *cellRailsSlot(cell);
        const ee::Ptr<PlacedObject> object = cell->object;
        if (object && object->column != column && object->row != row)
        {
            *hasRails = 0;
            parkStreamWrite(stream, hasRails, 1);
            return;
        }
        if (!rails)
        {
            *hasRails = 0;
            parkStreamWrite(stream, hasRails, 1);
            return;
        }
        *hasRails = 1;
        parkStreamWrite(stream, hasRails, 1);
        parkStreamWrite(stream, &rails->group, 1);
        parkStreamWrite(stream, &rails->slots, 1);
        for (ee::Ptr<Rail> rail = firstRail(rails); rail;)
        {
            const ee::Ptr<Rail> next = nextRail(rail);
            parkStreamWrite(stream, &rail->type, 1);
            parkStreamWrite(stream, &rail->slot, 1);
            rail = next;
        }
    }
    READABLE_FUNCTION(saveCellRails, Verified);

    // A cell's rails read back and placed.
    void loadCellRails(ee::Ptr<ParkEditor> editor, uint32_t stream, uint32_t column, uint32_t row)
    {
        column &= 0xFF;
        row &= 0xFF;
        const ee::Ptr<ee::Ptr<CellRails>> holder = cellRailsSlot(parkCell(editor, column, row));
        ee::Local<uint8_t> hasRails;
        ee::Local<uint8_t> slots;
        ee::Local<uint8_t> type;
        ee::Local<uint8_t> slot;
        parkStreamRead(stream, hasRails, 1);
        if (!*hasRails)
            return;
        const ee::Ptr<CellRails> rails = newCellRails();
        *holder = rails;
        parkStreamRead(stream, &rails->group, 1);
        parkStreamRead(stream, slots, 1);
        for (int32_t i = 0; i < int32_t(kRailSlotCount); ++i)
        {
            if ((*slots >> i) & 1)
            {
                parkStreamRead(stream, type, 1);
                parkStreamRead(stream, slot, 1);
                placeRail(editor, column, row, *type, *slot);
            }
        }
    }
    READABLE_FUNCTION(loadCellRails, Verified);

    void initRails()
    {
        *g_cellRailsPool = newEditPool(sizeof(CellRails), kRailPoolRecords);
        *g_railPool = newEditPool(sizeof(Rail), kRailPoolRecords);
        *g_railType = 0;
    }
    READABLE_FUNCTION(initRails, Verified);

    void freeRailList()
    {
        for (int32_t i = 0; i < int32_t(*g_railGroupCount); ++i)
            zge::heapFree((*g_railGroups)[i]);
        zge::heapFree(*g_railGroups);
        *g_railGroups = ee::Ptr<ee::Ptr<RailGroup>>();
        *g_railGroupCount = 0;
        for (int32_t i = 0; i < int32_t(*g_railObjectCount); ++i)
        {
            const ee::Ptr<RailObject> object = (*g_railObjects)[i];
            for (int32_t type = 0; type < int32_t(*g_railTypeCount); ++type)
                zge::heapFree(object->models[type]);
            zge::heapFree(object->models);
            zge::heapFree(object);
        }
        zge::heapFree(*g_railObjects);
        *g_railObjects = ee::Ptr<ee::Ptr<RailObject>>();
        *g_railObjectCount = 0;
    }
    READABLE_FUNCTION(freeRailList, Verified);

    void removeCellRails(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row)
    {
        const ee::Ptr<ee::Ptr<CellRails>> slot = railsSlotOf(parkCell(editor, column & 0xFF, row & 0xFF));
        if (!*slot)
            return;
        ee::Local<ee::Ptr<Rail>> rail;
        *rail = firstRail(*slot);
        while (*rail)
        {
            const ee::Ptr<Rail> next = nextRail(*rail);
            if ((*rail)->node)
                releaseEditNode(&(*rail)->node);
            --*g_railCount;
            editPoolFree(*g_railPool, rail);
            *rail = next;
        }
        editPoolFree(*g_cellRailsPool, slot);
    }
    READABLE_FUNCTION(removeCellRails, Verified);

    uint32_t cellHasRail(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row)
    {
        return *railsSlotOf(parkCell(editor, column & 0xFF, row & 0xFF)) ? 1 : 0;
    }
    READABLE_FUNCTION(cellHasRail, Verified);

    uint32_t railSlotOpen(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row, uint32_t slot)
    {
        const ee::Ptr<ParkCell> cell = parkCell(editor, column & 0xFF, row & 0xFF);
        uint32_t group = kGroundRailGroup;
        uint32_t turns = 0;
        if (cell->object)
        {
            group = editObjectRailGroup(cell->object->object);
            turns = cell->object->rotation;
        }
        *g_railGroupSlot = kRailSlotByRotation[turns * kRailSlotCount + (slot & 0xFF)];
        const ee::Ptr<RailGroup> record = (*g_railGroups)[group];
        if (!record)
            return 0;
        return record->slots[*g_railGroupSlot].object != kNoRailObject;
    }
    READABLE_FUNCTION(railSlotOpen, Verified);

    ee::Ptr<CellRails> newCellRails()
    {
        const ee::Ptr<CellRails> rails(editPoolAllocate(*g_cellRailsPool));
        zge::initList(&rails->rails);
        rails->group = kGroundRailGroup;
        rails->slots = 0;
        return rails;
    }
    READABLE_FUNCTION(newCellRails, Verified);

    void markRailPreviewStale()
    {
        *g_railPreviewStale = 1;
    }
    READABLE_FUNCTION(markRailPreviewStale, Verified);

    void releaseRailPreview()
    {
        if (*g_railPreviewNode)
            game::tail(releaseEditNode, ee::Ptr<ee::Ptr<zge::Node>>(sym::g_railPreviewNode));
    }
    READABLE_FUNCTION(releaseRailPreview, Verified);

    void nextRailSlot(ee::Ptr<ParkEditor> editor)
    {
        const ee::Ptr<ParkCell> cell = parkCell(editor, editor->cursorColumn, editor->cursorRow);
        // An object of rail group 0 takes no rails.
        if (cell->object && editObjectRailGroup(cell->object->object) == 0)
            return;
        uint32_t tries = 0;
        do
        {
            ++*g_railSlot;
            if (*g_railSlot >= kRailSlotCount)
                *g_railSlot = 0;
            ++tries;
        } while (!railSlotOpen(editor, cell->column, cell->row, *g_railSlot) && tries < kSlotTries);
        *g_railPreviewStale = 1;
    }
    READABLE_FUNCTION(nextRailSlot, Verified);

    void placeRailAtCursor(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<uint8_t> column;
        ee::Local<uint8_t> row;
        cursorCell(editor, column, row);
        placeRail(editor, *column, *row, *g_railType, *g_railSlot);
    }
    READABLE_FUNCTION(placeRailAtCursor, Verified);

    void removeRailsAtCursor(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<uint8_t> column;
        ee::Local<uint8_t> row;
        cursorCell(editor, column, row);
        removeCellRails(editor, *column, *row);
    }
    READABLE_FUNCTION(removeRailsAtCursor, Draft);

    void nextRailType()
    {
        int8_t type = int8_t(*g_railType + 1);
        *g_railPreviewStale = 1;
        if (!(type < int32_t(*g_railTypeCount)))
            type = 0;
        *g_railType = uint8_t(type);
    }
    READABLE_FUNCTION(nextRailType, Verified);

    void previousRailType()
    {
        int8_t type = int8_t(*g_railType - 1);
        if (type < 0)
            type = int8_t(*g_railTypeCount - 1);
        *g_railType = uint8_t(type);
        *g_railPreviewStale = 1;
    }
    READABLE_FUNCTION(previousRailType, Verified);

    ee::Ptr<const char> railTypeText()
    {
        return editorText(kRailTypeNames[*g_railType]);
    }
    READABLE_FUNCTION(railTypeText, Draft);

    uint32_t railObjectIndex(ee::Ptr<const char> name)
    {
        if (zge::compareStrings(kTextNone, name) == 0)
            return kNoRailObject;
        for (int32_t i = 0; i < int32_t(*g_railObjectCount); ++i)
        {
            if (zge::compareStrings((*g_railObjects)[i]->name, name) == 0)
                return uint32_t(i) & 0xFF;
        }
        return *g_railObjectCount;
    }
    READABLE_FUNCTION(railObjectIndex, Verified);

    // The file's GROUPS are found by name (the objects' RAIL word names one).
    uint32_t railTypeIndex(uint32_t nameHash)
    {
        const uint32_t count = *g_railGroupCount;
        for (int32_t i = 0; i < int32_t(count); ++i)
        {
            if ((*g_railGroups)[i]->name == nameHash)
                return uint32_t(i) & 0xFF;
        }
        return count;
    }
    READABLE_FUNCTION(railTypeIndex, Verified);

    namespace
    {
        constexpr ee::Ptr<const char> kTextOpen{sym::textBlockOpen};   // "{"
        constexpr ee::Ptr<const char> kTextClose{sym::textBlockClose}; // "}"
        constexpr ee::Ptr<const char> kTextType{sym::textType};        // "TYPE"
        constexpr ee::Ptr<const char> kTextName{sym::textName};        // "NAME"
        constexpr ee::Ptr<const char> kTextTypes{sym::textTypes};      // "TYPES"
        constexpr ee::Ptr<const char> kTextObjects{sym::textObjects};  // "OBJECTS"
        constexpr ee::Ptr<const char> kTextGroups{sym::textGroups};    // "GROUPS"
        constexpr uint32_t kTokenBytes = 0x100;
    }

    // TYPES { TYPE name ... }: the rail types' names, as hashes.
    void readRailTypes()
    {
        ee::Local<char[kTokenBytes]> token;
        zge::nextToken(token, kTokenBytes);
        if (zge::compareStrings(kTextOpen, token) != 0)
            return;
        *g_railTypeCount = 0;
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kTextClose, token) != 0)
        {
            if (zge::compareStrings(kTextType, token) == 0)
            {
                zge::nextToken(token, kTokenBytes);
                const uint32_t name = zge::nameHash(token);
                kRailTypeNames[*g_railTypeCount] = name;
                ++*g_railTypeCount;
                zge::nextToken(token, kTokenBytes);
            }
            else
            {
                zge::nextToken(token, kTokenBytes);
            }
        }
    }
    READABLE_FUNCTION(readRailTypes, Verified);

    // OBJECTS { NAME name { a world object's name per rail type } ... }.
    // The table gets 0x28 bytes an object where a pointer would do; an
    // object whose name no "{" follows ends the reading with no table made.
    void readRailObjects()
    {
        *g_railObjectCount = 0;
        ee::Local<char[kTokenBytes]> token;
        ee::Local<zge::List> found;
        zge::initList(found);
        zge::nextToken(token, kTokenBytes);
        if (zge::compareStrings(kTextOpen, token) != 0)
            return;
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kTextClose, token) != 0)
        {
            if (zge::compareStrings(kTextName, token) == 0)
            {
                const ee::Ptr<RailObject> object(zge::heapAllocate(sizeof(RailObject)));
                ++*g_railObjectCount;
                object->models = ee::Ptr<ee::Ptr<const char>>(zge::heapAllocate(uint32_t(*g_railTypeCount) << 2));
                zge::listPush(found, object);
                zge::nextToken(token, kTokenBytes);
                zge::stringCopy(object->name, token);
                zge::nextToken(token, kTokenBytes);
                if (zge::compareStrings(kTextOpen, token) != 0)
                    return;
                uint32_t type = 0;
                zge::nextToken(token, kTokenBytes);
                while (zge::compareStrings(kTextClose, token) != 0)
                {
                    const uint32_t text = zge::heapAllocate(uint32_t(zge::stringLength(token) + 1));
                    object->models[type] = ee::Ptr<const char>(text);
                    zge::stringCopy(object->models[type], token);
                    ++type;
                    zge::nextToken(token, kTokenBytes);
                }
                zge::nextToken(token, kTokenBytes);
            }
            else
            {
                zge::nextToken(token, kTokenBytes);
            }
        }
        uint32_t index = 0;
        *g_railObjects = ee::Ptr<ee::Ptr<RailObject>>(zge::heapAllocate(uint32_t(*g_railObjectCount) * uint32_t(sizeof(RailObject))));
        for (ee::Ptr<zge::ListNode> node = found->head; node;)
        {
            (*g_railObjects)[index++] = zge::recordOfNode<RailObject>(node);
            const ee::Ptr<zge::ListNode> next = node->next;
            zge::listRemove(found, node);
            node = next;
        }
    }
    READABLE_FUNCTION(readRailObjects, Verified);

    // GROUPS { NAME name, then for each of the eight slots: a rail object's
    // name (or NONE), its quarter turns and its drop ... }.
    void readRailGroups()
    {
        *g_railGroupCount = 0;
        ee::Local<char[kTokenBytes]> token;
        ee::Local<zge::List> found;
        ee::Local<int32_t> number;
        zge::initList(found);
        zge::nextToken(token, kTokenBytes);
        if (zge::compareStrings(kTextOpen, token) != 0)
            return;
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kTextClose, token) != 0)
        {
            if (zge::compareStrings(kTextName, token) == 0)
            {
                const ee::Ptr<RailGroup> group(zge::heapAllocate(sizeof(RailGroup)));
                ++*g_railGroupCount;
                zge::listPush(found, group);
                zge::nextToken(token, kTokenBytes);
                group->name = zge::nameHash(token);
                for (uint32_t slot = 0; slot < kRailSlotCount; ++slot)
                {
                    zge::nextToken(token, kTokenBytes);
                    group->slots[slot].object = uint8_t(railObjectIndex(token));
                    zge::readNumberToken(number);
                    group->slots[slot].quarterTurns = uint8_t(*number);
                    zge::readNumberToken(number);
                    group->slots[slot].drop = uint8_t(*number);
                }
                zge::nextToken(token, kTokenBytes);
            }
            else
            {
                zge::nextToken(token, kTokenBytes);
            }
        }
        uint32_t index = 0;
        *g_railGroups = ee::Ptr<ee::Ptr<RailGroup>>(zge::heapAllocate(uint32_t(*g_railGroupCount) * uint32_t(sizeof(RailGroup))));
        for (ee::Ptr<zge::ListNode> node = found->head; node;)
        {
            (*g_railGroups)[index++] = zge::recordOfNode<RailGroup>(node);
            const ee::Ptr<zge::ListNode> next = node->next;
            zge::listRemove(found, node);
            node = next;
        }
    }
    READABLE_FUNCTION(readRailGroups, Verified);

    // The RAILS block: { TYPES ... OBJECTS ... GROUPS ... }.
    void readRailList()
    {
        ee::Local<char[kTokenBytes]> token;
        zge::nextToken(token, kTokenBytes);
        if (zge::compareStrings(kTextOpen, token) != 0)
            return;
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kTextClose, token) != 0)
        {
            if (zge::compareStrings(kTextTypes, token) == 0)
                readRailTypes();
            else if (zge::compareStrings(kTextObjects, token) == 0)
                readRailObjects();
            else if (zge::compareStrings(kTextGroups, token) == 0)
                readRailGroups();
            zge::nextToken(token, kTokenBytes);
        }
    }
    READABLE_FUNCTION(readRailList, Verified);

    ee::Ptr<ee::Ptr<CellRails>> cellRailsSlot(ee::Ptr<ParkCell> cell)
    {
        return railsSlotOf(cell);
    }
    READABLE_FUNCTION(cellRailsSlot, Verified);
}
