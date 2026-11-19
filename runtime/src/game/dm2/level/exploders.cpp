// The exploders: the "Exploder" level objects, which burst into shards and
// sparks when activated (or touched, where their tag says so; BMX XXX's
// exploders descend from them): hidden and untested once gone off. Its
// state, place and turn are saved. DM2's own code (its game block, the
// level objects).

#include "breakable_save.h"
#include "dm2/fx/spawns.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/collision/node_faces.h"
#include "dm2/sound/sound_events.h"
#include "dynamics_objects.h"
#include "object_node.h"
#include "object_sounds.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/vector.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include <cstring>
#include "sym/dm2/level/exploders.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kExploderTypeName{sym::exploderTypeName}; // "Exploder"

        // The exploder's component: its sound (word 0, object_sounds.h),
        // whether it has gone off (byte 4) and whether a touch sets it off
        // (byte 5).
        constexpr uint32_t kTagByte = 2, kTagWord = 8, kExploderTagSound = 0, kExploderTagExploded = 4, kExploderTagOnTouch = 5;
        // The blast's sample: soundEvent 37 (the sound sources), b 3: the
        // level bank's sample 2 once at the exploder.
        constexpr uint32_t kSoundSourcesEvent = 37, kPlayOnce = 3, kBlastSample = 2, kFullLevel = 0x7F;
        uint32_t exploderSound(ee::Ptr<zge::SceneObject> exploder)
        {
            return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(exploder), kTagWord, kExploderTagSound));
        }
    }

    // registerExploderType: the "Exploder" level objects made exploders.
    uint32_t registerExploderType()
    {
        return game::tail(zge::registerEntityInit, kExploderTypeName, sym::newExploder);
    }
    READABLE_FUNCTION(registerExploderType, Verified);

    int32_t activateExploder(ee::Ptr<zge::EngineMessage> message);
    void explodeNode(uint64_t node, ee::Ptr<zge::Vec4> velocity, ee::Ptr<zge::Vec4> position);

    // exploderExploded: whether it has gone off.
    uint32_t exploderExploded(ee::Ptr<zge::SceneObject> exploder)
    {
        return zge::tagByte(zge::objectComponent(exploder), kTagByte, kExploderTagExploded);
    }
    READABLE_FUNCTION(exploderExploded, Verified);

    // setExploderExploded: it marked gone off (or not).
    void setExploderExploded(ee::Ptr<zge::SceneObject> exploder, uint32_t exploded)
    {
        game::tail(zge::setTagByte, zge::objectComponent(exploder), kTagByte, kExploderTagExploded, exploded & 0xFFu);
    }
    READABLE_FUNCTION(setExploderExploded, Verified);

    // exploderTouched (the touch message): one gone off lets the toucher
    // through; one a touch sets off is activated (its own handler, with a
    // message of its own that names it and an activation carrying the
    // touch's velocity) and lets the toucher through. Its sound has its
    // turn.
    int32_t exploderTouched(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<ActivationData> activation;
        ee::Local<zge::EngineMessage> activate;
        const ee::Ptr<zge::SceneObject> exploder(message->target);
        const ee::Ptr<TouchData> touch(message->data);
        if (exploderExploded(exploder) == 1)
            touch->accept = 0;
        else if (zge::tagByte(zge::objectComponent(exploder), kTagByte, kExploderTagOnTouch))
        {
            zge::memoryFill(activation, 0, sizeof(ActivationData));
            activation->quiet = 0;
            activation->velocity = zge::quadwordOf(ee::Ptr<zge::Vec4>(&touch->velocity));
            activate->target = exploder;
            activate->data = activation.address();
            activateExploder(activate);
            touch->accept = 0;
        }
        objectSoundMessage(message, exploderSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(exploderTouched, Verified);

    // exploderSaveSize (the save-size message): an exploder's save is 48
    // bytes; its sound has its turn.
    int32_t exploderSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(BreakableSave);
        objectSoundMessage(message, exploderSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(exploderSaveSize, Verified);

    // exploderSave (the save message): whether it has gone off, where it
    // stands and how it is turned; its sound has its turn.
    int32_t exploderSave(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<BreakableSave> save(message->data);
        save->broken = exploderExploded(ee::Ptr<zge::SceneObject>(message->target));
        // (A quadword store: the save may sit off a 16-byte boundary.)
        zge::aligned(ee::Ptr<zge::Vec4>(&save->position)) =
            zge::quadwordOf(zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)));
        zge::quaternionCopy(&save->orientation, zge::objectOrientation(ee::Ptr<zge::SceneObject>(message->target)));
        objectSoundMessage(message, exploderSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(exploderSave, Verified);

    namespace
    {
        using rider::collision::NodeFace;
        using rider::collision::NodeFaceVertex;

        // The largest float: where the search for a face's shortest side
        // starts.
        inline ee::Global<ee::Float, sym::g_exploderLargest> g_exploderLargest;
        // A face is cut in four until its shortest side is under 1.25 (the
        // square is compared), or it has been cut four times.
        constexpr float kLargestShardSideSquared = 1.5625f;
        constexpr int32_t kMostCuts = 4;
        // How many cuts deep explodeFace is.
        inline ee::Global<int32_t, sym::g_explodeFaceDepth> g_explodeFaceDepth;
        // The four triangles a cut makes of a triangle, by point: 0 to 2 its
        // corners, 3 to 5 the middles of its sides (3 between corners 0 and
        // 1, 4 between 1 and 2, 5 between 2 and 0).
        constexpr uint32_t kCutPoints = 6, kCutTriangles = 4;
        struct CutTriangle
        {
            uint32_t points[3];
        };
        inline ee::Global<CutTriangle[kCutTriangles], sym::explodeFaceCuts> g_explodeFaceCuts;
        struct FaceUv
        {
            ee::Float u, v;
        };
        // A shard's speed along its face's normal: the least, and what each
        // step of a random 0 to 255 adds (up to a fifteenth more).
        inline ee::Global<ee::Float, sym::g_shardSpeedStep> g_shardSpeedStep;
        inline ee::Global<ee::Float, sym::g_shardLeastSpeed> g_shardLeastSpeed;
        // The share of the breaking velocity a shard takes.
        constexpr float kShardVelocityShare = 0.75f;
        constexpr int32_t kShardLifetime = 1250;
    }

    // faceShortestSideSquared: the square of the shortest of a triangle's
    // three sides (explodeFace cuts a face up until this is small).
    ee::Float faceShortestSideSquared(ee::Ptr<NodeFace> face)
    {
        ee::Local<zge::Vec4> side;
        ee::Float shortest = *g_exploderLargest;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t next = (corner + 1) % 3;
            *side = face->vertices[next].position - face->vertices[corner].position;
            const ee::Float squared = zge::lengthSquared(&side->xyz());
            if (squared < shortest)
                shortest = squared;
        }
        return shortest;
    }
    READABLE_FUNCTION(faceShortestSideSquared, Verified);

    // explodeFace: a triangle thrown as shards with the breaking velocity
    // (BMX XXX's explodeFace is its descendant). One small enough (or cut
    // four times already) becomes one shard, of the face's material and its
    // first corner's colour, leaving along the face's normal at a random
    // speed on top of three quarters of the breaking velocity. A larger one
    // is cut in four through the middles of its sides (positions, texture
    // coordinates and colours halved between the corners) and each quarter
    // thrown the same way.
    void explodeFace(ee::Ptr<NodeFace> face, ee::Ptr<zge::Vec4> velocity)
    {
        ee::Local<zge::Vec4> carried;
        ee::Local<zge::Vec4[kCutPoints]> points;
        ee::Local<FaceUv[kCutPoints]> uvs;
        ee::Local<zge::VertexColour[kCutPoints]> colours;
        ee::Local<NodeFace> quarter;
        ee::Local<NodeFaceVertex[3]> quarterVertices;
        ee::Local<ShardTriangle> shard;
        ee::Local<CutTriangle[kCutTriangles]> cuts;
        for (uint32_t cut = 0; cut < kCutTriangles; ++cut)
            cuts[cut] = (*g_explodeFaceCuts)[cut];
        if (*g_explodeFaceDepth >= kMostCuts || faceShortestSideSquared(face) < ee::Float(kLargestShardSideSquared))
        {
            shard->material = face->material;
            shard->flags = 0;
            std::memcpy(shard->colour, &face->vertices[0].colour, sizeof(shard->colour));
            shard->lifetime = kShardLifetime;
            const uint32_t random = zge::randomNumber() & 0xFFu;
            const ee::Float speed = ee::toFloat(static_cast<int32_t>(random)) * *g_shardSpeedStep + *g_shardLeastSpeed;
            zge::Vec4 &shardVelocity = zge::quadwordOf(ee::Ptr<ee::Float>(&shard->velocity[0]));
            shardVelocity = face->normal * speed;
            *carried = *velocity * ee::Float(kShardVelocityShare);
            shardVelocity = shardVelocity + *carried;
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                shard->corners[corner].position = zge::quadwordOf(ee::Ptr<zge::Vec4>(&face->vertices[corner].position));
                shard->corners[corner].u = face->vertices[corner].u;
                shard->corners[corner].v = face->vertices[corner].v;
            }
            spawnShardFromTriangle(shard);
            return;
        }
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t next = (corner + 1) % 3;
            const uint32_t middle = 3 + corner;
            const ee::Ptr<NodeFaceVertex> from(&face->vertices[corner]);
            const ee::Ptr<NodeFaceVertex> to(&face->vertices[next]);
            points[corner] = zge::quadwordOf(ee::Ptr<zge::Vec4>(&from->position));
            uvs[corner].u = from->u;
            uvs[corner].v = from->v;
            colours[corner] = from->colour;
            points[middle] = from->position + to->position;
            points[middle] = points[middle] * ee::Float(0.5f);
            uvs[middle].u = (from->u + to->u) * ee::Float(0.5f);
            uvs[middle].v = (from->v + to->v) * ee::Float(0.5f);
            colours[middle].red = static_cast<uint8_t>((static_cast<uint32_t>(from->colour.red) + to->colour.red) >> 1);
            colours[middle].green = static_cast<uint8_t>((static_cast<uint32_t>(from->colour.green) + to->colour.green) >> 1);
            colours[middle].blue = static_cast<uint8_t>((static_cast<uint32_t>(from->colour.blue) + to->colour.blue) >> 1);
            colours[middle].alpha = static_cast<uint8_t>((static_cast<uint32_t>(from->colour.alpha) + to->colour.alpha) >> 1);
        }
        for (uint32_t cut = 0; cut < kCutTriangles; ++cut)
        {
            quarter->material = face->material;
            quarter->vertexCount = 3;
            quarter->normal = zge::quadwordOf(ee::Ptr<zge::Vec4>(&face->normal));
            quarter->vertices = quarterVertices;
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const uint32_t point = cuts[cut].points[corner];
                quarter->vertices[corner].position = points[point];
                quarter->vertices[corner].u = uvs[point].u;
                quarter->vertices[corner].v = uvs[point].v;
                quarter->vertices[corner].colour = colours[point];
            }
            *g_explodeFaceDepth = *g_explodeFaceDepth + 1;
            explodeFace(quarter, velocity);
            *g_explodeFaceDepth = *g_explodeFaceDepth - 1;
        }
    }
    READABLE_FUNCTION_WAITS(explodeFace, Verified);

    // explodeFaceVisit (forEachNodeFace's visit; the context is the
    // breaking velocity): the face thrown. Always 1: every face.
    uint32_t explodeFaceVisit(uint64_t /*node*/, ee::Ptr<NodeFace> face, ee::Ptr<zge::Vec4> velocity)
    {
        explodeFace(face, velocity);
        return 1;
    }
    READABLE_FUNCTION_WAITS(explodeFaceVisit, Verified);

    namespace
    {
        // explodeNode's sparks: eight, each drifting 1/30 a step out in a
        // random direction across, with half the breaking velocity, and
        // dropping a random share (up to a fifteenth) more.
        constexpr uint32_t kExplosionSparks = 8;
        inline ee::Global<ee::Float, sym::g_explosionSparkDrift> g_explosionSparkDrift;
        inline ee::Global<ee::Float, sym::g_explosionSparkDropStep> g_explosionSparkDropStep;
        inline ee::Global<ee::Float, sym::g_explosionSparkDropScale> g_explosionSparkDropScale;
        constexpr float kSparkVelocityShare = 0.5f;
        // Each spark spawn's count: 4 to 11.
        constexpr uint32_t kSparkCountMask = 7, kLeastSparks = 4;
    }

    // explodeNode: the node hidden and no longer tested; every face of its
    // model thrown as shards with `velocity`; eight sprays of sparks at
    // `position`.
    void explodeNode(uint64_t node, ee::Ptr<zge::Vec4> velocity, ee::Ptr<zge::Vec4> position)
    {
        ee::Local<zge::Vec4> drift;
        ee::Local<zge::Vec4> carried;
        rider::collision::showNode(node, 0);
        rider::collision::setNodeCollidable(node, 0);
        rider::collision::forEachNodeFace(node, rider::collision::NodeFaceVisit(sym::explodeFaceVisit), velocity);
        for (uint32_t spray = 0; spray < kExplosionSparks; ++spray)
        {
            drift->z = *g_explosionSparkDrift;
            drift->x = ee::Float(0.0f);
            drift->y = ee::Float(0.0f);
            zge::vec3RotateY(&drift->xyz(), &drift->xyz(), static_cast<zge::Angle>(zge::randomNumber()));
            *carried = *velocity * ee::Float(kSparkVelocityShare);
            *drift = *drift + *carried;
            const uint32_t random = zge::randomNumber() & 0xFFu;
            drift->y = drift->y - (ee::toFloat(static_cast<int32_t>(random)) * *g_explosionSparkDropStep * *g_explosionSparkDropScale +
                                   *g_explosionSparkDrift);
            sparks(&position->x, &drift->x, static_cast<int32_t>((zge::randomNumber() & kSparkCountMask) + kLeastSparks));
        }
    }
    READABLE_FUNCTION_WAITS(explodeNode, Verified);

    // newExploder: the object made an exploder: shown and tested unless gone
    // off; its activate, touch, save-size, save and restore messages; its
    // sound started.
    int32_t newExploder(ee::Ptr<zge::SceneObject> exploder)
    {
        const uint64_t node = objectNodeHandle(exploder);
        const uint32_t whole = exploderExploded(exploder) == 0 ? 1u : 0u;
        rider::collision::showNode(node, whole);
        rider::collision::setNodeCollidable(node, whole);
        zge::subscribe(exploder, kActivateMessage, zge::MessageHandler(sym::activateExploder));
        zge::subscribe(exploder, kTouchMessage, zge::MessageHandler(sym::exploderTouched));
        zge::subscribe(exploder, zge::kSaveSizeMessage, zge::MessageHandler(sym::exploderSaveSize));
        zge::subscribe(exploder, zge::kSaveMessage, zge::MessageHandler(sym::exploderSave));
        zge::subscribe(exploder, zge::kRestoreMessage, zge::MessageHandler(sym::restoreExploder));
        startObjectSound(exploder, static_cast<uint32_t>(zge::tagWord(zge::objectComponent(exploder), kTagWord, kExploderTagSound)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newExploder, Verified);

    // activateExploder (the activate message): one not gone off goes off:
    // unless the activation is quiet, its node bursts (explodeNode, with the
    // activation's velocity), the blast's sample plays and its sound has its
    // turn. 1.
    int32_t activateExploder(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> exploder(message->target);
        const ee::Ptr<ActivationData> activation(message->data);
        if (exploderExploded(exploder) == 1)
            return 1;
        if (activation->quiet == 0)
        {
            const uint64_t node = objectNodeHandle(exploder);
            explodeNode(node, &activation->velocity, zge::objectPosition(exploder));
            soundEvent(kSoundSourcesEvent, zge::objectPosition(exploder), kPlayOnce, kBlastSample, kFullLevel);
            objectSoundMessage(message, exploderSound(ee::Ptr<zge::SceneObject>(message->target)));
        }
        setExploderExploded(exploder, 1);
        return 1;
    }
    READABLE_FUNCTION_WAITS(activateExploder, Verified);

    // restoreExploder (the restore message): gone off or whole as saved
    // (shown and tested while whole), at its saved place and turn; its sound
    // has its turn.
    int32_t restoreExploder(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> exploder(message->target);
        const uint64_t node = objectNodeHandle(exploder);
        const ee::Ptr<BreakableSave> save(message->data);
        setExploderExploded(exploder, save->broken);
        const uint32_t whole = exploderExploded(exploder) == 0 ? 1u : 0u;
        rider::collision::showNode(node, whole);
        rider::collision::setNodeCollidable(node, whole);
        zge::setObjectPosition(exploder, &save->position);
        zge::setObjectOrientation(exploder, &save->orientation);
        objectSoundMessage(message, exploderSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreExploder, Verified);
}
