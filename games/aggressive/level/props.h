#pragma once

// The level's props (inlineFx.c): the pick-ups a locator of kind "prop box"
// places (level/zones.cpp), each with a trigger zone: juice, attributes,
// keys, locks, the career's icons and rescue animals, special tricks,
// collectibles and cards. initPropDefinitions hashes their definitions' names
// ("PropDef_PowerJuiceExtend" and the rest), which name the world objects
// they are drawn with, and the twister that turns around most of them.
//
// Field names past the offsets are inferred from what the code does.

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/props.h.h"

namespace aggressive::level
{
    // A prop's kind: the index of its definition's hash in g_propDefHashes.
    enum PropKind : uint32_t
    {
        kPropJuiceExtend = 0x00,
        kPropJuiceRefill = 0x01,
        kPropJuiceHalffill = 0x02,
        kPropSkateA = 0x03,
        kPropAttJump = 0x04,   // to kPropAttFakie: the attributes
        kPropAttSpeed = 0x05,
        kPropAttSpin = 0x06,
        kPropAttManual = 0x07,
        kPropAttGrind = 0x08,
        kPropAttWallRide = 0x09,
        kPropAttFakie = 0x0A,
        kPropKeyAirfield = 0x0B, // to kPropKeyZoo: the levels' keys
        kPropKeyBoardwalk = 0x0C,
        kPropKeyCannery = 0x0D,
        kPropKeyCivic = 0x0E,
        kPropKeyIndustrial = 0x0F,
        kPropKeyMovie = 0x10,
        kPropKeyMuseum = 0x11,
        kPropKeyZoo = 0x12,
        kPropLockAirfield = 0x13, // to 0x1A: the levels' locks
        kPropIconPeanut = 0x1B,   // to kPropIconArt: the career's icons
        kPropIconArt = 0x2D,
        kPropParrot = 0x2E,       // to kPropSloth: the rescue animals
        kPropMonkey = 0x2F,
        kPropSloth = 0x30,
        kPropUltraSkateA = 0x31,
        kPropShane = 0x32,        // to kPropTaig: the riders' special tricks
        kPropJaren = 0x33,
        kPropChris = 0x34,
        kPropFranky = 0x35,
        kPropMatt = 0x36,
        kPropSven = 0x37,
        kPropTaig = 0x38,
        // Past here only as a prop turns into one (applyLevelPropState):
        // no locator's kind.
        kPropEggSmall = 0x39,
        kPropEggBig = 0x3A,
        kPropBeaker = 0x3B,
        kPropDropBomb = 0x3C,
        kPropCandyCane = 0x3D,
        kPropDinoEgg = 0x3E,
        kPropGear = 0x3F,
        kPropBalloon = 0x40,
        kPropTunaHead = 0x41,
        kPropEggBig2 = 0x42,
        kPropCard = 0x43,         // to 0x4F: the playing cards, ace to king
        kPropTwisterC = 0x50,     // to kPropTwisterG: the twisters
        kPropTwisterM = 0x51,
        kPropTwisterB = 0x52,
        kPropTwisterO = 0x53,
        kPropTwisterG = 0x54,
        kPropDefinitions = 0x55,
    };

    inline bool isRescueAnimal(uint32_t kind) { return kind - kPropParrot < 3u; }

    struct Prop
    {
        ee::Ptr<zge::Node> node;     // +0x00 its model (0: none)
        // +0x04 a rescue animal's model object (0: none, or no animal): where
        // its near sound is measured from.
        ee::Ptr<zge::SceneObject> animal;
        ee::Ptr<zge::Node> turner;   // +0x08 its twister, turned every frame (0: none)
        uint32_t _0c;
        zge::Matrix rest;        // +0x10 the bobbing prop's frame at rest
        uint32_t id;             // +0x50 the locator's name
        uint32_t definition;     // +0x54 the locator's kind
        uint32_t kind;           // +0x58 what it is now
        uint32_t card;           // +0x5C the card drawn (drawRandomCard)
        uint32_t handler;        // +0x60 the object told "bonus_collect" (a name for findObject)
        uint32_t enabled;        // +0x64
        uint32_t rising;         // +0x68 bobbing up (y grows down)
        uint32_t taken;          // +0x6C gone once the player has it (else it stays)
        uint32_t collected;      // +0x70 counted in the career
        int8_t bob;              // +0x74 steps from rest, -24 .. 24
        uint8_t _75[0x03];
        ee::Float sinceSound;    // +0x78 seconds since the near sound
        ee::Ptr<Prop> next;      // +0x7C
    };
    static_assert(sizeof(Prop) == 0x80 && offsetof(Prop, id) == 0x50 && offsetof(Prop, kind) == 0x58 &&
                  offsetof(Prop, enabled) == 0x64 && offsetof(Prop, bob) == 0x74 && offsetof(Prop, next) == 0x7C);

    inline ee::Global<ee::Ptr<Prop>, sym::g_props> g_props;
    // The definitions' name hashes, by PropKind.
    constexpr ee::Ptr<uint32_t> kPropDefHashes(sym::g_propDefPowerJuiceExtendHash);

    inline uint32_t propDefHash(uint32_t kind) { return kPropDefHashes[kind]; }
}

namespace aggressive::level
{
    // (props.cpp)
    void animateProps(ee::Float dt);
    void applyLevelPropState();
    void cheatGiveLevelKeys();
    void freeProps();
    void initPropDefinitions();
    void loadLevelPropState(ee::Address saved);
    uint32_t newPropInstance(ee::Address locatorAt, uint32_t kind, uint32_t handler);
    void returnRescueAnimal(ee::Ptr<zge::SceneObject> object);
    ee::Ptr<uint8_t> saveLevelPropState(ee::Ptr<uint32_t> bytes);
    void setAnimalRescue(uint32_t rescue);
    int32_t setPropPowerUpState(uint32_t id, uint32_t state);
}
