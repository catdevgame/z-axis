// The bail: a crash (skaterCrash) puts the skater down (setSkaterDown),
// with its message, rumble and crash animation, and settles the split-screen
// games' scores; the respawn comes later (skaterUpdate, General / Respawn
// Time).

#include "skater.h"

#include "zge/collision/dynamics.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/tuning.h"
#include "zge/scene/animator.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/text/strings.h"
#include "zge/scene/scene.h"
#include "sce/libgcc.h"
#include "moves.h"
#include "zge/math/random.h"
#include "aggressive/card_game.h"
#include "aggressive/pads.h"
#include "aggressive/hud/hud.h"
#include "aggressive/level/animal_rescue.h"
#include "aggressive/level/objectives.h"

#include <cstring>
#include "sym/aggressive/skater/crash.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // How the skater crashed (the second argument): the HUD's message.
        enum CrashKind : uint32_t
        {
            kCrashed = 0,
            kLandedSideways = 1,
            kGreed = 2,
            kCrashedToo = 3,
            kLandedTooHard = 4,
            kCratered = 5,
            kKnockedOut = 6,
            kGoneSwimming = 7,
            kBurnedUp = 8,
            kOutOfBounds = 9,
            kTraffic = 10,
            kCrashKindCount,
        };
        constexpr uint32_t kCrashMessages[kCrashKindCount] = {
            sym::g_textYouCrashed, // "You crashed!"
            sym::g_textYouLandedSideways, // "You landed sideways!"
            sym::g_textGreed, // "Greed!"
            sym::g_textYouCrashed, // "You crashed!"
            sym::g_textYouLandedTooHard, // "You landed too hard!"
            sym::g_textYouCratered, // "You cratered!"
            sym::g_textKnockedOut, // "Knocked out!"
            sym::g_textGoneSwimming, // "Gone Swimming!"
            sym::g_textBurnedUp, // "Burned Up!"
            sym::g_textDonTSkateHere, // "Don't skate here!"
            sym::g_textStopPlayingInTraffic, // "Stop playing in traffic!"
        };

        // "global_assets.cedwards.crashA" .. "crashD", one at random.
        inline ee::Global<uint32_t[4], sym::g_crashAnimations> g_crashAnimations;
        inline ee::Global<ee::Float, sym::g_crashBlendTime> g_crashBlendTime; // 0.3

        // The hips bone's name hash, worked out on the first bail.
        inline ee::Global<uint32_t, sym::g_hipsHashKnown> g_hipsHashKnown;
        inline ee::Global<uint32_t, sym::g_hipsHash> g_hipsHash;
        inline ee::Global<char, sym::g_hipsName> g_hipsName;            // "hips"

        // Set while an Animal Rescue runs outside split screen.
        inline ee::Global<uint32_t, sym::g_animalRescue> g_animalRescue;
        // Vibrate / Crash Big, Crash Small, Crash Time.
        ee::Float vibrate(uint32_t entry)
        {
            return (*g_tuningCategories)[18].entries[entry].value;
        }
        enum VibrateEntry : uint32_t
        {
            kCrashBig = 12,
            kCrashSmall = 13,
            kCrashTime = 14,
        };
    }

    // setSkaterDown: the skater goes down (a bail) or gets up. Going down
    // starts the down time, the skater flies loose (Air) and the model's
    // effects are released. Always 1.
    int32_t setSkaterDown(ee::Ptr<Player> player, uint8_t down)
    {
        if (down)
            player->flags |= kDown;
        else
            player->flags &= ~kDown;
        if (!down)
            return 1;

        if (!*g_hipsHashKnown)
        {
            *g_hipsHash = zge::nameHash(g_hipsName.get());
            *g_hipsHashKnown = 1;
        }
        player->pushSpeed = 0.0f;
        player->downTime = 0.0f;
        player->rates = zge::kZero;
        setSkaterState(player, static_cast<uint32_t>(State::Air));
        zge::stopAnimation(player->node);
        return 1;
    }
    READABLE_FUNCTION(setSkaterDown, Verified);

    namespace
    {
        // Its result is the crash's in the split-screen games.
        int32_t pointsAfterCrash(ee::Ptr<Player> player, int32_t points) { return ee::game<int32_t>(sym::setSkaterPoints, player, points); }

        // What a skater's crash returns where no callee sets it: the jump
        // table the split-screen games go through, or its upper half.
        constexpr int32_t kCrashCaseTable = sym::skaterCrashCases, kCrashCaseTablePage = ee::high(sym::g_textYouLandedSideways);
    }

    // skaterCrash: the skater crashes (kind: CrashKind). Once: a skater
    // already down crashes no more.
    //
    // Callers mostly drop the result, but a handler (skatersCollide, the
    // traffic hit) passes it back as its own: 2 when already down, else the
    // last callee's result or the value the original last loaded (kept
    // exactly, path by path).
    int32_t skaterCrash(ee::Ptr<Player> player, uint32_t kind)
    {
        ee::Local<uint32_t[4]> animations;
        std::memcpy(&animations[0], g_crashAnimations.get(), sizeof(*g_crashAnimations));
        if (player->flags & kDown)
            return static_cast<int32_t>(player->flags & kDown);

        int32_t &crashes = player->crashCount;
        ++crashes;
        endCombo(player);
        setSkaterDown(player, uint8_t(1));
        aggressive::level::objectiveCrash(player, 0);
        loseTrickScore(player);
        setJizzed(player, 0);

        // The message (the jump table); out of the water, the fire
        // or the level the skater respawns at a locator (and the first two
        // hide the model).
        player->flags &= ~kRespawnAtLocator;
        if (kind < kCrashKindCount)
        {
            aggressive::hud::showCrashMessage(kCrashMessages[kind], ee::Float(1.0f));
            if (kind == kGoneSwimming || kind == kBurnedUp)
            {
                player->flags |= kRespawnAtLocator;
                zge::setSceneObjectShown(player->node, 0);
            }
            else if (kind == kOutOfBounds)
                player->flags |= kRespawnAtLocator;
        }

        // Rumble for Crash Time, and one of the four crash animations.
        const int32_t rumbleTime = sce::libgcc::fptoui(vibrate(kCrashTime));
        aggressive::vibrate(&player->controls, rumbleTime, vibrate(kCrashSmall), vibrate(kCrashBig));
        const uint32_t animation = animations[zge::randomNumber() & 3u];
        zge::playAnimationNamed(player->node, animation, 4, *g_crashBlendTime, ee::Float(-1.0f));

        // What a crash costs in the running game.
        const GameType type = *g_gameType;
        if (type == GameType::SplitScreen)
        {
            const uint8_t game = currentLevel()->splitGame;
            switch (game)
            {
            case kSplitGameCollect:
                // Half the points (as unsigned).
                return pointsAfterCrash(player, static_cast<int32_t>(static_cast<uint32_t>(skaterPoints(player)) >> 1));
            case kSplitGameCards:
                aggressive::resetCards(&player->cardHand);
                return pointsAfterCrash(player, player->cardHand.points);
            case kSplitGameAnimalRescue:
                return aggressive::dropAnimals(player, 0);
            default:
                return game < 5 ? kCrashCaseTable : kCrashCaseTablePage;
            }
        }
        const uint32_t animalRescue = *g_animalRescue;
        if (type != GameType::TimedRun && animalRescue)
            return aggressive::dropAnimals(player, 0);
        return static_cast<int32_t>(animalRescue);
    }
    READABLE_FUNCTION_WAITS(skaterCrash, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_tuning> g_tuning; // g_tuning (g_tuningCategories, (anonymous)::g_tuning)
        inline ee::Global<int32_t, sym::g_hipsHash> g_crashHipsHash; // g_hipsHash ((anonymous)::g_hipsHash)
    }

    namespace
    {
        zge::Vec3 * vec3Negate(ee::Ptr<zge::Vec3> x0, ee::Ptr<const zge::Vec3> x02) { return zge::vec3Negate(x0, x02); }
        ee::Float vec3Length(ee::Ptr<const zge::Vec3> velocity) { return zge::vec3Length(velocity); }
    }

    namespace
    {
        constexpr ee::Float kRagdollStep = 0.0085f;  // seconds a step at most
        constexpr ee::Float kRagdollLeast = 0.001f;  // a last step shorter than this is not taken
    }

    // stepRagdoll: the skater's ragdoll moved on `seconds` in steps of at
    // most 0.0085 (a last bit under 0.001 left out), and the skater put
    // where its hips body is: the drawn orientation, the velocity, the
    // drawn and the real position, then the speed. The time down grows by
    // `seconds`; past General / Respawn Time the skater respawns, not at a
    // locator. Nothing calls it (the crash's ragdoll is cut).
    // Kept from the original: a walk to the ragdoll's last body that is not
    // used; the hips' orientation made a matrix whose axes are negated and
    // then not used; with no body called "hips", one at address 0 is read.
    void stepRagdoll(ee::Ptr<Player> player, ee::Float seconds)
    {
        ee::Local<zge::Matrix> hipsMatrix;
        ee::Ptr<zge::Dynamics> ragdoll(player->ragdoll);
        for (ee::Ptr<zge::RigidBody> body = ragdoll->bodies; body; body = body->next)
        {
        }
        if (ee::Float(0.0f) < seconds)
        {
            ee::Float left = seconds;
            do
            {
                if (kRagdollStep < left)
                {
                    left = left - zge::updateDynamics(ee::Ptr<zge::Dynamics>(player->ragdoll), ee::Ptr<zge::Sweep>(), kRagdollStep);
                }
                else if (kRagdollLeast < left)
                {
                    const ee::Float last = left;
                    left = ee::Float(0.0f);
                    zge::updateDynamics(ee::Ptr<zge::Dynamics>(player->ragdoll), ee::Ptr<zge::Sweep>(), last);
                }
                else
                {
                    left = ee::Float(0.0f);
                }
            } while (ee::Float(0.0f) < left);
            ragdoll = ee::Ptr<zge::Dynamics>(player->ragdoll);
        }

        ee::Ptr<zge::RigidBody> hips = ragdoll->bodies;
        while (hips && hips->nameHash != static_cast<uint32_t>(*g_crashHipsHash))
            hips = hips->next;
        zge::quaternionToMatrix(&*hipsMatrix, &hips->orientation);
        for (int axis = 0; axis < 3; ++axis)
            vec3Negate(ee::Ptr<zge::Vec3>(&hipsMatrix->row(axis).xyz()), ee::Ptr<const zge::Vec3>(&hipsMatrix->row(axis).xyz()));
        zge::quaternionCopy(&player->drawnOrientation, &hips->orientation);
        player->velocity = hips->velocity;
        player->drawnPosition = hips->position;
        player->position = hips->position;
        player->previousSpeed = player->speed;
        player->speed = vec3Length(ee::Ptr<const zge::Vec3>(&player->velocity.xyz()));
        player->downTime = player->downTime + seconds;
        if ((*g_tuningCategories)[0].entries[kTuningRespawnTime].value < player->downTime)
        {
            player->flags = player->flags & ~kRespawnAtLocator;
            respawn(player, 0, 0);
        }
    }
    READABLE_FUNCTION_WAITS(stepRagdoll, Verified);

}
