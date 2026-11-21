# Dave Mirra Freestyle BMX 2 (SLUS-20159): its workspace, and how to build, run and check it

A short guide to the current way, not a log. `docs/BRINGUP.md` has what DM2 shares with Aggressive Inline (build
directories, unpaced runs, scratch HOME, environment).

DM2 runs on the runtime that runs Aggressive Inline and ships with it and BMX XXX in the collection app. In the
readable engine it is a version difference of AI: identical engine functions share AI's C++ body, close ones share it
with a per-game difference, and DM2's own code is `runtime/src/game/dm2` (docs/READABLE.md "Symbols"). It has its own
recording corpus, logic check and A/B, as AI does (below). What it has of AI's enhancements: docs/ENHANCEMENTS.md.

## Layout

- `recomp/`: DM2's recompiler config (`config.toml`, the Ghidra export), `recomp/tools/make-config.py` (writes
  `config.game.toml`, and `config.ns.toml` with `namespace = "dm2"` for the collection's `recomp/generated-ns`), the
  Ghidra function map and `run.sh`. Not committed: the disc copy (`recomp/disc-root`, `extracted/SLUS_201.59`), the
  recompiler's output (`recomp/generated`, `generated-ns`), the VU1 micro memory image (`recomp/generated-vu1`), the
  made configs.
- `tools/`: the PCSX2 reference tools and the logic checks (`tools/PCSX2-DM2.md`, `dm2pcsx2.py`, `dm2logic.py`;
  BMX XXX's `bmxpcsx2.py`, `bmxlogic.py`), DM2's and BMX XXX's routes and recording corpus (`dm2routes.py`,
  `bmxroutes.py`, `dm2corpus.py`, `dm2cov.py`, `dm2proof.py`, `dm2ride.py`), the Windows build of DM2 alone
  (`make-windows-dm2.ps1`, `tools/windows/`), and the cross-build match and work database (`fmatch3.py`,
  `modmap.py`, `xmatch.py`, `gcmatch.py`, `readable_carry.py`, `readable_ghidra.py`, `workdb.py`, `tools/ghidra/`;
  MATCH3.md).
- The tree: DM2's PC features are `runtime/src/game_overrides/dave_mirra*.cpp`, its own readable code
  `runtime/src/game/dm2` (`functions.tsv`, `close.tsv`, `symbols_status.tsv`, `instance_sources.txt`), compiled in
  DM2's instance only (never in AI's library).

## Build

`tooling/recompile.sh --dm2 <disc image>` does all of it: the ELF from the disc, the configs, the recompiler run
into `recomp/generated-ns`, and the VU1 micro memory image. By hand:

```sh
cd tooling/dm2/recomp
python3 tools/make-config.py                                  # config.toml -> config.game.toml and config.ns.toml
<build>/recompiler/ps2xRecomp/ps2_recomp config.game.toml     # -> recomp/generated (config.ns.toml -> generated-ns)
```

A runner for DM2 alone: configure the tree with `-DZX_RUNNER_GENERATED_DIR=<workspace>/recomp/generated
-DZX_VU1_AOT_DIR=<workspace>/recomp/generated-vu1`. Several games in one runner (the collection's layout): AI's
generated code as the main set, DM2's `generated-ns` in `ZX_RUNNER_EXTRA_GENERATED_DIRS` and its VU1 folder in
`ZX_VU1_AOT_EXTRA_DIRS`; the function table in use is chosen by the ELF's name at load. BMX XXX joins them the same
way: its `generated-ns` (`$BMX_RECOMP/generated-ns`, paths.env, made by `tooling/bmx/recomp/recompile.py`) in
`ZX_RUNNER_EXTRA_GENERATED_DIRS` too. Each game's instance holds its own folder (`game/dm2`, `game/bmx`) and, with
`ZX_BIND_DIR` (the tree's committed `runtime/tools/bind`: `dm2.tsv`, `bmx.tsv`), the shared sources compiled again
for it: every source of the game's `instance_sources.txt`. `game/dm2` and `game/bmx` call that engine by name, so
each is built only with its game's bind table. The apps are built with `tooling/ai/tools/make-app.sh`
(`GAMES="ai dm2 bmx"`) and `make-windows.ps1 -Games ai,dm2,bmx`; `tools/make-windows-dm2.ps1` builds DM2 alone on
Windows.

### VU1 AOT

DM2 has one VU1 micro memory image (836c2d1891f4ba03). `recompile.sh` writes it into `recomp/generated-vu1` with the
tree's `runtime/tools/vu1_image.py`; a build given that folder (`ZX_VU1_AOT_DIR` or `ZX_VU1_AOT_EXTRA_DIRS`)
compiles its microprograms ahead of time with its own `zx-vu1aot` (`runtime/tools/VU1_AOT.md`). Without it DM2's
VU1 runs on the interpreter. A run can also dump the images it meets:
`ZX_VU1_CODE_DUMP=$PWD/generated-vu1 ./run.sh 85 vu1codes`.

## Run

```sh
cd tooling/dm2/recomp
RUNNER=<zx-runner> ./run.sh 80 myrun          # scratch HOME, card and support under $DM2_SCRATCH (/tmp/ai-dm2)
../tools/run-game-dm2.sh NAME MAX_SECS [RUNNER]     # for checks: unpaced, scratch HOME; GAME=bmx for BMX XXX
```

- Pictures: window captures come out black; use `ZX_SHOT_AT="<sec>:<png>,..."` or
  `tools/nctl.py NAME "shot FILE"`.
- `ZX_DEV_UNLOCK_ALL=1` (or `ZX_DM2_OPEN_LEVELS=1`) opens every level;
  `ZX_DEV_START_LEVEL=<n>` (the level select's value: 0 Woodward Camp .. 7 Airport) or `PARK` goes straight into
  one in FREERIDE (`docs/DEVTOOLS.md`).
- The PS2 paths back, for comparisons: `ZX_SOUND=spu2`, `ZX_MOVIES=hle`, `ZX_IOP=1`, `ZX_SAVES=card`
  (`docs/NATIVE.md`).

## Checks

- Logic: `tools/dm2logic.py run NAME` then `compare NAME` against the PCSX2 reference run (`tools/PCSX2-DM2.md`):
  run 1, 6/6 MATCH; `--run 2` (a ProQuest ride with a goal done), 21/21. Defaults must not change DM2's logic.
  BMX XXX's: `tools/bmxlogic.py`, the same method on its own reference run.
- The A/B: `tooling/checks/career_ab.py gate --game dm2` (its routes, `ab_allow_dm2.txt`; `--game bmx` with
  `ab_allow_bmx.txt`; docs/READABLE.md "The gate").
- The recording corpus: routes in `tools/dm2routes.py`, recorded with `tools/dm2corpus.py record` (a build with
  `ZX_GAME_CORPUS=dm2`) and replayed with `tooling/checks/fncheck_corpus.py`; `dm2cov.py` says what the routes
  reach, `dm2proof.py` what a replay proves by level. BMX XXX's corpus goes the same way (`--game bmx`,
  `tools/bmxroutes.py`).
- Play: `tooling/ai/tools/playbot/dm2.py`; BMX XXX: `RUNNER=... BMXCORPUS=DIR tooling/ai/tools/playbot/bmx.py play
  NAME LEVEL SEED TICKS` (`bmxmap.py build` makes the level maps in DIR first).
