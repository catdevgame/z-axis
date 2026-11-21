// The vault (game objects of kind 7; were the sweep's drafts of vaultTrickScore ..
// vaultTrickId): the trick the Vault state makes (startVaultTrick,
// state_tricks.cpp), Vault or FakieVault by the skater's stance, worth 200
// on its start, and a point on the multiplier. It may be cut short only
// once its animation has ended. See action_tricks.h for the modules' shape.

#include "action_tricks.h"

#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "trick_objects.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/vault_trick.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {
        // A vault's state block (16 bytes, from its pool).
        struct VaultTrickState
        {
            uint32_t fakie;    // +0x00 the table's id: 0 Vault, 1 FakieVault
            uint32_t neverRead; // +0x04 cleared when made
            uint32_t finished; // +0x08 areYouFinished's answer: once its animation ends
            ee::Float worth;   // +0x0C 200
        };
        static_assert(sizeof(VaultTrickState) == 16 && offsetof(VaultTrickState, worth) == 0x0C);
        inline constexpr ee::Float kVaultWorth = 200.0f;

        inline ee::Global<uint32_t, sym::g_vaultTrickPool> g_vaultTrickPool;
        constexpr uint32_t kVaultTrickCount = 2;
        inline ee::Global<TrickEntry[kVaultTrickCount], sym::vaultTricks> g_vaultTricks;
        constexpr uint32_t kVaultTrickText = sym::vaultTrickText2;  // the text buffer (32 bytes)
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat5;  // "Could not find the '%s' animation."
        constexpr uint32_t kEmpty = sym::empty6;           // ""
        constexpr uint32_t kSomeVaultTrick = sym::someVaultTrick;  // "Some Vault Trick"
        constexpr uint32_t kVaultMode = 4;                 // playAnimation's mode for the vault

        // The module's functions, as their callers call them.
        constexpr uint32_t kVaultTrickAnimationAt = sym::vaultTrickAnimation;
        constexpr uint32_t kResolveVaultTricksAt = sym::resolveVaultTricks;
        constexpr uint32_t kFindVaultTrickAt = sym::findVaultTrick;
        constexpr uint32_t kVaultTrickTextAt = sym::vaultTrickText;
    }

    // The handlers (the class's message table).

    // vaultTrickScore ("getScore").
    uint32_t vaultTrickScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &VaultTrickState::worth);
    }
    READABLE_FUNCTION(vaultTrickScore, Verified);

    // vaultMultiplierBonus ("getMultiplierBonus"): one.
    uint32_t vaultMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 1);
    }
    READABLE_FUNCTION(vaultMultiplierBonus, Verified);

    // vaultTrickName ("getName").
    uint32_t vaultTrickName(ee::Ptr<ObjectMessageNote> note)
    {
        return answerName(note, kVaultTrickTextAt, kSomeVaultTrick);
    }
    READABLE_FUNCTION(vaultTrickName, Verified);

    // vaultTrickPossible ("areYouPossible"): in Vault.
    int32_t vaultTrickPossible(ee::Ptr<ObjectMessageNote> note)
    {
        return answerPossible(note, [](uint32_t state) { return state == static_cast<uint32_t>(State::Vault); });
    }
    READABLE_FUNCTION(vaultTrickPossible, Verified);

    // vaultTrickFinished ("areYouFinished").
    uint32_t vaultTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return answerFinished(note, &VaultTrickState::finished);
    }
    READABLE_FUNCTION(vaultTrickFinished, Verified);

    // vaultTrickSaveSize ("GetSpaceNeededForSaving").
    uint32_t vaultTrickSaveSize(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(VaultTrickState));
    }
    READABLE_FUNCTION(vaultTrickSaveSize, Verified);

    // saveVaultTrick ("SaveToBuffer").
    uint32_t saveVaultTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(VaultTrickState));
    }
    READABLE_FUNCTION(saveVaultTrick, Verified);

    // loadVaultTrick ("LoadFromBuffer").
    uint32_t loadVaultTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_vaultTrickPool, sizeof(VaultTrickState));
    }
    READABLE_FUNCTION_WAITS(loadVaultTrick, Verified);

    uint32_t vaultTrickText(ee::Ptr<GameObject> object);

    ee::Ptr<zge::Motion> vaultTrickAnimation(ee::Ptr<GameObject> object);

    // vaultTrickStarted (startVaultTrick2, "TrickStarted"): the variant from the skater's stance,
    // the trick's text shown, the worth banked, the animation played.
    // Returns the animation's play.
    uint32_t vaultTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<VaultTrickState> state(gameObjectState(note->object));
        if (player->fakie)
            state->fakie = 1;
        else
            state->fakie = 0;
        const uint32_t text = vaultTrickText(note->object);
        if (text && zge::compareStrings(text, kEmpty) != 0)
            hud::showSpinText(player);
        bankStartWorth(player, state->worth);
        const ee::Ptr<zge::Motion> animation = vaultTrickAnimation(note->object);
        if (!animation)
            return 0;
        const uint32_t played = zge::playAnimation(player->node, animation, kVaultMode, animationBlendTime(), kPlayOnce);
        return played;
    }
    READABLE_FUNCTION_WAITS(sym::startVaultTrick2, vaultTrickStarted, Verified);

    // finishVaultTrick ("TrickFinished").
    uint32_t finishVaultTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(finishVaultTrick, Verified);

    // updateVaultTrick ("TrickUpdate"): the skates' trails started.
    uint32_t updateVaultTrick(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Ptr<Player>(ee::Ptr<TrickUpdate>(note->argument)->player)->startTrails = 1;
        return 1;
    }
    READABLE_FUNCTION(updateVaultTrick, Verified);

    // endVaultTrickAnimation ("AnimationEnded"): finished; the skater's state's
    // object made current.
    uint32_t endVaultTrickAnimation(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<VaultTrickState> state(gameObjectState(note->object));
        const ee::Ptr<Player> player = ee::Ptr<AnimationEnd>(note->argument)->player;
        state->finished = 1;
        selectStateObject(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(endVaultTrickAnimation, Verified);

    // dropVaultTrick ("TrickDroppedFromQueue").
    uint32_t dropVaultTrick(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_vaultTrickPool);
    }
    READABLE_FUNCTION(dropVaultTrick, Verified);

    // vaultTrickAnimation: the trick's animation.
    ee::Ptr<zge::Motion> vaultTrickAnimation(ee::Ptr<GameObject> object)
    {
        return trickAnimation(object, kFindVaultTrickAt);
    }
    READABLE_FUNCTION(vaultTrickAnimation, Verified);

    // resolveVaultTricks: the table resolved (the class's setup).
    uint32_t resolveVaultTricks()
    {
        return resolveTricks(*g_vaultTricks, kNotFoundFormat, kEmpty);
    }
    READABLE_FUNCTION_WAITS(resolveVaultTricks, Verified);

    // findVaultTrick: the table's entry of a variant.
    ee::Ptr<TrickEntry> findVaultTrick(uint32_t variant)
    {
        return findTrick(*g_vaultTricks, variant);
    }
    READABLE_FUNCTION(findVaultTrick, Verified);

    // vaultTrickText: the trick's text (in the module's buffer).
    uint32_t vaultTrickText(ee::Ptr<GameObject> object)
    {
        return trickText<TrickEntry>(object, kVaultTrickText, kFindVaultTrickAt);
    }
    READABLE_FUNCTION(vaultTrickText, Verified);

    // setupVaultTricks: the class set up (setupObjectClasses' list): the state
    // blocks' pool (3 of 16 bytes), the class and its handlers, the table.
    uint32_t setupVaultTricks()
    {
        *g_vaultTrickPool = newPool(static_cast<uint32_t>(sizeof(VaultTrickState)), 3, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible, *g_msgAreYouFinished,
                                     *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer, *g_msgTrickStarted,
                                     *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgAnimationEnded, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::vaultTrickScore, sym::vaultMultiplierBonus, sym::vaultTrickName, sym::vaultTrickPossible, sym::vaultTrickFinished, sym::vaultTrickSaveSize, sym::saveVaultTrick,
                                     sym::loadVaultTrick, sym::startVaultTrick2, sym::finishVaultTrick, sym::updateVaultTrick, sym::endVaultTrickAnimation, sym::dropVaultTrick};
        return registerTrickClass(kObjectVaultTrick, messages, handlers, kResolveVaultTricksAt);
    }
    READABLE_FUNCTION_WAITS(setupVaultTricks, Verified);

    // shutdownVaultTricks: the class shut down.
    int32_t shutdownVaultTricks()
    {
        return unregisterTrickClass(g_vaultTrickPool, kObjectVaultTrick);
    }
    READABLE_FUNCTION_WAITS(shutdownVaultTricks, Verified);

    // confirmVaultTrick (player): the vault's answer while it is the skater's
    // current object (currentVaultObjectValue asks it then; neither has a caller): 1.
    uint32_t confirmVaultTrick(ee::Ptr<Player> player)
    {
        skaterCurrentObject(player);
        return 1;
    }
    READABLE_FUNCTION(confirmVaultTrick, Verified);

    // newVaultTrick: a new vault (startVaultTrick's), worth 200.
    ee::Ptr<GameObject> newVaultTrick()
    {
        ee::Ptr<VaultTrickState> state;
        const ee::Ptr<GameObject> object = newTrickObject(g_vaultTrickPool, kObjectVaultTrick, state);
        state->neverRead = 0;
        state->worth = kVaultWorth;
        state->fakie = 0;
        state->finished = 0;
        return object;
    }
    READABLE_FUNCTION_WAITS(newVaultTrick, Verified);

    // vaultTrickId: a vault's trick id (makeSkaterTrickObjects': Vault,
    // FakieVault).
    uint32_t vaultTrickId(uint32_t variant)
    {
        return trickId(kObjectVaultTrick) | variant;
    }
    READABLE_FUNCTION(vaultTrickId, Verified);
}
