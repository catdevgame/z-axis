// The level's trigger zones and what a prop's zone does (inlineFx.c): each
// locator of kind kZoneLocator is a box in the level, listed in a coarse
// grid (skater/zones.cpp walks it every frame). A locator with a prop kind
// in its tags places that prop too, and its box is the prop's (a fixed box
// about the locator): entering it picks the prop up (zoneEnterHandler)
// rather than sending "boxEnter".

#include "zones.h"

#include "matrix_points.h"
#include "props.h"

#include "aggressive/career.h"
#include "aggressive/globals.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"
#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/assets/tag_table.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/collision/collision.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/locators.h"
#include "sce/libgcc.h"
#include "aggressive/card_game.h"
#include "aggressive/zone_memory.h"
#include "aggressive/level/props.h"
#include "aggressive/level/animal_rescue.h"
#include "aggressive/level/zones.h"

#include <cstddef>
#include "sym/aggressive/level/zones.cpp.h"

namespace aggressive::level
{
    using skater::Player;

    void checkLevelUnlocks(ee::Ptr<Player> player);

    namespace
    {
        // A locator of the level (the scene's).
        struct Locator
        {
            zge::Vec4 position;        // +0x00
            zge::Quaternion turn;      // +0x10
            uint32_t kind;             // +0x20 its kind's name hash
            uint32_t name;             // +0x24 its name's hash
            uint32_t _28;
            ee::Ptr<zge::TagRecord> tags; // +0x2C its tag record
        };
        static_assert(offsetof(Locator, kind) == 0x20 && offsetof(Locator, tags) == 0x2C);

        // A node of the list findLocators fills.
        struct LocatorNode
        {
            ee::Ptr<LocatorNode> next; // +0x00
            uint32_t previous;         // +0x04
            ee::Ptr<Locator> locator;  // +0x08
        };
        static_assert(offsetof(LocatorNode, locator) == 0x08);

        // The collision world's extent, which is also the grid's origin.
        struct WorldBounds
        {
            zge::Vec4 min, max;
        };
        static_assert(sizeof(WorldBounds) == 0x20);

        constexpr uint32_t kZoneLocator = 0x5362;  // Locator::kind: a box
        // Its tag record (tagByte with 2, tagWord and tagVector with 8 / 0x40).
        constexpr uint32_t kTagBoxMin = 0, kTagBoxMax = 12, kTagPropKind = 24, kTagTarget = 28;
        constexpr uint32_t kNoProp = 255;          // the prop kind tag of a plain box

        // The list of zones and its last.
        inline ee::Global<uint32_t, sym::g_zones> g_zones;
        inline ee::Global<uint32_t, sym::g_lastZone> g_lastZone;

        constexpr uint32_t kZoneGridSide = 5; // the cells initZones clears and freeLevelZones frees, per side

        // The head of a grid cell's list. A box past the far edge lands past
        // the cells kept, and one short of the near edge wraps (see setupLocatorZone).
        uint32_t &gridCell(uint32_t column, uint32_t row)
        {
            return ee::Ptr<uint32_t>(kZoneGrid)[column * kZoneGridRowWords + row];
        }

        // A prop's box about its locator.
        const zge::Vec4 kPropBoxMin = {ee::Float(-1.75f), ee::Float(-3.1f), ee::Float(-2.2f), ee::Float(0.0f)};
        const zge::Vec4 kPropBoxMax = {ee::Float(1.75f), ee::Float(0.2f), ee::Float(2.2f), ee::Float(0.0f)};

    }

    void initZoneLocators();

    // initZones: the zones of a level being loaded. The events' names
    // are hashed, the grid cleared, the prop definitions hashed, and the
    // grid laid over the collision world, five cells a side.
    void initZones()
    {
        *g_enterEvent = zge::nameHash(ee::Ptr<const char>(sym::boxEnterName));
        *g_exitEvent = zge::nameHash(ee::Ptr<const char>(sym::boxExitName));

        for (uint32_t column = 0; column < kZoneGridSide; ++column)
            for (uint32_t row = 0; row < kZoneGridSide; ++row)
                gridCell(column, row) = 0;

        initPropDefinitions();
        const ee::Ptr<WorldBounds> bounds = ee::Ptr<WorldBounds>(ee::Address(zge::collisionWorldBounds()));
        const ee::Float perCell(0.2f);
        *g_zoneCellWidth = (bounds->max.x - bounds->min.x) * perCell;
        *g_zoneCellHeight = (bounds->max.y - bounds->min.y) * perCell;
        *g_lastZone = 0;
        *g_zones = 0;
        *g_zoneCellDepth = (bounds->max.z - bounds->min.z) * perCell;
        zge::memoryCopy(sym::g_zoneGridMinX, bounds, sizeof(WorldBounds));
        game::tail(initZoneLocators);
    }
    READABLE_FUNCTION_WAITS(initZones, Verified);

    // freeLevelZones: the level's end: the zones, the grid's lists, the props.
    void freeLevelZones()
    {
        for (uint32_t zone = *g_zones; zone;)
        {
            const uint32_t next = ee::Ptr<Zone>(zone)->next;
            aggressive::releaseZoneBlock(zone);
            zone = next;
        }
        for (uint32_t column = 0; column < kZoneGridSide; ++column)
            for (uint32_t row = 0; row < kZoneGridSide; ++row)
                for (uint32_t node = gridCell(column, row); node;)
                {
                    const uint32_t next = ee::Ptr<ZoneListNode>(node)->next;
                    aggressive::releaseZoneBlock(node);
                    node = next;
                }
        game::tail(aggressive::level::freeProps);
    }
    READABLE_FUNCTION_WAITS(freeLevelZones, Verified);

    namespace
    {
        // The zone on the end of a grid cell's list.
        void addToCell(uint32_t &cell, uint32_t zone)
        {
            uint32_t *link = &cell;
            while (*link)
                link = &ee::Ptr<ZoneListNode>(*link)->next;
            const uint32_t added = aggressive::allocateLockedBlock(sizeof(ZoneListNode));
            *link = added;
            ee::Ptr<ZoneListNode>(added)->next = 0;
            ee::Ptr<ZoneListNode>(added)->zone = zone;
        }
    }

    // setupLocatorZone: a locator set up: a kZoneLocator makes a zone on the
    // end of the list and enters it in every grid cell its box reaches. A
    // box tagged with a prop kind places the prop (none placed, as in the
    // split-screen games for most kinds: no zone either).
    void setupLocatorZone(ee::Ptr<Locator> locator)
    {
        ee::Local<zge::Vec4[8]> corners;
        ee::Local<uint32_t> firstRow;
        if (locator->kind != kZoneLocator)
            return;

        const uint32_t propKind = zge::tagByte(locator->tags, 2, kTagPropKind);
        if (propKind != kNoProp)
        {
            const int32_t handler = zge::tagWord(locator->tags, 8, kTagTarget);
            if (!newPropInstance(locator, propKind, static_cast<uint32_t>(handler)))
                return;
        }

        // A new zone, last on the list.
        const uint32_t added = aggressive::allocateLockedBlock(sizeof(Zone));
        if (!*g_lastZone)
            *g_zones = added;
        else
            ee::Ptr<Zone>(*g_lastZone)->next = added;
        *g_lastZone = added;

        const ee::Ptr<Zone> zone(added);
        zone->id = locator->name;
        if (propKind == kNoProp)
        {
            zge::tagVector(locator->tags, 0x40, kTagBoxMin, &zone->box.low);
            zge::tagVector(locator->tags, 0x40, kTagBoxMax, &zone->box.high);
            zone->target = static_cast<uint32_t>(zge::tagWord(locator->tags, 8, kTagTarget));
            zone->enterHandler = 0;
        }
        else
        {
            zone->box.low.x = kPropBoxMin.x;
            zone->box.low.y = kPropBoxMin.y;
            zone->box.low.z = kPropBoxMin.z;
            zone->box.high.x = kPropBoxMax.x;
            zone->box.high.y = kPropBoxMax.y;
            zone->box.high.z = kPropBoxMax.z;
            zone->enterHandler = 1;
            zone->target = locator->name;
        }
        zone->next = 0;

        zge::matrixIdentity(&zone->turn);
        zge::quaternionToMatrix(&zone->turn, &locator->turn);

        // The box's eight corners turned by the locator; the box becomes
        // their extent, moved to the locator.
        for (int corner = 0; corner < 8; ++corner)
        {
            corners[corner].x = (corner & 4) ? zone->box.high.x : zone->box.low.x;
            corners[corner].y = (corner & 2) ? zone->box.high.y : zone->box.low.y;
            corners[corner].z = (corner & 1) ? zone->box.high.z : zone->box.low.z;
        }
        for (int corner = 0; corner < 8; ++corner)
            corners[corner] = zge::transformed(zone->turn, corners[corner]);
        zone->box.low = corners[0];
        zone->box.high = corners[0];

        for (int corner = 1; corner < 8; ++corner)
        {
            const zge::Vec4 &at = corners[corner];
            if (at.x < zone->box.low.x)
                zone->box.low.x = at.x;
            if (at.y < zone->box.low.y)
                zone->box.low.y = at.y;
            if (at.z < zone->box.low.z)
                zone->box.low.z = at.z;
            if (zone->box.high.x < at.x)
                zone->box.high.x = at.x;
            if (zone->box.high.y < at.y)
                zone->box.high.y = at.y;
            if (zone->box.high.z < at.z)
                zone->box.high.z = at.z;
        }
        zone->box.low = zone->box.low + locator->position;
        zone->box.high = zone->box.high + locator->position;

        // The grid cells it reaches. A box past the grid's far edge indexes
        // as skater/zones.cpp does, and one short of its near edge wraps
        // round to a huge cell and enters none.
        const ee::Float minX = *g_zoneGridMinX;
        const ee::Float cellWidth = *g_zoneCellWidth;
        const uint32_t firstColumn = sce::libgcc::fptoui((zone->box.low.x - minX) / cellWidth);
        const ee::Float minZ = *g_zoneGridMinZ;
        const ee::Float cellDepth = *g_zoneCellDepth;
        *firstRow = sce::libgcc::fptoui((zone->box.low.z - minZ) / cellDepth);
        const uint32_t endColumn = sce::libgcc::fptoui((zone->box.high.x - minX) / cellWidth) + 1;
        const uint32_t endRow = sce::libgcc::fptoui((zone->box.high.z - minZ) / cellDepth) + 1;

        for (uint32_t column = firstColumn; column < endColumn; ++column)
            for (uint32_t row = *firstRow; row < endRow; ++row)
                addToCell(gridCell(column, row), *g_lastZone);
    }
    READABLE_FUNCTION_WAITS(setupLocatorZone, Verified);

    namespace
    {
        using LocatorList = ee::Ptr<ee::Ptr<LocatorNode>>;
    }

    // initZoneLocators: every locator of the level gets its zone.
    void initZoneLocators()
    {
        ee::Local<ee::Ptr<LocatorNode>> list;
        zge::initList(list);
        zge::findLocators(list, 0, 0);
        for (ee::Ptr<LocatorNode> node = *list; node; node = node->next)
            setupLocatorZone(node->locator);
        zge::freeLocators(list);
    }
    READABLE_FUNCTION_WAITS(initZoneLocators, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_levelNumber> g_currentLevel;

        constexpr uint32_t kParrotAnimation = sym::parrotAnimation;  // "parrot.animation"
        constexpr uint32_t kMonkeyAnimation = sym::monkeyAnimation;  // "monkey.animation"
        constexpr uint32_t kSlothAnimation = sym::slothAnimation;   // "sloth.animation"
        constexpr uint32_t kBonusCollectName = sym::bonusCollectName; // "bonus_collect"
        constexpr uint32_t kTrickUnlockedText = sym::trickUnlockedText; // "Special trick unlocked!"
        constexpr uint32_t kCheatUnlockedText = sym::cheatUnlockedText; // "New cheat unlocked!"
        constexpr uint32_t kUltraSkatesText = sym::ultraSkatesText;   // "Ultra skates unlocked!"

        // The pick-up sound (soundEvent) and its variants.
        constexpr uint32_t kBonusSound = 73;
        enum BonusSound : uint32_t
        {
            kBonusJuice = 0,
            kBonusExtend = 1,
            kBonusAttribute = 2,
            kBonusCollectible = 4,
            kBonusTrick = 6,
        };

        constexpr uint32_t kTuningAnimationBlendTime = 2;  // kTuningControl's "Animation Blend Time"

        constexpr int32_t kExtendPoints = 100000, kJuiceScore = 25000, kAttributePoints = 200000, kKeyPoints = 500000;

        // A row of the levels' table: the cheat a level's collectibles
        // unlock, by bit.
        struct LevelInfo
        {
            uint32_t cheat;
            uint8_t _04[0x40];
        };
        static_assert(sizeof(LevelInfo) == 0x44);
        inline ee::Global<LevelInfo[kProfileLevels], sym::cheats> g_levelInfo;
        inline ee::Global<uint32_t[kCareerLevelCount], sym::careerLevelOrder> g_careerLevelOrder;
        inline ee::Global<uint32_t, sym::g_careerCheats> g_careerCheats;

        // What the "bonus_collect" event carries.
        struct BonusCollected
        {
            uint32_t id;
            uint32_t playerNumber;
        };

        bool everyCareerCollectibleFound(ee::Ptr<SkaterProfile> profile)
        {
            for (const uint32_t level : *g_careerLevelOrder)
            {
                const LevelRecord &record = profile->levels[level];
                if (!record.collectibles || record.collected < record.collectibles)
                    return false;
            }
            return true;
        }

        ee::Ptr<Prop> findProp(uint32_t id)
        {
            for (ee::Ptr<Prop> prop(*g_props); prop; prop = prop->next)
                if (prop->id == id)
                    return prop;
            return ee::Ptr<Prop>();
        }
    }

    // checkLevelUnlocks: a pick-up counted: every collectible of a level but the
    // tutorial unlocks its cheat, and all of every career level's unlock
    // the ultra skates, once: a bonus to each attribute and their trick.
    void checkLevelUnlocks(ee::Ptr<Player> player)
    {
        const ee::Ptr<SkaterProfile> profile = skater::skaterProfile(player);
        const int32_t level = *g_currentLevel;
        if (level != static_cast<int32_t>(kLevelTutorial))
        {
            const LevelRecord &record = profile->levels[level];
            if (record.collected == record.collectibles)
            {
                *g_careerCheats = *g_careerCheats | 1u << ((*g_levelInfo)[level].cheat & 0x1Fu);
                aggressive::hud::showMessage(kCheatUnlockedText, ee::Float(3.5f));
            }
        }
        if ((profile->lootFlags & kLootUltraSkates) || !everyCareerCollectibleFound(profile))
            return;

        profile->lootFlags |= kLootUltraSkates;
        aggressive::hud::showMessage(kUltraSkatesText, ee::Float(3.5f));
        for (uint32_t stat = 0; stat < kStatCount; ++stat)
            skater::levelUpAttribute(player, stat);
        profile->unlockedTricks = static_cast<uint16_t>(profile->unlockedTricks | kTrickUltraSkates);
    }
    READABLE_FUNCTION_WAITS(checkLevelUnlocks, Verified);

    // zoneEnterHandler: a skater entered a prop's zone (`id`, the zone's target,
    // is the prop's). Each kind gives what it gives; a prop gone with it is
    // hidden, the rest are released but a rescue animal, which the skater
    // picks up if it can (then plays its animation). A prop left disabled
    // tells its handler object "bonus_collect" with the id and the player.
    void zoneEnterHandler(uint32_t id, ee::Ptr<Player> player)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<BonusCollected> event;
        ee::Local<ee::Float> capacity;
        ee::Local<ee::Float> juice;

        const ee::Ptr<Prop> prop = findProp(id);
        if (!prop || !prop->enabled)
            return;
        const ee::Ptr<SkaterProfile> profile = skater::skaterProfile(player);
        const auto sound = [&](uint32_t variant) {
            soundEvent(kBonusSound, variant, 0, 0, 0);
        };
        const auto addPoints = [&](int32_t points) {
            aggressive::skater::setSkaterPoints(player, skater::skaterPoints(player) + points);
        };

        prop->taken = *g_currentLevel == static_cast<int32_t>(kLevelTutorial) ? 1u : 0u;
        const uint32_t kind = prop->kind;
        if (kind > kPropCard)
            return;

        // The join for collecting a thing whose points are worth `points`.
        const auto collectible = [&](int32_t points) {
            aggressive::skater::setSkaterPoints(player, points);
            sound(kBonusCollectible);
            prop->taken = 0;
        };
        // An attribute: stat is its stat, `flag` its loot bit, and each
        // case's code is the same from its leader.
        const auto attribute = [&](uint64_t flag, uint32_t stat) {
            profile->lootFlags = profile->lootFlags | flag;
            skater::levelUpAttribute(player, stat);
            sound(kBonusAttribute);
            addPoints(kAttributePoints);
            prop->collected = 1;
        };
        const auto key = [&](uint32_t level) {
            profile->lootFlags = profile->lootFlags | 1ull << (level + 9u);
            profile->experienceMultiplier = static_cast<uint8_t>(profile->experienceMultiplier + 1);
            sound(kBonusCollectible);
            addPoints(kKeyPoints);
            prop->collected = 1;
        };
        const auto specialTrick = [&](uint16_t bit) {
            const uint16_t unlocked = static_cast<uint16_t>(profile->unlockedTricks | bit);
            profile->unlockedTricks = unlocked;
            sound(kBonusTrick);
            aggressive::hud::showMessage(kTrickUnlockedText, ee::Float(3.5f));
            prop->collected = 1;
        };
        // One of the level's collectibles found.
        const auto countCollected = [&]() {
            if (*g_currentLevel != static_cast<int32_t>(kLevelTutorial))
                prop->collected = 1;
            LevelRecord &record = profile->levels[*g_currentLevel];
            record.collected = static_cast<uint8_t>(record.collected + 1);
            checkLevelUnlocks(player);
        };

        switch (kind)
        {
        case kPropEggBig:
        {
            collectible(skater::skaterPoints(player) + 3);
            break;
        }
        case kPropEggSmall:
        {
            collectible(skater::skaterPoints(player) + 1);
            break;
        }
        case kPropCard:
        {
            aggressive::collectCard(&player->cardHand, prop->card);
            collectible(player->cardHand.points);
            break;
        }
        case kPropJuiceExtend:
            if (*g_currentLevel != static_cast<int32_t>(kLevelTutorial))
            {
                skater::levelUpBonus(player);
                addPoints(kExtendPoints);
            }
            sound(kBonusExtend);
            if (!prop->collected && *g_gameType == GameType::Career)
            {
                prop->collected = 1;
                LevelRecord &record = profile->levels[*g_currentLevel];
                record.juiceExtends = static_cast<uint8_t>(record.juiceExtends + 1);
            }
            break;
        case kPropJuiceRefill:
        {
            skater::cartonSize(player->juiceMeter, capacity);
            skater::setJuiceMeter(player->juiceMeter, *capacity);
            sound(kBonusJuice);
            if (*g_currentLevel != static_cast<int32_t>(kLevelTutorial))
            {
                const int32_t score = skater::skaterScore(player);
                skater::setSkaterScore(player, static_cast<uint32_t>(score + kJuiceScore), 0);
            }
            const uint32_t counted = prop->collected;
            prop->sinceSound = ee::Float(30.0f);
            prop->taken = 1;
            if (counted)
                break;
            if (*g_gameType == GameType::Career)
                countCollected();
            break;
        }
        case kPropJuiceHalffill:
        {
            skater::cartonSize(player->juiceMeter, capacity);
            skater::juiceAmount(player->juiceMeter, juice);
            const ee::Float size = *capacity;
            const ee::Float filled = *juice + size / ee::Float(2.0f);
            *juice = filled;
            if (size < filled)
                skater::setJuiceMeter(player->juiceMeter, size);
            else
                skater::setJuiceMeter(player->juiceMeter, filled);
            sound(kBonusJuice);
            const uint32_t counted = prop->collected;
            prop->sinceSound = ee::Float(15.0f);
            prop->taken = 1;
            if (counted)
                break;
            if (*g_gameType == GameType::Career)
                countCollected();
            break;
        }
        case kPropAttJump:
            attribute(0x1, 0);
            break;
        case kPropAttSpeed:
            attribute(0x2, 1);
            break;
        case kPropAttSpin:
            attribute(0x4, 2);
            break;
        case kPropAttGrind:
            attribute(0x20, 3);
            break;
        case kPropAttManual:
            attribute(0x10, 4);
            break;
        case kPropAttFakie:
            attribute(0x40, 5);
            break;
        case kPropAttWallRide:
            attribute(0x8, 6);
            break;
        case kPropKeyAirfield:
            key(3);
            break;
        case kPropKeyBoardwalk:
            key(1);
            break;
        case kPropKeyCannery:
            key(2);
            break;
        case kPropKeyCivic:
            key(0);
            break;
        case kPropKeyIndustrial:
            key(4);
            break;
        case kPropKeyMovie:
            key(5);
            break;
        case kPropKeyMuseum:
            key(6);
            break;
        case kPropKeyZoo:
            key(7);
            break;
        case kPropShane:
            specialTrick(0x4);
            break;
        case kPropJaren:
            specialTrick(0x80);
            break;
        case kPropChris:
            specialTrick(0x2);
            break;
        case kPropFranky:
            specialTrick(0x10);
            break;
        case kPropMatt:
            specialTrick(0x100);
            break;
        case kPropSven:
            specialTrick(0x40);
            break;
        case kPropTaig:
            specialTrick(0x8);
            break;
        case kPropSkateA:
        case kPropParrot:
        case kPropMonkey:
        case kPropSloth:
            // Nothing given.
            break;
        default:
            // Locks, icons, the ultra skates: not picked up here.
            return;
        }

        if (prop->taken)
        {
            const ee::Ptr<zge::Node> node = prop->node;
            if (node)
                zge::setNodeShown(node, 0, 0);
            const ee::Ptr<zge::Node> turner = prop->turner;
            if (turner)
                zge::setNodeShown(turner, 0, 0);
            prop->enabled = 0;
        }
        else
        {
            const uint32_t current = prop->kind;
            if (isRescueAnimal(current))
            {
                const int32_t carried = aggressive::pickUpAnimal(player, prop->animal);
                if (!carried)
                    prop->enabled = 1;
                else
                {
                    const auto animate = [&](uint32_t animation) {
                        zge::playAnimationNamed(prop->animal, animation, 5, tuningValue(kTuningControl, kTuningAnimationBlendTime), ee::Float(-1.0f));
                    };
                    const uint32_t definition = prop->definition;
                    if (definition == kPropParrot || definition == kPropMonkey)
                        animate(definition == kPropParrot ? kParrotAnimation : kMonkeyAnimation);
                    else
                    {
                        if (definition == kPropSloth)
                            animate(kSlothAnimation);
                    }
                    prop->enabled = 0;
                }
            }
            else
            {
                const ee::Ptr<zge::Node> node = prop->node;
                if (node)
                    zge::releaseNode(node, 0);
                const ee::Ptr<zge::Node> turner = prop->turner;
                if (turner)
                    zge::releaseNode(turner, 0);
                prop->turner = nullptr;
                prop->node = nullptr;
                prop->enabled = 0;
            }
        }
        if (prop->enabled)
            return;
        const ee::Ptr<ee::Ptr<zge::SceneObject>> receivers = zge::findObjects(&prop->handler, 1);
        if (!receivers)
            return;

        event->id = id;
        event->playerNumber = player->number;
        message->flags = zge::kMessageNow;
        message->target = *receivers;
        message->data = event.address();
        message->sender = 0;
        message->type = zge::nameHash(ee::Ptr<const char>(kBonusCollectName));
        zge::sendEngineMessage(message);
    }
    READABLE_FUNCTION_WAITS(zoneEnterHandler, Verified);

}
