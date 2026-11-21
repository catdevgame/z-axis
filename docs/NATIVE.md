# Native in place of the PS2: what replaced what

For each part of the console the games use: what replaced it, where that code is, and the switch that brings the
PS2 path back for checks and comparisons. The same parts are replaced in all three games. Settings and their
environment variables are in `docs/SETTINGS.md`; the GPU renderer's design is in `docs/GPU_BACKEND.md`.

Native is the default. The PS2 path is a fallback and a test mode only. With the defaults, game logic stays identical
to the original's as PCSX2 runs it (the logic checks compare the two). A replacement hooks the game at the engine
layer, where its intent is plain ("read this file", "play this sample on channel n", "save this buffer"), so the PS2
protocol under it can go.

## The machinery

| PS2 part | Replaced by | Code (z-axis `games/<game>/overrides/`, `engine/overrides/`) | Back to the PS2 path |
|---|---|---|---|
| Memory card and its dialogs | plain save files written at once (the card's data file's bytes), `.bak`, profiles (docs/SETTINGS.md): the save jobs' host form, engine code (`engine/zge/host/save_jobs.cpp`, over `engine/host/save_files.h`); AI's and BMX XXX's save screen in `games/<game>/host/save_screen.cpp`; DM2's memory card frame in `games/dm2/host/game_save.cpp` (the game save, the auto load, the start check) and `slot_saves.cpp` (replays and parks, a file a slot); AI's autosave | `zge_saves.cpp` (the switch); each game's part in `aggressive_inline_saves.cpp`, `dave_mirra_saves.cpp`, `bmx_xxx_saves.cpp` | `ZX_SAVES=card` (HLE card folders, the console's dialogs) |
| Disc and the ZCDDVD client on the IOP | the game's file functions read the installed files directly, with the `Mods` overlay: the disc device's host form, engine code (`engine/zge/host/disc_device.cpp`, over `engine/host/disc_files.h`), one body for the three games | `zge_disc.cpp` (the switch); AI's additions in `aggressive_inline_disc.cpp`, DM2's in `dave_mirra_disc.cpp`, BMX XXX's in `bmx_xxx_native.cpp` | `[host] disc = ps2` (`ZX_DISC=ps2`) |
| MPEG movies (libmpeg/IPU, movie sound through SDRDRV) | native decoding with FFmpeg, the pictures through the game's own texture upload, the sound mixed into the game's: the movie stream's host form, engine code (`engine/zge/host/movie_stream.cpp`, over `engine/host/movies.h`), one body for the three games | `zge_movies.cpp` (the switch); AI's Xbox movies in `aggressive_inline_movies.cpp`, DM2's intro movies in `dave_mirra_movies.cpp`, BMX XXX's in `bmx_xxx_native.cpp` | `ZX_MOVIES=hle` |
| Sound driver (ZAUDIO, LIBSD, SDRDRV on the IOP, the SPU2) | a native ZAudio driver behind the game's one RPC function: a 48 kHz mixer with music, effects and speech buses | `zge_sound.cpp`, one body; each game's ZAUDIO in `aggressive_inline_sound.cpp`, `dave_mirra_sound.cpp` (DM2's older streams), `bmx_xxx_sound.cpp` (AI's with three differences) | `ZX_SOUND=spu2` (docs/SPU2.md) |
| The IOP | not run at all once the four above are native | stopped in `aggressive_inline_slus20327.cpp`, `dave_mirra.cpp`, `bmx_xxx.cpp` | `ZX_IOP=1` runs the idle IOP anyway |
| Console texts and button pictures | the PC's wording and prompts that match the device, patched into the game's own scripts at load | `zge_prompts.cpp`, one body; each game's in `aggressive_inline_prompts.cpp`, `dave_mirra_prompts.cpp`, `bmx_xxx_prompts.cpp`; AI's script patches in `aggressive_inline_settings.cpp` | the developer runner leaves the menus alone unless `ZX_GAME_SETTINGS_MENUS=1` |

A recording made before DM2's replays, parks, auto load and start check went native (1.2.0 and earlier) plays back
differently from the point it opens one of them (the card dialogs it saw are gone); the playback page warns about
recordings from another version.

Kept as they are, because they keep game logic identical: the EE kernel model and scheduler, the pad HLE (the one
input seam), VU1 and GS emulation for what is not drawn natively, and the 60 Hz vsync.

## Drawing

- The GPU renderer (SDL_GPU: Metal, Direct3D 12, Vulkan on Android) is the default. The CPU renderer is the
  accurate software reference, and the logic checks and the A/B use it (`ZX_GS_BACKEND`).
- Native meshes, "observe and substitute": the game's own renderers run unchanged, so EE RAM and EE time do not
  change. Where VIF1 reaches a node's packet, the VU1 work is dropped and a native draw goes into the GS stream at
  the same place. In Aggressive Inline these are drawn natively: rigid prelit meshes (render type 4), terrain with
  its grass shells (0x20), the sky (0x18) and skinned characters (8, 0xC). Still on the GS path: shadow volumes,
  particles and world strips, the fog planes (drawn again as one smooth pass by the picture quality options), 2D.
  Switch: `[graphics] render_path` / `ZX_RENDER_PATH` (docs/SETTINGS.md "Native meshes").
- `render_path = records` (DM2's and BMX XXX's default, their `auto`; Aggressive Inline's on request): the engine's
  class renderers draw natively from their draw records, in all three games
  (`engine/zge/host/class_draws.cpp`, the host form beside `zge/render/ps2/class_draws.cpp`, over
  `engine/host/native_draws.h`). The mesh packet gets each draw's texture request and VU1 state, and a VIF NOP where
  the draw stands (`PS2Memory::addVif1NativeAnchor`), not its VU1 work.
  - Game RAM differs from the strict path only in the frame packets' buffers (the A/B's owner rule). The draw's DMA
    tag stands where the original called the part's packet, for the held-register table too
    (`ee::held::placeAs`, docs/SCAFFOLDING.md), so the class renderers' callees save into their frames what they
    save on the strict path.
  - A skinned mesh follows its game's packet builder (`zx::zaxis::SkinnedDrawForm`: ZGE2's dropped light bones,
    three weights and float UVs; ZGE4's three-bone limit). A part whose streams the game makes at run time (BMX
    XXX's riders, joined from the model's parts) is made from those streams when its packet is built.
  - ZGE4's rigid parts with normals (`zx::zaxis::RigidDrawForm`) carry the normals where the colours go, and are lit
    from them with their node's lights (`GSNativeLights`: 255 times the light, alpha 0xC5, as VU1 0x04 / 0x06 do).
  - A part whose packet is rebuilt from changed streams (DM2's water) has its mesh decoded from that packet once per
    build. Models in asset libraries (DM2's `com\motions.zif`: ULIB, UAST, UMDL) are read like GEO files.
  - Without the VU1 work the encoder still begins a DMA tag where the part's call would be, so the draw's anchor
    follows a chain called before it, as on the PS2 path (DM2's and BMX XXX's riders and bikes after the shadow
    buffer is applied, not under it).
  - Left to the strict encoder: skinned parts rebuilt from changed streams. `ZX_TRACE_NATIVE=2` names each part
    left without a mesh or rebuilt, and why.
- Textures: the Xbox release's textures are the HD default wherever the Xbox has a counterpart, keyed by the game's
  own texture objects, not by GS memory (docs/SETTINGS.md "Xbox textures, skaters and movies"). The runtime fetches
  VIF1's chain as the DMAC does, after the texture interrupt patches TEX0.
- Picture quality past the PS2 (FXAA, anisotropic filtering, smooth fog, sharp shadows, float depth,
  perspective-correct shading, the Xbox's character lighting), full height, widescreen, draw distance and LOD ends
  that follow it: settings, each off in the strict path (docs/SETTINGS.md "Picture quality").
- `ZX_GSMEM_AUDIT=1` audits the game's GS texture memory per level (docs/DEVTOOLS.md). Converted levels are made to
  fit at build time.

## Data on the host

- Assets from other releases (the three games' Xbox textures, AI's Xbox movies, the Zoo and the Ape, DM2's levels,
  Estate, BMX XXX's other releases' content) are made once into ready-made files: by the app's setup from the
  player's discs (`collection/extras/setup.cpp`), or at packaging time into the app
  (`platform/mac/make-app.sh`, `make-windows.ps1`). The running game never reads a disc. What it makes at start
  is made in memory from the installed files: a Custom Freeplay map converted for another game
  (docs/CUSTOM-LEVELS.md), DM2's rider creator's data.
- The readable code reads the level's collision and GEO meshes from host-side copies made from the files
  (`engine/zge/native`). `ZX_NATIVE_COLLISION=0` falls back to the RAM copies. That is BMX XXX's default,
  because its host data differs from its RAM copy in some reads; `=1` turns it on there, `=verify` compares each
  read. `ZX_NATIVE_COLLISION_CHECK` and `ZX_NATIVE_MESHES_CHECK` compare the two.
