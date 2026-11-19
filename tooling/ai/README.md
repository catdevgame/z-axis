# Aggressive Inline's workspace (PS2, SLUS-20327)

Aggressive Inline's recompiler config, tools and reference runs, and the build of Aggressive Inline alone as an
app. The runtime, the readable C++ and the guides are the checkout's own (`runtime/`, `docs/BRINGUP.md`,
`docs/READABLE.md`, `docs/DEVTOOLS.md`); per-function facts are work database notes (`workdb.py notes ADDR`).
No game data is stored here.

| Path | What it is |
|---|---|
| `decomp/` | The retail ELF split into assembly by splat, with the original toolchain (Sony ee-gcc 2.9-ee-991111, `-O2 -G16`, its assembler and binutils); rebuilds the ELF byte for byte. The listing and the binutils are used by the checks and the PCSX2 hooks. See `decomp/README.md`. |
| `recomp/` | Recompiler config and Ghidra function map, `run.sh`; generated code, disc copy, logs and dumps are links into `$AI_DATA` (never committed). |
| `ghidra/` | Headless Ghidra pipeline (Emotion Engine extension) that produces the function map, plus reports (`ghidra/NOTES.md`). |
| `tools/` | Packaging (`make-app.sh`, `make-windows.ps1`, `build-ffmpeg.sh`), run and check drivers (`run-game.sh`, `logiccheck.py`, `routes.py`, `playbot/` ...; docs/BRINGUP.md "Checks"), the PCSX2 tools (`pcsx2pad.py`, `tools/PCSX2PAD.md`; the reference runs, `tools/REFERENCE-RUN.md`, `tools/REFERENCE-RUN-2.md`), disc readers (`gc_disc.py`, `ps2mc.py`) and `share/zaxis_texdump.py` (texture dump and replace for the three Z-Axis games). |
| `baseline/` | A link to `$AI_DATA/baseline`: the disc in PCSX2 with its own config, the reference for correct behaviour. |
| `reference/` | A link: screenshots from PCSX2 and the native build, for comparison. |

Inputs expected outside the checkout: `$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso` and, for the
PCSX2 tools, `scph39001.bin`.

The collection app (all three games) is built as the top README's "Building" says. This file covers what is
particular to Aggressive Inline: the app of this game alone, its saves, and the variables for automated runs.
Playing, controls and settings: README "Playing", `CONTROLS.md`, `SETTINGS.md`, `docs/MODS.md`.

## The Mac app (`Aggressive Inline.app`)

`tools/make-app.sh` with no `GAMES` builds Aggressive Inline alone; `GAMES="ai dm2 bmx"` builds the collection. The
header of `tools/make-app.sh` lists every input, with its defaults.

Needs Xcode command line tools, CMake and Ninja, the generated code (`recomp/generated`, made by
`tooling/recompile.sh`), the ELF it was recompiled from (`recomp/config.game.toml`'s `input`) and your disc image
(`$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso`, or `ISO=`). The first build downloads FFmpeg's source
(network needed once; see below). Optional inputs, each read only and turned into ready-made files in the bundle:
the Xbox release (`XBOX_ISO`: HD textures, movies, the Zoo and the Ape) and the GameCube release (`GC_IMAGE`:
Estate).

```sh
tools/make-app.sh                       # release build + bundle -> dist/Aggressive Inline.app
SKIP_BUILD=1 tools/make-app.sh          # re-bundle the existing build
DIST=$PWD/dist-test tools/make-app.sh   # write somewhere else (dist-*/ is ignored)
ISO=/Volumes/x/ai.iso tools/make-app.sh # another copy of the disc image
NO_GAME_DATA=1 tools/make-app.sh        # no game data: the disc is set up at first start
```

The disc image is only read. Its files are extracted with `bsdtar` into `dist/.disc-cache` (about 960 MB, redone
only when the image's size or date changes) and the script checks that the disc boots the exact ELF the code was
recompiled from (SHA-256), else it stops. The files are cloned into the bundle (APFS clones: no extra disk space or
copy time). Because the bundle then holds game data, do not share it; `dist/` is ignored by git.

The script configures `out/build-release` in the checkout (Release: -O3 and LTO; the verbose logs and RPC traces
compiled out; debug UI compiled in but hidden) with the app front end (`PS2X_APP_NAME`), builds the SHA-256 of the
recompiled ELF into it, links FFmpeg statically, makes the icon from the local `baseline/icon/square.png` if present,
writes `Info.plist` and signs ad hoc. The app is a single arm64 executable (only system frameworks) plus the game
files. Everything, SDL3 included, is built for `MIN_MACOS` (default 11.0; `PUBLIC=1`, the release's build, 15.0),
and the script reports the highest `LC_BUILD_VERSION` minos in the bundle as `LSMinimumSystemVersion`.

### FFmpeg

The runtime uses FFmpeg to decode the MPEG-2 video of the PSS movies (intros, menu FMV window, level fly-ins):
libavcodec's `mpeg2video` decoder and `mpegvideo` parser, libswscale for YUV to RGBA, and libavutil; and for a custom
soundtrack libavformat with the file protocol, the Ogg, FLAC and MP3 demuxers and the Vorbis, FLAC and MP3 decoders.
Movie sound and the game's own sound are not FFmpeg's: the movies' PCM and the game's PS-ADPCM are decoded by the
runtime itself. `tools/build-ffmpeg.sh` downloads the pinned release (checked by SHA-256), configures exactly that
(`--disable-everything`, no programs, no network, no autodetected or external libraries, no avdevice, avfilter or
swresample, LGPL), builds static libraries for the deployment target and prints the install prefix. The tarball and
the build are cached in `third_party-cache/` (ignored by git); a changed version or configure line builds into a new
directory. It runs on Linux unchanged; on Windows it is meant for an MSYS2 shell with
`EXTRA_CONFIGURE=--toolchain=msvc`.

The build takes the prefix as `-DPS2X_FFMPEG_PREFIX=<prefix>` and then searches only there; empty (the default) uses
the system FFmpeg through pkg-config, e.g. Homebrew's. `make-app.sh` uses the static build (`FFMPEG=system` falls
back to Homebrew's, whose dylibs are copied into `Contents/Frameworks` and raise the minimum macOS to theirs). The
developer build (`out/build-ai`, `recomp/run.sh`) can use either:

```sh
cmake ... -DPS2X_FFMPEG_PREFIX=$(tools/build-ffmpeg.sh) -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
```

## Windows (`Aggressive Inline.exe`)

The same game for Windows 10/11 x64: a folder `dist\Aggressive Inline` with `Aggressive Inline.exe` (no console
window, statically linked: SDL3, FFmpeg, the C++ runtime; nothing to install), the game files from your disc in
`disc\` and a `README.txt`. Like the Mac app it holds game data: do not share it (`dist\` is ignored by git).

Needs Visual Studio 2022 or its Build Tools with the C++ workload, the "C++ Clang compiler" (clang-cl) and "C++
CMake tools" components and a Windows 10/11 SDK (its `dxc` compiles the GPU renderer's shaders); MSYS2 with `make`,
`diffutils` and `nasm` for the FFmpeg build (a portable copy is fine, `-Msys <folder>`); and this workspace's
`recomp\generated` and `recomp\generated-vu1`. The header of `tools\make-windows.ps1` lists every option.

```powershell
powershell -ExecutionPolicy Bypass -File tools\make-windows.ps1 -Iso "D:\Aggressive Inline (USA).iso"
... -DiscDir recomp\disc-root     # an extracted copy of the disc instead
... -SkipBuild                    # repackage the existing build
... -Zip                          # also dist\Aggressive Inline.zip
... -ProfileData x.profdata       # profile-guided optimization (below)
```

It mirrors `make-app.sh`: the disc image is extracted with Windows' own `tar` into `dist\.disc-cache` (once per
image) and its boot ELF must have the SHA-256 of the ELF the code was recompiled from; FFmpeg comes from
`tools/build-ffmpeg.sh` run in MSYS2 with MSVC (`--toolchain=msvc`, static CRT, the same minimal LGPL
configuration); the runner is built with clang-cl, lld-link and ThinLTO into `out\build-release-win` in the
checkout; the icon is made from the local `baseline\icon\square.png` if present (PNG images in an `.ico`, built into
the `.exe`). The game files are hard links to the cache (no extra space). The GPU renderer runs on Direct3D 12.

Profile-guided build (about 10% faster): build with `-fprofile-instr-generate -DPS2X_PGO_GENERATE=1` (and
`clang_rt.profile-x86_64.lib` from VS's LLVM on the link line), play a few levels with
`LLVM_PROFILE_FILE=...\run-%p.profraw`, quit normally, merge with
`llvm-profdata merge -o x.profdata run-*.profraw` and pass `-ProfileData x.profdata`.

## Where the app of this game alone keeps things

The collection keeps each game's files in `<support>/games/ai` (README "Your discs"); the app of Aggressive Inline
alone keeps the same files in its own support folder:

| What | Where |
|---|---|
| Settings, controls, gameplay options | `settings.ini`, `controls.ini`, `gameplay.ini` in `~/Library/Application Support/Aggressive Inline` (Windows: `%APPDATA%\Aggressive Inline`) |
| Saves (career, parks) | `Saves` in the same folder |
| Memory cards of earlier versions (imported once, left as they are) | `memcards/slot1`, `memcards/slot2` in the same folder |
| Mods | `Mods` in the same folder (`docs/MODS.md`) |
| Log (this run and the previous one) | `~/Library/Logs/Aggressive Inline/aggressive-inline.log` (Windows: `Logs` in the support folder) |
| Screenshots (F12) | `~/Pictures/Aggressive Inline` (Windows: `Pictures\Aggressive Inline`) |

Portable mode: an empty `portable.txt` next to the app keeps all of it in an `Aggressive Inline Data` folder beside
it (a collection app: `Z-Axis Collection Data`).

If PCSX2 has an Aggressive Inline save in its memory card folder, the app offers once to continue from it (it is
copied to `Saves/Career.sav`).

## Saves

There is no memory card. Save and load write and read plain files at once, with no card checks, slot choice or
overwrite questions, and the game loads the career when you press START at the title. It also saves the career by
itself when a career level ends and when you quit (if a save exists). Files, in `Saves`:

| File | What |
|---|---|
| `Career.sav` | the career (skaters, records, unlocks, cheats, volumes), 459780 bytes |
| `Parks/Park 000.sav`, ... | park editor parks, 8196 bytes each |
| `*.bak` | the previous version of each, kept on every save (rename it to go back) |
| `imported.txt` | what was copied from `memcards/slot1` and `slot2` the first time (they are not changed) |

Each is written to a temporary file and renamed over the old one, so a crash cannot leave half a save; a damaged
file is skipped for its `.bak`. The files hold exactly the bytes the game writes to a memory card (the card file
`BASLUS-20327CEAIGAME/BASLUS-20327CEAIGAME`, a u32 byte sum then the data), so moving a save between here and PCSX2
is a copy: `tools/ps2mc.py extract CARD.ps2 BASLUS-20327CEAIGAME OUTDIR` and copy
`OUTDIR/BASLUS-20327CEAIGAME/BASLUS-20327CEAIGAME` to `Saves/Career.sav`, or `ps2mc.py import` the other way (with
the card folder layout). Profiles: `profile = Career` under `[saves]` in `settings.ini` names the career file;
another name keeps a separate career (`Saves/<name>.sav`); parks are shared. `PS2X_SAVES=card` brings back the
memory card (HLE card folders, the console's dialogs) for comparisons with PCSX2.

## Automated runs

The app takes `PS2X_APP_SUPPORT_DIR` (a scratch folder instead of the support folder; logs go to its `Logs`,
screenshots to its `Screenshots`), `PS2X_APP_LOG_DIR`, `PS2X_GAMEPLAY_INI`, `PS2X_SCREENSHOT_DIR`, `PS2X_SAVES_DIR`
and `PS2X_SAVE_PROFILE` (the saves folder and profile; the developer runner uses `<card folder>-saves` next to
`PS2X_MC_ROOT`), `PS2X_SAVES=card`, `PS2X_MODS_DIR` (another mods folder), `PS2X_DISC=cdvd` (the game's files
through the emulated IOP disc module and drive), `PS2X_MOVIES=hle` (the movies through the emulated MPEG decoder and
the IOP sound path), `PS2X_SOUND=spu2` (the game's sound driver on the emulated IOP and sound chip), `PS2X_IOP=1`
(run the idle IOP anyway), `PS2X_APP_DISC_DIR` (another folder of the disc's files; exit code 2 if the game files
are missing or different), `PS2X_APP_IMPORT_CARD` (import a PCSX2 card's save without asking) and
`PS2X_WIDESCREEN=1`/`0` (overrides the setting; the developer runner too). The developer switches
(`PS2X_PAD_SCRIPT`, `PS2X_SHOT_AT`, `PS2X_DEBUG_UI=1`, `PS2X_PERF_LOG=<seconds>`, ...; docs/DEVTOOLS.md) work in the
app too; with a pad script the game is never paused for lack of focus (nor under `PS2X_CONTROL`).

The developer runner (`recomp/run.sh`, the check tools) uses the CPU renderer unless `PS2X_GS_BACKEND=gpu`, and
leaves the game's menus as they are unless `PS2X_GAME_SETTINGS_MENUS=1` (the added widgets move the game's heap,
which the logic and replay checks compare). The settings' runner variables: `PS2X_GS_FULL_HEIGHT=1`,
`PS2X_MOTION_BLUR=0|1|2`, `PS2X_DRAW_DISTANCE=0|1|2`, `PS2X_DEV_MENU=1`, `PS2X_TUNING_INI=<file>`,
`PS2X_LANGUAGE=<name>`.
