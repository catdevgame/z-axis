#!/usr/bin/env python3
"""Scripted routes for test runs: any level, the skater states the Tutorial's lessons reach.

Drives a native runner through ZX_CONTROL (tools/nctl.py), closed loop: the
moves wait on the game's state (position, mode), not on wall-clock time. Used
to record readable functions (ZX_FNREC) in states the logiccheck and
careercheck inputs never reach (docs/READABLE.md, round 6).

    routes.py start NAME [KEY=VAL ...]   launch RUNNER with a scratch HOME, card and support folder
                                         under $ROUTES_DIR/NAME (default /tmp/routes); a fresh card
    routes.py NAME CMD [ARGS] [; CMD ...]
    routes.py stop NAME                  stop the runner (by its PID)

Commands (NAME's runner must be running):
    boot                     intro skipped, to the main menu
    tutorial                 CAREER, first skater, the Tutorial (on any card), placed
    lesson K                 (in the Tutorial) pause, lesson K of the list, its tips closed
    stall | vpole | vpolea | hpole | skitch | skitchgo | wallride | walljump | vault | grind | manual | cess
                             (in the Tutorial) the lesson and its move; prints the skater
                             states seen (skater.h State numbers and names)
    stalldrop                (test only) the stall lesson, the stall's balance (Player +0x274)
                             poked to 1.5 twenty vsyncs in: 8 Stall, then 3 Air QPipe from it
    moves                    every move above in turn, then a table of the states reached
    doublegrind [TAPS [HOLD [ACTION [SPEED]]]]
                             (in the Museum's career, `career 7` first) 7 Double Grind on
                             the beam at x 63.8 .. 90.6, z -52.0 / -53.2 (docs/DEVTOOLS.md):
                             paused, the skater put over it facing +x, CROSS, the velocity
                             poked to SPEED (12) along +x (test only), TAPS (UP) three
                             times, TRIANGLE held HOLD vsyncs; ACTION a button held on the
                             rails, letgo, or raw:cmd,cmd. TAPS DOWN gives 6 Hand Grind.
                             The first two after a level load can miss.
    doublegrindat X Z DX DZ [HOLD [ACTION [SPEED]]]
                             the same on another Museum double rail (the ring:
                             11.4 -39.2 0.25 -0.97)
    spine [SIDE [SPEED [X [HOLD [JUMP [TURN]]]]]]
                             (in the Movie Lot's free skate, `freeskate 0` first) 4 Air
                             Spine over the spine whose lip runs x -33.1 .. -28.6 at
                             z -153.69, y -3.35 (docs/DEVTOOLS.md): paused, the skater
                             put on the floor 11.3 units off the lip on SIDE (+ or -, the
                             +z or -z side) at X (-31), facing the lip, upright; unpaused;
                             (test only) the velocity poked to SPEED (14) toward the lip,
                             UP held; the states over HOLD (150) vsyncs (Ground, 4 Air
                             Spine, then Air QPipe or Ground on the far side)
                             JUMP: an ollie (CROSS) that many units before the lip (0: none;
                             the enter's launch push runs only within 10 frames of a jump)
                             TURN: a button held from 4 Air Spine on (L1, R1: the turn about up)
    spines [N [SPEED [X [JUMP]]]]
                             N (4) spine runs, alternating sides
    ride X Y Z DX DZ [SPEED [JUMP [HOLD]]]
                             (test only) paused, the skater put at (X, Y, Z) facing (DX, 0, DZ),
                             upright; unpaused, the velocity poked to SPEED (14) along it, UP
                             held; JUMP: an ollie (CROSS) that many units along (0: none); the
                             states over HOLD (150) vsyncs. Take-offs off faces with launch
                             values (docs/DEVTOOLS.md, readable-game9)
    grindx [FACE [LEAN [SPIN [LAUNCH [HOLD [word]]]]]]
                             (test only, for recording) the Grinding lesson's grind with paths
                             forced: FACE back turns the skater half round in the air before
                             TRIANGLE (the Grind enter faces it along the motion, fakie flips);
                             LEAN (0) poked into the balance (Player +0x270) in the air (the
                             enter's lean, Best Reset ease-back and clamp); SPIN a button held
                             with TRIANGLE (L1, R1: the spin paths); LAUNCH (0: none) on the
                             rail, paused: the rail's edge record gets launch values (least 0,
                             most 0, speed LAUNCH), the rail record's direction (+0x240) is
                             tilted 0.25 up (its ends with it), the ollie counter (+0x79)
                             cleared, then CROSS: the
                             Air enter's rail launch; the states over HOLD (150) vsyncs;
                             word: on the rail, its event word (Player +0x1F8) poked to
                             another, so the next segment sends GrindEnd and GrindStart
    grindtricks [START [P1/P2/... [A1/...]]]
                             the Grinding lesson's grind started with d-pad taps START (- none,
                             taps comma separated, e.g. UP,UP) before TRIANGLE, then on the rail a
                             fresh TRIANGLE press after each P's taps (default -/LEFT/LEFT/RIGHT/
                             DOWN/UP): grind tricks combined with the current one (FUN_001EA138)
                             or dropped as the same trick (FUN_001EA050); then A's presses once
                             the grind is over (default -: the trick still current on the ground)
    hazards [LEAST]          the level's nodes with LEAST (2) or more faces whose surface sound is a
                             hazard (water, fire, out of bounds: skaterTouchSurface), from a RAM
                             dump, with their world boxes; `teleport` above one drops the skater on it
    hazardrun [MOST [HOLD]]  (for recording) the skater dropped, paused, two units over each of the
                             MOST (8) hazard nodes with most faces; the states over HOLD (120) vsyncs
    pickups                  the level's pickups (NPC challenge bubbles and the levels' items: their
                             0x90-byte state blocks, from a RAM dump) with mode, position, parts
    pickuprun [MOST [HOLD [FIRST [isolate]]]]
                             (for recording; the pushes and isolate test only) each pickup (an NPC)
                             run into: the skater put on it, or pushed through it from 3 units off;
                             its modes over HOLD (150) vsyncs, the skater 2 units off for the second
                             half (updatePickup); FIRST skipped; isolate: the others' `still` set
    combos [sf,fm,ms [SIDE]] (for recording, test only: pokes) the grind combos (SF, FM, MS Combo) asked
                             for on the Grinding lesson's rail, every trick unlocked, jizzed, and
                             Jizzed->Non-Jizzed Percent poked to 0; SIDE (RIGHT) the side tap
    doublegrindenter [LEAN [SHORTS [SPIN [SPEED [Z]]]]]
                             (for recording, test only: pokes; in the Museum, `career 7`) a Double
                             Grind with the balance's lean (Player +0x274), the short grind count
                             (+0x280) poked before TRIANGLE and L1 held with SPIN (doubleGrindEnter),
                             over the beam at Z (-52.6); tried until a Double Grind (at most 4)
    stalljump [STICK [AFTER]], grindjump [STICK [AFTER]]
                             (for recording; in the Tutorial) a hand plant, or the Grinding lesson's
                             grind, jumped off AFTER (20) vsyncs in with the d-pad STICK held (- none):
                             the jump's ways off (stallControls, grindControls)
    grindpoke [SCALE [AFTER [HOLD]]]
                             (test only, for recording) the Grinding lesson's grind, AFTER (10)
                             vsyncs on the rail its velocity poked to SCALE (3) times itself: past
                             Max Grind Speed, stopped (0), back along the rail (-1)
    poleaim BTN [DZ ...], poleaima, poleaim540, poleaima540
                             (test only, for recording) the Vertical Poles lesson with
                             the pole's aim and exit paths forced by pokes
    career K                 from the main menu: CAREER, first skater, level-select entry K
    splitscreen              from the main menu: MULTIPLAYER, the default game (Most Points,
                             Movie Lot, split screen), P2 on the next skater; placed
                             (start with ZX_PAD_PORT1=mirror: without a second
                             gamepad port 1 is empty and the game skips MULTIPLAYER)
    freeskate K [SKATER]     from the main menu: FREE SKATE, skater SKATER (0), level K
                             (other levels than the Tutorial and the Movie Lot need
                             ZX_DEV_UNLOCK_ALL=1 or an unlocked card)
    skate [N]                N rounds (1) of tools/sweep.py's routine: pushes, jumps, grabs,
                             grinds, turns, a manual
    menu                     pause, EXIT (back to the main menu)
    teleport X Y Z           the skater to (X, Y, Z), standing (y grows downward)
    states FRAMES            the skater's state changes over FRAMES vsyncs
    xp STAT POINTS           (test only) a stat's experience points (Player +0x638 + 12 STAT + 4;
                             0 Jump ... 6 Wallride) poked, e.g. 99 so the next trick levels up
    score N [PENDING]        (test only) the skater's score poked (Player +0x8C4's record +0x14), e.g.
                             999999000; PENDING: a pending trick score poked and a jump landed, so
                             it is banked (past 999,999,999 the score rolls over: "Rollover!")
    traffic [TRIES [PUSH [LEAD]]]
                             (in the Movie Lot, career 1 or freeskate 0) a hit by a taxi: the
                             skater put LEAD seconds ahead of the fastest car facing it,
                             the velocity poked to PUSH (16) toward it (test only); up to
                             TRIES cars (about one in three hits)
    challenge [complete|fail]
                             (in the Movie Lot's career, career 1) Kelly's "Get 40,000 points
                             in 60 seconds" played to its end: Death by Juice poked off and,
                             for complete, a 41,000 trick score poked and landed (test only)
    mash [FRAMES [SEED [KEY=VAL ...]]]
                             (in a level: career K, freeskate K first) FRAMES (10800: three
                             minutes) of seeded play from the skater's state: pushes, turns,
                             jumps; grabs, spins and flips only with the air time for them
                             (the take-off height, the vertical speed, the gravity measured),
                             let go before the landing; grinds, stalls, manuals with their
                             balance steered to the middle, grind tricks, poles, wall rides,
                             cess slides, vaults, NPCs talked to. A bail, a stop (2 s) or a
                             stay in a 5-unit square (6 s), and every 20 s on the ground
                             (visit=FRAMES, 0 never) teleports the skater (paused) to
                             one of the level's spots: its spawn points, its NPCs, the start,
                             and the approaches (position, velocity, buttons held about 25
                             frames before) to the rails, pipes, poles, walls the run has
                             reached. Actions and spots weighted by the uncovered game
                             functions each state's callbacks reach (the work database,
                             less play1 and MASH_REACHED profiles). Logs every event to
                             $ROUTES_DIR/NAME/mash-SEED.jsonl and a summary (bail rate,
                             states, teleports). KEY=VAL: tweaks=juice,push (the default;
                             also invulnerable, superspin, lowgravitywall, tricks,
                             perfectgrind, perfecthandplant, perfectmanual,
                             Category/Entry_Name=value; none), teleport=0, bias=0, step=2, visit=1200
                             (vsyncs a step), secs=540 (wall-clock limit; MASH_SECS).
                             style=natural: a player's mix for profiling (no bias, lines:
                             air into a manual, a manual into a grind, stalls on coping;
                             visits every 60 s; the perfect-balance cheats and
                             invulnerable on unless tweaks= is given). Manuals are Up-Down-
                             Up (or Down-Up-Down) taps with UP let go; riding up a ramp
                             holds TRIANGLE into the lip (a stall); a "stall" action
                             teleports to a learned quarter-pipe approach for one.
                             MASH_SPOTS=DIR keeps the learned spots per level.
                             coverage=FILE (or MASH_COVERAGE; fncheck_corpus.py coverage
                             --json): the weights from the uncovered blocks of the
                             covstatus= (Verified) functions each state's callbacks reach
                             instead; with ZX_FNREC_NEW_BLOCKS=1 the recorder's new-block
                             lines credit the spot the skater was put on (visited more).
    where                    position, state, level, mode
    sound [front|pause [N]]  (for recording) the sound options: front (from the main menu, cursor on
                             CAREER): OPTIONS, SOUND, the three volume sliders from 0 to 127,
                             accepted, back; pause (in a level): START, SOUND OPTIONS (pause entry N:
                             2, in split screen 3), the sliders,
                             R1 the playlist: tracks changed, every track off and on again, back to play
    controls                 (for recording; in the Tutorial, after `tutorial`) the left stick as an analog
                             stick (inside and past Control / Controller Deadzone on each axis: readStick),
                             then L1+R1, R2 and L2 presses on the ground (meant for twoButtonCommand and
                             cessSlideButton; they reach nothing new: the pad's pressed word, PadState
                             +0x0C, stays 0 for buttons held through ZX_CONTROL)
    pe2enter [THEME [load]]  (paths2: the Park Editor) from the main menu: PARK EDITOR, the first skater, New
                             Park in theme THEME (0-3, 1), or Load Saved Park (card slot 1, the first park)
    pe2terrain, pe2objects [VARIANTS], pe2rails [CELLS], pe2start, pe2camera
                             (in the editor) one tool each: the Terrain Tool's selections raised, lowered,
                             reselected, deleted, dropped; the 13 piece groups (VARIANTS pieces of each, R2),
                             rotated, placed in a row, one on a raised block, one refused, one deleted; the
                             Rail Tool's buttons on each piece of the row; the Start Tool; the camera sticks.
                             The tool roll (R1/L1) is 0 Terrain, 1-13 the piece groups, 14 Rail, 15 Start
                             (ASSETS/EDITOR/INLINE.PEP; the careers' in-level editors have Pisser, Crane and
                             Vacuum Tools instead, reached only from their givers)
    pe2test [N], pe2sound, pe2clear [no], pe2save [DESC [AUTHOR [ROW]]], pe2load [ROW], pe2exit [no]
                             the editor's pause menu: Test Run (skated N rounds, the test run's Sound
                             Options, Return to Editor), Sound Options, Clear Park, Save Park (card slot 1,
                             park ROW: 0 NEW; the keyboard), Load Park, Exit Editor (NO first, then YES)
    pe2railrow [VARIANTS]    (in the editor) the piece groups that take rails (4, 5, 8-13): VARIANTS pieces of
                             each placed, the Rail Tool at once on each: rails placed, changed, deleted
    pe2towers                (in the editor) pieces on raised and lowered terrain, the terrain under placed
                             pieces raised and lowered (the towers under them)
    pe2tools [THEME]         pe2enter, every tool, the camera and every pause entry, then exit
    pe2play [N]              from the main menu: FREE SKATE, the Editor Level (level select entry 7), Load
                             Park (slot 1, the first park), skated N rounds, pause, EXIT
    pe2ingame [ROUNDS [N]]   (in a career level whose in-level editor runs) every button, Do challenge, skated
    keys [MASK]              (paths3: career progress; in the front end) the levels' keys MASK (0xFE00: all seven,
                             bit level + 9, isKeyAwarded) ORed into the 20 skater records (+8, in the career save;
                             a level loaded next copies them into Player +0x5E8): the key areas open
    tweaks [T,T...]          (in a level) mash's named tweaks (juice,invulnerable: no Game Over)
    giver NAME [DIST]        (in a career level) the challenge giver NAME (the level script's NPC object: its node
                             from the scene's name-hash table): the skater put DIST (2.3) units off it on each side
                             in turn facing it, CIRCLE until its box opens, CROSS (accepted)
    editing                  wait for a career's in-level editor (after its giver)
    tool crane|vacuum|pisser [ROUNDS]
                             (in a career's in-level editor) its tool ROUNDS (3) times: Industrial's Crane (picks
                             and releases over a spiral: pieces dropped in the melt pits), Airfield's Vacuum
                             (pick, move, rotate, release), Cannery's Pisser (pieces chosen, placed, created,
                             rotated, moved, deleted); the camera turned and zoomed
    dochallenge              (in a career's in-level editor) START, Do challenge
    bank [POINTS]            (test only) a pending trick score POINTS (60,000) poked and a jump landed (banked)
    cardsave                 (main menu, cursor on CAREER) OPTIONS > SAVE & LOAD > SAVE to slot 1, YES
    carddialog [save|load [SLOT [STEP ...]]]
                             (paths9; main menu) OPTIONS > SAVE & LOAD > SAVE or LOAD, card slot SLOT (0), then each STEP (buttons
                             joined by +, e.g. DOWN+CROSS) pressed with 150 vsyncs after it: the dialogs of an unformatted,
                             missing or failing card (ZX_MC_SLOT1/2, ZX_MC_FAIL); back to the main menu
    menumash [PRESSES [SEED]]
                             (paths9; from the main menu, for recording) PRESSES (800) seeded random presses (d-pad half of them, CROSS a\n                             quarter, TRIANGLE, START, SQUARE, CIRCLE, L1, R1) 12 to 40 vsyncs apart: the menus' input handlers, no closed loop
    boxes, points [CLASS], talk NAME
                             CROSS through message boxes; the level's data points (all, or of CLASS, hex);
                             the skater put by the data point NAME, CIRCLE (the 0x5343 points named like a
                             giver are not where the giver stands: use giver)
    mainmenu                 back on the main menu after a mode (its panel up), the cursor on CAREER
    options [nocheats]       (for recording; from the main menu) OPTIONS: Save & Load (save to slot 1 twice:
                             new and overwrite; load slot 1, load and save slot 2), Sound and Controller
                             (values stepped both ways), Media Viewer (three entries played), Credits
                             (rolled), Cheats (every code of BRINGUP.md's table typed on the keyboard, w a s d
                             the arrow keys, and a wrong one; the cheat list toggled); back to the main menu
    timedrun K [SKATER]      from the main menu: TIMED RUN, skater SKATER (0), level-select entry K; placed
    timedend [THEN]          (in a Timed Run) UP held to the end of the clock, the statistics, then the Timed
                             Game menu's entry THEN (0 Start Run, 1 Sound Options then Start Run, 2 Level
                             Select, 3 Exit Game: back on the main menu)
    mpgame G [FRAMES [SEED]] (in split screen, `splitscreen` first) the Multiplayer menu's game G (0 Most
                             Points, 1 Best Trick, 2 Egg Hunt, 3 Twenty One, 4 Animal Rescue, 5 Sound
                             Options): FRAMES (3000) of mash, UP held to its end, the results, back to the menu
    airtricks [E,E..|all [LIFT [BUTTON [jizz]]]]
                             (test only, for recording; in the Tutorial) each air trick of the trick table
                             (0x245928: d-pad taps + Square; Tom Hayes' FAQ section 5): from the level's
                             start an ollie, the vertical velocity poked to LIFT (-40: up) for the air time,
                             the taps and BUTTON (SQUARE), to the landing; jizz: every trick unlocked and
                             the skater jizzed first (the special tricks, 33 to 38)
    airchain [JUMPS [SEED [LIFT]]]
                             (test only, for recording; in the Tutorial) JUMPS (40) ollies with the air time
                             poked (-60), each with two to four seeded air tricks in a row (Square or
                             Circle, held or let go; every trick unlocked, jizzed): the trick objects'
                             combine, land and bail events
    pausetour [K,K..]        (in a career level) the pause menu's pages K (1,2,4,5,3: Challenges, Attributes,
                             View Loot, Trick List, Save): each opened, presses inside (a challenge's
                             detail and fly-thru, a list's categories), back to play
    devmenu [STEP]           (in a level; the runner with ZX_DEV_MENU=1) the hidden debug menu
                             (docs/DEVTOOLS.md): Toggle Debug three times, Respawn, the Art Viewer (every
                             button, Select: its screen dump), every Tuning category (each STEP-th (3) entry
                             stepped up and back), Resume
    buttons [N]              every button held in turn (20 vsyncs each) for N (600) vsyncs: the developer
                             game modes' input (readable_corpus.py cov-devmodes)
    mpround G [FRAMES [SEED [CRASHES [TOUCH]]]]
                             (paths4; in split screen, `splitscreen` first) the Multiplayer menu's game G (0 Most
                             Points, 1 Best Trick, 2 Egg Hunt, 3 Twenty One, 4 Animal Rescue): from play pause >
                             GAME SELECT and the RESULTS (the round ended early), Continue, G chosen, FRAMES (1500)
                             of mash, TOUCH (1): every live trigger zone touched (zonerun), CRASHES (3) bails (test
                             only: mpcrash), UP held to the end of its clock, the RESULTS, Continue. (mpgame G picks
                             split screen's pause entry G, not a game: Resume, Game Select, Trick List, ...)
    mpmenu                   (in split screen) to the Multiplayer menu (the game choice) by the pause and RESULTS
    mpcrash [N [LIFT [BUTTON]]]
                             (test only) N (3) bails: an ollie, the vertical velocity poked to LIFT (-15), BUTTON
                             (R1: a spin) held through the landing (landed sideways)
    zones, zonerun [T,T..|all [HOLD]]
                             the level's trigger zones (zones.cpp's list at 0x279674: type, live, position); (test
                             only) the skater put on each live zone (of types T) for HOLD (40) vsyncs
    pausepages [MOST]        (in a career level) Challenges: CIRCLE on each of the first MOST (16) entries (those with
                             the camera icon play their fly-thru: inlinePausePlayFlyThru), Summary (R1), a detail;
                             the Trick List's six categories, Specials last (every trick unlocked first: test only)
    cammode [M,M.. [FRAMES]] (test only; in a level) the follow camera's mode byte (0x24C9D0 + 0x71) set to each M
                             (1,4: updateCamera's other cameras) for FRAMES (150) vsyncs, riding, then restored
    poke ADDR VAL [ADDR VAL ...]
                             (test only) words written (e.g. zfile's watcher switch 0x273A98)
    quit [WAIT]              (test only) main's loop flag 0x23FD64 set: main (FUN_001176C8) leaves its loop, the
                             game and engine shutdown run, the guest calls Exit(0) (the runner stays up)
    pieces                   (paths5; in a career level whose in-level editor ran) the edit pieces (Cannery's Pisser,
                             Airfield's Vacuum pieces: the list at 0x278B20): record, whole, scene object, position
    piecetouch [SPEED [HOLD]]
                             (test only: rides) each edit piece ridden into from 6 units off on each side at SPEED
                             (8) for HOLD (60) vsyncs: skaterEffects' piece check (Airfield: FUN_001C6728 per touch,
                             Cannery: the piece broken, its debris)
    player                   player one's record, its scene object (+0x14) and its ragdoll system (+0x0C, edwards.zad)
    wait N                   (paths6) N vsyncs of nothing (the game runs on)
    pausefor N               (paths6; in a level) paused (START) for N vsyncs, then back to play: what runs and
                             draws while the game is paused (the effects' clock goes on)
    cardfill [KB]            (paths6) card slot 1 filled with a KB (8000) file of its own (an 8 MB card holds 8,135
                             KB): too full for a save; KB 0 empties it again
    restartlevel [LEVEL]     (paths7; in Career or Freeskate; the runner with ZX_GAME_SETTINGS_MENUS=1) the pause
                             menu's Restart Level (item 1, the port's: the level's exit, then the same level,
                             skaters and mode loaded again); LEVEL (test
                             only): the level byte (0x24C9B4) set to it first, so that level loads instead (9:
                             LOCAL, the January 2002 test level no menu reaches, with its own scripts)
    shot FILE                (paths7) the presented picture to FILE (a run with the GS on: ZX_GS_NULL=0)
    buttons7 B,B.. [K,K..]   (paths7) each button pressed in turn; with K (the Park Editor's roll entries, as pe2roll:
                             0 Terrain, 1-13 the piece groups, 14 Rail, 15 Start) the presses in each of those tools
                             (the editors' button callbacks no other route pressed: Triangle, Square, Left, L3 ...)
    careerpoke K ADDR VAL [ADDR VAL ...]
                             (paths7; test only) `career K` with the words poked on the level select just before
                             its Cross (after the menus' own file opens), e.g. zfile's default device
    manualhold [B,B.. [HOLD]] (paths7; in the Tutorial) the Manuals lesson's manual, then each button B (SQUARE,
                             CIRCLE, TRIANGLE, CROSS, L1, R1) held HOLD (30) vsyncs and let go while in it (the
                             manual trick's StartHolding / StoppedHolding)
    stallhold [VSYNCS [TAPS]] (paths7; in the Tutorial) the Hand Plants lesson's stall, the stall table's trick of
                             the d-pad TAPS (as stalltrick), held VSYNCS (600) vsyncs (with `tweaks
                             perfecthandplant` first the balance holds): the trick's animation runs out

Lesson numbers (the Tutorial's pause list): 1 Talking to People, 2 Basic Skating,
3 Sharp Turns, 4 Fakie, 5 Jumping, 6 Manuals, 7 Grinding, 8 Modifying Grinds,
9 Air Tricks, 10 Exit Pipes and Bowls, 11 Bail off a Pipe, 12 Hand Plants,
13 Vaults, 14 Vertical Poles, 15 Horizontal Poles, 16 Skitching, 17 Wall Rides,
18 Cess Slides, then Trick Lines, Juice is Life, Attributes, Power Ups,
Graduation Day.

Environment: RUNNER (default the z-axis checkout's out/build-ai), ROUTES_DIR, ROUTES_SLOW (wall-clock
limits that many times longer, for a runner slowed down by recording); mash: MASH_SECS,
MASH_SPOTS, MASH_REACHED, MASH_COVERAGE, WORKDB_PY. Every KEY=VAL
of start goes to the runner (ZX_FNREC=..., ZX_DEV_UNLOCK_ALL=1, ...); ZX_ROUTES_CARD=DIR
puts a copy of a ready-made card (recomp/run/mc-sweep-park, ...) in slot 1 (the original is
never written).
"""
import math
import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nctl  # noqa: E402
import sweep  # noqa: E402

MODE, PANEL, LEVEL, FRAME, POS = sweep.MODE, sweep.PANEL, sweep.LEVEL, sweep.FRAME, sweep.POS
LEVEL_OBJECT = 0x24C9BC          # -> the level; its +0x10 -> the players; player one at +0x10
STATES = ("Ground Stand Air AirQPipe AirSpine Grind HandGrind DoubleGrind Stall VerticalPole "
          "VerticalPoleA HorizontalPole WallRide WallTap Manual CessSlide Vault Skitch").split()
# Player (game/aggressive/skater/skater.h)
P_STATE, P_DRAWN, P_POSITION, P_VELOCITY, P_LAST_POSITION = 0x74, 0x50, 0x90, 0xA0, 0xF0
NAME_ENTRY_END = 38             # the name entry's keyboard cursor on END (a new skater)
CART = 0x711CD0                  # the Tutorial's golf cart (node position), on the default heap
# ROUTES_SLOW=<n>: wall-clock limits n times longer, for a runner slowed
# down by recording (readable_corpus.py sets it): the closed-loop waits are
# on the game's vsyncs, but a batch's reply and the steering loops have a
# time limit that assumes the game runs at about 30 vsyncs a second.
SLOW = max(1.0, float(os.environ.get("ROUTES_SLOW", "1") or 1))


def f2u(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def above(addr, v):
    """u32 condition (StateProbe) for the float at addr > v."""
    return ("%#x<%#x" % (addr, f2u(v))) if v < 0 else ("%#x>%#x&%#x<0x80000000" % (addr, f2u(v), addr))


def below(addr, v):
    """u32 condition for the float at addr < v (v <= 0, or a value that is not negative)."""
    return ("%#x>%#x" % (addr, f2u(v))) if v <= 0 else ("%#x<%#x" % (addr, f2u(v)))


class Route(sweep.Driver):
    def batch(self, *cmds, timeout=600):
        """Append all commands at once (the runner runs them back to back at
        VBlank, with no client round trip between them); the replies."""
        if not cmds:
            return []
        d = nctl.ctl_dir(self.name)
        cmd_path, out_path = os.path.join(d, "cmd"), os.path.join(d, "out")
        with open(cmd_path) as f:
            first = sum(1 for _ in f) + 1
        with open(cmd_path, "a") as f:
            for c in cmds:
                f.write(c + "\n")
        last = first + len(cmds) - 1
        deadline = time.time() + timeout * SLOW
        while time.time() < deadline:
            got = {}
            with open(out_path) as f:
                for line in f:
                    i, _, rest = line.partition(" ")
                    if i.isdigit() and first <= int(i) <= last:
                        got[int(i)] = rest.rstrip()
            if last in got:
                return [got.get(i) for i in range(first, last + 1)]
            if not nctl.alive(d):
                raise nctl.RunnerGone(self.name)
            time.sleep(0.005)
        raise SystemExit("no reply within %d s" % (timeout * SLOW))

    # ---- the skater ---------------------------------------------------------

    def player(self):
        level = self.u32(LEVEL_OBJECT)
        return self.u32(level + 0x10) + 0x10 if level else 0

    def state(self, player=None):
        player = player or self.player()
        return self.u32(player + P_STATE) & 0xFF if player else None

    def watch(self, frames, step=2):
        """State changes over `frames` vsyncs (sampled every `step`)."""
        player, seen = self.player(), []
        for _ in range(max(1, frames // step)):
            r = self.batch("peek %#x" % (player + P_STATE), "peekf %#x 3" % POS, "wait %d" % step)
            s = int(r[0].split()[1], 16) & 0xFF
            if not seen or seen[-1] != s:
                seen.append(s)
                self.log("  state %d %s at %s" % (s, STATES[s] if s < len(STATES) else "?",
                                                   " ".join("%.2f" % float(v) for v in r[1].split()[1:])))
        return seen

    def teleport(self, x, y, z):
        player = self.player()
        cmds = []
        for base in (P_POSITION, P_LAST_POSITION, P_DRAWN):
            cmds += ["poke %#x %#x" % (player + base + 4 * i, f2u(v)) for i, v in enumerate((x, y, z))]
        cmds += ["poke %#x 0" % (player + P_VELOCITY + 4 * i) for i in range(3)]
        self.batch(*cmds)

    # ---- menus ------------------------------------------------------------

    def boot(self):
        self.batch("wait 400", "press START 10 30", "press START 10 30", "press START 10 30",
                   "until 0x276FE4==1 1800", "until 0x240A20!=0 1800", "wait 90")
        self.log("main menu, panel", hex(self.u32(PANEL)))

    def tips(self):
        """Close message boxes (CROSS while the game is in its box mode)."""
        n = 0
        while self.u32(MODE) == 3 and n < 30:
            self.press("CROSS", gap=40)
            n += 1
        return n

    def wait_placed(self, timeout=9000):
        ok = super().wait_placed(timeout)
        self.tips()
        return ok

    def career(self, k):
        p0 = self.u32(PANEL)
        self.press("CROSS", gap=30)
        p1 = self.new_panel(p0)            # player select
        self.press("CROSS", gap=30)
        p2 = self.new_panel(p1)            # a new skater's name entry (the cursor on END), or the level select
        if self.panel_cursor()[1] == NAME_ENTRY_END:
            self.press("CROSS", gap=30)
            self.new_panel(p2)
        self.set_cursor(k)
        self.log("career, level select entry", self.panel_cursor()[1])
        self.press("CROSS", gap=30)
        return self.wait_placed()

    def freeskate(self, k, skater=0):
        p0 = self.u32(PANEL)
        self.press("RIGHT", gap=30)
        self.press("CROSS", gap=30)
        p1 = self.new_panel(p0)            # player select
        self.set_cursor(skater)
        self.press("CROSS", gap=30)
        p2 = self.new_panel(p1)            # a new skater's name entry (the cursor on END), or the level select
        if self.panel_cursor()[1] == NAME_ENTRY_END:
            self.press("CROSS", gap=30)
            self.new_panel(p2)
        self.set_cursor(k)
        self.log("free skate, skater", skater, "level select entry", self.panel_cursor()[1])
        self.press("CROSS", gap=30)
        return self.wait_placed()

    def mode_level(self, entry, k, skater=0):
        """From the main menu (cursor on CAREER): main-menu entry `entry`
        (0 CAREER, 1 FREE SKATE, 2 TIMED RUN, 3 MULTIPLAYER, 4 PARK EDITOR,
        5 OPTIONS), skater SKATER, a new skater's name, level-select entry K."""
        p0 = self.u32(PANEL)
        for _ in range(6):
            self.press("LEFT", gap=30)
        for _ in range(entry):
            self.press("RIGHT", gap=30)
        self.press("CROSS", gap=30)
        p1 = self.new_panel(p0)            # player select
        self.set_cursor(skater)
        self.press("CROSS", gap=30)
        p2 = self.new_panel(p1)            # a new skater's name entry (the cursor on END), or the level select
        if self.panel_cursor()[1] == NAME_ENTRY_END:
            self.press("CROSS", gap=30)
            self.new_panel(p2)
        self.set_cursor(k)
        self.log("main menu entry", entry, "skater", skater, "level select entry", self.panel_cursor()[1])
        self.press("CROSS", gap=30)
        return self.wait_placed()

    def splitscreen(self):
        """Main menu (cursor on CAREER) -> MULTIPLAYER with the default game
        (Most Points, Movie Lot), P2 on the next skater, as speedsweep.py's
        mp; returns once the players are placed."""
        for _ in range(3):
            self.press("RIGHT", gap=40)
        for i in range(7):
            if i == 4:
                self.press("RIGHT", gap=40)
            self.press("CROSS", gap=60)
            self.batch("wait 30")
            if self.u32(MODE) == 2:
                break
        ok = self.wait_placed()
        self.log("split screen, placed" if ok else "split screen, not placed")
        return ok

    def menu(self):
        self.tips()
        return self.pause_exit("routes", "exit")

    def lesson(self, k):
        for _ in range(3):
            self.tips()
            before = self.f32(POS)
            self.press("START", gap=40)
            self.batch("wait 30")
            for _ in range(40):
                c = self.panel_cursor()[1]
                if c == k:
                    break
                self.press("DOWN" if c is None or c < k else "UP", gap=10)
            self.batch("wait 10")
            if self.panel_cursor()[1] == k:
                self.press("CROSS", gap=40)
                self.batch("wait 100")
                n = self.tips()
                if n:
                    self.log("lesson", k, "start", " ".join("%.2f" % v for v in self.f32(POS)))
                    return True
            self.log("lesson", k, "not started (at %s); again" % before)
            if self.u32(MODE) == 3:
                self.press("TRIANGLE", gap=40)
        return False

    # ---- closed-loop skating --------------------------------------------------

    def hold(self, held, want, *extra):
        cmds = []
        if held - want:
            cmds.append("up " + "+".join(sorted(held - want)))
        if want - held:
            cmds.append("down " + "+".join(sorted(want - held)))
        return self.batch(*(cmds + list(extra)))

    def goto(self, x, z, stop, limit=900, extra=()):
        """Hold UP (+extra) and steer with LEFT/RIGHT toward (x, z) from the
        measured heading until stop(px, py, pz); UP (+extra) left held."""
        held, heading = set(), None
        px, _, pz = self.f32(POS)
        deadline = time.time() + limit / 30.0 * SLOW
        while time.time() < deadline:
            r = self.hold(held, held, "peekf %#x 3" % POS)
            nx, ny, nz = (float(v) for v in r[-1].split()[1:])
            dx, dz, px, pz = nx - px, nz - pz, nx, nz
            want = {"UP"} | set(extra)
            if stop(px, ny, pz):
                self.hold(held, want)
                return True
            if dx * dx + dz * dz > 1e-4:
                heading = math.atan2(dz, dx)
            if heading is not None:
                err = (math.atan2(z - pz, x - px) - heading + math.pi) % (2 * math.pi) - math.pi
                want |= {"LEFT"} if err > 0.08 else {"RIGHT"} if err < -0.08 else set()
            self.hold(held, want)
            held = want
        self.hold(held, set())
        self.log("goto: timeout at", px, pz)
        return False

    def chase(self, addr, back=2.5, radius=3.0, limit=900, grab="CIRCLE"):
        """Skate behind a moving object (float vec3 at addr): to a point `back`
        units behind it, then at its back holding `grab` within `radius`,
        until the skater skitches (state 17) or is lifted (y < -0.5)."""
        player, held = self.player(), set()
        r = self.batch("peekf %#x 3" % POS, "peekf %#x 3" % addr)
        px, _, pz = (float(v) for v in r[0].split()[1:])
        ox, _, oz = (float(v) for v in r[1].split()[1:])
        heading = direction = None
        phase = 1
        deadline = time.time() + limit / 30.0 * SLOW
        while time.time() < deadline:
            r = self.hold(held, held, "peekf %#x 3" % POS, "peekf %#x 3" % addr, "peek %#x" % (player + P_STATE))[-3:]
            nx, ny, nz = (float(v) for v in r[0].split()[1:])
            cx, _, cz = (float(v) for v in r[1].split()[1:])
            s = int(r[2].split()[1], 16) & 0xFF
            dx, dz, px, pz = nx - px, nz - pz, nx, nz
            ex, ez, ox, oz = cx - ox, cz - oz, cx, cz
            if s == 17 or ny < -0.5:
                self.hold(held, {grab})
                return True
            if 1e-4 < ex * ex + ez * ez < 25:
                n = math.hypot(ex, ez)
                direction = (ex / n, ez / n)
            if dx * dx + dz * dz > 1e-4:
                heading = math.atan2(dz, dx)
            if direction is None:
                continue
            k = back if phase == 1 else 0.3
            tx, tz = cx - k * direction[0], cz - k * direction[1]
            if phase == 1 and math.hypot(tx - px, tz - pz) < 1.5:
                phase = 2
            want = {"UP"}
            if heading is not None:
                err = (math.atan2(tz - pz, tx - px) - heading + math.pi) % (2 * math.pi) - math.pi
                want |= {"LEFT"} if err > 0.08 else {"RIGHT"} if err < -0.08 else set()
            if phase == 2 and math.hypot(cx - px, cz - pz) < radius:
                want.add(grab)
            self.hold(held, want)
            held = want
        self.hold(held, set())
        return False

    # ---- the Tutorial's moves (the PCSX2 reference run 2's segments) ---------

    def move(self, name):
        release = "up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1"
        if name == "stall":                       # Hand Plants: up the quarter pipe, TRIANGLE at the lip
            self.lesson(12)
            self.batch("down UP", "until " + above(POS, -8.5) + " 600", "up UP", "down TRIANGLE")
            seen = self.watch(90)
            self.batch("up TRIANGLE")
            seen += self.watch(60)
        elif name == "stalldrop":                 # Hand Plants, the stall lost into the pipe (test only: a poke)
            self.lesson(12)
            self.batch("down UP", "until " + above(POS, -8.5) + " 600", "up UP", "down TRIANGLE")
            player = self.player()
            for _ in range(150):
                if self.state(player) == 8:
                    break
                self.batch("wait 2")
            # the stall's balance (Player +0x274) past 1 on the pipe's side:
            # holdUpdate drops the skater back into the pipe (3 Air QPipe)
            self.batch("wait 20", "poke %#x %#x" % (player + 0x274, f2u(1.5)))
            seen = self.watch(90)
            self.batch("up TRIANGLE")
            seen += self.watch(30)
        elif name == "vpole":                     # Vertical Poles: UP+CIRCLE into the pole
            self.lesson(14)
            self.batch("down UP+CIRCLE")
            seen = self.watch(240)
        elif name == "vpolea":                    # Vertical Poles: jump into the pole holding CIRCLE
            self.lesson(14)
            self.batch("down UP", "until " + above(POS, -9.0) + " 600", "down CROSS", "wait 3", "up CROSS",
                       "down CIRCLE")
            seen = self.watch(240)
        elif name == "hpole":                     # Horizontal Poles: a late jump, CIRCLE at the bar
            self.lesson(15)
            self.batch("down UP+CROSS", "until " + above(POS, -10) + " 600", "up CROSS", "down CIRCLE")
            seen = self.watch(120)
            self.batch("up UP", "wait 30", "up CIRCLE")
            seen += self.watch(90)
        elif name == "skitch":                    # Skitching: wait for the cart, chase it, hold CIRCLE
            self.lesson(16)
            self.batch("until " + above(CART + 8, 20) + " 1200", "until " + below(CART, -30) + " 1200",
                       "until " + below(CART + 8, 2) + " 1200")
            self.chase(CART)
            seen = self.watch(240)
        elif name == "skitchgo":                  # Skitching, CIRCLE let go a second into the skitch
            self.lesson(16)
            self.batch("until " + above(CART + 8, 20) + " 1200", "until " + below(CART, -30) + " 1200",
                       "until " + below(CART + 8, 2) + " 1200")
            self.chase(CART)
            self.batch("wait 60", "up CIRCLE")
            seen = self.watch(120)
        elif name == "wallride":                  # Wall Rides: jump at the wall at 45 degrees, DOWN+CROSS
            self.lesson(17)
            self.batch("down UP+CROSS", "until " + above(POS, -11) + " 600", "up CROSS",
                       "until " + above(POS, -5.5) + " 600", "up UP", "down DOWN+CROSS", "wait 6", "up DOWN")
            seen = self.watch(160)
        elif name == "walljump":                  # Wall Rides, CROSS let go on the wall: the jump off it
            self.lesson(17)
            self.batch("down UP+CROSS", "until " + above(POS, -11) + " 600", "up CROSS",
                       "until " + above(POS, -5.5) + " 600", "up UP", "down DOWN+CROSS", "wait 6", "up DOWN",
                       "wait 10", "up CROSS")
            seen = self.watch(160)
        elif name == "vault":                     # Vaults: UP+CIRCLE into the waist-high edge
            self.lesson(13)
            self.batch("down UP+CIRCLE")
            seen = self.watch(240)
        elif name == "grind":                     # Grinding: line up, jump, TRIANGLE
            self.lesson(7)
            self.goto(0, -0.3, lambda x, y, z: x > -16)
            self.batch("up LEFT+RIGHT", "down UP+CROSS", "wait 3", "up CROSS", "down TRIANGLE")
            seen = self.watch(150)
        elif name == "manual":                    # Manuals: rolling, UP let go, Up-Down-Up taps
            self.lesson(6)
            # The lesson starts the skater at x 46 (the old x > -15.1 wait
            # ended at once, standing: no manual); a manual is three quick
            # taps, Up-Down-Up (Down-Up-Down a nose manual).
            self.batch("down UP", "wait 50", "up UP", "wait 2", "press UP 2 2", "press DOWN 2 2", "press UP 2 2")
            seen = self.watch(150)
        elif name == "cess":                      # Cess Slides: rolling, R2 held
            self.lesson(18)
            self.batch("down UP", "wait 60", "down R2", "wait 3")
            seen = self.watch(60)
            self.batch("up R2+UP")
            seen += self.watch(60)
        else:
            raise SystemExit("unknown move " + name)
        self.batch(release)
        self.log(name, "states:", " ".join("%d %s" % (s, STATES[s]) for s in seen))
        return seen


MOVES = ["stall", "vpole", "vpolea", "hpole", "skitch", "skitchgo", "wallride", "walljump", "vault", "grind",
         "manual", "cess"]


# ---- paths2: the Park Editor, tool by tool -----------------------------------------
# (round paths2: routes to the Park Editor's swept drafts.) The editor's tools come
# from its theme file, ASSETS/EDITOR/INLINE.PEP: the front end's four themes have the
# Terrain, Object, Rail and Start tools (StartTool 0: Terrain first); the careers' in-level
# editors have one tool each (Cannery: EditPisserTool, Industrial: EditCraneTool, AirField
# 1-3: EditVacuumTool). In the editor the tool roll (R1 next, L1 previous: the UI script's
# OnNextTool/OnPrevTool) has 16 entries: 0 Terrain Tool, 1-13 the Object Tool's piece
# groups (R2/L2: the next/previous piece of the group, OnNextObject), 14 Rail Tool, 15 Start
# Tool. The on-screen help: Terrain "X Select / Reselect, O Drop, Square Delete, d-pad Move"
# (UP/DOWN raise and lower the selection); Object "X Place, O Rotate, Square Delete"; Rail
# "This object does not accept rails" off a rail piece; Start "X Place, O Rotate". The left
# stick turns the camera, the right stick's Y zooms. The pause menu (ParkEditor.zsc):
# Continue, Test Run, Save Park, Load Park, Clear Park, Sound Options, Exit Editor; in a
# test run: Continue, Sound Options, Return to Editor; in a career's editor: Continue, Do
# challenge. Confirmations are YES/NO with NO preselected.
PE_ROLL = 16
PE_RAIL, PE_START = 14, 15
PE_EDITOR = 0x278808              # the editor object while the editor runs (BRINGUP.md polish2)
# The card dialogs' keyboard (park description, author): 9 keys a row, END last.
PE_KEYS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+!? \x7f\x03"


def pe_taps(r, moves, gap=20):
    """d-pad taps: 'R3 U2' = RIGHT three times, UP twice (one grid cell a tap)."""
    names = {"R": "RIGHT", "L": "LEFT", "U": "UP", "D": "DOWN"}
    for m in moves.split():
        for _ in range(int(m[1:] or 1)):
            r.batch("press %s 6 %d" % (names[m[0]], gap))


def pe_roll(r, k):
    """The tool roll to entry K (R1 / L1, the shorter way)."""
    at = getattr(r, "pe_at", 0)
    fwd = (k - at) % PE_ROLL
    button, n = ("R1", fwd) if fwd <= PE_ROLL // 2 else ("L1", PE_ROLL - fwd)
    for _ in range(n):
        r.batch("press %s 6 50" % button)
    r.batch("wait 20")
    r.pe_at = k


def pe_yes(r, yes=True):
    """A YES/NO confirmation (NO preselected)."""
    if yes:
        r.batch("press UP 6 20")
    r.batch("press CROSS 6 60")


def pe_key(r, k):
    """The keyboard cursor to key K: across first, then down (the last row has 7 keys: a
    DOWN from columns 7 and 8 of the row above wraps to the top)."""
    for _ in range(40):
        c = r.panel_cursor()[1]
        if c is None or c == k:
            return c == k
        cr, cc = divmod(c, 9)
        kr, kc = divmod(k, 9)
        r.batch("press %s 6 12" % (("RIGHT" if kc > cc else "LEFT") if kc != cc else ("DOWN" if kr > cr else "UP")))
    return False


def pe_type(r, text):
    for ch in text:
        pe_key(r, PE_KEYS.index(ch))
        r.batch("press CROSS 6 15")
    pe_key(r, len(PE_KEYS) - 1)
    r.batch("press CROSS 6 60")


def pe_menu_cursor(r, k):
    for _ in range(12):
        c = r.panel_cursor()[1]
        if c == k or c is None:
            break
        r.batch("press %s 6 20" % ("DOWN" if c < k else "UP"))
    return r.panel_cursor()[1] == k


def pe_main_entry(r, k):
    """Main menu: entry K counted from OPTIONS backwards (5 OPTIONS, 4 PARK EDITOR,
    1 FREE SKATE): the menu does not wrap and skips MULTIPLAYER without a second pad."""
    r.batch("until 0x276FE4==1 3600", "until 0x240A20!=0 3600", "wait 60")
    for _ in range(6):
        r.batch("press RIGHT 6 30")
    for _ in range(5 - k if k >= 3 else 0):
        r.batch("press LEFT 6 30")
    if k < 3:
        for _ in range(6):
            r.batch("press LEFT 6 30")
        for _ in range(k):
            r.batch("press RIGHT 6 30")


def pe_skater(r):
    """Player select: the first skater; a new skater's name entry (cursor on END) passed."""
    p0 = r.u32(PANEL)
    r.press("CROSS", gap=60)
    p1 = r.new_panel(p0)
    r.press("CROSS", gap=60)
    p2 = r.new_panel(p1)
    if r.panel_cursor()[1] == NAME_ENTRY_END:
        r.press("CROSS", gap=60)
        p2 = r.new_panel(p2)
    return p2


def pe_load_dialog(r, row=0):
    """Load Park: card slot 1, park ROW, YES."""
    r.batch("press CROSS 6 60", "wait 150")
    pe_menu_cursor(r, row)
    r.batch("press CROSS 6 60", "wait 100")
    pe_yes(r)


def pe_editing(r, timeout=3600):
    """In the editor (the editor object set, play mode)."""
    r.batch("until 0x276FE4==2 %d" % timeout, "until %#x!=0 %d" % (PE_EDITOR, timeout), "wait 300")
    ok = r.u32(PE_EDITOR) != 0 and r.u32(MODE) == 2
    r.log("park editor: editing" if ok else "park editor: NOT editing", "editor %#x" % r.u32(PE_EDITOR))
    return ok


def pe_enter(r, theme=1, load=False):
    """Main menu > PARK EDITOR, the first skater, New Park and theme THEME (0-3), or Load
    Saved Park (slot 1, the first park)."""
    pe_main_entry(r, 4)
    pe_skater(r)
    if load:
        pe_menu_cursor(r, 1)
        r.press("CROSS", gap=60)
        r.batch("wait 150")
        pe_load_dialog(r)
    else:
        pe_menu_cursor(r, 0)
        p = r.u32(PANEL)
        r.press("CROSS", gap=60)
        r.new_panel(p)
        r.set_cursor(theme)
        r.log("park editor theme", r.panel_cursor()[1])
        r.press("CROSS", gap=60)
    r.pe_at = 0                       # a new editor starts on the Terrain Tool (StartTool 0); a test run keeps the tool
    return pe_editing(r)


def pe_terrain(r):
    """Terrain Tool: selections raised, lowered, reselected and grown, deleted, dropped."""
    pe_roll(r, 0)
    r.batch("press CROSS 6 40", "hold UP 25", "wait 10", "press CIRCLE 6 40")
    pe_taps(r, "R3")
    r.batch("press CROSS 6 40", "hold DOWN 25", "wait 10", "press CROSS 6 40")
    pe_taps(r, "R1 U1")
    r.batch("hold UP 15", "wait 10", "press CIRCLE 6 40")
    pe_taps(r, "L6")
    r.batch("press CROSS 6 40", "hold UP 50", "wait 10", "press SQUARE 6 40", "press CROSS 6 40", "hold UP 40",
            "wait 10", "press CIRCLE 6 40")
    pe_taps(r, "D3")
    r.batch("press CROSS 6 40", "hold UP 10", "hold DOWN 40", "wait 10", "press CIRCLE 6 40")
    pe_taps(r, "U3 R3")           # back to the first raised block
    r.log("terrain tool done")


def pe_objects(r, variants=2):
    """Object Tool: each of the 13 piece groups, VARIANTS pieces of each (R2), rotated a
    group's number of times, placed along a row; one on the raised block (a tower under
    it), one over another piece (refused), one deleted (Square)."""
    placed = []
    pe_roll(r, 1)
    r.batch("press CROSS 6 60", "wait 20")        # on the first raised block
    placed.append("")
    for g in range(1, 14):
        pe_roll(r, g)
        for v in range(variants):
            if v:
                r.batch("press R2 6 40")
            pe_taps(r, "D3" if v else "L3")
            for _ in range((g + v) % 4):
                r.batch("press CIRCLE 6 30")
            r.batch("press CROSS 6 60", "wait 10")
            if v:
                pe_taps(r, "U3")
        if g == 5:
            r.batch("press CROSS 6 60")             # over the piece just placed: refused
        if g == 9:
            r.batch("press SQUARE 6 60", "wait 10")  # deleted again
            r.batch("press CROSS 6 60", "wait 10")   # and placed again
    r.log("object tool: %d groups placed" % 13)


def pe_rails(r, cells=13):
    """Rail Tool: back along the row of pieces; on each its buttons (X, O, Triangle, Square,
    the d-pad: where a piece takes rails they place, turn, delete them)."""
    pe_roll(r, PE_RAIL)
    for i in range(cells):
        r.batch("press CROSS 6 40", "press CIRCLE 6 30", "press CROSS 6 40", "press TRIANGLE 6 30", "wait 10")
        r.batch("press DOWN 6 20", "press CROSS 6 40", "press UP 6 20", "press SQUARE 6 30", "press CROSS 6 40", "wait 10")
        pe_taps(r, "R3")
    r.log("rail tool done")


PE_RAIL_GROUPS = (4, 5, 8, 9, 10, 11, 12, 13)    # the piece groups whose first piece takes rails ("Add rails to this object")


def pe_rail_row(r, variants=2):
    """Rail Tool on pieces that take rails: each such group's first VARIANTS pieces (R2) placed
    in a row (the Object Tool, 4 cells apart), the Rail Tool at once on it (its help: X Place,
    O Change, Square Delete): rails placed, changed, deleted and placed again."""
    for g in PE_RAIL_GROUPS:
        for v in range(variants):
            pe_roll(r, g)
            for _ in range(v):
                r.batch("press R2 6 40")
            pe_taps(r, "L4")
            r.batch("press CROSS 6 60", "wait 10")
            pe_roll(r, PE_RAIL)
            r.batch("press CROSS 6 40", "press CIRCLE 6 40", "press CROSS 6 40", "press CIRCLE 6 40", "press CIRCLE 6 40",
                    "press CROSS 6 40", "press TRIANGLE 6 30", "press SQUARE 6 40", "press CROSS 6 40", "press SQUARE 6 40",
                    "press SQUARE 6 40", "press CROSS 6 40", "press CIRCLE 6 40", "press CROSS 6 40", "wait 10")
    r.log("rail tool on %d pieces" % (len(PE_RAIL_GROUPS) * variants))


def pe_towers(r):
    """Pieces on raised and lowered terrain (the towers under them) and terrain raised and
    lowered under placed pieces: a block raised, a piece placed on it, raised further and
    lowered under it; a piece placed on flat ground, the ground under it raised and lowered;
    a piece on a pit; each deleted and placed again."""
    for g, up, down in ((1, 30, 0), (4, 50, 20), (8, 0, 30), (11, 70, 60)):
        pe_taps(r, "U4")
        pe_roll(r, 0)
        r.batch("press CROSS 6 40")
        if up:
            r.batch("hold UP %d" % up, "wait 10")
        if down and not up:
            r.batch("hold DOWN %d" % down, "wait 10")
        r.batch("press CIRCLE 6 40")
        pe_roll(r, g)
        r.batch("press CROSS 6 60", "wait 10")            # on the block (or pit)
        pe_roll(r, 0)
        r.batch("press CROSS 6 40", "hold UP 25", "wait 10", "hold DOWN %d" % (down or 15), "wait 10", "press CIRCLE 6 40")
        pe_roll(r, g)
        r.batch("press SQUARE 6 60", "wait 10", "press CIRCLE 6 30", "press CROSS 6 60", "wait 10")
    r.log("towers done")


def pe_start(r):
    """Start Tool: the player's start rotated and placed twice."""
    pe_roll(r, PE_START)
    pe_taps(r, "D2")
    r.batch("press CIRCLE 6 30", "press CROSS 6 60")
    pe_taps(r, "L4")
    r.batch("press CIRCLE 6 30", "press CIRCLE 6 30", "press CROSS 6 60", "wait 20")
    r.log("start tool done")


def pe_camera(r):
    """The camera: turned (left stick) both ways, zoomed (right stick Y) in and out."""
    for axis, v, n in (("LX", 1, 40), ("LX", -1, 60), ("LY", 1, 30), ("LY", -1, 30), ("RY", -1, 30), ("RY", 1, 50),
                       ("RX", 1, 20)):
        r.batch("stick %s %s" % (axis, v), "wait %d" % n, "stick %s 0" % axis, "wait 5")


def pe_pause(r, k):
    """The editor's pause menu, entry K."""
    for _ in range(10):
        if r.u32(MODE) != 3:
            break
        r.batch("press TRIANGLE 6 40")
    r.batch("press START 6 60", "wait 30")
    pe_menu_cursor(r, k)
    r.batch("press CROSS 6 60", "wait 60")


def pe_unpause(r):
    """Close a pause menu left open (TRIANGLE: OnCancel, OnExitPause)."""
    for _ in range(4):
        if r.u32(MODE) != 3:
            return
        r.batch("press TRIANGLE 6 40", "wait 20")


def pe_test_run(r, skate=1):
    """Pause > Test Run: skated (sweep.py's routine), the test run's pause: Sound Options
    (the sliders), Return to Editor."""
    pe_pause(r, 1)
    r.batch("until %#x!=%#x 3000" % (POS + 4, sweep.Y_UNPLACED), "wait 60")
    r.tips()
    r.log("test run: pos", r.f32(POS))
    for i in range(skate):
        r.skate("pe2-test-%d" % i)
    r.batch("press START 6 60", "wait 30")
    pe_menu_cursor(r, 1)
    r.batch("press CROSS 6 60", "wait 30")
    sound_sliders(r)
    r.batch("press TRIANGLE 6 60", "wait 30")
    if r.u32(MODE) != 3:
        r.batch("press START 6 60", "wait 30")
    pe_menu_cursor(r, 2)
    r.batch("press CROSS 6 60")
    ok = pe_editing(r)
    r.log("test run: back in the editor" if ok else "test run: NOT back in the editor")


def pe_save(r, desc="PE", author="AI", row=0):
    """Pause > Save Park: card slot 1, park ROW (NEW, or a park: overwritten), the
    description and author on the keyboard, YES."""
    pe_pause(r, 2)
    r.batch("press CROSS 6 60", "wait 200")       # slot 1
    pe_menu_cursor(r, row)
    r.batch("press CROSS 6 60", "wait 120")
    pe_type(r, desc)
    r.batch("wait 60")
    pe_type(r, author)
    r.batch("wait 120")
    pe_yes(r)
    r.batch("wait 400")
    pe_unpause(r)
    r.log("save park: mode", r.u32(MODE))


def pe_load(r, row=0):
    """Pause > Load Park: YES, card slot 1, park ROW, YES."""
    pe_pause(r, 3)
    pe_yes(r)
    r.batch("wait 120")
    pe_load_dialog(r, row)
    r.batch("wait 600")
    pe_unpause(r)
    r.log("load park: mode", r.u32(MODE))


def pe_clear(r, yes=True):
    pe_pause(r, 4)
    pe_yes(r, yes)
    r.batch("wait 120")
    pe_unpause(r)
    r.log("clear park", "yes" if yes else "no")


def pe_sound(r):
    pe_pause(r, 5)
    r.batch("wait 30")
    sound_sliders(r)
    r.batch("press TRIANGLE 6 60", "wait 30")
    pe_unpause(r)


def pe_exit(r, yes=True):
    """Pause > Exit Editor: NO first (back to the editor), then YES: the main menu."""
    pe_pause(r, 6)
    pe_yes(r, False)
    r.batch("wait 30")
    pe_unpause(r)
    if not yes:
        return
    pe_pause(r, 6)
    pe_yes(r, True)
    r.batch("until 0x276FE4==1 3600", "until 0x240A20!=0 3600", "wait 120")
    r.log("exit editor: mode", r.u32(MODE))


def pe_play(r, skate=2):
    """Main menu > FREE SKATE, the first skater, the Editor Level (level select entry 7):
    Load Park (slot 1, the first park, YES), skated, pause, EXIT."""
    pe_main_entry(r, 1)
    pe_skater(r)
    r.set_cursor(7)
    r.log("free skate: level select entry", r.panel_cursor()[1])
    r.press("CROSS", gap=60)
    r.batch("wait 300")
    pe_load_dialog(r)
    ok = r.wait_placed(6000)
    r.log("editor level placed" if ok else "editor level NOT placed", "level", r.u32(LEVEL))
    for i in range(skate):
        r.skate("pe2-play-%d" % i)
    r.menu()


def pe_tools(r, theme=1):
    """The whole front-end editor: every tool, the camera, the pause menu's entries."""
    if not pe_enter(r, theme):
        return False
    pe_terrain(r)
    pe_objects(r)
    pe_rails(r)
    pe_start(r)
    pe_camera(r)
    pe_test_run(r)
    pe_sound(r)
    pe_clear(r, False)
    pe_save(r)
    pe_clear(r, True)
    pe_load(r)
    pe_test_run(r)
    pe_save(r, "PE", "AI", 1)        # over the park just saved
    pe_exit(r)
    return True


def pe_ingame(r, rounds=3, skate=2):
    """(In a career level whose in-level editor its giver started: Cannery's Pisser Tool,
    Industrial's Crane Tool, Airfield's Vacuum Tool) jumps until the editor runs; ROUNDS of
    every button (pick, move, turn, drop, delete; the shoulder buttons); pause > Do challenge;
    skated (sweep.py's routine). (ZX_GUEST_CALL of activateParkEditor 0x1BFE18 from the
    level's update sets the editor object and then crashes the EE: no route uses it.)"""
    for _ in range(40):
        if r.u32(PE_EDITOR):
            break
        r.batch("press CROSS 6 30", "wait 30")
    r.batch("wait 200")
    r.log("in-level editor: editor %#x mode %d" % (r.u32(PE_EDITOR), r.u32(MODE)))
    if not r.u32(PE_EDITOR):
        return False
    for i in range(rounds):
        r.batch("press CROSS 6 40")
        pe_taps(r, "R2 U2")
        r.batch("press CIRCLE 6 30", "press CROSS 6 40", "wait 10")
        pe_taps(r, "L3 D1")
        r.batch("press CROSS 6 40", "press CIRCLE 6 30", "press TRIANGLE 6 30", "press CROSS 6 40", "wait 10")
        r.batch("press R1 6 40", "press R2 6 40", "press CROSS 6 40", "press L1 6 40", "press L2 6 40", "wait 10")
        pe_taps(r, "D2 R%d" % (i + 1))
        r.batch("press SQUARE 6 40", "press CROSS 6 40", "hold UP 20", "hold LEFT 20", "press CROSS 6 40", "wait 10")
    pe_camera(r)
    pe_unpause(r)
    r.batch("press START 6 60", "wait 30")
    pe_menu_cursor(r, 1)
    r.batch("press CROSS 6 60", "wait 300")
    r.tips()
    r.log("do challenge: editor %#x mode %d pos %s" % (r.u32(PE_EDITOR), r.u32(MODE), r.f32(POS)))
    for i in range(skate):
        r.skate("pe2-ingame-%d" % i)
    return True


PE_COMMANDS = {
    "pe2enter": lambda r, a: pe_enter(r, int(a[0]) if a else 1, len(a) > 1 and a[1] == "load"),
    "pe2terrain": lambda r, a: pe_terrain(r),
    "pe2objects": lambda r, a: pe_objects(r, int(a[0]) if a else 2),
    "pe2rails": lambda r, a: pe_rails(r, int(a[0]) if a else 13),
    "pe2start": lambda r, a: pe_start(r),
    "pe2towers": lambda r, a: pe_towers(r),
    "pe2railrow": lambda r, a: pe_rail_row(r, int(a[0]) if a else 2),
    "pe2camera": lambda r, a: pe_camera(r),
    "pe2test": lambda r, a: pe_test_run(r, int(a[0]) if a else 1),
    "pe2save": lambda r, a: pe_save(r, a[0] if a else "PE", a[1] if len(a) > 1 else "AI", int(a[2]) if len(a) > 2 else 0),
    "pe2load": lambda r, a: pe_load(r, int(a[0]) if a else 0),
    "pe2clear": lambda r, a: pe_clear(r, not (a and a[0] == "no")),
    "pe2sound": lambda r, a: pe_sound(r),
    "pe2exit": lambda r, a: pe_exit(r, not (a and a[0] == "no")),
    "pe2play": lambda r, a: pe_play(r, int(a[0]) if a else 2),
    "pe2tools": lambda r, a: pe_tools(r, int(a[0]) if a else 1),
    "pe2roll": lambda r, a: pe_roll(r, int(a[0])),
    "pe2ingame": lambda r, a: pe_ingame(r, int(a[0]) if a else 3, int(a[1]) if len(a) > 1 else 2),
}


def edge_record(r, node, face):
    """The grind-edge record (FUN_00187140) of a node's face: the collision
    part (Node +0x174 -> +0x10), its edge stream (+0x4C), the face's index
    (u8 or u16 table at +0x10, 0xFF / 0xFFFF none), the records after it."""
    frame = r.u32(node + 0x174) if node else 0
    part = r.u32(frame + 0x10) if frame else 0
    stream = r.u32(part + 0x4C) if part else 0
    if not stream:
        return 0
    head, entries = r.u32(stream + 4), r.u32(stream + 8)
    kind, stride = head & 0xF, (head >> 4) & 0x3FF
    if kind == 0:
        index = r.u32((stream + 0x10 + face) & ~3) >> (8 * ((stream + 0x10 + face) & 3)) & 0xFF
        none, unit = 0xFF, 1
    else:
        a = stream + 0x10 + 2 * face
        index = r.u32(a & ~3) >> (8 * (a & 3)) & 0xFFFF
        none, unit = 0xFFFF, 2
    if index == none:
        return 0
    return stream + 0x10 + ((unit * entries + 15) & ~15) + index * stride


def pole_aim(r, button, dz=0.0, lesson=14, air=False, aim540=False, delay=1, boost=0.0, turn=0.0):
    """Vertical Poles lesson, optionally moved dz across; into the pole
    (UP+CIRCLE, or a jump holding CIRCLE); on the pole (state 9 or 10) UP is
    let go and `button` tapped (the d-pad's aim); the states seen."""
    r.lesson(lesson)
    if dz:
        x, y, z = r.f32(POS)
        r.teleport(x, y, z + dz)
        r.batch("wait 10")
    player = r.player()
    if air:
        r.batch("down UP", "until " + above(POS, -9.0) + " 600", "down CROSS", "wait 3", "up CROSS", "down CIRCLE")
    else:
        r.batch("down UP+CIRCLE")
    deadline = time.time() + 60 * SLOW
    while time.time() < deadline:
        s = int(r.batch("peek %#x" % (player + P_STATE), "wait 1")[0].split()[1], 16) & 0xFF
        if s in (9, 10):
            break
    if boost:
        # Test only: the pole's exit speed (its face's grind-edge record,
        # +0x14; FUN_00187380) poked, so the exit's boost path runs.
        rec = edge_record(r, r.u32(player + 0x214), r.u32(player + 0x210))
        if rec and turn:
            # and its exit heading (+0xC, degrees about up) turned
            heading = r.f32(rec + 0xC, 1)[0]
            r.batch("poke %#x %#x" % (rec + 0xC, f2u(heading + turn)))
            r.log("exit heading", heading, "->", heading + turn)
        if rec:
            r.batch("poke %#x %#x" % (rec + 0x14, f2u(boost)))
            r.log("pole edge record", hex(rec), "exit speed", boost)
    if aim540:
        # Test only: the swing still to make poked to the unaimed 540
        # degrees (Player +0x1D0; only the Horizontal Pole writes it in the
        # game), so the aim (FUN_001A1E80) runs.
        r.batch("poke %#x %#x" % (player + 0x1D0, f2u(540.0)))
    r.batch("up UP", "wait %d" % delay, "press %s 3 3" % button)
    seen = r.watch(200)
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    r.log("poleaim", button, dz, "states:", " ".join("%d %s" % (s, STATES[s]) for s in seen))
    return seen


def grind_x(r, face="fwd", lean=0.0, spin="", launch=0.0, hold=150, word=False):
    """The Grinding lesson's grind (lesson 7: lined up, a jump, TRIANGLE)
    with test-only pokes that force the Grind and Air enters' rarer paths
    (see the command's help)."""
    r.lesson(7)
    r.goto(0, -0.3, lambda x, y, z: x > -16)
    player = r.player()
    r.batch("up LEFT+RIGHT", "down UP+CROSS", "wait 3", "up CROSS")
    pokes = []
    if face == "back":
        # half a turn about up: right and forward negated (the rows at +0xB0, +0xD0)
        rows = r.f32(player + 0xB0, 12)
        for k in (0, 2):
            pokes += ["poke %#x %#x" % (player + 0xB0 + 16 * k + 4 * i, f2u(-rows[4 * k + i])) for i in range(3)]
    if lean:
        pokes.append("poke %#x %#x" % (player + 0x270, f2u(lean)))
    if pokes:
        r.batch(*pokes)
    if spin:
        r.batch("down " + spin.upper())
    r.batch("down TRIANGLE")
    seen = []
    if launch:
        for _ in range(120):
            if r.state(player) == 5:
                break
            r.batch("wait 1")
        seen.append(r.state(player))
        if seen[-1] == 5:
            paused(r, True)
            rec = edge_record(r, r.u32(player + 0x214), r.u32(player + 0x210))
            d = r.f32(player + 0x240, 3)
            n = math.sqrt(d[0] * d[0] + d[2] * d[2]) or 1.0
            tilt = (0.968 * d[0] / n, -0.25, 0.968 * d[2] / n)
            cmds = ["poke %#x %#x" % (player + 0x240 + 4 * i, f2u(v)) for i, v in enumerate(tilt)]
            # the segment's ends moved onto the tilted line, 20 units either side of
            # the skater (else it runs off the short segment and the next one's
            # direction replaces the tilt)
            pos = r.f32(player + P_POSITION, 3)
            cmds += ["poke %#x %#x" % (player + 0x220 + 4 * i, f2u(pos[i] - 20 * tilt[i])) for i in range(3)]
            cmds += ["poke %#x %#x" % (player + 0x230 + 4 * i, f2u(pos[i] + 20 * tilt[i])) for i in range(3)]
            if rec:
                cmds += ["poke %#x 0" % (rec + 0xC), "poke %#x 0" % (rec + 0x10),
                         "poke %#x %#x" % (rec + 0x14, f2u(launch))]
            r.batch(*cmds)
            r.log("grindx: rail edge record %s, direction %s -> %s" % (hex(rec), d, tilt))
            r.batch("poke %#x 0" % (player + 0x79))
            paused(r, False)
            r.batch("poke %#x 0" % (player + 0x79), "press CROSS 3 1")
    if word:
        # on the rail: the rail's event word (Player +0x1F8) poked to another,
        # so the next segment counts as another rail (GrindEnd, GrindStart)
        for _ in range(120):
            if r.state(player) == 5:
                break
            r.batch("wait 1")
        r.batch("poke %#x 0x7fff" % (player + 0x1F8))
        r.log("grindx: rail word poked, state", r.state(player))
    seen += r.watch(hold)
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    seen += r.watch(30)
    r.log("grindx", face, lean, spin, launch, "states:", " ".join("%d %s" % (s, STATES[s]) for s in seen))
    return seen


def grind_tricks(r, start="-", presses=("-", "LEFT", "LEFT", "RIGHT", "DOWN", "UP"), after=("-",), verbose=False):
    """The Grinding lesson's grind started with the d-pad taps START (- none)
    before TRIANGLE, then a grind trick asked for on the rail for each of
    PRESSES (d-pad taps, comma separated, ~B lets go of B, !K (test only) the current trick's kind poked to K, !J every trick unlocked and jizzed, then a fresh TRIANGLE press): with
    a trick current while grinding, the trick input combines the two
    (FUN_001EA138) and drops a trick on top of the same one (FUN_001EA050).
    Then AFTER's presses once the grind is over (the trick still current on
    the ground: FUN_001EA050 only). Logs the state at each press."""
    r.lesson(7)
    r.goto(0, -0.3, lambda x, y, z: x > -16)
    player = r.player()

    def taps(spec):
        cmds = []
        for b in ([] if spec == "-" else spec.split(",")):
            # ~B lets go of a held button (UP, held to roll on, hides taps of it);
            # !K (test only) pokes the current trick's kind (TrickState +4) to K
            if b.startswith("~"):
                cmds += ["up %s" % b[1:].upper()]
            elif b == "!J":
                # (test only) every unlockable trick (Player +0x5F0) and jizzed (+0x8C0 bit 0)
                cmds += ["poke %#x %#x" % (player + 0x5F0, r.u32(player + 0x5F0) | 0xFFFF),
                         "poke %#x %#x" % (player + 0x8C0, r.u32(player + 0x8C0) | 1)]
            elif b.startswith("!"):
                slot = r.u32(player + 0x8A8) if r.u32(player + 0x8AC) else 0
                node = r.u32(slot + 4) if slot else 0
                trick = r.u32(node + 8) if node else 0
                state = r.u32(trick + 4) if trick else 0
                cmds += ["poke %#x %d" % (state + 4, int(b[1:]))] if state else []
            else:
                cmds += ["press %s 2 2" % b.upper()]
        return cmds

    r.batch("up LEFT+RIGHT", "down UP+CROSS", "wait 3", "up CROSS", *taps(start))
    r.batch("down TRIANGLE")
    for _ in range(60):
        if r.state(player) == 5:
            break
        r.batch("wait 1")
    r.batch("up TRIANGLE", "wait 1")
    log = []
    for spec in presses:
        st = r.state(player)
        r.batch(*taps(spec), "press TRIANGLE 2 1")
        log.append("%s@%d" % (spec, st))
        if verbose:
            slot = r.u32(player + 0x8A8) if r.u32(player + 0x8AC) else 0
            node = r.u32(slot + 4) if slot else 0
            trick = r.u32(node + 8) if node else 0
            state = r.u32(trick + 4) if trick else 0
            # Controls::history (Player +0x550 + 0x20), its first four entries
            word = r.u32(player + 0x570)
            hist = " ".join("%02x" % ((word >> (8 * i)) & 0xFF) for i in range(4))
            r.log("  %s: history %s, the current trick's type %s, jizzed %d" % (
                spec, hist, r.u32(state) if state else "-", r.u32(player + 0x8C0) & 1))
    for _ in range(150):
        if r.state(player) in (0, 1):
            break
        r.batch("wait 2")
    r.batch("up UP", "wait 10")
    for spec in after:
        st = r.state(player)
        r.batch(*taps(spec), "press TRIANGLE 2 1", "wait 6")
        log.append("after %s@%d" % (spec, st))
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    r.log("grindtricks start %s: presses (taps@state) %s" % (start, " ".join(log)))


# The three grind combos of the trick table (.data 0x245E50 entries 2 .. 4:
# SF, FM and MS Combo, types 21 .. 23): three d-pad taps each (patterns 01 03
# 03, 01 01 03, 05 05 03, Controls::history newest first: 1 UP, 3 RIGHT,
# 5 DOWN, 7 LEFT) and an unlock bit of Player +0x5F0 (10, 4, 8) with the
# skater jizzed (+0x8C0 bit 0).
COMBO_TAPS = {"sf": "RIGHT,RIGHT,UP", "fm": "RIGHT,UP,UP", "ms": "RIGHT,DOWN,DOWN"}


def combos(r, which=("sf", "fm", "ms"), side="RIGHT"):
    """(for recording; test only: pokes) Each grind combo in WHICH asked for
    on the Grinding lesson's rail: every trick unlocked and the skater
    jizzed (as grindtricks' !J), with Scoring: Juice / Jizzed->Non-Jizzed
    Percent poked to 0 so the jizz is not lost before the press; then the
    combo's three taps (SIDE the side tap: LEFT makes 07, no combo) and
    TRIANGLE: newSfCombo, newFmCombo, newMsCombo (and their combines on the
    current trick). tricks-c's !J tries made none: the jizz went off (below
    the percent) before the press."""
    tuning = mash_tuning(mash_dump(r, "tuning"))
    addr = tuning.get(("Scoring: Juice", "Jizzed->Non-Jizzed Percent"))
    if addr is None:
        r.log("combos: no tuning entry Scoring: Juice / Jizzed->Non-Jizzed Percent")
    else:
        r.batch("poke %#x 0" % addr)
    for w in which:
        taps = COMBO_TAPS[w].replace("RIGHT", side.upper())
        grind_tricks(r, "-", ("~UP,!J," + taps, "!J," + taps), ("-",), verbose=True)
        r.log("combos: %s (%s) asked for twice on the rail" % (w, taps))


HEAD_RAY_LENGTH = 0x246D68 + 35 * 0x30 + 0x20   # Physics / Head Ray Length (0.75; the Tuning page's range 0 to 2)


def grind_wall(r, length=20.0, hold=120):
    """A wall hit while grinding (skaterMoveCollide's grind-to-air branch):
    the Grinding lesson's grind, then (test only) Physics / Head Ray Length
    poked from 0.75 to LENGTH once on the rail, so the head probes (forward,
    and forward to the sides) reach the walls by the rail; the states over
    HOLD vsyncs, then the length put back."""
    r.lesson(7)
    r.goto(0, -0.3, lambda x, y, z: x > -16)
    player = r.player()
    r.batch("up LEFT+RIGHT", "down UP+CROSS", "wait 3", "up CROSS", "down TRIANGLE")
    for _ in range(60):
        if r.state(player) == 5:
            break
        r.batch("wait 1")
    old = r.u32(HEAD_RAY_LENGTH)
    r.batch("poke %#x %#x" % (HEAD_RAY_LENGTH, f2u(length)))
    seen = r.watch(hold)
    r.batch("poke %#x %#x" % (HEAD_RAY_LENGTH, old), "up UP+TRIANGLE")
    r.log("grindwall %.1f: states %s" % (length, " ".join(str(x) for x in seen)))


def stall_jump(r, stick="-", after=20):
    """(covrec) A hand plant (the Hand Plants lesson, as `stall`), then
    AFTER vsyncs in the stall the d-pad STICK held (- none) and CROSS: the
    stall's jump off (stallControls: out across the edge with UP or DOWN,
    along it with LEFT or RIGHT, back into the pipe with none)."""
    r.lesson(12)
    r.batch("down UP", "until " + above(POS, -8.5) + " 600", "up UP", "down TRIANGLE")
    player = r.player()
    for _ in range(150):
        if r.state(player) == 8:
            break
        r.batch("wait 2")
    hold = [] if stick == "-" else ["down " + stick.upper()]
    r.batch("wait %d" % after, *hold, "wait 2", "press CROSS 3 1", "up TRIANGLE")
    seen = r.watch(90)
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    r.log("stalljump %s: states %s" % (stick, " ".join(str(x) for x in seen)))


def grind_jump(r, stick="-", after=20):
    """(covrec) The Grinding lesson's grind, then AFTER vsyncs on the rail the
    d-pad STICK held (- none) and CROSS: grindControls' jump off (sideways
    with LEFT or RIGHT; a short grind counted with none)."""
    r.lesson(7)
    r.goto(0, -0.3, lambda x, y, z: x > -16)
    player = r.player()
    r.batch("up LEFT+RIGHT", "down UP+CROSS", "wait 3", "up CROSS", "down TRIANGLE")
    for _ in range(60):
        if r.state(player) == 5:
            break
        r.batch("wait 1")
    hold = [] if stick == "-" else ["down " + stick.upper()]
    r.batch("up UP+TRIANGLE", "wait %d" % after, *hold, "wait 2", "press CROSS 3 1")
    seen = r.watch(90)
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    r.log("grindjump %s: states %s" % (stick, " ".join(str(x) for x in seen)))


def grind_poke(r, scale=3.0, after=10, hold=90):
    """(covrec; test only: a poke) The Grinding lesson's grind, then AFTER
    vsyncs on the rail the velocity scaled by SCALE: past
    Max Grind Speed (3), stopped (0) or back along the rail (-1);
    grindPhysics' rarer paths (still on the rail, turned round, capped)."""
    r.lesson(7)
    r.goto(0, -0.3, lambda x, y, z: x > -16)
    player = r.player()
    r.batch("up LEFT+RIGHT", "down UP+CROSS", "wait 3", "up CROSS", "down TRIANGLE")
    for _ in range(60):
        if r.state(player) == 5:
            break
        r.batch("wait 1")
    r.batch("up UP+TRIANGLE", "wait %d" % after)
    v = r.f32(player + P_VELOCITY, 3)
    r.batch(*["poke %#x %#x" % (player + P_VELOCITY + 4 * i, f2u(c * scale)) for i, c in enumerate(v)])
    seen = r.watch(hold)
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    r.log("grindpoke %g: velocity %s, states %s" % (scale, " ".join("%.2f" % c for c in v), " ".join(str(x) for x in seen)))


def stall_trick(r, taps="-"):
    """A hand plant (the Hand Plants lesson, as `stall`) with the d-pad taps
    TAPS (comma separated, - none) between letting go of UP and TRIANGLE at
    the lip: the stall's own trick from the stall table (.data 0x2460A8:
    UP Abstract, DOWN Rocket, LEFT Method, RIGHT Stale; none the plain Hand
    Plant; FUN_0018B600)."""
    r.lesson(12)
    presses = [] if taps == "-" else ["press %s 2 2" % b.upper() for b in taps.split(",")]
    r.batch("down UP", "until " + above(POS, -8.5) + " 600", "up UP", *presses, "down TRIANGLE")
    seen = r.watch(90)
    r.batch("up TRIANGLE")
    seen += r.watch(60)
    r.log("stalltrick %s: states %s" % (taps, " ".join(str(x) for x in seen)))


HAZARD_SOUNDS = {5: "water", 12: "fire", 14: "out-of-bounds", 15: "out-of-bounds-hidden"}


def hazards(r, least=2):
    """The level's faces whose surface sound (FUN_001874F0) is a hazard of
    skaterTouchSurface (FUN_00199418), from a RAM dump: the collision
    world's sectors (0x27396C: +0x4C count, +0x50 0x50-byte sectors, +0x40
    a node list) -> each node's collision part (Node +0x174 -> +0x10) -> a
    face's material index (part +0x08 faces: byte +0x10 + 8 face) -> the
    part's material table (part +0x28: a list, its first entry; +0x14 + 4
    index) -> the material's tag record (material +0x10, a tag buffer;
    byte 0 of its record 0). Nodes with LEAST or more such faces, with
    their world box (Node +0xD0 / +0xE0); fewer are mostly stray bytes."""
    path = os.path.join(nctl.ctl_dir(r.name), "hazards.ram")
    r.batch("dump " + path)
    m = open(path, "rb").read()
    u32 = lambda a: struct.unpack_from("<I", m, a & 0x1FFFFFF)[0]
    u16 = lambda a: struct.unpack_from("<H", m, a & 0x1FFFFFF)[0]
    world = u32(0x27396C)
    nodes = set()
    for i in range(u32(world + 0x4C)):
        lst = u32(u32(world + 0x50) + 0x50 * i + 0x40)
        if lst:
            nodes.update(u32(u32(lst) + 4 * k) for k in range(u16(lst + 8)))
    unit = [u32(0x23FEE0 + 4 * k) for k in range(16)]   # a tag buffer's index bytes by its kind
    out = []
    for node in sorted(nodes):
        lods = u32(node + 0x174)
        part = u32(lods + 0x10) if lods else 0
        faces = u32(part + 8) if part else 0
        lst = u32(part + 0x28) if part else 0
        if not faces or not lst or not u16(lst + 8):
            continue
        table, found = u32(u32(lst)), {}
        for f in range(u16(part + 0x36)):
            mat = u32(table + 0x14 + 4 * m[(faces + 0x10 + 8 * f) & 0x1FFFFFF])
            if mat:
                buf = mat + 0x10
                rec = buf + 0x10 + ((unit[u32(buf + 4) & 0xF] * u32(buf + 8) + 15) & ~15)
                sound = m[rec & 0x1FFFFFF]
                if sound in HAZARD_SOUNDS:
                    found[sound] = found.get(sound, 0) + 1
        for sound, n in found.items():
            if n >= least:
                box = struct.unpack_from("<3f4x3f", m, (node + 0xD0) & 0x1FFFFFF)
                r.log("hazards: node %#x %s %d faces, box %s .. %s" % (
                    node, HAZARD_SOUNDS[sound], n, " ".join("%.1f" % v for v in box[:3]),
                    " ".join("%.1f" % v for v in box[3:])))
                out.append((node, HAZARD_SOUNDS[sound], n, box))
    os.remove(path)
    return out


def pickups(r):
    """The level's pickups (game/aggressive/pickups.cpp: the spinning items
    updatePickup runs each frame), from a RAM dump: their 0x90-byte state
    blocks (pickupOf's tag word, FUN_001E3500 makes them), found by shape:
    the box's three end points right after the block (+0x1C = block +
    0x90; pickups without a box, `still`, are not found), +0x38 the mode
    (0 .. 5), +0x18 the object's node (its position at node +0x10).
    [(block, kind, mode, hidden, (x, y, z), spinners, bobbers)]"""
    path = os.path.join(nctl.ctl_dir(r.name), "pickups.ram")
    r.batch("dump " + path)
    m = open(path, "rb").read()
    os.remove(path)
    u32 = lambda a: struct.unpack_from("<I", m, a & 0x1FFFFFF)[0]
    end = min(len(m), 0x2000000) - 0x100
    ok = lambda a: 0x100000 <= a < end
    out = []
    for p in range(0x100000, end, 16):
        if u32(p + 0x1C) != p + 0x90 or u32(p + 0x38) > 5 or not ok(u32(p + 0x18)):
            continue
        at = struct.unpack_from("<3f", m, (u32(p + 0x18) + 0x10) & 0x1FFFFFF)
        bobbers = [u32(p + 8 + 4 * i) for i in range(4)]
        out.append((p, u32(p), u32(p + 0x38), u32(p + 0x24), at, u32(p + 4), bobbers))
        r.log("pickups: %#x kind %#x mode %d hidden %d at %.1f %.1f %.1f spinners %#x bobbers %s" % (
            p, u32(p), u32(p + 0x38), u32(p + 0x24), at[0], at[1], at[2], u32(p + 4),
            " ".join("%#x" % b for b in bobbers)))
    return out


def pickup_run(r, most=40, hold=150, first=0, isolate=False):
    """(for recording) Each of the level's pickups (`pickups`: the levels'
    NPCs; at most MOST, the shown ones first, FIRST skipped) run into:
    paused, the skater put on it (a challenge bubble's NPC reacts so);
    while its mode has not changed 30 vsyncs on, put 3 units off (-x, -z,
    +x) and pushed through it at 8 units a second (test only: the velocity
    poked), as riding into it (FUN_001E3E40: knocked back unless the level
    script answers NPCSkaterCollision). Then its modes over HOLD vsyncs (2
    knocked back, 5 its get-up animation, then home: 1 or 4;
    updatePickup's paths), the skater moved 2 units off halfway (it turns
    to face him). ISOLATE (test only: pokes): the other NPCs' `still`
    (+0x30) set while one is tried (no box or light update: their frames
    make 3 calls), so a recorder with ZX_FNREC_MIN_CALLS=6 keeps only the
    tried NPC's frames, the get-up's end (6 calls) among them."""
    r.tips()
    found = sorted(pickups(r), key=lambda f: f[3])
    taken = 0
    player = r.player()
    for p, kind, mode, hidden, _, spin, bobbers in found[first:first + most]:
        node = r.u32(p + 0x18)
        others = [q for q, *_ in found if q != p and r.u32(q + 0x30) == 0] if isolate else []
        r.batch(*["poke %#x 1" % (q + 0x30) for q in others])
        mode = r.u32(p + 0x38)
        modes = [mode]
        now = mode
        # (NPCs walk: their position read again before each try)
        for dx, dz in ((0.0, 0.0), (-3.0, 0.0), (0.0, -3.0), (3.0, 0.0)):
            x, y, z = r.f32(node + 0x10, 3)
            r.batch("press START 2 20")
            r.teleport(x + dx, y - 0.2, z + dz)
            if dx or dz:
                # (test only) pushed through it at 8 units a second
                r.batch("poke %#x %#x" % (player + P_VELOCITY, f2u(-8.0 * dx / 3.0)),
                        "poke %#x %#x" % (player + P_VELOCITY + 8, f2u(-8.0 * dz / 3.0)))
            r.batch("press START 2 6", "wait 30")
            now = r.u32(p + 0x38)
            if now != mode:
                break
        if now != mode:
            modes.append(now)
        for i in range(max(1, hold // 6)):
            if i == hold // 12:
                r.batch("press START 2 20")
                r.teleport(x + 2.0, y - 0.2, z)
                r.batch("press START 2 6")
            r.batch("wait 6")
            v = r.u32(p + 0x38)
            if v != modes[-1]:
                modes.append(v)
        taken += len(modes) > 1
        r.batch(*["poke %#x 0" % (q + 0x30) for q in others])
        r.tips()
        r.log("pickuprun: %#x kind %#x hidden %d at %.1f %.1f %.1f: modes %s" % (
            p, kind, hidden, x, y, z, " ".join(str(v) for v in modes)))
    r.log("pickuprun: %d of %d pickups changed mode" % (taken, len(found[first:first + most])))

def hazard_run(r, most=8, hold=120):
    """(covrec) The skater dropped, paused, on each of the level's hazard
    nodes (`hazards`: water, fire, out of bounds; at most MOST, the ones with
    most faces first) two units over its box's middle, then the states over
    HOLD vsyncs: skaterTouchSurface's hazard paths (a respawn, a bail)."""
    r.tips()
    found = sorted(hazards(r), key=lambda h: -h[2])
    seen_nodes = set()
    for node, kind, n, box in found:
        if node in seen_nodes or len(seen_nodes) >= most:
            continue
        seen_nodes.add(node)
        cx, cz = (box[0] + box[3]) / 2, (box[2] + box[5]) / 2
        r.batch("press START 2 20")
        r.teleport(cx, min(box[1], box[4]) - 2.0, cz)
        r.batch("press START 2 6", "down UP")
        seen = r.watch(hold)
        r.batch("up UP")
        r.tips()
        r.log("hazardrun: node %#x %s at %.1f %.1f: states %s" % (node, kind, cx, cz, " ".join(str(x) for x in seen)))


def double_grind(r, taps="UP", hold=90, action="", speed=12.0, at=(66.0, -52.6, 1.0, 0.0), pre=()):
    """Double Grind (state 7) in the Museum (career 7): the beam at
    x 63.8 .. 90.6, z -52.0 / -53.2, y -45.42 whose two edges are grind edges
    tagged 5 and 6 (the collision part's edge stream, +8 of each record).
    Paused (START, so the pokes land together), the skater is put at
    (66, -47.5, -52.6), two units over the beam's middle, facing +x, speed
    0; unpaused; CROSS; (test only) the velocity poked to 12 along +x, so
    it rolls along the beam; the d-pad UP three times (the button table
    0x245E50 entry 0: trick type 0x18, kind 6 -> 0x1A0BA8) and TRIANGLE
    held, then let go; the states over 150 vsyncs. With DOWN three times
    (entry 1, type 0x19, kind 5) it is a Hand
    Grind (6) instead."""
    player = r.player()
    r.tips()
    r.batch("press START 2 20")
    x, z, dx, dz = at
    r.teleport(x, -47.5, z)
    r.batch(*["poke %#x %#x" % (player + 0xD0 + 4 * i, f2u(v)) for i, v in enumerate((dx, 0.0, dz))],
            "poke %#x 0" % (player + 0x1E8))
    r.batch("press START 2 6")
    r.batch("press CROSS 3 1", "wait 1", "poke %#x %#x" % (player + P_VELOCITY, f2u(speed * dx)),
            "poke %#x %#x" % (player + P_VELOCITY + 8, f2u(speed * dz)))
    # (pre: commands sent with the taps, before TRIANGLE: doublegrindenter's pokes)
    r.batch("press %s 2 2" % taps, "press %s 2 2" % taps, "press %s 2 2" % taps, *pre, "down TRIANGLE")
    seen = r.watch(hold)
    # then, while on the rails: ACTION (a button held, + for several) for 40
    # vsyncs, or TRIANGLE let go first ("letgo")
    if action == "letgo":
        r.batch("up TRIANGLE")
    elif action.startswith("raw:"):
        # raw nctl commands, "," between them, "_" for spaces
        r.batch(*[c.replace("_", " ") for c in action[4:].split(",")])
        seen += r.watch(60)
    elif action:
        r.batch("down " + action.upper())
        seen += r.watch(40)
        r.batch("up " + action.upper())
    r.batch("up TRIANGLE")
    seen += r.watch(60)
    r.log("doublegrind", taps, hold, action, "states:", " ".join("%d %s" % (s, STATES[s]) for s in seen))
    return seen


def double_grind_enter(r, lean=0.0, shorts=0, spin=False, speed=12.0, z=-52.6, tries=4):
    """(for recording; test only: pokes) A Museum Double Grind (`doublegrind`)
    with doubleGrindEnter's inputs set just before TRIANGLE: the balance's
    lean (Player +0x274) to LEAN (past Grind Balance Best Reset it is eased
    back; under Grind Balance Dead the speed's parity picks the drift's
    way), the short grind count (+0x280) to SHORTS (past Grind Balance Short
    Dead: no delay) and L1 held through the enter with SPIN (the spin
    paths), the velocity SPEED, put over the beam at Z (-52.6, its middle;
    -52.8 or -52.4 for either side of the rails' middle); up to TRIES
    tries until it is a Double Grind (7: every other try is a Grind)."""
    player = r.player()
    pre = ["poke %#x %#x" % (player + 0x274, f2u(lean)), "poke %#x %d" % (player + 0x280, shorts & 0xFFFFFFFF)]
    pre += ["down L1"] if spin else []
    for k in range(tries):
        seen = double_grind(r, "UP", 60, "", speed, at=(66.0, z, 1.0, 0.0), pre=tuple(pre))
        r.batch("up L1")
        if 7 in seen:
            break
    r.log("doublegrindenter lean %g shorts %d spin %d z %g: try %d, states %s" % (
        lean, shorts, spin, z, k + 1, " ".join(str(x) for x in seen)))


CAR_POSITION = -0xEC              # a traffic car's position from its name ("car_Taxi01" ...)
P_FLAGS, P_CRASHES = 0x80, 0x618  # Player flags (0x2 down) and crash count


def cars(r, prefix=b"car_"):
    """The traffic cars of the level (their names' addresses, from a RAM dump):
    the Movie Lot's car_Taxi01 .. 06 (cardef_Taxi), the Tutorial's golf cart."""
    import re
    path = os.path.join(os.environ.get("ROUTES_DIR", "/tmp/routes"), r.name, "cars.ram")
    r.batch("dump " + path)
    data = open(path, "rb").read()
    os.remove(path)
    return [m.start() for m in re.finditer(re.escape(prefix) + rb"[A-Za-z0-9]+\0", data)
            if 0x100 < m.start() and data[m.start() - 1] != 0x5F]


def traffic(r, tries=10, push=16.0, lead=0.25):
    """A traffic hit (the crash "Stop playing in traffic!", kind 10): the
    cars' motion measured over 10 vsyncs; paused (START, so the pokes land
    together), the skater is put `lead` seconds ahead of the fastest car on
    its path, facing it; unpaused, (test only) the velocity poked to `push`
    toward the car (FUN_00199E60 knocks the skater down when a CAR object
    hits it faster than 40 units a second, relative). Until the crash
    count grows, at most `tries` cars."""
    player = r.player()
    r.tips()
    names = cars(r)
    r.log("traffic: %d cars" % len(names))
    crashes = r.u32(player + P_CRASHES)
    for attempt in range(tries):
        # every car's position twice, 10 vsyncs apart, in one batch (the
        # level frame counter says how many frames passed)
        got = r.batch(*(["peek %#x" % FRAME] + ["peekf %#x 3" % (n + CAR_POSITION) for n in names] + ["wait 10"] +
                        ["peek %#x" % FRAME] + ["peekf %#x 3" % (n + CAR_POSITION) for n in names]))
        k = len(names)
        frames = max(1, int(got[k + 2].split()[1], 16) - int(got[0].split()[1], 16))
        moving = []
        for i, n in enumerate(names):
            p = [float(c) for c in got[1 + i].split()[1:]]
            q = [float(c) for c in got[k + 3 + i].split()[1:]]
            v = [(q[j] - p[j]) * 60.0 / frames for j in range(3)]
            moving.append((math.hypot(v[0], v[2]), n, q, v))
        speed, n, q, v = max(moving)  # the fastest (cars waiting at a corner stand still)
        if speed < 20:
            r.log("traffic: no car moving fast (%.1f)" % speed)
            r.batch("wait 60")
            continue
        dx, dz = v[0] / speed, v[2] / speed
        x, z = q[0] + v[0] * lead, q[2] + v[2] * lead
        r.batch("press START 2 20")
        r.teleport(x, q[1] - 1.0, z)
        r.batch(*["poke %#x %#x" % (player + 0xD0 + 4 * i, f2u(c)) for i, c in enumerate((-dx, 0.0, -dz))])
        r.batch("press START 2 6", "wait 2", "poke %#x %#x" % (player + P_VELOCITY, f2u(-push * dx)),
                "poke %#x %#x" % (player + P_VELOCITY + 8, f2u(-push * dz)))
        r.log("traffic: car %#x at %.1f %.1f %.1f, %.1f a second; skater at %.1f %.1f" %
              (n, q[0], q[1], q[2], speed, x, z))
        for _ in range(60):
            got = r.batch("peek %#x" % (player + P_CRASHES), "peek %#x" % (player + P_FLAGS), "wait 1")
            if int(got[0].split()[1], 16) != crashes:
                shot = os.path.join(os.environ.get("ROUTES_DIR", "/tmp/routes"), r.name, "shots", "traffic.png")
                r.batch("wait 6", "shot " + shot)
                r.log("traffic: hit (crash count %d, flags %s), %s" % (int(got[0].split()[1], 16), got[1].split()[1],
                                                                         shot))
                r.batch("wait 150")
                return True
        r.log("traffic: missed")
        crashes = r.u32(player + P_CRASHES)
    return False


DEATH_BY_JUICE = 0x246248      # General / Death by Juice's value (the tuning table)
VISIBLE_TIMER = 0x277CCC       # the HUD's countdown timer (gp-0x2C24), 0 when none
KELLY = (60.9, -1.0, -121.7)   # next to Kelly (Movie Lot career): "Get 40,000 points in 60 seconds"
P_PENDING, P_PENDING_ON, P_SCORE = 0x8C8, 0x8CC, 0x8C4  # the trick score to bank (float), its flag; -> score +0x14


def challenge(r, finish="complete"):
    """A career challenge played to its end: the Movie Lot's Kelly (career 1
    first; her script: talking to her starts a 60 second timer, the HUD's
    countdown and challenge score; 40,000 points scored before it runs out
    complete it, else it fails). Test only: Death by Juice off (a poke of
    the tuning value), so the juice does not end the run meanwhile. Talks to
    her (CIRCLE) until the countdown shows; then with `complete` (test only)
    a pending trick score of 41,000 is poked (her handler wants more than
    40,000) and a jump lands it, so her skaterPointsChanged handler completes the
    challenge; with `fail` the timer runs out (TimerFinished, then
    isSkaterNotPullingAnyTricks). Waits for the countdown to go."""
    player = r.player()
    r.tips()
    r.batch("poke %#x 0" % DEATH_BY_JUICE)
    for _ in range(5):
        r.teleport(*KELLY)
        r.batch("wait 30")
        x, _, z = r.f32(POS)
        if abs(x - KELLY[0]) < 1 and abs(z - KELLY[2]) < 1:
            break
        r.log("challenge: placed at %.1f %.1f, again" % (x, z))
    for _ in range(6):
        r.batch("press CIRCLE 3 40")
        if r.u32(VISIBLE_TIMER):
            break
    timer = r.u32(VISIBLE_TIMER)
    score = r.u32(player + P_SCORE)
    r.log("challenge: countdown %#x, score %d" % (timer, r.u32(score + 0x14)))
    if not timer:
        return False
    if finish == "complete":
        r.batch("poke %#x %#x" % (player + P_PENDING, f2u(41000.0)), "poke %#x 1" % (player + P_PENDING_ON),
                "down UP", "wait 20", "press CROSS 3 10", "wait 60", "up UP")
    for i in range(80):
        if not r.u32(VISIBLE_TIMER):
            break
        r.batch("wait 60")
    shot = os.path.join(os.environ.get("ROUTES_DIR", "/tmp/routes"), r.name, "shots", "challenge.png")
    r.batch("shot " + shot)
    r.log("challenge %s: countdown %s after %d s, score %d, points %d, %s" %
          (finish, "gone" if not r.u32(VISIBLE_TIMER) else "still up", i, r.u32(score + 0x14),
           r.u32(player + offsetof_points), shot))
    return not r.u32(VISIBLE_TIMER)


offsetof_points = 0x8D8           # Player::points, the challenge score (setChallengeScore)


SPINE_LIP_Z = -153.69     # the Movie Lot spine's lip (edge x -33.1 .. -28.6, y -3.35)


def paused(r, want):
    """START until the game mode is 3 (paused) or 2 (skating), as wanted."""
    mode = 3 if want else 2
    for _ in range(6):
        if r.u32(MODE) == mode:
            return True
        r.batch("press START 6 2", "until %#x==%d 40" % (MODE, mode))
    r.log("paused(%s): mode still %d" % (want, r.u32(MODE)))
    return False


def spine(r, side="+", speed=14.0, x=-31.0, hold=150, jump=0.0, turn=""):
    """Air Spine (state 4) in the Movie Lot (freeskate 0): the spine whose
    lip (a lip edge of two faces of surface kind 2, FUN_001873F8) runs along
    x from -33.1 to -28.6 at z -153.69, 3.35 above the floor. Leaving the
    ground on a kind-2 face, steep (up against the world's up under
    gp 0x272DC8) and heading up it, gives Air Spine (FUN_00198550); its
    enter (0x19DDD0) sends the skater over the lip. Paused, the skater is
    put on the floor 11.3 units off the lip on `side` facing it, upright,
    at rest; unpaused; (test only) the velocity poked to `speed` toward the
    lip and UP held, so it rolls up the face and over. TURN (gamefix): a
    button held from 4 Air Spine on (L1 or R1: airSpineTurn's turn about
    up; a d-pad side does not turn it)."""
    if r.u32(LEVEL) != 5:
        r.log("spine: not in the Movie Lot (level %d); freeskate 0 first" % r.u32(LEVEL))
        return []
    player = r.player()
    dz = -1.0 if side == "+" else 1.0
    r.tips()
    # On the ground first: the air states over a pipe hold the skater in
    # the pipe's plane, which would undo the teleport.
    for _ in range(100):
        if r.state(player) in (0, 1):
            break
        r.batch("wait 4")
    z0 = SPINE_LIP_Z - 11.3 * dz
    for attempt in range(4):
        # Paused (closed loop on the game mode: 3 paused, 2 skating) while
        # the pokes land, as they take a vsync each.
        paused(r, True)
        r.teleport(x, -0.3, z0)
        # rows right, up, forward (right = forward x up)
        rows = ((dz, 0.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, dz))
        r.batch(*["poke %#x %#x" % (player + 0xB0 + 16 * k + 4 * i, f2u(v))
                  for k, row in enumerate(rows) for i, v in enumerate(row)], "poke %#x 0" % (player + 0x1E8))
        paused(r, False)
        for _ in range(60):       # standing again after the drop onto the floor
            if r.state(player) in (0, 1):
                break
            r.batch("wait 2")
        px, _, pz = r.f32(POS)
        if abs(px - x) < 2.0 and abs(pz - z0) < 2.0:
            break
        r.log("spine: the teleport did not hold (at %.2f %.2f); again" % (px, pz))
    r.batch("wait 2", "poke %#x %#x" % (player + P_VELOCITY + 8, f2u(speed * dz)), "down UP")
    if jump:
        # an ollie `jump` units before the lip (CROSS down, let go 3 vsyncs later)
        near = below(POS + 8, SPINE_LIP_Z + jump) if dz < 0 else above(POS + 8, SPINE_LIP_Z - jump)
        r.batch("until %s 300" % near, "down CROSS", "wait 3", "up CROSS")
    seen = []
    if turn:
        for _ in range(100):
            if r.state(player) == 4:
                break
            r.batch("wait 1")
        r.batch("down " + turn.upper())
    seen += r.watch(hold)
    r.batch("up UP" + ("+" + turn.upper() if turn else ""))
    seen += r.watch(30)
    r.log("spine", side, speed, x, turn, "states:", " ".join("%d %s" % (s, STATES[s]) for s in seen))
    return seen


def ride(r, x, y, z, dx, dz, speed=14.0, jump=0.0, hold=150):
    """Rides at a ramp, pipe or spine and leaves it (readable-game9: the
    take-offs off faces with launch values, docs/DEVTOOLS.md). Paused, the
    skater is put at (x, y, z) facing (dx, 0, dz), upright, at rest; once
    standing (unpaused), (test only) the velocity is poked to `speed` along
    the heading and UP held; `jump`: an ollie (CROSS) after that many units
    along the heading (0: none); the states over `hold` vsyncs."""
    player = r.player()
    n = math.hypot(dx, dz)
    dx, dz = dx / n, dz / n
    r.tips()
    for _ in range(100):
        if r.state(player) in (0, 1):
            break
        r.batch("wait 4")
    for attempt in range(4):
        paused(r, True)
        r.teleport(x, y, z)
        # rows right, up, forward (right = forward x up, y down)
        rows = ((dz, 0.0, -dx), (0.0, -1.0, 0.0), (dx, 0.0, dz))
        r.batch(*["poke %#x %#x" % (player + 0xB0 + 16 * k + 4 * i, f2u(v))
                  for k, row in enumerate(rows) for i, v in enumerate(row)], "poke %#x 0" % (player + 0x1E8))
        paused(r, False)
        for _ in range(60):
            if r.state(player) in (0, 1):
                break
            r.batch("wait 2")
        px, _, pz = r.f32(POS)
        if abs(px - x) < 2.0 and abs(pz - z) < 2.0:
            break
        r.log("ride: the teleport did not hold (at %.2f %.2f); again" % (px, pz))
    r.batch("wait 2", "poke %#x %#x" % (player + P_VELOCITY, f2u(speed * dx)),
            "poke %#x %#x" % (player + P_VELOCITY + 8, f2u(speed * dz)), "down UP")
    if jump:
        # the ollie once `jump` units along the heading (on its main axis)
        if abs(dx) >= abs(dz):
            near = above(POS, x + jump * dx) if dx > 0 else below(POS, x + jump * dx)
        else:
            near = above(POS + 8, z + jump * dz) if dz > 0 else below(POS + 8, z + jump * dz)
        r.batch("until %s 300" % near, "down CROSS", "wait 3", "up CROSS")
    seen = r.watch(hold)
    r.batch("up UP")
    seen += r.watch(30)
    r.log("ride", x, y, z, dx, dz, speed, jump, "states:", " ".join("%d %s" % (s, STATES[s]) for s in seen))
    return seen


# ---- mash: seeded play that keeps going (recording, coverage) ---------------
#
# One level load (career K, freeskate K, ...), then seeded input that plays
# sensibly from the skater's state: tricks only with the air time or the
# rail for them, grabs and spins let go before the landing, grind, manual
# and stall balance steered to the middle. A bail, a stop or a stay in a
# small area teleports the skater to another of the level's own spots (its
# spawn points, NPCs, the start, and the approaches to rails, pipes, poles
# and walls the run has seen). Actions and spots are weighted toward the
# states whose callbacks reach functions the work database lists as
# uncovered. docs/DEVTOOLS.md "mash".

MASH_BUTTONS = "UP DOWN LEFT RIGHT CROSS CIRCLE TRIANGLE SQUARE L1 R1 L2 R2".split()
TUNING_CATEGORIES = 0x24AD28      # {entries, count, name[32]} 0x28 bytes, ended by "None"
STATE_TABLE = 0x24B468            # skater.h StateRow[18] (0x34 bytes: name, enter, exit, update[5], request)
POINT_RESPAWN, POINT_PLAYERONE = 0x06A846EE, 0x06DA4725   # zge::nameHash of the data points' names
POINT_SPAWN, POINT_NPC = 0x46BE, 0x5343                    # their class hashes (low 16 bits)
GROUNDED, AIRBORNE, RAILS = (0, 1), (2, 3, 4), (5, 6, 7)
P_BALANCE, P_FORWARD, P_SPEED = 0x270, 0xD0, 0x1E8
# Spots learned on the way into these states (the approach a moment before).
MASH_TARGETS = (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17)
# Named tweaks: the game's cheats (what their codes write, BRINGUP.md
# "Cheats page"), the juice kept up in a career, the tricks unlocked.
# (category, entry, value) tuning writes; "pokes" are Player fields.
MASH_TWEAKS = {
    "juice": [("General", "Death by Juice", 0.0), ("Scoring: Juice", "Max Juice Degradation/sec", -0.2)],
    "invulnerable": [("Scoring: Juice", "Juice Lost/Level on crash", 0.0)],
    "superspin": [("Physics", "Air Rotate", 640.0)],
    "lowgravitywall": [("Grind/Walls", "Wall Ride Gravity", 3.0)],
    # Perfect Grind, Perfect Handplant, Perfect Manual (the cheats' writes): the
    # balance math still runs, with no drift, so balance never ends a move
    "perfectgrind": [("Grind/Walls", "Grind Balance Factor", 0.0), ("Grind/Walls", "Grind Balance Init", 0.0)],
    "perfecthandplant": [("Grind/Walls", "Stall Balance Factor", 0.0), ("Grind/Walls", "Stall Balance Init", 0.0)],
    "perfectmanual": [("Grind/Walls", "Manual Balance Factor", 0.0), ("Grind/Walls", "Manual Balance Init", 0.0)],
    "tricks": "pokes",            # every unlockable trick (Player +0x5F0) and jizzed (+0x8C0 bit 0), as grindtricks' !J
    "push": "teleports",          # a teleport gives the skater the approach's velocity back (else it stands)
}
MASH_DEFAULT_TWEAKS = ("juice", "push")


def mash_dump(r, tag):
    path = os.path.join(os.environ.get("ROUTES_DIR", "/tmp/routes"), r.name, tag + ".ram")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    r.batch("dump " + path)
    data = open(path, "rb").read()
    os.remove(path)
    return data


def mash_tuning(m):
    """The tuning table from a RAM image: {(category, entry): value address}."""
    u32 = lambda a: struct.unpack_from("<I", m, a & 0x1FFFFFF)[0]
    name = lambda a: m[a & 0x1FFFFFF:(a & 0x1FFFFFF) + 32].split(b"\0")[0].decode("latin-1")
    out = {}
    for k in range(40):
        cat = TUNING_CATEGORIES + 0x28 * k
        cname = name(cat + 8)
        if cname == "None" or not cname:
            break
        entries, count = u32(cat), u32(cat + 4)
        for i in range(min(count, 200)):
            out[(cname, name(entries + 0x30 * i))] = entries + 0x30 * i + 0x20
    return out


def mash_points(m):
    """The level's data points as loaded (0x40-byte records on a list whose
    node is at +0x10: class +0x00 (u16), name hash +0x04, position +0x20,
    a turn about up as a quaternion +0x30), found from a "respawn" point."""
    u32 = lambda a: struct.unpack_from("<I", m, a & 0x1FFFFFF)[0]
    start = None
    i = m.find(struct.pack("<I", POINT_RESPAWN), 0x100000)
    while i >= 0 and start is None:
        rec = i - 4
        nxt = u32(rec + 0x10)
        if rec % 16 == 0 and 0x100000 < nxt < 0x2000000 and u32(nxt + 4) == rec + 0x10:
            start = rec
        i = m.find(struct.pack("<I", POINT_RESPAWN), i + 4)
    points, rec, seen = [], start, set()
    while rec is not None and rec not in seen and len(seen) < 5000:
        seen.add(rec)
        cls, h = u32(rec) & 0xFFFF, u32(rec + 4)
        pos = struct.unpack_from("<3f", m, (rec + 0x20) & 0x1FFFFFF)
        q = struct.unpack_from("<4f", m, (rec + 0x30) & 0x1FFFFFF)
        if all(abs(v) < 1e5 for v in pos):
            yaw = 2 * math.atan2(q[1], q[3])
            points.append((cls, h, pos, (math.sin(yaw), math.cos(yaw))))
        nxt = u32(rec + 0x10)
        rec = nxt - 0x10 if 0x100000 < nxt < 0x2000000 else None
    return points


def mash_coverage(path, statuses):
    """{address: uncovered blocks} of the functions of the statuses in a
    `fncheck_corpus.py coverage --json` file (the recordings' path coverage)."""
    import json
    rows = json.load(open(path))
    return {int(x["addr"], 16): len(x["uncovered"]) for x in rows
            if x.get("game", "ai") == "ai" and x["status"] in statuses and x["uncovered"]}


def mash_bias(m, log, coverage=None, statuses=("Verified",)):
    """Per skater state, the work database's uncovered game functions its
    callbacks reach (the call graph, depth 8) that Ground's and Stand's do
    not, less those a profile has seen (play1, and MASH_REACHED: profiles of
    earlier runs, comma separated); weights 1 .. 5 from it. With `coverage`
    (MASH_COVERAGE or coverage=FILE: fncheck_corpus.py coverage --json), the
    uncovered blocks of the functions of `statuses` (Verified) that each
    state's callbacks reach instead: the recordings' paths still missing."""
    u32 = lambda a: struct.unpack_from("<I", m, a & 0x1FFFFFF)[0]
    workdb = os.environ.get("WORKDB_PY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "dm2", "tools", "workdb.py"))
    weights = {s: 1.0 for s in range(len(STATES))}
    if not os.path.exists(workdb):
        log("mash: no work database (%s): weights all 1" % workdb)
        return weights, {}
    import json
    import subprocess

    def q(sql):
        p = subprocess.run([sys.executable, "-I", workdb, "query", sql, "--json"], capture_output=True, text=True)
        try:
            return json.loads(p.stdout)
        except ValueError:
            return []
    blocks = None
    if coverage:
        try:
            blocks = mash_coverage(coverage, statuses)
        except (OSError, ValueError, KeyError) as e:
            log("mash: coverage file %s not read (%s): the work database's uncovered functions" % (coverage, e))
    uncovered = {int(x["addr"]) for x in q("SELECT addr FROM uncovered WHERE layer='game'")} if blocks is None else set(blocks)
    reached = {int(x["addr"]) for x in q("SELECT addr FROM profile WHERE calls > 0")} if blocks is None else set()
    for path in filter(None, os.environ.get("MASH_REACHED", "").split(",")):
        try:
            for line in open(path):
                a, _, rest = line.partition(",")
                if a.startswith("0x") and int(rest.split(",")[0] or 0) > 0:
                    reached.add(int(a, 16))
        except OSError:
            log("mash: MASH_REACHED %s not read" % path)
    want = uncovered - reached
    calls = {}
    for x in q("SELECT DISTINCT caller, callee FROM calls WHERE build='ai_ps2'"):
        calls.setdefault(int(x["caller"]), set()).add(int(x["callee"]))

    def reach(roots):
        seen, edge = set(roots), set(roots)
        for _ in range(8):
            edge = {c for f in edge for c in calls.get(f, ())} - seen
            seen |= edge
        return seen
    rows = {}
    for s in range(len(STATES)):
        row = STATE_TABLE + 0x34 * s
        rows[s] = [u32(row + o) for o in (0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C) if u32(row + o)]
    common = reach(rows[0] + rows[1])
    if blocks is not None:
        # the uncovered blocks of the functions each state reaches
        count = {s: sum(blocks[f] for f in (reach(rows[s]) - (common if s > 1 else set())) & want) for s in rows}
    else:
        count = {s: len((reach(rows[s]) - (common if s > 1 else set())) & want) for s in rows}
    top = max(count.values()) or 1
    for s in rows:
        weights[s] = 1.0 + 4.0 * count[s] / top
    if blocks is not None:
        log("mash: bias from path coverage (%s): %d %s functions with %d uncovered blocks; per state %s"
            % (coverage, len(blocks), "/".join(statuses), sum(blocks.values()),
               " ".join("%s=%d" % (STATES[s], count[s]) for s in rows)))
    else:
        log("mash: bias from the work database: %d uncovered game functions, %d of them not in a profile; per state %s"
            % (len(uncovered), len(want), " ".join("%s=%d" % (STATES[s], count[s]) for s in rows)))
    return weights, count


class Mash:
    """One mash run on the level that is loaded (see the command's help)."""

    def __init__(self, r, frames, seed, opts):
        import random
        self.r, self.frames, self.seed, self.opts = r, frames, seed, opts
        self.rng = random.Random(seed)
        self.wait = int(opts.get("step", 2))
        self.wall = float(opts.get("secs", os.environ.get("MASH_SECS", 540)))
        tw = opts.get("tweaks")
        # style=natural: the perfect-balance cheats on too (lines hold), unless tweaks= says otherwise
        default = MASH_DEFAULT_TWEAKS + (("perfectgrind", "perfecthandplant", "perfectmanual", "invulnerable")
                                         if opts.get("style") == "natural" else ())
        self.tweaks = [t for t in (tw.split(",") if tw is not None else default) if t and t != "none"]
        self.teleports_on = opts.get("teleport", "1") != "0"
        self.held = set()
        self.stats = {"bails": 0, "teleports": 0, "attempts": 0, "landed": 0, "entries": {}, "frames": {},
                      "why": {}, "tricks": {}}
        self.spots, self.samples, self.window = [], [], []
        base = os.path.join(os.environ.get("ROUTES_DIR", "/tmp/routes"), r.name)
        os.makedirs(base, exist_ok=True)
        self.logfile = open(os.path.join(base, "mash-%s.jsonl" % seed), "a")
        # The recorder's [fncheck:newblocks] lines (ZX_FNREC_NEW_BLOCKS=1):
        # each credits the spot the skater was last put on and the state it
        # is in, so spots that led to new paths are visited again.
        self.runner_log = os.path.join(nctl.LOGS, r.name + ".log")
        try:
            self.runner_log_at = os.path.getsize(self.runner_log)
        except OSError:
            self.runner_log_at = None
        self.spot = None
        self.stats["newblocks"] = {}

    # ---- logging ----------------------------------------------------------

    def event(self, kind, **kw):
        import json
        kw.update(kind=kind, frame=getattr(self, "frame", None))
        self.logfile.write(json.dumps(kw) + "\n")
        self.logfile.flush()

    # ---- setting up ---------------------------------------------------------

    def setup(self):
        r = self.r
        r.tips()
        self.p = r.player()
        self.level = r.u32(LEVEL)
        m = mash_dump(r, "mash")
        self.tuning = mash_tuning(m)
        self.points = mash_points(m)
        self.weights, self.counts = mash_bias(m, r.log, self.opts.get("coverage", os.environ.get("MASH_COVERAGE")),
                                              tuple(self.opts.get("covstatus", "Verified").split(",")))
        # style=natural: play as a person would (lines, few teleports), no
        # coverage bias: for profiling live play
        self.natural = self.opts.get("style") == "natural"
        if self.opts.get("bias") == "0" or self.natural:
            self.weights = {s: 1.0 for s in self.weights}
        del m
        x, y, z = r.f32(self.p + P_POSITION, 3)
        fx, _, fz = r.f32(self.p + P_FORWARD, 3)
        self.spots.append(dict(kind="start", pos=(x, y, z), dir=(fx, fz), vel=None, held=[], uses=0, hits=0))
        for cls, h, pos, d in self.points:
            if cls == POINT_SPAWN:
                self.spots.append(dict(kind="spawn", pos=pos, dir=d, vel=None, held=[], uses=0, hits=0))
            elif cls == POINT_NPC:
                # at the NPC's point (CIRCLE there talks: a goal's giver starts its goal)
                self.spots.append(dict(kind="npc", pos=(pos[0] - 0.5 * d[0], pos[1], pos[2] - 0.5 * d[1]), dir=d,
                                       vel=None, held=[], uses=0, hits=0))
        self.spots_file = None
        if os.environ.get("MASH_SPOTS"):
            import json
            os.makedirs(os.environ["MASH_SPOTS"], exist_ok=True)
            self.spots_file = os.path.join(os.environ["MASH_SPOTS"], "spots-level%d.json" % self.level)
            try:
                learned = json.load(open(self.spots_file))
                for s in learned:
                    s.update(uses=0, hits=0)
                self.spots += learned
                r.log("mash: %d learned spots from %s" % (len(learned), self.spots_file))
            except (OSError, ValueError):
                pass
        self.apply_tweaks()
        self.event("setup", seed=self.seed, level=self.level, frames=self.frames, tweaks=self.tweaks,
                   weights={STATES[s]: round(w, 2) for s, w in self.weights.items()},
                   spots=[(s["kind"], [round(v, 2) for v in s["pos"]]) for s in self.spots], opts=self.opts)
        r.log("mash: seed %s level %d, %d frames; %d spots (%s); tweaks %s" % (
            self.seed, self.level, self.frames, len(self.spots),
            ", ".join("%d %s" % (sum(1 for s in self.spots if s["kind"] == k), k)
                      for k in sorted({s["kind"] for s in self.spots})), ",".join(self.tweaks) or "none"))

    def apply_tweaks(self):
        r, cmds = self.r, []
        for t in self.tweaks:
            what = MASH_TWEAKS.get(t)
            if what is None and "=" in t:            # Category/Entry=value: any tuning entry
                key, _, v = t.partition("=")
                what = [tuple(key.replace("_", " ").split("/", 1)) + (float(v),)]
            if isinstance(what, list):
                for cat, entry, v in what:
                    addr = self.tuning.get((cat, entry))
                    if addr is None:
                        r.log("mash: tweak %s: no tuning entry %s / %s" % (t, cat, entry))
                        continue
                    old = r.f32(addr, 1)[0]
                    cmds.append("poke %#x %#x" % (addr, f2u(v)))
                    self.event("tweak", name=t, entry="%s / %s" % (cat, entry), address=hex(addr), old=old, new=v)
                    r.log("mash: tweak %s: %s / %s %g -> %g" % (t, cat, entry, old, v))
            elif what == "pokes" and t == "tricks":
                p = self.p
                cmds += ["poke %#x %#x" % (p + 0x5F0, r.u32(p + 0x5F0) | 0xFFFF),
                         "poke %#x %#x" % (p + 0x8C0, r.u32(p + 0x8C0) | 1)]
                self.event("tweak", name=t, pokes=["Player +0x5F0 |= 0xFFFF", "Player +0x8C0 |= 1"])
            elif what is None:
                r.log("mash: unknown tweak " + t)
        if cmds:
            r.batch(*cmds)

    # ---- sensing and input --------------------------------------------------

    def step(self, want, extra=()):
        r, p = self.r, self.p
        cmds = []
        if self.held - want:
            cmds.append("up " + "+".join(sorted(self.held - want)))
        if want - self.held:
            cmds.append("down " + "+".join(sorted(want - self.held)))
        self.held = set(want)
        cmds += list(extra)
        cmds += ["wait %d" % self.wait, "peek %#x 4" % (p + P_STATE), "peekf %#x 8" % (p + P_POSITION),
                 "peekf %#x 4" % (p + P_BALANCE), "peekf %#x 3" % (p + P_FORWARD), "peek %#x" % (p + P_CRASHES),
                 "peek %#x" % FRAME, "peek %#x" % MODE]
        rep = r.batch(*cmds)[-7:]
        w = [int(v, 16) for v in rep[0].split()[1:]]
        f = [float(v) for v in rep[1].split()[1:]]
        s = dict(state=w[0] & 0xFF, flags=w[3], pos=f[0:3], vel=f[4:7], bal=[float(v) for v in rep[2].split()[1:]],
                 fwd=[float(v) for v in rep[3].split()[1:]], crashes=int(rep[4].split()[1], 16),
                 frame=int(rep[5].split()[1], 16), mode=int(rep[6].split()[1], 16))
        s["speed"] = math.sqrt(sum(v * v for v in s["vel"]))
        s["held"] = set(self.held)
        return s

    def release(self):
        return self.step(set())

    def new_blocks(self, s):
        """New lines of the runner's log: the recorder's new blocks since the
        last look, credited to the current spot and state."""
        if self.runner_log_at is None:
            return
        try:
            with open(self.runner_log, "rb") as f:
                f.seek(self.runner_log_at)
                data = f.read()
        except OSError:
            return
        end = data.rfind(b"\n") + 1
        self.runner_log_at += end
        added = 0
        for line in data[:end].split(b"\n"):
            if line.startswith(b"[fncheck:newblocks]"):
                kv = dict(w.split(b"=", 1) for w in line.split()[1:] if b"=" in w)
                n = int(kv.get(b"added", b"0"))
                added += n
                name = kv.get(b"name", b"?").decode("latin-1")
                self.event("newblocks", function=name, added=n, state=sname(s["state"]),
                           spot=self.spot["kind"] if self.spot else None)
        if added:
            k = sname(s["state"])
            self.stats["newblocks"][k] = self.stats["newblocks"].get(k, 0) + added
            if self.spot is not None:
                self.spot["new"] = self.spot.get("new", 0) + added

    # ---- teleports ----------------------------------------------------------

    def choose_spot(self):
        def weight(s):
            k = s["kind"]
            # NPCs give the career's goals (their scripts' natives); states by the bias
            w = self.weights.get(STATES.index(k), 1.0) if k in STATES else {"npc": 3.0, "start": 0.5}.get(k, 1.0)
            if s["hits"]:
                w *= 2.0
            if s.get("new"):
                w *= 1.0 + min(4.0, s["new"] / 4.0)   # led to new paths of the recorded functions
            if k in STATES and s["uses"] >= 3 and not s["hits"]:
                w *= 0.1                       # never led to its state: rarely again
            return w / (1.0 + 0.5 * s["uses"])
        ws = [weight(s) for s in self.spots]
        return self.rng.choices(self.spots, ws)[0]

    def teleport(self, why, s, kinds=None):
        r, p = self.r, self.p
        self.stats["why"][why] = self.stats["why"].get(why, 0) + 1
        if not self.teleports_on:
            return s
        pool = [x for x in self.spots if kinds and x["kind"] in kinds]
        spot = self.rng.choice(pool) if pool else self.choose_spot()
        spot["uses"] += 1
        self.spot = spot
        x, y, z = spot["pos"]
        dx, dz = spot["dir"]
        n = math.hypot(dx, dz) or 1.0
        dx, dz = dx / n, dz / n
        self.release()
        paused(r, True)
        r.teleport(x, y - (0.0 if spot["vel"] else 0.5), z)
        rows = ((dz, 0.0, -dx), (0.0, -1.0, 0.0), (dx, 0.0, dz))
        r.batch(*["poke %#x %#x" % (p + 0xB0 + 16 * k + 4 * i, f2u(v)) for k, row in enumerate(rows)
                  for i, v in enumerate(row)], "poke %#x 0" % (p + P_SPEED))
        paused(r, False)
        extra = []
        if spot["vel"] and "push" in self.tweaks:
            # the approach's velocity, no faster than the top speed's 20
            v = spot["vel"]
            k = min(1.0, 20.0 / (math.sqrt(sum(c * c for c in v)) or 1.0))
            extra = ["poke %#x %#x" % (p + P_VELOCITY + 4 * i, f2u(c * k)) for i, c in enumerate(v)]
        self.stats["teleports"] += 1
        self.last_teleport = s["frame"]
        self.pending = dict(spot=spot, until=None)
        self.window = []
        self.event("teleport", why=why, spot=spot["kind"], to=[round(v, 2) for v in spot["pos"]],
                   held=spot["held"], frm=[round(v, 2) for v in s["pos"]])
        if why == "stall":
            # up the quarter pipe the approach led to, TRIANGLE held into its lip
            s = self.step({"UP", "TRIANGLE"}, extra)
        elif spot["kind"] == "npc":
            # standing by an NPC: CIRCLE talks (a goal's giver: its box, which CROSS accepts)
            s = self.step(set(), extra + ["wait 10", "press CIRCLE 4 30"])
        else:
            s = self.step(set(spot["held"]) & {"UP", "TRIANGLE", "CIRCLE", "CROSS"} or {"UP"}, extra)
        self.pending["until"] = s["frame"] + 150
        return s

    def learn(self, state, s):
        """Entering a target state: the approach (from about 30 frames back)
        becomes a spot of that kind, unless one is near it already."""
        back = [x for x in self.samples if s["frame"] - x["frame"] >= 24 and x["state"] in GROUNDED + AIRBORNE]
        if not back:
            return
        a = back[-1]
        kind = STATES[state]
        for sp in self.spots:
            if sp["kind"] == kind and math.dist(sp["pos"], a["pos"]) < 3.0:
                return
        if sum(1 for sp in self.spots if sp["kind"] == kind) >= 16:
            return
        v = a["vel"]
        n = math.hypot(v[0], v[2])
        d = (v[0] / n, v[2] / n) if n > 0.5 else (a["fwd"][0], a["fwd"][2])
        spot = dict(kind=kind, pos=tuple(a["pos"]), dir=d, vel=tuple(v), held=sorted(a["held"]), uses=0, hits=1)
        self.spots.append(spot)
        self.event("spot", spot=kind, pos=[round(x, 2) for x in a["pos"]], vel=[round(x, 2) for x in v],
                   held=spot["held"])

    # ---- the air ------------------------------------------------------------

    def landing_time(self, s):
        """Seconds to the take-off height (y grows downward) at the gravity
        measured so far; 0 once below it and falling."""
        d = self.air["y0"] - s["pos"][1]
        vy, g = s["vel"][1], self.gravity
        if d <= 0:
            return 0.0 if vy > 0 else 0.1
        return (-vy + math.sqrt(max(0.0, vy * vy + 2 * g * d))) / g

    def air_plan(self, state):
        w = self.weights
        rail = (w[5] + w[6] + w[7]) / 3
        options = [("grab", 3.0), ("spin", 3.0), ("grabspin", 1.5), ("flip", 1.5), ("grind", 2.0 * rail),
                   ("pole", (w[9] + w[10] + w[11]) / 3), ("wall", w[12]), ("none", 1.0)]
        if state == 3:
            options += [("stall", 2.0 * w[8])]
        if state in (3, 4):
            options += [("grab", 2.0), ("spin", 2.0)]
        options += [("doublegrind", w[7] * 0.5), ("handgrind", w[6] * 0.5)]
        return self.rng.choices([o for o, _ in options], [x for _, x in options])[0]

    # ---- the run --------------------------------------------------------------

    def run(self):
        r, rng = self.r, self.rng
        self.setup()
        t0 = time.time()
        s = self.step({"UP"})
        f0 = self.frame = s["frame"]
        last = s
        self.gravity = 51.0
        rot = self.tuning.get(("Physics", "Air Rotate"))
        self.air_rotate = max(60.0, r.f32(rot, 1)[0]) if rot else 320.0
        self.air = None
        self.rail = None
        self.pending = None
        crashes = s["crashes"]
        self.last_teleport = f0
        visit = int(self.opts.get("visit", 3600 if self.natural else 1200))
        stall_until = 0
        climb = None   # riding up a ramp: TRIANGLE held into its lip (a stall), or not
        line = None    # natural: the next move of a line (a manual after a landing, a grind after a manual)
        steer, steer_until = None, 0
        action, action_until, cooldown = None, 0, f0 + 30
        down_since, slow_since = None, None
        grind_sign, manual_sign = 1.0, 1.0
        while True:
            self.frame = s["frame"]
            elapsed = s["frame"] - f0
            if elapsed >= self.frames or time.time() - t0 > self.wall:
                break
            st = s["state"]
            self.steps = getattr(self, "steps", 0) + 1
            if self.steps % 30 == 0:
                self.new_blocks(s)
            self.stats["frames"][st] = self.stats["frames"].get(st, 0) + (s["frame"] - last["frame"])
            sample = dict(frame=s["frame"], state=st, pos=s["pos"], vel=s["vel"], fwd=s["fwd"], held=set(self.held))
            self.samples = self.samples[-40:] + [sample]
            if s["mode"] == 1:
                r.log("mash: the front end (mode 1): stopped")
                break
            if s["mode"] == 3:
                # a message box (an NPC, a goal): CROSS closes it (accepts)
                s = self.step(set(), ["press CROSS 4 20"])
                last = s
                continue
            if s["mode"] != 2:
                s = self.step(set())
                last = s
                continue

            # state changes
            if st != last["state"]:
                self.stats["entries"][st] = self.stats["entries"].get(st, 0) + 1
                self.event("state", state=sname(st), pos=[round(v, 2) for v in s["pos"]])
                if st in MASH_TARGETS:
                    self.learn(st, s)
                if self.pending and sname(st) == self.pending["spot"]["kind"]:
                    self.pending["spot"]["hits"] += 1
                    self.pending = None
                if st in AIRBORNE and last["state"] not in AIRBORNE:
                    y0 = last["pos"][1]
                    self.air = dict(y0=y0, plan=action if action in ("grind", "wall", "pole") else None,
                                    vy=s["vel"][1], frame=s["frame"], spin=None, grab=None, released=False,
                                    started=False)
                    action = None
                if st in GROUNDED and last["state"] in AIRBORNE + RAILS + (8, 12, 14):
                    if s["crashes"] == crashes:
                        self.stats["landed"] += 1
                    air = self.air or {}
                    self.event("land", frm=sname(last["state"]), plan=air.get("plan"),
                               yaw=round(math.degrees(air.get("yaw", 0.0))), crashed=s["crashes"] != crashes)
                    # a line: the landing rolls into a manual
                    if s["crashes"] == crashes and last["state"] in AIRBORNE and \
                            rng.random() < (0.3 if self.natural else 0.25):
                        line = "manual"
                if st in RAILS + (8, 9, 10, 11, 12, 14, 17):
                    # a stall's balance drifts faster the longer it is held: off within a second
                    self.rail = dict(frame=s["frame"], leave=s["frame"] + (rng.randint(20, 60) if st == 8 else rng.randint(30, 90) if st == 14
                                                       else rng.randint(30, 180)))
                    self.stats["attempts"] += 1
                    self.stats["tricks"][sname(st)] = self.stats["tricks"].get(sname(st), 0) + 1
            if self.pending and self.pending["until"] and s["frame"] > self.pending["until"]:
                self.pending = None

            # a bail: wait until up again, then elsewhere
            if s["crashes"] != crashes:
                self.stats["bails"] += 1
                air = self.air or {}
                self.event("bail", state=sname(last["state"]), plan=air.get("plan"), pos=[round(v, 2) for v in s["pos"]],
                           yaw=round(math.degrees(air.get("yaw", 0.0))), vel=[round(v, 2) for v in s["vel"]],
                           held=sorted(last.get("held", ())), since=s["frame"] - air.get("frame", s["frame"]))
                crashes = s["crashes"]
                down_since = s["frame"]
            if s["flags"] & P_FLAG_DOWN:
                down_since = down_since or s["frame"]
                last = s
                s = self.step(set())
                continue
            if down_since is not None:
                down_since = None
                last = s
                s = self.teleport("bail", s)
                continue

            # stopped, or stuck in a small area
            if st in GROUNDED and s["speed"] < 1.0:
                slow_since = slow_since or s["frame"]
                if s["frame"] - slow_since > 120:
                    slow_since = None
                    last = s
                    s = self.teleport("stop", s)
                    continue
            else:
                slow_since = None
            if not self.window or s["frame"] - self.window[-1][0] >= 30:
                self.window = [w for w in self.window if s["frame"] - w[0] <= 420] + [(s["frame"], s["pos"])]
                if len(self.window) >= 12 and s["frame"] - self.window[0][0] >= 360:
                    xs = [w[1][0] for w in self.window]
                    zs = [w[1][2] for w in self.window]
                    if max(xs) - min(xs) < 5.0 and max(zs) - min(zs) < 5.0:
                        last = s
                        s = self.teleport("stuck", s)
                        continue

            # every `visit` frames on the ground: another spot (the level
            # covered, not only where the skater happens to roll)
            if visit and st in GROUNDED and s["frame"] - self.last_teleport > visit:
                last = s
                s = self.teleport("visit", s)
                continue

            want, extra = set(), []
            if st in GROUNDED:
                self.air = None
                want.add("UP")
                # up a ramp (y grows downward): TRIANGLE held into its lip
                # stalls on the coping (or grinds it)
                if s["frame"] < stall_until:
                    want.add("TRIANGLE")
                    steer = None
                elif s["vel"][1] < -4.0:
                    if climb is None:
                        climb = rng.random() < (0.6 if self.natural else 0.4 * self.weights[8])
                        if climb:
                            self.event("action", action="stall")
                    if climb:
                        want.add("TRIANGLE")
                        steer = None
                elif s["vel"][1] > -1.0:
                    climb = None
                if line == "manual":
                    # Up-Down-Up (a nose manual Down-Up-Down): three quick
                    # taps with UP let go first (held, it hides the taps)
                    line = None
                    want.discard("UP")
                    want.discard("TRIANGLE")
                    seq = ("UP", "DOWN", "UP") if rng.random() < 0.7 else ("DOWN", "UP", "DOWN")
                    extra = ["press %s 2 2" % b for b in seq]
                    action, action_until, cooldown = "manual", s["frame"] + 6, s["frame"] + 30
                    self.event("action", action="manual", taps="".join(b[0] for b in seq), line=True)
                    last = s
                    s = self.step(want, extra)
                    continue
                if s["frame"] >= steer_until:
                    steer = rng.choice((None, None, "LEFT", "RIGHT"))
                    steer_until = s["frame"] + rng.randint(10, 70)
                if steer:
                    want.add(steer)
                if action and s["frame"] < action_until:
                    want |= ACTION_HOLD.get(action, set())
                elif s["frame"] >= cooldown and rng.random() < 0.08:
                    w = self.weights
                    options = [("jump", 5.0), ("grind", 2.0 * (w[5] + w[6] + w[7]) / 3), ("manual", 1.5 * w[14]),
                               ("cess", w[15]), ("circle", (w[9] + w[16] + w[17]) / 3 + 0.5),
                               ("wall", w[12]), ("pole", (w[9] + w[10] + w[11]) / 3), ("brake", 0.5)]
                    if self.natural:
                        # a player's mix: jumps and grinds, manuals, a wall now and then
                        options = [("jump", 5.0), ("grind", 3.0), ("manual", 1.5), ("cess", 0.5), ("circle", 0.5),
                                   ("wall", 1.0), ("pole", 0.3), ("brake", 0.2)]
                    # a stall: to a quarter pipe the run has seen (its approach), TRIANGLE into the lip
                    if any(x["kind"] in ("AirQPipe", "Stall") for x in self.spots):
                        options.append(("stall", 1.0 if self.natural else 1.0 * w[8]))
                    action = rng.choices([o for o, _ in options], [x for _, x in options])[0]
                    if action == "stall":
                        self.event("action", action="stall", how="teleport")
                        action = None
                        last = s
                        s = self.teleport("stall", s, kinds=("Stall", "AirQPipe"))
                        stall_until = s["frame"] + 90
                        continue
                    action_until = s["frame"] + {"cess": rng.randint(20, 50), "circle": 40, "brake": 20}.get(action, 4)
                    cooldown = s["frame"] + rng.randint(20, 60)
                    if action in ("jump", "grind", "wall", "pole"):
                        extra = ["press CROSS 3 1"]
                    elif action == "manual":
                        line = "manual"
                    want |= ACTION_HOLD.get(action, set())
                    self.event("action", action=action)
            elif st in AIRBORNE:
                if self.air is None:
                    self.air = dict(y0=s["pos"][1], plan=None, vy=s["vel"][1], frame=s["frame"], spin=None, grab=None,
                                    released=False, started=False)
                a = self.air
                dt = (s["frame"] - last["frame"]) / 60.0
                if last["state"] in AIRBORNE and dt > 0:
                    g = (s["vel"][1] - last["vel"][1]) / dt
                    if 20.0 < g < 150.0:
                        self.gravity = 0.8 * self.gravity + 0.2 * g
                t = self.landing_time(s)
                if a["plan"] is None:
                    a["plan"] = self.air_plan(st)
                    a["started"] = False
                    self.event("air", plan=a["plan"], t=round(t, 2), state=sname(st))
                plan = a["plan"]
                if plan in ("grab", "grabspin", "flip") and not a["started"] and t > (0.9 if plan == "flip" else 0.9 if st == 3 else 0.65):
                    a["started"] = True
                    a["grab"] = rng.choice(("UP", "DOWN", "LEFT", "RIGHT", ""))
                    self.stats["attempts"] += 1
                    self.stats["tricks"][plan] = self.stats["tricks"].get(plan, 0) + 1
                    if plan == "flip":
                        taps = [rng.choice(("UP", "DOWN", "LEFT", "RIGHT")) for _ in range(rng.randint(1, 2))]
                        extra = ["press %s 2 1" % b for b in taps] + ["press SQUARE 2 1"]
                        a["grab"] = None
                # the turn about up so far (a spin lands on a half turn)
                yaw = math.atan2(s["fwd"][0], s["fwd"][2])
                if "yawlast" in a:
                    a["yaw"] = a.get("yaw", 0.0) + ((yaw - a["yawlast"] + math.pi) % (2 * math.pi) - math.pi)
                a["yawlast"] = yaw
                # a half turn takes 180 / Air Rotate seconds (and the let-go)
                if plan in ("spin", "grabspin") and a["spin"] is None and t > 180.0 / self.air_rotate + 0.3:
                    a["spin"] = rng.choice(("L1", "R1"))
                    if plan == "spin":
                        self.stats["attempts"] += 1
                        self.stats["tricks"]["spin"] = self.stats["tricks"].get("spin", 0) + 1
                if a["started"] and a["grab"] is not None and plan in ("grab", "grabspin") and t > (0.65 if st == 3 else 0.45):
                    want |= {"SQUARE"} | ({a["grab"]} if a["grab"] else set())
                if a["spin"] and not a["released"]:
                    # let go on a half turn (within 25 degrees) once the
                    # landing is near; at the last moment whatever it is
                    off = abs((math.degrees(a.get("yaw", 0.0)) + 90.0) % 180.0 - 90.0)
                    if t > 0.25 or (t > 0.08 and off > 25.0):
                        want.add(a["spin"])
                    else:
                        a["released"] = True
                # TRIANGLE from the top of the jump (a rail or a lip on the
                # way down), let go just before the take-off height: held
                # into a landing it bails
                if plan in ("grind", "stall") and s["vel"][1] > -3.0 and t > 0.1:
                    want.add("TRIANGLE")
                if plan == "pole":
                    want.add("CIRCLE")
                if plan == "wall" and not a["started"] and s["vel"][1] > -3.0:
                    # DOWN with CROSS held starts a wall ride by a wall
                    a["started"] = True
                    extra = ["press DOWN 6 1"]
                if plan == "wall" and a["started"] and t > 0.05:
                    want.add("CROSS")
                if plan in ("doublegrind", "handgrind") and not a["started"] and s["vel"][1] > -4.0:
                    a["started"] = True
                    b = "UP" if plan == "doublegrind" else "DOWN"
                    extra = ["press %s 2 2" % b] * 3
                if plan in ("doublegrind", "handgrind") and a["started"] and t > 0.1:
                    want.add("TRIANGLE")
            elif st in RAILS:
                bal, drift = s["bal"][0], s["bal"][2]
                # steer the balance to the middle: the drift wanted is
                # against the balance; the stick across adds to the drift
                target = -0.05 * bal
                # a vsync's tap across, as for the manual (held through a
                # step it overshoots)
                if drift > target + 0.004:
                    extra = ["press %s 1 1" % ("LEFT" if grind_sign > 0 else "RIGHT")]
                elif drift < target - 0.004:
                    extra = ["press %s 1 1" % ("RIGHT" if grind_sign > 0 else "LEFT")]
                if (last["state"] == st and last["bal"][2] != drift and len(self.held & {"LEFT", "RIGHT"}) == 1
                        and abs(drift - last["bal"][2]) > 1e-5):
                    pushed = 1.0 if "RIGHT" in self.held else -1.0
                    self.sign_votes = getattr(self, "sign_votes", 0) + (1 if (drift - last["bal"][2]) * pushed > 0 else -1)
                    if self.sign_votes < -6:
                        grind_sign, self.sign_votes = -grind_sign, 0
                        self.event("calibrated", what="grind stick", sign=grind_sign)
                rail = self.rail or dict(frame=s["frame"], leave=s["frame"] + 90)
                if s["frame"] > rail["leave"]:
                    extra = ["press CROSS 3 1"]
                    self.air = dict(y0=s["pos"][1], plan=None, vy=0.0, frame=s["frame"], spin=None, grab=None,
                                    released=False, started=False)
                    if rng.random() < 0.3:
                        want = {rng.choice(("LEFT", "RIGHT"))}
                elif rng.random() < 0.06 * (self.weights[5] + self.weights[6] + self.weights[7]) / 3:
                    taps = [rng.choice(("UP", "DOWN", "LEFT", "RIGHT")) for _ in range(rng.randint(0, 2))]
                    extra = ["press %s 2 2" % b for b in taps] + ["press TRIANGLE 2 1"]
                    self.stats["tricks"]["grindtrick"] = self.stats["tricks"].get("grindtrick", 0) + 1
            elif st in (8, 14):
                bal, speed = s["bal"][1], s["bal"][3]
                target = -0.05 * bal
                # the stick forward (UP: stickY < 0) takes from the balance's
                # speed, DOWN adds; held for a step (2 vsyncs) it moves the
                # speed by about 0.04 and overshoots (measured in the Movie
                # Lot), so a vsync's tap when the speed is 0.01 off
                tol = 0.005 if st == 8 else 0.01
                if speed > target + tol:
                    extra = ["press %s 1 1" % ("UP" if manual_sign > 0 else "DOWN")]
                elif speed < target - tol:
                    extra = ["press %s 1 1" % ("DOWN" if manual_sign > 0 else "UP")]
                rail = self.rail or dict(frame=s["frame"], leave=s["frame"] + 90)
                if s["frame"] > rail["leave"]:
                    extra = ["press CROSS 3 1"]
                    want = set() if st == 8 and rng.random() < 0.5 else want
                    if not self.natural and rng.random() < 0.6:
                        # the jump off with the stick one way: the exits' paths
                        # (out across the edge, along it, back into the pipe)
                        want = {rng.choice(("UP", "DOWN", "LEFT", "RIGHT"))}
                    self.air = None
                    # a line: out of a manual onto a rail (TRIANGLE in the air)
                    if st == 14 and rng.random() < (0.6 if self.natural else 0.3):
                        action = "grind"
            elif st in (9, 10, 11, 17):
                rail = self.rail or dict(frame=s["frame"], leave=s["frame"] + 90)
                want.add("CIRCLE")
                if s["frame"] > rail["leave"]:
                    want.discard("CIRCLE")
                    extra = ["press CROSS 3 1"] if st != 17 else []
                elif rng.random() < 0.05:
                    extra = ["press %s 2 2" % rng.choice(("UP", "DOWN", "LEFT", "RIGHT"))]
            elif st == 12:
                rail = self.rail or dict(frame=s["frame"], leave=s["frame"] + 30)
                if s["frame"] < rail["frame"] + min(40, rail["leave"] - rail["frame"]):
                    want.add("CROSS")
            elif st == 15:
                want |= self.held & {"R2", "L2"}
            last = s
            s = self.step(want, extra)
        self.release()
        self.finish(s, f0, time.time() - t0)

    def finish(self, s, f0, secs):
        import json
        st = self.stats
        frames = max(1, s["frame"] - f0)
        minutes = frames / 3600.0
        ended = max(1, st["landed"] + st["bails"])
        reached = sorted(st["entries"])
        summary = dict(seed=self.seed, level=self.level, frames=frames, seconds=round(secs),
                       bails=st["bails"], attempts=st["attempts"], landed=st["landed"],
                       bail_rate=round(st["bails"] / ended, 3), bails_per_minute=round(st["bails"] / minutes, 2),
                       teleports=st["teleports"], teleport_why=st["why"], tricks=st["tricks"],
                       states={STATES[k]: v for k, v in sorted(st["entries"].items()) if k < len(STATES)},
                       state_frames={STATES[k]: v for k, v in sorted(st["frames"].items()) if k < len(STATES)},
                       spots={k: sum(1 for x in self.spots if x["kind"] == k) for k in sorted({x["kind"] for x in self.spots})},
                       tweaks=self.tweaks, newblocks=st["newblocks"],
                       spots_new=sorted(([x["kind"], [round(v, 1) for v in x["pos"]], x["new"]]
                                         for x in self.spots if x.get("new")), key=lambda t: -t[2])[:12])
        self.event("summary", **summary)
        self.r.log("mash summary: " + json.dumps(summary))
        self.r.log("mash: states reached: " + " ".join("%d %s" % (k, STATES[k]) for k in reached if k < len(STATES)))
        if self.spots_file:
            learned = [dict(x, uses=0, hits=0) for x in self.spots if x["kind"] in STATES]
            for x in learned:
                x.pop("new", None)
            json.dump(learned, open(self.spots_file, "w"))


P_FLAG_DOWN = 0x2


def sname(state):
    return STATES[state] if 0 <= state < len(STATES) else str(state)
# Buttons an action holds on the ground while it runs.
ACTION_HOLD = {"cess": {"R2"}, "circle": {"UP", "CIRCLE"}, "brake": {"DOWN"}}


def mash(r, args):
    """mash [FRAMES [SEED [KEY=VAL ...]]]: see the command's help."""
    frames = int(args[0]) if args else 10800
    seed = int(args[1]) if len(args) > 1 else 1
    opts = dict(a.split("=", 1) for a in args[2:] if "=" in a)
    Mash(r, frames, seed, opts).run()


def run(r, argv):
    cmd, args = argv[0], argv[1:]
    if cmd in PE_COMMANDS:                     # paths2: the Park Editor (pe2*)
        return PE_COMMANDS[cmd](r, args)
    if cmd == "xp":                            # STAT POINTS (test only): a stat's experience points poked
        player = r.player()
        r.batch("poke %#x %d" % (player + 0x638 + 12 * int(args[0]) + 4, int(args[1])))
        r.log("xp: stat %s points %s" % (args[0], args[1]))
        return
    if cmd == "score":                         # N (test only): the skater's score poked (a rollover past 999,999,999)
        player = r.player()
        record = r.u32(player + P_SCORE)
        r.batch("poke %#x %d" % (record + 0x14, int(args[0])))
        if len(args) > 1:
            # a pending trick score poked and a jump landed: banked on landing
            r.batch("poke %#x %#x" % (player + P_PENDING, f2u(float(args[1]))), "poke %#x 1" % (player + P_PENDING_ON),
                    "down UP", "wait 20", "press CROSS 3 10", "wait 60", "up UP")
            r.batch("wait 240")  # "Rollover!" shows after a random delay, for 3 seconds
        # the score, the rollovers (Player +0x614) and the level's count of them
        # (Player +0x6A4 + 0x24 * level, the level at 0x24C9B4)
        level = r.u32(0x24C9B4)
        r.log("score: %d rollovers %d level %d rollovers %d" % (
            r.u32(record + 0x14), r.u32(player + 0x614), level, r.u32(player + 0x6A4 + 0x24 * level)))
        return
    if cmd == "grindwall":                     # [LENGTH [HOLD]] (test only) a wall hit while grinding: Head Ray Length poked
        grind_wall(r, float(args[0]) if args else 20.0, int(args[1]) if len(args) > 1 else 120)
        return
    if cmd == "stalltrick":                    # [B,B...] a hand plant with its d-pad taps (the stall trick table)
        stall_trick(r, args[0] if args else "-")
        return
    if cmd == "hazards":                       # [LEAST] the level's water, fire and out-of-bounds faces
        hazards(r, int(args[0]) if args else 2)
        return
    if cmd == "combos":                        # [sf,fm,ms [SIDE]] the grind combos asked for on the rail (gamefix, test only)
        combos(r, tuple(args[0].split(",")) if args else ("sf", "fm", "ms"), args[1] if len(args) > 1 else "RIGHT")
        return
    if cmd == "doublegrindenter":              # [LEAN [SHORTS [SPIN [SPEED]]]] (gamefix, test only: pokes) in the Museum
        double_grind_enter(r, float(args[0]) if args else 0.0, int(args[1]) if len(args) > 1 else 0,
                           len(args) > 2 and args[2] not in ("0", "-"), float(args[3]) if len(args) > 3 else 12.0,
                           float(args[4]) if len(args) > 4 else -52.6)
        return
    if cmd == "pickups":                       # the level's pickups (their state blocks, from a RAM dump)
        pickups(r)
        return
    if cmd == "pickuprun":                     # [MOST [HOLD]] each pickup taken, its modes watched (gamefix)
        pickup_run(r, int(args[0]) if args else 40, int(args[1]) if len(args) > 1 else 150,
                   int(args[2]) if len(args) > 2 else 0, len(args) > 3 and args[3] == "isolate")
        return
    if cmd == "hazardrun":                     # [MOST [HOLD]] dropped on each hazard node (covrec)
        hazard_run(r, int(args[0]) if args else 8, int(args[1]) if len(args) > 1 else 120)
        return
    if cmd == "grindpoke":                     # [SCALE [AFTER [HOLD]]] the lesson's grind, its velocity scaled (covrec, test only)
        grind_poke(r, float(args[0]) if args else 3.0, int(args[1]) if len(args) > 1 else 10,
                   int(args[2]) if len(args) > 2 else 90)
        return
    if cmd == "stalljump":                     # [STICK [AFTER]] a hand plant jumped off with the stick held (covrec)
        stall_jump(r, args[0] if args else "-", int(args[1]) if len(args) > 1 else 20)
        return
    if cmd == "grindjump":                     # [STICK [AFTER]] the lesson's grind jumped off with the stick held (covrec)
        grind_jump(r, args[0] if args else "-", int(args[1]) if len(args) > 1 else 20)
        return
    if cmd == "grindtricks":                   # [START [PRESS,PRESS... [AFTER,...]]] grind tricks asked for on the rail
        grind_tricks(r, args[0] if args else "-",
                     tuple(args[1].split("/")) if len(args) > 1 else ("-", "LEFT", "LEFT", "RIGHT", "DOWN", "UP"),
                     tuple(args[2].split("/")) if len(args) > 2 else ("-",))
        return
    if cmd == "challenge":                     # [complete|fail] in the Movie Lot's career (career 1)
        challenge(r, args[0] if args else "complete")
        return
    if cmd == "traffic":                       # [TRIES [PUSH [LEAD]]] in a level with traffic (career 1)
        traffic(r, int(args[0]) if args else 10, float(args[1]) if len(args) > 1 else 16.0,
                float(args[2]) if len(args) > 2 else 0.25)
        return
    if cmd == "doublegrindat":                 # X Z DX DZ [HOLD [ACTION [SPEED]]] on another Museum double rail
        at = tuple(float(a) for a in args[:4])
        double_grind(r, "UP", int(args[4]) if len(args) > 4 else 90, args[5] if len(args) > 5 else "",
                     float(args[6]) if len(args) > 6 else 12.0, at)
        return
    if cmd == "ride":                          # X Y Z DX DZ [SPEED [JUMP [HOLD]]] (test only: the velocity poked)
        v = [float(a) for a in args]
        ride(r, v[0], v[1], v[2], v[3], v[4], v[5] if len(v) > 5 else 14.0, v[6] if len(v) > 6 else 0.0,
             int(v[7]) if len(v) > 7 else 150)
        return
    if cmd == "spine":
        spine(r, args[0] if args else "+", float(args[1]) if len(args) > 1 else 14.0,
              float(args[2]) if len(args) > 2 else -31.0, int(args[3]) if len(args) > 3 else 150,
              float(args[4]) if len(args) > 4 else 0.0, args[5] if len(args) > 5 and args[5] != "-" else "")
        return
    if cmd == "spines":
        n, speed = int(args[0]) if args else 4, float(args[1]) if len(args) > 1 else 14.0
        x, jump = float(args[2]) if len(args) > 2 else -31.0, float(args[3]) if len(args) > 3 else 0.0
        hits = 0
        for i in range(n):
            hits += 4 in spine(r, "+-"[i % 2], speed, x, 150, jump)
        r.log("spines: %d of %d reached 4 Air Spine" % (hits, n))
        return
    if cmd == "grindx":                        # [FACE [LEAN [SPIN [LAUNCH [HOLD [word]]]]]] (test only: pokes)
        grind_x(r, args[0] if args else "fwd", float(args[1]) if len(args) > 1 else 0.0,
                args[2] if len(args) > 2 and args[2] != "-" else "", float(args[3]) if len(args) > 3 else 0.0,
                int(args[4]) if len(args) > 4 else 150, len(args) > 5 and args[5] == "word")
        return
    if cmd == "doublegrind":
        double_grind(r, args[0] if args else "UP", int(args[1]) if len(args) > 1 else 90,
                     args[2] if len(args) > 2 else "", float(args[3]) if len(args) > 3 else 12.0)
        return
    if cmd in ("poleaim", "poleaima", "poleaim540", "poleaima540"):
        pole_aim(r, args[0], float(args[1]) if len(args) > 1 else 0.0, air="aima" in cmd, aim540=cmd.endswith("540"),
                 delay=int(args[2]) if len(args) > 2 else 1, boost=float(args[3]) if len(args) > 3 else 0.0,
                 turn=float(args[4]) if len(args) > 4 else 0.0)
    elif cmd == "boot":
        r.boot()
    elif cmd == "tutorial":
        r.career(0)
    elif cmd == "lesson":
        r.lesson(int(args[0]))
    elif cmd in MOVES or cmd == "stalldrop":
        r.move(cmd)
    elif cmd == "moves":
        reached = {}
        for m in MOVES:
            reached[m] = r.move(m)
        seen = sorted({s for v in reached.values() for s in v})
        r.log("states reached:", " ".join("%d %s" % (s, STATES[s]) for s in seen))
    elif cmd == "career":
        r.career(int(args[0]))
    elif cmd == "splitscreen":
        r.splitscreen()
    elif cmd == "freeskate":
        r.freeskate(int(args[0]), int(args[1]) if len(args) > 1 else 0)
    elif cmd == "options":
        options_tour(r, not (args and args[0] == "nocheats"))
    elif cmd == "mainmenu":
        main_menu(r)
    elif cmd == "timedend":
        timed_end(r, int(args[0]) if args else 3)
    elif cmd == "mpgame":
        mp_game(r, int(args[0]), int(args[1]) if len(args) > 1 else 3000, int(args[2]) if len(args) > 2 else 1)
    elif cmd == "pausetour":
        pause_tour(r, tuple(int(x) for x in args[0].split(",")) if args else (1, 2, 4, 5, 3))
    elif cmd == "airchain":
        air_chain(r, int(args[0]) if args else 40, int(args[1]) if len(args) > 1 else 1,
                  float(args[2]) if len(args) > 2 else -60.0)
    elif cmd == "airtricks":
        air_tricks(r, [int(x) for x in args[0].split(",")] if args and args[0] != "all" else None,
                   float(args[1]) if len(args) > 1 else -40.0, args[2] if len(args) > 2 else "SQUARE",
                   jizz=len(args) > 3 and args[3] == "jizz")
    elif cmd == "buttons":                     # N: every button held in turn for N vsyncs in all (the developer modes' input)
        n, k = int(args[0]) if args else 600, 0
        order = ("UP", "LEFT", "DOWN", "RIGHT", "TRIANGLE", "SQUARE", "CIRCLE", "CROSS", "L1", "R1", "L2", "R2")
        while k < n:
            r.batch("hold %s 20" % order[(k // 25) % len(order)], "wait 5")
            k += 25
    elif cmd == "devmenu":
        dev_menu(r, int(args[0]) if args else 3)
    elif cmd == "timedrun":
        r.mode_level(2, int(args[0]), int(args[1]) if len(args) > 1 else 0)
    elif cmd == "skate":
        for i in range(int(args[0]) if args else 1):
            r.skate("routes-%d" % i)
    elif cmd == "menu":
        r.menu()
    elif cmd in CAREER3_COMMANDS:              # paths3: keys, tweaks, boxes, talk, points
        CAREER3_COMMANDS[cmd](r, args)
    elif cmd == "teleport":
        r.teleport(*(float(a) for a in args[:3]))
    elif cmd == "states":
        r.watch(int(args[0]))
    elif cmd == "mash":
        mash(r, args)
    elif cmd == "controls":
        controls(r)
    elif cmd == "sound":
        sound(r, args[0] if args else "pause", int(args[1]) if len(args) > 1 else 2)
    elif cmd in PATHS4_COMMANDS:               # paths4: mpround, mpmenu, mpcrash, zones, zonerun, pausepages, cammode, quit
        PATHS4_COMMANDS[cmd](r, args)
    elif cmd in PATHS5_COMMANDS:               # paths5: pieces, piecetouch, player
        PATHS5_COMMANDS[cmd](r, args)
    elif cmd in PATHS6_COMMANDS:               # paths6: wait, pausefor, cardfill
        PATHS6_COMMANDS[cmd](r, args)
    elif cmd in PATHS7_COMMANDS:               # paths7: restartlevel, shot, careerpoke, manualhold, stallhold
        PATHS7_COMMANDS[cmd](r, args)
    elif cmd in PATHS8_COMMANDS:               # paths8: polebutton
        PATHS8_COMMANDS[cmd](r, args)
    elif cmd == "where":
        r.log("pos", r.f32(POS), "state", r.state(), "level", r.u32(LEVEL), "mode", r.u32(MODE))
    else:
        raise SystemExit("unknown command " + cmd)


TRACKS_ENABLED = 0x24CBD0         # the music tracks the player has on (a bit each, 13 tracks)
VOLUMES = 0x24CBCC                # the options' volumes: effects, music, speech (0 .. 127)


def sound_sliders(r, full=False):
    """The three volume sliders (SFX, music, speech; DOWN to the next): each
    held left and right (a slider moves about 0.4 a vsync held); full: from
    0 to 127 and back to where it was (the clamps)."""
    for i in range(3):
        if i:
            r.batch("press DOWN 6 20")
        n = 340 if full else 40
        r.batch("press LEFT 6 10", "hold LEFT %d" % n, "wait 10", "press RIGHT 6 10", "hold RIGHT %d" % (2 * n if full else n),
                "wait 10", "hold LEFT 10", "wait 10")
    r.batch("press DOWN 6 20")


def playlist(r):
    """(In the pause's Sound Options) R1: the playlist. The current track
    changed (RIGHT, LEFT), every track turned off (CROSS on each row, the
    cursor walked up and down), next and previous with none on ("No enabled
    tracks"), every track turned on again; R1 back to the volumes."""
    r.batch("press R1 6 40", "wait 30")
    r.batch("press RIGHT 6 20", "press RIGHT 6 20", "press LEFT 6 20", "wait 10")
    seen = []
    for want_on in (False, True):
        way = "UP"
        for _ in range(60):
            e = r.u32(TRACKS_ENABLED) & 0xFFFF
            if e == (0x1FFF if want_on else 0):
                break
            r.batch("press CROSS 6 20", "wait 4")
            if ((r.u32(TRACKS_ENABLED) & 0xFFFF) > e) != want_on:
                r.batch("press CROSS 6 20", "wait 4")      # it was the other way: back
            c = r.panel_cursor()[1]
            r.batch("press %s 6 20" % way)
            if r.panel_cursor()[1] == c:
                way = "DOWN" if way == "UP" else "UP"
        seen.append(r.u32(TRACKS_ENABLED) & 0xFFFF)
        if not want_on:
            r.batch("press RIGHT 6 20", "press LEFT 6 20", "wait 10")
    r.batch("press RIGHT 6 20", "wait 10", "press R1 6 40", "wait 30")
    r.log("playlist: tracks on %#x after all off, %#x after all on" % tuple(seen))


def sound(r, where="pause", entry=2):
    """The sound options (round gamehand3: soundEvent's volume and music
    events). front: from the main menu (cursor on CAREER) OPTIONS, SOUND, the
    sliders from 0 to 127, accepted, back to CAREER. pause: (in a level) START,
    SOUND OPTIONS (entry 2; 3 in split screen), the sliders, the playlist, back to play."""
    if where == "front":
        p0 = r.u32(PANEL)
        for _ in range(5):
            r.press("RIGHT", gap=30)
        r.press("CROSS", gap=60)
        r.new_panel(p0)
        r.press("RIGHT", gap=30)
        r.press("CROSS", gap=60)
        r.batch("wait 30")
        r.log("sound options: panel", hex(r.u32(PANEL)), "volumes %#08x" % r.u32(VOLUMES))
        sound_sliders(r, full=True)
        r.press("CROSS", gap=60)
        r.press("TRIANGLE", gap=60)
        r.batch("wait 60")
        r.new_panel(0)
        for _ in range(5):
            r.press("LEFT", gap=30)
        r.batch("wait 30")
        r.log("front end sound options done: volumes %#08x" % r.u32(VOLUMES))
        return
    r.tips()
    r.press("START", gap=40)
    r.batch("wait 30")
    for _ in range(10):
        c = r.panel_cursor()[1]
        if c == entry:
            break
        r.press("DOWN" if c is None or c < entry else "UP", gap=20)
    r.press("CROSS", gap=40)
    r.batch("wait 30")
    sound_sliders(r)
    playlist(r)
    r.press("TRIANGLE", gap=40)
    r.batch("wait 20")
    r.press("TRIANGLE", gap=40)
    r.batch("wait 30")
    r.log("pause sound options done: mode %d volumes %#08x tracks %#x" % (
        r.u32(MODE), r.u32(VOLUMES), r.u32(TRACKS_ENABLED) & 0xFFFF))


def controls(r):
    """The controls update's analog and two-button paths (round gamehand,
    aggressive/skater/control_steps.cpp): the left stick in and past the dead
    zone (0.1) on each axis, L1+R1 together on the ground, R2 and L2 taps."""
    player = r.player()
    seen = []
    steps = [("LY", -0.8, 40), ("LX", 0.05, 20), ("LX", 0.7, 20), ("LX", -0.7, 20), ("LX", 0.0, 1),
             ("LY", -0.05, 20), ("LY", 0.8, 20), ("LY", -0.8, 30)]
    for axis, v, n in steps:
        r.batch("stick %s %s" % (axis, v), "wait %d" % n)
        seen.append(r.state(player))
    for presses in ("L1+R1", "L1+R1", "R2", "L2", "L1+R1"):
        r.batch("press %s 2" % presses, "wait 40")
        seen.append(r.state(player))
    r.batch("stick LX 0", "stick LY 0", "wait 30")
    r.log("controls: states " + " ".join("%d %s" % (st, STATES[st]) for st in seen if st is not None))


# ---- paths1: the front end, Timed Run, Multiplayer games, air tricks, the debug menu ----
# (round paths1: routes to the swept drafts no route reached; docs/READABLE.md.)
CHEAT_CODES = ("sddsasasddd", "wwssadadBABA", "SKELETON", "wswsadadABABS", "aaddadswwsAI", "KHUFU",
               "BIGUPYASELF", "JUSTIN BAILEY", "aaaaddddadadw", "QUEZDONTSLEEP", "BAKABAKA", "ZZZ")
# Options > Cheats keyboard: 9 a row, w a s d the arrow keys (BRINGUP.md "Level and mode sweep")
CHEAT_KEYS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ+0123456789wasd \x7f\x03"
# the card dialogs' keyboard (park description, author)
CARD_KEYS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+!? \x7f\x03"


def kb_goto(r, k, cols=9):
    for _ in range(40):
        c = r.panel_cursor()[1]
        if c is None or c == k:
            return c == k
        cr, cc = divmod(c, cols)
        kr, kc = divmod(k, cols)
        if kr != cr:
            r.press("DOWN" if kr > cr else "UP", gap=12)
        else:
            r.press("RIGHT" if kc > cc else "LEFT", gap=12)
    return False


def kb_type(r, text, keys):
    """Type TEXT on a zgui keyboard (KEYS: its keys in cursor order, END last), then END."""
    for ch in text:
        if ch in keys:
            kb_goto(r, keys.index(ch))
            r.press("CROSS", gap=15)
    kb_goto(r, len(keys) - 1)
    r.press("CROSS", gap=40)


def main_menu(r):
    """Back on the main menu after a mode: wait for its panel, the cursor on CAREER."""
    r.batch("until 0x276FE4==1 3600", "until 0x240A20!=0 3600", "wait 90")
    for _ in range(6):
        r.press("LEFT", gap=30)
    r.batch("wait 20")


def back_to(r, panel, most=6):
    """TRIANGLE until the top panel is PANEL again."""
    for _ in range(most):
        if r.u32(PANEL) == panel:
            return True
        r.press("TRIANGLE", gap=60)
        r.batch("wait 30")
    return r.u32(PANEL) == panel


def vmenu(r, k, most=20):
    """A vertical zgui menu's cursor (+0x114) to entry K."""
    for _ in range(most):
        c = r.panel_cursor()[1]
        if c == k or c is None or c > 255:   # (no menu on top: the word is not a cursor)
            break
        r.press("DOWN" if c < k else "UP", gap=20)
    return r.panel_cursor()[1] == k


def card_save(r, slot=0, yes=True):
    """(In a card dialog's slot list) slot SLOT, then YES (UP: NO is preselected) on each question."""
    vmenu(r, slot)
    r.press("CROSS", gap=60)
    r.batch("wait 150")
    for _ in range(2):
        if r.u32(MODE) not in (1, 3):
            break
        r.press("UP" if yes else "DOWN", gap=20)
        r.press("CROSS", gap=60)
        r.batch("wait 240")


def options_tour(r, cheats=True):
    """Main menu > OPTIONS: Save & Load (save, overwrite, load; load from slot 2), Sound,
    Controller, Media Viewer (the first entry played), Credits (rolled), Cheats (every code
    typed and a wrong one, the list toggled); back to the main menu."""
    main_menu(r)
    top = r.u32(PANEL)
    for _ in range(5):
        r.press("RIGHT", gap=30)
    r.press("CROSS", gap=60)
    opts = r.new_panel(top)
    r.log("options panel", hex(opts))
    # 0 Save & Load: SAVE twice (new, overwrite), LOAD slot 1, LOAD slot 2
    for what, slot in (("save", 0), ("save", 0), ("load", 0), ("load", 1), ("save", 1)):
        r.set_cursor(0)
        r.press("CROSS", gap=60)
        sub = r.new_panel(opts)
        vmenu(r, 0 if what == "save" else 1)
        r.press("CROSS", gap=60)
        r.batch("wait 90")
        card_save(r, slot)
        r.log("save & load:", what, "slot", slot + 1, "panel", hex(r.u32(PANEL)), "mode", r.u32(MODE))
        back_to(r, opts)
    # 1 Sound, 2 Controller: values changed both ways
    for k in (1, 2):
        r.set_cursor(k)
        r.press("CROSS", gap=60)
        r.batch("wait 60")
        for b in ("RIGHT", "LEFT", "DOWN", "RIGHT", "LEFT", "DOWN", "RIGHT", "LEFT", "DOWN", "CROSS", "UP", "UP"):
            r.press(b, gap=20)
        back_to(r, opts)
        r.log("options entry", k, "visited")
    # 3 Media Viewer: each entry played a while
    r.set_cursor(3)
    r.press("CROSS", gap=60)
    mv = r.new_panel(opts)
    for i in range(3):
        r.press("CROSS", gap=60)
        r.batch("wait 600")
        r.press("TRIANGLE", gap=60)
        r.batch("wait 60")
        if r.u32(PANEL) != mv:
            back_to(r, mv, 2)
        r.press("DOWN", gap=30)
        r.press("RIGHT", gap=30)
    back_to(r, opts)
    r.log("media viewer visited")
    # 4 Credits: rolled
    r.set_cursor(4)
    r.press("CROSS", gap=60)
    r.batch("wait 2400")
    back_to(r, opts)
    r.log("credits rolled")
    # 5 Cheats
    if cheats:
        for code in CHEAT_CODES:
            r.set_cursor(5)
            r.press("CROSS", gap=60)
            r.batch("wait 30")
            kb_type(r, code, CHEAT_KEYS)
            r.batch("wait 60")
            back_to(r, opts)
            r.log("cheat typed:", code, "flags %#x %#x" % (r.u32(0x24CBB8), r.u32(0x24CBC8)))
        r.set_cursor(5)
        r.press("CROSS", gap=60)
        r.batch("wait 30")
        r.press("CIRCLE", gap=60)
        r.batch("wait 30")
        for _ in range(12):
            r.press("CROSS", gap=20)
            r.press("DOWN", gap=20)
        for _ in range(12):
            r.press("UP", gap=20)
            r.press("CROSS", gap=20)
        back_to(r, opts)
        r.log("cheat list toggled: flags %#x" % r.u32(0x24CBC8))
    back_to(r, top)
    r.log("options done, panel", hex(r.u32(PANEL)))


def timed_end(r, then=3):
    """(In a Timed Run) to the end of the clock (UP held), the statistics, then the Timed
    Game menu's entry THEN (0 Start Run, 1 Sound Options, 2 Level Select, 3 Exit Game)."""
    r.batch("down UP", "until 0x276FE4==3 20000", "up UP", "wait 120")
    r.press("CROSS", gap=60)
    r.batch("wait 60")
    if then == 1:
        vmenu(r, 1)
        r.press("CROSS", gap=60)
        r.batch("wait 30")
        sound_sliders(r)
        r.press("TRIANGLE", gap=60)
        r.batch("wait 30")
        then = 0
    vmenu(r, then)
    r.log("timed game menu entry", r.panel_cursor()[1])
    r.press("CROSS", gap=60)
    if then == 0:
        r.wait_placed()
    elif then == 3:
        main_menu(r)
    else:
        r.batch("wait 300")





def mp_game(r, game, frames=3000, seed=1):
    """(In split screen, on the Multiplayer menu: 0 Most Points, 1 Best Trick, 2 Egg Hunt,
    3 Twenty One, 4 Animal Rescue, 5 Sound Options) game GAME played: seeded play (mash,
    player one; port 1 mirrors the input), to its end, the results, back to the menu."""
    import random
    for _ in range(20):
        if r.u32(MODE) == 3 and r.u32(PANEL):
            break
        r.press("START" if r.u32(MODE) == 2 else "CROSS", gap=60)
        r.batch("wait 60")
    menu = r.u32(PANEL)
    vmenu(r, game)
    r.log("multiplayer menu entry", r.panel_cursor()[1])
    r.press("CROSS", gap=60)
    r.batch("wait 60")
    if game == 5:
        sound_sliders(r)
        r.press("TRIANGLE", gap=60)
        r.batch("wait 30")
        return
    r.tips()
    r.batch("until 0x276FE4==2 3000", "wait 60")
    mash(r, [str(frames), str(seed)])
    r.batch("down UP", "until 0x276FE4==3 30000", "up UP", "wait 120")
    for _ in range(6):
        if r.u32(PANEL) == menu:
            break
        r.press("CROSS", gap=60)
        r.batch("wait 120")
    r.log("multiplayer game", game, "over: mode", r.u32(MODE), "panel", hex(r.u32(PANEL)))


# ---- paths4: the Multiplayer games to their results, pause pages, effects -------------------
# (round paths4.) A zgui screen is known by the name hash (zge::nameHash, +0x10) of the top
# panel's parent group: the pause's "Pause", split screen's game choice "MultiplayerStart" and
# its results "MultiplayerOverStats" (UILIB.ZAL's InGame scripts).
SCREEN_PAUSE, SCREEN_MP_MENU, SCREEN_MP_RESULTS = 0x546A75, 0x29AAAB4, 0x6BA1013
MP_GAMES = ("Most Points", "Best Trick", "Egg Hunt", "Twenty One", "Animal Rescue")


def screen(r):
    """The name hash of the top zgui panel's parent group (0: no panel)."""
    p = r.u32(PANEL)
    return r.u32(r.u32(p + 8) + 0x10) if p else 0


def mp_to_menu(r):
    """(In split screen) to the Multiplayer menu (the game choice): from play START, GAME
    SELECT (pause entry 1: inlinePauseMultiplayerEndRun), the RESULTS screen
    (inlinePausePopulateMultiplayerOverStats), CROSS (Continue)."""
    for _ in range(16):
        mode, scr = r.u32(MODE), screen(r)
        if mode == 3 and scr == SCREEN_MP_MENU:
            return True
        if mode == 2:
            r.press("START", gap=40)
            r.batch("wait 40")
        elif scr == SCREEN_PAUSE:
            vmenu(r, 1)
            r.log("mp: pause, GAME SELECT")
            r.press("CROSS", gap=60)
            r.batch("wait 90")
        elif scr == SCREEN_MP_RESULTS:
            r.log("mp: results, Continue")
            r.batch("wait 60")
            r.press("CROSS", gap=60)
            r.batch("wait 90")
        else:
            r.press("CROSS", gap=40)
            r.batch("wait 40")
    return r.u32(MODE) == 3 and screen(r) == SCREEN_MP_MENU


def mp_crash(r, n=3, lift=-15.0, hold="R1"):
    """(Test only: a poke) N bails: an ollie, the vertical velocity poked to LIFT, a spin (HOLD,
    R1) held through the landing, so the skater lands sideways; the states seen."""
    player = r.player()
    before = r.u32(player + P_CRASHES)
    for _ in range(n):
        r.tips()
        r.batch("down UP", "wait 30", "press CROSS 3 1", "until %#x==2 120" % (player + P_STATE))
        r.batch("poke %#x %#x" % (player + P_VELOCITY + 4, f2u(lift)), "up UP", "wait 4",
                "down %s" % hold)
        seen = r.watch(150)
        r.batch("up %s" % hold, "wait 90")
        r.log("mp crash: states", "/".join(STATES[x] for x in seen if x < len(STATES)),
              "crashes", r.u32(player + P_CRASHES))
    return r.u32(player + P_CRASHES) - before


# The level's trigger zones (zones.cpp: the list at 0x279674, +0x7C next; FUN_001C50D0 runs
# a zone's enter by its type +0x58 when the skater enters it): the world position at +0x40,
# the target's id +0x50, +0x64 nonzero when the zone is live.
ZONE_LIST, ZONE_NEXT, ZONE_POS, ZONE_TARGET, ZONE_TYPE, ZONE_LIVE = 0x279674, 0x7C, 0x40, 0x50, 0x58, 0x64


def zones(r):
    """[(zone, type, live, (x, y, z), target)] of the level's trigger zones."""
    out, z = [], r.u32(ZONE_LIST)
    while z and len(out) < 4000:
        out.append((z, r.u32(z + ZONE_TYPE), r.u32(z + ZONE_LIVE), tuple(r.f32(z + ZONE_POS)),
                    r.u32(z + ZONE_TARGET)))
        z = r.u32(z + ZONE_NEXT)
    return out


def zone_run(r, types=None, hold=40, most=200):
    """(For recording, test only: teleports) the skater put on each live zone (of TYPES, or
    all), HOLD vsyncs there, then on to the next; the zone types touched."""
    seen = {}
    for z, t, live, at, target in zones(r)[:most]:
        if not live or (types and t not in types):
            continue
        r.tips()
        r.teleport(at[0], at[1], at[2])
        r.batch("wait %d" % hold)
        seen[t] = seen.get(t, 0) + 1
    r.log("zones touched (type: count):", " ".join("%d:%d" % kv for kv in sorted(seen.items())))
    return seen


def mp_round(r, game, frames=1500, seed=1, crashes=3, touch=1):
    """(In split screen, `splitscreen` first) the Multiplayer menu's game GAME (0 Most Points,
    1 Best Trick, 2 Egg Hunt, 3 Twenty One, 4 Animal Rescue): to the menu (mp_to_menu), the game
    chosen (inlinePauseMultiplayerStartEnd), FRAMES of mash (seeded), TOUCH: every live trigger
    zone touched (zonerun: the games' items), CRASHES bails (test only), UP held to the end of
    its clock, the results, Continue: back on the Multiplayer menu."""
    if not mp_to_menu(r):
        r.log("mp: no Multiplayer menu (mode %d, screen %#x)" % (r.u32(MODE), screen(r)))
        return False
    vmenu(r, game)
    r.log("mp: game", game, MP_GAMES[game] if game < len(MP_GAMES) else "?", "cursor", r.panel_cursor()[1])
    r.press("CROSS", gap=60)
    r.batch("wait 90")
    r.tips()
    r.batch("until 0x276FE4==2 3000", "wait 60")
    if frames:
        mash(r, [str(frames), str(seed)])
    if touch:
        zone_run(r)
    if crashes:
        mp_crash(r, crashes)
    r.tips()
    r.batch("down UP", "until 0x276FE4==3 30000", "up UP", "wait 120")
    r.log("mp: game", game, "over: mode", r.u32(MODE), "screen %#x" % screen(r))
    return mp_to_menu(r)


def pause_pages(r, most=16, unlock=True):
    """(In a career level) the pause pages paths1's pausetour missed: Challenges, each entry's
    fly-thru (CIRCLE: inlinePausePlayFlyThru, the camera animation played to its end; entries
    without one ignore it), the Summary (R1) and a detail (CROSS); the Trick List's six
    categories, Specials last (inlinePausePopulateSpecialTrickList; UNLOCK, test only: every
    trick unlocked first, Player +0x5F0, as airtricks' jizz); back to play."""
    if unlock:
        player = r.player()
        r.batch("poke %#x %#x" % (player + 0x5F0, r.u32(player + 0x5F0) | 0xFFFF))
    r.tips()
    r.press("START", gap=40)
    r.batch("wait 40")
    pause = r.u32(PANEL)
    vmenu(r, 1)
    r.press("CROSS", gap=60)
    r.batch("wait 60")
    page = r.u32(PANEL)
    flown = []
    for k in range(most):
        if r.u32(PANEL) != page or not vmenu(r, k):
            break
        r.press("CIRCLE", gap=40)
        r.batch("wait 30")
        if r.u32(MODE) == 2:
            r.batch("until 0x276FE4==3 6000", "wait 60")
            flown.append(k)
    r.log("pause pages: challenges, fly-thrus played:", flown)
    if r.u32(PANEL) == page:
        r.batch("press R1 6 40", "wait 40", "press R1 6 40", "wait 40", "press CROSS 6 40", "wait 40")
    back_to(r, pause, 8)
    vmenu(r, 5)
    r.press("CROSS", gap=60)
    r.batch("wait 60")
    cats = r.u32(PANEL)
    for c in range(6):
        if r.u32(PANEL) != cats:
            back_to(r, cats, 3)
        vmenu(r, c)
        r.batch("press CROSS 6 40", "wait 40", "press DOWN 6 20", "press DOWN 6 20", "press DOWN 6 20",
                "press UP 6 20", "wait 30", "press TRIANGLE 6 40", "wait 30")
    r.log("pause pages: trick list categories done")
    back_to(r, pause, 8)
    if r.u32(MODE) == 3 and r.u32(PANEL) == pause:
        r.press("TRIANGLE", gap=40)
        r.batch("wait 30")
    r.tips()


FOLLOW_CAMERA, CAMERA_MODE = 0x24C9D0, 0x71   # camera.h g_followCamera; updateCamera's mode byte


def camera_modes(r, modes=(1, 4), frames=150):
    """(Test only: a poke; in a level) the follow camera's mode byte (+0x71: 0 follow, 1, 3, 4
    the other cameras updateCamera runs) set to each of MODES for FRAMES vsyncs while the
    skater rides (UP held), then back to what it was."""
    word = (FOLLOW_CAMERA + CAMERA_MODE) & ~3
    shift = 8 * ((FOLLOW_CAMERA + CAMERA_MODE) & 3)
    old = r.u32(word)
    for m in modes:
        r.batch("poke %#x %#x" % (word, (old & ~(0xFF << shift)) | (m << shift)), "down UP",
                "wait %d" % frames, "up UP")
        r.log("camera mode %d: %d vsyncs (mode byte now %d)" % (m, frames, (r.u32(word) >> shift) & 0xFF))
    r.batch("poke %#x %#x" % (word, old), "wait 30")


GAME_QUIT = 0x23FD64             # main's (FUN_001176C8) loop runs while this word is 0


def game_quit(r, wait=600):
    """(Test only: a poke) the game's own exit: main's loop flag (FUN_001176C8 loops while
    0x23FD64 is 0) set, so main leaves its loop: the game's shutdown (FUN_00185D78), the
    engine's (FUN_00137D58), FUN_001194A0, and main returns (never on a retail PS2, whose
    loop flag nothing sets). Waits up to WAIT vsyncs for the runner to end."""
    r.log("quit: main's loop flag set")
    try:
        r.batch("poke %#x 1" % GAME_QUIT)
        for _ in range(max(1, wait // 60)):
            r.batch("wait 60", timeout=60)
            if r.u32(MODE) == 0xFFFFFFFF:     # the sound shutdown (FUN_001C11C0) sets the mode to -1
                r.batch("wait 120", timeout=60)
                break
    except (nctl.RunnerGone, SystemExit):
        r.log("quit: the runner ended")
        return True
    r.log("quit: mode %#x (-1: the game shut down; the runner stays after the guest's Exit)" % r.u32(MODE))
    return r.u32(MODE) == 0xFFFFFFFF


PATHS4_COMMANDS = {
    "mpround": lambda r, a: mp_round(r, int(a[0]), *(int(v) for v in a[1:5])),
    "mpmenu": lambda r, a: mp_to_menu(r),
    "poke": lambda r, a: r.batch(*["poke %#x %#x" % (int(a[i], 0), int(a[i + 1], 0)) for i in range(0, len(a) - 1, 2)]),
    "quit": lambda r, a: game_quit(r, *(int(v) for v in a[:1])),
    "cammode": lambda r, a: camera_modes(r, tuple(int(v) for v in a[0].split(",")) if a else (1, 4),
                                         *(int(v) for v in a[1:2])),
    "pausepages": lambda r, a: pause_pages(r, *(int(v) for v in a[:1])),
    "zones": lambda r, a: [r.log("zone %#x type %d live %d at %.1f %.1f %.1f target %#x" % ((z, t, l) + at + (g,)))
                           for z, t, l, at, g in zones(r)],
    "zonerun": lambda r, a: zone_run(r, set(int(v) for v in a[0].split(",")) if a and a[0] != "all" else None,
                                     *(int(v) for v in a[1:2])),
    "mpcrash": lambda r, a: mp_crash(r, *([int(a[0])] if a else []) + [float(v) for v in a[1:2]] + a[2:3]),
}


# Round paths5. The careers' in-level editor pieces (Cannery's Pisser, Airfield's Vacuum: the
# edit selection, FUN_001F1BD8 / FUN_001F1C20 walk it): the list's head at gp-0x1DD0
# (0x278B20) points to its first node; a node's record is node + 0x10 (+0x00 whole, +0x1C its
# scene object, whose position is at +0x40); the next node is the node's first word.
# skaterEffects (FUN_001C7ED8) in levels 2 and 3 checks each piece against Player +0x1A0 (the
# last object ridden) and +0x1A4: Cannery breaks it, Airfield calls FUN_001C6728 (its touch).
EDIT_PIECES = 0x278B20
P_RIDDEN, P_LAST_RIDDEN = 0x19C, 0x1A0


def edit_pieces(r):
    out, node = [], r.u32(r.u32(EDIT_PIECES)) if r.u32(EDIT_PIECES) else 0
    while node and len(out) < 64:
        rec = node + 0x10
        obj = r.u32(rec + 0x1C)
        out.append((rec, r.u32(rec), obj, tuple(r.f32(obj + 0x40, 3)) if obj else (0.0, 0.0, 0.0)))
        node = r.u32(node)
    return out


def piece_touch(r, speed=8.0, hold=60):
    """(test only: rides) each in-level editor piece ridden into from 6 units off on each side."""
    player = r.player()
    touched = set()
    for rec, whole, obj, (x, y, z) in edit_pieces(r):
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ride(r, x - 6 * dx, y - 1, z - 6 * dz, dx, dz, speed, 0.0, hold)
            if obj in (r.u32(player + P_RIDDEN), r.u32(player + P_LAST_RIDDEN)):
                touched.add(obj)
    r.log("piecetouch: %d pieces, %d ridden (%s)" % (len(edit_pieces(r)), len(touched),
                                                      " ".join("%#x" % o for o in sorted(touched))))
    return len(touched)


PATHS5_COMMANDS = {
    "pieces": lambda r, a: [r.log("piece %#x whole %d object %#x at %.2f %.2f %.2f" % ((rec, w, o) + at))
                            for rec, w, o, at in edit_pieces(r)],
    "piecetouch": lambda r, a: piece_touch(r, *(float(v) for v in a[:1]), *(int(v) for v in a[1:2])),
    "player": lambda r, a: r.log("player one %#x, its scene object (Player +0x14) %#x, its ragdoll (Player +0x0C) %#x"
                                 % (r.player(), r.u32(r.player() + 0x14), r.u32(r.player() + 0xC))),
}

# Round paths6: small helpers for the weather, effect and card routes.
def pause_for(r, frames=120):
    """(In a level) paused for FRAMES vsyncs, then back to play."""
    paused(r, True)
    r.batch("wait %d" % frames, timeout=max(600, frames // 10))
    paused(r, False)
    r.log("pausefor: %d vsyncs paused, mode %d" % (frames, r.u32(MODE)))


def card_fill(r, kb=8000):
    """The card in slot 1 (this run's mc0 folder) filled: a KB file in a directory of its own, so the
    runtime's card reports that much less free space (a card too full for a save), or emptied again
    (KB 0)."""
    d = os.path.join(os.environ.get("ROUTES_DIR", "/tmp/routes"), r.name, "mc0", "BADATA-FILLER")
    if kb <= 0:
        shutil.rmtree(d, ignore_errors=True)
    else:
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "FILLER"), "wb") as f:
            f.truncate(kb * 1024)
    r.log("cardfill: %d KB in %s" % (kb, d))


PATHS6_COMMANDS = {
    "cardfill": lambda r, a: card_fill(r, *(int(v) for v in a[:1])),
    "wait": lambda r, a: r.batch("wait %d" % int(a[0]), timeout=max(600, int(a[0]) // 10)),
    "pausefor": lambda r, a: pause_for(r, *(int(v) for v in a[:1])),
}


def career_poke(r, k, pokes):
    """From the main menu: CAREER, first skater, level-select entry K as `career`, with the words POKES
    ([(addr, value)], test only) written on the level select just before its Cross (after the menus'
    own file opens: their preview movies), then the load and placement."""
    p0 = r.u32(PANEL)
    r.press("CROSS", gap=30)
    p1 = r.new_panel(p0)
    r.press("CROSS", gap=30)
    p2 = r.new_panel(p1)
    if r.panel_cursor()[1] == NAME_ENTRY_END:
        r.press("CROSS", gap=30)
        r.new_panel(p2)
    r.set_cursor(k)
    r.batch("wait 60", *("poke %#x %#x" % pv for pv in pokes))
    r.log("careerpoke: level select entry", r.panel_cursor()[1], "poked", " ".join("%#x=%#x" % pv for pv in pokes))
    r.press("CROSS", gap=30)
    return r.wait_placed()


def restart_level(r, level=None):
    """(In Career or Freeskate) the pause menu's Restart Level (the port's item 1, after Resume Game:
    aggressive_inline_restart.cpp writes the loader's 0 and 1 and switches, so the level's exit runs and
    the same level, skaters and mode load again); LEVEL (test only): the level byte 0x24C9B4 set to it
    on the paused menu first, so that level loads instead (9: LOCAL)."""
    paused(r, True)
    for _ in range(30):                    # the pause panel and its cursor up
        if r.panel_cursor()[1] is not None:
            break
        r.batch("wait 10")
    r.set_cursor(1, inc="DOWN", dec="UP")
    if level is not None:
        w = r.u32(LEVEL)
        r.batch("poke %#x %#x" % (LEVEL, (w & ~0xFF) | (level & 0xFF)))
    r.log("restartlevel: pause cursor", r.panel_cursor()[1], "level byte", r.u32(LEVEL) & 0xFF)
    r.press("CROSS", gap=30)
    r.batch("until %#x!=3 600" % MODE)
    r.batch("wait 60")
    return r.wait_placed()


def manual_hold(r, buttons="SQUARE,CIRCLE,TRIANGLE,CROSS,L1,R1", hold=30):
    """(In the Tutorial) for each button: the Manuals lesson, rolling, UP let go, Up-Down-Up taps (as
    `manual`), then once the skater is in 14 Manual the button held HOLD vsyncs and let go."""
    seen = set()
    for b in buttons.split(","):
        r.lesson(6)
        player = r.player()
        r.batch("down UP", "wait 50", "up UP", "wait 2", "press UP 2 2", "press DOWN 2 2", "press UP 2 2")
        for _ in range(40):
            if r.state(player) == 14:
                break
            r.batch("wait 2")
        if r.state(player) != 14:
            r.log("manualhold: no manual for", b)
            continue
        r.batch("down %s" % b, "wait %d" % hold, "up %s" % b)
        seen.update(r.watch(20))
        r.log("manualhold %s: held in the manual, state now %d" % (b, r.state(player)))
        r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    r.log("manualhold: states", " ".join("%d %s" % (s_, STATES[s_]) for s_ in sorted(seen)))


def stall_hold(r, vsyncs=600, taps="-"):
    """(In the Tutorial) the Hand Plants lesson: up the quarter pipe, the d-pad TAPS (as `stalltrick`: UP
    Abstract, DOWN Rocket, LEFT Method, RIGHT Stale, - the plain Hand Plant), TRIANGLE held at the lip for
    VSYNCS."""
    r.lesson(12)
    presses = [] if taps == "-" else ["press %s 2 2" % b.upper() for b in taps.split(",")]
    r.batch("down UP", "until " + above(POS, -8.5) + " 600", "up UP", *presses, "down TRIANGLE")
    seen = r.watch(vsyncs, step=10)
    r.batch("up TRIANGLE")
    seen += r.watch(60)
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")
    r.log("stallhold %d %s: states" % (vsyncs, taps), " ".join("%d %s" % (s_, STATES[s_]) for s_ in sorted(set(seen))))


def press_buttons(r, buttons, tools=None):
    """Each button pressed in turn (40 vsyncs apart); with TOOLS (the Park Editor's roll entries, pe2roll's)
    the round of presses in each tool after rolling to it."""
    for k in (tools if tools is not None else [None]):
        if k is not None:
            pe_roll(r, k)
        for b in buttons:
            r.batch("press %s 6 40" % b)
        r.batch("wait 30")
        r.log("buttons %s%s pressed, mode %d" % (",".join(buttons), "" if k is None else " in tool %d" % k,
                                                 r.u32(MODE)))


PATHS7_COMMANDS = {
    "manualhold": lambda r, a: manual_hold(r, *([a[0]] if a else []), *(int(v) for v in a[1:2])),
    "stallhold": lambda r, a: stall_hold(r, *(int(v) for v in a[:1]), *a[1:2]),
    "buttons7": lambda r, a: press_buttons(r, a[0].split(","), [int(k) for k in a[1].split(",")] if len(a) > 1 else None),
    "restartlevel": lambda r, a: restart_level(r, *(int(v, 0) for v in a[:1])),
    "careerpoke": lambda r, a: career_poke(r, int(a[0]), [(int(a[i], 0), int(a[i + 1], 0)) for i in range(1, len(a) - 1, 2)]),
    "shot": lambda r, a: r.batch("wait 4", "shot %s" % os.path.abspath(a[0])),
}


def pole_button(r, taps=(1, 2), lesson=14):
    """(In the Tutorial) the Vertical Poles lesson, on the pole (state 9 or 10): UP let go, then for each TAPS entry
    CIRCLE (the grab button) let go and tapped for that many vsyncs: a press of the button while on a pole tells
    the pole object ButtonReleased (controls: the pole's `pressed` without `held`; releasePoleButton)."""
    r.lesson(lesson)
    player = r.player()
    r.batch("down UP+CIRCLE")
    deadline = time.time() + 60 * SLOW
    while time.time() < deadline:
        s = int(r.batch("peek %#x" % (player + P_STATE), "wait 1")[0].split()[1], 16) & 0xFF
        if s in (9, 10):
            break
    r.batch("up UP", "wait 20")
    seen = []
    for n in taps:
        r.batch("up CIRCLE", "wait 10", "press CIRCLE %d 5" % n)
        seen += r.watch(30)
        r.log("polebutton: CIRCLE tapped %d vsyncs, state now %d" % (n, r.state(player)))
    r.batch("up UP+DOWN+LEFT+RIGHT+CROSS+CIRCLE+TRIANGLE+SQUARE+L1+R1")


PATHS8_COMMANDS = {
    "polebutton": lambda r, a: pole_button(r, tuple(int(v) for v in a[0].split(",")) if a else (1, 2)),
}


# The trick table's air tricks (0x245928; names and taps from Tom Hayes' FAQ section 5 and
# triggers1's triggers.tsv): entry -> the d-pad taps (+ the button, Square for all of these).
AIR_TRICKS = {0: "LEFT,RIGHT,RIGHT,LEFT", 1: "DOWN,UP,UP", 2: "DOWN,DOWN,DOWN", 3: "DOWN,LEFT,LEFT",
              4: "DOWN,RIGHT,LEFT", 5: "DOWN,LEFT,RIGHT", 6: "DOWN,RIGHT,RIGHT", 7: "DOWN,UP,DOWN",
              8: "DOWN,DOWN,UP", 9: "RIGHT,UP,UP", 10: "RIGHT,DOWN,DOWN", 11: "RIGHT,LEFT,LEFT",
              12: "RIGHT,RIGHT,LEFT", 13: "RIGHT,LEFT,RIGHT", 14: "RIGHT,RIGHT,RIGHT", 15: "RIGHT,UP,DOWN",
              16: "RIGHT,DOWN,UP", 17: "UP,UP,UP", 18: "UP,DOWN,DOWN", 19: "UP,LEFT,LEFT", 20: "UP,RIGHT,LEFT",
              21: "UP,LEFT,RIGHT", 22: "UP,RIGHT,RIGHT", 23: "UP,UP,DOWN", 24: "UP,DOWN,UP", 25: "LEFT,UP,UP",
              26: "LEFT,DOWN,DOWN", 27: "LEFT,LEFT,LEFT", 28: "LEFT,RIGHT,LEFT", 29: "LEFT,LEFT,RIGHT",
              30: "LEFT,RIGHT,RIGHT", 31: "LEFT,UP,DOWN", 32: "LEFT,DOWN,UP", 33: "UP,RIGHT,DOWN",
              34: "UP,DOWN,RIGHT", 35: "DOWN,DOWN,RIGHT", 36: "RIGHT,UP,LEFT", 37: "LEFT,UP,RIGHT",
              38: "DOWN,RIGHT,UP", 39: "UP,UP", 40: "DOWN,DOWN", 41: "LEFT,LEFT", 42: "LEFT,RIGHT",
              43: "RIGHT,LEFT", 44: "RIGHT,RIGHT", 45: "UP,DOWN", 46: "DOWN,UP", 47: "UP+RIGHT",
              48: "UP+LEFT", 49: "UP", 50: "DOWN+RIGHT", 51: "RIGHT", 52: "DOWN+LEFT", 53: "LEFT", 54: "DOWN"}


def air_tricks(r, entries=None, lift=-40.0, button="SQUARE", hold=6, jizz=False):
    """(For recording, test only: a poke) each air trick of the table: the skater put back on
    the level's start, an ollie (CROSS), in the air the vertical velocity poked to LIFT (up:
    y grows downward) for the air time, the taps (2 vsyncs each) and BUTTON held HOLD vsyncs,
    then to the landing (or the bail). jizz (test only): every unlockable trick and the
    skater jizzed before each (as grindtricks' !J; Jizzed->Non-Jizzed Percent poked to 0):
    the special tricks (entries 33 to 38), which need the special bar flashing."""
    player = r.player()
    if jizz:
        addr = mash_tuning(mash_dump(r, "tuning")).get(("Scoring: Juice", "Jizzed->Non-Jizzed Percent"))
        if addr is not None:
            r.batch("poke %#x 0" % addr)
    start = r.f32(POS)
    seen = {}
    for e in (entries if entries is not None else sorted(AIR_TRICKS)):
        taps = [t for t in AIR_TRICKS[e].split(",") if t]
        r.tips()
        r.teleport(*start)
        r.batch("wait 30", "down UP", "wait 40", "press CROSS 3 1", "until %#x==2 120" % (player + P_STATE))
        if jizz:
            r.batch("poke %#x %#x" % (player + 0x5F0, r.u32(player + 0x5F0) | 0xFFFF),
                    "poke %#x %#x" % (player + 0x8C0, r.u32(player + 0x8C0) | 1))
        r.batch("poke %#x %#x" % (player + P_VELOCITY + 4, f2u(lift)), "up UP", "wait 4")
        cmds = []
        for t in taps:
            cmds += ["press %s 2 2" % t]
        cmds += ["press %s %d 2" % (button, hold)]
        r.batch(*cmds)
        states = r.watch(150)
        seen[e] = states
        r.batch("wait 30")
    r.log("air tricks:", " ".join("%d:%s" % (e, "/".join(STATES[x] for x in v if x < len(STATES))) for e, v in seen.items()))


def dev_menu(r, step=3):
    """(In a level; the runner started with ZX_DEV_MENU=1) the hidden debug menu
    (docs/DEVTOOLS.md): Toggle Debug three times, Respawn, the Art Viewer (the camera moved
    with every button, Select: its screen dump 0x110BC0), every Tuning category opened (each
    STEP-th entry stepped up and back down), Resume."""
    def open_menu(item):
        r.tips()
        r.batch("set dev.debug_menu 1", "wait 40")
        vmenu(r, item)
        r.press("CROSS", gap=40)
        r.batch("wait 40")
    for _ in range(3):
        open_menu(4)                     # Toggle Debug
        r.batch("wait 60")
        r.press("TRIANGLE", gap=40)
        r.batch("wait 30")
    open_menu(3)                         # Respawn
    r.batch("wait 60")
    open_menu(1)                         # Art Viewer
    for b in ("UP", "DOWN", "LEFT", "RIGHT", "TRIANGLE", "SQUARE", "CIRCLE", "CROSS", "L1", "R1", "L2", "R2"):
        r.batch("hold %s 20" % b, "wait 4")
    r.press("SELECT", gap=60)
    r.batch("wait 60")
    r.press("START", gap=60)
    r.batch("wait 60")
    r.tips()
    if r.u32(MODE) == 3 and r.u32(PANEL):
        r.press("START", gap=40)         # the pause menu the same press opened
        r.batch("wait 30")
    open_menu(2)                         # Tuning: the categories
    for c in range(21):
        r.press("CROSS", gap=30)             # the category: its entries
        r.batch("wait 20")
        for _ in range(0, 30, step):
            r.press("RIGHT", gap=8)
            r.press("LEFT", gap=8)
            for _ in range(step):
                r.press("DOWN", gap=8)
        r.press("TRIANGLE", gap=30)
        r.batch("wait 20")
        r.press("DOWN", gap=20)              # the next category
    r.log("tuning categories visited")
    r.press("TRIANGLE", gap=40)
    r.press("TRIANGLE", gap=40)
    r.batch("wait 30")
    open_menu(0)                         # Resume
    r.log("debug menu done: mode", r.u32(MODE))



def air_chain(r, jumps=40, seed=1, lift=-60.0, jizz=True):
    """(test only, for recording; in the Tutorial) JUMPS ollies with the air time poked (LIFT),
    each with two to four air tricks of the table in a row (seeded), some held, some let go,
    a bail now and then from a trick still on at the landing: the trick objects' combine,
    land and bail events."""
    import random
    rng = random.Random(seed)
    player = r.player()
    start = r.f32(POS)
    if jizz:
        addr = mash_tuning(mash_dump(r, "tuning")).get(("Scoring: Juice", "Jizzed->Non-Jizzed Percent"))
        if addr is not None:
            r.batch("poke %#x 0" % addr)
    for j in range(jumps):
        r.tips()
        r.teleport(*start)
        r.batch("wait 30", "down UP", "wait 40", "press CROSS 3 1", "until %#x==2 120" % (player + P_STATE))
        if jizz:
            r.batch("poke %#x %#x" % (player + 0x5F0, r.u32(player + 0x5F0) | 0xFFFF),
                    "poke %#x %#x" % (player + 0x8C0, r.u32(player + 0x8C0) | 1))
        r.batch("poke %#x %#x" % (player + P_VELOCITY + 4, f2u(lift)), "up UP", "wait 4")
        cmds = []
        for _ in range(rng.randint(2, 4)):
            for t in AIR_TRICKS[rng.choice(sorted(AIR_TRICKS))].split(","):
                cmds.append("press %s 2 2" % t)
            cmds.append("press %s %d %d" % (rng.choice(("SQUARE", "SQUARE", "CIRCLE")), rng.choice((3, 6, 20, 40)),
                                            rng.randint(2, 12)))
        r.batch(*cmds)
        r.watch(150)
        r.batch("wait 20")
    r.log("air chains: %d jumps" % jumps)


def pause_tour(r, entries=(1, 2, 4, 5, 3), presses=("DOWN", "RIGHT", "CROSS", "DOWN", "CROSS", "LEFT", "UP", "CROSS")):
    """(In a career level) the pause menu's pages: for each entry in ENTRIES (the career's
    pause: 0 Resume, 1 Challenges, 2 Attributes, 3 Save, 4 View Loot, 5 Trick List, 6 Sound
    Options, 7 Level Select, 8 Exit)
    START, the entry, CROSS, PRESSES inside (a challenge's detail and fly-thru, a category's
    list), TRIANGLE back to the pause menu, START back to play."""
    for k in entries:
        r.tips()
        r.press("START", gap=40)
        r.batch("wait 30")
        pause = r.u32(PANEL)
        vmenu(r, k)
        r.log("pause entry", r.panel_cursor()[1], "panel", hex(pause))
        r.press("CROSS", gap=60)
        r.batch("wait 60")
        for b in presses:
            if r.u32(MODE) == 2:
                break
            r.press(b, gap=40)
            r.batch("wait 30")
            if r.u32(MODE) == 3 and r.u32(PANEL) == 0:
                r.batch("wait 300")       # a fly-thru: let it run
        back_to(r, pause, 8)
        if r.u32(MODE) == 3:
            r.press("START", gap=40)
            r.batch("wait 30")
        if r.u32(MODE) == 3 and r.u32(PANEL) == pause:
            r.press("TRIANGLE", gap=40)
            r.batch("wait 30")
        r.log("pause entry", k, "done: mode", r.u32(MODE))

# ---- paths3: career progress (keys), the challenge givers, the career editors -------------
# (round paths3.) A career level's givers are its script's NPC objects (createNPCBubble on
# the object of the same name; CIRCLE by it sends the script object "action"); their places are
# the level's data points of class 0x5343 (named by zge::nameHash of the object's name; the
# list at +0x10 next / +0x14 previous, mash_points walks it one way from the respawn point).
# Areas behind doors open with this level's key (isKeyAwarded: bit level + 9 of Player
# +0x5E8, the skater record's +8 copied in at level load; the record is in the career save,
# 0x24DB30 + n * 0x510, n the skater, the name at +0x48): Industrial's junkyard (the Clear
# the Junkyard machine), Cannery's frozen room (Construct and Score), Airfield's 'B' room
# (Place and Trick) and the key areas' challenges (Wilikai's "Appears after: Getting the ...
# Key"). Level select entries and level words (0x24C9B4): 1 Movie Lot 5, 2 Civic Center 0,
# 3 Industrial 4, 4 Boardwalk 1, 5 Cannery 2, 6 Airfield 3, 7 Museum 6.
SKATER_RECORDS, SKATER_RECORD_SIZE, SKATER_RECORD_COUNT = 0x24DB30, 0x510, 20
SKATER_KEYS = 0x8                 # the record's keys doubleword (Player +0x5E8)
KEYS_ALL = 0xFE00                 # the seven career levels' keys (levels 0 .. 6: bits 9 .. 15)
POINT_LINKS = (0x10, 0x14)


def name_hash(s):
    """zge::nameHash (aggressive_script_dis.py's)."""
    h = 0
    for c in s.upper().encode("latin-1"):
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


def level_points(r):
    """Every data point of the level (both ways along the list from the respawn point):
    {name hash: (class, (x, y, z), (dx, dz))}."""
    path = os.path.join(nctl.ctl_dir(r.name), "points.ram")
    r.batch("dump " + path)
    m = open(path, "rb").read()
    os.remove(path)
    u32 = lambda a: struct.unpack_from("<I", m, a & 0x1FFFFFF)[0]
    start, i = None, m.find(struct.pack("<I", POINT_RESPAWN), 0x100000)
    while i >= 0 and start is None:
        rec = i - 4
        nxt = u32(rec + 0x10)
        if rec % 16 == 0 and 0x100000 < nxt < 0x2000000 and u32(nxt + 4) == rec + 0x10:
            start = rec
        i = m.find(struct.pack("<I", POINT_RESPAWN), i + 4)
    out, seen = {}, set()
    for link in POINT_LINKS:
        rec = start
        while rec is not None and len(seen) < 5000:
            if rec not in seen:
                seen.add(rec)
                pos = struct.unpack_from("<3f", m, (rec + 0x20) & 0x1FFFFFF)
                q = struct.unpack_from("<4f", m, (rec + 0x30) & 0x1FFFFFF)
                if all(abs(v) < 1e5 for v in pos):
                    yaw = 2 * math.atan2(q[1], q[3])
                    out.setdefault(u32(rec + 4), (u32(rec) & 0xFFFF, pos, (math.sin(yaw), math.cos(yaw))))
            nxt = u32(rec + link)
            rec = nxt - 0x10 if 0x100000 < nxt < 0x2000000 and nxt - 0x10 not in seen else None
    return out


def cr_keys(r, mask=KEYS_ALL):
    """(front end) The keys MASK ORed into every skater record (a level loaded next copies
    them into the player); in a level the player's too."""
    cmds = []
    for n in range(SKATER_RECORD_COUNT):
        a = SKATER_RECORDS + SKATER_RECORD_SIZE * n + SKATER_KEYS
        cmds.append("poke %#x %#x" % (a, r.u32(a) | mask))
    p = r.player() if r.u32(MODE) in (2, 3) else 0
    if p:
        cmds.append("poke %#x %#x" % (p + 0x5E8, r.u32(p + 0x5E8) | mask))
    r.batch(*cmds)
    r.log("keys %#x in %d skater records%s" % (mask, SKATER_RECORD_COUNT, " and the player" if p else ""))


def cr_tweaks(r, names=("juice", "invulnerable")):
    """(in a level) mash's named tweaks (MASH_TWEAKS: the cheats' tuning writes)."""
    path = os.path.join(nctl.ctl_dir(r.name), "tuning.ram")
    r.batch("dump " + path)
    m = open(path, "rb").read()
    os.remove(path)
    tuning, cmds = mash_tuning(m), []
    p = r.player()
    for t in names:
        what = MASH_TWEAKS.get(t)
        if isinstance(what, list):
            cmds += ["poke %#x %#x" % (tuning[(c, e)], f2u(v)) for c, e, v in what if (c, e) in tuning]
        elif t == "tricks":
            cmds += ["poke %#x %#x" % (p + 0x5F0, r.u32(p + 0x5F0) | 0xFFFF)]
    r.batch(*cmds)
    r.log("tweaks", ",".join(names))


def cr_boxes(r, most=30):
    """CROSS through message boxes (mode 3) that are not the pause menu."""
    n = 0
    while r.u32(MODE) == 3 and n < most:
        r.batch("press CROSS 6 40")
        n += 1
    return n


def cr_put(r, x, y, z, dx, dz):
    """Paused, the skater put at (x, y, z) facing (dx, dz), standing; unpaused."""
    p = r.player()
    n = math.hypot(dx, dz) or 1.0
    dx, dz = dx / n, dz / n
    paused(r, True)
    r.teleport(x, y, z)
    rows = ((dz, 0.0, -dx), (0.0, -1.0, 0.0), (dx, 0.0, dz))
    r.batch(*["poke %#x %#x" % (p + 0xB0 + 16 * k + 4 * i, f2u(v)) for k, row in enumerate(rows)
              for i, v in enumerate(row)], "poke %#x 0" % (p + P_SPEED))
    paused(r, False)


def cr_talk(r, name, back=1.0, lift=0.5):
    """The giver NAME (a script NPC object's name): the skater put BACK units in front of its
    data point facing it, LIFT up; CIRCLE. Returns the mode after (3: its box)."""
    pts = getattr(r, "cr_points", None)
    if pts is None or r.u32(LEVEL) != getattr(r, "cr_points_level", None):
        pts = r.cr_points = level_points(r)
        r.cr_points_level = r.u32(LEVEL)
    h = name_hash(name)
    if h not in pts:
        r.log("talk %s: no data point %#x" % (name, h))
        return None
    cls, (x, y, z), (dx, dz) = pts[h]
    cr_boxes(r)
    # in front of the point (its facing), turned toward it
    cr_put(r, x + back * dx, y - lift, z + back * dz, -dx, -dz)
    r.batch("wait 20", "press CIRCLE 6 30", "wait 30")
    mode = r.u32(MODE)
    r.log("talk %s at %.1f %.1f %.1f: mode %d, now at %s" % (name, x, y, z, mode,
                                                           " ".join("%.1f" % v for v in r.f32(POS))))
    return mode


def scene_objects(r, names):
    """Scene objects by name from a RAM dump: the scene's name-hash table pairs each name hash
    with the object's record {next, prev, node, component, class} (the class's name at +4: NPC,
    ScriptObj, AnimObj ...); the node's position at +0x10, its turn (a quaternion) at +0x20.
    {name: (class name, (x, y, z), (qx, qy, qz, qw))}."""
    path = os.path.join(nctl.ctl_dir(r.name), "objects.ram")
    r.batch("dump " + path)
    m = open(path, "rb").read()
    os.remove(path)
    u32 = lambda a: struct.unpack_from("<I", m, a & 0x1FFFFFF)[0]
    ok = lambda a: 0x100000 <= a < 0x2000000
    out = {}
    for name in names:
        key = struct.pack("<I", name_hash(name))
        i = m.find(key, 0x100000)
        while i >= 0 and name not in out:
            rec = u32(i + 4)
            if i % 4 == 0 and ok(rec) and ok(u32(rec + 4)) and u32(u32(rec + 4)) == rec and ok(u32(rec + 8)) \
                    and ok(u32(rec + 0x10)):
                node, cls = u32(rec + 8), u32(rec + 0x10)
                out[name] = (m[cls + 4:cls + 20].split(b"\0")[0].decode("latin-1", "replace"),
                             struct.unpack_from("<3f", m, node + 0x10), struct.unpack_from("<4f", m, node + 0x20))
            i = m.find(key, i + 4)
    return out


def cr_giver(r, name, dist=2.3):
    """Talk to the giver NAME (a level script's NPC object, the challenge bubble's): the skater
    put DIST units from the NPC's node on each side in turn (behind, in front, left, right of
    its turn), facing it, CIRCLE, until its box opens (mode 3). The box is left open (CROSS
    accepts). Returns True when one opened."""
    found = scene_objects(r, [name]).get(name)
    if not found:
        r.log("giver %s: no object" % name)
        return False
    cls, (x, y, z), q = found
    yaw = 2 * math.atan2(q[1], q[3])
    fx, fz = math.sin(yaw), math.cos(yaw)
    cr_boxes(r)
    for sx, sz in ((-fx, -fz), (fx, fz), (fz, -fx), (-fz, fx)):
        cr_put(r, x + dist * sx, y - 0.5, z + dist * sz, -sx, -sz)
        r.batch("wait 20", "press CIRCLE 6 30", "wait 30")
        if r.u32(MODE) == 3:
            r.log("giver %s (%s) at %.1f %.1f %.1f: box open, skater at %s" % (
                name, cls, x, y, z, " ".join("%.1f" % v for v in r.f32(r.player() + P_POSITION))))
            return True
    r.log("giver %s at %.1f %.1f %.1f: no box" % (name, x, y, z))
    return False


def cr_editing(r, timeout=1800):
    """A career's in-level editor running (the editor object, play mode)."""
    r.batch("until %#x!=0 %d" % (PE_EDITOR, timeout), "until %#x==2 %d" % (MODE, timeout), "wait 120")
    ok = r.u32(PE_EDITOR) != 0
    r.log("career editor: running" if ok else "career editor: NOT running", "editor %#x" % r.u32(PE_EDITOR))
    return ok


def cr_points(r):
    p = r.player()
    return struct.unpack("<i", struct.pack("<I", r.u32(p + 0x8D8)))[0] if p else 0


def cr_tool(r, kind, rounds=3):
    """(in a career's editor) The level's tool, ROUNDS times over: the left stick turns the
    camera and the right stick zooms each round.
    crane (Industrial, "X Pick, d-pad Move", carrying "X Release, O Rotate"): X at the cursor,
    then a square spiral out to 6 cells with X at each cell (a pick, a release where it may be
    dropped, the melt pit's ParkEditDestroyObject adding 5,000 to the target) and O on the way;
    vacuum (Airfield, "X Pick, O Rotate"): a snake of cells, X pick, two cells on, O, X release;
    pisser (Cannery: the piece dispenser "d-pad Change, Triangle Place", then "X Create,
    O Rotate, Triangle Selection, Square Delete, d-pad Move"): piece N (RIGHT N times), placed,
    created, rotated, moved, created again, one deleted, back to the dispenser."""
    target0 = cr_points(r)
    for k in range(rounds):
        if kind == "crane":
            r.batch("press CROSS 6 30")
            n, side = 1, 0
            dirs = ("RIGHT", "DOWN", "LEFT", "UP")
            while n <= 6:
                for _ in range(2):
                    for _ in range(n):
                        r.batch("press %s 6 10" % dirs[side % 4], "press CROSS 6 24")
                    side += 1
                r.batch("press CIRCLE 6 16")
                n += 1
        elif kind == "vacuum":
            for row in range(3):
                for col in range(4):
                    side = "RIGHT" if row % 2 == 0 else "LEFT"
                    r.batch("press CROSS 6 30", "press %s 6 12" % side, "press %s 6 12" % side,
                            "press CIRCLE 6 20", "press CROSS 6 30", "press %s 6 12" % side)
                r.batch("press DOWN 6 12", "press DOWN 6 12")
        elif kind == "pisser":
            for piece in range(4):
                for _ in range(piece + k):
                    r.batch("press RIGHT 6 24")
                r.batch("press TRIANGLE 6 50", "press UP 6 14", "press UP 6 14", "press CROSS 6 40",
                        "press CIRCLE 6 24", "press RIGHT 6 14", "press RIGHT 6 14", "press CROSS 6 40",
                        "press LEFT 6 14", "press SQUARE 6 40", "press DOWN 6 14", "press CROSS 6 40",
                        "press TRIANGLE 6 50")
        else:
            raise SystemExit("tool: crane, vacuum or pisser")
        r.batch("stick LX %s" % ("1" if k % 2 == 0 else "-1"), "wait 30", "stick LX 0",
                "stick RY %s" % ("1" if k % 2 == 0 else "-1"), "wait 20", "stick RY 0", "wait 10")
        r.log("career editor: %s round %d, target %d -> %d, mode %d, editor %#x" % (
            kind, k, target0, cr_points(r), r.u32(MODE), r.u32(PE_EDITOR)))
        if r.u32(PE_EDITOR) == 0:
            break


def cr_do_challenge(r):
    """(in a career's editor) START, Do challenge (the pause menu's second entry)."""
    paused(r, True)
    r.batch("wait 20", "press DOWN 6 20", "press CROSS 6 60", "until %#x==2 600" % MODE, "wait 60")
    r.log("do challenge: mode %d, editor %#x" % (r.u32(MODE), r.u32(PE_EDITOR)))


def cr_card_save(r):
    """(main menu, cursor on CAREER) OPTIONS > SAVE & LOAD > SAVE, card slot 1, YES to each
    question (NO is preselected: overwrite), back to the main menu."""
    top = r.u32(PANEL)
    for _ in range(5):
        r.press("RIGHT", gap=30)
    r.press("CROSS", gap=60)
    opts = r.new_panel(top)
    r.set_cursor(0)
    r.press("CROSS", gap=60)
    r.new_panel(opts)
    for _ in range(4):
        if r.panel_cursor()[1] in (0, None):
            break
        r.press("UP", gap=20)
    r.press("CROSS", gap=60)
    r.batch("wait 90", "press CROSS 6 60", "wait 150")
    for _ in range(3):
        if r.u32(MODE) not in (1, 3) or r.u32(PANEL) == opts:
            break
        r.batch("press UP 6 20", "press CROSS 6 60", "wait 240")
    for _ in range(6):
        if r.u32(PANEL) == top:
            break
        r.batch("press TRIANGLE 6 60", "wait 30")
    r.log("card save: panel %#x (main menu %#x)" % (r.u32(PANEL), top))


def cr_card_dialog(r, what="save", slot=0, answers=()):
    """(paths9; main menu, cursor on CAREER) OPTIONS > SAVE & LOAD > SAVE (or LOAD), card slot SLOT (0 or 1), then each of
    ANSWERS (a step: buttons joined by +, e.g. DOWN+CROSS) pressed in turn with 150 vsyncs after it: the dialogs a card
    that is unformatted, missing or failing puts up (the runner's ZX_MC_SLOT1/2, ZX_MC_FAIL); back to the main menu."""
    top = r.u32(PANEL)
    for _ in range(5):
        r.press("RIGHT", gap=30)
    r.press("CROSS", gap=60)
    opts = r.new_panel(top)
    r.set_cursor(0)
    r.press("CROSS", gap=60)
    r.new_panel(opts)
    for _ in range(4):
        if r.panel_cursor()[1] in (0, None):
            break
        r.press("UP", gap=20)
    if what == "load":
        r.press("DOWN", gap=20)
    r.press("CROSS", gap=60)
    r.batch("wait 90")
    for _ in range(slot):
        r.press("DOWN", gap=20)
    r.press("CROSS", gap=60)
    r.batch("wait 150")
    for step in answers:
        for b in step.split("+"):
            r.press(b, gap=40)
        r.batch("wait 150")
    for _ in range(8):
        if r.u32(PANEL) == top:
            break
        r.batch("press TRIANGLE 6 60", "wait 30")
    r.log("card dialog %s slot %d: panel %#x (main menu %#x)" % (what, slot + 1, r.u32(PANEL), top))


def menu_mash(r, presses=800, seed=1, wait=(12, 40)):
    """(paths9; from the main menu, for recording) PRESSES random button presses, seeded (SEED): the d-pad half of them, CROSS a quarter,
    TRIANGLE, START, SQUARE, CIRCLE, L1, R1 now and then, WAIT[0] to WAIT[1] vsyncs between: the menus' input handlers (keyboard, toggler,
    input group, menu, tree view) on whatever the presses reach, a game started and played on by the same presses if CROSS gets there.
    No closed loop: the routes that need a state (a level, a card) make it first."""
    import random
    rng = random.Random(seed)
    pool = ["UP"] * 3 + ["DOWN"] * 3 + ["LEFT"] * 3 + ["RIGHT"] * 3 + ["CROSS"] * 5 + ["TRIANGLE"] * 2 + ["START", "SQUARE", "CIRCLE", "L1", "R1"]
    cmds = []
    for i in range(presses):
        cmds.append("press %s 4 %d" % (rng.choice(pool), rng.randint(*wait)))
        if len(cmds) >= 40:
            r.batch(*cmds, timeout=600)
            cmds = []
    if cmds:
        r.batch(*cmds, timeout=600)
    r.log("menumash: %d presses (seed %d), mode %d panel %#x" % (presses, seed, r.u32(MODE), r.u32(PANEL)))


def cr_bank(r, points=60000.0):
    """(test only: a poke, as `challenge complete`) a pending trick score of POINTS poked and a
    jump landed, so it is banked: a career challenge's points (Construct and Score) reached."""
    p = r.player()
    before = r.u32(r.u32(p + P_SCORE) + 0x14)
    r.batch("poke %#x %#x" % (p + P_PENDING, f2u(points)), "poke %#x 1" % (p + P_PENDING_ON),
            "down UP", "wait 20", "press CROSS 3 10", "wait 60", "up UP", "wait 120")
    r.log("bank %d: score %d -> %d, challenge points %d" % (points, before, r.u32(r.u32(p + P_SCORE) + 0x14),
                                                           cr_points(r)))


CAREER3_COMMANDS = {
    "keys": lambda r, a: cr_keys(r, int(a[0], 0) if a else KEYS_ALL),
    "tweaks": lambda r, a: cr_tweaks(r, a[0].split(",") if a else ("juice", "invulnerable")),
    "boxes": lambda r, a: cr_boxes(r),
    "talk": lambda r, a: cr_talk(r, a[0], *(float(v) for v in a[1:3])),
    "giver": lambda r, a: cr_giver(r, a[0], *(float(v) for v in a[1:2])) and (r.batch("press CROSS 6 60", "wait 60") or True),
    "editing": lambda r, a: cr_editing(r),
    "tool": lambda r, a: cr_tool(r, a[0], *(int(v) for v in a[1:2])),
    "dochallenge": lambda r, a: cr_do_challenge(r),
    "cardsave": lambda r, a: cr_card_save(r),
    "menumash": lambda r, a: menu_mash(r, int(a[0]) if a else 800, int(a[1]) if len(a) > 1 else 1),
    "carddialog": lambda r, a: cr_card_dialog(r, a[0] if a else "save", int(a[1]) if len(a) > 1 else 0, a[2:]),
    "bank": lambda r, a: cr_bank(r, float(a[0]) if a else 60000.0),
    "points": lambda r, a: [r.log("point %04x %08x %8.2f %8.2f %8.2f dir %5.2f %5.2f" % ((c, h) + p + d))
                            for h, (c, p, d) in sorted(level_points(r).items(), key=lambda kv: kv[1][0])
                            if not a or c == int(a[0], 16)],
}


def start(name, extra):
    base = os.path.join(os.environ.get("ROUTES_DIR", "/tmp/routes"), name)
    shutil.rmtree(base, ignore_errors=True)
    for sub in ("home", "support", "logs", "shots"):
        os.makedirs(os.path.join(base, sub))
    os.makedirs(os.path.join(base, "mc0"))
    # ZX_ROUTES_CARD=DIR: a copy of a ready-made card (recomp/run/mc-sweep-park, ...) in slot 1
    for kv in extra:
        if kv.startswith("ZX_ROUTES_CARD="):
            shutil.copytree(os.path.expanduser(kv.split("=", 1)[1]), os.path.join(base, "mc0"), dirs_exist_ok=True)
    env = ["HOME=%s/home" % base, "ZX_MC_ROOT=%s/mc0" % base, "ZX_APP_SUPPORT_DIR=%s/support" % base,
           "ZX_APP_LOG_DIR=%s/logs" % base, "ZX_SCREENSHOT_DIR=%s/shots" % base,
           "ZX_CONTROLS_INI=%s/home/controls.ini" % base] + extra
    nctl.start(name, env)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    if argv[1] == "start":
        start(argv[2], argv[3:])
        return 0
    if argv[1] == "stop":
        nctl.stop(argv[2])
        return 0
    r = Route(argv[1])
    words = " ".join(argv[2:]).split(";")
    for w in words:
        if w.strip():
            run(r, w.split())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except nctl.RunnerGone:
        print("runner gone")
        sys.exit(2)
