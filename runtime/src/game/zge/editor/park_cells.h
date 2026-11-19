#pragma once

// The park editor's park (park_cells.cpp): its grid of cells and their anchors,
// its stream, the breakables' pool and the tool set's buttons.

#include "zge/editor/breakables.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "zge/scene/node.h"
#include <cstddef>
#include <cstring>
#include "zge/editor/edit_records.h"
#include "zge/memory/pool.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstdint>

namespace zge::editor
{
    void buildParkCells(uint32_t park, uint32_t catalog);
    int32_t closeParkStream(uint32_t stream);
    int32_t collectBreakables(uint32_t park);
    void copyParkOrigin(uint32_t park, ee::Ptr<zge::Vec4> position);
    void deleteParkPieces(uint32_t address);
    ee::Ptr<EditObjectPart> findParkPiece(uint32_t park, uint32_t piece);
    void freeBreakables();
    void freeEditPark(uint32_t address);
    void loadEditPark(uint32_t park, uint32_t stream);
    void loadToolSetCamera(ee::Ptr<EditToolSet> toolSet, uint32_t stream);
    uint32_t newEditPark();
    uint32_t openParkStream(uint32_t data, uint32_t size, uint32_t writing);
    uint32_t parkCellAnchor(uint32_t park, ee::Ptr<const CellIndex> indices);
    void parkCellAtPosition(uint32_t address, uint32_t position, ee::Ptr<CellIndex> indices);
    int32_t parkCellBlocked(uint32_t park, ee::Ptr<const CellIndex> indices);
    int32_t parkCellFree(uint32_t park, ee::Ptr<const CellIndex> indices);
    void parkCellPosition(uint32_t address, ee::Ptr<const CellIndex> cell, ee::Address position);
    uint32_t parkCellSize(uint32_t park);
    void parkCellStep(uint32_t address, uint32_t offset, ee::Ptr<CellIndex> step);
    uint32_t parkCellValid(uint32_t address, ee::Ptr<const CellIndex> indices);
    void parkClampCells(uint32_t address, ee::Ptr<const CellIndex> start, ee::Ptr<const CellIndex> offset, ee::Ptr<CellIndex> clamped);
    void parkFarCorner(uint32_t address, ee::Address corner);
    void parkNearCorner(uint32_t address, ee::Address corner);
    int32_t parkStartCell(uint32_t address, ee::Ptr<CellIndex> indices);
    void parkStepCell(uint32_t address, ee::Ptr<const CellIndex> indices, ee::Address offset);
    void parkStreamRead(uint32_t address, ee::Address bytes, uint32_t count);
    void parkStreamWrite(uint32_t address, ee::Address bytes, uint32_t count);
    void readParkMap(uint32_t address, uint32_t path);
    int32_t resetParkCells(uint32_t park);
    void saveEditPark(uint32_t park, uint32_t stream);
    int32_t saveToolSetCamera(ee::Ptr<EditToolSet> toolSet, uint32_t stream);
    void setCameraButtons(ee::Address address);
    void setToolSetCameraActive(ee::Ptr<EditToolSet> toolSet, uint32_t active);
    uint32_t toolSetUi(ee::Ptr<EditToolSet> toolSet);

    struct EditPark
    {
        zge::Vec4 origin;     // +0x00 where the park's first cell is
        zge::Vec3 cellSize;   // +0x10
        uint32_t _1c;
        uint32_t startCell[3]; // +0x20
        uint32_t flag2c;      // +0x2C 1 when made
        int32_t width;        // +0x30
        int32_t depth;        // +0x34
        int32_t height;       // +0x38
        uint32_t cells;       // +0x3C
        uint32_t cellDefs;    // +0x40 the map's entries, as many as cells
        uint8_t _44[0xC];
        ee::Float boundOffsetA[3]; // +0x50 MapCameraBoundOffset's first three: x past the far corner, y and z past the near one
        uint32_t _5c;
        ee::Float boundOffsetB[3]; // +0x60 its last three: x past the near corner, y and z past the far one
        uint32_t _6c;
        int32_t defaultHeight; // +0x70 MapDefaultHeight: a tower's top sits this many steps down
        int32_t maximumHeight; // +0x74 MapMaximumHeight (20 when made)
        ee::Float heightIncrement; // +0x78 MapHeightIncrement (1.0 when made)
        uint32_t catalog;     // +0x7C
    };
    static_assert(sizeof(EditPark) == 0x80 && offsetof(EditPark, width) == 0x30 && offsetof(EditPark, boundOffsetA) == 0x50 &&
                  offsetof(EditPark, boundOffsetB) == 0x60 && offsetof(EditPark, defaultHeight) == 0x70 &&
                  offsetof(EditPark, catalog) == 0x7C);

    // A cell of the grid, in the order forEachParkCell walks them.
    struct ParkCell
    {
        union
        {
            CellIndex index;  // +0x00 its place in the grid
            struct
            {
                int32_t x, y, z;
            };
        };
        uint32_t anchor;      // +0x0C its anchor
    };
    static_assert(sizeof(ParkCell) == 0x10);

    // A cell's entry in the map's MapData, in the same order.
    struct ParkCellDef
    {
        uint32_t status;      // +0x00 0 free, kCellBlocked
        int32_t height;       // +0x04 Height: steps the piece stands raised
        uint32_t anchor;      // +0x08 an anchor, "EditAnchor" with the position and what the map says it accepts
        uint16_t orient;      // +0x0C Orient as an angle
        uint16_t _0e;
    };
    static_assert(sizeof(ParkCellDef) == 0x10 && offsetof(ParkCellDef, orient) == 0xC);

    // A saved park: the card's buffer read or written byte by byte.
    struct ParkStream
    {
        uint32_t data;        // +0x00
        uint32_t position;    // +0x04 (the block comes zeroed)
        uint32_t size;        // +0x08
        uint32_t writing;     // +0x0C 1 write, 0 read
    };
    static_assert(sizeof(ParkStream) == 0x10);

    // (park_cells.cpp, for the games' own parts)
    void allocateParkCells(uint32_t address);
    uint32_t cellAnchor(uint32_t cell);
    void forEachParkCell(uint32_t address, uint32_t visit, ee::Address context);
    void makeParkCellAnchors(uint32_t address);
    uint32_t parkCell(uint32_t address, ee::Ptr<const CellIndex> indices);
    uint32_t parkCellDef(uint32_t address, ee::Ptr<const CellIndex> indices);
    int32_t parkCellEmpty(uint32_t , uint32_t cell);
    int32_t readParkCellDefs(uint32_t park);
    void readParkUserData();
    ee::Float parkHeightIncrement(uint32_t address);
}
