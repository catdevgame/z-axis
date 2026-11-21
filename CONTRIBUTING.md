# Contributing

Thanks for helping with the Z-Axis Collection. This file says how to build, test and send a change. The
README covers what the project is and how its source is made; `docs/READABLE.md` is the guide to writing
readable game code.

## The one rule: no game data

Nothing from the discs goes into the repository, an issue or a pull request: no disc images or files from them,
no executables or modules, no extracted assets, textures, sounds, movies or saves, no generated code (the
recompiler's output, `tooling/ai/recomp/generated` and the like), no recordings, dumps or traces, no screenshots
or videos of the games. They stay on your machine, in the data folder `tooling/setup.sh` links in (git ignores
it). The code is written from the games' own code and names its own things; it never carries their bytes.

The repository has a scanner for this, and every change goes through it:

```sh
python3 -I tooling/checks/public_scan.py --added origin/main   # the lines you added: home folders, e-mail, secrets
python3 -I tooling/checks/public_scan.py --tree HEAD           # every committed file: disc formats, binaries, dumps
```

Both must end with `0 failing`. A finding names the rule and the file; `--list-rules` explains each rule. Paths
in code and scripts are relative or come from `tooling/paths.env` (through `tooling/paths.sh` or
`tooling/zpaths.py`), never a folder on your own machine.

## Building

README "Building" goes from a clean machine to the app: what to install, `tooling/setup.sh`, the recompile from
your own discs, `make-app.sh` or `make-windows.ps1`. `docs/BRINGUP.md` covers development builds: the runner,
a single game, the debugger, the run scripts.

## Testing

The unit tests need no game data, no generated code and no disc (a clean clone on a Mac: about 6 minutes to build at -j6, under a minute to run):

```sh
cmake -S . -B out/dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_IOP_BUILD_TESTS=ON
ninja -C out/dev zx-tests ps2_iop_emulator_tests ps2_iop_import_tests ps2_iop_compatibility_tests \
  ps2_iop_import_version_tests ps2_iop_spu2_tests
out/dev/tests/zx-tests                 # from the repository's root; every test must pass
ctest --test-dir out/dev/iop            # the IOP's own tests
```

On Windows, configure from a Visual Studio 2022 x64 developer prompt with clang-cl as `docs/BRINGUP.md` shows,
then build and run the same targets. Run the tests with a scratch home folder if you want your own settings and
saves left alone (`ZX_APP_SUPPORT_DIR`, `ZX_APP_LOG_DIR`).

A change to game code is checked further on the maintainers' machines before it lands: each readable function
against every recorded call of the original (the corpus replay), each game's logic against a reference run, and
A/B runs with the readable code on and off (`docs/READABLE.md`, "The gate"). You don't need the recordings to
contribute: say in your pull request what you ran, and the maintainers run the rest.

## Writing readable code

`docs/READABLE.md` is the guide. In short: no address literals (symbols, resolved per game), every name and type
for what it is (a wrong name is worse than none, so don't guess), code organized by subsystem in named files, and
game logic that reads as game logic, without the proof scaffolding. Identical engine code is shared across the
three games in `runtime/src/game/zge`; each game's own code is in `runtime/src/game/aggressive`, `dm2` and `bmx`.
The PlayStation 2 release of Aggressive Inline is the reference for behaviour. Fix causes, not symptoms.
What is known about a function (its purpose, the traps in changing it, the bugs kept, the layouts) is in the work
database's notes: `python3 -I tooling/dm2/tools/workdb.py card NAME` (or an address, `--build dm2_ps2` for DM2's)
and `workdb.py notes NAME` read their public export, no game data needed.

Libraries (Sony's SDK, libc, libm, libgcc) are never rewritten: they are ported from the pinned version's source,
with its notices kept (`NOTICE`).

## Sending a change

1. Fork the repository and make a branch from `main`, one topic per branch.
2. Commit with messages that say what changed and why. Keep unrelated clean-ups out.
3. Run the unit tests and the scan (above).
4. Open a pull request against `main` and fill in its template: what changed, why, how you tested it, and which
   games it touches.

Changes land in batches: a pull request that passes review is taken into the next batch, which is pushed only
when every check passes for all three games. That can take a few days; a check that fails comes back to you with
what it saw.

New settings and behaviour go behind a setting whose default keeps the games as they are.

## Issues

Use the forms: a bug (with the game, your disc's serial and its check, your system, the app's version and the
log), a readable-code report (a wrong name, unclear code, an address left in), or a feature idea. Never attach
game files or anything made from them.

## Licence

GPL-3.0 (`LICENSE`). By sending a change you agree that it is released under the same licence. Code from
elsewhere needs a GPL-3.0-compatible licence, its notice kept and a line in `NOTICE`.
