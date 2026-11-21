// The Ground state's controls for a frame (groundControls, the state table's
// fifth callback for Ground, Stand and Cess Slide, not while down):
// pushing raises the speed the skater pushes to (Acceleration, up to the
// skater's speed stat), braking takes Brake Rate off the forward speed,
// a jump launches the skater (its sound by the surface), and the stick
// steers (Turn Rate, or Slide Turn Rate with a shoulder button, which
// also brakes by Slide Turn Brake; fakie scales both by a stat).
//
// Its vector code stores vectors whose w it never set: what the last vector
// operation left. It reaches the velocity's w, and what this leaves is the
// next function's stale lane, so it runs on VU0 as the original does.

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "aggressive/sound/events.h"
#include "sym/aggressive/skater/ground_controls.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // skaterStat's stats (experience.cpp), with `use` 0: the jump speed,
        // the top pushing speed, how riding fakie multiplies the turn.
        constexpr int32_t kJumpSound = 0x19;
        constexpr uint32_t kNoSurface = 0xFFFFFFFFu;
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp; // (0, -1, 0, 0): y grows downward

        enum PhysicsEntry : uint32_t
        {
            kAcceleration = 1,
            kTurnRate = 20,
            kSlideTurnRate = 22,
            kSlideTurnBrake = 23,
            kBrakeRate = 27,
        };
        ee::Float physics(uint32_t entry)
        {
            return (*g_tuningCategories)[3].entries[entry].value;
        }
    }

    void groundControls(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> change;
        Vec4 &velocity = player->velocity;
        const Vec4 &forward = player->axes.forward;
        ee::Float &pushSpeed = player->pushSpeed; // the speed the skater pushes to
        uint8_t &pushing = player->pushing;

        const auto stat = [&](int32_t which) {
            return skaterStat(player, which, 0);
        };
        // velocity -= along * amount, if the forward speed is larger; else stopped.
        const auto brake = [&](ee::Float amount) {
            if (amount < zge::dot(forward, velocity))
            {
                *change = zge::scaledAfterMeasure(forward, amount);
                velocity = velocity - *change;
                return true;
            }
            velocity = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
            player->speed = 0.0f;
            return false;
        };

        player->flags &= ~kBraking;

        if (player->controls.actions & kActionPush)
        {
            const ee::Float topSpeed = stat(kStatSpeed);
            if (pushSpeed < topSpeed)
            {
                pushing = 1;
                pushSpeed = pushSpeed + physics(kAcceleration) * frameTime;
            }
        }

        if (player->controls.actions & kActionBrake)
        {
            if (brake(physics(kBrakeRate) * frameTime))
                player->flags |= kBraking;
            newSpeed(*player);
            if (player->speed < pushSpeed)
                pushSpeed = player->speed;
        }

        if (player->controls.actions & kActionJump)
        {
            *change = *g_worldUp * stat(kStatJump);
            velocity = velocity + *change;
            player->settleFrames = 0;
            leaveGround(player);
            if (player->state != State::Ground)
                return;

            // The jump's sound: the surface's, louder with speed (64 + 4 a unit, to 127).
            const ee::Ptr<zge::Node> object = player->objectNode;
            const uint32_t surface = player->surface;
            int32_t sound = 0;
            if (object && surface != kNoSurface)
                sound = surfaceSound(object, surface);
            ee::Float volume = player->speed * ee::Float(4.0f) + ee::Float(64.0f);
            if (ee::Float(127.0f) < volume)
                volume = 127.0f;
            aggressive::soundEvent(kJumpSound, &player->position, sound, ee::toInt(volume), 0);
        }

        // Steering.
        const ee::Float steer = player->controls.stickX;
        if (steer == 0.0f)
        {
            player->turnRate = 0.0f;
            return;
        }
        if (!(player->controls.actions & (kActionButton100 | kActionButton200)))
        {
            player->turnRate = player->fakie ? steer * (physics(kTurnRate) * stat(kStatFakie))
                                              : (steer * physics(kTurnRate));
            return;
        }
        // A sliding turn (a shoulder button held) brakes too.
        player->turnRate =
            player->fakie ? steer * (physics(kSlideTurnRate) * stat(kStatFakie))
                          : (steer * physics(kSlideTurnRate));
        if (brake(physics(kSlideTurnBrake) * frameTime))
            newSpeed(*player);
        if (player->speed < pushSpeed)
            pushSpeed = player->speed;
    }
    READABLE_FUNCTION(groundControls, Verified);
}
