// BMX XXX's "Exploder" level object type: an object that breaks apart when
// it is activated, or (one whose tag says so) when something runs into it.
// BMX XXX's own code (its game block), the line of DM2's exploder and of the
// glass beside it (glass.cpp): every face of its model is thrown as shards
// (explodeFace: a large face cut up first), two effects mark the blast unless its tag
// says not, the type's "activate" sound plays, and the object is hidden and
// no longer collided with until it is enabled again. Its state is saved
// with its place.

#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/fx/emitter_objects.h"
#include "bmx/fx/spawn_effects.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/engine_calls.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/assets/tag_table.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/exploders.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;
    using rider::collision::forEachNodeFace;
    using rider::collision::NodeFace;
    using rider::collision::NodeFaceCorner;
    using rider::collision::NodeFaceVisit;

    namespace
    {
        // The object's own tag data (objectComponent).
        constexpr uint32_t kExplodedTag = 0;         // byte: exploded
        constexpr uint32_t kBreaksOnCollisionTag = 1; // byte: a collision explodes it
        constexpr uint32_t kSkipsBlastTag = 3;       // byte: explodes without the two blast effects
        // Its class's tag data (levelObjectClassTag).
        constexpr uint32_t kSoundNameTag = 4;        // text: the middle of its sound's name
        // The tag accessors' second argument (the value's kind).
        constexpr uint32_t kTagByte = 2, kTagText = 0x10;

        constexpr uint32_t kTypeName = sym::exploderTypeName;                            // "Exploder"
        constexpr uint32_t kActivateSoundFormat = sym::bmx::exploderActivateSoundFormat; // "%s.%s.activate"
        // The largest float: where the search for a face's shortest side starts.
        inline ee::Global<ee::Float, sym::g_exploderLargest> g_exploderLargest;
        // The blast effects' sizes, as shares of the object's size (doubles: 0.2 and 0.1).
        inline ee::Global<uint64_t, sym::g_exploderBlastSizeScale> g_exploderBlastSizeScale;
        inline ee::Global<uint64_t, sym::g_exploderFlashSizeScale> g_exploderFlashSizeScale;
        inline ee::Global<ee::Float, sym::g_exploderHundredth> g_exploderHundredth; // 0.01

        constexpr uint32_t kCreateEntity = sym::createEntity;

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

    namespace
    {
        namespace tail
        {
        // The original's tail jump to the rider's collision layer.
        uint32_t setNodeCollidable(uint64_t node, uint32_t collides) { return ee::tailCall<uint32_t>(sym::setNodeCollidable, node, collides); }
        }
    }

    uint32_t initExploder(ee::Ptr<zge::SceneObject> exploder);
    uint32_t exploderExploded(ee::Ptr<zge::SceneObject> exploder);
    uint32_t exploderSkipsBlast(ee::Ptr<zge::SceneObject> exploder);
    void setExploderExploded(ee::Ptr<zge::SceneObject> exploder, uint32_t exploded);
    int32_t explodeExploder(ee::Ptr<zge::EngineMessage> message);
    void explodeNode(uint64_t node, ee::Ptr<zge::Vec4> velocity, ee::Ptr<zge::Vec4> position, uint32_t skipsBlast);
    void explodeFace(ee::Ptr<NodeFace> face, ee::Ptr<zge::Vec4> velocity);

    // registerExploderType: the "Exploder" type, made as a plain entity and
    // set up by initExploder.
    uint32_t registerExploderType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initExploder));
    }
    READABLE_FUNCTION(registerExploderType, Verified);

    // initExploder (the type's init): shown and collided with unless
    // exploded; its messages subscribed. Always 1.
    uint32_t initExploder(ee::Ptr<zge::SceneObject> exploder)
    {
        const uint64_t node = objectNodeHandle(exploder);
        if (exploderExploded(exploder) == 0)
        {
            rider::collision::showNode(node, 1);
            rider::collision::setNodeCollidable(node, 1);
        }
        else
        {
            rider::collision::showNode(node, 0);
            rider::collision::setNodeCollidable(node, 0);
        }
        zge::subscribe(exploder, kActivateMessage, zge::MessageHandler(sym::explodeExploder));
        zge::subscribe(exploder, kEnableMessage, zge::MessageHandler(sym::enableExploder));
        zge::subscribe(exploder, kRiderTouchMessage, zge::MessageHandler(sym::exploderCollide));
        zge::subscribe(exploder, kMessage105, zge::MessageHandler(sym::exploderCollide));
        zge::subscribe(exploder, zge::kSaveSizeMessage, zge::MessageHandler(sym::exploderSaveSize));
        zge::subscribe(exploder, zge::kSaveMessage, zge::MessageHandler(sym::saveExploder));
        zge::subscribe(exploder, zge::kRestoreMessage, zge::MessageHandler(sym::restoreExploder));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initExploder, Verified);

    // exploderExploded: whether it has exploded.
    uint32_t exploderExploded(ee::Ptr<zge::SceneObject> exploder)
    {
        return zge::tagByte(zge::objectComponent(exploder), kTagByte, kExplodedTag);
    }
    READABLE_FUNCTION(exploderExploded, Verified);

    // exploderSkipsBlast: whether it explodes without the blast effects.
    uint32_t exploderSkipsBlast(ee::Ptr<zge::SceneObject> exploder)
    {
        return zge::tagByte(zge::objectComponent(exploder), kTagByte, kSkipsBlastTag);
    }
    READABLE_FUNCTION(exploderSkipsBlast, Verified);

    // setExploderExploded: marked exploded or whole; shown and collided
    // with only while whole.
    void setExploderExploded(ee::Ptr<zge::SceneObject> exploder, uint32_t exploded)
    {
        const uint32_t whole = exploded == 0;
        zge::setTagByte(zge::objectComponent(exploder), kTagByte, kExplodedTag, exploded & 0xFFu);
        const uint64_t node = objectNodeHandle(exploder);
        rider::collision::showNode(node, whole);
        tail::setNodeCollidable(node, whole);
    }
    READABLE_FUNCTION_WAITS(setExploderExploded, Verified);

    // explodeExploder (kActivateMessage): a whole one explodes: its faces
    // thrown with the activation's velocity and its "activate" sound
    // (neither when the event's first word is set), then hidden, not
    // collided with, exploded.
    int32_t explodeExploder(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        const ee::Ptr<zge::SceneObject> exploder(message->target);
        const ee::Ptr<ActivationEvent> event(message->data);
        if (exploderExploded(exploder) == 1)
            return 0;
        const uint64_t node = objectNodeHandle(exploder);
        if (event->quiet == 0)
        {
            const ee::Ptr<zge::Vec4> position = zge::objectPosition(exploder);
            const uint32_t skipsBlast = exploderSkipsBlast(exploder);
            explodeNode(node, ee::Ptr<zge::Vec4>(&event->velocity), position, skipsBlast);
            zge::tagString(zge::levelObjectClassTag(ee::Ptr<zge::SceneObject>(message->target)), kTagText, kSoundNameTag, className.address());
            zge::formatString(soundName, kActivateSoundFormat, kTypeName, className.address());
            const ee::Ptr<zge::Vec4> at = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
            soundEvent(kObjectSoundEvent, at, kObjectSoundOnce, soundName.address(), kFullVolume);
        }
        rider::collision::showNode(node, 0);
        rider::collision::setNodeCollidable(node, 0);
        setExploderExploded(exploder, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(explodeExploder, Verified);

    // enableExploder (kEnableMessage): whole again.
    int32_t enableExploder(ee::Ptr<zge::EngineMessage> message)
    {
        setExploderExploded(ee::Ptr<zge::SceneObject>(message->target), 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableExploder, Verified);

    // explodeNode: every face of the node's model thrown as shards with
    // `velocity`; then, unless told to skip them, the two blast effects at
    // `position`, sized by the node's collision box (the sum of its three
    // sides: a fifth of it for the first effect, a tenth for the second).
    void explodeNode(uint64_t node, ee::Ptr<zge::Vec4> velocity, ee::Ptr<zge::Vec4> position, uint32_t skipsBlast)
    {
        ee::Local<rider::collision::NodeBounds> box;
        forEachNodeFace(node, NodeFaceVisit(sym::explodeFaceVisit), velocity);
        if (skipsBlast != 0)
            return;
        rider::collision::nodeCollisionBounds(node, box);
        ee::Float size = box->high[0] - box->low[0];
        size = size + box->high[1];
        size = size - box->low[1];
        size = size + box->high[2];
        size = size - box->low[2];
        const ee::Float blastSize = sce::libgcc::dptofp(sce::libgcc::dpmul(sce::libgcc::fptodp(size), *g_exploderBlastSizeScale));
        spawnSmoke(position, ee::Ptr<zge::Matrix>(), ee::Float(7.0f), ee::Float(1.0f), blastSize, *g_exploderHundredth);
        const ee::Float flashSize = sce::libgcc::dptofp(sce::libgcc::dpmul(sce::libgcc::fptodp(size), *g_exploderFlashSizeScale));
        spawnExplosionFire(position, ee::Float(5.0f), flashSize);
    }
    READABLE_FUNCTION_WAITS(explodeNode, Verified);

    // exploderCollide (kRiderTouchMessage and kMessage105): an exploded one
    // lets the collider through. A whole one whose tag says a collision
    // explodes it is activated by the collider's rider with the collision's
    // velocity (the game is asked first and may take the activation), and
    // lets the collider through; any other whole one leaves it pushed off.
    int32_t exploderCollide(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<ActivationEvent> event;
        ee::Local<zge::EngineMessage> activation;
        const ee::Ptr<zge::SceneObject> exploder(message->target);
        const ee::Ptr<RiderTouch> collision(message->data);
        if (exploderExploded(exploder) == 1)
        {
            collision->pushOff = 0;
            return 0;
        }
        if (zge::tagByte(zge::objectComponent(exploder), kTagByte, kBreaksOnCollisionTag) == 0)
            return 0;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(rider::playerIndexOfNode(message->sender));
        zge::memoryFill(event, 0, sizeof(ActivationEvent));
        event->quiet = 0;
        event->player = rider->player;
        if (message->type != kMessage105)
            event->velocity = zge::quadwordOf(ee::Ptr<zge::Vec4>(&collision->velocity));
        activation->sender = message->sender;
        activation->target = message->target;
        activation->type = kActivateMessage;
        activation->flags = zge::kMessageNow;
        activation->data = event;
        if (riderActivatedObject(activation) == 0)
            explodeExploder(activation);
        collision->pushOff = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(exploderCollide, Verified);

    namespace
    {
        // What an exploder keeps in a saved level state.
        struct ExploderSave
        {
            uint32_t exploded;           // +0x00
            uint8_t _04[0xC];
            zge::Vec4 position;          // +0x10
            zge::Quaternion orientation; // +0x20
        };
        static_assert(sizeof(ExploderSave) == 0x30 && offsetof(ExploderSave, orientation) == 0x20);
    }

    // exploderSaveSize (the engine's save size message).
    int32_t exploderSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(ExploderSave);
        return 0;
    }
    READABLE_FUNCTION(exploderSaveSize, Verified);

    // restoreExploder (the engine's restore message): exploded or whole as
    // saved, at its saved place.
    int32_t restoreExploder(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> exploder(message->target);
        objectNodeHandle(exploder);
        const ee::Ptr<ExploderSave> saved(message->data);
        setExploderExploded(exploder, saved->exploded);
        zge::setObjectPosition(exploder, &saved->position);
        zge::setObjectOrientation(exploder, &saved->orientation);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreExploder, Verified);

    // saveExploder (the engine's save message).
    int32_t saveExploder(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ExploderSave> saved(message->data);
        saved->exploded = exploderExploded(ee::Ptr<zge::SceneObject>(message->target));
        zge::aligned(ee::Ptr<zge::Vec4>(&saved->position)) = zge::quadwordOf(zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)));
        zge::quaternionCopy(&saved->orientation, zge::objectOrientation(ee::Ptr<zge::SceneObject>(message->target)));
        return 0;
    }
    READABLE_FUNCTION(saveExploder, Verified);

    // faceShortestSideSquared: the square of the shortest of a triangle's
    // three sides (explodeFace cuts a face up until this is small).
    ee::Float faceShortestSideSquared(ee::Ptr<NodeFace> face)
    {
        ee::Local<zge::Vec4> side;
        ee::Float shortest = *g_exploderLargest;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t next = (corner + 1) % 3;
            *side = face->corners[next].position - face->corners[corner].position;
            const ee::Float lengthSquared = zge::dot(*side, *side);
            if (lengthSquared < shortest)
                shortest = lengthSquared;
        }
        return shortest;
    }
    READABLE_FUNCTION(faceShortestSideSquared, Verified);

    // explodeFace: a triangle thrown as shards with the breaking velocity.
    // One small enough (or cut four times already) becomes one shard, of
    // the face's material and its first corner's colour, leaving along the
    // face's normal at a random speed on top of three quarters of the
    // breaking velocity. A larger one is cut in four through the middles of
    // its sides (positions, texture coordinates and colours halved between
    // the corners) and each quarter thrown the same way.
    void explodeFace(ee::Ptr<NodeFace> face, ee::Ptr<zge::Vec4> velocity)
    {
        ee::Local<zge::Vec4> carried;
        ee::Local<zge::Vec4[kCutPoints]> points;
        ee::Local<FaceUv[kCutPoints]> uvs;
        ee::Local<zge::VertexColour[kCutPoints]> colours;
        ee::Local<NodeFace> quarter;
        ee::Local<NodeFaceCorner[3]> quarterCorners;
        ee::Local<ShardTriangle> shard;
        ee::Local<CutTriangle[kCutTriangles]> cuts;
        for (uint32_t cut = 0; cut < kCutTriangles; ++cut)
            cuts[cut] = (*g_explodeFaceCuts)[cut];
        if (*g_explodeFaceDepth >= kMostCuts || faceShortestSideSquared(face) < ee::Float(kLargestShardSideSquared))
        {
            shard->material = face->material;
            shard->flags = 0;
            shard->colour = face->corners[0].colour;
            shard->lifetime = kShardLifetime;
            const uint32_t random = zge::randomNumber() & 0xFFu;
            const ee::Float speed = ee::toFloat(static_cast<int32_t>(random)) * *g_shardSpeedStep + *g_shardLeastSpeed;
            shard->velocity = face->normal * speed;
            *carried = *velocity * ee::Float(kShardVelocityShare);
            shard->velocity = shard->velocity + *carried;
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                shard->corners[corner].position = zge::quadwordOf(ee::Ptr<zge::Vec4>(&face->corners[corner].position));
                shard->corners[corner].u = face->corners[corner].u;
                shard->corners[corner].v = face->corners[corner].v;
            }
            spawnShardFromTriangle(shard);
            return;
        }
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t next = (corner + 1) % 3;
            const uint32_t middle = 3 + corner;
            const ee::Ptr<NodeFaceCorner> from(&face->corners[corner]);
            const ee::Ptr<NodeFaceCorner> to(&face->corners[next]);
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
            quarter->cornerCount = 3;
            quarter->material = face->material;
            quarter->normal = zge::quadwordOf(ee::Ptr<zge::Vec4>(&face->normal));
            quarter->corners = quarterCorners;
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const uint32_t point = cuts[cut].points[corner];
                quarter->corners[corner].position = points[point];
                quarter->corners[corner].u = uvs[point].u;
                quarter->corners[corner].v = uvs[point].v;
                quarter->corners[corner].colour = colours[point];
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
}
