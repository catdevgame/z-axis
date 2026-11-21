# Native in place of the PS2: what replaced what

The design note for how the PC version leaves the PS2's machinery behind: for each part of the
console the game used, what replaced it, where that code is, and the switch that brings the PS2
path back for checks and comparisons. Settings and their environment variables are in `SETTINGS.md`; the GPU renderer's own design in `runtime/src/lib/gs/GPU_BACKEND.md`.

The rule behind all of it: native is the default, the strict PS2 path a
fallback and test mode only, and the defaults keep game logic identical to PCSX2 (logiccheck 21/21).
Replacements hook the game at the engine layer, where its intent is visible ("read this file",
"play this sample on channel n", "save this buffer"), so the PS2 protocol under it can go.

## The machinery

| PS2 part | Replaced by | Code (z-axis `runtime/src/game_overrides/`) | Back to the PS2 path |
|---|---|---|---|
| Memory card and its dialogs | the save jobs' host form, engine code (`runtime/src/game/zge/host/save_jobs.cpp`, over `game/host/save_files.h`): plain save files written at once (the card's data file's bytes), `.bak`, profiles (SETTINGS.md); AI's and BMX XXX's save screen form in `game/<game>/host/save_screen.cpp`; DM2's memory card frame's host form for the game save, `game/dm2/host/game_save.cpp`; AI's autosave | `zge_saves.cpp` (its switch; each game's part in `aggressive_inline_saves.cpp`, `bmx_xxx_saves.cpp`, `dave_mirra_saves.cpp`) | `ZX_SAVES=card` (HLE card folders, the console's dialogs) |
| Disc and the ZCDDVD client on the IOP | the device's host form, engine code (`runtime/src/game/zge/host/disc_device.cpp`, over `game/host/disc_files.h`): the game's file functions read the bundled files directly, with the `Mods` overlay | `zge_disc.cpp` (its switch; AI's additions in `aggressive_inline_disc.cpp`) | `[host] disc = ps2` (`ZX_DISC=ps2`) |
| MPEG movies (libmpeg/IPU, movie sound through SDRDRV) | the movie stream's host form, engine code (`runtime/src/game/zge/host/movie_stream.cpp`, over `game/host/movies.h`): native decoding with FFmpeg, the pictures through the game's own texture upload, sound mixed into the game's | `zge_movies.cpp` (its switch; AI's Xbox movies in `aggressive_inline_movies.cpp`, DM2's intro movies in `dave_mirra_movies.cpp`) | `ZX_MOVIES=hle` |
| Sound driver (ZAUDIO, LIBSD, SDRDRV on the IOP, the SPU2) | a native ZAudio driver behind the game's one RPC function, 48 kHz mixer, music, effects and speech buses | `zge_sound.cpp` (AI's ZAUDIO in `aggressive_inline_sound.cpp`) | `ZX_SOUND=spu2` (docs/SPU2.md) |
| The IOP | not run at all once the four above are native | `aggressive_inline_slus20327.cpp` | `ZX_IOP=1` runs the idle IOP anyway |
| Console texts and button pictures | the PC's wording and device-matched prompts, patched into the game's own scripts at load | `aggressive_inline_prompts.cpp`, `aggressive_inline_settings.cpp` | the developer runner leaves the menus alone unless `ZX_GAME_SETTINGS_MENUS=1` |

Kept as they are, because they are what keeps game logic identical: the EE kernel model and
scheduler, the pad HLE (the one input seam), VU1 and GS emulation for what is not drawn natively,
and the 60 Hz vsync. DM2 and BMX XXX have the same replacements (their saves in the first row): the disc device is
one engine body for all three games (zge/host, each game's instance at its own addresses), and so is
the movie stream (each game's additions in `aggressive_inline_movies.cpp`, `dave_mirra_movies.cpp` and
`bmx_xxx_native.cpp`); the sound driver is
one body too (`zge_sound.cpp`) with each game's ZAUDIO in `dave_mirra_sound.cpp` (DM2's older
streams) and `bmx_xxx_sound.cpp` (AI's with three differences); the IOP is stopped in
`dave_mirra.cpp` and `bmx_xxx.cpp`.

## Drawing

- The GPU renderer (SDL_GPU: Metal, Direct3D 12, Vulkan on Android) is the default; the CPU renderer
  is the accurate software reference and what logiccheck and the A/B use (`ZX_GS_BACKEND`).
- Native meshes, "observe and substitute": the game's own renderers run unchanged, so EE RAM and EE
  time do not change; where VIF1 reaches a node's packet, the VU1 work is dropped and a native draw
  goes into the GS stream at the same place. Natively drawn: rigid prelit meshes (render type 4),
  terrain with its grass shells (0x20), the sky (0x18) and skinned characters (8, 0xC). Still on the
  GS path: shadow volumes, particles and world strips, the fog planes (drawn again as one smooth pass
  by the picture quality options), 2D. Switch: `[graphics] render_path` / `ZX_RENDER_PATH`
  (SETTINGS.md "Native meshes").
- `render_path = records` (DM2's and BMX XXX's default, their `auto`; Aggressive Inline's on request): the engine's class renderers draw natively from their draw records
  (`runtime/src/game/zge/host/class_draws.cpp`, the host form beside `zge/render/ps2/class_draws.cpp`, over
  `game/host/native_draws.h`), in all three games: the mesh packet gets each draw's texture request and VU1 state and a
  VIF NOP where the draw stands (`PS2Memory::addVif1NativeAnchor`), not its VU1 work. Game RAM differs from the
  strict path only in the frame packets' buffers (the A/B's owner rule); the draw's DMA tag stands where the original
  called the part's packet for the held-register table too (`ee::held::placeAs`, docs/SCAFFOLDING.md), so the class
  renderers' callees save into their frames what they save on the strict path. A skinned mesh follows its game's packet builder
  (`zx::zaxis::SkinnedDrawForm`: ZGE2's dropped light bones, three weights and float UVs, ZGE4's three-bone
  limit); a part whose streams the game makes at run time (BMX XXX's riders, joined from the model's parts) is made
  from those streams when its packet is built. ZGE4's rigid parts with normals (`zx::zaxis::RigidDrawForm`) carry
  them where the colours go, lit from them with their node's lights (`GSNativeLights`: 255 times the light, alpha
  0xC5, as VU1 0x04 / 0x06 do). A part whose packet is rebuilt from changed streams (DM2's water) has its mesh
  decoded from that packet once per build. Models in asset libraries (DM2's `com\motions.zif`: ULIB, UAST, UMDL) are
  read like GEO files. Without the VU1 work the encoder still begins a DMA tag where the part's call would be: the
  draw's anchor then follows a chain called before it (DM2's and BMX XXX's riders and bikes after the shadow buffer
  is applied, not under it) as on the PS2 path. Left to the strict encoder: skinned parts rebuilt from changed
  streams (none seen); `ZX_TRACE_NATIVE=2` names each part left without a mesh or rebuilt, and why.
- Textures: the Xbox release's textures are the HD default wherever the Xbox has a counterpart, keyed
  by the game's own texture objects, not by GS memory (SETTINGS.md "Xbox textures, skaters and movies"). The
  runtime fetches VIF1's chain as the DMAC does, after the texture interrupt patches TEX0.
- Picture quality past the PS2 (FXAA, anisotropic filtering, smooth fog, sharp shadows, float depth,
  perspective-correct shading, the Xbox's character lighting), full height, widescreen, draw
  distance and LOD ends that follow it: settings, each off in the strict path (SETTINGS.md
  "Picture quality").
- `ZX_GSMEM_AUDIT=1` audits the game's GS texture memory per level (docs/DEVTOOLS.md); converted
  levels are made to fit at build time.

## Data on the host

- Assets from other releases (the three games' Xbox textures, AI's Xbox movies, the Zoo and the Ape, DM2's levels,
  Estate, BMX XXX's other releases' content) are made once into ready-made files: by the app's setup from the
  player's discs (`runtime/src/collection/extras/setup.cpp`), or at packaging time into the app
  (`tooling/ai/tools/make-app.sh`, `make-windows.ps1`); the running game never reads a disc. What it makes at start is made in memory from the
  installed files: a Custom Freeplay map converted for another game (docs/CUSTOM-LEVELS.md), DM2's rider creator's
  data.
- The readable code reads the level's collision and GEO meshes from host-side copies made from the
  files (`runtime/src/game/zge/native`): `ZX_NATIVE_COLLISION=0` falls back to the RAM copies (BMX XXX's
  default: its host data differs from its RAM copy in some reads; `=1` turns it on there, `=verify` compares each
  read), `ZX_NATIVE_COLLISION_CHECK` and `ZX_NATIVE_MESHES_CHECK` compare the two.
