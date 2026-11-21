# Developer tools: the games' own and the runner's

The games' own developer tools come first: Aggressive Inline's hidden debug menu and tuning table (every entry:
docs/TUNING.md), BMX XXX's and Dave Mirra BMX 2's TUNING table and screen, their debug displays, and the language
switch. Then the developer runner's switches for test runs, routes, recordings, the A/B and tracing, the disc and
archive tools and where the tools may run. Each tool's own `--help` or header comment has the details. The code
(z-axis): each game's `overrides/` (`games/<game>/overrides/`) `aggressive_inline_devtools.cpp`,
`dave_mirra_devtools.cpp`, `bmx_xxx_devtools.cpp`, the TUNING table `engine/overrides/zge_tuning.cpp` with
`bmx_xxx_tuning.cpp` and `dave_mirra_tuning.cpp`, and the tuning.ini parser `ps2/runtime/tuning_ini.h`. [V] =
verified in the running game or the code, [I] = inferred from a name.

## How to use

| What | App | Runner (developer) |
|---|---|---|
| Debug menu | Settings > Game > Developer Menu: On (next start), then **F1** in a level. Aggressive Inline opens its hidden debug menu (below). DM2 and BMX XXX have no debug menu: F1 turns their debug display on and off (DM2: the frame rate and free texture memory, the profiler and the engine's debug drawing; BMX XXX: the profiler, its backdrop and the debug drawing; `dave_mirra_devtools.cpp`, `bmx_xxx_devtools.cpp`) | `ZX_DEV_MENU=1`; F1, or `set dev.debug_menu 1` on ZX_CONTROL |
| Tuning overrides | `Mods/tuning.ini` in the game's support folder, or a mod's (the last mod that has one; docs/SETTINGS.md "Mods"), always read when it exists | `tuning.ini` in the last of ZX_MODS_DIR's folders that has one, or `ZX_TUNING_INI=<file>` |
| Reference of all entries | With the developer menu on, every start writes `Mods/tuning-reference.ini` | same, into ZX_MODS_DIR's first folder |
| Language (AI, BMX XXX) | Settings > Game > Language (next start; AI's with the Xbox release's files, docs/SETTINGS.md "Languages from the Xbox release") | `ZX_LANGUAGE=english\|french\|german\|spanish`; in AI, `uk` and, without the Xbox files, `french` and `german`: the PS2 disc's own string tables (below) |
| BMX XXX's tuning | `Mods/tuning.ini`, section `[TUNING]`: its GameCube release's TUNING rows (their names) on the PS2's values (bmx_xxx_tuning.cpp: 96 rows; the workdb notes, topic layout, give each row's PS2 global); with the developer menu, the reference file and **F2** in a level: the live TUNING screen (up and down pick a row, left and right step its value; after the table's rows, the riding rider's eight abilities) | the same; `ZX_TUNING_SCREEN=1` shows the screen from the start |
| DM2's tuning | the same as BMX XXX's, on DM2's values (dave_mirra_tuning.cpp: 60 of the GameCube's rows, each paired with the DM2 global by matched code or by both games' readable code reading it in the same term; the workdb notes, topic layout); the screen's abilities are the riding rider's seven; the table and the screen are one body for both games (zge_tuning.cpp) | the same |

The game's settings.ini (`games/<id>/settings.ini`), `[general]`: `developer_menu = true|false` in all three games,
`language = english|french|german|spanish` in AI and BMX XXX (`us` and `uk` read as English). The defaults are false
and english. With the defaults nothing is hooked or written: no tuning.ini means no tuning code runs, the developer
menu off means no hook, and English leaves the game's files as they are.

tuning.ini format (the reference file lists every entry this way, commented out). Aggressive Inline's:

    ; comment
    [Camera]
    Distance = 6
    Height = 0.8
    FOV = 85
    [Physics]
    Jump = 20

Sections are the game's categories (any case), names are the entries' names (any case). BMX XXX and Dave Mirra BMX 2
have one section, `[TUNING]`, with BMX XXX's GameCube row names. A name without a section is looked up in every
category; a name that two categories have (AI's `Forward Friction`, `Side Friction`) needs its section. Unknown names
and bad numbers are logged (`[tuning] ...`) and skipped; values outside the table's own min/max are applied and
flagged. Values are written once when the game starts, before its first instruction. In AI, Far Clip, the one entry
the game writes itself (at every level start), is written again at every vsync. Any tuning change can change how the
game plays: logic checks and replays assume no tuning.ini.

## Aggressive Inline's hidden debug menu [V]

AI has 8 game modes in a table at 0x24B810, `{mode, enter, exit,
update, 0}` (0x14 bytes each); `func_001A70F0(mode)` runs the old mode's
exit and the new one's enter. Mode 2 is a level (update 0x1ACA68), 3 the
pause menu, 0 the Art Viewer, 6 the debug menu: enter 0x1BC470 plays
`InGameUI.ShowDebug` (`MultiplayerUI.ShowDebug` in split screen), saves
the input mode and sets it to 3 (menus); exit 0x1BC5B0; update 0x1BC518 (the UI update; it also sets the
camera from the tuning table's Camera `Type` every frame). Nothing in the
retail game enters mode 6: no caller passes 6. The hook (only with the developer menu on) wraps the level's
update: on F1 it runs `func_001A70F0(6)` in place of one update, like a tail
call. (0x276FE4, the "game mode" the other tools read, is the input mode
the modes set: 1 front end, 2 play, 3 menus; it reads 3 in the debug menu.)

The menu is InGame.zsc's `Debug`: a guiFrame with a guiMenu in the game's arial16 font, "DEBUGMENU STATE" top left.

| Item | What it does (inlineDebugMenuSelect 0x1BC600) |
|---|---|
| Resume | back to the level (mode 2) |
| Art Viewer | mode 0: a free camera. The skater and the level stop; the d-pad and buttons move and turn the camera (button queries 0x18A1E8/0x18A210 on the game's own button bits: Up 0x1, Down 0x2, Left 0x4, Right 0x8, Triangle 0x10, Square 0x20, Circle 0x40, Cross 0x80, L1 0x100, R1 0x200, L2 0x400, R2 0x800, Start 0x4000, Select 0x8000). **Start** goes back to the level (and the same press opens the pause menu); Select calls 0x110BC0(0). The view is upside down in the Tutorial, as the game's code sets it up. |
| Tuning | the tuning table editor: a list of the 20 categories; Cross opens one as a two-column list of names and values, left/right step the selected value by its step within min/max (`inlineTuneDown`/`inlineTuneUp`), Triangle goes back. Changes are live where the game reads the entry live (docs/TUNING.md). |
| Respawn | sets skater flag 0x4 and resets the skater to the respawn point (0x193DE0), back to the level |
| Toggle Debug | cycles a debug display level (low bits of 0x24C9B0) 0, 1, 2: level 1 shows physics state text (state, substate, edge, balance, object and terrain names) and VU/CPU/DMA timing bars top right; level 2 only small markers; 0 off (0x150CE0 and 0x14B7D0 with 0/1) |

Cancel (Triangle) on the menu is Resume.

## Aggressive Inline's PS2 string tables [V]

AI's Language row's French, German and Spanish are the Xbox release's text
(docs/SETTINGS.md "Languages from the Xbox release"; `aggressive_inline_language.cpp`).
This section is the PS2 disc's own string tables. A run gets them with
`ZX_LANGUAGE=uk`, or with `french` or `german` without the Xbox files or the
settings menus (a runner given the ELF).

- The game loads one string table at start: `assets\string.eus` (name at
  0x268788, loaded by FUN_001BC9F0 through 0x1483B8, made current with
  0x148628). The disc also has `STRING.EUK`, `.FRE`, `.GER` (the same 236
  keys, hash-sorted) and `STRINGLE.*` (275 park editor piece names, Dave
  Mirra leftovers, identical in all four files, never loaded). The switch
  changes the three letters of the name before the game runs.
- **What the table holds:** memory card messages (the Sony-style card UI at
  0x1E80B8-0x1EE4F0, the only users of the current table), trick names
  (identical in all four files), and BMX tips from Dave Mirra ("tap C to
  turn", "pedalling") that AI never shows.
- **What changes:** only the memory card texts, which show only with
  card saves (`ZX_SAVES=card`; the app's native saves never show them).
  Even there the French and German files are mostly English (the long
  "memory card (PS2) in MEMORY CARD slot" texts are untranslated) and out
  of step: the French "Save game?" key holds "No memory card (PS2) found in
  MEMORY CARD slot %d.", so the save dialog asks the wrong question with a
  garbage slot number. UK English differs in 15 strings.
- **What stays English:** everything the player normally sees. Menus,
  pause pages, HUD texts and button prompts are UIBuilder scripts
  (UILIB.ZAL, literal `Text` properties); zgui looks its text up in its
  own table (0x240C70 via 0x1714E8), not in STRING.*; Tutorial tips and
  goals are level data; textures (titles, the main menu words) and speech
  are English on this disc. With native saves such a run changes nothing visible.

## Test runs: any level, any state

For recording or checking code in states the logiccheck and career inputs never reach. All are runner
environment, off by default, and hook nothing when off.

The app passes nothing through the environment. Each game it boots gets a BootConfig (runtime/boot_config.h;
its settings, folders and renderer, `zx::app::gameBootConfig`). The runtime and the game overrides read a `ZX_*`
name with `zx::boot::text`; the environment, when it sets the name, still wins. A check that boots several games in
one process gives each a BootConfig of its own: the pad and host input scripts, `ZX_DUMP_WHEN`, `ZX_DUMP_PREFIX`,
`ZX_STATE_LOG(_FILE)`, `ZX_PAD_DUMP`/`ZX_PAD_LOG`, `ZX_POKE_SCRIPT`, `ZX_SAVES`, `ZX_MC_FD_BASE0`,
`ZX_MC_FIXED_TIME` and `ZX_DEV_UNLOCK_ALL` are read again at every boot (`BootConfig::names()` lists what it
carries). The other switches are the process's.

| Switch | What |
|---|---|
| `ZX_DEV_UNLOCK_ALL=1` | AI and DM2. In AI, in the front end, writes what the game's two unlock codes write plus the Media Viewer cutscenes (Settings > Cheats > Unlock Everything; docs/CHEATS.md). It goes into the career save the run writes: use a scratch HOME, card and support folder. In DM2 it opens every level and every trick (`ZX_DM2_OPEN_LEVELS=1`, `ZX_DM2_OPEN_TRICKS=1`: each alone) |
| `ZX_DEV_START_LEVEL=<entry>` | the runtime presses its own way from boot to a level, through the game's own menus. AI: the free skate level-select entry (Movie Lot 0 ... Museum 6) or `ESTATE`. DM2: the FREERIDE level select's value (0 Woodward Camp ... 7 Airport) or `PARK`. BMX XXX: the HARDCORE TOUR level select's row (0 the Bronx) or a level's folder name (`CMP_RMPG`). `ZX_DEV_START_SKATER=<n>` (the skater or rider entry, or `ask`), `ZX_DEV_START_QUIT=<s>` (stop s seconds after); in AI `ZX_DEV_START_SKATE=<n>` (rounds of sweep.py's skate routine). Logs `[devstart] placed ... load N s` |
| `ZX_SOUND_LOG=<file>` | every ZAudio command (fn, payload) and the channels' status bytes the native driver (or ZAUDIO on the IOP) writes, in guest time |
| `ZX_SOUND_PCM=<file>` | every frame the native sound driver makes (16-bit stereo, 48 kHz, raw), in guest time. With `ZX_SOUND_LOG`, it proves a sound change leaves the sound alone: both are byte-identical on a fixed input |
| `ZX_VSYNC_PHASE=0` | paced runs: the guest's vblank no longer held in phase with the window's latch (runtime/host_pacing.h). On by default, the lock keeps a 60 frames/s game's frames 16.7 ms apart on a 120 Hz display, and moves the vblanks a refresh later while a frame waits a refresh late. Host time only: the game's logic follows EE cycles |
| `ZX_FRAME_QUEUE=0` | the window shows the newest picture at each refresh (latched on the window thread). By default every game frame's picture is made on the GS worker at its buffer swap and waits for the window (GS::setFrameQueue), so none is lost to a host frame that took two refreshes; headless runs too, so their pictures (`shot`, `ZX_SHOT_AT`, the Screenshot key) are never a buffer being drawn again. The pictures the window still latches itself (the first, after a renderer switch, a stretch with no swap, everything with the queue off) are taken again when the game swapped while one was copied (by the display's change count, so A, B, A too), and one the game swapped through every try is not kept. `ZX_PERF_LOG`'s `[perf] shown:` line counts the game frames by the refreshes they stayed on screen, those never shown, and the time from a picture to its present |
| `ZX_AUDIO=off` | no sound: the audio device is SDL's dummy driver, so everything before the speakers (the SPU2 mix, the host streams, the device's pace) runs as with sound. Unpaced runs (`ZX_UNPACED=1`) get it too; `ZX_AUDIO=on` plays sound anyway. The tooling's shared environment (`paths.sh`, `zpaths.py`) sets `off` for every test run, paced ones included: a run that has to be heard (audiocheck) sets `on`. `[host] audio:` says which driver |
| `ZX_VRR=1` | variable refresh (the app's Display > Variable Refresh): each game frame is presented when it is ready and a G-Sync, FreeSync or adaptive-sync display follows; on a fixed-rate display a frame waits for the next refresh with no phase lock |
| `ZX_TRACE_HEAP=1` | the guest heap's high-water marks per level (`runtime/heap_trace.h`): the bytes in use and the peak, in the 32 MB heap and in extended memory, of each stretch of one level name (the panel capture's level folder, `no level` in the front end and while loading), logged when it ends and every 600 vsyncs (`[heap] woodward (ended): 32 MB heap peak ... ticks A..B`); `tools/playbot/memuse.py` `heap()` reads them, a park's `PARK_MEMORY` run sets it. Off: nothing installed |
| `ZX_GS_SHOT_VSYNC="T,T,...:PREFIX"` | all three games: at each vsync tick T, the display as the GS has it once it has done everything sent before that vsync (a GS sideband, in stream order: behind a buffer swap the EE queued, so never the buffer the next frame is being drawn into), drawn to `PREFIX-vT.png`. Two runs of one input give the same pictures whatever the host's timing, so render paths can be compared (strict against records). The GPU renderer's are at its internal resolution. `ZX_GS_SHOT_PRIMS=N`: every primitive the GS draws in the N vsyncs before each picture to `PREFIX-vT.prims` (GS::logPrimitive's records: the state and three vertices, a native draw's marked), to set two paths' draws side by side; `ZX_GS_SHOT_TRACE=1` with it: a GS trace of those vsyncs (`PREFIX-vT.gsr`) for `zx-gsreplay --gpu`. One difference is deliberate: the GPU draws native meshes crossing the near plane whole, the PS2 (and the CPU renderer) as its clipping program's split, clamped triangles |
| `ZX_TRACE_NATIVE=1` (`=2`) | `render_path = records`: the draws drawn from their records by class and those left to the strict encoder by why, every 20,000 draws (`[native-records] ...`); `=2` also each part left without a mesh, once, and why |
| `ZX_PRESENT_LOG=<file>` | a line for every new picture the window shows (`present`: host frame, vsync, the game frame it is, `latched=1` for one the window latched itself, `disturbed=1` for one copied while the game swapped, the CPU renderer's pixels hashed) and for every shot (`shot`, the same for its picture): two runs' presentation compared frame by frame, and shots checked for torn pictures |
| `ZX_TEXTURE_DUMP=<folder>` (`ZX_TEXTURE_DUMP_DIRECT=1`: RGB textures too, movie frames included) | texture packs' dump mode, all three games, either renderer: every texture the GS draws, once, as a PNG named as PCSX2 names it (`<texture>-<clut>-<bits>.png`, runtime/texture_pack.h), the palette applied and the PS2's alpha (0x80 opaque). An edited copy (a PNG, or a DDS of the same name) in a pack folder (`ZX_TEXTURE_PACKS=<folder>`, docs/SETTINGS.md "Texture packs") is drawn in its place. `ZX_TEXTURE_PRELOAD=0` reads a pack's pictures at first use only, `ZX_TEXTURE_PACK_MB=<n>` sets their memory budget, `ZX_TEXTURE_PACK_UPLOAD_MB=<n>` the GPU uploads of pictures per frame (6), `ZX_TEXTURE_PRELOAD=wait` makes the boot wait for the pictures, `ZX_TRACE_TEXTURE_REPLACE=1` names each picture as it is first drawn. `ZX_TEXTURE_PACK_LOD_STATS=1` (CPU renderer): at exit, which of PCSX2's names (hardware mipmapping on) the textured primitives get and how many the packs' lookup has |
| `ZX_GSMEM_AUDIT=1` (`=2`: every overrun) | all three games: the GS texture memory per level: allocations, peak block, the CLUTs' floor, overruns (`[gsmem] ...` lines); converted levels must stay under the CLUTs |
| `ZX_VIRTUAL_PADS=2`, `ZX_VIRTUAL_PADS_FILE` | a second pad; without one, port 1 is empty and no game offers multiplayer (AI skips its MULTIPLAYER icon, DM2 and BMX XXX grey theirs). `ZX_PAD_PORT1=mirror`: port 1 a copy of port 0, for split screen with one input |
| `ZX_HOST_INPUT_SCRIPT` | host keyboard, mouse and focus events (`<t>:KEY=<name>[:<dur>]`, `<t>:MOUSE=<dx>/<dy>[:<dur>]`, `<t>:FOCUS=0\|1`, times in seconds or `120v`) through the same mapping as real input, on the pad script's clock; real input is then ignored. `ZX_TRACE_HOST_INPUT=1` logs the mapped pad, `ZX_TRACE_MOUSELOOK=1` the mouse-look camera, `ZX_SHOT_AT="<t>:<file.png>,..."` saves pictures. `games/aggressive/tools/inputcheck.py` runs the input checks with them |
| `ZX_PROMPTS=keyboard\|playstation\|xbox\|nintendo` | fixes the button prompts |
| `ZX_CLEAN_TOP_SCREEN=0\|1\|always` | Clean Top Screen (docs/SETTINGS.md Display) over the app's setting: `1` as the setting on (the HUD pieces a bottom screen shows left off the top screen while it shows them), `always` every piece left out whether or not a bottom screen is up (logiccheck and shots; with `ZX_GAME_SETTINGS_MENUS=1`, which installs the games' widget-draw hooks). Each game logs the first draw of each widget it leaves out (`Clean Top Screen: InGameUI.OverlayToggle.Timer left off the top screen`) and the count when the level is left (runtime/panel_snapshot.h) |
| `ZX_CONTROLS_INI=<file>` | another controls.ini (automated runs never touch the real one) |

Example, unattended on any machine (AI's ELF):

    ZX_DEV_START_LEVEL=0 ZX_DEV_START_SKATE=1 ZX_DEV_START_QUIT=3 ZX_PERF_LOG=1 \
      <zx-runner> recomp/disc-root/SLUS_203.27

## Routes, play and the control channel

- `ZX_CONTROL=<dir>` and `tools/checks/nctl.py` (DM2's: `games/dm2/tools/nctl.py`; `nctl.py start NAME`, `nctl.py NAME
  "CMD; CMD"`, `stop`): drive a running game from outside (presses, waits, pokes, dumps, shots, `set` a setting,
  `padport 0|1` pulls and plugs the pad).
- `games/aggressive/tools/routes.py` (AI): closed-loop routes over the control channel, so moves land whatever the host's
  speed: `routes.py start NAME [KEY=VAL ...]`, `routes.py NAME "boot; tutorial; moves"`, `stop NAME`.
  Its docstring lists every command: the Tutorial's moves (stall, poles, skitch, wall ride, grind,
  manual ...), `career K`, `freeskate K`, `teleport`, `ride` (a take-off off a face with launch
  values), `grindtricks`, `hazards`, `splitscreen`, the Park Editor's `pe2*`, `giver NAME`, `carddialog`,
  `menumash`, and `mash [FRAMES [SEED [KEY=VAL ...]]]` (seeded play that keeps going, teleporting the
  skater on a bail or a stop; `style=natural` for live-play profiles). Test-only commands poke the
  game: what they record earns Tested, never Verified (READABLE.md "Statuses").
- `tools/playbot/`: closed-loop players (testing gameplay means playing). The game stops
  at every pad read (`ZX_PLAY_LINK`); the player reads its state and chooses the pad. `ai.py` (a
  career session, split screen, the Park Editor), `dm2.py`, `bmx.py`; each reports the game's own
  numbers. Each game's park players ride a park built in that game's own editor: `aipark.py`, `dm2park.py`,
  `bmxpark.py` build it in the park editor and save it; `aipark.py ride` and `bikepark.py`
  ride the saved park: a tour of its features from `parkmap.py`'s map, made from the save and the
  pieces' own models (`RIDE=circuit`: the zone circuit in cells), and `memuse.py` reads the heap's
  high-water and the GS audit (`PARK_MEMORY=1`). Usage in each file's docstring.
- Spots (`ZX_SPOT_FORK=<dir>`, Mac and Linux, `runtime/spot_fork.h`): a run that reaches a spot
  (`ZX_SPOT_AT=tick>=N|script>=N`, or the file `<dir>/now`) stops there at a VBlank and forks a copy of
  the whole machine for each request file `<dir>/req/NAME` (its environment: a play link, dumps, a
  recorder), which goes on from that guest moment; headless, null GS, `ZX_AUDIO=off`, no recorder in
  the parent. `tools/playbot/proofrun.py` plays a target list (bodies with blocks the corpus never ran)
  from the game's own starts and records only what is new (`ZX_FNREC_NEW_BLOCKS=1` seeded with the
  corpus's coverage).
- `tools/checks/sweep.py` (every level and mode), `tools/checks/speedsweep.py`, `loadtime.py`, `framespikes.py`
  (speed), `games/aggressive/tools/audiocheck.py` (sound).

## The player's own play: recordings and replays

Settings > Game > Record Play (settings.ini `[general] record_play`, off by default; from the next start) records
each boot to the game's `Recordings` folder. The Recordings line under it says where and how many; on Android that
is the app's external files folder (`replay.py pull`). `ZX_RECORD=<folder>` does the same for any run. Nothing is
shown or asked while playing.

A `.zrec` (runtime/play_recording.h) holds:
- what the game's thread read from the host: each pad port as the game saw it after the keyboard, mouse, touch and
  gamepad mapping, mouse-look, shortcuts, the prompt device, dates, the picture's aspect, libc rand, and the
  settings as the game's own Settings pages read them (rows, names, values);
- the build's commit, the game and its ELF's hash, every BootConfig value and `ZX_*` switch;
- the device (and Low Power Mode, or on Android the CPU governor and battery);
- a copy of the saves at boot;
- EE RAM checksums per megabyte every 60 vsyncs, and each host frame's time and threads' CPU time on the same vsync
  clock.

Records are written every second and whole, so a crash keeps all but the last second. A file without its end
record ended in a crash or a kill.

`tools/checks/replay.py` (`--help`):
- `info`; `moments` (hitches, slow stretches, stalls, pacer drops, setting changes from the app menu, errors,
  restarts, the end or the crash, by vsync); `telemetry --csv`.
- `run FILE [--to V | --moment N] [--from V --env ZX_FNPROF=...] [--record]`: unpaced, host input off, null GS,
  from power-on with the recorded saves and settings (`ZX_REPLAY`, `ZX_REPLAY_STOP`). The runtime compares every
  checksum and says where the state first differs.
- `twice FILE --at V,...` (two replays' RAM byte for byte), `compare A B` (two recordings' checksums), `route FILE
  OUT` (a corpus recording run over the session), `script FILE OUT` (port 0 as a pad script).

A recording the app made (`front_end=app` in its header) replays through the app's front end (`RUNNER --game GAME`,
this machine's disc folder), which boots differently from a runner given the ELF: replayed by a bare runner, every
checksum differs. `run` turns on the readable functions the recording's build ran (its commit's Verified rows, by
address; `--readable runner` keeps the runner's). The readable code does not charge the original's loop cycles, so
a function made readable after the recording moves the vsync a read falls in. A setting changed from the app's
menus reaches the game at a host frame, not at a read of its own: the replay applies it at the recorded vsync and
says the state may differ from there.

The app plays recordings back itself (Settings > Recordings, `collection/recordings.h`): the same replay, paced,
with the window's own input live for Pause and Leave (`collection/playback.h`). The boot is the recorded one
(every BootConfig value from the header, but the renderer, the screenshots' folder and the developer switches
of this run), from the recording's saves copied into the game's `Playback` folder; nothing the player keeps is
written while it plays. While recording, the app notes each level the game enters (`level` events: the panel
file's name) for the page. `--play FILE` (or `ZX_APP_PLAY`) plays one back with no menu first, as its row
does; `ZX_APP_PLAYBACK_SCRIPT="pause@600/2.5,leave@1800"` gives the player's Pause (for 2.5 s of host time)
and Leave at those vsyncs. The log's `[app] playback:` lines and the runtime's `[replay] summary:` give the
verdict.

## The career A/B

`tools/checks/career_ab.py` (`--help`): `run NAME --dir DIR --runner R [--spec off] [--input
career|logic|frontend|messages|split|editor|crane|pisser|vacuum|FILE] [--clock K]`, `compare A B`,
`gate --dir DIR --runner R` (every input, both sides, at 4096x, against `ab_allow.txt`), `stale`
(stack poison compare), `packets A B TICK vif1`, `script TRACE OUT` (a pad script from a
`ZX_PAD_TRACE` log, `--pokes` with a `ZX_POKE_TRACE` log). The gate's rules are READABLE.md
"The gate". It compares every state field at every tick, every RAM word of each dump by region, the
scratchpad, and what rendering is handed (`ZX_PACKET_LOG`: hashes of the VIF1, GIF and VIF0
streams, the GS register writes and VU0 per vsync), and prints what it did not compare. The inputs are
`tools/checks/ab_inputs/`. Do not use the older scripts (statecmp.py, cmpstate2.py, same.sh,
ab.sh): they hide differences.

## Two flavours: the dev build and the app

One source, two builds (`ZX_DEV_CHECKS`, ps2/CMakeLists.txt).
- ON, the default everywhere but Android, is the dev build every check runs: the recorder and the traced originals,
  diffcall, the cycle and tail-call profilers (`ZX_FNCYCLES`, `ZX_TAILCALL_STATS`), the guest-call trace
  (`ZX_GUEST_CALL`), the stack fills (`ZX_STACK_POISON*`), zx-fncheck and the unit tests.
- OFF is the app's flavour: `make-app.sh`, `make-windows.ps1` and the Android build (where it is the default) build
  none of it, and the checkers' hooks are constant nulls, so a call pays nothing for them; the runner's text is
  about 40% smaller.

What the game computes is the same in both. `tools/checks/flavour_ab.py` proves the app's flavour against the dev
build: every game's A/B input on both binaries, the defaults on both, compared with no allow list (a side that
stops short of the input's end fails, as in the gate), and each game's logic route. The binary it proves is the one
that ships: the collection app's release build, made by `make-app.sh BUILD_ONLY=1`. The app is packed from it
(`make-app.sh FROM_BUILD=<the job's app-build>`), not built again. FROM_BUILD takes the build when its stamp is the
one make-app.sh would build: the tree of CMakeLists.txt, runtime and iop, the configure arguments with every path
cut to its last part, and the data folders they name, by content (a folder the recompiler made by its
.ps2recomp-manifest, when its files are the ones it lists: a listing, not a read of every file). The stamp names no
path, so a build from one checkout packs from another's. `STAMP_CHECK=1 FROM_BUILD=DIR make-app.sh` says which,
building nothing; `make-windows.ps1 -FromBuild` does the same on Windows. `ZX_GUEST_MAP_CHECK=ON` is a tool build
that checks every readable access against the resolution before the guest map and says where the special
addresses occur (ee/guest_map_check.cpp).

## Games one after another in one process

`python3 -I tools/checks/seqcheck.py run ai,dm2 [dm2,ai ai,ai ...] --runner R` checks that a game
booted after others (the collection menu's boot, exit to the menu, boot) runs as it does in a process
of its own. It runs `zx-runner --sequence ai:STEP,dm2:STEP` (runtime/boot_sequence.h: each
step's ELF, tick limit, stop file, card and BootConfig values; the same teardown as an exit to the
menu, no menu drawn), with each game's input, dumps and state log from logiccheck, dm2logic and
bmxlogic. Each game is also run twice alone by the same runner; words those two runs differ in are
noise. Per step it prints MATCH or DIFF: the checker values at every checkpoint, each side's checker
verdict against PCSX2, and the RAM words that differ (dead stack and noise counted apart) by region
and by the nearest named data (readable_symbols.tsv, other games through their bind tables), every
word in `<out>/seq-*/N-GAME-words.txt`. Exit 0 only when every step matches. `step GAME PREFIX`
prints a step file.

A game override keeps what it learns while the game runs in its boot's session
(runtime/session.h: `zx::session::state<T>()`, made at first use and destroyed when the boot ends),
not in file statics, and registers with the host through it (`session::addSetting`, `session::onEnd` to
undo a provider or patch, `session::addShutdownHook`), so the next boot in the process starts clean.

The collection app itself (several games: it opens on its menu, collection/menu.h) is driven headless by
`ZX_MENU_SCRIPT=game:dm2,Enter,...`: one step every 20 menu frames, across visits. Steps are key names, `pad:cross`
and the other pad buttons, `wait`, `game:ID` (Left or Right to that game's card by its id, wherever the menu shows
it), and `drop:PATH` (a file or folder dropped on the window, as the Games page's Find Discs takes it). The cards are
in release order: DM2, AI, BMX XXX. A script or a headless run skips the intro; the script waits while a setup job
runs. Inside a game, `ZX_CONTROL`'s `set hotkey.switch_game 1` presses the switch_game key (back to the menu; host
input scripts do not reach the [hotkeys]). `ZX_MENU_SHOT_DIR=DIR` writes the menu's picture as a PNG at every change
it shows (not its movie's pictures; the menu's own settings page too: `Up,Enter` opens it). `[app] menu: first
picture N ms after the start` is the launch's time to a usable menu. `tools/checks/settings_shots.py` pictures a
game's Settings page with no extras discs and with every one (the rows `collection/offered.h` hides).
`ZX_MAP_WINDOW_SHOT=FILE.png` writes the map window's picture (the bottom screen, 1240x1080) painted ten seconds
after a level is in it, headless too: the painter runs without the window (and counts as a bottom screen while it
paints: the menu's and the game's Clean Top Screen rows show). `ZX_PAIR_SHOT=<trigger file>` takes the top and the
bottom screen at one moment: when the file appears (its first line a path prefix) the game's next picture goes to
`PREFIX-top.png` and the map window's last to `PREFIX-map.png`, and the file is removed (a player holding the game
at a pad read gets one moment in both).

The log's `[app] menu: boot ID`, `[app] ID: running`, `[app] ID: left for the menu` and `[app] menu (visit N)`
lines say what happened. A game left by its own Exit to Collection item logs, before `[app] ID: left for the menu`:
`[dave-mirra] Exit to Collection (its main menu|the pause menu)`, `[bmx-xxx]` alike, and for Aggressive Inline
`[aggressive-inline] quit to desktop (menu)`. A pad script chooses it with `ZX_CONTROL`'s `press`, and `shot FILE`
takes the game's picture headless with the CPU renderer (`ZX_GS_BACKEND=cpu`, no `ZX_GS_NULL`).

## Guest calls and test-only switches

- `ZX_GUEST_CALL=HOOK@VSYNC:TARGET[:A0[:A1...]][,...]` calls a guest function from HOOK's entry at
  or after a vsync, for paths no input reaches (it changes what the game does). Argument forms
  (strings `$N` of `ZX_GUEST_STRINGS`, zeroed buffers `#N`, word blocks `=V`, earlier results
  `^N`, expressions `%EXPR`, `+N` vsync offsets, `~ADDR` for a call that may wait, `W`/`H`/`B`
  stores): the header comment of z-axis `ps2/ee/guest_calls.cpp`.
- Memory card: `ZX_MC_SLOT1`/`ZX_MC_SLOT2` `none` or `unformatted` at boot, `ZX_MC_FAIL=CMD:N:CODE`
  (the Nth libmc command fails as a pulled, full or changed card does), `ZX_MC_FIXED_TIME=1`,
  `ZX_MC_FD_BASE0=1`, `ZX_JOB_RETURNS=1` (a card job's call is kept by the recorder),
  `ZX_TRACE_MC=1`.
- `ZX_POKE_SCRIPT` (`tick:address=value,...`), `ZX_DIFFCALL` (generated calls,
  `diffcall.py --help`).

## Tracing a divergence

| Switch | What it writes |
|---|---|
| `ZX_FNCYCLES=<addr>,...:<file>` (`@<list>`) | a line per call of those functions: tick, EE cycle at entry, cycles to the return, how it ended; `_ARGS=<file>` the arguments and returned registers, `_FROM=<tick>`, `_WATCH=<addr>[+<bytes>],...:<file>` a write-watch at every traced entry and return and every vsync, `_VU0=<reg>,...:<file>` VU0 registers the same way |
| `ZX_TRACE_EE=<from>:<to>:<file>` | every EE cycle charge, checkpoint test and scheduler dispatch in that cycle range |
| `ZX_SAFEPOINT_TRACE=1` | every fiber that leaves at a safe point and every resume |
| `ZX_STACK_POISON=1` | fills the stack below each readable function's sp with 0x5A (a run against one without shows reads of stale stack) |
| `ZX_READABLE_HELD_TRACE=1` (`=LO-HI[,LO-HI...]`, `=count`) | each guest call's place in the original's calls (ee/held.h): the site, the callee-saved slots given and their values, then the entry values handed on (`e<slot>=`), the callee's sp; only for the functions in those ranges; `count`: calls placed and not, at exit |
| `ZX_READABLE_HELD_RANGE=[!]LO-HI,...` | the held-register table for the functions in those ranges only (less a `!` range): bisects a difference to a function's row |
| `ZX_READABLE_SAFEPOINTS=0`, `ZX_READABLE_FIBER_CALLS=0` | safe points, or the shared fiber off, to tell whether a difference is theirs (`ZX_READABLE_FRAMES=0` stops at the first `ee::Local`: every one needs its function's frame) |
| `ZX_READABLE_LEAN=1` (the app's default for AI) | the held-register table off (ee/abi.h `leanParts`): a third of the EE thread in AI's and BMX XXX's levels. The app's boot sets it; a runner given an ELF (the A/B, logiccheck) and zx-fncheck keep the table. `=held,saves,registers` for parts alone (saves off changes EE time: the original times itself by dead stack); `ZX_READABLE_LEAN_RANGE=[!]LO-HI,...` saves and registers only in those functions |
| `ZX_STATE_LOG=<addr>,...`, `ZX_DUMP_WHEN="label:cond&cond"`, `ZX_DUMP_RAM_ON_EXIT` | a line per vsync a listed word changed; RAM dumps at conditions (the 32 MB; `ZX_DUMP_EXTENDED=1`: the 64 MB a recording's checksums cover) |
| `ZX_TRACE_MOVIES=1`, `ZX_TRACE_RPC`, `_CALLS`, `_VU1`, `_DMA`, `_MC`, `_EE` | subsystem traces |

The recorder's knobs (`ZX_FNREC*`), zx-fncheck's flags and the corpus tools are READABLE.md "How a
function is proven". `zx-vu1check CORPUS --reads QWORD` counts loads of a VU1 word, `--poke` puts a
canary there.

The EE side of the same question (a leftover vector lane: a stale w, a spilled upper half, a frame word read
unset) is `tools/checks/wlane.py` (its text has the model and the limits):
- `trace` follows a lane statically through the recompiled code to its uses and landings.
- `poke` runs an input twice with `ZX_LANE_POKE` forcing the lane to two values (register lanes and frame words at
  calls, returns and entries: `runtime/lane_watch.h`) and compares everything the career A/B compares. It then
  watches the moved words (`ZX_LANE_WATCH`, a `ZX_LANE_WATCH_BUILD=ON` tool build: every load, store and readable
  access of them by its site; `ZX_LANE_WATCH_PCS` the addresses a site reaches; `ZX_LANE_CALLERS` the run's call
  graph for `--callers`) and follows each load. A dead lane's words go to the A/B as a `lanes=FILE input=NAME`
  allow line (career_ab.py).
- `poke --diff` is the verdict where the static follow cannot decide (a GPR's upper half in every caller,
  dead-stack reads, aliasing): the lane at four values (1.1, -123.456, 0x7FFFFFFF, 0xFFFFFFFF) and the first again,
  each run with `ZX_LANE_DIFF` (the same tool build, its DM2 generated code a `tools/checks/wlane_branches.py`
  copy: every branch and register jump hooked): per instruction its count, its addresses, directions or targets
  and its values, per callee its call sites. The lane is dead on that input when control flow, addresses and calls
  are the same in every run, nothing observable differs and the repeat is identical; the loads whose values differ
  (where the lane is read) head the lanes file.

## The bottom screen (dual-screen devices)

`collection/panel*.h`, `collection/panel*.cpp` (the collection layer): plain C++, no platform or
game dependency; a game fills its snapshot (`runtime/panel_snapshot.h`) from its own override.
`captureSnapshot` reads guest RAM read-only once a game frame; `Panel::touch` and `Panel::draw` run on the Android
painter thread. `panel_policy.h` says which screen shows and why: the default (follow map and second HUD), and the
references (goal list, items map, trick table) that a pause brings and a line takes away, unless held.

    python3 -I tools/modding/panel_map.py build --disc <disc> --out <dir> [--png <dir>] \
      [--zoo <dir>] [--estate <dir>]

makes `<LEVEL>.pmap` per level and `UI.pmap` at build time (make-app.sh, Resources/panel). An app with no game data
makes the same files at its setup from the player's disc (`collection/extras/panel_maps.h`), and `zx-extras panel`
does it from a terminal. By default it covers AI's eight levels, the Zoo and Estate; `--game dm2` and `--game bmx`
the other games' levels. The map is the collidable faces seen from above, the goals come from the level script
(`panel_goals.py`), the trick tables from the executable.

`zx-panelshot --data <dir> --ram <dump> --out <png> [--touch "ram <dump>; tap X Y; ..."]` draws a real moment on
the Mac from `nctl.py NAME "dump FILE"` dumps. BMX XXX's: `--game bmx`. DM2's: `--game dm2`, and
`--dm2-xbox-levels VENICE,GREENVIL` for its Xbox levels' values (their files from `build --game dm2 --dm2-xbox
<dm2_xbox.py's folder>`). The shots come from the game: keep them in scratch.

## Disc images and archives

- `zx-disc list | extract | to-iso IMAGE ...` (`tools/setup/disc_tool.cpp`): a disc image's files, whatever its
  format (ISO, BIN/CUE, CHD, CSO, ZSO); `tools/setup/recompile.sh` takes each game's ELF from the disc with it.
- `zal list | unpack | repack | verify` (`tools/modding/zal_tool.cpp`): a game's `.ZAL` archive as a folder of its
  entries and `zal.txt`, and back, byte for byte: docs/MODS.md "Archives".

Both are targets of the build, and build alone in seconds: `cmake -S . -B out/zal -DZX_BUILD_RUNTIME=OFF
-DZX_BUILD_TEST=OFF -DZX_BUILD_RECOMPILER=OFF`, then `cmake --build out/zal --target zal` (or `zx-disc`).

## Where the tools run

The shared tools stop with a message (exit 2) when started from the main checkout or told to write into it
(`tools/zguard.py`: `career_ab.py`, `run-game.sh`, the playbot, `replay.py` and the maintainers' check
scripts): run them from your worktree or scratch with absolute paths. The maintainers' merge and batch scripts,
which work there by design, export `ZAXIS_IN_MAIN=1`.

A contributor runs the unit tests and whatever else the change needs on their own machine (CONTRIBUTING.md); the
maintainers run the full checks with scripts that are not part of this repository. Linux and macOS agree on every
logiccheck row, state field, the scratchpad, VIF1 and VU0; only movie pixels differ (FFmpeg versions), so compare a
run with a run on the same host.
