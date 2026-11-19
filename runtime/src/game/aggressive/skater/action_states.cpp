// The small callbacks of the Action states (state table, skater.h): the
// Vertical Pole (9) and Vertical Pole A (10, the pole grabbed in the air),
// the Horizontal Pole (11), the Vault (16), the Skitch (17), and the Wall
// Ride's exit (12). Recorded over tools/routes.py's Tutorial moves (vpole,
// vpolea, hpole, vault, skitch, wallride, walljump).
//
// A pole turns the skater about the world's up for Action / VPole Speed
// seconds, half a turn either way by the side it was grabbed from; the
// horizontal pole holds the skater at the bar until a timeout drops it
// into the Air with Action / HPole Release Angle Drop.

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "moves.h"
#include "aggressive/game_start.h"

#include "sym/aggressive/skater/action_states.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_levelNumber> g_level;

        // Tuning (aggressive/tuning.h; docs/TUNING.md "Entries").
        constexpr uint32_t kAction = 15;
        constexpr uint32_t kVPoleSpeed = 14;           // seconds on a vertical pole
        constexpr uint32_t kHPoleReleaseAngleDrop = 19;
        ee::Float action(uint32_t entry) { return (*g_tuningCategories)[kAction].entries[entry].value; }

        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;        // (0, -1, 0): y grows downward
        inline ee::Global<ee::Float, sym::g_barHangTime> g_barHangTime;   // seconds on the bar before it drops the skater
        inline ee::Global<ee::Float, sym::g_skitchLetGoPush> g_skitchLetGoPush; // the push along forward when a skitch ends

        // Records (updateLevelRecord).
        constexpr uint32_t kStatWallRideTime = 9;
        constexpr uint32_t kStatWallRideDistance = 10;
    }

    // verticalPoleTurn, the Vertical Poles' first callback: the turn round the
    // pole, half a turn (540 degrees a second over VPole Speed seconds,
    // the way the side says), until VPole Speed seconds are up: Ground.
    void verticalPoleTurn(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Float &time = player->stateValue;
        time = time + frameTime;
        const ee::Float duration = action(kVPoleSpeed);
        if (duration < time)
        {
            setSkaterState(player, static_cast<uint32_t>(State::Ground));
            return;
        }
        const ee::Float rate = (player->side ? ee::Float(540.0f) : ee::Float(-540.0f)) / duration;
        rotateOrientation(&player->orientation, &g_worldUp->xyz(), rate * frameTime);
    }
    READABLE_FUNCTION(verticalPoleTurn, Verified);

    // verticalPoleIdle, the Vertical Poles' second callback: nothing.
    void verticalPoleIdle(ee::Ptr<Player>, ee::Float) {}
    READABLE_FUNCTION(verticalPoleIdle, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        // An empty step, told which pole state called it.
        void poleStep(ee::Ptr<Player> player, uint32_t which) { ee::tailCall(points::verticalPoleStep::done, player, which); }
        }
    }

    // verticalPoleStep / verticalPoleAStep, the third callback of the Vertical
    // Pole and of the Vertical Pole A: both end in the empty step, the first
    // by running on into it: nothing happens.
    void verticalPoleStep(ee::Ptr<Player>, ee::Float) {}
    READABLE_FUNCTION(verticalPoleStep, Verified);

    void verticalPoleAStep(ee::Ptr<Player> player, ee::Float)
    {
        tail::poleStep(player, 1);
    }
    READABLE_FUNCTION(verticalPoleAStep, Verified);

    // verticalPoleAExit, the Vertical Pole A's exit: the Vertical Pole's, then
    // a lift up along the world's up when the pole leaves some above.
    void verticalPoleAExit(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> lift;
        verticalPoleExit(player);
        const ee::Float height = railLaunchMax(player->grindRail, player->grindSegment);
        if (!(0.0f < height))
            return;
        *lift = *g_worldUp * height;
        player->velocity = player->velocity + *lift;
    }
    READABLE_FUNCTION(verticalPoleAExit, Verified);

    // horizontalPoleIdle, the Horizontal Pole's first callback: nothing.
    void horizontalPoleIdle(ee::Ptr<Player>, ee::Float) {}
    READABLE_FUNCTION(horizontalPoleIdle, Verified);

    // horizontalPoleGravity, the Horizontal Pole's second callback: gravity once the
    // skater is let go (substate 2).
    void horizontalPoleGravity(ee::Ptr<Player> player, ee::Float frameTime)
    {
        if (player->substate == 2)
            game::tail(airGravity, player, frameTime);
    }
    READABLE_FUNCTION(horizontalPoleGravity, Verified);

    // horizontalPoleHang, the Horizontal Pole's third callback: the skater held
    // at the bar; after the hang time, dropped into the Air.
    void horizontalPoleHang(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Float &hung = player->airTime;
        hung = hung + frameTime;
        player->position = player->hangFrom;
        if (!(*g_barHangTime < hung))
            return;
        player->barDirection = 5;
        player->releaseAngle = action(kHPoleReleaseAngleDrop);
        game::tail(aggressive::skater::setSkaterState, player, static_cast<uint32_t>(State::Air));
    }
    READABLE_FUNCTION(horizontalPoleHang, Verified);

    // vaultApproach, the Vault's second callback: the Ground physics while
    // the vault has not started (substate 0).
    void vaultApproach(ee::Ptr<Player> player, ee::Float frameTime)
    {
        if (player->substate == 0)
            game::tail(groundPhysics, player, frameTime);
    }
    READABLE_FUNCTION(vaultApproach, Verified);

    // skitchLetGo, the Skitch's fifth callback: letting go of the pole
    // grab button lets go of the vehicle, with a push along forward: Air.
    void skitchLetGo(ee::Ptr<Player> player, ee::Float)
    {
        if (player->controls.actions & kActionPoleGrab)
            return;
        ee::Local<zge::Vec4> push;
        player->settleFrames = 0;
        *push = player->axes.forward * *g_skitchLetGoPush;
        player->velocity = player->velocity + *push;
        setSkaterState(player, static_cast<uint32_t>(State::Air));
    }
    READABLE_FUNCTION(skitchLetGo, Verified);

    // skitchContact, the Skitch's contact callback: no other state from a
    // contact while skitching.
    int32_t skitchContact(ee::Ptr<Player>, int32_t)
    {
        return -1;
    }
    READABLE_FUNCTION(skitchContact, Verified);

    // wallRideExit, the Wall Ride's exit: off a moving wall; the ride's
    // time and distance to the records (stats 9 and 10), then cleared.
    void wallRideExit(ee::Ptr<Player> player)
    {
        if (player->onMovingSurface)
        {
            player->onMovingSurface = 0;
            player->movingObject = 0;
        }
        aggressive::updateLevelRecord(kStatWallRideTime, *g_level, player->name, player->shortTime);
        aggressive::updateLevelRecord(kStatWallRideDistance, *g_level, player->name, player->stateDistance);
        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;
    }
    READABLE_FUNCTION(wallRideExit, Verified);
}
