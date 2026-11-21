# Aggressive Inline's tuning table: every entry

Aggressive Inline's tuning table as the debug menu's editor and `tuning.ini` see it (how to use both: DEVTOOLS.md "How to
use"). DM2 has none; BMX XXX's PS2 release has none either, and its `tuning.ini` takes its GameCube release's TUNING
rows (MODS.md "tuning.ini"). [V] = verified in the running game or the code, [I] = inferred from a name.

## Layout and how the game reads it [V]

- **Layout.** Categories at 0x24AD28: `{entries pointer, count, name[32]}`,
  0x28 bytes, 20 of them, ended by one named "None". Entries are 0x30
  bytes: `name[32], value, max, min, step` (floats), 405 of them from
  0x246138 to 0x24AD48 (BoobTech and Mem Check point at the same three).
  The debug menu's editor lists exactly this.
- **How the game reads it.** As `lw r, 0x24AD28+0x28k; lwc1 f, off(r)`, so
  no code holds an entry's address; the readers below were found by
  following those loads in the decompilation's assembly (decomp/asm) and
  then counting calls of each reader function in a run (a temporary probe
  in the runner: front end, Tutorial load, about 45 s of skating, a pause;
  not committed).
- **Live or copied.** Almost every entry is read straight from the table
  when it is used: a write (tuning.ini, the debug menu, a future setting)
  takes effect at the next read. The exceptions found:
  - `FOV`: read by 0x18C828 when the camera mode is set (0x18FD68 ->
    0x18FCA0: the level's fly-in, leaving the debug menu, the R1 camera),
    then kept in the camera. A FOV setting has to write the entry and set
    the camera again (call the camera-mode switch, or hook 0x18FCA0).
  - `Far Clip`: read every frame by the camera update 0x18FA00, but
    rewritten by the level camera setup 0x1901C0 at every level start
    (from the level's Globals, or the `MP ... Far Clip` entries in split
    screen). tuning.ini re-writes it every vsync.
  - `Normal Blur`, the `MP ... Far Clip` and fog: copied at level start
    (0x1901C0: the FSMotionBlur object and the camera).
- Read column below: *every frame* (a reader ran about once per vsync in
  play), *while it applies* (per frame in some states: on ramps, turning,
  grinding), *on the event* (a trick, crash, impact, juice award),
  *level start*, *camera mode set*, *not reached in the test* (its reader
  never ran in the Tutorial test: other levels, tricks, modes),
  *never read* (no code loads it: 19 entries, e.g. `Render Speed %`,
  `Show Skater Position`, the Competitor block, `Tap Dampen/Side/Timeout`).

## Entries

Default, range and step are the ELF's; Read is how the game reads it (see
above); Notes say what changing it does where it was tried, else what the
name suggests [I].

#### General (0x246138, 7)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Death Plane` | 100 | -500 to 500 (10) | every frame | height below which the skater is reset [I] |
| `Respawn Time` | 2 | 0 to 500 (0.1) | every frame |  |
| `Render Speed %` | 100 | 0 to 1000 (1) | never read | no code reads it (slow motion compiled out?) |
| `Show Skater Position` | 1 | 0 to 1 (1) | never read | no code reads it |
| `Rigid Body Stop Speed` | 15 | 0.15 to 40 (0.05) | not reached in the test |  |
| `Death by Juice` | 1 | 0 to 1 (1) | every frame | an empty juice meter ends the run [I]; Gameplay > Out of Juice |
| `Points/Continue` | 100000 | 100 to 2e+09 (100) | not reached in the test | points that buy a continue [I] |

#### FX (0x246318, 12)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Smoke Blast Size` | 0.7 | 0.1 to 10 (0.05) | not reached in the test |  |
| `Smoke Blast YVel` | 12 | 0 to 50 (1) | not reached in the test |  |
| `Smoke Blast Spread` | 0.25 | 0 to 5 (0.05) | not reached in the test |  |
| `Smoke Blast EmitFreq` | 1 | 1 to 6 (1) | not reached in the test |  |
| `Smoke Blast Color1 Time` | 1 | 1 to 800 (1) | not reached in the test |  |
| `Smoke Blast Color2 Time` | 150 | 2 to 800 (1) | not reached in the test |  |
| `Smoke Blast Color3 Time` | 300 | 3 to 800 (1) | not reached in the test |  |
| `Smoke Blast Color4 Time` | 800 | 4 to 800 (1) | not reached in the test |  |
| `Smoke Blast Scale Rate` | 0.006 | 0.001 to 0.1 (0.001) | not reached in the test |  |
| `Smoke Blast Fade Rate` | 128 | 0 to 512 (1) | not reached in the test |  |
| `Smoke Blast Ground Height` | -2 | -50 to 50 (1) | not reached in the test |  |
| `Impact Ring Speed` | 40 | 10 to 500 (1) | on the event |  |

#### Camera (0x246558, 43)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Distance` | 4 | 0.5 to 50 (0.5) | every frame | **tried**: 7 (tuning.ini) puts the camera clearly further back; Gameplay > Camera Distance scales it |
| `Distance HPole` | 6 | 0.5 to 50 (0.5) | every frame | Gameplay > Camera Distance scales it |
| `Height` | 0.45 | 0.05 to 50 (0.05) | every frame | **tried**: 1.2 raises the camera; Gameplay > Camera Height scales it |
| `Char Offset` | 1.4 | 0.5 to 50 (0.05) | every frame | height of the look-at point above the skater [I]; candidate with Height |
| `Char Offset Skcth` | 0.25 | 0.5 to 50 (0.05) | every frame |  |
| `Falling Base` | 2.75 | 0 to 5 (0.01) | every frame |  |
| `Falling Speed` | 0.12 | 0 to 5 (0.01) | every frame |  |
| `FOV` | 75 | 1 to 80 (0.5) | camera mode set | **tried**: 50 in the debug menu narrows the view; copied into the camera when its mode is set (level fly-in, leaving the debug menu, R1 camera), so a setting must also re-set the camera; Gameplay > Field of View leaves it alone and draws the game's view with its own field of view |
| `Type` | 0 | 0 to 4 (1) | camera mode set | camera mode the debug menu applies every frame (0x18FD68) |
| `Far Clip` | 1000 | 5 to 9999 (10) | every frame | the game writes it at level start (from the level, or the MP entries); **tried**: 400 in tuning.ini (held by the vsync re-write); the Draw Distance setting scales the far clip where the game sets it, not this entry |
| `MP Boardwalk FOV` | 52 | 1 to 80 (0.5) | camera mode set | split screen FOV; candidate |
| `MP Civic FOV` | 60 | 1 to 80 (0.5) | camera mode set | split screen FOV |
| `MP Movie FOV` | 60 | 1 to 80 (0.5) | camera mode set | split screen FOV |
| `MP Museum FOV` | 54 | 1 to 80 (0.5) | camera mode set | split screen FOV |
| `MP Brdwalk Far Clip` | 100 | 5 to 9999 (10) | level start | split screen draw distance (the Draw Distance setting scales the far clip made from it) |
| `MP Civic Far Clip` | 100 | 5 to 9999 (10) | level start |  |
| `MP Movie Far Clip` | 100 | 5 to 9999 (10) | level start |  |
| `MP Museum Far Clip` | 150 | 5 to 9999 (10) | level start |  |
| `Tap Dampen` | 0.1 | 0 to 1 (0.01) | never read |  |
| `Tap Side` | 6 | 0 to 20 (0.5) | never read |  |
| `Tap Timeout` | 0.7 | 0 to 4 (0.01) | never read |  |
| `Grind Slerp` | 0.1 | 0 to 4 (0.01) | every frame |  |
| `QPipe Dampen` | 0.52 | 0 to 4 (0.01) | every frame |  |
| `QPipe Offset` | 0 | -4 to 4 (0.01) | every frame |  |
| `Spine Dampen` | 0.021 | 0 to 1 (0.001) | every frame |  |
| `R1 Distance` | 3 | 1 to 50 (0.1) | not reached in the test | a second (R1) camera [I] |
| `R1 FOV` | 90 | 1 to 160 (0.5) | camera mode set | R1 camera; also passed to 0x18FCA0 on camera switch |
| `R1 Height` | 0.65 | -50 to 50 (0.05) | not reached in the test |  |
| `R1 Char Offset` | 1.25 | 0.5 to 50 (0.05) | not reached in the test |  |
| `Replay Type` | 3 | 0 to 4 (1) | not reached in the test | camera of a cut replay mode [I] |
| `Vert Lookaround` | 39.9 | 0 to 150 (0.1) | every frame | stick look-around limit, degrees [I] |
| `Horz Lookaround` | 60 | 0 to 150 (1) | every frame | stick look-around limit, degrees [I] |
| `Invert Vert Look` | 1 | 0 to 1 (1) | every frame | stick look inversion; candidate (stick camera) |
| `Invert Horiz Look` | 0 | 0 to 1 (1) | every frame | stick look inversion; candidate |
| `VPole Rotate` | 3100 | 0 to 10000 (10) | every frame |  |
| `VPole Rotate Time` | 0.8 | 0 to 5 (0.05) | every frame |  |
| `HPole Char Offset` | 0.25 | 0.5 to 50 (0.05) | every frame |  |
| `Grind Height` | 1.8 | 0.05 to 50 (0.05) | every frame | Gameplay > Camera Height scales it |
| `Grind Distance` | 3.5 | 0.5 to 50 (0.5) | every frame | Gameplay > Camera Distance scales it |
| `Grind COffset` | 1.1 | 0.5 to 50 (0.05) | every frame |  |
| `Horiz Offset` | 1 | 0 to 10 (0.1) | every frame |  |
| `Horiz Offset Dampen` | 0.1 | 0 to 1 (0.01) | every frame |  |
| `Normal Blur` | 65 | 0 to 255 (1) | level start | motion blur alpha, copied at level start; the Motion Blur setting changes the blend where it is drawn, not this entry |

#### Physics (0x246D68, 44)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Gravity` | 65 | 0 to 300 (0.5) | while it applies | modding only |
| `Acceleration` | 25 | 0 to 45 (0.5) | while it applies |  |
| `Jump` | 16 | 0 to 45 (0.5) | every frame | jump strength; modding only (effect not measured) |
| `MaxSpeed` | 16 | 0 to 145 (1) | every frame | top speed; modding only |
| `Min Move Dist` | 0.001 | 0 to 0.2 (0.001) | never read |  |
| `Forward Friction` | 0 | 0 to 1 (0.001) | while it applies |  |
| `Over Forward Friction` | 0.001 | 0 to 1 (0.001) | while it applies |  |
| `Side Friction` | 1 | 0 to 1 (0.01) | while it applies |  |
| `Step Height` | 0.5 | 0 to 5 (0.1) | every frame |  |
| `Min Ray Length` | 0.0001 | 0 to 1 (0.0001) | never read |  |
| `Hover Percent` | 0.01 | 0 to 0.25 (0.001) | never read |  |
| `Bounce Angle` | 0.1 | 0 to 1 (0.01) | while it applies |  |
| `Bounce Angle QPipe` | 0.7 | 0 to 1 (0.01) | while it applies |  |
| `Collision V Min%` | 0.25 | 0 to 1 (0.01) | while it applies |  |
| `Collision V Max%` | 0.7 | 0 to 1 (0.01) | while it applies |  |
| `Collision V Angle` | 45 | 0 to 90 (0.1) | while it applies |  |
| `Collision V% Air` | 0.9 | 0 to 1 (0.01) | while it applies |  |
| `Air Transition Time` | 0.1 | 0 to 2 (0.01) | while it applies |  |
| `Min Gravity %` | 0.2 | 0.01 to 1 (0.01) | while it applies |  |
| `Max Gravity Speed` | 5 | 0 to 100 (1) | while it applies |  |
| `Turn Rate` | 125 | 0 to 500 (5) | while it applies |  |
| `Man Turn Rate` | 80 | 0 to 500 (5) | not reached in the test |  |
| `Slide Turn Rate` | 250 | 0 to 500 (5) | while it applies |  |
| `Slide Turn Brake` | 5 | 0 to 500 (1) | while it applies |  |
| `Min Auto 180 Speed` | 0 | 0 to 500 (0.2) | not reached in the test |  |
| `Max Auto 180 Angle` | 1 | 0 to 1 (0.002) | not reached in the test |  |
| `Gravity Hill Factor` | 0.45 | 0 to 1 (0.01) | while it applies |  |
| `Brake Rate` | 25 | 0 to 45 (0.5) | while it applies |  |
| `Idle Velocity` | 3 | 0 to 45 (0.5) | while it applies |  |
| `Accel Speed Diff` | 8 | 0 to 45 (0.5) | while it applies |  |
| `Bounce Vector Angle` | 3 | 0 to 89 (1) | every frame |  |
| `QPipe Gravity` | 40 | 0 to 89 (1) | while it applies |  |
| `Spine Gravity` | 60 | 0 to 89 (1) | while it applies |  |
| `Air Rotate` | 320 | 0 to 6000 (2) | every frame |  |
| `Landing Angle` | 0.3 | 0 to 1 (0.01) | while it applies |  |
| `Head Ray Length` | 0.75 | 0 to 2 (0.01) | every frame |  |
| `Acid Drop Time` | 2 | 0 to 10 (0.1) | while it applies |  |
| `Fakie Mult` | 0.85 | 0 to 1 (0.001) | every frame |  |
| `Bowl Jumpout` | 0.3 | 0 to 1 (0.001) | on the event |  |
| `Bowl Jumpout Time` | 0.2 | 0 to 5 (0.001) | every frame |  |
| `Cant Stand Angle` | 0.4 | 0 to 1 (0.01) | while it applies |  |
| `Turndown Min Angle` | 0.85 | 0 to 1 (0.01) | every frame |  |
| `Turndown Min Speed` | 12.1 | 0 to 90 (0.1) | every frame |  |
| `Turndown Speed` | 90 | 0 to 1000 (1) | every frame |  |

#### Skater (0x2475A8, 20)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Jump Trick XP` | 3 | 0 to 200 (1) | on the event |  |
| `Jump Speed Add` | 0.2 | 0 to 200 (0.1) | every frame |  |
| `Top Speed XP` | 5 | 0 to 200 (1) | on the event |  |
| `Top Speed Speed Add` | 0.3 | 0 to 200 (0.1) | every frame |  |
| `Top Speed Juice Add` | 1.25 | 0 to 200 (0.1) | every frame |  |
| `Top Speed Juice Speed Add` | 0.3 | 0 to 200 (0.01) | every frame |  |
| `Spin XP` | 12 | 0 to 200 (1) | on the event |  |
| `Spin Add` | 25.6 | 0 to 200 (1) | every frame |  |
| `Manual XP` | 2 | 0 to 200 (1) | on the event |  |
| `Manual Balance Add` | 1e-05 | 0 to 1 (1e-05) | every frame |  |
| `Manual Balance Init Add` | 0.001 | 0 to 1 (1e-05) | every frame |  |
| `Grind XP` | 2 | 0 to 200 (1) | on the event |  |
| `Grind Grind Add` | 9e-06 | 0 to 1 (1e-05) | every frame |  |
| `Grind Grind Init Add` | 0.0004 | 0 to 1 (1e-05) | every frame |  |
| `Grind Stall Add` | 7e-06 | 0 to 1 (1e-05) | every frame |  |
| `Grind Stall Init Add` | 0.0003 | 0 to 1 (1e-05) | every frame |  |
| `Fakie XP` | 10 | 0 to 200 (1) | on the event |  |
| `Fakie Add` | 0.015 | 0 to 1 (0.001) | every frame |  |
| `Wallride XP` | 9 | 0 to 200 (1) | on the event |  |
| `Wallride Add` | 0.8 | 0 to 1 (0.001) | every frame |  |

#### Control (0x247968, 10)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Controller Deadzone` | 0.1 | 0 to 1 (0.05) | every frame | the game's stick dead zone; candidate (next to the host stick threshold) |
| `Max Idle Velocity` | 3 | 0.5 to 5 (0.1) | never read |  |
| `Animation Blend Time` | 0.1 | 0 to 2 (0.01) | on the event |  |
| `# cmd delay frames` | 3 | 0 to 60 (1) | every frame |  |
| `Button Timeout` | 1 | 0.1 to 10 (0.01) | every frame |  |
| `Button Revive Timeout` | 0.25 | 0.01 to 0.5 (0.01) | every frame |  |
| `Cessslide Button Timeout` | 1 | 0.1 to 10 (0.01) | every frame |  |
| `Acid Drop Timeout` | 0.1 | 0.01 to 5 (0.01) | not reached in the test |  |
| `Ground Manual Timeout` | 0.5 | 0.01 to 999 (0.01) | every frame |  |
| `Min Grind (Time) Length` | 0.6 | 0.001 to 1 (0.001) | not reached in the test |  |

#### Scoring: Juice (0x247B48, 6)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Juice:Score ratio` | 0.008 | 0 to 100 (0.0001) | while it applies |  |
| `Award is Premultiplied` | 1 | 0 to 1 (1) | while it applies |  |
| `Juice Lost/Level on crash` | 0.5 | 0.1 to 10 (0.1) | on the event | juice lost on a crash; Gameplay > Juice Lost on Crash scales it |
| `Max Juice Degradation/sec` | 2 | 0 to 20 (0.1) | every frame | juice drain; Gameplay > Juice Drain scales it |
| `Jizzed->Non-Jizzed Percent` | 0.6 | 0 to 1 (0.01) | every frame |  |
| `Max Juice Time` | 7.5 | 0 to 200 (0.1) | every frame | juice duration [I] |

#### Scoring: Grabs (0x247C68, 16)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Rocket Init. Worth` | 200 | 0 to 9999 (25) | not reached in the test |  |
| `Long Dick Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `Stale Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `LookBackJudo Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract Init. Worth` | 400 | 0 to 9999 (25) | on the event |  |
| `ParallelThat Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `Method Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `LuiKang Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket Worth/sec` | 250 | 0 to 999 (5) | not reached in the test |  |
| `Long Dick Worth/sec` | 250 | 0 to 999 (5) | not reached in the test |  |
| `Stale Worth/sec` | 250 | 0 to 999 (5) | not reached in the test |  |
| `LookBackJudo Worth/sec` | 250 | 0 to 999 (5) | not reached in the test |  |
| `Abstract Worth/sec` | 250 | 0 to 999 (5) | on the event |  |
| `ParallelThat Worth/sec` | 250 | 0 to 999 (5) | not reached in the test |  |
| `Method Worth/sec` | 250 | 0 to 999 (5) | not reached in the test |  |
| `LuiKang Worth/sec` | 250 | 0 to 999 (5) | not reached in the test |  |

#### Scoring: AirTricks (0x247F68, 15)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `FrontFlip Worth` | 800 | 0 to 9999 (25) | not reached in the test |  |
| `HotFlip Worth` | 900 | 0 to 9999 (25) | not reached in the test |  |
| `FlatspinClockwise Worth` | 1000 | 0 to 9999 (25) | not reached in the test |  |
| `Bio Worth` | 900 | 0 to 9999 (25) | not reached in the test |  |
| `BackFlip Worth` | 800 | 0 to 9999 (25) | not reached in the test |  |
| `MistyFlip Worth` | 1000 | 0 to 9999 (25) | not reached in the test |  |
| `FlatspinCounterClockwise Worth` | 1000 | 0 to 9999 (25) | not reached in the test |  |
| `BeraniMcTwistFlip Worth` | 1200 | 0 to 9999 (25) | not reached in the test |  |
| `CESupermanLateFlip Worth` | 7500 | 0 to 9999 (25) | not reached in the test |  |
| `MLDoubleBackFlip Worth` | 8000 | 0 to 9999 (25) | not reached in the test |  |
| `TKAbstractDoubleBackFlip Worth` | 9000 | 0 to 9999 (25) | not reached in the test |  |
| `JGBio900 Worth` | 9000 | 0 to 9999 (25) | not reached in the test |  |
| `SB1260Corkscrew Worth` | 10000 | 0 to 9999 (25) | not reached in the test |  |
| `SYDoubleFlatSpin Worth` | 12000 | 0 to 9999 (25) | not reached in the test |  |
| `EY1440 Worth` | 11000 | 0 to 9999 (25) | not reached in the test |  |

#### Scoring: ComboTricks (0x248238, 64)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Rocket FrontFlip Init. Worth` | 2000 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket HotFlip Init. W` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket FlatspinClockwise Init. ` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket Bio Init. Worth` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket BackFlip Init. Worth` | 2000 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket MistyFlip` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket FlatspinCounterClockwise` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket BeraniMcTwistFlip Init. ` | 2800 | 0 to 9999 (25) | not reached in the test |  |
| `Stale FrontFlip Init. Worth` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Stale HotFlip Init. Wo` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Stale FlatspinClockwise Init. W` | 2600 | 0 to 9999 (25) | not reached in the test |  |
| `Stale Bio Init. Worth` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Stale BackFlip Init. Worth` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Stale MistyFlip` | 2600 | 0 to 9999 (25) | not reached in the test |  |
| `Stale FlatspinCounterClockwise ` | 2600 | 0 to 9999 (25) | not reached in the test |  |
| `Stale BeraniMcTwistFlip Init. W` | 3000 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract FrontFlip Init. Worth` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract HotFlip Init` | 2600 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract FlatspinClockwise Ini` | 2800 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract Bio Init. Worth` | 2600 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract BackFlip Init. Worth` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract MistyFlip` | 2800 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract FlatspinCounterClockw` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract BeraniMcTwistFlip Ini` | 3200 | 0 to 9999 (25) | not reached in the test |  |
| `Method FrontFlip Init. Worth` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Method HotFlip Init. W` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Method FlatspinClockwise Init. ` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Method Bio Init. Worth` | 2400 | 0 to 9999 (25) | not reached in the test |  |
| `Method BackFlip Init. Worth` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Method MistyFlip` | 2600 | 0 to 9999 (25) | not reached in the test |  |
| `Method FlatspinCounterClockw` | 2200 | 0 to 9999 (25) | not reached in the test |  |
| `Method BeraniMcTwistFlip Init. ` | 3000 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket FrontFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Rocket HotFlip Worth/s` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Rocket FlatspinClockwise Worth/` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Rocket Bio Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Rocket BackFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Rocket MistyFlip` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Rocket FlatspinCounterClockw` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Rocket BeraniMcTwistFlip Worth/` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale FrontFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale HotFlip Worth/se` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale FlatspinClockwise Worth/s` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale Bio Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale BackFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale MistyFlip` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale FlatspinCounterClockw ` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Stale BeraniMcTwistFlip Worth/s` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract FrontFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract HotFlip Wort` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract FlatspinClockwise Wor` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract Bio Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract BackFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract MistyFlip` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract FlatspinCounterClockw` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Abstract BeraniMcTwistFlip Wor` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method FrontFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method HotFlip Worth/s` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method FlatspinClockwise Worth/` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method Bio Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method BackFlip Worth/sec` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method MistyFlip` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method FlatspinCounterClockw` | 0 | 0 to 999 (5) | not reached in the test |  |
| `Method BeraniMcTwistFlip Worth/` | 0 | 0 to 999 (5) | not reached in the test |  |

#### Scoring: Grinds (0x248E38, 50)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Soul Init. Worth` | 100 | 0 to 9999 (25) | while it applies |  |
| `BackSlide Init. Worth` | 100 | 0 to 9999 (25) | on the event |  |
| `Miszou Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `FishBrain Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `Royale Init. Worth` | 100 | 0 to 9999 (25) | not reached in the test |  |
| `Makio Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `Pornstar Init. Worth` | 100 | 0 to 9999 (25) | not reached in the test |  |
| `Fastslide Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `TorqueSoul Init. Worth` | 400 | 0 to 9999 (25) | not reached in the test |  |
| `Soyale Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `X Init. Worth` | 400 | 0 to 9999 (25) | not reached in the test |  |
| `AcidSoul Init. Worth` | 400 | 0 to 9999 (25) | not reached in the test |  |
| `FreestyleFastslide Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `FreestyleBackslide Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `FreestyleFishbrain Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `FreestyleMakio Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `Wonder Init. Worth` | 400 | 0 to 9999 (25) | not reached in the test |  |
| `SweatStance Init. Worth` | 400 | 0 to 9999 (25) | not reached in the test |  |
| `Mistrial Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `Unity Init. Worth` | 400 | 0 to 9999 (25) | not reached in the test |  |
| `SF Combo Init. Worth` | 7500 | 0 to 9999 (25) | not reached in the test |  |
| `FM Combo Init. Worth` | 10000 | 0 to 9999 (25) | not reached in the test |  |
| `MS Combo Init. Worth` | 9000 | 0 to 9999 (25) | not reached in the test |  |
| `Double Grind Init. Worth` | 600 | 0 to 9999 (25) | not reached in the test |  |
| `Hand Grind Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `Soul Worth/sec` | 200 | 0 to 999 (5) | while it applies |  |
| `BackSlide Worth/sec` | 300 | 0 to 999 (5) | on the event |  |
| `Miszou Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |
| `FishBrain Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |
| `Royale Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |
| `Makio Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |
| `Pornstar Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |
| `FrontSlide Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |
| `TorqueSoul Worth/sec` | 400 | 0 to 999 (5) | not reached in the test |  |
| `Soyale Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |
| `X Worth/sec` | 400 | 0 to 999 (5) | not reached in the test |  |
| `AcidSoul Worth/sec` | 400 | 0 to 999 (5) | not reached in the test |  |
| `FreestyleFrontslide Worth/sec` | 500 | 0 to 999 (5) | not reached in the test |  |
| `FreestyleBackslide Worth/sec` | 500 | 0 to 999 (5) | not reached in the test |  |
| `FreestyleFishbrain Worth/sec` | 500 | 0 to 999 (5) | not reached in the test |  |
| `FreestyleMakio Worth/sec` | 500 | 0 to 999 (5) | not reached in the test |  |
| `Wonder Worth/sec` | 400 | 0 to 999 (5) | not reached in the test |  |
| `SweatStance Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |
| `Mistrial Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |
| `Unity Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |
| `SF Combo Worth/sec` | 100 | 0 to 999 (5) | not reached in the test |  |
| `FM Combo Worth/sec` | 100 | 0 to 999 (5) | not reached in the test |  |
| `MS Combo Worth/sec` | 100 | 0 to 999 (5) | not reached in the test |  |
| `Double Grind Worth/sec` | 600 | 0 to 999 (5) | not reached in the test |  |
| `Hand Grind Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |

#### Scoring: Stalls (0x249798, 10)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `HP Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract HP Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket HP Init. Worth` | 300 | 0 to 9999 (25) | not reached in the test |  |
| `Method HP Init. Worth` | 600 | 0 to 9999 (25) | not reached in the test |  |
| `Stale HP Init. Worth` | 600 | 0 to 9999 (25) | not reached in the test |  |
| `HP Worth/sec` | 100 | 0 to 9999 (25) | not reached in the test |  |
| `Abstract HP Worth/sec` | 200 | 0 to 9999 (25) | not reached in the test |  |
| `Rocket HP Worth/sec` | 200 | 0 to 9999 (25) | not reached in the test |  |
| `Method HP Worth/sec` | 400 | 0 to 9999 (25) | not reached in the test |  |
| `Stale HP Worth/sec` | 400 | 0 to 9999 (25) | not reached in the test |  |

#### Scoring: Manuals (0x249978, 8)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Double Heel Manual Init. Worth` | 50 | 0 to 9999 (25) | not reached in the test |  |
| `Single Heel Manual Init. Worth` | 50 | 0 to 9999 (25) | not reached in the test |  |
| `Double Toe Manual Init. Worth` | 50 | 0 to 9999 (25) | not reached in the test |  |
| `Single Toe Manual Init. Worth` | 50 | 0 to 9999 (25) | not reached in the test |  |
| `Double Heel Manual Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |
| `Single Heel Manual Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |
| `Double Toe Manual Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |
| `Single Toe Manual Worth/sec` | 200 | 0 to 999 (5) | not reached in the test |  |

#### Scoring: Wall Tricks (0x249AF8, 4)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Wall Tap Init. Worth` | 100 | 0 to 9999 (25) | not reached in the test |  |
| `Wall Ride Init. Worth` | 500 | 0 to 9999 (25) | not reached in the test |  |
| `Wall Tap Worth/sec` | 50 | 0 to 999 (5) | not reached in the test |  |
| `Wall Ride Worth/sec` | 300 | 0 to 999 (5) | not reached in the test |  |

#### Grind/Walls (0x249BB8, 50)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Min Grind Angle` | 0.01 | 0 to 1 (0.005) | while it applies |  |
| `Stall Speed` | 40 | 0 to 59 (0.1) | while it applies |  |
| `Max Grind Speed` | 28 | 0 to 200 (0.1) | not reached in the test |  |
| `Grind Balance Factor` | 0.0001 | 0 to 1 (1e-06) | every frame |  |
| `Grind Balance Adjust` | 0.22 | 0 to 5 (0.001) | not reached in the test |  |
| `Grind Balance Init` | 0.004 | 0 to 5 (0.001) | every frame |  |
| `Grind Balance Delay` | 0.2 | 0 to 5 (0.01) | not reached in the test |  |
| `Grind Balance Best Reset` | 0.6 | 0 to 1 (0.01) | not reached in the test |  |
| `Grind Balance Reset` | 0.75 | 0 to 1 (0.01) | not reached in the test |  |
| `Grind Balance Spin Dead` | 0 | 0 to 1 (1) | not reached in the test |  |
| `Grind Balance Spin Reset` | 1 | 0 to 1 (1) | not reached in the test |  |
| `Grind Balance Short Time` | 0.3 | 0 to 5 (0.01) | not reached in the test |  |
| `Grind Balance Short Init` | 0.015 | 0 to 5 (0.001) | not reached in the test |  |
| `Grind Balance Short Dead` | 20 | 0 to 50 (1) | not reached in the test |  |
| `Stall Balance Factor` | 0.00012 | 0 to 1 (1e-06) | every frame |  |
| `Stall Balance Adjust` | 0.22 | 0 to 5 (0.001) | not reached in the test |  |
| `Stall Balance Init` | 0.003 | 0 to 5 (0.001) | every frame |  |
| `Stall Balance Delay` | 0.25 | 0 to 5 (0.01) | not reached in the test |  |
| `Stall Timeout` | 0.5 | 0 to 5 (0.01) | every frame |  |
| `Manual Balance Factor` | 0.00012 | 0 to 1 (1e-06) | every frame |  |
| `Manual Balance Adjust` | 0.24 | 0 to 5 (0.001) | not reached in the test |  |
| `Manual Balance Init` | 0.01 | 0 to 5 (0.001) | every frame |  |
| `Manual Balance Delay` | 0.25 | 0 to 5 (0.01) | not reached in the test |  |
| `Grind Radius` | 1.75 | 0 to 5 (0.05) | while it applies |  |
| `Grind Radius Ground` | 1 | 0 to 5 (0.05) | while it applies |  |
| `Grind Radius QPipe` | 2.8 | 0 to 5 (0.05) | while it applies |  |
| `Grind Lean Angle` | 0 | 0 to 90 (1) | not reached in the test |  |
| `Grind Approach Angle` | 70 | 0 to 90 (1) | while it applies |  |
| `Grind Acceleration` | 0 | 0 to 90 (1) | never read |  |
| `Grind Speed Loss` | 0.75 | 0 to 1 (0.01) | not reached in the test |  |
| `Manual Lean Angle` | 45 | 0 to 90 (1) | never read |  |
| `Manual Min Speed` | 5 | 0 to 90 (0.2) | on the event |  |
| `Forward Friction` | 0 | 0 to 1 (0.001) | not reached in the test |  |
| `Side Friction` | 1 | 0 to 1 (0.01) | never read |  |
| `Min Wall Ride Speed` | 5 | 0 to 20 (0.2) | never read |  |
| `Wall Check Radius` | 4 | 0 to 20 (0.1) | while it applies |  |
| `Min Wall Angle` | 0 | -1 to 1 (0.02) | while it applies |  |
| `Wall Vert Dot` | 0.5 | 0 to 1 (0.01) | not reached in the test |  |
| `Wall Tap Time` | 0.15 | 0 to 20 (0.01) | not reached in the test |  |
| `Wall Tap Jump Factor` | 0.8 | 0 to 1 (0.01) | not reached in the test |  |
| `Wall Tap Out Speed` | 8 | 0 to 20 (0.05) | not reached in the test |  |
| `Wall Ride Roll Angle` | 0.68 | 0 to 1 (0.01) | not reached in the test |  |
| `Wall Ride Timeout` | 0.5 | 0 to 5 (0.01) | every frame |  |
| `Wall Ride Gravity` | 12 | 0 to 100 (1) | every frame |  |
| `Wall Ride Offset` | -0.4 | -1 to 0 (0.01) | not reached in the test |  |
| `Side Jump Speed` | 8 | 0 to 25 (0.5) | while it applies |  |
| `Wall Ride Jump Speed` | 3.5 | 0 to 25 (0.5) | not reached in the test |  |
| `Grind Hill Gravity` | 0.3 | 0 to 1 (0.01) | not reached in the test |  |
| `Grind Extension Time` | 0.25 | 0 to 5 (0.01) | not reached in the test |  |
| `Sloppy Vert Weld` | 0.02 | 0 to 0.1 (0.001) | while it applies |  |

#### Action (0x24A518, 20)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Action Check Radius` | 4 | 0 to 10 (0.1) | on the event |  |
| `Vault Check Radius` | 6 | 0 to 30 (0.1) | on the event |  |
| `Vault Check Angle` | 0.95 | 0 to 1 (0.001) | on the event |  |
| `Vault Min Speed` | 10 | 0 to 50 (0.1) | on the event |  |
| `Vault Timing` | 0.24 | 0 to 1 (0.01) | not reached in the test |  |
| `Vault Jump` | 18 | 0 to 50 (0.1) | not reached in the test |  |
| `Vault Min Height` | 0.5 | 0 to 10 (0.1) | while it applies |  |
| `Vault Max Height` | 2 | 0 to 10 (0.1) | while it applies |  |
| `Vault Approach Angle` | 0.71 | 0 to 1 (0.01) | while it applies |  |
| `Vert Pole Radius` | 1 | 0 to 10 (0.1) | never read |  |
| `Horiz Pole Radius` | 2 | 0 to 10 (0.1) | not reached in the test |  |
| `Vert Pole Angle` | 0.2 | -1 to 1 (0.02) | on the event |  |
| `Pole Grab Timeout` | 0.1 | 0 to 5 (0.02) | every frame |  |
| `VPole Min SPeed` | 5 | 0 to 35 (0.5) | on the event |  |
| `VPole Speed` | 1 | 0 to 1080 (0.05) | not reached in the test |  |
| `HPole Min SPeed` | 5 | 0 to 35 (0.5) | not reached in the test |  |
| `HPole Side Angle` | 30 | 0 to 90 (0.5) | not reached in the test |  |
| `HPole Release Angle` | 25 | 0 to 600 (1) | not reached in the test |  |
| `HPole Release Angle Up` | 75 | 0 to 600 (1) | not reached in the test |  |
| `HPole Release Angle Drop` | 10 | 0 to 600 (1) | not reached in the test |  |

#### BoobTech (0x246288, 3)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Displacement Force` | 800 | 50 to 2000 (10) | every frame |  |
| `Damping Force` | 0.1 | 0.001 to 1 (0.001) | every frame |  |
| `Implant Size` | 1 | 0.5 to 3 (0.1) | every frame | cloth/jiggle size (BoobTech, also listed as Mem Check) |

#### Competitor (0x24A8D8, 5)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Initial Speed Ratio` | 1.15 | 0 to 10 (0.01) | never read |  |
| `Final Speed Ratio` | 0.25 | 0 to 10 (0.01) | never read |  |
| `Speed Decrement Ratio` | 0.85 | 0 to 10 (0.01) | never read |  |
| `Speed Increment Ratio` | 0.8 | 0 to 10 (0.01) | never read |  |
| `Boost` | 0.3 | 0 to 10 (0.01) | never read |  |

#### Vibrate (0x24A9C8, 18)

| Entry | Default | Range (step) | Read | Notes |
|---|---|---|---|---|
| `Grind Wood Big` | 0.5 | 0 to 1 (0.01) | not reached in the test |  |
| `Grind Wood Small` | 0 | 0 to 5 (1) | not reached in the test |  |
| `Grind Wood Time` | 50 | 0 to 1000 (2) | not reached in the test |  |
| `Grind Metal Big` | 0.5 | 0 to 1 (0.01) | not reached in the test |  |
| `Grind Metal Small` | 0 | 0 to 5 (1) | not reached in the test |  |
| `Grind Metal Time` | 50 | 0 to 1000 (2) | not reached in the test |  |
| `CessSlide Big` | 0 | 0 to 1 (0.01) | on the event |  |
| `CessSlide Small` | 1 | 0 to 5 (1) | on the event |  |
| `CessSlide Time` | 350 | 0 to 10000 (2) | on the event |  |
| `NPC Big` | 1 | 0 to 1 (0.01) | not reached in the test |  |
| `NPC Small` | 0 | 0 to 1 (1) | not reached in the test |  |
| `NPC Time` | 95 | 0 to 1000 (2) | not reached in the test |  |
| `Crash Big` | 1 | 0 to 1 (0.01) | on the event |  |
| `Crash Small` | 1 | 0 to 1 (1) | on the event |  |
| `Crash Time` | 250 | 0 to 1000 (2) | on the event |  |
| `Impact Big` | 1 | 0 to 1 (0.01) | on the event |  |
| `Impact Small` | 1 | 0 to 1 (1) | on the event |  |
| `Impact Time` | 100 | 0 to 1000 (2) | on the event |  |

#### Mem Check

The same three entries as BoobTech.
