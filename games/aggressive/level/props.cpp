// The level's props (inlineFx.c; level/props.h): the list newPropInstance
// builds from the level's prop locators, what each level's save keeps of
// them, and their frame. By its kind a prop turns about its up, bobs up and
// down, or sounds when a player comes near; its second node, if it has one,
// turns. What the kinds turn into depends on the game: in split screen the
// juice bottles become the level's collectibles or cards, and in the career
// a bottle counted once stays gone.

#include "aggressive/level/props.h"
#include "matrix_points.h"
#include "props.h"

#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/skater/skater.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/tuning.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/collision/collision.h"
#include "zge/scene/animation.h"
#include "sce/libgcc.h"
#include "aggressive/card_game.h"

#include <cstddef>
#include <cstring>
#include "sym/aggressive/level/props.cpp.h"

namespace aggressive::level
{
    using skater::Player;

    struct Prop;
    void swapPropModel(ee::Ptr<Prop> prop, uint32_t hash);
    void spawnRescueAnimal(ee::Ptr<Prop> prop, ee::Ptr<zge::Node> worldNode);
    uint32_t levelPropDefHash(uint32_t fallback);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_bobUp> g_bobUp;     // -0.02 a frame
        inline ee::Global<ee::Float, sym::g_bobDown> g_bobDown; // 0.02

        constexpr uint32_t kMemset = sym::memset;
        constexpr uint32_t kNodeMatrix = sym::nodeMatrix;      // (node, 1) -> its world matrix
        constexpr uint32_t kRotateY = sym::matrixRotateY;         // (out, in, angle) by a 16-bit angle (engine)
        constexpr uint32_t kCopyNodeMatrix = sym::setNodeLocalMatrix;  // (node, matrix): the matrix copied into the node
        constexpr uint32_t kSoundEvent = sym::soundEvent;      // (event, a, 0, 0, 0) (sound/events.cpp)

        constexpr uint32_t kTurnStep = 0x38E;              // 5 degrees a frame (65536 a turn)
        constexpr uint64_t kNearSquared = 0x4079000000000000ull; // 400.0: within 20
        constexpr uint32_t kNearSound = 53;
        constexpr int8_t kBobSteps = 24;

        // How a prop of each kind moves; the rest bob.
        enum class Way { Turn, Sound, Bob };
        Way wayOf(uint32_t kind)
        {
            const bool lock = kind >= kPropLockAirfield && kind < kPropIconPeanut;
            const bool specialTrick = kind >= kPropShane && kind <= kPropTaig;
            if (lock || specialTrick || kind == kPropCard)
                return Way::Turn;
            return isRescueAnimal(kind) ? Way::Sound : Way::Bob;
        }

        zge::Vec4 translationOf(const zge::Matrix &matrix)
        {
            zge::Vec4 translation;
            std::memcpy(&translation, matrix.m[3], sizeof(zge::Vec4));
            return translation;
        }
        void setTranslation(zge::Matrix &matrix, const zge::Vec4 &translation)
        {
            std::memcpy(matrix.m[3], &translation, sizeof(zge::Vec4));
        }

    }

    namespace
    {
        void memset(ee::Ptr<zge::Vec4> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // animateProps: the props' frame (dt the frame's time). Each enabled
    // prop by its kind: turned 5 degrees about its up in place; or bobbed
    // a step (0.02 along its rest frame's up, 24 steps each way, back at
    // rest when the steps reach 0); or (the parrot, the monkey) sound event 53
    // when a player (two in split screen) is within 20 of its sound node
    // and 7 seconds have passed since the last. Its second node turns 5
    // degrees too.
    //
    // The bob's step vector is zeroed once: each bob sets its y and turns it
    // by the rest frame in place, so the next bob starts from what the last
    // one left in its x, z and w.
    void animateProps(ee::Float dt)
    {
        ee::Local<zge::Matrix> turned;
        ee::Local<zge::Vec4> kept;
        ee::Local<zge::Vec4> step;
        ee::Local<zge::Vec4> toPlayer;
        memset(step, 0, sizeof(zge::Vec4));

        for (ee::Ptr<Prop> prop(*g_props); prop; prop = prop->next)
        {
            if (!prop->enabled)
                continue;
            switch (wayOf(prop->kind))
            {
            case Way::Turn:
                if (prop->node)
                {
                    // Turned in place: the world matrix itself, its translation kept.
                    const ee::Ptr<zge::Matrix> world = zge::nodeMatrix(prop->node, 1);
                    *kept = translationOf(*world);
                    zge::matrixRotateY(world, world, kTurnStep);
                    setTranslation(*world, *kept);
                }
                break;

            case Way::Sound:
            {
                if (!prop->animal || prop->kind == kPropSloth)
                    break;
                const uint32_t animal = prop->kind == kPropParrot ? 0 : 1;
                const ee::Ptr<zge::Vec4> at = zge::objectPosition(prop->animal);
                const int32_t players = *g_gameType == GameType::SplitScreen ? 2 : 1;
                bool sounded = false;
                for (int32_t i = 0; i < players; ++i)
                {
                    *toPlayer = *at - ee::Ptr<Player>(parts::skaterHandle(i))->position;
                    const ee::Float squared = zge::squaredLength(*toPlayer);
                    if (!sounded && sce::libgcc::dpcmp(sce::libgcc::fptodp(squared), kNearSquared) < 0 && ee::Float(7.0f) < prop->sinceSound)
                    {
                        soundEvent(kNearSound, animal, 0, 0, 0);
                        sounded = true;
                        prop->sinceSound = 0.0f;
                    }
                }
                if (!sounded)
                    prop->sinceSound = prop->sinceSound + dt;
                break;
            }

            case Way::Bob:
            {
                if (!prop->node)
                    break;
                if (prop->rising)
                {
                    step->y = *g_bobUp;
                    prop->bob = static_cast<int8_t>(prop->bob + 1);
                    if (prop->bob == kBobSteps)
                        prop->rising = 0;
                }
                else
                {
                    step->y = *g_bobDown;
                    prop->bob = static_cast<int8_t>(prop->bob - 1);
                    if (prop->bob == -kBobSteps)
                        prop->rising = 1;
                }
                if (prop->bob == 0)
                {
                    zge::setNodeLocalMatrix(prop->node, &prop->rest);
                    break;
                }
                const ee::Ptr<zge::Matrix> world = zge::nodeMatrix(prop->node, 1);
                *step = zge::rotated(prop->rest, *step);
                setTranslation(*world, translationOf(*world) + *step);
                break;
            }
            }

            // The second node: its world matrix turned (translation kept),
            // set as its own, its frame worked out.
            if (prop->turner)
            {
                *turned = *zge::nodeMatrix(prop->turner, 1);
                *kept = translationOf(*turned);
                zge::matrixRotateY(turned, turned, kTurnStep);
                setTranslation(*turned, *kept);
                zge::setNodeLocalMatrix(prop->turner, turned);
                zge::updateMovedNode(prop->turner);
            }
        }
    }
    READABLE_FUNCTION_WAITS(animateProps, Verified);

    namespace
    {
        constexpr uint32_t kSendEngineMessage = sym::sendEngineMessage; // hooked by a game override: by address
        constexpr uint32_t kUpdatePose = sym::updatePose;

        // What each level keeps of its props: a byte per prop (PropState),
        // by level, made at the first save.
        inline ee::Global<ee::Ptr<uint8_t>[kProfileLevels], sym::levelPropStates> g_levelPropStates;
        inline ee::Global<int32_t, sym::g_propCount> g_propCount;
        inline ee::Global<int32_t, sym::g_propStateBytes> g_propStateBytes;
        enum PropState : uint8_t
        {
            kPropStateEnabled = 1,
            kPropStateRising = 2,
            kPropStateTaken = 4,
            kPropStateCollected = 8,
            kPropStateFlags = 0x0F,
        };
        // How many of the level's props count: its collectibles and extends
        // (the level's load hands them to the players' records).
        inline ee::Global<uint8_t, sym::g_levelCollectibles> g_levelCollectibles;
        inline ee::Global<uint8_t, sym::g_levelExtends> g_levelExtends;
        inline ee::Global<uint32_t, sym::g_animalRescue> g_animalRescue;

        // The game's state words.
        inline ee::Global<int32_t, sym::g_levelNumber> g_currentLevel;

        constexpr uint32_t kIdentityMatrix = sym::identityMatrix;   // the engine's identity matrix
        constexpr uint32_t kNamesHigh = ee::high(sym::parrotObjectName);        // the compiler's base for the strings below
        constexpr uint32_t kParrotObjectName = sym::parrotObjectName; // "AnimObj_Parrot"
        constexpr uint32_t kMonkeyObjectName = sym::monkeyObjectName; // "AnimObj_Monkey"
        constexpr uint32_t kSlothObjectName = sym::slothObjectName;  // "AnimObj_Sloth"
        constexpr uint32_t kParrotAnimation = sym::parrotAnimation;  // "parrot.animation"
        constexpr uint32_t kMonkeyAnimation = sym::monkeyAnimation;  // "monkey.animation"
        constexpr uint32_t kSlothAnimation = sym::slothAnimation;   // "sloth.animation"
        constexpr uint32_t kParrotStartName = sym::parrotStartName;  // "parrotStart"
        constexpr uint32_t kParrotEndName = sym::parrotEndName;    // "parrotEnd"
        constexpr uint32_t kTuningAnimationBlendTime = 2;   // kTuningControl's "Animation Blend Time"

        // A prop's rest frame is the locator's orientation and position.
        struct Locator
        {
            zge::Vec4 position;        // +0x00
            zge::Quaternion turn;      // +0x10
            uint8_t _20[4];
            uint32_t name;             // +0x24 its name's hash
        };
        static_assert(offsetof(Locator, name) == 0x24);

        struct PropDefName
        {
            uint32_t name;
            uint32_t kind;
            uint32_t returnsTo;
        };
        // The twister around a kind and where its case is.
        struct TwisterCase
        {
            uint32_t leader;
            uint32_t kind;       // 0: none
            uint32_t returnsTo;  // the find's return address
        };
        TwisterCase twisterCase(uint32_t kind)
        {
            namespace here = points::newPropInstance;
            if (kind == kPropJuiceExtend)
                return {here::next120, kPropTwisterO, here::afterFindWorldObjectByHash2};
            if (kind == kPropJuiceRefill)
                return {here::notTaken11, kPropTwisterM, here::afterFindWorldObjectByHash3};
            if (kind == kPropJuiceHalffill)
                return {here::notTaken12, kPropTwisterG, here::afterFindWorldObjectByHash3};
            if (kind <= kPropAttFakie || kind == kPropIconArt)
                return {here::notTaken13, kPropTwisterB, here::afterFindWorldObjectByHash4};
            if ((kind >= kPropIconPeanut && kind < kPropIconArt) || (kind >= kPropShane && kind <= kPropTaig))
                return {here::notTaken14, kPropTwisterC, here::afterFindWorldObjectByHash5};
            return {here::taken9, 0, 0};
        }

        constexpr uint32_t kEggBigName = sym::eggBigName;  // "PropDef_EggBig", held in s1 for its second use
        constexpr PropDefName kPropDefNames[] = {
            {sym::g_textPropDefPowerJuiceExtend, 0x00, points::initPropDefinitions::afterNameHash}, // PropDef_PowerJuiceExtend
            {sym::g_textPropDefPowerJuiceRefill, 0x01, points::initPropDefinitions::afterNameHash2}, // PropDef_PowerJuiceRefill
            {sym::g_textPropDefPowerJuiceHalffill, 0x02, points::initPropDefinitions::afterNameHash3}, // PropDef_PowerJuiceHalffill
            {sym::g_textPropDefPowerSkateA, 0x03, points::initPropDefinitions::afterNameHash4}, // PropDef_PowerSkateA
            {sym::g_textPropDefUtlraSkateA, 0x31, points::initPropDefinitions::afterNameHash5}, // PropDef_UtlraSkateA
            {sym::g_textPropDefAttJump, 0x04, points::initPropDefinitions::afterNameHash6}, // PropDef_AttJump
            {sym::g_textPropDefAttSpeed, 0x05, points::initPropDefinitions::afterNameHash7}, // PropDef_AttSpeed
            {sym::g_textPropDefAttSpin, 0x06, points::initPropDefinitions::afterNameHash8}, // PropDef_AttSpin
            {sym::g_textPropDefAttWallRide, 0x09, points::initPropDefinitions::afterNameHash9}, // PropDef_AttWallRide
            {sym::g_textPropDefAttManual, 0x07, points::initPropDefinitions::afterNameHash10}, // PropDef_AttManual
            {sym::g_textPropDefAttGrind, 0x08, points::initPropDefinitions::afterNameHash11}, // PropDef_AttGrind
            {sym::g_textPropDefAttFakie, 0x0A, points::initPropDefinitions::afterNameHash12}, // PropDef_AttFakie
            {sym::g_textPropDefKeyAirfield, 0x0B, points::initPropDefinitions::afterNameHash13}, // PropDef_KeyAirfield
            {sym::g_textPropDefKeyBoardwalk, 0x0C, points::initPropDefinitions::afterNameHash14}, // PropDef_KeyBoardwalk
            {sym::g_textPropDefKeyCannery, 0x0D, points::initPropDefinitions::afterNameHash15}, // PropDef_KeyCannery
            {sym::g_textPropDefKeyCivic, 0x0E, points::initPropDefinitions::afterNameHash16}, // PropDef_KeyCivic
            {sym::g_textPropDefKeyIndustrial, 0x0F, points::initPropDefinitions::afterNameHash17}, // PropDef_KeyIndustrial
            {sym::g_textPropDefKeyMovie, 0x10, points::initPropDefinitions::afterNameHash18}, // PropDef_KeyMovie
            {sym::g_textPropDefKeyMuseum, 0x11, points::initPropDefinitions::afterNameHash19}, // PropDef_KeyMuseum
            {sym::g_textPropDefKeyZoo, 0x12, points::initPropDefinitions::afterNameHash20}, // PropDef_KeyZoo
            {sym::g_textPropDefLockAirfield, 0x13, points::initPropDefinitions::afterNameHash21}, // PropDef_LockAirfield
            {sym::g_textPropDefLockBoardwalk, 0x14, points::initPropDefinitions::afterNameHash22}, // PropDef_LockBoardwalk
            {sym::g_textPropDefLockCannery, 0x15, points::initPropDefinitions::afterNameHash23}, // PropDef_LockCannery
            {sym::g_textPropDefLockCivic, 0x16, points::initPropDefinitions::afterNameHash24}, // PropDef_LockCivic
            {sym::g_textPropDefLockIndustrial, 0x17, points::initPropDefinitions::afterNameHash25}, // PropDef_LockIndustrial
            {sym::g_textPropDefLockMovie, 0x18, points::initPropDefinitions::afterNameHash26}, // PropDef_LockMovie
            {sym::g_textPropDefLockMuseum, 0x19, points::initPropDefinitions::afterNameHash27}, // PropDef_LockMuseum
            {sym::g_textPropDefLockZoo, 0x1A, points::initPropDefinitions::afterNameHash28}, // PropDef_LockZoo
            {sym::g_textPropDefIconPeanut, 0x1B, points::initPropDefinitions::afterNameHash29}, // PropDef_IconPeanut
            {sym::g_textPropDefIconBanana, 0x1C, points::initPropDefinitions::afterNameHash30}, // PropDef_IconBanana
            {sym::g_textPropDefIconBread, 0x1D, points::initPropDefinitions::afterNameHash31}, // PropDef_IconBread
            {sym::g_textPropDefIconPan, 0x1E, points::initPropDefinitions::afterNameHash32}, // PropDef_IconPan
            {sym::g_textPropDefIconMagnum, 0x1F, points::initPropDefinitions::afterNameHash33}, // PropDef_IconMagnum
            {sym::g_textPropDefIconBullet, 0x20, points::initPropDefinitions::afterNameHash34}, // PropDef_IconBullet
            {sym::g_textPropDefIconPill, 0x21, points::initPropDefinitions::afterNameHash35}, // PropDef_IconPill
            {sym::g_textPropDefIconComb, 0x22, points::initPropDefinitions::afterNameHash36}, // PropDef_IconComb
            {sym::g_textPropDefIconHair, 0x23, points::initPropDefinitions::afterNameHash37}, // PropDef_IconHair
            {sym::g_textPropDefIconGrecian, 0x24, points::initPropDefinitions::afterNameHash38}, // PropDef_IconGrecian
            {sym::g_textPropDefIconBunny, 0x25, points::initPropDefinitions::afterNameHash39}, // PropDef_IconBunny
            {sym::g_textPropDefIconHonda, 0x26, points::initPropDefinitions::afterNameHash40}, // PropDef_IconHonda
            {sym::g_textPropDefIconApartment, 0x27, points::initPropDefinitions::afterNameHash41}, // PropDef_IconApartment
            {sym::g_textPropDefIconPlayer, 0x28, points::initPropDefinitions::afterNameHash42}, // PropDef_IconPlayer
            {sym::g_textPropDefIconTrack, 0x29, points::initPropDefinitions::afterNameHash43}, // PropDef_IconTrack
            {sym::g_textPropDefIconMag, 0x2A, points::initPropDefinitions::afterNameHash44}, // PropDef_IconMag
            {sym::g_textPropDefIconWorld, 0x2B, points::initPropDefinitions::afterNameHash45}, // PropDef_IconWorld
            {sym::g_textPropDefIconPoster, 0x2C, points::initPropDefinitions::afterNameHash46}, // PropDef_IconPoster
            {sym::g_textPropDefShane, 0x32, points::initPropDefinitions::afterNameHash47}, // PropDef_Shane
            {sym::g_textPropDefJaren, 0x33, points::initPropDefinitions::afterNameHash48}, // PropDef_Jaren
            {sym::g_textPropDefChris, 0x34, points::initPropDefinitions::afterNameHash49}, // PropDef_Chris
            {sym::g_textPropDefFranky, 0x35, points::initPropDefinitions::afterNameHash50}, // PropDef_Franky
            {sym::g_textPropDefMatt, 0x36, points::initPropDefinitions::afterNameHash51}, // PropDef_Matt
            {sym::g_textPropDefSven, 0x37, points::initPropDefinitions::afterNameHash52}, // PropDef_Sven
            {sym::g_textPropDefTaig, 0x38, points::initPropDefinitions::afterNameHash53}, // PropDef_Taig
            {sym::g_textPropDefEggSmall, 0x39, points::initPropDefinitions::afterNameHash54}, // PropDef_EggSmall
            {sym::eggBigName, 0x3A, points::initPropDefinitions::afterNameHash55}, // PropDef_EggBig
            {sym::g_textPropDefBeaker, 0x3B, points::initPropDefinitions::afterNameHash56}, // PropDef_Beaker
            {sym::g_textPropDefDropBomb, 0x3C, points::initPropDefinitions::afterNameHash57}, // PropDef_DropBomb
            {sym::g_textPropDefCandyCane, 0x3D, points::initPropDefinitions::afterNameHash58}, // PropDef_CandyCane
            {sym::g_textPropDefDinoEgg, 0x3E, points::initPropDefinitions::afterNameHash59}, // PropDef_DinoEgg
            {sym::g_textPropDefGear, 0x3F, points::initPropDefinitions::afterNameHash60}, // PropDef_Gear
            {sym::g_textPropDefBalloon, 0x40, points::initPropDefinitions::afterNameHash61}, // PropDef_Balloon
            {sym::g_textPropDefTunaHead, 0x41, points::initPropDefinitions::afterNameHash62}, // PropDef_TunaHead
            {sym::eggBigName, 0x42, points::initPropDefinitions::afterNameHash63}, // PropDef_EggBig
            {sym::g_textPropDefCardAce, 0x43, points::initPropDefinitions::afterNameHash64}, // PropDef_CardAce
            {sym::g_textPropDefCardTwo, 0x44, points::initPropDefinitions::afterNameHash65}, // PropDef_CardTwo
            {sym::g_textPropDefCardThree, 0x45, points::initPropDefinitions::afterNameHash66}, // PropDef_CardThree
            {sym::g_textPropDefCardFour, 0x46, points::initPropDefinitions::afterNameHash67}, // PropDef_CardFour
            {sym::g_textPropDefCardFive, 0x47, points::initPropDefinitions::afterNameHash68}, // PropDef_CardFive
            {sym::g_textPropDefCardSix, 0x48, points::initPropDefinitions::afterNameHash69}, // PropDef_CardSix
            {sym::g_textPropDefCardSeven, 0x49, points::initPropDefinitions::afterNameHash70}, // PropDef_CardSeven
            {sym::g_textPropDefCardEight, 0x4A, points::initPropDefinitions::afterNameHash71}, // PropDef_CardEight
            {sym::g_textPropDefCardNine, 0x4B, points::initPropDefinitions::afterNameHash72}, // PropDef_CardNine
            {sym::g_textPropDefCardTen, 0x4C, points::initPropDefinitions::afterNameHash73}, // PropDef_CardTen
            {sym::g_textPropDefCardJack, 0x4D, points::initPropDefinitions::afterNameHash74}, // PropDef_CardJack
            {sym::g_textPropDefCardQueen, 0x4E, points::initPropDefinitions::afterNameHash75}, // PropDef_CardQueen
            {sym::g_textPropDefCardKing, 0x4F, points::initPropDefinitions::afterNameHash76}, // PropDef_CardKing
            {sym::g_textPropDefIconArt, 0x2D, points::initPropDefinitions::afterNameHash77}, // PropDef_IconArt
            {sym::parrotObjectName, 0x2E, points::initPropDefinitions::afterNameHash78}, // AnimObj_Parrot
            {sym::monkeyObjectName, 0x2F, points::initPropDefinitions::afterNameHash79}, // AnimObj_Monkey
            {sym::slothObjectName, 0x30, points::initPropDefinitions::afterNameHash80}, // AnimObj_Sloth
            {sym::g_textPropDefTwisterC, 0x50, points::initPropDefinitions::afterNameHash81}, // PropDef_TwisterC
            {sym::g_textPropDefTwisterM, 0x51, points::initPropDefinitions::afterNameHash82}, // PropDef_TwisterM
            {sym::g_textPropDefTwisterB, 0x52, points::initPropDefinitions::afterNameHash83}, // PropDef_TwisterB
            {sym::g_textPropDefTwisterO, 0x53, points::initPropDefinitions::afterNameHash84}, // PropDef_TwisterO
            {sym::g_textPropDefTwisterG, 0x54, points::initPropDefinitions::afterNameHash85}, // PropDef_TwisterG
        };
    }

    // initPropDefinitions: the definitions' names hashed, the levels' prop states
    // of the last game freed, nothing placed.
    void initPropDefinitions()
    {
        *g_props = 0;
        for (const PropDefName &row : kPropDefNames)
            kPropDefHashes[row.kind] = zge::nameHash(ee::Ptr<const char>(row.name));
        for (uint32_t level = 0; level < kProfileLevels; ++level)
        {
            ee::Ptr<uint8_t> &states = (*g_levelPropStates)[level];
            if (states)
                zge::heapFree(states);
            states = ee::Ptr<uint8_t>();
        }
        *g_propCount = 0;
        *g_propStateBytes = 0;
        *g_levelCollectibles = 0;
        *g_levelExtends = 0;
    }
    READABLE_FUNCTION_WAITS(initPropDefinitions, Verified);

    // newPropInstance: the prop of a prop box, on the end of the list; 0 if the
    // game has no place for it (outside the career only the juice refills
    // and the rescue animals are placed). A rescue animal is only its frame
    // (setAnimalRescue makes its model); the rest are their definition's
    // world object and, by kind, a twister around it.
    uint32_t newPropInstance(ee::Address locatorAt, uint32_t kind, uint32_t handler)
    {
        ee::Local<zge::Matrix> rest;
        const ee::Ptr<Locator> locator(locatorAt);

        const bool juice = kind == kPropJuiceRefill || kind == kPropJuiceHalffill;
        if (*g_gameType != GameType::Career && !juice && !isRescueAnimal(kind))
            return 0;

        // The new prop goes at the list's end.
        ee::Ptr<Prop> *link = &*g_props;
        while (*link)
            link = &(*link)->next;
        const ee::Ptr<Prop> prop(zge::heapAllocate(sizeof(Prop)));
        zge::memoryFill(prop, 0, sizeof(Prop));
        *link = prop;

        prop->taken = 0;
        prop->rising = 1;
        prop->enabled = 1;
        prop->collected = 0;
        prop->bob = 0;
        prop->id = locator->name;
        prop->handler = handler;
        prop->definition = kind;
        prop->kind = kind;
        prop->next = nullptr;
        prop->animal = nullptr;
        *g_propCount = *g_propCount + 1;
        *g_propStateBytes = *g_propStateBytes + 1;

        // The rest frame: the locator's orientation, its position as the
        // matrix's last row.
        const auto makeRestFrame = [&]() {
            zge::matrixIdentity(rest);
            zge::quaternionToMatrix(rest, &locator->turn);
            setTranslation(*rest, locator->position);
        };

        makeRestFrame();
        if (isRescueAnimal(kind))
        {
            makeRestFrame();
            prop->rest = *rest;
            prop->turner = nullptr;
            prop->node = nullptr;
            prop->enabled = 0;
            return 1;
        }

        const uint32_t object = zge::findWorldObjectByHash(propDefHash(kind));
        if (object)
        {
            prop->node = ee::Ptr<zge::Node>(zge::newAppInstanceNode(object, 1));
            zge::setNodeCollides(prop->node, 0, 0);
            zge::setNodeLocalMatrix(prop->node, rest);
            prop->rest = *rest;
        }

        // Its twister, by kind (an unchecked table: each case a dispatch).
        const TwisterCase which = twisterCase(kind);
        uint32_t twister = 0;
        if (which.kind)
        {
            twister = zge::findWorldObjectByHash(propDefHash(which.kind));
            if (kind == kPropJuiceExtend)
                *g_levelExtends = static_cast<uint8_t>(*g_levelExtends + 1);
            else if (juice)
                *g_levelCollectibles = static_cast<uint8_t>(*g_levelCollectibles + 1);
        }
        if (twister)
        {
            prop->turner = ee::Ptr<zge::Node>(zge::newAppInstanceNode(twister, 1));
            zge::setNodeCollides(prop->turner, 0, 0);
            zge::setNodeLocalMatrix(prop->turner, rest);
            return 1;
        }
        prop->turner = nullptr;
        return 1;
    }
    READABLE_FUNCTION_WAITS(newPropInstance, Verified);

    // loadLevelPropState: a level's saved props taken up again: the state bytes
    // kept, and each prop as they say: one no longer enabled loses its
    // models, a rescue animal stays disabled until the game starts them.
    void loadLevelPropState(ee::Address saved)
    {
        ee::Ptr<uint8_t> &states = (*g_levelPropStates)[*g_currentLevel];
        if (!states)
            states = ee::Ptr<uint8_t>(zge::heapAllocate(*g_propStateBytes));
        zge::memoryCopy(states, saved, *g_propStateBytes);

        ee::Ptr<uint8_t> state = states;
        for (ee::Ptr<Prop> prop(*g_props); prop; prop = prop->next, ++state)
        {
            const uint8_t bits = *state;
            prop->enabled = bits & 1u;
            prop->rising = bits >> 1 & 1u;
            prop->taken = bits >> 2 & 1u;
            prop->collected = bits >> 3 & 1u;
            if (!prop->enabled)
            {
                if (prop->node)
                    zge::releaseNode(prop->node, 0);
                if (prop->turner)
                    zge::releaseNode(prop->turner, 0);
                prop->turner = nullptr;
                prop->node = nullptr;
            }
            else if (isRescueAnimal(prop->definition))
                prop->enabled = 0;
        }
        game::tail(aggressive::level::applyLevelPropState);
    }
    READABLE_FUNCTION_WAITS(loadLevelPropState, Verified);

    // saveLevelPropState: the props' state bytes of this level, made up to date;
    // their size goes to *bytes.
    ee::Ptr<uint8_t> saveLevelPropState(ee::Ptr<uint32_t> bytes)
    {
        ee::Ptr<uint8_t> &states = (*g_levelPropStates)[*g_currentLevel];
        if (!states)
            states = ee::Ptr<uint8_t>(zge::heapAllocate(*g_propStateBytes));
        ee::Ptr<uint8_t> state = states;
        for (ee::Ptr<Prop> prop(*g_props); prop; prop = prop->next, ++state)
            *state = static_cast<uint8_t>((*state & ~kPropStateFlags) | (prop->enabled & 1u) | (prop->rising & 1u) << 1 | (prop->taken & 1u) << 2 | (prop->collected & 1u) << 3);
        *bytes = static_cast<uint32_t>(*g_propStateBytes);
        return states;
    }
    READABLE_FUNCTION_WAITS(saveLevelPropState, Verified);

    // freeProps: every prop released and freed.
    void freeProps()
    {
        for (ee::Ptr<Prop> prop(*g_props); prop;)
        {
            if (prop->node)
                zge::releaseNode(prop->node, 1);
            const ee::Ptr<Prop> next = prop->next;
            zge::heapFree(prop);
            prop = next;
        }
        *g_propCount = 0;
        *g_propStateBytes = 0;
    }
    READABLE_FUNCTION_WAITS(freeProps, Verified);

    // setPropPowerUpState: the script's setPowerUpState: the first prop of an id
    // enabled and shown, or disabled and hidden; 1 if there was one.
    int32_t setPropPowerUpState(uint32_t id, uint32_t state)
    {
        ee::Ptr<Prop> prop = *g_props;
        if (!prop)
            return 0;
        for (;;)
        {
            const bool found = prop->id == id;
            if (found)
            {
                prop->enabled = state;
                if (state)
                {
                    const ee::Ptr<zge::Node> node = prop->node;
                    if (node)
                        zge::setNodeShown(node, 1, 0);
                    const ee::Ptr<zge::Node> turner = prop->turner;
                    if (turner)
                        zge::setNodeShown(turner, 1, 0);
                }
                else
                {
                    const ee::Ptr<zge::Node> node = prop->node;
                    if (node)
                        zge::setNodeShown(node, 0, 0);
                    const ee::Ptr<zge::Node> turner = prop->turner;
                    if (turner)
                        zge::setNodeShown(turner, 0, 0);
                }
            }
            prop = prop->next;
            if (!prop)
                return found ? 1 : 0;
            if (found)
                return 1;
        }
    }
    READABLE_FUNCTION_WAITS(setPropPowerUpState, Verified);

    namespace
    {
        // The split-screen game the players chose, a byte of the level object
        // (inferred from what it picks).
        constexpr uint32_t kSplitGameCollect = 2, kSplitGamePoker = 3, kLevelObjectSplitGame = 2;
        uint32_t splitGame()
        {
            return ee::Ptr<uint8_t>(parts::levelObject())[kLevelObjectSplitGame];
        }
    }

    // applyLevelPropState: every prop made what this game and level want. In split
    // screen the juice bottles become their game's: the level's collectible
    // (the fallback an egg), or a card drawn from the pack; otherwise the
    // bottle itself. Elsewhere a bottle collected before is gone for good,
    // and one not has its definition's model, but in the tutorial, which
    // keeps it off. The locks and icons, and any past them, are enabled.
    void applyLevelPropState()
    {
        ee::Ptr<Prop> prop = *g_props;
        uint32_t model = 0; // the hash of the model a prop swaps to
        if (!prop)
            return;
        for (;;)
        {
            const uint32_t definition = prop->definition;
            const ee::Ptr<Prop> next = prop->next;

            bool swap = false;
            // The bottle stays one, enabled.
            const auto becomes = [&](uint32_t hash, uint32_t kind) {
                model = hash;
                prop->kind = kind;
                swap = true;
                prop->enabled = 1;
            };
            const auto card = [&]() {
                const uint32_t drawn = aggressive::drawRandomCard();
                prop->card = drawn;
                becomes(propDefHash(kPropCard + (drawn >> 2)), kPropCard);
            };
            const auto collectible = [&](uint32_t fallback) {
                const uint32_t hash = levelPropDefHash(fallback);
                becomes(hash, fallback);
            };
            const auto bottle = [&](uint32_t kind) {
                becomes(propDefHash(kind), kind);
            };
            // Gone: the models released.
            const auto remove = [&]() {
                const ee::Ptr<zge::Node> turner = prop->turner;
                if (turner)
                {
                    zge::releaseNode(turner, 1);
                    prop->turner = nullptr;
                }
                const ee::Ptr<zge::Node> node = prop->node;
                if (node)
                {
                    zge::releaseNode(node, 1);
                    prop->node = nullptr;
                }
                prop->enabled = 0;
            };
            // Its own model again.
            const auto restore = [&] {
                model = propDefHash(definition);
                prop->kind = definition;
                prop->enabled = 1;
                swap = true;
            };

            if (definition == kPropJuiceHalffill)
            {
                if (*g_gameType == GameType::SplitScreen)
                {
                    const uint32_t game = splitGame();
                    if (game == kSplitGameCollect)
                        collectible(kPropEggSmall);
                    else if (game == kSplitGamePoker)
                        card();
                    else
                        bottle(kPropJuiceHalffill);
                }
                else
                {
                    if (prop->collected)
                        remove();
                    else
                    {
                        if (*g_currentLevel == static_cast<int32_t>(kLevelTutorial))
                            prop->enabled = 0;
                        else
                            restore();
                    }
                }
            }
            else if (definition == kPropJuiceRefill)
            {
                if (*g_gameType == GameType::SplitScreen)
                {
                    const uint32_t game = splitGame();
                    if (game == kSplitGameCollect)
                        collectible(kPropEggBig);
                    else if (game == kSplitGamePoker)
                        card();
                    else
                        bottle(kPropJuiceRefill);
                }
                else
                {
                    if (prop->collected)
                        remove();
                    else
                    {
                        if (*g_currentLevel == static_cast<int32_t>(kLevelTutorial))
                            prop->enabled = 0;
                        else
                            restore();
                    }
                }
            }
            else if (!(definition < kPropEggSmall && (definition < kPropLockAirfield || isRescueAnimal(definition) || definition >= kPropShane)))
                prop->enabled = 1;

            if (swap)
                swapPropModel(prop, model);
            prop = next;
            if (!prop)
                break;
        }
    }
    READABLE_FUNCTION_WAITS(applyLevelPropState, Verified);

    // setAnimalRescue: Animal Rescue on or off: every rescue animal's model
    // made (its world object's, playing its animation) or taken away, and
    // the level's AnimObj_Parrot told "parrotStart" or "parrotEnd".
    void setAnimalRescue(uint32_t rescue)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint32_t> parrotName;
        const uint32_t wanted = rescue & 0xFFu;
        if (*g_animalRescue == wanted)
            return;
        *g_animalRescue = wanted;

        for (ee::Ptr<Prop> prop(*g_props); prop;)
        {
            const ee::Ptr<Prop> next = prop->next;
            const uint32_t definition = prop->definition;
            if (isRescueAnimal(definition))
            {
                const uint32_t name = definition == kPropParrot ? kParrotObjectName : definition == kPropMonkey ? kMonkeyObjectName : kSlothObjectName;
                const ee::Ptr<zge::Node> worldNode = zge::findWorldNodeByHash(zge::nameHash(ee::Ptr<const char>(name)), 0, 0);
                if (wanted)
                    spawnRescueAnimal(prop, worldNode);
                else if (prop->animal)
                {
                    zge::stopAnimation(prop->animal);
                    zge::freeObjectAnimator(prop->animal);
                    zge::deleteSceneObject(prop->animal);
                    prop->animal = nullptr;
                    prop->node = nullptr;
                }
                prop->enabled = wanted;
            }
            prop = next;
        }

        *parrotName = zge::nameHash(ee::Ptr<const char>(kParrotObjectName));
        const ee::Ptr<ee::Ptr<zge::SceneObject>> receivers = zge::findObjects(parrotName, 1);
        if (!receivers)
            return;
        message->sender = 0;
        message->flags = zge::kMessageNow;
        message->target = *receivers;
        message->data = 0;
        message->type = zge::nameHash(ee::Ptr<const char>(wanted ? kParrotStartName : kParrotEndName));
        zge::sendEngineMessage(message);
    }
    READABLE_FUNCTION_WAITS(setAnimalRescue, Verified);

    // returnRescueAnimal: an animal the skater dropped goes back to its prop's
    // place and the prop is enabled again (dropAnimals).
    void returnRescueAnimal(ee::Ptr<zge::SceneObject> object)
    {
        for (ee::Ptr<Prop> prop(*g_props); prop; prop = prop->next)
        {
            if (!isRescueAnimal(prop->definition) || prop->animal != object)
                continue;
            zge::setObjectRotationMatrix(object, &prop->rest);
            zge::setObjectPosition(prop->animal, &prop->rest.translation());
            zge::updatePose(prop->animal);
            prop->enabled = 1;
            return;
        }
    }
    READABLE_FUNCTION_WAITS(returnRescueAnimal, Verified);

    // spawnRescueAnimal: a rescue animal's model: an instance of its world node
    // in the prop's place, its object with an animator playing the animal's
    // animation.
    void spawnRescueAnimal(ee::Ptr<Prop> prop, ee::Ptr<zge::Node> worldNode)
    {

        prop->node = zge::instanceGeometryNode(worldNode, (3u << 20));
        zge::setNodeCollides(prop->node, 0, 0);
        zge::setNodeLocalMatrix(prop->node, ee::Ptr<const zge::Matrix>(kIdentityMatrix));
        const ee::Ptr<zge::Scene> model = zge::setupNodeSceneModel(prop->node);
        zge::releaseNode(prop->node, 1);
        prop->animal = ee::Ptr<zge::SceneObject>(zge::newModelObject(model, 0));
        zge::setObjectRotationMatrix(prop->animal, &prop->rest);
        zge::setObjectPosition(prop->animal, &prop->rest.translation());
        zge::updatePose(prop->animal);
        zge::createObjectAnimator(prop->animal);

        const auto animate = [&](uint32_t animation) {
            game::tail(zge::playAnimationNamed, prop->animal, animation, 5, tuningValue(kTuningControl, kTuningAnimationBlendTime), ee::Float(-1.0f));
        };
        const uint32_t definition = prop->definition;
        if (definition == kPropParrot)
        {
            animate(kParrotAnimation);
            return;
        }
        if (definition == kPropMonkey)
        {
            animate(kMonkeyAnimation);
            return;
        }
        if (definition == kPropSloth)
        {
            animate(kSlothAnimation);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(spawnRescueAnimal, Verified);

    // swapPropModel: the prop's model made the definition `hash` names (none
    // found: it keeps what it has), and its twister, by what it was placed
    // as, made again.
    void swapPropModel(ee::Ptr<Prop> prop, uint32_t hash)
    {

        const uint32_t object = zge::findWorldObjectByHash(hash);
        if (object)
        {
            const ee::Ptr<zge::Node> old = prop->node;
            if (old)
                zge::releaseNode(old, 1);
            prop->node = ee::Ptr<zge::Node>(zge::newAppInstanceNode(object, 1));
            zge::setNodeCollides(prop->node, 0, 0);
            zge::setNodeLocalMatrix(prop->node, &prop->rest);
        }

        const uint32_t definition = prop->definition;
        uint32_t twisterKind = 0;
        if (definition < kPropEggSmall)
        {
            if (definition == kPropJuiceExtend)
                twisterKind = kPropTwisterO;
            else if (definition == kPropJuiceRefill)
            {
                if (prop->kind == kPropJuiceHalffill)
                    twisterKind = kPropTwisterG;
                else
                    twisterKind = kPropTwisterM;
            }
            else if (definition == kPropJuiceHalffill)
                twisterKind = kPropTwisterG;
            else if (definition <= kPropAttFakie || definition == kPropIconArt)
                twisterKind = kPropTwisterB;
            else if ((definition >= kPropIconPeanut && definition < kPropIconArt) || definition >= kPropShane)
                twisterKind = kPropTwisterC;
        }

        uint32_t twister = 0;
        if (twisterKind)
            twister = zge::findWorldObjectByHash(propDefHash(twisterKind));

        const ee::Ptr<zge::Node> old = prop->turner;
        if (twister)
        {
            if (old)
            {
                zge::releaseNode(old, 1);
                prop->turner = nullptr;
            }
            prop->turner = ee::Ptr<zge::Node>(zge::newAppInstanceNode(twister, 1));
            zge::setNodeCollides(prop->turner, 0, 0);
            zge::setNodeLocalMatrix(prop->turner, &prop->rest);
            game::tail(zge::updateMovedNode, prop->turner);
            return;
        }
        if (old)
            zge::releaseNode(old, 1);
        prop->turner = nullptr;
    }
    READABLE_FUNCTION_WAITS(swapPropModel, Verified);

    // levelPropDefHash: the level's collectible: the definition's hash by level;
    // `fallback`, the kind's name, for the tutorial and any past the table.
    uint32_t levelPropDefHash(uint32_t fallback)
    {
        switch (*g_currentLevel)
        {
        case 0:
            return propDefHash(kPropCandyCane);
        case 1:
            return propDefHash(kPropBalloon);
        case 2:
            return propDefHash(kPropTunaHead);
        case 3:
            return propDefHash(kPropDropBomb);
        case 4:
            return propDefHash(kPropGear);
        case 5:
            return propDefHash(kPropBeaker);
        case 6:
            return propDefHash(kPropDinoEgg);
        case 8:
            return propDefHash(kPropEggBig2);
        default:
            return fallback;
        }
    }
    READABLE_FUNCTION(levelPropDefHash, Verified);

    namespace
    {
        constexpr uint64_t kAllLevelKeys = 0xFE00;   // SkaterProfile::lootFlags: levels 0 to 6
        inline ee::Global<uint32_t, sym::g_cheatKeysGiven> g_cheatKeysGiven;
    }

    // cheatGiveLevelKeys: the unlock cheat: every skater record gets the keys of
    // the seven career levels, each worth its experience multiplier's step.
    void cheatGiveLevelKeys()
    {
        uint32_t record = kSkaterRecords;
        const uint32_t end = kSkaterRecords + kSkaterRecordCount * kSkaterRecordSize;
        for (;;)
        {
            const ee::Ptr<SkaterProfile> profile(record);
            profile->experienceMultiplier = static_cast<uint8_t>(profile->experienceMultiplier + 7);
            profile->lootFlags = profile->lootFlags | kAllLevelKeys;
            record += kSkaterRecordSize;
            if (record >= end)
                break;
        }
        *g_cheatKeysGiven = 1;
    }
    READABLE_FUNCTION(cheatGiveLevelKeys, Verified);
}
