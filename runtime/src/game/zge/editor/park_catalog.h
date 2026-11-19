#pragma once

// The park editor's catalog of pieces (park_catalog.cpp): categories and
// piece definitions from the theme's piece file.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/route.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/scene/locators.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/collision/collision.h"
#include "zge/scene/scene_database.h"
#include <cstddef>
#include <cstring>
#include "zge/editor/edit_records.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/box.h"

#include <cstdint>

namespace zge::editor
{
    // (park_catalog.cpp)
    void addPieceSceneFile(uint32_t /*catalog*/, uint32_t name);
    int32_t catalogCategory(uint32_t catalog, uint32_t index);
    uint32_t catalogCategoryCount(uint32_t catalog);
    uint32_t catalogPieceDef(uint32_t catalog, uint32_t index);
    ee::Ptr<PieceDef> categoryFirstPiece(uint32_t category);
    uint32_t categoryIconChar(uint32_t category);
    ee::Ptr<PieceDef> categoryLastPiece(uint32_t category);
    int32_t categoryName(uint32_t category);
    ee::Ptr<PieceDef> categoryNextPiece(uint32_t /*category*/, ee::Ptr<PieceDef> piece);
    ee::Ptr<PieceDef> categoryPreviousPiece(uint32_t /*category*/, ee::Ptr<PieceDef> piece);
    uint32_t categorySystemFlag(uint32_t category);
    uint32_t findPieceDef(uint32_t catalog, uint32_t name);
    void freePieceCatalog(uint32_t catalog);
    int32_t newPieceCategories();
    int32_t newPieceDefTable();
    uint32_t pieceDefAnchors(uint32_t piece);
    void pieceDefBox(uint32_t piece, ee::Ptr<zge::Box> box);
    void pieceDefCellOffset(uint32_t piece, ee::Ptr<int32_t> originX, ee::Ptr<int32_t> originY, ee::Ptr<int32_t> originZ);
    int32_t pieceDefClassName(ee::Address piece);
    void pieceDefFootprint(uint32_t piece, ee::Ptr<int32_t> cellsX, ee::Ptr<int32_t> cellsY, ee::Ptr<int32_t> cellsZ);
    uint32_t pieceDefIndex(uint32_t piece);
    ee::Ptr<const char> pieceDefName(ee::Ptr<PieceDef> piece);
    uint32_t pieceDefObjectHash(uint32_t piece);
    uint32_t pieceDefZero();
    void readPieceFile(uint32_t catalog, uint32_t name);
    void registerEditObjectClasses();
    void setEditObjectOwner(ee::Ptr<EditObjectPart> part, uint32_t owner);
    void setEditObjectParentAnchor(ee::Ptr<EditObjectPart> object, uint32_t anchor);
    uint32_t toolSetCatalog(ee::Ptr<EditToolSet> toolSet);

    struct PieceCatalog
    {
        uint32_t ready;         // +0x00
        uint32_t pieceCount;    // +0x04
        ee::Ptr<uint32_t> pieces; // +0x08 kMaxPieces PieceDefs
        uint32_t categoryCount; // +0x0C
        uint32_t categories;    // +0x10 PieceCategories (kCategories)
    };
    static_assert(sizeof(PieceCatalog) == 20);

    struct PieceCategory
    {
        zge::List pieces;      // +0x00 PieceDefs, latest first
        char name[0x40];       // +0x0C "Name"
        char description[0x100]; // +0x4C "Description"
        int8_t iconChar;       // +0x14C the first byte of "IconChar"
        uint8_t _14d[3];
        uint32_t systemFlag;   // +0x150 "SystemFlag"
    };
    static_assert(sizeof(PieceCategory) == 0x154 && offsetof(PieceCategory, iconChar) == 0x14C);

    // (park_catalog.cpp, for the games' own parts)
    int32_t addPieceDefAnchor(uint32_t piece, uint32_t anchor);
    int32_t cellsSpanned(ee::Float cells, ee::Float slack);
    int32_t findPieceDefByHash(uint32_t catalog, uint32_t hash);
    int32_t freePieceCategories(uint32_t categories);
    int32_t freePieceDefs(ee::Ptr<uint32_t> table);
    int32_t readCategoryDef(uint32_t catalog);
    int32_t readCategoryList(uint32_t catalog);
    int32_t readObjectDef(uint32_t catalog);
    int32_t readObjectList(uint32_t catalog);
}
