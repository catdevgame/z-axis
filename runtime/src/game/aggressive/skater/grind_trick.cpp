// The grind tricks' class (game objects of kind 2; gamehand12, were the
// sweep's drafts of grindTrickGetScore .. unregisterGrindTrickClass): the handlers of the tricks the
// trick table makes (trick_objects.cpp: the slides, the souls, the Double
// and the Hand Grind), their two animation tables and the class's setup.
// A slide's entry has its animations by facing (left, right), alley-oop
// (the trick's fakie) and the balance meter's lean (centre, left, right);
// a soul's by alley-oop and lean. A combine may make a hop into the trick
// first (FrontHop .. BackHop), the trick's own animation after it. The
// combos SF, FM and MS (types 21 .. 23) have their name in green and a
// sound of their own. The module has the action tricks' shape
// (action_tricks.h).
//

#include "action_tricks.h"
#include "trick_objects.h"

#include "aggressive/hud/hud.h"
#include "zge/scene/animator.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/skater/grind_trick.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    // An animation by name, found at the class's setup.
    struct AnimationSlot
    {
        uint32_t name;      // "global_assets.cedwards.soulGrind"
        ee::Ptr<zge::Motion> animation;
    };
    // A slide (the table slideEntries's 26, 0x88 bytes): twelve animations,
    // slideSlot's order.
    struct SlideEntry
    {
        uint32_t type;              // +0x00 the trick's type (TrickState::type)
        AnimationSlot slots[12];    // +0x04
        uint32_t textKey;           // +0x64 "FrontsideGrind"
        char text[0x20];            // +0x68
    };
    static_assert(sizeof(SlideEntry) == 0x88 && offsetof(SlideEntry, textKey) == 0x64 && offsetof(SlideEntry, text) == 0x68);
    // A soul (the table soulEntries's 26, 0x58 bytes): six animations.
    struct SoulEntry
    {
        uint32_t type;              // +0x00
        AnimationSlot slots[6];     // +0x04 soulSlot's order
        uint32_t textKey;           // +0x34 "SoulGrind"
        char text[0x20];            // +0x38
    };
    static_assert(sizeof(SoulEntry) == 0x58 && offsetof(SoulEntry, textKey) == 0x34 && offsetof(SoulEntry, text) == 0x38);

    namespace
    {

        constexpr uint32_t kGrindEntryCount = 26;
        inline ee::Global<SlideEntry[kGrindEntryCount], sym::slideEntries> g_slideEntries;
        inline ee::Global<SoulEntry[kGrindEntryCount], sym::soulEntries> g_soulEntries;

        // The balance meter's lean an animation is for.
        enum Lean : uint32_t
        {
            kLeanCentre = 0,
            kLeanLeft = 1,  // "balance.left."
            kLeanRight = 2, // "balance.right."
        };
        // A slide's slot: by lean, alley-oop and facing (0 left, 1 right).
        constexpr uint32_t slideSlot(uint32_t lean, uint32_t alleyOop, uint32_t facing)
        {
            return lean * 4 + alleyOop * 2 + facing;
        }
        // A soul's: the centre's normal and alley-oop, then the leans' normal,
        // then their alley-oop.
        constexpr uint32_t soulSlot(uint32_t lean, uint32_t alleyOop)
        {
            return lean == kLeanCentre ? alleyOop : 2 + alleyOop * 2 + (lean - 1);
        }
        ee::Ptr<zge::Motion> slideAnimation(ee::Ptr<SlideEntry> entry, uint32_t lean, uint32_t alleyOop, uint32_t facing)
        {
            return entry->slots[slideSlot(lean, alleyOop, facing)].animation;
        }
        ee::Ptr<zge::Motion> soulAnimation(ee::Ptr<SoulEntry> entry, uint32_t lean, uint32_t alleyOop)
        {
            return entry->slots[soulSlot(lean, alleyOop)].animation;
        }

        // TrickState::kind and ::stance (trick_objects.cpp's create functions).
        constexpr uint32_t kKindOther = 0, kKindSlide = 1, kKindSoul = 2;
        constexpr uint32_t kStanceSoul = 1, kStanceLeftFacing = 2, kStanceRightFacing = 3, kStanceSoulAlleyOop = 4;
        constexpr uint32_t kTrickMirrored = 0x8; // TrickState::flags: a soul's fakie, mirrored
        constexpr uint32_t kDoubleGrindType = 24;
        constexpr uint32_t kFirstComboType = 21, kLastComboType = 23; // SF, FM, MS

        inline ee::Global<uint32_t, sym::g_trickPool> g_trickPool; // 5 blocks of TrickState

        uint32_t typeOf(ee::Ptr<GameObject> object)
        {
            return ee::Ptr<TrickState>(gameObjectState(object))->type;
        }

        // The hops (a combine's way into the trick, TrickState::hop 1 .. 4).
        constexpr uint32_t kHopNames[4] = {sym::g_textGlobalAssetsCedwardsFrontHop, sym::g_textGlobalAssetsCedwardsLeftHop, sym::g_textGlobalAssetsCedwardsRightHop, sym::g_textGlobalAssetsCedwardsBackHop}; // "FrontHop" .. "BackHop"

        constexpr uint32_t kEmpty = sym::empty2;           // ""
        constexpr uint32_t kComboColour = sym::comboColour;     // "#RGB{48 144 32}" (green)
        constexpr uint32_t kTextColour = sym::textColour;      // "#RGB{83 130 186}" (the text's own)
        constexpr uint32_t kComboNameBuffer = sym::comboNameBuffer; // the combo's coloured name
        constexpr uint32_t kComboSound = 78;               // soundEvent's: the main bank's one-shot 91
        // "Could not find the '%s' animation.": each table has its own copy.
        constexpr uint32_t kSlideNotFoundFormat = sym::notFoundFormat2;
        constexpr uint32_t kSoulNotFoundFormat = ee::high(sym::notFoundFormat2) - 0x1E68;
        constexpr uint32_t kGrindMode = 5;                 // playAnimation's mode
        constexpr uint32_t kQueueMirrored = 8;             // queueNodeAnimation's flag
        inline constexpr ee::Float kHopBlendFactor = 2.5f;
        // The hold let go: the animation time steps back a frame and a half.
        inline ee::Global<ee::Float, sym::g_halfFrameSeconds3> g_halfFrameSeconds; // 1 / 120
        inline ee::Global<ee::Float, sym::g_frameSeconds3> g_frameSeconds;     // 1 / 60

        // Control / Min Grind (Time) Length (tuning category 5, entry 9).
        ee::Float minGrindLength()
        {
            return (*g_tuningCategories)[kTuningControl].entries[9].value;
        }
    }

    ee::Ptr<SoulEntry> grindTrickNameEntry(ee::Ptr<GameObject> object);
    ee::Ptr<SlideEntry> grindTrickEntry(ee::Ptr<GameObject> object);
    ee::Ptr<char> grindTrickName(ee::Ptr<GameObject> object);
    ee::Ptr<zge::Motion> grindTrickAnimation(ee::Ptr<GameObject> object, ee::Ptr<uint32_t> mirrored);
    ee::Ptr<zge::Motion> grindBalanceAnimation(ee::Ptr<TrickState> state, ee::Ptr<uint32_t> mirrored, ee::Float lean);
    void initGrindTrickTable();

    // The handlers (the class's message table).

    // grindTrickGetScore ("getScore"): what the trick has earned.
    uint32_t grindTrickGetScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &TrickState::worth);
    }
    READABLE_FUNCTION(grindTrickGetScore, Verified);

    // grindTrickGetMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t grindTrickGetMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(grindTrickGetMultiplierBonus, Verified);

    // grindTrickGetName ("getName"): the trick's name, empty (and not taken)
    // when it has none.
    uint32_t grindTrickGetName(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<NameRequest> request(note->argument);
        const ee::Ptr<char> name = grindTrickName(note->object);
        if (name)
        {
            if (zge::compareStrings(kEmpty, name) != 0)
            {
                zge::stringCopyN(request->out, name, request->size);
                return 1;
            }
        }
        *ee::Ptr<char>(request->out) = 0;
        return 0;
    }
    READABLE_FUNCTION(grindTrickGetName, Verified);

    // grindTrickAreYouPossible ("areYouPossible", the player): not while down.
    uint32_t grindTrickAreYouPossible(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        gameObjectState(note->object);
        return (player->flags & kDown) == 0 ? 1 : 0;
    }
    READABLE_FUNCTION(grindTrickAreYouPossible, Verified);

    // grindTrickAreYouFinished ("areYouFinished", the player): by the skater's state.
    // Grinding or stalling: once held, past its hop and longer than Control
    // / Min Grind (Time) Length; on the ground, in a manual, a cess slide
    // or a skitch: once held and past its hop; otherwise always.
    uint32_t grindTrickAreYouFinished(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<TrickState> state(gameObjectState(note->object));
        uint32_t finished = 1;
        switch (player->state)
        {
        case State::Grind:
        case State::HandGrind:
        case State::DoubleGrind:
        case State::Stall:
            finished = 0;
            if (state->held != 0 && state->hopDone != 0 && minGrindLength() < state->time)
                finished = 1;
            break;
        case State::Ground:
        case State::Stand:
        case State::Manual:
        case State::CessSlide:
        case State::Skitch:
            finished = 0;
            if (state->held != 0)
                finished = state->hopDone != 0 ? 1 : 0;
            break;
        default:
            break;
        }
        return finished;
    }
    READABLE_FUNCTION(grindTrickAreYouFinished, Verified);

    // grindTrickSpaceNeededForSaving ("GetSpaceNeededForSaving").
    uint32_t grindTrickSpaceNeededForSaving(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(TrickState));
    }
    READABLE_FUNCTION(grindTrickSpaceNeededForSaving, Verified);

    // grindTrickSaveToBuffer ("SaveToBuffer").
    uint32_t grindTrickSaveToBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(TrickState));
    }
    READABLE_FUNCTION(grindTrickSaveToBuffer, Verified);

    // grindTrickLoadFromBuffer ("LoadFromBuffer").
    uint32_t grindTrickLoadFromBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_trickPool, sizeof(TrickState));
    }
    READABLE_FUNCTION_WAITS(grindTrickLoadFromBuffer, Verified);

    // grindTrickOnTrickStarted ("TrickStarted", the player): the trick's table entry;
    // a combine's hop found to play first, else the hop done; riding fakie
    // ends (not in a Double Grind); the worth banked; a combo's sound; the
    // name shown (unless empty); the hop or the trick's own animation
    // played (after a hop, blended in over two and a half blend times).
    // Returns the play.
    uint32_t grindTrickOnTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Local<ee::Ptr<zge::Motion>> animation;
        ee::Local<uint32_t> mirrored;
        *animation = 0;
        const ee::Ptr<GameObject> object = note->object;
        *mirrored = 0;
        const ee::Ptr<TrickState> state(gameObjectState(object));
        const ee::Ptr<Player> player(note->argument);
        if (state->kind == kKindSlide)
        {
            state->slideEntry = grindTrickEntry(note->object);
        }
        else
        {
            state->soulEntry = grindTrickNameEntry(note->object);
        }
        const uint32_t hop = state->hop;
        state->time = 0.0f;
        if (hop != 0 && hop < 5)
        {
            zge::findAnimation(kHopNames[hop - 1], animation);
        }
        else
        {
            if (hop == 0)
                state->hopDone = 1;
        }
        const uint8_t fakie = player->fakie;
        if (fakie != 0 && state->type != kDoubleGrindType)
        {
            player->fakie = 0;
            player->drawnRefresh = 1;
        }
        bankStartWorth(player, state->worth);
        const uint32_t type = state->type;
        if (type >= kFirstComboType && type <= kLastComboType)
        {
            aggressive::soundEvent(kComboSound, 0, 0, 0, 0);
        }
        const ee::Ptr<char> name = grindTrickName(note->object);
        if (name && zge::compareStrings(kEmpty, name) != 0)
            hud::showSpinText(player);
        ee::Ptr<zge::Motion> toPlay = *animation;
        if (!toPlay)
        {
            toPlay = grindTrickAnimation(note->object, mirrored);
            *animation = toPlay;
            if (!toPlay)
                return 0;
        }
        const ee::Float blendTime = animationBlendTime();
        ee::Float blend;
        if (state->hop == 0)
            blend = 0.0f;
        else
            blend = blendTime * kHopBlendFactor;
        const uint32_t isMirrored = *mirrored;
        const uint32_t played =
            zge::playAnimation(player->node, toPlay, isMirrored != 0 ? kHalfMirrored : kGrindMode, blend, kPlayOnce);
        return played;
    }
    READABLE_FUNCTION_WAITS(grindTrickOnTrickStarted, Verified);

    // grindTrickOnTrickFinished ("TrickFinished").
    uint32_t grindTrickOnTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(grindTrickOnTrickFinished, Verified);

    // grindTrickOnTrickUpdate ("TrickUpdate"): the time in the trick; the frame's
    // worth (per second times the frame's seconds) added to the trick's and
    // to the line's pending score; the balance meter's lean animation
    // queued, as much as the lean.
    uint32_t grindTrickOnTrickUpdate(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Local<uint32_t> mirrored;
        *mirrored = 0;
        const ee::Ptr<TrickUpdate> update(note->argument);
        const ee::Ptr<Player> player(update->player);
        const ee::Ptr<TrickState> state(gameObjectState(note->object));
        const ee::Float seconds = update->seconds;
        const ee::Float earned = state->worthPerSecond * seconds;
        state->time = state->time + seconds;
        const ee::Float zero = 0.0f;
        if (ee::Float(0.0f) < earned)
        {
            state->worth = state->worth + earned;
            const ee::Float pending = storedPendingScore(player);
            setPendingScore(player, pending + earned);
        }
        const ee::Float lean = player->balance[0];
        const ee::Ptr<zge::Motion> animation = grindBalanceAnimation(state, mirrored, lean);
        const uint32_t isMirrored = *mirrored;
        if (animation == 0)
            return 1;
        uint32_t flags = kQueueMirrored;
        if (isMirrored == 0)
            flags = 0;
        const uint32_t queued = zge::addPoseLayer(player->node, animation, flags, zero, ee::abs(lean));
        return queued;
    }
    READABLE_FUNCTION_WAITS(grindTrickOnTrickUpdate, Verified);

    // grindTrickOnTrickStartHolding ("TrickStartHolding", {player, time}): held, the time kept.
    uint32_t grindTrickOnTrickStartHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<TrickState> state(gameObjectState(note->object));
        state->held = 1;
        state->holdTime = holding->seconds;
        return 1;
    }
    READABLE_FUNCTION(grindTrickOnTrickStartHolding, Verified);

    // grindTrickOnTrickStoppedHolding ("TrickStoppedHolding", {player, time}): the kept time
    // steps back a frame and a half (not below 0) and the skater's
    // animation is put there.
    uint32_t grindTrickOnTrickStoppedHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<TrickState> state(gameObjectState(note->object));
        const ee::Float back = state->holdTime - *g_halfFrameSeconds - *g_frameSeconds;
        state->holdTime = back;
        if (back < ee::Float(0.0f))
            state->holdTime = 0.0f;
        return zge::setAnimationTime(ee::Ptr<Player>(holding->player)->node, state->holdTime);
    }
    READABLE_FUNCTION_WAITS(grindTrickOnTrickStoppedHolding, Verified);

    // grindTrickOnAnimationEnded ("AnimationEnded", {player, ...}): after the hop, the
    // trick's own animation played; after that, the skater's state's object
    // made current.
    uint32_t grindTrickOnAnimationEnded(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Local<uint32_t> mirrored;
        const ee::Ptr<TrickState> state(gameObjectState(note->object));
        const ee::Ptr<AnimationEnd> ended(note->argument);
        uint32_t result = 0;
        if (state->hopDone != 0)
        {
            selectStateObject(ended->player);
            result = 1;
        }
        else
        {
            *mirrored = 0;
            state->hopDone = 1;
            const ee::Ptr<zge::Motion> animation = grindTrickAnimation(note->object, mirrored);
            if (animation != 0)
            {
                const uint32_t played = zge::playAnimation(ee::Ptr<Player>(ended->player)->node, animation,
                                                           *mirrored != 0 ? kHalfMirrored : kGrindMode, animationBlendTime(), kPlayOnce);
                return played;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(grindTrickOnAnimationEnded, Verified);

    // grindTrickOnTrickDroppedFromQueue ("TrickDroppedFromQueue").
    uint32_t grindTrickOnTrickDroppedFromQueue(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_trickPool);
    }
    READABLE_FUNCTION(grindTrickOnTrickDroppedFromQueue, Verified);

    // grindTrickNameEntry: the souls' table's entry of the object's type.
    ee::Ptr<SoulEntry> grindTrickNameEntry(ee::Ptr<GameObject> object)
    {
        return findTrick(*g_soulEntries, &SoulEntry::type, typeOf(object));
    }
    READABLE_FUNCTION(grindTrickNameEntry, Verified);

    // grindTrickEntry: the slides' table's entry of the object's type.
    ee::Ptr<SlideEntry> grindTrickEntry(ee::Ptr<GameObject> object)
    {
        return findTrick(*g_slideEntries, &SlideEntry::type, typeOf(object));
    }
    READABLE_FUNCTION(grindTrickEntry, Verified);

    // grindTrickAnimation: the trick's own animation (the centre lean's): a slide's
    // by its facing (stance 2 left, 3 right) and alley-oop; a soul's by
    // alley-oop, `mirrored` its fakie. 0 for none.
    ee::Ptr<zge::Motion> grindTrickAnimation(ee::Ptr<GameObject> object, ee::Ptr<uint32_t> mirrored)
    {
        *mirrored = 0;
        const ee::Ptr<TrickState> state(gameObjectState(object));
        const uint32_t kind = state->kind;
        ee::Ptr<zge::Motion> animation;
        if (kind == kKindSlide)
        {
            const ee::Ptr<SlideEntry> entry = grindTrickEntry(object);
            if (entry)
            {
                const uint32_t alleyOop = state->flags & kTrickFakie;
                if (alleyOop)
                {
                    const uint32_t stance = state->stance;
                    if (stance == kStanceLeftFacing)
                        animation = slideAnimation(entry, kLeanCentre, 1, 0);
                    else
                    {
                        if (stance == kStanceRightFacing)
                            animation = slideAnimation(entry, kLeanCentre, 1, 1);
                    }
                }
                else
                {
                    const uint32_t stance = state->stance;
                    if (stance == kStanceLeftFacing)
                        animation = slideAnimation(entry, kLeanCentre, 0, 0);
                    else
                    {
                        if (stance == kStanceRightFacing)
                            animation = slideAnimation(entry, kLeanCentre, 0, 1);
                    }
                }
            }
        }
        else if (kind == kKindOther || kind == kKindSoul)
        {
            const ee::Ptr<SoulEntry> entry = grindTrickNameEntry(object);
            if (entry)
            {
                *mirrored = state->flags & kTrickMirrored;
                animation = soulAnimation(entry, kLeanCentre, (state->flags & kTrickFakie) ? 1 : 0);
            }
        }
        else
            return 0;
        return animation;
    }
    READABLE_FUNCTION(grindTrickAnimation, Verified);

    // grindTrickName: the trick's name: a slide's or a soul's text; a combo
    // (SF, FM, MS) coloured green. 0 for none.
    ee::Ptr<char> grindTrickName(ee::Ptr<GameObject> object)
    {
        const uint32_t kind = ee::Ptr<TrickState>(gameObjectState(object))->kind;
        ee::Ptr<char> name;
        if (kind == kKindSlide)
        {
            const ee::Ptr<SlideEntry> entry = grindTrickEntry(object);
            if (entry)
                return entry->text;
        }
        else if (kind == kKindOther || kind == kKindSoul)
        {
            const ee::Ptr<SoulEntry> entry = grindTrickNameEntry(object);
            const ee::Ptr<TrickState> state(gameObjectState(object));
            if (entry)
            {
                const uint32_t type = state->type;
                const ee::Ptr<char> text = entry->text;
                if (type <= kLastComboType)
                {
                    name = text;
                    if (type >= kFirstComboType)
                    {
                        zge::stringCopy(kComboNameBuffer, kComboColour);
                        zge::appendString(kComboNameBuffer, text);
                        zge::appendString(kComboNameBuffer, kTextColour);
                        name = ee::Ptr<char>(kComboNameBuffer);
                    }
                }
                else
                    name = text;
            }
        }
        else
            return 0;
        return name;
    }
    READABLE_FUNCTION(grindTrickName, Verified);

    // grindBalanceAnimation (state, &mirrored, lean): the balance meter's animation
    // for the lean (below 0 the left's, above it the right's, 0 none): a
    // slide's by its facing and alley-oop; a soul's by alley-oop (stance 1
    // or 4), `mirrored` its fakie.
    ee::Ptr<zge::Motion> grindBalanceAnimation(ee::Ptr<TrickState> state, ee::Ptr<uint32_t> mirrored, ee::Float lean)
    {
        *mirrored = 0;
        const uint32_t kind = state->kind;
        const ee::Float zero = 0.0f;
        if (kind == kKindSlide)
        {
            const uint32_t stance = state->stance;
            const ee::Ptr<SlideEntry> entry = state->slideEntry;
            if (stance == kStanceLeftFacing)
            {
                if (state->flags & kTrickFakie)
                {
                    if (lean < zero)
                        return slideAnimation(entry, kLeanLeft, 1, 0);
                    if (zero < lean)
                        return slideAnimation(entry, kLeanRight, 1, 0);
                    return 0;
                }
                if (lean < zero)
                    return slideAnimation(entry, kLeanLeft, 0, 0);
                if (zero < lean)
                    return slideAnimation(entry, kLeanRight, 0, 0);
                return 0;
            }
            if (stance == kStanceRightFacing)
            {
                if (state->flags & kTrickFakie)
                {
                    if (lean < zero)
                        return slideAnimation(entry, kLeanLeft, 1, 1);
                    if (zero < lean)
                        return slideAnimation(entry, kLeanRight, 1, 1);
                    return 0;
                }
                if (lean < zero)
                    return slideAnimation(entry, kLeanLeft, 0, 1);
                if (zero < lean)
                    return slideAnimation(entry, kLeanRight, 0, 1);
                return 0;
            }
            return 0;
        }
        if (kind != kKindOther && kind != kKindSoul)
            return 0;
        const ee::Ptr<SoulEntry> entry = state->soulEntry;
        if (entry)
            *mirrored = state->flags & kTrickMirrored;
        const uint32_t stance = state->stance;
        if (stance == kStanceSoul)
        {
            if (lean < zero)
                return soulAnimation(entry, kLeanLeft, 0);
            if (zero < lean)
                return soulAnimation(entry, kLeanRight, 0);
            return 0;
        }
        if (stance == kStanceSoulAlleyOop)
        {
            if (lean < zero)
                return soulAnimation(entry, kLeanLeft, 1);
            if (zero < lean)
                return soulAnimation(entry, kLeanRight, 1);
            return 0;
        }
        return 0;
    }
    READABLE_FUNCTION(grindBalanceAnimation, Verified);

    // initGrindTrickTable (the class's setup): the souls' table resolved, then the
    // slides'.
    void initGrindTrickTable()
    {
        ee::Local<uint8_t[0x180]> message;
        TableResolver souls{message, kSoulNotFoundFormat, kEmpty};
        for (SoulEntry &entry : *g_soulEntries)
        {
            for (AnimationSlot &slot : entry.slots)
                souls.animation(slot.name, slot.animation);
            souls.text(entry.textKey, entry.text);
        }
        TableResolver slides{message, kSlideNotFoundFormat, kEmpty};
        for (SlideEntry &entry : *g_slideEntries)
        {
            for (AnimationSlot &slot : entry.slots)
                slides.animation(slot.name, slot.animation);
            slides.text(entry.textKey, entry.text);
        }
    }
    READABLE_FUNCTION_WAITS(initGrindTrickTable, Verified);

    // registerGrindTrickClass (the skater's setup): the class registered with its pool
    // (five state blocks) and handlers, then its tables resolved. Returns
    // the last registration's result.
    uint32_t registerGrindTrickClass()
    {
        *g_trickPool = newPool(static_cast<uint32_t>(sizeof(TrickState)), 5, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible,
                                     *g_msgAreYouFinished, *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer,
                                     *g_msgTrickStarted, *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgTrickStartHolding,
                                     *g_msgTrickStoppedHolding, *g_msgAnimationEnded, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::grindTrickGetScore, sym::grindTrickGetMultiplierBonus, sym::grindTrickGetName, sym::grindTrickAreYouPossible, sym::grindTrickAreYouFinished, sym::grindTrickSpaceNeededForSaving, sym::grindTrickSaveToBuffer, sym::grindTrickLoadFromBuffer,
                                     sym::grindTrickOnTrickStarted, sym::grindTrickOnTrickFinished, sym::grindTrickOnTrickUpdate, sym::grindTrickOnTrickStartHolding, sym::grindTrickOnTrickStoppedHolding, sym::grindTrickOnAnimationEnded, sym::grindTrickOnTrickDroppedFromQueue};
        aggressive::registerObjectClass(kObjectTrick);
        uint32_t result = 0;
        for (uint32_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
            result = aggressive::setClassHandler(uint64_t(kObjectTrick), messages[i], handlers[i]);
        initGrindTrickTable();
        return result;
    }
    READABLE_FUNCTION_WAITS(registerGrindTrickClass, Verified);

    // unregisterGrindTrickClass (the skater's shutdown).
    uint32_t unregisterGrindTrickClass()
    {
        return static_cast<uint32_t>(unregisterTrickClass(g_trickPool, kObjectTrick));
    }
    READABLE_FUNCTION_WAITS(unregisterGrindTrickClass, Verified);

    // grindTrickId: a grind trick's id (its type under the kind's five top
    // bits).
    uint32_t grindTrickId(uint32_t type)
    {
        return type | trickId(kObjectTrick);
    }
    READABLE_FUNCTION(grindTrickId, Verified);
}
