#pragma once

// BMX XXX's rider (SLUS_204.15): the records the rider's physics (action/
// physics.c) and action states keep, as far as the written functions read
// them. Its line is DM2's (games/dm2/rider/rider.h): the same records, with
// BMX XXX's own offsets, each read in BMX XXX's machine code by the function
// that uses it.
//
// These are BMX XXX's own functions, compiled in BMX XXX's instance only
// and registered for BMX XXX there (READABLE_FUNCTION): they install only
// when BMX XXX runs.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "bmx/rider/vector.h"
#include "zge/input/pads.h"
#include "zge/scene/bone_chain.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zge
{
    struct Dynamics; // zge/collision/dynamics.h
    struct RigidBody; // zge/collision/dynamics.h
    struct Motion;   // zge/scene/animation.h
    struct AnimationChannel; // zge/scene/animation.h
    struct BoneMirror;       // zge/scene/animation.h
    struct SceneObject;      // zge/scene/scene.h
    struct SceneBone;        // zge/scene/scene.h
    struct ModelRecord;      // zge/scene/model_records.h
    struct SoftPartsRig;     // zge/scene/animate_system.h
}

namespace game::bmx
{
    struct NewRider; // bmx/game/player_riders.h
    struct Projectile; // bmx/weapon/weapon.h
    struct ProjectileMessage;
}

namespace game::bmx::rider
{
    // A direction as the trick tables index it (stickDirection's table):
    // none, the four, then the diagonals (DM2's numbering).
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

    // How buttonTest tests a set of buttons (the move table's modes, DM2's).
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

    // A pad's buttons as the game reads them each frame: held and changed
    // buttons, the sticks, and the sticks a frame ago (DM2's record).
    struct ButtonRecord
    {
        uint32_t held;     // +0x00
        uint32_t changed;  // +0x04 went down or up this frame
        Stick sticks[2];   // +0x08 left, +0x10 right
        Stick previous[2]; // +0x18 the sticks last frame
    };
    static_assert(sizeof(ButtonRecord) == 0x28);
    static_assert(offsetof(ButtonRecord, previous) == 0x18);

    // A port's pad (0x58 bytes a port; DM2's is 0x38, without the face
    // levels), read each frame by the game's pads module (input/pads.cpp).
    constexpr uint32_t kPadFaceLevels = 4;
    struct PadInput
    {
        ee::Ptr<zge::Pad> pad; // +0x00 the engine's pad of the port
        uint32_t ignored;     // +0x04 buttons ignored until let go
        uint32_t heldAside;   // +0x08 the held buttons kept while something else has the pads (takePads)
        uint16_t flags;       // +0x0C kPadMissing (a replay does not record a missing pad); bit 1: it came back this frame
        uint16_t repeatWait;  // +0x0E a game screen's frame: frames until the held directions repeat (DM2's name)
        ButtonRecord buttons; // +0x10
        // +0x38 the pad's levels of the four face buttons, in the order the
        // player's controller configuration gives (what each of the four
        // places is used for: not read yet); +0x48 a frame ago.
        ee::Float faceLevels[kPadFaceLevels];
        ee::Float previousFaceLevels[kPadFaceLevels];
    };
    // PadInput::flags: no pad is connected at the port.
    constexpr uint16_t kPadMissing = 0x1;
    static_assert(offsetof(PadInput, buttons) == 0x10 && offsetof(PadInput, faceLevels) == 0x38 &&
                  offsetof(PadInput, previousFaceLevels) == 0x48);
    static_assert(sizeof(PadInput) == 0x58);

    // One entry of the move table (for each of the three controller
    // configurations, 25 moves of three tests; DM2 has 22): the buttons and
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
    constexpr uint32_t kMovesPerLayout = 25;
    struct MoveLayout
    {
        Move moves[kMovesPerLayout];
    };
    static_assert(sizeof(MoveLayout) == 0x258);

    // What a contact record holds by kind (Rider::contactKind, a wheel's
    // kind): a face (node and face index, 8 bytes) for every kind but an
    // object's (4 bytes, the object), up to kContactKinds. BMX XXX has one
    // kind more than DM2 (12 to 11), and the object's is 9 (DM2's 8).
    constexpr uint8_t kContactObject = 9;
    constexpr uint8_t kContactWater = 10; // deep water a wheel went into (groundImpact; the bail slows in it)
    constexpr uint32_t kContactKinds = 12;

    // What the bodies of the rider's dynamics touched this frame
    // (Rider::touches; the moves that follow the bodies clear it): the
    // contact callbacks fill it, the moves read it.
    enum BodyTouchFlag : uint32_t
    {
        kTouchFace = 0x01,        // a body of the air's motions (riderAnim) touched a face (airBodyContact)
        kTouchWall = 0x02,        // the face was a wall the rider moved into
        kTouchWallHeadOn = 0x04,  // and the rider's velocity within 30 degrees of its normal
        kTouchBump = 0x08,        // a body hit something hard enough to sound (riderBodyContact)
    };
    struct BodyTouches
    {
        uint32_t flags;           // +0x00 BodyTouchFlag
        ee::Float nearest;        // +0x04 the squared distance from its body of the nearest face point met (1e6 to start)
        uint8_t _08[8];
        ee::Float point[4];       // +0x10 the faces' points met, each moving it halfway to the next
        ee::Float normal[4];      // +0x20 their normals the same way
        uint32_t nearestNode;     // +0x30 the nearest face met: its node
        uint32_t nearestFace;     // +0x34 and index
        uint32_t bumpLevel;       // +0x38 the hardest hit's level (0 .. 3 by speed: from 4, 9, 17, 30): its sounds' level
        uint32_t bumpPart;        // +0x3C its body part (BodyPart): the hit sound's sample
        uint32_t bumpSurface;     // +0x40 the surface it hit (g_bumpSurfaces by the face's sound)
        uint8_t _44[0x0C];
    };
    static_assert(offsetof(BodyTouches, nearestNode) == 0x30 && offsetof(BodyTouches, bumpSurface) == 0x40 && sizeof(BodyTouches) == 0x50);

    // A wheel's contact (Rider::wheels; setWheelContact).
    struct WheelContact
    {
        uint8_t kind;       // +0x00
        uint8_t _01[7];
        uint8_t contact[8]; // +0x08 the face or object (by kind)
        ee::Float point[4]; // +0x10 where it touches
    };
    static_assert(sizeof(WheelContact) == 0x20);

    // A wheel's pose (Rider::wheelPoses; DM2's record, 0x80 bytes), as far
    // as the written functions read it.
    struct WheelPose
    {
        ee::Float velocity[4]; // +0x00 the wheel's velocity (turned with the body)
        ee::Float speed;       // +0x10 its speed over the ground (groundImpact's dust goes by it)
        uint8_t _14[0x0C];
        ee::Float matrix[16]; // +0x20 the wheel's matrix (turned with the body)
        uint8_t ground;      // +0x60 how it is on the ground (WheelGround)
        uint8_t touched;     // +0x61 bit 0: groundProbe's ray met something (DM2's, inferred)
        int16_t angle;       // +0x62 the wheel's turn (0x10000 a turn)
        int32_t spin;        // +0x64 the wheel's turn a frame; the rolling sound's rate
        ee::Float springLength; // +0x68 the wheel's height below its point on the body (its spring's length; DM2's)
        ee::Float springRate;   // +0x6C and its change a frame (DM2's)
        uint32_t sound;      // +0x70 the surface sound under the wheel
        ee::Ptr<zge::AnimationChannel> bone; // +0x74 the wheel's bone
        uint8_t _78[8];
    };
    static_assert(sizeof(WheelPose) == 0x80);
    static_assert(offsetof(WheelPose, ground) == 0x60 && offsetof(WheelPose, spin) == 0x64 && offsetof(WheelPose, springLength) == 0x68 && offsetof(WheelPose, sound) == 0x70);
    // WheelPose::ground (DM2's): off the ground the wheel makes no sound.
    constexpr uint8_t kWheelRolling = 0; // (DM2's name; a reset leaves the wheels so)
    constexpr uint8_t kWheelSliding = 1; // (DM2's name; the brake's skid holds the back wheel so)
    constexpr uint8_t kWheelOffGround = 2;

    // A bail's hand on an edge (bailHandContact fills it; DM2's record with
    // the nearest point added).
    struct BailHand
    {
        ee::Float edgeStart[4]; // +0x00 the edge's ends, in its node's space
        ee::Float edgeEnd[4];   // +0x10
        ee::Float nearest[4];   // +0x20 the edge's point nearest the hand, in the world
        uint32_t node;          // +0x30 the edge's node
        uint32_t nodeRef;       // +0x34 the node as the edge's reference has it
        uint32_t body;          // +0x38 the hand's body (a zge::BodyContact)
        uint32_t heldBody;      // +0x3C the body the contact holds on to (a zge::RigidBody: removeBodyContact's; DM2's)
        ee::Float share;        // +0x40 from edgeStart toward edgeEnd
        uint8_t holding;        // +0x44 an edge found (bailHands clears it)
        uint8_t _45[3];
        // +0x48 the rider model's "LeftHand" bone (newRider sets the left hand's: its second lookup, also of
        // "LeftHand", goes to the left hand's again, so the right hand's stays 0)
        ee::Ptr<zge::SceneBone> bone;
        uint8_t _4c[4];
    };
    static_assert(sizeof(BailHand) == 0x50 && offsetof(BailHand, body) == 0x38 && offsetof(BailHand, holding) == 0x44);

    // The turn's score records (DM2's, with four times the room): a line is
    // a run of tricks scored together; a new line opens when a trick begins,
    // and each trick landed is chained under the current line. The fields
    // named so far are named by the objectives' and the game's use.
    struct ScoreTrick
    {
        uint8_t flags;               // +0x00 kScoreTrickAlleyOop, kScoreTrickBackTakeoff
        uint8_t trick;               // +0x01 the trick (kNoTrick none)
        uint8_t modifier;            // +0x02 a second trick with it (kNoTrick none)
        uint8_t loops;               // +0x03 the rider's trickLoops (trickScoreFrame; DM2's)
        uint8_t turns;               // +0x04 a turning trick's whole turns (Rider::trickTurned, rounded; 3 at most)
        uint8_t _05;
        uint16_t framesHeld;         // +0x06 the frames the trick has been held (trickScoreFrame; DM2's)
        ee::Float distance;          // +0x08 the rider's speed summed over a grind's, a lip trick's or a wheelie's frames
        int32_t points;              // +0x0C what the trick scored
        ee::Ptr<ScoreTrick> next;    // +0x10 the line's next trick
    };
    static_assert(sizeof(ScoreTrick) == 0x14 && offsetof(ScoreTrick, points) == 0x0C && offsetof(ScoreTrick, next) == 0x10);
    // ScoreTrick::flags: a grind trick ridden backwards (kFlagFakie: the
    // objectives' alley-oop); the air spun after a take-off rolling
    // backwards (DM2's).
    constexpr uint8_t kScoreTrickAlleyOop = 0x2;
    constexpr uint8_t kScoreTrickBackTakeoff = 0x4;
    struct ScoreLine
    {
        uint8_t flags;               // +0x00 kScoreLineSpinFlag
        uint8_t spin;                // +0x01 the line's spin in half turns
        uint8_t landingGrade;        // +0x02 (DM2's) 0 .. 3: scoreLines scales the line's points by its factor
                                     //       (1, 1.1, 0.9, 1.05)
        uint8_t gap;                 // +0x03 (DM2's) the level gap the line made, kNoGap none (as the line opens;
                                     //       HORSE's match compares it as DM2's does)
        uint32_t points;             // +0x04 the line's points (trickLineEnd checks them against SCORE_TRICK)
        ee::Ptr<ScoreTrick> tricks;  // +0x08 the line's first trick (0: none landed yet)
    };
    static_assert(sizeof(ScoreLine) == 0x0C && offsetof(ScoreLine, tricks) == 0x08);
    constexpr uint8_t kNoGap = 0xFF; // ScoreLine::gap
    // ScoreLine::flags: set for a spin off a lip by which way it turns
    // against the way the rider travels (trickScoreFrame; DM2's).
    constexpr uint8_t kScoreLineSpinFlag = 0x2;
    // ScoreLine::flags: set where the line's text says "opposite" (with
    // kScoreLineSpinFlag it says "alley-oop": trickText; DM2's).
    constexpr uint8_t kScoreLineOpposite = 0x1;
    constexpr uint32_t kScoreLines = 128;
    constexpr uint32_t kRepeatRows = 101, kRepeatRowBytes = 22; // ScoreBlock::trickRepeats
    constexpr uint32_t kScoreTricks = 256;

    // The rider's score block (Rider +0x1C0; DM2's ScoreBlock with BMX XXX's
    // sizes): the turn's and the game's score, the turn's trick lines and
    // counts. The game's scoring functions take its address (openScoreLine,
    // addScoreTrick); the offsets here are the block's own. The round's code
    // reads and clears these (setRoundPhase, startGame; DM2's names). Its
    // end is taken from DM2's block, which runs to the wheels' contacts.
    struct ScoreBlock
    {
        ee::Float turnScore;         // +0x0000 the score of the rider's turn
        uint32_t gameScore;          // +0x0004 the rider's score in the game: the turns' sum or the best turn, by
                                     //         the mode's scoring
        uint32_t linePoints;         // +0x0008 (DM2's) the points of the current line (the crowd cheers a line over 4000)
        uint32_t horseLetters;       // +0x000C the letters of the HORSE word the rider has; in a game scored by
                                     //         rounds, the rounds the rider won
        // +0x0010 the line's distances by trick kind: the grinds', the
        // manuals' (trick 0x48), the nose wheelies' (0x49) and the wall
        // rides' (0x47): trickLineEnd sums its tricks' distances and checks
        // them against the records DISTANCE_GRIND, _MANUAL, _NOSEWHEELIE and
        // _WALLRIDE.
        ee::Float kindScores[4];
        ScoreLine lines[kScoreLines];    // +0x0020 the turn's trick lines
        ScoreTrick tricks[kScoreTricks]; // +0x0620 the turn's tricks, chained under their lines
        uint8_t lineState;           // +0x1A20 the current trick line: 0 none
        uint8_t lineCount;           // +0x1A21 the lines in `lines`
        uint8_t unreadLineByte;      // +0x1A22 cleared as a line opens (trickScoreFrame, wipeoutScoreFrame); nothing
                                     //         reads it (DM2's +0x69E likewise)
        uint8_t trickCount;          // +0x1A23 the tricks in `tricks`
        // +0x1A24 how often each trick was landed this turn with each
        // modifier, three times at most: a row a trick (kNoTrick's first),
        // two bits a modifier (kNoTrick's first) (trickLineEnd).
        uint8_t trickRepeats[kRepeatRows][kRepeatRowBytes];
        int16_t cellTrickCounts[17][7]; // +0x22D2 tricks by the level's 16 cells, and the total
        ee::Float longestHold;       // +0x23C0 the greatest groundHeight of the line's tricks 0x43 .. 0x47 (trickScoreFrame;
                                     //         DM2's)
        uint32_t tipsLeft;           // +0x23C4 a bit a tip still to show
        // Three runs of bytes a turn's start clears (setRoundPhase); what they
        // hold is not read yet.
        uint8_t turnBytes4[4];       // +0x23C8
        uint8_t turnBytes2[2];       // +0x23CC
        uint8_t _23ce[2];
        uint8_t turnBytesLoop[4];    // +0x23D0
        uint8_t _23d4[0x23DC - 0x23D4];
        uint32_t recordBeaten;       // +0x23DC (DM2's) 1 when a saved record is beaten (checkGameRecord), then
                                     //         the frames of the rider's sparkle, up to 60; cleared at the spawn
    };
    static_assert(sizeof(ScoreBlock) == 0x23E0 && offsetof(ScoreBlock, gameScore) == 0x0004 && offsetof(ScoreBlock, horseLetters) == 0x000C);
    static_assert(offsetof(ScoreBlock, kindScores) == 0x0010 && offsetof(ScoreBlock, lines) == 0x0020 && offsetof(ScoreBlock, tricks) == 0x0620);
    static_assert(offsetof(ScoreBlock, lineState) == 0x1A20 && offsetof(ScoreBlock, lineCount) == 0x1A21 && offsetof(ScoreBlock, trickCount) == 0x1A23);
    static_assert(offsetof(ScoreBlock, unreadLineByte) == 0x1A22);
    static_assert(offsetof(ScoreBlock, trickRepeats) == 0x1A24 && offsetof(ScoreBlock, cellTrickCounts) == 0x22D2 && offsetof(ScoreBlock, tipsLeft) == 0x23C4);
    static_assert(offsetof(ScoreBlock, turnBytes4) == 0x23C8 && offsetof(ScoreBlock, turnBytesLoop) == 0x23D0 && offsetof(ScoreBlock, recordBeaten) == 0x23DC);

    // A competition run as the five judges marked it (judgeScore): the
    // turn's score as a whole number, each judge's mark (0 .. 255) and their
    // average, the run's mark.
    constexpr uint32_t kCompetitionJudges = 5;
    struct CompetitionRun
    {
        int32_t turnScore;                 // +0x00
        uint8_t judges[kCompetitionJudges]; // +0x04
        uint8_t mark;                      // +0x09
        uint8_t _0a[2];
    };
    static_assert(offsetof(CompetitionRun, judges) == 0x04 && offsetof(CompetitionRun, mark) == 0x09 && sizeof(CompetitionRun) == 0x0C);

    // The rider record, as far as the written functions read it.
    struct Rider
    {
        // The rider's head (newRider: a replay's is copied over it, 0x120 bytes): the models' names, the stats,
        // the bike's set-up.
        char riderModelName[0x20];   // +0x000 "rider.NAME" by the character (its model's name, unless the request's
                                     //        model record is bound to a "skeleton")
        // The rider's stats (DM2's, from +0x20; BMX XXX has one more before
        // the balance).
        ee::Float airStat;           // +0x020 the hop's height (crouchState; DM2 +0x20)
        ee::Float speedStat;         // +0x024 the top speed: a grind is no faster than 0.35 times it (grindSlope)
        ee::Float spinStat;          // +0x028 the air's spins: the most the air turns a frame (airState; DM2 +0x28)
        ee::Float flipStat;          // +0x02C the flips' turn a frame (airTrick's backflips and front flips, its only
                                     //        reader: named from that; DM2 turns them by its spinStat)
        ee::Float balanceStat;       // +0x030 the grind's balance (a rail drags less with more; DM2 +0x2C)
        ee::Float techniqueStat;     // +0x034 the trick motions' rate (the flatland's tricks; DM2 +0x30)
        ee::Float manualStat;        // +0x038 the one-wheel balance: the flatland's tips less with more (DM2 +0x34)
        uint8_t _03c[4];
        ee::Float wallRideStat;      // +0x040 the less of the air's gravity on a lip (airGravity; DM2 +0x3C)
        uint8_t ownTricks[2];        // +0x044 the tricks tapTrick's codes 0x57 and 0x58 stand for (DM2 +0x40)
        uint8_t _046[0x50 - 0x46];
        char bikeModelName[0x20];    // +0x050 "bike.LINE.NN": the bike's line and its number from 1
        int16_t barsTilt;            // +0x070 the bars' turn about their right row (a stat; poseBikeGround)
        uint8_t _072[0x80 - 0x72];
        ee::Float seatGrip[4];       // +0x080 a hand's point on the bike's "Hipsdeck" part, in its space (the seat
                                     //        grab poses put a hand there, poseRiderBones; DM2 +0x90)
        ee::Float grips[2][4];       // +0x090 the hands' grips in the bars' bone's space (DM2 +0xA0)
        // The suspension (DM2's, the same block 0x10 lower): each wheel's spring (WheelPose::springLength,
        // springRate) pulled towards wheelHeightMost by springStiffness over half of mass on the ground (the
        // wheel's share of the mass with gravity across the face: wheelieMove), damped by springDamping.
        ee::Float massHeight;        // +0x0B0 added to a wheel's height in the weight's share (its factor is 0)
        ee::Float mass;              // +0x0B4
        ee::Float springMass;        // +0x0B8 (DM2's: the air's)
        ee::Float wheelHeightLeast;  // +0x0BC the wheels' heights below their points are kept between these (groundAhead; DM2's)
        ee::Float wheelHeightMost;   // +0x0C0
        ee::Float springStiffness;   // +0x0C4
        ee::Float springDamping[2];  // +0x0C8 shortening, lengthening
        ee::Float grip;              // +0x0D0 the wheels' grip and slip by the surface's (DM2's names; newRider: 1)
        ee::Float slip;              // +0x0D4
        uint8_t _0d8[0xE0 - 0xD8];
        ee::Float frontWheelPoint[4]; // +0x0E0 the wheels' points in the drawn body's space: wheelieEnter puts the rider on
                                     //        the balancing wheel's (DM2 +0x0F0)
        ee::Float frontWheelHeight;  // +0x0F0 the wheels' heights: half their sum lifts the body off the ground
                                     //        (resetPhysics; DM2 +0x100)
        uint8_t _0f4[0x100 - 0xF4];
        ee::Float backWheelPoint[4]; // +0x100 (DM2 +0x110)
        ee::Float backWheelHeight;   // +0x110 (DM2 +0x120)
        uint8_t _114[0x120 - 0x114];
        ee::Ptr<zge::ModelRecord> model; // +0x120 the rider's model record (the engine's; gameSetup keeps a copy for the
                                     //        game, gameCleanup puts it back)
        uint8_t controllerLayout;    // +0x124 the player's controller configuration (a turn's start copies it from the
                                     //        player's setup; DM2 +0x130)
        uint8_t tapCount;            // +0x125
        uint16_t tapIdleFrames;      // +0x126 frames since the last tap: past 40 the taps are forgotten (riderFrame;
                                     //        cleared on a reset)
        uint8_t taps[4];             // +0x128 the direction taps (Direction), oldest first
        uint8_t tapTimes[4];         // +0x12C when each tap came: a count of frames in a byte, wrapping (tapFramesApart)
        ButtonRecord input;          // +0x130 the rider's buttons this frame
        uint8_t _158[0x178 - 0x158];
        uint8_t flags[6];            // +0x178 bytes of bits (RiderFlag: hasFlag, setFlag, clearFlag)
        uint8_t player;              // +0x17E the rider's player (DM2 +0x164)
        uint8_t actionStates[4];     // +0x17F the action state levels (DM2 +0x165)
        uint8_t _183;
        uint16_t actionFrames[4];    // +0x184 frames in each level's state (DM2 +0x16A)
        uint8_t chainedTricks[2];    // +0x18C the flatland tricks asked for next (kNoTrick none), the first first
        uint8_t trick;               // +0x18E the current trick (DM2 +0x172)
        uint8_t trickMotion;         // +0x18F the current trick's motion (or a second trick; DM2 +0x173)
        uint8_t trickLoops;          // +0x190 the times the trick's motion has played through (DM2 +0x174)
        uint8_t topTrickLoopFrames;  // +0x191 a trick on top's loops are counted every this many frames (wheelieSubState; DM2 +0x175)
        uint8_t voiceWait;           // +0x192 frames before the rider's voice (a body's hit in a tumble) can sound again
                                     //        (40 after it; riderFrame counts down; cleared on a reset, where DM2 clears
                                     //        its sound's wait +0x176)
        uint8_t bumpWait;            // +0x193 the same for a body's hit sound (7 after it; cleared with it; BMX XXX's own)
        uint8_t health;              // +0x194 the health display's value (the game's frame); 0xFF at the spawn
                                     //        (spawnPlayer), a HEALTH pickup adds its number up to 0xFF, crashes take it
                                     //        (addRideDistance), a projectile's hit (86) and a blast (128 x its strength:
                                     //        riderHitByProjectile): at 0 it sets kFlagDowned (DM2 +0x177, a byte DM2
                                     //        gives the overlay each rider frame)
        uint8_t landingKind;         // +0x195 which landing plays (landingState): 0 none of its own, 1 .. 4 the
                                     //        landings "ground.land.00" .. "03", 6 a skitch's end
        uint8_t bailKind;            // +0x196 why the rider bailed (7, 8 a trick not finished on landing; DM2 +0x178)
        uint8_t _197;
        int32_t yawTurned;           // +0x198 the air's turn about the up row so far (0x10000 a turn; DM2 +0x17C)
        int32_t trickTurned;         // +0x19C a turning trick's own turn so far (DM2 +0x180)
        ee::Float skidDistance;      // +0x1A0 cleared as the brake starts (DM2 +0x184)
        int32_t rideDistance;        // +0x1A4 cleared on a reset (DM2 +0x188)
        int32_t balanceFrames;       // +0x1A8 (DM2 +0x18C, which DM2 clears on a grind's start; BMX XXX on a
                                     //        landing and in the riding and other states)
        int16_t landingTravelAngle;  // +0x1AC at a landing, the angle of the travel along the ground from the forward
                                     //        (DM2 +0x190; trickLineEvent grades the landing by it)
        uint8_t _1ae[2];
        ee::Float edgeHeight;        // +0x1B0 a hop from less than half a unit of it is a weak one (crouchState; DM2 +0x194)
        // +0x1B4 the stick's last push past half in the air (x, y; airState keeps them for g_airStickHoldFrames
        // frames): a hop in the air with x past half is a hop to the side.
        ee::Float airStick[2];
        uint8_t _1bc[4];
        ScoreBlock score;            // +0x1C0 the turn's and the game's score, the trick lines and counts (DM2 +0x1A0)
        WheelContact wheels[2];      // +0x25A0 (DM2 +0xF90)
        CompetitionRun competitionRuns[2]; // +0x25E0 a competition's two runs, as the judges marked them (judgeScore)
        uint8_t _25f8[0x2600 - 0x25F8];
        uint8_t physicsState;        // +0x2600 (PhysicsState; DM2 +0xFF0)
        uint8_t contactKind;         // +0x2601 (DM2 +0xFF1)
        int16_t snapFrames;          // +0x2602 frames left of a snap (the approaches; DM2 +0xFF2)
        uint8_t _2604[0x2610 - 0x2604];
        ee::Float right[4];          // +0x2610 the rows: right,
        ee::Float up[4];             // +0x2620 up,
        ee::Float forward[4];        // +0x2630 forward
        ee::Float rowsOrigin[4];     // +0x2640 and (0, 0, 0, 1): right .. here are a matrix (DM2 +0x1000)
        ee::Float position[4];       // +0x2650 the physics position (y grows downwards; DM2 +0x1040)
        ee::Float rootOffset[4];     // +0x2660 the drawn rider's offset from it (the motion's root in the world)
        ee::Float groundHeight;      // +0x2670 the height above the ground below (the ray down's hit less its start:
                                     //        groundContactBelow; the models' tint by it; DM2 +0x1060, named airFrames there)
        // The balances: BMX XXX keeps one for the grind and stall (their
        // sub-states and the approaches' enter, which turn it round with the
        // rail's side) and one for the wheelies (the wheelie's states, its
        // move and the flatland); DM2 keeps one (+0x1064).
        ee::Float grindBalance;      // +0x2674
        ee::Float wheelieBalance;    // +0x2678
        uint32_t clearedWithBalances; // +0x267C setActionState writes 0 here with the balances; nothing else in BMX XXX's
                                      //         PS2 code reads or writes it (every access checked)
        ee::Float barLean[2];        // +0x2680 the bars' lean (DM2 +0x1068)
        uint8_t _2688[0x2690 - 0x2688];
        uint32_t touchedObject;      // +0x2690 the object last touched; cleared on every physics state change (DM2 +0x1070)
        uint32_t touchedPart;        // +0x2694 its part (a bone of it)
        uint8_t _2698[8];
        ee::Float touchPoint[4];     // +0x26A0 the touched part's origin in the world a frame ago (rideTouchedObject)
        ee::Float touchRotation[4];  // +0x26B0 the part's rotation then, a quaternion
        ee::Float velocity[4];       // +0x26C0 units a frame (DM2 +0x10A0)
        ee::Float force[4];          // +0x26D0 this frame's acceleration (the air's gravity), units a frame a frame (DM2 +0x10B0)
        ee::Float snapStep[4];       // +0x26E0 a snap's move a frame (approachGravity adds gravity; DM2 +0x10C0)
        int16_t pitch;               // +0x26F0 the air's angles (0x10000 a turn; DM2 +0x10D0)
        int16_t yawRate;             // +0x26F2
        int16_t roll;                // +0x26F4
        int16_t bodyPitch;           // +0x26F6 the drawn body's turn about its right row (DM2 +0x10D6)
        ee::Float speed;             // +0x26F8 |velocity| (DM2 +0x10D8)
        uint8_t _26fc[0x2740 - 0x26FC];
        uint8_t contact[8];          // +0x2740 the contact by kind (setContact; DM2 +0x1120)
        uint8_t _2748[8];
        ee::Float snapPoint[4];      // +0x2750 the snap target's point: a grind's enter not onto a rail puts the rider there
                                     //        (DM2 +0x1130)
        ee::Float snapRows[16];      // +0x2760 the snap target's rows (the rail's; DM2 +0x1140)
        ee::Float alignFrom[16];     // +0x27A0 airRotate's alignment: a matrix the rows are turned through (DM2 +0x1180)
        int16_t alignAngles[4];      // +0x27E0 and the angles turned a frame (DM2 +0x11C0)
        uint8_t _27e8[8];
        ee::Float spinFrom[16];      // +0x27F0 a turning trick's turn (kFlagTrickTurn): the rows as the trick started
                                     //        (airTrick), turned through by the angles (DM2 +0x11D0)
        int16_t spinAngles[4];       // +0x2830 and its angles a frame: a flip's the first (DM2 +0x1210)
        ee::Ptr<zge::SceneObject> messenger; // +0x2838 the rider's engine object (its model; DM2 +0x1218)
        ee::Ptr<zge::AnimationChannel> headBone; // +0x283C the rider model's "Head" bone (physicsFrame turns it by barLean)
        uint32_t motionFlags;        // +0x2840 (kMotionRestart, kMotionFlag4; DM2 +0x1220)
        uint32_t motionName;         // +0x2844 the motion's name hash (DM2 keeps an index at +0x1224)
        ee::Ptr<zge::Motion> motion; // +0x2848 the motion found by it (riderMotionByName)
        ee::Float motionRate;        // +0x284C (DM2 +0x1228)
        ee::Float motionTime;        // +0x2850 the motions' clock (stepRiderMotion: 0 at a restart, set on both models;
                                     //        DM2 +0x122C)
        uint8_t _2854[0x0C];
        ee::Float motionRoot[4];     // +0x2860 the motion's root (the Hips) in the body's space (DM2 +0x1230)
        ee::Float motionRootRotation[4]; // +0x2870 the motion's root's rotation, a quaternion (BMX XXX's own: the
                                     //        air's after callback gives both to the Hips bone)
        // +0x2880 the lean motions ("grind.NAME.wobble.r", ".l": the grind's sub-state finds them) and
        // +0x29A8 how much of each is blended in (the grind balance's two sides; with kMotionLean; DM2's).
        ee::Ptr<zge::Motion> leanMotions[2];
        // +0x2888 each lean blend's 17 rider bones (newRider: the name hashes of those of g_leanBoneNames the
        // model has) and their weights, set from leanBlend each frame the lean is on (stepRiderMotion; DM2
        // +0x1244, 16 a side), and the two lists (count, first) the blends take (DM2's).
        struct LeanWeight
        {
            uint32_t bone;
            ee::Float weight;
        } leanWeights[2][17];
        struct LeanWeightList
        {
            uint32_t count;
            ee::Ptr<LeanWeight> weights;
        } leanWeightLists[2];        // +0x2998
        ee::Float leanBlend[2];
        ee::Float topTrickBlend;     // +0x29B0 how far the trick on top is in (0 when kFlagTopTrick is set; poseRiderBones
                                     //        scales the bars' turn by it; the swivel's crouch takes it back to 0)
        int32_t topTrickFrames;      // +0x29B4 the frames the trick on top has been shown (counted while it shows;
                                     //        poseRiderBones turns the bars by it; DM2 uses the sub-state's frames)
        uint8_t _29b8[8];
        zge::BoneChain limbTargets[4]; // +0x29C0 the limbs' bone chains aimBoneChain aims (left hand, right hand,
                                      //         then DM2's feet; DM2 +0x1360)
        uint32_t limbRig;            // +0x2AC0 the rider model's limbs' rig (the engine's newLimbRig makes it; engine::BodyRig)
        ee::Ptr<zge::Dynamics> riderDynamics; // +0x2AC4 the rider model's dynamics made from "dynamics.rider" (newRider; DM2 +0x1460)
        // +0x2AC8 the rider model's dynamics made from "dynamics.riderAnim"
        // (BMX XXX's own): in the air its bodies move with the rider.
        ee::Ptr<zge::Dynamics> riderAnimDynamics;
        ee::Ptr<zge::BoneMirror> riderMirror; // +0x2ACC the rider model's bone mirror: stepRiderMotion mirrors the pose
                                     //        by it when the bike is mirrored (DM2 +0x1464)
        ee::Ptr<zge::RigidBody> bailBody; // +0x2AD0 the bail's tumbling body (DM2 +0x1468)
        uint8_t _2ad4[0x2AE0 - 0x2AD4];
        BailHand hands[2];           // +0x2AE0 the bail's hands: left, right (DM2 +0x1470)
        BodyTouches touches;         // +0x2B80 what the dynamics' bodies touched this frame
        // +0x2BD0 the "Eyelids" bone's steps (stepRiderMotion turns the bone
        // by them; coasting's idle puts them to kEyelidsIdle, waking to
        // kEyelidsWoken) and the frames of the step.
        uint8_t eyelidState;
        uint8_t _2bd1;
        uint16_t eyelidFrames;       // +0x2BD2
        ee::Float eyelidsShut;       // +0x2BD4 how far the eyelids are shut, 0 open to 1 (stepRiderMotion turns the
                                     //        bone by it)
        ee::Ptr<zge::SoftPartsRig> softParts; // +0x2BD8 the rider model's soft parts' rig (hair,
                                     //         clothes; 0 without one)
        uint32_t shooter;            // +0x2BDC the rider's shooter record (weapon.c's; spawnPlayer resets it)
        ee::Float drawnPosition[4];  // +0x2BE0 the drawn body's position (DM2 +0x14F0)
        ee::Float drawnOffset[4];    // +0x2BF0 the bike's root in the body's space (DM2 +0x1500)
        ee::Float body[16];          // +0x2C00 the drawn body's matrix (DM2 +0x1510)
        ee::Float pedalForce;        // +0x2C40 cleared on a reset (DM2 +0x1550)
        ee::Float wheelDrag;         // +0x2C44 the share of the wheels' rolling drag the action state asks for (DM2 +0x1554)
        uint8_t _2c48[8];
        WheelPose wheelPoses[2];     // +0x2C50 the wheels' turn (front, back; DM2 +0x1560)
        ee::Ptr<zge::SceneObject> bikeModel; // +0x2D50 the bike's engine object (its model; DM2 +0x1660)
        ee::Ptr<zge::AnimationChannel> bikeBarsBone; // +0x2D54 the bike model's "Handlebars" bone
        ee::Ptr<zge::AnimationChannel> sprocketBone; // +0x2D58 the bike model's "Sprocket" bone (the crank spin turns it; DM2 +0x1668)
        ee::Ptr<zge::Dynamics> bikeDynamics; // +0x2D5C the bike model's dynamics made from "dynamics.bike" (newRider)
        ee::Ptr<zge::BoneMirror> bikeMirror; // +0x2D60 the bike model's bone mirror (as riderMirror; DM2 +0x1670)
        uint32_t rollingVoice;       // +0x2D64 the rolling sound's voice (DM2 +0x1674)
        uint8_t _2d68[8];            // +0x2D68 to the record's end (newRider allocates and clears the record; no function written so far reads these)
    };
    static_assert(sizeof(Rider) == 0x2D70);

    // The contact's eight bytes as the reference they hold (an edge's or a
    // face's, by contactKind).
    inline uint64_t contactReference(const Rider &rider)
    {
        uint64_t reference;
        std::memcpy(&reference, rider.contact, sizeof(reference));
        return reference;
    }
    static_assert(offsetof(Rider, leanMotions) == 0x2880 && offsetof(Rider, leanBlend) == 0x29A8);
    static_assert(offsetof(Rider, tapIdleFrames) == 0x126 && offsetof(Rider, motionTime) == 0x2850 && offsetof(Rider, leanWeights) == 0x2888 && offsetof(Rider, leanWeightLists) == 0x2998);
    static_assert(offsetof(Rider, leanWeights[1]) == 0x2910 && offsetof(Rider, riderMirror) == 0x2ACC && offsetof(Rider, bikeMirror) == 0x2D60);
    static_assert(offsetof(Rider, eyelidsShut) == 0x2BD4 && offsetof(Rider, touches) == 0x2B80);
    static_assert(offsetof(Rider, frontWheelPoint) == 0xE0 && offsetof(Rider, backWheelPoint) == 0x100 && offsetof(Rider, wheelHeightMost) == 0xC0);
    static_assert(offsetof(Rider, flipStat) == 0x2C && offsetof(Rider, spinFrom) == 0x27F0 && offsetof(Rider, spinAngles) == 0x2830);
    static_assert(offsetof(Rider, ownTricks) == 0x44 && offsetof(Rider, wallRideStat) == 0x40 && offsetof(Rider, manualStat) == 0x38 && offsetof(Rider, techniqueStat) == 0x34);
    static_assert(offsetof(Rider, force) == 0x26D0 && offsetof(Rider, riderAnimDynamics) == 0x2AC8);
    static_assert(offsetof(Rider, eyelidState) == 0x2BD0 && offsetof(Rider, eyelidFrames) == 0x2BD2 && offsetof(Rider, softParts) == 0x2BD8);
    static_assert(offsetof(Rider, tapCount) == 0x125 && offsetof(Rider, taps) == 0x128);
    static_assert(offsetof(Rider, spinStat) == 0x28 && offsetof(Rider, airStick) == 0x1B4 && offsetof(Rider, alignFrom) == 0x27A0 &&
                  offsetof(Rider, alignAngles) == 0x27E0 && offsetof(Rider, messenger) == 0x2838);
    static_assert(offsetof(Rider, input) == 0x130 && offsetof(Rider, flags) == 0x178);
    static_assert(offsetof(Rider, actionStates) == 0x17F && offsetof(Rider, wheels) == 0x25A0);
    static_assert(offsetof(Rider, competitionRuns) == 0x25E0);
    static_assert(offsetof(Rider, trick) == 0x18E && offsetof(Rider, trickMotion) == 0x18F && offsetof(Rider, health) == 0x194 &&
                  offsetof(Rider, bailKind) == 0x196);
    static_assert(offsetof(Rider, model) == 0x120 && offsetof(Rider, controllerLayout) == 0x124 && offsetof(Rider, score) == 0x1C0);
    static_assert(offsetof(Rider, contactKind) == 0x2601 && offsetof(Rider, contact) == 0x2740 && offsetof(Rider, snapPoint) == 0x2750);
    static_assert(offsetof(Rider, right) == 0x2610 && offsetof(Rider, forward) == 0x2630 && offsetof(Rider, groundHeight) == 0x2670);
    static_assert(offsetof(Rider, touchedObject) == 0x2690 && offsetof(Rider, pitch) == 0x26F0 && offsetof(Rider, roll) == 0x26F4);
    static_assert(offsetof(Rider, physicsState) == 0x2600 && offsetof(Rider, velocity) == 0x26C0 && offsetof(Rider, speed) == 0x26F8);
    static_assert(offsetof(Rider, body) == 0x2C00 && offsetof(Rider, frontWheelHeight) == 0xF0 && offsetof(Rider, backWheelHeight) == 0x110);
    static_assert(offsetof(Rider, airStat) == 0x20 && offsetof(Rider, skidDistance) == 0x1A0 && offsetof(Rider, edgeHeight) == 0x1B0);
    static_assert(offsetof(Rider, voiceWait) == 0x192 && offsetof(Rider, bailBody) == 0x2AD0);
    static_assert(offsetof(Rider, rideDistance) == 0x1A4 && offsetof(Rider, softParts) == 0x2BD8 && offsetof(Rider, pedalForce) == 0x2C40);
    static_assert(offsetof(Rider, player) == 0x17E && offsetof(Rider, actionFrames) == 0x184 && offsetof(Rider, trickLoops) == 0x190);
    static_assert(offsetof(Rider, yawTurned) == 0x198 && offsetof(Rider, trickTurned) == 0x19C && offsetof(Rider, balanceFrames) == 0x1A8);
    static_assert(offsetof(Rider, grindBalance) == 0x2674 && offsetof(Rider, wheelieBalance) == 0x2678 && offsetof(Rider, barLean) == 0x2680);
    static_assert(offsetof(Rider, motionFlags) == 0x2840 && offsetof(Rider, motionName) == 0x2844 && offsetof(Rider, motionRate) == 0x284C);
    static_assert(offsetof(Rider, topTrickBlend) == 0x29B0 && offsetof(Rider, topTrickFrames) == 0x29B4 && offsetof(Rider, wheelDrag) == 0x2C44);
    static_assert(offsetof(Rider, touchPoint) == 0x26A0 && offsetof(Rider, touchRotation) == 0x26B0 && offsetof(WheelPose, matrix) == 0x20);
    static_assert(offsetof(Rider, snapRows) == 0x2760 && offsetof(Rider, headBone) == 0x283C && offsetof(WheelPose, angle) == 0x62 && offsetof(WheelPose, bone) == 0x74);
    static_assert(offsetof(Rider, snapFrames) == 0x2602 && offsetof(Rider, snapStep) == 0x26E0);
    static_assert(offsetof(Rider, speedStat) == 0x24 && offsetof(Rider, balanceStat) == 0x30);
    static_assert(offsetof(Rider, massHeight) == 0xB0 && offsetof(Rider, springStiffness) == 0xC4 && offsetof(Rider, springDamping) == 0xC8);
    static_assert(offsetof(Rider, barsTilt) == 0x70 && offsetof(Rider, grips) == 0x90 && offsetof(Rider, position) == 0x2650);
    static_assert(offsetof(Rider, seatGrip) == 0x80 && offsetof(Rider, sprocketBone) == 0x2D58);
    static_assert(offsetof(Rider, rootOffset) == 0x2660 && offsetof(Rider, bodyPitch) == 0x26F6 && offsetof(Rider, messenger) == 0x2838);
    static_assert(offsetof(Rider, motionRoot) == 0x2860 && offsetof(Rider, motionRootRotation) == 0x2870 && offsetof(Rider, limbTargets) == 0x29C0 && offsetof(Rider, drawnOffset) == 0x2BF0);
    static_assert(offsetof(Rider, bikeModel) == 0x2D50 && offsetof(Rider, bikeBarsBone) == 0x2D54);
    static_assert(offsetof(Rider, shooter) == 0x2BDC && offsetof(Rider, limbRig) == 0x2AC0 && offsetof(Rider, hands) == 0x2AE0 && offsetof(Rider, drawnPosition) == 0x2BE0 && offsetof(Rider, wheelPoses) == 0x2C50 && offsetof(Rider, rollingVoice) == 0x2D64);

    // The rider's contact as the collision code's reference (a face's or an
    // edge's: its node and face index, collision::FaceRef / EdgeRef).
    inline uint64_t contactRef(const Rider *rider)
    {
        uint64_t ref;
        std::memcpy(&ref, rider->contact, sizeof(ref));
        return ref;
    }

    // Rider::motionFlags (DM2's).
    constexpr uint32_t kMotionRestart = 0x1u; // play the motion from its start
    constexpr uint32_t kMotionFlag4 = 0x4u; // the motions not stepped while it is set (stepRiderMotion; setActionState clears it)
    constexpr uint32_t kMotionLean = 0x10u; // the lean motions blended in by leanBlend (DM2's kMotionFlagLean)
    constexpr uint32_t kMotionFlag2 = 0x2u; // (the flatland trick's cut sets it; stepRiderMotion clears it, reading it to no
                                            // effect: not named)

    // A rider flag: a bit of one of the flag bytes (Rider::flags). DM2 keeps
    // the same flags in one word (its Rider +0x160), in another order; BMX
    // XXX's has some of its own between them.
    struct RiderFlag
    {
        uint8_t byte; // which of the flag bytes
        uint8_t mask; // its bit
    };
    inline bool hasFlag(const Rider *rider, RiderFlag flag) { return (rider->flags[flag.byte] & flag.mask) != 0; }
    inline void setFlag(Rider *rider, RiderFlag flag) { rider->flags[flag.byte] |= flag.mask; }
    inline void clearFlag(Rider *rider, RiderFlag flag) { rider->flags[flag.byte] &= static_cast<uint8_t>(~flag.mask); }

    // The flags, each the counterpart of DM2's flag of the same name (the
    // functions both games share use them alike), but for BMX XXX's own.
    // No player's rider (an NPC's): newRider sets it for a request with bit 0 of its first byte (DM2's
    // kRiderNoPlayer) and then makes no "dynamics.rider" bodies (the bail tumbles in state 12 instead); a level-0
    // change leaves the camera alone. DM2's name (r9dm2game6, DM2's newRider: the same test the same way).
    constexpr RiderFlag kFlagNoPlayer{0, 0x01};
    constexpr RiderFlag kFlagShown{0, 0x02};         // shown (riderReset; DM2's name)
    // The rider's and the bike's models shown (riderReset sets both with
    // kFlagShown; the camera's update hands them to setSceneObjectShown).
    constexpr RiderFlag kFlagRiderModelShown{0, 0x04};
    constexpr RiderFlag kFlagBikeModelShown{0, 0x08};
    constexpr RiderFlag kFlagModelChanging{0, 0x10}; // the rider's model still being built (riderModelReady clears it
                                                     // the frame the build ends)
    // The rider's model is a female rider's: newRider sets it when one of its parts' names has an "F" second, as
    // the female riders' "GFHAC_NIGHTVISION" has (the male riders' "GM..").
    constexpr RiderFlag kFlagFemale{0, 0x20};
    constexpr RiderFlag kFlagIdle{0, 0x80};          // still for the idle's frames (the coast state sets it)
    constexpr RiderFlag kFlagAutoPedal{1, 0x01};     // pedals by itself while under the pedalling speed (pedalState; DM2's name)
    constexpr RiderFlag kFlagTapsOn{1, 0x02};        // the stick's direction counts as a tap (clearTaps)
    constexpr RiderFlag kFlagTrickAWaiting{1, 0x04}; // trick button A pressed before the taps
    constexpr RiderFlag kFlagTrickBWaiting{1, 0x08}; // the same for button B
    // An action taken this frame (DM2's kFlagActed at the same places): set by ridingState (the taps into a manual
    // or a nose wheelie), airState, wheelieState (a trick with taps takes the manual to action state 6),
    // swivelState and flatlandTrickAState; riderFrame clears it before the states; the scoring reads it
    // (trickScoreFrame, riderHasMoveToScore).
    constexpr RiderFlag kFlagActed{1, 0x10};
    constexpr RiderFlag kFlagTrickHeld{1, 0x20};     // the trick button still held (unfinishedTrickBail)
    // The motion restarted in the last motion step (stepRiderMotion sets it, riderFrame clears it before the
    // step; the replay's frame reads it).
    constexpr RiderFlag kFlagMotionRestarted{1, 0x80};
    // A turn by alignAngles over the snap's frames (DM2's name, inferred there): while it is set and the snap
    // lasts, airState takes alignAngles[1] into yawTurned, and clears it once its own turn is the faster.
    constexpr RiderFlag kFlagSnapAlign{2, 0x40};
    constexpr RiderFlag kFlagMotionEnded{2, 0x01};   // the motion played to its end
    constexpr RiderFlag kFlagMirrored{2, 0x02};      // the bike mirrored
    constexpr RiderFlag kFlagTopTrick{2, 0x04};      // a trick on top (poseBikeGround)
    constexpr RiderFlag kFlagTrickTurn{2, 0x20};     // a turning trick's turn on
    constexpr RiderFlag kFlagHopUp{3, 0x01};         // hopped with the stick pushed straight up, off contact kinds 1 and 3
                                                     // (DM2's name, inferred there; landing on the ground clears it)
    constexpr RiderFlag kFlagFellOff{3, 0x04};       // fell off the rail's side (the grind's move sets it)
    constexpr RiderFlag kFlagGrindPressed{3, 0x08};  // the grind button pressed while riding (ridingState; a grind is asked
                                                     // for only after it)
    constexpr RiderFlag kFlagGrindAsked{3, 0x10};    // the grind button held (wheelieTurn)
    constexpr RiderFlag kFlagFakie{3, 0x40};         // riding backwards (groundEnter)
    constexpr RiderFlag kFlagMirrorSide{4, 0x01};    // the rail, lip or wall met is on the bike's mirrored side
    constexpr RiderFlag kFlagLeftRail{4, 0x10};
    constexpr RiderFlag kFlagTakeoffBack{4, 0x20};   // took off rolling backwards (airEnter sets it; the score frame marks a spin after it; DM2's name)
    constexpr RiderFlag kFlagWeakHop{4, 0x40};       // hopped with edgeHeight under half a unit (DM2's name, inferred there)      // the air came off a grind, a stall or a lip
    constexpr RiderFlag kFlagBailHands{5, 0x01};     // the bail's hands reach for the ground (DM2's name)
    // The rider's model has the night vision goggles (its part "GMHAC_NIGHTVISION" or "GFHAC_NIGHTVISION":
    // riderModelReady); cameraTask tints the first-person views by it.
    constexpr RiderFlag kFlagNightVision{5, 0x04};
    // BMX XXX's own, each cleared on every physics state change
    // (setPhysicsState) and both set by BMX XXX's flatland (between
    // DM2's wheelie and landing):
    // - the steering leaves the bars as they are: poseBikeGround sets it when
    //   the trick on top turns them itself, and while it is set does not turn
    //   them by the yaw rate (DM2 tests the trick each frame instead);
    constexpr RiderFlag kFlagBarsHeld{2, 0x08};
    constexpr RiderFlag kFlagLipLanding{2, 0x80};    // a landing on a lip's face tests the lip (landingTest; DM2's name)
    // - wheelieMove does not lean the drawn body by the balance (its turn
    //   about the right row in the manual and the nose wheelie) while it is
    //   set.
    constexpr RiderFlag kFlagBalanceLeanOff{2, 0x10};
    // BMX XXX's own: the grind's balance turned round, a grind against the rail rows' ahead row (the rider's
    // forward): the approaches' enter and railMount set it with kFlagMirrorSide, turning the grind balance round
    // (railMount: whenever it is set or set again); the grind's and stall's sub-states turn the stick's lean
    // round by it and the balance display is shown turned round (kBalanceTurnedRound); a level-0 change to
    // riding or to a state past the landing clears it.
    constexpr RiderFlag kFlagBalanceTurnedRound{3, 0x80};
    // The rider is down for good: his health ran out (addRideDistance, riderHitByProjectile: at 0 health), or an
    // NPC's bail (npcBailFrame, for its 180 frames). The bail does not end while it is set (bailHeld), the game
    // modes' turn ends take it as the turn decided, and riderReset (the rider shown) and the respawn clear it.
    constexpr RiderFlag kFlagDowned{0, 0x40};
    // - trickLineEvent grades a landing by landingTravelAngle only with it
    //   set, where DM2's trickEvent tests its kFlagTravelFound (the landing's
    //   travel along the ground was found).
    constexpr RiderFlag kFlagTravelFound{4, 0x80};
    // Move 24 held while riding (ridingState; in every controller layout move 24 is the skitch's button, and
    // ridingState is its only test): asks a standing NPC (meetNpc) or a path man (riderMayTakeObjective) near
    // the rider for its objective. riderFrame clears it before the states.
    constexpr RiderFlag kFlagObjectiveAsked{5, 0x02};
    // On one wheel: set as a manual or a nose wheelie starts (ridingState's taps, the air's landing onto one
    // wheel), cleared as the wheelie ends (wheelieSubState's way off) and by a level-0 change to the air (DM2's
    // change to the air clears its kFlagManual and kFlagNoseWheelie there). groundAhead's second ray (this wheel
    // down, the other off) runs only with it set.
    constexpr RiderFlag kFlagOnOneWheel{4, 0x02};
    // One wheel off the ground and the other's up row within 50 degrees of upright (the ground move sets or
    // clears it then; DM2's kFlagUpright): no auto-pedal while it is set; physicsFrame clears it before each
    // frame's move.
    constexpr RiderFlag kFlagOneWheelUpright{4, 0x04};
    // Something hard met (DM2's kFlagHitHard, at the same place): groundProbe sets it on a wall hit faster than 0.1
    // (with kFlagMirrorSide by the side), airBodyContact on a body's touch of a face. Then the air move does not
    // fly the rider on by his velocity, and the fakie entered with it set turns round as soon as its start motion
    // has played; ridingState clears it when not riding backwards.
    constexpr RiderFlag kFlagHitHard{4, 0x08};
    // grindMove took the rider off the rail into the air (past its end with no next rail, or pushed off a blocked
    // rail; its only setter): airState's side hop (a hop in the air's first 15 frames) needs it and clears it;
    // airEnter clears it.
    constexpr RiderFlag kFlagOffRailIntoAir{3, 0x20};
    // A hop over a ramp's edge that drops: with it the take-off's hop stops the velocity (DM2's kFlagHopRamp, at
    // the same tests, falls by gravity alone there) instead of turning it along the edge, and airState's coast
    // is not taken. Nothing in BMX XXX's PS2 code sets it (every store to its byte checked: only tests, and
    // clears with kFlagHopUp); DM2's code does not set its own either.
    constexpr RiderFlag kFlagHopDrop{3, 0x02};
    // The coast just started: airState sets it as it enters the coast (kAirCoast: the "air.coast.loop" motion), riderFrame
    // clears it before the states. That frame airRotate starts the alignment to the world's up over 20 frames,
    // and trickScoreFrame ends an open line as a bail of kind 11.
    constexpr RiderFlag kFlagCoastStarted{1, 0x40};

    // The physics states (Rider::physicsState): BMX XXX has 18, as many as
    // its exit and enter tables (DM2 11); these are DM2's.
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
        kPhysicsLipTrick = 8,
        kPhysicsNoseWheelie = 9, // on the front wheel
        kPhysicsManual = 10, // on the back wheel (poseBikeGround lifts the bike's root)
        kPhysicsPullUp = 11, // the bail's pull-up off a held edge (bailState)
        kPhysicsTumble = 12, // the rider's and the bike's bodies tumble (a bail of a rider with no player; a fall off a rail)
        kPhysicsTumbleLanded = 13, // the tumble on the ground (bailState; the tumble's pose puts the hips bone there)
        // BMX XXX's own: the skitch. Its approach (the skitch action state asks for it): its enter finds a tagged
        // corner of a car near the rider that is moving with it and snaps to the hold point there over the
        // snap's frames; then the hold, pinned to the car's corner each frame with the car's velocity
        // (physics/skitch.cpp).
        kPhysicsSkitchApproach = 14,
        kPhysicsSkitch = 15,
        // BMX XXX's own: the ghost ride (its action state asks for it): the rider tumbles off and the bike rolls
        // on by itself; then both tumble, after 240 frames or when the rolling bike hits a wall hard (groundProbe).
        kPhysicsGhostRide = 16,
        kPhysicsGhostRideTumble = 17,
        kPhysicsNone = 0xFF, // before the first
    };
    constexpr uint32_t kPhysicsStates = 18;

    // The action state levels (Rider::actionStates; DM2's).
    enum ActionLevel : uint32_t
    {
        kLevelAction = 0,   // ActionState
        kLevelSubState = 1, // the action's sub-state
        kLevelPhase = 2,    // the sub-state's phase
    };
    constexpr uint32_t kActionLevels = 4;
    // The action states of level 0 (the action table's order): DM2's to the
    // wheelie, then one of BMX XXX's own, then DM2's landing and the rest.
    enum ActionState : uint8_t
    {
        kActionRiding = 0,
        kActionAir = 1,
        kActionGrind = 2,
        kActionStall = 3,    // (its state function is the lip trick's too)
        kActionLipTrick = 4,
        kActionWheelie = 5,
        // BMX XXX's own: the flatland tricks on one wheel (its sub-states'
        // motions "flatland.*"): it keeps the manual's balance (a bail when
        // it tips too far) and sets kFlagBarsHeld and kFlagBalanceLeanOff.
        kActionFlatland = 6,
        kActionState6 = kActionFlatland, // (its old name, which objectives/trick_type.cpp still uses: to go)
        kActionLanding = 7,
        kActionBail = 8,
        // BMX XXX's own: the skitch, holding on to a car ("ground.skitch.*";
        // riding enters it with move 9 held when physics state 14, the skitch's approach, takes),
        // and the ghost ride ("ground.ghostRide.*"; riding enters it by a
        // move of its own, ridingState).
        kActionSkitch = 9,
        kActionGhostRide = 10,
    };
    // A sub-state's phases (level 2; DM2's).
    enum Phase : uint8_t
    {
        kPhaseIn = 0,   // entered this frame: start the motion
        kPhaseHeld = 1, // the motion running
    };
    // Riding's sub-states (level 1 of kActionRiding; DM2's).
    enum RidingSubState : uint8_t
    {
        kRidingCoast = 0,
        kRidingPedal = 1,
        kRidingBrake = 2,   // (DM2's "manual": BMX XXX's motions name it, "ground.brake.start")
        kRidingCrouch = 3,
        kRidingFakie = 4,
    };
    // The air's sub-states (level 1 of kActionAir; DM2's).
    enum AirSubState : uint8_t
    {
        kAirNoTrick = 0, // airNoTrick
        kAirTrick = 1,
        kAirTrickB = 2, // the second trick button's
        kAirSubState3 = 3, // the air after a rail, a stall or a lip (DM2's name)
        kAirCoast = 4,     // BMX XXX's own: "air.coast.loop" with no trick (move 17 off a lip; airNoTrick runs it)
    };
}

namespace game::bmx::rider
{
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

    // A trick slot with no trick.
    constexpr uint8_t kNoTrick = 0xFF;

    // The riders' slots (riderOf's indexes).
    constexpr uint32_t kRiderSlots = 16;

    // (contact.cpp, input.cpp, physics_state.cpp, rider_state.cpp, tricks.cpp)
    uint8_t actionState(const Rider *rider, uint32_t level);
    ee::Ptr<Rider> riderOf(uint32_t index);
    void setRiderBikeStats(Rider *rider, uint32_t line, uint32_t bike);
    void addImpulse(Rider *rider, const ee::Float *impulse);
    void physicsInit();
    uint8_t physicsState(const Rider *rider);
    uint8_t setPhysicsState(Rider *rider, uint32_t state);
    uint32_t groundEnter(Rider *rider, uint32_t from, uint32_t to);
    uint32_t grindExit(Rider *rider, uint32_t from, uint32_t to);
    uint32_t bailExit(Rider *rider, uint32_t from, uint32_t to);
    int32_t unfinishedTrickBail(Rider *rider);
    int32_t wheelieTurn(Rider *rider);
    uint8_t grindTrick(const Rider *rider);
    uint8_t stallTrick(const Rider *rider);
    uint8_t airTrickByTaps(Rider *rider);
    uint8_t lipTrick(const Rider *rider);
    uint8_t tapTrick(const Rider *rider, uint32_t tap);
    uint8_t trickFromTaps(const Rider *rider);
    uint8_t flatlandTrickByTaps(const Rider *rider, uint32_t button);
    int32_t buttonTest(const ButtonRecord *record, uint32_t buttons, uint32_t mode);
    void clearTaps(Rider *rider, uint32_t keepStick);
    uint8_t setActionState(Rider *rider, uint32_t level, uint32_t state);
    void groundAfter(Rider *rider);
    void airAfter(Rider *rider);
    void poseBikeAir(Rider *rider);
    void poseRiderBones(Rider *rider); // (trick_pose.cpp)
    void poseBikeGround(Rider *rider);
    int32_t riderMotionByHash(uint32_t name, ee::Ptr<zge::Motion> *motion);
    int32_t riderMotionByName(uint32_t name, uint32_t *nameHash, ee::Ptr<zge::Motion> *motion);
    int32_t airNoTrick(Rider *rider);
    void airGravity(Rider *rider);
    void grindSlope(Rider *rider);
    void leanRowsToRail(Rider *rider, uint32_t trick);
    void grindOnto(Rider *rider, uint64_t edge, int32_t boost, uint32_t trick);
    uint32_t grindEnter(Rider *rider, uint32_t from, uint32_t state);
    void stallMove(Rider *rider);
    uint32_t leaveAirExit(Rider *rider, uint32_t from, uint32_t to);
    void closestOnRail(const ee::Float *point, uint64_t edge, int32_t clamp, ee::Float *out);
    int32_t sameRailEdge(uint64_t a, uint64_t b);
    uint64_t closestRailEdge(Rider *rider, const ee::Float *point, ee::Float *closest, ee::Float radius);
    void approachGravity(Rider *rider);
    uint32_t approachMove(Rider *rider);
    void groundPoseAfter(Rider *rider);
    void grindAfter(Rider *rider);
    void lipTest(Rider *rider, uint64_t edge, int32_t anyAngle);
    // (physics/air.cpp) The angles a frame that turn the rows `current` into `previous` over `frames` frames.
    void airRotationTrack(const ee::Float *current, ee::Float *previous, int32_t frames, int16_t *angles);
    void airRotate(Rider *rider);
    // (pose.cpp) Physics states 9 and 10's after callback.
    void wheelieAfter(Rider *rider);
    void poseTumble(Rider *rider, int32_t landed);
    void tumbleAfter(Rider *rider);
    // (actions/bail.cpp) The rider's and the bike's tumbles a frame.
    void riderTumbleMove(Rider *rider);
    uint32_t wakeRiderTumble(Rider *rider, ee::Ptr<zge::Vec4> hipsHalfExtents, uint32_t twoSidedFaces);
    void bikeTumbleMove(Rider *rider);
    void landingFaceTest(Rider *rider, uint64_t face);
    // Declared for the other modules that call them; bodies not written yet.
    // The rider's and the bike's motion stepped by `seconds` (started over
    // when asked, blended, mirrored), the motion's root read, the pose made.
    // The rider's physics put back at the spawn face (DM2's resetPhysics):
    // still, on the ground; with bit 0 of `rowsFromSpawn` the rows are the
    // spawn's own, else they are stood on the face's normal.
    void resetPhysics(Rider *rider, uint32_t rowsFromSpawn);
    // (physics/respawn.cpp) The face to spawn on near a point, and the spawn's rows on it.
    uint64_t findSpawnFace(ee::Float *point, uint32_t kind, zge::Matrix *rows);
    // A player's rider put back for a new start and shown, or hidden.
    void riderReset(uint32_t index, uint32_t shown);
    // (actions/frame.cpp) A rider's frame, by its index; the motions stepped by `seconds` (1 when the motion
    // ended).
    void riderFrame(uint32_t index);
    int32_t stepRiderMotion(Rider *rider, ee::Float seconds);
    // 1 unless the rider's model is still changing (flag byte 0, bit 0x10);
    // the frame the change ends, the bones are found again and the rigs remade.
    int32_t riderModelReady(Rider *rider);
    // Each rider there is, by index, until the visit returns 0.
    using RiderVisit = ee::FunctionPtr<uint32_t(uint32_t riderIndex, ee::Address context)>;
    void forEachRider(RiderVisit visit, ee::Address context);
    void setUpLimbChains(Rider *rider);
    uint32_t playerIndexOfNode(uint32_t node);
    void freeRider(uint32_t index);
    // Once at start: the bone name hashes, the physics task, the rider records cleared, the dynamics loaded.
    void riderSetup();
    // (rider_state.cpp) A rider made for a request (game/player_riders.h): its slot, or 0xFF.
    uint8_t newRider(ee::Ptr<NewRider> request);
    void addRideDistance(Rider *rider, ee::Float distance);
    uint32_t riderIsFemale(const Rider *rider);
    void freeRiderDynamics();
    uint32_t deletePhysicsTask();
    uint32_t physicsShutdown();
    uint32_t riderModelHasPart(Rider *rider, uint32_t aspect, ee::Ptr<const char> part);
    // (actions/rails.cpp) The fall off a rail the grind's and stall's balance tip over into: onto the ground
    // beside the rail (physics state 0), else pushed off the rail's side in physics state 12 with kFlagFellOff.
    void fallOffRail(Rider *rider);
    // (actions/riding.cpp)
    int32_t coastState(Rider *rider);
    int32_t pedalState(Rider *rider);
    int32_t brakeState(Rider *rider);
    int32_t crouchState(Rider *rider);
    int32_t fakieState(Rider *rider);
    int32_t ridingState(Rider *rider);
    // (actions/landing.cpp)
    int32_t landingState(Rider *rider);
    // (actions/bail.cpp)
    int32_t bailState(Rider *rider);
    // (actions/manual.cpp)
    int32_t wheelieState(Rider *rider);
    // (actions/grind.cpp)
    int32_t grindState(Rider *rider);
    int32_t stallState(Rider *rider);
    int32_t grindSubState(Rider *rider);
    int32_t stallSubState(Rider *rider);
    // (actions/air.cpp)
    int32_t airSubState3(Rider *rider);
    int32_t airTrick(Rider *rider);
    int32_t airTrickB(Rider *rider);
    int32_t skitchState(Rider *rider);
    int32_t ghostRideState(Rider *rider);
    int32_t flatlandState(Rider *rider);
    int32_t swivelState(Rider *rider);
    int32_t flatlandTrickBState(Rider *rider);
    int32_t flatlandTrickAState(Rider *rider);
    // (tricks.cpp)
    uint32_t tapFramesApart(const Rider *rider, uint32_t later, uint32_t earlier);
    // A projectile's hit on a player's rider (the weapon module's message).
    uint32_t riderHitByProjectile(uint32_t player, ee::Ptr<Projectile> projectile, ee::Ptr<ProjectileMessage> message);
    void bailBodyRelease(Rider *rider);
    void bailHands(Rider *rider);
    void bailHandsHeld(Rider *rider);
    void bailHandContact(Rider *rider, BailHand *hand, const zge::Vec4 *point);
    void airPoseGroundSoundAfter(Rider *rider);
    Stick *inputStick(const ButtonRecord *record, uint32_t which);
    int32_t moveButtons(uint32_t layout, const ButtonRecord *record, uint32_t move, uint32_t which);
    int32_t padButtons(uint32_t port, uint32_t buttons, uint32_t mode);
    PadInput *padInput(uint32_t port);
    uint8_t stickDirection(const Rider *rider);
    Bounds *levelBounds();
    int32_t groundBelow(const ee::Float *point, GroundHit *hit);
    int32_t groundContactBelow(Rider *rider, int32_t *kind, uint64_t *face, uint64_t *edge, uint32_t *object);
    ee::Float planeDistance(const ee::Float *point, const ee::Float *plane);
    void setContact(Rider *rider, const void *contact, uint32_t kind);
    void setWheelContact(Rider *rider, uint32_t wheel, const void *contact, uint32_t kind, const ee::Float *point);
    int32_t surfaceSoundFlags(uint32_t flag20000, uint32_t flag10000, uint32_t kind, uint32_t surface);
}
