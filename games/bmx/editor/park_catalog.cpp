// The park editor's catalog of pieces (were the sweep's blocks_001f2e90):
// the piece file's text ("Inline.Pep": CategoryListBegin and ObjectListBegin
// blocks, to ObjectFileEnd) made into categories and piece definitions,
// each piece measured in grid cells from the world object of its
// MaxObjectName, and the anchors its scene's "EditAnchor" locators give it.
// BMX XXX's own part of it; what both games have is zge/editor/park_catalog.cpp.

#include "bmx/editor/conversions.h"
#include "zge/editor/park_catalog.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "bmx/editor/edit_records.h"
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
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/editor/park_catalog.cpp.h"

namespace zge::editor
{
    namespace
    {
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

        // An "EditAnchor" locator's tags: the piece it is on and those its
        // anchor takes.
        struct AnchorTags
        {
            uint32_t piece;       // +0x00 a piece's objectHash
            uint32_t accepts[8];  // +0x04 the same, 0 for none
        };
        constexpr ee::Float kCellSlack = 0.01f; // a piece's size may overrun whole cells by this before it takes another
        constexpr uint32_t kEditAnchorName = sym::g_pieceAnchorsEditAnchorName; // "EditAnchor"
        constexpr uint32_t kWorldObjectBox = 0x10; // a world object: its bounding box
    }

    int32_t findPieceDefByHash(uint32_t catalog, uint32_t hash);

    int32_t addPieceDefAnchor(uint32_t piece, uint32_t anchor);

    // A piece's category: its index among the catalog's ("CategoryIndex").
    uint32_t pieceDefCategory(uint32_t piece)
    {
        return ee::Ptr<PieceDef>(piece)->category;
    }
    READABLE_FUNCTION(pieceDefCategory, Verified);

    // A piece's "SecretID".
    uint32_t pieceDefSecretId(uint32_t piece)
    {
        return ee::Ptr<PieceDef>(piece)->secretId;
    }
    READABLE_FUNCTION(pieceDefSecretId, Verified);

    void findPieceAnchors(uint32_t catalog);

    // Every piece of the catalog measured against the park's cells, from its world object's
    // box: how many cells it spans, from its first cell to the one its origin is in, and its
    // bounds about that cell; each gets an empty list of anchors. Then (BMX XXX) the catalog's
    // "Tower Object" gets one anchor at its origin, and the scene's anchors are found.
    void measurePieceDefs(uint32_t catalog, uint32_t cellRecord)
    {
        ee::Local<zge::Vec3> centre;
        ee::Local<zge::Box> bounds;
        ee::Local<char[0x100]> message;
        const ee::Ptr<PieceCatalog> self(catalog);
        const ee::Float half(0.5f);

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

        for (uint32_t i = 0; i < self->pieceCount; ++i)
        {
            const ee::Ptr<PieceDef> piece(self->pieces[i]);
            if (piece->objectHash != 0)
            {
                const uint32_t object = zge::findWorldObjectByHash(piece->objectHash);
                if (object == 0)
                {
                    zge::formatString(message, sym::bmx::g_invalidMaxNameFormat, &piece->name[0]);
                    continue;
                }
                measure(piece, object);
            }
            piece->anchors = newEditList();
        }
        const uint32_t towerAnchor = newEditObject(kEditAnchorName, 0);
        bounds->low.x = ee::Float(0.0f);
        bounds->low.y = ee::Float(0.0f);
        bounds->low.z = ee::Float(0.0f);
        setAnchorPosition(towerAnchor, &bounds->low);
        addPieceDefAnchor(findPieceDef(catalog, sym::g_textTowerObject), towerAnchor);
        findPieceAnchors(catalog);
    }
    READABLE_FUNCTION_WAITS(measurePieceDefs, Verified);

    // The scene's "EditAnchor" locators made into the pieces' anchors: each stands where its
    // locator does, keeps the locator's name (BMX XXX: the rail places table asks for anchors
    // by name), goes on the piece its tags name and accepts the pieces they list.
    void findPieceAnchors(uint32_t catalog)
    {
        ee::Local<zge::LocatorList> list;
        ee::Local<zge::EulerAngles> angles;
        ee::Local<AnchorTags> tags;
        const uint32_t kindHash = zge::nameHash(ee::Ptr<const char>(kEditAnchorName));
        zge::findLocators(list, kindHash, 0);
        for (ee::Ptr<zge::Locator> locator = list->head; locator; locator = locator->next)
        {
            const ee::Ptr<AnchorPoint> point(locator->point);
            *tags = *ee::Ptr<AnchorTags>(point->tags);
            const uint32_t anchor = newEditObject(kEditAnchorName, 0);
            zge::quaternionToEuler(angles, &point->rotation);
            setAnchorNameHash(anchor, point->nameHash);
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
}
