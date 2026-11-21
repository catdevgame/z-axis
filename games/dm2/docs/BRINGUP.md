# Dave Mirra Freestyle BMX 2 (SLUS-20159) and BMX XXX (SLUS-20415): the workspace, and how to build, run and check them

`docs/BRINGUP.md` has what the three games share (build directories, unpaced runs, scratch HOME, environment).

DM2 and BMX XXX run on the same runtime as Aggressive Inline, and the three ship together in the collection app.
In the readable engine each is a version difference of AI: identical engine functions share AI's C++ body, close
ones share it with a per-game difference, and each game's own code is in its folder, `games/dm2` and
`games/bmx` (docs/READABLE.md "Symbols"). Each has its own recording corpus, logic check and A/B, as AI
does (below). Which of AI's enhancements each has: docs/ENHANCEMENTS.md.

## Layout

- `games/dm2/workspace/recomp/` (`recomp/` below): DM2's recompiler config (`config.toml`, the Ghidra export),
  `recomp/tools/make-config.py` (writes `config.game.toml`, and `config.ns.toml` with `namespace = "dm2"` for the
  collection's `recomp/generated-ns`), the Ghidra function map and `run.sh`. Not committed: the disc copy
  (`recomp/disc-root`, `extracted/SLUS_201.59`), the recompiler's output (`recomp/generated`, `generated-ns`), the VU1
  micro memory image (`recomp/generated-vu1`), the made configs.
- `games/dm2/tools/`: DM2's PCSX2 reference tools and logic check (`games/dm2/docs/PCSX2.md`, `dm2pcsx2.py`,
  `dm2logic.py`), its routes (`dm2routes.py`), `dm2ride.py`, `nctl.py` (DM2's paths) and its player (`playbot/`). BMX
  XXX's are in `games/bmx/tools/` (`bmxpcsx2.py`, `bmxlogic.py`, `bmxroutes.py`,
  `playbot/`).
- Shared with AI: the game runs and the recording corpus in `tools/checks/` (`run-game-dm2.sh`, `dm2corpus.py`,
  `dm2cov.py`, `dm2proof.py`), the Windows build of DM2 alone (`platform/windows/make-windows-dm2.ps1`), and the
  cross-build match and work database in `tools/workdb/` (`fmatch3.py`, `modmap.py`, `xmatch.py`, `gcmatch.py`,
  `readable_carry.py`, `readable_ghidra.py`, `workdb.py`, `ghidra/`; MATCH3.md).
- The tree: DM2's PC features are `games/dm2/overrides/dave_mirra*.cpp` and BMX XXX's
  `games/bmx/overrides/bmx_xxx_*.cpp`. Each game's own readable code (`games/dm2`, `games/bmx`: `functions.tsv`,
  `close.tsv`, `symbols_status.tsv`, `instance_sources.txt`) is compiled in that game's instance only, never in AI's
  library.

## Build

`tools/setup/recompile.sh --dm2 <disc image>` does all of it: the ELF from the disc, the configs, the recompiler run
into `recomp/generated-ns`, and the VU1 micro memory image. `--bmx <disc image>` does the same for BMX XXX, through
its one step `games/bmx/workspace/recomp/recompile.py` (into `$BMX_RECOMP`). DM2 by hand:

```sh
cd games/dm2/workspace/recomp
python3 tools/make-config.py                                  # config.toml -> config.game.toml and config.ns.toml
<build>/recompiler/ps2xRecomp/ps2_recomp config.game.toml     # -> recomp/generated (config.ns.toml -> generated-ns)
```

A runner for DM2 alone: configure the tree with `-DZX_RUNNER_GENERATED_DIR=<workspace>/recomp/generated
-DZX_VU1_AOT_DIR=<workspace>/recomp/generated-vu1`. Several games in one runner (the collection's layout): AI's
generated code as the main set, DM2's `generated-ns` in `ZX_RUNNER_EXTRA_GENERATED_DIRS` and its VU1 folder in
`ZX_VU1_AOT_EXTRA_DIRS`; the function table in use is chosen by the ELF's name at load. BMX XXX joins them the same
way: its `generated-ns` (`$BMX_RECOMP/generated-ns`, paths.env, made by `games/bmx/workspace/recomp/recompile.py`) in
`ZX_RUNNER_EXTRA_GENERATED_DIRS` too. Each game's instance holds its own folder (`games/dm2`, `games/bmx`) and, with
`ZX_BIND_DIR` (the tree's committed `tools/readable/bind`: `dm2.tsv`, `bmx.tsv`), the shared sources compiled again
for it: every source of the game's `instance_sources.txt`. `games/dm2` and `games/bmx` call that engine by name, so
each is built only with its game's bind table. The apps are built with `platform/mac/make-app.sh`
(`GAMES="ai dm2 bmx"`) and `make-windows.ps1 -Games ai,dm2,bmx`; `platform/windows/make-windows-dm2.ps1` builds DM2 alone on
Windows.

### VU1 AOT

DM2 has one VU1 micro memory image (836c2d1891f4ba03). `recompile.sh` writes it into `recomp/generated-vu1` with the
tree's `tools/setup/vu1_image.py`; a build given that folder (`ZX_VU1_AOT_DIR` or `ZX_VU1_AOT_EXTRA_DIRS`)
compiles its microprograms ahead of time with its own `zx-vu1aot` (`docs/VU1_AOT.md`). Without it DM2's
VU1 runs on the interpreter. A run can also dump the images it meets:
`ZX_VU1_CODE_DUMP=$PWD/generated-vu1 ./run.sh 85 vu1codes`.

## Run

```sh
cd games/dm2/workspace/recomp
RUNNER=<zx-runner> ./run.sh 80 myrun          # scratch HOME, card and support under $DM2_SCRATCH (/tmp/ai-dm2)
cd ../../../..                                # the tree's root
tools/checks/run-game-dm2.sh NAME MAX_SECS [RUNNER]   # for checks: unpaced, scratch HOME; GAME=bmx for BMX XXX
```

- Pictures: window captures come out black; use `ZX_SHOT_AT="<sec>:<png>,..."` or
  `games/dm2/tools/nctl.py NAME "shot FILE"`.
- `ZX_DEV_UNLOCK_ALL=1` (or `ZX_DM2_OPEN_LEVELS=1`) opens every level;
  `ZX_DEV_START_LEVEL=<n>` (the level select's value: 0 Woodward Camp .. 7 Airport) or `PARK` goes straight into
  one in FREERIDE (`docs/DEVTOOLS.md`).
- The PS2 paths back, for comparisons: `ZX_SOUND=spu2`, `ZX_MOVIES=hle`, `ZX_IOP=1`, `ZX_SAVES=card`
  (`docs/NATIVE.md`).

## Checks

- Logic: `games/dm2/tools/dm2logic.py run NAME` then `compare NAME` against the PCSX2 reference run (`games/dm2/docs/PCSX2.md`).
  A pass is 6/6 MATCH for run 1 and 21/21 for `--run 2` (a ProQuest ride with a goal done). BMX XXX's is
  `games/bmx/tools/bmxlogic.py`, the same method on its own reference run. Defaults must not change either game's logic.
- The A/B: `tools/checks/career_ab.py gate --game dm2` (its routes, `ab_allow_dm2.txt`; `--game bmx` with
  `ab_allow_bmx.txt`; docs/READABLE.md "The gate").
- The recording corpus: routes in `games/dm2/tools/dm2routes.py`, recorded with `tools/checks/dm2corpus.py record` (a build with
  `ZX_GAME_CORPUS=dm2`) and replayed with `tools/checks/fncheck_corpus.py`; `dm2cov.py` says what the routes
  reach, `dm2proof.py` what a replay proves by level. BMX XXX's corpus goes the same way (`--game bmx`,
  `games/bmx/tools/bmxroutes.py`).
- Play: `games/dm2/tools/playbot/dm2.py`; BMX XXX: `RUNNER=... BMXCORPUS=DIR games/bmx/tools/playbot/bmx.py play
  NAME LEVEL SEED TICKS` (`bmxmap.py build` makes the level maps in DIR first).
