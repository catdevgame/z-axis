// The text objects (game objects of kind 5; gamehand13, were the sweep's
// drafts of textObjectGetScore .. newTextObject): a named bonus in the trick line that is
// no trick: newTextObject makes one with its text (the HUD's trick list
// shows it green, "#RGB{48 144 32}", the line's colour after it), a worth
// and a multiplier bonus. Three callers make them: the level scripts'
// addBonusTrickToTrickQueue (nativeAddBonusTrickToTrickQueue), the goals' tellGoalsOver
// (tellGoalsOver) and the line's " #RGB{154 28 33}Bail" (setPlayerComboState).
// Possible and finished at any time; on its start the worth
// goes to the line's pending score, the bonus to the multiplier and the
// text to the spin text; when finished it is added to the line and the
// spin text shown again. The module has the action tricks' shape
// (action_tricks.h); only its setup and shutdown have recordings.
//

#include "action_tricks.h"

#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/text_object.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {

        // A text object's state block (104 bytes, from its pool).
        struct TextObjectState
        {
            ee::Float worth;          // +0x00 added to the line's pending score on its start
            uint32_t multiplierBonus; // +0x04 added to the line's multiplier
            char text[0x60];          // +0x08 (95 characters kept)
        };
        static_assert(sizeof(TextObjectState) == 104 && offsetof(TextObjectState, text) == 8);
        constexpr uint32_t kTextObjectTextMax = sizeof(TextObjectState::text) - 1;

        inline ee::Global<uint32_t, sym::g_textObjectPool> g_textObjectPool; // 3 blocks
        constexpr uint32_t kEmpty = sym::empty4;      // ""
        constexpr uint32_t kTextColour = sym::textColour2; // "#RGB{48 144 32}"
        constexpr uint32_t kLineColour = sym::lineColour; // "#RGB{83 130 186}"
    }

    // The handlers (the class's message table).

    // textObjectGetScore ("getScore").
    uint32_t textObjectGetScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &TextObjectState::worth);
    }
    READABLE_FUNCTION(textObjectGetScore, Verified);

    // textObjectGetMultiplierBonus ("getMultiplierBonus").
    uint32_t textObjectGetMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerStateWord(note, &TextObjectState::multiplierBonus);
    }
    READABLE_FUNCTION(textObjectGetMultiplierBonus, Verified);

    // textObjectGetName ("getName"): the text in its colour, the line's colour
    // after it; whether it fitted the request. Empty (0) when it has none.
    uint32_t textObjectGetName(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<NameRequest> request(note->argument);
        const ee::Ptr<TextObjectState> state(gameObjectState(note->object));
        if (zge::compareStrings(kEmpty, state->text) != 0)
        {
            zge::stringCopy(request->out, kTextColour);
            zge::appendString(request->out, state->text);
            zge::appendString(request->out, kLineColour);
            return zge::stringLength(state->text) < request->size ? 1 : 0;
        }
        *ee::Ptr<char>(request->out) = 0;
        return 0;
    }
    READABLE_FUNCTION(textObjectGetName, Verified);

    // textObjectAreYouPossible ("areYouPossible"): always.
    uint32_t textObjectAreYouPossible()
    {
        return 1;
    }
    READABLE_FUNCTION(textObjectAreYouPossible, Verified);

    // textObjectAreYouFinished ("areYouFinished"): always.
    uint32_t textObjectAreYouFinished()
    {
        return 1;
    }
    READABLE_FUNCTION(textObjectAreYouFinished, Verified);

    // textObjectOnTrickStarted ("TrickStarted", the player): the worth onto the line's
    // pending score, the bonus onto its multiplier, the spin text shown
    // (unless the text is empty).
    uint32_t textObjectOnTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<TextObjectState> state(gameObjectState(note->object));
        if (ee::Float(0.0f) < state->worth)
        {
            const ee::Float pending = storedPendingScore(player);
            setPendingScore(player, pending + state->worth);
        }
        if (state->multiplierBonus != 0)
        {
            const int32_t multiplier = trickMultiplier(player);
            setTrickMultiplier(player, state->multiplierBonus + static_cast<uint32_t>(multiplier));
        }
        if (zge::compareStrings(kEmpty, state->text) != 0)
            hud::showSpinText(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(textObjectOnTrickStarted, Verified);

    // textObjectOnTrickFinished ("TrickFinished", the player): added to the line's, the
    // spin text shown.
    uint32_t textObjectOnTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument); // TrickFinished's argument: the player
        hud::addCompletedTrick(player, note->object);
        hud::showSpinText(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(textObjectOnTrickFinished, Verified);

    // textObjectOnTrickDroppedFromQueue ("TrickDroppedFromQueue").
    uint32_t textObjectOnTrickDroppedFromQueue(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_textObjectPool);
    }
    READABLE_FUNCTION(textObjectOnTrickDroppedFromQueue, Verified);

    // textObjectSpaceNeededForSaving ("GetSpaceNeededForSaving").
    uint32_t textObjectSpaceNeededForSaving(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(TextObjectState));
    }
    READABLE_FUNCTION(textObjectSpaceNeededForSaving, Verified);

    // textObjectSaveToBuffer ("SaveToBuffer").
    uint32_t textObjectSaveToBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(TextObjectState));
    }
    READABLE_FUNCTION(textObjectSaveToBuffer, Verified);

    // textObjectLoadFromBuffer ("LoadFromBuffer").
    uint32_t textObjectLoadFromBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_textObjectPool, sizeof(TextObjectState));
    }
    READABLE_FUNCTION_WAITS(textObjectLoadFromBuffer, Verified);

    // textObjectOnTrickUpdate ("TrickUpdate"): nothing.
    uint32_t textObjectOnTrickUpdate()
    {
        return 1;
    }
    READABLE_FUNCTION(textObjectOnTrickUpdate, Verified);

    // registerTextObjectClass (the skater's setup): the class registered with its pool
    // (three blocks) and handlers. Returns the last registration's result.
    uint32_t registerTextObjectClass()
    {
        *g_textObjectPool = newPool(static_cast<uint32_t>(sizeof(TextObjectState)), 3, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible,
                                     *g_msgAreYouFinished, *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer,
                                     *g_msgTrickStarted, *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::textObjectGetScore, sym::textObjectGetMultiplierBonus, sym::textObjectGetName, sym::textObjectAreYouPossible, sym::textObjectAreYouFinished, sym::textObjectSpaceNeededForSaving,
                                     sym::textObjectSaveToBuffer, sym::textObjectLoadFromBuffer, sym::textObjectOnTrickStarted, sym::textObjectOnTrickFinished, sym::textObjectOnTrickUpdate, sym::textObjectOnTrickDroppedFromQueue};
        return registerClassHandlers(kObjectText, messages, handlers);
    }
    READABLE_FUNCTION_WAITS(registerTextObjectClass, Verified);

    // unregisterTextObjectClass (the skater's shutdown).
    uint32_t unregisterTextObjectClass()
    {
        return static_cast<uint32_t>(unregisterTrickClass(g_textObjectPool, kObjectText));
    }
    READABLE_FUNCTION_WAITS(unregisterTextObjectClass, Verified);

    // newTextObject (text, multiplier bonus, worth): a new text
    // object, its text copied (95 characters at most); none (0) without a
    // text.
    ee::Ptr<GameObject> newTextObject(ee::Address text, uint32_t multiplierBonus, ee::Float worth)
    {
        ee::Ptr<TextObjectState> state;
        const ee::Ptr<GameObject> object = newTrickObject(g_textObjectPool, kObjectText, state);
        if (text == 0)
        {
            poolGive(*g_textObjectPool, state);
            releaseGameObject(object);
            return 0;
        }
        zge::stringCopyN(state->text, text, kTextObjectTextMax);
        state->multiplierBonus = multiplierBonus;
        state->worth = worth;
        return object;
    }
    READABLE_FUNCTION_WAITS(newTextObject, Verified);
}
