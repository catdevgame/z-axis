// The level objects' sounds (object_sounds.h): started from the objects that
// have one (the traffic lights, the cars, the dynamics objects) and kept by
// object, the one-shot sounds the objects' messages play, and the frame that
// moves the kept sounds with their objects. DM2's own code (its game block,
// after the level objects).

#include "object_sounds.h"

#include "dynamics_objects.h"

#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/tree.h"
#include "zge/memory/pool.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/object_sounds.cpp.h"

namespace game::dm2
{
    namespace
    {
        // An object's sound: its node in the tree, the object (the key) and
        // its sound source.
        struct ObjectSound
        {
            zge::SearchTreeNode node; // +0x00
            uint32_t object;          // +0x08
            uint32_t source;          // +0x0C
        };
        static_assert(offsetof(ObjectSound, object) == zge::kSearchTreeValue && sizeof(ObjectSound) == 0x10);
        // The record a tree node starts (0 for none).
        ee::Ptr<ObjectSound> objectSoundOf(ee::Ptr<zge::SearchTreeNode> node) { return node.cast<ObjectSound>(); }
        // The records' pool (made with the first sound: 64 a block) and the
        // tree of them by object.
        inline ee::Global<uint32_t, sym::g_objectSoundPool> g_objectSoundPool;
        inline ee::Global<zge::SearchTree, sym::g_objectSounds> g_objectSounds;
        constexpr uint32_t kObjectSoundsABlock = 0x40;
        // The sounds an object can have, and where they start in the level
        // bank: an object's sound n of row r is the bank's sample
        // r * 16 + n + 7 (row 0: the one kept playing, startObjectSound's).
        constexpr uint32_t kObjectSounds = 16, kFirstObjectSample = 7, kSoundsARow = 16;
        // soundEvent's sound sources (38): b 0 starts one (at level d), 1
        // stops source c, 2 moves source c to a (level d), 3 plays the bank's
        // c once at a.
        constexpr uint32_t kSoundSourcesEvent = 38, kStartSource = 0, kStopSource = 1, kMoveSource = 2, kPlayOnce = 3;
        constexpr uint32_t kFullLevel = 0x7F;

        // The rows of the one-shot sounds the messages play (objectSoundMessage).
        enum ObjectSoundRow : int32_t
        {
            kNoRow = -1,
            kRowActivated = 1,  // the activate message
            kRowUpdate = 2,     // every update message
            kRowMessage101 = 3, // message 0x101
            kRowRiderTouch = 4, // a rider touches it (at most once in kRecentTouchFrames)
            kRowStallTouch = 5, // a rider touches it heading into a stall or a lip trick
            kRowGrindTouch = 6, // a rider touches it heading into a grind
            kRowBailTouch = 7,  // a bailing rider touches it (at most once in kRecentTouchFrames)
        };
        // The activate message (goals.cpp's kActivateMessage): its data's
        // first word 0 (as every sender clears it) plays the row.
        constexpr uint32_t kActivateMessage = 0x100;
        // Scaffolding (unnamed-field): a message no DM2 code sends (none of
        // its senders pass 0x101); it plays row 3.
        constexpr uint32_t kMessage101 = 0x101;
        // playerIndexOfNode's answer for a node no rider's.
        constexpr uint32_t kNoRiderOfNode = 255;

        // A touch's sound lately played from an object: the same row plays
        // from it again only once the record's frames are counted out
        // (ageRecentTouches, every frame).
        struct RecentTouch
        {
            uint32_t object;    // +0x00
            uint32_t row;       // +0x04 ObjectSoundRow
            int32_t framesLeft; // +0x08
        };
        static_assert(sizeof(RecentTouch) == 0x0C);
        // The records' pool (made with the first: 4 a block, destroyed once
        // none is left).
        inline ee::Global<uint32_t, sym::g_recentTouchPool> g_recentTouchPool;
        constexpr uint32_t kRecentTouchesABlock = 4;
        constexpr int32_t kRecentTouchFrames = 45;
    }

    // startObjectSound (object, sound): the object's sound (1 .. 16) started
    // at its position as a source of the level bank's sample sound + 7, and
    // kept by object in the tree (its pool made the first time).
    void startObjectSound(ee::Ptr<zge::SceneObject> object, uint32_t sound)
    {
        if (!(sound - 1 < kObjectSounds))
            return;
        const int32_t source = soundEvent(kSoundSourcesEvent, zge::objectPosition(object), kStartSource,
                                          sound + kFirstObjectSample, kFullLevel);
        if (source == static_cast<int32_t>(kNoSource))
            return;
        if (*g_objectSoundPool == 0)
        {
            *g_objectSoundPool = zge::createPool(sizeof(ObjectSound), kObjectSoundsABlock, 0);
            zge::initSearchTree(&*g_objectSounds, sym::lessWord);
        }
        const ee::Ptr<ObjectSound> record(zge::poolAlloc(ee::Ptr<zge::Pool>(*g_objectSoundPool)));
        record->object = ee::Address(object);
        record->source = static_cast<uint32_t>(source);
        game::tail(zge::searchTreeInsert, &*g_objectSounds, &record->node);
    }
    READABLE_FUNCTION_WAITS(startObjectSound, Verified);

    // findRecentTouch (object, row, anyRow): the object's recent touch of
    // the row (of any row with anyRow), or 0.
    uint32_t findRecentTouch(uint32_t object, uint32_t row, uint32_t anyRow)
    {
        if (*g_recentTouchPool == 0)
            return 0;
        for (ee::Ptr<zge::ListNode> link = zge::firstPoolLink(ee::Ptr<zge::Pool>(*g_recentTouchPool)); link;
             link = zge::nextPoolLink(link))
        {
            const ee::Ptr<RecentTouch> touch = zge::poolRecordAs<RecentTouch>(link);
            if (touch->object == object && (anyRow || touch->row == row))
                return ee::Address(touch);
        }
        return 0;
    }
    READABLE_FUNCTION(findRecentTouch, Verified);

    // ageRecentTouches: a frame counted off each recent touch; one counted
    // out is let go.
    void ageRecentTouches()
    {
        if (*g_recentTouchPool == 0)
            return;
        ee::Ptr<zge::ListNode> link = zge::firstPoolLink(ee::Ptr<zge::Pool>(*g_recentTouchPool));
        while (link)
        {
            const ee::Ptr<RecentTouch> touch = zge::poolRecordAs<RecentTouch>(link);
            link = zge::nextPoolLink(link);
            if (--touch->framesLeft == 0)
                zge::poolFree(ee::Ptr<zge::Pool>(*g_recentTouchPool), touch);
        }
    }
    READABLE_FUNCTION(ageRecentTouches, Verified);

    // addRecentTouch (object, row): the touch's sound kept as played for
    // the next 45 frames (the pool made the first time).
    void addRecentTouch(uint32_t object, uint32_t row)
    {
        if (*g_recentTouchPool == 0)
            *g_recentTouchPool = zge::createPool(sizeof(RecentTouch), kRecentTouchesABlock, 0);
        const ee::Ptr<RecentTouch> touch(zge::poolAlloc(ee::Ptr<zge::Pool>(*g_recentTouchPool)));
        touch->object = object;
        touch->row = row;
        touch->framesLeft = kRecentTouchFrames;
    }
    READABLE_FUNCTION(addRecentTouch, Verified);

    // dropRecentTouches (object, row, anyRow): the object's recent touches
    // of the row (of every row with anyRow) let go; the pool destroyed when
    // none is left.
    void dropRecentTouches(uint32_t object, uint32_t row, uint32_t anyRow)
    {
        if (*g_recentTouchPool == 0)
            return;
        while (const uint32_t touch = findRecentTouch(object, row, anyRow))
            zge::poolFree(ee::Ptr<zge::Pool>(*g_recentTouchPool), touch);
        if (zge::poolCount(ee::Ptr<zge::Pool>(*g_recentTouchPool)) == 0)
        {
            zge::destroyPool(ee::Ptr<zge::Pool>(*g_recentTouchPool));
            *g_recentTouchPool = 0;
        }
    }
    READABLE_FUNCTION(dropRecentTouches, Draft);

    // objectSoundMessage (message, sound): the object's sound (1 .. 16) has
    // its turn at a message the object took: the activate message (its
    // data's first word 0), every update and message 0x101 play their row
    // once at the object; a rider's touch plays the row of what the rider
    // is doing (a plain touch or a bail's at most once in 45 frames from
    // one object); the removed message stops the kept sound and lets its
    // record go. soundEvent's answer, or -1 when nothing played.
    int32_t objectSoundMessage(ee::Ptr<zge::EngineMessage> message, uint32_t sound)
    {
        int32_t row = kNoRow;
        if (sound - 1 < kObjectSounds)
        {
            switch (message->type)
            {
            case kTouchMessage:
            {
                const uint32_t index = rider::playerIndexOfNode(message->sender);
                if (index == kNoRiderOfNode)
                    break;
                const uint8_t state = rider::physicsState(rider::riderOf(index));
                if (state == rider::kPhysicsGrindApproach)
                    row = kRowGrindTouch;
                else if (state == rider::kPhysicsStallApproach || state == rider::kPhysicsLipApproach)
                    row = kRowStallTouch;
                else
                {
                    const uint32_t touchRow = state == rider::kPhysicsBail ? uint32_t(kRowBailTouch) : uint32_t(kRowRiderTouch);
                    if (!findRecentTouch(message->target, touchRow, 0))
                    {
                        addRecentTouch(message->target, touchRow);
                        row = static_cast<int32_t>(touchRow);
                    }
                }
                break;
            }
            case kActivateMessage:
                if (*ee::Ptr<uint32_t>(message->data) == 0)
                    row = kRowActivated;
                break;
            case kMessage101:
                row = kRowMessage101;
                break;
            case zge::kUpdateMessage:
                row = kRowUpdate;
                break;
            case zge::kRemovedMessage:
            {
                const ee::Ptr<ObjectSound> kept = objectSoundOf(zge::searchTreeFind(&*g_objectSounds, &message->target));
                if (!kept)
                    break;
                dropRecentTouches(message->target, 0, 1);
                soundEvent(kSoundSourcesEvent, zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)), kStopSource,
                           kept->source, 0);
                zge::searchTreeRemove(&*g_objectSounds, &message->target);
                zge::poolFree(ee::Ptr<zge::Pool>(*g_objectSoundPool), kept);
                if (zge::poolCount(ee::Ptr<zge::Pool>(*g_objectSoundPool)) == 0)
                {
                    zge::destroyPool(ee::Ptr<zge::Pool>(*g_objectSoundPool));
                    *g_objectSoundPool = 0;
                }
                break;
            }
            default:
                break;
            }
        }
        if (row == kNoRow)
            return -1;
        return game::tail(soundEvent, kSoundSourcesEvent, zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)),
                          kPlayOnce, static_cast<uint32_t>(row) * kSoundsARow + sound + kFirstObjectSample, kFullLevel);
    }
    READABLE_FUNCTION_WAITS(objectSoundMessage, Verified);

    // objectSoundsFrame: each kept sound moved to its object (full level),
    // then the recent touches aged a frame (the play frame's and the
    // replay's, after the objects' update).
    void objectSoundsFrame()
    {
        if (*g_objectSoundPool != 0)
        {
            for (ee::Ptr<zge::ListNode> link = zge::firstPoolLink(ee::Ptr<zge::Pool>(*g_objectSoundPool)); link;
                 link = zge::nextPoolLink(link))
            {
                const ee::Ptr<ObjectSound> kept = zge::poolRecordAs<ObjectSound>(link);
                soundEvent(kSoundSourcesEvent, zge::objectPosition(ee::Ptr<zge::SceneObject>(kept->object)), kMoveSource, kept->source,
                           kFullLevel);
            }
        }
        game::tail(ageRecentTouches);
    }
    READABLE_FUNCTION_WAITS(objectSoundsFrame, Verified);

    // freeObjectSounds: the level's end: every kept sound stopped, both
    // pools destroyed, the tree emptied.
    void freeObjectSounds()
    {
        if (*g_objectSoundPool != 0)
        {
            for (ee::Ptr<zge::ListNode> link = zge::firstPoolLink(ee::Ptr<zge::Pool>(*g_objectSoundPool)); link;
                 link = zge::nextPoolLink(link))
            {
                const ee::Ptr<ObjectSound> kept = zge::poolRecordAs<ObjectSound>(link);
                soundEvent(kSoundSourcesEvent, zge::objectPosition(ee::Ptr<zge::SceneObject>(kept->object)), kStopSource, kept->source, 0);
            }
            zge::destroyPool(ee::Ptr<zge::Pool>(*g_objectSoundPool));
            *g_objectSoundPool = 0;
        }
        if (*g_recentTouchPool != 0)
        {
            zge::destroyPool(ee::Ptr<zge::Pool>(*g_recentTouchPool));
            *g_recentTouchPool = 0;
        }
        game::tail(zge::initSearchTree, &*g_objectSounds, sym::lessWord);
    }
    READABLE_FUNCTION_WAITS(freeObjectSounds, Verified);
}
