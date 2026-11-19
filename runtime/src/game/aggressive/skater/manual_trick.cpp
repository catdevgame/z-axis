// The manuals (game objects of kind 4; gamehand12, were the sweep's
// drafts of manualTrickGetScore .. newSingleToeManual): the trick the Manual state makes
// (startGroundTricks matches one, state_tricks.cpp): Double Heel, Single
// Heel, Double Toe and Single Toe, each worth its Scoring: Manuals "Init.
// Worth" when it starts and its "Worth/sec" while held; its animation (or
// the fakie one) and the balance meter's side (heel -1, toe 1) from the
// table, its text from the string table. The module has the action
// tricks' shape (action_tricks.h).
//

#include "action_tricks.h"

#include "aggressive/hud/hud.h"
#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/manual_trick.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {
        constexpr uint32_t kManualTrickEntry = sym::manualTrickEntry;
        constexpr uint32_t kResolveManualTricks = sym::resolveManualTricks;
        constexpr uint32_t kNewManualTrickObject = sym::newManualTrickObject;

        // A manual (the table manualEntries's four, 0x3C bytes): its variant, its
        // animations by name (found at the class's setup), its text and the
        // balance meter's side.
        struct ManualEntry
        {
            uint32_t variant;            // +0x00 ManualVariant
            uint32_t animationName;      // +0x04 "global_assets.cedwards.doubleHeelManual"
            ee::Ptr<zge::Motion> animation;          // +0x08
            uint32_t fakieAnimationName; // +0x0C "global_assets.cedwards.doubleHeelManualFakie"
            ee::Ptr<zge::Motion> fakieAnimation;     // +0x10
            uint32_t textKey;            // +0x14 "DoubleHeelManual"
            char text[0x20];             // +0x18
            int8_t balanceSide;          // +0x38 -1 the heel, 1 the toe (hud setSafeDirection)
            uint8_t _39[3];
        };
        static_assert(sizeof(ManualEntry) == 0x3C && offsetof(ManualEntry, text) == 0x18 && offsetof(ManualEntry, balanceSide) == 0x38);
        constexpr uint32_t kManualCount = 4;
        inline ee::Global<ManualEntry[kManualCount], sym::manualEntries> g_manualEntries;

        enum ManualVariant : uint32_t
        {
            kDoubleHeelManual = 0,
            kSingleHeelManual = 1,
            kDoubleToeManual = 2,
            kSingleToeManual = 3,
            kManualNone = 4,
        };

        // The state block (0x10 bytes from the pool).
        struct ManualTrickState
        {
            uint32_t variant;       // +0x00 ManualVariant
            ee::Float worth;        // +0x04 points so far
            ee::Float worthPerSecond; // +0x08
            ee::Float holdTime;     // +0x0C the animation's time, kept while held
        };
        static_assert(sizeof(ManualTrickState) == 0x10);
        inline ee::Global<uint32_t, sym::g_manualTrickPool> g_manualTrickPool; // 5 blocks

        // Scoring: Manuals (tuning category 12): each variant's Init. Worth
        // (entries 0 .. 3) and Worth/sec (4 .. 7).
        ee::Float manualInitialWorth(uint32_t variant)
        {
            return (*g_tuningCategories)[kTuningScoringManuals].entries[variant].value;
        }
        ee::Float manualWorthPerSecond(uint32_t variant)
        {
            return (*g_tuningCategories)[kTuningScoringManuals].entries[4 + variant].value;
        }

        constexpr uint32_t kEmpty = sym::empty;             // ""
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat;    // "Could not find the '%s' animation."
        constexpr uint32_t kManualMode = 5;                  // playAnimation's mode
        // The hold let go: the animation time steps back a frame and a half.
        inline ee::Global<ee::Float, sym::g_halfFrameSeconds2> g_halfFrameSeconds; // 1 / 120
        inline ee::Global<ee::Float, sym::g_frameSeconds2> g_frameSeconds;     // 1 / 60
    }

    // The handlers (the class's message table).

    // manualTrickGetScore ("getScore"): the points so far.
    uint32_t manualTrickGetScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &ManualTrickState::worth);
    }
    READABLE_FUNCTION(manualTrickGetScore, Verified);

    // manualTrickGetMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t manualTrickGetMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(manualTrickGetMultiplierBonus, Verified);

    ee::Ptr<ManualEntry> manualTrickEntry(ee::Ptr<GameObject> object);

    // manualTrickGetName ("getName"): the manual's text, empty (and not taken)
    // when it has none.
    uint32_t manualTrickGetName(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<NameRequest> request(note->argument);
        const ee::Ptr<ManualEntry> entry = manualTrickEntry(note->object);
        if (entry && zge::compareStrings(kEmpty, entry->text) != 0)
        {
            zge::stringCopyN(request->out, entry->text, request->size);
            return 1;
        }
        *ee::Ptr<char>(request->out) = 0;
        return 0;
    }
    READABLE_FUNCTION(manualTrickGetName, Verified);

    // manualTrickAreYouPossible ("areYouPossible", the player): not while down, and when
    // the skater's state takes the manual move.
    uint32_t manualTrickAreYouPossible(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        gameObjectState(note->object);
        uint32_t possible = 0;
        if (!(player->flags & kDown))
            possible = requestMove(player, kMoveManual) != -1 ? 1 : 0;
        return possible;
    }
    READABLE_FUNCTION_WAITS(manualTrickAreYouPossible, Verified);

    // manualTrickAreYouFinished ("areYouFinished"): always.
    uint32_t manualTrickAreYouFinished()
    {
        return 1;
    }
    READABLE_FUNCTION(manualTrickAreYouFinished, Verified);

    // manualTrickOnTrickStarted ("TrickStarted", the player): the initial worth banked,
    // the text shown (unless empty), the balance meter's side set, the
    // animation (fakie's when fakie) played. Returns the play.
    uint32_t manualTrickOnTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<ManualTrickState> state(gameObjectState(note->object));
        const ee::Ptr<ManualEntry> entry = manualTrickEntry(note->object);
        state->holdTime = 0.0f;
        bankStartWorth(player, state->worth);
        if (entry && zge::compareStrings(kEmpty, entry->text) != 0)
            hud::showSpinText(player);
        hud::setSafeDirection(player, entry->balanceSide);
        const uint8_t fakie = player->fakie;
        const ee::Ptr<zge::Motion> animation = !entry ? ee::Ptr<zge::Motion>{} : fakie ? entry->fakieAnimation : entry->animation;
        if (animation == 0)
            return 0;
        return zge::playAnimation(player->node, animation, kManualMode, animationBlendTime(), kPlayOnce);
    }
    READABLE_FUNCTION_WAITS(manualTrickOnTrickStarted, Verified);

    // manualTrickOnTrickFinished ("TrickFinished").
    uint32_t manualTrickOnTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(manualTrickOnTrickFinished, Verified);

    // manualTrickSpaceNeededForSaving ("GetSpaceNeededForSaving").
    uint32_t manualTrickSpaceNeededForSaving(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(ManualTrickState));
    }
    READABLE_FUNCTION(manualTrickSpaceNeededForSaving, Verified);

    // manualTrickSaveToBuffer ("SaveToBuffer").
    uint32_t manualTrickSaveToBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(ManualTrickState));
    }
    READABLE_FUNCTION(manualTrickSaveToBuffer, Verified);

    // manualTrickLoadFromBuffer ("LoadFromBuffer").
    uint32_t manualTrickLoadFromBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_manualTrickPool, sizeof(ManualTrickState));
    }
    READABLE_FUNCTION_WAITS(manualTrickLoadFromBuffer, Verified);

    // manualTrickOnTrickUpdate ("TrickUpdate"): the frame's worth (per second times the
    // frame's seconds) added to the manual's and to the line's pending score.
    uint32_t manualTrickOnTrickUpdate(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> update(note->argument);
        const ee::Ptr<Player> player(update->player);
        const ee::Ptr<ManualTrickState> state(gameObjectState(note->object));
        const ee::Float earned = state->worthPerSecond * update->seconds;
        if (ee::Float(0.0f) < earned)
        {
            state->worth = state->worth + earned;
            const ee::Float pending = storedPendingScore(player);
            setPendingScore(player, pending + earned);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(manualTrickOnTrickUpdate, Verified);

    // manualTrickOnTrickStartHolding ("TrickStartHolding", {player, time}): the time kept.
    uint32_t manualTrickOnTrickStartHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<ManualTrickState> state(gameObjectState(note->object));
        state->holdTime = holding->seconds;
        return 1;
    }
    READABLE_FUNCTION(manualTrickOnTrickStartHolding, Verified);

    // manualTrickOnTrickStoppedHolding ("TrickStoppedHolding", {player, time}): the kept time
    // steps back a frame and a half (not below 0) and the skater's
    // animation is put there.
    uint32_t manualTrickOnTrickStoppedHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<ManualTrickState> state(gameObjectState(note->object));
        const ee::Float back = state->holdTime - *g_halfFrameSeconds - *g_frameSeconds;
        state->holdTime = back;
        if (back < ee::Float(0.0f))
            state->holdTime = 0.0f;
        return zge::setAnimationTime(ee::Ptr<Player>(holding->player)->node, state->holdTime);
    }
    READABLE_FUNCTION_WAITS(manualTrickOnTrickStoppedHolding, Verified);

    // manualTrickOnTrickDroppedFromQueue ("TrickDroppedFromQueue").
    uint32_t manualTrickOnTrickDroppedFromQueue(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_manualTrickPool);
    }
    READABLE_FUNCTION(manualTrickOnTrickDroppedFromQueue, Verified);

    // manualTrickEntry: the table's entry of the object's variant (0: none).
    ee::Ptr<ManualEntry> manualTrickEntry(ee::Ptr<GameObject> object)
    {
        const uint32_t variant = ee::Ptr<ManualTrickState>(gameObjectState(object))->variant;
        return findTrick(*g_manualEntries, &ManualEntry::variant, variant);
    }
    READABLE_FUNCTION(manualTrickEntry, Verified);

    // resolveManualTricks (the class's setup): each manual's animations found by
    // name ("Could not find the '%s' animation." made when not, never
    // shown) and its text (the string table's, else the key; an empty key
    // or text left out).
    void resolveManualTricks()
    {
        ee::Local<uint8_t[0x180]> message;
        TableResolver resolve{message, kNotFoundFormat, kEmpty};
        for (ManualEntry &entry : *g_manualEntries)
        {
            resolve.animation(entry.animationName, entry.animation);
            resolve.animation(entry.fakieAnimationName, entry.fakieAnimation);
            resolve.text(entry.textKey, entry.text);
        }
    }
    READABLE_FUNCTION_WAITS(resolveManualTricks, Verified);

    // registerManualTrickClass (the skater's setup): the class registered with its
    // pool (five state blocks) and handlers, then the table resolved.
    // Returns the last registration's result.
    uint32_t registerManualTrickClass()
    {
        *g_manualTrickPool = newPool(static_cast<uint32_t>(sizeof(ManualTrickState)), 5, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible,
                                     *g_msgAreYouFinished, *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer,
                                     *g_msgTrickStarted, *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgTrickStartHolding,
                                     *g_msgTrickStoppedHolding, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::manualTrickGetScore, sym::manualTrickGetMultiplierBonus, sym::manualTrickGetName, sym::manualTrickAreYouPossible, sym::manualTrickAreYouFinished, sym::manualTrickSpaceNeededForSaving, sym::manualTrickSaveToBuffer,
                                     sym::manualTrickLoadFromBuffer, sym::manualTrickOnTrickStarted, sym::manualTrickOnTrickFinished, sym::manualTrickOnTrickUpdate, sym::manualTrickOnTrickStartHolding, sym::manualTrickOnTrickStoppedHolding, sym::manualTrickOnTrickDroppedFromQueue};
        registerObjectClass(kObjectManualTrick);
        uint32_t result = 0;
        for (uint32_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
            result = setClassHandler(uint64_t(kObjectManualTrick), messages[i], handlers[i]);
        resolveManualTricks();
        return result;
    }
    READABLE_FUNCTION_WAITS(registerManualTrickClass, Verified);

    // unregisterManualTrickClass (the skater's shutdown).
    uint32_t unregisterManualTrickClass()
    {
        return static_cast<uint32_t>(unregisterTrickClass(g_manualTrickPool, kObjectManualTrick));
    }
    READABLE_FUNCTION_WAITS(unregisterManualTrickClass, Verified);

    // manualMatchesBalance (startGroundTricks' match): whether the manual is on the
    // side the skater leans to (`lean` above 0: a toe manual, else a heel
    // one).
    uint32_t manualMatchesBalance(ee::Ptr<GameObject> object, ee::Float lean)
    {
        uint32_t variant = ee::Ptr<ManualTrickState>(gameObjectState(object))->variant;
        if (ee::Float(0.0f) < lean)
            variant = variant - kDoubleToeManual;
        return variant < 2 ? 1 : 0;
    }
    READABLE_FUNCTION(manualMatchesBalance, Verified);

    // newManualTrickObject: a manual object, its state block cleared, no variant
    // yet and worth nothing (the variants' makers below set them).
    ee::Ptr<GameObject> newManualTrickObject()
    {
        ee::Ptr<ManualTrickState> state;
        const ee::Ptr<GameObject> object = newTrickObject(g_manualTrickPool, kObjectManualTrick, state);
        state->holdTime = 0.0f;
        state->variant = kManualNone;
        state->worth = 0.0f;
        state->worthPerSecond = 0.0f;
        return object;
    }
    READABLE_FUNCTION_WAITS(newManualTrickObject, Verified);

    namespace
    {
        // A variant's object: its initial worth and its worth a second.
        ee::Ptr<GameObject> newManual(uint32_t variant)
        {
            const ee::Ptr<GameObject> object = newManualTrickObject();
            const ee::Ptr<ManualTrickState> state(gameObjectState(object));
            state->variant = variant;
            state->worth = manualInitialWorth(variant);
            const ee::Float perSecond = manualWorthPerSecond(variant);
            state->holdTime = 0.0f;
            state->worthPerSecond = perSecond;
            return object;
        }
    }

    // newDoubleHeelManual .. newSingleToeManual: the four manuals' objects.
    ee::Ptr<GameObject> newDoubleHeelManual()
    {
        return newManual(kDoubleHeelManual);
    }
    READABLE_FUNCTION_WAITS(newDoubleHeelManual, Draft);
    ee::Ptr<GameObject> newSingleHeelManual()
    {
        return newManual(kSingleHeelManual);
    }
    READABLE_FUNCTION_WAITS(newSingleHeelManual, Verified);
    ee::Ptr<GameObject> newDoubleToeManual()
    {
        return newManual(kDoubleToeManual);
    }
    READABLE_FUNCTION_WAITS(newDoubleToeManual, Draft);
    ee::Ptr<GameObject> newSingleToeManual()
    {
        return newManual(kSingleToeManual);
    }
    READABLE_FUNCTION_WAITS(newSingleToeManual, Verified);

    // manualTrickId: a manual's trick id (its variant under the kind's five
    // top bits).
    uint32_t manualTrickId(uint32_t variant)
    {
        return variant | trickId(kObjectManualTrick);
    }
    READABLE_FUNCTION(manualTrickId, Verified);
}
