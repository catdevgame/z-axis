// The cess slide (game objects of kind 9; gamehand13, were the sweep's
// drafts of cessSlideGetScore .. cessSlideAnimation): the trick a press of the cess slide
// button makes on the ground (the action's start, state_tricks.cpp:
// newCessSlide while Controls::cessSlideTime runs), possible when the
// skater's state takes the cess slide move, finished at any time, worth
// nothing (newCessSlide clears its worth; only a loaded block brings one).
// Its animation is "cesslide" or "cesslideFakie" by the skater's stance; a
// "FrameFlag" event of 5 at its end flips the stance, and the skater goes
// back to the Ground state. Its text is the string table's "Cesslide". The
// module has the action tricks' shape (action_tricks.h).
//

#include "action_tricks.h"

#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/cess_slide.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {

        // A cess slide's state block (8 bytes; its pool's blocks are 4, as
        // the original makes them).
        struct CessSlideState
        {
            uint32_t id;     // +0x00 its entry of the table (0)
            ee::Float worth; // +0x04 0 (newCessSlide's)
        };
        static_assert(sizeof(CessSlideState) == 8);
        constexpr uint32_t kCessSlidePoolBlock = 4;

        // The table's one cess slide (0x38 bytes).
        struct CessSlideEntry
        {
            uint32_t id;                // +0x00 CessSlideState::id
            uint32_t animationName;     // +0x04 "global_assets.cedwards.cesslide"
            ee::Ptr<zge::Motion> animation;         // +0x08
            uint32_t fakieName;         // +0x0C "global_assets.cedwards.cesslideFakie"
            ee::Ptr<zge::Motion> fakieAnimation;    // +0x10
            uint32_t textKey;           // +0x14 "Cesslide"
            char text[0x20];            // +0x18
        };
        static_assert(sizeof(CessSlideEntry) == 0x38 && offsetof(CessSlideEntry, text) == 0x18);
        constexpr uint32_t kCessSlideCount = 1;
        inline ee::Global<CessSlideEntry[kCessSlideCount], sym::cessSlides> g_cessSlides;

        inline ee::Global<uint32_t, sym::g_cessSlidePool> g_cessSlidePool; // 5 blocks
        constexpr uint32_t kEmpty = sym::empty7;          // ""
        constexpr uint32_t kFrameFlagName = sym::frameFlagName2;  // "FrameFlag"
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat6; // "Could not find the '%s' animation."
        constexpr int32_t kRefused = -1;
        constexpr uint32_t kCessSlideMode = 5;            // playAnimation's mode
    }

    ee::Ptr<CessSlideEntry> cessSlideEntry(ee::Ptr<GameObject> object);
    ee::Ptr<char> cessSlideText(ee::Ptr<GameObject> object);
    ee::Ptr<zge::Motion> cessSlideAnimation(ee::Ptr<GameObject> object, uint32_t fakie);

    // The handlers (the class's message table).

    // cessSlideGetScore ("getScore").
    uint32_t cessSlideGetScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &CessSlideState::worth);
    }
    READABLE_FUNCTION(cessSlideGetScore, Verified);

    // cessSlideGetMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t cessSlideGetMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(cessSlideGetMultiplierBonus, Verified);

    // cessSlideGetName ("getName"): the cess slide's text, empty (and not
    // taken) when it has none.
    uint32_t cessSlideGetName(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<NameRequest> request(note->argument);
        const ee::Ptr<char> text = cessSlideText(note->object);
        if (text && zge::compareStrings(kEmpty, text) != 0)
        {
            zge::stringCopyN(request->out, text, request->size);
            return 1;
        }
        *ee::Ptr<char>(request->out) = 0;
        return 0;
    }
    READABLE_FUNCTION(cessSlideGetName, Verified);

    // cessSlideAreYouPossible ("areYouPossible", the player): not while down, and when
    // the skater's state takes the cess slide move.
    uint32_t cessSlideAreYouPossible(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<CessSlideState> state(gameObjectState(note->object));
        if (player->flags & kDown)
            return 0;
        int32_t entered = kRefused;
        if (state->id == 0)
            entered = requestMove(player, kMoveCessSlide);
        const uint32_t possible = entered != kRefused ? 1 : 0;
        return possible;
    }
    READABLE_FUNCTION_WAITS(cessSlideAreYouPossible, Verified);

    // cessSlideAreYouFinished ("areYouFinished"): always.
    uint32_t cessSlideAreYouFinished()
    {
        return 1;
    }
    READABLE_FUNCTION(cessSlideAreYouFinished, Verified);

    // cessSlideSpaceNeededForSaving ("GetSpaceNeededForSaving").
    uint32_t cessSlideSpaceNeededForSaving(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(CessSlideState));
    }
    READABLE_FUNCTION(cessSlideSpaceNeededForSaving, Verified);

    // cessSlideSaveToBuffer ("SaveToBuffer").
    uint32_t cessSlideSaveToBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(CessSlideState));
    }
    READABLE_FUNCTION(cessSlideSaveToBuffer, Verified);

    // cessSlideLoadFromBuffer ("LoadFromBuffer").
    uint32_t cessSlideLoadFromBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_cessSlidePool, sizeof(CessSlideState));
    }
    READABLE_FUNCTION_WAITS(cessSlideLoadFromBuffer, Verified);

    // cessSlideOnTrickStarted ("TrickStarted", the player): the worth banked, the text
    // shown (unless empty), the animation for the skater's stance played.
    // Returns the play.
    uint32_t cessSlideOnTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<CessSlideState> state(gameObjectState(note->object));
        bankStartWorth(player, state->worth);
        const ee::Ptr<char> text = cessSlideText(note->object);
        if (text && zge::compareStrings(kEmpty, text) != 0)
            hud::showSpinText(player);
        const ee::Ptr<zge::Motion> animation = cessSlideAnimation(note->object, player->fakie);
        if (animation == 0)
            return 0;
        return zge::playAnimation(player->node, animation, kCessSlideMode, animationBlendTime(), kPlayOnce);
    }
    READABLE_FUNCTION_WAITS(cessSlideOnTrickStarted, Verified);

    // cessSlideOnAnimationEnded ("AnimationEnded", an AnimationEnd): a "FrameFlag" event
    // of 5 flips the skater's stance (the model's effects released); then
    // the skater is back on the ground.
    uint32_t cessSlideOnAnimationEnded(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<AnimationEnd> ended(note->argument);
        flipStanceAtEnd(ended, kFrameFlagName);
        setSkaterState(ee::Ptr<Player>(ended->player), static_cast<uint32_t>(State::Ground));
        return 1;
    }
    READABLE_FUNCTION_WAITS(cessSlideOnAnimationEnded, Verified);

    // cessSlideOnTrickDroppedFromQueue ("TrickDroppedFromQueue").
    uint32_t cessSlideOnTrickDroppedFromQueue(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_cessSlidePool);
    }
    READABLE_FUNCTION(cessSlideOnTrickDroppedFromQueue, Verified);

    // cessSlideOnTrickFinished ("TrickFinished").
    uint32_t cessSlideOnTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(cessSlideOnTrickFinished, Verified);

    // cessSlideOnTrickUpdate ("TrickUpdate"): nothing.
    uint32_t cessSlideOnTrickUpdate()
    {
        return 1;
    }
    READABLE_FUNCTION(cessSlideOnTrickUpdate, Verified);

    // cessSlideEntry: the table's entry of the object's cess slide (0: none).
    ee::Ptr<CessSlideEntry> cessSlideEntry(ee::Ptr<GameObject> object)
    {
        const uint32_t id = ee::Ptr<CessSlideState>(gameObjectState(object))->id;
        return findTrick(*g_cessSlides, id);
    }
    READABLE_FUNCTION(cessSlideEntry, Verified);

    // cessSlideText: the object's cess slide's text (0: no entry).
    ee::Ptr<char> cessSlideText(ee::Ptr<GameObject> object)
    {
        const ee::Ptr<CessSlideEntry> entry = cessSlideEntry(object);
        return entry ? ee::Ptr<char>(entry->text) : ee::Ptr<char>();
    }
    READABLE_FUNCTION(cessSlideText, Verified);

    // initCessSlideTable (the class's setup): the cess slide's animations found
    // by name ("Could not find the '%s' animation." made when not, never
    // shown) and its text (the string table's, else the key; an empty key
    // or text left out).
    void initCessSlideTable()
    {
        ee::Local<uint8_t[0x180]> message;
        TableResolver resolve{message, kNotFoundFormat, kEmpty};
        for (CessSlideEntry &entry : *g_cessSlides)
        {
            resolve.animation(entry.animationName, entry.animation);
            resolve.animation(entry.fakieName, entry.fakieAnimation);
            resolve.text(entry.textKey, entry.text);
        }
    }
    READABLE_FUNCTION_WAITS(initCessSlideTable, Verified);

    // registerCessSlideClass (the skater's setup): the class registered with its pool
    // (five blocks) and handlers, then its table resolved. Returns the last
    // registration's result.
    uint32_t registerCessSlideClass()
    {
        *g_cessSlidePool = newPool(kCessSlidePoolBlock, 5, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible,
                                     *g_msgAreYouFinished, *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer,
                                     *g_msgTrickStarted, *g_msgTrickFinished, *g_msgAnimationEnded, *g_msgTrickUpdate,
                                     *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::cessSlideGetScore, sym::cessSlideGetMultiplierBonus, sym::cessSlideGetName, sym::cessSlideAreYouPossible, sym::cessSlideAreYouFinished, sym::cessSlideSpaceNeededForSaving, sym::cessSlideSaveToBuffer,
                                     sym::cessSlideLoadFromBuffer, sym::cessSlideOnTrickStarted, sym::cessSlideOnTrickFinished, sym::cessSlideOnAnimationEnded, sym::cessSlideOnTrickUpdate, sym::cessSlideOnTrickDroppedFromQueue};
        const uint32_t result = registerClassHandlers(kObjectCessSlide, messages, handlers);
        initCessSlideTable();
        return result;
    }
    READABLE_FUNCTION_WAITS(registerCessSlideClass, Verified);

    // unregisterCessSlideClass (the skater's shutdown).
    uint32_t unregisterCessSlideClass()
    {
        return static_cast<uint32_t>(unregisterTrickClass(g_cessSlidePool, kObjectCessSlide));
    }
    READABLE_FUNCTION_WAITS(unregisterCessSlideClass, Verified);

    // newCessSlide: a new cess slide (the action's start), worth nothing.
    ee::Ptr<GameObject> newCessSlide()
    {
        ee::Ptr<CessSlideState> state;
        const ee::Ptr<GameObject> object = newTrickObject(g_cessSlidePool, kObjectCessSlide, state);
        state->worth = 0.0f;
        state->id = 0;
        return object;
    }
    READABLE_FUNCTION_WAITS(newCessSlide, Verified);

    // cessSlideTrickId: a cess slide's trick id (makeSkaterTrickObjects' list
    // has none).
    uint32_t cessSlideTrickId(uint32_t variant)
    {
        return trickId(kObjectCessSlide) | variant;
    }
    READABLE_FUNCTION(cessSlideTrickId, Verified);

    // cessSlideAnimation (object, fakie): the object's cess slide's animation for
    // the stance (0: no entry).
    ee::Ptr<zge::Motion> cessSlideAnimation(ee::Ptr<GameObject> object, uint32_t fakie)
    {
        const ee::Ptr<CessSlideEntry> entry = cessSlideEntry(object);
        if (!entry)
            return 0;
        return fakie ? entry->fakieAnimation : entry->animation;
    }
    READABLE_FUNCTION(cessSlideAnimation, Verified);
}
