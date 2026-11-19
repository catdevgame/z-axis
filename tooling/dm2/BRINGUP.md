# Dave Mirra Freestyle BMX 2 (SLUS-20159): how to build, run and check

A short guide to the current way, not a log. The main repository's `docs/BRINGUP.md` has what DM2 shares with Aggressive Inline (build directories,
unpaced runs, scratch HOME, environment). What DM2 is and where it stands: PLAN.md.

## Layout

- This repository: `recomp/config.toml` (Ghidra export), `recomp/tools/make-config.py` (writes
  `config.game.toml`, and `config.ns.toml` with `namespace = "dm2"` for the collection's
  `recomp/generated-ns`), `recomp/run.sh`, the tools (PLAN.md "What is here"). Not committed: the disc
  copy (`recomp/disc-root`, `extracted/SLUS_201.59`), the recompiler's output (`recomp/generated`,
  `generated-ns`), the VU1 ahead-of-time code (`recomp/generated-vu1`), the made configs.
- PS2Recomp: one tree builds both games. DM2's PC features are `runtime/src/game_overrides/
  dave_mirra*.cpp`, its own readable code `runtime/src/game/dm2` (the rider), compiled in DM2's instance
  only (never in AI's library).

## Build

```sh
cd tooling/dm2/recomp
python3 tools/make-config.py                         # config.toml -> config.game.toml and config.ns.toml
<build>/recompiler/ps2xRecomp/ps2_recomp config.game.toml       # -> recomp/generated (config.ns.toml -> generated-ns)
# VU1 AOT: dump the micro memory once (one image: 836c2d1891f4ba03), then generate
PS2X_VU1_CODE_DUMP=$PWD/generated-vu1 ./run.sh 85 vu1codes
<build>/ps2xRuntime/ps2xVu1Aot generated-vu1 generated-vu1/*.vu1code
```

A runner for DM2 alone: configure PS2Recomp with `-DPS2X_RUNNER_GENERATED_DIR=<this repo>/recomp/generated
-DPS2X_VU1_AOT_DIR=<this repo>/recomp/generated-vu1`. Both games in one runner (the collection's
layout): AI's generated code as the main set, DM2's `generated-ns` in `PS2X_RUNNER_EXTRA_GENERATED_DIRS`
and its VU1 code in `PS2X_VU1_AOT_EXTRA_DIRS`; the function table in use is chosen by the ELF's name at
load. BMX XXX joins them the same way: its `generated-ns` (`$BMX_RECOMP/generated-ns`, paths.env, made by `bmx/recomp/recompile.py`) in
`PS2X_RUNNER_EXTRA_GENERATED_DIRS` too. Each game's instance holds its own folder (`game/dm2`, `game/bmx`) and,
with `PS2X_BIND_DIR` (the tree's committed `runtime/tools/bind`: `dm2.tsv`, `bmx.tsv`), the shared sources compiled again for it: DM2's every
source of `game/dm2/instance_sources.txt`. `game/dm2` calls that engine by name, so it is built only with DM2's bind
table; `game/bmx` with or without one. The apps are built from the main repository (`GAMES="ai dm2"
tools/make-app.sh`, `make-windows.ps1 -Games ai,dm2`; no `bmx` row yet); `tools/make-windows-dm2.ps1` builds DM2
alone on Windows.

## Run

```sh
cd tooling/dm2/recomp
RUNNER=<ps2EntryRunner> ./run.sh 80 myrun          # scratch HOME, card and support under $DM2_SCRATCH (/tmp/ai-dm2)
tools/run-game-dm2.sh NAME MAX_SECS [RUNNER]        # for checks: unpaced, scratch HOME; GAME=bmx for BMX XXX
```

- Pictures: window captures come out black; use `PS2X_SHOT_AT="<sec>:<png>,..."` or
  `tools/nctl.py NAME "shot FILE"`.
- `PS2X_DEV_UNLOCK_ALL=1` (or `PS2X_DM2_OPEN_LEVELS=1`) opens every level;
  `PS2X_DEV_START_LEVEL=<folder>` (`WOODWARD`, `AIRPORT`, ...) goes straight into one (main repository
  `docs/DEVTOOLS.md`).
- The PS2 paths back, for comparisons: `PS2X_SOUND=spu2`, `PS2X_MOVIES=hle`, `PS2X_IOP=1`,
  `PS2X_SAVES=card` (main repository `docs/NATIVE.md`).

## Checks

- Logic: `tools/dm2logic.py run NAME` then `compare NAME`: 6/6 MATCH against the PCSX2 reference run
  (`tools/PCSX2-DM2.md`). Defaults must not change DM2's logic.
- The A/B: `career_ab.py gate --game dm2` (its routes, `ab_allow_dm2.txt`; main repository
  `docs/READABLE.md` "The gate").
- The recording corpus: routes in `tools/dm2routes.py`, recorded with `tools/dm2corpus.py record` (a
  build with `PS2X_GAME_CORPUS=dm2`), shipped to the Linux worker with `dm2corpus.py ship`, replayed there with
  `rcheck.sh COMMIT` sent to the Linux worker with `--dm2-corpus --fncheck-arg=--semantic`; `dm2cov.py` says what the routes
  reach, `dm2proof.py` what a replay proves by level. BMX XXX's corpus goes the same way (`--game bmx`,
  `tools/bmxroutes.py`, `reccorpus/bmx-v1` on the Linux worker): `game/bmx` is in the tree; its A/B (`career_ab.py
  --game bmx`) waits for the pinned checkers' BMX lines.
- BMX XXX played: `RUNNER=... BMXCORPUS=DIR tools/playbot/bmx.py play NAME LEVEL SEED TICKS` in the main
  repository (`bmxmap.py build` makes the level maps in DIR first).
- Play: the main repository's `tools/playbot/dm2.py`.
