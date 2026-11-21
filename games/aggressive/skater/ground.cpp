// The Ground state's physics for a frame (groundPhysics, the state table's
// second callback for Ground, Stand, Air QPipe, Manual and Cess Slide):
// push or brake, gravity along the slope, friction, and standing still.
//
// Vectors built by scaling keep the w of the last vector operation, and those
// lanes end in the velocity: the order of the vector statements is the game's.

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sym/aggressive/skater/ground.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kSurfaceWithGravity = 3; // surfacePhysics: a slope with its own gravity
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp; // (0, -1, 0, 0): y grows downward

        // Physics tuning (DEVTOOLS.md).
        ee::Float physics(uint32_t entry)
        {
            return (*g_tuningCategories)[3].entries[entry].value;
        }
        enum PhysicsEntry : uint32_t
        {
            kGravity = 0,
            kAcceleration = 1,
            kForwardFriction = 5,
            kOverForwardFriction = 6,
            kSideFriction = 7,
            kGravityHillFactor = 26,
            kBrakeRate = 27,
            kIdleVelocity = 28,
            kAccelSpeedDiff = 29,
            kCantStandAngle = 40,
        };

    }

    void groundPhysics(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> push;
        ee::Local<Vec4> forwardFriction;
        ee::Local<Vec4> sideFriction;
        ee::Local<Vec4> intoGround;

        Vec4 &velocity = player->velocity;
        const PlayerAxes &axes = player->axes;
        ee::Float &pushSpeed = player->pushSpeed; // the speed the skater pushes to
        uint8_t &pushing = player->pushing;

        if (player->speed < pushSpeed)
        {
            // Pushing: Acceleration along the skater's forward axis.
            *push = axes.forward * (frameTime * physics(kAcceleration));
            velocity = velocity + *push;
            newSpeed(*player);
            // Still far from the push speed (Accel Speed Diff): marked pushing.
            if (physics(kAccelSpeedDiff) < pushSpeed - player->speed || pushing)
                pushing = 1;
            else
                pushing = 0;
        }
        else
        {
            pushing = 0;
            // Down and faster than the push speed: Brake Rate against the motion.
            if (pushSpeed < player->speed && (player->flags & kDown))
            {
                *push = zge::unit(velocity);
                *push = *push * (frameTime * physics(kBrakeRate));
                velocity = velocity - *push;
                newSpeed(*player);
            }
        }

        // Gravity: straight down by Gravity, or the slope's own; the part
        // along the ground kept by Gravity Hill Factor.
        ee::Float pull;
        if (player->object && surfacePhysics(player->objectNode, player->surface) == kSurfaceWithGravity)
        {
            pull = surfaceGravity(player->objectNode, player->surface);
            pull = pull * frameTime;
        }
        else
            pull = physics(kGravity) * frameTime;
        Vec4 gravity = Vec4{0.0f, 1.0f, 0.0f, 1.0f} * pull;

        const ee::Float down = zge::dot(axes.up, gravity);
        // Into the ground: along up on flat ground, along the surface's own
        // normal on an object.
        const Vec4 &normal = player->object ? player->surfaceNormal : axes.up;
        *intoGround = normal * down;
        Vec4 alongGround = gravity - *intoGround;
        alongGround = alongGround * physics(kGravityHillFactor);
        gravity = alongGround + *intoGround;
        velocity = velocity + gravity;

        // Friction: Side Friction across, Forward Friction (Over Forward
        // Friction past the top speed) along.
        const ee::Float across = zge::dot(velocity, axes.right);
        *sideFriction = axes.right * (physics(kSideFriction) * across);
        const ee::Float along = zge::dot(velocity, axes.forward);
        const ee::Float topSpeed = skaterStat(player, kStatSpeed, 0);
        const ee::Float forwardRate = topSpeed < player->speed ? physics(kOverForwardFriction) : physics(kForwardFriction);
        *forwardFriction = axes.forward * (forwardRate * along);
        const Vec4 friction = *sideFriction + *forwardFriction;
        velocity = velocity - friction;
        newSpeed(*player);

        // Slower than Idle Velocity, not pushing and not on too steep a
        // slope (up within Cant Stand Angle of world up): the skater stands.
        if (player->speed < physics(kIdleVelocity) && player->state != State::Stand && pushSpeed == 0.0f &&
            physics(kCantStandAngle) < zge::dot(axes.up, *g_worldUp) && player->state != State::CessSlide)
            changeState(player, State::Stand);
    }
    READABLE_FUNCTION_VU0(groundPhysics, Verified);
}
