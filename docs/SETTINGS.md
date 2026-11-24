# Settings: the pages in each game's own menus

Each game in the collection has one Settings screen, drawn in that game's own menus, fonts and highlight. The page
body is the same in all three games (`engine/overrides/zge_settings.cpp`). This file says what each row
does, which games have it and where it is kept. Keys, mouse and gamepads are docs/CONTROLS.md; mods are docs/MODS.md.

## Where it is

- Aggressive Inline: Options > Settings (the Controller icon), and Settings in every pause menu (Career, Free
  Skate, Timed Run, the Tutorial's lesson list, split screen).
- Dave Mirra BMX 2 and BMX XXX: the SETTINGS line in Options and in the pause menu.
- macOS and Windows, all three games: Settings... in the app menu (macOS, Cmd+,) or in the window's system menu
  (Windows: the title bar's icon, a right click on the title bar, or Alt+Space, then T). In a level it pauses as Start
  does and opens the pause menu on Settings. Outside a level, Aggressive Inline goes to Options > Settings; Dave Mirra
  BMX 2 and BMX XXX say "Settings are in Options". Android has no menu: use the game's menus.

L1 / R1 (Q / E) change the section, L2 / R2 jump between the headings within a section (Controls' KEYS and BUTTONS,
Gameplay's CAMERA, ASSISTS and CHEATS), up and down pick a row, left and right change it, Cross changes a switch or
starts a rebind, Triangle goes back. A row applies at once unless its help line says "next start". A row shows dimmed
while another setting makes it idle (the GPU renderer's rows under the CPU renderer, Wide Aspect with Widescreen off);
you can still change it. A row that could do nothing in your setup is not shown at all: one that needs an extras disc
you have not added, the readable code, or something the platform lacks (a second window, window modes on Android)
(`collection/offered.h`).

The collection menu has its own Settings page for what spans the games: the window and sound rows every game shares,
the games' discs (Settings > Games), Mods, Save Profiles (each game's careers: Save Profile picks the one it plays
from its next start, New Save Profile starts another, Copy Save Profile copies the chosen one's save and time played),
Record Play and its Recordings, Custom Freeplay, the menu's intro movie and music, and last Check for Updates and
Roll Back Update (the app's own update and the version it replaced: README.md "Updates"). Settings > Recordings lists
every game's recordings with their level, date and length. Choosing one plays it back exactly as it was played, from
the saves it was recorded with, which stay apart from yours (docs/CONTROLS.md "Watching a recording").

## The sections

**Display**: Display Mode (window, full screen, and on macOS and Windows borderless; Android is always full screen and
has no Display Mode row), Scaling (smooth, integer), VSync, Variable Refresh, Frame Interpolation (pictures between
the game's frames on a display faster than 60 Hz, GPU renderer, off by default; on Android it puts a 120 Hz
screen at 120 Hz while it is on, the screen otherwise at the game's 60; only with Variable Refresh off: with it on,
each game frame is shown when it is ready and no picture between is made), Widescreen (drawn only: the game
plays as at 4:3, so what only the sides show stays still, as just off a 4:3 screen), Wide Aspect (16:9, 16:10, 21:9,
32:9, the window's), Show Frame Rate, Pause in Background, Map Window (the bottom screen in a second window; not on
Android), Always on Top (the Map Window kept above other windows; off by default), Clean Top Screen (only while a
bottom screen is up: the Map Window open, or a dual-screen handheld's second display; off by default: the game's HUD
pieces the bottom screen shows are left off the top screen while it shows them, drawn only. Aggressive Inline: the
clock and the trick line; DM2: the clock, the trick lines and the goal label; BMX XXX: a timed challenge's clock and
the challenge's display, and the trick lines. The score and the other pieces stay).

**Graphics**: Renderer (GPU, CPU), Resolution (Native, 2x, 3x, 4x: 3x by default on Mac and Windows, 2x on Android),
Full Height, the picture quality rows (below: Anti-Aliasing, Texture Filtering, Fog, Shadows, Depth Precision, Depth
Test, Perspective Correction, Character Lighting), Native Meshes (below), Motion Blur (off, low, original), Draw
Distance (original, far, farthest). With the game's Xbox disc set up: Xbox Textures in all three games, and in
Aggressive Inline also Xbox Skaters (from the next level) and Xbox Movies (below). Last, Dump Textures (writes every
texture drawn to `<support>/Texture Dumps`, for texture packs; next start).

Draw Distance is the game's own state, so it is Original by default, applies at the next start, and its row says it
changes the game. Far and Farthest move the far clip, the fog by the same ratio, and the models' level-of-detail
distances with it. In Aggressive Inline: split screen's 100 to 250 or 1000, and with Farthest every level's 1000 to
2500 and every camera's far clip the game does not set again, 200 to 500. In Dave Mirra BMX 2 and BMX XXX: every
level's, single player's too.

**Sound**: Master Volume, Music Volume, Effects Volume, Speech Volume (Aggressive Inline and BMX XXX; Dave Mirra
BMX 2 has none), Movie Volume, French Voices (Aggressive Inline with the Xbox files, below), Mute in Background, Shuffle (the soundtrack in
a random order, all three games, off by default: below, "Shortcuts"). The
game volumes are the game's own Options > Sound values, read from and set through the game, so both pages show the
same.

**Controls**: Button Prompts (auto, keyboard, Xbox, PlayStation, Nintendo), Gamepad (on, off), Stick Dead Zone,
Connected Gamepads (the pads in player order; Cross writes their details to the log), Test Rumble, Swap Pads 1 and 2,
Mouse Look (Free Look, Right Stick), Mouse Sensitivity, Invert Mouse Y, then KEYS (a row per action and per PC
shortcut: Restart, Hide HUD, Screenshot, Photo Mode), BUTTONS (Gamepad Layout: PS2 Original or Modern Triggers, every
gamepad button at once; then a row per game button and its gamepad button) and Reset Controls (it says "Sure?": a
second press within 5 seconds resets, and says so). The gamepad rows dim while Gamepad is off. Rebinding is in
docs/CONTROLS.md "Rebinding in the game". Every change is written to `controls.ini` at once.

**Gameplay** (kept in the game's `gameplay.ini`, applied live):

- CAMERA: Field of View, Camera Distance and Camera Height (each a share of the game's own), and in Aggressive
  Inline Camera Style (PS2, Xbox: the Xbox release's field of view, single player only). In Aggressive Inline and
  BMX XXX they change the picture, not play: the game decides by its own view (docs/ENHANCEMENTS.md).
- ASSISTS: Aggressive Inline's Out of Juice (an empty juice meter ends the run or not), Juice Drain and Juice Lost
  on Crash. Dave Mirra BMX 2's and BMX XXX's Grind Balance Loss and Manual Balance Loss (DM2's file also reads
  `assist.wheelie_balance_loss`).
- CHEATS: the game's own cheats as switches that leave the saved game alone, and Unlock Everything (two steps,
  "Sure?" then "Unlocked"; the one row that changes the save). Aggressive Inline: Perfect Grind, Perfect Manual,
  Perfect Handplant, Super Spin, Low Gravity Wall Ride, Juice Regeneration, Invulnerability. Dave Mirra BMX 2 and
  BMX XXX: Perfect Grind and Perfect Manual. docs/CHEATS.md has what each does.

**Game**: Language, Skip Intro Movies, Gameplay Tuning, Career Scripts, Developer Menu (the game's hidden debug menu
or display on F1, next start: docs/DEVTOOLS.md), Record Play (each start of the game recorded to its Recordings
folder, next start: docs/DEVTOOLS.md), Recordings (how many; Cross opens their folder, or on Android says where it
is), Custom Freeplay (in a collection: the tile for custom maps on the menu), then EXTRAS: the content beyond the PS2
disc and the cut content each game has (below). In a collection, after a change that applies at the next start, the
last row is Restart Game (how many changes wait): it starts the game again at once (a custom map's too).

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
| Zoo Level, Zoo Unlock, Camera Style | yes | no | no |
| Career Scripts, Ape Skater, Xbox Free Skate | with the Xbox files | no | no |
| Estate Level | with the GameCube files | no | no |
| Xbox Levels, Create Rider, Desert and Highway Themes, Retro Movie, Attract Demo | no | yes (three of them with the Xbox files) | no |
| Park Editor, Xbox Challenges, Pool and Trash Can Pieces, Level Previews, Z-Axis Logo Movie, Intro Movie, GameCube Levels, Unused Menu Lines, All Multiplayer Modes, Z-Axis Logo at Start, Turok and Vexx Trailers, Create-a-Rider Extras, Green Skin, Topless Girl, Xbox Songs | no | no | yes (eleven with the other releases' files) |

"With the Xbox files" and the like: the row shows only once your own disc of that release is set up with the game
(Settings > Games: Find Discs finds it beside the PS2 disc, or the game's Add Xbox Disc and Add GameCube Disc). The
setup makes its files once; the game never reads that disc. An app built with `make-app.sh` and the discs (README
"Building") can carry them instead.

## Where the settings are kept

In the collection's support folder (README "Your discs"):

- `settings.ini` at its root, what every game shares: `[display]` (window, full screen, scaling, vsync, aspect,
  the frame rate, the map window, Clean Top Screen, Xbox textures, skaters and movies, texture dump), `[graphics]` (renderer,
  resolution, full height, motion blur, the picture quality rows, depth test, render path), `[audio]` (master and
  movie volume, mute in background, shuffle), `[general]` (pause in background, skip intro movies, log file, record play,
  Custom Freeplay), `[host] disc`, `[menu]` (the collection menu's intro movie and music) and `[mod order]`.
- `games/<id>/settings.ini`, the game's own options: `[general] language`, `french_voices`, `developer_menu`,
  `[gameplay]` (draw distance, tuning, career scripts), `[extras]` (each extra's switch), `[saves] profile` (the
  career's save file), `[memcard] pcsx2_import_offered`, `[mods]` (a mod switched off for this game).
- `games/<id>/gameplay.ini` (the Gameplay rows), `games/<id>/controls.ini` (docs/CONTROLS.md), `games/<id>/Saves`.

Each file's comments say what each key takes. The developer runner reads the same names from the environment
(`zx::boot::text`; docs/DEVTOOLS.md "Test runs").

## Shortcuts and the pause menus

- PC shortcuts, the same in all three games (`controls.ini` `[hotkeys]`): Restart (Backspace; gamepad L3+R3), Hide
  HUD (F9), Screenshot (F12), Photo Mode (P), Switch Game (F8: back to the collection menu; it has no row, change it
  in `controls.ini`), Previous Track (F5; gamepad L3+L1), Next Track (F6; gamepad L3+R1), Shuffle (F7; on a handheld: Settings > Sound > Shuffle). Each has a `<name>_pad` chord.
- Next Track and Previous Track, in a level with music: the song after or before the one playing, of the songs the
  game has on (Aggressive Inline's pause menu Playlist; every song in Dave Mirra BMX 2 and BMX XXX) and the songs
  mods add (docs/MODS.md "Songs"), as the game's own Next Track asks for one (in play, or paused). With Shuffle on,
  Next Track is a song at random and Previous Track the song played before. Shuffle (the key, or Settings > Sound):
  when a song ends, a song at random instead of the next, none twice until each has played; the level's first song
  too. With Shuffle off the game picks as it always has; a game set to repeat its song keeps repeating it.
- Restart does what fits the mode, and acts only in play. Aggressive Inline: the challenge you were given again
  (Career; else the level), the lesson (Tutorial), the level (Free Skate) or the run (Timed Run, split screen). Dave
  Mirra BMX 2: the run again from its start (a free ride's or a timed one's). BMX XXX: the turn from its start. The
  pause menus have it as an item too (Aggressive Inline: Restart Level, Restart Run, Retry Challenge, Restart
  Lesson).
- Hide HUD hides the score, meters and messages until pressed again or the front end comes back.
- Screenshot saves the picture as a PNG in `Pictures/Z-Axis Collection/<game>` and says "Screenshot saved" top
  left (not in the picture).
- Exit to Collection, in each game's main menu and pause menu, goes back to the collection menu without restarting
  the app (Aggressive Inline's Quit to Desktop item becomes it). Aggressive Inline asks "Are you sure?" first before
  Level Select, Exit Game and that item, as its Xbox release does.

## Practice (Settings > Game > Practice)

Off by default. On, the `practice_point` key (F2; on a gamepad L3+L2, `practice_point_pad`) sets a
restart point where the player is, and Restart goes back to it instead of restarting; with no point Restart does what
it did. A point can be set only on the ground. Each try is timed in the game's seconds of play from the point (not
while paused), shown top left as it runs; going back shows its time and the best ("Try 3: 2.47   Best 1.48"). The
point goes when the level is left. What comes back:

- Aggressive Inline: the skater where the point was, heading and speed as there, and the juice: through the game's own
  respawn (as its challenge retry), then rolling on at the point's speed. The level's own state (score, goals,
  pickups, a Timed Run's clock) goes on as played.
- Dave Mirra BMX 2 and BMX XXX (single player in DM2): the rider stopped, on the ground face nearest the point, heading
  as there: the games' own respawn (as after a bail) aimed at the point. Their speed lives in the wheels, which no
  placement of the game's sets, so the rider starts again from rest.

The setting is the app's (`[general] practice` in the root settings.ini); a developer run without the app turns it on
with `ZX_PRACTICE=1`. Code: `engine/overrides/zge_practice.cpp` (keys, clock, lines), `zge_practice_rider.cpp`,
`aggressive_inline_practice.cpp`, `dave_mirra_practice.cpp`, `bmx_xxx_practice.cpp`; the row is
`collection/practice.cpp`.

## Photo mode

Photo Mode in the pause menu, or the `photo_mode` key anywhere in a level (from play it pauses first and resumes
after): the game stays paused, the HUD is hidden and a free camera takes over, within 40 units of where it started.
The keys and filters are in docs/CONTROLS.md "Photo mode"; screenshots get the filtered picture at full resolution.

How the settings, prompts, pause items and photo mode reach each game's menus is in the overrides' own comments
(`aggressive_inline_settings.cpp`, `aggressive_inline_prompts.cpp`, `aggressive_inline_gameplay.cpp`,
`aggressive_inline_restart.cpp`, `aggressive_inline_photo.cpp`, and their `dave_mirra_*` and `bmx_xxx_*`
counterparts); per-function facts are in the work database.

## Aggressive Inline's extras

### The Zoo (Game > Extras > Zoo Level)

The level cut from the PS2 release, in the free skate and timed run level select between the Museum and the Editor
Level, always unlocked (`[extras] zoo`, on by default; next start;
`games/aggressive/overrides/aggressive_inline_zoo.cpp`). With the Xbox disc set up it is the Xbox release's level made
into the PS2's form, else the PS2 disc's prototype. With the Xbox files it also has its ambient NPCs (kelp, sharks,
the willows' swaying foliage), and the GPU renderer draws the Xbox pictures.

In split screen the level select has the Zoo as a fifth entry: the single-player level with smaller textures (64 a
side), a player two start, the free skate script without its intro camera (doors open, willows sway), the
split-screen games' props (Egg Hunt's eggs, Twenty One's cards, from the PS2 disc's MP\MOVIE), egg spots below the
respawn points, each willow once, its NPCs animated, and the Movie Lot's split-screen field of view.

Nothing of the Zoo is converted while the game runs. The setup makes it when the Xbox disc is set up
(`games/ai/extras/xbox-converted`, `collection/extras/ai_converted.cpp`); at build time `zx-xbox-convert`
(`tools/modding/xbox_convert.cpp`, `runtime/zaxis_zoo.h`, run by platform/mac/make-app.sh and
make-windows.ps1 from the extracted Xbox and PS2 discs) writes the level, its split-screen files, the NPC models and
the Xbox originals of their textures (`textures/<path>.xtp`) into Resources/xbox-converted (Windows:
xbox-converted\ next to disc\). The app sets `ZX_XBOX_CONVERTED` to the setup's folder, else the bundled one, and
the override only reads those files (`aggressive_inline_converted.h`).

Developer runs: `ZX_ZOO=1` (off in the runner by default); `zx-xbox-convert --xbox <extracted Xbox disc> --ps2
<extracted PS2 disc> --out <dir>`, then `ZX_XBOX_CONVERTED=<dir>` (without it the runner's Zoo is the PS2 disc's
prototype). The tool's options: `--split-screen-texture-side N`, `--group-entries N`, `--hide a,b`,
`--split-screen-eggs "x,y,z;..."`, `--keep-willow-twins`. Variables: `ZX_ZOO_NPCS=0` (no ambient NPCs),
`ZX_ZOO_MP_SCRIPT=empty|free` (split screen with the empty script, or the free skate one as it is: no timer),
`ZX_ZOO_MP_NPC_ANIMATION=0` (split screen's NPCs animate only as the game decides: hardly ever), `ZX_TRACE_ZOO=1`
(also the NPC type lookups), `ZX_TRACE_ZOO_NPCS=1` (each Xbox NPC type lookup, counted by type and caller).

### Estate (Game > Extras > Estate Level)

TUT/Estate, an earlier export of the Tutorial site left on the GameCube disc, as "Estate" in the free skate level
select, before the Editor Level, always unlocked (`[extras] estate`, off by default; shown only with the GameCube disc
set up; next start; `games/aggressive/overrides/aggressive_inline_estate.cpp`). The career's Tutorial is always the
retail one.

Estate's own scripts run, with its 25-lesson tutorial, and its givers answer Circle. Its pause menu is the
Tutorial's lesson list: the 23 lesson items start Estate's lessons 1-8, 10-20 and 22-25, Restart Lesson the current
one, and the checkboxes show the lessons done since the level started. Lesson 9 has no giver, rail or respawn point
in the level, so it hands over to lesson 10 when it starts. Estate's records are kept beside the career save
(`<saves>/<profile>.estaterecords`), so the Tutorial's own row stays the Tutorial's. The Xbox texture pack is kept
off its textures.

The setup makes it when the GameCube disc is set up (`games/ai/extras/estate`); at build time `zx-gc-convert`
(`tools/modding/gc_convert.cpp`, `runtime/zaxis_estate.h`, run by platform/mac/make-app.sh and
make-windows.ps1 on what tools/modding/gc_disc.py extracted from the GameCube image) writes the level's files and
its level-select icon into Resources/estate (Windows: estate\ next to disc\); `--picture <file.bmp>` writes the
icon's picture too. The app sets `ZX_ESTATE_DIR` to the setup's folder, else the bundled one.

Developer runs: `python3 -I tools/modding/gc_disc.py extract <gc image> --out <gc> assets/levels/TUT/Estate`,
`zx-gc-convert --gc <gc> --out <dir>`, then `ZX_ESTATE=1 ZX_ESTATE_DIR=<dir>`; `ZX_DEV_START_LEVEL=ESTATE` starts in
it. Variables: `ZX_ESTATE_RECORDS=0` (Estate shares the Tutorial's records row), `ZX_TRACE_ESTATE=1` (each file
served, a line every 10 s in the level), `ZX_ESTATE_PROBE=<points>:<out>` (the game's collision under each point of
ai_level.py ground's grid; `ZX_ESTATE_PROBE_ANY=1` in any level), `ZX_ESTATE_GRINDS=<edges>:<out>` (the game's rail
search at each of ai_level.py edges' grind edges), `ZX_ESTATE_LESSONS=<out>` (the 23 lessons in turn, each started
by its giver; a line per lesson and a summary), `ZX_ESTATE_ACTION=1` (the Action move's target search traced).

### The Ape, Xbox tuning, Xbox scripts, Zoo unlock, camera (Game, Gameplay)

These rows take what the Xbox release has beyond the PS2's. The Ape, Xbox Free Skate and Career Scripts rows show
only with the Xbox files each reads. The Game rows apply at the next start; the developer runner has them off
unless its variable is set.

- **Ape Skater** (`[extras] ape`, on by default; `ZX_APE=1`; `games/aggressive/overrides/aggressive_inline_ape.cpp`):
  the Xbox's secret skater, last in the skater select, with the Xbox's stats (the Tinman's), the default name APE and
  the Xbox's picture. Its model is made from the Xbox's GOR.ZIT once, at setup or build time (`zx-xbox-convert`,
  `runtime/zaxis_ape.h`: xbox-converted/ASSETS/SKATERS/GORILLA/GOR.ZIT, 256 a side; the GPU renderer draws the 512
  originals). It unlocks with the Mummy (the Museum's hidden challenges, the all-skaters code, Unlock Everything), as
  on the Xbox. The game never saves the Ape's career, so the native saves write it after the game's data in the same
  file (an "XAPE" block; the file stays a PS2 save).
- **Gameplay Tuning** PS2 / Xbox (`[gameplay] tuning = ps2|xbox`, default ps2; `ZX_TUNING=xbox`;
  `games/aggressive/overrides/aggressive_inline_tuning.cpp`): the Xbox balance. Its tuning table is the PS2's value
  for value; what differs is experience (special tricks +300,000, the eight key items nothing instead of +500,000) and
  Civic Center's 350,000 point trick (exactly 350,000 passes). With ps2 nothing is hooked.
- **Xbox Free Skate** (`[extras] xbox_free_skate`, on by default; `ZX_XBOX_FREESKATE=1`;
  `games/aggressive/overrides/aggressive_inline_freeskate.cpp`): the Boardwalk's and the Cannery's free skate scripts
  from the Xbox files (`levels/brdwlk/brdwlkt.bin`, `levels/cannery/canneryt.bin`): their doors open as in the career,
  coaster cars, fan, boulder loop, the Cannery's cutter, idle hints.
- **Career Scripts** PS2 / Xbox (`[gameplay] career_scripts = xbox|ps2`, default xbox;
  `ZX_CAREER_SCRIPTS=xbox|ps2`; `aggressive_inline_careerscripts.cpp`; the Xbox files `levels/<level>/<level>.bin`,
  `levels/civic/motions.zal`): the career's level scripts from the Xbox release, the PS2's with the Xbox's bug fixes
  (challenges that could stick or be offered again, wrong previews, texts that say what the code checks). The
  Tutorial stays the PS2's. Two PS2 texts the Xbox scripts dropped are put back (in memory, only into the exact US
  Xbox files, checked by size and hash): the Movie Lot's first-start hint "Use the select button to view the current
  challenges." and the Hangar's "Turret challenge complete!". A skater's saved progress in a level holds that
  script's variables, whose layout differs in four levels, so a level the skater has played keeps the script it was
  played with; the setting is for the levels not played yet.
- **Zoo Unlock** Museum / Always (`[extras] zoo_unlock = museum|always`, default always; `ZX_ZOO_UNLOCK`;
  `aggressive_inline_zoo.cpp`): Museum is the Xbox's rule: the Museum's eighth challenge (challenge points) unlocks
  the Zoo for that skater ("New level unlocked!"), and the free skate select shows the game's own lock until then;
  the all-levels code and Unlock Everything open it too. Always: open whatever the career.
- **Camera Style** PS2 / Xbox (Settings > Gameplay; gameplay.ini `[camera] style = 0|1`, default 0, live): the Xbox
  release's field of view. Its follow camera is the PS2's (same code, tuning, collision probe and smoothing); its
  projection takes the vertical field of view as FOV x 1.1 / aspect, so it keeps about the same width and crops the
  top and bottom in widescreen, where the collection's widescreen widens the sides: at 16:9 the skater is 1.34x as
  tall as with PS2, at 4:3 0.96x. Single player only (the Xbox fixes 50 in split screen; the PS2's per-level field
  of view stays). Field of View applies on top. Both are drawn only: the game plays with the PS2's field of view.

### Languages from the Xbox release

(`games/aggressive/overrides/aggressive_inline_language.cpp`; runtime `zaxis_text.h`, `xbox_audio.h`.) The US PS2 disc
is English only; the Xbox release has French, German and Spanish. Its files come from your Xbox disc, copied once when
it is set up (Settings > Games), or at build time: platform/mac/make-app.sh (`XBOX_ISO`) and make-windows.ps1
(`-XboxIso`) extract the image and bundle what tools/modding/xbox-files.txt lists into Resources/xbox (`ZX_XBOX_DIR`,
read through `zx::xbox_files`). The game never reads an image.

**Rows** (only when the files are there): Game > Language (English, Français, Deutsch, Español; next start;
settings.ini `language = english|french|german|spanish`) and Sound > French Voices (live; `french_voices =
auto|true|false`, auto = on with French). Changing the language puts French Voices back to auto.

The text is translated where it is drawn, from the Xbox string tables and level scripts paired with the PS2's. The
PS2 fonts are swapped for the Xbox ones, which have the accented letters; texts the Xbox narrows are narrowed the
same way. French Voices plays the Xbox's French dub of the speech. With an Xbox-style pad the Tutorial's loading
screens are the Xbox's pictures of its controller. `ZX_TRACE_LANGUAGE=1` lists texts drawn with no translation,
`=2` every text with its translation.

**English** changes nothing: no file is replaced, no table is made, and the text hooks pass the text through.

## Dave Mirra BMX 2's and BMX XXX's extras

Each is off by default (DM2's Xbox Levels on) and applies at the next start. What each does, and why it belongs to
that game, is docs/ENHANCEMENTS.md "Per game". In `games/<id>/settings.ini` `[extras]`:

| Row | Key | Game | What |
|---|---|---|---|
| Xbox Levels | `dm2_xbox_levels` | DM2 | Venice and Greenville, the Xbox release's levels, in Free Ride and ProQuest (Xbox files) |
| Create Rider | `create_player` | DM2 | the rider creator its code has and no release shows: CREATE RIDER on the main menu |
| Desert and Highway Themes | `xbox_themes` | DM2 | the park editor's two Xbox themes (Xbox files; a park saved on one does not load with it off) |
| Retro Movie | `retro_movie` | DM2 | the movie gallery's sixteenth line (Xbox files) |
| Attract Demo | `attract_demo` | DM2 | the main menu's idle demo (with the Xbox files the Xbox's three demo replays, else level 3's alone) |
| Park Editor | `park_editor` | BMX XXX | the park editor no retail menu reaches, as the main menu's third line; SESSION rides a saved park |
| Xbox Challenges | `xbox_challenges` | BMX XXX | the Xbox and GameCube challenge scripts (their spoken lines, Sheep's tethers, two fixed steps) |
| Pool and Trash Can Pieces | `editor_pieces` | BMX XXX | the GameCube editor's two extra pieces |
| Level Previews | `level_previews` | BMX XXX | the Xbox's level-select preview movies |
| Z-Axis Logo Movie | `logo_movie` | BMX XXX | the movie gallery's twentieth line |
| Intro Movie | `intro_movie` | BMX XXX | the intro movie at boot, as the GameCube plays it |
| GameCube Levels | `gc_levels` | BMX XXX | the Secret Level and three test levels the GameCube disc keeps |
| Unused Menu Lines | `menu_lines` | BMX XXX | the pause and game over lines no release's menus put up |
| All Multiplayer Modes | `all_mp_modes` | BMX XXX | the multiplayer modes SELECT GAME leaves out |
| Z-Axis Logo at Start | `logo_splash` | BMX XXX | the logo movie at start in place of the developer's picture, as the GameCube starts (Xbox files) |
| Turok and Vexx Trailers | `trailers` | BMX XXX | the Xbox's two Competitive Edge trailers in Extras, Movies (Xbox files) |
| Create-a-Rider Extras | `car_textures` | BMX XXX | create-a-rider's other-release textures and two parts (a rider saved with one turns them on) |
| Green Skin | `green_skin` | BMX XXX | the other releases' Green Skin mode: create-a-rider's green skins and faces |
| Topless Girl | `topless_girl` | BMX XXX | the other releases' Topless Girl mode: create-a-rider's topless top |
| Xbox Songs | `xbox_songs` | BMX XXX | the Xbox disc's eight songs no soundtrack names, as the soundtrack's tracks 21 to 28 |

BMX XXX's Gameplay Tuning (`[gameplay] tuning = xbox`) gives Syracuse's two timed challenges the Xbox and
GameCube's 60 s. Its Language row picks the game's own French, German or Spanish, filled in from the other
releases' translations. Both need the other releases' files (their discs set up, Settings > Games).

## Xbox textures, skaters and movies

With the game's own Xbox disc set up (Settings > Games; or at build time: `XBOX_ISO` or `XBOX_DIR`, `DM2_XBOX_ISO`,
`BMX_XBOX_ISO` for platform/mac/make-app.sh, `-XboxIso` or `-XboxDir`, `-Dm2XboxIso`, `-BmxXboxIso` for
make-windows.ps1), its Xbox textures are made once into the game's `xbox-pack` folder, and the game never reads the
disc. The GPU renderer then draws the Xbox picture wherever the Xbox has a counterpart of a PS2 texture, and the PS2
picture where there is none. The game's memory and logic are unchanged, and the CPU renderer always shows the PS2
textures.

- Aggressive Inline: Xbox Textures covers levels, skaters, park editor pieces and the multiplayer levels (a
  counterpart is the same texture, as large or larger, and alike). The Zoo draws the Xbox originals of its converted
  textures (up to 512, DXT3), not the 128-texel copies the PS2 renderer's texture cache gets. It also has Xbox
  Skaters and Xbox Movies.
- Dave Mirra BMX 2 and BMX XXX: Xbox Textures alone. The Xbox disc's level textures are mostly twice the PS2's size
  a side, with real detail; riders, menus and the editor are the PS2's size on the Xbox too, so they stay the PS2's.
  BMX XXX's level props' textures come from its levels' `models.zal` archives: a prop whose Xbox texture is not found
  by name gets the archive's Xbox picture most like the PS2's (as large or larger, compared at 32 a side), and keeps
  the PS2's texture where none is alike.

The rows are on by default; off is the PS2 release's look:

| Row | settings.ini | What it does |
|---|---|---|
| Xbox Textures | `[display] xbox_textures = true` | the Xbox release's larger, redrawn textures in place of the PS2 ones (GPU renderer); applies at once |
| Xbox Skaters | `[display] xbox_skaters = true` | Aggressive Inline: the skaters' and people's models from the Xbox release (`ZX_XBOX_MODELS`), from the next level |
| Xbox Movies | `[display] xbox_movies = true` | Aggressive Inline: the Xbox release's 640x480 Bink movies (cutscenes, intro, logos, flythroughs re-recorded with the Xbox textures) in place of the PS2 ones; read at the start of each movie |

Developer runner and tests: `ZX_XBOX_PACK=<pack folder>`, `ZX_XBOX_TEXTURES=0/1`, `ZX_XBOX_MOVIES=0/1`; `set
display.xbox_textures 0` on ZX_CONTROL. Which texture a draw shows comes from the game's texture cache (its texture
objects), not from GS memory (`docs/GPU_BACKEND.md`, "Replacement pictures").

## Texture packs

A texture pack is a mod (the Mods page) with a `Textures` folder of PNG or DDS pictures, drawn in place of the
game's own textures at their own size. Packs work in all three games. On the GPU renderer a larger picture is drawn
at its resolution (with its mip levels, trilinear and the Graphics page's anisotropic filtering). The game itself
never sees a pack: its memory and logic are the same with one or without. The CPU renderer draws the pictures at the
PS2's resolution (each box-filtered to its texture's size), so edits show there too, extra detail does not.

- Layout: `<mod>/Textures/<game>/**/*.png` (or `.dds`), `<game>` the collection's id (`ai`, `dm2`, `bmx`) or the
  disc's serial (`SLUS-20327` and so on). A mod for one game alone can put them in `Textures` itself. Later mods win
  on the same picture; the Xbox textures sit under every pack.
- Names: PCSX2's, so a pack made for PCSX2 (its `textures/<serial>/replacements` folder) and its dump tooling work
  unchanged: `<texture hash>-<palette hash>-<bits>.png`, `<texture hash>-<bits>.png` without a palette,
  `-r<w>x<h>` for a region of a texture, `-mip<n>` for a picture's own mip level (optional: made from the picture
  otherwise), each with `.png` or `.dds`. A picture's alpha is the PS2's: 128 (0x80) is opaque, as in PCSX2's dumps.
- DDS: read as PCSX2 reads them (runtime/texture_dds.h): BC1 (DXT1), BC2 (DXT2, DXT3), BC3 (DXT4, DXT5) and BC7
  (a DX10 header), kept compressed for the GPU (a quarter to an eighth of a PNG's memory), and uncompressed 32 and
  24-bit RGB; a block format's size a multiple of 4. A DDS holds its own mip levels: make them when compressing
  (texconv `-m 0`; ImageMagick does by default), as a block-format picture without them is drawn without
  mipmapping, as in PCSX2. An uncompressed one without alpha gets PCSX2's: 255 for X8R8G8B8 and R8G8B8, 128 for
  X8B8G8R8. BC1 holds alpha 0 or 255 alone, not the PS2's 128: an opaque texel becomes 255 (ImageMagick) or vanishes
  (texconv's default alpha threshold, 0.5), so use BC3 or BC7 for the games' pictures. `.xtp` files there load as
  the Xbox pack's do (keyed by the texture's indices and name: Aggressive Inline's).
- Making one: Graphics > Dump Textures, or dump mode (`ZX_TEXTURE_DUMP=<folder>`, docs/DEVTOOLS.md), writes every
  texture the game draws under that name; edit or upscale the files and put them in a pack.
- Memory: the pictures are read on loader threads when the game starts (all of them while they fit 1 GB on Mac and
  Windows, 256 MB on Android; the rest when first drawn, the PS2 texture until then).

| Environment | What it does |
|---|---|
| `ZX_TEXTURE_PACKS=<folder>[:<folder>...]` (`;` on Windows) | the packs' folders, lowest first (the app sets it from the game's mods) |
| `ZX_TEXTURE_DUMP=<folder>` | dump mode |
| `ZX_TEXTURE_PRELOAD=0` | pictures read when first drawn only |
| `ZX_TEXTURE_PACK_MB=<n>` | the pictures' memory budget |

## Native meshes (render path)

The GPU draws the game's meshes from vertex buffers made once from the game's own mesh data, not through its VU1
microprograms vertex by vertex. The game itself runs exactly the same (its memory and timing do not change).

- Aggressive Inline (on the GPU renderer): the rigid prelit meshes (render type 4), the terrain with its grass
  shells (0x20), the sky (0x18) and the skaters and other skinned characters (8 and 0xC: bones and lights in a
  vertex shader), in every level, split screen included. They look as they do through VU1, a little smoother where
  they cross the near plane. Shadows, particles and 2D still go the PS2 way.
- Dave Mirra BMX 2 and BMX XXX: the meshes the engine's class renderers make from their draw records, with either
  renderer (DM2's riders and bikes too).

| settings.ini | Environment | What it does |
|---|---|---|
| `[graphics] render_path = native` (default) | `ZX_RENDER_PATH=auto` | native meshes in each game's form: Aggressive Inline's packets substituted on the GPU renderer (strict on the CPU one); Dave Mirra BMX 2's and BMX XXX's from the draw records, with either renderer (as `records` below) |
| `render_path = strict` | `ZX_RENDER_PATH=strict` | every mesh through VIF1, VU1 and the GS as on the console |
| | `ZX_RENDER_PATH=native` | the packet substitution, also with the CPU renderer (the same triangles sent to it as GS primitives); DM2's without the records (developers), BMX XXX's the records |
| `render_path = records` | `ZX_RENDER_PATH=records` | native meshes made by the engine's class renderers from their draw records (zge/host/class_draws.cpp): no VU1 work in the mesh packet (its texture requests stay), with either renderer, in all three games (DM2's and BMX XXX's default); a draw it does not make goes through VU1 (`ZX_TRACE_NATIVE=1` counts why) |

The Graphics page's Native Meshes row switches between `native` (on) and `strict` (off); `records` is set in
`settings.ini` only. In the developer runner `set display.native_meshes 0/1` on ZX_CONTROL switches while running.
Debugging: `ZX_NATIVE_CLASSES=4,0x20,0x18,8` (render types drawn natively; 8 or 0xC for the skinned ones),
`ZX_NATIVE_LEVELS=zoo`, `ZX_NATIVE_TINT=1`, `ZX_NATIVE_GPU=0`, `ZX_TRACE_NATIVE=1`; the design is in docs/NATIVE.md
and `docs/GPU_BACKEND.md` ("Native draws").

All three games read their files straight from the host (the game's mods over its own, "Mods" below) through the
engine's disc device. The PS2's CD/DVD client over the emulated IOP is still there:

| settings.ini | Environment | What it does |
|---|---|---|
| `[host] disc = native` (default) | `ZX_DISC=native` | the game's files from the host, ZCDDVD.IRX not loaded |
| `disc = ps2` | `ZX_DISC=ps2` | the game's CD/DVD client over the emulated IOP and ZCDDVD, as on the console |

## Mods

The players' guide is docs/MODS.md. Mods are folders or zips (read in place, never unpacked:
`runtime/zip_archive.h`, miniz 3.0.2) in the collection's Mods folder, `<support>/Mods`, each with a `mod.ini`
(`[mod]` id, name, version, author, games, needs, conflicts). What a mod holds is told by its layout:

| In a mod's folder | What it replaces |
|---|---|
| `ASSETS/...`, `MOVIES/...`, any disc path (names compared without case) | that file of the game's disc, through the engine's file system (`runtime/game_files.h`; the game's own in-memory patches, such as the Settings menus' scripts, still apply on top) |
| `ASSETS/<NAME>.ZAL/<entry>` (a folder named as an archive) | just those entries of that archive, put in the archive the game reads (below it on the disc or a mod's), the lowest mod's first, so mods with other entries combine (`zal/zal_archive.h`; MODS.md "Archives", `zal`) |
| `Music/NN*.ogg` (`.wav`, `.opus`, `.flac`, `.mp3`) | song NN of the soundtrack, in all three games (AI 01 to 13, DM2 01 to 10, BMX XXX 01 to 20; `zge_sound.cpp`) |
| `Music/<any other song>` | a song added after the soundtrack's (MUSIC.ZSD served with a wave more for each; `zge_sound.cpp`, `zge_soundtrack.cpp`; MODS.md "Songs") |
| `tuning.ini` | Aggressive Inline's tuning overrides, and BMX XXX's GameCube TUNING rows on BMX XXX's and on Dave Mirra BMX 2's own values (docs/MODS.md "tuning.ini", `docs/DEVTOOLS.md`) |
| `Textures/<game>/` (or the disc serial, or `Textures/` for a mod of one game) | the GPU renderer's replacement pictures (`ZX_TEXTURE_PACKS`) |
| `[map]` in `mod.ini`, or `map.ini` | a custom map (Custom Freeplay, below), read by no other start |

Each game reads, in order, a later mod's file over an earlier one's (`collection/mods.h`,
`collection/mods.cpp`):

1. the game's own Mods folder, `<support>/games/<id>/Mods` (the single-game app's `<support>/Mods`). There each
   folder or zip is a mod of that game, by its name, without a `mod.ini`. Files loose there are read under every
   mod; a folder named like one of the game's own top-level disc folders, or Music, is part of those loose files; a
   folder with a `map.ini` is a map, not a mod;
2. then the collection's mods that name the game.

The game gets these folders as `ZX_MODS_DIR` (separated by `;` on Windows and `:` elsewhere; the developer runner
takes one folder there), and the mods' texture folders as `ZX_TEXTURE_PACKS` (lowest first, unset with none). The
log lists each mod (`[mods] ai: Mods/hd.zip 1.2 read`, or why not) and, where two have the same file, song,
tuning.ini or texture, which one the game reads (`[mods] ai: Mods/b over Mods/a: 3 things (ASSETS/X.ZAL, ...)`);
`[disc] files from ..., mods from ...` lists the folders.

The collection menu's Mods page (Settings > Mods) lists every mod, numbered in the order, each with its state. A
mod's page has its switch in each game it is for, Move Up and Move Down, what it wins and loses, and why a game does
not read it. The page reads the folders again each time it opens. A switch is kept in the game's `settings.ini` and
applies at the game's next start; the order is kept in the collection's root `settings.ini`:

| settings.ini | What it does |
|---|---|
| `[mods] <key> = off` (the game's file) | the mod is not read by this game; the key is the manifest's id, or for a mod without one its folder's name as it is (the case kept; a name with `=` in it ends at the last one); `= on`, or no line, is on |
| `[mod order] <n> = <place>` (the root file) | the order, first read first, each mod by where it is (`Mods/hd.zip`, `games/ai/Mods/10-park`); mods not named come after, each game's own folder's first, then the collection's, by name. None: that order alone |

A game does not read a mod while it is switched off there, while a mod it needs is missing or not read, while a mod
it conflicts with is read, or when its id is one a mod before it has (the page says which). A mod is read after the
mods it needs. The single-game app has no menu: its `settings.ini` takes the same `[mods]` lines.

## Custom Freeplay (Settings > Custom Freeplay)

A fourth tile on the collection menu, off by default (`[general] custom_freeplay` in the collection's root
`settings.ini`, or the Settings page's Custom Freeplay row; with it off, nothing changes). The tile opens two pages
in the menu's own look. First the player: the three games' tiles again, an inline skater from Aggressive Inline or a
BMX rider from Dave Mirra Freestyle BMX 2 or BMX XXX. Then a custom map for that game, from one list of every map,
each startable by every player. Enter on a map starts the player's game straight into free play on the map: the
game's own player select comes up first (its own skaters or riders), then the level loads with no more menus. The
player picks the game, so each game keeps its physics, tricks and camera; the mix is in the menu and in the content
(docs/CUSTOM-LEVELS.md section 1).

A custom map is a folder or zip in the collection's Mods folder (`<support>/Mods/<map>/`) or in a game's own Mods
folder (`<support>/games/<id>/Mods/<map>/`), or a folder in the maps folder (`<support>/Maps/<map>/`). It holds the
level files of the game it was made in, laid out as one of that game's slots has them (below), and a description: a
`[map]` section in its `mod.ini`, or a `map.ini`. The map page lists all three places, each map once, and reads them
again each time it opens. No start but its own reads a map: a folder with a `map.ini` in a game's Mods folder is not
a mod either, whatever its line in `[mods]` says.

```ini
[map]
name = Test Park        ; shown in the menu
game = ai               ; the game it was made in (ai, dm2, bmx): its level formats
                        ; (in a game's own Mods folder: that game when not named)
slot = TUT              ; that game's slot its files are laid out in (below)
slot.dm2 = COMMERCI     ; optional, per other game: the slot its player gets it converted into
                        ; (default: that game's first slot the setup can start)
park = park.dat         ; DM2's park slot only (slot = PARK): the saved park in the folder
preview = preview.png   ; a picture in the folder, optional (a PNG, a JPEG, or a BMP of 4, 8, 24 or 32 bits)
author = ...            ; optional
npcs = none             ; none (the default) | slot, below
```

**People.** A map's people, riders and traffic are those its own files place (its level objects, its scripts).
Where a game keeps a level's NPCs outside the level's folder, by the level's number (BMX XXX's NPC riders: the
executable's records, the rides in `assets/replay.zal`), a map leaves its slot's out, unless it says `npcs = slot`:
a map made from the slot's own level, which keeps its riders.

**Goals.** A map's goals are its own files': Aggressive Inline's level scripts (`<slot>T.BIN` is the one free skate
runs: the Estate's has its lessons), BMX XXX's `OBJCTV.TXT` (the tour reads it). They run in the map's own game as
the slot level's would, and what they mark done is not kept: the start puts the slot level's record in the player's
save back as it was (Aggressive Inline: the level's records, top speed and the rest, `ZX_AI_KEEP_LEVEL_RECORDS`;
Dave Mirra BMX 2's free ride sets none of the row's saved records, and `ZX_DM2_KEEP_LEVEL_RECORDS` puts them back
should one; BMX XXX: the row's level record and saved records, `ZX_BMX_KEEP_LEVEL_GOALS`, also in a save written
meanwhile). Dave Mirra BMX 2's goals are the executable's, for its own levels, and its FREERIDE runs none; its gaps
fire on the faces whose tag words the row's table names (a map's faces carry those its maker gave them: `#tag=` in
the converters, docs/CUSTOM-LEVELS.md). Goals are not translated from one game's system into another's: a map
converted for another game keeps what that game reads by name (the faces' and edges' tag words, the named points its
scripts or modes read), not its goals.

**Converting.** Each game lends its free play several slots (`collection/maps.h` `mapSlots`); a map in a slot its
own game lends starts for every player. The player of the game it was made in reads its folder as it is, in that
slot. Any other player's start gets it converted into its own game's level files for one of its slots (the one
`slot.<game>` names, else the game's first the setup can start). That takes a moment, and the page says so; the
conversion is held in memory for that start and read over the mods: nothing is written, and the map's folder is not
read then. A map in a slot its own game's setup cannot start (Aggressive Inline's Tutorial without the Estate) is
converted into that game's first slot that starts, with its own goals' files (its scripts and motions) under that
slot's names. A map that does not fit a game's budgets (too many parts drawn with no visibility zones, too many
texels, level files larger than the game's largest stock level's, which would not fit its memory) does not start;
once a start of it has failed, the list shows the converter's reason for that game until the app quits. The bottom
screen's map of the level is made at the start from the files the game reads (the map's, or its conversion, over the
disc), in memory, as the setup makes the stock levels' (with the goals of the map's own script or `OBJCTV.TXT`).

**The slots.** Each game's own start route (its devstart override) takes the game from the title to its own player
select, waits for the player's pick there, then picks the level and loads (`ZX_DEV_START_LEVEL`,
`ZX_DEV_START_SKATER=ask`):

- Aggressive Inline: `TUT`, the Tutorial's level 7 (`ASSETS/LEVELS/TUT`), reached through the Estate's free skate
  entry (the start gets `ZX_ESTATE=1` and `ZX_DEV_START_LEVEL=ESTATE`; `aggressive_inline_devstart.cpp`; the
  Estate made from the GameCube disc gives the entry), and `MOVIE`, the Movie Lot (`ASSETS/LEVELS/MOVIE`), free
  skate's first entry, open from the start (`ZX_DEV_START_LEVEL=0`): the slot without the GameCube disc. A map
  converted for Aggressive Inline goes to `TUT` when the setup has the Estate, else to `MOVIE`.
- Dave Mirra Freestyle BMX 2: the eight FREERIDE rows by their folders, `WOODWARD` (Woodward Camp, the first),
  `TRAINYAR`, `SWAMPYTR`, `COMMERCI`, `GALLOON`, `CLOVERLE`, `DEVILSPE`, `AIRPORT` (`ASSETS/LEVELS/<row>`, as
  `dm2_level_convert.py --level <row>` writes a level), reached as FREERIDE's level select values 0 to 7
  (`dave_mirra_devstart.cpp`: FREERIDE, the rider and bike choices the player's, the modifiers, the level). Only
  Woodward Camp is open in a fresh game: for the pick the row is opened in every character's record of the saved
  game in memory, and the bits are put back once the level has begun. A FREERIDE level has no NPCs and no goals; the
  rows differ in the far clip (the game's table: Woodward Camp 187 m, Commercial 331 m) and the tree fade (Swampy
  Trails' and Galloon's). Their level-point camera (the instructions, game over and after-replay screens) looks from
  the map's own camera points, not the row's in the executable's table: the start gets
  `ZX_DM2_LEVEL_CAMERA_POINTS=1`, DM2's option for a level whose scene has its own "CameraPos" idle points (off: the
  table, as on the disc; a level without its own keeps the table either way). And `PARK`, FREERIDE's level 8: a
  park saved in DM2's park editor (`park = park.dat`, a park stream; with the theme's files, `ASSETS/EDITOR/
  <theme>/`, when the map changes them), loaded by the game's own park code (the start gets `ZX_DM2_PARK_FILE`,
  `dave_mirra_saves.cpp`; `ZX_DEV_START_LEVEL=PARK`). A park plays in DM2 only, and no map is converted into the
  park slot (a level there also holds its theme's editor objects).
- BMX XXX: the eight HARDCORE TOUR rows by their folders, `CHL_MULY` (The Bronx, the select's first row),
  `CHL_DAM`, `CHL_MNCO`, `CHL_NASA`, `CHL_SHEP`, `CHL_SYRA`, `CMP_RMPG`, `CMP_ROOT` (`ASSETS/LEVELS/<row>`), each
  reached as its row (`bmx_xxx_devstart.cpp`: the rider select the player's, the row picked; the level is opened in
  the saved game in memory for the pick and the bits put back once the level has begun, so a save made after keeps
  the player's own progress). A challenge row has no clock: the ride lasts as long as the player likes; a
  competition row (`CMP_*`) is a two-minute run. The tour reads the slot's `OBJCTV.TXT` (the map's own goals; a
  converted map's has none) and counts and marks its goals into the player's record of the row's level: the start
  keeps that record (the counts, the flags, the marks of the goals done) and puts it back on the screens a ride ends
  on (game over, where it saves) and in the menus (`ZX_BMX_KEEP_LEVEL_GOALS`). A row's NPC riders are the level
  number's, not its folder's: the start gets `ZX_BMX_LEVEL_NPCS=0` (`bmx_xxx_npcs.cpp`: the game sets up no NPC
  riders, as in multiplayer), or `1` with `npcs = slot` (a map made in BMX XXX, started by BMX XXX's player).

When a game starts into a map, the map's files are read last of the mods (they win), the other maps are left out,
and the mods are read as the Mods page has them. Custom maps are the player's files: nothing is bundled with the app.

Developer runs: `ZX_MENU_SCRIPT` drives the pages like the others (the tile is the fourth, then Enter, the player's
tile, Enter, the map's row, Enter); `--game <id> --map <folder>` (or `ZX_APP_GAME`, `ZX_APP_MAP`) starts the listed
map of that folder name for that game's player, with no menu first, converted as the map page does it;
`ZX_DEV_START_SKATER=<n>` in the environment wins over `ask` for an unattended run. `[app] custom freeplay: ...` in
the log names the map, the game it was made in, its slot, the start, the mods the game was given, for a converted
map the conversion's time and the files held in memory, and the bottom screen's map made for it. `zx-map-convert` (a
build tool) writes a conversion to a folder.

## Picture quality (GPU renderer)

Seven Graphics rows, each in settings.ini `[graphics]`, all on by default and applied at once (they dim under the
CPU renderer). An eighth, Depth Test (`depth_test = exact|fast`, fast by default on Android, next start), lets the
GPU keep 16-bit depth targets: faster on phones, with a few pixels different. They change only how the GPU renderer
draws; the game itself runs exactly the same. The CPU renderer and the strict render path ignore them.

| Row | settings.ini | Values (default first) | What it does |
|---|---|---|---|
| Anti-Aliasing | `anti_aliasing` | fxaa, off | Aggressive Inline: smooths jagged edges of the 3D picture (FXAA, before the HUD is drawn, so text stays sharp). All three games: a window smaller than the picture is filled from a filtered half-size copy |
| Texture Filtering | `texture_filtering` | 16x, 8x, 4x, 2x, off | Anisotropic filtering: ground and walls at a grazing angle stay sharp |
| Fog | `fog` | smooth, original | Aggressive Inline: per-pixel fog instead of the PS2's eight fog bands (Dave Mirra BMX 2 and BMX XXX keep their own fog) |
| Shadows | `shadows` | sharp, original | The shadow drawn in one piece, as the CPU renderer draws it (original: on the GPU, Aggressive Inline's skater shadow can come out in pale fragments with its edges showing) |
| Depth Precision | `depth_precision` | high, original | Far surfaces stop flickering through each other (the PS2's 16-bit depth, one step 49 units at 2000 with Draw Distance Farthest) |
| Perspective Correction | `perspective_correct` | true, false | Shading without the PS2's screen-space warping (and its jump in brightness where the ground meets the camera), character textures without affine warping |
| Character Lighting | `character_lighting` | xbox, ps2 | Aggressive Inline and BMX XXX: skaters, riders and people lit by the Xbox release's lights (brighter, from other angles: the Ape's fur shows as on the Xbox) instead of the PS2's. Dave Mirra BMX 2's riders keep the PS2's lights |

Always on the GPU renderer: surfaces crossing the near plane are drawn whole, where the PS2's clipping program
splits them and clamps their corners to the GS window, which bends their textures at the bottom of the screen
(Aggressive Inline's Industrial grating floor; the CPU renderer draws the PS2's triangles).

Developer runner and tests: `ZX_GS_AA`, `ZX_GS_ANISO`, `ZX_NATIVE_FOG`, `ZX_NATIVE_SHADOWS`, `ZX_GS_PRECISE_DEPTH`,
`ZX_NATIVE_PERSPECTIVE`, `ZX_NATIVE_LIGHTING` (a value name or its index); on ZX_CONTROL `set
display.anti_aliasing 0` and so on. The design is in `docs/GPU_BACKEND.md` ("Picture quality beyond
the PS2").
