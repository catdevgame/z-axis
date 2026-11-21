// Scene objects that play an animation by name: what they play is kept in
// their component's tags, so a level's save can put it back.

#include "aggressive/level/animated_objects.h"
#include "aggressive/script/script.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/aggressive/level/animated_objects.cpp.h"

namespace aggressive::level
{
    namespace
    {
        constexpr uint32_t kCountAnimObject = sym::countAnimObjects, kSaveAnimObject = sym::saveAnimObjectState; // the visits
    }

    // playObjectAnimation
    int32_t playObjectAnimation(ee::Ptr<zge::SceneObject> sceneObject, ee::Address name, uint32_t mode, ee::Float blend)
    {
        const ee::Ptr<zge::TagRecord> record = zge::objectComponent(sceneObject);
        const int32_t playing = zge::playAnimationNamed(sceneObject, name, mode, blend, ee::Float(-1.0f));
        if (playing != 0)
        {
            const int32_t hash = zge::nameHash(ee::Ptr<const char>(name));
            zge::setTagWord(record, kTagWords, kTagAnimationName, hash);
            zge::setTagByte(record, kTagBytes, kTagAnimationMode, mode & 0xFFu);
            zge::setTagByte(record, kTagBytes, kTagAnimationPlaying, 1);
        }
        return playing;
    }
    READABLE_FUNCTION_WAITS(playObjectAnimation, Verified);

    // animObjectStateBytes: how many bytes a save of the animated objects takes.
    int32_t animObjectStateBytes()
    {
        ee::Local<int32_t> count;
        *count = 0;
        zge::forEachObject(kCountAnimObject, count);
        return *count * static_cast<int32_t>(sizeof(AnimObjectState));
    }
    READABLE_FUNCTION_WAITS(animObjectStateBytes, Verified);

    // saveAnimObjectStates: every animated object's state, written from `states` on.
    void saveAnimObjectStates(ee::Address states)
    {
        ee::Local<uint32_t> cursor;
        zge::memoryFill(cursor, 0, sizeof(uint32_t));
        *cursor = states;
        zge::forEachObject(kSaveAnimObject, cursor);
    }
    READABLE_FUNCTION_WAITS(saveAnimObjectStates, Verified);

    // restoreAnimObjectStates: each record's object found by name and put back.
    int32_t restoreAnimObjectStates(ee::Ptr<AnimObjectState> states, uint32_t bytes)
    {
        ee::Local<uint32_t> name;
        for (; bytes != 0; bytes -= sizeof(AnimObjectState), ++states)
        {
            *name = states->name;
            aggressive::script::restoreAnimObjectState(*zge::findObjects(name, 1), states);
        }
        return zge::updateAnimators(ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(restoreAnimObjectStates, Verified);

    namespace
    {
        // Where a camera animation object keeps its offset: three floats of
        // its component's tags. Tag reads and writes take a second argument
        // they never read; the game passes 8.
        constexpr uint32_t kTagOffsetX = 4, kTagOffsetY = 8, kTagOffsetZ = 12;
        constexpr uint32_t kTagUnread = 8;

    }

    // updateDynamicsObjectMotion: a camera animation object's update (the
    // CameraAnim type's: subscribeDynamicsObjectMotion): its own bone's
    // offset, turned with the object (animatorBoneOffset), into its tags.
    // The offset there is read first and not used; when the object has no
    // such bone the offset written is whatever its local held. 0. No level
    // of the nine releases has a CameraAnim object.
    uint32_t updateDynamicsObjectMotion(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> offset;
        ee::Local<zge::Vec3> previous;
        const ee::Ptr<zge::SceneObject> object(message->target); // the message's target, its word
        const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(object);
        previous->x = zge::tagFloat(tags, kTagUnread, kTagOffsetX);
        previous->y = zge::tagFloat(tags, kTagUnread, kTagOffsetY);
        previous->z = zge::tagFloat(tags, kTagUnread, kTagOffsetZ);
        zge::animatorBoneOffset(object, offset);
        zge::setTagFloat(tags, kTagUnread, kTagOffsetX, offset->x);
        zge::setTagFloat(tags, kTagUnread, kTagOffsetY, offset->y);
        zge::setTagFloat(tags, kTagUnread, kTagOffsetZ, offset->z);
        return 0;
    }
    READABLE_FUNCTION(updateDynamicsObjectMotion, Verified);

    // subscribeDynamicsObjectMotion: the object's every-frame update message
    // handled by updateDynamicsObjectMotion. Returns 1.
    uint32_t subscribeDynamicsObjectMotion(uint32_t object)
    {
        zge::subscribe(object, zge::kUpdateMessage, sym::updateDynamicsObjectMotion);
        return 1;
    }
    READABLE_FUNCTION_WAITS(subscribeDynamicsObjectMotion, Verified);
}
