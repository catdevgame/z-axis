// The park editor's catalog of pieces (were the sweep's blocks_001f2e90):
// the piece file's text ("Inline.Pep": CategoryListBegin and ObjectListBegin
// blocks, to ObjectFileEnd) made into categories and piece definitions,
// each piece measured in grid cells from the world object of its
// MaxObjectName, and the anchors its scene's "EditAnchor" locators give it.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_catalog.cpp.

#include "aggressive/conversions.h"
#include "zge/editor/park_catalog.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "aggressive/level/edit_records.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/box.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/scene/locators.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/collision/collision.h"
#include "zge/scene/scene_database.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/park_catalog.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kListInit = sym::initList;
        constexpr uint32_t kListPush = sym::listPush;
        constexpr uint32_t kEditAllocate = sym::editAllocate;
        constexpr uint32_t kEditFree = sym::editFree;
        constexpr uint32_t kRegisterEditClass = sym::registerEditClass;
        constexpr uint32_t kNewEditObject = sym::newEditObject;
        constexpr uint32_t kFreePieceCategories = sym::freePieceCategories;
        constexpr uint32_t kReadCategoryDef = sym::readCategoryDef;
        constexpr uint32_t kReadCategoryList = sym::readCategoryList;
        constexpr uint32_t kFreePieceDefs = sym::freePieceDefs;
        constexpr uint32_t kReadObjectDef = sym::readObjectDef;
        constexpr uint32_t kReadObjectList = sym::readObjectList;
        constexpr uint32_t kCellsSpanned = sym::cellsSpanned;
        constexpr uint32_t kFindPieceAnchors = sym::findPieceAnchors;
        constexpr uint32_t kFindPieceDefByHash = sym::findPieceDefByHash;
        constexpr uint32_t kPieceDefIndex = sym::pieceDefIndex;
        constexpr uint32_t kAddPieceDefAnchor = sym::addPieceDefAnchor;
        constexpr uint32_t kNewEditList = sym::newEditList;
        constexpr uint32_t kDeleteAnchorList = sym::deleteAnchorList;
        constexpr uint32_t kFreeAnchorList = sym::freeAnchorList;
        constexpr uint32_t kSetAnchorPosition = sym::setAnchorPosition;
        constexpr uint32_t kSetAnchorAngles = sym::setAnchorAngles;
        constexpr uint32_t kAddAnchorAccept = sym::addAnchorAccept;
        constexpr uint32_t kEditAnchorName = sym::g_pieceAnchorsEditAnchorName;
        constexpr uint32_t kInvalidMaxName = sym::invalidMaxName;      // "Invalid max name in database for object: %s"
        constexpr uint32_t kHeldPage = ee::high(sym::textEmpty);            // the page of those names, held in a saved register
        constexpr uint32_t kFormatsPage = ee::high(sym::g_textCategoryDefBegin);
        constexpr uint32_t kWorldObjectBox = 0x10; // a world object: its bounding box

        // A scene point of kind EditAnchor: a ScenePoint with its tags.
        struct AnchorPoint
        {
            zge::Vec4 position;  // +0x00
            zge::Quaternion rotation; // +0x10
            uint32_t kindHash;   // +0x20
            uint32_t nameHash;   // +0x24
            uint32_t _28;
            uint32_t tags;       // +0x2C AnchorTags
        };
        static_assert(offsetof(AnchorPoint, tags) == 0x2C);

        // An "EditAnchor" locator's tags: the piece it is on and those its
        // anchor takes.
        struct AnchorTags
        {
            uint32_t piece;       // +0x00 a piece's objectHash
            uint32_t accepts[8];  // +0x04 the same, 0 for none
        };
        constexpr ee::Float kCellSlack = 0.01f; // a piece's size may overrun whole cells by this before it takes another
    }

    int32_t freePieceCategories(uint32_t categories);

    int32_t freePieceDefs(ee::Ptr<uint32_t> table);

    // measurePieceDefs: each piece's size in cells from its world object's box
    // (the cells' size from the park's cell record), where its origin falls
    // in them, its bounds; every piece gets its anchors' list, then the
    // anchors. A piece whose MaxObjectName names no object skips all that:
    // the message is made and never shown.
    void measurePieceDefs(uint32_t catalog, uint32_t cellRecord)
    {
        ee::Local<zge::Vec3> centre;
        ee::Local<zge::Box> bounds;
        ee::Local<char[0x100]> message;
        const ee::Ptr<PieceCatalog> self(catalog);
        const ee::Float half(0.5f);

        // The piece's cells, from its world object's box: how many it spans,
        // and from its first cell to the one its origin is in.
        const auto measure = [&](ee::Ptr<PieceDef> piece, uint32_t object) {
            const ee::Ptr<zge::Box> box(object + kWorldObjectBox);
            zge::boxCentre(centre, box);
            const ee::Ptr<zge::Vec3> cell(parkCellSize(cellRecord));
            piece->cellsX = cellsSpanned((box->high.x - box->low.x) / cell->x, kCellSlack);
            piece->cellsZ = cellsSpanned((box->high.z - box->low.z) / cell->z, kCellSlack);
            piece->cellsY = cellsSpanned((box->high.y - box->low.y) / cell->y, kCellSlack);

            piece->originCellX = cellsSpanned(unsignedToFloat(piece->cellsX) * half - centre->x / cell->x, kCellSlack) - 1;
            piece->originCellY = 0;
            piece->originCellZ = cellsSpanned(unsignedToFloat(piece->cellsZ) * half + centre->z / cell->z, kCellSlack) - 1;
            bounds->low.y = ee::Float(0.0f);
            bounds->low.x = (ee::toFloat(-piece->originCellX) - half) * cell->x;
            bounds->low.z = (ee::toFloat(-piece->originCellZ) - half) * -cell->z;
            bounds->high.x = bounds->low.x + unsignedToFloat(piece->cellsX) * cell->x;
            bounds->high.y = bounds->low.y + unsignedToFloat(piece->cellsY) * -cell->y;
            bounds->high.z = bounds->low.z - unsignedToFloat(piece->cellsZ) * cell->z;
            std::memcpy(&piece->bounds, &*bounds, sizeof(zge::Box));
        };

    void findPieceAnchors(uint32_t catalog);

        for (uint32_t i = 0; i < self->pieceCount; ++i)
        {
            const ee::Ptr<PieceDef> piece(self->pieces[i]);
            if (piece->objectHash != 0)
            {
                const uint32_t object = zge::findWorldObjectByHash(piece->objectHash);
                if (object == 0)
                {
                    zge::formatString(message, kInvalidMaxName, &piece->name[0]);
                    continue;
                }
                measure(piece, object);
            }
            piece->anchors = newEditList();
        }
        findPieceAnchors(catalog);
    }
    READABLE_FUNCTION_WAITS(measurePieceDefs, Verified);

    int32_t findPieceDefByHash(uint32_t catalog, uint32_t hash);

    int32_t addPieceDefAnchor(uint32_t piece, uint32_t anchor);

    // findPieceAnchors: an anchor object for each "EditAnchor" locator of the
    // scene: where it is, the piece it goes on, the pieces it takes.
    void findPieceAnchors(uint32_t catalog)
    {
        ee::Local<zge::LocatorList> list;
        ee::Local<zge::EulerAngles> angles;
        const uint32_t kindHash = zge::nameHash(ee::Ptr<const char>(kEditAnchorName));
        zge::findLocators(list, kindHash, 0);
        for (ee::Ptr<zge::Locator> locator = list->head; locator; locator = locator->next)
        {
            const ee::Ptr<AnchorPoint> point(locator->point);
            const ee::Ptr<AnchorTags> tags(point->tags);
            const uint32_t anchor = newEditObject(kEditAnchorName, 0);
            zge::quaternionToEuler(angles, &point->rotation);
            setAnchorPosition(anchor, point);
            setAnchorAngles(anchor, angles);
            addPieceDefAnchor(static_cast<uint32_t>(findPieceDefByHash(catalog, tags->piece)), anchor);
            for (uint32_t k = 0; k < 8; ++k)
            {
                if (tags->accepts[k] != 0)
                {
                    const uint32_t accepted = static_cast<uint32_t>(findPieceDefByHash(catalog, tags->accepts[k]));
                    addAnchorAccept(anchor, pieceDefIndex(accepted));
                }
            }
        }
        zge::freeLocators(list);
    }
    READABLE_FUNCTION_WAITS(findPieceAnchors, Verified);

    uint32_t pieceDefSecretId(ee::Ptr<PieceDef> piece)
    {
        return piece->secretId;
    }
    READABLE_FUNCTION(pieceDefSecretId, Verified);

    namespace
    {
        inline ee::Global<uint16_t, sym::g_lockedSecretPieces> g_lockedSecretPieces; // g_lockedSecretPieces ((anonymous)::g_lockedSecretPieces, (anonymous)::g_unlockedEditorPieces)
    }

    uint32_t pieceUnlocked(ee::Ptr<PieceDef> pieceDef)
    {
        const int32_t secretId = pieceDefSecretId(pieceDef);
        int32_t result = 1;
        if (secretId >= 0)
        {
            const int32_t secretBit = pieceDefSecretId(pieceDef);
            result = *g_lockedSecretPieces & 1 << (secretBit & 0x1Fu);
            return result;
        }
        return result;
    }
    READABLE_FUNCTION(pieceUnlocked, Verified);
}
