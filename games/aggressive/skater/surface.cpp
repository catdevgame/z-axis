// What the skater stands on: a scene object's collision mesh and, for a face
// of it, its face tag (TGFT: the physics kind and its three values) and its
// material's surface sound.
//
// The face tag table is the mesh's, read through the tag table accessors
// (zge/assets/tag_table.cpp), which read the host's mirror of it with
// ZX_NATIVE_COLLISION on (the default): surfacePhysics's kind is host data.
// Its three floats go through the engine's tagFloat (a copy out of RAM). The
// surface sound is the first byte of the face's material's tag record: a
// material holds a one-record tag table its loader fills from the MATI chunk;
// the host has no mirror of materials, so that one is read from RAM.
//
// Faces are numbered in the mesh; "no face" is 0xFFFFFFFF.

#include "skater.h"

#include "zge/scene/animator.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/assets/geo.h"
#include "zge/scene/scene.h"
#include "sym/aggressive/skater/surface.cpp.h"

namespace aggressive::skater
{
    ee::Ptr<zge::TagRecord> faceTagRecord(ee::Ptr<zge::Node> node, uint32_t face);

    namespace
    {
        constexpr uint32_t kNoFace = 0xFFFFFFFFu;
        constexpr uint32_t kNoSound = 0x10;

        // The size the tag accessors take with each read; they never use it.
        constexpr uint32_t kByteRead = 2, kWordRead = 8;

        // A surface's three values go through the same steps: the face's tag
        // record, then a float of it. 0 for an untagged face.
        ee::Float surfaceValue(ee::Ptr<zge::Node> node, uint32_t face, uint32_t field)
        {
            const ee::Ptr<zge::TagRecord> record = faceTagRecord(node, face);
            if (!record)
                return ee::Float::fromBits(0);
            const ee::Float value = zge::tagFloat(record, kWordRead, field);
            return value;
        }
    }

    // collisionMeshOf: the scene object's collision mesh, 0 without one.
    ee::Ptr<zge::ZgePart> collisionMeshOf(ee::Ptr<zge::Node> node)
    {
        if (!node)
            return {};
        return node->lods->part;
    }
    READABLE_FUNCTION(collisionMeshOf, Verified);

    // faceTagRecord: the face's face tag record (TGFT, a zge::FaceTagRecord),
    // none when there is none.
    ee::Ptr<zge::TagRecord> faceTagRecord(ee::Ptr<zge::Node> node, uint32_t face)
    {
        if (!node || face == kNoFace)
            return {};
        const ee::Ptr<zge::ZgePart> mesh = node->lods->part;
        if (!mesh)
            return {};
        const ee::Ptr<zge::TagTableHeader> tags = mesh->faceTagTable;
        if (!tags)
            return {};
        return zge::tagElementRecord(tags, face);
    }
    READABLE_FUNCTION(faceTagRecord, Verified);

    // railEdgeRecord: the record of an edge of the object's collision mesh in
    // its grind edge table (TGFE), 0 when there is none.
    ee::Ptr<zge::TagRecord> railEdgeRecord(ee::Ptr<zge::Node> node, uint32_t edge)
    {
        const ee::Ptr<zge::ZgePart> mesh = collisionMeshOf(node);
        if (!mesh)
            return {};
        const ee::Ptr<zge::TagTableHeader> edges = mesh->faceEdgeTable;
        if (!edges)
            return {};
        return zge::tagElementRecord(edges, edge);
    }
    READABLE_FUNCTION(railEdgeRecord, Verified);

    // surfacePhysics: the face's physics kind: 1 a quarter pipe, 2 a spine, 3 a
    // slope with its own gravity (0 for an untagged face).
    uint32_t surfacePhysics(ee::Ptr<zge::Node> node, uint32_t face)
    {
        const ee::Ptr<zge::TagRecord> record = faceTagRecord(node, face);
        if (!record)
            return 0;
        const uint32_t kind = zge::tagByte(record, kByteRead, offsetof(zge::FaceTagRecord, kind));
        return kind;
    }
    READABLE_FUNCTION(surfacePhysics, Verified);

    // surfaceGravity / surfaceLaunchCap / surfaceLaunchPush: the face tag's three
    // values (0 untagged): a slope's gravity or a pipe's speed scale; a
    // take-off's cap on velocity.y; its push up (air_enters.cpp, ground.cpp).
    ee::Float surfaceGravity(ee::Ptr<zge::Node> node, uint32_t face) { return surfaceValue(node, face, offsetof(zge::FaceTagRecord, gravity)); }
    READABLE_FUNCTION(surfaceGravity, Verified);
    ee::Float surfaceLaunchCap(ee::Ptr<zge::Node> node, uint32_t face) { return surfaceValue(node, face, offsetof(zge::FaceTagRecord, launchCap)); }
    READABLE_FUNCTION(surfaceLaunchCap, Verified);
    ee::Float surfaceLaunchPush(ee::Ptr<zge::Node> node, uint32_t face) { return surfaceValue(node, face, offsetof(zge::FaceTagRecord, launchPush)); }
    READABLE_FUNCTION(surfaceLaunchPush, Verified);

    namespace
    {
        // A material's tag record.
        struct MaterialTags
        {
            uint8_t surfaceSound;
        };
    }

    // surfaceSound: the face's surface sound (its material's: 0 most, 1
    // wood, 2 metal, 4 grass, 5 water, 12 fire, 14 and 15 out of bounds ...;
    // move_collide.cpp skaterTouchSurface), 0x10 for no face.
    uint32_t surfaceSound(ee::Ptr<zge::Node> node, uint32_t face)
    {
        const ee::Ptr<zge::ZgePart> mesh = collisionMeshOf(node);
        if (!node || face == kNoFace)
            return kNoSound;
        const ee::Ptr<zge::MaterialTable> materials = zge::meshMaterials(mesh, 0);
        const uint32_t index = zge::faceMaterial(mesh, face);
        const ee::Ptr<zge::TagTableHeader> tags = zge::materialTags(materials->materials[index]);
        const ee::Ptr<MaterialTags> record = zge::tagRecordAs<MaterialTags>(zge::tagRecordAt(tags, 0));
        return record->surfaceSound;
    }
    READABLE_FUNCTION(surfaceSound, Verified);

    // ---- the edge and face tag fields ---------------------------------------
    //
    // A rail's edge record (TGFE; zge::EdgeRecord) and a face's tag record
    // (TGFT; zge::FaceTagRecord) read field by field through the tag
    // accessors. A rail is named by its object and segment (the grind states'
    // Player::rail). Each reads 0 when there is no record. The names are the
    // callers' uses: the grind enters and exits (state_enters.cpp, grind.cpp,
    // grind_search.cpp) and the poles (poles_walls.cpp, action_states.cpp,
    // pole_vault.cpp).
    namespace
    {
        uint32_t edgeWord(ee::Ptr<zge::Node> rail, uint32_t segment, uint32_t field)
        {
            const ee::Ptr<zge::TagRecord> record = railEdgeRecord(rail, segment);
            if (!record)
                return 0;
            const uint32_t word = static_cast<uint32_t>(zge::tagWord(record, kWordRead, field));
            return word;
        }
        uint32_t edgeByte(ee::Ptr<zge::Node> rail, uint32_t segment, uint32_t field)
        {
            const ee::Ptr<zge::TagRecord> record = railEdgeRecord(rail, segment);
            if (!record)
                return 0;
            const uint32_t byte = zge::tagByte(record, kByteRead, field);
            return byte;
        }
        ee::Float edgeFloat(ee::Ptr<zge::Node> rail, uint32_t segment, uint32_t field)
        {
            const ee::Ptr<zge::TagRecord> record = railEdgeRecord(rail, segment);
            if (!record)
                return ee::Float(0.0f);
            const ee::Float value = zge::tagFloat(record, kWordRead, field);
            return value;
        }
    }

    // railEventWord: the word a rail's Grind events carry.
    uint32_t railEventWord(ee::Ptr<zge::Node> rail, uint32_t segment)
    {
        return edgeWord(rail, segment, offsetof(zge::EdgeRecord, eventId));
    }
    READABLE_FUNCTION(railEventWord, Verified);

    // railEdgeKind: a rail's edge kind (1 or 9 a rail the skater falls off
    // the side of, 2 one he jumps off, 4 a horizontal pole).
    uint32_t railEdgeKind(ee::Ptr<zge::Node> rail, uint32_t segment)
    {
        return edgeByte(rail, segment, offsetof(zge::EdgeRecord, kind));
    }
    READABLE_FUNCTION(railEdgeKind, Verified);

    // railMaterial: a rail's material (its grinding sound and rumble).
    uint32_t railMaterial(ee::Ptr<zge::Node> rail, uint32_t segment)
    {
        return edgeByte(rail, segment, offsetof(zge::EdgeRecord, material));
    }
    READABLE_FUNCTION(railMaterial, Verified);

    // railLaunchMin / railLaunchMax / railLaunchSpeed: a rail's launch off its
    // end (airEnter): faster than the first, slower than the second (0: no
    // limit), at the third. A pole reads them as its exit heading, lift and
    // exit speed.
    ee::Float railLaunchMin(ee::Ptr<zge::Node> rail, uint32_t segment)
    {
        return edgeFloat(rail, segment, offsetof(zge::EdgeRecord, launchMin));
    }
    READABLE_FUNCTION(railLaunchMin, Verified);
    ee::Float railLaunchMax(ee::Ptr<zge::Node> rail, uint32_t segment)
    {
        return edgeFloat(rail, segment, offsetof(zge::EdgeRecord, launchMax));
    }
    READABLE_FUNCTION(railLaunchMax, Verified);
    ee::Float railLaunchSpeed(ee::Ptr<zge::Node> rail, uint32_t segment)
    {
        return edgeFloat(rail, segment, offsetof(zge::EdgeRecord, launchSpeed));
    }
    READABLE_FUNCTION(railLaunchSpeed, Verified);

    // surfaceKind: a face's surface kind, the word its object's "Jump" and
    // "Land" events carry (0 for an untagged face).
    uint32_t surfaceKind(ee::Ptr<zge::Node> node, uint32_t face)
    {
        const ee::Ptr<zge::TagRecord> record = faceTagRecord(node, face);
        if (!record)
            return 0;
        const uint32_t kind = static_cast<uint32_t>(zge::tagWord(record, kWordRead, offsetof(zge::FaceTagRecord, eventKind)));
        return kind;
    }
    READABLE_FUNCTION(surfaceKind, Verified);

    namespace
    {
        // A class's tag record.
        struct ClassTags
        {
            uint32_t typeName; // the name hash of the class's type
        };
    }

    // classTagRecord: an object class's tag record (the first of its tag
    // table), 0 for none.
    ee::Ptr<zge::TagRecord> classTagRecord(ee::Ptr<zge::GeoObject> objectClass)
    {
        const ee::Ptr<zge::TagTableHeader> table = zge::classTags(objectClass);
        if (!table)
            return {};
        return zge::tagRecordAt(table, 0u);
    }
    READABLE_FUNCTION(classTagRecord, Verified);

    // classTypeName: the name hash of a class's type, 0 for none
    // (move_collide.cpp: what the skater touched).
    uint32_t classTypeName(ee::Ptr<zge::GeoObject> objectClass)
    {
        const ee::Ptr<zge::TagRecord> record = classTagRecord(objectClass);
        if (!record)
            return 0;
        const uint32_t name = static_cast<uint32_t>(zge::tagWord(record, kWordRead, offsetof(ClassTags, typeName)));
        return name;
    }
    READABLE_FUNCTION(classTypeName, Verified);
}

// The surfaces' rest: an edge's flag, the mover test, and a rail's, a pipe's
// lip edge's and a wall's points put into the world.
namespace aggressive::skater
{
    namespace
    {
        // The six mover classes' name hashes (setupLevelRails makes them): the
        // first three (AnimPropDef, AnimObj, Part) move while animated, the
        // rest (Car, Trailer, Collision) always.
        constexpr uint32_t kMoverClassCount = 6;
        constexpr uint32_t kAnimatedMovers = 3;
        inline ee::Global<uint32_t[kMoverClassCount], sym::moverClassHashes> g_moverClassHashes;

        constexpr uint8_t kEdgeAnyKind = 1u; // EdgeRecord::flags: a search of no kind takes this edge
    }

    // railEdgeAnyKind: whether a search of no kind takes this rail edge
    // (closestRailEdge). 0 without a record.
    uint32_t railEdgeAnyKind(ee::Ptr<zge::Node> node, uint32_t edge)
    {
        const ee::Ptr<zge::TagRecord> record = railEdgeRecord(node, edge);
        if (!record)
            return 0;
        const uint32_t flags = zge::tagByte(record, kByteRead, offsetof(zge::EdgeRecord, flags));
        return flags & kEdgeAnyKind;
    }
    READABLE_FUNCTION(railEdgeAnyKind, Verified);

    // isMover: whether the object moves (what the skater stands on
    // carries it, carried.cpp): its class one of the six movers
    // (setupLevelRails' list): Car, Trailer and Collision always, the
    // first three while their owner's animation runs unpaused.
    uint32_t isMover(ee::Ptr<zge::Node> object)
    {
        ee::Local<uint32_t> paused;
        const uint32_t typeName = classTypeName(zge::objectClass(object));
        for (uint32_t mover = 0; mover < kMoverClassCount; ++mover)
        {
            if (typeName != (*g_moverClassHashes)[mover])
                continue;
            if (mover >= kAnimatedMovers)
                return 1;
            const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(object, 0);
            if (owner == 0)
                return 0;
            if (zge::animationPaused(owner, paused) == 0)
                return 0;
            return *paused == 0 ? 1 : 0;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(isMover, Verified);

    // pipeEdgeOf: a pipe face's lip edge (its face tag's nearest edge) into
    // the edge record: the edge, the object, its mesh, the ends in the world
    // and the direction (end to start). 0 when the face has no tag or there
    // is no record to fill.
    uint32_t pipeEdgeOf(ee::Ptr<zge::Node> object, uint32_t face, ee::Ptr<RailRecord> edge)
    {
        ee::Local<zge::Matrix> world;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        const ee::Ptr<zge::TagRecord> record = faceTagRecord(object, face);
        uint32_t element = 0;
        if (record)
        {
            zge::tagByte(record, kByteRead, offsetof(zge::FaceTagRecord, kind)); // (read, unused)
            element = static_cast<uint32_t>(zge::tagWord(record, kWordRead, offsetof(zge::FaceTagRecord, nearestEdge)));
        }
        if (!record || !edge)
            return 0;
        edge->surface = element;
        edge->node = object;
        const ee::Ptr<zge::ZgePart> mesh = collisionMeshOf(object);
        edge->found = mesh;
        world = *zge::nodeMatrix(object, 1);
        edge->joinPoint = zge::kZero;
        zge::elementEdgeVertices(mesh, static_cast<int32_t>(element), &*vertices);
        const auto toWorld = [&](zge::Vec4 &point, uint32_t end)
        {
            zge::vertexPosition(mesh, (*vertices)[end], &point);
            point = zge::transformed(*world, point);
        };
        toWorld(edge->start, 0);
        toWorld(edge->end, 1);
        edge->direction = edge->start - edge->end;
        edge->direction = zge::unit(edge->direction);
        return 1;
    }
    READABLE_FUNCTION_VU0(pipeEdgeOf, Verified);

    // railToWorld (the record of a rail ground on): its ends in the world
    // again (the object may have moved) and its direction (start to end).
    void railToWorld(ee::Ptr<RailRecord> rail)
    {
        ee::Local<zge::Matrix> world;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        const ee::Ptr<zge::ZgePart> mesh = collisionMeshOf(rail->node);
        zge::elementEdgeVertices(mesh, static_cast<int32_t>(rail->surface), &*vertices);
        const ee::Ptr<uint16_t> ends = *vertices;
        zge::vertexPosition(mesh, ends[0], &rail->start);
        zge::vertexPosition(mesh, (*vertices)[1], &rail->end);
        world = *zge::nodeMatrix(rail->node, 1);
        rail->start = zge::transformed(*world, rail->start);
        rail->end = zge::transformed(*world, rail->end);
        rail->direction = rail->end - rail->start;
        rail->direction = zge::unit(rail->direction);
    }
    READABLE_FUNCTION(railToWorld, Verified);

    // wallToWorld (the wall ridden): its corners in the world again and its
    // face's normal turned with the object.
    void wallToWorld(ee::Ptr<WallContact> wall)
    {
        ee::Local<zge::Matrix> world;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        const ee::Ptr<zge::ZgePart> mesh = collisionMeshOf(wall->object);
        zge::faceVertices(mesh, wall->face, &*vertices);
        world = *zge::nodeMatrix(wall->object, 1);
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            zge::vertexPosition(mesh, (*vertices)[corner], &wall->corners[corner]);
            wall->corners[corner] = zge::transformed(*world, wall->corners[corner]);
        }
        zge::faceNormal(mesh, wall->face, &wall->normal);
        zge::matrixClearTranslation(&*world); // the normal turns, it does not move
        wall->normal = zge::transformed(*world, wall->normal);
    }
    READABLE_FUNCTION(wallToWorld, Verified);
}
