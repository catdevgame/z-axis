// The park editor's park and its tool set's buttons: a park is a grid of
// cells read from a theme's map file, each with an anchor pieces sit on;
// breakables are the pieces a test run can knock down, kept in a pool of their
// own so that they can be put back. A saved park is a byte stream: a word of
// header, then per cell a mark and what its anchor or tower holds, an end mark.

#include "zge/editor/game.h"
#include "zge/editor/park_cells.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "zge/scene/node.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_display.h"
#include "zge/editor/edit_tools.h"
#include "zge/editor/park_camera.h"
#include "zge/editor/park_ui.h"
#include "zge/editor/edit_core.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/edit_tower.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_messages.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/editor/park_cells.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kSetEditObjectDisplay = sym::setEditObjectDisplay; // (object, display)
        constexpr uint32_t kForEachParkCell = sym::forEachParkCell;    // (park, visit, context)
        constexpr uint32_t kTiltEditCamera = sym::tiltEditCamera;
        constexpr uint32_t kTurnEditCamera = sym::turnEditCamera;
        constexpr uint32_t kZoomEditCamera = sym::zoomEditCamera;
        constexpr uint32_t kSaveEditCamera = sym::saveEditCamera;

        // The cell visits and handlers the functions here hand on.
        constexpr uint32_t kCameraButtonTurn = sym::cameraButtonTurn;
        constexpr uint32_t kCameraButtonTilt = sym::cameraButtonTilt;
        constexpr uint32_t kCameraButtonZoom = sym::cameraButtonZoom;
        constexpr uint32_t kCollectBreakable = sym::collectCellBreakable;
        constexpr uint32_t kSaveParkCell = sym::saveParkCell;
        constexpr uint32_t kLoadParkCell = sym::loadParkCell;
        constexpr uint32_t kResetParkCell = sym::resetParkCell;
        constexpr uint32_t kFindPieceInCell = sym::findPieceInCell;
        constexpr uint32_t kReadParkCellDef = sym::readParkCellDef;
        constexpr uint32_t kBuildParkCell = sym::buildParkCell;
        constexpr uint32_t kDeleteAnchored = sym::makeDeleteMessage;
        constexpr uint32_t kHideMessage = sym::makeHideMessage;
        constexpr uint32_t kFindMessage = sym::makeFindMessage;

        // The map file's words, the class names.
        constexpr uint32_t kEmptyString = sym::emptyString4;
        constexpr uint32_t kMapDefinitionEnd = sym::g_textMapDefinitionEnd;
        constexpr uint32_t kMapDimension = sym::g_textMapDimension;
        constexpr uint32_t kMapGridCenter = sym::g_textMapGridCenter;
        constexpr uint32_t kMapStartPosition = sym::g_textMapStartPosition;
        constexpr uint32_t kMapData = sym::g_textMapData;
        constexpr uint32_t kMapUserData = sym::g_textMapUserData;
        constexpr uint32_t kMapCameraBoundOffset = sym::g_textMapCameraBoundOffset;
        constexpr uint32_t kMapDefaultHeight = sym::g_textMapDefaultHeight;
        constexpr uint32_t kMapMaximumHeight = sym::g_textMapMaximumHeight;
        constexpr uint32_t kMapHeightIncrement = sym::g_textMapHeightIncrement;
        constexpr uint32_t kMapSectorSize = sym::g_textMapSectorSize;
        constexpr uint32_t kCellStatus = sym::g_textStatus2;
        constexpr uint32_t kCellObject = sym::g_textObject;
        constexpr uint32_t kCellHeight = sym::g_textHeight2;
        constexpr uint32_t kCellOrient = sym::g_textOrient;
        constexpr uint32_t kCellEnd = sym::cellEnd;            // ">"
        constexpr uint32_t kEditAnchorName = sym::g_parkCellEditAnchorName;
        constexpr uint32_t kEditObjectName = sym::g_parkCellEditObjectName;
        constexpr uint32_t kEditTowerObjectName = sym::g_parkCellEditTowerObjectName;

        constexpr uint32_t kBreakableBytes = 0x20, kBreakableBlocks = 0x400;
        constexpr uint8_t kParkEndMark = 170;                 // the byte after a park's cells
        constexpr uint8_t kCellHoldsPiece = 204, kCellHoldsNothing = 187; // a saved cell's first byte
        constexpr uint32_t kParkStreamMagic = 3618;           // the stream's first word; a load does not check it
        constexpr uint32_t kCellBlocked = 2;                  // a map entry's status
        constexpr uint32_t kOrientAngle = 14;                 // a map entry's Orient is quarter turns
        constexpr ee::Float kCellMetres = 4.572f;             // 15 feet a cell
        constexpr ee::Float kCameraRate = 0.05f;

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_breakablePool> g_breakablePool;

        // Bytes of a grid of 16-byte records.
        uint32_t cellBytes(const ee::Ptr<EditPark> park)
        {
            return static_cast<uint32_t>(park->width) * static_cast<uint32_t>(park->height) * (static_cast<uint32_t>(park->depth) << 4);
        }

        // A knocked-down piece's record in the pool (0x20 bytes).
        // A cell's visit: (park, cell, its map entry, context).
        using ParkCellVisit = ee::FunctionPtr<void(uint32_t, uint32_t, uint32_t, uint32_t)>;

        // What findParkPiece asks of the cells: the record of the piece, and
        // the anchor that answers (the find message's handler fills it).
        struct PieceSearch
        {
            uint32_t piece;
            ee::Ptr<EditObjectPart> found;
        };

        constexpr uint32_t kButtonTurn = 0x10, kButtonTilt = 17, kButtonZoom = 19, kButtonHeld = 2;
    }

    // setCameraButtons: the sticks' buttons on the tool set's camera.
    void setCameraButtons(ee::Address address)
    {
        const ee::Ptr<EditToolSet> toolSet(address);
        const ee::Float rate = kCameraRate;
        setUiButton(toolSet->ui, kButtonTurn, kButtonHeld, kCameraButtonTurn, toolSet->editCamera, 1, rate);
        setUiButton(toolSet->ui, kButtonTilt, kButtonHeld, kCameraButtonTilt, toolSet->editCamera, 1, rate);
        game::tail(setUiButton, toolSet->ui, kButtonZoom, kButtonHeld, kCameraButtonZoom, toolSet->editCamera, 1, rate);
    }
    READABLE_FUNCTION(setCameraButtons, Verified);

    void setToolSetCameraActive(ee::Ptr<EditToolSet> toolSet, uint32_t active)
    {
        toolSet->cameraActive = active;
    }
    READABLE_FUNCTION(setToolSetCameraActive, Verified);

    int32_t toolButtonNextTool(uint32_t button)
    {
        const ee::Ptr<EditToolSet> toolSet(ee::Ptr<EditUiButton>(button)->data); // the button's data word: the tool set
        return game::tail(selectNextTool, toolSet);
    }
    READABLE_FUNCTION_WAITS(toolButtonNextTool, Verified);

    int32_t toolButtonPreviousTool(uint32_t button)
    {
        const ee::Ptr<EditToolSet> toolSet(ee::Ptr<EditUiButton>(button)->data); // the button's data word: the tool set
        return game::tail(selectPreviousTool, toolSet);
    }
    READABLE_FUNCTION_WAITS(toolButtonPreviousTool, Verified);

    int32_t toolButtonFirstTool(uint32_t button)
    {
        const ee::Ptr<EditToolSet> toolSet(ee::Ptr<EditUiButton>(button)->data); // the button's data word: the tool set
        return game::tail(selectFirstTool, toolSet);
    }
    READABLE_FUNCTION_WAITS(toolButtonFirstTool, Verified);

    namespace
    {
        // A camera button: the help page goes, the camera moves by its rate.
        int32_t cameraButton(uint32_t button, uint32_t move)
        {
            const ee::Ptr<EditUiButton> self(button);
            dismissEditHelpPage(self->ui, 0);
            return ee::tailCall<int32_t>(move, self->data, self->rate);
        }
    }

    int32_t cameraButtonTurn(uint32_t button)
    {
        return cameraButton(button, kTurnEditCamera);
    }
    READABLE_FUNCTION_WAITS(cameraButtonTurn, Verified);

    int32_t cameraButtonTilt(uint32_t button)
    {
        return cameraButton(button, kTiltEditCamera);
    }
    READABLE_FUNCTION_WAITS(cameraButtonTilt, Verified);

    int32_t cameraButtonZoom(uint32_t button)
    {
        return cameraButton(button, kZoomEditCamera);
    }
    READABLE_FUNCTION_WAITS(cameraButtonZoom, Verified);

    uint32_t toolSetUi(ee::Ptr<EditToolSet> toolSet)
    {
        return toolSet->ui;
    }
    READABLE_FUNCTION(toolSetUi, Verified);

    namespace
    {
        // saveEditCamera, and what it leaves in the result register (saveToolSetCamera returns it).
        int32_t saveEditCameraKeepingResult(ee::Ptr<EditCamera> camera, uint32_t stream) { return ee::tailCall<int32_t>(kSaveEditCamera, camera, stream); }
    }

    int32_t saveToolSetCamera(ee::Ptr<EditToolSet> toolSet, uint32_t stream)
    {
        return saveEditCameraKeepingResult(toolSet->editCamera, stream);
    }
    READABLE_FUNCTION(saveToolSetCamera, Verified);

    void loadToolSetCamera(ee::Ptr<EditToolSet> toolSet, uint32_t stream)
    {
        game::tail(loadEditCamera, toolSet->editCamera, stream);
    }
    READABLE_FUNCTION(loadToolSetCamera, Verified);

    // newEditPark: an empty park; the breakables' pool, once.
    uint32_t newEditPark()
    {
        const uint32_t address = editAllocate(static_cast<uint32_t>(sizeof(EditPark)));
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<zge::Pool> pool = *g_breakablePool;
        park->heightIncrement = ee::Float(1.0f);
        park->maximumHeight = 20;
        park->cellSize = zge::Vec3{kCellMetres, kCellMetres, kCellMetres};
        park->flag2c = 1;
        park->height = 0;
        park->width = 0;
        park->depth = 0;
        park->defaultHeight = 0;
        park->cells = 0;
        park->cellDefs = 0;
        std::memset(&park->origin, 0, 12);
        std::memset(park->startCell, 0, sizeof(park->startCell));
        park->catalog = 0;
        if (!pool)
            *g_breakablePool = ee::Ptr<zge::Pool>(zge::createPool(kBreakableBytes, kBreakableBlocks, 0));
        return address;
    }
    READABLE_FUNCTION_WAITS(newEditPark, Verified);

    // freeBreakables: every breakable's node let go, the pool gone.
    void freeBreakables()
    {
        const ee::Ptr<zge::Pool> pool = *g_breakablePool;
        if (pool != 0)
        {
            for (ee::Ptr<zge::ListNode> link = zge::firstPoolLink(ee::Ptr<zge::Pool>(pool)); link; link = zge::nextPoolLink(link))
            {
                const ee::Ptr<Breakable> breakable = poolRecordAs<Breakable>(link);
                if (breakable->state != 0)
                    zge::releaseNode(ee::Ptr<zge::Node>(breakable->node), 1);
            }
            zge::poolReset(*g_breakablePool);
            zge::destroyPool(ee::Ptr<zge::Pool>(*g_breakablePool));
            *g_breakablePool = 0;
        }
    }
    READABLE_FUNCTION_WAITS(freeBreakables, Verified);

    namespace
    {
        // forEachParkCell, and what it leaves in the result register (its callers here return it).
        int32_t forEachParkCellKeepingResult(uint32_t address, uint32_t visit, uint32_t context) { return ee::tailCall<int32_t>(kForEachParkCell, address, visit, context); }
    }

    int32_t collectBreakables(uint32_t park)
    {
        const ee::Ptr<zge::Pool> pool = *g_breakablePool;
        if (pool != 0)
            return forEachParkCellKeepingResult(park, kCollectBreakable, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(collectBreakables, Verified);

    uint32_t parkCell(uint32_t address, ee::Ptr<const CellIndex> indices);

    namespace
    {
        namespace tail
        {
        int32_t poolReset(ee::Ptr<zge::Pool> pool) { return ee::tailCall<int32_t>(sym::poolReset, pool); }
        }
    }

    void forEachParkCell(uint32_t address, uint32_t visit, ee::Address context);

    void saveEditPark(uint32_t park, uint32_t stream)
    {
        ee::Local<uint8_t> bytes;
        forEachParkCell(park, kSaveParkCell, stream);
        *bytes = kParkEndMark;
        parkStreamWrite(stream, bytes, 1);
    }
    READABLE_FUNCTION_WAITS(saveEditPark, Verified);

    void loadEditPark(uint32_t park, uint32_t stream)
    {
        ee::Local<uint8_t[0x30]> bytes;
        forEachParkCell(park, kLoadParkCell, stream);
        parkStreamRead(stream, bytes, 1);
    }
    READABLE_FUNCTION_WAITS(loadEditPark, Verified);

    void copyParkOrigin(uint32_t park, ee::Ptr<zge::Vec4> position)
    {
        *position = ee::Ptr<EditPark>(park)->origin;
    }
    READABLE_FUNCTION(copyParkOrigin, Verified);

    uint32_t cellAnchor(uint32_t cell);

    // deleteParkPieces: every piece on the park's anchors detached and deleted.
    void deleteParkPieces(uint32_t address)
    {
        ee::Local<CellIndex> cell;
        const ee::Ptr<EditPark> park(address);
        uint32_t anchor = 0;
        int32_t result = 0;
        if (park->height <= 0)
            return;
        for (int32_t y = 0;;)
        {
            if (park->depth > 0)
            {
                for (int32_t z = 0;;)
                {
                    if (park->width > 0)
                    {
                        for (int32_t x = 0;;)
                        {
                            cell->x = x;
                            cell->y = y;
                            cell->z = z;
                            const uint32_t found = parkCell(address, cell);
                            anchor = cellAnchor(found);
                            if (anchorAttachedObject(anchor) != 0)
                            {
                                const ee::Ptr<EditObjectPart> object = detachFromAnchor(anchor);
                                broadcastEditMessage(object, kDeleteAnchored, 0);
                            }
                            if (!(++x < park->width))
                                break;
                        }
                    }
                    if (!(++z < park->depth))
                        break;
                }
            }
            result = ++y < park->height ? 1 : 0;
            if (result == 0)
                break;
        }
    }
    READABLE_FUNCTION_WAITS(deleteParkPieces, Verified);

    int32_t resetParkCells(uint32_t park)
    {
        return forEachParkCellKeepingResult(park, kResetParkCell, 0);
    }
    READABLE_FUNCTION_WAITS(resetParkCells, Verified);

    // freeEditPark: every cell's anchor told to delete what it holds, the map
    // entries' anchors deleted, the grids and the park freed.
    void freeEditPark(uint32_t address)
    {
        ee::Local<CellIndex> cell;
        const ee::Ptr<EditPark> park(address);
        int32_t x = 0;
        int32_t z = 0;
        int32_t nextZ = 0;
        int32_t nextY = 0;
        int32_t y = 0;
        int32_t count = 0;
        if (park->height > 0)
        {
            for (;;)
            {
                z = 0;
                if (park->depth > 0)
                {
                    nextY = y + 1;
                    for (;;)
                    {
                        x = 0;
                        if (park->width > 0)
                        {
                            nextZ = z + 1;
                            cell->x = x;
                            for (;;)
                            {
                                cell->y = y;
                                cell->z = z;
                                ++x;
                                const uint32_t found = parkCell(address, cell);
                                const uint32_t anchor = cellAnchor(found);
                                broadcastEditMessage(anchor, kDeleteAnchored, 0);
                                const ee::Ptr<ParkCellDef> def(park->cellDefs + (static_cast<uint32_t>(count) << 4));
                                ++count;
                                deleteEditObject(def->anchor);
                                if (!(x < park->width))
                                    break;
                                cell->x = x;
                            }
                        }
                        else
                            nextZ = z + 1;
                        z = nextZ;
                        if (!(z < park->depth))
                            break;
                    }
                }
                else
                    nextY = y + 1;
                y = nextY;
                if (!(y < park->height))
                    break;
            }
        }
        if (park->cells != 0)
            editFree(park->cells);
        if (park->cellDefs != 0)
            editFree(park->cellDefs);
        editFree(address);
    }
    READABLE_FUNCTION_WAITS(freeEditPark, Verified);

    // makeParkCellAnchors: each cell's own anchor, made from its map entry's and
    // turned as the entry says.
    void makeParkCellAnchors(uint32_t address)
    {
        ee::Local<uint16_t> angles;
        const ee::Ptr<EditPark> park(address);
        int32_t result = 0;
        if (park->height <= 0)
            return;
        uint32_t count = 0;
        for (int32_t y = 0;;)
        {
            const int32_t nextY = y + 1;
            if (park->depth > 0)
            {
                for (int32_t z = 0;;)
                {
                    const int32_t nextZ = z + 1;
                    if (park->width > 0)
                    {
                        for (int32_t x = 0;;)
                        {
                            const uint32_t entry = count << 4;
                            ++x;
                            ++count;
                            const uint32_t anchor = newEditObject(kEditAnchorName, ee::Ptr<ParkCellDef>(park->cellDefs + entry)->anchor);
                            const ee::Ptr<ParkCell> cell(park->cells + entry);
                            cell->anchor = anchor;
                            (&*angles)[0] = 0;
                            (&*angles)[1] = ee::Ptr<ParkCellDef>(park->cellDefs + entry)->orient;
                            (&*angles)[2] = 0;
                            setAnchorAngles(cell->anchor, angles);
                            if (!(x < park->width))
                                break;
                        }
                    }
                    z = nextZ;
                    if (!(z < park->depth))
                        break;
                }
            }
            y = nextY;
            result = y < park->height ? 1 : 0;
            if (result == 0)
                break;
        }
    }
    READABLE_FUNCTION_WAITS(makeParkCellAnchors, Verified);

    // allocateParkCells: the grid, each cell knowing its indices.
    void allocateParkCells(uint32_t address)
    {
        ee::Local<int32_t> indices;
        const ee::Ptr<EditPark> park(address);
        park->cells = editAllocate(cellBytes(park));
        uint32_t count = 0;
        if (park->height > 0)
        {
            for (int32_t y = 0;;)
            {
                if (park->depth > 0)
                {
                    for (int32_t z = 0;;)
                    {
                        if (park->width > 0)
                        {
                            for (int32_t x = 0;;)
                            {
                                const ee::Ptr<ParkCell> cell(park->cells + (count << 4));
                                (&*indices)[0] = x;
                                (&*indices)[1] = y;
                                (&*indices)[2] = z;
                                cell->x = x;
                                cell->y = y;
                                cell->z = z;
                                cell->anchor = 0;
                                ++x;
                                ++count;
                                if (!(x < park->width))
                                    break;
                            }
                        }
                        if (!(++z < park->depth))
                            break;
                    }
                }
                if (!(++y < park->height))
                    break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(allocateParkCells, Verified);

    // readParkUserData: what MapUserData's block would be read by: empty, as the
    // shipped maps' blocks are.
    void readParkUserData()
    {
    }
    READABLE_FUNCTION(readParkUserData, Verified);

    int32_t readParkCellDefs(uint32_t park)
    {
        return forEachParkCellKeepingResult(park, kReadParkCellDef, 0);
    }
    READABLE_FUNCTION_WAITS(readParkCellDefs, Verified);

    // readParkMap: a theme's map file, Assets\Editor\<theme>\<theme>.map (a
    // saved park's map name may be empty: then nothing), read into memory and
    // through the script reader, key by key to MapDefinitionEnd.
    void readParkMap(uint32_t address, uint32_t path)
    {
        ee::Local<uint8_t[0x100]> token;
        ee::Local<uint32_t> bytes;
        const ee::Ptr<EditPark> park(address);
        if (zge::compareStrings(path, kEmptyString) == 0)
            return;
        const ee::Ptr<zge::ZFile> file = zge::openFile(path, zge::kFileModeRead, 0).file();
        zge::fileSize(file, bytes, 0);
        const uint32_t text = editAllocate(*bytes);
        zge::fileRead(file, text, *bytes, 0);
        zge::closeFile(file, 0);
        const ee::Ptr<zge::ZFile> memoryFile = zge::openMemoryFile(text, *bytes);
        const ee::Ptr<const char> memoryPath = zge::filePath(memoryFile);
        zge::beginScript(memoryPath);
        zge::nextToken(token, 0x100);
        park->boundOffsetB[2] = ee::Float(10.0f);
        park->boundOffsetB[1] = ee::Float(40.0f);
        park->boundOffsetA[0] = ee::Float(10.0f);
        park->boundOffsetA[2] = ee::Float(10.0f);
        park->boundOffsetA[1] = ee::Float(0.0f);
        park->boundOffsetB[0] = ee::Float(10.0f);

        const auto isKey = [&](uint32_t key) {
            return zge::stringCompareNoCase(key, token) == 0;
        };
        const auto skipToken = [&]() {
            zge::nextToken(token, 0x100);
        };

        // One key's values. A block's opening brace is a token; a key ending in
        // a colon has none.
        const auto readKey = [&]() {
            if (isKey(kMapDimension))
            {
                skipToken();
                zge::readNumberToken(&park->width);
                zge::readNumberToken(&park->depth);
                zge::readNumberToken(&park->height);
                return;
            }
            if (isKey(kMapGridCenter))
            {
                skipToken();
                zge::readFloatToken(&park->origin.x);
                zge::readFloatToken(&park->origin.y);
                zge::readFloatToken(&park->origin.z);
                return;
            }
            if (isKey(kMapStartPosition))
            {
                skipToken();
                zge::readNumberToken(&park->startCell[0]);
                zge::readNumberToken(&park->startCell[1]);
                zge::readNumberToken(&park->startCell[2]);
                return;
            }
            if (isKey(kMapData))
            {
                skipToken();
                allocateParkCells(address);
                park->cellDefs = editAllocate(cellBytes(park));
                readParkCellDefs(address);
                return;
            }
            if (isKey(kMapUserData))
            {
                skipToken();
                readParkUserData();
                return;
            }
            if (isKey(kMapCameraBoundOffset))
            {
                skipToken();
                zge::readFloatToken(&park->boundOffsetA);
                zge::readFloatToken(&park->boundOffsetA[1]);
                zge::readFloatToken(&park->boundOffsetA[2]);
                zge::readFloatToken(&park->boundOffsetB);
                zge::readFloatToken(&park->boundOffsetB[1]);
                zge::readFloatToken(&park->boundOffsetB[2]);
                return;
            }
            if (isKey(kMapDefaultHeight))
            {
                zge::readNumberToken(&park->defaultHeight);
                return;
            }
            if (isKey(kMapMaximumHeight))
            {
                zge::readNumberToken(&park->maximumHeight);
                return;
            }
            if (isKey(kMapHeightIncrement))
            {
                zge::readFloatToken(&park->heightIncrement);
                return;
            }
            if (isKey(kMapSectorSize))
            {
                skipToken();
                zge::readFloatToken(&park->cellSize);
                zge::readFloatToken(&park->cellSize.y);
                zge::readFloatToken(&park->cellSize.z);
            }
        };

        for (;;)
        {
            if (isKey(kMapDefinitionEnd))
                break;
            readKey();
            skipToken();
        }
        makeParkCellAnchors(address);
        zge::endScript();
        zge::closeFile(memoryFile, 0);
        editFree(text);
    }
    READABLE_FUNCTION_WAITS(readParkMap, Verified);

    // buildParkCells: the pieces a loaded map or saved park holds, made cell by cell.
    void buildParkCells(uint32_t park, uint32_t catalog)
    {
        ee::Ptr<EditPark>(park)->catalog = catalog;
        game::tail(forEachParkCell, park, kBuildParkCell, catalog);
    }
    READABLE_FUNCTION_WAITS(buildParkCells, Verified);

    namespace
    {
        // Bug kept: the index a cell's y adds is y * (width + depth), not
        // y * width * depth, so it agrees with the grid's order only at y 0.
        // Every shipped map is one cell high.
        uint32_t cellNumber(const ee::Ptr<EditPark> park, ee::Ptr<const CellIndex> cell)
        {
            return static_cast<uint32_t>(cell->x) + static_cast<uint32_t>(cell->z + cell->y) * static_cast<uint32_t>(park->width) +
                   static_cast<uint32_t>(cell->y) * static_cast<uint32_t>(park->depth);
        }
    }

    uint32_t parkCell(uint32_t address, ee::Ptr<const CellIndex> indices)
    {
        const ee::Ptr<EditPark> park(address);
        return park->cells + cellNumber(park, indices) * static_cast<uint32_t>(sizeof(ParkCell));
    }
    READABLE_FUNCTION(parkCell, Verified);

    uint32_t parkCellDef(uint32_t address, ee::Ptr<const CellIndex> indices)
    {
        const ee::Ptr<EditPark> park(address);
        return park->cellDefs + cellNumber(park, indices) * static_cast<uint32_t>(sizeof(ParkCellDef));
    }
    READABLE_FUNCTION(parkCellDef, Verified);

    int32_t parkCellEmpty(uint32_t /*park*/, uint32_t cell)
    {
        return anchorAttachedObject(ee::Ptr<ParkCell>(cell)->anchor) == 0 ? 1 : 0;
    }
    READABLE_FUNCTION(parkCellEmpty, Verified);

    // parkCellPosition: a cell's centre in the world: the grid about its origin,
    // z running away, y down from the top by whole cells.
    void parkCellPosition(uint32_t address, ee::Ptr<const CellIndex> indices, ee::Address position)
    {
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<zge::Vec4> out(position);
        const ee::Float half(0.5f);
        const ee::Float cellHeight = park->cellSize.y;
        out->y = -(ee::toFloat(indices->y) * cellHeight) - cellHeight * half;
        out->x = (ee::toFloat(indices->x) - ee::toFloat(park->width - 1) * half) * park->cellSize.x;
        out->z = (ee::toFloat(indices->z) - ee::toFloat(park->depth - 1) * half) * -park->cellSize.z;
        *out = *out + park->origin;
    }
    READABLE_FUNCTION_VU0(parkCellPosition, Verified);

    namespace
    {
        // Half a cell away from zero: the conversion after it truncates, so this
        // is rounding to the nearest cell.
        ee::Float roundedCell(ee::Float value)
        {
            return value < ee::Float(0.0f) ? value - ee::Float(0.5f) : value + ee::Float(0.5f);
        }
    }

    // parkCellAtPosition: the cell a world position falls in.
    void parkCellAtPosition(uint32_t address, uint32_t position, ee::Ptr<CellIndex> out)
    {
        const ee::Ptr<EditPark> park(address);
        const zge::Vec4 fromOrigin = *ee::Ptr<zge::Vec4>(position) - park->origin;
        const ee::Float half(0.5f);
        const ee::Float x = fromOrigin.x / park->cellSize.x + ee::toFloat(park->width - 1) * half;
        out->x = ee::toInt(roundedCell(x));
        const ee::Float z = fromOrigin.z / -park->cellSize.z + ee::toFloat(park->depth - 1) * half;
        out->z = ee::toInt(roundedCell(z));
        out->y = 0;
    }
    READABLE_FUNCTION(parkCellAtPosition, Verified);

    // parkCellStep: a world offset in whole cells.
    void parkCellStep(uint32_t address, uint32_t offset, ee::Ptr<CellIndex> out)
    {
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<zge::Vec3> vector(offset);
        const ee::Float x = vector->x / park->cellSize.x;
        ee::Float roundedX;
        if (x < ee::Float(0.0f))
            roundedX = x - ee::Float(0.5f);
        else
            roundedX = x + ee::Float(0.5f);
        out->x = ee::toInt(roundedX);
        const ee::Float z = vector->z / park->cellSize.z;
        ee::Float roundedZ;
        if (z < ee::Float(0.0f))
            roundedZ = z - ee::Float(0.5f);
        else
            roundedZ = z + ee::Float(0.5f);
        out->z = ee::toInt(roundedZ);
        out->y = 0;
    }
    READABLE_FUNCTION(parkCellStep, Verified);

    int32_t parkStartCell(uint32_t address, ee::Ptr<CellIndex> out)
    {
        const ee::Ptr<EditPark> park(address);
        out->x = static_cast<int32_t>(park->startCell[0]);
        out->y = static_cast<int32_t>(park->startCell[1]);
        out->z = static_cast<int32_t>(park->startCell[2]);
        return out->z;
    }
    READABLE_FUNCTION(parkStartCell, Verified);

    // parkClampCells: how far an offset from a cell gets: steps of one cell on each
    // axis it moves on, on past blocked cells, up to the first free or outside
    // cell (a step off the grid is not blocked, so that ends it too).
    void parkClampCells(uint32_t address, ee::Ptr<const CellIndex> start, ee::Ptr<const CellIndex> wanted, ee::Ptr<CellIndex> out)
    {
        ee::Local<ParkCell> step; // (the original's frame has a cell record's 16 bytes for each)
        ee::Local<ParkCell> cell;
        uint64_t blocked = 0;
        out->x = 0;
        out->y = 0;
        out->z = 0;
        step->x = 0;
        step->y = 0;
        step->z = 0;
        const int32_t wantedX = wanted->x;
        if (wantedX > 0)
            step->x = 1;
        const int32_t wantedY = wanted->y;
        if (wantedY > 0)
            step->y = 1;
        const int32_t wantedZ = wanted->z;
        if (wantedZ > 0)
            step->z = 1;
        if (wantedX < 0)
            step->x = -1;
        if (wantedY < 0)
            step->y = -1;
        if (wantedZ < 0)
            step->z = -1;
        cell->x = start->x;
        cell->y = start->y;
        cell->z = start->z;
        for (;;)
        {
            out->x = out->x + step->x;
            out->y = out->y + step->y;
            out->z = out->z + step->z;
            cell->x = cell->x + step->x;
            cell->y = cell->y + step->y;
            cell->z = cell->z + step->z;
            blocked = static_cast<uint32_t>(parkCellBlocked(address, &cell->index));
            const uint32_t valid = parkCellValid(address, &cell->index);
            if (blocked == 0)
                break;
            if (valid == 0)
                break;
        }
    }
    READABLE_FUNCTION(parkClampCells, Verified);

    // parkStepCell: the world offset that brings an outside cell back onto the
    // grid, axis by axis. Returns the last axis's cells.
    void parkStepCell(uint32_t address, ee::Ptr<const CellIndex> cell, ee::Address offset)
    {
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<zge::Vec3> out(offset);
        out->x = out->y = out->z = ee::Float(0.0f);

        // Cells to come back along one axis, when outside the grid.
        const auto back = [](int32_t index, int32_t size, int32_t &cells)
        {
            if (index >= size)
                cells = size - (index + 1);
            else if (index < 0)
                cells = -index;
            else
                return false;
            return true;
        };
        int32_t cells = 0;
        if (back(cell->x, park->width, cells))
            out->x = ee::toFloat(cells) * park->cellSize.x;
        if (back(cell->z, park->depth, cells))
            out->z = ee::toFloat(cells) * park->cellSize.z;
        if (back(cell->y, park->height, cells))
            out->y = ee::toFloat(cells) * park->cellSize.y;
    }
    READABLE_FUNCTION(parkStepCell, Verified);

    int32_t parkCellBlocked(uint32_t park, ee::Ptr<const CellIndex> indices)
    {
        int32_t result = 0;
        if (static_cast<int32_t>(parkCellValid(park, indices)) != 0)
        {
            const uint32_t def = parkCellDef(park, indices);
            result = ee::Ptr<ParkCellDef>(def)->status == kCellBlocked ? 1 : 0;
        }
        return result;
    }
    READABLE_FUNCTION(parkCellBlocked, Verified);

    uint32_t parkCellValid(uint32_t address, ee::Ptr<const CellIndex> cell)
    {
        const ee::Ptr<EditPark> park(address);
        const int32_t x = cell->x;
        if (x < park->width && x >= 0)
        {
            const int32_t z = cell->z;
            if (z < park->depth && z >= 0 && cell->y < park->height && cell->y >= 0)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(parkCellValid, Verified);

    int32_t parkCellFree(uint32_t park, ee::Ptr<const CellIndex> indices)
    {
        int32_t result = 0;
        if (static_cast<int32_t>(parkCellValid(park, indices)) != 0)
        {
            const uint32_t def = parkCellDef(park, indices);
            result = ee::Ptr<ParkCellDef>(def)->status == 0 ? 1 : 0;
        }
        return result;
    }
    READABLE_FUNCTION(parkCellFree, Verified);

    uint32_t cellAnchor(uint32_t cell)
    {
        return ee::Ptr<ParkCell>(cell)->anchor;
    }
    READABLE_FUNCTION(cellAnchor, Verified);

    // parkCellSize: the park's cell size, for those holding the park.
    uint32_t parkCellSize(uint32_t park)
    {
        return park + static_cast<uint32_t>(offsetof(EditPark, cellSize));
    }
    READABLE_FUNCTION(parkCellSize, Verified);

    // parkFarCorner: the camera's lowest corner: the far left bottom cell's
    // centre less the map's bound offsets.
    void parkFarCorner(uint32_t address, ee::Address corner)
    {
        ee::Local<CellIndex> indices;
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<zge::Vec3> out(corner);
        indices->x = 0;
        indices->y = park->height - 1;
        indices->z = park->depth - 1;
        parkCellPosition(address, indices, corner);
        out->x = out->x - park->boundOffsetA[0];
        out->z = out->z - park->boundOffsetB[2];
        out->y = out->y - park->boundOffsetB[1];
    }
    READABLE_FUNCTION(parkFarCorner, Verified);

    // parkNearCorner: the highest corner, the near right top cell's and the offsets.
    void parkNearCorner(uint32_t address, ee::Address corner)
    {
        ee::Local<CellIndex> indices;
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<zge::Vec3> out(corner);
        indices->y = 0;
        indices->z = 0;
        indices->x = park->width - 1;
        parkCellPosition(address, indices, corner);
        out->x = out->x + park->boundOffsetB[0];
        out->z = out->z + park->boundOffsetA[2];
        out->y = out->y + park->boundOffsetA[1];
    }
    READABLE_FUNCTION(parkNearCorner, Verified);

    uint32_t parkCellAnchor(uint32_t park, ee::Ptr<const CellIndex> indices)
    {
        return ee::Ptr<ParkCell>(parkCell(park, indices))->anchor;
    }
    READABLE_FUNCTION(parkCellAnchor, Verified);

    // parkHeightIncrement: MapHeightIncrement. No caller in AI; BMX XXX has one.
    ee::Float parkHeightIncrement(uint32_t address)
    {
        return ee::Ptr<EditPark>(address)->heightIncrement;
    }
    READABLE_FUNCTION(parkHeightIncrement, Draft);

    namespace
    {
        namespace tail
        {
        int32_t setEditObjectDisplay(ee::Ptr<EditObjectPart> object, uint32_t display) { return ee::tailCall<int32_t>(kSetEditObjectDisplay, object, display); }
        }
    }

    // collectCellBreakable: a cell's piece taken down for a test run: a record to put it
    // back from, its display let go; the object stays, showing nothing.
    int32_t collectCellBreakable(uint32_t /*park*/, uint32_t address)
    {
        const ee::Ptr<ParkCell> cell(address);
        const uint32_t anchor = cellAnchor(address);
        const ee::Ptr<EditObjectPart> object = anchorAttachedObject(anchor);
        ee::Address result = object;
        if (object)
        {
            result = anchorReserved(anchor);
            if (result == 0)
            {
                const uint32_t record = zge::poolAlloc(*g_breakablePool);
                const ee::Ptr<Breakable> breakable(record);
                breakable->state = 1;
                breakable->cell.x = cell->x;
                breakable->cell.y = cell->y;
                breakable->cell.z = cell->z;
                const uint32_t ownAnchor = cellAnchor(address);
                const uint32_t angles = anchorAngles(ownAnchor);
                breakable->angles = *ee::Ptr<zge::EulerAngles>(angles);
                const uint32_t def = editObjectDef(object);
                breakable->piece = pieceDefIndex(def);
                const uint32_t display = editObjectDisplay(object);
                breakable->node = editDisplayNode(display);
                const uint32_t sameDisplay = editObjectDisplay(object);
                freeEditDisplay(sameDisplay);
                return tail::setEditObjectDisplay(object, 0);
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(collectCellBreakable, Verified);

    // readParkCellDef: one <Status: Object: Height: Orient: > of MapData: its
    // anchor, at the cell's floor, accepting the Object piece if any.
    int32_t readParkCellDef(uint32_t address, ee::Ptr<const CellIndex> cell, uint32_t def)
    {
        struct CellLines
        {
            int32_t status, object, height, orient;
        };
        ee::Local<char[0x100]> token;
        ee::Local<zge::Vec4> position;
        ee::Local<CellLines> lines;
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<ParkCellDef> out(def);
        const auto isKey = [&](uint32_t key) {
            return zge::stringCompareNoCase(key, token) == 0;
        };
        do
        {
            zge::nextToken(token, 0x100);
            if (isKey(kCellStatus))
                zge::readNumberToken(&lines->status);
            else if (isKey(kCellObject))
                zge::readNumberToken(&lines->object);
            else if (isKey(kCellHeight))
                zge::readNumberToken(&lines->height);
            else if (isKey(kCellOrient))
                zge::readNumberToken(&lines->orient);
        } while (!isKey(kCellEnd));
        const uint32_t anchor = newEditObject(kEditAnchorName, 0);
        parkCellPosition(address, cell, position);
        position->y = position->y + park->cellSize.y * ee::Float(0.5f);
        setAnchorPosition(anchor, position);
        if (lines->object >= 0)
            addAnchorAccept(anchor, lines->object);
        const int32_t angle = lines->orient << kOrientAngle;
        out->anchor = anchor;
        out->orient = static_cast<uint16_t>(angle);
        out->status = static_cast<uint32_t>(lines->status);
        out->height = lines->height;
        return angle;
    }
    READABLE_FUNCTION_WAITS(readParkCellDef, Verified);

    // buildParkCell: a free cell filled with what its anchor accepts: a piece on
    // the anchor, or a tower: four cells make one, the first of them holding the
    // shared display. Raised by the map's Height.
    int32_t buildParkCell(uint32_t address, uint32_t cell, uint32_t def, uint32_t catalog)
    {
        ee::Local<zge::Vec4> raise;
        ee::Local<CellIndex> firstOfFour;
        const ee::Ptr<EditPark> park(address);
        const ee::Ptr<ParkCell> self(cell);
        if (parkCellEmpty(address, cell) == 0)
            return 0;
        const uint32_t accepts = anchorAcceptList(self->anchor);
        for (uint32_t entry = acceptListFirst(accepts); entry != 0; entry = acceptEntryNext(entry))
        {
            const uint32_t index = acceptEntryValue(entry);
            const uint32_t piece = catalogPieceDef(catalog, index);
            const uint32_t className = static_cast<uint32_t>(pieceDefClassName(piece));
            const uint32_t part = newEditObject(className, piece);
            const ee::Ptr<EditObjectPart> object(editObjectPart(part, kEditObjectName));
            const uint32_t sameName = static_cast<uint32_t>(pieceDefClassName(piece));
            if (zge::compareStrings(sameName, kEditTowerObjectName) == 0)
            {
                const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(object, kEditTowerObjectName));
                attachToAnchor(self->anchor, object);

                // Which of the four: 1 on even x and z.
                int32_t state;
                if ((self->x & 1) == 0)
                    state = (self->z & 1) != 0 ? 2 : 1;
                else
                    state = (self->z & 1) == 0 ? 3 : 4;
                setTowerState(tower, state);
                setTowerTop(tower, ee::toFloat(-park->defaultHeight) * park->heightIncrement);
                setTowerBottom(tower, ee::toFloat(park->maximumHeight - park->defaultHeight) * park->heightIncrement);
                if (state == 1)
                {
                    makeEditObjectDisplay(object, 0x800);
                    freeTowerCollision(object);
                }
                else
                {
                    firstOfFour->x = self->x & -2;
                    firstOfFour->y = self->y;
                    firstOfFour->z = self->z & -2;
                    const uint32_t firstCell = parkCell(address, firstOfFour);
                    const uint32_t firstAnchor = cellAnchor(firstCell);
                    const ee::Ptr<EditObjectPart> firstObject = anchorAttachedObject(firstAnchor);
                    const uint32_t display = editObjectDisplay(firstObject);
                    setEditObjectDisplay(object, display);
                }
            }
            else
            {
                makeEditObjectDisplay(object, 0);
                placeEditPiece(self->anchor, object);
            }
            const ee::Float raised = ee::toFloat(ee::Ptr<ParkCellDef>(def)->height);
            raise->x = raise->z = ee::Float(0.0f);
            raise->y = -park->heightIncrement * raised;
            translateEditObject(object, raise);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(buildParkCell, Verified);

    // saveParkCell: a cell into a saved park: a tower saves itself; else a mark,
    // and what the anchor holds if the piece is the player's.
    void saveParkCell(uint32_t /*park*/, uint32_t cell, uint32_t /*def*/, uint32_t stream)
    {
        ee::Local<uint8_t> bytes;
        ee::Local<uint8_t> bytes2;
        ee::Local<uint8_t> bytes3;
        const uint32_t anchor = cellAnchor(cell);
        const ee::Ptr<EditObjectPart> object = anchorAttachedObject(anchor);
        if (object != 0)
        {
            const ee::Ptr<const char> className = editObjectClassName(object);
            if (zge::compareStrings(kEditTowerObjectName, className) == 0)
                saveEditObject(object, stream);
            else
            {
                if (anchorHoldsPiece(anchor) != 0)
                {
                    *bytes = kCellHoldsPiece;
                    parkStreamWrite(stream, bytes, 1);
                    saveAnchor(anchor, stream);
                }
                else
                {
                    *bytes2 = kCellHoldsNothing;
                    parkStreamWrite(stream, bytes2, 1);
                }
            }
        }
        else
        {
            *bytes3 = kCellHoldsNothing;
            parkStreamWrite(stream, bytes3, 1);
        }
    }
    READABLE_FUNCTION_WAITS(saveParkCell, Verified);

    // loadParkCell: a cell from a saved park.
    int32_t loadParkCell(uint32_t /*park*/, uint32_t cell, uint32_t /*def*/, uint32_t stream)
    {
        ee::Local<uint8_t> bytes;
        const uint32_t anchor = cellAnchor(cell);
        const ee::Ptr<EditObjectPart> object = anchorAttachedObject(anchor);
        int32_t result;
        if (object != 0)
        {
            const ee::Ptr<const char> className = editObjectClassName(object);
            result = zge::compareStrings(kEditTowerObjectName, className);
            if (result == 0)
                result = loadEditObject(object, stream);
        }
        else
        {
            parkStreamRead(stream, bytes, 1);
            result = kCellHoldsPiece;
            if (*bytes == kCellHoldsPiece)
                result = loadAnchor(anchor, stream);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(loadParkCell, Verified);

    int32_t resetParkCell(uint32_t /*park*/, uint32_t cell)
    {
        const uint32_t anchor = cellAnchor(cell);
        return game::tail(broadcastEditMessage, anchor, kHideMessage, 0);
    }
    READABLE_FUNCTION_WAITS(resetParkCell, Verified);

    int32_t findPieceInCell(uint32_t /*park*/, uint32_t cell, uint32_t /*def*/, uint32_t search)
    {
        const uint32_t anchor = cellAnchor(cell);
        return game::tail(broadcastEditMessage, anchor, kFindMessage, search);
    }
    READABLE_FUNCTION_WAITS(findPieceInCell, Verified);

    // forEachParkCell: visit(park, cell, its map entry, context) over the grid.
    void forEachParkCell(uint32_t address, uint32_t visit, ee::Address context)
    {
        const ee::Ptr<EditPark> park(address);
        const ParkCellVisit visitCell(visit);
        uint32_t index = 0;
        for (int32_t y = 0; y < park->height; ++y)
        {
            for (int32_t z = 0; z < park->depth; ++z)
            {
                for (int32_t x = 0; x < park->width; ++x)
                {
                    const uint32_t entry = index * sizeof(ParkCell);
                    ++index;
                    visitCell(address, park->cells + entry, park->cellDefs + entry, context);
                }
            }
        }
    }
    READABLE_FUNCTION(forEachParkCell, Verified);

    // findParkPiece: the anchor message's search over the cells: the second word
    // comes back as the find.
    ee::Ptr<EditObjectPart> findParkPiece(uint32_t park, uint32_t piece)
    {
        ee::Local<PieceSearch> search;
        search->piece = piece;
        search->found = 0;
        forEachParkCell(park, kFindPieceInCell, search);
        return search->found;
    }
    READABLE_FUNCTION_WAITS(findParkPiece, Verified);

    // openParkStream: a stream on a saved park's bytes. Writing puts the magic word
    // down; reading skips it unchecked.
    uint32_t openParkStream(uint32_t data, uint32_t size, uint32_t writing)
    {
        ee::Local<uint8_t[0x4]> bytes;
        ee::Local<uint32_t> bytes2;
        const uint32_t address = editAllocate(static_cast<uint32_t>(sizeof(ParkStream)));
        const ee::Ptr<ParkStream> stream(address);
        stream->data = data;
        stream->size = size;
        stream->writing = writing;
        if (writing == 0)
            parkStreamRead(address, bytes, 4);
        if (writing == 1)
        {
            *bytes2 = kParkStreamMagic;
            parkStreamWrite(address, bytes2, 4);
        }
        return address;
    }
    READABLE_FUNCTION_WAITS(openParkStream, Verified);

    int32_t closeParkStream(uint32_t stream)
    {
        return game::tail(editFree, stream);
    }
    READABLE_FUNCTION_WAITS(closeParkStream, Verified);

    void parkStreamWrite(uint32_t address, ee::Address bytes, uint32_t count)
    {
        const ee::Ptr<ParkStream> stream(address);
        const ee::Ptr<uint8_t> source(bytes);
        for (uint32_t i = 0; i < count; ++i)
        {
            ee::Ptr<uint8_t>(stream->data)[stream->position] = source[i];
            stream->position = stream->position + 1;
        }
    }
    READABLE_FUNCTION(parkStreamWrite, Verified);

    void parkStreamRead(uint32_t address, ee::Address bytes, uint32_t count)
    {
        const ee::Ptr<ParkStream> stream(address);
        const ee::Ptr<uint8_t> destination(bytes);
        for (uint32_t i = 0; i < count; ++i)
        {
            destination[i] = ee::Ptr<uint8_t>(stream->data)[stream->position];
            stream->position = stream->position + 1;
        }
    }
    READABLE_FUNCTION(parkStreamRead, Verified);
}
