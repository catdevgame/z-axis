# Aggressive Inline (PS2, SLUS-20327) on macOS and Windows

Personal project to run Aggressive Inline natively on Apple Silicon, using a
disc and BIOS dumped from hardware I own. No game data is stored in this repo.

## Layout

AI's workspace in z-axis's `tooling/` (the runtime, the readable C++ and the guides are z-axis's: `runtime/`,
`docs/BRINGUP.md`, `docs/READABLE.md`, `docs/DEVTOOLS.md`; per-function facts are work database notes,
`workdb.py notes ADDR`).

| Path | What it is |
|---|---|
| `decomp/` | The game's source: matching decompilation (splat + Sony ee-gcc 2.9-ee-991111, `-O2 -G16`). Rebuilds the retail ELF byte for byte. See `decomp/README.md`. |
| `recomp/` | Recompiler config and Ghidra function map, `run.sh`; generated code, disc copy, logs and dumps are links into `$AI_DATA` (never committed). |
| `ghidra/` | Headless Ghidra pipeline (Emotion Engine extension) that produces the function map, plus reports. |
| `tools/` | Packaging (`make-app.sh`, `make-windows.ps1`, `build-ffmpeg.sh`), run and check drivers (`run-game.sh`, `logiccheck.py`, `routes.py`, `playbot/` ...; z-axis docs/BRINGUP.md "Checks"), the PCSX2 tools (`pcsx2pad.py`, `tools/PCSX2PAD.md`; the reference runs, `tools/REFERENCE-RUN.md`), disc readers (`gc_disc.py`, `ps2mc.py`) and `share/zaxis_texdump.py` (texture dump and replace for the three Z-Axis games). |
| `baseline/` | A link to `$AI_DATA/baseline`: `Aggressive Inline.app`, the disc in PCSX2 with its own config. The reference for correct behaviour. |
| `mods/` | PCSX2 patches for the disc (`mods/pcsx2`). |
| `reference/` | A link: screenshots from PCSX2 and the native build, for comparison. |

Controls (keyboard, mouse, gamepad; rebinding via `controls.ini`): z-axis `CONTROLS.md`.

Inputs expected outside the repo: `$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso`
and `scph39001.bin`.

## The Mac app (`Aggressive Inline.app`)

A double-clickable native build for your own use. It is standalone: the
game's files are extracted from your disc image when the app is built and
go into the bundle (`Contents/Resources/disc`), so playing needs no disc
image and nothing to choose. Because the bundle holds game data, do not
share it; `dist/` is ignored by git and no game data is ever committed.

### Build it

Needs Xcode command line tools, CMake and Ninja, plus this repo's generated
code (`recomp/generated`, see `recomp/BRINGUP.md`), the ELF it was
recompiled from (`recomp/config.game.toml`'s `input`) and your disc image
(`$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso`, or `ISO=`).
The first build downloads FFmpeg's source (network needed once; see below).
Optional inputs, each read only and turned into ready-made files in the
bundle: the Xbox release (`XBOX_ISO`: HD textures, movies, the Zoo and the
Ape), Dave Mirra BMX 2 and its Xbox release (`DM2_ISO`, `DM2_XBOX_ISO`: its
levels in Free Skate), the GameCube release (`GC_IMAGE`: Estate), and
`GAMES="ai dm2"` for the Z-Axis Collection (both games in one app); the
header of `tools/make-app.sh` lists them all, with their defaults.

```sh
tools/make-app.sh            # release build + bundle -> dist/Aggressive Inline.app
SKIP_BUILD=1 tools/make-app.sh   # re-bundle the existing build
DIST=$PWD/dist-test tools/make-app.sh   # write somewhere else (dist-*/ is ignored)
ISO=/Volumes/x/ai.iso tools/make-app.sh  # another copy of the disc image
```

The disc image is only read (never changed or copied into the repo). Its
files are extracted with `bsdtar` into `dist/.disc-cache` (about 960 MB,
redone only when the image's size or date changes) and the script checks
that the disc boots the exact ELF the code was recompiled from (SHA-256),
else it stops. The files are cloned into the bundle (APFS clones: no extra
disk space or copy time).

The script configures `out/build-release` in the z-axis checkout (Release: -O3 and LTO;
the verbose logs and RPC traces compiled out; debug UI compiled in but
hidden) with the app front end (`PS2X_APP_NAME`), builds the SHA-256 of the
recompiled ELF into it, links FFmpeg statically, makes the icon from the
local `baseline/icon/square.png` if present (nothing is added to git),
writes `Info.plist` and signs ad hoc. `dist/` is ignored by git.

The app is a single arm64 executable (about 70 MB, no bundled dylibs, only
system frameworks) plus the game files (about 960 MB) and needs **macOS 11.0** (Big Sur, the first macOS on
Apple Silicon). Everything, SDL3 included (window, input, audio and
SDL_GPU presentation on Metal), is built with
`CMAKE_OSX_DEPLOYMENT_TARGET=11.0` (`MIN_MACOS=` to change it), and the
script reports the highest `LC_BUILD_VERSION` minos in the bundle as
`LSMinimumSystemVersion`.

#### FFmpeg

The runtime uses FFmpeg to decode the MPEG-2 video of the PSS movies
(intros, menu FMV window, level fly-ins): libavcodec's `mpeg2video` decoder
and `mpegvideo` parser, libswscale for YUV to RGBA, and libavutil; and for
a custom soundtrack (see Mods) libavformat with the file protocol, the
Ogg, FLAC and MP3 demuxers and the Vorbis, FLAC and MP3 decoders.
Movie sound and the game's own sound are not FFmpeg's: the movies' PCM and
the game's PS-ADPCM are read and decoded by the runtime itself.
`tools/build-ffmpeg.sh` downloads the pinned release (FFmpeg 9.0.2, checked
by SHA-256), configures exactly that (`--disable-everything`, no programs,
no network, no autodetected or external libraries, no avdevice,
avfilter or swresample, LGPL), builds static libraries for the deployment
target (about 3.4 MB of `.a`, in about 30 s) and prints the install prefix.
The tarball and the build are cached in `third_party-cache/` (ignored by
git); a changed version or configure line builds into a new directory.
It runs on Linux unchanged; on Windows it is meant for an MSYS2 shell with
`EXTRA_CONFIGURE=--toolchain=msvc`.

PS2Recomp takes the prefix as `-DPS2X_FFMPEG_PREFIX=<prefix>` and then
searches only there; empty (the default) uses the system FFmpeg through
pkg-config, e.g. Homebrew's. `make-app.sh` uses the static build
(`FFMPEG=system` falls back to Homebrew's, whose dylibs are copied into
`Contents/Frameworks` and raise the minimum macOS to theirs, 26.0 with the
current bottles). The developer build (`out/build-ai`, `recomp/run.sh`) can
use either:

```sh
cmake ... -DPS2X_FFMPEG_PREFIX=$(tools/build-ffmpeg.sh) -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
```

Homebrew ships the same FFmpeg version (9.0.2), so both should decode the
movies the same; the static build was checked by playing the intros, the
menu FMV window and the Tutorial fly-in, not frame by frame.

### Set it up

1. Open `Aggressive Inline.app` (it is signed ad hoc: the first time,
   right-click it and choose Open, or allow it in System Settings, Privacy &
   Security). No disc image and no BIOS file are needed (the PS2 BIOS and
   IOP are emulated at a high level).
2. If PCSX2 has an Aggressive Inline save in its memory card folder
   (`~/Library/Application Support/PCSX2/memcards`), the app offers once to
   continue from it (it is copied to `Saves/Career.sav`).

The game reads its files straight from `Contents/Resources/disc` with
ordinary file reads: the game's own file functions (Z-Axis' CD/DVD client)
and its movie player are replaced by native code, so no disc drive or IOP
disc module is emulated for them and every read completes at once. The
game's sound driver (Z-Axis' ZAUDIO on the PS2's sound processor) is native
too: sample banks decoded when loaded, voices, streamed music and speech
read through the same file layer, mixed at 48 kHz. No PlayStation sound
chip and no IOP processor run at all.
Movies are decoded natively and their sound is mixed into the game's. At start the app
checks the boot ELF there against the SHA-256 it was built with and says
so if the files are missing or different (build the app again).

Where things are:

| What | Where |
|---|---|
| Settings (window, scaling, widescreen, renderer, vsync, frame rate, volume, pause and mute in background, skip intro movies, log file) | `~/Library/Application Support/Aggressive Inline/settings.ini` |
| Controls | `controls.ini` in the same folder (see `CONTROLS.md`) |
| Camera, Assists and Cheats settings | `gameplay.ini` in the same folder |
| Screenshots (F12) | `~/Pictures/Aggressive Inline` |
| Saves (career, parks; see below) | `Saves` in the same folder |
| Memory cards of earlier versions (imported once, left as they are) | `memcards/slot1`, `memcards/slot2` in the same folder |
| Mods (loose replacement files for the game's own, see below) | `Mods` in the same folder |
| Log (this run and the previous one) | `~/Library/Logs/Aggressive Inline/aggressive-inline.log` |

File > Show Settings Folder / Show Logs Folder open them. Edit
`settings.ini` while the game is not running (an `image =` line from
earlier versions is ignored).

Settings screen: the game's own menus have the PC settings, drawn by the
game in its style. In the front end, Options > Settings (the controller
icon); in a level, the pause menu's Settings item (Start, then Settings, on
a gamepad too; also in the Tutorial's lesson list and the split screen's
pause menu); Cmd+, or the app menu's Settings... opens it from a level
(Tutorial and split screen too), the main menu, Options, or player and
level select (it goes back to the main menu, then to Options > Settings).
L1/R1 (Q/E) switch between Display, Sound, Controls, Camera, Assists,
Cheats and Game; up/down picks
a row, left/right changes it, Cross changes a toggle or rebinds, Triangle
goes back. Changes apply at once and are saved (settings.ini, controls.ini)
except Skip Intro Movies, which applies at the next start. Display: window,
full screen or borderless full screen, renderer and GPU resolution, full
height (GPU), widescreen and its aspect, motion blur, draw distance,
scaling, vsync, frame rate counter (the only thing drawn over the game),
pause in background. Sound: master volume, mute in background,
movie volume, and music, effects and speech volume (these three are the
game's own Options > Sound volumes, kept in its save: both pages show and
change the same values).
Controls: see `CONTROLS.md` (also Button Prompts: Auto, or Keyboard, Xbox,
PlayStation or Nintendo always, for Steam Input or a remapper).
Camera: Field of View (50-110, the game's 75), Camera Distance and Camera
Height (50-250 % and 50-300 % of the game's; grinds and poles follow). They
apply at once, also in a level, and add to widescreen. Assists (off by
default): Out of Juice (Run Ends, or Keep Skating: no game over when the
juice meter empties in Career), Juice Drain and Juice Lost on Crash (100 %
down to 0 %). Cheats: the game's own seven cheats as switches (Low Gravity
Wall Ride, Juice Regeneration, Invulnerability, Perfect Grind, Perfect
Handplant, Super Spin, Perfect Manual), doing what their codes do but kept
in `gameplay.ini`, not in your career save, and Unlock Everything (below).
None of these change the game until you change them.
The View menu items show and change the same
settings. How it is done: PS2Recomp `SETTINGS.md`. The developer runner
(`recomp/run.sh`, the check tools) leaves the game's menus as they are
unless `PS2X_GAME_SETTINGS_MENUS=1` (the added widgets move the game's
heap, which the logic and replay checks compare).

Button prompts follow the device you last used: the game's footers ("X
Select"), the Tutorial's tips and the trick lists show keyboard keys and
the mouse (as bound in `controls.ini`, so a rebind shows at once), Xbox
buttons (A, B, X, Y from the disc's own Xbox button pictures, LB, RB, ...)
for an Xbox-style or unknown gamepad, Nintendo labels for a Switch pad, or
the PlayStation buttons for a PlayStation pad (following `[gamepad]`
remaps in `controls.ini`); pressing a key or a gamepad
button switches them. The wording follows too ("Holding Space prepares
you to jump", "D-pad", "left stick", "Press any key"). Nothing of the
game's art is stored: the key caps are drawn at run time in the game's own
font, read from your disc.

Level Select, Exit Game and Quit to Desktop in the pause menus ask "Are
you sure?" first, as the Xbox release does.

Quit to Desktop: Escape or Back (right click, Triangle) on the main menu
asks "Quit to Desktop?"; the pause menus (also the Tutorial's) have a Quit
to Desktop item. Both quit like Cmd+Q, so the career is saved. In menus
the mouse wheel moves the selection, a left click selects and a right
click goes back (pointing at items does nothing: the game's menu engine has
no pointer support).

Restart, Hide HUD, Screenshot: Backspace (or both sticks pressed in on a
gamepad, L3+R3) restarts while you skate: Career and Freeskate load the
level again (as leaving and starting it again does, so career progress is
kept and saved), a Timed Run starts over in place (the game's own Play
Again). The pause menus have Restart Level (Career, Freeskate) and Restart
Run (Timed Run) after Resume Game. F9 hides the score, juice meter, trick
list, balance meters and messages until pressed again (or until you leave
the level). F12 saves the picture as a PNG at the renderer's resolution to
`~/Pictures/Aggressive Inline` ("Screenshot saved" shows top left for a
moment). The keys are rebindable in Settings > Controls (Restart Level,
Hide HUD, Screenshot, Photo Mode) and in `controls.ini` `[hotkeys]`.

Photo mode: Photo Mode in the pause menu, or P anywhere in a level, stops
the game and hides the HUD and menus; fly a free camera (mouse and W A S D,
or the sticks), roll it, zoom, pick a filter (Letterbox, Vignette, Black &
White, Cinema, Noir) and save the picture with F12 (or Cross / A) at full
resolution. Esc (Circle / B) puts the game back as it was. CONTROLS.md has
all the keys.

Unlock Everything (Settings > Cheats): right twice (the first press asks
"Sure? Again") unlocks what the game's two unlock codes do plus the Media
Viewer cutscenes no code unlocks: every level in every mode, the secret
skaters and park editor pieces, and every skater's levels. This changes
your career save (the unlock word at 0x24CBB8, bits 0-19 and 48-54, and
each skater record's level mask); it is written with the career at the
next autosave (level end, quit) or save, and cannot be undone except by
keeping a copy of `Saves`.

Portable mode: an empty `portable.txt` next to `Aggressive Inline.app`
keeps settings, controls, saves, mods, logs and screenshots in an
`Aggressive Inline Data` folder beside it instead of `~/Library` and
`~/Pictures`.

Window: the 4:3 picture is fitted to the window with black bars. View >
Enter Full Screen (Cmd+Ctrl+F, also Cmd+F and Alt+Enter); View > Integer
Scaling (whole multiples of 640x480, sharp pixels) and View > Pause in
Background (on by default) are saved to `settings.ini`. Cmd+Q quits and
saves the window state.

Renderer: View > Renderer picks the GPU renderer (Metal) at native, 2x, 3x
or 4x the internal resolution (the default: GPU at 3x, a sharper picture,
close to but not exactly the CPU renderer's) or the CPU renderer (accurate,
native 640x448; used by itself when there is no GPU device). It switches at
once and is saved under `[graphics]` in `settings.ini` (`renderer =
cpu|gpu`, `internal_resolution = 1-4`); a settings file saved by an earlier
version keeps the renderer it names. The developer runner and the test
tools (`recomp/run.sh`, `nctl.py`, `logiccheck.py`, ...) use the CPU
renderer unless `PS2X_GS_BACKEND=gpu` is given.

Full height (GPU), on by default (`full_height` under `[graphics]`): the
game draws its frames 224 lines tall (640x224 in the menus, 512x224 in
levels; the PS2 showed each line twice), so "3x" used to be 3x of 224
lines. With this setting the GPU renderer draws them at twice its
resolution vertically (3x: 1536x1344 in a level instead of 1536x672), as
sharp vertically as horizontally. Developer runner: `PS2X_GS_FULL_HEIGHT=1`.

Motion blur (`motion_blur = off|low|original`, default original): in levels
the game blends a quarter of the previous frame into each new one (its
tuning value "Normal Blur"); Low halves it, Off turns it off. Switches at
once; split screen has none. Runner: `PS2X_MOTION_BLUR=0|1|2`.

Draw distance (`draw_distance = original|far|farthest`, default far): how
far the view reaches before the fog. Split screen ends it at 100 units
(Museum 150) with fog from 65 to 90, a tenth of single player; Far makes
that 250 (fog 162-225), Farthest 1000 like single player (fog 650-900).
Single player is at the game's maximum (1000) already and stays as it is.
Switches at once; 60 frames per second either way (split screen, GPU 3x
full height: the VU1 worker goes from 11 to about 13 ms per vsync). Runner:
`PS2X_DRAW_DISTANCE=0|1|2` (default 0, the game's own).

Widescreen: View > Widescreen, off by default (`widescreen = true`
under `[display]` in `settings.ini`), switches at once. Its shape is the
Wide Aspect setting (`aspect = 16:9|16:10|21:9|32:9|window`, default 16:9;
window follows the window's shape between 4:3 and 32:9). In a level the
game draws a wider view (more at the sides, same height) and the picture is
stretched to that aspect; the HUD, in-level menus and message boxes keep their 4:3
shape (unlike in PCSX2), with the HUD at the sides of the wide picture. The
front end, loading screens and movies stay 4:3.
It does what the PCSX2 "Widescreen 16:9" patch for this disc does (by
BloodRaynare, based on the PAL patch by ElHecht; `tools/pcsx2pad.pnach`),
as run-time hooks in the game override instead of code patches.

Saves: there is no memory card. Save and load (Options > Save & Load, the
pause menu's Save, the park editor) write and read plain files at once, with
no card checks, slot choice or overwrite questions, and the game loads the
career when you press START at the title. It also saves the career by itself
when a career level ends and when you quit (if a save exists). Files, in
`Saves`:

| File | What |
|---|---|
| `Career.sav` | the career (skaters, records, unlocks, cheats, volumes), 459780 bytes |
| `Parks/Park 000.sav`, ... | park editor parks, 8196 bytes each |
| `*.bak` | the previous version of each, kept on every save (rename it to go back) |
| `imported.txt` | what was copied from `memcards/slot1` and `slot2` the first time (they are not changed) |

Each is written to a temporary file and renamed over the old one, so a crash
cannot leave half a save; a damaged file is skipped for its `.bak`. The files
hold exactly the bytes the game writes to a memory card (the card file
`BASLUS-20327CEAIGAME/BASLUS-20327CEAIGAME`, a u32 byte sum then the data), so
moving a save between here and PCSX2 is a copy: `tools/ps2mc.py extract
CARD.ps2 BASLUS-20327CEAIGAME /tmp/x` and copy `/tmp/x/BASLUS-20327CEAIGAME/BASLUS-20327CEAIGAME`
to `Saves/Career.sav`, or `ps2mc.py import` the other way (with the card
folder layout). Profiles: `profile = Career` under `[saves]` in
`settings.ini` names the career file; another name keeps a separate career
(`Saves/<name>.sav`); parks are shared. `PS2X_SAVES=card` brings back the
memory card (HLE card folders, the console's dialogs) for comparisons with
PCSX2.

Mods: a file in `Mods` with the path it has on the disc (for example
`Mods/ASSETS/STRING.EUS`, names in any case) is used instead of the disc's
file; the disc folder in the bundle is never changed. Every file the game
reads goes through this (levels, textures, menus and their scripts, sound
banks, streamed music and speech, movies). Custom soundtrack: a file
`Mods/Music/NN<anything>.ogg` (or `.wav`, `.flac`, `.mp3`) plays
instead of song NN of the game's track list (01 falling for you,
02 Crawling in the Dark, 03 Track 13, 04 Don't Sweat the Technique, 05 Wrong
Way, 06 An Idea for a Movie, 07 Your Disease, 08 Passing me by, 09 Sellout,
10 Monday Morning, 11 Youth of the Nation, 12 I'm a Cloud, 13 The choice is
yours), e.g. `Mods/Music/01 My Song.ogg`; the game goes on to the next song
when it ends. The pause menu's playlist shows the file's title and artist
tags, else its name without the number ("My Song"). The settings screen's changes to the menu scripts are made on top of
whichever `UILIB.ZAL` is used. Restart the game after adding or changing a
mod.

Tuning (modders): `Mods/tuning.ini` sets entries of the game's developer
tuning table (camera, physics, juice, trick worth; 405 named values) when the
game starts, e.g. `[Camera]` then `Distance = 6`. Any change alters how the
game plays. Settings > Game > Developer Menu (`developer_menu = true` under
`[general]`, next start) makes F1 in a level open the game's hidden debug
menu (a live tuning table editor, a free camera, respawn, debug text) and
writes `Mods/tuning-reference.ini` with every entry, its value and range.
Language (`language = us|uk|french|german`, next start) loads the disc's
other string table; it only holds the memory card texts, so the game stays
English. Details in `recomp/DEVTOOLS.md`; runner: `PS2X_DEV_MENU=1`,
`PS2X_TUNING_INI=<file>`, `PS2X_LANGUAGE=<name>`.

For automated runs the app takes `PS2X_APP_SUPPORT_DIR` (a scratch folder
instead of the support folder; logs go to its `Logs`, screenshots to its
`Screenshots`), `PS2X_APP_LOG_DIR`, `PS2X_GAMEPLAY_INI`,
`PS2X_SCREENSHOT_DIR`,
`PS2X_SAVES_DIR` and `PS2X_SAVE_PROFILE` (the saves folder and profile; the
developer runner uses `<card folder>-saves` next to `PS2X_MC_ROOT`),
`PS2X_SAVES=card`,
`PS2X_MODS_DIR` (another mods folder), `PS2X_DISC=cdvd` (the game's
files through the emulated IOP disc module and drive, as before),
`PS2X_MOVIES=hle` (the movies through the emulated MPEG decoder and the
IOP sound path, as before), `PS2X_SOUND=spu2` (the game's sound driver on
the emulated IOP and sound chip, as before), `PS2X_IOP=1` (run the idle
IOP anyway),
`PS2X_APP_DISC_DIR` (another folder of the disc's files; exit code 2 if
the game files are missing or different),
`PS2X_APP_IMPORT_CARD` (import a PCSX2 card's save without asking) and
`PS2X_WIDESCREEN=1`/`0` (overrides the setting; the developer runner too). The
developer switches (`PS2X_PAD_SCRIPT`, `PS2X_SHOT_AT`, `PS2X_DEBUG_UI=1`,
`PS2X_PERF_LOG=<seconds>`, ...) work in the app too; with a pad script the
game is never paused for lack of focus (nor under `PS2X_CONTROL`).

## The Windows app (`Aggressive Inline.exe`)

The same game for Windows 10/11 x64: a folder `dist\Aggressive Inline`
with `Aggressive Inline.exe` (no console window, about 48 MB, statically
linked: SDL3, FFmpeg, the C++ runtime; nothing to install), the game files
from your disc in `disc\` and a `README.txt`. Like the Mac app it holds
game data: do not share it (`dist\` is ignored by git).

### Build it (on Windows)

Needs Visual Studio 2022 or its Build Tools with the C++ workload, the
"C++ Clang compiler" (clang-cl) and "C++ CMake tools" components and a
Windows 10/11 SDK (its `dxc` compiles the GPU renderer's shaders); MSYS2
with `make`, `diffutils` and `nasm` for the FFmpeg build (a portable copy is
fine, `-Msys <folder>`); and, as on the Mac, `recomp\generated`,
`recomp\generated-vu1` and the PS2Recomp checkout in `PS2Recomp\`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\make-windows.ps1 -Iso "D:\Aggressive Inline (USA).iso"
... -DiscDir recomp\disc-root     # an extracted copy of the disc instead
... -SkipBuild                    # repackage the existing build
... -Zip                          # also dist\Aggressive Inline.zip
... -ProfileData x.profdata       # profile-guided optimization (below)
```

It mirrors `make-app.sh`: the disc image is extracted with Windows' own
`tar` into `dist\.disc-cache` (once per image) and its boot ELF must have
the SHA-256 of the ELF the code was recompiled from; FFmpeg comes from
`tools/build-ffmpeg.sh` run in MSYS2 with MSVC (`--toolchain=msvc`, static
CRT, the same minimal LGPL configuration); the runner is built with
clang-cl, lld-link and ThinLTO into `PS2Recomp\out\build-release-win`; the
icon is made from the local `baseline\icon\square.png` if present (PNG
images in an `.ico`, built into the `.exe`). The game files are hard links
to the cache (no extra space). A full build takes about 5 minutes on a
12-core Ryzen.

### Use it

Settings, controls, saves, mods and logs are in `%APPDATA%\Aggressive
Inline` (`settings.ini`, `controls.ini`, `gameplay.ini`, `Saves`, `Mods`,
`Logs`), the same files as on the Mac; screenshots (F12) in
`Pictures\Aggressive Inline`. A `portable.txt` next to the `.exe` keeps all
of it in `Aggressive Inline Data` beside it. There is no menu bar: Alt+Enter or F11 toggles full
screen, and the game's own Settings pages (Options > Settings, the pause
menu's Settings) have everything the Mac's View menu has. The GPU renderer
runs on Direct3D 12 (3x by default, as on the Mac); the CPU renderer is in
Settings. Gamepads: SDL3's (XInput, PlayStation, Switch and other HID pads).
The automation variables (`PS2X_APP_SUPPORT_DIR`, `PS2X_CONTROL`, ...) work
as on the Mac.

Speed: on a Ryzen 9 5900X with the GPU renderer the Tutorial, MOVIE LOT
and split screen (draw distance original) run at about 60 frames per
second; the VU1 thread needs 6-12 ms per frame there. Split screen at draw
distance far runs at about 32: its frames need about 20 ms of VU1 work and
as much GS work, one after the other. A profile-guided build is about 10%
faster: build PS2Recomp with
`-fprofile-instr-generate -DPS2X_PGO_GENERATE=1` (and
`clang_rt.profile-x86_64.lib` from VS's LLVM on the link line), play a few
levels with `LLVM_PROFILE_FILE=...\run-%p.profraw`, quit normally, merge
with `llvm-profdata merge -o x.profdata run-*.profraw` and pass
`-ProfileData x.profdata`.
