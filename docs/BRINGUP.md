# Building and running: the developer's guide

How to build the runner and the tests, recompile, and run a game for development and checks. This file has what
the three games share, with Aggressive Inline (AI) as the example; DM2's and BMX XXX's own workspace, build, runs
and checks are `games/dm2/docs/BRINGUP.md`. The app for players is README "Building"; how to send a change is
CONTRIBUTING.md. What is known about a function is in the work database (`workdb.py card ADDR`,
`workdb.py notes ADDR`, see READABLE.md "Where knowledge lives"). The other guides are listed in `docs/README.md`.

## Layout

- The z-axis checkout (README "Layout"): the PS2 machine (`ps2`: EE, VU1, GS, the IOP's SPU2, the renderers, the
  host runtime), the engine (`engine`), the games (`games/<game>`: the readable game code at the top, its workspace
  in `workspace/`), the unit tests (`tests`), the tools (`tools`) and the recompiler that makes the generated code
  (`recompiler`, built by the same CMake).
  The paths below without a root are AI's workspace, `games/aggressive/workspace`.
- `recomp/generated` (and `generated-vu1`): the recompiler's output. Generated code is never committed;
  `ZX_RUNNER_GENERATED_DIR` (AI's) and `ZX_RUNNER_EXTRA_GENERATED_DIRS` (DM2's, BMX XXX's) point a build at it. Each
  game's generated code is a game module (`runtime/game_module.h`, `game_module/abi.h`): compiled with no library
  header against the module's view of the runtime (`ps2_generated.h`, `engine/game_module/`,
  `engine/game_module/game_module_flags.txt`, the flags the app uses for the code it builds from the player's disc)
  and reaching the runtime through one table each way. `ZX_GAME_MODULES=static` (the default) links the modules into
  the runner; `shared` makes one shared library each in the build's `game-code/`, which the runner loads at boot. Each
  set of generated code is a folder of its own, `<folder>-<set>` (`recomp/generated-<set>`, DM2's
  `recomp/generated-ns-<set>`, BMX XXX's in `BMX_DATA`), the set being `recompiler/generated_key.py`'s key of the
  recompiler and the games' inputs, made once by `recompiler/make-generated.sh` and never written again;
  `GENERATED_SET` (the environment, else `paths.env`, where it is empty: the plain folders `tools/setup/recompile.sh`
  writes) names the set the checkout's builds use (`zgen`, `zpaths.generated()`). `generated-vu1` (`ZX_VU1_AOT_DIR`,
  `ZX_VU1_AOT_EXTRA_DIRS`) holds the micro memory images (`<hash>.vu1code`) and zx-vu1aot's code for them; the build
  uses the code made by its own tree's generator: a folder whose `vu1_aot.key` is not the tree's key (a hash of
  zx-vu1aot's sources and the images) is left as it is, and the build makes and uses `vu1-aot/<key>` in its build dir
  (generated output never goes into a source checkout; a cross build builds the generator alone for the build machine,
  `ps2/vu1/aot_host`, or takes `ZX_VU1_AOT_TOOL`, a host build's zx-vu1aot). The app's first run makes the same code
  itself: the recompiler writes the image (its staged config's `[vu1]` table, `tools/setup/vu1_image.py`), the app's
  generator its code, and the game module carries it (`docs/VU1_AOT.md`).
- `recomp/disc-root`: a read-only extracted copy of the disc (`tar -xf <iso> -C disc-root`; bsdtar reads
  ISO9660); the ELF there (AI's `SLUS_203.27`) is byte-identical to the disc's. The runtime's cdRoot defaults to
  the ELF's directory. Discs and other releases' files are build-time inputs only, never committed (CONTRIBUTING.md "The one rule: no game data").
- `recomp/config.toml` is the Ghidra-derived recompiler config; `recomp/config.game.toml` is made from it by
  `recomp/tools/make-config.py` (it adds `recompile = ["sceVif1Pk*", "sceGifPk*"]` so libvifpk/libgifpk run as recompiled
  original code); `config.vu1-local.toml` and the other `config.*-local.toml` are the VU1 and check variants.
- `recomp/run.sh`, `recomp/run/` (cwd and memory cards of developer runs), `recomp/logs/` (run logs and shots),
  `recomp/traces/`, `recomp/dumps/`: the developer runner's own files. Scratch (notes, runs, recordings)
  goes in `$ZAXIS_SCRATCH/<task>/` (`tools/paths.env`).
- `games/aggressive/tools/` (AI's own) and the tree's `tools/` (shared): run and check drivers (below). `baseline/`
  ($AI_DATA/baseline) is PCSX2 with the reference config; leave it as it is.

## Build

Needs Xcode's command line tools (Apple clang), CMake and Ninja; the first configure fetches the dependencies
(network once: ELFIO, toml11, fmt, rabbitizer, SDL3, imgui, sse2neon and the rest; FFmpeg statically from
`third_party/build-ffmpeg.sh` through `ZX_FFMPEG_PREFIX`, otherwise the system's through pkg-config; on Windows a
prebuilt shared FFmpeg is downloaded). On arm64
the runtime's x86 SIMD goes through sse2neon. Windows: `platform/windows/make-windows.ps1`. A Windows build by hand, from a VS 2022 x64 developer prompt (the C++ workload with its
Clang/LLVM and CMake components):

```bat
cmake -S . -B out/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DZX_FFMPEG_PREFIX=<static FFmpeg from third_party/build-ffmpeg.sh> ^
  -DZX_RUNNER_GENERATED_DIR=<generated code> -DZX_VU1_AOT_DIR=<zx-vu1aot output>
ninja -C out/build-win zx-runner zx-tests
```

x86-64 needs SSE4.2 (`ZX_X64_LEVEL`, default `x86-64-v2`; `x86-64-v3` for AVX2). The GPU renderer's Direct3D 12 shaders are
compiled with the Windows SDK's `dxc` (`docs/GPU_BACKEND.md`).

Recompile only when the recompiler, a config or an ELF changed. By hand, from AI's workspace (the key covers the
three games' configs):

```sh
cd games/aggressive/workspace/recomp
python3 tools/make-config.py                                   # config.toml -> config.game.toml (when it changed)
KEY=$(python3 ../../../../recompiler/generated_key.py config.game.toml <DM2's config.ns.toml> <BMX XXX's config.ns.toml>)
../../../../recompiler/make-generated.sh ../../../../out/<name>/recompiler/ps2xRecomp/ps2_recomp config.game.toml \
  $AI_DATA/recomp/generated $KEY       # -> $AI_DATA/recomp/generated-<KEY>, and the same for DM2 and BMX XXX
```

`tools/setup/recompile.sh --ai IMAGE --dm2 IMAGE --bmx IMAGE` (Windows: `tools\setup\recompile.ps1`) does that whole step for
the three games from your disc images, the ELF taken from the disc and checked first (README "Building"). Any image takes any format (ISO,
BIN/CUE, CHD, CSO, ZSO), told by its bytes: the ELF comes out through `zx-disc` (`tools/setup/disc_tool.cpp`, the
app's own reader, `ps2/disc/disc_image_file.h`), which the script builds with the recompiler. BMX XXX's
is its workspace's one step, `games/bmx/workspace/recomp/recompile.py` from its Ghidra export
(`games/bmx/workspace/recomp/config.toml`): the configs, the namespaced code, its VU0 upload and its VU1 micro memory image
(`tools/setup/vu0_upload.py`, `vu1_image.py`: both read from the ELF, no run), into paths.env's `BMX_RECOMP`.

`NO_GAME_DATA=1 platform/mac/make-app.sh` (`make-windows.ps1 -NoGameData`) builds an app with no game data: it sets each game up
from the player's disc at first start (`collection/game_setup.cpp`, the search for every game's discs in a
folder `disc_finder.cpp`; from a terminal `--setup FOLDER|IMAGE`,
`--check-games [--full]`, `--remove-game ID`).

Build the runner in your worktree (the first configure needs the fetched deps; later ones reuse
`FETCHCONTENT_SOURCE_DIR_*` from an earlier build dir, so no network):

```sh
cd <z-axis worktree>
cmake -S . -B out/<name> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DZX_RUNNER_GENERATED_DIR=$(source tools/paths.sh && zgen $AI_WORKSPACE/recomp/generated)
ninja -C out/<name> zx-runner zx-fncheck zx-tests
cd <worktree root> && out/<name>/tests/zx-tests        # unit tests run from the worktree root
ctest --test-dir out/<name>/iop                          # the IOP's tests: configure with -DPS2X_IOP_BUILD_TESTS=ON and
                                                         # build their targets (CONTRIBUTING.md); a ctest from the build dir's top finds none
```

App builds (`dist/`, the Windows dist, the collections): `platform/mac/make-app.sh` and `platform/windows/make-windows.ps1` read the
discs and write the ready-made files into the app; the running game never opens a disc or converts assets.

## Run

AI's run scripts (DM2's and BMX XXX's are in `games/dm2/docs/BRINGUP.md` "Run"):

```sh
cd games/aggressive/workspace/recomp
./run.sh 40 myrun                  # runner for 40 s; log logs/myrun.log; CD root, memory card and hidden debugger set;
                                   # RUNNER=... picks the binary, SHOT_EVERY=n takes window shots, RUN_UNTIL=file stops on a file
cd ../../../..                                 # the tree's root
tools/checks/run-game.sh NAME MAX_SECS [RUNNER]   # for checks: scratch HOME, card and cwd under /tmp/ai-runs/NAME,
                                                  # unpaced, STOP_TICK=<vsync> stops there, MAX_SECS is the hard limit
```

On Windows, `tools\checks\run-game.ps1 NAME MAX_SECS [RUNNER]` is run-game.sh and run-game-dm2.sh in one
(`$env:GAME` ai, dm2 or bmx), under `%TEMP%\ai-runs` unless `RUN_DIR`; `tools\setup\setup.ps1` makes the data
links as setup.sh does (folders as junctions; a file's link needs Developer Mode), and `tools\paths.ps1` is paths.sh.

- Game runs for checks (logiccheck, A/B, recordings, routes, probes) are unpaced: `ZX_UNPACED=1` (guest time follows
  EE cycles, same logic, several times faster). Paced (`ZX_UNPACED=0`) only for host-time measurements: fps,
  pacing, audio, visuals.
- Runs use a scratch HOME (`ZX_APP_SUPPORT_DIR`, `ZX_APP_LOG_DIR`, `ZX_SCREENSHOT_DIR`), so your own
  `~/Library` and `~/Pictures` stay untouched. Give every run a time limit, and check none is left after
  (`pgrep -x zx-runner`, `pgrep -x zx-fncheck`).
- GS backend: the GPU renderer (`ZX_GS_BACKEND=gpu`, SDL_GPU) is the default in the app, native meshes, players and
  Xbox textures on top; `cpu` is the software reference. `ZX_GS_NULL=1` draws nothing (logic at full speed),
  `ZX_VU1_SKIP=1` with it skips VU1 programs. A null-GS run is headless (`ZX_HEADLESS=1` or `0` says so
  for any run): no window and no GPU device, so it works over SSH on Windows, where no swapchain can exist.

Common environment (the runtime reads many more: `grep -rn 'ZX_' ps2 engine games collection platform`, and
docs/SETTINGS.md for settings):

| Variable | Effect |
|---|---|
| `ZX_CD_IMAGE`, `ZX_CD_ROOT`, `ZX_HOST_ROOT`, `ZX_MC_ROOT` | I/O roots (run.sh sets the CD root and memory card) |
| `ZX_SAVES=card` | memory-card saves (the app uses native save files) |
| `ZX_CONTROL=<dir>` | drive a running game from outside: `tools/checks/nctl.py start NAME`, `nctl.py NAME "press CROSS; wait 60"`, `stop` |
| `ZX_PAD_SCRIPT`, `ZX_PAD_SCRIPT_CLOCK=vsync`, `ZX_PAD_PORT1=mirror`, `ZX_VIRTUAL_PADS=2` | scripted and virtual pads; port 1 is empty without a second gamepad |
| `ZX_TRACE_RPC`, `_CALLS`, `_VU1`, `_DMA`, `_MC`, `_EE`, `_MOVIES` | traces (calls: `0xA,0xB,@0xSITE`) |
| `ZX_DUMP_WHEN="label:cond&cond"`, `ZX_DUMP_AT_FRAME`, `ZX_DUMP_RAM_ON_EXIT`, `ZX_STATE_LOG=<addr>,...` | RAM dumps at conditions; one line per vsync a listed word changed |
| `ZX_READABLE`, `ZX_FNREC*`, `ZX_FNPROF`, `ZX_FNCYCLES` | the readable code and its recording (READABLE.md) |
| `ZX_DEV_MENU`, `ZX_DEV_UNLOCK_ALL`, `ZX_DEV_START_LEVEL`, `ZX_TUNING_INI`, `ZX_LANGUAGE` | developer tools (DEVTOOLS.md) |
| `ZX_PERF_LOG=1`, `ZX_GSMEM_AUDIT=1` | frames per second in the log; GS texture-memory audit |

## Android (dual-screen handhelds)

The same runner cross-built for arm64, the game on the top screen and a panel on the bottom one: building the APK,
putting the games on a device and how it runs there are `platform/android/README.md`.

## Checks

- Unit tests: `zx-tests` from the worktree root.
- Logic checks: each game's play, run natively, against its PCSX2 reference run. AI's is logiccheck
  (`games/aggressive/docs/REFERENCE-RUN.md`, 21 checkpoints): `games/aggressive/tools/lc-when-free.sh NAME` queues `logiccheck.py run NAME` and
  `compare NAME` (up to 3 at once, RUNNER and a scratch HOME exported); `/tmp/lc-NAME-cmp.log` must say 21/21 MATCH.
  About 35 s unpaced. DM2's and BMX XXX's are `games/dm2/tools/dm2logic.py` and `bmxlogic.py`. Defaults must not
  change game logic.
- A/B: `tools/checks/career_ab.py gate` (AI's inputs, the career one from `games/aggressive/tools/careercheck.py`; `--game dm2` and
  `--game bmx` for the others), unpaced with the null GS at `ZX_EE_SCALE=4096`, defaults against `ZX_READABLE=off`
  (READABLE.md "The gate").
- Readable code: the semantic corpus replay, `readable_args.py --check` and the lint (READABLE.md "The gate").
- Play: the closed-loop players, `games/aggressive/tools/playbot/ai.py`, `games/dm2/tools/playbot/dm2.py`,
  `games/bmx/tools/playbot/bmx.py` and the rest on the shared core in `tools/playbot/`, that report the game's own
  numbers: distance, tricks, score, goals.
- AI's input and UI: `games/aggressive/tools/inputcheck.py`; sound: `games/aggressive/tools/audiocheck.py`; speed:
  `tools/checks/speedsweep.py`, `loadtime.py`, `framespikes.py`; every level and mode: `tools/checks/sweep.py`,
  `games/aggressive/tools/routes.py` (DEVTOOLS.md).

## Where the features are

Every feature's settings are in `docs/SETTINGS.md`, its facts per function in the work database (`workdb.py notes`).
The games' cheat codes, what each writes, and what the Settings > Cheats page does are in `docs/CHEATS.md`.
