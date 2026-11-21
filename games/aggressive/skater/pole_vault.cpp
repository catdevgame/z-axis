// The poles' and the vault's bigger callbacks (the small ones are in
// action_states.cpp): the Vertical Pole's enter (shared by Vertical Pole
// A, the pole grabbed in the air), the Horizontal Pole's enter and exit,
// the Vault's enter and its third callback. Recorded over
// games/aggressive/tools/routes.py's Tutorial moves (vpole, vpolea, hpole, vault).
//
// The pole or edge grabbed is Player::rail (as a grind's rail, skater.h):
// its segment and object (grindSegment, grindRail), its direction
// (railDirection) and the point the skater holds (railCameFrom).
//
// A vertical pole stands the skater upright facing along it, on the hold
// point, its side by which way the pole is from the skater's right. A
// horizontal pole squares the skater to the bar, by the side of the bar
// the skater came from; letting go swings the skater off it forward
// (Action / Horiz Pole Radius in front of the bar), rolled about the bar
// by the release angle and turned about up by HPole Side Angle (or the
// bar's own angle), at no less than HPole Min SPeed, with the bar's own
// push; the bar's script hears "Swing". A vault heads the skater for the
// edge's point; the Ground physics run until the point is within Action /
// Vault Timing seconds, then the skater goes straight for it and over it
// with Vault Jump up: the Air.
//
// A scaled vector's w is whatever the last vector operation left, and it
// reaches the position and the velocity: zge/math/vec.h runs on VU0, so the
// order of the statements below is what decides it.

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "moves.h"
#include "trick_objects.h"

#include "sym/aggressive/skater/pole_vault.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kSwing = sym::swing; // "Swing"
        constexpr uint32_t kVault = sym::vault; // "Vault"

        // Action tuning (aggressive/tuning.h; docs/TUNING.md "Action").
        constexpr uint32_t kAction = 15;
        enum ActionEntry : uint32_t
        {
            kVaultTiming = 4,      // seconds before the edge the vault starts
            kVaultJump = 5,        // up, going over
            kHorizPoleRadius = 10, // how far in front of the bar a swing lets go
            kHPoleMinSpeed = 15,
            kHPoleSideAngle = 16,
        };
        ee::Float action(uint32_t entry) { return (*g_tuningCategories)[kAction].entries[entry].value; }

        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        // The bar's frame, squared up by the Horizontal Pole's enter
        // (rows: along the bar, up, across).
        inline ee::Global<zge::Matrix, sym::g_barFrame> g_barFrame;
    }

    // verticalPoleEnter, the Vertical Pole and Vertical Pole A states' enter:
    // the side is 0 with the pole on the skater's right, 1 otherwise (the
    // turn round it goes the way the side says, action_states.cpp); the
    // skater stands upright on the hold point, its pole vector back along
    // forward. Returns 1.
    int32_t verticalPoleEnter(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> toPole;
        PlayerAxes &axes = player->axes;

        *toPole = player->railCameFrom - player->position;
        *toPole = zge::unit(*toPole);
        player->side = 0.0f < zge::dot(*toPole, axes.right) ? 0 : 1;

        axes.up = *g_worldUp;
        orthonormalize(&axes.forward, &axes.right, &axes.up);
        player->position = player->railCameFrom;
        player->stateValue = 0.0f;
        zge::vec3Negate(&player->poleVector.xyz(), &axes.forward.xyz());
        return 1;
    }
    READABLE_FUNCTION(verticalPoleEnter, Verified);

    // horizontalPoleEnter, the Horizontal Pole's enter: the bar's frame (along it,
    // up, across) squared up, and the side is which side of the bar the
    // skater is on in it (1 behind). The skater's right runs along the bar
    // (against it from side 1), squared to it; the pole vector is to the
    // hold point; the hang time and the pole grab's timer start from 0.
    // Returns 1.
    int32_t horizontalPoleEnter(ee::Ptr<Player> player)
    {
        ee::Local<zge::Matrix> toBar;
        PlayerAxes &axes = player->axes;
        zge::Matrix &bar = *g_barFrame;

        bar.row(1) = *g_worldUp;
        bar.row(0) = player->railDirection;
        orthonormalize(&bar.row(2), &bar.row(1), &bar.row(0));
        zge::matrixClearTranslation(&bar);
        zge::matrixInvertRigid(toBar, &bar);

        // The skater's offset from the hold point, in the bar's frame: only
        // its across is used.
        const zge::Vec4 offset = player->position - player->railCameFrom;
        const zge::Vec4 inBar = zge::transformed(*toBar, offset);
        player->side = inBar.z < 0.0f ? 1 : 0;

        player->lastRail = {};
        player->controls.poleGrabTime = 0.0f;
        player->airTime = 0.0f;
        if (player->side == 1)
            zge::vec3Negate(&axes.right.xyz(), &player->railDirection.xyz());
        else
            axes.right = player->railDirection;
        orthonormalize(&axes.forward, &axes.right, &axes.up);
        player->poleVector = player->railCameFrom - player->position;
        player->barDirection = 0;
        return 1;
    }
    READABLE_FUNCTION(horizontalPoleEnter, Verified);

    // horizontalPoleExit, the Horizontal Pole's exit: the swing lets go Horiz
    // Pole Radius in front of the hold point, rolled about the bar by the
    // release angle (the other way from side 1) and turned about up by the
    // bar's angle (HPole Side Angle without one) for swing kinds 3 and 7;
    // forward at no less than HPole Min SPeed, plus the bar's push. The
    // bar's script hears "Swing" with its tag.
    void horizontalPoleExit(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> push;
        ee::Local<zge::Vec4> ahead;
        ee::Local<uint32_t> eventData;
        PlayerAxes &axes = player->axes;

        *ahead = axes.forward * action(kHorizPoleRadius);
        player->position = player->railCameFrom + *ahead;
        player->previousSpeed = player->speed;
        player->speed = zge::vec3Length(&player->velocity.xyz());

        if (player->side == 1)
            player->releaseAngle = -player->releaseAngle;
        rotateOrientation(&player->orientation, &player->railDirection.xyz(), player->releaseAngle);
        ee::Float turn = railLaunchMax(player->grindRail, player->grindSegment);
        if (turn == 0.0f)
            turn = action(kHPoleSideAngle);
        if (player->barDirection == 3)
            rotateOrientation(&player->orientation, &g_worldUp->xyz(), -turn);
        else if (player->barDirection == 7)
            rotateOrientation(&player->orientation, &g_worldUp->xyz(), turn);

        const ee::Float minSpeed = action(kHPoleMinSpeed);
        if (player->speed < minSpeed)
            player->speed = minSpeed;
        player->velocity = axes.forward * player->speed;
        const ee::Float barPush = railLaunchSpeed(player->grindRail, player->grindSegment);
        if (0.0f < barPush)
        {
            *push = axes.forward * barPush;
            player->velocity = player->velocity + *push;
        }
        player->airTime = 0.0f;
        player->controls.poleGrabTime = 0.0f;

        player->lastRail = player->grindRail;
        *eventData = railEventWord(player->grindRail, player->grindSegment);
        sendObjectEvent(player, player->grindRail, kSwing, eventData);
    }
    // Waiting: the event goes to a script synchronously, and a handler can
    // wait (as the trigger zones' events, zones.cpp). So for the vault's
    // enter.
    READABLE_FUNCTION_WAITS(horizontalPoleExit, Verified);

    // vaultEnter, the Vault's enter: the vault not started (substate 0),
    // the distance to the edge's point kept (the update's), the velocity
    // turned toward the point at the same speed and forward along it. The
    // edge's script, if any, hears "Vault" with its tag; then the Ground's
    // enter. Returns 1.
    int32_t vaultEnter(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> toEdge;
        ee::Local<uint32_t> eventData;
        PlayerAxes &axes = player->axes;

        player->airTime = 0.0f;
        player->substate = 0;
        *toEdge = player->railCameFrom - player->position;
        player->stateValue = zge::vec3Length(&toEdge->xyz());
        *toEdge = zge::unit(*toEdge);
        player->velocity = *toEdge * player->speed;
        axes.forward = zge::unit(player->velocity);
        orthonormalize(&axes.right, &axes.up, &axes.forward);

        if (player->grindRail)
        {
            *eventData = railEventWord(player->grindRail, player->grindSegment);
            sendObjectEvent(player, player->grindRail, kVault, eventData);
        }
        groundEnter(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(vaultEnter, Verified);

    // vaultUpdate, the Vault's third callback. Before the vault (substate
    // 0): the Ground physics; moving away from the edge's point ends it
    // (Ground); within Vault Timing seconds of it the vault starts, straight
    // for the point at the same speed (substate 1). In the vault: the move
    // toward the point; moving away ends it (Air), and passing the point
    // this frame jumps (Vault Jump up): Air. The distance is kept for the
    // next frame.
    void vaultUpdate(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<zge::Vec4> toEdge;
        ee::Local<zge::Vec4> step;

        *toEdge = player->railCameFrom - player->position;
        const ee::Float distance = zge::vec3Length(&toEdge->xyz());
        ee::Float &lastDistance = player->stateValue;

        if (player->substate == 0)
        {
            if (lastDistance < distance)
            {
                setSkaterState(player, static_cast<uint32_t>(State::Ground));
                return;
            }
            groundMove(player, frameTime);
            if (distance < player->speed * action(kVaultTiming))
            {
                *toEdge = zge::unit(*toEdge);
                player->velocity = *toEdge * player->speed;
                player->substate = 1;
                scoreStateChange(player, player->previousState, static_cast<uint32_t>(player->state));
            }
        }
        else if (player->substate == 1)
        {
            if (lastDistance < distance)
            {
                setSkaterState(player, static_cast<uint32_t>(State::Air));
                return;
            }
            *step = player->velocity * frameTime;
            player->position = *step + player->position;
            if (distance < player->speed * frameTime)
            {
                *step = *g_worldUp * action(kVaultJump);
                player->velocity = player->velocity + *step;
                setSkaterState(player, static_cast<uint32_t>(State::Air));
            }
        }
        lastDistance = distance;
    }
    READABLE_FUNCTION(vaultUpdate, Verified);
}
