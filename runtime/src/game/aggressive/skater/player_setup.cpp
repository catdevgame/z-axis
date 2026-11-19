// A player's life in a level: the skater's motions and profile, the set-up
// at the level's start, its removal, the respawn at a locator, and the
// animation events a skater's motions carry.
//

#include "moves.h"
#include "skater.h"
#include "zge/memory/regions.h"

#include "aggressive/camera.h"
#include "aggressive/globals.h"
#include "aggressive/hud/hud.h"
#include "aggressive/objects.h"

#include "aggressive/game_state.h"
#include "zge/scene/animator.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/collision/sweep.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/collision/collision.h"
#include "zge/collision/dynamics.h"
#include "action_tricks.h"
#include "trick_objects.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/locators.h"
#include "aggressive/level/level_collision.h"
#include "aggressive/sound/events.h"
#include "aggressive/pads.h"
#include "aggressive/fx/library.h"
#include "aggressive/session.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/skater/player_setup.cpp.h"

namespace aggressive
{
}

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kFindLocator = sym::findLocator;       // (near *, name hash, matrix *) -> 1 when the level has one
        constexpr uint32_t kLocatorKind = sym::g_textSpawn;       // their kind's name
        // A level's locator: its place and orientation.
        struct Locator
        {
            Vec4 position;
            zge::Quaternion orientation; // +0x10
        };
        // findLocators' list.
        struct LocatorEntry
        {
            ee::Ptr<LocatorEntry> next;
            uint32_t _04;
            ee::Ptr<Locator> locator; // +0x08
        };
        struct alignas(16) LocatorList
        {
            ee::Ptr<LocatorEntry> first;
        };
        inline ee::Global<ee::Float, sym::g_floatMax> g_floatMax;
        constexpr uint32_t kMemoryFill = sym::memoryFill;        // (dst, byte, bytes)

        constexpr uint32_t kSkaterMotions = sym::skaterMotions;     // "assets\skaters\motions.zal"
        constexpr uint32_t kPlayerOne = sym::playerOne;         // "PlayerOne"
        constexpr uint32_t kPlayerTwo = sym::playerTwo;         // "PlayerTwo"
        constexpr uint32_t kRespawnLocator = sym::respawnLocator;    // "Respawn"
        constexpr uint32_t kIdleAnimation = sym::idleAnimation;     // "global_assets.cedwards.idle"
        constexpr uint32_t kStatStarts = sym::statStarts;        // the skaters' stats at the start, seven bytes each
        constexpr uint32_t kSkaterNames = sym::skaterNames;       // 16 bytes each
        constexpr uint32_t kChunkFormMorf = 0x4D524F46u;     // 'MORF'
        constexpr uint32_t kProfileBytes = 0x2A0;
        constexpr uint16_t kFirstEditorPieceLevels = 0x80;

        // Another skater closer than this (squared) to a respawn's place:
        // the other player's locator is taken.
        inline constexpr float kCloseSquared = 4.0f;

        inline ee::Global<ee::Float, sym::g_ragdollRestitution> g_ragdollRestitution;
        inline ee::Global<uint32_t, sym::g_msgTrickFinished> g_msgTrickFinished;

        // The animation events a skater's motions carry: this kind, with
        // the event's number under this key.
        constexpr uint32_t kSkaterEventKind = 0x068B10D7u;
        constexpr uint32_t kSkaterEventNumber = 0x0619FB37u;
        enum SkaterEvent : uint32_t
        {
            kEventStartHolding = 2,
            kEventStoppedHolding = 3,
            kEventEndedFirst = 4,
            kEventEndedSecond = 5,
            kEventBleedFirst = 10, // twelve: the points of kBleedPoints
            kEventBleedLast = 21,
            kEventHookFirst = 22,
            kEventHookSecond = 23,
            kEventWarningFirst = 30,
            kEventWarningSecond = 31,
        };
        constexpr uint32_t kBleedPoints[12] = {0, 4, 8, 1, 5, 9, 2, 6, 3, 7, 10, 11};
        constexpr uint32_t kSoundHit = 30, kSoundWarning = 44;
    }

    // loadSkaterMotions: the skater's motions (the zal), then the game's object
    // classes.
    int32_t loadSkaterMotions()
    {
        int32_t result = zge::loadAnimationArchive(kSkaterMotions);
        if (result != 0)
            result = skaterSetup();
        return result;
    }
    READABLE_FUNCTION_WAITS(loadSkaterMotions, Verified);

    // freeSkaterClasses: at the level's end, the object classes loadSkaterMotions
    // made.
    void freeSkaterClasses()
    {
        freeGameObjectClasses();
    }
    READABLE_FUNCTION_WAITS(freeSkaterClasses, Verified);

    // resetPlayerProfile: a profile as a new game has it: the skater's starting
    // stats (the table's), its name, and nothing done. Returns 1.
    uint32_t resetPlayerProfile(ee::Ptr<SkaterProfile> profile, uint32_t skaterArgument)
    {
        const uint32_t skater = skaterArgument & 0xFFu;
        profile->index = static_cast<uint8_t>((profile->index & 0x80u) | (skater & 0x1Fu));
        profile->bytes = kProfileBytes;
        profile->experienceMultiplier = 1;
        const ee::Ptr<uint8_t> starts = ee::Ptr<uint8_t>(kStatStarts) + skater * kStatCount;
        for (uint32_t stat = 0; stat < kStatCount; ++stat)
        {
            StatRecord &record = profile->stats[stat];
            const uint8_t start = starts[stat];
            record.id = static_cast<uint8_t>(stat);
            record.bonus = 0;
            record.level = start;
            record.total = start;
            record.points = 0;
            record.totalF = ee::toFloat(static_cast<int32_t>(record.total));
        }
        for (uint32_t level = 0; level < kProfileLevels; ++level)
        {
            LevelRecord &record = profile->levels[level];
            record.challengePoints = 0;
            record.goalsDone = 0;
            record.resetWord = 0;
            record.collected = 0;
            record.rewards = 0;
            record.score = 0;
            record.rollovers = 0;
            record.continues = 0;
            record.secondsPlayed = ee::Float(0.0f);
        }
        profile->editorPieceLevels = kFirstEditorPieceLevels;
        std::memset(&profile->completion, 0, offsetof(SkaterProfile, statTotal) - offsetof(SkaterProfile, completion));
        profile->statTotal = 0;
        profile->level = 0;
        profile->levelUps = 0;
        zge::stringCopy(ee::Ptr<char>(profile->name), kSkaterNames + (skater << 4));
        return 1;
    }
    READABLE_FUNCTION(resetPlayerProfile, Verified);

    // debugMenuHook: empty in this build (the player's set-up and the debug
    // menu's select call it).
    void debugMenuHook()
    {
    }
    READABLE_FUNCTION(debugMenuHook, Verified);

    // setupSkaterForRun: a player at the level's start: the physics reset below
    // the world, its rigid body from the skater's dynamics file, the rolling
    // sound, the controls, and empty object slots. Returns 1.
    uint32_t setupSkaterForRun(ee::Ptr<Player> player)
    {
        ee::Local<uint32_t> bytesOut;
        player->active = 0;
        player->respawnLocator = 0;
        player->drawnPosition.x = ee::Float(0.0f);
        player->drawnPosition.z = ee::Float(0.0f);
        player->drawnPosition.y = ee::Float(-10.0f);
        resetSkaterPhysics(player, 1);
        player->position = player->drawnPosition;
        clearCarriedAnimals(player); // clearCarriedAnimals
        player->ragdoll = 0;
        const uint32_t dynamics = aggressive::loadSkaterDynamics(bytesOut);
        const uint32_t bytes = *bytesOut;
        if (dynamics != 0)
        {
            const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(dynamics, bytes);
            const ee::Ptr<zge::ChunkReader> chunks = zge::openChunkFile(file, kChunkFormMorf, 0);
            player->ragdoll = 0;
            zge::parseChunks(chunks, &player->ragdoll);
            zge::closeChunkFile(chunks);
            zge::closeFile(file, 0);
            zge::heapFree(dynamics);
        }
        zge::setDynamicsRestitution(ee::Ptr<zge::Dynamics>(player->ragdoll), *g_ragdollRestitution);
        player->box = 0;
        player->rollSound = static_cast<uint32_t>(startRollSound());
        player->splashed = 0;
        player->wallTapping = 0;
        player->startTrails = 0;
        for (uint32_t zone = 0; zone < 10; ++zone)
            player->zones[zone] = 0;
        aggressive::resetControls(&player->controls, player->number);
        debugMenuHook();
        zge::memoryFill(&player->objectSlots[0], 0, 0x20);
        const auto newSlot = [&]() {
            return static_cast<uint32_t>(createSlot(player));
        };
        player->objectSlots[0] = newSlot();
        player->objectSlots[1] = newSlot();
        player->objectSlots[2] = newSlot();
        player->objectSlots[3] = newSlot();
        const uint32_t currentSlot = newSlot();
        player->hasCurrentObject = 0;
        player->spinsClockwise = 0;
        player->spinsCounterClockwise = 0;
        player->currentObjectSlot = currentSlot;
        zge::memoryFill(&player->hud, 0, 36);
        player->upDownAnimation = 0;
        player->bodyAnimation = 0;
        player->bobAnimation = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupSkaterForRun, Verified);

    // stepAnimators: the animators' frame, for the motion viewer. 1: the object walk that calls it goes on.
    int32_t stepAnimators(ee::Float seconds)
    {
        zge::updateAnimators(seconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(stepAnimators, Verified);

    // playSkaterAnimation: a motion on the skater, by name, blended in (the
    // motion viewer).
    uint32_t playSkaterAnimation(ee::Ptr<Player> player, uint32_t name, ee::Float blend)
    {
        return zge::playAnimationNamed(player->node, name, 5, blend, ee::Float(-1.0f));
    }
    READABLE_FUNCTION_WAITS(playSkaterAnimation, Verified);

    // removePlayer: a player out of the level: its box, rolling sound, bone
    // animations, score and object slots let go.
    void removePlayer(ee::Ptr<Player> player)
    {
        player->active = 0;
        if (*g_levelWord == static_cast<int32_t>(kLevelTutorial))
            aggressive::fx::releaseSkaterReflection(player);
        if (player->box != 0)
        {
            const ee::Ptr<zge::Sweep> sweep = levelSweep();
            zge::removeSweepBody(sweep, ee::Ptr<zge::SweepBox>(player->box));
            player->box = 0;
        }
        stopRollSound(player->rollSound);
        const auto freeAnimation = [&](ee::Ptr<zge::AnimateSystem> &animation) {
            if (animation)
            {
                zge::freeAnimateSystem(animation);
                animation = {};
            }
        };
        freeAnimation(player->bodyAnimation);
        freeAnimation(player->bobAnimation);
        freeAnimation(player->upDownAnimation);
        freeSkaterScore(player);
        const auto destroy = [&](uint32_t &slot) {
            destroySlot(slot);
            slot = 0;
        };
        destroy(player->objectSlots[0]);
        destroy(player->objectSlots[1]);
        destroy(player->objectSlots[2]);
        destroy(player->objectSlots[3]);
        destroy(player->currentObjectSlot);
    }
    READABLE_FUNCTION_WAITS(removePlayer, Verified);

    // placeSkaterAtLevelStart: the skater back at a locator (`locator`, a name hash;
    // the flags' kRespawnAtLocator decides, and in split screen the other
    // player being within two metres): its trick and object cleared, the
    // pad rumbling once, the physics reset, standing at the locator's place
    // and turned round, the camera cutting there, idle. `resetTricks` when
    // the tricks start again too.
    int32_t findLocator(ee::Ptr<Vec4> position, uint32_t nameHash, ee::Ptr<zge::Matrix> matrix);

    void placeSkaterAtLevelStart(ee::Ptr<Player> player, uint32_t locator, uint32_t resetTricks)
    {
        ee::Local<zge::Matrix> placeMatrix;
        ee::Local<zge::Matrix> turned;
        ee::Local<Vec4> apart;
        ee::Local<uint8_t[0x80]> result;
        uint32_t startName = locator;
        const Vec4 &place = row(*placeMatrix, 3);

        if (resetTricks != 0)
            resetPlayerTricks(player);
        const ee::Ptr<GameObject> object = skaterCurrentObject(player);
        if (object != 0)
        {
            sendObjectMessage(ee::Ptr<GameObject>(object), *g_msgTrickFinished, player, result);
            player->hasCurrentObject = 0;
        }
        aggressive::vibrate(&player->controls, 5, ee::Float(0.0f), ee::Float(0.1f));
        player->respawnLocator = 0;
        zge::setSceneObjectShown(player->node, 1);
        resetSkaterPhysics(player, player->flags & kRespawnAtLocator);
        player->fakie = 0;
        player->drawnRefresh = 1;
        bankTrickScore(player);

        if (*g_gameType == GameType::SplitScreen)
        {
            // The other player on the spot: this one goes to its own start,
            // or if that one is taken too the other start.
            const Player &other = currentLevel()->players[player->number < 1 ? 1 : 0];
            *apart = player->position - other.position;
            if (zge::squaredLength(*apart) < ee::Float(kCloseSquared))
            {
                player->flags |= kRespawnAtLocator;
                startName = zge::nameHash(ee::Ptr<const char>(kPlayerOne));
                if (findLocator(&player->drawnPosition, startName, placeMatrix) != 0)
                {
                    *apart = player->position - place;
                    if (zge::squaredLength(*apart) < ee::Float(kCloseSquared))
                        startName = zge::nameHash(ee::Ptr<const char>(kPlayerTwo));
                }
            }
        }
        if (player->flags & kRespawnAtLocator)
        {
            if (findLocator(&player->drawnPosition, startName, placeMatrix) != 0)
            {
                player->position = place;
                probeGround(player);
                player->orientation = *placeMatrix;
                player->orientation.m[1][1] = -player->orientation.m[1][1];
                player->velocity = zge::kZero;
                *turned = player->orientation;
                vec3Negate(row(*turned, 0), row(*turned, 0));
                vec3Negate(row(*turned, 1), row(*turned, 1));
                vec3Negate(row(*turned, 2), row(*turned, 2));
                zge::matrixToQuaternion(&player->drawnOrientation, turned);
                zge::setObjectOrientation(player->node, &player->drawnOrientation);
                nodeToPosition(player);
                player->flags &= ~kRespawnAtLocator;
            }
            changeState(player, State::Stand);
            (*g_followCameras)[player->number].snap = 1;
        }
        else
            changeState(player, State::Stand);
        loseTrickScore(player);
        hud::hideGrindMeter(player);
        hud::hideBalanceMeter(player);
        zge::playAnimationNamed(player->node, kIdleAnimation, 4, ee::Float(0.0f), ee::Float(-1.0f));
        zge::updateAnimators(ee::Float(0.0f));
        zge::updatePose(player->node);
        player->splashed = 0;
        aggressive::fx::clearTrailsAfterRespawn();
        if (player->bodyAnimation != 0)
            zge::resetAnimatePoints(player->bodyAnimation);
        if (player->bobAnimation != 0)
            zge::resetAnimatePoints(player->bobAnimation);
        if (player->upDownAnimation != 0)
            zge::resetAnimatePoints(player->upDownAnimation);
    }
    READABLE_FUNCTION_WAITS(placeSkaterAtLevelStart, Verified);

    // respawn: the skater to its start (`atStart`: "PlayerOne" or
    // "PlayerTwo"), else the locator the level gave it, else "Respawn".
    void respawn(ee::Ptr<Player> player, uint32_t atStart, uint32_t resetTricks)
    {
        uint32_t locator;
        if ((atStart & 0xFFu) != 0)
        {
            if (player->number == 0)
                locator = zge::nameHash(ee::Ptr<const char>(kPlayerOne));
            else
                locator = zge::nameHash(ee::Ptr<const char>(kPlayerTwo));
        }
        else
        {
            locator = player->respawnLocator;
            if (locator == 0)
                locator = zge::nameHash(ee::Ptr<const char>(kRespawnLocator));
        }
        game::tail(aggressive::skater::placeSkaterAtLevelStart, player, locator, resetTricks); // placeSkaterAtLevelStart
    }
    READABLE_FUNCTION_WAITS(respawn, Verified);

    // nodeToPosition: the scene node to the position, drawn there now.
    void nodeToPosition(ee::Ptr<Player> player)
    {
        player->drawnPosition = player->position;
        zge::setObjectPosition(player->node, &player->drawnPosition);
        game::tail(zge::updatePose, player->node);
    }
    READABLE_FUNCTION_WAITS(nodeToPosition, Verified);

    // skaterAnimationEvent: an animation event on a skater (animations.cpp's
    // handleAnimationEvent): a held trick's start and stop, the animation's
    // end, blood at a point with a thud, and a warning sound.
    void skaterAnimationEvent(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time)
    {
        const uint32_t clock = zge::collisionClock();
        if (zge::eventKind(event) != kSkaterEventKind)
            return;
        const uint32_t number = zge::eventInt(event, kSkaterEventNumber);
        const auto splashed = [&] {
            if (player->splashed == 0)
                return false;
            return true;
        };
        switch (number)
        {
        case kEventStartHolding:
            game::tail(sendTrickStartHolding, player, event, time);
            return;
        case kEventStoppedHolding:
            game::tail(sendTrickStoppedHolding, player, event, time);
            return;
        case kEventEndedFirst:
        case kEventEndedSecond:
            game::tail(sendAnimationEnded, player, event, time);
            return;
        case kEventHookFirst:
        case kEventHookSecond:
        {
            const bool second = number == kEventHookSecond;
            if (splashed())
                return;
            game::tail(aggressive::fx::animationEventHook, player, second ? 11 : 10);
            return;
        }
        case kEventWarningFirst:
        case kEventWarningSecond:
            game::tail(aggressive::soundEvent, kSoundWarning, player->number, 0, 0, 0);
            return;
        default:
            break;
        }
        if (number >= kEventBleedFirst && number <= kEventBleedLast)
        {
            if (splashed())
                return;
            aggressive::fx::bleed(player, kBleedPoints[number - kEventBleedFirst]);
            game::tail(aggressive::soundEvent, kSoundHit, &player->position, clock & 3u, (clock >> 4) & 3u, 0);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(skaterAnimationEvent, Verified);

    // playerIndexOfNode: which player's skater a node is, 255 for none.
    uint32_t playerIndexOfNode(ee::Ptr<zge::SceneObject> node)
    {
        const ee::Ptr<Level> level = currentLevel();
        for (uint32_t index = 0; index < 2; ++index)
        {
            if (level->players[index].node == node)
                return index;
        }
        return 255;
    }
    READABLE_FUNCTION(playerIndexOfNode, Verified);

    // findLocator: the level's nearest locator of this name to `position`:
    // its orientation and place in `matrix`. 0 when the level has none.
    int32_t findLocator(ee::Ptr<Vec4> position, uint32_t nameHash, ee::Ptr<zge::Matrix> matrix)
    {
        ee::Local<LocatorList> list;
        ee::Local<Vec4> toLocator;
        ee::Float nearest = *g_floatMax;
        ee::Ptr<LocatorEntry> best;
        const int32_t kind = zge::nameHash(ee::Ptr<const char>(kLocatorKind));
        zge::findLocators(list, kind, nameHash);
        for (ee::Ptr<LocatorEntry> entry = list->first; entry; entry = entry->next)
        {
            *toLocator = *position - entry->locator->position;
            const ee::Float squared = zge::squaredLength(*toLocator);
            if (squared < nearest)
            {
                nearest = squared;
                best = entry;
            }
        }
        if (!best)
        {
            zge::freeLocators(list);
            return 0;
        }
        zge::matrixIdentity(matrix);
        zge::quaternionToMatrix(matrix, &best->locator->orientation);
        row(*matrix, 3) = best->locator->position;
        zge::freeLocators(list);
        return 1;
    }
    READABLE_FUNCTION_WAITS(findLocator, Verified);
}
