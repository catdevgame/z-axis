## What changed and why

<!-- One topic per pull request. Name the games it touches (Aggressive Inline, DM2, BMX XXX, the collection). -->

## How it was tested

<!-- The unit tests (zx-tests from the repository's root), a game run, anything else you ran. The maintainers
run the corpus replays, logic checks and A/Bs before it lands. -->

## Checklist

- [ ] No game data: no disc files, saves, generated code, recordings, dumps, or pictures of the games.
- [ ] `python3 -I tools/checks/public_scan.py --added origin/main` and `--tree HEAD`: 0 failing.
- [ ] `zx-tests` passes.
- [ ] Game code follows `docs/READABLE.md` (no address literals, no guessed names, no scaffolding in game logic).
- [ ] New behaviour is behind a setting whose default keeps each game as it is.
- [ ] Docs changed where the behaviour or the way to build changed.
