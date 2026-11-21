// The pole tricks (game objects of kind 8; were the sweep's drafts of
// poleTrickScore .. poleTrickId): the trick the pole states make (startPoleTrick,
// state_tricks.cpp): VerticalPole, VerticalAirPole and HorizontalPole by the
// state, worth 100 on the start. Held while the button is: once it is let
// go (ButtonReleased) the next end of the animation releases the pole (the
// state's move 11); ForcePoleRelease releases it at once. See
// action_tricks.h for the modules' shape.

#include "action_tricks.h"

#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "trick_objects.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/pole_trick.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {
        // A pole trick's state block (16 bytes, from its pool).
        struct PoleTrickState
        {
            uint32_t variant;  // +0x00 the table's id: kVerticalPole, kVerticalAirPole, kHorizontalPole
            ee::Float worth;   // +0x04 100
            uint32_t finished; // +0x08 areYouFinished's answer: once released
            uint32_t holding;  // +0x0C 1 until the button is let go
        };
        static_assert(sizeof(PoleTrickState) == 16 && offsetof(PoleTrickState, holding) == 0x0C);
        constexpr uint32_t kVerticalPole = 0, kVerticalAirPole = 1, kHorizontalPole = 2;
        inline constexpr ee::Float kPoleWorth = 100.0f;

        // A pole trick of the table (0x48 bytes): its four animations by
        // name (the skater's side of the pole, forward or fakie: alley-oop)
        // and its text.
        struct PoleAnimation
        {
            uint32_t name;      // "global_assets.cedwards.VertPoleLeft"
            ee::Ptr<zge::Motion> animation; // found at the class's setup
        };
        struct PoleTrickEntry
        {
            uint32_t id;                    // +0x00
            PoleAnimation left;             // +0x04
            PoleAnimation right;            // +0x0C
            PoleAnimation leftAlleyOop;     // +0x14
            PoleAnimation rightAlleyOop;    // +0x1C
            uint32_t textKey;               // +0x24 "VerticalPole"
            char text[0x20];                // +0x28
        };
        static_assert(sizeof(PoleTrickEntry) == 0x48 && offsetof(PoleTrickEntry, textKey) == 0x24 &&
                      offsetof(PoleTrickEntry, text) == 0x28);

        inline ee::Global<uint32_t, sym::g_poleTrickPool> g_poleTrickPool;
        // The class's own message: nameHash("ForcePoleRelease") (its argument: the player).
        inline ee::Global<uint32_t, sym::g_msgForcePoleRelease> g_msgForcePoleRelease;
        constexpr uint32_t kForcePoleReleaseName = sym::forcePoleReleaseName; // "ForcePoleRelease"
        constexpr uint32_t kPoleTrickCount = 3;
        inline ee::Global<PoleTrickEntry[kPoleTrickCount], sym::poleTricks> g_poleTricks;
        constexpr uint32_t kPoleTrickText = sym::poleTrickText2;   // the text buffer (32 bytes)
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat7;  // "Could not find the '%s' animation."
        constexpr uint32_t kEmpty = sym::empty8;           // ""
        constexpr uint32_t kSomePoleTrick = sym::somePoleTrick;   // "Some Pole Trick"
        constexpr uint32_t kPoleMode = 5;                  // playAnimation's mode

        // The module's functions, as their callers call them.
        constexpr uint32_t kPoleAnimationAt = sym::poleAnimation;
        constexpr uint32_t kResolvePoleTricksAt = sym::resolvePoleTricks;
        constexpr uint32_t kFindPoleTrickAt = sym::findPoleTrick;
        constexpr uint32_t kPoleTrickTextAt = sym::poleTrickText;
    }

    // The handlers (the class's message table).

    // poleTrickScore ("getScore").
    uint32_t poleTrickScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &PoleTrickState::worth);
    }
    READABLE_FUNCTION(poleTrickScore, Verified);

    // poleMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t poleMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(poleMultiplierBonus, Verified);

    // poleTrickName ("getName").
    uint32_t poleTrickName(ee::Ptr<ObjectMessageNote> note)
    {
        return answerName(note, kPoleTrickTextAt, kSomePoleTrick);
    }
    READABLE_FUNCTION(poleTrickName, Verified);

    // poleTrickPossible ("areYouPossible"): in a pole state.
    int32_t poleTrickPossible(ee::Ptr<ObjectMessageNote> note)
    {
        return answerPossible(note, [](uint32_t state) { return state - static_cast<uint32_t>(State::VerticalPole) < 3u; });
    }
    READABLE_FUNCTION(poleTrickPossible, Verified);

    // poleTrickFinished ("areYouFinished").
    uint32_t poleTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return answerFinished(note, &PoleTrickState::finished);
    }
    READABLE_FUNCTION(poleTrickFinished, Verified);

    // poleTrickSaveSize ("GetSpaceNeededForSaving").
    uint32_t poleTrickSaveSize(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(PoleTrickState));
    }
    READABLE_FUNCTION(poleTrickSaveSize, Verified);

    // savePoleTrick ("SaveToBuffer").
    uint32_t savePoleTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(PoleTrickState));
    }
    READABLE_FUNCTION(savePoleTrick, Verified);

    // loadPoleTrick ("LoadFromBuffer").
    uint32_t loadPoleTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_poleTrickPool, sizeof(PoleTrickState));
    }
    READABLE_FUNCTION_WAITS(loadPoleTrick, Verified);

    ee::Ptr<PoleTrickEntry> findPoleTrick(uint32_t variant);
    uint32_t poleTrickText(ee::Ptr<GameObject> object);

    ee::Ptr<zge::Motion> poleAnimation(ee::Ptr<Player> player, ee::Ptr<GameObject> object);

    // poleTrickStarted (startPoleTrick2, "TrickStarted"): the variant from the skater's state,
    // the skater's first object slot emptied, the trick's text shown, worth
    // 100 banked, the animation for the skater's side and stance played.
    // Returns the animation's play.
    uint32_t poleTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<PoleTrickState> state(gameObjectState(note->object));
        switch (player->state)
        {
        case State::VerticalPole:
            state->variant = kVerticalPole;
            break;
        case State::VerticalPoleA:
            state->variant = kVerticalAirPole;
            break;
        case State::HorizontalPole:
            state->variant = kHorizontalPole;
            break;
        default:
            break;
        }
        emptySlot(player->objectSlots[0]);
        const uint32_t text = poleTrickText(note->object);
        if (text && zge::compareStrings(text, kEmpty) != 0)
            hud::showSpinText(player);
        const uint32_t variant = state->variant;
        if (variant == kVerticalAirPole || variant == kVerticalPole || variant == kHorizontalPole)
            state->worth = kPoleWorth;
        bankStartWorth(player, state->worth);
        const ee::Ptr<GameObject> object = note->object;
        const ee::Ptr<zge::Motion> animation = poleAnimation(player, object);
        if (!animation)
            return 0;
        const uint32_t played = zge::playAnimation(player->node, animation, kPoleMode, ee::Float(0.0f), kPlayOnce);
        return played;
    }
    READABLE_FUNCTION_WAITS(sym::startPoleTrick2, poleTrickStarted, Verified);

    // finishPoleTrick ("TrickFinished").
    uint32_t finishPoleTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(finishPoleTrick, Verified);

    // updatePoleTrick ("TrickUpdate"): the skates' trails started.
    uint32_t updatePoleTrick(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Ptr<Player>(ee::Ptr<TrickUpdate>(note->argument)->player)->startTrails = 1;
        return 1;
    }
    READABLE_FUNCTION(updatePoleTrick, Verified);

    // releasePoleButton ("ButtonReleased"): no longer held.
    uint32_t releasePoleButton(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Ptr<PoleTrickState>(gameObjectState(note->object))->holding = 0;
        return 1;
    }
    READABLE_FUNCTION(releasePoleButton, Verified);

    // forcePoleRelease ("ForcePoleRelease"): finished and let go at once; the
    // skater's state's object made current.
    uint32_t forcePoleRelease(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        PoleTrickState &state = *ee::Ptr<PoleTrickState>(gameObjectState(note->object));
        state.finished = 1;
        state.holding = 0;
        selectStateObject(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(forcePoleRelease, Verified);

    // endPoleTrickAnimation ("AnimationEnded"): once the button is let go, finished:
    // the skater lets go of the pole and its state's object is made current.
    uint32_t endPoleTrickAnimation(ee::Ptr<ObjectMessageNote> note)
    {
        PoleTrickState &state = *ee::Ptr<PoleTrickState>(gameObjectState(note->object));
        const ee::Ptr<AnimationEnd> ended(note->argument);
        if (!state.holding)
        {
            state.finished = 1;
            requestMove(ee::Ptr<Player>(ended->player), kMoveReleasePole);
            selectStateObject(ended->player);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(endPoleTrickAnimation, Verified);

    // dropPoleTrick ("TrickDroppedFromQueue").
    uint32_t dropPoleTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_poleTrickPool);
    }
    READABLE_FUNCTION(dropPoleTrick, Verified);

    // poleAnimation: the trick's animation for the skater (its side of the
    // pole, and fakie: the alley-oop ones).
    ee::Ptr<zge::Motion> poleAnimation(ee::Ptr<Player> player, ee::Ptr<GameObject> object)
    {
        const ee::Ptr<PoleTrickState> state(gameObjectState(object));
        const ee::Ptr<PoleTrickEntry> entry = findPoleTrick(state->variant);
        if (!entry)
            return 0;
        const uint8_t side = player->side;
        ee::Ptr<zge::Motion> animation;
        if (player->fakie)
            animation = side ? entry->leftAlleyOop.animation : entry->rightAlleyOop.animation;
        else
            animation = side ? entry->right.animation : entry->left.animation;
        return animation;
    }
    READABLE_FUNCTION(poleAnimation, Verified);

    // resolvePoleTricks: the table resolved (the class's setup): each trick's
    // four animations found by name (left, right, right alley-oop, left
    // alley-oop), its text from the string table. Returns the last find.
    uint32_t resolvePoleTricks()
    {
        ee::Local<uint8_t[0x170]> message;
        TableResolver resolve{message, kNotFoundFormat, kEmpty};
        for (PoleTrickEntry &entry : *g_poleTricks)
        {
            resolve.animation(entry.left.name, entry.left.animation);
            resolve.animation(entry.right.name, entry.right.animation);
            resolve.animation(entry.rightAlleyOop.name, entry.rightAlleyOop.animation);
            resolve.animation(entry.leftAlleyOop.name, entry.leftAlleyOop.animation);
            resolve.text(entry.textKey, entry.text);
        }
        return resolve.found;
    }
    READABLE_FUNCTION_WAITS(resolvePoleTricks, Verified);

    // findPoleTrick: the table's entry of a variant.
    ee::Ptr<PoleTrickEntry> findPoleTrick(uint32_t variant)
    {
        return findTrick(*g_poleTricks, variant);
    }
    READABLE_FUNCTION(findPoleTrick, Verified);

    // poleTrickText: the trick's text (in the module's buffer).
    uint32_t poleTrickText(ee::Ptr<GameObject> object)
    {
        return trickText<PoleTrickEntry>(object, kPoleTrickText, kFindPoleTrickAt);
    }
    READABLE_FUNCTION(poleTrickText, Verified);

    // setupPoleTricks: the class set up (setupObjectClasses' list): the state
    // blocks' pool (5 of 16 bytes), the class's own message, the class and
    // its handlers, the table.
    uint32_t setupPoleTricks()
    {
        *g_poleTrickPool = newPool(static_cast<uint32_t>(sizeof(PoleTrickState)), 5, 0);
        *g_msgForcePoleRelease = zge::nameHash(ee::Ptr<const char>(kForcePoleReleaseName));
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible, *g_msgAreYouFinished,
                                     *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer, *g_msgTrickStarted,
                                     *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgButtonReleased, *g_msgForcePoleRelease,
                                     *g_msgAnimationEnded, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::poleTrickScore, sym::poleMultiplierBonus, sym::poleTrickName, sym::poleTrickPossible, sym::poleTrickFinished, sym::poleTrickSaveSize, sym::savePoleTrick, sym::loadPoleTrick,
                                     sym::startPoleTrick2, sym::finishPoleTrick, sym::updatePoleTrick, sym::releasePoleButton, sym::forcePoleRelease, sym::endPoleTrickAnimation, sym::dropPoleTrick};
        return registerTrickClass(kObjectPoleTrick, messages, handlers, kResolvePoleTricksAt);
    }
    READABLE_FUNCTION_WAITS(setupPoleTricks, Verified);

    // shutdownPoleTricks: the class shut down.
    int32_t shutdownPoleTricks()
    {
        return unregisterTrickClass(g_poleTrickPool, kObjectPoleTrick);
    }
    READABLE_FUNCTION_WAITS(shutdownPoleTricks, Verified);

    // newPoleTrick: a new pole trick (startPoleTrick's), held.
    ee::Ptr<GameObject> newPoleTrick()
    {
        ee::Ptr<PoleTrickState> state;
        const ee::Ptr<GameObject> object = newTrickObject(g_poleTrickPool, kObjectPoleTrick, state);
        state->holding = 1;
        state->variant = kVerticalPole;
        state->worth = ee::Float::fromBits(0);
        state->finished = 0;
        return object;
    }
    READABLE_FUNCTION_WAITS(newPoleTrick, Verified);

    // poleTrickId: a pole trick's id (skaterSetup's list: the three variants).
    uint32_t poleTrickId(uint32_t variant)
    {
        return variant | trickId(kObjectPoleTrick);
    }
    READABLE_FUNCTION(poleTrickId, Verified);
}
