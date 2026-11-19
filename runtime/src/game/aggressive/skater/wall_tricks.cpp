// The wall tricks (game objects of kind 6; were the sweep's drafts of
// wallTrickScore .. newWallTrick): Wall Ride and Wall Tap, the trick the Wall Ride
// and Wall Tap states make (startWallRideTrick, state_tricks.cpp). Worth
// Scoring: Wall Tricks' Init. Worth on the start and its Worth/sec while it
// lasts. See action_tricks.h for the modules' shape.

#include "action_tricks.h"

#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "trick_objects.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/wall_tricks.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {
        // A wall trick's state block (20 bytes, from its pool).
        struct WallTrickState
        {
            uint32_t variant;          // +0x00 kWallRide or kWallTap (the table's id)
            ee::Float worth;           // +0x04 points so far
            ee::Float worthPerSecond;  // +0x08
            uint32_t finished;         // +0x0C areYouFinished's answer (1 from the start, and once its animation ends)
            uint32_t neverRead;        // +0x10 cleared when made
        };
        static_assert(sizeof(WallTrickState) == 20 && offsetof(WallTrickState, finished) == 0x0C);
        constexpr uint32_t kWallRide = 0, kWallTap = 1;

        inline ee::Global<uint32_t, sym::g_wallTrickPool> g_wallTrickPool;
        constexpr uint32_t kWallTrickCount = 2;
        inline ee::Global<TrickEntry[kWallTrickCount], sym::wallTricks> g_wallTricks;
        constexpr uint32_t kWallTrickText = sym::wallTrickText2;   // the text buffer (32 bytes)
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat4;  // "Could not find the '%s' animation."
        constexpr uint32_t kEmpty = sym::empty5;           // ""
        constexpr uint32_t kSomeWallTrick = sym::someWallTrick;   // "Some Wall Trick"

        // Scoring: Wall Tricks (tuning category 13).
        constexpr uint32_t kScoringWallTricks = 13;
        constexpr uint32_t kWallTapWorth = 0, kWallRideWorth = 1, kWallTapWorthPerSecond = 2, kWallRideWorthPerSecond = 3;
        const ee::Float &wallTrickTuning(uint32_t entry)
        {
            return (*g_tuningCategories)[kScoringWallTricks].entries[entry].value;
        }

        // The module's functions, as their callers call them.
        constexpr uint32_t kWallTrickAnimationAt = sym::wallTrickAnimation;
        constexpr uint32_t kResolveWallTricksAt = sym::resolveWallTricks;
        constexpr uint32_t kFindWallTrickAt = sym::findWallTrick;
        constexpr uint32_t kWallTrickTextAt = sym::wallTrickText;
    }

    // The handlers (the class's message table; the note's argument as each
    // message has it, aggressive/objects.h).

    // wallTrickScore ("getScore").
    uint32_t wallTrickScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &WallTrickState::worth);
    }
    READABLE_FUNCTION(wallTrickScore, Verified);

    // wallTrickMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t wallTrickMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(wallTrickMultiplierBonus, Verified);

    // wallTrickName ("getName").
    uint32_t wallTrickName(ee::Ptr<ObjectMessageNote> note)
    {
        return answerName(note, kWallTrickTextAt, kSomeWallTrick);
    }
    READABLE_FUNCTION(wallTrickName, Verified);

    // wallTrickPossible ("areYouPossible"): in Wall Ride or Wall Tap.
    int32_t wallTrickPossible(ee::Ptr<ObjectMessageNote> note)
    {
        return answerPossible(note, [](uint32_t state) { return state - static_cast<uint32_t>(State::WallRide) < 2u; });
    }
    READABLE_FUNCTION(wallTrickPossible, Verified);

    // wallTrickFinished ("areYouFinished").
    uint32_t wallTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return answerFinished(note, &WallTrickState::finished);
    }
    READABLE_FUNCTION(wallTrickFinished, Verified);

    // wallTrickSaveSize ("GetSpaceNeededForSaving").
    uint32_t wallTrickSaveSize(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(WallTrickState));
    }
    READABLE_FUNCTION(wallTrickSaveSize, Verified);

    // saveWallTrick ("SaveToBuffer").
    uint32_t saveWallTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(WallTrickState));
    }
    READABLE_FUNCTION(saveWallTrick, Verified);

    // loadWallTrick ("LoadFromBuffer").
    uint32_t loadWallTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_wallTrickPool, sizeof(WallTrickState));
    }
    READABLE_FUNCTION_WAITS(loadWallTrick, Verified);

    uint32_t wallTrickText(ee::Ptr<GameObject> object);

    ee::Ptr<zge::Motion> wallTrickAnimation(ee::Ptr<GameObject> object);

    // startWallTrick ("TrickStarted"): the variant from the skater's state,
    // the trick's text shown, its worth and worth a second from the tuning,
    // the worth banked, and the trick's animation played (mirrored when the
    // wall is on the other side of the skater's stance). Returns the
    // animation's play.
    uint32_t startWallTrick(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<WallTrickState> state(gameObjectState(note->object));
        if (player->state == State::WallRide)
            state->variant = kWallRide;
        else if (player->state == State::WallTap)
            state->variant = kWallTap;
        const uint32_t text = wallTrickText(note->object);
        if (text && zge::compareStrings(text, kEmpty) != 0)
            hud::showSpinText(player);
        if (state->variant == kWallRide)
        {
            state->worth = wallTrickTuning(kWallRideWorth);
            const ee::Float perSecond = wallTrickTuning(kWallRideWorthPerSecond);
            state->worthPerSecond = perSecond;
        }
        else if (state->variant == kWallTap)
        {
            state->worth = wallTrickTuning(kWallTapWorth);
            const ee::Float perSecond = wallTrickTuning(kWallTapWorthPerSecond);
            state->worthPerSecond = perSecond;
        }
        bankStartWorth(player, state->worth);
        const ee::Ptr<zge::Motion> animation = wallTrickAnimation(note->object);
        if (!animation)
            return 0;
        uint32_t mode = 5;
        const uint8_t fakie = player->fakie;
        if (player->side == 1)
        {
            if (fakie != 1)
                mode = kHalfMirrored;
        }
        else
        {
            if (fakie != 0)
                mode = kHalfMirrored;
        }
        const uint32_t played = zge::playAnimation(player->node, animation, mode, animationBlendTime(), kPlayOnce);
        return played;
    }
    READABLE_FUNCTION_WAITS(startWallTrick, Verified);

    // finishWallTrick ("TrickFinished").
    uint32_t finishWallTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(finishWallTrick, Verified);

    // updateWallTrick ("TrickUpdate"): the worth a second over the frame, on
    // the trick's worth and the line's pending score.
    uint32_t updateWallTrick(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> update(note->argument);
        const ee::Ptr<Player> player(update->player);
        const ee::Ptr<WallTrickState> state(gameObjectState(note->object));
        const ee::Float gained = state->worthPerSecond * update->seconds;
        if (ee::Float(0.0f) < gained)
        {
            state->worth = state->worth + gained;
            const ee::Float pending = storedPendingScore(player);
            setPendingScore(player, pending + gained);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateWallTrick, Verified);

    // endWallTrickAnimation ("AnimationEnded"): finished; the skater's state's
    // object made current.
    uint32_t endWallTrickAnimation(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<WallTrickState> state(gameObjectState(note->object));
        const ee::Ptr<Player> player = ee::Ptr<AnimationEnd>(note->argument)->player;
        state->finished = 1;
        selectStateObject(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(endWallTrickAnimation, Verified);

    // dropWallTrick ("TrickDroppedFromQueue").
    uint32_t dropWallTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_wallTrickPool);
    }
    READABLE_FUNCTION(dropWallTrick, Verified);

    // wallTrickAnimation: the trick's animation.
    ee::Ptr<zge::Motion> wallTrickAnimation(ee::Ptr<GameObject> object)
    {
        return trickAnimation(object, kFindWallTrickAt);
    }
    READABLE_FUNCTION(wallTrickAnimation, Verified);

    // resolveWallTricks: the table resolved (the class's setup).
    uint32_t resolveWallTricks()
    {
        return resolveTricks(*g_wallTricks, kNotFoundFormat, kEmpty);
    }
    READABLE_FUNCTION_WAITS(resolveWallTricks, Verified);

    // findWallTrick: the table's entry of a variant.
    ee::Ptr<TrickEntry> findWallTrick(uint32_t variant)
    {
        return findTrick(*g_wallTricks, variant);
    }
    READABLE_FUNCTION(findWallTrick, Verified);

    // wallTrickText: the trick's text (in the module's buffer).
    uint32_t wallTrickText(ee::Ptr<GameObject> object)
    {
        return trickText<TrickEntry>(object, kWallTrickText, kFindWallTrickAt);
    }
    READABLE_FUNCTION(wallTrickText, Verified);

    // setupWallTricks: the class set up (setupObjectClasses' list): the state
    // blocks' pool (5 of 20 bytes), the class and its handlers, the table.
    uint32_t setupWallTricks()
    {
        *g_wallTrickPool = newPool(static_cast<uint32_t>(sizeof(WallTrickState)), 5, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible, *g_msgAreYouFinished,
                                     *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer, *g_msgTrickStarted,
                                     *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgAnimationEnded, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::wallTrickScore, sym::wallTrickMultiplierBonus, sym::wallTrickName, sym::wallTrickPossible, sym::wallTrickFinished, sym::wallTrickSaveSize, sym::saveWallTrick,
                                     sym::loadWallTrick, sym::startWallTrick, sym::finishWallTrick, sym::updateWallTrick, sym::endWallTrickAnimation, sym::dropWallTrick};
        return registerTrickClass(kObjectWallRideTrick, messages, handlers, kResolveWallTricksAt);
    }
    READABLE_FUNCTION_WAITS(setupWallTricks, Verified);

    // shutdownWallTricks: the class shut down.
    int32_t shutdownWallTricks()
    {
        return unregisterTrickClass(g_wallTrickPool, kObjectWallRideTrick);
    }
    READABLE_FUNCTION_WAITS(shutdownWallTricks, Verified);

    // newWallTrick: a new wall trick (startWallRideTrick's), finished from
    // the start (it may be cut short any time).
    ee::Ptr<GameObject> newWallTrick()
    {
        ee::Ptr<WallTrickState> state;
        const ee::Ptr<GameObject> object = newTrickObject(g_wallTrickPool, kObjectWallRideTrick, state);
        state->neverRead = 0;
        state->finished = 1;
        state->variant = 1;
        state->worth = ee::Float::fromBits(0);
        state->worthPerSecond = ee::Float::fromBits(0);
        return object;
    }
    READABLE_FUNCTION_WAITS(newWallTrick, Verified);

    // wallTrickId: a wall trick's id, its kind in the top five bits and
    // the variant below (skaterSetup's list: WallRide 0, WallTap 1).
    uint32_t wallTrickId(uint32_t variant)
    {
        return variant | trickId(kObjectWallRideTrick);
    }
    READABLE_FUNCTION(wallTrickId, Verified);
}
