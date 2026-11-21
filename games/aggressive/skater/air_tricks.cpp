// The air tricks (game objects of kind 3, aggressive/objects.h): a grab, a
// flip, a grab with a flip (the "combo" tricks) or a rider's special trick.
// The creators the trick table calls (g_airTricks, the buttons of Tom
// Hayes' FAQ section 5) each make a new air trick (newAirTrick) and name it
// in its state: the grab and the flip, and what it is worth from the
// tuning table (Scoring: Grabs, AirTricks, ComboTricks, whose entry names
// these follow): a grab earns its "Init. Worth" and then its "Worth/sec"
// while held; a flip its "Worth".
//
// The class (gamehand13; were the sweep's drafts of airTrickGetScore .. airTrickIsGrab):
// its handlers, the three tables (the grabs' animations, their "Extreme"
// tweak animations and texts; the flips' animations and texts; the combos'
// animations, four grabs by nine flips) resolved at its setup, the trick's
// text ("Rocket Grab", "Tweaked Method FrontFlip", a special in gold), and
// the ids makeSkaterTrickObjects keeps. On its start a McTwist in plain air
// is a Berani flip, a special plays its sound. Held past half a second a
// grab blends its tweak animation in over a quarter second; fully tweaked
// it adds a point to the line's multiplier and its own bonus, shows the
// spin text and plays the tweak's sound.
//
// The creators keep the sweep's status (most are Drafts: paths1's
// cov-airtricks routes reached them and found every call identical;
// promoting them is the tools round's).
//

#include "action_tricks.h"

#include "aggressive/objects.h"
#include "aggressive/tuning.h"
#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "trick_objects.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"

#include <cstddef>
#include "sym/aggressive/skater/air_tricks.cpp.h"

namespace aggressive::skater
{
    using namespace action_tricks;

    namespace
    {
        // A trick's grab and flip (AirTrick::grab, flip; the tables' ids).
        enum Grab : int32_t
        {
            kNoGrab = 0,
            kUnnamedGrab = 1, // newUnnamedGrab's: a named grab let go (no worth; finished)
            kRocket = 2,
            kLongDick = 3,
            kStale = 4,
            kLookBackJudo = 5,
            kAbstract = 6,
            kParallelThat = 7,
            kMethod = 8,
            kLuiKang = 9,
        };
        constexpr uint32_t kGrabCount = 10;
        enum Flip : int32_t
        {
            kNoFlip = 0,
            kUnnamedFlip = 1, // newUnnamedFlip's: the jump (no worth; finished)
            kFrontFlip = 2,
            kBackFlip = 3,
            kHotFlip = 4,
            kFlatspinClockwise = 5,
            kFlatspinCounterClockwise = 6,
            kMistyFlip = 7,
            kMcTwistFlip = 8, // in plain air (State::Air) it starts as the Berani flip
            kBeraniFlip = 9,
            kBioFlip = 10,
            // The riders' special tricks (Scoring: AirTricks' entries 8 .. 14).
            kSupermanLateFlip = 11,      // CE
            kDoubleBackFlip = 12,        // ML
            kAbstractDoubleBackFlip = 13, // TK
            kBio900 = 14,                // JG
            kCorkscrew1260 = 15,         // SB
            kDoubleFlatspin = 16,        // SY
            kSpin1440 = 17,              // EY
        };
        constexpr uint32_t kFlipCount = 18;
        constexpr bool isSpecial(uint32_t flip)
        {
            return flip >= kSupermanLateFlip && flip < kFlipCount;
        }

        // An air trick's state block (from its pool; 0x28 bytes).
        struct AirTrick
        {
            int32_t grab;             // +0x00 Grab
            int32_t flip;             // +0x04 Flip
            ee::Float worth;          // +0x08 the points so far (a grab's grow while held)
            uint32_t multiplierBonus; // +0x0C getMultiplierBonus's: one when the grab is fully tweaked
            ee::Float worthPerSecond; // +0x10 a grab's, while held
            uint32_t finished;        // +0x14 scores no more (its hold over, its animation ended, or a trick of no name)
            // +0x18 1 from a grab's maker: the grab holds its pose
            // (TrickStoppedHolding steps its animation back while set and
            // finishes the trick once clear). Cleared when its button is
            // let go (ButtonReleased), or when an object waits in the action
            // slot (TrickUpdate takes that one out and releases it).
            uint32_t holding;
            uint32_t earnsWhileHeld;  // +0x1C 1 for grabs and combos: TrickUpdate scores and tweaks
            ee::Float holdTime;       // +0x20 the animation's time at TrickStartHolding, stepped back as it holds
            // +0x24 seconds the grab has been held toward its tweak (from
            // 0.5 to 0.75 s the tweak animation blends in; below 0 it starts
            // again from 0 unless holding).
            ee::Float tweakTime;
        };
        static_assert(sizeof(AirTrick) == 0x28 && offsetof(AirTrick, finished) == 0x14 && offsetof(AirTrick, tweakTime) == 0x24);

        // The tables (resolved at the class's setup).
        struct GrabEntry // 0x38 bytes, one per Grab
        {
            uint32_t id;             // +0x00 Grab
            uint32_t animationName;  // +0x04 "global_assets.cedwards.RocketGrab"
            ee::Ptr<zge::Motion> animation;      // +0x08
            uint32_t tweakName;      // +0x0C "global_assets.cedwards.RocketGrabExtreme"
            ee::Ptr<zge::Motion> tweakAnimation; // +0x10 blended in as the grab is tweaked
            uint32_t textKey;        // +0x14 "Rocket"
            char text[0x20];         // +0x18
        };
        static_assert(sizeof(GrabEntry) == 0x38 && offsetof(GrabEntry, text) == 0x18);
        struct FlipEntry // 0x30 bytes, one per Flip
        {
            uint32_t id;            // +0x00 Flip
            uint32_t animationName; // +0x04 "global_assets.cedwards.FrontFlip" (the unnamed flip's: "jump")
            ee::Ptr<zge::Motion> animation;     // +0x08
            uint32_t textKey;       // +0x0C "FrontFlip"
            char text[0x20];        // +0x10
        };
        static_assert(sizeof(FlipEntry) == 0x30 && offsetof(FlipEntry, text) == 0x10);
        struct ComboEntry // 0x10 bytes: Rocket, Stale, Abstract, Method by the nine flips FrontFlip .. BioFlip
        {
            int32_t flip;           // +0x00
            int32_t grab;           // +0x04
            uint32_t animationName; // +0x08 "global_assets.cedwards.RocketFrontFlipCombo"
            ee::Ptr<zge::Motion> animation;     // +0x0C
        };
        static_assert(sizeof(ComboEntry) == 0x10);
        constexpr uint32_t kComboFlips = 9;
        constexpr uint32_t kComboCount = 36;
        inline ee::Global<GrabEntry[kGrabCount], sym::grabs> g_grabs;
        inline ee::Global<FlipEntry[kFlipCount], sym::flips> g_flips;
        inline ee::Global<ComboEntry[kComboCount], sym::combos> g_combos;

        inline ee::Global<uint32_t, sym::g_airTrickPool> g_airTrickPool; // 5 blocks
        constexpr uint32_t kEmpty = sym::empty10;          // ""
        constexpr uint32_t kSomeAirTrick = sym::someAirTrick;   // "Some Air Trick"
        constexpr uint32_t kNotFoundFormat = sym::notFoundFormat9; // "Could not find the '%s' animation."
        constexpr uint32_t kFrameFlagName = sym::frameFlagName3;  // "FrameFlag"
        constexpr uint32_t kJumpFakieName = sym::jumpFakieName;  // "global_assets.cedwards.jumpFakie": the unnamed flip's, fakie
        constexpr uint32_t kAirName = sym::airName;        // "global_assets.cedwards.air": any trick's without one
        constexpr uint32_t kAirFakieName = sym::airFakieName;   // "global_assets.cedwards.airFakie"
        constexpr uint32_t kAirSpinName = sym::airSpinName;    // "global_assets.cedwards.AirSpin"
        // The air animation, found when a trick has none (looked up again
        // each time: g_airAnimationKnown is never set).
        inline ee::Global<uint32_t, sym::g_airAnimationKnown> g_airAnimationKnown;
        inline ee::Global<ee::Ptr<zge::Motion>, sym::g_airAnimation> g_airAnimation;
        // The three animations' name hashes, each made at its first use
        // (its flag starts at 1).
        inline ee::Global<uint32_t, sym::g_airHashNeeded> g_airHashNeeded;
        inline ee::Global<uint32_t, sym::g_airFakieHashNeeded> g_airFakieHashNeeded;
        inline ee::Global<uint32_t, sym::g_airSpinHashNeeded> g_airSpinHashNeeded;
        inline ee::Global<uint32_t, sym::g_airHash> g_airHash;
        inline ee::Global<uint32_t, sym::g_airFakieHash> g_airFakieHash;
        inline ee::Global<uint32_t, sym::g_airSpinHash> g_airSpinHash;

        // The trick's text (airTrickText) is made here (96 bytes), in its
        // colours: a tweak's prefix gold, a special's name gold, the line's
        // colour after either.
        constexpr uint32_t kAirTrickTextBuffer = sym::g_tweakText;
        constexpr uint32_t kAirTrickTextMax = 95;
        constexpr uint32_t kTweakedPrefix = sym::tweakedPrefix; // "#RGB{176 144 24}Tweaked "
        constexpr uint32_t kSpecialColour = sym::specialColour; // "#RGB{176 142 21}"
        constexpr uint32_t kLineColour = sym::lineColour2;    // " #RGB{83 130 186}"
        inline ee::Global<uint32_t, sym::g_grabWord> g_grabWord; // -> "Grab": a grab alone is "Rocket Grab"

        // The tweak: from half a second held to three quarters the tweak
        // animation's weight goes from 0 to 1.
        constexpr float kTweakStart = 0.5f;
        constexpr float kTweakSpan = 0.25f;
        // The weight for `time` (0 .. 1).
        inline ee::Float tweakWeight(ee::Float time)
        {
            const ee::Float scaled = (time - ee::Float(kTweakStart)) / ee::Float(kTweakSpan);
            if (!(scaled < ee::Float(1.0f)))
                return 1.0f;
            return ee::Float(0.0f) < scaled ? scaled : ee::Float(0.0f);
        }

        // The sounds (soundEvent's: the main bank's one-shots 91 and 103).
        constexpr uint32_t kSpecialSound = 78; // the grind combos' too
        constexpr uint32_t kTweakSound = 76;
        constexpr uint32_t kAirMode = 5;       // playAnimation's mode
        // The hold over: the animation time steps back a frame and a half.
        inline ee::Global<ee::Float, sym::g_halfFrameSeconds5> g_halfFrameSeconds; // 1 / 120
        inline ee::Global<ee::Float, sym::g_frameSeconds5> g_frameSeconds;     // 1 / 60

        // Scoring: Grabs: eight grabs' "Init. Worth", then their "Worth/sec".
        constexpr uint32_t kGrabWorthPerSecond = 8;
        // Scoring: ComboTricks: four grabs (Rocket, Stale, Abstract,
        // Method) by eight flips' "Init. Worth", then their "Worth/sec".
        constexpr uint32_t kComboWorthPerSecond = 32;
        constexpr uint32_t kFlipsPerGrab = 8;
        constexpr uint32_t kSpecialWorth = 8; // Scoring: AirTricks' first special

        // The tuning tables' order of the flips (FrontFlip, HotFlip,
        // FlatspinClockwise, Bio, BackFlip, MistyFlip,
        // FlatspinCounterClockwise, BeraniMcTwistFlip).
        constexpr uint32_t flipWorthEntry(Flip flip)
        {
            switch (flip)
            {
            case kFrontFlip: return 0;
            case kHotFlip: return 1;
            case kFlatspinClockwise: return 2;
            case kBioFlip: return 3;
            case kBackFlip: return 4;
            case kMistyFlip: return 5;
            case kFlatspinCounterClockwise: return 6;
            default: return 7; // kMcTwistFlip
            }
        }
        // The combo tricks' grab order (Rocket, Stale, Abstract, Method).
        constexpr uint32_t comboGrabRow(Grab grab) { return static_cast<uint32_t>(grab) / 2 - 1; }

        // A name's hash made once (airAnimationHash and its two copies).
        template <typename Needed, typename Hash>
        uint32_t cachedNameHash(Needed &needed, Hash &hash, uint32_t name)
        {
            if (*needed != 0)
            {
                *hash = zge::nameHash(ee::Ptr<const char>(name));
                *needed = 0;
            }
            return *hash;
        }

        ee::Ptr<AirTrick> airTrickOf(ee::Ptr<GameObject> object)
        {
            return ee::Ptr<AirTrick>(gameObjectState(object));
        }
    }

    // The handlers (the class's message table).

    // airTrickGetScore ("getScore").
    uint32_t airTrickGetScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerScore(note, &AirTrick::worth);
    }
    READABLE_FUNCTION(airTrickGetScore, Verified);

    // airTrickGetMultiplierBonus ("getMultiplierBonus"): its tweak's.
    uint32_t airTrickGetMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerStateWord(note, &AirTrick::multiplierBonus);
    }
    READABLE_FUNCTION(airTrickGetMultiplierBonus, Verified);

    // airTrickGetName ("getName").
    uint32_t airTrickGetName(ee::Ptr<ObjectMessageNote> note)
    {
        return answerName(note, sym::airTrickText, kSomeAirTrick);
    }
    READABLE_FUNCTION(airTrickGetName, Verified);

    // airTrickAreYouPossible ("areYouPossible", the player): not while down; the
    // unnamed grab only while the skater's current object is an air trick of
    // a named grab; the rest in the air states (Air, AirQuarterPipe,
    // AirSpine).
    uint32_t airTrickAreYouPossible(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<AirTrick> trick(gameObjectState(note->object));
        if (player->flags & kDown)
            return 0;
        const int32_t grab = trick->grab;
        if (grab == kUnnamedGrab)
        {
            uint32_t possible = 0;
            const ee::Ptr<GameObject> current = skaterCurrentObject(player);
            if (current == 0)
                return 0;
            const ee::Ptr<AirTrick> currentTrick(gameObjectState(current));
            if (gameObjectKind(current) != kObjectAirTrick)
                return 0;
            const int32_t currentGrab = currentTrick->grab;
            if (currentGrab != kNoGrab)
                possible = currentGrab != kUnnamedGrab ? 1 : 0;
            return possible;
        }
        const uint32_t state = static_cast<uint32_t>(player->state);
        const uint32_t inAir = state - static_cast<uint32_t>(State::Air) < 3 ? 1 : 0;
        return inAir;
    }
    READABLE_FUNCTION(airTrickAreYouPossible, Verified);

    // airTrickAreYouFinished ("areYouFinished").
    uint32_t airTrickAreYouFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return answerFinished(note, &AirTrick::finished);
    }
    READABLE_FUNCTION(airTrickAreYouFinished, Verified);

    uint32_t airTrickText(ee::Ptr<GameObject> object);

    ee::Ptr<zge::Motion> airTrickAnimation(ee::Ptr<GameObject> object);

    ee::Ptr<GrabEntry> findGrabEntry(uint32_t grab);
    ee::Ptr<FlipEntry> findFlipEntry(uint32_t flip);
    ee::Ptr<ComboEntry> findComboEntry(int32_t flip, int32_t grab);

    // airTrickOnTrickStarted ("TrickStarted", the player): a McTwist in plain air
    // made the Berani flip, a special's sound played; the trick's text
    // shown (unless empty), its worth banked, its animation played (the
    // unnamed flip's fakie jump; else the table's; else the air
    // animation). Returns the play.
    uint32_t airTrickOnTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Local<ee::Ptr<zge::Motion>> animation;
        *animation = 0;
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<AirTrick> trick = airTrickOf(note->object);
        const uint32_t flip = static_cast<uint32_t>(trick->flip);
        if (flip == kMcTwistFlip || flip == kBeraniFlip)
            trick->flip = player->state == State::Air ? kBeraniFlip : kMcTwistFlip;
        else if (isSpecial(flip))
            soundEvent(kSpecialSound, 0, 0, 0, 0);

        const uint32_t text = airTrickText(note->object);
        if (text && zge::compareStrings(text, kEmpty) != 0)
            hud::showSpinText(player);
        bankStartWorth(player, trick->worth);

        if (trick->flip == kUnnamedFlip && player->fakie)
            zge::findAnimation(kJumpFakieName, animation);
        else
            *animation = airTrickAnimation(note->object);
        if (!*animation)
        {
            if (*g_airAnimationKnown == 0)
                zge::findAnimation(kAirName, g_airAnimation.get());
            *animation = *g_airAnimation;
            if (!*animation)
                return 0;
        }
        return zge::playAnimation(player->node, *animation, kAirMode, animationBlendTime(), kPlayOnce);
    }
    READABLE_FUNCTION_WAITS(airTrickOnTrickStarted, Verified);

    // airTrickOnTrickFinished ("TrickFinished").
    uint32_t airTrickOnTrickFinished(ee::Ptr<ObjectMessageNote> note)
    {
        return finishTrick(note);
    }
    READABLE_FUNCTION_WAITS(airTrickOnTrickFinished, Verified);

    // airTrickSpaceNeededForSaving ("GetSpaceNeededForSaving").
    uint32_t airTrickSpaceNeededForSaving(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(AirTrick));
    }
    READABLE_FUNCTION(airTrickSpaceNeededForSaving, Verified);

    // airTrickSaveToBuffer ("SaveToBuffer").
    uint32_t airTrickSaveToBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return saveState(note, sizeof(AirTrick));
    }
    READABLE_FUNCTION(airTrickSaveToBuffer, Verified);

    // airTrickLoadFromBuffer ("LoadFromBuffer").
    uint32_t airTrickLoadFromBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        return loadState(note, *g_airTrickPool, sizeof(AirTrick));
    }
    READABLE_FUNCTION_WAITS(airTrickLoadFromBuffer, Verified);

    // airTrickOnTrickUpdate ("TrickUpdate", a TrickUpdate): for a grab or a combo
    // still scoring, the frame's worth (per second times the frame's
    // seconds) added to the trick's and to the line's pending score; a grab
    // alone tweaks (its tweak animation queued at its weight; reaching 1
    // the multiplier and the trick's bonus go up, the spin text and the
    // tweak's sound). While holding, an object waiting in the action slot
    // is taken out and released, and the hold ends.
    uint32_t airTrickOnTrickUpdate(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> update(note->argument);
        const ee::Ptr<Player> player(update->player);
        const ee::Ptr<AirTrick> trick = airTrickOf(note->object);
        if (!trick->earnsWhileHeld || trick->finished)
            return 1;
        const ee::Float zero = 0.0f;
        const ee::Float earned = trick->worthPerSecond * update->seconds;
        if (zero < earned)
        {
            trick->worth = trick->worth + earned;
            setPendingScore(player, storedPendingScore(player) + earned);
        }

        const uint32_t grab = static_cast<uint32_t>(trick->grab);
        if (trick->flip == kNoFlip && grab >= kRocket && grab < kGrabCount)
        {
            if (trick->tweakTime < zero)
            {
                if (!trick->holding)
                    trick->tweakTime = zero;
            }
            else
            {
                const ee::Float weight = tweakWeight(trick->tweakTime);
                zge::addPoseLayer(player->node, findGrabEntry(grab)->tweakAnimation, 0, zero, weight);
                trick->tweakTime = trick->tweakTime + update->seconds;
                if (weight < ee::Float(1.0f) && ee::Float(1.0f) <= tweakWeight(trick->tweakTime))
                {
                    // Fully tweaked, this frame.
                    setTrickMultiplier(player, static_cast<uint32_t>(trickMultiplier(player) + 1));
                    hud::showSpinText(player);
                    ++trick->multiplierBonus;
                    soundEvent(kTweakSound, 0, 0, 0, 0);
                }
            }
        }

        if (!trick->holding)
            return 1;
        const ee::Ptr<GameObject> waiting = firstInSlot(player->objectSlots[0]);
        if (waiting != 0)
        {
            gameObjectState(waiting); // read and not used
            releaseGameObject(takeFromSlot(player->objectSlots[0]));
            trick->holding = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(airTrickOnTrickUpdate, Verified);

    // airTrickOnButtonReleased ("ButtonReleased"): the hold ends.
    uint32_t airTrickOnButtonReleased(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Ptr<AirTrick>(gameObjectState(note->object))->holding = 0;
        return 1;
    }
    READABLE_FUNCTION(airTrickOnButtonReleased, Verified);

    // airTrickOnTrickStartHolding ("TrickStartHolding", {player, time}): the time kept.
    uint32_t airTrickOnTrickStartHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<AirTrick> trick(gameObjectState(note->object));
        trick->holdTime = holding->seconds;
        return 1;
    }
    READABLE_FUNCTION(airTrickOnTrickStartHolding, Verified);

    // airTrickOnTrickStoppedHolding ("TrickStoppedHolding", {player, time}): the trick
    // finished when it no longer holds; else the kept time steps back a
    // frame and a half (not below 0) and the skater's animation is put
    // there, so the pose holds.
    uint32_t airTrickOnTrickStoppedHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> holding(note->argument);
        const ee::Ptr<AirTrick> trick = airTrickOf(note->object);
        if (!trick->holding)
        {
            trick->finished = 1;
            return 1;
        }
        const ee::Float back = trick->holdTime - *g_halfFrameSeconds - *g_frameSeconds;
        trick->holdTime = back;
        if (back < ee::Float(0.0f))
            trick->holdTime = 0.0f;
        return zge::setAnimationTime(ee::Ptr<Player>(holding->player)->node, trick->holdTime);
    }
    READABLE_FUNCTION_WAITS(airTrickOnTrickStoppedHolding, Verified);

    // airTrickOnAnimationEnded ("AnimationEnded", an AnimationEnd): finished; a
    // "FrameFlag" event of 5 flips the skater's stance (the model's effects
    // released); the skater's state's object made current.
    uint32_t airTrickOnAnimationEnded(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<AirTrick> trick = airTrickOf(note->object);
        const ee::Ptr<AnimationEnd> ended(note->argument);
        trick->finished = 1;
        flipStanceAtEnd(ended, kFrameFlagName);
        selectStateObject(ended->player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(airTrickOnAnimationEnded, Verified);

    // airTrickOnTrickDroppedFromQueue ("TrickDroppedFromQueue").
    uint32_t airTrickOnTrickDroppedFromQueue(ee::Ptr<ObjectMessageNote> note)
    {
        return dropTrick(note, g_airTrickPool);
    }
    READABLE_FUNCTION(airTrickOnTrickDroppedFromQueue, Verified);

    // airTrickAnimation: the trick's animation: a grab with a flip the combo's
    // (0 when the table has none), a flip the flip's, a grab the grab's.
    ee::Ptr<zge::Motion> airTrickAnimation(ee::Ptr<GameObject> object)
    {
        const ee::Ptr<AirTrick> trick = airTrickOf(object);
        const int32_t grab = trick->grab;
        const int32_t flip = trick->flip;
        if (grab != kNoGrab && flip != kNoFlip)
        {
            const ee::Ptr<ComboEntry> combo = findComboEntry(flip, grab);
            return combo ? combo->animation : 0;
        }
        if (flip != kNoFlip)
        {
            const ee::Ptr<FlipEntry> entry = findFlipEntry(flip);
            return entry ? entry->animation : 0;
        }
        if (grab == kNoGrab)
            return 0;
        const ee::Ptr<GrabEntry> entry = findGrabEntry(grab);
        return entry ? entry->animation : 0;
    }
    READABLE_FUNCTION(airTrickAnimation, Verified);

    // resolveAirTricks (the class's setup): the three tables resolved: each
    // grab's animation and tweak animation, each flip's and each combo's
    // animation found by name ("Could not find the '%s' animation." made
    // when not, never shown), the grabs' and the flips' texts (the string
    // table's, else the key; an empty key or text left out). Returns the
    // last animation's find.
    uint32_t resolveAirTricks()
    {
        ee::Local<uint8_t[0x1A0]> message;
        TableResolver resolve{message, kNotFoundFormat, kEmpty};
        for (GrabEntry &grab : *g_grabs)
        {
            resolve.animation(grab.animationName, grab.animation);
            resolve.animation(grab.tweakName, grab.tweakAnimation);
            resolve.text(grab.textKey, grab.text);
        }
        for (FlipEntry &flip : *g_flips)
        {
            resolve.animation(flip.animationName, flip.animation);
            resolve.text(flip.textKey, flip.text);
        }
        for (ComboEntry &combo : *g_combos)
            resolve.animation(combo.animationName, combo.animation);
        return resolve.found;
    }
    READABLE_FUNCTION_WAITS(resolveAirTricks, Verified);

    // findFlipEntry: the flips' entry of `flip` (0: none).
    ee::Ptr<FlipEntry> findFlipEntry(uint32_t flip)
    {
        return findTrick(*g_flips, flip);
    }
    READABLE_FUNCTION(findFlipEntry, Verified);

    // findGrabEntry: the grabs' entry of `grab` (0: none).
    ee::Ptr<GrabEntry> findGrabEntry(uint32_t grab)
    {
        return findTrick(*g_grabs, grab);
    }
    READABLE_FUNCTION(findGrabEntry, Verified);

    // findComboEntry: the combos' entry of a flip and a grab (0: none); a row
    // of another grab is passed over whole.
    ee::Ptr<ComboEntry> findComboEntry(int32_t flip, int32_t grab)
    {
        ee::Ptr<ComboEntry> entry(g_combos.get()[0]);
        for (uint32_t looked = 0; looked < kComboCount;)
        {
            if (entry->grab != grab)
            {
                entry += kComboFlips;
                looked += kComboFlips;
            }
            else
            {
                if (entry->flip == flip)
                    return entry;
                ++entry;
                ++looked;
            }
        }
        return ee::Ptr<ComboEntry>();
    }
    READABLE_FUNCTION(findComboEntry, Verified);

    // airTrickText: the trick's text (in the module's buffer, 95 characters
    // at most): "#RGB{176 144 24}Tweaked " first once the grab is fully
    // tweaked; then the grab's text and the flip's ("Rocket FrontFlip"),
    // the grab's and "Grab" ("Rocket Grab"), or the flip's alone (a
    // special's in gold); the line's colour after a tweak or a special.
    uint32_t airTrickText(ee::Ptr<GameObject> object)
    {
        ee::Ptr<char> cursor(kAirTrickTextBuffer);
        uint32_t room = kAirTrickTextMax;
        bool coloured = false;
        *cursor = 0;
        const ee::Ptr<AirTrick> trick = airTrickOf(object);
        const ee::Float weight = tweakWeight(trick->tweakTime);

        const auto add = [&](ee::Address text, uint32_t length) {
            zge::stringCopyN(cursor, text, room);
            cursor += length;
            room -= length;
        };
        const auto append = [&](ee::Address text) { add(text, zge::stringLength(text)); };
        // A word and the space after it; false when it is empty or leaves no
        // room for more.
        const auto appendWord = [&](ee::Address text) {
            const uint32_t length = zge::stringLength(text);
            if (length == 0)
                return false;
            add(text, length);
            if (room == 0)
                return false;
            *cursor = ' ';
            ++cursor;
            --room;
            *cursor = 0;
            return true;
        };

        if (ee::Float(1.0f) <= weight)
        {
            coloured = true;
            append(kTweakedPrefix);
        }
        const int32_t grab = trick->grab;
        const int32_t flip = trick->flip;
        if (grab != kNoGrab)
        {
            const ee::Ptr<GrabEntry> grabEntry = findGrabEntry(grab);
            if (flip != kNoFlip)
            {
                const ee::Ptr<FlipEntry> flipEntry = findFlipEntry(flip);
                if (appendWord(grabEntry->text))
                    append(flipEntry->text);
            }
            else if (appendWord(grabEntry->text))
            {
                append(*g_grabWord);
            }
        }
        else if (flip != kNoFlip)
        {
            const ee::Ptr<FlipEntry> flipEntry = findFlipEntry(flip);
            if (isSpecial(static_cast<uint32_t>(flip)))
            {
                coloured = true;
                zge::stringCopyN(cursor, kSpecialColour, room);
                const uint32_t colourLength = zge::stringLength(kSpecialColour);
                cursor += colourLength;
                room -= colourLength;
            }
            append(flipEntry->text);
        }
        if (coloured)
            append(kLineColour);
        return kAirTrickTextBuffer;
    }
    READABLE_FUNCTION(airTrickText, Verified);

    // airAnimationHash, airFakieAnimationHash, airSpinAnimationHash: the name hashes of the
    // "air", "airFakie" and "AirSpin" animations (made at the first use).
    uint32_t airAnimationHash()
    {
        return cachedNameHash(g_airHashNeeded, g_airHash, kAirName);
    }
    READABLE_FUNCTION(airAnimationHash, Verified);

    uint32_t airFakieAnimationHash()
    {
        return cachedNameHash(g_airFakieHashNeeded, g_airFakieHash, kAirFakieName);
    }
    READABLE_FUNCTION(airFakieAnimationHash, Verified);

    uint32_t airSpinAnimationHash()
    {
        return cachedNameHash(g_airSpinHashNeeded, g_airSpinHash, kAirSpinName);
    }
    READABLE_FUNCTION(airSpinAnimationHash, Verified);

    // registerAirTrickClass (the skater's setup): the state blocks' pool (5 of 0x28
    // bytes), the class and its handlers, the tables. Returns the last
    // animation's find.
    uint32_t registerAirTrickClass()
    {
        *g_airTrickPool = newPool(static_cast<uint32_t>(sizeof(AirTrick)), 5, 0);
        const uint32_t messages[] = {*g_msgGetScore, *g_msgGetMultiplierBonus, *g_msgGetName, *g_msgAreYouPossible,
                                     *g_msgAreYouFinished, *g_msgGetSpaceNeededForSaving, *g_msgSaveToBuffer, *g_msgLoadFromBuffer,
                                     *g_msgTrickStarted, *g_msgTrickFinished, *g_msgTrickUpdate, *g_msgButtonReleased,
                                     *g_msgTrickStartHolding, *g_msgTrickStoppedHolding, *g_msgAnimationEnded, *g_msgTrickDroppedFromQueue};
        const uint32_t handlers[] = {sym::airTrickGetScore, sym::airTrickGetMultiplierBonus, sym::airTrickGetName, sym::airTrickAreYouPossible, sym::airTrickAreYouFinished, sym::airTrickSpaceNeededForSaving, sym::airTrickSaveToBuffer, sym::airTrickLoadFromBuffer,
                                     sym::airTrickOnTrickStarted, sym::airTrickOnTrickFinished, sym::airTrickOnTrickUpdate, sym::airTrickOnButtonReleased, sym::airTrickOnTrickStartHolding, sym::airTrickOnTrickStoppedHolding, sym::airTrickOnAnimationEnded, sym::airTrickOnTrickDroppedFromQueue};
        return registerTrickClass(kObjectAirTrick, messages, handlers, sym::resolveAirTricks);
    }
    READABLE_FUNCTION_WAITS(registerAirTrickClass, Verified);

    // unregisterAirTrickClass (the skater's shutdown).
    uint32_t unregisterAirTrickClass()
    {
        return static_cast<uint32_t>(unregisterTrickClass(g_airTrickPool, kObjectAirTrick));
    }
    READABLE_FUNCTION_WAITS(unregisterAirTrickClass, Verified);

    // The trick ids (makeSkaterTrickObjects' list): the kind, the grab
    // from bit 13, the flip below.
    constexpr uint32_t kGrabIdShift = 13;

    // airFlipTrickId: a flip's id (FrontFlip .. BioFlip in the list).
    uint32_t airFlipTrickId(uint32_t flip)
    {
        return trickId(kObjectAirTrick) | flip;
    }
    READABLE_FUNCTION(airFlipTrickId, Verified);

    // airGrabTrickId: a grab's id (Rocket .. LuiKang).
    uint32_t airGrabTrickId(uint32_t grab)
    {
        return grab << kGrabIdShift | trickId(kObjectAirTrick);
    }
    READABLE_FUNCTION(airGrabTrickId, Verified);

    // airComboTrickId: a grab's id and a flip's together (a combo's).
    uint32_t airComboTrickId(uint32_t grabId, uint32_t flipId)
    {
        return grabId | flipId;
    }
    READABLE_FUNCTION(airComboTrickId, Verified);

    // airTrickIsGrab: whether the object is an air trick of a named grab
    // (Rocket .. LuiKang; the release in the air follows one with an
    // unnamed grab, control_steps.cpp).
    uint32_t airTrickIsGrab(ee::Ptr<GameObject> object)
    {
        if (gameObjectKind(object) != kObjectAirTrick)
            return 0;
        const uint32_t grab = static_cast<uint32_t>(airTrickOf(object)->grab);
        const uint32_t named = grab < kGrabCount && grab >= kRocket ? 1 : 0;
        return named;
    }
    READABLE_FUNCTION(airTrickIsGrab, Verified);

    // The creators.

    ee::Ptr<GameObject> newAirTrick();

    namespace
    {
        // A new air trick named by `name` (its state filled in).
        template <typename Name>
        ee::Ptr<GameObject> createAirTrick(Name name)
        {
            const ee::Ptr<GameObject> object = newAirTrick();
            name(*airTrickOf(object));
            return object;
        }
        ee::Ptr<GameObject> newGrab(Grab grab)
        {
            return createAirTrick([&](AirTrick &trick) {
                const uint32_t entry = static_cast<uint32_t>(grab) - kRocket;
                trick.holding = 1;
                trick.grab = grab;
                trick.earnsWhileHeld = 1;
                trick.finished = 0;
                trick.worth = tuningValue(kTuningScoringGrabs, entry);
                trick.worthPerSecond = tuningValue(kTuningScoringGrabs, entry + kGrabWorthPerSecond);
            });
        }
        ee::Ptr<GameObject> newFlip(Flip flip, uint32_t worthEntry)
        {
            return createAirTrick([&](AirTrick &trick) {
                trick.finished = 0;
                trick.flip = flip;
                trick.worth = tuningValue(kTuningScoringAirTricks, worthEntry);
            });
        }
        ee::Ptr<GameObject> newFlip(Flip flip) { return newFlip(flip, flipWorthEntry(flip)); }
        ee::Ptr<GameObject> newSpecial(Flip special)
        {
            return newFlip(special, static_cast<uint32_t>(special) - kSupermanLateFlip + kSpecialWorth);
        }
        ee::Ptr<GameObject> newCombo(Grab grab, Flip flip)
        {
            return createAirTrick([&](AirTrick &trick) {
                const uint32_t entry = comboGrabRow(grab) * kFlipsPerGrab + flipWorthEntry(flip);
                trick.holding = 1;
                trick.flip = flip;
                trick.grab = grab;
                trick.earnsWhileHeld = 1;
                trick.finished = 0;
                trick.worth = tuningValue(kTuningScoringComboTricks, entry);
                trick.worthPerSecond = tuningValue(kTuningScoringComboTricks, entry + kComboWorthPerSecond);
            });
        }
        // The two tricks of no name: a grab or a flip worth nothing, finished.
        ee::Ptr<GameObject> newUnnamed(bool grab)
        {
            const ee::Ptr<GameObject> object = newAirTrick();
            if (!object)
                return 0;
            const ee::Ptr<AirTrick> trick = airTrickOf(object);
            if (!trick)
                return 0;
            trick->finished = 1;
            if (grab)
                trick->grab = kUnnamedGrab;
            else
                trick->flip = kUnnamedFlip;
            return object;
        }
    }

    // newAirTrick: a new air trick, nothing in it yet (finished), or 0.
    ee::Ptr<GameObject> newAirTrick()
    {
        const uint32_t state = poolTake(*g_airTrickPool);
        const ee::Ptr<GameObject> object = newGameObject(uint64_t{kObjectAirTrick}, state);
        AirTrick &trick = *ee::Ptr<AirTrick>(state);
        trick.tweakTime = 0.0f;
        trick.finished = 1;
        trick.grab = kNoGrab;
        trick.flip = kNoFlip;
        trick.worth = 0.0f;
        trick.multiplierBonus = 0;
        trick.worthPerSecond = 0.0f;
        trick.holdTime = 0.0f;
        trick.earnsWhileHeld = 0;
        trick.holding = 0;
        return object;
    }
    READABLE_FUNCTION_WAITS(newAirTrick, Verified);

    ee::Ptr<GameObject> newUnnamedFlip()
    {
        return newUnnamed(false);
    }
    READABLE_FUNCTION_WAITS(newUnnamedFlip, Verified);
    ee::Ptr<GameObject> newUnnamedGrab()
    {
        return newUnnamed(true);
    }
    READABLE_FUNCTION_WAITS(newUnnamedGrab, Verified);

    // The grabs.
    ee::Ptr<GameObject> newRocketGrab() { return newGrab(kRocket); }
    READABLE_FUNCTION_WAITS(newRocketGrab, Verified);
    ee::Ptr<GameObject> newLongDickGrab() { return newGrab(kLongDick); }
    READABLE_FUNCTION_WAITS(newLongDickGrab, Verified);
    ee::Ptr<GameObject> newStaleGrab() { return newGrab(kStale); }
    READABLE_FUNCTION_WAITS(newStaleGrab, Verified);
    ee::Ptr<GameObject> newLookBackJudoGrab() { return newGrab(kLookBackJudo); }
    READABLE_FUNCTION_WAITS(newLookBackJudoGrab, Verified);
    ee::Ptr<GameObject> newAbstractGrab() { return newGrab(kAbstract); }
    READABLE_FUNCTION_WAITS(newAbstractGrab, Verified);
    ee::Ptr<GameObject> newParallelThatGrab() { return newGrab(kParallelThat); }
    READABLE_FUNCTION_WAITS(newParallelThatGrab, Verified);
    ee::Ptr<GameObject> newMethodGrab() { return newGrab(kMethod); }
    READABLE_FUNCTION_WAITS(newMethodGrab, Verified);
    ee::Ptr<GameObject> newLuiKangGrab() { return newGrab(kLuiKang); }
    READABLE_FUNCTION_WAITS(newLuiKangGrab, Verified);

    // The flips. (triggers1 names newFlatspinClockwise and newFlatspinCounterClockwise the other way
    // round; the tuning entries they read name them as here.)
    ee::Ptr<GameObject> newFrontFlip() { return newFlip(kFrontFlip); }
    READABLE_FUNCTION_WAITS(newFrontFlip, Verified);
    ee::Ptr<GameObject> newHotFlip() { return newFlip(kHotFlip); }
    READABLE_FUNCTION_WAITS(newHotFlip, Verified);
    ee::Ptr<GameObject> newFlatspinClockwise() { return newFlip(kFlatspinClockwise); }
    READABLE_FUNCTION_WAITS(newFlatspinClockwise, Verified);
    ee::Ptr<GameObject> newBioFlip() { return newFlip(kBioFlip); }
    READABLE_FUNCTION_WAITS(newBioFlip, Verified);
    ee::Ptr<GameObject> newBackFlip() { return newFlip(kBackFlip); }
    READABLE_FUNCTION_WAITS(newBackFlip, Verified);
    ee::Ptr<GameObject> newMistyFlip() { return newFlip(kMistyFlip); }
    READABLE_FUNCTION_WAITS(newMistyFlip, Verified);
    ee::Ptr<GameObject> newFlatspinCounterClockwise() { return newFlip(kFlatspinCounterClockwise); }
    READABLE_FUNCTION_WAITS(newFlatspinCounterClockwise, Verified);
    ee::Ptr<GameObject> newMcTwistFlip() { return newFlip(kMcTwistFlip); }
    READABLE_FUNCTION_WAITS(newMcTwistFlip, Verified);

    // The riders' special tricks.
    ee::Ptr<GameObject> newSupermanLateFlip() { return newSpecial(kSupermanLateFlip); }
    READABLE_FUNCTION_WAITS(newSupermanLateFlip, Verified);
    ee::Ptr<GameObject> newDoubleBackFlip() { return newSpecial(kDoubleBackFlip); }
    READABLE_FUNCTION_WAITS(newDoubleBackFlip, Verified);
    ee::Ptr<GameObject> newAbstractDoubleBackFlip() { return newSpecial(kAbstractDoubleBackFlip); }
    READABLE_FUNCTION_WAITS(newAbstractDoubleBackFlip, Verified);
    ee::Ptr<GameObject> newBio900() { return newSpecial(kBio900); }
    READABLE_FUNCTION_WAITS(newBio900, Verified);
    ee::Ptr<GameObject> newCorkscrew1260() { return newSpecial(kCorkscrew1260); }
    READABLE_FUNCTION_WAITS(newCorkscrew1260, Verified);
    ee::Ptr<GameObject> newDoubleFlatspin() { return newSpecial(kDoubleFlatspin); }
    READABLE_FUNCTION_WAITS(newDoubleFlatspin, Verified);
    ee::Ptr<GameObject> newSpin1440() { return newSpecial(kSpin1440); }
    READABLE_FUNCTION_WAITS(newSpin1440, Verified);

    // The grabs with a flip.
    ee::Ptr<GameObject> newRocketFrontFlip() { return newCombo(kRocket, kFrontFlip); }
    READABLE_FUNCTION_WAITS(newRocketFrontFlip, Verified);
    ee::Ptr<GameObject> newRocketHotFlip() { return newCombo(kRocket, kHotFlip); }
    READABLE_FUNCTION_WAITS(newRocketHotFlip, Verified);
    ee::Ptr<GameObject> newRocketFlatspinClockwise() { return newCombo(kRocket, kFlatspinClockwise); }
    READABLE_FUNCTION_WAITS(newRocketFlatspinClockwise, Verified);
    ee::Ptr<GameObject> newRocketBioFlip() { return newCombo(kRocket, kBioFlip); }
    READABLE_FUNCTION_WAITS(newRocketBioFlip, Verified);
    ee::Ptr<GameObject> newRocketBackFlip() { return newCombo(kRocket, kBackFlip); }
    READABLE_FUNCTION_WAITS(newRocketBackFlip, Verified);
    ee::Ptr<GameObject> newRocketMistyFlip() { return newCombo(kRocket, kMistyFlip); }
    READABLE_FUNCTION_WAITS(newRocketMistyFlip, Verified);
    ee::Ptr<GameObject> newRocketFlatspinCounterClockwise() { return newCombo(kRocket, kFlatspinCounterClockwise); }
    READABLE_FUNCTION_WAITS(newRocketFlatspinCounterClockwise, Verified);
    ee::Ptr<GameObject> newRocketMcTwistFlip() { return newCombo(kRocket, kMcTwistFlip); }
    READABLE_FUNCTION_WAITS(newRocketMcTwistFlip, Verified);
    ee::Ptr<GameObject> newStaleFrontFlip() { return newCombo(kStale, kFrontFlip); }
    READABLE_FUNCTION_WAITS(newStaleFrontFlip, Verified);
    ee::Ptr<GameObject> newStaleHotFlip() { return newCombo(kStale, kHotFlip); }
    READABLE_FUNCTION_WAITS(newStaleHotFlip, Verified);
    ee::Ptr<GameObject> newStaleFlatspinClockwise() { return newCombo(kStale, kFlatspinClockwise); }
    READABLE_FUNCTION_WAITS(newStaleFlatspinClockwise, Verified);
    ee::Ptr<GameObject> newStaleBioFlip() { return newCombo(kStale, kBioFlip); }
    READABLE_FUNCTION_WAITS(newStaleBioFlip, Verified);
    ee::Ptr<GameObject> newStaleBackFlip() { return newCombo(kStale, kBackFlip); }
    READABLE_FUNCTION_WAITS(newStaleBackFlip, Verified);
    ee::Ptr<GameObject> newStaleMistyFlip() { return newCombo(kStale, kMistyFlip); }
    READABLE_FUNCTION_WAITS(newStaleMistyFlip, Verified);
    ee::Ptr<GameObject> newStaleFlatspinCounterClockwise() { return newCombo(kStale, kFlatspinCounterClockwise); }
    READABLE_FUNCTION_WAITS(newStaleFlatspinCounterClockwise, Verified);
    ee::Ptr<GameObject> newStaleMcTwistFlip() { return newCombo(kStale, kMcTwistFlip); }
    READABLE_FUNCTION_WAITS(newStaleMcTwistFlip, Verified);
    ee::Ptr<GameObject> newAbstractFrontFlip() { return newCombo(kAbstract, kFrontFlip); }
    READABLE_FUNCTION_WAITS(newAbstractFrontFlip, Verified);
    ee::Ptr<GameObject> newAbstractHotFlip() { return newCombo(kAbstract, kHotFlip); }
    READABLE_FUNCTION_WAITS(newAbstractHotFlip, Verified);
    ee::Ptr<GameObject> newAbstractFlatspinClockwise() { return newCombo(kAbstract, kFlatspinClockwise); }
    READABLE_FUNCTION_WAITS(newAbstractFlatspinClockwise, Verified);
    ee::Ptr<GameObject> newAbstractBioFlip() { return newCombo(kAbstract, kBioFlip); }
    READABLE_FUNCTION_WAITS(newAbstractBioFlip, Verified);
    ee::Ptr<GameObject> newAbstractBackFlip() { return newCombo(kAbstract, kBackFlip); }
    READABLE_FUNCTION_WAITS(newAbstractBackFlip, Verified);
    ee::Ptr<GameObject> newAbstractMistyFlip() { return newCombo(kAbstract, kMistyFlip); }
    READABLE_FUNCTION_WAITS(newAbstractMistyFlip, Verified);
    ee::Ptr<GameObject> newAbstractFlatspinCounterClockwise() { return newCombo(kAbstract, kFlatspinCounterClockwise); }
    READABLE_FUNCTION_WAITS(newAbstractFlatspinCounterClockwise, Verified);
    ee::Ptr<GameObject> newAbstractMcTwistFlip() { return newCombo(kAbstract, kMcTwistFlip); }
    READABLE_FUNCTION_WAITS(newAbstractMcTwistFlip, Verified);
    ee::Ptr<GameObject> newMethodFrontFlip() { return newCombo(kMethod, kFrontFlip); }
    READABLE_FUNCTION_WAITS(newMethodFrontFlip, Verified);
    ee::Ptr<GameObject> newMethodHotFlip() { return newCombo(kMethod, kHotFlip); }
    READABLE_FUNCTION_WAITS(newMethodHotFlip, Verified);
    ee::Ptr<GameObject> newMethodFlatspinClockwise() { return newCombo(kMethod, kFlatspinClockwise); }
    READABLE_FUNCTION_WAITS(newMethodFlatspinClockwise, Verified);
    ee::Ptr<GameObject> newMethodBioFlip() { return newCombo(kMethod, kBioFlip); }
    READABLE_FUNCTION_WAITS(newMethodBioFlip, Verified);
    ee::Ptr<GameObject> newMethodBackFlip() { return newCombo(kMethod, kBackFlip); }
    READABLE_FUNCTION_WAITS(newMethodBackFlip, Verified);
    ee::Ptr<GameObject> newMethodMistyFlip() { return newCombo(kMethod, kMistyFlip); }
    READABLE_FUNCTION_WAITS(newMethodMistyFlip, Verified);
    ee::Ptr<GameObject> newMethodFlatspinCounterClockwise() { return newCombo(kMethod, kFlatspinCounterClockwise); }
    READABLE_FUNCTION_WAITS(newMethodFlatspinCounterClockwise, Verified);
    ee::Ptr<GameObject> newMethodMcTwistFlip() { return newCombo(kMethod, kMcTwistFlip); }
    READABLE_FUNCTION_WAITS(newMethodMcTwistFlip, Verified);
}
