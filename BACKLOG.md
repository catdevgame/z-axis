# Backlog

Open work for the Aggressive Inline PC version, and ideas parked on purpose. Only what is still to
do: what each finished round did is in its commit messages, per-function facts are notes in the
work database (`workdb.py notes ADDR`). How things work now: README.md, docs/BRINGUP.md,
docs/READABLE.md, docs/NATIVE.md.

## Now: AI's readable code

The direction (the user, 2026-11-05): finish Aggressive Inline's readable C++. Its source is this repository's
own, published with it (the user, 2026-11-17: the separate ai-decomp repository is replaced by this public tree).
Dave Mirra BMX 2 and BMX XXX get no new work for now; shared code changes must keep them from regressing
(dm2logic 6/6, DM2's corpus replay).

- What is left function by function: the coverage page (`tools/readable_map/update.sh`) and the work
  database (`readable`, `sweep_latest`, `stale_verified`, `uncovered`); the method is
  docs/READABLE.md.
- initZgui and finishUiImageLoad: their check needs the pinned replay checker to supply a register
  word their caller leaves. The user's call (a checker change), not asked yet.
- Dead engine rows stay generated (docs/DEAD-CODE.md).
- Paths no route reaches: the functions only a person can reach are in docs/MANUAL-TESTS.md; the
  rest have routes in `tools/routes.py` and the corpus's `cov-` routes.

## The extras: known gaps

Each extra is in the apps behind its setting (`SETTINGS.md`). None is being worked on; these
are the gaps they ship with.

- **The Zoo** (the cut level, from the Xbox release): no career goals on any disc and not in the
  career level select; no intro flight in split screen, whose eight Egg Hunt and Twenty One spots
  are the port's own; textures at most 128 texels on the PS2 renderer's side (the GPU renderer
  draws the Xbox originals).
- **Estate** (the GameCube disc's earlier Tutorial): lessons 9 and 21 cannot run as the level is,
  17 and 19 are placeholders in its own script; the lessons have not been played through by hand
  (MANUAL-TESTS.md); with `PS2X_SAVES=card` a save made in Estate holds its row in the Tutorial's
  place.
- **Dave Mirra BMX 2**: DM2's Devil's Peak overruns the GS CLUTs at Farthest with 32:9 (left as it
  is, the user).
- **The Xbox release's content** (the Ape, Xbox tuning, scripts, camera): the Ape's career is not
  saved with `PS2X_SAVES=card`.

## Ideas not started

- Custom levels, and a Custom Freeplay mode where you pick a skater or rider from any of the three
  games, then a custom map (the user, 2026-11-15): plan in docs/CUSTOM-LEVELS.md.

From a survey of other PC ports:

- Restart for a career challenge (Restart covers levels and the Timed Run; a challenge needs its
  giver's start path).
- Per-texture replacement packs with PCSX2's names (`<texhash>-<cluthash>-<bits>.png`), and a dump
  mode. `tools/share/zaxis_texdump.py` already dumps and puts back whole texture files.
- Practice tools: a timer, position save and load, savestates.
- Replays of a run from its recorded input (the game's logic is deterministic).
- Gyro camera and a Steam Deck profile.
- Music: more than 13 songs, shuffle, per-song toggles (the custom soundtrack replaces songs only).
- A mod manager: per-mod folders or zips, switched on and off in a settings page.
- Local achievements.
- A first-run setup that builds the game files from the player's own disc image, for anyone without
  this workshop.
- Above 60 frames per second by interpolation on the render side only.

Rendering, measured and judged not worth it for now:
native shadow volumes (at most 0.2 ms a vsync to gain), a native sky clipper (fixes nothing
visible). Particles, world strips and 2D stay on the GS path, already at the internal resolution.

## Parked by the user

- **Online play from split screen** (2026-10-22): both PCs run the split-screen game in input
  lockstep, each drawing only its own player's view; Discord for lobbies. Risks: determinism across
  Mac and Windows, latency beyond input delay, NAT traversal. No study run yet.
- **BMX XXX**, the third game on the engine, after DM2. What is known: its
  PS2 build keeps a complete park editor behind a skipped menu entry, its GameCube disc has four test
  levels the PS2 lacks, and it pairs closely with AI in the work database. Details:
  tooling/dm2/MATCH3.md.
- Probably never: more than two split-screen players, Discord rich presence, higher-resolution
  movies, a park editor overhaul, RetroAchievements, a new renderer or new models.
