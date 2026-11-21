// The level's movable sounds: the
// "MovableSound" level object type (registerLevelObjectTypes, load.cpp), a
// sound placed in the level that follows an object: a bone of a scene
// object its tag data names, or its own node. Its init hides the object,
// gives it a MovableSound record (in its tag data, word 12) and subscribes
// its messages: the frame's update moves a looping sound's source to the
// bone (or the node) and sets its level; the object's removal stops the
// source and frees the record; "play" plays the source bank's sample once
// there. A sound that is not one-shot has its source started at the init
// (the source bank's sample, at its level and pitch).
//

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/memory/regions.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/sound/bank.h"
#include "zge/sound/sound.h"
#include "zge/collision/collision.h"
#include "aggressive/sound/events.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/movable_sound.cpp.h"

namespace zge
{
    struct TagRecord;
}
namespace aggressive
{
}

namespace aggressive::level
{
    namespace
    {
        constexpr uint32_t kObjectComponent = sym::objectComponent;    // zge::objectComponent, by address
        constexpr uint32_t kTagWord = sym::tagWord;            // zge::tagWord, by address
        constexpr uint32_t kMemset = sym::memset;             // libc memset (dst, byte, bytes)
        constexpr uint32_t kSoundEvent = sym::soundEvent;         // soundEvent, by address

        constexpr uint32_t kUpdateMovableSound = sym::updateMovableSound;
        constexpr uint32_t kFreeMovableSound = sym::freeMovableSound;
        constexpr uint32_t kPlayMovableSound = sym::playMovableSound;

        // The engine's messages it takes (zge/scene/message.h): the frame's
        // update, and the one an object gets as it is removed (removeLevelObject
        // sends it, then takes the object off the level's lists).
        constexpr uint32_t kRemovedMessage = 0x7001u;
        constexpr uint32_t kPlayName = sym::playName;           // "play"
        // "play"'s name hash, made at the first init (its flag starts at 1).
        inline ee::Global<uint32_t, sym::g_playHashNeeded> g_playHashNeeded;
        inline ee::Global<uint32_t, sym::g_playHash> g_playHash;

        constexpr uint32_t kSourceBankKind = 5;               // soundBankOfKind's: the source bank
        constexpr uint32_t kStartPosition = sym::startPosition;      // (0, 20000, 0, 0): where the source starts (the update moves it)
        // soundEvent's: 50 a source's level; 57 the sound sources, its b 3
        // the source bank's sample c played once at a (at level d).
        constexpr uint32_t kSourceLevelEvent = 50;
        constexpr uint32_t kSourcesEvent = 57;
        constexpr uint32_t kPlayOnce = 3;

        // A movable sound's record (24 bytes; the object's tag word 12).
        struct MovableSound
        {
            ee::Ptr<zge::SceneObject> object; // +0x00 its own object (where it sounds without a bone)
            ee::Ptr<zge::SceneObject> model;  // +0x04 the scene object its tag word 4 names (0: none)
            ee::Ptr<zge::SceneBone> bone; // +0x08 the model's bone the tag word 8 names (0: the object's position)
            uint16_t source;  // +0x0C the looping sound's source
            // +0x10 non-zero (the class's tag byte 4): played on "play"
            // only; else a looping source started at the init, moved each
            // frame and stopped at the removal.
            uint32_t oneShot;
            uint8_t sample;   // +0x14 the source bank's sample (class tag byte 5)
            uint8_t level;    // +0x15 its level (class tag byte 6)
            uint16_t pitch;   // +0x16 (class tag word 8)
        };
        static_assert(sizeof(MovableSound) == 24 && offsetof(MovableSound, source) == 0x0C && offsetof(MovableSound, level) == 0x15);

        // The tag data's entries (tagByte's and tagWord's second argument
        // 2 and 8).
        constexpr uint32_t kByte = 2, kWord = 8;
        constexpr uint32_t kModelTag = 4, kBoneTag = 8, kRecordTag = 12;
        constexpr uint32_t kOneShotTag = 4, kSampleTag = 5, kLevelTag = 6, kPitchTag = 8;

        // The object's record (0 without tag data).
        ee::Ptr<MovableSound> movableSoundOf(ee::Ptr<zge::TagRecord> tags)
        {
            return ee::Ptr<MovableSound>(tags ? zge::tagWord(tags, kWord, kRecordTag) : 0);
        }
    }

    namespace
    {
        void memset(ee::Ptr<zge::Vec4> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // updateMovableSound (kUpdateMessage, an EngineMessage): a looping sound's
    // source moved to the bone's origin (or its node's position), then set
    // to its level. Returns 0 (the update goes on to the object's other
    // handlers).
    int32_t updateMovableSound(ee::Ptr<zge::EngineMessage> update)
    {
        ee::Local<zge::Vec4> out;
        ee::Local<zge::Vec4> origin;
        ee::Ptr<MovableSound> sound;
        if (const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(ee::Ptr<zge::SceneObject>(update->target)))
            sound = ee::Ptr<MovableSound>(zge::tagWord(tags, kWord, kRecordTag));
        if (!sound->oneShot)
        {
            if (sound->bone)
            {
                memset(origin, 0, 0x10);
                origin->w = 1.0f;
                zge::objectBonePoint(sound->model, sound->bone, origin, out, 0);
                zge::setSourcePosition(sound->source, out);
            }
            else
            {
                const uint16_t source = sound->source;
                zge::setSourcePosition(source, zge::objectPosition(sound->object));
            }
        }
        soundEvent(kSourceLevelEvent, sound->source, sound->level, 0, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateMovableSound, Verified);

    // freeMovableSound (kRemovedMessage, an EngineMessage): a looping source
    // stopped, the record taken off the tag data and freed.
    uint32_t freeMovableSound(ee::Ptr<zge::EngineMessage> removed)
    {
        // The tag data is asked for twice, as the original does.
        ee::Ptr<MovableSound> sound;
        const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(ee::Ptr<zge::SceneObject>(removed->target));
        const ee::Ptr<zge::TagRecord> recordTags = zge::objectComponent(ee::Ptr<zge::SceneObject>(removed->target));
        if (recordTags != 0)
            sound = movableSoundOf(recordTags);
        if (sound)
        {
            if (sound->oneShot == 0)
                zge::stopSource(sound->source);
            if (tags != 0)
                zge::setTagWord(tags, kWord, kRecordTag, 0);
            zge::heapFree(sound);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeMovableSound, Verified);

    // playMovableSound ("play", an EngineMessage): the source bank's sample
    // played once at the bone's origin (or the node's position).
    uint32_t playMovableSound(ee::Ptr<zge::EngineMessage> play)
    {
        ee::Local<zge::Vec4> out;
        ee::Local<zge::Vec4> origin;
        memset(origin, 0, 0x10);
        ee::Ptr<MovableSound> sound;
        const ee::Ptr<zge::SceneObject> target(play->target);
        origin->w = 1.0f;
        const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(target);
        if (tags != 0)
            sound = ee::Ptr<MovableSound>(zge::tagWord(tags, kWord, kRecordTag));
        if (sound->model)
        {
            zge::objectBonePoint(sound->model, sound->bone, origin, out, 0);
            soundEvent(kSourcesEvent, out, kPlayOnce, sound->sample, sound->level);
            return 1;
        }
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(sound->object);
        soundEvent(kSourcesEvent, position, kPlayOnce, sound->sample, sound->level);
        return 1;
    }
    READABLE_FUNCTION_WAITS(playMovableSound, Verified);

    // initMovableSound (the "MovableSound" type's init, a scene object): see the
    // top.
    uint32_t initMovableSound(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<zge::Node> geometry = zge::objectLevelNode(object);
        zge::setNodeShown(geometry, 0, 0);
        zge::setNodeCollides(geometry, 0, 0);
        if (*g_playHashNeeded != 0)
        {
            *g_playHashNeeded = 0;
            *g_playHash = zge::nameHash(ee::Ptr<const char>(kPlayName));
        }
        zge::subscribe(object, zge::kUpdateMessage, kUpdateMovableSound);
        zge::subscribe(object, kRemovedMessage, kFreeMovableSound);
        zge::subscribe(object, *g_playHash, kPlayMovableSound);

        // The record, from the object's tag data and its class's.
        const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(object);
        const ee::Ptr<MovableSound> sound = zge::heapAllocateAs<MovableSound>(sizeof(MovableSound));
        zge::memoryFill(sound, 0, sizeof(MovableSound));
        sound->object = object;
        const uint32_t modelName = zge::tagWord(tags, kWord, kModelTag);
        const uint32_t boneName = zge::tagWord(tags, kWord, kBoneTag);
        if (modelName != 0)
        {
            sound->model = ee::Ptr<zge::SceneObject>(zge::findSceneObject(modelName, 0, 0));
            if (sound->model)
                sound->bone = zge::findModelBone(sound->model, boneName);
        }
        zge::setTagWord(tags, kWord, kRecordTag, sound);
        const ee::Ptr<zge::TagRecord> classTags = zge::levelObjectClassTag(object);
        sound->oneShot = zge::tagByte(classTags, kByte, kOneShotTag);
        sound->sample = static_cast<uint8_t>(zge::tagByte(classTags, kByte, kSampleTag));
        sound->level = static_cast<uint8_t>(zge::tagByte(classTags, kByte, kLevelTag));
        const int32_t pitch = zge::tagWord(classTags, kWord, kPitchTag);
        const uint32_t oneShot = sound->oneShot;
        sound->pitch = static_cast<uint16_t>(pitch);

        // A looping sound's source started.
        if (oneShot == 0)
        {
            const uint32_t bank = aggressive::soundBankOfKind(kSourceBankKind);
            const ee::Ptr<zge::BankSample> sample = zge::sample(bank, sound->sample);
            const uint32_t source = zge::startSource(sample, kStartPosition);
            sound->source = static_cast<uint16_t>(source);
            zge::sourceVolume(source & 0xFFFFu, sound->level);
            // The pitch goes out sign-extended from its 16 bits.
            zge::sourcePitch(sound->source, static_cast<int16_t>(sound->pitch));
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(initMovableSound, Verified);
}
