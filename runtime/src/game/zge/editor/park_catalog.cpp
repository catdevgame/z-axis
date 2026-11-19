// The park editor's catalog of pieces (were the sweep's blocks_001f2e90):
// the piece file's text ("Inline.Pep": CategoryListBegin and ObjectListBegin
// blocks, to ObjectFileEnd) made into categories and piece definitions,
// each piece measured in grid cells from the world object of its
// MaxObjectName, and the anchors its scene's "EditAnchor" locators give it.

#include "aggressive/conversions.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "zge/editor/edit_records.h"
#include "zge/editor/game.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/box.h"
#include "zge/math/types.h"
#include "zge/scene/locators.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/collision/collision.h"
#include "zge/scene/scene_database.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/editor/park_catalog.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kTextEmpty = sym::textEmpty;
        constexpr uint32_t kTextCategoryDefBegin = sym::g_textCategoryDefBegin;
        constexpr uint32_t kTextCategoryListBegin = sym::g_textCategoryListBegin;
        constexpr uint32_t kTextCategoryDefEnd = sym::g_textCategoryDefEnd;
        constexpr uint32_t kTextName = sym::g_textName;
        constexpr uint32_t kTextDescription = sym::g_textDescription2;
        constexpr uint32_t kTextIconChar = sym::textIconChar;
        constexpr uint32_t kTextSystemFlag = sym::g_textSystemFlag;
        constexpr uint32_t kTextCategoryListEnd = sym::g_textCategoryListEnd;
        constexpr uint32_t kTextObjectDefEnd = sym::g_textObjectDefEnd;
        constexpr uint32_t kTextMaxObjectName = sym::g_textMaxObjectName;
        constexpr uint32_t kTextCategoryIndex = sym::g_textCategoryIndex;
        constexpr uint32_t kTextSecretId = sym::g_textSecretID;
        constexpr uint32_t kTextClassName = sym::g_textClassName;
        constexpr uint32_t kTextChildListBegin = sym::g_textChildListBegin;
        constexpr uint32_t kTextChildListEnd = sym::textChildListEnd;
        constexpr uint32_t kTextObjectListEnd = sym::g_textObjectListEnd;
        constexpr uint32_t kTextObjectDefBegin = sym::g_textObjectDefBegin;
        constexpr uint32_t kTextObjectFileEnd = sym::g_textObjectFileEnd;
        constexpr uint32_t kTextObjectListBegin = sym::g_textObjectListBegin;
        constexpr uint32_t kEditObjectName = sym::g_editObjectName;
        constexpr uint32_t kEditBaseClassName = sym::g_editObjectEditBaseClassName;
        constexpr uint32_t kEditTowerObjectName = sym::g_editTowerObjectName;

        // The edit object classes' functions (level/park_objects, still the sweep's).
        constexpr uint32_t kConstructEditObject = sym::constructEditObject;
        constexpr uint32_t kDestructEditObject = sym::destructEditObject;
        constexpr uint32_t kEditObjectMessage = sym::editObjectMessage;
        constexpr uint32_t kConstructEditTower = sym::constructEditTowerObject;
        constexpr uint32_t kDestructEditTower = sym::destructEditTowerObject;
        constexpr uint32_t kMaxPieces = 1024, kCategories = 32;

        constexpr uint32_t kTokenBytes = 0x100;
    }

    int32_t freePieceCategories(uint32_t categories);

    int32_t freePieceDefs(ee::Ptr<uint32_t> table);

    void freePieceCatalog(uint32_t catalog)
    {
        const ee::Ptr<PieceCatalog> self(catalog);
        freePieceCategories(self->categories);
        freePieceDefs(self->pieces);
        game::tail(editFree, catalog);
    }
    READABLE_FUNCTION_WAITS(freePieceCatalog, Verified);

    int32_t newPieceCategories()
    {
        const uint32_t table = editAllocate(kCategories * static_cast<uint32_t>(sizeof(PieceCategory)));
        const ee::Ptr<PieceCategory> categories(table);
        for (uint32_t i = 0; i < kCategories; ++i)
        {
            zge::initList(categories + i);
            zge::stringCopy(categories[i].name, kTextEmpty);
            zge::stringCopy(categories[i].description, kTextEmpty);
            categories[i].systemFlag = 0;
        }
        return static_cast<int32_t>(table);
    }
    READABLE_FUNCTION_WAITS(newPieceCategories, Verified);

    int32_t freePieceCategories(uint32_t categories)
    {
        return game::tail(editFree, categories);
    }
    READABLE_FUNCTION_WAITS(freePieceCategories, Verified);

    // readCategoryDef: the next category, from the script, to "CategoryDefEnd".
    int32_t readCategoryDef(uint32_t catalog)
    {
        ee::Local<int8_t> token;
        ee::Local<uint32_t> value;
        const ee::Ptr<PieceCatalog> self(catalog);
        const ee::Ptr<PieceCategory> category(self->categories + self->categoryCount * sizeof(PieceCategory));
        const auto is = [&](uint32_t name) {
            return zge::stringCompareNoCase(name, token) == 0;
        };
        const auto copyNext = [&](char *to) {
            zge::nextToken(token, kTokenBytes);
            zge::stringCopy(to, token);
        };
        zge::nextToken(token, kTokenBytes);
        while (!is(kTextCategoryDefEnd))
        {
            if (is(kTextName))
                copyNext(category->name);
            else if (is(kTextDescription))
                copyNext(category->description);
            if (is(kTextIconChar))
            {
                zge::nextToken(token, kTokenBytes);
                category->iconChar = *token;
            }
            else if (is(kTextSystemFlag))
            {
                zge::readNumberToken(value);
                category->systemFlag = *value;
            }
            zge::nextToken(token, kTokenBytes);
        }
        self->categoryCount += 1;
        return static_cast<int32_t>(self->categoryCount);
    }
    READABLE_FUNCTION_WAITS(readCategoryDef, Verified);

    int32_t readCategoryList(uint32_t catalog)
    {
        ee::Local<uint8_t[0x140]> token;
        zge::nextToken(token, kTokenBytes);
        for (bool first = true;; first = false)
        {
            if (!first)
            {
                if (zge::stringCompareNoCase(kTextCategoryDefBegin, token) == 0)
                    readCategoryDef(catalog);
                zge::nextToken(token, kTokenBytes);
            }
            if (zge::stringCompareNoCase(kTextCategoryListEnd, token) == 0)
                return 0;
        }
    }
    READABLE_FUNCTION_WAITS(readCategoryList, Verified);

    uint32_t categorySystemFlag(uint32_t category)
    {
        return ee::Ptr<PieceCategory>(category)->systemFlag;
    }
    READABLE_FUNCTION(categorySystemFlag, Verified);

    int32_t categoryName(uint32_t category)
    {
        return static_cast<int32_t>(category + offsetof(PieceCategory, name));
    }
    READABLE_FUNCTION(categoryName, Verified);

    uint32_t categoryIconChar(uint32_t category)
    {
        return static_cast<uint32_t>(static_cast<int32_t>(ee::Ptr<PieceCategory>(category)->iconChar));
    }
    READABLE_FUNCTION(categoryIconChar, Verified);

    int32_t newPieceDefTable()
    {
        const uint32_t table = editAllocate(kMaxPieces * 4u);
        const ee::Ptr<uint32_t> pieces(table);
        for (uint32_t i = 0; i < kMaxPieces; ++i)
            pieces[i] = 0;
        return static_cast<int32_t>(table);
    }
    READABLE_FUNCTION_WAITS(newPieceDefTable, Verified);

    int32_t freePieceDefs(ee::Ptr<uint32_t> table)
    {
        for (uint32_t i = 0; i < kMaxPieces; ++i)
        {
            const ee::Ptr<PieceDef> piece(table[i]);
            if (piece)
            {
                deleteAnchorList(piece->anchors);
                freeAnchorList(piece->anchors);
                editFree(piece);
            }
        }
        return game::tail(editFree, table);
    }
    READABLE_FUNCTION_WAITS(freePieceDefs, Verified);

    // readObjectDef: the next piece, to "ObjectDefEnd". A ChildList is read
    // past and left unused.
    int32_t readObjectDef(uint32_t catalog)
    {
        ee::Local<uint8_t[0x100]> token;
        ee::Local<uint32_t> value;
        ee::Local<uint32_t> value2;
        const ee::Ptr<PieceCatalog> self(catalog);
        const uint32_t pieceAddress = editAllocate(static_cast<uint32_t>(sizeof(PieceDef)));
        const ee::Ptr<PieceDef> piece(pieceAddress);
        zge::nextToken(token, kTokenBytes);
        const auto is = [&](uint32_t name) {
            return zge::stringCompareNoCase(name, token) == 0;
        };
        for (bool first = true;; first = false)
        {
            if (!first)
            {
                // A token handled by name or file goes past the join.
                if (is(kTextName))
                {
                    zge::nextToken(token, kTokenBytes);
                    zge::stringCopy(pieceAddress + offsetof(PieceDef, name), token);
                }
                else
                {
                    if (is(kTextMaxObjectName))
                    {
                        zge::nextToken(token, kTokenBytes);
                        piece->objectHash = zge::nameHash(ee::Ptr<const char>(token.address()));
                    }
                    else
                    {
                        if (is(kTextCategoryIndex))
                        {
                            zge::readNumberToken(value);
                            piece->category = *value;
                        }
                        else
                        {
                            if (is(kTextSecretId))
                            {
                                zge::readNumberToken(value2);
                                piece->secretId = *value2;
                            }
                            else
                            {
                                if (is(kTextClassName))
                                    zge::nextToken(pieceAddress + offsetof(PieceDef, className), 0x40);
                                else
                                {
                                    if (is(kTextChildListBegin))
                                    {
                                        for (;;)
                                        {
                                            zge::nextToken(token, kTokenBytes);
                                            if (is(kTextChildListEnd))
                                                break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                zge::nextToken(token, kTokenBytes);
            }
            if (is(kTextObjectDefEnd))
                break;
        }
        piece->index = self->pieceCount;
        self->pieces[self->pieceCount] = pieceAddress;
        self->pieceCount += 1;
        return static_cast<int32_t>(self->pieceCount);
    }
    READABLE_FUNCTION_WAITS(readObjectDef, Verified);

    // readObjectList: the pieces, to "ObjectListEnd"; then each into its
    // category's list. The result is 0.
    int32_t readObjectList(uint32_t catalog)
    {
        ee::Local<uint8_t[0x140]> token;
        const ee::Ptr<PieceCatalog> self(catalog);
        zge::nextToken(token, kTokenBytes);
        for (bool first = true;; first = false)
        {
            if (!first)
            {
                if (zge::stringCompareNoCase(kTextObjectDefBegin, token) == 0)
                    readObjectDef(catalog);
                zge::nextToken(token, kTokenBytes);
            }
            if (zge::stringCompareNoCase(kTextObjectListEnd, token) == 0)
                break;
        }
        if (self->pieceCount != 0)
        {
            for (uint32_t i = 0;;)
            {
                const uint32_t pieceAddress = self->pieces[i];
                ++i;
                zge::listPush(self->categories + ee::Ptr<PieceDef>(pieceAddress)->category * sizeof(PieceCategory), pieceAddress);
                if (!(i < self->pieceCount))
                    break;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(readObjectList, Verified);

    void addPieceSceneFile(uint32_t /*catalog*/, uint32_t name)
    {
        if (zge::compareStrings(name, kTextEmpty) != 0)
        {
            game::tail(zge::addSceneFile, name);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(addPieceSceneFile, Verified);

    // readPieceFile: a piece file read whole and run through the token
    // reader. A name of "" is no file.
    void readPieceFile(uint32_t catalog, uint32_t name)
    {
        ee::Local<uint8_t[0x100]> token;
        ee::Local<uint32_t> bytes;
        if (zge::compareStrings(name, kTextEmpty) == 0)
            return;
        const ee::Ptr<zge::ZFile> file = zge::openFile(name, zge::kFileModeRead, 0).file();
        zge::fileSize(file, bytes, 0);
        const uint32_t text = editAllocate(*bytes);
        zge::fileRead(file, text, *bytes, 0);
        zge::closeFile(file, 0);
        const ee::Ptr<zge::ZFile> memoryFile = zge::openMemoryFile(text, *bytes);
        zge::beginScript(zge::filePath(memoryFile));
        zge::setTokenCapitalize(0);
        zge::nextToken(token, kTokenBytes);
        while (zge::stringCompareNoCase(kTextObjectFileEnd, token) != 0)
        {
            if (zge::stringCompareNoCase(kTextCategoryListBegin, token) == 0)
                readCategoryList(catalog);
            else if (zge::stringCompareNoCase(kTextObjectListBegin, token) == 0)
                readObjectList(catalog);
            zge::nextToken(token, kTokenBytes);
        }
        zge::endScript();
        zge::closeFile(memoryFile, 0);
        editFree(text);
    }
    READABLE_FUNCTION_WAITS(readPieceFile, Verified);

    // cellsSpanned: cells a size takes: its whole part, one more when what
    // is over passes `slack`.
    int32_t cellsSpanned(ee::Float cells, ee::Float slack)
    {
        int32_t whole = ee::toInt(cells);
        if (slack < cells - ee::toFloat(whole))
            ++whole;
        return whole;
    }
    READABLE_FUNCTION(cellsSpanned, Verified);

    int32_t findPieceDefByHash(uint32_t catalog, uint32_t hash);

    int32_t addPieceDefAnchor(uint32_t piece, uint32_t anchor);

    int32_t pieceDefClassName(ee::Address piece)
    {
        return static_cast<int32_t>(piece + offsetof(PieceDef, className));
    }
    READABLE_FUNCTION(pieceDefClassName, Verified);

    void pieceDefBox(uint32_t piece, ee::Ptr<zge::Box> box)
    {
        std::memcpy(box, &ee::Ptr<PieceDef>(piece)->bounds, sizeof(zge::Box));
    }
    READABLE_FUNCTION(pieceDefBox, Verified);

    void pieceDefFootprint(uint32_t piece, ee::Ptr<int32_t> cellsX, ee::Ptr<int32_t> cellsY, ee::Ptr<int32_t> cellsZ)
    {
        const ee::Ptr<PieceDef> self(piece);
        *cellsX = self->cellsX;
        *cellsY = self->cellsY;
        *cellsZ = self->cellsZ;
    }
    READABLE_FUNCTION(pieceDefFootprint, Verified);

    void pieceDefCellOffset(uint32_t piece, ee::Ptr<int32_t> originX, ee::Ptr<int32_t> originY, ee::Ptr<int32_t> originZ)
    {
        const ee::Ptr<PieceDef> self(piece);
        *originX = self->originCellX;
        *originY = self->originCellY;
        *originZ = self->originCellZ;
    }
    READABLE_FUNCTION(pieceDefCellOffset, Verified);

    uint32_t pieceDefAnchors(uint32_t piece)
    {
        return ee::Ptr<PieceDef>(piece)->anchors;
    }
    READABLE_FUNCTION(pieceDefAnchors, Verified);

    uint32_t pieceDefObjectHash(uint32_t piece)
    {
        return ee::Ptr<PieceDef>(piece)->objectHash;
    }
    READABLE_FUNCTION(pieceDefObjectHash, Verified);

    // pieceDefName: the piece's name (its "Name").
    ee::Ptr<const char> pieceDefName(ee::Ptr<PieceDef> piece)
    {
        return piece->name;
    }
    READABLE_FUNCTION(pieceDefName, Verified);

    // pieceDefZero: always 0 (the object tool asks it beside pieceDefName).
    uint32_t pieceDefZero()
    {
        return 0;
    }
    READABLE_FUNCTION(pieceDefZero, Verified);

    uint32_t catalogPieceDef(uint32_t catalog, uint32_t index)
    {
        return ee::Ptr<PieceCatalog>(catalog)->pieces[index];
    }
    READABLE_FUNCTION(catalogPieceDef, Verified);

    uint32_t findPieceDef(uint32_t catalog, uint32_t name)
    {
        const ee::Ptr<PieceCatalog> self(catalog);
        uint32_t found = 0;
        for (uint32_t i = 0;;)
        {
            if (i >= self->pieceCount)
                break;
            if (zge::stringCompareNoCase(name, self->pieces[i] + offsetof(PieceDef, name)) == 0)
            {
                found = self->pieces[i];
                break;
            }
            ++i;
        }
        return found;
    }
    READABLE_FUNCTION(findPieceDef, Verified);

    int32_t findPieceDefByHash(uint32_t catalog, uint32_t hash)
    {
        const ee::Ptr<PieceCatalog> self(catalog);
        const uint32_t count = self->pieceCount;
        uint32_t found = 0;
        if (count != 0)
        {
            const ee::Ptr<uint32_t> pieces = self->pieces;
            if (hash == ee::Ptr<PieceDef>(pieces[0])->objectHash)
                found = pieces[0];
            else
            {
                for (uint32_t i = 1;;)
                {
                    if (i >= count)
                        break;
                    const uint32_t piece = pieces[i];
                    ++i;
                    if (hash == ee::Ptr<PieceDef>(piece)->objectHash)
                    {
                        found = piece;
                        break;
                    }
                }
            }
        }
        return static_cast<int32_t>(found);
    }
    READABLE_FUNCTION(findPieceDefByHash, Verified);

    int32_t catalogCategory(uint32_t catalog, uint32_t index)
    {
        return static_cast<int32_t>(ee::Ptr<PieceCatalog>(catalog)->categories + index * sizeof(PieceCategory));
    }
    READABLE_FUNCTION(catalogCategory, Verified);

    uint32_t catalogCategoryCount(uint32_t catalog)
    {
        return ee::Ptr<PieceCatalog>(catalog)->categoryCount;
    }
    READABLE_FUNCTION(catalogCategoryCount, Verified);

    uint32_t pieceDefIndex(uint32_t piece)
    {
        return ee::Ptr<PieceDef>(piece)->index;
    }
    READABLE_FUNCTION(pieceDefIndex, Verified);

    int32_t addPieceDefAnchor(uint32_t piece, uint32_t anchor)
    {
        return game::tail(appendAnchor, ee::Ptr<PieceDef>(piece)->anchors, anchor);
    }
    READABLE_FUNCTION(addPieceDefAnchor, Verified);

    ee::Ptr<PieceDef> categoryFirstPiece(uint32_t category)
    {
        return zge::recordOfNode<PieceDef>(ee::Ptr<PieceCategory>(category)->pieces.head);
    }
    READABLE_FUNCTION(categoryFirstPiece, Verified);

    ee::Ptr<PieceDef> categoryNextPiece(uint32_t /*category*/, ee::Ptr<PieceDef> piece)
    {
        return zge::recordOfNode<PieceDef>(piece->link.next);
    }
    READABLE_FUNCTION(categoryNextPiece, Verified);

    ee::Ptr<PieceDef> categoryPreviousPiece(uint32_t /*category*/, ee::Ptr<PieceDef> piece)
    {
        return zge::recordOfNode<PieceDef>(piece->link.prev);
    }
    READABLE_FUNCTION(categoryPreviousPiece, Verified);

    ee::Ptr<PieceDef> categoryLastPiece(uint32_t category)
    {
        return zge::recordOfNode<PieceDef>(ee::Ptr<PieceCategory>(category)->pieces.tail);
    }
    READABLE_FUNCTION(categoryLastPiece, Verified);

    // registerEditObjectClasses: the classes a park's pieces are made of: EditObject,
    // and EditTowerObject (the column under a raised piece) from it.
    void registerEditObjectClasses()
    {
        registerEditClass(kEditObjectName, 0xB0u, kEditBaseClassName, kConstructEditObject, kDestructEditObject, kEditObjectMessage);
        game::tail(registerEditClass, kEditTowerObjectName, 12u, kEditObjectName, kConstructEditTower, kDestructEditTower, kEditObjectMessage);
    }
    READABLE_FUNCTION(registerEditObjectClasses, Verified);

    void setEditObjectOwner(ee::Ptr<EditObjectPart> part, uint32_t owner)
    {
        part->owner = owner;
    }
    READABLE_FUNCTION(setEditObjectOwner, Verified);

    void setEditObjectParentAnchor(ee::Ptr<EditObjectPart> object, uint32_t anchor)
    {
        object->parentAnchor = anchor;
    }
    READABLE_FUNCTION(setEditObjectParentAnchor, Verified);

    uint32_t toolSetCatalog(ee::Ptr<EditToolSet> toolSet)
    {
        return toolSet->catalog;
    }
    READABLE_FUNCTION(toolSetCatalog, Verified);
}
