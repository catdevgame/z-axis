# PCSX2 reference run 2: rest of the Tutorial, then Career in MOVIE LOT

Second scripted PCSX2 playthrough of Aggressive Inline (SLUS-20327), set up the
same way as run 1 (`REFERENCE-RUN.md`): savestate, atomic 32 MB EE RAM image and
frame thumbnail at every checkpoint, plus a native pad script per checkpoint.
It has two parts:

1. **Tutorial, lessons 9 to 18.** Continues run 1 from its last checkpoint
   (ref21-modgrind-go, slot 50), through Modifying Grinds, Air Tricks, Exit Pipes
   and Bowls, Bail Off A Pipe, Handplants, Vaulting, Vertical Poles, Horizontal
   Poles, Skitching and Wall Rides, to the start of "Cess Slides". The scripts
   stop there (see "Where scripted play stops").
2. **Career, MOVIE LOT.** Fresh boot, CAREER, level select (MOVIE LOT, the first
   career level), loading screen, fly-thru, pause menu, CHALLENGES list, a save to
   the memory card from the pause menu, then skating: a rail grind, a balanced
   manual and an ollie with a grab.

Everything runs through `pcsx2pad.py` on the private PCSX2 (PINE slot 28012, data
dir `tools/pcsx2-test-data`).

## Reproduce

Tutorial part (continues run 1's timeline: the savestate restores the vsync
counter, so the log carries on from run 1's):

```sh
cd tools
python3 pcsx2pad.py launch
python3 pcsx2pad.py loadstate 50                                   # ref21-modgrind-go
PCSX2PAD_RECORD=/tmp/ref2-tut.log python3 pcsx2pad.py run scripts/tutorial-run2.txt
python3 pcsx2pad.py quit
```

Career part (fresh boot with the memory card as it was before run 2; the run
writes a save to it):

```sh
cd tools
python3 pcsx2pad.py quit                                           # if running
cp pcsx2-test-data/reference/Mcd001-before-run2.ps2 pcsx2-test-data/PCSX2/memcards/Mcd001.ps2
python3 pcsx2pad.py launch
PCSX2PAD_RECORD=/tmp/ref2-career.log python3 pcsx2pad.py run scripts/career-all.txt
python3 pcsx2pad.py quit
```

The Tutorial part took 3 min 16 s of game time (11,734 vsyncs) and about 4 min
wall time. The Career part took 90 s of game time from boot (5,424 vsyncs) and
about 2 min wall time. Every segment passed first time in the final runs. Both
parts were also run once more before the final runs (Tutorial lessons 10-18 from
a different start, Career from a fresh boot), with all checkpoints reached.

## Scripts (`tools/scripts/`)

Game time is from the vsync counter at the segment's first and last checkpoint
(about 0.5 s of it per checkpoint is the savestate being written: the game keeps
running).

| Script | From | Does | Game time |
|---|---|---|---|
| `tutorial-09-modgrind.txt` | 50 | jump onto the rail, release TRIANGLE, tap LEFT+TRIANGLE: Frontside Grind + Soul Grind 2x, `balance grind` to the end of the rail | 9.3 s |
| `tutorial-10-air.txt` | 52 | tips; UP over the kicker (L10, x -13.6), LEFT+SQUARE in the air: grab, "Good job" | 17.1 s |
| `tutorial-11-bowl.txt` | 55 | tips; hold UP up the bowl face and over the lip | 16.3 s |
| `tutorial-12-bail.txt` | 57 | tips; `goto -4 8` up the quarterpipe at an angle, off its side at z > 13.3, CIRCLE: lands safely (without CIRCLE: "You cratered!") | 15.6 s |
| `tutorial-13-handplant.txt` | 59 | tips; UP to the face, release, hold TRIANGLE 24 frames (Hand Plant), CROSS back in | 15.5 s |
| `tutorial-14-vault.txt` | 61 | tips; hold UP+CIRCLE into the waist-high edge (vault) | 16.4 s |
| `tutorial-15-vpole.txt` | 63 | tips; hold UP+CIRCLE into the pole (quick turn round it) | 19.6 s |
| `tutorial-16-hpole.txt` | 65 | tips; jump released at x > -10 with CIRCLE held: hang from the bar (y -4.47), let go | 15.3 s |
| `tutorial-17-skitch.txt` | 68 | tips; wait for the golf cart to pass, `chase` it and hold CIRCLE: Skitch, "Good Job" | 45.4 s |
| `tutorial-18-wallride.txt` | 71 | tips; jump at the wall at 45 degrees, DOWN+CROSS, hold CROSS: Wall Ride; then the "Cess Slides" tips | 25.1 s |
| `tutorial-run2.txt` | 50 | segments 9-18 in order | 195.8 s |
| `career-01-menus.txt` | fresh boot | movies skipped, main menu, CAREER, player select, name (END), level select, RIGHT to MOVIE LOT | 25.5 s after boot (17.8 s after the first pad poll) |
| `career-02-loadin.txt` | 77 | start MOVIE LOT, loading, fly-thru, placed on the footbridge; pause menu, CHALLENGES, back; SAVE, slot 1, YES, saving, back to pause | 51.6 s |
| `career-03-skate.txt` | 86 | resume; down the footbridge ramp onto the right-hand rail (Frontside Grind 345), along the median, manual (`balance manual`), ollie + Method Grab (Single Toe Manual + Method Grab 2x, 2,215 points), pause | 13.4 s |
| `career-all.txt` | fresh boot | segments 1-3 | 90.5 s after boot |

### Tool additions (`pcsx2pad.py`)

| Verb / change | What |
|---|---|
| `balance grind\|manual FRAMES [KD] [EXTRA]` | holds a grind (LEFT/RIGHT) or manual (UP/DOWN) with a PD rule on the balance meter in the player object (+0x280 rails, +0x284 manuals): press the correcting button when `bal + KD * (change per frame)` is past +-0.02, KD 24. Stops when the meter and the combo points both read 0 for 10 frames (combo banked or lost). Without it, a grind after a switch falls off within ~35 frames and a manual within ~100 |
| `chase ADDR BACK R [TIMEOUT] [GRAB] [UNTIL_Y]` | closed-loop pursuit of a moving object (vec3 at ADDR): first to a point BACK units behind it (opposite its measured motion), then at its back, holding GRAB within R; returns with GRAB held when the player's y drops below UNTIL_Y (lifted onto it) |
| `save_state` retry | PCSX2 cancels a savestate while the memory card is busy ("The memory card is busy, so the state save operation has been cancelled to prevent data loss"), during a write and for a few seconds after. `savestate` / `checkpoint` now re-request it every 4 s (up to 8 times) until the file appears |

## Where scripted play stops

**Tutorial: at the start of "Cess Slides" (ref46).** The level has 25 lessons (its
script objects are numbered `..._L2` to `..._L25`, e.g. `Scriptobj_qpipe_1_L19`,
`Scriptobj_rail_3_L23`, `ScriptObj_Davehelper_L24`, `Scriptobj_qpipe_Lcess25`), so
about 6 lessons remain after Cess Slides; the end of the Tutorial was not reached.
The Cess Slides goal is "By using cess slides and manuals, perform a 5,000 point
series of tricks chained together." Its two quarterpipes (`qpipe_1/2_L19` at
x 4.53, z 4.32 and -8.85) face each other and form a small halfpipe. What was
tried:

- a single manual held with `balance manual` while steering a circle and banked
  with an ollie at 5,053 points (score 3,766 to 8,872): banked, but the goal does
  not complete (it wants several tricks);
- vert air on qpipe_1, R2 on landing (cess slide, counts as a trick when landing
  in a pipe), manual across, up the other pipe, repeated: the HUD shows
  "Cess Slide + Single Toe Manual + Cess Slide + Single Toe Manual, 2x", but each
  pass adds only ~200 points and the multiplier rises slowly (602 points banked as
  1,806), and the closed-loop line across the 11-unit-wide halfpipe drifts in x
  until the skater misses a face or flies off its side;
- flat R2 (not a trick), a jump + grab landed into a manual and an ollie from one
  manual into another: each banks separately.

So this goal would need a much better controller (or a human). Everything up to it
replays reliably.

**Career: after the first trick line in MOVIE LOT.** Not attempted: the CHALLENGES.
"Skitch a bus" looks feasible with `chase` but the transit buses on the reachable
lanes run at 0.417 to 0.583 units/frame and the skater tops out at ~0.39 on the
flat, so it can only catch one when it stops (not observed). The juice pickups
near the start float 10 to 25 units up (PropDef_PowerJuiceRefill at
(233.78, -9.72, -146.65), (314.04, -25.86, -153.48)), out of reach of a flat
ollie. No NPC is needed for the listed challenges; the NPC nodes (`NPC_HAMLET`,
`NPC_ELVIS`, `NPC_MIME`, ...) exist but were not located in the world.

## What the game shows

- **End of a Tutorial lesson**: on-screen "Nice Job" / "Good job" (no box), then
  the next lesson's title box, or a HINT box first. XP awards appear in the corner
  ("GRIND XP +3", "WALLRIDE XP +6"). Juice does not drain in the Tutorial (31.00 at
  every lesson start).
- **MOVIE LOT start**: "Loading..." screen (an AI logo in the final run; in an
  exploratory session it showed a skater card, "SHANE", instead), then a fly-thru of the studio lot (gate,
  sound stages, ~500 level frames), then the skater stands on a footbridge over a
  divided boulevard, score 0. No tip boxes. Buses, cars and a pizza truck drive on
  the road below.
- **Juice**: the bar top left. It starts at 25 and drains 2 per second (1 per 30
  frames) while the skater stands still; it does not drain while skating or paused,
  and tricks add to it (grind + manual + grab: 22.5 to 34.9). At 0: "GAME OVER /
  CONTINUE FOR 0 PTS / EXIT / You ran out of Juice." Continue puts the skater
  back on the footbridge with juice refilled. Idle from the start, game over comes
  700-780 frames after the player is placed.
- **Pause menu** (START): PAUSE, RESUME GAME, CHALLENGES, ATTRIBUTES, SAVE, VIEW
  LOOT, TRICK LIST, more below (not opened). TRIANGLE back, X select.
- **CHALLENGES**: GET 100,000 POINTS (1 pt), 20,000 POINT TRICK (1), 5 BIKE RACKS IN
  1 TRICK (2), SKITCH A BUS (1), GRIND 2 TRAFFIC LIGHTS (3), GET ON THE GUARD HOUSE
  PIPE (1), GRIND CLIFF EDGE (2), more below; some entries have a camera icon
  (presumably the ones with a fly-thru, O). Buttons: X DETAIL, R1 SUMMARY, TRIANGLE BACK, O FLY-THRU.
- **ATTRIBUTES** (seen while exploring, no checkpoint): JUMP, SPEED, SPIN, GRIND,
  MANUAL, FAKIE, WALLRIDE levels, LEVEL 1, XP 0/500.
- **Memory card save** (pause, SAVE): "SAVE GAME / SELECT MEMORY CARD SLOT", MEMORY
  CARD SLOT 1 / MEMORY CARD SLOT 2; then "ARE YOU SURE YOU WANT TO SAVE 'AGGRESSIVE
  INLINE SKATERS' DATA TO MEMORY CARD (PS2) IN MEMORY CARD SLOT 1?" YES / NO (NO
  preselected); then "SAVING 'AGGRESSIVE INLINE SKATERS' / DO NOT REMOVE THE MEMORY
  CARD (PS2) FROM MEMORY CARD SLOT 1, RESET, OR TURN OFF THE CONSOLE." (~120
  frames), "SAVING COMPLETE." (~60 frames), back to PAUSE. With a save already on
  the card the second dialog is "'AGGRESSIVE INLINE SKATERS' DATA ALREADY EXISTS ON
  MEMORY CARD (PS2) IN MEMORY CARD SLOT 1. OVERWRITE OLD DATA?" OVERWRITE / CANCEL
  (CANCEL preselected). The same inputs (UP, X) work for both. No format or
  "no card" dialog appeared with the baseline's formatted card.

## Checkpoints

Savestate slots 51-91 in `pcsx2-test-data/PCSX2/sstates/`, named copies in
`pcsx2-test-data/reference/NAME.p2s`, RAM images in `recomp/dumps/ref2/NAME.ram`
(all git-ignored), thumbnails in `reference/pcsx2-run2/NAME.png` (PCSX2's 640x480
frame thumbnail from the savestate, reduced to 256 colours like run 1). The
"Saving" dialog cannot be savestated (memory card busy), so
`reference/pcsx2-run2/car10-saving-window.png` is a window capture of it instead
(cropped to 4:3, 640x480, PCSX2 OSD messages in the corner). Slot 85 holds no
run-2 checkpoint.

Values from the RAM images: vsync 0x231CA4, frame 0x231CAC, mode 0x276FE4, panel
0x240A20, level 0x24C9B4, score = zgui `Score` 0x836250, juice = zgui `CartonSize`
0x838F70, pos 0x2A76F0 (- = not placed / sentinel).

| Slot | Name | vsync | frame | mode | panel | level | score | juice | pos (x, y, z) |
|---|---|---|---|---|---|---|---|---|---|
| 51 | ref22-modgrind-rail | 12135 | 9645 | 2 | 0 | 7 | 811 | 31.00 | (0.11, -1.29, -0.33) |
| 52 | ref23-modgrind-done | 12537 | 10047 | 3 | 831bb0 | 7 | 2971 | 31.00 | (44.10, -0.01, -45.58) |
| 53 | ref24-air-go | 13116 | 10626 | 2 | 0 | 7 | 2971 | 31.00 | (-25.86, -0.01, -2.26) |
| 54 | ref25-air-grab | 13249 | 10759 | 2 | 0 | 7 | 2971 | 31.00 | (-6.00, -5.96, -0.88) |
| 55 | ref26-air-done | 13564 | 11074 | 3 | 831bb0 | 7 | 2971 | 31.00 | (45.79, -0.01, 35.89) |
| 56 | ref27-bowl-go | 14121 | 11631 | 2 | 0 | 7 | 2971 | 31.00 | (3.42, -0.16, -9.12) |
| 57 | ref28-bowl-done | 14542 | 12052 | 3 | 831bb0 | 7 | 2971 | 31.00 | (45.09, -0.01, 15.99) |
| 58 | ref29-bail-go | 14937 | 12447 | 2 | 0 | 7 | 2971 | 31.00 | (-25.86, -0.01, 0.00) |
| 59 | ref30-bail-done | 15475 | 12985 | 3 | 831bb0 | 7 | 2971 | 31.00 | (-40.12, -0.01, 51.94) |
| 60 | ref31-handplant-go | 15995 | 13505 | 2 | 0 | 7 | 2971 | 31.00 | (-27.38, -0.01, 0.00) |
| 61 | ref32-handplant-done | 16406 | 13916 | 3 | 831bb0 | 7 | 3310 | 33.90 | (-46.90, -1.40, 0.00) |
| 62 | ref33-vault-go | 17014 | 14524 | 2 | 0 | 7 | 3310 | 31.00 | (-25.86, -0.01, -2.27) |
| 63 | ref34-vault-done | 17391 | 14901 | 3 | 831bb0 | 7 | 3510 | 32.71 | (46.50, -0.01, -4.23) |
| 64 | ref35-vpole-go | 18051 | 15561 | 2 | 0 | 7 | 3510 | 31.00 | (-27.38, -0.01, -3.10) |
| 65 | ref36-vpole-done | 18565 | 16075 | 3 | 831bb0 | 7 | 3810 | 32.28 | (-49.62, -6.11, 2.36) |
| 66 | ref37-hpole-go | 18970 | 16480 | 2 | 0 | 7 | 3810 | 32.28 | (-25.86, -0.01, -0.05) |
| 67 | ref38-hpole-hang | 19093 | 16603 | 2 | 0 | 7 | 3810 | 32.28 | (-4.53, -4.47, -0.05) |
| 68 | ref39-hpole-done | 19483 | 16993 | 3 | 831bb0 | 7 | 3910 | 33.13 | (45.94, -0.01, -14.60) |
| 69 | ref40-skitch-go | 20214 | 17724 | 2 | 0 | 7 | 3910 | 31.00 | (-27.38, -0.01, 0.38) |
| 70 | ref41-skitching | 21880 | 19390 | 2 | 0 | 7 | 3910 | 31.00 | (31.55, 0.15, 33.67) |
| 71 | ref42-skitch-done | 22206 | 19716 | 3 | 831bb0 | 7 | 3910 | 31.00 | (-32.47, -0.79, 31.36) |
| 72 | ref43-wallride-go | 22663 | 20173 | 2 | 0 | 7 | 3910 | 31.00 | (-25.86, -0.01, 0.68) |
| 73 | ref44-wallride | 22825 | 20335 | 2 | 0 | 7 | 3910 | 31.00 | (-2.73, -1.65, 23.81) |
| 74 | ref45-wallride-done | 23103 | 20613 | 3 | 831bb0 | 7 | 4670 | 37.49 | (-32.51, -0.01, 50.69) |
| 75 | ref46-cess-go | 23713 | 21223 | 2 | 0 | 7 | 4670 | 31.00 | (-27.38, -0.01, 0.99) |
| 76 | car01-main-menu | 1122 | 462 | 1 | 8323e0 | 14 | - | - | - |
| 77 | car02-level-select-movielot | 1527 | 809 | 1 | 857d40 | 14 | - | - | - |
| 78 | car03-loading | 2076 | 0 | 2 | 0 | 5 | 0 | - | - |
| 79 | car04-flythru | 3207 | 200 | 2 | 0 | 5 | 0 | 25.00 | - |
| 80 | car05-level-start | 3508 | 487 | 2 | 0 | 5 | 0 | 24.37 | (302.18, -10.85, -153.86) |
| 81 | car06-pause | 3578 | 551 | 3 | 839d00 | 5 | 0 | 23.20 | same |
| 82 | car07-challenges | 3674 | 647 | 3 | 863e70 | 5 | 0 | 23.20 | same |
| 83 | car08-save-slot | 3939 | 882 | 3 | 8a8c40 | 5 | 0 | 23.20 | same |
| 84 | car09-save-confirm | 4045 | 988 | 3 | 8a8c40 | 5 | 0 | 23.20 | same |
| 86 | car11-save-done | 4622 | 1565 | 3 | 839d00 | 5 | 0 | 23.20 | same |
| 87 | car12-grind | 4829 | 1772 | 2 | 0 | 5 | 0 | 22.47 | (289.90, -8.87, -148.34) |
| 88 | car13-manual | 5043 | 1986 | 2 | 0 | 5 | 345 | 25.41 | (220.34, -0.31, -154.24) |
| 89 | car14-air-grab | 5260 | 2203 | 2 | 0 | 5 | 345 | 25.41 | (159.68, -1.55, -152.59) |
| 90 | car15-landed | 5354 | 2297 | 2 | 0 | 5 | 2560 | 34.86 | (133.38, -0.31, -151.88) |
| 91 | car16-pause-end | 5424 | 2367 | 3 | 839d00 | 5 | 2560 | 34.86 | (124.16, -0.31, -151.63) |

The Career front end lands on exactly the same vsyncs as run 1 (main menu at
1122, game frame 462), so the boot path is deterministic across runs. In MOVIE
LOT, vsync - frame goes from 3007 at the fly-thru to 3057 at the end, so a few
main-loop frames there took two vsyncs.

## Game-state addresses (new)

All in PCSX2 EE RAM. "Heap" addresses are PCSX2 values for these exact paths
(fresh boot, front end, the level); natively they are offset (run 1 found zgui
panels at -0x1290, see REFERENCE-RUN.md).

| Address | Type | Meaning | How confirmed |
|---|---|---|---|
| 0x0024C9B4 | u32 | **current level id**: index into the level table at 0x0024B8B0 (0x20-byte entries, name first): 0 CIVIC, 1 BRDWLK, 2 CANNERY, 3 HANGER, 4 INDUST, 5 MOVIE, 6 MUSEUM, 7 TUT, 8 ZOO, 9 LOCAL, 10-13 PE*, 14 MPCIVIC (the front end's background level). Set when loading starts | equals 7 in all 44 Tutorial RAM images of runs 1 and 2, 5 in all 13 MOVIE LOT images from the loading screen on, 14 at the main menu and level select in both runs. Static search over 0x100000-0x2B3B70 for a value constant per level, different between levels; the table is in the ELF (file offset 0x14C8B0) |
| 0x002795C8 | u32 | the same id one step behind (14 on the loading screen, then 5) | same images |
| 0x0024C860 | u32 ptr | player object (0 in the front end, set while the level loads): 0x971990 Tutorial, 0x972FF0 MOVIE LOT | all level images; the fields below |
| player obj +0x280 | f32 | **grind balance** (horizontal meter): drifts to +-1 and the skater falls off; RIGHT raises it, LEFT lowers it | logged on the rail with and without input; `balance grind` holds Frontside + Soul Grind to the end of the rail |
| player obj +0x284 | f32 | **manual balance** (vertical meter): with no input drifts negative, DOWN raises it, UP lowers it, crash at +-1. Gets faster the longer the manual lasts | logged per frame; the same PD rule (KD 24) held a manual for 1,563 frames (5,053 points) |
| player obj +0x8D8 | f32 | points of the current combo before the multiplier (manual 200/s; 0 once banked or bailed) | equals the HUD trick value (343 vs 353 on screen), resets to 0 at bank/bail |
| player obj +0x7AC | u32 | score, Tutorial only (0x97213C there: 0, 186, 811, ..., 4670). In MOVIE LOT the banked score is at 0x973754 (obj+0x764) instead, so use the zgui variable below | run 1 and 2 Tutorial images; MOVIE LOT images |
| 0x00836250 | u32 | **score shown in the HUD**: zgui script variable `Score` (copies: 0x8361E0, `CurValue` 0x8362C0). Heap, same address in the Tutorial and MOVIE LOT. The variable record has the name at value-0x20 | 186 / 811 at ref17 / ref20, 4670 at ref46 (HUD 4,670), 2560 at car15 (HUD 2,560) |
| 0x00838F70 | f32 | **juice** (the level timer): zgui script variable `CartonSize` (name at 0x838F50). MOVIE LOT: 25 at the fly-thru, -2/s standing, frozen while paused or skating, raised by tricks, game over at 0. Tutorial: 31 at every lesson start | polled per 60 frames to game over (28 -> 0 in 840 frames, at 0 the "You ran out of Juice" screen); search for floats with that ratio that are exactly 0 at game over found only this variable and two copies |
| zgui panel +0x114 | u32 | **list cursor** of the active zgui menu: level select 0x857E54 (0 TUTORIAL, 1 MOVIE LOT, 2 CIVIC CENTER), pause menu 0x839E14 (0 RESUME GAME, 1 CHALLENGES, 2 ATTRIBUTES, 3 SAVE, 4 VIEW LOOT, 5 TRICK LIST) | RAM diffs over RIGHT/LEFT and DOWN/UP presses; used by the scripts |
| 0x00240A20 | u32 ptr | zgui panel (run 1), new values: 0x839D00 pause menu, 0x863E70 CHALLENGES, save game dialogs 0x83FAA0 or 0x8A8C40 (depends on what was opened before), 0 while the card is written | per-frame logs in MOVIE LOT |
| 0x00277488 | u32 | **scripted camera on**: 1 during the MOVIE LOT fly-thru and during Tutorial lesson intros (title box and tips), 0 in normal play and in the pause menu. This is the nearest thing to a camera mode found; no separate follow-camera mode variable was found | constant 0 over 10 play checkpoints of both levels, 1 at car04-flythru; run 1's intro observations |
| 0x002732FC | u32 | Tutorial progress gauge (run 1): +1 per lesson through all 18 lessons played, 13 at Talking to people to 30 at Cess Slides. No true lesson index was found | rises by exactly 1 from each lesson's "go" checkpoint to the next, over runs 1 and 2 |
| 0x002A76F0 | f32 x3 | player position (run 1), also valid in MOVIE LOT (spawn (302.18, -10.85, -153.86); (0, 20000, 0) until placed, through the fly-thru) | car05-car16 |
| node 0x712C10 (+0x40) | f32 x3 | Tutorial golf cart (`Car_GolfCart`), translation at 0x712C50: drives a loop round the yard, ~0.25 units/frame, period ~1,110 frames | tracked for 1,500 frames; the `chase` target |
| 0x659A50 | node | MOVIE LOT player skin node (`Skin_Object_grob`; Tutorial 0x6F9E00) | translation matches 0x2A76F0 |

Also seen, not needed by the scripts: MOVIE LOT transit buses `Car_TransitBus01..04`
(nodes 0x700730, 0x700100, 0x6FFAD0, 0x6FF4A0, translation at +0x40) on lanes
z -137.5 / -142 (towards -x) and -169.5 (towards +x); juice and attribute pickups
are prop instances whose record points at `PropDef_PowerJuiceRefill` (0x6212A0, 44
instances), `PropDef_PowerJuiceHalffill` (0x620F90, 39), `PropDef_IconGrind`
(0x6211C0, 4), `PropDef_KeyMuseum` (0x621700, 1), with the instance's world
translation 0xCC bytes before that pointer.

Not found: an objective / challenge completion state (no challenge was completed),
a timer other than juice (MOVIE LOT has no clock on screen), and a camera mode
beyond the scripted-camera flag.

## Native pad scripts

`scripts/native/<checkpoint>.padscript` for every checkpoint, made with
`padscript` (seconds after the first pad poll, `<sec>:<BUTTON>:<dur>`):

- `ref22-...` to `ref46-...`: run 1's recorded inputs up to ref21
  (`ref-run1-input.txt`) followed by run 2's (`ref-run2-tutorial-input.txt`). The
  run-2 Tutorial part was played from the slot-50 savestate, whose restored vsync
  counter continues run 1's timeline, so the joined log is one continuous input
  stream from the first pad poll. Up to 8.9 KB.
- `car01-...` to `car16-...`: from `ref-run2-career-input.txt` (fresh boot). The
  front-end part is identical to run 1's (`car01` = `ref01`). Up to 1.2 KB.

As in run 1, all inputs are buttons, and the closed-loop parts (`goto`, `chase`,
`balance`) are recorded as the buttons they pressed, so they replay exactly only
if the native game state matches PCSX2 frame for frame. Use them with
`PS2X_PAD_SCRIPT_CLOCK=vsync`.
