// Trick objects (trick_objects.h): the trick table's create functions, the
// stall table's, and the grind trick combiners combineGrindTrick
// (tricks.cpp) picks.
//
// A grind trick takes its worth from the developers' tuning table,
// category 10 (Scoring: Grinds: 25 Init. Worth entries, then 25 Worth/sec,
// in the order of the trick types; the plain trick, type 0, is worth a
// BackSlide); a stall trick from category 11 (Scoring: Stalls, 5 and 5).

#include "skater.h"
#include "trick_objects.h"

#include "aggressive/objects.h"
#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/trick_objects.cpp.h"

namespace aggressive::skater { uint32_t airFlipTrickId(uint32_t); }
namespace aggressive::skater { uint32_t airGrabTrickId(uint32_t); }
namespace aggressive::skater { uint32_t grindTrickId(uint32_t); }
namespace aggressive::skater { uint32_t manualTrickId(uint32_t); }
namespace aggressive::skater { uint32_t poleTrickId(uint32_t); }
namespace aggressive::skater { uint32_t stallTrickId(uint32_t); }
namespace aggressive::skater { uint32_t vaultTrickId(uint32_t); }
namespace aggressive::skater { uint32_t wallTrickId(uint32_t); }

namespace aggressive::skater
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_trickPool> g_trickPool;      // grind tricks' state blocks
        inline ee::Global<uint32_t, sym::g_stallTrickPool> g_stallTrickPool; // stall tricks' state blocks

        constexpr uint32_t kScoringGrinds = 10, kScoringStalls = 11;
        constexpr uint32_t kGrindTricks = 25, kStallTricks = 5;

        ee::Ptr<TrickState> trickState(ee::Ptr<GameObject> trick)
        {
            return ee::Ptr<TrickState>(gameObjectState(ee::Ptr<GameObject>(trick)));
        }

        const TuningEntry &tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry];
        }

        // A create function's trick: its type, kind, flags and stance.
        struct GrindTrickKind
        {
            uint32_t type, kind, flags, stance;
            bool clearsTime; // the plain trick and the Soul store +0x2C again (already 0)
        };
        constexpr uint32_t kSlideFlags = 0x6, kSoulFlags = 0x10;
        constexpr GrindTrickKind slide(uint32_t type) { return {type, 1, kSlideFlags, 2, false}; }
        constexpr GrindTrickKind soul(uint32_t type) { return {type, 2, kSoulFlags, 1, false}; }

        ee::Ptr<GameObject> makeGrindTrick(const GrindTrickKind &k)
        {
            const ee::Ptr<GameObject> trick = newTrickObject();
            const ee::Ptr<TrickState> state = trickState(trick);
            state->type = k.type;
            state->kind = k.kind;
            state->flags = k.flags;
            state->stance = k.stance;
            state->hop = 0;
            state->switched = 0;
            if (k.clearsTime)
                state->time = 0.0f;
            const uint32_t entry = k.type == 0 ? 1u : k.type - 1u;
            state->worth = tuning(kScoringGrinds, entry).value;
            state->holdTime = 0.0f;
            state->worthPerSecond = tuning(kScoringGrinds, kGrindTricks + entry).value;
            return trick;
        }

        ee::Ptr<GameObject> makeStallTrick(uint32_t type)
        {
            const ee::Ptr<GameObject> trick = newStallTrickObject();
            const ee::Ptr<StallTrickState> state(gameObjectState(ee::Ptr<GameObject>(trick)));
            state->type = type;
            state->worth = tuning(kScoringStalls, type).value;
            state->worthPerSecond = tuning(kScoringStalls, kStallTricks + type).value;
            return trick;
        }

        // The combiners' stance tables: the foot a stance is on (1 or 2),
        // and the stance on the other foot.
        uint32_t footOfStance(uint32_t stance)
        {
            static constexpr uint32_t kFoot[5] = {0, 2, 1, 1, 2};
            return stance < 5 ? kFoot[stance] : 0;
        }
        uint32_t switchedStance(uint32_t stance)
        {
            return stance < 5 && stance != 0 ? 5 - stance : 0;
        }
    }

    // newTrickObject: a new grind trick object (kind 2), its state block from
    // the trick pool, cleared: the plain trick's type, its own animation
    // on (no hop), worth nothing yet.
    ee::Ptr<GameObject> newTrickObject()
    {
        const uint32_t block = aggressive::poolTake(*g_trickPool);
        const ee::Ptr<GameObject> trick = newGameObject(uint64_t(kObjectTrick), block);
        zge::memoryFill(block, 0, static_cast<uint32_t>(sizeof(TrickState)));
        const ee::Ptr<TrickState> state(block);
        state->holdTime = 0.0f;
        state->hopDone = 1;
        state->type = 1;
        state->held = 0;
        state->worth = ee::Float::fromBits(0);
        state->worthPerSecond = ee::Float::fromBits(0);
        return trick;
    }
    READABLE_FUNCTION(newTrickObject, Verified);

    // newStallTrickObject: a new stall trick object (kind 10), cleared.
    ee::Ptr<GameObject> newStallTrickObject()
    {
        const uint32_t block = aggressive::poolTake(*g_stallTrickPool);
        const ee::Ptr<GameObject> trick = newGameObject(uint64_t(kObjectStallTrick), block);
        zge::memoryFill(block, 0, static_cast<uint32_t>(sizeof(StallTrickState)));
        *ee::Ptr<StallTrickState>(block) = StallTrickState{};
        return trick;
    }
    READABLE_FUNCTION(newStallTrickObject, Verified);

    // The trick table's create functions (g_trickButtons and the plain one).
    ee::Ptr<GameObject> newPlainGrindTrick() { return makeGrindTrick({0, 1, kSlideFlags, 2, true}); }
    READABLE_FUNCTION(newPlainGrindTrick, Verified);
    ee::Ptr<GameObject> newSoul() { return makeGrindTrick({1, 2, kSoulFlags, 1, true}); }
    READABLE_FUNCTION(newSoul, Verified);
    ee::Ptr<GameObject> newBackSlide() { return makeGrindTrick(slide(2)); }
    READABLE_FUNCTION(newBackSlide, Verified);
    ee::Ptr<GameObject> newMiszou() { return makeGrindTrick(soul(3)); }
    READABLE_FUNCTION(newMiszou, Verified);
    ee::Ptr<GameObject> newFishBrain() { return makeGrindTrick(soul(4)); }
    READABLE_FUNCTION(newFishBrain, Verified);
    ee::Ptr<GameObject> newRoyale() { return makeGrindTrick(slide(5)); }
    READABLE_FUNCTION(newRoyale, Verified);
    ee::Ptr<GameObject> newMakio() { return makeGrindTrick(soul(6)); }
    READABLE_FUNCTION(newMakio, Verified);
    ee::Ptr<GameObject> newPornstar() { return makeGrindTrick(soul(7)); }
    READABLE_FUNCTION(newPornstar, Verified);
    ee::Ptr<GameObject> newFastslide() { return makeGrindTrick(slide(8)); }
    READABLE_FUNCTION(newFastslide, Verified);
    ee::Ptr<GameObject> newTorqueSoul() { return makeGrindTrick(soul(9)); }
    READABLE_FUNCTION(newTorqueSoul, Verified);
    ee::Ptr<GameObject> newSoyale() { return makeGrindTrick(soul(10)); }
    READABLE_FUNCTION(newSoyale, Verified);
    ee::Ptr<GameObject> newX() { return makeGrindTrick(soul(11)); }
    READABLE_FUNCTION(newX, Verified);
    ee::Ptr<GameObject> newAcidSoul() { return makeGrindTrick(soul(12)); }
    READABLE_FUNCTION(newAcidSoul, Verified);
    ee::Ptr<GameObject> newFreestyleFastslide() { return makeGrindTrick(slide(13)); }
    READABLE_FUNCTION(newFreestyleFastslide, Verified);
    ee::Ptr<GameObject> newFreestyleBackslide() { return makeGrindTrick(slide(14)); }
    READABLE_FUNCTION(newFreestyleBackslide, Verified);
    ee::Ptr<GameObject> newFreestyleFishbrain() { return makeGrindTrick(soul(15)); }
    READABLE_FUNCTION(newFreestyleFishbrain, Verified);
    ee::Ptr<GameObject> newFreestyleMakio() { return makeGrindTrick(soul(16)); }
    READABLE_FUNCTION(newFreestyleMakio, Verified);
    ee::Ptr<GameObject> newWonder() { return makeGrindTrick(slide(17)); }
    READABLE_FUNCTION(newWonder, Verified);
    ee::Ptr<GameObject> newSweatStance() { return makeGrindTrick(soul(18)); }
    READABLE_FUNCTION(newSweatStance, Verified);
    ee::Ptr<GameObject> newMistrial() { return makeGrindTrick(soul(19)); }
    READABLE_FUNCTION(newMistrial, Verified);
    ee::Ptr<GameObject> newUnity() { return makeGrindTrick(slide(20)); }
    READABLE_FUNCTION(newUnity, Verified);
    ee::Ptr<GameObject> newSfCombo() { return makeGrindTrick(soul(21)); }
    READABLE_FUNCTION(newSfCombo, Verified);
    ee::Ptr<GameObject> newFmCombo() { return makeGrindTrick(soul(22)); }
    READABLE_FUNCTION(newFmCombo, Verified);
    ee::Ptr<GameObject> newMsCombo() { return makeGrindTrick(soul(23)); }
    READABLE_FUNCTION(newMsCombo, Verified);
    ee::Ptr<GameObject> newDoubleGrindTrick() { return makeGrindTrick({24, 0, 0, 1, false}); }
    READABLE_FUNCTION(newDoubleGrindTrick, Verified);
    ee::Ptr<GameObject> newHandGrindTrick() { return makeGrindTrick({25, 0, 0, 1, false}); }
    READABLE_FUNCTION(newHandGrindTrick, Verified);

    // The stall table's (g_stallTricks) and the plain stall trick.
    ee::Ptr<GameObject> newHandPlant() { return makeStallTrick(0); }
    READABLE_FUNCTION(newHandPlant, Verified);
    ee::Ptr<GameObject> newAbstractHandPlant() { return makeStallTrick(1); }
    READABLE_FUNCTION(newAbstractHandPlant, Verified);
    ee::Ptr<GameObject> newRocketHandPlant() { return makeStallTrick(2); }
    READABLE_FUNCTION(newRocketHandPlant, Verified);
    ee::Ptr<GameObject> newMethodHandPlant() { return makeStallTrick(3); }
    READABLE_FUNCTION(newMethodHandPlant, Verified);
    ee::Ptr<GameObject> newStaleHandPlant() { return makeStallTrick(4); }
    READABLE_FUNCTION(newStaleHandPlant, Verified);

    // skaterCurrentObject: the object the skater acts with (a trick, a pole, a
    // vehicle), 0 for none: the front of its slot (Player::currentObjectSlot)
    // while Player::hasCurrentObject says there is one.
    ee::Ptr<GameObject> skaterCurrentObject(ee::Ptr<Player> player)
    {
        if (!player->hasCurrentObject)
            return {};
        const ee::Ptr<GameObject> object = objectInSlot(player->currentObjectSlot);
        return object;
    }
    READABLE_FUNCTION(skaterCurrentObject, Verified);

    namespace
    {
        // The ids keepTrickIds makes, kept in order: only written, no code
        // reads them.
        inline ee::Global<uint32_t[57], sym::g_trickIds> g_trickIds;

        constexpr uint32_t kFirstTrickIdReturn = points::keepTrickIds::afterGrindTrickId; // keepTrickIds' first call returns here, each next 12 on

        constexpr uint32_t kMessageNames[] = {
            sym::g_textGetScore, // getScore
            sym::g_textGetMultiplierBonus, // getMultiplierBonus
            sym::g_textGetName, // getName
            sym::g_textAreYouFinished, // areYouFinished
            sym::g_textAreYouPossible, // areYouPossible
            sym::g_textGetSpaceNeededForSaving, // GetSpaceNeededForSaving
            sym::g_textSaveToBuffer, // SaveToBuffer
            sym::g_textLoadFromBuffer, // LoadFromBuffer
            sym::g_textTrickStarted, // TrickStarted
            sym::g_textTrickFinished, // TrickFinished
            sym::g_textTrickUpdate, // TrickUpdate
            sym::g_textButtonReleased, // ButtonReleased
            sym::g_textTrickStartHolding, // TrickStartHolding
            sym::g_textTrickStoppedHolding, // TrickStoppedHolding
            sym::g_textAnimationEnded, // AnimationEnded
            sym::g_textTrickDroppedFromQueue, // TrickDroppedFromQueue
        };
        constexpr uint32_t kMessageHashes[] = {
            sym::g_msgGetScore, sym::g_msgGetMultiplierBonus, sym::g_msgGetName, sym::g_msgAreYouFinished, sym::g_msgAreYouPossible, sym::g_msgGetSpaceNeededForSaving, sym::g_msgSaveToBuffer, sym::g_msgLoadFromBuffer,
            sym::g_msgTrickStarted, sym::g_msgTrickFinished, sym::g_msgTrickUpdate, sym::g_msgButtonReleased, sym::g_msgTrickStartHolding, sym::g_msgTrickStoppedHolding, sym::g_msgAnimationEnded, sym::g_msgTrickDroppedFromQueue,
        };
        constexpr uint32_t kMessageCount = sizeof(kMessageNames) / sizeof(kMessageNames[0]);
        constexpr uint32_t kKeepTrickIds = sym::keepTrickIds;

        // A word handed on to the object the skater acts with, as the
        // message's argument (AnimationEnd's shape), if it has one. The
        // first call returns at `first`, the message's at first + 0x24.
        int32_t tellCurrentObject(ee::Ptr<Player> player, ee::Address word, ee::Float time, uint32_t message)
        {
            const ee::Ptr<GameObject> object = skaterCurrentObject(player);
            if (object == 0)
                return 0;
            ee::Local<AnimationEnd> note;
            note->player = player;
            note->time = time;
            note->event = word;
            return sendObjectMessage(object, message, note, 0);
        }
    }

    // keepTrickIds: every trick's id into the table of ids.
    void keepTrickIds()
    {
        uint32_t next = 0;
        for (uint32_t type = 0; type <= 20; ++type, ++next)
            (*g_trickIds)[next] = grindTrickId(type);
        for (uint32_t type = 24; type <= 25; ++type, ++next)
            (*g_trickIds)[next] = grindTrickId(type);
        for (uint32_t type = 0; type <= 4; ++type, ++next)
            (*g_trickIds)[next] = stallTrickId(type);
        for (uint32_t type = 2; type <= 9; ++type, ++next)
            (*g_trickIds)[next] = airGrabTrickId(type);
        for (uint32_t type = 2; type <= 10; ++type, ++next)
            (*g_trickIds)[next] = airFlipTrickId(type);
        for (uint32_t type = 0; type <= 3; ++type, ++next)
            (*g_trickIds)[next] = manualTrickId(type);
        for (uint32_t type = 0; type <= 1; ++type, ++next)
            (*g_trickIds)[next] = vaultTrickId(type);
        for (uint32_t type = 0; type <= 1; ++type, ++next)
            (*g_trickIds)[next] = wallTrickId(type);
        for (uint32_t type = 0; type <= 2; ++type, ++next)
            (*g_trickIds)[next] = poleTrickId(type);
        (*g_trickIds)[next] = skitchTrickId();
    }
    READABLE_FUNCTION(keepTrickIds, Verified);

    // skaterSetup: the game objects at boot: the messages' hashes
    // (objects.h), the slots' pools, the ids, the object classes: every step
    // must do. Whether all did.
    int32_t skaterSetup()
    {
        for (uint32_t i = 0; i < kMessageCount; ++i)
            *ee::Ptr<uint32_t>(kMessageHashes[i]) = zge::nameHash(ee::Ptr<const char>(kMessageNames[i]));
        setupSlotPools();
        keepTrickIds();
        bool done = false;
        if (setupObjectClasses() != 0)
        {
            done = registerGroundObjectClass() != 0 && registerGrindTrickClass() != 0 &&
                   registerStallTrickClass() != 0 && registerManualTrickClass() != 0 &&
                   registerAirTrickClass() != 0 && registerTextObjectClass() != 0 &&
                   setupWallTricks() != 0 && setupPoleTricks() != 0 &&
                   setupVaultTricks() != 0 && setupSkitch() != 0 &&
                   registerCessSlideClass() != 0;
        }
        return done ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(skaterSetup, Verified);

    // freeGameObjectClasses: skaterSetup undone, as far as each step does. Whether
    // all did.
    int32_t freeGameObjectClasses()
    {
        const bool classesDone =
            unregisterGroundObjectClass() != 0 && unregisterGrindTrickClass() != 0 &&
            unregisterStallTrickClass() != 0 && unregisterManualTrickClass() != 0 &&
            unregisterAirTrickClass() != 0 && unregisterTextObjectClass() != 0 &&
            shutdownWallTricks() != 0 && shutdownPoleTricks() != 0 &&
            shutdownVaultTricks() != 0 && shutdownSkitch() != 0 &&
            unregisterCessSlideClass() != 0;
        bool poolsDone = false;
        if (classesDone)
            poolsDone = freeSlotPools() != 0;
        bool done = false;
        if (poolsDone)
            done = freeObjectClasses() != 0;
        return done ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(freeGameObjectClasses, Verified);

    // sendTrickStartHolding, sendTrickStoppedHolding, sendAnimationEnded: an animation event to the
    // object the skater acts with (zgui's trick animation events).
    int32_t sendTrickStartHolding(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time)
    {
        return tellCurrentObject(player, event, time, *g_msgTrickStartHolding);
    }
    READABLE_FUNCTION_WAITS(sendTrickStartHolding, Verified);

    int32_t sendTrickStoppedHolding(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time)
    {
        return tellCurrentObject(player, event, time, *g_msgTrickStoppedHolding);
    }
    READABLE_FUNCTION_WAITS(sendTrickStoppedHolding, Verified);

    int32_t sendAnimationEnded(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time)
    {
        return tellCurrentObject(player, event, time, *g_msgAnimationEnded);
    }
    READABLE_FUNCTION_WAITS(sendAnimationEnded, Verified);

    // setTrickSoulFakie: a soul trick's fakie (flags 0x18 fakie, 0x10 not).
    int32_t setTrickSoulFakie(ee::Ptr<GameObject> trick, int32_t fakie)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        const uint32_t cleared = state->flags & ~0x18u;
        state->flags = cleared;
        uint32_t flags = cleared | 0x18u;
        if (!fakie)
            flags = cleared | 0x10u;
        state->flags = flags;
        return 1;
    }
    READABLE_FUNCTION(setTrickSoulFakie, Verified);

    // ---- the grind trick combiners -------------------------------------------
    //
    // A grind trick asked for on the rail while another is current (trick:
    // the new one, current: the one the skater does; matched: matchTrick's
    // bits). The new trick carries on from the current one: its flags,
    // stance and side, then with an extra button 1 the stance on the other
    // foot, with a button 5 fakie the other way. Always 1.

    // combineSlideOnSlide: a slide on a slide.
    int32_t combineSlideOnSlide(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        const ee::Ptr<TrickState> from = trickState(current);
        state->flags = from->flags;
        state->stance = from->stance;
        const uint32_t side = from->side;
        state->hop = 0;
        state->side = side;
        state->switched = 0;
        state->hopDone = 0;
        if (matched & kMatchedSwitch)
        {
            state->hop = footOfStance(from->stance);
            state->stance = switchedStance(from->stance);
            state->switched = 1;
        }
        if (matched & kMatchedFakie)
        {
            setTrickFakie(trick, static_cast<int32_t>(~from->flags & kTrickFakie));
            if (state->hop != 0)
                return 1;
            state->hop = state->stance;
        }
        return 1;
    }
    READABLE_FUNCTION(combineSlideOnSlide, Verified);

    // combineSoulOnSlide: a soul on a slide: stance 1 (4 switched, and fakie),
    // with a hop.
    int32_t combineSoulOnSlide(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        const ee::Ptr<TrickState> from = trickState(current);
        state->flags = kSoulFlags;
        state->stance = 1;
        const uint32_t side = from->side;
        state->hop = 1;
        state->side = side;
        state->switched = 0;
        state->hopDone = 0;
        if (matched & kMatchedSwitch)
        {
            state->hop = 4;
            state->stance = 4;
            setTrickFakie(trick, 1);
        }
        if (matched & kMatchedFakie)
            state->flags |= 0x18;
        return 1;
    }
    READABLE_FUNCTION(combineSoulOnSlide, Verified);

    // combineOtherOnSlide: a Double or Hand Grind trick on a slide: as a soul.
    int32_t combineOtherOnSlide(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        return combineSoulOnSlide(trick, current, matched);
    }
    READABLE_FUNCTION(combineOtherOnSlide, Verified);

    // combineSoulOnSoul: a soul on a soul (or on a Double or Hand Grind trick).
    int32_t combineSoulOnSoul(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        const ee::Ptr<TrickState> from = trickState(current);
        state->flags = from->flags;
        state->stance = from->stance;
        const uint32_t side = from->side;
        state->hop = 0;
        state->side = side;
        state->switched = 0;
        state->hopDone = 0;
        if (matched & kMatchedSwitch)
        {
            state->hop = footOfStance(from->stance);
            state->stance = switchedStance(from->stance);
            setTrickFakie(trick, static_cast<int32_t>(~from->flags & kTrickFakie));
            state->switched = 1;
        }
        if (matched & kMatchedFakie)
        {
            setTrickSoulFakie(trick, (state->flags & 0x8) == 0 ? 1 : 0);
            if (state->hop != 0)
                return 1;
            state->hop = state->stance;
        }
        return 1;
    }
    READABLE_FUNCTION(combineSoulOnSoul, Verified);

    // combineSlideOnSoul: a slide on a soul (or a Double or Hand Grind trick):
    // the stance from the current trick's side, as a slide's is, its hop
    // the current trick's stance.
    int32_t combineSlideOnSoul(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        const ee::Ptr<TrickState> from = trickState(current);
        state->flags = kSlideFlags;
        state->stance = from->side;
        const uint32_t side = from->side;
        state->side = side;
        const uint32_t hop = from->stance;
        state->switched = 0;
        state->hop = hop;
        state->hopDone = 0;
        if (matched & kMatchedSwitch)
        {
            const uint32_t switched = switchedStance(side);
            const uint32_t flags = state->flags;
            state->stance = switched;
            state->flags = (flags & ~0x6u) | 0x4u;
        }
        if (matched & kMatchedFakie)
            setTrickFakie(trick, 1);
        return 1;
    }
    READABLE_FUNCTION(combineSlideOnSoul, Verified);

    // combineOtherOnSoul: a Double or Hand Grind trick on a soul: as a soul on a
    // soul, its hop its stance when the combine made none.
    int32_t combineOtherOnSoul(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        trickState(current); // (asked for, not used)
        const int32_t combined = combineSoulOnSoul(trick, current, matched);
        if (state->hop == 0)
        {
            if (combined == 0)
                return 0;
            state->hop = state->stance;
        }
        return combined;
    }
    READABLE_FUNCTION(combineOtherOnSoul, Verified);

    // combineOtherOnSoulB: a second entry to combineOtherOnSoul (the same
    // three arguments, its result).
    int32_t combineOtherOnSoulB(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        return aggressive::skater::combineOtherOnSoul(trick, current, matched);
    }
    READABLE_FUNCTION(combineOtherOnSoulB, Verified);
}
