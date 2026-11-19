# The documentation

Where to start, by what you want to do. Each guide says the current method and state; none of them is a log.

## Playing

- `README.md` (the repository's front page): getting the app, opening it the first time on a Mac or on Windows,
  and setting the games up from your own discs.
- `CONTROLS.md`: keyboard, mouse and gamepad, rebinding, `controls.ini`, photo mode, mouse-look.
- `SETTINGS.md`: every page and row of the Settings screen in the games' own menus, what each does and where it
  is kept (`settings.ini`), and the extras each game offers (the Zoo, Estate, DM2's Xbox levels, BMX XXX's other
  releases' content).
- `docs/CHEATS.md`: the games' cheat codes and the Settings > Cheats page.
- `android/README.md`: the build for dual-screen Android handhelds (a release APK from 1.1.0, or built from source).

## Modding

- `docs/MODS.md`: what a mod is, `mod.ini`, what a mod can hold, the Mods page.
- `docs/CUSTOM-LEVELS.md`: custom maps and the Custom Freeplay mode: the map format across the three games, the
  converters, the limits.
- `docs/TUNING.md`: every entry of Aggressive Inline's tuning table, which `tuning.ini` and the debug menu change.
- `docs/DEVTOOLS.md`: the games' hidden debug menus, `tuning.ini`, texture dumps and packs, and the developer
  runner's switches.
- `docs/DEAD-CODE.md`: functions no game path reaches, and the cut or unfinished features among them.
- `docs/ENHANCEMENTS.md`: what the collection adds to each game, and what is left to port.

## Contributing

- `CONTRIBUTING.md`: the one rule (no game data), the tests, the scan, how to send a change.
- `docs/BRINGUP.md`: development builds, the recompile, running one game, the checks.
- `docs/READABLE.md`: writing a readable function and how it is proven (the recordings, the corpus replay, the
  gate).
- `docs/SCAFFOLDING.md`: the temporary forms the readable source still uses, and what removes each
  (`tooling/checks/scaffolding.py` reads this file).
- `docs/NATIVE.md`: what replaced the PS2's machinery (saves, disc, movies, sound, drawing) and the switches back.
- `docs/SPU2.md`: the emulated sound path, the fallback to the native driver.
- `runtime/src/lib/gs/GPU_BACKEND.md`: the GPU renderer's design. `runtime/tools/VU1_AOT.md`: VU1 programs
  compiled ahead of time. `iop/README.md`: the IOP side.
- `tooling/README.md`: the tooling's layout. Each game's workspace: `tooling/ai/README.md` (Aggressive Inline alone
  as an app, its saves and automation), `tooling/dm2/BRINGUP.md` (DM2 and BMX XXX: build, run, check); the work
  database and the matching of the nine builds: `tooling/dm2/MATCH3.md`; the recompiler:
  `tooling/recompiler/ps2xRecomp/README.md`; the PCSX2 reference runs the logic checks replay:
  `tooling/ai/tools/REFERENCE-RUN.md`, `tooling/dm2/tools/PCSX2-DM2.md`; AI's split assembly and original toolchain:
  `tooling/ai/decomp/README.md`; the GS and VU1 checks: `tooling/checks/GS_REPLAY.md`, `VU1_CORPUS.md`.
