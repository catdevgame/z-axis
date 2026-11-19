// Skitch (game objects of kind 11, with no state block; were the sweep's
// drafts of skitchScore .. skitchTrickId): the trick the Skitch state makes
// (startSkitchTrick, state_tricks.cpp), worth nothing, finished at any
// time; its animation is the skitch's or the fakie skitch's, its text the
// string table's "Skitch". See action_tricks.h for the modules' shape.

#include "action_tricks.h"

#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "aggressive/objects.h"
#include "sym/aggressive/skater/skitch_trick.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {

        // The animations (found at the class's setup) and the text.
        inline ee::Global<ee::Ptr<zge::Motion>, sym::g_skitchAnimation> g_skitchAnimation;
        inline ee::Global<ee::Ptr<zge::Motion>, sym::g_fakieSkitchAnimation> g_fakieSkitchAnimation;
        constexpr uint32_t kSkitchAnimationName = sym::skitchAnimationName;      // "global_assets.cedwards.skitch"
        constexpr uint32_t kFakieSkitchAnimationName = sym::fakieSkitchAnimationName; // "global_assets.cedwards.skitchFakie"
        constexpr uint32_t kSkitchTextKey = sym::skitchTextKey;            // "Skitch"
        constexpr uint32_t kSkitchText = sym::skitchText;               // its text (32 bytes)
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat8;           // "Could not find the '%s' animation."
        constexpr uint32_t kEmpty = sym::empty9;                    // ""
        constexpr uint32_t kSomeSkitchTrick = sym::someSkitchTrick;          // "Some Skitch Trick"
        constexpr uint32_t kSkitchMode = 5;                         // playAnimation's mode
    }

    // The handlers (the class's message table).

    // skitchScore ("getScore"): none.
    uint32_t skitchScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(skitchScore, Verified);

    // skitchMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t skitchMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(skitchMultiplierBonus, Verified);

    // skitchName ("getName"): the skitch's text.
    uint32_t skitchName(ee::Ptr<ObjectMessageNote> note)
    {
        // (The original tests the text's address and would answer "Some
        // Skitch Trick" for none.)
        (void)kSomeSkitchTrick;
        const ee::Ptr<NameRequest> request(note->argument);
        zge::stringCopyN(request->out, kSkitchText, request->size);
        return 1;
    }
    READABLE_FUNCTION(skitchName, Verified);

    // skitchPossible ("areYouPossible"): in Skitch, not while down.
    int32_t skitchPossible(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        if (player->flags & kDown)
            return 0;
        const State state = player->state;
        return state == State::Skitch ? 1 : 0;
    }
    READABLE_FUNCTION(skitchPossible, Verified);

    // skitchFinished ("areYouFinished"): always.
    uint32_t skitchFinished()
    {
        return 1;
    }
    READABLE_FUNCTION(skitchFinished, Verified);

    // skitchSaveSize ("GetSpaceNeededForSaving"): nothing to save.
    uint32_t skitchSaveSize(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(skitchSaveSize, Verified);

    // saveSkitch ("SaveToBuffer") and loadSkitch ("LoadFromBuffer"):
    // nothing to do.
    uint32_t saveSkitch()
    {
        return 1;
    }
    READABLE_FUNCTION(saveSkitch, Verified);
    uint32_t loadSkitch()
    {
        return 1;
    }
    READABLE_FUNCTION(loadSkitch, Verified);

    // startSkitch ("TrickStarted"): the text shown (unless empty), the
    // skitch's animation (fakie's when fakie) played. Returns the play.
    uint32_t startSkitch(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        if (zge::compareStrings(kSkitchText, kEmpty) != 0)
            hud::showSpinText(player);
        const uint8_t fakie = player->fakie;
        const ee::Ptr<zge::Motion> fakieAnimation = *g_fakieSkitchAnimation;
        const ee::Ptr<zge::Motion> animation = fakie ? fakieAnimation : *g_skitchAnimation;
        if (!animation)
            return 0;
        const uint32_t played = zge::playAnimation(player->node, animation, kSkitchMode, ee::Float(0.0f), kPlayOnce);
        return played;
    }
    READABLE_FUNCTION_WAITS(startSkitch, Verified);

    // finishSkitch ("TrickFinished"): the name added to the line's; the
    // skater model's effects released.
    uint32_t finishSkitch(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        hud::addCompletedTrick(player, note->object);
        zge::stopAnimation(player->node);
        return 1;
    }
    READABLE_FUNCTION_WAITS(finishSkitch, Verified);

    // updateSkitch ("TrickUpdate"): nothing.
    uint32_t updateSkitch()
    {
        return 1;
    }
    READABLE_FUNCTION(updateSkitch, Verified);

    // setupSkitch: the class set up (setupObjectClasses' list): the class
    // and its handlers, the two animations found by name, the text.
    // Returns the fakie animation's find.
    uint32_t setupSkitch()
    {
        ee::Local<uint8_t[0x100]> out;
        ee::Local<uint8_t[0x140]> out2;
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible, *g_msgAreYouFinished,
                                     *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer, *g_msgTrickStarted,
                                     *g_msgTrickFinished, *g_msgTrickUpdate};
        const uint32_t handlers[] = {sym::skitchScore, sym::skitchMultiplierBonus, sym::skitchName, sym::skitchPossible, sym::skitchFinished, sym::skitchSaveSize,
                                     sym::saveSkitch, sym::loadSkitch, sym::startSkitch, sym::finishSkitch, sym::updateSkitch};
        registerTrickClass(kObjectSkitchTrick, messages, handlers, 0);
        if (!zge::findAnimation(kSkitchAnimationName, g_skitchAnimation.get()))
            zge::formatString(out, kNotFoundFormat, kSkitchAnimationName);
        const uint32_t found = zge::findAnimation(kFakieSkitchAnimationName, g_fakieSkitchAnimation.get());
        if (!found)
            zge::formatString(out2, kNotFoundFormat, kFakieSkitchAnimationName);
        const char *text = zge::stringTableText(zge::nameHash(ee::Ptr<const char>(kSkitchTextKey)));
        if (text && zge::compareStrings(text, kEmpty) != 0)
            zge::stringCopyN(kSkitchText, text, 0x20);
        else
            zge::stringCopyN(kSkitchText, kSkitchTextKey, 0x20);
        return found;
    }
    READABLE_FUNCTION_WAITS(setupSkitch, Verified);

    // shutdownSkitch: the class shut down.
    int32_t shutdownSkitch()
    {
        return unregisterObjectClass(uint64_t(kObjectSkitchTrick));
    }
    READABLE_FUNCTION_WAITS(shutdownSkitch, Verified);

    // newSkitch: a new skitch (startSkitchTrick's).
    ee::Ptr<GameObject> newSkitch()
    {
        return newGameObject(uint64_t(kObjectSkitchTrick), 0);
    }
    READABLE_FUNCTION_WAITS(newSkitch, Verified);

    // skitchTrickId: the skitch's trick id.
    uint32_t skitchTrickId()
    {
        return trickId(kObjectSkitchTrick);
    }
    READABLE_FUNCTION(skitchTrickId, Verified);
}
