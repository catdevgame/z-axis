# PCSX2 reference run: boot to Tutorial gameplay

A scripted, reproducible PCSX2 playthrough of Aggressive Inline (SLUS-20327):
fresh boot, front end, Tutorial level, and seven tutorial goals (talk to a
challenge giver, banners, slalom, fakie, jump, manual, grind), ending at the
start of the eighth goal ("Modifying Grinds"). Every checkpoint has a savestate,
an atomic 32 MB EE RAM image and a frame thumbnail, so the native build can be
compared against it later, from the start or from any checkpoint.

Continued in `REFERENCE-RUN-2.md`: Tutorial lessons 9-18 from ref21, and Career
mode in MOVIE LOT (pause menu, challenges, memory card save), with more game-state
addresses (level id, juice, HUD score, balance meters).

Everything runs through `pcsx2pad.py` (see `PCSX2PAD.md`) on the private PCSX2
(PINE slot 28012, data dir `games/aggressive/tools/pcsx2-test-data`). Nothing touches the user's
baseline PCSX2.

## Reproduce

```sh
cd games/aggressive/tools
python3 pcsx2pad.py launch                                   # fresh boot
PCSX2PAD_RECORD=/tmp/ref-run.log python3 pcsx2pad.py run scripts/tutorial-all.txt
python3 pcsx2pad.py padscript /tmp/ref-run.log               # the run as a ZX_PAD_SCRIPT
python3 pcsx2pad.py quit
```

The whole run takes about 3 min 20 s wall at 100% emulation speed. To start mid-way, load the checkpoint a segment starts from
and run only that segment, e.g.

```sh
python3 pcsx2pad.py loadstate 44 && python3 pcsx2pad.py run scripts/tutorial-07-manual.txt
```

(about 22 s, the same result as in the full run). Running a segment rewrites its
checkpoint files. To keep the run-1 files, redirect the outputs with
`PCSX2PAD_RAM_DIR`, `PCSX2PAD_PNG_DIR` and `PCSX2PAD_STATE_DIR`, and copy the
slots back from `pcsx2-test-data/reference/` afterwards.

## Scripts (`games/aggressive/tools/scripts/`)

Each segment starts at the previous segment's last checkpoint. `tutorial-all.txt`
runs them in order. Game time per segment is from the run-1 vsync counter (add
about 1 s wall per checkpoint for saving and extracting).

| Script | From | Does | Game time |
|---|---|---|---|
| `tutorial-01-start.txt` | fresh boot | START until the front end is up (skips the 3 movies), CAREER, player select, name entry (END), level select TUTORIAL, loading, first tip | 39.5 s after the first pad poll (47 s after boot) |
| `tutorial-02-talk.txt` | 32 | 3 intro tips; skate to the chef, brake (DOWN), CIRCLE: "There you go." | 14 s |
| `tutorial-03-banners.txt` | 35 | Basic Skating tips; skate through the 3 banners: "Nice work." | 20 s |
| `tutorial-04-slalom.txt` | 37 | Sharp Turns tips; slalom with R1/L1 while turning: "Good job." | 18 s |
| `tutorial-05-fakie.txt` | 39 | Fakie tips; R2 while rolling (cess slide) to go fakie, the same 4 banners backwards: "Good work. You made it through." | 28 s (21 s in run 2) |
| `tutorial-06-jump.txt` | 41 | Jumping tips; hold UP+CROSS, release CROSS at x > -18.5, clear the low wall: "Nice Job" | 16 s |
| `tutorial-07-manual.txt` | 44 | Manuals tips; DOWN, UP taps at x > -15.1: Single Toe Manual over the funbox: "Good job" | 22 s |
| `tutorial-08-grind.txt` | 46 | Grinding tips; line up with the rail, jump, hold TRIANGLE: Frontside Grind: "Good job"; then the Modifying Grinds tips | 36 s |

Every movement uses the D-pad (UP forward, DOWN brake, LEFT/RIGHT steer). The
analog sticks are never used, so the whole run is buttons only.

Robustness: waits are on game state (game mode, zgui panel, player position),
not wall time. Mid-level movement is closed loop. `goto X Z R` holds UP and steers
with LEFT/RIGHT from the measured heading, so small timing differences between
runs do not change the outcome. Note that the game keeps running while a
savestate is written (about 30 frames), so every mid-motion checkpoint is
followed by a closed-loop step.

### The verbs this run uses (`pcsx2pad.py`)

| Verb | What |
|---|---|
| `waitval ADDR TYPE OP VALUE [TIMEOUT]` | wait for a typed compare (`u8 u16 u32 s32 f32`, `== != < <= > >=`) |
| `pressuntil BTN ADDR TYPE OP VALUE [TIMEOUT] [GAP]` | tap BTN every GAP frames until the condition holds |
| `nexttip [TIMEOUT]` / `tips [SETTLE] [TIMEOUT]` | dismiss one message box / all boxes until none opens for SETTLE frames |
| `goto X Z R\|x>V [TIMEOUT] [EXTRA]` | skate to (X, Z) by D-pad. EXTRA adds held buttons, `SHARP` holds R1/L1 while turning |
| `waitnear ADDR X Z R`, `peekf ADDR [N]` | position wait, print floats |
| `checkpoint SLOT NAME` | savestate SLOT, copy to `pcsx2-test-data/reference/NAME.p2s`, extract `eeMemory.bin` to `recomp/dumps/ref/NAME.ram` and the PCSX2 frame thumbnail to `reference/pcsx2-run1/NAME.png` |
| `PCSX2PAD_RECORD=FILE` | log every input change with its vsync |
| `padscript LOG [FROM [TO]]` | turn such a log into a `ZX_PAD_SCRIPT` string |

The RAM image comes from the savestate, not from PINE: a PINE `dump` of 32 MB
takes about 0.6 s while the game keeps running, so different parts of it are
from different frames. The `.p2s` is a zip whose `eeMemory.bin` is one consistent frame.
Its members are zstd-compressed; Python 3.14+ `zipfile` reads them.

## Checkpoints

Savestate slots 30-50 in `pcsx2-test-data/PCSX2/sstates/SLUS-20327 (67DB3ED8).NN.p2s`,
with named copies in `pcsx2-test-data/reference/NAME.p2s` (both git-ignored).
RAM images: `recomp/dumps/ref/NAME.ram` (git-ignored). Screenshots:
`reference/pcsx2-run1/NAME.png`. These are PCSX2's own 640x480 frame thumbnails
from the savestate, so they are the exact frame of the RAM image and have no OSD.
They are reduced to 256 colours to keep the repo small, and they look darker
than a window capture. The full-colour thumbnail is inside each `.p2s`.

Values below are read from the run-1 RAM images. vsync = 0x231CA4 (first pad
poll at 461), frame = 0x231CAC, mode = 0x276FE4, pos = 0x2A76F0, node = player
skin node translation 0x6F9E40, yaw in degrees from +x towards +z.

| Slot | Name | Screen | vsync | frame | mode | pos (x, y, z) | node yaw |
|---|---|---|---|---|---|---|---|
| 30 | ref01-main-menu | MAIN MENU, CAREER | 1122 | 462 | 1 | - | - |
| 31 | ref02-level-select | LEVEL SELECT, TUTORIAL 0% | 1480 | 802 | 1 | - | - |
| 32 | ref03-first-tip | "Talking to people" | 2826 | 352 | 3 | (0, 20000, 0) sentinel; node at (-34.79, -0.01, 0.91) | 0 |
| 33 | ref04-talk-go | play, bubble and chef ahead | 3230 | 756 | 2 | (-34.79, -0.01, 0.91) | 0 |
| 34 | ref05-talk-skating | rolling | 3319 | 845 | 2 | (-24.95, -0.01, -0.25) | -2.5 |
| 35 | ref06-talk-done | HINT "Sometimes radios, kiosks..." | 3681 | 1207 | 3 | (-0.05, -0.01, -2.98) | -2.5 |
| 36 | ref07-banners-go | three banners ahead | 4313 | 1839 | 2 | (-27.38, -0.01, 8.37) | 0 |
| 37 | ref08-banners-done | "Sharp Turns" title | 4852 | 2378 | 3 | proxy (42.52, -0.01, 45.95), node already at (-25.86, -0.01, 10.21) | 0 |
| 38 | ref09-slalom-go | slalom banners | 5257 | 2783 | 2 | (-25.86, -0.01, 10.21) | 0 |
| 39 | ref10-slalom-done | "Fakie" title | 5921 | 3447 | 3 | proxy (21.47, -0.01, -47.51), node (-31.98, -0.01, 10.21) | 0 |
| 40 | ref11-fakie-go | fakie banners | 6326 | 3852 | 2 | (-31.97, -0.01, 10.21) | 0 |
| 41 | ref12-fakie-done | HINT "cannot go as fast ... fakie" | 7573 | 5099 | 3 | (45.13, -0.01, -17.62) | -97.7 |
| 42 | ref13-jump-go | low wall ahead | 8135 | 5661 | 2 | (-25.86, -0.01, -2.27) | 0 |
| 43 | ref14-jump-air | in the air over the wall | 8227 | 5753 | 2 | (-16.77, -1.09, -2.27) | 0 |
| 44 | ref15-jump-done | HINT "To jump without speeding up" | 8525 | 6051 | 3 | (46.26, -0.01, -7.20) | -83.0 |
| 45 | ref16-manual-go | funbox ahead | 9428 | 6954 | 2 | (-20.48, -0.01, -4.53) | 0 |
| 46 | ref17-manual-done | HINT "Use the manual to connect tricks", score 186 | 9840 | 7366 | 3 | (46.45, 0.83, -0.11) | 0 |
| 47 | ref18-grind-go | rail ahead | 10564 | 8090 | 2 | (-27.38, -0.01, -3.07) | 0 |
| 48 | ref19-grind-on-rail | Frontside Grind | 10707 | 8233 | 2 | (0.32, -1.29, -0.02) | 0 |
| 49 | ref20-grind-done | HINT "To jump out of a grind", score 811 | 11056 | 8582 | 3 | (43.55, -0.01, 46.06) | 156.8 |
| 50 | ref21-modgrind-go | Modifying Grinds, rail ahead | 11979 | 9505 | 2 | (-25.86, -0.01, -3.37) | 0 |

Camera eye 0x24C9E0 is (pos.x - 3.99, -1.97, pos.z) at every "go" checkpoint
(standing, facing +x). In-level frame = vsync - 2474 throughout: in PCSX2 the
level runs one main-loop frame per vsync (60 fps).

### Reproducibility

A second run of the same scripts from a fresh boot reaches all 21 checkpoints
with every goal completed. Game mode, zgui panel pointer, 0x277488 and
0x2732FC are identical at every checkpoint, and so are the standing positions
at all "go" checkpoints and the camera eye. The front end and the first two
goals land within 1-3 vsyncs. From the fakie goal on the closed-loop skating
takes a different number of frames, and where the skater coasts to before a
HINT differs; mid-motion checkpoints match closely. The recorded run 1 is the
reference.

## Game-state addresses

All are read from PCSX2 EE RAM. Static addresses are the same natively (same
ELF). Heap addresses differ natively by a constant per area, as noted.

| Address | Type | Meaning |
|---|---|---|
| 0x00231CA4 | u32 | vsync count (VBlank handler), 60 per second |
| 0x00231CA8 | u32 | vsync count latched at the last flip; equals 0x231CA4 in PCSX2 level play (a flip every vsync) |
| 0x00231CAC | u32 | main-loop frame count (`D_00231900+0x3AC`, incremented at 0x100AE4 after each flip; the loop is 0x100A54-0x100AF4). Reset to 0 at level start, +1 per vsync in level play |
| 0x0023FD80 | f32 | frame-limiter target fps. The loop at 0x100A54 waits until `vsync - lastflip >= 60/fps - 1`; 0 = no limit. Over a boot: 0 in the front end, 20.0 from the loading screen until level frame 10, 0.0 after that, plus one frame at 30.0 at level frame 283. The only static writer is `func_00122C40` |
| 0x00276FE4 | u32 | game mode: 0 boot movies / loading, 1 front end, 2 level play, 3 level paused by a message box (tip or HINT); 3 exactly when a box with "X Continue" is shown |
| 0x00240A20 | u32 ptr | zgui active panel (`D_00240A10+0x10`, `D_00240A10` is the zgui global used by zgui.c). PCSX2 values: 0x8323E0 main menu, 0x840F90 player select, 0x847700 name entry, 0x857D40 level select, in level 0x832CD0 box opening (input ignored, about 40 frames), 0x831BB0 box waiting for X, 0 none; the same in both runs |
| 0x00277488 | u32 | 1 while a tutorial section is being introduced (title box and its tips), 0 in play and during HINT boxes. Stale 1 in the front end |
| 0x002A76F0 | f32 x3 | player position (x, y, z): +x is ahead at goal starts, y is up when negative (jump peak -2.0, rail top -1.29), RIGHT turns from +x to -z. It is entry 0 of a 0x60-stride collision-proxy array (entries 1-11 are static props, e.g. the chef at (0.23, 0, -4.41)). Holds (0, 20000, 0) before the player is placed and keeps the old position while a section-title box is up |
| 0x006F9E00 | node | player skin node (ZGE node, name `Skin_Object_grob` at +0x12C). World matrix at +0x10: rows 0-2 rotation, row 3 (0x6F9E40) translation, equal to the proxy position during play. Forward = -row2, so yaw = atan2(-r2.z, -r2.x). Heap. Native: 0x6F8E80 (same name, -0xF80) |
| 0x0024C9E0 / 0x0024C9F0 | f32 x3 | camera eye / look-at in world space. Look-at = player + (0, -1.5, 0), eye 4 behind and 1.97 up (in the air over the jump goal's wall, eye y = -3.19) |
| 0x00735BBC | camera | camera node (native 0x734BBC, -0x1000): FOV 0.5833 at 0x735BB0 (native 0x734BB0), 170 / 250 at 0x735BBC / 0x735BC0, view matrix from 0x735BCC |
| 0x002732FC | u32 | request counter on the `D_0027A930` object (incremented in `func_00107FB0` / `func_00107FE0`). Moves +1 per tutorial goal (13 at the first goal, 20 at Modifying Grinds), so it is a progress gauge, but its absolute value depends on the front-end path |
| 0x0027E0C4 | u32 | not game state: word 1 of D_0027E0C0, the buffer func_00117EE8 (sceSifCallRpc wrapper) copies unaligned RPC send buffers into (memcpy 0x223ACC, sceSifCallRpc 0x217800), i.e. the last such IOP request (sound driver calls) |
| 0x37E8C0+ / 0x39E940+ | VIF list | double-buffered render list: per object `13000000 07000000 01000404 6C048000` (VIF codes) then a 4x4 world matrix. The banner and rail positions the `goto` targets use come from here |

The level id, the score, the juice and the balance meters are in
`REFERENCE-RUN-2.md` "Game-state addresses (run 2)". No true tutorial goal
index is known (0x2732FC is the best proxy).

Two traps:
- 0x002ABC40 looks like a tip counter (it rises with tips), but it is libpad
  internal state (`D_002ABC40 + port*32 + slot*128`, written in the
  scePadPortOpen code at 0x211840) that moves on pad-related events. Do not use it.
- The fps target reading 20.0 natively in a level's first frames is not a
  divergence: PCSX2 also has 20.0 until level frame 10.

## Native build: pad scripts

`scripts/native/<checkpoint>.padscript` holds, for every checkpoint, the inputs of run 1 up to it in the
native syntax (`<sec>:<BUTTON>:<dur>`). With `ZX_PAD_SCRIPT_CLOCK=vsync` (script time in emulated vsyncs
from the first pad read) and stick entries `<t>:LX=<v>[:<dur>]` they replay natively; logiccheck.py (below)
replays the whole run that way.

## Native replay and comparison (`games/aggressive/tools/logiccheck.py`)

For checks use `games/aggressive/tools/lc-when-free.sh NAME` (RUNNER and a scratch HOME exported; it queues `run` and
`compare`, unpaced, about 35 s): `/tmp/lc-NAME-cmp.log` must say 21/21 MATCH (docs/BRINGUP.md "Checks").
By hand:

```sh
RUNNER=<zx-runner> ZX_VU1_SKIP=1 python3 games/aggressive/tools/logiccheck.py run logic-NAME 420
python3 games/aggressive/tools/logiccheck.py compare logic-NAME      # per checkpoint: mode, panel, gauge, pos, node, camera
python3 games/aggressive/tools/logiccheck.py waypoints logic-NAME    # mid-motion positions read during run 1
python3 games/aggressive/tools/logiccheck.py anchors logic-NAME      # how far each anchor moved the native clock
```

`run` replays the exact run-1 input (`scripts/native/ref-run1-input.txt`, the
PCSX2PAD_RECORD log in vsync units) with `ZX_GS_NULL=1` and
`ZX_PAD_SCRIPT_CLOCK=vsync`, and dumps native RAM at each checkpoint's vsync
with `ZX_DUMP_WHEN` (into `recomp/dumps/logic/`). What makes it line up:

- Anchors (`SYNC`/`WAIT` entries, see `PadScript` in the runtime) for the waits
  of the reference that are not under test: the front-end mode/panel waits
  (`scripts/native/ref-run1-commands.txt` is the pcsx2pad output with the
  vsync of each outcome), the level frame counter at the first in-level
  checkpoint (frame 352 at vsync 2826), and one hold-only `WAIT` per message
  box. `tips` taps CROSS from the moment a box starts opening; only the tap
  that closed the box is replayed (a tap followed by the next one exactly 26
  vsyncs later was ignored by the game).
- Inputs are delayed by one vsync: pcsx2pad writes during vsync V and the game
  reads it in frame V+1.
- In the level PCSX2 runs one main-loop frame per vsync and so does native, so
  after the frame anchor the level frame counter (0x231CAC) and the level time
  accumulator (0x276FF8, float seconds) are identical to PCSX2 at every
  checkpoint.

### PCSX2 replay of the recorded input (`games/aggressive/tools/pcsx2replay.py`)

`compare` checks native against run 1 itself, which holds only where the
record is exactly what the game saw in run 1. pcsx2pad.py wrote it closed
loop over PINE, so a write lands on the next pad read or, now and then,
another one. `pcsx2replay.py` replays a record inside PCSX2 with no
PINE timing at all: `pcsx2replay_hook.s` (same call site as the pad hook)
takes the buttons from a schedule in EE RAM, appends a record of watched
words at every pad read to a ring buffer, and can freeze the game at a
vsync for an exact RAM image. It needs its own PCSX2 data dir
(PCSX2REPLAY_DATA, a copy of pcsx2-test-data with its own PINESlot,
PCSX2REPLAY_SLOT):

```sh
cd games/aggressive/tools
python3 pcsx2replay.py install                 # pnach into $PCSX2REPLAY_DATA
python3 pcsx2replay.py launch --card Mcd-run1.ps2
python3 pcsx2replay.py run boot 12000 ../workspace/recomp/dumps/logic2/px-full \
  --watch 0x231CAC:1,0x276FE4:1,0x240A20:1,0x277488:1,0x2732FC:1,0x27E0C4:1,0x2A76F0:3,0x6F9E10:16,0x24C9E0:3,0x24C9F0:3,0x735BB0:1,0x735BCC:12,0x276FF8:1
cd ../../.. && python3 games/aggressive/tools/logiccheck.py pxcompare NAME games/aggressive/workspace/recomp/dumps/logic2/px-full.log
```

(`Mcd-run1.ps2` is a copy of `reference/Mcd001-before-run2.ps2`.) Replays
start from a fresh boot: loading a savestate makes the first frame take two
vsyncs, the game's adaptive frame limiter (func_00185DA0) then holds 30 fps
for 0.5 s, and the level frame counter ends 16 frames behind. Only the
first freeze of a run is exact (VBlanks keep coming while the EE spins).

The boot replay is bit-identical to run 1 at every checkpoint up to ref11
(player position, level time, sound stamps; a record at the pad read of
vsync V+1 holds what the savestate at V holds, except the frame counter and
the zgui panel, which the main loop updates after the flip). From the fakie
goal on it differs from run 1 wherever the run was mid-motion or coasting
(ref12, ref14-ref17, ref20), and is identical again at every section start
(ref13, ref18, ref21). So the record is not exactly what run 1 saw there
(the four edges below). With the same input file on both sides, the PCSX2
replay is the per-frame reference for native. Natively the same
point is the pad read too: `ZX_PAD_LOG` (same word list, native heap
addresses) and `logiccheck.py frames NAME PXLOG` give the first level frame
where the two differ.

### What run 1's game saw (`scripts/native/ref-run1-seen.txt`)

Four edges of the record reached the game one vsync before the vsync it
says (a PINE write that landed before that frame's pad read).
`ref-run1-seen.txt` is the record with those four moved. Replayed with
pcsx2replay.py it reproduces run 1 at all 21 checkpoints in every watched
word (player, skin node, camera, view matrix, level time, sound stamps,
panel, gauge). `games/aggressive/tools/inputsearch.py` finds such edges: it moves each edge
of a window by one vsync, replays natively and scores the checkpoint
against run 1.

| Recorded | Seen | Input | Checkpoints it fixes |
|---|---|---|---|
| 7251 | 7250 | UP+LEFT back to UP (end of the fakie circling `goto`) | ref12 |
| 8175 | 8174 | UP+CROSS down (jump goal) | ref14, ref15 |
| 8882 | 8881 | CROSS tap in the manual goal's tip chain (8626 instead also fixes the position, but not the sound stamp 0x276FF0) | ref16, ref17 |
| 10650 | 10649 | UP+RIGHT (grind goal approach) | ref19, ref20 |

logiccheck.py uses `ref-run1-seen.txt` when it exists (LOGICCHECK_INPUT
overrides; `ref-run1-input.txt` is the record as written).
