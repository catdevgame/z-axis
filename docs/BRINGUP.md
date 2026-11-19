# Building and running: the developer's guide

How to build the runner and the tests, recompile, and run a game for development and checks. The app for players
is README "Building"; how to send a change is CONTRIBUTING.md. Per-function findings are notes in the work database
(`workdb.py card ADDR`, `workdb.py notes ADDR`, see READABLE.md "Where knowledge lives"). The other guides are
listed in `docs/README.md`.

## Layout

- The z-axis checkout: the runtime (`runtime`: EE/VU/GS/IOP, the renderers, the readable game code in
  `src/game`), the unit tests (`tests`), and `tooling/` (the game workspaces, and the recompiler that makes
  the generated code: `tooling/recompiler`, built by the same CMake).
  The paths below without a root are AI's workspace, `tooling/ai`.
- `recomp/generated` (and `generated-vu1`): the recompiler's output. Generated code is never committed;
  `PS2X_RUNNER_GENERATED_DIR` (AI's) and `PS2X_RUNNER_EXTRA_GENERATED_DIRS` (DM2's, BMX XXX's) point a build at it.
  Each game's generated code is a game module (`runtime/game_module.h`, `game_module/abi.h`): compiled with no
  library header against the module's view of the runtime (`ps2_generated.h`, `include/game_module/`,
  `cmake/game_module_flags.txt`, the flags the app uses for the code it builds from the player's disc) and reaching
  the runtime through one table each way. `PS2X_GAME_MODULES=static` (the default) links the modules into the runner;
  `shared` makes one shared library each in the build's `game-code/`, which the runner loads at boot. A recompiler
  from before game modules made folders these builds refuse: each set of generated code is a folder of its own,
  `<folder>-<set>` (`recomp/generated-<set>`, DM2's `recomp/generated-ns-<set>`, BMX XXX's in `BMX_DATA`), the set
  being `tooling/recompiler/generated_key.py`'s key of the recompiler and the games' inputs, made once by
  `tooling/recompiler/make-generated.sh` and never written again; `GENERATED_SET` (the environment, else
  `paths.env`, where it is empty: the plain folders `tooling/recompile.sh` writes) names the set the tooling's
  builds use (`zgen`, `zpaths.generated()`).
  `generated-vu1` (`PS2X_VU1_AOT_DIR`, `PS2X_VU1_AOT_EXTRA_DIRS`) holds the micro memory images
  (`<hash>.vu1code`) and ps2xVu1Aot's code for them; the build uses the code made by its own tree's
  generator: a folder whose `vu1_aot.key` is not the tree's key (a hash of ps2xVu1Aot's sources and the
  images) is left as it is, and the build makes and uses `vu1-aot/<key>` in its build dir (generated output
  never goes into a source checkout; a cross build builds the generator alone for the build machine,
  `runtime/tools/vu1_aot_host`, or takes `PS2X_VU1_AOT_TOOL`, a host build's ps2xVu1Aot).
  The app's first run makes the same code itself: the recompiler writes the image (its staged config's `[vu1]`
  table, `runtime/tools/vu1_image.py`), the app's generator its code, and the game module carries it
  (`runtime/tools/VU1_AOT.md`).
- `recomp/disc-root`: a read-only extracted copy of the disc (`tar -xf <iso> -C disc-root`; bsdtar reads
  ISO9660); the ELF there (`SLUS_203.27`) is byte-identical to the disc's. The runtime's cdRoot defaults to
  the ELF's directory. Discs and other releases' files are build-time inputs only, never committed (CONTRIBUTING.md "The one rule: no game data").
- `recomp/config.toml` is the Ghidra-derived recompiler config; `recomp/config.game.toml` is made from it by
  `tools/make-config.py` (it adds `recompile = ["sceVif1Pk*", "sceGifPk*"]` so libvifpk/libgifpk run as recompiled
  original code); `config.vu1-local.toml` and the other `config.*-local.toml` are the VU1 and check variants.
- `recomp/run.sh`, `recomp/run/` (cwd and memory cards of developer runs), `recomp/logs/` (run logs and shots),
  `recomp/traces/`, `recomp/dumps/`: the developer runner's own files. Scratch (notes, runs, recordings)
  goes in `$ZAXIS_SCRATCH/<task>/` (`tooling/paths.env`).
- `tools/`: run and check drivers (below). `baseline/` ($AI_DATA/baseline) is PCSX2 with the reference
  config; never touch it.

## Build

Needs Xcode's command line tools (Apple clang), CMake and Ninja; the first configure fetches the dependencies
(network once: ELFIO, toml11, fmt, rabbitizer, SDL3, imgui, sse2neon and the rest; FFmpeg statically from
`tools/build-ffmpeg.sh` through `PS2X_FFMPEG_PREFIX`, otherwise a prebuilt shared FFmpeg is downloaded). On arm64
the runtime's x86 SIMD goes through sse2neon. Windows: `tools/make-windows.ps1`. A Windows build by hand, from a VS 2022 x64 developer prompt (the C++ workload with its
Clang/LLVM and CMake components):

```bat
cmake -S . -B out/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPS2X_BUILD_STUDIO=OFF ^
  -DPS2X_FFMPEG_PREFIX=<static FFmpeg from tools/build-ffmpeg.sh> ^
  -DPS2X_RUNNER_GENERATED_DIR=<generated code> -DPS2X_VU1_AOT_DIR=<ps2xVu1Aot output>
ninja -C out/build-win ps2EntryRunner ps2x_tests
```

x86-64 needs SSE4.1/SSSE3 (`PS2X_X64_LEVEL`, default `x86-64-v2`). The GPU renderer's Direct3D 12 shaders are
compiled with the Windows SDK's `dxc` (`runtime/src/lib/gs/GPU_BACKEND.md`).

Recompile only when the recompiler, the config or the ELF changed:

```sh
cd tooling/ai/recomp
python3 tools/make-config.py                                   # config.toml -> config.game.toml (when it changed)
KEY=$(python3 ../../recompiler/generated_key.py config.game.toml <DM2's config.ns.toml> <BMX XXX's config.ns.toml>)
../../recompiler/make-generated.sh ../../../out/build-macos/recompiler/ps2xRecomp/ps2_recomp config.game.toml \
  $AI_DATA/recomp/generated $KEY       # -> $AI_DATA/recomp/generated-<KEY>, and the same for DM2 and BMX XXX
```

`tooling/recompile.sh --ai IMAGE --dm2 IMAGE --bmx IMAGE` (Windows: `tooling\recompile.ps1`) does that whole step from
your disc images, the ELF taken from the disc and checked first (README "Building"). Any image takes any format (ISO,
BIN/CUE, CHD, CSO, ZSO), told by its bytes: the ELF comes out through `ps2x_disc` (`iop/tools/ps2x_disc.cpp`, the
app's own reader, `iop/include/ps2x/iop/disc_image_file.h`), which the script builds with the recompiler. BMX XXX's
is its workspace's one step, `tooling/bmx/recomp/recompile.py` from its Ghidra export
(`tooling/bmx/recomp/config.toml`): the configs, the namespaced code, its VU0 upload and its VU1 micro memory image
(`runtime/tools/vu0_upload.py`, `vu1_image.py`: both read from the ELF, no run), into paths.env's `BMX_RECOMP`.

`NO_GAME_DATA=1 tools/make-app.sh` (`make-windows.ps1 -NoGameData`) builds an app with no game data: it sets each game up
from the player's disc at first start (`runtime/src/collection/game_setup.cpp`, the search for every game's discs in a
folder `disc_finder.cpp`; from a terminal `--setup FOLDER|IMAGE`,
`--check-games [--full]`, `--remove-game ID`).

Build the runner in your worktree (the first configure needs the fetched deps; later ones reuse
`FETCHCONTENT_SOURCE_DIR_*` from an earlier build dir, so no network):

```sh
cd <z-axis worktree>
cmake -S . -B out/<name> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPS2X_RUNNER_GENERATED_DIR=$(source tooling/paths.sh && zgen $AI_WORKSPACE/recomp/generated)
ninja -C out/<name> ps2EntryRunner ps2xFnCheck ps2x_tests
cd <worktree root> && out/<name>/ps2xTest/ps2x_tests        # unit tests run from the worktree root
ctest --test-dir out/<name>/ps2xIOP                          # the IOP's tests (a ctest from the build dir's top finds none)
```

App builds (`dist/`, the Windows dist, the collections): `tools/make-app.sh` and `tools/make-windows.ps1` read the
discs and write the ready-made files into the app; the running game never opens a disc or converts assets.

## Run

```sh
cd tooling/ai/recomp
./run.sh 40 myrun                  # runner for 40 s; log logs/myrun.log; CD root, memory card and hidden debugger set;
                                   # RUNNER=... picks the binary, SHOT_EVERY=n takes window shots, RUN_UNTIL=file stops on a file
../tools/run-game.sh NAME MAX_SECS [RUNNER]    # for checks: scratch HOME, card and cwd under /tmp/ai-runs/NAME, unpaced,
                                               # STOP_TICK=<vsync> stops there, MAX_SECS is the hard limit
```

- Game runs for checks (logiccheck, A/B, recordings, routes, probes) are unpaced: `PS2X_UNPACED=1` (guest time follows
  EE cycles, same logic, several times faster). Paced (`PS2X_UNPACED=0`) only for host-time measurements: fps,
  pacing, audio, visuals.
- Runs use a scratch HOME (`PS2X_APP_SUPPORT_DIR`, `PS2X_APP_LOG_DIR`, `PS2X_SCREENSHOT_DIR`); never the user's
  `~/Library` or `~/Pictures`. Every run has a time limit; before reporting check `pgrep -x ps2EntryRunner` and `pgrep -x ps2xFnCheck` and kill yours by PID.
- GS backend: the GPU renderer (`PS2X_GS_BACKEND=gpu`, SDL_GPU) is the default in the app, native meshes/skaters/Xbox
  textures on top; `cpu` is the software reference. `PS2X_GS_NULL=1` draws nothing (logic at full speed),
  `PS2X_VU1_SKIP=1` with it skips VU1 programs. A null-GS run is headless (`PS2X_HEADLESS=1` or `0` says so
  for any run): no window and no GPU device, so it works over SSH on Windows, where no swapchain can exist.

Common environment (the runtime reads many more: `grep -rn 'PS2X_' runtime`, and SETTINGS.md for settings):

| Variable | Effect |
|---|---|
| `PS2X_CD_IMAGE`, `PS2X_CD_ROOT`, `PS2X_HOST_ROOT`, `PS2X_MC_ROOT` | I/O roots (run.sh sets the CD root and memory card) |
| `PS2X_SAVES=card` | memory-card saves (the app uses native save files) |
| `PS2X_CONTROL=<dir>` | drive a running game from outside: `tools/nctl.py start NAME`, `nctl.py NAME "press CROSS; wait 60"`, `stop` |
| `PS2X_PAD_SCRIPT`, `PS2X_PAD_SCRIPT_CLOCK=vsync`, `PS2X_PAD_PORT1=mirror`, `PS2X_VIRTUAL_PADS=2` | scripted and virtual pads; port 1 is empty without a second gamepad |
| `PS2X_TRACE_RPC`, `_CALLS`, `_VU1`, `_DMA`, `_MC`, `_EE`, `_MOVIES` | traces (calls: `0xA,0xB,@0xSITE`) |
| `PS2X_DUMP_WHEN="label:cond&cond"`, `PS2X_DUMP_AT_FRAME`, `PS2X_DUMP_RAM_ON_EXIT`, `PS2X_STATE_LOG=<addr>,...` | RAM dumps at conditions; one line per vsync a listed word changed |
| `PS2X_READABLE`, `PS2X_FNREC*`, `PS2X_FNPROF`, `PS2X_FNCYCLES` | the readable code and its recording (READABLE.md) |
| `PS2X_DEV_MENU`, `PS2X_DEV_UNLOCK_ALL`, `PS2X_DEV_START_LEVEL`, `PS2X_TUNING_INI`, `PS2X_LANGUAGE` | developer tools (DEVTOOLS.md) |
| `PS2X_PERF_LOG=1`, `PS2X_GSMEM_AUDIT=1` | frames per second in the log; GS texture-memory audit |

## Android (dual-screen handhelds)

The same runner cross-built for arm64, the game on the top screen and a panel on the bottom one: building the APK,
putting the games on a device and how it runs there are `android/README.md`.

## Checks

- Unit tests: `ps2x_tests` from the worktree root.
- Logiccheck: replays the PCSX2 reference run natively (`tools/REFERENCE-RUN.md`) and compares state at 21
  checkpoints. `tools/lc-when-free.sh NAME` queues `logiccheck.py run NAME` and `compare NAME` (up to 3 at once,
  RUNNER and a scratch HOME exported); `/tmp/lc-NAME-cmp.log` must say 21/21 MATCH. About 35 s unpaced. Defaults must
  not change game logic. DM2's and BMX XXX's counterparts are `tooling/dm2/tools/dm2logic.py` and `bmxlogic.py`.
- Career A/B: `tooling/checks/career_ab.py gate` (its inputs, the career one from `tools/careercheck.py`), unpaced
  with the null GS at `PS2X_EE_SCALE=4096`, defaults against `PS2X_READABLE=off` (READABLE.md "The gate").
- Readable code: the semantic corpus replay, `readable_args.py --check` and the lint (READABLE.md "The gate").
- Play: `tools/playbot/` (closed-loop players that report the game's own numbers: distance, tricks, score, goals).
- Input and UI: `tools/inputcheck.py`; sound: `tools/audiocheck.py`; speed: `tools/speedsweep.py`, `loadtime.py`,
  `framespikes.py`; every level and mode: `tools/sweep.py`, `routes.py` (DEVTOOLS.md).

## Where the features are

Every feature's settings are in `SETTINGS.md`, its facts per function in the work database (`workdb.py notes`).

## Cheats

The game's cheat codes, what each writes, and what the Settings > Cheats page does are in `docs/CHEATS.md`.

## Keeping this guide current

Change this file only when the way to build or run changes: the commands above, a variable that matters to every
run, a new check. A feature's story, a measurement, a fixed crash goes into the commit message and, per function,
into workdb notes; if a feature needs a manual, give it its own short guide (like CHEATS.md), not a section here.
