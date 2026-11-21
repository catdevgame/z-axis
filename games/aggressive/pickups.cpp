// The levels' NPCs. The file and the first functions keep the name an early
// round gave them, "pickup": they are NPC objects (initNpcObject makes the
// block; their messages are NPCSkaterCollision, NPCAnimEnded, AreYouCloser).
// Each has a looping sound and a collision box; the Tutorial has 20.
//
// A walking NPC follows the nearest path; at a junction it picks an exit
// from the walks' clock. Run into by the skater it is pushed aside along the
// skater's motion (mode kNpcKnocked: moving for 0.3 s, the crash animation),
// plays its angry animation after 1.5 s (kNpcAngry), faces the skater while
// it is on screen, and is back home the first frame it is off screen.
// A level's script may answer NPCSkaterCollision itself.
//
// Above an NPC can hang a challenge bubble (createNPCBubble): two nodes that
// spin. The pointers that bob and spin (createNPCPointer) are in no shipped
// level script (every ASSETS/LEVELS/*/*.BIN disassembled), so the bobbing half
// of spinPickup never runs in play.

#include "aggressive/game_state.h"
#include "aggressive/level/matrix_points.h"
#include "aggressive/npc.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"
#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/sweep.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/locators.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/memory/regions.h"
#include "zge/sound/bank.h"
#include "zge/sound/sound.h"
#include "zge/collision/collision.h"
#include "zge/scene/follower.h"
#include "zge/scene/scene_database.h"
#include "aggressive/level/level_collision.h"
#include "sce/libgcc.h"
#include "aggressive/skater/moves.h"
#include "aggressive/pads.h"
#include "aggressive/sound/events.h"
#include "aggressive/level/scene_objects.h"
#include "aggressive/level/traffic_car.h"
#include "aggressive/level/closers.h"
#include "aggressive/level/npc_types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/pickups.cpp.h"

namespace aggressive
{
    namespace
    {
        using skater::Player;
        using zge::EngineMessage;
        using zge::Matrix;
        using zge::Quaternion;
        using zge::SceneObject;
        using zge::Vec4;

        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(sym::memset, m, c, n); }

        // The walks' clock (seconds summed over every walking NPC's frames);
        // its low byte picks a junction's exit.
        inline ee::Global<ee::Float, sym::g_walkClock> g_walkClock;
        inline ee::Global<ee::Float, sym::g_degreesToAngle16> g_degreesToAngle; // 65536 / 360
        inline ee::Global<ee::Float, sym::g_riseTime> g_knockTime;              // 0.3 s
        inline ee::Global<ee::Float, sym::g_still> g_stillSpeed;                // 1e-5
        inline ee::Global<ee::Float, sym::g_blend> g_animationBlend;            // 0.1
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;

        // An NPC gets up 1.5 s after a knock.
        constexpr float kGetUpTime = 1.5f;
        constexpr float kFaceBlend = 0.25f;
        constexpr uint32_t kLoopAnimation = 3, kOnceAnimation = 4, kOnceMirrored = 12;

        ee::Ptr<SceneObject> objectOf(ee::Ptr<EngineMessage> message) { return ee::Ptr<SceneObject>(message->target); }
        bool standsStill(ee::Float walkSpeed) { return ee::abs(walkSpeed) < ee::Float(1e-05f); }
        ee::Ptr<zge::Motion> animationOf(ee::Ptr<NpcType> type, NpcAnimation which) { return type->animations[which].animation; }

        // A matrix's translation row.
        Vec4 &translationOf(Matrix &matrix) { return *reinterpret_cast<Vec4 *>(matrix.m[3]); }

        // A degree count as a 16-bit angle.
        int16_t angleOf(uint32_t degrees)
        {
            return static_cast<int16_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(degrees)) * *g_degreesToAngle));
        }

        void spinOn(uint32_t &degrees)
        {
            degrees += 5;
            if (degrees > 360u)
                degrees -= 360u;
        }

        // A node's world matrix turned about y, staying where it is.
        ee::Ptr<Matrix> turnInPlace(ee::Ptr<zge::Node> node, int32_t angle)
        {
            const ee::Ptr<Matrix> matrix = zge::nodeMatrix(node, 1);
            const Vec4 place = translationOf(*matrix);
            zge::matrixRotateY(matrix, matrix, angle);
            translationOf(*matrix) = place;
            return matrix;
        }
    }

    // pickupOf: the object's NPC block (a word tag of its component), 0
    // without a component.
    ee::Ptr<Npc> pickupOf(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(object);
        if (!component)
            return ee::Ptr<Npc>();
        return ee::Ptr<Npc>(zge::tagWord(component, 8u, 0x40u));
    }
    READABLE_FUNCTION(pickupOf, Verified);

    // movePickupBox: the NPC's collision box (1.5 wide, 3 high, standing on
    // its position) moved in the level's sweep. 1.
    int32_t movePickupBox(ee::Ptr<SceneObject>, ee::Ptr<Npc> npc, ee::Float)
    {
        const Vec4 at = *zge::objectPosition(npc->object);
        const ee::Float half = 0.75f, height = 3.0f;
        const ee::Ptr<zge::SweepBox> box = npc->box;
        box->lowEnds[0]->value = at.x - half;
        box->lowEnds[1]->value = at.y - height;
        box->lowEnds[2]->value = at.z - half;
        box->lowEnds[0]->other->value = at.x + half;
        box->lowEnds[1]->other->value = at.y;
        box->lowEnds[2]->other->value = at.z + half;
        const ee::Ptr<zge::Sweep> sweep = levelSweep();
        zge::sweepUpdateBox(sweep, npc->box);
        return 1;
    }
    READABLE_FUNCTION(movePickupBox, Verified);

    // spinPickup: the NPC's bubble turned and its pointers bobbed for a frame. 1.
    int32_t spinPickup(ee::Ptr<SceneObject>, ee::Ptr<Npc> npc, ee::Float)
    {
        // The bob's step, turned into each pointer's frame in turn: its x and
        // z carry over from one pointer to the next.
        ee::Local<Vec4> step;
        memset(step, 0, sizeof(Vec4));
        ee::Ptr<Matrix> matrix;
        const ee::Ptr<NpcBubble> bubble = npc ? npc->bubble : ee::Ptr<NpcBubble>();
        if (bubble)
        {
            static constexpr int32_t kTurns[4] = {0x38E, -0x16C, -0x16C, -0x16C}; // 5 degrees, the rest -2 back
            for (int i = 0; i < 4; ++i)
            {
                if (bubble->node[i])
                    matrix = turnInPlace(bubble->node[i], kTurns[i]);
            }
        }
        for (const ee::Ptr<NpcPointer> pointer : npc->pointers)
        {
            if (!pointer || !pointer->node)
                continue;
            // Up 1/16 a frame for 8 frames, then down for 8.
            if (pointer->rising)
            {
                step->y = 0.0625f;
                pointer->bob += 1;
                if (pointer->bob == 8)
                    pointer->rising = 0;
            }
            else
            {
                step->y = -0.0625f;
                pointer->bob -= 1;
                if (pointer->bob == -8)
                    pointer->rising = 1;
            }
            if (pointer->bob == 0)
            {
                // At the middle: its rotation put back. The spin that follows
                // turns the matrix the last turn left, the bubble's or the
                // pointer before's.
                zge::setNodeLocalMatrix(pointer->node, &pointer->matrix);
                spinOn(pointer->spin);
                zge::matrixRotateY(matrix, matrix, static_cast<int32_t>(angleOf(pointer->spin)));
            }
            else
            {
                matrix = zge::nodeMatrix(pointer->node, 1);
                *step = level::throughRotation(pointer->matrix, *step);
                translationOf(*matrix) = translationOf(*matrix) + *step;
                spinOn(pointer->spin);
                zge::matrixRotateY(matrix, matrix, 0x38E);
            }
        }
        return 1;
    }
    READABLE_FUNCTION(spinPickup, Verified);

    // walkPickup: an NPC's walk along its path for `seconds`: the follower
    // moved on by its speed; at a segment's end (along 1 going forward, 0 going
    // back) one of the segment's exits at that end is taken (three at most,
    // the segment itself not counted; the first one set from the walks'
    // clock), the follower put on it at the nearest point to where it got
    // to, snapped to its nearer end and sent away from it; with no exit it
    // turns back. Then its object is put at the follower's position and, when
    // it is on screen, turned along the path. Returns 1.
    uint32_t walkPickup(ee::Ptr<Npc> npc, ee::Float seconds)
    {
        const ee::Ptr<zge::Follower> follower = npc->follower;
        ee::Local<Vec4> before;
        ee::Local<Vec4> position;
        ee::Local<Matrix> orientation;
        ee::Local<Quaternion> rotation;
        *g_walkClock = *g_walkClock + seconds;
        zge::followerPosition(follower, before);
        zge::advanceFollower(follower, seconds);
        zge::followerPosition(follower, position);
        const ee::Float along = zge::followerAlong(follower);
        const ee::Float speed = zge::followerSpeed(follower);
        if ((along >= 1.0f && speed > 0.0f) || (along <= 0.0f && speed < 0.0f))
        {
            const ee::Ptr<zge::PathRecord> segment = zge::followerSegment(follower);
            ee::Ptr<zge::PathRecord> exits[3];
            uint32_t found = 0;
            for (uint32_t i = 0; i < 3; ++i)
            {
                exits[i] = zge::segmentExit(zge::followerSegment(follower), i, along <= 0.0f ? 1u : 0u);
                if (exits[i] == segment)
                    exits[i] = {};
                else if (exits[i] != 0)
                    ++found;
            }
            if (found == 0)
                zge::setFollowerSpeed(follower, -speed);
            else
            {
                uint32_t pick = g_walkClock->bits() & 0x15u;
                while (exits[pick % 3] == 0)
                    ++pick;
                zge::setFollowerSegment(follower, exits[pick % 3]);
                zge::followerSnap(follower, position);
                ee::Float end = zge::followerAlong(follower);
                if (end < 1.0f && end > 0.0f)
                {
                    end = end > 0.5f ? ee::Float(1.0f) : ee::Float(0.0f);
                    zge::setFollowerAlong(follower, end);
                }
                const ee::Float pace = zge::followerSpeed(follower);
                if (end <= 0.0f)
                    zge::setFollowerSpeed(follower, ee::abs(pace));
                else if (end >= 1.0f)
                    zge::setFollowerSpeed(follower, -ee::abs(pace));
            }
        }
        zge::followerPosition(follower, position);
        zge::setObjectPosition(npc->object, position);
        if (objectDrawn(npc->object) != 0)
        {
            const ee::Ptr<const zge::Vec3> up(upVector()); // upVector's result word
            zge::followerOrientation(follower, up, orientation);
            zge::matrixToQuaternion(rotation, orientation);
            zge::setObjectOrientation(npc->object, rotation);
        }
        return 1;
    }
    READABLE_FUNCTION(walkPickup, Verified);

    namespace
    {
        void lookAtMatrix(ee::Ptr<Matrix> matrix, ee::Address eye, ee::Ptr<Vec4> target, uint32_t roll) { ee::game(sym::lookAtMatrix, matrix, eye, target, roll); }
    }

    // updatePickup: an NPC's frame (its object's update message): its mode's
    // step, then its box and sound moved and its parts spun. 0, or what the
    // animation call returned when an NPC off screen is sent back home.
    uint32_t updatePickup(ee::Ptr<EngineMessage> message)
    {
        ee::Local<Vec4> moved;
        ee::Local<Vec4> target;
        ee::Local<Matrix> matrix;
        ee::Local<Quaternion> rotation;
        const ee::Ptr<Npc> npc = pickupOf(objectOf(message));
        const ee::Float seconds = *ee::Ptr<ee::Float>(message->data);
        switch (npc->mode)
        {
        case kNpcWalking:
            if (!npc->locked)
                walkPickup(npc, seconds);
            break;
        case kNpcKnocked:
        {
            npc->knockedTime = npc->knockedTime + seconds;
            const ee::Float left = *g_knockTime - npc->knockedTime;
            if (left > 0.0f)
            {
                // It slows to a stop as the time runs out.
                const Vec4 step = npc->knockVelocity * ((seconds * ee::Float(12.0f)) * left);
                *moved = step + *zge::objectPosition(npc->object);
                zge::setObjectPosition(npc->object, moved);
            }
            if (npc->knockedTime > kGetUpTime)
            {
                const ee::Ptr<NpcType> type = findNpcType(npc->typeHash);
                npc->mode = kNpcAngry;
                zge::playAnimation(npc->object, animationOf(type, kNpcAnimationAngry), kLoopAnimation, kFaceBlend, npc->animationSpeed);
            }
            break;
        }
        case kNpcAttacking:
        case kNpcAngry:
            if (objectDrawn(npc->object))
            {
                // On screen it faces the skater, once he is more than a unit away.
                Player &skater = currentLevel()->players[0];
                *target = *zge::objectPosition(npc->object);
                const Vec4 toSkater = skater.position - *target;
                if (zge::squaredLength(toSkater) > 1.0f)
                {
                    target->y = skater.drawnPosition.y;
                    lookAtMatrix(matrix, &skater.drawnPosition, target, 0);
                    zge::matrixToQuaternion(rotation, matrix);
                    zge::setObjectOrientation(npc->object, rotation);
                }
            }
            else
            {
                // Off screen: back home, on to the next mode.
                const ee::Ptr<NpcType> type = findNpcType(npc->typeHash);
                if (!npc->nextMode)
                    zge::playAnimation(npc->object, npc->scriptAnimation, npc->scriptAnimationMode, ee::Float(0.0f), npc->animationSpeed);
                npc->mode = npc->nextMode;
                if (npc->locked)
                    npc->nextMode = kNpcLocked;
                else
                    npc->nextMode = ee::abs(npc->walkSpeed) < *g_stillSpeed ? kNpcStanding : kNpcWalking;
                zge::setObjectPosition(npc->object, &npc->home);
                zge::setObjectOrientation(npc->object, &npc->homeRotation);
                const bool stands = npc->locked || npc->nextMode == kNpcStanding;
                if (type)
                {
                    const ee::Ptr<zge::Motion> next = animationOf(type, stands ? kNpcAnimationIdle : kNpcAnimationWalk);
                    if (next)
                        return zge::playAnimation(npc->object, next, kLoopAnimation, *g_animationBlend, npc->animationSpeed);
                }
            }
            break;
        default:
            break;
        }
        if (!npc->invisible)
        {
            movePickupBox(objectOf(message), npc, seconds);
            zge::setSourcePosition(npc->sound, zge::objectPosition(npc->object));
        }
        spinPickup(objectOf(message), npc, seconds);
        return 0;
    }
    READABLE_FUNCTION(updatePickup, Verified);

    namespace
    {
        constexpr ee::Ptr<const char> kFrameFlag(sym::g_textFrameFlag);
        constexpr ee::Ptr<const char> kNpcAnimEnded(sym::g_textNPCAnimEnded);
        constexpr ee::Ptr<const char> kHips(sym::g_textHips2);
        constexpr ee::Ptr<const char> kLeftFoot(sym::g_textLFoot);
        constexpr ee::Ptr<const char> kNpcName(sym::g_textNpc);
        constexpr ee::Ptr<const char> kAreYouCloser(sym::g_textAreYouCloser2);
        constexpr ee::Ptr<const char> kListenForActions(sym::g_textListenForActions);
        constexpr ee::Ptr<const char> kStateTag(sym::g_textStateTag);

        // An object's component tags: a table of words and a table of bytes.
        constexpr uint32_t kWordTags = 8, kByteTags = 2;
        constexpr uint32_t kTagType = 0, kTagWalkSpeed = 0x10, kTagNpc = 0x40, kTagVelocity = 36;
        constexpr uint32_t kTagLocked = 12, kTagSpeaks = 13, kTagGiver = 0x20, kTagSoundSample = 60, kTagVoiceKind = 61;

        constexpr int32_t kAnimationEnded = 4; // FrameFlag

        // The query AreYouCloser hands over (level/closers.cpp).
        struct CloserQuery
        {
            Vec4 position;     // +0x00
            uint32_t found;    // +0x10
            ee::Float nearest; // +0x14
            uint32_t kind;     // +0x18 0 a giver, 1 not one, 2 any
        };
        static_assert(offsetof(CloserQuery, kind) == 0x18);

        // What newBuffer is told about a tag table: one record of `recordBytes`.
        struct TagTableRequest
        {
            uint32_t kind : 4;
            uint32_t recordBytes : 10;
            uint32_t records : 18;
            uint32_t reserved;
        };
        static_assert(sizeof(TagTableRequest) == 8);

        // A record the level's loader hands an object's creator.
        struct LevelEntry
        {
            int32_t kind;             // +0x00 1: placed by a scene point
            ee::Ptr<zge::ScenePoint> point; // +0x04
        };

        inline ee::Global<int32_t, sym::g_npcAnimEndedHashPending> g_npcAnimEndedHashPending;
        inline ee::Global<int32_t, sym::g_npcAnimEndedHash> g_npcAnimEndedHash;
        inline ee::Global<int32_t, sym::g_footHashPending> g_footHashPending;
        inline ee::Global<int32_t, sym::g_hipsHash3> g_hipsHash;
        inline ee::Global<int32_t, sym::g_leftFootHash> g_leftFootHash;
        inline ee::Global<int32_t, sym::g_npcNameHashPending> g_npcNameHashPending;
        inline ee::Global<int32_t, sym::g_npcNameHash> g_npcNameHash;
        inline ee::Global<uint8_t, sym::g_gameTypeByte> g_gameTypeByte;
        constexpr uint32_t kGameTypeCareer = 1;
    }

    void setNodeTagTable(ee::Ptr<zge::ScenePoint> point, ee::Ptr<zge::Node> rootNode);

    // spawnNpcModel: an NPC's model by the type in its tags, placed at the
    // scene point.
    ee::Ptr<SceneObject> spawnNpcModel(ee::Ptr<zge::ScenePoint> point)
    {
        const int32_t type = zge::tagWord(point->tags, kWordTags, kTagType);
        const ee::Ptr<SceneObject> model = zge::newSceneModelByHash(1, type);
        const ee::Ptr<zge::Node> root = zge::sceneRootNode(zge::sceneOf(model));
        zge::setNodeNameHash(root, point->nameHash);
        setNodeTagTable(point, root);
        zge::setObjectPosition(model, &point->position);
        zge::setObjectOrientation(model, &point->orientation);
        return model;
    }
    READABLE_FUNCTION_WAITS(spawnNpcModel, Verified);

    // setNodeTagTable: the point's tags copied into a buffer the node holds.
    void setNodeTagTable(ee::Ptr<zge::ScenePoint> point, ee::Ptr<zge::Node> rootNode)
    {
        ee::Local<TagTableRequest> request;
        request->kind = 3;
        request->recordBytes = point->tagBytes;
        request->records = 1;
        request->reserved = 0;
        const ee::Ptr<zge::TagTableHeader> table = zge::newBuffer(request);
        zge::memoryCopy(table + 1, point->tags, point->tagBytes);
        zge::setNodeTags(rootNode, table);
        zge::releaseTagTable(table);
    }
    READABLE_FUNCTION_WAITS(setNodeTagTable, Verified);

    // tagFloat24Message: an object's velocity asked by message: the
    // component's three tag floats into the message's data.
    int32_t tagFloat24Message(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<ee::Float> out(message->data);
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(objectOf(message));
        for (uint32_t i = 0; i < 3; ++i)
            out[i] = zge::tagFloat(component, kWordTags, kTagVelocity + i * 4);
        return 1;
    }
    READABLE_FUNCTION(tagFloat24Message, Verified);

    // answerAreYouCloser: AreYouCloser: a shown NPC of the query's kind inside
    // the query's reach, nearer than the query's nearest, is the new nearest.
    int32_t answerAreYouCloser(ee::Ptr<EngineMessage> message)
    {
        ee::Local<Vec4> offset;
        const ee::Ptr<CloserQuery> query(message->data);
        const ee::Ptr<SceneObject> object = objectOf(message);
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(object);
        const ee::Ptr<Npc> npc = pickupOf(object);
        uint64_t asks = 0;
        if (npc->invisible)
            asks = query->kind != 1;
        else
        {
            int32_t wanted = 0;
            if (query->kind == 1)
                wanted = zge::tagByte(component, kByteTags, kTagGiver) == 0;
            else if (query->kind == 0)
                wanted = zge::tagByte(component, kByteTags, kTagGiver);
            else if (query->kind == 2)
                wanted = 1;
            if (wanted != 0 && (object->flags & zge::kObjectMoved) != 0)
                asks = 1;
            if (asks == 0)
                return 1;
            const uint32_t node = objectNode(object);
            if (node != 0)
                asks = zge::nodeShown(ee::Ptr<zge::Node>(node));
        }
        if (asks != 0)
        {
            *offset = *zge::objectPosition(object) - query->position;
            const ee::Float reach = query->nearest;
            if (ee::abs(offset->x) <= reach && ee::abs(offset->y) <= reach && ee::abs(offset->z) <= reach)
            {
                const ee::Float distance = zge::vec3Length(&offset->xyz());
                if (distance < query->nearest)
                {
                    query->nearest = distance;
                    query->found = message->target;
                }
            }
        }
        return 1;
    }
    READABLE_FUNCTION(answerAreYouCloser, Verified);

    // setTagByte20Message: ListenForActions: the giver tag from the message's data.
    int32_t setTagByte20Message(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<uint8_t> data(message->data);
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(objectOf(message));
        zge::setTagByte(component, kByteTags, kTagGiver, *data);
        return 1;
    }
    READABLE_FUNCTION(setTagByte20Message, Verified);

    // npcAnimationEvent: an animation's event on an NPC. At the end of an
    // animation a locked NPC tells its script (NPCAnimEnded) and a knocked
    // one starts its angry animation.
    int32_t npcAnimationEvent(ee::Ptr<EngineMessage> message)
    {
        if (*g_npcAnimEndedHashPending != 0)
        {
            *g_npcAnimEndedHash = zge::nameHash(kNpcAnimEnded);
            *g_npcAnimEndedHashPending = 0;
        }
        const ee::Ptr<Npc> npc = pickupOf(objectOf(message));
        const auto frameFlag = [&]
        {
            const int32_t field = zge::nameHash(kFrameFlag);
            return static_cast<int32_t>(zge::eventInt(ee::Ptr<zge::AnimationEvent>(message->data), field));
        };
        if (npc->mode > kNpcAngry)
            return 0;
        switch (npc->mode)
        {
        case kNpcLocked:
            if (frameFlag() == kAnimationEnded)
                zge::sendNewMessage(0u, npc->object, *g_npcAnimEndedHash, 0u, 1u);
            return 0;
        case kNpcKnocked:
            if (frameFlag() == kAnimationEnded)
            {
                const ee::Ptr<NpcType> type = findNpcType(npc->typeHash);
                const ee::Float speed = npc->animationSpeed;
                npc->mode = kNpcAngry;
                return zge::playAnimation(npc->object, animationOf(type, kNpcAnimationAngry), kLoopAnimation, kFaceBlend, speed);
            }
            frameFlag(); // the original asks a second time, and does nothing with it
            return 1;
        default:
            return 1;
        }
    }
    READABLE_FUNCTION_WAITS(npcAnimationEvent, Verified);

    // destroyNpcComponent: the object's end: what hangs on the NPC released,
    // out of the sweep, off its path, its sound stopped, the block freed.
    int32_t destroyNpcComponent(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(objectOf(message));
        const ee::Ptr<Npc> npc = pickupOf(objectOf(message));
        if (!npc)
            return 1;
        const ee::Ptr<NpcBubble> bubble = npc->bubble;
        if (bubble)
        {
            zge::releaseNode(bubble->node[0], 1);
            zge::releaseNode(bubble->node[1], 1);
            if (bubble->node[2])
                zge::releaseNode(bubble->node[2], 1);
            if (bubble->node[3])
                zge::releaseNode(bubble->node[3], 1);
            zge::heapFree(bubble);
        }
        for (ee::Ptr<NpcPointer> &pointer : npc->pointers)
        {
            if (pointer)
            {
                zge::releaseNode(pointer->node, 1);
                zge::heapFree(pointer);
                pointer = ee::Ptr<NpcPointer>();
            }
        }
        if (npc->box)
        {
            zge::removeSweepBody(levelSweep(), npc->box);
            npc->box = ee::Ptr<zge::SweepBox>();
        }
        if (npc->follower != 0)
        {
            zge::deletePathFollower(npc->follower);
            npc->follower = 0;
        }
        if (!npc->invisible)
            zge::stopSource(npc->sound);
        if (component != 0)
            zge::setTagWord(component, kWordTags, kTagNpc, 0);
        zge::heapFree(npc);
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyNpcComponent, Verified);

    // npcBoneHeight: how tall the model stands, foot to hips, over `height`
    // (what its animations' speed is scaled by); -1 without those bones.
    ee::Float npcBoneHeight(ee::Ptr<SceneObject> object, ee::Float height)
    {
        ee::Local<Vec4> origin;
        ee::Local<Vec4> footPoint;
        ee::Local<Vec4> hipsPoint;
        memset(origin, 0, sizeof(Vec4));
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(object);
        if (*g_footHashPending != 0)
        {
            *g_hipsHash = zge::nameHash(kHips);
            *g_leftFootHash = zge::nameHash(kLeftFoot);
            *g_footHashPending = 0;
        }
        if (scene)
        {
            const ee::Ptr<zge::SceneBone> foot = zge::findSceneModelBone(scene, *g_leftFootHash);
            const ee::Ptr<zge::SceneBone> hips = zge::findSceneModelBone(scene, *g_hipsHash);
            if (foot && hips)
            {
                zge::bonePoint(scene, foot, origin, footPoint, 0);
                zge::bonePoint(scene, hips, origin, hipsPoint, 0);
                (void)(*footPoint - *hipsPoint); // worked out on VU0, left there, unused: y is done again below
                return ee::abs(footPoint->y - hipsPoint->y) / height;
            }
        }
        return ee::Float(-1.0f);
    }
    READABLE_FUNCTION(npcBoneHeight, Verified);

    namespace
    {
        constexpr uint32_t kCrashSoundEvent = 30, kSpeakEvent = 0x50;

        // What an NPC that speaks says when it is hit: by its type, a line.
        struct HitLine
        {
            uint32_t firstType, lastType;
            uint32_t line;
        };
        constexpr HitLine kHitLines[] = {
            {0x0108A0A1u, 0x0108A0A3u, 198},
            {0x0B7B0D91u, 0x0B7B0D91u, 3},
            {0x0B7B0D92u, 0x0B7B0D92u, 7},
            {0x0B7B0D93u, 0x0B7B0D93u, 6},
            {0x0E1779C4u, 0x0E1779C4u, 5},
        };
    }

    // npcHitBySkater: the skater ran into an NPC: unless it is up already it
    // remembers where it stood, it is pushed aside of the skater's motion
    // (mode kNpcKnocked), a sound, the pad's rumble, the crash animation
    // mirrored by the side it was hit from, and a word from one that speaks.
    uint32_t npcHitBySkater(ee::Ptr<Npc> npc, ee::Ptr<Player> player)
    {
        ee::Local<Vec4> side;
        const ee::Ptr<NpcType> type = findNpcType(npc->typeHash);
        const uint32_t clock = zge::collisionClock();
        if (npc->mode != kNpcAngry)
        {
            npc->home = *zge::objectPosition(npc->object);
            zge::quaternionCopy(&npc->homeRotation, zge::objectOrientation(npc->object));
            npc->nextMode = npc->mode;
        }
        npc->mode = kNpcKnocked;
        const Vec4 heading = standsStill(player->speed) ? player->axes.last : zge::unit(player->velocity);
        *side = zge::unit(zge::cross(heading, *g_worldUp));
        const Vec4 toSkater = zge::unit(player->position - npc->home);
        uint32_t mode;
        if (zge::dot(toSkater, *side) > 0.0f)
        {
            mode = kOnceMirrored;
            zge::vec3Negate(&npc->knockVelocity.xyz(), &side->xyz());
        }
        else
        {
            mode = kOnceAnimation;
            npc->knockVelocity = *side;
        }
        soundEvent(kCrashSoundEvent, ee::Ptr<Vec4>(&npc->home), clock & 3u, clock >> 4 & 3u, 0);
        const uint32_t milliseconds = sce::libgcc::fptoui(tuningValue(kTuningVibrate, kVibrateNpcTime));
        vibrate(&player->controls, milliseconds, tuningValue(kTuningVibrate, kVibrateNpcSmall), tuningValue(kTuningVibrate, kVibrateNpcBig));
        const uint32_t result = zge::playAnimation(npc->object, animationOf(type, kNpcAnimationCrash), mode, kFaceBlend, npc->animationSpeed);
        if (npc->speaks != 0)
        {
            for (const HitLine &row : kHitLines)
            {
                if (npc->typeHash >= row.firstType && npc->typeHash <= row.lastType)
                    soundEvent(kSpeakEvent, row.line, npc->object, 0, 0);
            }
        }
        npc->knockedTime = 0.0f;
        return result;
    }
    READABLE_FUNCTION_WAITS(npcHitBySkater, Verified);

    // npcHash: "NPC"'s hash, once.
    int32_t npcHash()
    {
        if (*g_npcNameHashPending != 0)
        {
            *g_npcNameHash = zge::nameHash(kNpcName);
            *g_npcNameHashPending = 0;
        }
        return *g_npcNameHash;
    }
    READABLE_FUNCTION(npcHash, Verified);

    // hideNpcForFlyThru: over a fly-through: hidden, remembering it was shown.
    int32_t hideNpcForFlyThru(ee::Ptr<SceneObject> object)
    {
        if (npcHash() == zge::levelObjectTypeHash(object))
        {
            const ee::Ptr<Npc> npc = pickupOf(object);
            const ee::Ptr<zge::Node> node = zge::objectLevelNode(object);
            if (node)
            {
                if ((node->flags & zge::kNodeShown) != 0)
                {
                    npc->shownBeforeFlyThru = 1;
                    zge::setNodeShown(node, 0, 1);
                }
                else
                    npc->shownBeforeFlyThru = 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(hideNpcForFlyThru, Verified);

    // showNpcAfterFlyThru: shown again if it was before the fly-through.
    int32_t showNpcAfterFlyThru(ee::Ptr<SceneObject> object)
    {
        if (npcHash() == zge::levelObjectTypeHash(object))
        {
            const ee::Ptr<Npc> npc = pickupOf(object);
            if (npc->shownBeforeFlyThru != 0)
            {
                const ee::Ptr<zge::Node> node = zge::objectLevelNode(object);
                if (node)
                    zge::setNodeShown(node, 1, 1);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(showNpcAfterFlyThru, Verified);

    // spawnNpcModelIfReady: an entry's model when it has a scene point.
    ee::Ptr<SceneObject> spawnNpcModelIfReady(ee::Ptr<LevelEntry> entry)
    {
        if (entry->kind == 1)
            return spawnNpcModel(entry->point);
        return {};
    }
    READABLE_FUNCTION_WAITS(spawnNpcModelIfReady, Verified);

    namespace
    {
        void subscribe(ee::Ptr<SceneObject> object, ee::Ptr<const char> messageName, uint32_t handler) { zge::subscribe(object, zge::nameHash(messageName), handler); }

        constexpr uint32_t kVelocityMessage = 0x7002u;
        constexpr uint32_t kNpcSoundBank = 5;
        constexpr ee::Ptr<Vec4> kOrigin(sym::originVector); // where a sound starts
        // The model's height, foot to hips, a walk's speed is authored for.
        constexpr float kAuthoredHeight = 1.12208f;

        // A type that is never shown, has no box and no sound.
        constexpr uint32_t kInvisibleType = 0x04943D3Cu;
        // Three types are left to the scripts: no skater runs into them, they
        // stay shown out of a career, and their animations keep their speed.
        bool isScriptedType(uint32_t typeHash)
        {
            return typeHash == 0x0B0D2ED5u || typeHash == 0x08C163A5u || typeHash == 0x0B733D6Fu;
        }
    }

    // initNpcObject: a level's NPC object: its messages, its block, what its
    // tags say. Out of a career only the scripted types stay shown. One that
    // walks takes the path nearest it, and is locked without one. The model's
    // height scales its animations, and a visible one has a sweep box and a
    // looping sound.
    int32_t initNpcObject(ee::Ptr<SceneObject> object)
    {
        ee::Local<Vec4> position;
        ee::Local<Quaternion> rotation;
        aggressive::level::registerCloserListener(object);
        zge::subscribe(object, zge::kUpdateMessage, sym::updatePickup);
        zge::subscribe(object, kVelocityMessage, sym::tagFloat24Message);
        subscribe(object, kAreYouCloser, sym::answerAreYouCloser);
        subscribe(object, kListenForActions, sym::setTagByte20Message);
        subscribe(object, kStateTag, sym::npcAnimationEvent);
        zge::subscribe(object, zge::kRemovedMessage, sym::destroyNpcComponent);
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(object);
        const ee::Ptr<Npc> npc(zge::heapAllocate(sizeof(Npc)));
        zge::memoryFill(npc, 0, sizeof(Npc));
        npc->object = object;
        zge::setTagWord(component, kWordTags, kTagNpc, npc);
        *position = *zge::objectPosition(npc->object);
        zge::quaternionCopy(rotation, zge::objectOrientation(npc->object));
        npc->typeHash = zge::tagWord(component, kWordTags, kTagType);
        npc->walkSpeed = zge::tagFloat(component, kWordTags, kTagWalkSpeed);
        npc->locked = zge::tagByte(component, kByteTags, kTagLocked);

        if (*g_gameTypeByte != kGameTypeCareer && !isScriptedType(npc->typeHash))
            zge::setSceneObjectShown(object, 0);
        npc->scriptAnimation = {};
        npc->scriptAnimationMode = 0;
        npc->invisible = npc->typeHash == kInvisibleType;
        if (npc->invisible)
        {
            npc->locked = 1;
            zge::setSceneObjectShown(object, 0);
        }
        npc->follower = 0;
        bool noPath = false;
        if (!npc->locked && !standsStill(npc->walkSpeed))
        {
            const ee::Ptr<zge::PathRecord> path = zge::findNearestPath(position, 0);
            if (path != 0)
            {
                const ee::Float speed = zge::tagFloat(component, kWordTags, kTagWalkSpeed);
                npc->follower = zge::newPathFollower(path, 0, speed, ee::Float(0.0f));
                zge::followerSnap(npc->follower, position);
            }
            else
            {
                noPath = true;
                npc->locked = 1;
            }
        }
        npc->animationSpeed = ee::Float(-1.0f);
        if (!npc->invisible)
        {
            const ee::Ptr<zge::Sweep> sweep = levelSweep();
            if (sweep != 0)
            {
                const ee::Ptr<zge::Box> box = zge::nodeBox(zge::objectLevelNode(object));
                npc->box = zge::sweepAddBody(sweep, 2, npc, &box->low.x, &box->high.x);
            }
            if (!isScriptedType(npc->typeHash))
            {
                const ee::Float scale = npcBoneHeight(npc->object, ee::Float(kAuthoredHeight));
                if (!standsStill(scale - ee::Float(1.0f)))
                    npc->animationSpeed = scale;
            }
            npc->speaks = zge::tagByte(component, kByteTags, kTagSpeaks);
            npc->soundSample = static_cast<uint8_t>(zge::tagByte(component, kByteTags, kTagSoundSample));
            npc->voiceKind = static_cast<uint8_t>(zge::tagByte(component, kByteTags, kTagVoiceKind));
            const uint32_t bank = soundBankOfKind(kNpcSoundBank);
            npc->sound = static_cast<uint16_t>(zge::startSource(zge::sample(bank, npc->soundSample), kOrigin));
        }

        if (npc->locked)
        {
            npc->mode = kNpcLocked;
            npc->nextMode = kNpcLocked;
            if (!noPath)
                return 1;
        }
        else
        {
            const uint32_t mode = standsStill(npc->walkSpeed) ? kNpcStanding : kNpcWalking;
            npc->nextMode = mode;
            npc->mode = mode;
        }
        const ee::Ptr<NpcType> type = findNpcType(npc->typeHash);
        const auto play = [&](ee::Ptr<zge::Motion> animation)
        {
            return static_cast<int32_t>(zge::playAnimation(object, animation, kLoopAnimation, ee::Float(0.1f), npc->animationSpeed));
        };
        if (npc->mode == kNpcStanding)
            return play(animationOf(type, kNpcAnimationIdle));
        if (noPath && animationOf(type, kNpcAnimationIdle))
            return play(animationOf(type, kNpcAnimationIdle));
        if (animationOf(type, kNpcAnimationWalk) != 0)
            return play(animationOf(type, kNpcAnimationWalk));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initNpcObject, Verified);

    namespace
    {
        // The script object's slot for a name hash.
        ee::Ptr<ee::Ptr<SceneObject>> findObjects(ee::Ptr<uint32_t> names, uint32_t count) { return ee::game<ee::Ptr<ee::Ptr<SceneObject>>>(sym::findObjects, names, count); }

        constexpr uint32_t kBailNpc = 6; // skaterCrash's reason: thrown off an NPC

        constexpr ee::Ptr<const char> kNpcSkaterCollisionName(sym::g_textNPCSkaterCollision);
        constexpr ee::Ptr<const char> kPointerName(sym::g_textPointer);
        inline ee::Global<uint32_t[8], sym::bubbleIcons> g_bubbleIcons; // the icons' world objects (hashes)
        inline ee::Global<int32_t, sym::g_collisionHashPending> g_collisionHashPending;
        inline ee::Global<int32_t, sym::g_collisionHash> g_collisionHash;
        inline ee::Global<int32_t, sym::g_npcPointerObjectHash> g_pointerObject; // the pointer's world object (hash)
        inline ee::Global<int32_t, sym::g_bubbleObject> g_bubbleObject;        // the bubble's

        constexpr int32_t kQuarterTurn = -16384;
        constexpr float kBubbleHeight = 4.5f; // over the NPC (up is -y)
    }

    // lockNpc: the script's lockNPC: no more walking.
    int32_t lockNpc(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Npc> npc = pickupOf(object);
        if (!npc->locked)
        {
            if (npc->follower != 0)
            {
                zge::deletePathFollower(npc->follower);
                npc->follower = 0;
            }
            npc->nextMode = kNpcLocked;
            npc->locked = 1;
            npc->mode = kNpcLocked;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(lockNpc, Verified);

    // unlockNpc: the script's unlockNPC. The path is the one nearest what
    // the stack holds where a position would be: the original never sets it,
    // and the new follower is snapped to it as well. With a path that is
    // all, and the NPC stays locked: the original never clears `locked`, so it
    // does not walk. Without one it plays its idle or walk animation in mode
    // walking.
    int32_t unlockNpc(ee::Ptr<SceneObject> object)
    {
        ee::Local<Vec4> unsetPoint;
        const ee::Ptr<Npc> npc = pickupOf(object);
        if (!npc->locked)
            return 1;
        const ee::Ptr<zge::PathRecord> path = zge::findNearestPath(unsetPoint, 0);
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(object);
        if (path != 0)
        {
            const ee::Float speed = zge::tagFloat(component, kWordTags, kTagWalkSpeed);
            npc->follower = zge::newPathFollower(path, 0, speed, ee::Float(0.0f));
            zge::followerSnap(npc->follower, unsetPoint);
            return 0;
        }
        const ee::Ptr<NpcType> type = findNpcType(npc->typeHash);
        npc->nextMode = kNpcWalking;
        npc->mode = kNpcWalking;
        if (animationOf(type, kNpcAnimationIdle) != 0)
        {
            zge::playAnimation(object, animationOf(type, kNpcAnimationIdle), kLoopAnimation, ee::Float(0.1f), npc->animationSpeed);
            return 0;
        }
        if (animationOf(type, kNpcAnimationWalk) != 0)
            return zge::playAnimation(object, animationOf(type, kNpcAnimationWalk), kLoopAnimation, ee::Float(0.1f), npc->animationSpeed);
        return 0;
    }
    READABLE_FUNCTION_WAITS(unlockNpc, Verified);

    // npcVelocity: a walking NPC's velocity: along its path by its speed
    // (the vector (1, 1, 1) by the path's orientation, then the speed); 0
    // when it does not walk.
    int32_t npcVelocity(ee::Ptr<Npc> npc, ee::Ptr<Vec4> velocity)
    {
        ee::Local<Matrix> orientation;
        if (!npc->locked && npc->follower != 0)
        {
            zge::followerOrientation(npc->follower, ee::Ptr<const zge::Vec3>(upVector()), orientation); // upVector's result word
            zge::vec3SetOne(&velocity->xyz());
            *velocity = level::throughMatrix(*orientation, *velocity);
            *velocity = *velocity * followerSpeed(npc->follower);
            return 1;
        }
        *velocity = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        return 1;
    }
    READABLE_FUNCTION(npcVelocity, Verified);

    // npcSkaterCollision: the skater ran into an NPC. The scripted types take
    // no notice; by its mode the NPC is knocked aside (npcHitBySkater), a
    // locked one's script may answer first, and one in mode kNpcAttacking
    // attacks: the skater bails, back the way he came, or thrown off when
    // too slow to go back. 1, or 0 from a mode past the six there are.
    int32_t npcSkaterCollision(ee::Ptr<Npc> npc, ee::Ptr<Player> player)
    {
        if (*g_collisionHashPending != 0)
        {
            *g_collisionHash = zge::nameHash(kNpcSkaterCollisionName);
            *g_collisionHashPending = 0;
        }
        if (isScriptedType(npc->typeHash))
            return 1;
        switch (npc->mode)
        {
        case kNpcLocked:
            if (zge::sendNewMessage(0u, npc->object, *g_collisionHash, 0u, 1u) != 0)
                return 1;
            return npcHitBySkater(npc, player);
        case kNpcWalking:
        case kNpcStanding:
        case kNpcAngry:
            return npcHitBySkater(npc, player);
        case kNpcKnocked:
            return 1;
        case kNpcAttacking:
        {
            // The throw's w is whatever the stack held: this mode is never set.
            ee::Local<Vec4> thrown;
            const ee::Ptr<NpcType> type = findNpcType(npc->typeHash);
            zge::playAnimation(npc->object, animationOf(type, kNpcAnimationAttack), kOnceAnimation, kFaceBlend, npc->animationSpeed);
            if (zge::squaredLength(player->velocity) > 1.5f)
                zge::vec3Negate(&player->velocity.xyz(), &player->velocity.xyz());
            else
            {
                thrown->x = 0.0f;
                thrown->y = -3.0f;
                thrown->z = -5.0f;
                player->velocity = *thrown;
            }
            skater::skaterCrash(player, kBailNpc);
            return 1;
        }
        default:
            return 0;
        }
    }
    READABLE_FUNCTION_WAITS(npcSkaterCollision, Verified);

    // createNpcBubble: the script's createNPCBubble: over the NPC `name`, a
    // bubble and in it the icon `icon` asks for (any bubble it had goes
    // first). spinPickup spins it.
    int32_t createNpcBubble(uint32_t name, uint32_t /*unused*/, uint32_t /*unused*/, uint32_t icon, uint32_t argument5, uint32_t argument6)
    {
        ee::Local<Vec4> place;
        ee::Local<Matrix> matrix;
        ee::Local<uint32_t> names;
        *names = name;
        const ee::Ptr<ee::Ptr<SceneObject>> slot = findObjects(names, 1);
        if (!slot)
            return 1;
        const ee::Ptr<Npc> npc = pickupOf(*slot);
        if (npc && npc->bubble)
            destroyNpcBubble(*names);
        const ee::Ptr<NpcBubble> bubble(zge::heapAllocate(sizeof(NpcBubble)));
        npc->bubble = bubble;
        bubble->node[3] = ee::Ptr<zge::Node>();
        bubble->node[2] = ee::Ptr<zge::Node>();
        bubble->alwaysTwo = 2;
        bubble->npcName = *names;
        bubble->alwaysOne = 1;
        bubble->scriptArgument5 = argument5;
        bubble->scriptArgument6 = argument6;
        bubble->icon = icon;
        const uint32_t bubbleObject = zge::findWorldObjectByHash(*g_bubbleObject);
        if (bubbleObject == 0)
            return 1;
        bubble->node[0] = ee::Ptr<zge::Node>(zge::newAppInstanceNode(bubbleObject, 1));
        if (bubble->node[0])
        {
            const ee::Ptr<Quaternion> rotation = zge::objectOrientation(*slot);
            const ee::Ptr<Vec4> position = zge::objectPosition(*slot);
            zge::matrixIdentity(matrix);
            zge::quaternionToMatrix(matrix, rotation);
            zge::matrixRotateY(matrix, matrix, static_cast<zge::Angle>(kQuarterTurn));
            *place = *position;
            place->y = place->y - kBubbleHeight;
            translationOf(*matrix) = *place;
            zge::setNodeLocalMatrix(bubble->node[0], matrix);
            bubble->turn[2] = 0;
            bubble->turn[1] = 0;
            bubble->turn[0] = 0;
            const uint32_t iconObject = zge::findWorldObjectByHash((*g_bubbleIcons)[icon]);
            if (iconObject != 0)
            {
                bubble->node[1] = ee::Ptr<zge::Node>(zge::newAppInstanceNode(iconObject, 1));
                if (bubble->node[1])
                {
                    zge::setNodeLocalMatrix(bubble->node[1], matrix);
                    bubble->matrix = *matrix;
                }
            }
            bubble->ready = 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(createNpcBubble, Verified);

    // destroyNpcBubble: the script's destroyNPCBubble.
    int32_t destroyNpcBubble(uint32_t name)
    {
        ee::Local<uint32_t> names;
        *names = name;
        const ee::Ptr<ee::Ptr<SceneObject>> slot = findObjects(names, 1);
        if (!slot)
            return 1;
        const ee::Ptr<Npc> npc = pickupOf(*slot);
        const ee::Ptr<NpcBubble> bubble = npc->bubble;
        if (bubble)
        {
            zge::releaseNode(bubble->node[0], 1);
            zge::releaseNode(bubble->node[1], 1);
            if (bubble->node[2])
                zge::releaseNode(bubble->node[2], 1);
            if (bubble->node[3])
                zge::releaseNode(bubble->node[3], 1);
            zge::heapFree(bubble);
        }
        npc->bubble = ee::Ptr<NpcBubble>();
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyNpcBubble, Verified);

    // createNpcPointerNode: the script's createNPCPointer, and restoreNpcState:
    // a pointer over the NPC `name`, at `owner`'s point "pointer" (no shipped
    // script makes one). With all four rows taken the search stops at 4, and
    // the block lands on Npc::object, as in the original.
    int32_t createNpcPointerNode(uint32_t name, uint32_t owner)
    {
        ee::Local<Matrix> matrix;
        ee::Local<uint32_t> names;
        *names = name;
        const ee::Ptr<ee::Ptr<SceneObject>> slot = findObjects(names, 1);
        if (!slot)
            return 1;
        const ee::Ptr<Npc> npc = pickupOf(*slot);
        const ee::Ptr<ee::Ptr<NpcPointer>> rows = npc->pointers;
        uint32_t row = 0;
        while (row < 4 && rows[row])
            ++row;
        const ee::Ptr<NpcPointer> pointer(zge::heapAllocate(kNpcPointerBytes));
        const uint32_t worldObject = zge::findWorldObjectByHash(*g_pointerObject);
        if (worldObject != 0)
        {
            pointer->node = ee::Ptr<zge::Node>(zge::newAppInstanceNode(worldObject, 1));
            const ee::Ptr<zge::ScenePoint> point = zge::findScenePoint(zge::nameHash(kPointerName), owner, 0);
            if (!point)
                return 1;
            zge::matrixIdentity(matrix);
            zge::quaternionToMatrix(matrix, &point->orientation);
            translationOf(*matrix) = point->position;
            zge::setNodeLocalMatrix(pointer->node, matrix);
            pointer->matrix = *matrix;
            pointer->rising = 1;
            pointer->owner = owner;
            pointer->bob = 0;
            pointer->spin = 0;
            rows[row] = pointer;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(createNpcPointerNode, Verified);

    // destroyNpcPointerNode: the script's destroyNPCPointer: `owner`'s pointer
    // over the NPC `name`.
    int32_t destroyNpcPointerNode(uint32_t name, uint32_t owner)
    {
        ee::Local<uint32_t> names;
        *names = name;
        const ee::Ptr<ee::Ptr<SceneObject>> slot = findObjects(names, 1);
        if (slot)
        {
            const ee::Ptr<Npc> npc = pickupOf(*slot);
            for (ee::Ptr<NpcPointer> &pointer : npc->pointers)
            {
                if (pointer && pointer->owner == owner)
                {
                    zge::releaseNode(pointer->node, 1);
                    zge::heapFree(pointer);
                    pointer = ee::Ptr<NpcPointer>();
                    break;
                }
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyNpcPointerNode, Verified);

    namespace
    {
        namespace tail
        {
            int32_t forEachObject(uint32_t visit, uint32_t data) { return ee::tailCall<int32_t>(sym::forEachObject, visit, data); }
        }
    }

    // hideNpcsForFlyThru
    int32_t hideNpcsForFlyThru()
    {
        return tail::forEachObject(sym::hideNpcForFlyThru, 0);
    }
    READABLE_FUNCTION_WAITS(hideNpcsForFlyThru, Verified);

    // showNpcsAfterFlyThru
    int32_t showNpcsAfterFlyThru()
    {
        return tail::forEachObject(sym::showNpcAfterFlyThru, 0);
    }
    READABLE_FUNCTION_WAITS(showNpcsAfterFlyThru, Verified);

    uint32_t npcSaveSize()
    {
        return 40;
    }
    READABLE_FUNCTION(npcSaveSize, Verified);

    // ---- an NPC saved and restored with the level (were the sweep's
    // npcSaveSizeMessage .. restoreNpcState)
    namespace
    {
        // What an NPC saves (0x40 bytes), then its bubble's (NpcBubbleSave).
        struct NpcSave
        {
            uint32_t locked;              // +0x00
            uint32_t mode;                // +0x04
            uint32_t nextMode;            // +0x08
            uint32_t segment;             // +0x0C its path segment's id (walking NPCs)
            ee::Float along;              // +0x10 how far along it
            ee::Float speed;              // +0x14
            ee::Ptr<zge::Motion> scriptAnimation; // +0x18
            uint32_t scriptAnimationMode; // +0x1C
            Vec4 home;                    // +0x20
            zge::Quaternion homeRotation; // +0x30
        };
        static_assert(sizeof(NpcSave) == 0x40 && offsetof(NpcSave, home) == 0x20);

        // An NPC's bubble and its pointers' owners as saved (40 bytes).
        struct NpcBubbleSave
        {
            uint32_t npcName;          // +0x00
            uint32_t alwaysTwo;        // +0x04 (the bubble's +0x08 and +0x04 swapped)
            uint32_t alwaysOne;        // +0x08
            uint32_t icon;             // +0x0C
            uint32_t scriptArgument5;  // +0x10
            uint32_t scriptArgument6;  // +0x14
            uint32_t pointerOwners[4]; // +0x18 0: no pointer
        };
        static_assert(sizeof(NpcBubbleSave) == 40);

    }

    // saveNpcState: the NPC's bubble (its words and its pointers' owners)
    // into `buffer`: 1, or 0 with no bubble (nothing written).
    uint32_t saveNpcState(uint32_t buffer, ee::Ptr<Npc> npc)
    {
        ee::Local<NpcBubbleSave> save;
        const ee::Ptr<NpcBubble> bubble = npc->bubble;
        if (!bubble)
            return 0;
        save->npcName = bubble->npcName;
        save->alwaysTwo = bubble->alwaysTwo;
        save->alwaysOne = bubble->alwaysOne;
        save->icon = bubble->icon;
        save->scriptArgument5 = bubble->scriptArgument5;
        save->scriptArgument6 = bubble->scriptArgument6;
        for (uint32_t i = 0; i < 4; ++i)
            save->pointerOwners[i] = npc->pointers[i] ? npc->pointers[i]->owner : 0;
        zge::memoryCopy(buffer, save, sizeof(NpcBubbleSave));
        return 1;
    }
    READABLE_FUNCTION(saveNpcState, Verified);

    // restoreNpcState: the saved bubble made again over its NPC, and a
    // pointer for each owner saved. Returns 1.
    uint32_t restoreNpcState(uint32_t buffer)
    {
        ee::Local<NpcBubbleSave> save;
        zge::memoryCopy(save, buffer, sizeof(NpcBubbleSave));
        aggressive::createNpcBubble(save->npcName, save->alwaysTwo, save->alwaysOne, save->icon, save->scriptArgument5, save->scriptArgument6);
        for (const uint32_t owner : save->pointerOwners)
        {
            if (owner)
                aggressive::createNpcPointerNode(save->npcName, owner);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreNpcState, Verified);

    // npcSaveSizeMessage: (GetSpaceNeededForSaving) an NPC's save: its own
    // and the room for its bubble. Returns 1.
    uint32_t npcSaveSizeMessage(ee::Ptr<EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = npcSaveSize() + sizeof(NpcSave);
        return 1;
    }
    READABLE_FUNCTION(npcSaveSizeMessage, Tested);

    // npcSaveMessage: (SaveToBuffer) the NPC's state, and a walking NPC's
    // place on its path, into the buffer; its bubble after it.
    int32_t npcSaveMessage(ee::Ptr<EngineMessage> message)
    {
        ee::Local<NpcSave> save;
        const ee::Ptr<SaveRequest> request(message->data);
        zge::memoryFill(save, 0, sizeof(NpcSave));
        const ee::Ptr<Npc> npc = pickupOf(objectOf(message));
        save->locked = npc->locked;
        save->mode = npc->mode;
        save->nextMode = npc->nextMode;
        save->scriptAnimation = npc->scriptAnimation;
        save->scriptAnimationMode = npc->scriptAnimationMode;
        save->home = zge::quadwordOf(ee::Ptr<Vec4>(&npc->home));
        zge::quaternionCopy(&save->homeRotation, &npc->homeRotation);
        if (!npc->locked)
        {
            save->segment = zge::pathNameHash(zge::followerSegment(npc->follower));
            save->along = zge::followerAlong(npc->follower);
            save->speed = zge::followerSpeed(npc->follower);
        }
        zge::memoryCopy(request->buffer, save, sizeof(NpcSave));
        return static_cast<int32_t>(aggressive::saveNpcState(request->buffer + sizeof(NpcSave), npc));
    }
    READABLE_FUNCTION(npcSaveMessage, Tested);

    // npcRestoreMessage: (LoadFromBuffer) the NPC's state, and a walking
    // NPC's place on its path (a follower made if it has none), from the
    // buffer; its bubble after it. Note: as in the original, the home and
    // its rotation go the other way, from the NPC into the copy read: the
    // saved home is not restored.
    int32_t npcRestoreMessage(ee::Ptr<EngineMessage> message)
    {
        ee::Local<NpcSave> save;
        const ee::Ptr<SaveRequest> request(message->data);
        zge::memoryCopy(save, request->buffer, sizeof(NpcSave));
        const ee::Ptr<Npc> npc = pickupOf(objectOf(message));
        npc->locked = save->locked;
        npc->mode = save->mode;
        npc->nextMode = save->nextMode;
        npc->scriptAnimation = save->scriptAnimation;
        npc->scriptAnimationMode = save->scriptAnimationMode;
        save->home = zge::quadwordOf(ee::Ptr<Vec4>(&npc->home));
        zge::quaternionCopy(&save->homeRotation, &npc->homeRotation);
        if (!npc->locked)
        {
            const ee::Ptr<zge::PathRecord> path = zge::findPathRecord(save->segment);
            if (npc->follower)
            {
                zge::setFollowerSegment(npc->follower, path);
                zge::setFollowerAlong(npc->follower, save->along);
                zge::setFollowerSpeed(npc->follower, save->speed);
            }
            else
                npc->follower = zge::newPathFollower(path, 0, save->speed, save->along);
        }
        return static_cast<int32_t>(aggressive::restoreNpcState(request->buffer + sizeof(NpcSave)));
    }
    READABLE_FUNCTION_WAITS(npcRestoreMessage, Tested);
}
