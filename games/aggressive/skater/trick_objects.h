#pragma once

// Trick objects (round 11): what the trick table's create functions make
// (game objects of kind 2, aggressive/objects.h; their state block below)
// and the grind trick combiners. The stall's tricks (kind 10) have a state
// block of their own.

#include "skater.h"

#include "aggressive/objects.h"
#include "ee/ee.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Motion;
}

namespace aggressive::skater
{
    // A grind trick's state block (0x38 bytes; gameObjectState of the object).
    struct SlideEntry;
    struct SoulEntry;

    struct TrickState
    {
        uint32_t type;           // +0x00 the trick: 1 .. 25 as in Scoring: Grinds (1 Soul, 2 BackSlide ... 24 Double Grind, 25 Hand Grind), 0 the plain one
        uint32_t kind;           // +0x04 1 slides (flags 6, stance 2), 2 souls (flags 0x10, stance 1), 0 Double and Hand Grind
        uint32_t flags;          // +0x08 bit 0 fakie; 2 | 4 a slide's, 0x10 a soul's, 8 a soul's fakie (inferred)
        uint32_t stance;         // +0x0C the animation variant (1 .. 4)
        uint32_t side;           // +0x10 the side (0, 2, 3)
        uint32_t held;           // +0x14 1 once TrickStartHolding came (grind_trick.cpp)
        // +0x18 its entry of the slides' or the souls' table (TrickStarted's; grind_trick.cpp).
        union
        {
            ee::Ptr<SlideEntry> slideEntry;
            ee::Ptr<SoulEntry> soulEntry;
        };
        // +0x1C the hop a combine makes into the trick: 1 .. 4 FrontHop,
        // LeftHop, RightHop, BackHop (the combiners give the foot of the
        // stance it comes from, or a stance); 0 none.
        uint32_t hop;
        uint32_t switched;       // +0x20 1 when a combine switched it to the other foot
        // +0x24 1 once the trick's own animation is on (1 when made); 0 while
        // a combine's hop plays first.
        uint32_t hopDone;
        ee::Float worth;         // +0x28 the trick's Init. Worth, then what it has earned
        ee::Float time;          // +0x2C seconds in the trick (TrickUpdate's)
        ee::Float worthPerSecond; // +0x30 its Worth/sec
        ee::Float holdTime;      // +0x34 the animation's time, kept while held
    };
    static_assert(sizeof(TrickState) == 0x38 && offsetof(TrickState, hop) == 0x1C && offsetof(TrickState, hopDone) == 0x24 &&
                  offsetof(TrickState, worth) == 0x28 && offsetof(TrickState, worthPerSecond) == 0x30);

    // A stall trick's state block (0x18 bytes).
    // A stall (the table's five, 0x40 bytes).
    struct StallEntry
    {
        uint32_t type;                 // +0x00 StallTrickState::type
        uint32_t animationName;        // +0x04 "global_assets.cedwards.HandPlant"
        ee::Ptr<zge::Motion> animation;            // +0x08
        uint32_t forwardName;          // +0x0C "global_assets.cedwards.balance.forward.HandPlant"
        ee::Ptr<zge::Motion> forwardAnimation;     // +0x10 the meter leaning forward (below 0)
        uint32_t backwardName;         // +0x14 "global_assets.cedwards.balance.backward.HandPlant"
        ee::Ptr<zge::Motion> backwardAnimation;    // +0x18
        uint32_t textKey;              // +0x1C "HandPlant"
        char text[0x20];               // +0x20
    };
    static_assert(sizeof(StallEntry) == 0x40 && offsetof(StallEntry, textKey) == 0x1C && offsetof(StallEntry, text) == 0x20);

    struct StallTrickState
    {
        uint32_t type;           // +0x00 0 .. 4 as in Scoring: Stalls (HP, Abstract, Rocket, Method, Stale HP)
        ee::Float worth;         // +0x04 Init. Worth
        ee::Float worthPerSecond; // +0x08 Worth/sec
        uint32_t held;           // +0x0C 1 once TrickStartHolding came; then the trick may end (stall_trick.cpp)
        ee::Ptr<StallEntry> entry; // +0x10 its entry of the stall table (TrickStarted's)
        ee::Float holdTime;      // +0x14 the animation's time, kept while held
    };
    static_assert(sizeof(StallTrickState) == 0x18);

    constexpr uint32_t kTrickFakie = 0x1;

    // The match result's bits (matchTrick, tricks.cpp): bit 0 a button 1
    // after the pattern, bit 1 a button 5.
    constexpr uint32_t kMatchedSwitch = 0x1;
    constexpr uint32_t kMatchedFakie = 0x2;

    ee::Ptr<GameObject> newTrickObject();
    ee::Ptr<GameObject> newStallTrickObject();
    int32_t setTrickSoulFakie(ee::Ptr<GameObject> trick, int32_t fakie);
    int32_t setTrickFakie(ee::Ptr<GameObject> trick, int32_t fakie);     // (tricks.cpp)
}

namespace aggressive::skater
{
    // (air_tricks.cpp)
    uint32_t airAnimationHash();
    uint32_t airComboTrickId(uint32_t grabId, uint32_t flipId);
    uint32_t airFakieAnimationHash();
    uint32_t airFlipTrickId(uint32_t flip);
    uint32_t airGrabTrickId(uint32_t grab);
    uint32_t airSpinAnimationHash();
    uint32_t airTrickIsGrab(ee::Ptr<GameObject> object);
    ee::Ptr<GameObject> newUnnamedFlip();
    ee::Ptr<GameObject> newUnnamedGrab();
    uint32_t registerAirTrickClass();
    uint32_t unregisterAirTrickClass();
}

namespace aggressive::skater
{
    // (cess_slide.cpp)
    uint32_t cessSlideTrickId(uint32_t variant);
    ee::Ptr<GameObject> newCessSlide();
    uint32_t registerCessSlideClass();
    uint32_t unregisterCessSlideClass();
}

namespace aggressive::skater
{
    // (grind_trick.cpp)
    uint32_t grindTrickId(uint32_t type);
    uint32_t registerGrindTrickClass();
    uint32_t unregisterGrindTrickClass();
}

namespace aggressive::skater
{
    // (ground_object.cpp)
    ee::Ptr<GameObject> newGroundObject();
    ee::Ptr<GameObject> newGroundObjectForAir();
    uint32_t registerGroundObjectClass();
    uint32_t setObjectState(ee::Address player, ee::Ptr<GameObject> object, uint32_t state, uint32_t step);
    uint32_t unregisterGroundObjectClass();
}

namespace aggressive::skater
{
    // (manual_trick.cpp)
    uint32_t manualMatchesBalance(ee::Ptr<GameObject> object, ee::Float lean);
    uint32_t manualTrickId(uint32_t variant);
    uint32_t registerManualTrickClass();
    uint32_t unregisterManualTrickClass();
}

namespace aggressive::skater
{
    // (pole_trick.cpp)
    uint32_t poleTrickId(uint32_t variant);
    uint32_t setupPoleTricks();
    int32_t shutdownPoleTricks();
}

namespace aggressive::skater
{
    // (skitch_trick.cpp)
    uint32_t setupSkitch();
    int32_t shutdownSkitch();
    uint32_t skitchTrickId();
}

namespace aggressive::skater
{
    // (stall_trick.cpp)
    uint32_t registerStallTrickClass();
    uint32_t stallTrickId(uint32_t type);
    uint32_t unregisterStallTrickClass();
}

namespace aggressive::skater
{
    // (state_tricks.cpp)
    void scoreStateChange(ee::Ptr<Player> player, uint32_t from, uint32_t to);
    void selectStateObject(ee::Ptr<Player> player);
    int32_t startPoleTrick(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (text_object.cpp)
    uint32_t registerTextObjectClass();
    uint32_t unregisterTextObjectClass();
}

namespace aggressive::skater
{
    // (trick_objects.cpp)
    int32_t freeGameObjectClasses();
    ee::Ptr<GameObject> newHandPlant();
    ee::Ptr<GameObject> newPlainGrindTrick();
    int32_t skaterSetup();
}

namespace aggressive::skater
{
    // (tricks.cpp)
    ee::Ptr<GameObject> matchAirTrick(ee::Ptr<Player> player);
    ee::Ptr<GameObject> matchManualTrick(ee::Ptr<Player> player, ee::Float timeout);
    int32_t startTrick(ee::Ptr<Player> player, ee::Ptr<GameObject> trick);
    void trickButtonsUpdate(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (vault_trick.cpp)
    uint32_t confirmVaultTrick(ee::Ptr<Player> player);
    uint32_t setupVaultTricks();
    int32_t shutdownVaultTricks();
    uint32_t vaultTrickId(uint32_t variant);
}

namespace aggressive::skater
{
    // (wall_tricks.cpp)
    uint32_t setupWallTricks();
    int32_t shutdownWallTricks();
    uint32_t wallTrickId(uint32_t variant);
}

namespace aggressive::skater
{
    // (trick_objects.cpp: the skater's animation events to its trick objects)
    int32_t sendTrickStartHolding(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time);
    int32_t sendTrickStoppedHolding(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time);
    int32_t sendAnimationEnded(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time);
}
