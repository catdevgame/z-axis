// The park editor's park and its tool set's buttons: a park is a grid of
// cells read from a theme's map file, each with an anchor pieces sit on;
// breakables are the pieces a test run can knock down, kept in a pool of their
// own so that they can be put back. A saved park is a byte stream: a word of
// header, then per cell a mark and what its anchor or tower holds, an end mark.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_cells.cpp.

#include "aggressive/level/park_cells.h"
#include "zge/editor/park_cells.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "aggressive/level/breakables.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "zge/scene/node.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_display.h"
#include "aggressive/level/edit_tools.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/edit_core.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/edit_tower.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_messages.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/park_cells.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kSetEditObjectDisplay = sym::setEditObjectDisplay; // (object, display)
        constexpr uint32_t kSelectFirstTool = sym::selectFirstTool;
        constexpr uint32_t kSelectNextTool = sym::selectNextTool;
        constexpr uint32_t kSelectPreviousTool = sym::selectPreviousTool;
        constexpr uint32_t kEditObjectDef = sym::editObjectDef;
        constexpr uint32_t kEditObjectDisplay = sym::editObjectDisplay;
        constexpr uint32_t kMakeEditObjectDisplay = sym::makeEditObjectDisplay;
        constexpr uint32_t kTranslateEditObject = sym::translateEditObject;
        constexpr uint32_t kSaveEditObject = sym::saveEditObject;
        constexpr uint32_t kLoadEditObject = sym::loadEditObject;
        constexpr uint32_t kSetEditObjectPosition = sym::setEditObjectPosition;
        constexpr uint32_t kSetTowerState = sym::setTowerState;
        constexpr uint32_t kSetTowerTop = sym::setTowerTop;
        constexpr uint32_t kSetTowerBottom = sym::setTowerBottom;
        constexpr uint32_t kAllocateParkCells = sym::allocateParkCells;
        constexpr uint32_t kMakeParkCellAnchors = sym::makeParkCellAnchors;
        constexpr uint32_t kReadParkCellDefs = sym::readParkCellDefs;
        constexpr uint32_t kReadParkUserData = sym::readParkUserData;
        constexpr uint32_t kParkCell = sym::parkCell;           // (park, cell indices *) -> the cell
        constexpr uint32_t kParkCellDef = sym::parkCellDef;        // (park, cell indices *) -> its map entry
        constexpr uint32_t kParkCellEmpty = sym::parkCellEmpty;
        constexpr uint32_t kParkCellPosition = sym::parkCellPosition;
        constexpr uint32_t kParkCellBlocked = sym::parkCellBlocked;
        constexpr uint32_t kParkCellValid = sym::parkCellValid;
        constexpr uint32_t kCellAnchor = sym::cellAnchor;         // (cell) -> its anchor
        constexpr uint32_t kForEachParkCell = sym::forEachParkCell;    // (park, visit, context)
        constexpr uint32_t kParkStreamWrite = sym::parkStreamWrite; // (stream, bytes *, count)
        constexpr uint32_t kParkStreamRead = sym::parkStreamRead;
        constexpr uint32_t kSaveEditCamera = sym::saveEditCamera;
        constexpr uint32_t kLoadEditCamera = sym::loadEditCamera;
        constexpr uint32_t kSetAnchorPosition = sym::setAnchorPosition;
        constexpr uint32_t kSetAnchorAngles = sym::setAnchorAngles;
        constexpr uint32_t kAnchorAcceptList = sym::anchorAcceptList;
        constexpr uint32_t kAcceptListFirst = sym::acceptListFirst;
        constexpr uint32_t kAcceptEntryNext = sym::acceptEntryNext;
        constexpr uint32_t kAcceptEntryValue = sym::acceptEntryValue;
        constexpr uint32_t kAttachToAnchor = sym::attachToAnchor;
        constexpr uint32_t kDetachFromAnchor = sym::detachFromAnchor;
        constexpr uint32_t kAnchorAttachedObject = sym::anchorAttachedObject;
        constexpr uint32_t kAnchorAngles = sym::anchorAngles;
        constexpr uint32_t kAnchorReserved = sym::anchorReserved;
        constexpr uint32_t kSaveAnchor = sym::saveAnchor;
        constexpr uint32_t kLoadAnchor = sym::loadAnchor;
        constexpr uint32_t kAddAnchorAccept = sym::addAnchorAccept;
        constexpr uint32_t kPlayEditPauseScript = sym::playEditPauseScript;
        constexpr uint32_t kDismissEditHelpPage = sym::dismissEditHelpPage;
        constexpr uint32_t kBroadcastEditMessage = sym::broadcastEditMessage;
        constexpr uint32_t kWrapEditDisplay = sym::wrapEditDisplay;
        constexpr uint32_t kFreeEditDisplay = sym::freeEditDisplay;
        constexpr uint32_t kEditDisplayNode = sym::editDisplayNode;
        constexpr uint32_t kClearParkCell = sym::clearParkCell;
        constexpr uint32_t kMovedMessage = sym::makeMovedMessage;
        constexpr uint32_t kDeleteAnchored = sym::makeDeleteMessage;
        constexpr uint32_t kRemoveFlaggedMessage = sym::makeRemoveFlaggedMessage;
        constexpr uint32_t kEditTowerObjectName = sym::g_parkCellEditTowerObjectName;
        constexpr uint32_t kNameStringsPage = ee::high(sym::g_textMapDefinitionEnd), kCellNamesPage = ee::high(sym::cellEnd);

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_breakablePool> g_breakablePool;
    }

    int32_t toolButtonPause(ee::Ptr<EditUiButton> button)
    {
        return game::tail(playEditPauseScript, button->ui);
    }
    READABLE_FUNCTION_WAITS(toolButtonPause, Verified);

    ee::Ptr<zge::Pool> breakablePool()
    {
        return *g_breakablePool;
    }
    READABLE_FUNCTION(breakablePool, Verified);

    uint32_t parkCell(uint32_t address, ee::Ptr<const CellIndex> indices);

    namespace
    {
        namespace tail
        {
        int32_t poolReset(ee::Ptr<zge::Pool> pool) { return ee::tailCall<int32_t>(sym::poolReset, pool); }
        }
    }

    // restoreBreakables: the knocked-down pieces made again where they sat, from
    // their records; the pool emptied.
    int32_t restoreBreakables(uint32_t park, uint32_t catalog)
    {
        const ee::Ptr<zge::Pool> pool = *g_breakablePool;
        if (!pool)
            return 0;
        for (ee::Ptr<zge::ListNode> link = zge::firstPoolLink(pool); link; link = zge::nextPoolLink(link))
        {
            const ee::Ptr<Breakable> breakable = poolRecordAs<Breakable>(link);
            if (breakable->state != 0)
            {
                const ee::Ptr<ParkCell> cell(parkCell(park, &breakable->cell));
                const uint32_t piece = catalogPieceDef(catalog, breakable->piece);
                const uint32_t className = static_cast<uint32_t>(pieceDefClassName(piece));
                const ee::Ptr<EditObjectPart> object(newEditObject(className, piece));
                const uint32_t display = wrapEditDisplay(breakable->node);
                setEditObjectDisplay(object, display);
                setAnchorAngles(cell->anchor, &breakable->angles);
                placeEditPiece(cell->anchor, object);
            }
        }
        return tail::poolReset(*g_breakablePool);
    }
    READABLE_FUNCTION_WAITS(restoreBreakables, Verified);

    void forEachParkCell(uint32_t address, uint32_t visit, ee::Address context);

    // A breakable's record: its state set and read, and what it showed.
    void setBreakableRecordState(ee::Ptr<Breakable> breakable, uint32_t state)
    {
        breakable->state = state;
    }
    READABLE_FUNCTION(setBreakableRecordState, Verified);

    uint32_t breakableRecordState(ee::Ptr<Breakable> breakable)
    {
        return breakable->state;
    }
    READABLE_FUNCTION(breakableRecordState, Verified);

    uint32_t breakableRecordObject(ee::Ptr<Breakable> breakable)
    {
        return breakable->node;
    }
    READABLE_FUNCTION(breakableRecordObject, Verified);

    void clearParkPieces(uint32_t park)
    {
        game::tail(forEachParkCell, park, kClearParkCell, 0);
    }
    READABLE_FUNCTION_WAITS(clearParkPieces, Verified);

    uint32_t cellAnchor(uint32_t cell);

    // parkMaximumHeight: no caller: the highest a tower can reach.
    ee::Float parkMaximumHeight(uint32_t address)
    {
        const ee::Ptr<EditPark> park(address);
        return park->heightIncrement * ee::toFloat(park->maximumHeight);
    }
    READABLE_FUNCTION(parkMaximumHeight, Draft);

    namespace
    {
        namespace tail
        {
        int32_t setEditObjectDisplay(ee::Ptr<EditObjectPart> object, uint32_t display) { return ee::tailCall<int32_t>(kSetEditObjectDisplay, object, display); }
        }
    }

    // clearParkCell: a cell emptied: a tower brought back to the map's height and
    // cleared, a piece deleted; the anchor told it moved.
    int32_t clearParkCell(uint32_t address, uint32_t cell, uint32_t def)
    {
        ee::Local<zge::Vec4> lowered;
        const uint32_t anchor = cellAnchor(cell);
        const ee::Ptr<EditObjectPart> object = anchorAttachedObject(anchor);
        if (object == 0)
            return 0;
        const ee::Ptr<const char> className = editObjectClassName(object);
        if (zge::compareStrings(kEditTowerObjectName, className) == 0)
        {
            const ee::Float increment = ee::Ptr<EditPark>(address)->heightIncrement;
            lowered->x = lowered->z = ee::Float(0.0f);
            lowered->y = -increment * ee::toFloat(ee::Ptr<ParkCellDef>(def)->height);
            setEditObjectPosition(object, lowered);
            broadcastEditMessage(object, kRemoveFlaggedMessage, 0);
        }
        else
        {
            detachFromAnchor(anchor);
            broadcastEditMessage(object, kDeleteAnchored, 0);
        }
        return broadcastEditMessage(anchor, kMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(clearParkCell, Verified);

    uint32_t parkDepth(uint32_t park)
    {
        return static_cast<uint32_t>(ee::Ptr<EditPark>(park)->depth);
    }
    READABLE_FUNCTION(parkDepth, Verified);

    uint32_t parkWidth(uint32_t park)
    {
        return static_cast<uint32_t>(ee::Ptr<EditPark>(park)->width);
    }
    READABLE_FUNCTION(parkWidth, Verified);

    uint32_t parkStreamData(uint32_t stream)
    {
        return ee::Ptr<ParkStream>(stream)->data;
    }
    READABLE_FUNCTION(parkStreamData, Draft);

    uint32_t parkStreamSize(uint32_t stream)
    {
        return ee::Ptr<ParkStream>(stream)->size;
    }
    READABLE_FUNCTION(parkStreamSize, Draft);

    uint32_t parkStreamPosition(uint32_t stream)
    {
        return ee::Ptr<ParkStream>(stream)->position;
    }
    READABLE_FUNCTION(parkStreamPosition, Draft);

    namespace
    {
        inline ee::Global<int32_t, sym::g_parkEditorCore> g_parkEditorCore;
    }
}
