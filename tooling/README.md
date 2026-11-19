# z-axis-tooling

Everything needed to work on the Z-Axis Collection that is not the runtime and the games themselves: the
recompiler, the game workspaces' tools, the checks and the Xbox tools. Game data is never here: it lives
in the data folder (`ZAXIS_DATA`, by default beside the checkout) and is linked in by `setup.sh`. (Until
2026-11 this was a repository of its own, a submodule; its history is the second parent of the commit that
folded it in, tag `tooling-fold`.)

| Path | What it is |
|---|---|
| `paths.env` | The one config of paths: data, scratch, workspaces, worktrees (the defaults; a workshop's own layout file, when there, wins over them). `paths.sh` (shell, `source` it) and `zpaths.py` (Python) read it; an environment variable of the same name wins. A z-axis tree's top-level folders, old or new (runtime, tests, iop, docs; ps2xRuntime, ps2xTest, ps2xIOP, recomp before tools17's renames): `zdir TREE runtime` in `paths.sh`, `zpaths.folder(tree, 'runtime')` (`checks/zlayout.py`). |
| `setup.sh` | Makes a checkout ready: the data links of `data-links.txt` in `ai/` and `dm2/` (and, in a workshop's main checkout, the link to its agents' rules). `--check` reports only. |
| `ai/` | Aggressive Inline's workspace (was the aggressive-inline repo): `tools/` (run, check, play, packaging (`make-app.sh`, `make-windows.ps1`, `gc_disc.py`, `xbox_pack.py`, `dm2_xbox.py`, `panel_map.py` and the level readers `ai_level.py`, `dm2_level.py`), PCSX2 and Xbox reference tools, `playbot/`), `recomp/` (recompiler config, Ghidra function map, `run.sh`), `decomp/` (the matching decomp setup), `ghidra/` (the headless pipeline and its reports), `mods/pcsx2/`. Its data folders (`recomp/generated`, `recomp/disc-root`, `recomp/dumps`, `extracted`, `dist`, ...) are links into `AI_DATA`. |
| `dm2/` | Dave Mirra BMX 2's workspace (was the DM2 repository), with the work database's tool `tools/workdb.py` that every game uses; data links into `DM2_DATA`. |
| `checks/` | The checks and analysis tools that read a z-axis tree but that its build never runs (moved from z-axis `ps2xRuntime/tools` by tools14): the corpus replay (`fncheck_corpus.py`, `readable_corpus.py`), the A/B (`career_ab.py`, `ab_inputs/`, the allow lists), `seqcheck.py`, the lints lanecheck and merge-check run (`readable_args.py`, `readable_decls.py`, `readable_plaincalls.py`, `readable_layering.py`, `readable_localscan.py`, `scaffolding.py`) with their baselines, `diffcall.py`, `readable_sweep.py` and the other authoring tools, `checks_test.py`. The tree they read: `ztree.py` (`ZAXIS_TREE`, else the checkout the current folder is in, else `ZAXIS_MAIN`). The build's own tools and tables stay in z-axis `ps2xRuntime/tools`. |
| `xbox/` | The Xbox release's comparison tools. |

New checkout: `git clone <the repository> z-axis`, then `tooling/setup.sh`. A worktree:
`git -C $ZAXIS_MAIN worktree add wt/NAME -b NAME main` (its own tooling/ comes with it).
