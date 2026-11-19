# Developer tools: the game's own and the runner's

A short guide to the developer tools: the game's hidden debug menu, its tuning table (every entry and how the
game reads it: docs/TUNING.md) and the language setting, then the developer runner's switches for test runs, routes, the A/B,
tracing and the remote workers. Each tool's own `--help` or header comment has the details. Code of the game's tools:
z-axis `runtime/src/game_overrides/aggressive_inline_devtools.cpp`, the tuning.ini parser
`runtime/include/runtime/ps2_tuning_ini.h`. [V] = verified in the running game or the code,
[I] = inferred from a name.

## How to use

| What | App | Runner (developer) |
|---|---|---|
| Debug menu | Settings > Game > Developer Menu: On (next start), then **F1** in a level | `PS2X_DEV_MENU=1`; F1, or `set dev.debug_menu 1` on PS2X_CONTROL |
| Tuning overrides | `Mods/tuning.ini` in the game's support folder, or a mod's (the last mod that has one; SETTINGS.md "Mods"), always read when it exists | `tuning.ini` in the last of PS2X_MODS_DIR's folders that has one, or `PS2X_TUNING_INI=<file>` |
| Reference of all entries | With the developer menu on, every start writes `Mods/tuning-reference.ini` | same, into PS2X_MODS_DIR's first folder |
| Language | Settings > Game > Language (next start) | `PS2X_LANGUAGE=us\|uk\|french\|german` |

settings.ini: `[general] language = us|uk|french|german`, `developer_menu =
true|false` (defaults us, false). With the defaults nothing is hooked or
written: no tuning.ini means no tuning code runs, the developer menu off
means no hook, US English means the ELF is untouched.

tuning.ini format (the reference file lists every entry this way, commented
out):

    ; comment
    [Camera]
    Distance = 6
    Height = 0.8
    FOV = 85
    [Physics]
    Jump = 20

Sections are the game's categories (any case), names are the entries' names
(any case). A name without a section is looked up in every category; a name
that two categories have (`Forward Friction`, `Side Friction`) needs its
section. Unknown names and bad numbers are logged (`[tuning] ...`) and
skipped; values outside the table's own min/max are applied and flagged.
Values are written once when the game starts (before its first
instruction); Far Clip, the one entry the game writes itself (at every
level start), is written again at every vsync. Any tuning change can change
how the game plays (logic checks and replays assume no tuning.ini).

## The hidden debug menu [V]

The game has 8 game modes in a table at 0x24B810, `{mode, enter, exit,
update, 0}` (0x14 bytes each); `func_001A70F0(mode)` runs the old mode's
exit and the new one's enter. Mode 2 is a level (update 0x1ACA68), 3 the
pause menu, 0 the Art Viewer, 6 the debug menu: enter 0x1BC470 plays
`InGameUI.ShowDebug` (`MultiplayerUI.ShowDebug` in split screen) and saves
the input mode and sets it to 3 (menus), exit 0x1BC5B0, update 0x1BC518 (UI update, and it sets the
camera from the tuning table's Camera `Type` every frame). Nothing in the
retail game enters mode 6 (no caller passes 6; whatever opened it in
development builds is gone [I]). The hook (only with the developer menu on) wraps the level's
update: on F1 it runs `func_001A70F0(6)` in place of one update, like a tail
call. (0x276FE4, the "game mode" the other tools read, is the input mode
the modes set: 1 front end, 2 play, 3 menus; it reads 3 in the debug menu.)

The menu (InGame.zsc `Debug`, a guiFrame with a guiMenu in the game's
arial16 font, "DEBUGMENU STATE" top left; `recomp/logs/devlang-dbg1.png`):

| Item | What it does (inlineDebugMenuSelect 0x1BC600) |
|---|---|
| Resume | back to the level (mode 2) |
| Art Viewer | mode 0: a free camera (`logs/devlang-dbg-art2.png`). The skater and the level stop; the d-pad and buttons move and turn the camera (button queries 0x18A1E8/0x18A210 on the game's own button bits: Up 0x1, Down 0x2, Left 0x4, Right 0x8, Triangle 0x10, Square 0x20, Circle 0x40, Cross 0x80, L1 0x100, R1 0x200, L2 0x400, R2 0x800, Start 0x4000, Select 0x8000). **Start** goes back to the level (and the same press opens the pause menu); Select calls 0x110BC0(0). The view is upside down in the Tutorial (as the code sets it up, not a port bug as far as seen). |
| Tuning | the tuning table editor: a list of the 20 categories (`logs/devlang-dbg2.png`), Cross opens one as a two-column list of names and values (`logs/devlang-dbg4.png`), left/right step the selected value by its step within min/max (`inlineTuneDown`/`inlineTuneUp`), Triangle goes back. Changes are live where the game reads the entry live (docs/TUNING.md). |
| Respawn | sets skater flag 0x4 and resets the skater to the respawn point (0x193DE0), back to the level |
| Toggle Debug | cycles a debug display level (low bits of 0x24C9B0) 0, 1, 2: level 1 shows physics state text (state, substate, edge, balance, object and terrain names: `logs/devlang-dbg-toggle1.png`) and VU/CPU/DMA timing bars top right; level 2 only small markers; 0 off (0x150CE0 and 0x14B7D0 with 0/1) |

Cancel (Triangle) on the menu is Resume. Checked in the Tutorial (Career's
first level) with the CPU renderer; not tried in split screen.

## Language [V]

- The game loads one string table at start: `assets\string.eus` (name at
  0x268788, loaded by FUN_001BC9F0 through 0x1483B8, made current with
  0x148628). The disc also has `STRING.EUK`, `.FRE`, `.GER` (same 236
  keys, hash-sorted) and `STRINGLE.*` (275 park editor piece names, Dave
  Mirra leftovers, identical in all four files, never loaded). The setting
  changes the three letters of the name before the game runs.
- **What the table holds:** memory card messages (the Sony-style card UI at
  0x1E80B8-0x1EE4F0, the only users of the current table), trick names
  (identical in all four files), and BMX tips from Dave Mirra ("tap C to
  turn", "pedalling") that this game never shows.
- **What changes:** only the memory card texts, which are only shown with
  card saves (`PS2X_SAVES=card`; the app's native saves never show them).
  Even there the French and German files are mostly English (the long
  "memory card (PS2) in MEMORY CARD slot" texts are untranslated) and out
  of step: French "Save game?" key holds "No memory card (PS2) found in
  MEMORY CARD slot %d.", so the save dialog asks the wrong question with a
  garbage slot number (`logs/devlang-fr13.png`; `devlang-fr12.png` shows
  the French file's "Select MEMORY CARD slot." with its full stop). UK
  English differs in 15 strings.
- **What stays English:** everything the player normally sees. Menus,
  pause pages, HUD texts and button prompts are UIBuilder scripts
  (UILIB.ZAL, literal `Text` properties); zgui looks its text up in its
  own table (0x240C70 via 0x1714E8), not in STRING.*; Tutorial tips and
  goals are level data; textures (titles, the main menu words) and speech
  are English on this disc.
  With native saves the Language row changes nothing visible (its help line says so).

## Test runs: any level, any state

For recording or checking code in states the logiccheck and career inputs never reach. All are
runner environment, off by default, and hook nothing when off.

The app passes nothing through the environment: each game it boots gets a BootConfig
(runtime/ps2_boot_config.h; its settings, folders and renderer, `ps2x::app::gameBootConfig`), and
the runtime and the game overrides read a `PS2X_*` name with `ps2x::boot::text`, where the
environment, when it sets the name, still wins. A check that boots several games in one process
gives each a BootConfig of its own: the pad and host input scripts, `PS2X_DUMP_WHEN`,
`PS2X_DUMP_PREFIX`, `PS2X_STATE_LOG(_FILE)`, `PS2X_PAD_DUMP`/`PS2X_PAD_LOG`, `PS2X_POKE_SCRIPT`,
`PS2X_SAVES`, `PS2X_MC_FD_BASE0`, `PS2X_MC_FIXED_TIME` and `PS2X_DEV_UNLOCK_ALL` are read again at
every boot (`BootConfig::names()` lists what it carries). The other switches are the process's.

| Switch | What |
|---|---|
| `PS2X_DEV_UNLOCK_ALL=1` | in the front end, writes what the game's two unlock codes write plus the Media Viewer cutscenes (Settings > Cheats > Unlock Everything; docs/CHEATS.md). It goes into the career save the run writes: use a scratch HOME, card and support folder. In DM2 it (or `PS2X_DM2_OPEN_LEVELS=1`) opens every level |
| `PS2X_DEV_START_LEVEL=<entry>\|ESTATE` | the runtime presses its own way from boot to free skate level-select entry `<entry>` (Movie Lot 0 ... Museum 6) or Estate; `PS2X_DEV_START_SKATER=<n>`, `PS2X_DEV_START_SKATE=<n>` (rounds of sweep.py's skate routine), `PS2X_DEV_START_QUIT=<s>` (stop s seconds after); logs `[devstart] placed ... load N s` |
| `PS2X_SOUND_LOG=<file>` | every ZAudio command (fn, payload) and the channels' status bytes the native driver (or ZAUDIO on the IOP) writes, in guest time |
| `PS2X_SOUND_PCM=<file>` | every frame the native sound driver makes (16-bit stereo, 48 kHz, raw), in guest time: with `PS2X_SOUND_LOG`, the proof that a sound change leaves the sound alone (both byte-identical on a fixed input) |
| `PS2X_VSYNC_PHASE=0` | paced runs: the guest's vblank no longer held in phase with the window's latch (runtime/ps2_host_pacing.h: on by default, it keeps a 60 frames/s game's frames 16.7 ms apart on a 120 Hz display, and moves the vblanks a refresh later while a frame waits a refresh late; host time only, the game's logic follows EE cycles) |
| `ZAXIS_FRAME_QUEUE=0` | the window shows the newest picture at each refresh (latched on the window thread), as before the frame queue: by default every game frame's picture is made on the GS worker at its buffer swap and waits for the window (GS::setFrameQueue), so none is lost to a host frame that took two refreshes; headless runs too, so their pictures (`shot`, `PS2X_SHOT_AT`, the Screenshot key) are never a buffer being drawn again. The pictures the window still latches itself (the first, after a renderer switch, a stretch with no swap, everything with the queue off) are taken again when the game swapped while one was copied (by the display's change count, so A, B, A too), and one the game swapped through every try is not kept. `PS2X_PERF_LOG`'s `[perf] shown:` line counts the game frames by the refreshes they stayed on screen, those never shown, and the time from a picture to its present |
| `ZAXIS_AUDIO=off` | no sound: the audio device is SDL's dummy driver, so everything before the speakers (the SPU2 mix, the host streams, the device's pace) runs as with sound. Unpaced runs (`PS2X_UNPACED=1`) get it too; `ZAXIS_AUDIO=on` plays sound anyway. The tooling's shared environment (`paths.sh`, `zpaths.py`) sets `off` for every test run, paced ones included: a run that has to be heard (audiocheck) sets `on`. `[host] audio:` says which driver |
| `ZAXIS_VRR=1` | variable refresh (the app's Display > Variable Refresh): each game frame is presented when it is ready and a G-Sync, FreeSync or adaptive-sync display follows; on a fixed-rate display a frame waits for the next refresh with no phase lock |
| `PS2X_TRACE_HEAP=1` | the guest heap's high-water marks per level (`runtime/heap_trace.h`): the bytes in use and the peak, in the 32 MB heap and in extended memory, of each stretch of one level name (the panel capture's level folder, `no level` in the front end and while loading), logged when it ends and every 600 vsyncs (`[heap] woodward (ended): 32 MB heap peak ... ticks A..B`); `playbot/memuse.py heap()` reads them, a park's `PARK_MEMORY` run sets it. Off: nothing installed |
| `PS2X_GS_SHOT_VSYNC="T,T,...:PREFIX"` | AI, DM2 and BMX XXX: at each vsync tick T the display as the GS has it once it has done everything sent before that vsync (a GS sideband, in stream order: behind a buffer swap the EE queued, so never the buffer the next frame is being drawn into), drawn to `PREFIX-vT.png`: the same pictures in two runs of one input whatever the host's timing (render paths compared: strict against records). The GPU renderer's at its internal resolution. `PS2X_GS_SHOT_PRIMS=N`: every primitive the GS draws in the N vsyncs before each picture to `PREFIX-vT.prims` (GS::logPrimitive's records: the state and three vertices, a native draw's marked), to set two paths' draws side by side; `PS2X_GS_SHOT_TRACE=1` with it: a GS trace of those vsyncs (`PREFIX-vT.gsr`) for `ps2xGsReplay --gpu`. A deliberate difference: the GPU draws native meshes crossing the near plane whole, the PS2 (and the CPU renderer) as its clipping program's split, clamped triangles (Industrial v14000, GPU at scale 1 against strict: mean 2.66, 2.96 % of pixels > 16 off, edges all over from the GPU's rasterization; the floor's bottom 20 rows 2.00 / 5.1 %) |
| `PS2X_TRACE_NATIVE=1` (`=2`) | `render_path = records`: the draws drawn from their records by class and those left to the strict encoder by why, every 20,000 draws (`[native-records] ...`); `=2` also each part left without a mesh, once, and why |
| `PS2X_PRESENT_LOG=<file>` | a line for every new picture the window shows (`present`: host frame, vsync, the game frame it is, `latched=1` for one the window latched itself, `disturbed=1` for one copied while the game swapped, the CPU renderer's pixels hashed) and for every shot (`shot`, the same for its picture): two runs' presentation compared frame by frame, and shots checked for torn pictures |
| `PS2X_TEXTURE_DUMP=<folder>` (`PS2X_TEXTURE_DUMP_DIRECT=1`: RGB textures too, movie frames included) | texture packs' dump mode, AI, DM2 and BMX XXX, either renderer: every texture the GS draws, once, as a PNG named as PCSX2 names it (`<texture>-<clut>-<bits>.png`, runtime/ps2_texture_pack.h), the palette applied and the PS2's alpha (0x80 opaque). An edited copy in a pack folder (`PS2X_TEXTURE_PACKS=<folder>`, SETTINGS.md "Texture packs") is drawn in its place. `PS2X_TEXTURE_PRELOAD=0` reads a pack's pictures at first use only, `PS2X_TEXTURE_PACK_MB=<n>` sets their memory budget, `PS2X_TEXTURE_PACK_UPLOAD_MB=<n>` the GPU uploads of pictures per frame (6), `PS2X_TEXTURE_PRELOAD=wait` makes the boot wait for the pictures, `PS2X_TRACE_TEXTURE_REPLACE=1` names each picture as it is first drawn. `PS2X_TEXTURE_PACK_LOD_STATS=1` (CPU renderer): at exit, which of PCSX2's names (hardware mipmapping on) the textured primitives get and how many the packs' lookup has |
| `PS2X_GSMEM_AUDIT=1` (`=2`: every overrun) | AI, DM2 and BMX XXX: the GS texture memory per level: allocations, peak block, the CLUTs' floor, overruns (`[gsmem] ...` lines); converted levels must stay under the CLUTs |
| `PS2X_VIRTUAL_PADS=2`, `PS2X_VIRTUAL_PADS_FILE` | a second pad; without one port 1 is empty and both games hide multiplayer. `PS2X_PAD_PORT1=mirror`: port 1 a copy of port 0, for split screen with one input |
| `PS2X_HOST_INPUT_SCRIPT` | host keyboard, mouse and focus events (`<t>:KEY=<name>[:<dur>]`, `<t>:MOUSE=<dx>/<dy>[:<dur>]`, `<t>:FOCUS=0\|1`, times in seconds or `120v`) through the same mapping as real input, on the pad script's clock; real input is then ignored. `PS2X_TRACE_HOST_INPUT=1` logs the mapped pad, `PS2X_TRACE_MOUSELOOK=1` the mouse-look camera, `PS2X_SHOT_AT="<t>:<file.png>,..."` saves pictures. `tools/inputcheck.py` runs the input checks with them |
| `PS2X_PROMPTS=keyboard\|playstation\|xbox\|nintendo` | fixes the button prompts |
| `PS2X_CONTROLS_INI=<file>` | another controls.ini (automated runs never touch the real one) |

Example, unattended on any machine:

    PS2X_DEV_START_LEVEL=0 PS2X_DEV_START_SKATE=1 PS2X_DEV_START_QUIT=3 PS2X_PERF_LOG=1 \
      <ps2EntryRunner> recomp/disc-root/SLUS_203.27

## Routes, play and the control channel

- `PS2X_CONTROL=<dir>` and `tools/nctl.py` (`nctl.py start NAME`, `nctl.py NAME "CMD; CMD"`, `stop`):
  drive a running game from outside (presses, waits, pokes, dumps, shots, `set` a setting, `padport
  0|1` pulls and plugs the pad).
- `tools/routes.py`: closed-loop routes over the control channel, so moves land whatever the host's
  speed: `routes.py start NAME [KEY=VAL ...]`, `routes.py NAME "boot; tutorial; moves"`, `stop NAME`.
  Its docstring lists every command: the Tutorial's moves (stall, poles, skitch, wall ride, grind,
  manual ...), `career K`, `freeskate K`, `teleport`, `ride` (a take-off off a face with launch
  values), `grindtricks`, `hazards`, `splitscreen`, the Park Editor's `pe2*`, `giver NAME`, `carddialog`,
  `menumash`, and `mash [FRAMES [SEED [KEY=VAL ...]]]` (seeded play that keeps going, teleporting the
  skater on a bail or a stop; `style=natural` for live-play profiles). Test-only commands poke the
  game: what they record earns Tested, never Verified (READABLE.md "Statuses").
- `tools/playbot/`: closed-loop players (testing gameplay means playing): the game stops
  at every pad read (`PS2X_PLAY_LINK`), the player reads its state and chooses the pad. `ai.py` (a
  career session, split screen, the Park Editor), `dm2.py`, `bmx.py`; each reports the game's own
  numbers. The custom-levels test park (CUSTOM-LEVELS.md, step 0): `aipark.py`, `dm2park.py`,
  `bmxpark.py` build it in each game's park editor and save it; `aipark.py ride` and `bikepark.py`
  ride the saved park: a tour of its features from `parkmap.py`'s map, made from the save and the
  pieces' own models (`RIDE=circuit`: the zone circuit in cells), and `memuse.py` reads the heap's
  high-water and the GS audit (`PARK_MEMORY=1`). Usage in each file's docstring.
- Spots (`PS2X_SPOT_FORK=<dir>`, Mac and Linux, `runtime/ps2_spot_fork.h`): a run that reaches a spot
  (`PS2X_SPOT_AT=tick>=N|script>=N`, or the file `<dir>/now`) stops there at a VBlank and forks a copy of
  the whole machine for each request file `<dir>/req/NAME` (its environment: a play link, dumps, a
  recorder), which goes on from that guest moment; headless, null GS, `ZAXIS_AUDIO=off`, no recorder in
  the parent. `tools/playbot/proofrun.py` plays a target list (bodies with blocks the corpus never ran)
  from the game's own starts and records only what is new (`PS2X_FNREC_NEW_BLOCKS=1` seeded with the
  corpus's coverage).
- `tools/sweep.py` (every level and mode), `tools/speedsweep.py`, `loadtime.py`, `framespikes.py`
  (speed), `tools/audiocheck.py` (sound).

## The player's own play: recordings and replays

Settings > Game > Record Play (settings.ini `[general] record_play`, off by default; from
the next start) records each boot to the game's `Recordings` folder (the Recordings line under it
says where and how many; on the Thor that is our package's external files folder, `replay.py pull`).
`ZAXIS_RECORD=<folder>` does the same for any run. Nothing is shown or asked while playing.
A `.zrec` (runtime/play_recording.h) holds what the game's thread read from the host (each pad
port as the game saw it after the keyboard, mouse, touch and gamepad mapping, mouse-look,
shortcuts, the prompt device, dates, the picture's aspect, libc rand, the settings as the game's own
Settings pages read them: rows, names, values), the build's commit, the game
and its ELF's hash, every BootConfig value and `PS2X_*` switch, the device (and Low Power Mode, or
the Thor's governor and battery), a copy of the saves at boot, EE RAM checksums per megabyte every
60 vsyncs, and each host frame's time and threads' CPU time on the same vsync clock. Records are
written every second and whole, so a crash keeps all but the last second; a file without its end
record ended in a crash or a kill.

`tools/replay.py` (`--help`): `info`, `moments` (hitches, slow stretches, stalls, pacer drops,
setting changes from the app menu, errors, restarts, the end or the crash, by vsync), `telemetry
--csv`, `run FILE [--to V | --moment N] [--from V --env PS2X_FNPROF=...] [--record]` (unpaced,
host input off, null GS, from power-on with the recorded saves and settings: `ZAXIS_REPLAY`,
`ZAXIS_REPLAY_STOP`; the runtime compares every checksum and says where the state first differs),
`twice FILE --at V,...` (two replays' RAM byte for byte), `compare A B` (two recordings' checksums),
`route FILE OUT` (a corpus recording run over the session), `script FILE OUT` (port 0 as a pad
script). A recording the app made (`front_end=app` in its header) replays through the app's front
end (`RUNNER --game GAME`, this machine's disc folder), which boots differently from a runner given
the ELF: replayed by a bare runner, every checksum differs. `run` turns on the readable functions the
recording's build ran (its commit's Verified rows, by address; `--readable runner` keeps the runner's): the
readable code does not charge the original's loop cycles, so a function promoted since moves the vsync a
read falls in (b131's strcpy moved the boot's first prompt read from vsync 4 to 3). A setting changed from the app's menus reaches the game at a host frame, not at a read of
its own: the replay applies it at the recorded vsync and says the state may differ from there.

## The career A/B

`tooling/checks/career_ab.py` (`--help`): `run NAME --dir DIR --runner R [--spec off] [--input
career|logic|frontend|messages|split|editor|crane|pisser|vacuum|FILE] [--clock K]`, `compare A B`,
`gate --dir DIR --runner R` (every input, both sides, at 4096x, against `ab_allow.txt`), `stale`
(stack poison compare), `packets A B TICK vif1`, `script TRACE OUT` (a pad script from a
`PS2X_PAD_TRACE` log, `--pokes` with a `PS2X_POKE_TRACE` log). The gate's rules are READABLE.md
"The gate". It compares every state field at every tick, every RAM word of each dump by region, the
scratchpad, and what rendering is handed (`PS2X_PACKET_LOG`: hashes of the VIF1, GIF and VIF0
streams and of VU0 per vsync), and prints what it did not compare. The inputs are
`tooling/checks/ab_inputs/`. Do not use the older scripts (statecmp.py, cmpstate2.py, same.sh,
ab.sh): they hide differences.

## Two flavours: the dev build and the app

One source, two builds (`ZAXIS_DEV_CHECKS`, runtime/CMakeLists.txt). ON, the default on Mac and Windows, is the dev
build every check runs: the recorder and the traced originals, diffcall, the cycle and tail-call profilers
(`PS2X_FNCYCLES`, `PS2X_TAILCALL_STATS`), the guest-call trace (`PS2X_GUEST_CALL`), the stack fills
(`PS2X_STACK_POISON*`), ps2xFnCheck and the unit tests. OFF is the app's flavour: `make-app.sh`,
`make-windows.ps1` and the Android build (where it is the default) build none of it, and the checkers' hooks are
constant nulls, so a call pays nothing for them; the runner's text is about 40% smaller. What the game computes is
the same in both. The app's flavour is proven against the dev build by `tooling/checks/flavour_ab.py` (every
game's A/B input on both binaries, the defaults on both, compared with no allow list; a side that stops short of the
input's end fails, as in the gate) and each game's logic route,
a step of every batch (`rcheck.sh COMMIT --flavour-ab`, the Mac's gate slot). The binary it proves is the one that
ships: the collection app's release build, made by `make-app.sh BUILD_ONLY=1` in the gate slot, and the app is packed
from it (`make-app.sh FROM_BUILD=<the job's app-build>`, which takes it when its stamp, the tree of CMakeLists.txt,
runtime and iop, the configure arguments with every path cut to its last part and the data folders they name by
content (a folder the recompiler made by its .ps2recomp-manifest, when its files are the ones it lists: a listing,
not a read of every file), is the one make-app.sh would build; it names no path, so a build from the gate slot's checkout and a
lane's tooling copy packs from main's; `STAMP_CHECK=1 FROM_BUILD=DIR make-app.sh` says which, building nothing;
`make-windows.ps1 -FromBuild` the same on Windows), not built again. `ZAXIS_GUEST_MAP_CHECK=ON` is a tool
build that checks every readable access against the resolution before the guest map and says where the special
addresses occur (ee/guest_map_check.cpp).

## Games one after another in one process

`python3 -I tooling/checks/seqcheck.py run ai,dm2 [dm2,ai ai,ai ...] --runner R` checks that a game
booted after others (the collection menu's boot, exit to the menu, boot) runs as it does in a process
of its own. It runs `ps2EntryRunner --sequence ai:STEP,dm2:STEP` (runtime/ps2_boot_sequence.h: each
step's ELF, tick limit, stop file, card and BootConfig values; the same teardown as an exit to the
menu, no menu drawn), with each game's input, dumps and state log from logiccheck, dm2logic and
bmxlogic. Each game is also run twice alone by the same runner; words those two runs differ in are
noise. Per step it prints MATCH or DIFF: the checker values at every checkpoint, each side's checker
verdict against PCSX2, and the RAM words that differ (dead stack and noise counted apart) by region
and by the nearest named data (readable_symbols.tsv, other games through their bind tables), every
word in `<out>/seq-*/N-GAME-words.txt`. Exit 0 only when every step matches. `step GAME PREFIX`
prints a step file.
A game override keeps what it learns while the game runs in its boot's session
(runtime/ps2_session.h: `ps2x::session::state<T>()`, made at first use and destroyed when the boot ends),
not in file statics, and registers with the host through it (`session::addSetting`, `session::onEnd` to
undo a provider or patch, `session::addShutdownHook`), so the next boot in the process starts clean.

The collection app itself (several games: it opens on its menu, collection/menu.h) is driven
headless by `PS2X_MENU_SCRIPT=game:dm2,Enter,...` (one step every 20 menu frames, across visits: key
names, `pad:cross` and the other pad buttons, `wait`, and `game:ID`, Left or Right to that game's card
by its id, wherever the menu shows it; the cards are in release order, DM2, AI, BMX XXX; a script
or a headless run skips the intro) and, inside a game, by `PS2X_CONTROL`'s
`set hotkey.switch_game 1` (the switch_game key's press: back to the menu; host input scripts do not
reach the [hotkeys]); `PS2X_MENU_SHOT_DIR=DIR` writes the menu's picture as a PNG at every change it shows (not
its movie's pictures; the menu's own settings page too: `Up,Enter` opens it). `[app] menu: first
picture N ms after the start` is the launch's time to a usable menu. `PS2X_MAP_WINDOW_SHOT=FILE.png` writes the map
window's picture (the bottom screen, 1240x1080) painted ten seconds after a level is in it, headless
too: the painter runs without the window. The log's
`[app] menu: boot ID`, `[app] ID: running`, `[app] ID: left for the menu` and `[app] menu (visit N)`
lines say what happened. A game left by its own Exit to Collection item logs `[dave-mirra] Exit to
Collection (its main menu|the pause menu)` (`[bmx-xxx]` alike; Aggressive Inline's item: `[aggressive-inline]
quit to desktop (menu)`) before `[app] ID: left for the menu`; a pad script chooses it with `PS2X_CONTROL`'s
`press`, and `shot FILE` takes the game's picture headless with the CPU renderer (`PS2X_GS_BACKEND=cpu`, no
`PS2X_GS_NULL`).

## Guest calls and test-only switches

- `PS2X_GUEST_CALL=HOOK@VSYNC:TARGET[:A0[:A1...]][,...]` calls a guest function from HOOK's entry at
  or after a vsync, for paths no input reaches (it changes what the game does). Argument forms
  (strings `$N` of `PS2X_GUEST_STRINGS`, zeroed buffers `#N`, word blocks `=V`, earlier results
  `^N`, expressions `%EXPR`, `+N` vsync offsets, `~ADDR` for a call that may wait, `W`/`H`/`B`
  stores): the header comment of z-axis `runtime/src/game/ee/guest_calls.cpp`.
- Memory card: `PS2X_MC_SLOT1`/`PS2X_MC_SLOT2` `none` or `unformatted` at boot, `PS2X_MC_FAIL=CMD:N:CODE`
  (the Nth libmc command fails as a pulled, full or changed card does), `PS2X_MC_FIXED_TIME=1`,
  `PS2X_MC_FD_BASE0=1`, `PS2X_JOB_RETURNS=1` (a card job's call is kept by the recorder),
  `PS2X_TRACE_MC=1`.
- `PS2X_POKE_SCRIPT` (`tick:address=value,...`), `PS2X_DIFFCALL` (generated calls,
  `diffcall.py --help`).

## Tracing a divergence

| Switch | What it writes |
|---|---|
| `PS2X_FNCYCLES=<addr>,...:<file>` (`@<list>`) | a line per call of those functions: tick, EE cycle at entry, cycles to the return, how it ended; `_ARGS=<file>` the arguments and returned registers, `_FROM=<tick>`, `_WATCH=<addr>[+<bytes>],...:<file>` a write-watch at every traced entry and return and every vsync, `_VU0=<reg>,...:<file>` VU0 registers the same way |
| `PS2X_TRACE_EE=<from>:<to>:<file>` | every EE cycle charge, checkpoint test and scheduler dispatch in that cycle range |
| `PS2X_SAFEPOINT_TRACE=1` | every fiber that leaves at a safe point and every resume |
| `PS2X_STACK_POISON=1` | fills the stack below each readable function's sp with 0x5A (a run against one without shows reads of stale stack) |
| `PS2X_READABLE_HELD_TRACE=1` (`=LO-HI[,LO-HI...]`, `=count`) | each guest call's place in the original's calls (ee/held.h): the site, the callee-saved slots given and their values, then the entry values handed on (`e<slot>=`), the callee's sp; only for the functions in those ranges; `count`: calls placed and not, at exit |
| `PS2X_READABLE_HELD_RANGE=[!]LO-HI,...` | the held-register table for the functions in those ranges only (less a `!` range): bisects a difference to a function's row |
| `PS2X_READABLE_SAFEPOINTS=0`, `PS2X_READABLE_FIBER_CALLS=0` | safe points, or the shared fiber off, to tell whether a difference is theirs (`PS2X_READABLE_FRAMES=0` stops at the first `ee::Local`: every one needs its function's frame) |
| `PS2X_READABLE_LEAN=1` (the app's default for AI) | the held-register table off (ee/abi.h `leanParts`): a third of the EE thread in AI's and BMX XXX's levels. The app's boot sets it; a runner given an ELF (the A/B, logiccheck) and ps2xFnCheck keep the table. `=held,saves,registers` for parts alone (saves off changes EE time: the original times itself by dead stack); `PS2X_READABLE_LEAN_RANGE=[!]LO-HI,...` saves and registers only in those functions |
| `PS2X_STATE_LOG=<addr>,...`, `PS2X_DUMP_WHEN="label:cond&cond"`, `PS2X_DUMP_RAM_ON_EXIT` | a line per vsync a listed word changed; RAM dumps at conditions (the 32 MB; `PS2X_DUMP_EXTENDED=1`: the 64 MB a recording's checksums cover) |
| `PS2X_TRACE_MOVIES=1`, `PS2X_TRACE_RPC`, `_CALLS`, `_VU1`, `_DMA`, `_MC`, `_EE` | subsystem traces |

The recorder's knobs (`PS2X_FNREC*`), ps2xFnCheck's flags and the corpus tools are READABLE.md "How a
function is proven". `ps2xVu1Check CORPUS --reads QWORD` counts loads of a VU1 word, `--poke` puts a
canary there.

The EE side of the same question (a leftover vector lane: a stale w, a spilled upper half, a frame word read
unset) is `tooling/checks/wlane.py` (its text has the model and the limits): `trace` follows a lane statically
through the recompiled code to its uses and landings; `poke` runs an input twice with `PS2X_LANE_POKE` forcing the
lane to two values (register lanes and frame words at calls, returns and entries: `runtime/lane_watch.h`) and
compares everything the career A/B compares, then watches the moved words (`PS2X_LANE_WATCH`, a
`ZAXIS_LANE_WATCH=ON` tool build: every load, store and readable access of them by its site; `PS2X_LANE_WATCH_PCS`
the addresses a site reaches; `PS2X_LANE_CALLERS` the run's call graph for `--callers`) and follows each load. A
dead lane's words go to the A/B as a `lanes=FILE input=NAME` allow line (career_ab.py). Where the static follow
cannot decide (a GPR's upper half in every caller, dead-stack reads, aliasing), `poke --diff` is the verdict: the lane
at four values (1.1, -123.456, 0x7FFFFFFF, 0xFFFFFFFF) and the first again, each run with `PS2X_LANE_DIFF` (the same
tool build, its DM2 generated code a `tooling/checks/wlane_branches.py` copy: every branch and register jump hooked):
per instruction its count, its addresses, directions or targets and its values, per callee its call sites. Dead on
that input when control flow, addresses and calls are the same in every run, nothing observable differs and the
repeat is identical; the loads whose values differ (where the lane is read) head the lanes file.

## The Thor's bottom screen

`runtime/include/collection/panel*.h`, `src/collection/panel*.cpp` (the collection layer): plain C++, no platform or game dependency; a game fills its snapshot (`runtime/panel_snapshot.h`) from its own override.
`captureSnapshot` reads guest RAM read-only once a game frame; `Panel::touch` and
`Panel::draw` run on the Android painter thread. `panel_policy.h` says which screen shows
and why: the default (follow map and second HUD), and the references (goal list, trick
table) that a pause brings and a line takes away, unless held.

    python3 -I tools/panel_map.py build --disc <disc> --out <dir> [--png <dir>] \
      [--zoo <dir>] [--estate <dir>]

makes `<LEVEL>.pmap` per level and `UI.pmap` at build time (make-app.sh, Resources/panel;
AI's eight levels, the Zoo and Estate): the map from the collidable faces seen from above, the
goals from the level script (`panel_goals.py`), the trick tables from the executable.
`ps2xPanelShot --data <dir> --ram <dump> --out <png> [--touch "ram <dump>; tap X Y; ..."]`
draws a real moment on the Mac from `nctl.py NAME "dump FILE"` dumps (DM2's: `--game dm2`, and
`--dm2-xbox-levels VENICE,GREENVIL` for its Xbox levels' values; their files from `build --game dm2
--dm2-xbox <dm2_xbox.py's folder>`). Shots are game
derived: scratch only.


## The remote workers

The shared tools stop with a message (exit 2) when started from the main checkout or told to write into it
(`tooling/zguard.py`: rcheck, lanecheck, `career_ab.py`, `run-game.sh`, the playbot, `replay.py`, round-merge's
helpers): run them from your worktree or scratch with absolute paths. The maintainers' merge and batch scripts,
which work there by design, export `ZAXIS_IN_MAIN=1`.

The maintainers check every batch on worker machines of their own (the unit tests on each system, the corpus
replays, logiccheck, the A/Bs), with scripts that are not part of this repository; a contributor runs the unit
tests and whatever else the change needs on their own machine (CONTRIBUTING.md).
Linux and macOS agree on every logiccheck row, state field, the scratchpad, VIF1 and VU0; only movie pixels differ
(FFmpeg versions), so compare a run with a run of the same host.
