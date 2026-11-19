# Settings: the pages in each game's own menus

The collection adds one Settings screen to each game, drawn in the game's own menus, fonts and highlight, the
same page body in all three games (`runtime/src/game_overrides/zge_settings.cpp`). This file says what each row
does, which games have it and where it is kept. Keys, mouse and gamepads are CONTROLS.md; mods are docs/MODS.md.

## Where it is

- Aggressive Inline: Options > Settings (the Controller icon), and Settings in every pause menu (Career, Free
  Skate, Timed Run, the Tutorial's lesson list, split screen).
- Dave Mirra BMX 2 and BMX XXX: the SETTINGS line in Options and in the pause menu.
- macOS: Settings... (Cmd+,) in the app menu opens the page where the game would: in a level it pauses as Start
  does and the pause menu opens on Settings; outside a level DM2 and BMX XXX say "Settings: in the game's
  Options", AI's front end goes to Options > Settings. Windows and Android have no menu bar: use the game's menus.

L1 / R1 (Q / E) change the section, up and down pick a row, left and right change it, Cross changes a switch or
starts a rebind, Triangle goes back. A row applies at once unless its help line says "next start". A row shows
dimmed while another setting makes it idle (the GPU renderer's rows under the CPU renderer, Wide Aspect with
Widescreen off); it can still be changed. A row that could do nothing in your setup is not shown at all: one that
needs an extras disc's files you have not added, the readable code, or what the platform lacks (a second window,
window modes on Android) (`runtime/include/collection/offered.h`). The collection menu has its own Settings page
for what spans the games: the games' discs (Settings > Games), Mods, Custom Freeplay, the menu's intro movie and
music.

## The sections

**Display**: Display Mode (window, full screen, and on macOS borderless; Android, always full screen, has no
Display Mode row), Scaling (smooth, integer), VSync, Variable Refresh, Frame Interpolation (pictures between the
game's frames on a display faster than 60 Hz, GPU renderer, off by default), Widescreen (drawn only: the game plays
as at 4:3, so what only the sides show stays still, as just off a 4:3 screen), Wide Aspect (16:9, 16:10, 21:9, 32:9,
the window's), Show Frame Rate, Pause in Background, Map Window (the bottom screen in a second window;
not on Android), Dump Textures (writes every texture drawn to `<support>/Texture Dumps`, for texture packs), Depth
Test (exact, fast: fast is Android's default; next start).

**Graphics**: Renderer (GPU, CPU), Resolution (Native, 2x, 3x, 4x: 3x on Mac and Windows, 2x on Android), Full
Height, the picture quality rows (below: Anti-Aliasing, Texture Filtering, Fog, Shadows, Depth Precision,
Perspective Correction, Character Lighting), Native Meshes (below), Motion Blur (off, low, original), Draw
Distance (original, far, farthest), and, with the game's Xbox disc set up, Xbox Textures (all three games), and in
Aggressive Inline Xbox Skaters (from the next level) and Xbox Movies (below).

Draw Distance is each game's own state, so it is Original by default and applies at the next start, and its row
says it changes the game: Far and Farthest move the far clip (AI: split screen's 100 to 250 or 1000, and with
Farthest every level's 1000 to 2500 and every camera's far clip the game does not set again, 200 to 500; DM2 and
BMX XXX: every level's, single player's too), the fog by the same ratio, and the models' level-of-detail distances
with it.

**Sound**: Master Volume, Music Volume, Effects Volume, Speech Volume (AI and BMX XXX: DM2 has none), Movie Volume,
French Voices (AI with the Xbox files, below), Mute in Background. The game volumes are the game's own Options >
Sound values, read from and set through the game, so both pages show the same.

**Controls**: Button Prompts (auto, keyboard, Xbox, PlayStation, Nintendo), Gamepad (on, off), Stick Dead Zone,
Connected Gamepads (the pads in player order; Cross writes their details to the log), Test Rumble, Swap Pads 1 and 2,
Mouse Look (Free Look, Right Stick), Mouse Sensitivity, Invert Mouse Y, then KEYS (a row per action and per PC
shortcut: Restart, Hide HUD, Screenshot, Photo Mode), BUTTONS (a row per game button and its gamepad button) and
Reset Controls. Cross on a key row takes the next key or mouse button (Esc cancels, Backspace or Delete unbinds; a
gamepad's Circle or Start cancels): it becomes the action's first binding and is taken off any other action. Cross
on a button row takes the next gamepad button and swaps it with the action that had it. Every change is written to
`controls.ini` at once (CONTROLS.md).

**Gameplay** (kept in the game's `gameplay.ini`, live):

- CAMERA: Field of View, Camera Distance and Camera Height (a share of the game's own), and in AI Camera Style
  (PS2, Xbox: the Xbox release's field of view, single player only). They change the picture, not play: the game
  decides by its own view (docs/ENHANCEMENTS.md). Not yet for BMX XXX's Camera Distance and Height, which still
  change play.
- ASSISTS: AI's Out of Juice (an empty juice meter ends the run or not), Juice Drain and Juice Lost on Crash; DM2's
  and BMX XXX's Grind Balance Loss and Manual Balance Loss (DM2's file also reads `assist.wheelie_balance_loss`).
- CHEATS: each game's own cheats as switches that leave the saved game alone, and Unlock Everything (two steps,
  "Sure?" then "Unlocked"; the one row that changes the save). AI: Perfect Grind, Perfect Manual, Perfect
  Handplant, Super Spin, Low Gravity Wall Ride, Juice Regeneration, Invulnerability. DM2 and BMX XXX: Perfect Grind
  and Perfect Manual. What each writes is docs/CHEATS.md.

**Game**: Language, Skip Intro Movies, Gameplay Tuning, Career Scripts, Developer Menu (the game's hidden debug
menu or display on F1, next start: docs/DEVTOOLS.md), Record Play (each boot recorded to the game's Recordings
folder, next start: docs/DEVTOOLS.md) and Recordings (where they are and how many), then EXTRAS, the content beyond
the PS2 disc and the cut content each game has (below).

## Which game has what

| Row | AI | DM2 | BMX XXX |
|---|---|---|---|
| Widescreen, Wide Aspect, Draw Distance, Movie Volume, Skip Intro Movies, the Gameplay pages, save profiles, PCSX2 save import, Developer Menu | yes | yes | yes |
| Motion Blur | yes | no (the game has no camera blur) | yes |
| Speech Volume | yes | no | yes |
| Xbox Textures | with the Xbox files | with the Xbox files | with the Xbox files |
| Xbox Skaters, Xbox Movies | with the Xbox files | no | no |
| Language | with the Xbox files | no | with the other releases' files |
| French Voices | with the Xbox files | no | no |
| Gameplay Tuning | yes | no | with the other releases' files |
| Career Scripts, Zoo Level, Zoo Unlock, Estate Level (with the GameCube files), Ape Skater, Xbox Free Skate, Camera Style | yes | no | no |
| Xbox Levels, Create Rider, Desert and Highway Themes, Retro Movie, Attract Demo | no | yes (three of them with the Xbox files) | no |
| Park Editor, Xbox Challenges, Pool and Trash Can Pieces, Level Previews, Z-Axis Logo Movie, Intro Movie, GameCube Levels, Unused Menu Lines, All Multiplayer Modes | no | no | yes (five with the other releases' files) |

"With the Xbox files" and the like: the row shows only once your own disc of that release is set up with the game
(Settings > Games: Find Discs finds it beside the PS2 disc, or the game's Add Xbox Disc and Add GameCube Disc), which
makes its files once; the game never reads that disc. An app built with `make-app.sh` and the discs (README
"Building") can carry them instead.

## Where the settings are kept

In the collection's support folder (README "Your discs"):

- `settings.ini` at its root: what every game shares: `[display]` (window, full screen, scaling, vsync, aspect,
  the frame rate, the map window, Xbox textures, skaters and movies), `[graphics]` (renderer, resolution, full
  height, motion blur, the picture quality rows, depth test, render path), `[audio]` (master and movie volume,
  mute in background), `[general]` (pause in background, skip intro movies, log file, record play, Custom Freeplay),
  `[host] disc`, `[menu]` (the collection menu's intro movie and music) and `[mod order]`.
- `games/<id>/settings.ini`: the game's own options: `[general] language`, `french_voices`, `developer_menu`, `[gameplay]` (draw
  distance, tuning, career scripts), `[extras]` (each extra's switch), `[saves] profile` (the career's save file),
  `[mods]` (a mod switched off for this game).
- `games/<id>/gameplay.ini` (the Gameplay rows), `games/<id>/controls.ini` (CONTROLS.md), `games/<id>/Saves`.

Each file's comments say what each key takes. The developer runner reads the same names from the environment
(`ps2x::boot::text`; docs/DEVTOOLS.md "Test runs").

## Shortcuts and the pause menus

- PC shortcuts (`controls.ini` `[hotkeys]`, the same in all three games): Restart (Backspace; gamepad L3+R3), Hide
  HUD (F9), Screenshot (F12), Photo Mode (P), Switch Game (F8: back to the collection menu; no row, change it in
  `controls.ini`). Each has a `<name>_pad` chord.
- Restart does what fits the mode: AI the challenge you were given again (Career; else the level), the lesson
  (Tutorial), the level (Free Skate) or the run (Timed Run, split screen); DM2 the turn again (ProQuest, Session);
  BMX XXX the turn from its start. The pause menus get the same as an item (AI: Restart Level, Restart Run, Retry
  Challenge, Restart Lesson). The key acts only in play.
- Hide HUD hides the score, meters and messages until pressed again or the front end comes back.
- Screenshot saves the picture as a PNG in `Pictures/Z-Axis Collection/<game>` and says "Screenshot saved" top
  left (not in the picture).
- Exit to Collection: in each game's main menu and pause menu (AI's Quit to Desktop item becomes it in the
  collection), back to the collection menu without restarting the app. AI asks "Are you sure?" first before Level
  Select, Exit Game and that item, as the Xbox release does.

## Photo mode

Photo Mode in the pause menu, or the `photo_mode` key anywhere in a level (from play it pauses first and resumes
after): the game stays paused, the HUD is hidden and a free camera takes over, within 40 units of where it started.

- Keyboard and mouse: mouse look, W A S D move, Space / Ctrl up and down, Q / E roll, wheel or Z / X zoom (field of
  view 15 to 110), Shift faster, Alt slower, - / = speed, F filter, R reset the camera, H hide the help, the
  Screenshot key saves, Esc or the photo key goes back.
- Gamepad: left stick move, right stick look, R2 / L2 up and down, L1 / R1 roll, d-pad up and down zoom, left and
  right speed, R3 faster, Triangle filter, Select reset, Cross saves, Square help, Circle or Start back.
- Filters: None, Letterbox (2.39:1), Vignette, Black & White, Cinema (letterbox and vignette), Noir (all three);
  screenshots get the filtered picture at full resolution.

How the settings, prompts, pause items and photo mode are put into Aggressive Inline's menus is in the overrides'
own comments (`aggressive_inline_settings.cpp`, `aggressive_inline_prompts.cpp`, `aggressive_inline_gameplay.cpp`,
`aggressive_inline_restart.cpp`, `aggressive_inline_photo.cpp`); per-function facts are in the work database.

## Aggressive Inline's extras

### The Zoo (Game > Extras > Zoo Level)

`game.zoo` (settings.ini `[extras] zoo`, default on; `PS2X_ZOO=1` for the
runner, where it is off by default) adds the level cut from the PS2
release to the free skate and timed run level select
(`game_overrides/aggressive_inline_zoo.cpp`): a UILIB.ZAL patch on
FrontEnd.zsc's `FreeskateOnChange` (the giraffe icon the level-select font
already has, between the Museum and the Editor Level, always unlocked, nine
icons), files of ASSETS/LEVELS/ZOO from a provider (runtime/ps2_game_files.h:
the Xbox release's level made into the PS2's at build time when the app was
built with an Xbox image, else the PS2 disc's prototype), and extended
memory. Like
the developer menu it applies at the next start (the UI library is read
once).

With the Xbox files the Zoo also has its ambient NPCs (kelp, sharks, the
willows' swaying foliage): the Xbox executable's six extra NPC types,
SCENE.TXT's models for the ZOO and MPZOO scenes, and the models from
`assets/npc/zoo` converted (`runtime/ps2_zaxis_xbox.h` convertModel), the
Xbox pictures drawn by the GPU renderer.

**Made at build time.** Nothing of the Zoo is converted while the game runs.
`ps2xXboxConvert` (runtime/tools/xbox_convert.cpp, `runtime/ps2_zaxis_zoo.h`; run by
tooling/ai/tools/make-app.sh and make-windows.ps1 from the extracted
Xbox disc and the PS2 disc) writes the level's files, split screen's
ZOO.TXR / ZOO.GEO / ZOO.DAT / ZOO.BIN, the NPC models and the Xbox originals
of their textures (`textures/<path>.xtp`) into Resources/xbox-converted
(Windows: xbox-converted\ next to disc\); the app sets
`PS2X_XBOX_CONVERTED` to it and the override only reads those files
(`aggressive_inline_converted.h`). Developer runs: `ps2xXboxConvert --xbox
<extracted Xbox disc> --ps2 <extracted PS2 disc> --out <dir>`, then
`PS2X_XBOX_CONVERTED=<dir>`; without it the runner's Zoo is the PS2 disc's
prototype. The conversion's developer switches are the tool's options now:
`--split-screen-texture-side N` (was `PS2X_ZOO_MP_TEXTURE_SIDE`),
`--group-entries N` (`PS2X_ZOO_GROUP`), `--hide a,b` (`PS2X_ZOO_HIDE`),
`--split-screen-eggs "x,y,z;..."` (`PS2X_ZOO_MP_EGGS`),
`--keep-willow-twins` (`PS2X_ZOO_MP_TWINS`). The split-screen level select gets
the Zoo as a fifth entry: MP\ZOO is the single-player level with smaller
textures (64 a side), a player two start, the free skate script without its
intro camera (doors open, willows sway), the split-screen games' props
(Egg Hunt's eggs, Twenty One's cards; from the PS2 disc's MP\MOVIE) and egg
spots below the respawn points, each willow once, its NPCs animated, and the
Movie Lot's split-screen FOV.
Developer variables: `PS2X_ZOO_NPCS=0` (no ambient NPCs),
`PS2X_ZOO_MP_SCRIPT=empty|free` (split screen with the empty script, or the
free skate one as it is: no timer), `PS2X_ZOO_MP_NPC_ANIMATION=0` (split
screen's NPCs animate only as the game decides: hardly ever),
`PS2X_TRACE_ZOO=1` (also the NPC type lookups), `PS2X_TRACE_ZOO_NPCS=1`
(each Xbox NPC type lookup, counted by type and caller: the NPCs a level
makes).

### Estate (Game > Extras > Estate Level)

`game.estate` (settings.ini `[extras] estate`, default off; shown only with
the GameCube disc set up; `PS2X_ESTATE=1` and `PS2X_ESTATE_DIR=<dir>`
for the runner) adds TUT/Estate, an earlier export of the Tutorial site left
on the GameCube disc, to the free skate level select as "Estate"
(`game_overrides/aggressive_inline_estate.cpp`): a UILIB.ZAL patch on
FrontEnd.zsc's `FreeskateOnChange` after the Zoo's (its
own icon, before the Editor Level, always unlocked, the Editor Level's still
picture), the Tutorial's level index 7 for its SelectedLevel "Estate" (the
name lookup func_1A8258 wrapped), and its files for ASSETS/LEVELS/TUT from a
provider while it is the level picked and the game is not a career (the
career's Tutorial is always the retail one). Estate's own scripts run (its
25-lesson tutorial); its givers answer Circle (the NPC constructor's free
skate hiding, func_13BD48 at 0x1E36AC, skipped in Estate as in the Zoo); its
pause menu is the Tutorial's lesson list, whose 23 lesson items (the same
topics) start Estate's lessons 1-8, 10-20 and 22-25, Restart Lesson the
current one, the checkboxes Estate's lessons done since the level started;
lesson 9, which has no giver, rail or respawn point in the level, hands over
to lesson 10 when it starts; it keeps its own records beside the career save
(`<saves>/<profile>.estaterecords`; every native save gets the Tutorial's own
row, `PS2X_ESTATE_RECORDS=0` shares the Tutorial's row as before); the Xbox
texture pack is kept off its textures. Applies at the next start.

**Made at build time.** `ps2xGcConvert` (runtime/tools/gc_convert.cpp,
`runtime/ps2_zaxis_estate.h`; run by tooling/ai/tools/make-app.sh and
make-windows.ps1 on what tooling/ai/tools/gc_disc.py extracted from the GameCube image)
writes TUT.ZIT, TUT.GEO, TUT.INS, TUT.TXR (its one 24-bit texture made
8-bit), TUT.DAT, TUT.BIN, TUTT.BIN, MOTIONS.ZAL, TEXTURES.TXT (the
textures' keys), VISIBILITY.TXT and ICONS.UFNT (its level-select icon, 'n' /
'N', from a picture of the level drawn from its own files; `--picture
<file.bmp>` writes the picture too) into Resources/estate (Windows: estate\ next to disc\); the
app sets `PS2X_ESTATE_DIR` to it. Developer runs: `python3 -I tooling/ai/tools/gc_disc.py
extract <gc image> --out <gc> assets/levels/TUT/Estate`, `ps2xGcConvert --gc
<gc> --out <dir>`, then `PS2X_ESTATE=1 PS2X_ESTATE_DIR=<dir>`;
`PS2X_DEV_START_LEVEL=ESTATE` starts in it. Developer variables:
`PS2X_TRACE_ESTATE=1` (each file served, a line every 10 s in the level),
`PS2X_ESTATE_PROBE=<points>:<out>` (the game's collision under each point
of ai_level.py ground's grid; `PS2X_ESTATE_PROBE_ANY=1` in any level, for a
control), `PS2X_ESTATE_GRINDS=<edges>:<out>` (the game's rail search at
each of ai_level.py edges' grind edges), `PS2X_ESTATE_LESSONS=<out>` (the
lesson list's 23 lessons in turn, each started by its jump, player one put
by its giver and Circle pressed through the pad; a line per lesson and a
summary), `PS2X_ESTATE_ACTION=1` (the Action move's target search traced:
the nearest talker asked, the ACTION messages sent; at frame 150 the nearest
talker at each giver).

### The Ape, Xbox tuning, Xbox scripts, Zoo unlock, camera (Game, Gameplay)

These rows take what the Xbox release has beyond the PS2's. The Ape, Xbox Free Skate and Career Scripts rows
show only with the Xbox files each reads. The Game rows apply at the next start; the runner has them off
unless its variable is set.

- `game.ape` "Ape Skater" (settings.ini `[extras] ape`, default on;
  `PS2X_APE=1`; needs GOR.ZIT made at build time in Resources/xbox-converted
  and the Xbox file `uilib.zal`): the Xbox's secret skater, last in the skater select
  (`game_overrides/aggressive_inline_ape.cpp`). The Ape takes skater slot
  0, the "None" row every PS2 skater table has (the Xbox's index 20 would
  run past the PS2 tables): code AP, the Xbox's stats (the Tinman's), the
  default name APE, the cha_ape picture from the Xbox uilib.zal, the model
  made from the Xbox GOR.ZIT at build time (`ps2xXboxConvert`,
  `runtime/ps2_zaxis_ape.h`: 256 a side; the GPU renderer draws the 512
  originals; nothing converted while the game runs). Locked until the Mummy is unlocked (the Museum's hidden
  challenges, the all-skaters code, Unlock Everything), which is when the
  Xbox unlocks it. Its career is skater record 0, which the game never
  saves: the native saves write it after the game's data in the same
  file ("XAPE" block; the file stays a PS2 save, the game reads only its
  own part). The converted model is the file
  xbox-converted/ASSETS/SKATERS/GORILLA/GOR.ZIT (`PS2X_APE_DUMP` is gone).
- `game.tuning` "Gameplay Tuning" PS2 / Xbox (settings.ini `[gameplay]
  tuning = ps2|xbox`, default ps2; `PS2X_TUNING=xbox`;
  `game_overrides/aggressive_inline_tuning.cpp`): the Xbox balance. Its
  tuning table is the PS2's value for value; what differs is experience
  (special tricks +300,000, the eight key items nothing instead of
  +500,000) and Civic Center's 350,000 point trick (exactly 350,000
  passes). With ps2 nothing is hooked (logic checks against PCSX2).
- `game.xbox_free_skate` "Xbox Free Skate" (settings.ini `[extras]
  xbox_free_skate`, default on; `PS2X_XBOX_FREESKATE=1`;
  `game_overrides/aggressive_inline_freeskate.cpp`): the Boardwalk's and
  the Cannery's free skate scripts from the Xbox files (`levels/brdwlk/
  brdwlkt.bin`, `levels/cannery/canneryt.bin`): their doors open as in the
  career, coaster cars, fan, boulder loop, the Cannery's cutter, idle hints.
- `game.career_scripts` "Career Scripts" PS2 / Xbox (settings.ini
  `[gameplay] career_scripts = xbox|ps2`, default xbox;
  `PS2X_CAREER_SCRIPTS=xbox|ps2`; `aggressive_inline_careerscripts.cpp`;
  the Xbox files `levels/<level>/<level>.bin`, `levels/civic/motions.zal`):
  the career's level scripts from the Xbox release, the PS2's with the
  Xbox's bug fixes (challenges that could stick or be offered again,
  wrong previews, texts that say what the code checks). The Tutorial stays
  the PS2's. Two PS2 texts the Xbox scripts dropped are put back (in
  memory, only into the exact US Xbox files, checked by size and hash):
  the Movie Lot's first-start hint "Use the select button to view the
  current challenges." and the Hangar's "Turret challenge complete!"
  (shown as the Xbox completes the challenge). A skater's saved progress
  in a level holds that script's variables, whose layout differs in four
  levels, so a level the skater has played keeps the script it was played
  with; the setting is for the levels not played yet.
- `game.zoo_unlock` "Zoo Unlock" Museum / Always (settings.ini `[extras]
  zoo_unlock = museum|always`, default always; `PS2X_ZOO_UNLOCK`;
  `aggressive_inline_zoo.cpp`): Museum is the Xbox's rule, the Museum's
  eighth challenge (challenge points) unlocks the Zoo for that skater
  ("New level unlocked!"), the free skate select showing the game's own
  lock until then; the all-levels code and Unlock Everything open it too.
  Always (the default): open whatever the career, as before.
- `camera.style` "Camera Style" PS2 / Xbox (Settings > Gameplay, AI only; gameplay.ini
  `[camera] style = 0|1`, default 0, live): the Xbox release's field of
  view. Its follow camera is the PS2's (same code, tuning, collision probe
  and smoothing); its projection takes the vertical FOV as FOV x 1.1 /
  aspect, so it keeps about the same width and crops the top and bottom in
  widescreen, where the port's widescreen widens the sides: at 16:9 the
  skater 1.34x as tall as with PS2 (the FOV given to the game 59.5 for 75),
  at 4:3 0.96x (77.3). Single player only (the Xbox fixes 50 in split
  screen; the PS2's per-level FOV stays). Field of View applies on top.
  Both are drawn only: the game plays with the PS2's field of view.

### Languages from the Xbox release

(`game_overrides/aggressive_inline_language.cpp`; runtime
`ps2_zaxis_text.h`, `ps2_xbox_audio.h`.) The US PS2 disc is English only;
the Xbox release has French, German and Spanish. Its files come from the
player's Xbox disc, copied once when it is set up (Settings > Games), or at
build time: tooling/ai/tools/make-app.sh (`XBOX_ISO`) and make-windows.ps1
(`-XboxIso`) extract the image and bundle what tooling/ai/tools/xbox-files.txt
lists into Resources/xbox (`PS2X_XBOX_DIR`, read through `ps2_xbox_disc`); the
game never reads an image.

**Rows** (only when the bundle has them): Game > Language (English,
Français, Deutsch, Español; next start; settings.ini `language =
english|french|german|spanish`) and Sound > French Voices (live;
`french_voices = auto|true|false`, auto = on with French). Changing the
language puts French Voices back to auto.

The text is translated where it is drawn, from the Xbox string tables and level scripts paired with the PS2's;
the PS2 fonts are swapped for the Xbox ones, which have the accented letters; texts the Xbox narrows are narrowed
the same way; French Voices plays the Xbox's French dub of the speech; with an Xbox-style pad the Tutorial's loading
screens are the Xbox's pictures of its controller (`aggressive_inline_language.cpp`). `PS2X_TRACE_LANGUAGE=1` lists
texts drawn with no translation, `=2` every text with its translation.

**English** changes nothing: no file is replaced, the table is not made,
and the text hooks pass the text through as before.

## Dave Mirra BMX 2's and BMX XXX's extras

Each is off by default (DM2's Xbox Levels on) and applies at the next start; what each does, and why it belongs to
that game, is docs/ENHANCEMENTS.md "Per game". In `games/<id>/settings.ini` `[extras]`:

| Row | Key | Game | What |
|---|---|---|---|
| Xbox Levels | `dm2_xbox_levels` | DM2 | Venice and Greenville, the Xbox release's levels, in Free Ride and ProQuest (Xbox files) |
| Create Rider | `create_player` | DM2 | the rider creator its code has and no release shows: CREATE RIDER on the main menu |
| Desert and Highway Themes | `xbox_themes` | DM2 | the park editor's two Xbox themes (Xbox files; a park saved on one does not load with it off) |
| Retro Movie | `retro_movie` | DM2 | the movie gallery's sixteenth line (Xbox files) |
| Attract Demo | `attract_demo` | DM2 | the main menu's idle demo, with the Xbox's three demo replays |
| Park Editor | `park_editor` | BMX XXX | the park editor no retail menu reaches, as the main menu's third line; SESSION rides a saved park |
| Xbox Challenges | `xbox_challenges` | BMX XXX | the Xbox and GameCube challenge scripts (their spoken lines, Sheep's tethers, two fixed steps) |
| Pool and Trash Can Pieces | `editor_pieces` | BMX XXX | the GameCube editor's two extra pieces |
| Level Previews | `level_previews` | BMX XXX | the Xbox's level-select preview movies |
| Z-Axis Logo Movie | `logo_movie` | BMX XXX | the movie gallery's twentieth line |
| Intro Movie | `intro_movie` | BMX XXX | the intro movie at boot, as the GameCube plays it |
| GameCube Levels | `gc_levels` | BMX XXX | the Secret Level and three test levels the GameCube disc keeps |
| Unused Menu Lines | `menu_lines` | BMX XXX | the pause and game over lines no release's menus put up |
| All Multiplayer Modes | `all_mp_modes` | BMX XXX | the multiplayer modes SELECT GAME leaves out |

BMX XXX's Gameplay Tuning (`[gameplay] tuning = xbox`) gives Syracuse's two timed challenges the Xbox and
GameCube's 60 s; its Language row picks the game's own French, German or Spanish, filled in from the other
releases' translations. Both need the other releases' files (their discs set up, Settings > Games).

## Xbox textures, skaters and movies

With a game's own Xbox disc set up (Settings > Games; for Aggressive Inline also at build time, `XBOX_ISO` or
`XBOX_DIR` for tooling/ai/tools/make-app.sh, `-XboxIso` or `-XboxDir` for make-windows.ps1), its Xbox textures are
made once into the game's `xbox-pack` folder, and the game never reads the disc. In Aggressive Inline the Xbox
release's textures are what every level shows
on the GPU renderer: wherever the Xbox has a counterpart of a PS2 texture
(the same texture, as large or larger, and alike), its picture is drawn;
the PS2 picture only where there is none. The Zoo draws the Xbox originals
of its converted textures (up to 512, DXT3), not the 128-texel copies the
PS2 renderer's texture cache gets. The Graphics page has three more rows, all
on by default (Xbox Skaters from the next level, the others at once); off is the PS2 release's look (and what
the CPU renderer, logic checks and PCSX2 comparisons always use):

| Row | settings.ini | What it does |
|---|---|---|
| Xbox Textures | `[display] xbox_textures = true` | The GPU renderer draws the Xbox release's larger, redrawn textures for levels, skaters, park editor pieces and the multiplayer levels in place of the PS2 ones (the game's memory and logic are unchanged; the CPU renderer shows the PS2 ones) |
| Xbox Skaters | `[display] xbox_skaters = true` | The skaters' and people's models from the Xbox release (`PS2X_XBOX_MODELS`) |
| Xbox Movies | `[display] xbox_movies = true` | The Xbox release's 640x480 Bink movies (cutscenes, intro, logos, flythroughs re-recorded with the Xbox textures) play in place of the PS2 ones; read at the start of each movie |

Dave Mirra BMX 2 and BMX XXX have the Xbox Textures row alone (on by default; no skaters or movies): the Xbox disc's
level textures are mostly twice the PS2's size a side, with real detail (riders, menus and the editor are the PS2's
size on the Xbox too, so they stay the PS2's). BMX XXX's level props' textures come from its levels' `models.zal`
archives; a prop whose Xbox name does not pair with the PS2's keeps the PS2's texture.

Environment (developer runner, tests): `PS2X_XBOX_PACK=<pack folder>`,
`PS2X_XBOX_TEXTURES=0/1`, `PS2X_XBOX_MOVIES=0/1`; `set
display.xbox_textures 0` on PS2X_CONTROL. Which texture a draw shows is
told by the game's texture cache (its texture objects), not guessed from GS
memory (`runtime/src/lib/gs/GPU_BACKEND.md`, "Replacement pictures").

## Texture packs

A texture pack is a mod (the Mods page) with a `Textures` folder of PNG
pictures that are drawn in place of the game's own textures, at their
own size: a larger picture is drawn at its resolution on the GPU
renderer (with its mip levels, trilinear and the Graphics page's
anisotropic filtering). The game itself never sees a pack (its memory and
logic are the same with one or without); the CPU renderer draws the
pictures at the PS2's resolution (each box-filtered to its texture's
size), so edits show there too, extra detail does not.

- Layout: `<mod>/Textures/<game>/**/*.png`, `<game>` the collection's id
  (`ai`, `dm2`, `bmx`) or the disc's serial (`SLUS-20327` and so on); a
  mod for one game alone can put them in `Textures` itself. Later mods win
  on the same picture; the Xbox textures sit under every pack.
- Names: PCSX2's, so a pack made for PCSX2 (its `textures/<serial>/replacements`
  folder) and its dump tooling work unchanged: `<texture hash>-<palette
  hash>-<bits>.png`, `<texture hash>-<bits>.png` without a palette,
  `-r<w>x<h>` for a region of a texture, `-mip<n>` for a picture's own
  mip level (optional: made from the picture otherwise). PNG only (DDS
  not yet). A picture's alpha is the PS2's: 128 (0x80) is opaque, as in
  PCSX2's dumps. `.xtp` files there load as the Xbox pack's do (keyed by
  the texture's indices and name: Aggressive Inline's).
- Making one: dump mode (`PS2X_TEXTURE_DUMP=<folder>`, docs/DEVTOOLS.md)
  writes every texture the game draws under that name; edit or upscale
  the files and put them in a pack.
- Memory: the pictures are read on loader threads when the game starts
  (all of them while they fit 1 GB on Mac and Windows, 256 MB on Android;
  the rest when first drawn, the PS2 texture until then).

| Environment | What it does |
|---|---|
| `PS2X_TEXTURE_PACKS=<folder>[:<folder>...]` (`;` on Windows) | the packs' folders, lowest first (the app sets it from the game's mods) |
| `PS2X_TEXTURE_DUMP=<folder>` | dump mode |
| `PS2X_TEXTURE_PRELOAD=0` | pictures read when first drawn only |
| `PS2X_TEXTURE_PACK_MB=<n>` | the pictures' memory budget |

## Native meshes (render path)

On the GPU renderer the game's level meshes are drawn by the GPU from
vertex buffers made once from the game's own mesh data, not by its VU1
microprograms vertex by vertex: the rigid prelit meshes (render type 4),
the terrain with its grass shells (type 0x20), the sky (0x18) and the
skaters and other skinned characters (8 and 0xC: bones and lights in a
vertex shader) in every level, split screen included. The game itself runs
exactly as before (its memory and timing do not change); the meshes look as
they do through VU1, a little smoother where they cross the near plane.
Shadows, particles and 2D still go the PS2 way.

| settings.ini | Environment | What it does |
|---|---|---|
| `[graphics] render_path = native` (default) | `PS2X_RENDER_PATH=auto` | native meshes in each game's form: Aggressive Inline's packets substituted on the GPU renderer (strict on the CPU one); DM2's and BMX XXX's from the draw records, with either renderer (as `records` below; DM2's riders and bikes too) |
| `render_path = strict` | `PS2X_RENDER_PATH=strict` | every mesh through VIF1, VU1 and the GS as on the console (the logic checks' and PCSX2 comparisons' mode) |
| | `PS2X_RENDER_PATH=native` | the packet substitution, also with the CPU renderer (the same triangles sent to it as GS primitives); DM2's without the records (developers), BMX XXX's the records |
| `render_path = records` | `PS2X_RENDER_PATH=records` | native meshes made by the engine's class renderers from their draw records (zge/host/class_draws.cpp): no VU1 work in the mesh packet (its texture requests stay), with either renderer, in all three games (DM2's and BMX XXX's default); a draw it does not make goes through VU1 (`PS2X_TRACE_NATIVE=1` counts why) |

The game's files are read straight from the host (the game's mods over its
own, "Mods" below) by the engine's disc device's host form; the PS2's CD/DVD
client over the emulated IOP is still there:

| settings.ini | Environment | What it does |
|---|---|---|
| `[host] disc = native` (default) | `ZAXIS_DISC=native` | the game's files from the host, ZCDDVD.IRX not loaded |
| `disc = ps2` | `ZAXIS_DISC=ps2` (or `PS2X_DISC=cdvd`) | the game's CD/DVD client over the emulated IOP and ZCDDVD, as on the console |

The Graphics page's Native Meshes row switches between `native` (on) and `strict` (off); `records` is set in
`settings.ini` only. In the developer runner `set display.native_meshes 0/1` on PS2X_CONTROL switches while running. Debugging:
`PS2X_NATIVE_CLASSES=4,0x20,0x18,8` (render types drawn natively; 8 or 0xC
for the skinned ones), `PS2X_NATIVE_LEVELS=zoo`,
`PS2X_NATIVE_TINT=1`, `PS2X_NATIVE_GPU=0`, `PS2X_TRACE_NATIVE=1`; design
in docs/NATIVE.md and `runtime/src/lib/gs/GPU_BACKEND.md` ("Native draws").

## Mods

The players' guide is docs/MODS.md. Mods are folders or zips (read in place, never unpacked: `runtime/ps2_zip_archive.h`,
miniz 3.0.2) in the collection's Mods folder, `<support>/Mods`, each with a `mod.ini` (`[mod]` id, name, version,
author, games, needs, conflicts); what a mod holds is told by its layout:

| In a mod's folder | What it replaces |
|---|---|
| `ASSETS/...`, `MOVIES/...`, any disc path (names compared without case) | that file of the game's disc, through the engine's file system (`runtime/ps2_game_files.h`; the game's own in-memory patches, such as the Settings menus' scripts, still apply on top) |
| `Music/NN.ogg` (`.wav`, `.flac`, `.mp3`) | song NN of the soundtrack (`zge_sound.cpp`) |
| `tuning.ini` | Aggressive Inline's tuning overrides (`docs/DEVTOOLS.md`); not read by the other two games |
| `Textures/<game>/` (or the disc serial, or `Textures/` for a mod of one game) | the GPU renderer's replacement pictures (`PS2X_TEXTURE_PACKS`) |
| `[map]` in `mod.ini`, or `map.ini` | a custom map (Custom Freeplay, below), read by no other boot |

The loader (`runtime/include/collection/mods.h`, `collection/mods.cpp`) gives each game, as `PS2X_MODS_DIR` (the
platform's path-list separator between them, `;` on Windows and `:` elsewhere; the developer runner takes one folder
there as before), the game's own Mods folder (`<support>/games/<id>/Mods`; the single-game app's `<support>/Mods`),
then the mods it reads in the order, a later mod's file over an earlier one's: the mods of the game's own Mods folder
(the layout before manifests: each folder or zip there is a mod of that game, by its name; files loose there are read
under every mod; a folder named like one of the game's own top-level disc folders, or Music, is that loose layout's;
a folder with a `map.ini` is no mod), then the collection's that name the game. The mods' texture folders for the game
go as `PS2X_TEXTURE_PACKS` (lowest first, unset with none). With no mods the boot's values are as before. The boot's
log lists each mod (`[mods] ai: Mods/hd.zip 1.2 read`, or why not) and, where two have the same file, song, tuning.ini
or texture, which one's the game reads (`[mods] ai: Mods/b over Mods/a: 3 things (ASSETS/X.ZAL, ...)`); `[disc] files
from ..., mods from ...` lists the folders.

The collection's menu has a Mods page (Settings > Mods): every mod, numbered in the order, each with its state; a
mod's page has its switch in each game it is for, Move Up and Move Down, what it wins and loses, and why a game
does not read it. It lists the folders again each time it opens. A switch is kept in the game's `settings.ini` and
applies at the game's next start; the order in the collection's root `settings.ini`:

| settings.ini | What it does |
|---|---|
| `[mods] <key> = off` (the game's file) | the mod is not read by this game; the key is the manifest's id, or for a mod without one its folder's name as it is (the case kept; a name with `=` in it ends at the last one); `= on`, or no line, is on |
| `[mod order] <n> = <place>` (the root file) | the order, first read first, each mod by where it is (`Mods/hd.zip`, `games/ai/Mods/10-park`); mods not named come after, each game's own folder's first, then the collection's, by name. None: that order alone |

A mod is not read by a game while it is switched off there, while a mod it needs is missing or not read, while a mod
it conflicts with is read, or when its id is one a mod before it has (the page says which); a mod is read after the
mods it needs. The single-game app has no menu: its `settings.ini` takes the same `[mods]` lines.

## Custom Freeplay (Settings > Custom Freeplay)

A fourth tile on the collection's menu, off by default (`settings.ini`
`[general] custom_freeplay`, the app's, in the collection's root file; the
Settings page's Custom Freeplay row; with it off the menu, the boots and the
games are as before). The tile opens two pages in the menu's own look: the
player (the three games' tiles again: an inline skater from Aggressive Inline,
a BMX rider from Dave Mirra Freestyle BMX 2 or BMX XXX), then a custom map for
that game: the one list of every map, each startable by every player. Enter on
a map boots the player's game and starts it straight into free play on the
map: the game's own player select comes up first (its own
skaters or riders, as the game lists them), then the level loads with no more
menus. The design (docs/CUSTOM-LEVELS.md section 1): the player type picks the
game, so each game keeps its physics, tricks and camera; the mix is in the
menu and in the content.

A custom map is a mod in the collection's Mods folder, `<support>/Mods/<map>/` (a
folder or a zip), or a folder in the maps folder, `<support>/Maps/<map>/` (as
before mods had manifests): the level files of the game it was made in, laid
out as that game's slot has them (below), and a description, a `[map]` section
in its `mod.ini` or a `map.ini`. The map page lists both places, each map once.
No boot but its own reads a map: a folder with a `map.ini` in a game's Mods
folder is not a mod either, whatever its line in `[mods]` says.

```ini
[map]
name = Test Park        ; shown in the menu
game = ai               ; the game it was made in (ai, dm2, bmx): its level formats
slot = TUT              ; that game's slot its files are laid out in (below)
preview = preview.bmp   ; a picture in the folder, optional (a BMP of 4, 8, 24 or 32 bits)
author = ...            ; optional
npcs = none             ; none (the default) | slot, below
```

A map's people, riders and traffic are those its own files place (its level
objects, its scripts). Where a game keeps a level's NPCs outside the level's
folder, by the level's number (BMX XXX's NPC riders: the executable's records,
the rides in `assets/replay.zal`), a map leaves its slot's out, unless it says
`npcs = slot`: a map made from the slot's own level, which keeps its riders.

The map page lists the maps folder's maps, read again each time the page
opens. Each game lends its free play one slot (`collection/maps.h`
`mapSlots`); a map in the slot its own game lends starts for every player.
The player of the game it was made in reads its folder as it is; any other
player's boot gets it converted into its own game's level files for its slot
when it starts (a moment; the page says so while it converts), held in memory
for the boot and read over the mods: nothing is written, and the map's folder
is not read at that boot. A map that does not convert for a game (over the
game's budgets: too many parts drawn with no visibility zones, too many
texels, level files larger than the game's largest stock level's, which
would not fit its memory) does not start; once a start of it has failed the
list shows the converter's reason for that game until the app quits. Each game's own start route (its devstart override) takes the
boot from the title to the game's own player select, waits for the player's
pick there, then picks the level and loads (`PS2X_DEV_START_LEVEL`,
`PS2X_DEV_START_SKATER=ask`, the boot's values):

- Aggressive Inline: `TUT`, the Tutorial's level 7 (`ASSETS/LEVELS/TUT`),
  reached through the Estate's free skate entry (the boot gets
  `PS2X_ESTATE=1` and `PS2X_DEV_START_LEVEL=ESTATE`;
  `aggressive_inline_devstart.cpp`; the Estate made from the GameCube disc
  is what gives the entry, so without it the map page offers Aggressive
  Inline's player no map and says what it needs).
- Dave Mirra Freestyle BMX 2: `WOODWARD`, Woodward Camp's level
  (`ASSETS/LEVELS/WOODWARD`, as `dm2_level_convert.py --level WOODWARD`
  writes a level), reached as FREERIDE's level select value 0
  (`dave_mirra_devstart.cpp`: FREERIDE, the rider and bike choices the
  player's, the modifiers, the level). The one row open from the start, and
  the one with the fewest extras of its own: a FREERIDE level has no NPCs,
  its gaps fire on its own faces' tags (a map's have none), it has no tree
  fade (Swampy Trails' and Galloon's rows have), and its far clip is the
  game's table's, 187 m. (The park slot also holds a park theme's editor
  objects: Woodward with them ran DM2's memory out.) Its level-point camera
  (the instructions, game over and after-replay screens) looks from the
  map's own camera points, not Woodward Camp's in the executable's table:
  the boot gets `PS2X_DM2_LEVEL_CAMERA_POINTS=1`, DM2's option for a level
  whose scene has its own "CameraPos" idle points (off: the table, as on
  the disc; a level without its own keeps the table either way).
- BMX XXX: `CHL_MULY`, The Bronx's level (`ASSETS/LEVELS/CHL_MULY`, the
  level select's first row), reached as a HARDCORE TOUR row
  (`bmx_xxx_devstart.cpp`: the rider select the player's, the row picked; the
  level is opened in the saved game in memory for the pick and the bits put
  back once the level has begun, so a save made after keeps the player's own
  progress). A challenge level has no clock: the ride lasts as long as the
  player likes. The map's `OBJCTV.TXT` gives it no goals, and as the game
  counts a level's goals into the player's record of it, the start keeps
  The Bronx's counts and puts them back on the screens a ride ends on (game
  over, where it saves) and in the menus (`PS2X_BMX_KEEP_LEVEL_GOALS`, which
  the boot gets). The Bronx's three NPC riders are the level number's, not
  its folder's: the boot gets `PS2X_BMX_LEVEL_NPCS=0` (`bmx_xxx_npcs.cpp`:
  the game sets up no NPC riders, as in multiplayer), or `1` with
  `npcs = slot` (a map made in BMX XXX, started by BMX XXX's player).

At a boot into a map the map's files are read last of the mods (they win),
the other maps are left out, and the mods are read as the Mods page has them. The custom map's
files are the player's: nothing is bundled with the app.

Developer runs: `PS2X_MENU_SCRIPT` drives the pages like the others (the
tile is the fourth, then Enter, the player's tile, Enter, the map's row,
Enter); `--game <id> --map <folder>` (or `PS2X_APP_GAME`, `PS2X_APP_MAP`)
starts the maps folder's map of that folder name for that game's player, no
menu first, converted as the map page does it; `PS2X_DEV_START_SKATER=<n>` in
the environment wins over the boot's `ask` for an unattended run;
`[app] custom freeplay: ...` in the log names the map, the game it was made
in, its slot, the start, the mods the boot was given and, for a converted
map, the conversion's time and the files held in memory. `ps2xMapConvert`
(a build tool) writes a conversion to a folder.

## Picture quality (GPU renderer)

Now that the meshes are drawn by the GPU, the PC version need not keep the
PS2's limits. Seven Graphics rows, each in settings.ini `[graphics]`, all on by
default; each applies at once (they dim under the CPU renderer). An eighth, Depth Test (`depth_test = exact|fast`,
fast by default on Android, next start), lets the GPU keep 16-bit depth targets, faster on phones with a few
pixels different. They change only how the GPU renderer draws
(the game itself runs exactly as before); the CPU renderer and the strict
render path (the logic checks' and PCSX2 comparisons' mode) ignore them.

| Row | settings.ini | Values (default first) | What it does |
|---|---|---|---|
| Anti-Aliasing | `anti_aliasing` | fxaa, off | Smooths jagged edges of the 3D picture (FXAA, before the HUD is drawn, so text stays sharp); a window smaller than the picture is filled from a filtered half-size copy |
| Texture Filtering | `texture_filtering` | 16x, 8x, 4x, 2x, off | Anisotropic filtering: ground and walls at a grazing angle stay sharp |
| Fog | `fog` | smooth, original | Per-pixel fog instead of the PS2's eight fog bands |
| Shadows | `shadows` | sharp, original | The skater's shadow drawn by the GPU at full resolution |
| Depth Precision | `depth_precision` | high, original | Far surfaces stop flickering through each other (the PS2's 16-bit depth, one step 49 units at 2000 with Draw Distance Farthest) |
| Perspective Correction | `perspective_correct` | true, false | Shading without the PS2's screen-space warping (and its jump in brightness where the ground meets the camera), skater and character textures without affine warping |
| Character Lighting | `character_lighting` | xbox, ps2 | Skaters, riders and people lit by the Xbox release's lights (brighter, from other angles: the Ape's fur shows as on the Xbox) instead of the PS2's; Aggressive Inline and BMX XXX (Dave Mirra 2's riders keep the PS2's lights: its Xbox release's are not known) |

Always on the GPU renderer, by design: surfaces crossing the near plane are drawn whole, where the PS2's clipping
program splits them and clamps their corners to the GS window, which bends their textures at the bottom of the screen
(Industrial's grating floor; the CPU renderer draws the PS2's triangles).

Environment (developer runner, tests): `PS2X_GS_AA`, `PS2X_GS_ANISO`,
`PS2X_NATIVE_FOG`, `PS2X_NATIVE_SHADOWS`, `PS2X_GS_PRECISE_DEPTH`,
`PS2X_NATIVE_PERSPECTIVE`, `PS2X_NATIVE_LIGHTING` (a value name or its index); on PS2X_CONTROL
`set display.anti_aliasing 0` and so on. Design:
`runtime/src/lib/gs/GPU_BACKEND.md` ("Picture quality beyond the PS2").

