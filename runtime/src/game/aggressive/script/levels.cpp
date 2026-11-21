// Natives the other levels' scripts run (round 6): the careers of the
// Civic Center, Industrial, Boardwalk, Cannery, Airfield and Museum and
// the Zoo's free skate, recorded with tools/routes.py and
// ZX_DEV_UNLOCK_ALL. Same conventions as library.cpp: pop the arguments
// off the script stack, call the game, push a result and return 1 when one
// was pushed.

#include "native_parts.h"
#include "script.h"

#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "sce/libgcc.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/library.h"
#include "sym/aggressive/script/levels.cpp.h"

namespace aggressive::script
{
    using namespace parts;

    namespace
    {

        // The animation natives' shape: the object named by the hash on the
        // stack, handed to the animation code.
        void withNamedObject(uint32_t function)
        {
            ee::Local<int32_t> name; // the object's name hash
            *name = popInt();
            const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(name, 1);
            ee::game(function, *found);
        }

        // A count from the skater's trick queue: (skater, trick) -> count.
        int32_t trickQueueCount(uint32_t function)
        {
            ee::Local<char[kComplaintBytes]> complaint;
            const int32_t index = popCondition();
            const int32_t trick = popInt();
            const ee::Ptr<skater::Player> player(skaterHandle(index));
            complainIfNoSkater(player, complaint, index);
            pushInt(ee::game<uint32_t>(function, player, trick));
            return 1;
        }
    }

    // pauseAnim(object): the object's animation stops where it is.
    int32_t nativePauseAnim()
    {
        withNamedObject(sym::pauseAnimation);
        return 0;
    }
    READABLE_FUNCTION(nativePauseAnim, Verified);

    // resumeAnim(object)
    int32_t nativeResumeAnim()
    {
        withNamedObject(sym::resumeAnimation);
        return 0;
    }
    READABLE_FUNCTION(nativeResumeAnim, Verified);

    // getGrindStartData (and its twins): the message's first word to a
    // variable, as getOBBData.
    int32_t nativeGetGrindStartData()
    {
        const ee::Ptr<uint32_t> data(scriptPopMessageData());
        const ee::Ptr<uint32_t> out(static_cast<uint32_t>(popInt()));
        *out = *data;
        return 0;
    }
    READABLE_FUNCTION(nativeGetGrindStartData, Verified);

    // getPotentialScoreIncrease() -> what player one's trick string would
    // bank now, as an integer.
    int32_t nativeGetPotentialScoreIncrease()
    {
        const ee::Float points = aggressive::skater::pendingTrickScore(aggressive::parts::levelPlayer(0));
        pushInt(sce::libgcc::fptoui(points));
        return 1;
    }
    READABLE_FUNCTION(nativeGetPotentialScoreIncrease, Verified);

    // getNumOfThisTrickInTrickQueue(skater, trick) -> how often that trick
    // is in the skater's current string.
    int32_t nativeGetNumOfThisTrickInTrickQueue()
    {
        return trickQueueCount(sym::timesInLine);
    }
    READABLE_FUNCTION(nativeGetNumOfThisTrickInTrickQueue, Verified);

    // getNumOfTricksLikeThisTrickInTrickQueue(skater, trick) -> how many of
    // its kind.
    int32_t nativeGetNumOfTricksLikeThisTrickInTrickQueue()
    {
        return trickQueueCount(sym::similarInLine);
    }
    READABLE_FUNCTION(nativeGetNumOfTricksLikeThisTrickInTrickQueue, Verified);

    // suspendWeatherEffects() / resumeWeatherEffects(): the level's weather
    // (rain, snow) stopped and started again around a scripted scene.
    int32_t nativeSuspendWeatherEffects()
    {
        aggressive::fx::suspendWeatherEffects();
        return 0;
    }
    READABLE_FUNCTION(nativeSuspendWeatherEffects, Verified);

    int32_t nativeResumeWeatherEffects()
    {
        aggressive::fx::resumeWeatherEffects();
        return 0;
    }
    READABLE_FUNCTION(nativeResumeWeatherEffects, Verified);

    namespace
    {
        inline ee::Global<uint32_t, sym::firstCall2> g_textTagHashPending;
        inline ee::Global<uint32_t, sym::textHash> g_textTagHash;
    }

    // getTextFromAnimEventData() -> the TEXT tag of the animation event's data
    // (eventInt looks a tag up by its name's hash, made on the first call).
    int32_t nativeGetTextFromAnimEventData()
    {
        if (*g_textTagHashPending)
        {
            *g_textTagHashPending = 0;
            *g_textTagHash = zge::nameHash(ee::Ptr<const char>(sym::g_textTEXT)); // "TEXT"
        }
        const uint32_t data = scriptPopMessageData();
        pushInt(zge::eventInt(ee::Ptr<zge::AnimationEvent>(data), *g_textTagHash)); // the script stack's word
        return 1;
    }
    READABLE_FUNCTION(nativeGetTextFromAnimEventData, Verified);

    // streamSpeech(speech): a line of speech streamed, as sound 0x4F.
    int32_t nativeStreamSpeech()
    {
        const int32_t speech = popInt();
        aggressive::soundEvent(0x4F, speech, 0, 0, 0);
        return 0;
    }
    READABLE_FUNCTION(nativeStreamSpeech, Verified);
}
