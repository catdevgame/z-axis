// The enters of the two air states over a pipe (skater.h StateRow::enter):
// Air QPipe (3, up a quarter pipe) and Air Spine (4, over a spine: the
// ridge between two quarter pipes back to back). Their per-frame callbacks
// are in air.cpp.
//
// Both start from the lip edge the skater left the face by (the block at
// Player +0x210: its ids, its ends railStart / railEnd and its direction
// railDirection), set up the state's air (the timers, the turn rates, the
// skater's offset from the plane of the lip), play the jump sound and tell
// the face's object "Jump".
//
// The face's surface record has three floats (surfaceGravity / 87470 /
// 874B0, its +0x10 / +0x14 / +0x18; ground.cpp reads the first as a
// slope's gravity). Leaving a pipe's face they shape the launch: the first
// scales the level part of the velocity, and a skater just off the ground
// is pushed up by the third (times how steeply it faces up the face) and
// launched at least at the second (y grows downward, so a cap on y).
//
// A scaled vector's w is whatever the last vector operation left, and it
// reaches the velocity: zge/math/vec.h runs on VU0, so the order of the
// statements below decides it.

#include "moves.h"

#include "aggressive/script/script.h"
#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "moves.h"
#include "aggressive/sound/events.h"
#include "aggressive/level/objectives.h"

#include "sym/aggressive/skater/air_enters.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr int32_t kJumpSound = 0x19;
        constexpr uint32_t kJumpEvent = sym::jumpEvent; // "Jump"
        constexpr uint32_t kNoSurface = 0xFFFFFFFFu;

        // The world's up (0, -1, 0, 0).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        // Slower than this vertically (1e-5) the jump is not timed.
        inline ee::Global<ee::Float, sym::g_stillVertical> g_stillVertical;
        // The closest a spine jump starts to the lip's plane (0.01).
        inline ee::Global<ee::Float, sym::g_minSpineOffset> g_minSpineOffset;
        // Added to twice the offset for the distance a spine jump carries
        // the skater across (0.8).
        inline ee::Global<ee::Float, sym::g_spineClearance> g_spineClearance;

        ee::Float physics(uint32_t entry)
        {
            return (*g_tuningCategories)[3].entries[entry].value;
        }
        enum PhysicsEntry : uint32_t
        {
            kMinAuto180Speed = 24,
            kMaxAuto180Angle = 25,
            kQPipeGravity = 31, // the spine's jump is timed with it too
        };

        bool fromGrindOrStall(uint8_t state)
        {
            return state == static_cast<uint8_t>(State::Stall) || state == static_cast<uint8_t>(State::Grind) ||
                   state == static_cast<uint8_t>(State::HandGrind);
        }

        // Across the pipe, square to the lip and the world's up, on the
        // side the skater's up points to.
        void acrossThePipe(ee::Ptr<Player> player, zge::Vec4 &across)
        {
            across = zge::cross(*g_worldUp, player->railDirection);
            if (zge::dot(player->axes.up, across) < 0.0f)
                zge::vec3Negate(&across.xyz(), &across.xyz());
        }

        // Off a pipe's face (not out of a grind or a stall): its first
        // float scales the level part of the velocity, when it has one.
        void scaleLevelSpeed(ee::Ptr<Player> player, zge::Vec4 &vertical, zge::Vec4 &level)
        {
            if (fromGrindOrStall(player->previousState))
                return;
            const ee::Float scale = surfaceGravity(player->objectNode, player->surface);
            if (scale == 0.0f)
                return;
            zge::Vec4 &velocity = player->velocity;
            vertical = zge::scaledAfterMeasure(*g_worldUp, zge::dot(velocity, *g_worldUp));
            level = velocity - vertical;
            level = level * scale;
            velocity = level + vertical;
            player->previousSpeed = player->speed;
            player->speed = zge::vec3Length(&velocity.xyz());
        }

        // A skater just off the ground (under 10 frames since it was on
        // it) is pushed up by the face's third float times `facingUp`, and
        // launched at least at its second.
        void launchOffFace(ee::Ptr<Player> player, ee::Float facingUp)
        {
            const ee::Float push = surfaceLaunchPush(player->objectNode, player->surface);
            player->velocity.y = player->velocity.y - push * facingUp;
            const ee::Float cap = surfaceLaunchCap(player->objectNode, player->surface);
            if (!(cap == 0.0f))
            {
                if (cap < player->velocity.y)
                    player->velocity.y = cap;
            }
        }

        // The jump sound: the surface's, at `volume`.
        void playJumpSound(ee::Ptr<Player> player, int32_t volume)
        {
            uint32_t sound = 0;
            if (player->object)
            {
                if (player->surface != kNoSurface)
                    sound = surfaceSound(player->objectNode, player->surface);
            }
            aggressive::soundEvent(kJumpSound, &player->position, sound, volume, 0);
        }

        // Unless the skater came out of a grind: the face's object hears
        // "Jump" with the surface's kind, and the watchers of the object it
        // belongs to are marked.
        void tellJump(ee::Ptr<Player> player, uint32_t &kind)
        {
            if (player->previousState == static_cast<uint8_t>(State::Grind))
                return;
            if (!player->object || player->surface == kNoSurface)
                return;
            kind = surfaceKind(player->objectNode, player->surface);
            sendObjectEvent(player, player->objectNode, kJumpEvent, &kind);
            // Read again: the event's handlers may change them.
            const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(ee::Ptr<zge::Node>(player->object), 0);
            if (owner)
            {
                const uint32_t name = zge::objectNameHash(owner);
                aggressive::level::goalJump(name, kind);
            }
        }
    }

    // airQuarterPipeEnter, Air QPipe's enter: the air timers and turn rates
    // cleared; off a pipe's face the launch shaped by the face (see the top
    // of the file); the skater's offset from the lip's plane kept (at least
    // 0); the lip's ids and the level velocity kept; the jump sound, louder
    // with speed (64 + 4 a unit, to 127).
    // The auto 180 (not out of a stall): launched up faster than Min Auto
    // 180 Speed, facing up the face less than Max Auto 180 Angle, the
    // skater turns about its up without a spin pressed (noSpinTurn), through
    // twice its heading's angle above the level (180 degrees heading
    // straight up), over the time the jump will last (2 v / QPipe Gravity
    // plus the time already in the air); the sign from the lip's direction.
    // Otherwise the jump is timed the same, or 0.5 s when it hardly moves
    // up or comes out of a grind or a stall. Then "Jump" (see tellJump).
    // Returns 1.
    int32_t airQuarterPipeEnter(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> across;
        ee::Local<zge::Vec4> heading;
        ee::Local<zge::Vec4> lipPart;
        ee::Local<zge::Vec4> vertical;
        ee::Local<zge::Vec4> acrossAgain;
        ee::Local<zge::Vec4> level;
        ee::Local<zge::Vec4> plane;
        ee::Local<uint32_t> surfaceWord;
        zge::Vec4 &velocity = player->velocity;

        player->pipeHeight = 0.0f;
        player->pipePeakHeight = 0.0f;
        player->rates = zge::kZero;
        player->stateDistance = 0.0f; // the time in this air

        acrossThePipe(player, *across);
        scaleLevelSpeed(player, *vertical, *level);

        // The offset from the lip's plane, not below it.
        zge::planeThrough(plane, &player->railStart, across);
        const ee::Float offset = zge::planeDistance(plane, &player->position);
        player->stateValue = offset;
        if (offset < 0.0f)
            player->stateValue = 0.0f;

        player->launchSegment = player->grindSegment;
        player->launchObject = player->grindRail;
        *vertical = zge::scaledAfterMeasure(*g_worldUp, zge::dot(velocity, *g_worldUp));
        player->launchLevelVelocity = velocity - *vertical;

        // How steeply the skater heads up the face: its heading in the
        // plane square to the lip, against the world's up.
        alongPlane(vertical, across, &player->axes.forward);
        *vertical = zge::unit(*vertical);
        const ee::Float facingUp = zge::dot(*vertical, *g_worldUp);

        ee::Float volume = player->speed * 4.0f + 64.0f;
        if (ee::Float(127.0f) < volume)
            volume = 127.0f;
        playJumpSound(player, ee::toInt(volume));

        const uint8_t before = player->previousState;
        const bool stalled = before == static_cast<uint8_t>(State::Stall);
        if (!stalled)
        {
            if (before != static_cast<uint8_t>(State::Grind))
            {
                if (!(before == static_cast<uint8_t>(State::HandGrind) || !(player->settleFrames < 10)))
                    launchOffFace(player, facingUp);
            }
        }

        const ee::Float up = -velocity.y;
        if (!stalled && physics(kMinAuto180Speed) < up && facingUp < physics(kMaxAuto180Angle))
        {
            player->noSpinTurn = 1;
            const ee::Float airTime = (up + up) / physics(kQPipeGravity) + player->airTime;
            player->expectedAirTime = airTime;

            // The heading less its part along the lip, against straight up.
            *acrossAgain = zge::cross(*g_worldUp, player->railDirection);
            *heading = player->axes.forward;
            *lipPart = zge::scaledAfterMeasure(*acrossAgain, zge::dot(*acrossAgain, *heading));
            *heading = *heading - *lipPart;
            *heading = zge::unit(*heading);
            const ee::Float offUp =
                ee::toFloat(static_cast<short>(zge::angleAcos(zge::dot(*g_worldUp, *heading)))) * zge::kAngleToDegrees;
            const ee::Float turn = 90.0f - offUp;
            ee::Float &turnSpeed = player->turnRate;
            turnSpeed = (turn + turn) / airTime;
            if (0.0f < zge::dot(player->railDirection, *heading))
                turnSpeed = -turnSpeed;
        }
        else
        {
            const bool still = ee::abs(velocity.y) < *g_stillVertical;
            if (still || fromGrindOrStall(before))
                player->expectedAirTime = 0.5f;
            else
            {
                const ee::Float upSpeed = -velocity.y;
                player->expectedAirTime = (upSpeed + upSpeed) / physics(kQPipeGravity) + player->airTime;
            }
            player->noSpinTurn = 0;
        }

        tellJump(player, *surfaceWord);

        player->substate = 1;
        player->lastPipeSegment = 0xFFFFFFFFu;
        player->lastPipeObject = {};
        player->overObject = 0;
        player->overSurfaceKind = 0;
        player->pipeHeight = 0.0f;
        player->pipePeakHeight = 0.0f;
        return 1;
    }
    // Waiting: "Jump" goes to the face's script synchronously, and a
    // handler can wait (as holdEnter's StallStart, stall.cpp).
    READABLE_FUNCTION_WAITS(airQuarterPipeEnter, Verified);

    // airSpineEnter, Air Spine's enter: the air timers and turn rates
    // cleared, no air turn without a spin; the lip turned to run along the
    // skater's right (its ends swapped with it); off a pipe's face the
    // level speed shaped by the face; the offset from the lip's plane kept
    // (at least g_minSpineOffset); the jump sound at full volume; a skater
    // just off the ground pushed and launched by the face.
    // The jump over: timed to its top (v / QPipe Gravity plus the time
    // already in the air), over which the skater rolls about the lip
    // (spineSpeed) by the angle its up is off the world's up (airSpineTurn
    // stops at 170), and its level velocity across the lip is set to carry
    // it twice its offset plus g_spineClearance in that time (the part
    // along the lip and the vertical part kept). Then "Jump" (see
    // tellJump). Returns 1.
    int32_t airSpineEnter(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> across;
        ee::Local<zge::Vec4> across2;
        ee::Local<zge::Vec4> scratch;
        ee::Local<uint32_t> surfaceWord;
        zge::Vec4 &velocity = player->velocity;
        const PlayerAxes &axes = player->axes;

        player->pipeHeight = 0.0f;
        player->pipePeakHeight = 0.0f;
        player->rates = zge::kZero;
        player->noSpinTurn = 0;
        ee::Float &spineAngle = player->spineRolled;
        spineAngle = 0.0f;
        player->stateDistance = 0.0f; // the time in this air

        // The lip along the skater's right.
        if (zge::dot(player->railDirection, axes.right) < 0.0f)
        {
            zge::vec3Negate(&player->railDirection.xyz(), &player->railDirection.xyz());
            *scratch = player->railStart;
            player->railStart = player->railEnd;
            player->railEnd = *scratch;
        }
        acrossThePipe(player, *across);

        scaleLevelSpeed(player, *across2, *scratch);

        // From the skater to the lip's end (the original also takes its
        // dot with across, unused).
        *scratch = player->railEnd - player->position;
        zge::planeThrough(plane, &player->railStart, across);
        const ee::Float offset = zge::planeDistance(plane, &player->position);
        player->stateValue = offset;
        if (offset < *g_minSpineOffset)
            player->stateValue = *g_minSpineOffset;

        playJumpSound(player, 0x7F);

        if (player->settleFrames < 10)
        {
            // How steeply the skater heads up the face (its heading square
            // to the lip).
            alongPlane(across2, across, &axes.forward);
            *across2 = zge::unit(*across2);
            launchOffFace(player, zge::dot(*across2, *g_worldUp));
        }

        player->noSpinTurn = 1;
        const ee::Float upSpeed = -velocity.y;
        const ee::Float toTop = upSpeed / physics(kQPipeGravity) + player->airTime;
        const ee::Float offUp = ee::toFloat(static_cast<short>(zge::angleAcos(zge::dot(*g_worldUp, axes.up)))) * zge::kAngleToDegrees;
        player->turnRate = 0.0f;
        player->rateWords[0] = 0;
        player->expectedAirTime = toTop;
        player->spineRollRate = offUp / toTop;

        // The level velocity: its part along the lip kept, the part across
        // it set to cross twice the offset and the clearance by the top.
        alongPlane(across2, g_worldUp.get(), &velocity);
        *scratch = zge::scaledAfterMeasure(player->railDirection, zge::dot(player->railDirection, velocity));
        *across2 = *across2 - *scratch;
        *scratch = velocity - *across2;
        *across2 = zge::unit(*across2);
        const ee::Float twice = player->stateValue + player->stateValue;
        const ee::Float acrossSpeed = ee::abs(twice + *g_spineClearance) / toTop;
        *across2 = *across2 * acrossSpeed;
        velocity = *across2 + *scratch;
        player->previousSpeed = player->speed;
        player->speed = zge::vec3Length(&velocity.xyz());

        tellJump(player, *surfaceWord);

        player->overObject = 0;
        player->substate = 1;
        player->overSurfaceKind = 0;
        player->pipeHeight = 0.0f;
        player->pipePeakHeight = 0.0f;
        return 1;
    }
    READABLE_FUNCTION_WAITS(airSpineEnter, Verified);
}
