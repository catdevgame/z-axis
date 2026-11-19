// The game's side of the engine's animator (zge/scene/animator.h): the
// engine's animation event hook (a SOUND_EVENT plays its sound, a skater's
// and the follow camera's events go to them, the rest as messages) and a
// motion's name looked up in the animation table. Names are inferred from
// what the code does.

#include "aggressive/game_state.h"
#include "aggressive/skater/skater.h"
#include "aggressive/sound/events.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/skater/animations.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr int32_t kPlayers = 2;
        constexpr uint32_t kSoundEventName = sym::soundEventName; // "SOUND_EVENT"
        inline ee::Global<uint32_t, sym::g_soundEventToHash> g_soundEventToHash; // 1 until hashed
        inline ee::Global<uint32_t, sym::g_soundEventHash> g_soundEventHash;
        // The engine animator's table of motions (zge/scene/animator.cpp), walked by address.
        constexpr uint32_t kAnimationTable = sym::animationTable;

        namespace routed
        {
            GAME_TABLE_ROUTE(searchTreeForEach, ee::game, sym::searchTreeForEach);
        }
    }

    namespace
    {
        // (event) -> its kind's name hash
        int32_t skaterAnimationEventKeepingResult(ee::Ptr<skater::Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time) { return ee::game<int32_t>(sym::skaterAnimationEvent, player, event, time); }
        // () -> the follow cameras (the first's object first)
        int32_t cameraAnimationEvent(ee::Ptr<zge::AnimationEvent> event, ee::Float time) { return ee::game<int32_t>(sym::cameraAnimationEvent, event, time); }
    }

    // handleAnimationEvent (object, event, time; the engine's animation
    // event hook): a SOUND_EVENT plays its sound (animationPartSound); a
    // skater's events go to the skater, the follow camera's to the camera,
    // any other object gets it as a message (its type the event's kind).
    int32_t handleAnimationEvent(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::AnimationEvent> event, uint32_t, ee::Float time)
    {
        ee::Local<zge::EngineMessage> message;
        const uint32_t kind = zge::eventKind(event);
        if (*g_soundEventToHash != 0)
        {
            *g_soundEventToHash = 0;
            *g_soundEventHash = zge::nameHash(ee::Ptr<const char>(kSoundEventName));
        }
        if (kind == *g_soundEventHash)
            return animationPartSound(object, event);

        // A skater's?
        for (int32_t index = 0; index < kPlayers; ++index)
        {
            const ee::Ptr<skater::Player> player = &currentLevel()->players[index];
            if (player->node == object)
                return skaterAnimationEventKeepingResult(player, event, time);
        }

        // The follow camera's?
        if (object == ee::Ptr<zge::SceneObject>(followCameras()->node)) // the camera's node, a word in FollowCamera
            return cameraAnimationEvent(event, time);
        message->target = object;
        message->sender = 0;
        message->type = zge::eventKind(event);
        message->data = event;
        message->flags = zge::kMessageNow;
        return zge::sendEngineMessage(message);
    }
    READABLE_FUNCTION_WAITS(handleAnimationEvent, Verified);

    // ---- a motion's name looked up (were the sweep's findKeyedEntryByValue
    // and matchKeyedEntryValue)
    namespace
    {
        // What findMotionName looks for, and what it finds.
        struct MotionSearch
        {
            uint32_t found;  // +0x00
            ee::Ptr<zge::Motion> motion; // +0x04 the motion looked for
            uint32_t name;   // +0x08 its entry's name hash
        };
        static_assert(sizeof(MotionSearch) == 12);
    }

    // matchMotionEntry: (the animation table's walk) the entry of the motion
    // looked for gives its name; 0 stops the walk there, else 1.
    uint32_t matchMotionEntry(ee::Ptr<zge::AnimationEntry> entry, ee::Ptr<MotionSearch> search)
    {
        if (entry->motion != search->motion)
            return 1;
        search->found = 1;
        search->name = entry->name;
        return 0;
    }
    READABLE_FUNCTION(matchMotionEntry, Draft);

    // findMotionName: the name hash the animation table has `motion` under,
    // into *name. 1 when it is there, else 0 (*name untouched).
    int32_t findMotionName(ee::Ptr<zge::Motion> motion, ee::Ptr<uint32_t> name)
    {
        ee::Local<MotionSearch> search;
        zge::memoryFill(search, 0, sizeof(MotionSearch));
        search->motion = motion;
        search->found = 0;
        routed::searchTreeForEach(kAnimationTable, sym::matchMotionEntry, search);
        if (search->found)
            *name = search->name;
        return static_cast<int32_t>(search->found);
    }
    READABLE_FUNCTION_WAITS(findMotionName, Verified);
}
