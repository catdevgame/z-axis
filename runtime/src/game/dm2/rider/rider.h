#pragma once

// Dave Mirra Freestyle BMX 2's rider (DM2, SLUS_201.59): the rider record,
// the pads' button records and the contact records the rider's physics
// (physics.c, from physicsInit on) and action states (from riderReset on) keep, as far as
// the ported functions read them.
//
// These are DM2's own functions, compiled in DM2's instance only and
// registered for DM2 there (READABLE_FUNCTION): they install only when DM2 runs, never over Aggressive
// Inline's code at the same addresses. Offsets are the PS2 build's; the
// GameCube build (the counterparts named in each function's comment) keeps
// the record's first 0x1000 bytes where they are and moves the physics
// block (+0x1000 here is +0x3780 there).
//
// Stack: every ported function that calls out keeps the original's frame
// (ee::StackFrame of its size), so its callees run at the stack depth DM2
// gives them, and levelBounds writes its frame's copy as the original does.
// DM2 reads stale stack in places (a vector's w from an uninitialised
// slot; a global keeping a callee's sp): without them dm2logic's RAM images
// differed in 279 bytes, then 5, at the level's start (positions equal).
//
// Names come from reading the code in the three DM2 builds (no build names
// these functions or fields); "(inferred)" marks a meaning read from use
// only.

#include "ee/ee.h"
#include "ee/plain.h"
#include "dm2/rider/vector.h"
#include "zge/scene/bone_chain.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zge
{
    struct AnimationChannel; // zge/scene/animation.h: a model's bone
    struct BoneMirror;  // zge/scene/animation.h
    struct RigidBody;   // zge/collision/dynamics.h
    struct Motion;      // zge/scene/animation.h
}

namespace game::dm2
{
    struct NewRider; // players.h
}

namespace game::dm2::rider
{
    // A direction as the trick tables index it (stickDirection's table,
    // g_directionByButtons): none, the four, then the diagonals.
    enum Direction : uint8_t
    {
        kDirNone = 0,
        kDirLeft = 1,
        kDirRight = 2,
        kDirUp = 3,
        kDirDown = 4,
        kDirUpLeft = 5,
        kDirUpRight = 6,
        kDirDownLeft = 7,
        kDirDownRight = 8,
    };

    // The low four bits of a button record's held and changed words: the
    // d-pad, and the left stick past half way (buttonTest, stickDirection).
    constexpr uint32_t kButtonUp = 0x1u;    // stick y < -0.5
    constexpr uint32_t kButtonDown = 0x2u;  // stick y > 0.5
    constexpr uint32_t kButtonLeft = 0x4u;  // stick x < -0.5
    constexpr uint32_t kButtonRight = 0x8u; // stick x > 0.5
    constexpr uint32_t kButtonDirections = 0xFu;

    // How buttonTest tests a set of buttons (the move table's modes; the
    // GameCube's buttonTest, 0x8005A0AC, numbers them differently and has
    // one more, all down and all pressed).
    enum ButtonMode : uint32_t
    {
        kAnyPressed = 0,        // one of them down and changed this frame
        kAllDownOnePressed = 1, // all down, one of them changed
        kAllDown = 2,
        kAnyDown = 3,
        kAllUpOneReleased = 4,  // all up, one of them changed
        kAnyReleased = 5,       // one of them up and changed
        kAlways = 6,
        // 7 and above: never
    };

    struct Stick
    {
        ee::Float x, y;
    };

    // A pad's buttons as the game reads them each frame (portRecords fills
    // the ports' records, g_pads[port].buttons): held and changed buttons, the sticks,
    // and the sticks a frame ago.
    struct ButtonRecord
    {
        uint32_t held;    // +0x00
        uint32_t changed; // +0x04 went down or up this frame
        Stick sticks[2];  // +0x08 left, +0x10 right
        Stick previous[2]; // +0x18 the sticks last frame
    };
    static_assert(sizeof(ButtonRecord) == 0x28);
    static_assert(offsetof(ButtonRecord, previous) == 0x18);

    // A port's pad (g_pads[port]).
    struct PadInput
    {
        uint32_t pad;        // +0x00 the pad driver's state (portRecords reads +4 connected, +8 buttons)
        uint32_t ignored;    // +0x04 buttons ignored until let go (inferred)
        uint32_t heldAside;  // +0x08 the held buttons kept while something else has the pads (takePads)
        uint16_t flags;      // +0x0C (kPadMissing)
        uint16_t repeatWait; // +0x0E frames until a held direction repeats for the menus (the action's frame
                             //       while a game screen is up counts it; the pads' set-up, clear and a pad's
                             //       return clear it, as BMX XXX's do)
        ButtonRecord buttons; // +0x10
    };
    static_assert(offsetof(PadInput, repeatWait) == 0x0E && offsetof(PadInput, buttons) == 0x10);
    static_assert(sizeof(PadInput) == 0x38);
    // PadInput::flags: no pad was connected at the port when the pads were made.
    constexpr uint16_t kPadMissing = 0x1;

    // One entry of the move table (g_moveLayouts: for each of the three
    // controller configurations, 22 moves of three tests): the buttons and
    // how they are tested.
    struct MoveTest
    {
        uint32_t buttons;
        uint32_t mode; // ButtonMode
    };
    struct Move
    {
        MoveTest tests[3];
    };
    static_assert(sizeof(Move) == 0x18);
    constexpr uint32_t kMovesPerLayout = 22;
    struct MoveLayout
    {
        Move moves[kMovesPerLayout];
    };
    static_assert(sizeof(MoveLayout) == 0x210);

    // What a contact record holds by kind (rider +0xFF1, a wheel's +0x00):
    // a face (node and face index, 8 bytes) for every kind but an object's
    // (4 bytes, the object), up to kContactKinds.
    constexpr uint8_t kContactObject = 8;
    constexpr uint32_t kContactKinds = 11;

    // A wheel's contact (rider +0xF90 + 0x20 x wheel; setWheelContact).
    struct WheelContact
    {
        uint8_t kind;      // +0x00
        uint8_t _01[7];
        uint8_t contact[8]; // +0x08 the face or object (by kind)
        ee::Float point[4]; // +0x10 where it touches
    };
    static_assert(sizeof(WheelContact) == 0x20);

    // A wheel's pose (rider +0x1560 + 0x80 x wheel; physicsFrame turns it,
    // resetPhysics clears it).
    struct WheelPose
    {
        ee::Float velocity[4]; // +0x00 the wheel's velocity (groundProbe's hitClosing takes it; turned with the body; cleared on a reset)
        ee::Float speed;    // +0x10 its length (groundImpact's dust: 15 a unit; cleared on a reset)
        uint8_t _14[0x20 - 0x14];
        ee::Float matrix[16]; // +0x20 the wheel's matrix (turned with the body; groundProbe's glancing hit makes its forward row the velocity's direction)
        uint8_t ground;     // +0x60 how it is on the ground (WheelGround)
        uint8_t touched;    // +0x61 bit 0: groundProbe's ray met something (inferred)
        int16_t angle;      // +0x62 the wheel's turn (0x10000 a turn)
        int32_t spin;       // +0x64 added to it a frame (its low 16 bits); the rolling sound's rate
        ee::Float springLength; // +0x68 the wheel's spring: its length (the wheel's height below the body, kept
                                //       between Rider::wheelHeightLeast and wheelHeightMost; groundMove, airMove,
                                //       wheelieMove), groundAhead's clearance is the new height less it,
        ee::Float springRate;   // +0x6C and its change a frame (the spring's force over the mass, damped)
        uint32_t sound;     // +0x70 the surface sound under the wheel (groundAfter's rolling sound)
        ee::Ptr<zge::AnimationChannel> bone; // +0x74 the wheel's bone
        uint8_t _78[8];
    };
    // WheelPose::ground: rolling (a reset's), sliding (groundMove's sliding
    // branch), off the ground (groundAfter, airMove and groundMove skip it).
    enum WheelGround : uint8_t
    {
        kWheelRolling = 0,
        kWheelSliding = 1,
        kWheelOffGround = 2,
    };
    static_assert(offsetof(WheelPose, ground) == 0x60 && offsetof(WheelPose, matrix) == 0x20 && offsetof(WheelPose, angle) == 0x62 && offsetof(WheelPose, bone) == 0x74);
    static_assert(sizeof(WheelPose) == 0x80 && offsetof(WheelPose, springLength) == 0x68 && offsetof(WheelPose, springRate) == 0x6C);

    // A bail's hand (rider +0x1470 left, +0x14B0 right): bailHandContact
    // fills it with the edge the hand holds (its two ends in its node's
    // space, the node, the share along it), bailHandsHeld holds the hand's
    // body there.
    struct BailHand
    {
        ee::Float edgeStart[4]; // +0x00
        ee::Float edgeEnd[4];   // +0x10
        uint32_t node;          // +0x20
        uint32_t nodeHandle;    // +0x24 the node as a handle (the tags')
        uint32_t body;          // +0x28 the hand's body (a zge::BodyContact)
        uint32_t heldBody;      // +0x2C the rigid body that contact is on (freeRider takes it off this one)
        ee::Float share;        // +0x30 from edgeStart toward edgeEnd
        uint8_t holding;        // +0x34 an edge found (riderReset and bailHands clear it)
        uint8_t _35[0x40 - 0x35];
    };
    static_assert(sizeof(BailHand) == 0x40 && offsetof(BailHand, body) == 0x28 && offsetof(BailHand, holding) == 0x34);

    // A tick of the rider's own trick slots and the trick availability bits.
    constexpr uint8_t kNoTrick = 0xFF;

    // The turn's score records (the score block at Rider +0x1A0; game.c's
    // trick scoring, trickScoreFrame). A line is a run of tricks scored
    // together: a new line opens when a trick begins (below 31 lines), and
    // each trick landed (64 at most) is chained under the current line.
    struct ScoreTrick
    {
        uint8_t flags;               // +0x00 kScoreTrickFakie, kScoreTrickBackTakeoff
        uint8_t trick;               // +0x01 the trick (kNoTrick none: the current trick is compared with it)
        uint8_t trickMotion;         // +0x02 the rider's trickMotion with it (kNoTrick none)
        uint8_t loops;               // +0x03 the rider's trickLoops
        uint8_t turns;               // +0x04 a turning trick's whole turns (Rider::trickTurned, rounded; 3 at most)
        uint8_t _05;
        uint16_t framesHeld;         // +0x06 the frames the trick has been held
        ee::Float distance;          // +0x08 the rider's speed summed over the frames of a grind or a wheelie
        uint32_t points;             // +0x0C the trick's points (scoreLines)
        ee::Ptr<ScoreTrick> next;    // +0x10 the line's next trick
    };
    static_assert(sizeof(ScoreTrick) == 0x14 && offsetof(ScoreTrick, next) == 0x10);
    struct ScoreLine
    {
        uint8_t flags;               // +0x00 kScoreLineSpinFlag
        uint8_t spinHalfTurns;       // +0x01 the air's spin so far in half turns (8 at most)
        uint8_t landingGrade;        // +0x02 trickEvent: 0, or 1 to 3 by how far the landing's travel is off
                                     //       the rider's forward (below 10 degrees 1, 25 to 90 degrees 2, more 3)
        uint8_t gap;                 // +0x03 the level gap the line made (trickEvent: findGap's), kNoGap none
        uint32_t points;             // +0x04 the line's points: its tricks', its spin's and its gap's (scoreLines)
        ee::Ptr<ScoreTrick> tricks;  // +0x08 the line's first trick (0: none landed yet)
    };
    static_assert(sizeof(ScoreLine) == 0x0C && offsetof(ScoreLine, tricks) == 0x08);
    // ScoreTrick::flags: a grind (tricks 0x33 .. 0x3B) ridden backwards; the
    // air spun after a take-off rolling backwards.
    constexpr uint8_t kScoreTrickFakie = 0x02;
    constexpr uint8_t kScoreTrickBackTakeoff = 0x04;
    // ScoreLine::flags: set for a spin off a lip by which way it turns
    // against the way the rider travels (trickScoreFrame; what the scoring
    // makes of it: trickLineEnd, not read yet).
    constexpr uint8_t kScoreLineSpinFlag = 0x02;
    // ScoreLine::flags: set where the line's text says "opposite" (with
    // kScoreLineSpinFlag it says "alley-oop": dm2TrickText; what sets it is
    // not read yet).
    constexpr uint8_t kScoreLineOpposite = 0x01;
    constexpr uint32_t kScoreLines = 32;
    constexpr uint32_t kScoreTricks = 64;
    // The turn's counts of tricks (Rider::trickRepeats, cellTrickCounts).
    constexpr uint32_t kRepeatRows = 73;
    constexpr uint32_t kRepeatRowBytes = 22;
    constexpr uint32_t kTrickCountRows = 17;
    constexpr uint32_t kTrickCountKinds = 7;
    constexpr uint32_t kAllTips = (1u << 22) - 1; // Rider::tipsLeft with each of the 22 tips still to show

    // A competition's run as its judges scored it (Rider::competitionRuns;
    // judgeScore): the run's points, the five judges' scores (a byte each,
    // 255 the most) and their average.
    struct CompetitionRun
    {
        uint32_t points;             // +0x00 the turn's score as a whole number
        uint8_t judges[5];           // +0x04
        uint8_t average;             // +0x09
        uint8_t _0a[2];
    };
    static_assert(sizeof(CompetitionRun) == 0x0C && offsetof(CompetitionRun, average) == 0x09);
    constexpr uint32_t kCompetitionJudges = 5;
    constexpr uint32_t kCompetitionRuns = 2;

    // The rider's score block (Rider +0x1A0): the turn's and the game's score, the turn's trick lines and
    // counts, and the level goals' bits. game.c's scoring functions and the overlay's trick display take its
    // address; the offsets here are the block's own.
    struct ScoreBlock
    {
    ee::Float turnScore;         // +0x000 the score of the rider's turn: cleared at the turn's start, set
                                 //        against the turn's records when its time is up (setRoundPhase)
    uint32_t gameScore;          // +0x004 the rider's score in the game: a single-player game's the turn's,
                                 //        a multiplayer mode's the best turn or the turns' sum; startGame
                                 //        clears it ("$KSTR_SCORE %6ld")
    uint32_t linePoints;         // +0x008 the points the trick display shows under the current line's
                                 //        tricks ("%ld points")
    uint32_t horseLetters;       // +0x00C the letters of the HORSE word (multiplayer mode 3) the rider has
                                 //        taken, one a turn without a score; out at the word's length
    ee::Float kindScores[3];     // +0x010 the turn's scores of three kinds of trick, summed as a line ends
                                 //        (tricks 0x33 .. 0x3B, 0x46 and 0x47; trickLineEnd); cleared as the
                                 //        turn's clock starts
    ScoreLine lines[kScoreLines];    // +0x01C the turn's trick lines (ScoreLine)
    ScoreTrick tricks[kScoreTricks]; // +0x19C the turn's tricks, chained under their lines (ScoreTrick)
    uint8_t lineState;           // +0x69C the current line (trickScoreFrame): 0 none, 1 a trick begun (a line
                                 //        opened), 2 landed and going on, 3 ended with its tricks recorded, 4
    uint8_t lineCount;           // +0x69D the lines in `lines` (trickEvent and goalObjectTrick read the last)
    uint8_t unreadLineByte;      // +0x69E cleared as a line opens (trickScoreFrame, mode 5's bail distance);
                                 //        nothing reads it (BMX XXX's +0x1A22 likewise)
    uint8_t trickCount;          // +0x69F the tricks in `tricks`
    uint8_t trickRepeats[kRepeatRows][kRepeatRowBytes]; // +0x6A0 how often the turn landed each trick: a row
                                 //        a trick (its id plus one), two bits a variation (up to 3;
                                 //        trickLineEnd counts, a turn's start clears)
    int16_t cellTrickCounts[kTrickCountRows][kTrickCountKinds]; // +0xCE6 the turn's tricks counted by kind
                                 //        in each of the level's 16 cells (the level's quarters), and
                                 //        in the last row over the whole level (trickLineEnd)
    ee::Float heldTrickHeight;   // +0xDD4 the most of Rider::groundHeight seen while the line's trick was one of
                                 //        0x43 .. 0x45 (trickScoreFrame); cleared as a line begins
    uint32_t tipsLeft;           // +0xDD8 the tips not shown yet, a bit each (trickScore shows a tip when
                                 //        its moment comes and clears its bit; kAllTips at a turn's start
                                 //        for a player with tips on)
    uint32_t goalFlags;          // +0xDDC flags of the level's goals: 0x8000 set with the character's LevelGoals
                                 //        flag 0x40 for the current level (the record check, addTrick)
    uint32_t gapsFound;          // +0xDE0 a bit per gap of the level found in this game (trickLineEnd sets it
                                 //        with the character's LevelGoals::gapsFound); cleared at a level-goals
                                 //        game's spawn
    uint32_t respectEarned;      // +0xDE4 the respect the turn's goals earned (goalsFrame adds a goal's as it is
                                 //        done; the round's rewards read it); cleared at the spawn
    uint32_t recordBeaten;       // +0xDE8 1 once the rider beat a game record and its reward was shown
                                 //        (roundRewards); cleared at the spawn, read by nothing
    uint8_t _dec[0xDF0 - 0xDEC];
    };
    static_assert(sizeof(ScoreBlock) == 0xDF0 && offsetof(ScoreBlock, gameScore) == 0x004 && offsetof(ScoreBlock, horseLetters) == 0x00C);
    static_assert(offsetof(ScoreBlock, kindScores) == 0x010 && offsetof(ScoreBlock, lines) == 0x01C && offsetof(ScoreBlock, tricks) == 0x19C);
    static_assert(offsetof(ScoreBlock, lineState) == 0x69C && offsetof(ScoreBlock, lineCount) == 0x69D && offsetof(ScoreBlock, trickCount) == 0x69F);
    static_assert(offsetof(ScoreBlock, unreadLineByte) == 0x69E);
    static_assert(offsetof(ScoreBlock, trickRepeats) == 0x6A0 && offsetof(ScoreBlock, cellTrickCounts) == 0xCE6 && offsetof(ScoreBlock, tipsLeft) == 0xDD8);
    static_assert(offsetof(ScoreBlock, goalFlags) == 0xDDC && offsetof(ScoreBlock, gapsFound) == 0xDE0 && offsetof(ScoreBlock, recordBeaten) == 0xDE8);

    // The rider record (g_riders[index]; about 0x1700 bytes).
    struct Rider
    {
        // The rider model's name (newRider: "Rider%02d", "Rider%02dC" for the
        // character's second outfit, a request's head's own); the record's
        // first 0x130 bytes are a request's RiderHead (players.h).
        char riderModelName[0x20];   // +0x000
        // The rider's eight stats, the select-player menu's (MENU/MAIN.ZSC:
        // "air:", "speed:", "spins:", "balance:", "technique:", "manuals:",
        // "landing:", "wallrides:"), in its order: creating a rider makes
        // each the stat's least plus the rider's level in it (up to 12) in
        // twelfths of the way to its most (0.85 .. 1; balance 0.75, wall
        // rides 0.5).
        ee::Float airStat;           // +0x020 the hop's height (crouchState)
        ee::Float speedStat;         // +0x024 the top speed: a wheel's speed is kept below 0.35 times this
                                     //        (groundMove); pedalling below 0.2705 times it
        ee::Float spinStat;          // +0x028 the air's spins (wheelieSubState)
        ee::Float balanceStat;       // +0x02C the grind's balance (grindBalance tips less with more)
        ee::Float techniqueStat;     // +0x030 the trick motions' rate (airTrickB, airTrick)
        ee::Float manualStat;        // +0x034 the wheelie's balance (wheelieSubState)
        ee::Float landingStat;       // +0x038
        ee::Float wallRideStat;      // +0x03C the less of the air's gravity on a lip or wall ride (airGravity)
        uint8_t ownTricks[2];        // +0x040 the tricks tapTrick's codes 0x57 and 0x58 stand for
        uint8_t tricksOff[0x14];     // +0x042 one bit a trick: set, the rider cannot do it (the character's
                                     //        tricksOff, newRider copies them)
        uint8_t _056[0x60 - 0x56];
        char bikeModelName[0x20];    // +0x060 the bike model's name (newRider: "Bike%02d_%d")
        int16_t barsTilt;            // +0x080 the bars' tilt about their right row (a stat; poseBikeGround)
        uint8_t _082[0x90 - 0x82];
        ee::Float seatGrip[4];       // +0x090 a hand's point on the bike's "Hipsdeck" part, in its space (the seat
                                     //        grab poses put a hand there, poseRiderBones)
        ee::Float grips[2][4];       // +0x0A0 the hands' grips in the bars' part's space (stats; inferred left, right)
        // The suspension (groundMove; inferred from use): each wheel's
        // spring (WheelPose::springLength, springRate) pulled
        // towards wheelHeightMost by springStiffness over springMass in the
        // air, over half of mass on the ground (with the bike's weight
        // there), damped by springDamping (shortening, lengthening).
        ee::Float massHeight;        // +0x0C0 added to a wheel's height in the weight's share (its factor is 0)
        ee::Float mass;              // +0x0C4
        ee::Float springMass;        // +0x0C8
        ee::Float wheelHeightLeast;  // +0x0CC a wheel's height above the ground kept between these (stats; groundAhead)
        ee::Float wheelHeightMost;   // +0x0D0
        ee::Float springStiffness;   // +0x0D4
        ee::Float springDamping[2];  // +0x0D8 shortening, lengthening
        ee::Float grip;              // +0x0E0 the wheels' grip and slip by the surface's (g_surfaces)
        ee::Float slip;              // +0x0E4
        uint8_t _0e8[0xF0 - 0xE8];
        // The wheels' points in the body's space. Wheel 0 is the front one
        // (groundMove's forward row runs from wheel 1 to wheel 0, the
        // leading wheel probes for walls, the drive pushes wheel 1). Each
        // point's height follows it (wheelieMove reads the height 0x10 past
        // the wheel's point).
        ee::Float frontWheelPoint[4]; // +0x0F0
        ee::Float frontWheelHeight;  // +0x100 the stats' wheel heights: half their sum lifts the body off the ground (inferred; wheel 0's)
        uint8_t _104[0x110 - 0x104];
        ee::Float backWheelPoint[4]; // +0x110
        ee::Float backWheelHeight;   // +0x120 (wheel 1's)
        uint8_t _124[0x130 - 0x124];
        uint8_t controllerLayout;    // +0x130 the controller configuration: which of the move table's three layouts reads its buttons
        uint8_t tapCount;            // +0x131
        uint8_t taps[4];             // +0x132 the direction taps (Direction), oldest first
        uint8_t _136[2];
        ButtonRecord input;          // +0x138 the rider's buttons this frame
        uint32_t flags;              // +0x160 (kFlag...)
        uint8_t player;              // +0x164
        uint8_t actionStates[4];     // +0x165 the action state levels (setActionState)
        uint8_t _169;
        uint16_t actionFrames[4];    // +0x16A frames in each level's state
        uint8_t trick;               // +0x172 the current trick
        uint8_t trickMotion;         // +0x173 the current trick's motion (or a second trick)
        uint8_t trickLoops;          // +0x174 the times the trick's motion has played through (its start, then each
                                     //        held loop that counts); trickScoreFrame keeps it with the trick
        uint8_t topTrickLoopFrames;  // +0x175 the frames a loop of the trick on top takes (15 more than 225
                                     //        times what the technique stat leaves of 1): held, it counts a
                                     //        loop each time round; poseRiderBones turns the bars through it
        uint8_t soundWait;           // +0x176 frames before sound event 0x18 may play again (45 when it plays;
                                     //        riderFrame counts it down, riderReset clears it)
        uint8_t health;              // +0x177 the rider's health, BMX XXX's Rider::health (+0x194) a version early:
                                     //        kFullHealth at the spawn (spawnPlayer) and given to the overlay each
                                     //        rider frame (gameRiderFrame) and as a turn starts (setRoundPhase), as
                                     //        BMX XXX's are; DM2 shows none (the overlay's frame kOverlayEmpty keeps
                                     //        the byte, read by nothing) and nothing else changes it
        uint8_t bailKind;            // +0x178 why the rider bailed (7, 8 a trick not finished on landing; 9 deep water)
        uint8_t _179[0x17C - 0x179];
        int32_t yawTurned;           // +0x17C the air's turn about the up row so far (its yaw rate added a frame;
                                     //        0x10000 a turn): trickScoreFrame counts it in half turns
        int32_t trickTurned;         // +0x180 a turning trick's own turn so far (airTrick's steps; 0x10000 a
                                     //        turn): trickScoreFrame counts it in whole turns
        ee::Float skidDistance;      // +0x184 groundMove adds a sliding wheel's speed (inferred)
        int32_t rideDistance;        // +0x188 (addRideDistance; inferred a distance or time stat)
        int32_t balanceFrames;       // +0x18C frames counted by riderFrame, cleared each riding frame and on a
                                     //        grind's start: the grind's and the wheelie's tip grows with it
        int16_t landingTravelAngle;  // +0x190 at a landing, the angle of the travel along the ground from the forward
                                     //        row (airMove; airEnter clears it)
        uint8_t _192[2];
        ee::Float edgeHeight;        // +0x194 at a take-off over a rail's or ramp's edge, the rider's y less the
                                     //        edge's nearest point's (airEnter; y grows downwards); crouchState's
                                     //        hop is a weak one under 0.5
        uint8_t _198[0x1A0 - 0x198];
        ScoreBlock score;            // +0x1A0 the turn's and the game's score, the trick lines, the goals' bits
        WheelContact wheels[2];      // +0xF90 back, front (inferred order)
        CompetitionRun competitionRuns[kCompetitionRuns]; // +0xFD0 a competition's runs judged (judgeScore)
        uint8_t _fe8[0xFF0 - 0xFE8];
        uint8_t physicsState;        // +0xFF0 (PhysicsState)
        uint8_t contactKind;         // +0xFF1
        int16_t snapFrames;          // +0xFF2 frames left of a snap or alignment
        uint8_t _ff4[0x1000 - 0xFF4];
        ee::Float right[4];          // +0x1000 the rows: right,
        ee::Float up[4];             // +0x1010 up (upright (0, -1, 0)),
        ee::Float forward[4];        // +0x1020 forward
        ee::Float rowsOrigin[4];     // +0x1030 (0, 0, 0, 1): right .. here are a matrix, the model's rotation
        ee::Float position[4];       // +0x1040 the physics position (y grows downwards)
        ee::Float rootOffset[4];     // +0x1050 the drawn rider's offset from it (the motion's root in the world)
        ee::Float groundHeight;      // +0x1060 the height above the ground below: each write is a ray's hit less its start
                                     //         (airMove, the take-off's probe, the stall's lip; 0 on a grind or a bail);
                                     //         physicsFrame's model tint and the air's shadow fade with it
        ee::Float balance;           // +0x1064
        ee::Float barLean[2];        // +0x1068 the bars' lean
        uint32_t touchedObject;      // +0x1070 the object last touched (touchObject); cleared on every physics state change
        uint32_t touchedPart;        // +0x1074 its part (kOwnerObject's second result)
        uint8_t _1078[0x1080 - 0x1078];
        ee::Float touchPoint[4];     // +0x1080 the touch in the object's part's space (inferred)
        ee::Float touchRotation[4];  // +0x1090 the part's rotation as a quaternion

        ee::Float velocity[4];       // +0x10A0 units a frame
        ee::Float force[4];          // +0x10B0 this frame's acceleration (the air's gravity), units a frame a frame
        ee::Float snapStep[4];       // +0x10C0 a snap's move a frame (grindApproachEnter sets it; approachGravity adds gravity)
        int16_t pitch;               // +0x10D0 the air's angles (0x10000 a turn)
        int16_t yawRate;             // +0x10D2
        int16_t roll;                // +0x10D4
        int16_t bodyPitch;           // +0x10D6 the drawn body's turn about its right row (inferred: the crouch's lean)
        ee::Float speed;             // +0x10D8 |velocity|
        uint8_t _10dc[0x1120 - 0x10DC];
        uint8_t contact[8];          // +0x1120 the contact by kind (setContact)
        uint8_t _1128[0x1130 - 0x1128];
        ee::Float snapPoint[4];      // +0x1130 the snap target's point (grindApproachEnter's target, grindEnter's
                                     //         closest point of the rail): a stall puts the rider there
        ee::Float snapRows[16];      // +0x1140 the snap target's rows (grindApproachEnter, stallEnter): approachAlign turns the rider's toward them
        ee::Float alignFrom[16];     // +0x1180 airRotate's alignment: a matrix the rows are turned through
        int16_t alignAngles[4];      // +0x11C0 and the angles turned a frame
        uint8_t _11c8[0x11D0 - 0x11C8];
        ee::Float spinFrom[16];      // +0x11D0 airRotate's second turn (kFlagTrickTurn): a matrix
        int16_t spinAngles[4];       // +0x1210 and its angles a frame
        uint32_t messenger;          // +0x1218 the rider's engine object (its model): messages are sent from it
        ee::Ptr<zge::AnimationChannel> headBone; // +0x121C the rider model's "Head" bone (riderFrame finds it by that name's
                                     //         hash each frame; physicsFrame turns it by barLean, the trick
                                     //         poses by the trick's share)
        uint32_t motionFlags;        // +0x1220
        int32_t motion;              // +0x1224 the motion index
        ee::Float motionRate;        // +0x1228
        ee::Float motionTime;        // +0x122C the motions' clock (riderFrame: 0 at a restart, set on both models)
        ee::Float motionRoot[4];     // +0x1230 the motion's root (the Hips) in the body's space
        // The grind's and stall's lean (grindLean): two motions blended
        // over the trick's own by leanBlend, the first's weight the
        // balance's positive part, the second's its negative part (swapped
        // when mirrored); riderFrame copies the weights to leanWeights'
        // sixteen entries a side each frame the lean is on.
        uint16_t leanMotions[2];     // +0x1240 the grind trick's fourth and fifth motions
        // Each lean blend's sixteen rider bones (newRider: the name hashes
        // of those the model has, of g_leanBoneNames) and their weights,
        // and the two lists (count, first) the blends take.
        struct LeanWeight
        {
            uint32_t bone;
            ee::Float weight;
        } leanWeights[2][16];        // +0x1244
        struct LeanWeightList
        {
            uint32_t count;
            ee::Ptr<LeanWeight> weights;
        } leanWeightLists[2];        // +0x1344
        ee::Float leanBlend[2];      // +0x1354
        ee::Float topTrickBlend;     // +0x135C the trick on top's share of the pose (kFlagTopTrick: grows in
                                     //        while the trick is held, falls out after; poseRiderBones)
        zge::BoneChain limbTargets[4]; // +0x1360 the limbs' bone chains aimBoneChain aims (left hand, right hand,
                                     //         left foot, right foot): the grips, the trick poses
        uint32_t riderDynamics;      // +0x1460 the rider model's dynamics (zge::Dynamics): the bail's bodies (bailEnter
                                     //         wakes them from the pose, bailMove steps them)
        ee::Ptr<zge::BoneMirror> riderMirror; // +0x1464 the rider model's bone mirror (newBoneMirror when the rider is
                                     //         made): riderFrame mirrors the pose by it when the bike is mirrored
        ee::Ptr<zge::RigidBody> bailBody; // +0x1468 the bail's tumbling body: riderFrame keeps it with the rider
                                     //         (his rows, position and velocity) while the camera follows him
        uint8_t _146c[0x1470 - 0x146C];
        BailHand hands[2];           // +0x1470 the bail's hands: left, right
        ee::Float drawnPosition[4];  // +0x14F0 the drawn body's position
        ee::Float drawnOffset[4];    // +0x1500 the bike's root in the body's space (inferred)
        ee::Float body[16];          // +0x1510 the drawn body's matrix
        ee::Float pedalForce;        // +0x1550 the drive on wheel 1 this frame (groundMove; cleared on a reset)
        ee::Float wheelDrag;         // +0x1554 the share of the wheels' rolling drag the action state asks for
                                     //         (groundMove scales the drag and the speed it rests below by it: 1
                                     //         stopped, g_manualBlend in a manual); cleared on a state change
        uint8_t _1558[8];
        WheelPose wheelPoses[2];     // +0x1560 the wheels' turn and their bones
        uint32_t bikeModel;          // +0x1660 the bike's engine object (its model)
        ee::Ptr<zge::AnimationChannel> bikeBarsBone; // +0x1664 the bike model's "Handlebars" bone
        ee::Ptr<zge::AnimationChannel> sprocketBone; // +0x1668 the bike model's "Sprocket" bone (riderFrame finds it by that name's
                                     //         hash; turned when mirrored, and by the crank tricks)
        uint32_t bikeDynamics;       // +0x166C the bike model's dynamics (zge::Dynamics): its bodies in a bail
        ee::Ptr<zge::BoneMirror> bikeMirror; // +0x1670 the bike model's bone mirror (newBoneMirror, as riderMirror)
        uint32_t rollingVoice;       // +0x1674 the rolling sound's voice

        // The eight stats in their order (airStat .. wallRideStat).
        ee::Float &stat(uint32_t index) { return (&airStat)[index]; }

        // The contact as the collision code's reference (a face's or an
        // edge's: its node and face index, collision::FaceRef / EdgeRef).
        uint64_t contactRef() const
        {
            uint64_t ref;
            std::memcpy(&ref, contact, sizeof(ref));
            return ref;
        }
    };
    static_assert(offsetof(Rider, ownTricks) == 0x40);
    static_assert(offsetof(Rider, controllerLayout) == 0x130);
    static_assert(offsetof(Rider, input) == 0x138);
    static_assert(offsetof(Rider, flags) == 0x160);
    static_assert(offsetof(Rider, actionStates) == 0x165);
    static_assert(offsetof(Rider, actionFrames) == 0x16A);
    static_assert(offsetof(Rider, trick) == 0x172);
    static_assert(offsetof(Rider, yawTurned) == 0x17C && offsetof(Rider, bailKind) == 0x178);
    static_assert(offsetof(Rider, health) == 0x177);
    // Rider::health at the spawn (BMX XXX's kFullHealth).
    constexpr uint8_t kFullHealth = 0xFF;
    static_assert(offsetof(Rider, trickLoops) == 0x174 && offsetof(Rider, trickTurned) == 0x180 && offsetof(Rider, balanceFrames) == 0x18C);
    static_assert(offsetof(Rider, rideDistance) == 0x188 && offsetof(Rider, edgeHeight) == 0x194 && offsetof(Rider, landingTravelAngle) == 0x190 && offsetof(Rider, score.lineCount) == 0x83D);
    static_assert(offsetof(Rider, score.turnScore) == 0x1A0 && offsetof(Rider, score.gameScore) == 0x1A4 && offsetof(Rider, score.horseLetters) == 0x1AC);
    static_assert(offsetof(Rider, wheels) == 0xF90);
    static_assert(offsetof(Rider, physicsState) == 0xFF0);
    static_assert(offsetof(Rider, score.lines) == 0x1BC && offsetof(Rider, score.tricks) == 0x33C && offsetof(Rider, score.lineState) == 0x83C && offsetof(Rider, score.trickCount) == 0x83F);
    static_assert(offsetof(Rider, score.goalFlags) == 0xF7C && offsetof(Rider, score.gapsFound) == 0xF80 && offsetof(Rider, score.recordBeaten) == 0xF88);
    static_assert(offsetof(Rider, score.kindScores) == 0x1B0 && offsetof(Rider, score.trickRepeats) == 0x840 && offsetof(Rider, score.cellTrickCounts) == 0xE86 && offsetof(Rider, score.tipsLeft) == 0xF78);
    static_assert(offsetof(Rider, right) == 0x1000);
    static_assert(offsetof(Rider, groundHeight) == 0x1060);
    static_assert(offsetof(Rider, touchedObject) == 0x1070);
    static_assert(offsetof(Rider, touchPoint) == 0x1080 && offsetof(Rider, touchRotation) == 0x1090);
    static_assert(offsetof(Rider, messenger) == 0x1218);
    static_assert(offsetof(Rider, velocity) == 0x10A0);
    static_assert(offsetof(Rider, pitch) == 0x10D0);
    static_assert(offsetof(Rider, speed) == 0x10D8);
    static_assert(offsetof(Rider, contact) == 0x1120);
    static_assert(offsetof(Rider, motionFlags) == 0x1220);
    static_assert(offsetof(Rider, motionRate) == 0x1228);
    static_assert(offsetof(Rider, body) == 0x1510);
    static_assert(offsetof(Rider, wheelDrag) == 0x1554);
    static_assert(offsetof(Rider, competitionRuns) == 0xFD0);
    static_assert(offsetof(Rider, position) == 0x1040 && offsetof(Rider, rootOffset) == 0x1050);
    static_assert(offsetof(Rider, force) == 0x10B0 && offsetof(Rider, bodyPitch) == 0x10D6);
    static_assert(offsetof(Rider, snapPoint) == 0x1130 && offsetof(Rider, snapStep) == 0x10C0 && offsetof(Rider, snapRows) == 0x1140 && offsetof(Rider, rowsOrigin) == 0x1030);
    static_assert(offsetof(Rider, alignFrom) == 0x1180 && offsetof(Rider, alignAngles) == 0x11C0);
    static_assert(offsetof(Rider, spinFrom) == 0x11D0 && offsetof(Rider, spinAngles) == 0x1210);
    static_assert(offsetof(Rider, headBone) == 0x121C && offsetof(Rider, motionRoot) == 0x1230);
    static_assert(offsetof(Rider, motionTime) == 0x122C && offsetof(Rider, leanMotions) == 0x1240 && offsetof(Rider, leanWeights) == 0x1244 && offsetof(Rider, leanWeightLists) == 0x1344);
    static_assert(offsetof(Rider, leanWeights[1]) == 0x12C4 && offsetof(Rider, leanBlend) == 0x1354 && offsetof(Rider, topTrickBlend) == 0x135C);
    static_assert(offsetof(Rider, topTrickLoopFrames) == 0x175);
    static_assert(offsetof(Rider, drawnPosition) == 0x14F0 && offsetof(Rider, drawnOffset) == 0x1500);
    static_assert(offsetof(Rider, wheelPoses) == 0x1560 && offsetof(Rider, bikeModel) == 0x1660);
    static_assert(offsetof(Rider, rollingVoice) == 0x1674 && offsetof(Rider, bikeBarsBone) == 0x1664);
    static_assert(offsetof(Rider, barsTilt) == 0x80 && offsetof(Rider, grips) == 0xA0 && offsetof(Rider, limbTargets) == 0x1360 && offsetof(Rider, seatGrip) == 0x90);
    static_assert(offsetof(Rider, wheelHeightLeast) == 0xCC && offsetof(Rider, wheelHeightMost) == 0xD0);
    static_assert(offsetof(Rider, frontWheelHeight) == 0x100 && offsetof(Rider, backWheelHeight) == 0x120);
    static_assert(offsetof(Rider, airStat) == 0x20 && offsetof(Rider, speedStat) == 0x24 && offsetof(Rider, wallRideStat) == 0x3C && offsetof(Rider, massHeight) == 0xC0 && offsetof(Rider, slip) == 0xE4);
    static_assert(offsetof(Rider, frontWheelPoint) == 0xF0 && offsetof(Rider, backWheelPoint) == 0x110);
    static_assert(offsetof(Rider, skidDistance) == 0x184 && offsetof(Rider, pedalForce) == 0x1550);
    static_assert(offsetof(Rider, soundWait) == 0x176 && offsetof(Rider, bailBody) == 0x1468);
    static_assert(offsetof(Rider, riderMirror) == 0x1464 && offsetof(Rider, sprocketBone) == 0x1668 && offsetof(Rider, bikeMirror) == 0x1670);
    static_assert(offsetof(Rider, hands) == 0x1470 && offsetof(Rider, drawnPosition) == 0x14F0);
    static_assert(offsetof(Rider, riderDynamics) == 0x1460 && offsetof(Rider, bikeDynamics) == 0x166C);

    // Rider::flags.
    constexpr uint32_t kFlagNoPlayer = 0x1u;          // no player's rider (an NPC's: newRider sets it for a kRiderNoPlayer request and makes no bail body); a level-0 change leaves the camera alone, a touch the game side's handler
    constexpr uint32_t kFlagShown = 0x2u;             // shown (riderReset)
    constexpr uint32_t kFlagIdle = 0x8u;              // still for the idle's frames (coastState: the idle effect plays)
    constexpr uint32_t kFlagTapsOn = 0x20u;           // the stick's direction counts as a tap
    constexpr uint32_t kFlagTrickAWaiting = 0x40u;    // trick button A pressed before the taps (airState waits for them)
    constexpr uint32_t kFlagTrickBWaiting = 0x80u;    // the same for button B (airState, and the grind's and stall's tricks)
    constexpr uint32_t kFlagMotionEnded = 0x400u;     // the motion played to its end (riderFrame)
    constexpr uint32_t kFlagMirrored = 0x800u;        // the bike mirrored
    constexpr uint32_t kFlagTopTrick = 0x1000u;       // a trick on top (trickMotion's motion over the grind's, stall's,
                                                      // wheelie's or air trick's); poseRiderBones poses the limbs for it
    constexpr uint32_t kFlagTrickTurn = 0x2000u;      // a turning trick's turn on: airRotate turns by spinFrom and spinAngles
    constexpr uint32_t kFlagFellOff = 0x40000u;       // fell off the rail's side (the grind's balance lost): the air
                                                      // after it starts in sub-state 3
    constexpr uint32_t kFlagFakie = (2u << 20);        // riding backwards
    // The rail, lip or wall met is on the bike's mirrored side: grindApproachEnter, grindEnter and groundProbe set it by
    // the side of the rider they meet it on (against kFlagMirrored); the grind and stall landings mirror the bike by it
    // (kFlagMirrored), the lean blends swap, and the lip's hop, its ray (airMove) and the take-off's turn go the other way.
    constexpr uint32_t kFlagMirrorSide = (1u << 22);
    constexpr uint32_t kFlagManual = (1u << 23);      // in a manual (physics 10: two down taps, the stick down)
    constexpr uint32_t kFlagNoseWheelie = (1u << 24); // in a nose wheelie (physics 9: two up taps, the stick up)
    constexpr uint32_t kFlagLeftRail = 0x8000000u;    // the air came off a grind, a stall or a lip
    constexpr uint32_t kFlagTakeoffBack = (1u << 28); // rolling backwards at the take-off (airEnter sets it; the
                                                      // trick score reads it for the spin and clears it)
    constexpr uint32_t kFlagBailHands = 0x80000000u;  // the bail's hands reach for the ground (bailState)

    // Rider::motionFlags.
    constexpr uint32_t kMotionFlag4 = 0x4u;

    // The physics states (rider +0xFF0; MIXING 14.1's table).
    enum PhysicsState : uint8_t
    {
        kPhysicsGround = 0,
        kPhysicsAir = 1,
        kPhysicsBail = 2,
        kPhysicsGrindApproach = 3,
        kPhysicsGrind = 4,
        kPhysicsStallApproach = 5,
        kPhysicsStall = 6,
        kPhysicsLipApproach = 7,
        kPhysicsLipTrick = 8,     // the lip tricks, the wall tap and the wall ride (tricks 0x3F .. 0x45)
        kPhysicsNoseWheelie = 9,  // on the front wheel (trick 0x47, two up taps and the stick up on landing)
        kPhysicsManual = 10,      // on the back wheel (trick 0x46, two down taps and the stick down)
        kPhysicsNone = 0xFF, // before the first (its exit is the bail's, g_physicsExits[kPhysicsBail])
    };

    // The action states of level 0 (g_actionStates).
    enum ActionState : uint8_t
    {
        kActionRiding = 0,
        kActionAir = 1,
        kActionGrind = 2,
        kActionStall = 3,
        kActionLipTrick = 4,
        kActionWheelie = 5,   // the manual and the nose wheelie (physics 9 and 10)
        kActionLanding = 6,
        kActionBail = 7,
    };
    constexpr uint32_t kActionLevels = 4;

    // The result of groundBelow (and the ray it casts).
    struct GroundHit
    {
        uint32_t flags;      // +0x00 kGroundObject: an object was hit
        uint8_t _04[0x0C];
        ee::Float point[4];  // +0x10 where the ray hit
        ee::Float plane[4];  // +0x20 the face's plane (normal, distance)
        int64_t face;        // +0x30 the face hit (when a face)
        uint32_t object;     // +0x38 the object hit (when an object)
    };
    static_assert(offsetof(GroundHit, point) == 0x10);
    static_assert(offsetof(GroundHit, face) == 0x30);
    static_assert(offsetof(GroundHit, object) == 0x38);
    constexpr uint32_t kGroundObject = 0x1u;

    // The level's bounding box (levelBounds; y grows downwards).
    struct Bounds
    {
        ee::Float min[4];
        ee::Float max[4];
    };
}

namespace game::dm2::rider
{
    // (contact.cpp, input.cpp, physics_state.cpp, rider_state.cpp, tricks.cpp)
    void addRideDistance(Rider *rider, ee::Float distance);
    uint8_t airTrickByTaps(Rider *rider);
    void clearTaps(Rider *rider, uint32_t keepStick);
    uint8_t grindTrick(const Rider *rider);
    int32_t groundBelow(const ee::Float *point, GroundHit *hit);
    Stick *inputStick(const ButtonRecord *record, uint32_t which);
    uint8_t lipTrick(const Rider *rider);
    void physicsInit();
    uint32_t deletePhysicsTask();
    uint32_t physicsShutdown();
    // A rider's record freed at the level's end: its bail body, sounds, bone
    // mirrors and models let go, its slot emptied.
    void freeRider(uint32_t index);
    // A rider made for a request (players.h's NewRider): its slot, or 0xFF.
    uint8_t newRider(ee::Ptr<game::dm2::NewRider> request);
    uint32_t playerIndexOfNode(ee::Address node);
    // A rider motion by index (riderSetup lists them; the level's animated
    // objects play theirs from 0x208 on).
    ee::Ptr<zge::Motion> riderMotion(uint32_t index);
    uint8_t stallTrick(const Rider *rider);
    uint8_t stickDirection(const Rider *rider);
    uint8_t trickFromTaps(const Rider *rider);
    // buttonTest on a port's pad (input.cpp).
    int32_t padButtons(uint32_t port, uint32_t buttons, uint32_t mode);
}
