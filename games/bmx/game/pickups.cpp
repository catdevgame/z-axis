// BMX XXX's level pickups (pickups.h): the level's pickups and checkpoints
// enabled or disabled as a turn starts, and what a rider's activation of one
// gives. BMX XXX's own code (game.c).

#include "pickups.h"
#include "overlay_messages.h"
#include "player_riders.h"
#include "records.h"
#include "round.h"

#include "bmx/level/entities.h"
#include "bmx/level/entity_messages.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/displays.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/weapon/weapon.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/pickups.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The level object types the set-up knows (the hashes of "Pickup" and
        // "CheckPoint").
        constexpr uint32_t kPickupType = 0x054D80A0;
        constexpr uint32_t kCheckPointType = 0x080142A4;

        // The pickup kinds, by the capitals an object's name starts with:
        // HEALTH, WEAPON, SCORE, BIKE, SNIPER (the game's table, in this order).
        struct PickupKindName
        {
            ee::Ptr<const char> name;
            uint32_t length;
        };
        static_assert(sizeof(PickupKindName) == 8);
        const ee::Ptr<PickupKindName> kPickupKindNames(sym::g_pickupKindNames);
        enum PickupKind : uint32_t
        {
            kHealthPickup,
            kWeaponPickup,
            kScorePickup,
            kBikePickup,
            kSniperPickup,
            kPickupKinds,
        };

        // A bike pickup is a bike part (RiderCharacter::bikeParts); the rest
        // of its name says which (the hashes of frame, fork, handlebars,
        // wheel, crank, seat).
        const ee::Ptr<uint32_t> kBikePartNames(sym::g_bikePartNames);

        // The checkpoint the challenge has ("van": the names that start so).
        constexpr uint32_t kVanName = sym::vanName;
        constexpr uint32_t kVanNameLength = 3;

        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        // Whether the current player's character found the bike part of this
        // name in the level being played.
        bool bikePartFoundHere(uint32_t partName, bool &known)
        {
            const ee::Ptr<RiderCharacter> character = playerCharacter(*g_currentPlayer);
            uint32_t part = 0;
            while (kBikePartNames[part] != partName)
            {
                if (++part >= kBikeParts)
                {
                    known = false;
                    return false;
                }
            }
            known = true;
            for (uint32_t bike = 0; bike < kCharacterBikes; ++bike)
            {
                const uint8_t found = character->bikeParts[bike][part];
                if ((found & kBikePartFound) != 0 && bikePartLevel(found) == *g_currentLevel)
                    return true;
            }
            return false;
        }
    }

    // nameWithoutUnderscores: the copy leaves every '_' out; underscores at
    // the copy's end are cut too (none can be left: the original's step).
    ee::Ptr<char> nameWithoutUnderscores(ee::Ptr<const char> name, uint32_t skip)
    {
        const ee::Ptr<char> out(sym::g_nameWithoutUnderscores);
        const char *from = name.get() + skip;
        out[0] = 0;
        uint32_t length = 0;
        for (; *from != 0; ++from)
        {
            if (*from != '_')
                out[length++] = *from;
        }
        out[length] = 0;
        if (length != 0)
        {
            char *const start = out.get();
            for (char *last = start + length - 1; last >= start && *last == '_'; --last)
                *last = 0;
        }
        return out;
    }
    READABLE_FUNCTION(nameWithoutUnderscores, Verified);

    // setUpLevelPickup: a health, weapon or score pickup is enabled; a bike
    // part only in the challenge, and only while the player's character has
    // not found that part in this level; the sniper pickup only in the
    // paintball; the van checkpoint only in the challenge. Every other
    // pickup and checkpoint is disabled; other objects are left alone.
    uint32_t setUpLevelPickup(ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<zge::EngineMessage> message;
        message->flags = zge::kMessageNow;
        message->type = kDisableMessage;
        message->target = object;
        message->sender = 0;
        message->data = 0;

        const uint32_t type = zge::levelObjectTypeHash(object);
        if (type == kPickupType)
        {
            const ee::Ptr<char> kindName = entityKindName(object);
            uint32_t kind = 0;
            while (kind < kPickupKinds &&
                   zge::compareStringsN(kindName, kPickupKindNames[kind].name, kPickupKindNames[kind].length) != 0)
                ++kind;
            if (kind < kPickupKinds)
            {
                const uint32_t name = zge::nameHash(nameWithoutUnderscores(kindName, kPickupKindNames[kind].length).get());
                if (kind == kBikePickup)
                {
                    if (gameMode() == kChallengeMode)
                    {
                        bool known = false;
                        if (!bikePartFoundHere(name, known) && known)
                            message->type = kEnableMessage;
                    }
                }
                else if (kind < kBikePickup)
                {
                    message->type = kEnableMessage;
                }
                else if (kind == kSniperPickup)
                {
                    if (gameMode() == kPaintballMode)
                        message->type = kEnableMessage;
                }
            }
        }
        else if (type == kCheckPointType)
        {
            if (zge::compareStringsN(entityKindName(object), kVanName, kVanNameLength) == 0 && gameMode() == kChallengeMode)
                message->type = kEnableMessage;
        }
        else
        {
            return 1;
        }
        zge::sendEngineMessage(message);
        return 1;
    }
    READABLE_FUNCTION(setUpLevelPickup, Verified);

    namespace
    {
        // The weapon pickups' names: the hashes of the weapon types' names,
        // by type ("None", "Winchester70", "AK47", "M60", "GrenadeLauncher",
        // "RocketLauncher": g_weaponNames).
        const ee::Ptr<uint32_t> kWeaponNameHashes(sym::g_weaponNameHashes);
        // A health or score pickup's number is the rest of its name, in
        // decimal.
        constexpr uint32_t kPickupNumberBase = 10;
        // The reward display's items the pickups show (the string table's
        // $KSTR_REWARD08 .. 10): "<number> COLLECTED!" (the paintball's
        // pickups), "BIKE PART FOUND!" (the part's number), "NEW BIKE
        // COMPLETED!" (the bike's).
        constexpr uint8_t kRewardItemCollected = 8, kRewardItemBikePart = 9, kRewardItemBikeDone = 10;
        // A score pickup's turn score is a run score record ($KSTR_REWARD14
        // "NEW HIGH SCORE!", announced once), as the turn's scoring checks it.
        constexpr uint32_t kRewardRunScore = 14;
        constexpr uint32_t kAnnounceOnce = 0x1;
        // A bike part's byte as a pickup finds it (kBikePartFound and bit 1),
        // then its level's bits.
        constexpr uint8_t kBikePartPickedUp = 0x03;
        constexpr uint8_t kBikePartLevelBits = kBikePartLevelMask << kBikePartLevelShift;
        // The paintball's pickups count only while its turn runs, before
        // the time is out (game_frame.cpp's kPaintballTimeUp stage).
        constexpr uint8_t kPaintballTimeUpStage = 4;
        // The challenge's van checkpoint ends the turn.
    }

    // riderActivatedObject: a rider's activation of a level object (the
    // object's message, its sender the rider's node). The objectives take it
    // first (1 when one did). Else a pickup gives by its kind: HEALTH its
    // number of health, WEAPON the weapon type it names, SCORE its number of
    // points to the turn (a run score record checked), BIKE the bike part
    // it names to the first of the character's four bikes still missing it
    // (shown on the reward display; a bike whose six parts are all found
    // opens the next bike line and the rider rides it), SNIPER one more
    // pickup collected in the paintball's running turn for the current
    // player. A pickup taken is sent its activation (1). The van checkpoint
    // ends the challenge's turn (1). Anything else: 0.
    uint32_t riderActivatedObject(ee::Ptr<zge::EngineMessage> activation)
    {
        ee::Local<RewardItemMessage> reward;

        const ee::Ptr<rider::Rider> rider = rider::riderOf(rider::playerIndexOfNode(activation->sender));
        const ee::Ptr<zge::SceneObject> object(activation->target);
        const uint8_t player = rider->player;
        rider::ScoreBlock &score = rider->score;
        if (objectives::sendObjectActivatedEvent(player, object, activation) != 0)
            return 1;
        if (activation->type != kActivateMessage)
            return 0;

        if (zge::levelObjectTypeHash(object) == kPickupType)
        {
            const ee::Ptr<char> kindName = entityKindName(object);
            uint32_t kind = 0;
            while (kind < kPickupKinds &&
                   zge::compareStringsN(kindName, kPickupKindNames[kind].name, kPickupKindNames[kind].length) != 0)
                ++kind;
            if (kind >= kPickupKinds)
                return 0;
            const ee::Ptr<char> rest = nameWithoutUnderscores(kindName, kPickupKindNames[kind].length);
            const uint32_t name = zge::nameHash(rest.get());
            switch (kind)
            {
            case kHealthPickup:
            {
                const uint32_t health = rider->health + static_cast<uint32_t>(zge::stringToInt(rest, kPickupNumberBase));
                rider->health = static_cast<uint8_t>(health < kFullHealth + 1u ? health : kFullHealth);
                break;
            }
            case kWeaponPickup:
                for (uint32_t type = 0; type < kWeaponTypes; ++type)
                {
                    if (kWeaponNameHashes[type] == name)
                    {
                        const ee::Ptr<Shooter> shooter(rider->shooter);
                        shooter->owned |= 1u << type;
                        break;
                    }
                }
                break;
            case kScorePickup:
            {
                const uint32_t points = static_cast<uint32_t>(zge::stringToInt(rest, kPickupNumberBase));
                score.turnScore = score.turnScore + zge::unsignedToFloat(points);
                checkGameRecord(player, kRecordRunScore, kRewardRunScore, kAnnounceOnce, score.turnScore);
                break;
            }
            case kBikePickup:
            {
                const ee::Ptr<RiderCharacter> character = playerCharacter(player);
                uint32_t part = 0;
                while (part < kBikeParts && kBikePartNames[part] != name)
                    ++part;
                if (part >= kBikeParts)
                    break;
                uint32_t bike = 0;
                while (bike < kCharacterBikes && (character->bikeParts[bike][part] & kBikePartFound) != 0)
                    ++bike;
                if (bike >= kCharacterBikes)
                    break;
                uint8_t &found = character->bikeParts[bike][part];
                found |= kBikePartPickedUp;
                found = static_cast<uint8_t>((found & ~kBikePartLevelBits) | (*g_currentLevel & kBikePartLevelMask) << kBikePartLevelShift);
                character->bikePartsFound = character->bikePartsFound + 1;
                reward->kind = kRewardItemBikePart;
                reward->player = player;
                reward->value = zge::unsignedToFloat(part);
                overlayMessage(kOverlayReward, reward);

                uint32_t partsFound = 0;
                while (partsFound < kBikeParts && (character->bikeParts[bike][partsFound] & kBikePartFound) != 0)
                    ++partsFound;
                if (partsFound < kBikeParts)
                    break;
                // The bike is whole: the next line of bikes opens and the
                // rider rides the newest (the original asks for the
                // character again at each step).
                reward->player = player;
                reward->kind = kRewardItemBikeDone;
                reward->value = zge::unsignedToFloat(bike);
                overlayMessage(kOverlayReward, reward);
                const ee::Ptr<RiderCharacter> owner = playerCharacter(player);
                const uint32_t bikes = bike + 1;
                owner->bikes = static_cast<uint8_t>(bikes < playerCharacter(player)->bikes ? playerCharacter(player)->bikes : bikes);
                playerCharacter(player)->flags |= kCharacterWonBike;
                const ee::Ptr<RiderCharacter> rides = playerCharacter(player);
                rides->bikeChoice = static_cast<uint8_t>(playerCharacter(player)->bikes - 1);
                const uint8_t line = playerCharacter(player)->bike;
                rider::setRiderBikeStats(rider, line, playerCharacter(player)->bikeChoice);
                break;
            }
            case kSniperPickup:
                if (gameMode() == kPaintballMode && roundPhase() == kTurnRunningPhase && g_round->turnEndStage < kPaintballTimeUpStage &&
                    player == *g_currentPlayer)
                {
                    score.turnScore = score.turnScore + 1.0f;
                    reward->kind = kRewardItemCollected;
                    reward->player = player;
                    reward->value = score.turnScore;
                    overlayMessage(kOverlayReward, reward);
                }
                break;
            default:
                break;
            }
            zge::sendEngineMessage(activation);
            return 1;
        }
        if (zge::levelObjectTypeHash(object) == kCheckPointType)
        {
            if (zge::compareStringsN(entityKindName(object), kVanName, kVanNameLength) != 0)
                return 0;
            if (gameMode() != kChallengeMode)
                return 0;
            setRoundFlags(kTurnEndedFlag, 1);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(riderActivatedObject, Verified);

    // riderHitAllowed (inferred name): riderHitByProjectile goes on only
    // when this says 1. A projectile meeting a rider counts unless the rider
    // is the shooter's own; a hit (and a blast, not on the shooter's own
    // rider) counts but in PAINTBALL's running turn on the current player's
    // rider (one without kFlagNoPlayer) while the turn's end stage is 0 or 3.
    uint32_t riderHitAllowed(uint8_t player, uint32_t unused, const ProjectileMessage *message)
    {
        switch (message->event)
        {
        case kProjectileMeetsRider:
            return static_cast<uint8_t>(message->subject) != riderIndexOf(player) ? 1u : 0u;
        case kProjectileBlastReachesRider:
            if (static_cast<uint8_t>(message->subject) == riderIndexOf(player))
                return 0;
            [[fallthrough]];
        case kProjectileHitsRider:
        {
            const ee::Ptr<rider::Rider> hit = rider::riderOf(static_cast<uint8_t>(message->subject));
            if (gameMode() != kPaintballMode)
                return 1;
            if (roundPhase() != kTurnRunningPhase)
                return 1;
            if (rider::hasFlag(hit.get(), rider::kFlagNoPlayer))
                return 1;
            if (hit->player != *g_currentPlayer)
                return 1;
            const uint8_t stage = g_round->turnEndStage;
            if (stage == kTurnRiding || stage == kTurnAtRest)
                return 0;
            return 1;
        }
        default:
            return 1;
        }
    }
    READABLE_FUNCTION(riderHitAllowed, Verified);
}
