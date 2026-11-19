// The wall search (findWall): the wall a skater in the air can ride.
// Air's actions ask for it when the wall-ride action is held
// (air.cpp airActions): a sphere query round the skater's position (the
// Grind/Walls tuning's Wall Check Radius), then over every face it touches
// the nearest that is a wall (its normal within Wall Vert Dot of level),
// big enough (area 0.2), not below the skater from level 10 on, and that
// the skater moves into. A face of physics kind 8 wins over the others
// whatever its distance: the first one met is taken, and after it only
// kind-8 faces nearer still. The wall goes to the caller's record
// (Player::wallContact: the face, its node and part, its corners, normal and the
// nearest point, all in world space).

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/closest.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "aggressive/skater/skater.h"
#include "aggressive/level/scene_objects.h"

#include "sym/aggressive/skater/wall_search.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // Grind/Walls tuning entries (docs/DEVTOOLS.md).
        constexpr uint32_t kGrindWalls = 14;
        constexpr uint32_t kWallCheckRadius = 35;
        constexpr uint32_t kWallVertDot = 37;

        // Physics kinds (FaceTagRecord::kind) the search passes over, and the
        // one it prefers.
        constexpr int32_t kRailEdgeKind1 = 1, kRailEdgeKind2 = 2, kNoWallKind = 13;
        constexpr int32_t kPreferredWallKind = 8;
        // From level 10 on, a wall point above the skater's position is passed over.
        constexpr uint32_t kHighWallsFromLevel = 10;

        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;           // (0, -1, 0, 0): y grows downward
        inline ee::Global<ee::Float, sym::g_smallestArea> g_smallestArea;  // 0.2
        inline ee::Global<ee::Float, sym::g_intoPreferred> g_intoPreferred; // -0.05: moving into a kind-8 wall
        inline ee::Global<ee::Float, sym::g_intoWall> g_intoWall;      // -0.05: moving into another wall
        inline ee::Global<uint32_t, sym::g_levelNumber> g_level;

        // taggedFaceFilter's context.
        struct TaggedFaceSearch
        {
            ee::Ptr<Player> player;       // +0x00
            ee::Ptr<WallContact> contact; // +0x04 the record to fill
            ee::Float radius;             // +0x08
            int32_t tag;                  // +0x0C the face tags' kind wanted
            ee::Float limit;              // +0x10 of the dot of the motion and the face's normal
        };
        static_assert(offsetof(TaggedFaceSearch, limit) == 0x10);

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
    }

    namespace
    {

        constexpr uint32_t kWorldMatrix = 1; // nodeMatrix's
        // A part's face tags are a tag table.
        ee::Ptr<zge::TagTableHeader> faceTagsOf(ee::Ptr<zge::ZgePart> mesh) { return mesh->faceTags.cast<zge::TagTableHeader>(); }
    }

    int32_t findWall(ee::Ptr<Player> player, ee::Ptr<WallContact> wall, ee::Float radius)
    {
        ee::Local<zge::QuerySphere> sphere;
        ee::Local<zge::QueryResults> results;
        ee::Local<Vec4> normal;
        ee::Local<Vec4[3]> corners;
        ee::Local<Vec4> nearestPoint;
        ee::Local<Vec4> motion;
        ee::Local<Vec4> toWall;
        ee::Local<zge::Matrix> matrix;
        ee::Local<ee::Ptr<uint16_t>> indices;
        ee::Local<ee::Float> squared;

        ee::Float best = tuning(kGrindWalls, kWallCheckRadius);
        if (!wall)
            return 0;
        sphere->radius = radius;
        wall->object = {};
        wall->part = {};
        sphere->centre = player->position;
        if (!zge::collisionQuery(0u, sphere, 0u, 0u, 0u, 0u, results, 0u) || results->count == 0)
            return 0;

        // Moving into it (`into`: a limit on the dot with its normal), toward it.
        const auto movingInto = [&](ee::Float into) {
            *motion = zge::unit(player->velocity);
            if (!(zge::dot(*motion, *normal) < into))
                return false;
            *toWall = *nearestPoint - player->position;
            *toWall = zge::unit(*toWall);
            return zge::dot(*toWall, *normal) < 0.0f;
        };

        int32_t found = 0;
        bool havePreferred = false;
        const uint32_t hitNodes = static_cast<uint32_t>(results->count);
        for (uint32_t n = 0; n < hitNodes; ++n)
        {
            const ee::Ptr<zge::HitNode> hit = results->nodes[n];
            const uint32_t faceCount = hit->faceCount;
            for (uint32_t i = 0; i < faceCount; ++i)
            {
                const uint32_t face = hit->faces[i].face;
                const int32_t kind = surfacePhysics(hit->node, face);
                if (kind == kRailEdgeKind1 || kind == kRailEdgeKind2 || kind == kNoWallKind)
                    continue;

                // A wall: its normal in world space within Wall Vert Dot of level.
                zge::faceNormal(hit->part, face, normal);
                *matrix = *zge::nodeMatrix(hit->node, kWorldMatrix);
                zge::matrixClearTranslation(&*matrix);
                *normal = zge::transformed(*matrix, *normal);
                const ee::Float upright = zge::dot(*g_worldUp, *normal);
                const ee::Float vertDot = tuning(kGrindWalls, kWallVertDot);
                if (!(upright < vertDot) || !(-vertDot < upright))
                    continue;

                // Its corners in world space, its size, its point nearest the skater.
                *matrix = *zge::nodeMatrix(hit->node, kWorldMatrix);
                zge::faceVertices(hit->part, face, &*indices);
                for (uint32_t c = 0; c < 3; ++c)
                {
                    zge::vertexPosition(hit->part, zge::faceVertexIndex(hit->part, face, *indices, c), &corners[c]);
                    corners[c] = zge::transformed(*matrix, corners[c]);
                }
                if (aggressive::triangleArea(corners) < *g_smallestArea)
                    continue;
                zge::closestPointOnTriangle(&player->position, &corners[0], &corners[1], &corners[2], &*nearestPoint, &*squared);
                if (*g_level >= kHighWallsFromLevel && player->position.y < nearestPoint->y)
                    continue;

                // The nearest of its kind so far (a preferred one before any other).
                const bool preferred = kind == kPreferredWallKind;
                const bool firstPreferred = preferred && !havePreferred;
                if (!firstPreferred && (preferred != havePreferred || !(*squared < best * best)))
                    continue;
                if (!movingInto(firstPreferred ? *g_intoPreferred : *g_intoWall))
                    continue;

                best = ee::sqrt(*squared);
                wall->face = face;
                wall->object = hit->node;
                wall->part = hit->part;
                for (uint32_t c = 0; c < 3; ++c)
                    wall->corners[c] = corners[c];
                wall->normal = *normal;
                wall->point = *nearestPoint;
                found = 1;
                if (firstPreferred)
                    havePreferred = true;
            }
        }
        return found;
    }
    READABLE_FUNCTION_VU0(findWall, Verified);

    // taggedFaceFilter, findTaggedFace's filter (its sphere query calls it for
    // each object it meets, with the search's context): over the faces of
    // the object's collision part carrying the tag's kind, the nearest within
    // the radius whose normal is turned from the skater's motion (its forward
    // standing still) by less than the limit. The face goes to the record, as
    // findWall's does. Returns 0. Kept bug: the radius takes the face's
    // squared distance, and the next face's squared distance is measured
    // against that squared again.
    int32_t taggedFaceFilter(ee::Ptr<TaggedFaceSearch> search, ee::Ptr<zge::Node> node)
    {
        constexpr ee::Float kStill(1e-05f);
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::Matrix> turn;
        ee::Local<Vec4[3]> corners;
        ee::Local<Vec4> normal;
        ee::Local<Vec4> nearestPoint;
        ee::Local<Vec4> looking;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        ee::Local<ee::Float> squared;
        const ee::Ptr<Player> player = search->player;

        const ee::Ptr<zge::ZgePart> mesh(collisionMeshOf(node));
        if (!mesh)
            return 0;
        const ee::Ptr<zge::TagTableHeader> tags = faceTagsOf(mesh);
        if (!tags)
            return 0;
        const uint32_t count = zge::tagRecordCount(tags);

        for (uint32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<zge::TagRecord> record = zge::tagRecordAt(tags, index);
            const uint32_t kind = zge::tagByte(record, 2, 12);
            const uint32_t face = static_cast<uint32_t>(zge::tagWord(record, 8, 8));
            if (static_cast<uint32_t>(search->tag) != kind)
                continue;

            zge::faceVertices(mesh, face, &*vertices);
            *matrix = *zge::nodeMatrix(node, kWorldMatrix);
            for (uint32_t c = 0; c < 3; ++c)
            {
                const ee::Ptr<uint16_t> indices = *vertices;
                zge::vertexPosition(mesh, indices[c], &corners[c]);
                corners[c] = zge::transformed(*matrix, corners[c]);
            }
            zge::closestPointOnTriangle(&player->position, &corners[0], &corners[1], &corners[2], &*nearestPoint, &*squared);
            if (!(*squared < search->radius * search->radius))
                continue;
            const bool still = ee::abs(vec3Length(player->velocity)) < kStill;
            if (!still)
                *looking = zge::unit(player->velocity);
            else
                *looking = player->axes.forward;
            zge::faceNormal(mesh, face, normal);
            *turn = *zge::nodeMatrix(node, kWorldMatrix);
            zge::matrixClearTranslation(&*turn);
            *normal = zge::transformed(*turn, *normal);
            if (!(zge::dot(*looking, *normal) < search->limit))
                continue;

            const ee::Ptr<WallContact> contact = search->contact;
            search->radius = *squared;
            contact->face = face;
            contact->object = node;
            contact->part = mesh;
            for (uint32_t c = 0; c < 3; ++c)
                contact->corners[c] = corners[c];
            contact->normal = *normal;
            contact->point = *nearestPoint;
        }
        return 0;
    }
    READABLE_FUNCTION(taggedFaceFilter, Verified);

    // findTaggedFace: no code calls it. The face of a tag's kind nearest the
    // skater within `radius` that it is turned from by less than `limit`,
    // into `contact`: a sphere query with taggedFaceFilter. Returns whether
    // one was found.
    int32_t findTaggedFace(ee::Ptr<Player> player, ee::Ptr<WallContact> contact, int32_t tag, ee::Float radius, ee::Float limit)
    {
        if (!contact)
            return 0;
        ee::Local<zge::QuerySphere> sphere;
        ee::Local<zge::QueryResults> results;
        ee::Local<TaggedFaceSearch> search;
        search->limit = limit;
        search->tag = tag;
        sphere->radius = radius;
        search->player = player;
        search->radius = radius;
        search->contact = contact;
        contact->part = 0;
        sphere->centre = player->position;
        zge::collisionQuery(0u, sphere, 0u, sym::taggedFaceFilter, search, 0u, results, 0u);
        return contact->part != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(findTaggedFace, Verified);
}
