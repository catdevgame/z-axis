# tooling

Everything needed to work on the Z-Axis Collection that is not the runtime and the games themselves: the
recompiler, the game workspaces' tools, the checks and the Xbox tools. Game data is never here: it lives
in the data folder (`ZAXIS_DATA`, by default beside the checkout) and is linked in by `setup.sh`. (It was a
repository of its own until it was folded in; its old history is the second parent of tag `tooling-fold`.)

| Path | What it is |
|---|---|
| `paths.env` | The one config of paths: data, scratch, workspaces, worktrees (the defaults; a workshop's own layout file, when there, wins over them). `paths.sh` (shell, `source` it) and `zpaths.py` (Python) read it; an environment variable of the same name wins. A z-axis tree's top-level folders: `zdir TREE runtime` in `paths.sh`, `zpaths.folder(tree, 'runtime')` (`checks/zlayout.py`). |
| `setup.sh` | Makes a checkout ready: the data links of `data-links.txt` in `ai/` and `dm2/`. `--check` reports only. |
| `recompile.sh`, `recompile.ps1` | Each game's generated code made from your own disc image (README "Building"): the ELF taken from the disc, the recompiler config made, the recompiler run, the VU1 micro memory image written. |
| `ai/` | Aggressive Inline's workspace: `tools/` (run, check, play, packaging (`make-app.sh`, `make-windows.ps1`, `gc_disc.py`, `xbox_pack.py`, `dm2_xbox.py`, `panel_map.py` and the level readers `ai_level.py`, `dm2_level.py`), PCSX2 and Xbox reference tools, `playbot/`), `recomp/` (recompiler config, Ghidra function map, `run.sh`), `decomp/` (the split assembly and the original toolchain), `ghidra/` (the headless pipeline and its reports). Its data folders (`recomp/generated`, `recomp/disc-root`, `recomp/dumps`, `extracted`, `dist`, ...) are links into `AI_DATA`. `ai/README.md`. |
| `dm2/` | Dave Mirra BMX 2's workspace, with the work database's tool `tools/workdb.py` that every game uses (`dm2/MATCH3.md`) and BMX XXX's PCSX2, route and logic tools; data links into `DM2_DATA`. `dm2/BRINGUP.md`. |
| `bmx/` | BMX XXX's recompiler config and its one-step `recomp/recompile.py` (writes into paths.env's `BMX_RECOMP`). |
| `checks/` | The checks and analysis tools that read a z-axis tree but that its build never runs: the corpus replay (`fncheck_corpus.py`, `readable_corpus.py`), the A/B (`career_ab.py`, `ab_inputs/`, the allow lists), `seqcheck.py`, the lints (`readable_args.py`, `readable_decls.py`, `readable_plaincalls.py`, `readable_layering.py`, `readable_localscan.py`, `scaffolding.py`) with their baselines, `diffcall.py`, `readable_sweep.py` and the other authoring tools, the GS and VU1 bit-exact checks (`GS_REPLAY.md`, `VU1_CORPUS.md`), `checks_test.py`. The tree they read: `ztree.py` (`ZAXIS_TREE`, else the checkout the current folder is in, else `ZAXIS_MAIN`). The build's own tools and tables are in `runtime/tools`. |
| `recompiler/` | The recompiler (`ps2xRecomp/README.md`) and the ELF analyzer (`ps2xAnalyzer/Readme.md`). |
| `xbox/` | The Xbox release's comparison tools. |

New checkout: `git clone <the repository> z-axis`, then `tooling/setup.sh`. A worktree:
`git -C $ZAXIS_MAIN worktree add wt/NAME -b NAME main` (its own tooling/ comes with it).
