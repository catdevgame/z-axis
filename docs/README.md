# The documentation

Where to start, by what you want to do.

## Playing

- `README.md` (the repository's front page): getting the app, opening it the first time on a Mac or on Windows,
  and setting the games up from your own discs.
- `docs/CONTROLS.md`: keyboard, mouse and gamepad, rebinding, `controls.ini`, photo mode, mouse-look.
- `docs/SETTINGS.md`: every page and row of the Settings screen in the games' own menus, what each does and where it
  is kept (`settings.ini`), and the extras each game offers (AI's Zoo and Estate, DM2's Xbox levels, BMX XXX's other
  releases' content).
- `docs/CHEATS.md`: the games' cheat codes and the Settings > Cheats page.
- `platform/android/README.md`: the build for dual-screen Android handhelds (the release APK, or one built from source).

## Modding

- `docs/MODS.md`: what a mod is, `mod.ini`, what a mod can hold, single entries of an archive and the `zal` tool,
  `tuning.ini` in all three games, the Mods page.
- `docs/CUSTOM-LEVELS.md`: custom maps and the Custom Freeplay mode: the map format across the three games, the
  converters, the limits.
- `docs/TUNING.md`: every entry of Aggressive Inline's tuning table, which `tuning.ini` and the debug menu change.
- `docs/DEVTOOLS.md`: the games' hidden debug menus, `tuning.ini`, texture dumps and packs, the developer
  runner's switches, and the disc and archive tools (`zx-disc`, `zal`).
- `docs/DEAD-CODE.md`: functions no game path reaches, and the cut or unfinished features among them.
- `docs/ENHANCEMENTS.md`: what the collection adds to each game, and what is left to port.

## Contributing

- `CONTRIBUTING.md`: the one rule (no game data), the tests, the scan, how to send a change.
- `docs/BRINGUP.md`: development builds, the recompile, running one game, the checks.
- `docs/READABLE.md`: writing a readable function and how it is proven (the recordings, the corpus replay, the
  gate).
- `docs/SCAFFOLDING.md`: the temporary forms the readable source still uses, and what removes each
  (`tools/checks/scaffolding.py` reads this file).
- `docs/NATIVE.md`: what replaced the PS2's machinery (saves, disc, movies, sound, drawing) and the switches back.
- `docs/SPU2.md`: the emulated sound path, the fallback to the native driver.
- `docs/GPU_BACKEND.md`: the GPU renderer's design. `docs/VU1_AOT.md`: VU1 programs
  compiled ahead of time. `iop/README.md`: the IOP side.
- `tools/README.md`: the tools, by purpose. Each game's tools and workspace: `games/aggressive/README.md` (Aggressive Inline alone
  as an app, its saves and automation), `games/dm2/docs/BRINGUP.md` (DM2 and BMX XXX: build, run, check); the work
  database and the matching of the nine builds: `docs/MATCH3.md`; the recompiler:
  `recompiler/ps2xRecomp/README.md`; the PCSX2 reference runs the logic checks replay:
  `games/aggressive/docs/REFERENCE-RUN.md`, `games/dm2/docs/PCSX2.md`; AI's split assembly and original toolchain:
  `games/aggressive/workspace/decomp/README.md`; the GS and VU1 checks: `docs/GS_REPLAY.md`, `VU1_CORPUS.md`.
