# Z-Axis Collection

Three Z-Axis games in one app for Mac, Windows and Android, on one engine, as source:

| Game | Engine | Its own code |
|---|---|---|
| Aggressive Inline | ZGE3, the reference | `games/aggressive` |
| Dave Mirra Freestyle BMX 2 | ZGE2, an older version | `games/dm2` |
| BMX XXX | ZGE4, a newer version | `games/bmx` |

The goal is each game as readable C++ that a person can understand and mod: the engine (`engine/zge`) written
once, with each game's version differences inside it, and each game's own code in its own folder. Your own copies
of the games supply their assets (levels, models, textures, sound, movies): the app copies them from your discs once,
when it sets a game up, and never plays from a disc.

The app opens on the collection menu. Pick a game; each game's own menus have Exit to Collection, which brings
the menu back without restarting. Closing the window quits.

The guides, for playing, modding and contributing, are listed in `docs/README.md`.

## Playing

The releases page has the app for Mac (Apple silicon, macOS 15 or later), Windows (x64) and Android (arm64,
Android 13 or later), with no game files in it, and `SHA256SUMS` to check the downloads against. The Mac and Windows
apps are not signed by Apple or Microsoft, so each system asks once before the first start.

**Mac**

1. Download `Z-Axis-Collection-<version>-mac.zip` and open it: Finder unzips it to `Z-Axis Collection.app`.
2. Move the app into Applications (drag it onto Applications in Finder's sidebar). Do this before the first
   start: an app started from Downloads runs from a temporary copy.
3. Open it. macOS says "Z-Axis Collection" Not Opened ("Apple could not verify ... is free of malware"), with
   Done and Move to Trash: click Done. Control-click > Open does not get past this.
4. Open System Settings > Privacy & Security and scroll down to Security: next to "Z-Axis Collection" was
   blocked to protect your Mac, click Open Anyway (it is there for about an hour after step 3), then enter your
   password. macOS asks once more: click Open Anyway. From then on the app opens like any other.

**Windows**

1. Download `Z-Axis-Collection-<version>-windows.zip`, right-click it, Extract All, and put the folder where you
   keep games. Start the app from the extracted folder, not from inside the zip.
2. Open `Z-Axis Collection.exe`. Windows says "Windows protected your PC" (Microsoft Defender SmartScreen):
   click More info, then Run anyway. It asks only this once.

**Android**

1. Download `Z-Axis-Collection-<version>-android.apk` on the device and open it; Android asks once to allow
   installs from that app (the browser or file manager). It is made for dual-screen handhelds: the game on the top
   screen, the level's map, goals and tricks on the bottom one.
2. Play with a gamepad, the handheld's own or a connected one. The first start opens Settings > Games, as below:
   Find Discs picks the folder your discs are in through Android's own file picker. `platform/android/README.md` has the
   rest.

**Your discs**

The games come from your own discs, the USA PlayStation 2 releases (Aggressive Inline SLUS-20327, Dave Mirra
Freestyle BMX 2 SLUS-20159, BMX XXX SLUS-20415), as a disc image in any of the usual formats (ISO, BIN/CUE such as
Redump's, CHD, CSO or ZSO) or a folder of the disc's files; the European releases, demos and prereleases are other
programs and do not run. The first time, the app opens on Settings > Games: choose Find Discs and pick the folder
your discs are in (or drop it on the window). The app finds each game's disc in it and its folders, and the Xbox and
GameCube discs of the games for their extras, lists them with a tick for each game, and Set Up Games checks each disc
and copies that game's files into its support folder (`~/Library/Application Support/Z-Axis Collection` on a Mac,
`%APPDATA%\Z-Axis Collection` on Windows), so the disc image is not needed after that. A game it did not find has its
own Set Up From Image. Setting up takes a few minutes a game: the app also builds the game's code for your machine
(on Android, on the device). A game's Xbox or GameCube disc can be added later (Settings > Games: Add Xbox Disc, Add
GameCube Disc). Settings > Games checks, repairs or removes a game's files. Your settings and saves live in the same
folder: deleting the app leaves them, and a new version finds them. When a new version makes some of a game's extras
differently, it makes those again at its first start from the Xbox and GameCube images you set them up from (the
Games page shows it); if an image is not there any more, its extras stay as they were and the app asks for it.

Keys, mouse and gamepads are `docs/CONTROLS.md`; the Settings pages in each game's menus are `docs/SETTINGS.md`; mods,
texture packs and custom maps are `docs/MODS.md`.

## How the source is made

Every function of a game starts out as generated code, a literal translation of the original program, so the game
runs from the first day. A readable C++ body takes its place one function at a time, and only once it is proven
to behave exactly as the original:

- **Recorded calls.** Each game has a corpus of recorded function calls from real play. A readable body runs
  against every recorded call of its function and must give the same results, word for word.
- **Whole-game checks.** Logic checks compare each game's play against a reference run, A/B gates compare RAM
  with the readable code on and off, and games run one after another in one process must match fresh runs.
- **Readable means readable.** No address literals, registers, raw offsets or guessed names; game logic reads
  as game logic. A lint enforces it, and the temporary forms still in use while the full picture comes in are
  listed with what removes each one (`docs/SCAFFOLDING.md`).

A change goes in only when every check passes.

## Layout

| Path | What it is |
|---|---|
| `engine/` | the games' engine: `zge` (shared by the three games), `host` (the host's services to it), `overrides` (the host code every game's hooks share, `zge_*`), `game_module` (a game's recompiled code as a module), `zal` (the games' .ZAL archives) |
| `ps2/` | the PS2 machine our code runs on: `ee` (the framework the readable code is built and checked with), `sce` (the system libraries the games call), `kernel`, `gs`, `vu1`, `spu2`, `disc` (the disc image reader), `runtime` (the host: memory, graphics on SDL3's GPU API, sound, input, saves), `include` (what the generated code includes) |
| `games/aggressive`, `dm2`, `bmx` | each game: its own code at the top, `overrides/` (the host code that hooks it), `tools/`, `docs/`, `workspace/` (its recompiler config and research workspace) |
| `collection/` | the collection: the app and its game picker, the shared settings, the bottom screen, mods, maps, recording and playback |
| `platform/` | Mac, Windows and Android: the platform glue and each one's packaging (Android: dual-screen handhelds, the game made for two screens) |
| `tools/` | by purpose: `setup` (setup and the recompile from your discs), `modding` (`zal`, the converters, the level and extras makers), `readable`, `checks`, `playbot`, `workdb` (the work database) |
| `recompiler/` | the recompiler that makes the generated code, and its analyzer |
| `iop/` | the I/O side the games' sound and file modules run on |
| `third_party/` | the LLVM toolchain build, the fetched libraries' CMake, FFmpeg's build |
| `tests/` | unit tests |
| `docs/` | the guides: `docs/README.md` lists them for players, modders and contributors; `docs/CONTROLS.md`, `docs/SETTINGS.md`: keyboard, mouse and gamepad; the settings in each game's own menus |

## Building

You need your own discs of the games, the USA PlayStation 2 releases (Aggressive Inline SLUS-20327, Dave Mirra
Freestyle BMX 2 SLUS-20159, BMX XXX SLUS-20415), as disc images: any game in any of the usual formats, an ISO, a
BIN/CUE dump such as Redump's (give its CUE), a CHD, a CSO or a ZSO. The European releases, demos and prereleases
are other programs and do not run. The Xbox and GameCube discs are optional extras (more levels, textures and
movies).

**What to install**

- macOS (Apple silicon): Xcode's command line tools (`xcode-select --install`), then
  [Homebrew](https://brew.sh) and `brew install cmake ninja python`.
- Windows x64: Visual Studio 2022 Build Tools with the "Desktop development with C++" workload and its
  "C++ Clang tools for Windows" and "C++ CMake tools" components; [Python 3](https://www.python.org);
  [Git](https://git-scm.com); [MSYS2](https://www.msys2.org) with `pacman -S make diffutils nasm` (for FFmpeg).
- The first build downloads the libraries it uses (SDL3, FFmpeg and the rest), so it needs the network once.

**Steps** (macOS; Windows below)

```sh
git clone <this repository's address> z-axis
cd z-axis
tools/setup/setup.sh                       # once: the folders the build writes into (paths.env's ZAXIS_DATA)
tools/setup/recompile.sh --ai <AI disc image> --dm2 <DM2 disc image> --bmx <BMX XXX disc image>
NO_GAME_DATA=1 GAMES="ai dm2 bmx" platform/mac/make-app.sh    # games/aggressive/workspace/dist/Z-Axis Collection.app
```

`recompile.sh` takes each game's program from your disc image (read by the app's own reader, `zx-disc`, which it
builds with the recompiler) and turns it into the C++ the app is built from (a few seconds a game).
`make-app.sh` builds the app (about ten minutes the first time).
With `NO_GAME_DATA=1` the app holds no game data: the first time it starts it opens on Settings > Games, where Find
Discs (or a drop on the window) finds every game's disc in a folder and Set Up Games checks each and copies the game's
files into its support folder (Settings > Games checks, repairs or removes them; `--setup <folder or image>` does the
same from a terminal). Without it, make-app.sh puts your
discs' files into the app itself; the script's header lists those inputs (where your disc images are, the optional
Xbox and GameCube releases, the build directory).

On Windows, from PowerShell in the clone (no developer prompt needed):

```bat
powershell -ExecutionPolicy Bypass -File tools\setup\recompile.ps1 -Ai <AI disc image> -Dm2 <DM2 disc image> -Bmx <BMX XXX disc image>
powershell -ExecutionPolicy Bypass -File platform\windows\make-windows.ps1 -NoGameData -Games ai,dm2,bmx -Msys C:\msys64
```

`docs/BRINGUP.md` covers development builds, the tests and running a single game.

An app built with your discs' files in it is for your own use, so do not share it (`dist/` is ignored by git).

## Contributing

`CONTRIBUTING.md` says how to build, test and send a change, and what never goes into the repository (game data);
`docs/README.md` lists the guides to the source.
Bugs, readable-code reports and ideas go through the issue forms.

## License and thanks

GPL-3.0 (`LICENSE`). The runtime's host layer and the recompiler descend from PS2Recomp (ran-j/PS2Recomp, forked
at eb8688b), the IOP's sound from PCSX2's SPU2 (at 091ab68), and the system libraries the games link are ported from
newlib 1.8.2 and GCC 2.95.2's libgcc with their notices kept. Uses ELFIO, toml11, fmt, libdwarf, rabbitizer,
nlohmann/json, SDL3, Dear ImGui, sse2neon, FFmpeg, miniz, xxHash, zstd, libchdr, LZ4, stb_truetype and bcdec, each downloaded at build time
and checked by SHA-256 (FFmpeg's source by the app scripts' `build-ffmpeg.sh`, the rest by CMake). The Android app
bundles LLVM's clang and lld (Apache-2.0 WITH LLVM-exception, its licence beside them), built from LLVM 23.1.5's source
release (`third_party/toolchain`), to make each game's code on the device.
`NOTICE` lists every third-party part, its licence and its notices.
