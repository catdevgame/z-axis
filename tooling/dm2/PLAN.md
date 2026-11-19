# Dave Mirra Freestyle BMX 2: what this repository is, where DM2 stands, what is left

This repository is Dave Mirra Freestyle BMX 2's (SLUS-20159) workshop for its PC version, and the home
of the work database tool every game uses (`tools/workdb.py`, MATCH3.md). How to build, run and check DM2:
BRINGUP.md.

## What is here

- `recomp/`: DM2's recompiler config (`config.toml` from Ghidra, `tools/make-config.py`), the Ghidra
  function map and `run.sh`. Generated code (`recomp/generated`, `generated-ns`, `generated-vu1`) and
  the disc copy are never committed.
- `tools/`: the PCSX2 reference tools and the logic check (`tools/PCSX2-DM2.md`, `dm2pcsx2.py`,
  `dm2logic.py`), DM2's and BMX XXX's routes and recording corpus (`dm2routes.py`, `bmxroutes.py`,
  `dm2corpus.py`, `dm2cov.py`, `dm2proof.py`, `dm2ride.py`), the Windows build (`make-windows-dm2.ps1`,
  `tools/windows/`), and the cross-build match and work database (`fmatch3.py`, `modmap.py`,
  `xmatch.py`, `gcmatch.py`, `readable_carry.py`, `readable_ghidra.py`, `workdb.py`, `tools/ghidra/`;
  MATCH3.md).
- DM2's code lives in PS2Recomp: `runtime/src/game_overrides/dave_mirra*.cpp` (the PC features)
  and `runtime/src/game/dm2` (DM2's instance of the shared readable engine: `functions.tsv`,
  `close.tsv`, `symbols_status.tsv`, `instance_sources.txt`, the rider).

## Where DM2 stands

- DM2 runs natively on the runtime that runs Aggressive Inline: native disc, movies, sound driver and
  saves with the IOP stopped, the GPU renderer, widescreen (3D and 2D), draw distance, Restart,
  mouse-look, the Xbox release's Venice and Greenville with their career (Settings > Game > Xbox
  Levels). dm2logic: 6/6 checkpoints against PCSX2.
- It ships with Aggressive Inline in one app, the Z-Axis Collection (`GAMES="ai dm2"
  tools/make-app.sh`, `make-windows.ps1 -Games ai,dm2` in the main repository): a picker or
  `--game`, per-game folders, shared settings and controls, Switch Game (F8, File > Switch Game).
- In the collection's readable engine DM2 is a version difference of AI: identical engine functions
  share AI's C++ body, close ones share it with a per-game difference, and DM2's own code is
  `game/dm2` (aggressive-inline `docs/READABLE.md` "Symbols"). DM2 has its own recording corpus
  (`reccorpus/dm2-v1`, on the Linux worker) and the same checks as AI (BRINGUP.md "Checks").

## Direction

The user (2026-11-05, later the same day): one executable with all three games working: Aggressive
Inline, DM2 and BMX XXX, each with its own riders on its own maps, in its career and free ride. The
games share no riders or levels. zge is the core platform each game's folder sits on (`game/aggressive`,
`game/dm2`, `game/bmx`). Shared code changes keep DM2 (dm2logic 6/6, its corpus replay,
`career_ab.py gate --game dm2`) and BMX XXX (its build and a played level) from regressing.

Dropped: mixing the games (DM2's rider and levels hosted in Aggressive Inline, AI's levels in DM2).
What was merged of it is taken out: its settings, menus and packaging, its converters and hooks.

## Left for DM2

- DM2's own readable code: the functions not identical or close to AI's (`game/dm2`), checked on
  DM2's corpus.
- Settings pages in DM2's own menus (the collection's settings.ini and the app menu cover the host
  side today), button prompt art, cheats as switches.
- The rider and bike still go through VU1 (DM2's level meshes are drawn natively).
- A longer logic check than the six checkpoints (a ProQuest goal, a second level).
