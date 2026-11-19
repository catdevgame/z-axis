#pragma once

// The skater (a player of the level: 0x930 bytes each, on the heap: the
// level object (g_topState) +0x10 points to them, player one at +0x10,
// player two 0x930 after it; updatePlayers runs the driver for each).
//
// Physics is a state machine: the state (Ground, Air, Grind ...) picks a
// row of the state table, whose callbacks the per-frame update
// (skaterUpdate) runs in order. Fields are named where code that uses them
// was read (the update, its debug overlay "Physics State %s Object %s",
// "Substate %d Side %d Balance 0 %.2f Balance 1 %.2f", the overrides);
// unknown bytes stay padding.

#include "aggressive/career.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include "sym/aggressive/skater/skater.h.h"

namespace zge
{
    struct AnimateSystem;
    struct AnimationChannel;
    struct AnimationEvent;
    struct Motion;
    struct Node;
    struct OrientedBox;
    struct SceneObject;
    struct SweepBox;
    struct ZgePart;
}

namespace zge::zgui
{
    struct Node;
}

namespace aggressive
{
    struct GameObject;
    struct TrafficCar;
}

namespace aggressive::level
{
    struct DoubleRail;
}

namespace aggressive::hud
{
    struct SkaterHud; // (hud/hud.h)
}

namespace aggressive::skater
{
    using zge::Vec4;

    enum class State : uint8_t
    {
        Ground = 0,
        Stand = 1,
        Air = 2,
        AirQuarterPipe = 3,
        AirSpine = 4,
        Grind = 5,
        HandGrind = 6,
        DoubleGrind = 7,
        Stall = 8,
        VerticalPole = 9,
        VerticalPoleA = 10,
        HorizontalPole = 11,
        WallRide = 12,
        WallTap = 13,
        Manual = 14,
        CessSlide = 15,
        Vault = 16,
        Skitch = 17,
    };
    constexpr uint32_t kStateCount = 18;

    struct Player;

    // A row of the state table (g_states). Callbacks are game
    // functions, 0 where a state has none.
    struct StateRow
    {
        char name[16];     // +0x00 "Ground", "Air QPipe", ...
        ee::FunctionPtr<int32_t(ee::Ptr<Player>)> enter; // +0x10
        ee::FunctionPtr<void(ee::Ptr<Player>)> exit;     // +0x14
        // +0x18 run each frame, in order, as (player, frame time); the last
        // not while the skater is down (Player::flags kDown).
        ee::FunctionPtr<void(ee::Ptr<Player>, ee::Float)> update[5];
        // +0x2C (player, move) -> the state a move asked of the skater leads
        // to, or -1 (requestMove).
        ee::FunctionPtr<int32_t(ee::Ptr<Player>, int32_t)> request;
        uint32_t _30;      // +0x30
    };
    static_assert(sizeof(StateRow) == 0x34 && offsetof(StateRow, update) == 0x18);

    inline ee::Global<StateRow[kStateCount], sym::g_states> g_states;

    // The skater's controls (Player +0x550; skaterControlsUpdate fills it each
    // frame): actions as bits, the steering stick, the buttons' recent
    // history and timers the states use as input windows.
    struct Controls
    {
        uint32_t actions;          // +0x00 this frame's Action bits
        // +0x04 no reader found: the update puts it back as it was after its
        // callees run.
        uint32_t keptWord;
        ee::Float stickX;          // +0x08 across: the steering (Ground: turn rate = it times Turn Rate)
        ee::Float stickY;          // +0x0C < 0 pushed forward (kActionPush), > 0 pulled back (kActionBrake)
        uint8_t _10[4];
        uint32_t stickFlags;       // +0x14 kStickAnalog ... (the stick, not the d-pad, gave stickX / stickY)
        union
        {
            uint8_t _18[4];
            // +0x18 set to 0 by resetControls (PS2, Xbox and GameCube alike);
            // no reader in the PS2 build.
            uint32_t clearedAtReset;
        };
        uint32_t holdHistory;      // +0x1C nonzero: the history is not added to (only its change flag kept)
        // +0x20 the stick's recent directions (stickDirection), newest first
        union
        {
            uint8_t history[8];
            uint32_t historyWords[2];
        };
        ee::Float historyAge[8];   // +0x28 seconds since each was entered (dropped past Control / Button Timeout)
        ee::Float historyHeld[8];  // +0x48 seconds each has been held
        uint8_t historyCount;      // +0x68
        uint8_t historyChanged;    // +0x69 the direction differs from the newest entry
        uint8_t layout;            // +0x6A the button layout (g_buttonLayouts)
        uint8_t port;              // +0x6B the pad
        int8_t trickDelay;         // +0x6C frames left before the trick input runs (Control / # cmd delay frames; -1: none)
        uint8_t lastDirection;     // +0x6D the newest direction when the jump was let go
        int8_t window200;          // +0x6E frames left since pad bit 0x200 was pressed (Control / # cmd delay frames; -1: none)
        int8_t window100;          // +0x6F the same for pad bit 0x100: both open together makes the two-button command
        union
        {
            uint8_t _70[4];
            ee::Float releaseTimer; // +0x70 (trickRelease)
        };
        ee::Float bowlJumpOutTime; // +0x74 how long the jump-out input has been held (Physics / Bowl Jumpout Time)
        ee::Float stallTime;       // +0x78 up to Grind/Walls / Stall Timeout
        ee::Float wallRideTime;    // +0x7C up to Grind/Walls / Wall Ride Timeout
        ee::Float poleGrabTime;    // +0x80 up to Action / Pole Grab Timeout
        ee::Float releasedTime;    // +0x84 since the layout's button 3 was let go (to 100)
        ee::Float cessSlideTime;   // +0x88 a press of pad bit 0x800 lasts this long (Control / Cessslide Button Timeout), -1 none
        uint8_t _8c[4];
    };
    static_assert(sizeof(Controls) == 0x90 && offsetof(Controls, history) == 0x20 && offsetof(Controls, historyCount) == 0x68);
    static_assert(offsetof(Controls, stickFlags) == 0x14 && offsetof(Controls, holdHistory) == 0x1C && offsetof(Controls, cessSlideTime) == 0x88);
    static_assert(offsetof(Controls, window200) == 0x6E && offsetof(Controls, historyHeld) == 0x48);
    // Controls::stickFlags
    constexpr uint32_t kStickAnalog = 0x4;
    constexpr uint32_t kStickAnalogX = 0x8;
    constexpr uint32_t kStickAnalogY = 0x10;
    static_assert(offsetof(Controls, layout) == 0x6A && offsetof(Controls, bowlJumpOutTime) == 0x74);

    // Controls::actions
    constexpr uint32_t kActionPush = 0x1;  // the layout's buttons 0 or 1, or the stick forward
    constexpr uint32_t kActionBrake = 0x2; // the layout's button 2, or the stick back
    constexpr uint32_t kActionJump = 0x8;
    constexpr uint32_t kActionWallRide = 0x80; // starts a wall ride from the air
    constexpr uint32_t kActionPoleGrab = 0x400;
    constexpr uint32_t kActionButton100 = 0x2000; // pad bit 0x100 held: a sliding turn, in the air a spin left
    constexpr uint32_t kActionButton200 = 0x4000; // pad bit 0x200 held: likewise, a spin right
    constexpr uint32_t kActionBowlJumpOut = 0x20000;

    // The stick's directions (stickDirection): 0 none, then clockwise from up.
    enum Direction : uint8_t
    {
        kDirNone = 0,
        kDirUp = 1,
        kDirUpRight = 2,
        kDirRight = 3,
        kDirDownRight = 4,
        kDirDown = 5,
        kDirDownLeft = 6,
        kDirLeft = 7,
        kDirUpLeft = 8,
    };

    // The moves asked of a state (requestMove).
    enum Move : int32_t
    {
        kMoveGrind = 4,
        kMoveHandGrind = 5,
        kMoveDoubleGrind = 6,
        kMoveWallRide = 7,
        kMoveAction = 10,
        kMoveReleasePole = 11,
        kMoveManual = 15,
        kMovePastGrinds = 16, // trickMove's for a trick type nothing makes; no state answers it
        kMoveCessSlide = 18,
        kMoveHold = 20,       // the stall and the poles
    };

    // The game's pad state per port (g_pads + port * 0x74): buttons
    // as the game's own bits (not the pad's), copied from the engine's pad
    // each frame (updatePads, aggressive/pads.cpp).
    struct PadState
    {
        uint32_t port;     // +0x00 the pad's port (initPlayerPads)
        uint32_t device;   // +0x04 the engine's pad: +0x04 bit 0 connected, +0x08 held, +0x0C pressed, +0x14 axes, +0x6C
        uint32_t held;     // +0x08
        uint32_t pressed;  // +0x0C this frame
        ee::Float axes[22]; // +0x10 the analog axes and the rest of the device's +0x14 .. +0x6C (0 x, 1 y; 2, 3 the right stick)
        uint32_t extraButtons; // +0x68 the device's extra buttons (its +0x6C): the pressure bits padDeviceBit reads
        uint32_t heldAtSnapshot; // +0x6C the buttons held when options bit 0x02 was set; ~0: none
        uint32_t options;  // +0x70 bits: 0x01 pressed from the snapshot, 0x02 take one, 0x04 and 0x08 hold
                           // button 0x80 and 0x4000 off, 0x10 vibration (BRINGUP "Rumble"), 0x20 connected
    };
    static_assert(sizeof(PadState) == 0x74 && offsetof(PadState, held) == 0x08 && offsetof(PadState, axes) == 0x10);
    static_assert(offsetof(PadState, heldAtSnapshot) == 0x6C && offsetof(PadState, options) == 0x70);
    inline ee::Global<PadState[2], sym::g_pads> g_pads;

    // The three button layouts (Options > Controller): game button bits.
    // [0] and [1] push, [2] brakes; [3] restarts Controls::releasedTime;
    // [4] grabs a pole (held) or lets go of it (pressed).
    struct ButtonLayout
    {
        uint32_t buttons[9];
    };
    static_assert(sizeof(ButtonLayout) == 0x24);
    inline ee::Global<ButtonLayout[3], sym::g_buttonLayouts> g_buttonLayouts;

    // The wall a wall ride or wall tap rides (Player::wall; findWall fills
    // a record of the same layout): the face hit and its object, its
    // corners in world space, its normal and the point hit.
    struct WallContact
    {
        uint32_t face;          // +0x00 the face's word
        ee::Ptr<zge::Node> object;    // +0x04
        ee::Ptr<zge::ZgePart> part;   // +0x08 the collision part (the hit's second word)
        uint8_t _0c[4];
        zge::Vec4 corners[3];   // +0x10
        zge::Vec4 normal;       // +0x40
        zge::Vec4 point;        // +0x50 the point hit (findWall: the wall's point nearest the skater)
    };
    static_assert(sizeof(WallContact) == 0x60 && offsetof(WallContact, corners) == 0x10 && offsetof(WallContact, normal) == 0x40 &&
                  offsetof(WallContact, point) == 0x50);

    // skaterStat's stats (SkaterProfile::stats) and, for a stat with several
    // values, which one: each value's tuning entry has the same name.
    enum SkaterStat : uint32_t
    {
        kStatJump = 0,
        kStatSpeed = 1,
        kStatSpin = 2,
        kStatGrind = 3,
        kStatManual = 4,
        kStatFakie = 5,
        kStatWallRide = 6,
    };
    enum GrindStatValue : uint32_t
    {
        kStatGrindFactor = 0, // Grind Balance Factor: how fast the balance's drift grows
        kStatGrindInit = 1,   // Grind Balance Init: the drift it starts with
        kStatStallFactor = 2, // the same of a stall
        kStatStallInit = 3,
    };
    enum ManualStatValue : uint32_t
    {
        kStatManualFactor = 0,
        kStatManualInit = 1,
    };

    // The skater's axes: Player::orientation's rows by what they are.
    struct PlayerAxes
    {
        zge::Vec4 right, up, forward, last;
    };
    static_assert(sizeof(PlayerAxes) == sizeof(zge::Matrix));

    // The object a skater hits or rides (Player::ride; rideObject keeps it) and
    // the one that carries it (Player::carrier; carryRider keeps it): a moving
    // one carries the skater along.
    struct RiddenObject
    {
        uint16_t kept;         // +0x00 0 on the first frame on the object, 1 once its matrices are kept
        uint8_t moving;        // +0x02 the object moved since (or is new)
        uint8_t _03;
        ee::Float frameTime;   // +0x04
        ee::Ptr<zge::Node> object; // +0x08
        uint8_t _0c[4];
        // +0x10 the skater's position last frame; carryRider's: the point
        // carried, in the object's frame on a wall tap or a skitch.
        union
        {
            zge::Vec4 lastPosition;
            zge::Vec4 carriedPoint;
        };
        uint8_t _20[0x10];
        zge::Matrix inverse;   // +0x30 the object's inverse world matrix last frame
        zge::Matrix world;     // +0x70 its world matrix last frame
        zge::Vec4 move;        // +0xB0 the object's move of the skater this frame
    };
    static_assert(sizeof(RiddenObject) == 0xC0 && offsetof(RiddenObject, lastPosition) == 0x10 &&
                  offsetof(RiddenObject, inverse) == 0x30 && offsetof(RiddenObject, move) == 0xB0);

    // The record at Player +0x210 (grindSegment .. railCameFrom), and the rail
    // search's result: a rail ground on, or the lip edge a pipe state follows
    // (kPipeEdgeOf fills the same shape).
    struct RailRecord
    {
        uint32_t surface;     // +0x00 the segment
        union
        {
            uint32_t object;  // +0x04 the rail object
            ee::Ptr<zge::Node> node;
        };
        ee::Ptr<zge::ZgePart> found; // +0x08 the edge's collision part, set once the filter took one
        uint8_t _0c[4];
        zge::Vec4 start;      // +0x10
        zge::Vec4 end;        // +0x20
        zge::Vec4 direction;  // +0x30 unit, start to end
        zge::Vec4 joinPoint;  // +0x40 where the skater gets on or holds on
    };
    static_assert(sizeof(RailRecord) == 0x50 && offsetof(RailRecord, start) == 0x10 && offsetof(RailRecord, joinPoint) == 0x40);

    // A skater's poker hand in the card levels (collectCard).
    struct CardHand
    {
        uint32_t cards[5]; // +0x00 the cards collected (card_game.cpp: rank times 4 plus suit)
        uint32_t count;    // +0x14 how many
        uint16_t points; // +0x18 what the hand is worth
        uint16_t widgets; // +0x1A 0: it shows on the Card1P1 .. Card5P1 widgets, else on Card1P2 .. Card5P2
    };
    static_assert(sizeof(CardHand) == 0x1C && offsetof(CardHand, count) == 0x14 && offsetof(CardHand, points) == 0x18);
    // What a player keeps per level (Player::statsByLevel).
    constexpr uint32_t kLevelStatsSize = 0x24;
    struct LevelStats
    {
        uint8_t _00[8];
        ee::Float secondsPlayed; // +0x08
        uint8_t _0c[kLevelStatsSize - 0x0C];
    };
    static_assert(sizeof(LevelStats) == kLevelStatsSize);

    struct Player
    {
        uint8_t number;                // +0x000 the player (0 or 1; zone events carry it)
        uint8_t active;                // +0x001 the driver skips a player that is not
        // +0x002 riding fakie (backwards): drawn turned half round about
        // its up axis (the physics' orientation keeps facing the motion).
        uint8_t fakie;
        // +0x003 the drawn orientation follows the physics one this frame
        // even when they are close (cleared by the driver).
        uint8_t drawnRefresh;
        uint32_t drawFlags;            // +0x004 kDrawnSnap
        // +0x008 the locator (name hash) the next respawn goes to; 0: the
        // level's "Respawn".
        uint32_t respawnLocator;
        uint32_t ragdoll;              // +0x00C the skater's rigid body (MORF chunks of its dynamics file) [low]
        // +0x010 the skater's box in the level's sweep (sweep-and-prune):
        // its three low end points, one an axis (updateSkaterBox).
        union
        {
            uint32_t box;
            ee::Ptr<zge::SweepBox> sweepBox;
        };
        ee::Ptr<zge::SceneObject> node;                 // +0x014 the skater's scene node (drawn at drawnPosition, drawnOrientation)
        uint32_t script;               // +0x018 the player's own script object (its events too; 0 for none)
        // +0x01C the trigger zones the skater is in (updateTriggerZones), 0 where free.
        uint32_t zones[10];
        uint32_t rollSound;            // +0x044 the rolling sound (updateRollSound)
        uint8_t _048[8];
        zge::Vec4 drawnPosition;       // +0x050 the node's position: position, after the physics
        zge::Quaternion drawnOrientation; // +0x060 turned toward the physics orientation a little each frame
        uint8_t resetBytes[4];         // +0x070 cleared by resetSkaterPhysics; no reader found
        State state;                   // +0x074
        uint8_t previousState;         // +0x075 the state left (setSkaterState)
        uint8_t substate;              // +0x076 (the state's own steps: grind, manual ...)
        uint8_t side;                  // +0x077
        uint8_t noSpinTurn;            // +0x078 over a spine with no spin pressed: no air turn
        // +0x079 counts frames up to 20; the Ground update sets it back to 0
        // (groundControls).
        uint8_t settleFrames;
        uint8_t pushing;               // +0x07A (ground.cpp)
        uint8_t barDirection;          // +0x07B the stick's last direction as a bar was grabbed (actionRequest)
        uint8_t landing;               // +0x07C how the last landing went: 0 clean, 1 and 2 worse, 3 a bail
        uint8_t _07d[3];
        uint32_t flags;                // +0x080 kDown, kRespawnAtLocator, ...
        uint8_t _084[0x0C];
        zge::Vec4 position;            // +0x090 (logiccheck's player position g_playerPosition)
        zge::Vec4 velocity;            // +0x0A0
        // +0x0B0 orientation, rows right (+0xB0), up (+0xC0), forward (+0xD0)
        // and +0xE0 (the follow camera turns about up and right).
        union
        {
            zge::Matrix orientation;
            PlayerAxes axes;
        };
        // +0x0F0 last frame's position, velocity and orientation (the moves keep them)
        zge::Vec4 lastPosition;
        zge::Vec4 lastVelocity;        // +0x100
        zge::Matrix lastOrientation;   // +0x110
        // +0x150 degrees a second: about the right axis; over a spine (Air
        // Spine) the same word rolls the skater about the spine's lip
        // (railDirection), until 170 degrees are done (spineRolled).
        union
        {
            struct
            {
                union
                {
                    ee::Float pitchRate;
                    ee::Float spineRollRate;
                };
                ee::Float turnRate;    // +0x154 degrees a second about the up axis (the steering)
                uint32_t rateWords[2]; // +0x158 cleared with the rates; no reader found
            };
            zge::Vec4 rates;           // the states stop them all with one vector store
        };
        zge::Vec4 lastHitNormal;       // +0x160 the last face a probe hit this frame (skaterMoveCollide)
        zge::Vec4 groundNormal;        // +0x170 0 while there is no ground
        zge::Vec4 surfaceNormal;       // +0x180 the object's surface under the skater
        // +0x190 the face the move touched this frame and its object
        // (+0x198), 0xFFFFFFFF / 0 for none; keepContact makes them the
        // surface and object the skater is on.
        uint32_t contact;
        uint32_t surface;              // +0x194 the face of the object the skater is on (surfaceSound), 0xFFFFFFFF for none
        union
        {
            uint32_t contactObject;    // +0x198
            ee::Ptr<zge::Node> contactNode;
        };
        // +0x19C the terrain / rail / object the skater is on, and the one
        // before (scene objects; their name at +0x12C).
        union
        {
            uint32_t object;
            ee::Ptr<zge::Node> objectNode;
        };
        uint32_t lastObject;           // +0x1A0
        // +0x1A4 the rail object ground last (grindExit, ontoDoubleRail), the
        // bar swung off last (pole_vault.cpp; 0 on one).
        ee::Ptr<zge::Node> lastRail;
        ee::Ptr<zge::Node> pipeObject; // +0x1A8 the object and surface a stall began on (the quarter pipe)
        uint32_t pipeSurface;          // +0x1AC
        // +0x1B0 from the skater to the pole when it was grabbed: back along
        // forward for a vertical pole, to the hold point for a bar (the way
        // the skater leaves it).
        zge::Vec4 poleVector;
        ee::Float heightAboveGround;   // +0x1C0 to the ground below (groundBelow; cleared entering the air)
        ee::Float pipeHeight;          // +0x1C4 Air QPipe: from the sweep's start to the face
        ee::Float pipePeakHeight;      // +0x1C8
        ee::Float swingMade;           // +0x1CC degrees swung round the pole so far
        // +0x1D0 the angle the Air turns by after letting go of a bar; a
        // vertical pole's swing still to make.
        union
        {
            ee::Float releaseAngle;
            ee::Float swingToMake;
        };
        // +0x1D4 the jump's turn so far in degrees, each way (turnInAir:
        // every half turn passed counts toward the spin score).
        ee::Float spinDegreesClockwise;
        ee::Float spinDegreesCounterClockwise; // +0x1D8
        // +0x1DC seconds moved without a hit (moveSkater adds to it): off
        // the ground for Air Transition Time takes off; in the air, under a
        // frame means landed, over Acid Drop Time an acid drop.
        ee::Float airTime;
        ee::Float downTime;            // +0x1E0 seconds since the skater went down (kDown)
        ee::Float pushSpeed;           // +0x1E4 the speed the skater pushes to (cleared going down)
        ee::Float speed;               // +0x1E8 |velocity|, this frame
        ee::Float previousSpeed;       // +0x1EC last frame's
        int32_t rollSoundSpeed;        // +0x1F0 the rolling sound's speed (speed * 60, a skitch's * 3600)
        ee::Float distance;            // +0x1F4 the distance ground and manualled
        // +0x1F8 the rail's event word (railEventWord): GrindStart's data,
        // with distance GrindEnd's.
        uint32_t railEvent;
        // +0x1FC the segment and object of the pipe's edge before (airQuarterPipeMove)
        uint32_t lastPipeSegment;
        ee::Ptr<zge::Node> lastPipeObject; // +0x200
        uint8_t _204[0x0C];
        union
        {
            RailRecord rail;                 // +0x210 the same fields as one record
            struct
            {
                uint32_t grindSegment;         // +0x210 the rail's segment ground on (with grindRail: railMaterial)
                ee::Ptr<zge::Node> grindRail;            // +0x214 the rail's object
                uint32_t grindMesh;            // +0x218 the rail's collision mesh (collisionMeshOf)
                // +0x210 .. +0x24F also a quarter pipe's lip edge (pipeEdgeOf fills them).
                uint8_t _21c[4];
                zge::Vec4 railStart;           // +0x220 the rail (or lip edge) ground on: its ends
                zge::Vec4 railEnd;             // +0x230
                zge::Vec4 railDirection;       // +0x240 unit, the way the skater heads
                // +0x250 by state: a rail's join point (inferred: breaks a tie when
                // heading square to the rail), where a horizontal pole hangs the
                // skater, the point a skitch holds on the vehicle.
                union
                {
                    zge::Vec4 railCameFrom;
                    zge::Vec4 hangFrom;
                    zge::Vec4 skitchHold;
                };
            };
        };
        ee::Ptr<level::DoubleRail> doubleGrindRails; // +0x260 the rails of a Double Grind
        // +0x264 by state: Air's late-jump window, Air QPipe's offset from
        // the pipe, Manual's balance delay, a grind's pause. stateValue where
        // several states share the code.
        union
        {
            ee::Float stateValue;
            ee::Float lateJumpTime;
            ee::Float pipeOffset;
            ee::Float balanceDelay;
            ee::Float graceTime;
        };
        // +0x268 moved in this state (grind, wall ride, manual); the time
        // aloft in the air.
        union
        {
            ee::Float stateDistance;
            ee::Float stateAirTime;
        };
        // +0x26C the debug overlay's "Short %d state 2 %f"; Air Spine's degrees
        // rolled so far.
        union
        {
            ee::Float shortTime;
            ee::Float spineRolled;
        };
        ee::Float balance[2];          // +0x270 the grind / manual / stall balance meters
        ee::Float balanceDrift;        // +0x278 added to balance[0]
        ee::Float balanceSpeed;        // +0x27C the manual balance's drift per frame
        int32_t shortCount;            // +0x280
        ee::Float holdTime;            // +0x284 seconds a stall is held (its events'; the line's counters clear it)
        ee::Float grindTime;           // +0x288
        ee::Float manualTime;          // +0x28C
        ee::Float wallRideTime;        // +0x290
        ee::Float totalAirTime;        // +0x294 (cleared by the line's counters' reset and resetSkaterPhysics)
        // +0x298 the time the jump is expected to last in the air (the trick
        // animations' speed reads it; the exit clears it).
        // Air QPipe's own use of it: the turn along the pipe.
        union
        {
            ee::Float expectedAirTime;
            ee::Float qpipeTurn;
        };
        ee::Float stateTime;           // +0x29C seconds in the current state
        zge::Matrix railLean;          // +0x2A0 a stall's lean about the edge; the identity on getting onto a rail
        // +0x2E0 the wall ridden (wall: its bytes, as DM2's rider host fills
        // them).
        union
        {
            WallContact wallContact;
            uint8_t wall[0x60];
        };
        // +0x340 the surface kind and the object the skater is over
        // (groundBelow: "OverStart" when they change, "OverEnd" to the old).
        uint32_t overSurfaceKind;
        union
        {
            uint32_t overObject;       // +0x344
            ee::Ptr<zge::Node> overNode;
        };
        uint8_t _348[8];
        // +0x350 the lip's segment and object at the launch (the Air QPipe's)
        uint32_t launchSegment;
        ee::Ptr<zge::Node> launchObject; // +0x354
        uint8_t _358[8];
        zge::Vec4 launchLevelVelocity; // +0x360 the velocity less its vertical part, at the launch
        // +0x370 what carries the skater (carryRider), and the words of it
        // others read.
        union
        {
            RiddenObject carrier;
            struct
            {
                uint16_t _370;
                uint8_t onMovingSurface;   // +0x372 1 on a moving rail or object
                uint8_t _373;
                ee::Float carriedTime;     // +0x374 the time carriedMove took
                uint32_t movingObject;     // +0x378 what onMovingSurface rides
                uint8_t _37c[0xA4];
                // +0x420 the moving surface's move this frame (added to the
                // position; the skitched vehicle's for the Skitch rolling sound).
                zge::Vec4 carriedMove;
            };
        };
        RiddenObject ride;             // +0x430
        // +0x4F0 kSkidStart: a cess slide or wall ride began, its skid marks
        // start at the next frame's foot points (wallRideContact).
        uint32_t skidFlags;
        uint8_t _4f4[0x0C];
        // +0x500 the feet's points on the surface ridden this frame (left,
        // right) and last frame's (+0x520): the skid marks join them.
        zge::Vec4 skidPoints[2];
        zge::Vec4 lastSkidPoints[2];
        uint8_t _540[0x10];
        Controls controls;             // +0x550
        uint8_t skaterIndex;           // +0x5E0 the skater (low 5 bits)
        uint8_t _5e1[0x0F];
        uint16_t unlockedTricks;       // +0x5F0 the bits of the secret tricks (awardSecretTrick; Trick::unlockBit)
        uint8_t _5f2[6];
        ee::Float timePlayed;          // +0x5F8 seconds the driver ran
        uint8_t _5fc[4];
        // +0x600 .. +0x608 grow with the time played in levels 3 and 0
        // (0.007 and 0.01 a second; level 0 feeds +0x604 only).
        ee::Float levelGrowth[3];
        uint8_t _60c[0x0C];
        int32_t crashCount;            // +0x618
        uint8_t _61c[0x0C];
        char name[16];                 // +0x628 the skater's name (the level records' holder)
        uint8_t _638[0x6C];
        // +0x6A4 per level (kLevelStatsSize each, by level index): +0 the
        // score's rollovers there, +8 the seconds played there.
        union
        {
            uint8_t levelStats[0x1E4];
            LevelStats statsByLevel[0x1E4 / kLevelStatsSize];
        };
        // +0x888 Animal Rescue: the animals carried (their objects, three at
        // most), how many, and how many were dropped (dropAnimals).
        ee::Ptr<zge::SceneObject> carriedAnimals[3];
        uint16_t carriedAnimalCount;   // +0x894
        uint16_t droppedAnimals;       // +0x896
        // +0x898 the skater's object slots (objects.cpp: lists of game
        // objects): the action's, two more, the ground object's (+0x8A4).
        uint32_t objectSlots[4];
        // +0x8A8 the current object's slot (the line's tricks; its front:
        // the object the skater acts with, while hasCurrentObject).
        uint32_t currentObjectSlot;
        uint32_t hasCurrentObject;     // +0x8AC (it is told "TrickUpdate" each frame: juice.cpp's updateCurrentObject)
        // +0x8B0 the jump's spin so far in half turns (180 degrees), each
        // way (score.cpp: every full turn of the two together adds to the
        // multiplier, every half turn 25 points; the HUD's trick list shows them).
        int32_t spinsCounterClockwise;
        int32_t spinsClockwise;        // +0x8B4
        // +0x8B8 the HUD's skater widgets (hud/hud.h).
        ee::Ptr<hud::SkaterHud> hud;
        ee::Ptr<zge::zgui::Node> juiceMeter; // +0x8BC the HUD's juice carton (a zgui widget)
        union
        {
            uint64_t flagsAndScore;        // the two words below as one, as the flags are changed
            struct
            {
                uint32_t scoreFlags;       // +0x8C0 bit 0: jizzed (the juice meter full), bit 1 (setSkaterPoints)
                // +0x8C4 the score's record: the score at +0x14 (skaterScore,
                // setSkaterScore; the HUD's score counter).
                union
                {
                    uint32_t score;
                    ee::Ptr<LevelRecord> scoreRecord;
                };
            };
        };
        // +0x8C8 the trick score waiting to be banked on landing, and the
        // multiplier it is banked with (the tricks in the line).
        ee::Float pendingScore;
        int32_t multiplier;            // +0x8CC
        uint32_t unreadLineWord;       // +0x8D0 cleared with the line's score; nothing reads it
        int32_t splitScore;            // +0x8D4 the score the split-screen points show (updatePlayersStep)
        // +0x8D8 the HUD's points counter: a career challenge's score
        // (setChallengeScore), the split-screen games' points (skaterPoints / setSkaterPoints).
        int32_t points;
        CardHand cardHand;             // +0x8DC
        // +0x8F8 .. +0x91C the rigs setupSkater builds and the bone
        // controllers sway.cpp moves, 0 where the model has none.
        ee::Ptr<zge::AnimationChannel> swayBoneLeft;         // +0x8F8
        ee::Ptr<zge::AnimationChannel> swayBoneRight;        // +0x8FC
        ee::Ptr<zge::AnimationChannel> swayBoneCentre;       // +0x900
        ee::Ptr<zge::AnimateSystem> bodyAnimation;        // +0x904 the shirt or coat, freed with the player
        ee::Ptr<zge::AnimationChannel> bobBoneLeft;          // +0x908
        ee::Ptr<zge::AnimationChannel> bobBoneRight;         // +0x90C
        ee::Ptr<zge::AnimateSystem> bobAnimation;         // +0x910
        ee::Ptr<zge::AnimationChannel> hairBone;             // +0x914
        ee::Ptr<zge::AnimationChannel> hairBoneLower;        // +0x918
        ee::Ptr<zge::AnimateSystem> upDownAnimation;      // +0x91C the hair
        // +0x920 1 once the skater has splashed into water (until the
        // respawn): no fire trails, and a landing makes no ring or sound.
        uint32_t splashed;
        uint32_t lastGrindState;       // +0x924 the grind state whose rail had no sound of its own
        // +0x928 set: the skates' trails (kind 1) start, once (the vault's
        // and the poles' TrickUpdate set it; fx/skater_effects.cpp clears it).
        uint32_t startTrails;
        uint32_t wallTapping;          // +0x92C 1 while the rolling sound is a wall tap's (0xD)
    };

    inline RailRecord &railRecordOf(uint32_t playerAddress) { return ee::Ptr<Player>(playerAddress)->rail; }

    static_assert(sizeof(Player) == 0x930 && offsetof(Player, respawnLocator) == 0x8 && offsetof(Player, ragdoll) == 0xC);
    static_assert(offsetof(Player, bodyAnimation) == 0x904 && offsetof(Player, bobAnimation) == 0x910 && offsetof(Player, upDownAnimation) == 0x91C);

    using aggressive::SkaterProfile;

    // The skater's profile, as this player works on it.
    inline ee::Ptr<SkaterProfile> skaterProfile(ee::Ptr<Player> player)
    {
        return ee::Ptr<uint8_t>(&player->skaterIndex).cast<SkaterProfile>();
    }
    static_assert(offsetof(Player, pitchRate) == 0x150 && offsetof(Player, turnRate) == 0x154);
    static_assert(offsetof(Player, launchSegment) == 0x350 && offsetof(Player, launchObject) == 0x354 && offsetof(Player, launchLevelVelocity) == 0x360);
    static_assert(offsetof(Player, surfaceNormal) == 0x180 && offsetof(Player, surface) == 0x194);
    static_assert(offsetof(Player, drawFlags) == 0x04 && offsetof(Player, node) == 0x14);
    static_assert(offsetof(Player, zones) == 0x1C && offsetof(Player, rollSound) == 0x44);
    static_assert(offsetof(Player, drawnPosition) == 0x50 && offsetof(Player, drawnOrientation) == 0x60);
    static_assert(offsetof(Player, rollSoundSpeed) == 0x1F0 && offsetof(Player, grindSegment) == 0x210);
    inline RailRecord &railRecordOf(Player &player) { return player.rail; }

    static_assert(offsetof(Player, doubleGrindRails) == 0x260 && offsetof(Player, carriedMove) == 0x420);
    static_assert(offsetof(Player, timePlayed) == 0x5F8 && offsetof(Player, levelGrowth) == 0x600);
    static_assert(offsetof(Player, levelStats) == 0x6A4 && offsetof(Player, hud) == 0x8B8 && offsetof(Player, lastGrindState) == 0x924);
    static_assert(offsetof(Player, wallTapping) == 0x92C && offsetof(Player, splashed) == 0x920 && offsetof(Player, startTrails) == 0x928);
    static_assert(offsetof(Player, heightAboveGround) == 0x1C0 && offsetof(Player, overObject) == 0x344);
    static_assert(offsetof(Player, spinDegreesClockwise) == 0x1D4 && offsetof(Player, spinDegreesCounterClockwise) == 0x1D8);
    static_assert(offsetof(Player, holdTime) == 0x284 && offsetof(Player, objectSlots) == 0x898 && offsetof(Player, hasCurrentObject) == 0x8AC);
    static_assert(offsetof(Player, spinsCounterClockwise) == 0x8B0 && offsetof(Player, spinsClockwise) == 0x8B4);
    static_assert(offsetof(Player, noSpinTurn) == 0x78 && offsetof(Player, lastOrientation) == 0x110);
    static_assert(offsetof(Player, pipeHeight) == 0x1C4 && offsetof(Player, airTime) == 0x1DC);
    static_assert(offsetof(Player, pushSpeed) == 0x1E4 && offsetof(Player, distance) == 0x1F4);
    static_assert(offsetof(Player, railStart) == 0x220 && offsetof(Player, railCameFrom) == 0x250);
    static_assert(offsetof(Player, stateValue) == 0x264 && offsetof(Player, balanceSpeed) == 0x27C);
    static_assert(offsetof(Player, grindTime) == 0x288 && offsetof(Player, totalAirTime) == 0x294);
    static_assert(offsetof(Player, wall) == 0x2E0 && offsetof(Player, onMovingSurface) == 0x372);
    static_assert(offsetof(Player, crashCount) == 0x618);
    static_assert(offsetof(Player, lastHitNormal) == 0x160 && offsetof(Player, ride) == 0x430);
    static_assert(offsetof(Player, script) == 0x18 && offsetof(Player, contactObject) == 0x198);
    static_assert(offsetof(Player, lastRail) == 0x1A4 && offsetof(Player, railEvent) == 0x1F8);
    static_assert(offsetof(Player, unlockedTricks) == 0x5F0);
    static_assert(offsetof(Player, carriedAnimals) == 0x888 && offsetof(Player, droppedAnimals) == 0x896);
    static_assert(offsetof(Player, previousState) == 0x75 && offsetof(Player, landing) == 0x7C && offsetof(Player, poleVector) == 0x1B0);
    static_assert(offsetof(Player, grindMesh) == 0x218 && offsetof(Player, swingMade) == 0x1CC && offsetof(Player, expectedAirTime) == 0x298 && offsetof(Player, movingObject) == 0x378);
    static_assert(offsetof(Player, skidFlags) == 0x4F0 && offsetof(Player, skidPoints) == 0x500 && offsetof(Player, lastSkidPoints) == 0x520);
    static_assert(offsetof(Player, state) == 0x74 && offsetof(Player, settleFrames) == 0x79);
    static_assert(offsetof(Player, flags) == 0x80 && offsetof(Player, position) == 0x90);
    static_assert(offsetof(Player, velocity) == 0xA0 && offsetof(Player, orientation) == 0xB0);
    static_assert(offsetof(Player, groundNormal) == 0x170 && offsetof(Player, object) == 0x19C);
    static_assert(offsetof(Player, downTime) == 0x1E0 && offsetof(Player, speed) == 0x1E8);
    static_assert(offsetof(Player, shortTime) == 0x26C && offsetof(Player, balance) == 0x270);
    static_assert(offsetof(Player, shortCount) == 0x280 && offsetof(Player, stateTime) == 0x29C);
    static_assert(offsetof(Player, controls) == 0x550 && offsetof(Player, skaterIndex) == 0x5E0);
    static_assert(offsetof(Player, name) == 0x628 && offsetof(Player, juiceMeter) == 0x8BC);
    static_assert(offsetof(Player, scoreFlags) == 0x8C0 && offsetof(Player, points) == 0x8D8);
    static_assert(offsetof(Player, pendingScore) == 0x8C8 && offsetof(Player, multiplier) == 0x8CC);

    // Player::flags
    constexpr uint32_t kDown = 0x2;             // bailed: the respawn timer runs
    constexpr uint32_t kRespawnAtLocator = 0x4; // the next respawn goes to a named locator
    constexpr uint32_t kBraking = 0x8;          // braking on the ground (the rolling sound scrapes: 0x400)
    // Player::skidFlags
    constexpr uint32_t kSkidStart = 0x1;
    // Player::drawFlags
    constexpr uint32_t kDrawnSnap = 0x1;        // the drawn orientation jumps to the physics one (once)

    // A scene object's name (char[] at +0x12C).
    constexpr uint32_t kSceneObjectName = 0x12Cu;

    // The frame time the skater update was given.
    inline ee::Global<ee::Float, sym::g_skaterFrameTime> g_skaterFrameTime;
    // The developers' debug display level (low 2 bits; Toggle Debug, DEVTOOLS.md).
    inline ee::Global<uint8_t, sym::g_debugDisplay> g_debugDisplay;
    // The level index as a word (updateLevelRecord takes it whole).
    inline ee::Global<uint32_t, sym::g_levelNumber> g_levelWord;

    // Engine and game functions the update calls (not ported).
    constexpr uint32_t kRespawn = sym::respawn;         // (player, 0, 0)
    constexpr uint32_t kCrashJuiceLoss = sym::crashJuiceLoss;  // (player) Juice Lost/Level on crash
    constexpr uint32_t kUpdateLevelRecord = sym::updateLevelRecord; // (record, level, holder name, value) -> 1 if beaten
    constexpr uint32_t kLevelRecordTopSpeed = 11;      // the record the update feeds the speed to (inferred)
    constexpr uint32_t kDebugPrint = sym::debugPrint;      // (x, y, format, ...) the debug text overlay
    // The tuning table's General / Respawn Time: seconds down before the respawn.
    constexpr uint32_t kTuningRespawnTime = 1; // entry of category 0 (aggressive/tuning.h)

    // padHeld / padPressed: whether any of the game buttons is held
    // / was pressed this frame on the pad.
    int32_t padHeld(uint32_t buttons, uint32_t port);
    int32_t padPressed(uint32_t buttons, uint32_t port);
    // skaterControlsUpdate: reads the skater's controls for the frame (Controls).
    // Always 1.
    int32_t skaterControlsUpdate(ee::Ptr<Player> player, ee::Float frameTime);

    // juiceUpdate: the juice for one frame: full ("jizzed") and back, the
    // drain, the low-juice warning, an empty meter in the Career (Death by
    // Juice). Returns whether the meter has a PotentialTangAmount.
    int32_t juiceUpdate(ee::Ptr<Player> player, ee::Float frameTime);

    // moveSkater: moves the skater from `from` to `to` for a frame: the part
    // of the step along the skater's forward axis first, then the rest, each
    // swept through the collision query (collisionQuery); a hit goes to the
    // collision response skaterCollisionResponse, whose result this returns (0 when
    // nothing was hit). Clears the ground normal; a skater on the ground
    // (or in the air) with a contact gets keepContact.
    int32_t moveSkater(ee::Ptr<Player> player, ee::Ptr<Vec4> from, ee::Ptr<Vec4> to, ee::Float frameTime);

    // skaterPoints: the skater's points counter (Player::points).
    int32_t skaterPoints(ee::Ptr<Player> player);
    // setSkaterPoints: sets them and tells the score display (showScore while
    // scoreFlags bit 1 is set, else showPoints outside split screen).
    void setSkaterPoints(ee::Ptr<Player> player, int32_t points);
    int32_t skaterScore(ee::Ptr<Player> player);                                 // (score.cpp)
    void setSkaterScore(ee::Ptr<Player> player, uint32_t score, int32_t animate);
    uint32_t cartonSize(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> out);                     // (juice.cpp): writes *size
    int32_t juiceAmount(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> out);                   // writes *amount
    int32_t setJuiceMeter(ee::Ptr<zge::zgui::Node> meter, ee::Float juice);
    void levelUpAttribute(ee::Ptr<Player> player, uint32_t statArgument);        // (experience.cpp)
    void levelUpBonus(ee::Ptr<Player> player);
    // crashJuiceLoss: a crash costs juice: no longer jizzed, and Juice
    // Lost/Level on crash times the skater's level off the carton (where
    // the level drains juice). Always 1.
    int32_t crashJuiceLoss(ee::Ptr<Player> player);

    // setSkaterState: the skater changes state (the old state's exit, the
    // new one's enter, the scoring told; states.cpp).
    void setSkaterState(ee::Ptr<Player> player, uint32_t state);
    // requestMove: a move asked of the skater's state (a trick's kind, the
    // action button: states.cpp); the state entered, or -1.
    int32_t requestMove(ee::Ptr<Player> player, int32_t move);

    // skaterUpdate: the skater's physics for one frame.
    void skaterUpdate(ee::Ptr<Player> player, ee::Float frameTime);
    // alignToGroundNormal: the Ground state's 4th callback: stands the skater on the
    // ground normal (up = the normal, the other axes made square to it).
    void alignToGroundNormal(ee::Ptr<Player> player);
    constexpr uint32_t kOrthonormalize = sym::orthonormalize; // (forward, right, up) VU0
}

namespace aggressive::skater
{
    // (experience.cpp)
    void awardExperience(ee::Ptr<Player> player, uint32_t stat, int32_t points);
    void computeLevel(ee::Ptr<SkaterProfile> profile);
    uint32_t levelPointsFor(uint32_t level);
}

namespace aggressive::skater
{
    // (move_collide.cpp)
    ee::Ptr<TrafficCar> trafficCarOf(ee::Ptr<zge::SceneObject> sceneObject);
    int32_t trafficCarVelocity(ee::Ptr<TrafficCar> car, ee::Ptr<Vec4> out);
}

namespace aggressive::skater
{
    // (player.cpp)
    void playerUpdate(ee::Ptr<Player> player, ee::Float frameTime);
    uint32_t skaterOrientedBox(ee::Ptr<Player> player, ee::Ptr<zge::OrientedBox> box);
}

namespace aggressive::skater
{
    // (player_setup.cpp)
    void debugMenuHook();
    void freeSkaterClasses();
    int32_t loadSkaterMotions();
    void nodeToPosition(ee::Ptr<Player> player);
    void placeSkaterAtLevelStart(ee::Ptr<Player> player, uint32_t locator, uint32_t resetTricks);
    uint32_t playSkaterAnimation(ee::Ptr<Player> player, uint32_t name, ee::Float blend);
    uint32_t playerIndexOfNode(ee::Ptr<zge::SceneObject> node);
    void removePlayer(ee::Ptr<Player> player);
    uint32_t resetPlayerProfile(ee::Ptr<SkaterProfile> profile, uint32_t skaterArgument);
    void respawn(ee::Ptr<Player> player, uint32_t atStart, uint32_t resetTricks);
    uint32_t setupSkaterForRun(ee::Ptr<Player> player);
    int32_t stepAnimators(ee::Float seconds);
}

namespace aggressive::skater
{
    // (reset.cpp)
    void resetParkCost();
}

namespace aggressive::skater
{
    // (rig.cpp)
    uint32_t setupSkater(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (score.cpp)
    int32_t clockwiseSpins(ee::Ptr<Player> player);
    int32_t counterClockwiseSpins(ee::Ptr<Player> player);
    void freezeMultiplayerScore(ee::Ptr<Player> player);
    uint32_t isJizzed(ee::Ptr<Player> player);
    int32_t noTrickPending(ee::Ptr<Player> player);
    ee::Float pendingTrickScore(ee::Ptr<Player> player);
    uint64_t setJizzed(ee::Ptr<Player> player, uint32_t on);
    void setSplitScore(ee::Ptr<Player> player, int32_t score);
    int32_t showFinalScore(ee::Ptr<Player> player, uint32_t showPoints);
    int32_t splitScore(ee::Ptr<Player> player);
    uint32_t trickKeyOf(ee::Ptr<GameObject> object);
}

namespace aggressive::skater
{
    // (surface.cpp)
    ee::Ptr<zge::ZgePart> collisionMeshOf(ee::Ptr<zge::Node> node);
    uint32_t surfaceKind(ee::Ptr<zge::Node> node, uint32_t face);
    uint32_t surfacePhysics(ee::Ptr<zge::Node> node, uint32_t face);
}

namespace aggressive::skater
{
    // (text_object.cpp)
    ee::Ptr<GameObject> newTextObject(ee::Address text, uint32_t multiplierBonus, ee::Float worth);
}

namespace aggressive::skater
{
    // (tricks.cpp)
    void initTrickMasks();
}
