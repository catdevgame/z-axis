// The stalls' class (game objects of kind 10; gamehand12, were the sweep's
// drafts of stallTrickGetScore .. unregisterStallTrickClass): the hand plants the trick table makes
// (trick_objects.cpp: HandPlant, Abstract, Rocket, Method, Stale), each
// worth its Scoring: Stalls "Init. Worth" when it starts and its
// "Worth/sec" while held, its animation and the stall balance meter's
// forward and backward ones from the table stallEntries, its text from the
// string table. The module has the action tricks' shape (action_tricks.h).
//

#include "action_tricks.h"
#include "trick_objects.h"

#include "aggressive/hud/hud.h"
#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/stall_trick.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {
        constexpr uint32_t kResolveStallTricks = sym::resolveStallTricks;

        constexpr uint32_t kStallCount = 5;
        inline ee::Global<StallEntry[kStallCount], sym::stallEntries> g_stallEntries;

        inline ee::Global<uint32_t, sym::g_stallTrickPool> g_stallTrickPool; // 5 blocks
        constexpr uint32_t kEmpty = sym::empty3;          // ""
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat3; // "Could not find the '%s' animation."
        constexpr uint32_t kStallMode = 5;                // playAnimation's mode
        // The hold let go: the animation time steps back a frame and a half.
        inline ee::Global<ee::Float, sym::g_halfFrameSeconds4> g_halfFrameSeconds; // 1 / 120
        inline ee::Global<ee::Float, sym::g_frameSeconds4> g_frameSeconds;     // 1 / 60
    }

    // The handlers (the class's message table).

    // stallTrickGetScore ("getScore"): what the stall has earned.
    uint32_t stallTrickGetScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &StallTrickState::worth);
    }
    READABLE_FUNCTION(stallTrickGetScore, Verified);

    // stallTrickGetMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t stallTrickGetMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(stallTrickGetMultiplierBonus, Verified);

    ee::Ptr<char> stallTrickText(uint32_t type);

    // stallTrickGetName ("getName"): the stall's text, empty (and not taken)
    // when it has none.
    uint32_t stallTrickGetName(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<NameRequest> request(note->argument);
        const ee::Ptr<StallTrickState> state(gameObjectState(note->object));
        const ee::Ptr<char> text = stallTrickText(state->type);
        if (text && zge::compareStrings(kEmpty, text) != 0)
        {
            zge::stringCopyN(request->out, text, request->size);
            return 1;
        }
        *ee::Ptr<char>(request->out) = 0;
        return 0;
    }
    READABLE_FUNCTION(stallTrickGetName, Verified);

    // stallTrickAreYouPossible ("areYouPossible", the player): not while down.
    uint32_t stallTrickAreYouPossible(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        gameObjectState(note->object);
        return (player->flags & kDown) == 0 ? 1 : 0;
    }
    READABLE_FUNCTION(stallTrickAreYouPossible, Verified);

    // stallTrickAreYouFinished ("areYouFinished"): once held.
    uint32_t stallTrickAreYouFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return ee::Ptr<StallTrickState>(gameObjectState(note->object))->held;
    }
    READABLE_FUNCTION(stallTrickAreYouFinished, Verified);

    // stallTrickSpaceNeededForSaving ("GetSpaceNeededForSaving").
    uint32_t stallTrickSpaceNeededForSaving(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(StallTrickState));
    }
    READABLE_FUNCTION(stallTrickSpaceNeededForSaving, Verified);

    // stallTrickSaveToBuffer ("SaveToBuffer").
    uint32_t stallTrickSaveToBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(StallTrickState));
    }
    READABLE_FUNCTION(stallTrickSaveToBuffer, Verified);

    // stallTrickLoadFromBuffer ("LoadFromBuffer").
    uint32_t stallTrickLoadFromBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_stallTrickPool, sizeof(StallTrickState));
    }
    READABLE_FUNCTION_WAITS(stallTrickLoadFromBuffer, Verified);

    ee::Ptr<StallEntry> stallTrickEntry(uint32_t type);
    ee::Ptr<zge::Motion> stallBalanceAnimation(ee::Ptr<StallTrickState> state, ee::Float lean);

    // stallTrickOnTrickStarted ("TrickStarted", the player): the initial worth banked,
    // the text shown (unless empty), the stall's entry kept and its
    // animation played. Returns the play.
    uint32_t stallTrickOnTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<StallTrickState> state(gameObjectState(note->object));
        const ee::Ptr<Player> player(note->argument);
        bankStartWorth(player, state->worth);
        const ee::Ptr<char> text = stallTrickText(state->type);
        if (text && zge::compareStrings(kEmpty, text) != 0)
            hud::showSpinText(player);
        const ee::Ptr<StallEntry> entry = stallTrickEntry(state->type);
        state->entry = entry;
        if (entry && entry->animation)
            return zge::playAnimation(player->node, entry->animation, kStallMode, animationBlendTime(), kPlayOnce);
        return 0;
    }
    READABLE_FUNCTION_WAITS(stallTrickOnTrickStarted, Verified);

    // stallTrickOnTrickFinished ("TrickFinished").
    uint32_t stallTrickOnTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(stallTrickOnTrickFinished, Verified);

    // stallTrickOnTrickUpdate ("TrickUpdate"): the frame's worth (per second times the
    // frame's seconds) added to the stall's and to the line's pending
    // score; the stall meter's lean animation queued, as much as the lean.
    uint32_t stallTrickOnTrickUpdate(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Local<ee::Float> animationTime;
        const ee::Ptr<TrickUpdate> update(note->argument);
        const ee::Ptr<GameObject> object = note->object;
        const ee::Ptr<Player> player(update->player);
        *animationTime = 0.0f;
        const ee::Float zero = 0.0f;
        const ee::Ptr<StallTrickState> state(gameObjectState(object));
        const ee::Float earned = state->worthPerSecond * update->seconds;
        if (ee::Float(0.0f) < earned)
        {
            state->worth = state->worth + earned;
            const ee::Float pending = storedPendingScore(player);
            setPendingScore(player, pending + earned);
        }
        zge::animationTime(player->node, animationTime);
        // (the animation's time since the hold began: kept, unused)
        *animationTime = *animationTime - state->holdTime;
        const ee::Float lean = player->balance[1];
        const ee::Ptr<zge::Motion> animation = stallBalanceAnimation(state, lean);
        if (animation == 0)
            return 1;
        const uint32_t queued = zge::addPoseLayer(player->node, animation, 0, zero, ee::abs(lean));
        return queued;
    }
    READABLE_FUNCTION_WAITS(stallTrickOnTrickUpdate, Verified);

    // stallTrickOnTrickStartHolding ("TrickStartHolding", {player, time}): held, the time kept.
    uint32_t stallTrickOnTrickStartHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<StallTrickState> state(gameObjectState(note->object));
        state->held = 1;
        state->holdTime = holding->seconds;
        return 1;
    }
    READABLE_FUNCTION(stallTrickOnTrickStartHolding, Verified);

    // stallTrickOnTrickStoppedHolding ("TrickStoppedHolding", {player, time}): the kept time
    // steps back a frame and a half (not below 0) and the skater's
    // animation is put there.
    uint32_t stallTrickOnTrickStoppedHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<StallTrickState> state(gameObjectState(note->object));
        const ee::Float back = state->holdTime - *g_halfFrameSeconds - *g_frameSeconds;
        state->holdTime = back;
        if (back < ee::Float(0.0f))
            state->holdTime = 0.0f;
        return zge::setAnimationTime(ee::Ptr<Player>(holding->player)->node, state->holdTime);
    }
    READABLE_FUNCTION_WAITS(stallTrickOnTrickStoppedHolding, Verified);

    // stallTrickOnAnimationEnded ("AnimationEnded", {player, ...}): the skater's state's
    // object made current.
    uint32_t stallTrickOnAnimationEnded(ee::Ptr<ObjectMessageNote> note)
    {
        gameObjectState(note->object);
        selectStateObject(ee::Ptr<AnimationEnd>(note->argument)->player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(stallTrickOnAnimationEnded, Verified);

    // stallTrickOnTrickDroppedFromQueue ("TrickDroppedFromQueue").
    uint32_t stallTrickOnTrickDroppedFromQueue(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_stallTrickPool);
    }
    READABLE_FUNCTION(stallTrickOnTrickDroppedFromQueue, Verified);

    // stallTrickEntry: the table's entry of a stall's type (0: none).
    ee::Ptr<StallEntry> stallTrickEntry(uint32_t type)
    {
        return findTrick(*g_stallEntries, &StallEntry::type, type);
    }
    READABLE_FUNCTION(stallTrickEntry, Verified);

    // stallBalanceAnimation (state, lean): the stall meter's animation for the lean
    // (below 0 forward, else backward); 0 without an entry.
    ee::Ptr<zge::Motion> stallBalanceAnimation(ee::Ptr<StallTrickState> state, ee::Float lean)
    {
        const ee::Ptr<StallEntry> entry = state->entry;
        if (!entry)
            return 0;
        if (lean < ee::Float(0.0f))
            return entry->forwardAnimation;
        return entry->backwardAnimation;
    }
    READABLE_FUNCTION(stallBalanceAnimation, Verified);

    // stallTrickText: a stall type's text (0: no entry).
    ee::Ptr<char> stallTrickText(uint32_t type)
    {
        const ee::Ptr<StallEntry> entry = stallTrickEntry(type);
        return entry ? ee::Ptr<char>(entry->text) : ee::Ptr<char>();
    }
    READABLE_FUNCTION(stallTrickText, Verified);

    // resolveStallTricks (the class's setup): each stall's animations found by
    // name ("Could not find the '%s' animation." made when not, never
    // shown) and its text (the string table's, else the key; an empty key
    // or text left out).
    void resolveStallTricks()
    {
        ee::Local<uint8_t[0x170]> message;
        TableResolver resolve{message, kNotFoundFormat, kEmpty};
        for (StallEntry &entry : *g_stallEntries)
        {
            resolve.animation(entry.animationName, entry.animation);
            resolve.animation(entry.forwardName, entry.forwardAnimation);
            resolve.animation(entry.backwardName, entry.backwardAnimation);
            resolve.text(entry.textKey, entry.text);
        }
    }
    READABLE_FUNCTION_WAITS(resolveStallTricks, Verified);

    // registerStallTrickClass (the skater's setup): the class registered with its pool
    // (five state blocks) and handlers, then its table resolved. Returns
    // the last registration's result.
    uint32_t registerStallTrickClass()
    {
        *g_stallTrickPool = newPool(static_cast<uint32_t>(sizeof(StallTrickState)), 5, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible,
                                     *g_msgAreYouFinished, *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer,
                                     *g_msgTrickStarted, *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgTrickStartHolding,
                                     *g_msgTrickStoppedHolding, *g_msgAnimationEnded, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::stallTrickGetScore, sym::stallTrickGetMultiplierBonus, sym::stallTrickGetName, sym::stallTrickAreYouPossible, sym::stallTrickAreYouFinished, sym::stallTrickSpaceNeededForSaving, sym::stallTrickSaveToBuffer, sym::stallTrickLoadFromBuffer,
                                     sym::stallTrickOnTrickStarted, sym::stallTrickOnTrickFinished, sym::stallTrickOnTrickUpdate, sym::stallTrickOnTrickStartHolding, sym::stallTrickOnTrickStoppedHolding, sym::stallTrickOnAnimationEnded, sym::stallTrickOnTrickDroppedFromQueue};
        registerObjectClass(kObjectStallTrick);
        uint32_t result = 0;
        for (uint32_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
            result = setClassHandler(uint64_t(kObjectStallTrick), messages[i], handlers[i]);
        resolveStallTricks();
        return result;
    }
    READABLE_FUNCTION_WAITS(registerStallTrickClass, Verified);

    // unregisterStallTrickClass (the skater's shutdown).
    uint32_t unregisterStallTrickClass()
    {
        return static_cast<uint32_t>(unregisterTrickClass(g_stallTrickPool, kObjectStallTrick));
    }
    READABLE_FUNCTION_WAITS(unregisterStallTrickClass, Verified);

    // stallTrickId: a stall's trick id (makeSkaterTrickObjects': the five
    // types; it sits after the hand plants' makers, trick_objects.cpp).
    uint32_t stallTrickId(uint32_t type)
    {
        return trickId(kObjectStallTrick) | type;
    }
    READABLE_FUNCTION(stallTrickId, Verified);
}
