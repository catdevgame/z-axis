# Aggressive Inline (PS2, SLUS-20327)

Aggressive Inline's folder: its readable code (here at the top: `skater/`, `level/`, `script/`, ...; the engine it runs
on is `engine/zge`), the host code that hooks it (`overrides/`), its own tools and docs, and its workspace: the
recompiler config, the research pipelines and the build of Aggressive Inline alone as an app. The runtime and the
guides are the checkout's own (`ps2/`, `docs/BRINGUP.md`, `docs/READABLE.md`, `docs/DEVTOOLS.md`); per-function facts
are work database notes (`workdb.py notes ADDR`). No game data is stored here.

| Path | What it is |
|---|---|
| `overrides/` | The host code that hooks the game (`aggressive_inline_*`; the engine's own, `zge_*`, in `engine/overrides`). |
| `tools/` | Its own tools: the logic check and its PCSX2 reference runs (`logiccheck.py`, `lc-when-free.sh`, `careercheck.py`, `pcsx2pad.py`, `pcsx2replay.py`, `scripts/`), `routes.py`, the players (`playbot/ai.py` ...; the shared core is `tools/playbot`). The collection's run and check drivers are `tools/checks` (`run-game.sh`, `nctl.py`, `sweep.py`; docs/BRINGUP.md "Checks"); the packaging is `platform/mac/make-app.sh` and `platform/windows/make-windows.ps1`; the readers and makers (`gc_disc.py`, `ps2mc.py`, `share/zaxis_texdump.py`) are `tools/modding`. |
| `docs/` | The PCSX2 tools (`PCSX2PAD.md`) and reference runs (`REFERENCE-RUN.md`, `REFERENCE-RUN-2.md`), the Ghidra pipeline (`GHIDRA.md`). |
| `workspace/decomp/` | The retail ELF split into assembly by splat, with the original toolchain (Sony ee-gcc 2.9-ee-991111, `-O2 -G16 -fno-strict-aliasing`, its assembler) and PS2 binutils; rebuilds the ELF byte for byte. The listing and the binutils are used by the checks and the PCSX2 hooks. See `workspace/decomp/README.md`. |
| `workspace/recomp/` | Recompiler config and Ghidra function map, `run.sh`; generated code, disc copy, logs and dumps are links into `$AI_DATA` (never committed). |
| `workspace/ghidra/` | Headless Ghidra pipeline (Emotion Engine extension) that produces the function map, plus reports (`docs/GHIDRA.md`). |
| `workspace/baseline/` | A link to `$AI_DATA/baseline`: the disc in PCSX2 with its own config, the reference for correct behaviour. |
| `workspace/reference/` | A link: screenshots from PCSX2 and the native build, for comparison. |

Inputs expected outside the checkout: `$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso` and, for the
PCSX2 tools, `scph39001.bin`.

The collection app (all three games) is built as the top README's "Building" says. This file covers what is
particular to Aggressive Inline: the app of AI alone, its saves, and the variables for automated runs.
Playing, controls and settings: README "Playing", `docs/CONTROLS.md`, `docs/SETTINGS.md`, `docs/MODS.md`.

## The Mac app (`Aggressive Inline.app`)

`platform/mac/make-app.sh` with no `GAMES` builds Aggressive Inline alone; `GAMES="ai dm2 bmx"` builds the collection.
The header of `platform/mac/make-app.sh` lists every input, with its defaults. The paths below are the workspace's
(`games/aggressive/workspace`); the commands run from the checkout's root.

Needs Xcode command line tools, CMake and Ninja, the generated code (`recomp/generated`, made by
`tools/setup/recompile.sh`), the ELF it was recompiled from (`recomp/config.game.toml`'s `input`) and your disc image
(`$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso`, or `ISO=`). The first build downloads FFmpeg's source
(network needed once; see below). Optional inputs, each read only and turned into ready-made files in the bundle:
the Xbox release (`XBOX_ISO`: HD textures, movies, the Zoo and the Ape) and the GameCube release (`GC_IMAGE`:
Estate).

```sh
platform/mac/make-app.sh                       # release build + bundle -> dist/Aggressive Inline.app
SKIP_BUILD=1 platform/mac/make-app.sh          # re-bundle the existing build
DIST=$PWD/dist-test platform/mac/make-app.sh   # write somewhere else
ISO=/Volumes/x/ai.iso platform/mac/make-app.sh # another copy of the disc image
NO_GAME_DATA=1 platform/mac/make-app.sh        # no game data: the disc is set up at first start
```

The disc image is only read. Its files are extracted with `bsdtar` into `dist/.disc-cache` (about 960 MB, redone
only when the image's size or date changes) and the script checks that the disc boots the exact ELF the code was
recompiled from (SHA-256), else it stops. The files are cloned into the bundle (APFS clones: no extra disk space or
copy time). Because the bundle then holds game data, do not share it; `dist/` is ignored by git.

The script configures `out/build-release` in the checkout (Release: -O3 and LTO; the verbose logs and RPC traces
compiled out; debug UI compiled in but hidden) with the app front end (`ZX_APP_NAME`), builds the SHA-256 of the
recompiled ELF into it, links FFmpeg statically, makes the icon from the local `baseline/icon/square.png` if present,
writes `Info.plist` and signs ad hoc. The app is a single arm64 executable (only system frameworks) plus the game
files. Everything, SDL3 included, is built for `MIN_MACOS` (default 11.0; `PUBLIC=1`, the release's build, 15.0),
and the script reports the highest `LC_BUILD_VERSION` minos in the bundle as `LSMinimumSystemVersion`.

### FFmpeg

The runtime uses FFmpeg to decode the MPEG-2 video of the PSS movies (intros, menu FMV window, level fly-ins):
libavcodec's `mpeg2video` decoder and `mpegvideo` parser, libswscale for YUV to RGBA, and libavutil; for a custom
soundtrack libavformat with the file protocol, the Ogg, FLAC and MP3 demuxers and the Vorbis, FLAC and MP3 decoders;
and for the other releases' movies and songs the Bink, XMV and ASF demuxers and the Bink video and audio, WMV2, Xbox
ADPCM and WMA 2 decoders. The PSS movies' sound and the game's own sound are not FFmpeg's: the movies' PCM and the
game's PS-ADPCM are decoded by the runtime itself. `third_party/build-ffmpeg.sh` downloads the pinned release (checked by
SHA-256), configures exactly that (`--disable-everything`, no programs, no network, no autodetected or external
libraries, no avdevice, avfilter or swresample, LGPL), builds static libraries for the deployment target and prints
the install prefix. The tarball and
the build are cached in `third_party-cache/` (ignored by git); a changed version or configure line builds into a new
directory. It runs on Linux unchanged; on Windows it runs in an MSYS2 shell started from a Visual Studio x64 developer
environment and builds with MSVC (`--toolchain=msvc`, the static CRT) by itself.

The build takes the prefix as `-DZX_FFMPEG_PREFIX=<prefix>` and then searches only there; empty (the default) uses
the system FFmpeg through pkg-config, e.g. Homebrew's. `make-app.sh` uses the static build (`FFMPEG=system` falls
back to Homebrew's, whose dylibs are copied into `Contents/Frameworks` and raise the minimum macOS to theirs). The
developer build (`out/build-ai`, `recomp/run.sh`) can use either:

```sh
cmake ... -DZX_FFMPEG_PREFIX=$(third_party/build-ffmpeg.sh) -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
```

## Windows (`Aggressive Inline.exe`)

Aggressive Inline alone for Windows 10/11 x64: a folder `dist\Aggressive Inline` with `Aggressive Inline.exe` (no console
window, statically linked: SDL3, FFmpeg, the C++ runtime; nothing to install), the game files from your disc in
`disc\` and a `README.txt`. Like the Mac app it holds game data: do not share it (`dist\` is ignored by git).

Needs Visual Studio 2022 or its Build Tools with the C++ workload, the "C++ Clang compiler" (clang-cl) and "C++
CMake tools" components and a Windows 10/11 SDK (its `dxc` compiles the GPU renderer's shaders); MSYS2 with `make`,
`diffutils` and `nasm` for the FFmpeg build (a portable copy is fine, `-Msys <folder>`); and this workspace's
`recomp\generated` and `recomp\generated-vu1`. The header of `platform\windows\make-windows.ps1` lists every option.

```powershell
powershell -ExecutionPolicy Bypass -File platform\windows\make-windows.ps1 -Iso "D:\Aggressive Inline (USA).iso"
... -DiscDir recomp\disc-root     # an extracted copy of the disc instead
... -SkipBuild                    # repackage the existing build
... -Zip                          # also dist\Aggressive Inline.zip
... -ProfileData x.profdata       # profile-guided optimization (below)
```

It mirrors `make-app.sh`: the disc image is extracted with Windows' own `tar` into `dist\.disc-cache` (once per
image) and its boot ELF must have the SHA-256 of the ELF the code was recompiled from; FFmpeg comes from
`third_party/build-ffmpeg.sh` run in MSYS2 with MSVC (`--toolchain=msvc`, static CRT, the same minimal LGPL
configuration); the runner is built with clang-cl, lld-link and ThinLTO into `out\build-release-win` in the
checkout; the icon is made from the local `baseline\icon\square.png` if present (PNG images in an `.ico`, built into
the `.exe`). The game files are hard links to the cache (no extra space). The GPU renderer runs on Direct3D 12.

Profile-guided build (about 10% faster): build with `-fprofile-instr-generate -DZX_PGO_GENERATE=1` (and
`clang_rt.profile-x86_64.lib` from VS's LLVM on the link line), play a few levels with
`LLVM_PROFILE_FILE=...\run-%p.profraw`, quit normally, merge with
`llvm-profdata merge -o x.profdata run-*.profraw` and pass `-ProfileData x.profdata`.

## Where the app of this game alone keeps things

The collection keeps each game's files in `<support>/games/<id>` (`games/ai` for AI; README "Your discs"). The app
of Aggressive Inline alone keeps the same files in its own support folder:

| What | Where |
|---|---|
| Settings, controls, gameplay options | `settings.ini`, `controls.ini`, `gameplay.ini` in `~/Library/Application Support/Aggressive Inline` (Windows: `%APPDATA%\Aggressive Inline`) |
| Saves (career, parks) | `Saves` in the same folder |
| Memory cards from older versions of the app (imported once, left as they are) | `memcards/slot1`, `memcards/slot2` in the same folder |
| Mods | `Mods` in the same folder (`docs/MODS.md`) |
| Log (this run and the previous one) | `~/Library/Logs/Aggressive Inline/aggressive-inline.log` (Windows: `Logs` in the support folder) |
| Screenshots (F12) | `~/Pictures/Aggressive Inline` (Windows: `Pictures\Aggressive Inline`) |

Portable mode: an empty `portable.txt` next to the app keeps all of it in an `Aggressive Inline Data` folder beside
it (a collection app: `Z-Axis Collection Data`).

If PCSX2 has an Aggressive Inline save in its memory card folder, the app offers once to continue from it (it is
copied to `Saves/Career.sav`).

## Saves

Aggressive Inline's saves, in either app. There is no memory card. Save and load write and read plain files at
once, with no card checks, slot choice or overwrite questions. AI loads the career when you press START at the
title, and saves it by itself when a career level ends and when you quit (if a save exists). Files, in `Saves`:

| File | What |
|---|---|
| `Career.sav` | the career (skaters, records, unlocks, cheats, volumes), 459780 bytes |
| `Parks/Park 000.sav`, ... | park editor parks, 8196 bytes each |
| `*.bak` | the previous version of each, kept on every save (rename it to go back) |
| `imported.txt` | what was copied from `memcards/slot1` and `slot2` the first time (they are not changed) |

Each is written to a temporary file and renamed over the old one, so a crash cannot leave half a save; a damaged
file is skipped for its `.bak`. The files hold exactly the bytes AI writes to a memory card (the card file
`BASLUS-20327CEAIGAME/BASLUS-20327CEAIGAME`, a u32 byte sum then the data), so moving a save between here and PCSX2
is a copy: `tools/modding/ps2mc.py extract CARD.ps2 BASLUS-20327CEAIGAME OUTDIR` and copy
`OUTDIR/BASLUS-20327CEAIGAME/BASLUS-20327CEAIGAME` to `Saves/Career.sav`, or `ps2mc.py import` the other way (with
the card folder layout). Profiles: `profile = Career` under `[saves]` in `settings.ini` names the career file;
another name keeps a separate career (`Saves/<name>.sav`); parks are shared. `ZX_SAVES=card` brings back the
memory card (HLE card folders, the console's dialogs) for comparisons with PCSX2.

## Automated runs

Both apps (AI alone and the collection; both are built from `collection/app.cpp`) take `ZX_APP_SUPPORT_DIR` (a scratch folder instead of the support folder; logs go to its `Logs`,
screenshots to its `Screenshots`), `ZX_APP_LOG_DIR`, `ZX_GAMEPLAY_INI`, `ZX_SCREENSHOT_DIR`, `ZX_SAVES_DIR`
and `ZX_SAVE_PROFILE` (the saves folder and profile; the developer runner uses `<card folder>-saves` next to
`ZX_MC_ROOT`), `ZX_SAVES=card`, `ZX_MODS_DIR` (another mods folder), `ZX_DISC=ps2` (the game's files
through the emulated IOP disc module and drive), `ZX_MOVIES=hle` (the movies through the emulated MPEG decoder and
the IOP sound path), `ZX_SOUND=spu2` (the game's sound driver on the emulated IOP and sound chip), `ZX_IOP=1`
(run the idle IOP anyway), `ZX_APP_DISC_DIR` (another folder of the disc's files; exit code 2 if the game files
are missing or different), `ZX_APP_IMPORT_CARD` (import a PCSX2 card's save without asking) and
`ZX_WIDESCREEN=1`/`0` (overrides the setting; the developer runner too). The developer switches
(`ZX_PAD_SCRIPT`, `ZX_SHOT_AT`, `ZX_DEBUG_UI=1`, `ZX_PERF_LOG=<seconds>`, ...; docs/DEVTOOLS.md) work in the
app too; with a pad script the game is never paused for lack of focus (nor under `ZX_CONTROL`).

The developer runner (`recomp/run.sh`, the check tools) uses the CPU renderer unless `ZX_GS_BACKEND=gpu`, and
leaves the game's menus as they are unless `ZX_GAME_SETTINGS_MENUS=1` (the added widgets move the game's heap,
which the logic and replay checks compare). The settings' runner variables: `ZX_GS_FULL_HEIGHT=1`,
`ZX_MOTION_BLUR=0|1|2`, `ZX_DRAW_DISTANCE=0|1|2`, `ZX_DEV_MENU=1`, `ZX_TUNING_INI=<file>`,
`ZX_LANGUAGE=<name>`.
