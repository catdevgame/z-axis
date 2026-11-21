# GS hardware renderer on SDL_GPU

`GSGpuBackend` (`ps2/gs/gs_gpu_backend.h`, `gs_gpu_backend.cpp`,
shaders in `gs_gpu_shaders_msl.h` and `gs_gpu_shaders.hlsl`) is a `GSRasterBackend` that draws GS
primitives with SDL3's GPU API instead of the CPU rasterizer, at the GS
resolution or an integer multiple of it (2x, 3x, 4x ...), the way PCSX2's
hardware renderer does. Metal on macOS (hand-written MSL), Direct3D 12
on Windows and Vulkan on Android (the same shaders in HLSL, compiled to DXIL
or SPIR-V at build time).

It draws the three games on the GPU and the window shows the upscaled picture
straight from it (no read-back), with widescreen, movies and the front end. It
is the app's default (GPU at 3x, 2x on Android; `settings.ini` without a
`renderer` line), and two Display settings switch it while the game runs
(Renderer CPU or GPU, Resolution Native to 4x). It is not bit-exact: that is
the CPU renderer's job, and the developer runner and the test tools use the
CPU renderer unless `ZX_GS_BACKEND=gpu`.

## Build and use

SDL3 is the host layer's pinned dependency (`third_party/cmake/zx_sdl3.cmake`,
3.4.18, static). `-DZX_GS_GPU=OFF` builds without the GPU backend. The backend is the library
`zx_gs_gpu`, linked into `zx-gsreplay`, `zx-runner` and the unit tests (`zx-tests`) only.

```sh
# offline, headless (no window): PNGs at the internal resolution
zx-gsreplay trace.gsr out/prefix --gpu --scale 3
# against the CPU renderer: mean / max per-channel error per frame and
# out/prefix-<n>-cmp.png = CPU | GPU | difference x4
zx-gsreplay trace.gsr out/prefix --gpu --compare [--scale n]
# in the game (opt-in)
ZX_GS_BACKEND=gpu ZX_GS_SCALE=3 zx-runner ...
```

| Variable | Effect |
|---|---|
| `ZX_GS_BACKEND=gpu` | runner: GPU renderer (falls back to the CPU one if no device); the app sets it from `settings.ini` `[graphics] renderer` (default `gpu`) unless it is set; `cpu` pins the CPU renderer |
| `ZX_GS_SCALE=n` | runner: internal resolution multiplier (default 1; the app: `internal_resolution`, 1-4, default 3, 2 on Android) |
| `ZX_GS_FULL_HEIGHT=1` | twice that multiplier vertically (see "Full height"; the app: `full_height`, default on) |
| `ZX_GS_PIPELINE_CACHE=<file>` | the game's pipeline file (see "GPU objects made off the GS worker"; the app gives each game `games/<id>/gpu_pipelines.txt`) |
| `ZX_GS_DEPTH16=fast` (or `exact`, `1`, `0`) | the Depth Test setting (`[graphics] depth_test`, a Display row; Fast by default on Android, Exact elsewhere; read at the renderer's start). Fast, in a game whose override allows it (`ZX_GS_D16_TARGETS=1`: BMX XXX): 16-bit Z buffers as D16 targets (Z stored as Z / 65535): the GPU rounds depth to the GS's steps and the fragment shader writes none, which keeps the GPU's early depth test (a shader that writes depth shades every layer of overdraw, which is slow on a handheld's GPU). Not exact: the GPU compares and rounds the interpolated depth itself, so at near-equal surfaces a few edge pixels resolve otherwise than with the shader's rounding. Not for games with native draws (declined on a D16 target: their precise depth is a float one) |
| `ZX_GS_DRAW_MEMO=0` | every primitive through drawPrepared (the draw memo off: consecutive primitives of one draw state in a GIF packet take the last one's draws and bookkeeping); `ZX_GS_DRAW_MEMO_CHECK=1` draws through drawPrepared and counts primitives whose draws differ from the memo's (`[gs-gpu] draw memo check: N primitives checked, 0 differ`) |
| `ZX_GS_SHADER_DEPTH=0` | for comparison: no depth written by the fragment shader for GS-path draws (the GPU's interpolated depth, not rounded to the Z buffer's steps) |
| `ZX_GS_STATS=1` | per presented frame: primitives, draw calls, render passes, shader-blended primitives, CPU-drawn primitives, texture uploads, target write-backs / loads, submits |
| `ZX_GS_GPU_TRACE=1` | logs every target load, write-back and shrink with its cause |
| `ZX_GS_GPU_DEBUG=1` | SDL_GPU validation (private device only) |
| `ZX_GS_DECODE_PACK_BLOCKS=1` | replacement pictures in BC1, BC2, BC3 and BC7 (texture packs' DDS pictures, the Xbox pictures' BC2) decoded to RGBA8 when uploaded, as on a device that samples none of those formats |
| `ZX_GS_NO_TEXTURE_POOL=1`, `ZX_GS_NO_TEXTURE_STOCK=1`, `ZX_GS_MESH_BUFFER_EACH=1` | for comparison (see "GPU objects made off the GS worker"): textures given back are released, no textures made ahead, a buffer per native mesh |
| `ZX_GPU_TEXTURE_CHECK=1` | texture handle check (gs/gs_gpu_texture_check.h, the GS renderer's and the window's textures): released handles kept 2 s, a destroyed target's handle replaced by a token kept 2 s, and any bind, blit, copy, upload, download or target use of either (or of a null or never-made handle) reported on stderr as `[gpu-check] ...` with the stacks of the use and of the release; a count when the device goes. Off by default (then the plain SDL calls) |

## Design

### Local memory stays on the CPU

The backend owns a `GSCpuBackend` on the same 4 MiB local memory and leaves
every memory-side operation to it: host-to-local, local-to-host and
local-to-local transfers, CLUT loads (CSM1/CSM2, CLD, the texture cache
staleness model), `ReadVram`/`WriteVram`, `SnapshotVram`, trace CLUT state.
Only primitives are drawn on the GPU. Local memory is therefore always
correct where no GPU drawing is pending, which keeps the hard GS cases
(transfers into frame buffers, CLUTs and textures inside frame buffers,
readbacks) simple and correct, at the price of a GPU round trip when they
happen.

### Render targets

A target is a GPU texture standing in for a GS frame or Z buffer: block
address, buffer width, storage class (CT24 shares CT32's target), size
`FBW*64 x height` GS pixels times the scale (times twice the scale
vertically with full height, below). Colour targets are RGBA8, Z
targets D32F. Height is the scissor's (the buffer's in practice), grows when
a draw goes lower, and shrinks when another buffer is written over its lower
pages (a target left 448 lines tall from a loading screen would otherwise
overlap the menu's Z buffer and both would reload on every draw). Colour and
Z attachments of a pass always have the same size.

Each target tracks:

- `pages`: the GS pages its area covers;
- `dirty` / `dirtyPages`: the rectangle drawn since it was last written back
  (and the pages of that rectangle): there the GPU is newer than memory;
- `stale`: memory is newer (something else wrote there); it reloads from
  memory before its next use.

Rules:

- Before anything reads local memory (transfer sources, CLUT loads, texture
  decodes, `ReadVram`, snapshots), targets dirty in those pages are written
  back: their dirty rectangle is downloaded (at scale > 1 the centre sample
  of each GS pixel), converted to the buffer's format (5551 for 16-bit, Z
  rounded to the format's integer) and stored with the swizzled writers.
- Before anything writes local memory, the same write-back, then the targets
  over the written pages are marked stale (or shrunk, see above).
- A new or stale target is loaded from memory: decoded at 1x into a staging
  texture and blitted (nearest) to the scaled target; Z through a small
  pass that writes depth.
- A draw marks its target's dirty rectangle; other targets over those pages
  go stale.

### Textures

- From memory: decoded on the CPU into a GPU texture of TW x TH texels:
  RGBA8 for CT32/CT24/CT16/CT16S (5551 expanded, alpha bit as 0x80) and the
  Z formats, R8_UINT indices for T8/T8H/T4/T4HL/T4HH. Cached by TEX0
  address, width, format and size; an entry is decoded again when any of
  its pages was written since (page generation counters).
- Palettes: 256 x 1 RGBA8 textures built from the CPU backend's CLUT buffer
  exactly as `LookupCLUT` reads it (CSA, CSM1 entry swizzle, 16-bit CLUTs),
  shared by content. The CLUT epoch moves only when a load is due (CLD and
  CBP0/1 as the CPU backend decides), a palette is reused while the
  buffer's contents stay the same, and kept by the last load's source (CBP,
  formats, CSA, TEXCLUT) and the newest generation of its pages: a model
  whose materials each reload their own CLUT gets each palette back without
  a rebuild.
- From a render target ("render to texture"): when the texture has the
  target's storage class and buffer width and starts at a page row of it,
  the shader samples the target itself at the internal resolution
  (Aggressive Inline's previous-frame blend and skater shadow use this, so
  they stay sharp when upscaled). If the draw's own target is the texture, the target is
  copied first. Other overlaps (for example a T8 texture over a CT32
  buffer) write the target back and decode from memory.
- Sampling is done in the shader with texel fetches, as the CPU renderer
  does it: point sampling truncates; bilinear samples at -0.5 and rounds;
  CLAMP modes REPEAT, CLAMP, REGION_CLAMP and REGION_REPEAT; TEXA (TA0, TA1,
  AEM) for 24- and 16-bit texels and CLUT entries; TCC and the four TFX
  functions; fog. Texture coordinates follow the CPU renderer: STQ
  interpolated linearly then divided per pixel, UV truncated to 1/16 for
  triangles, sprite UV rounded to 1/16. The GS's own mipmapping (MXL, the
  MIPTBP1/2 levels, LCM, L and K) is chosen per pixel as the CPU renderer
  chooses it (`gsMipSelect`, gs/gs_types.h).
- Upscaled and bilinear, coordinates are clamped to the texel range of the
  primitive (half a texel in), so quads do not filter in texels from their
  neighbours (the seams PCSX2 users know).

### Replacement pictures

`runtime/texture_replace.h` holds host-side pictures that stand in
for GS textures (the Xbox releases' textures in all three games, and
Aggressive Inline's 640x480 Xbox movie frames). Nothing of the game changes: its RAM, GS memory and
CLUTs stay as they are, the GPU renderer only draws another picture.

- **Key: the game's own texture objects.** A game override tells the
  backend, in stream order, which texture of the game's GS memory holds
  (`GSTextureObjectNote` through `PS2Memory::postGsSideband` and
  `GS::noteTextureObjects`: object, mip level, block address and allocated
  blocks, size, PSM, and the picture key). Each game wraps its texture
  cache's level allocator (placeTextureLevel, by symbol per game in
  `engine/overrides/zge_xbox_textures.cpp`, called by the VIF1
  interrupt handler when it gives a level a GS address and patches TEX0 /
  MIPTBP1); the key is the object's level 0 as the game holds it in RAM:
  TW x TH and the FNV-1a 64 of its indices (one byte per texel, row order,
  T4 low nibble first), the same key the pack's entries have. A draw is
  matched by TEX0's TBP0 (a note for level 0 with the same PSM and size)
  or, when only mip levels 1 and up are resident (the game's distant
  models, TBP0 = 0), by MIPTBP1's TBP1 (a level 1 note). The PS2 texture
  is then not decoded at all. A note holds until another note covers its
  blocks, or a transfer other than its own upload (same start, PSM and
  size, at 0,0), a new render target or a pixel write touches them, or the
  backend resets. GS memory contents never decide which picture is drawn,
  so the game's texture cache can move, flush and refill textures (fast
  turns flush it every frame) without a picture landing on the wrong
  surface.
- **Content keys (fallback):** with `ZX_TEXTURE_REPLACE_CONTENT=1` an
  indexed texture no note names is also keyed by its decoded indices'
  FNV-1a 64 at each decode. RGB textures (only while some source has rgb
  entries: movie frames) are always keyed by the FNV-1a of the R, G, B
  bytes of all rows but the last.
- **Lookup:** per note (or `TexEntry`), again only when the registry's
  generation changes (a pack loaded, the setting switched). Not replaced:
  a texture drawn with a palette other than the first one it was drawn
  with (CLUT animation), and REGION_REPEAT with a mask that is not a
  power-of-two cell (REGION_REPEAT over the whole texture is drawn as
  REPEAT; over a 2^k cell at a FIX on a cell boundary, how Aggressive
  Inline repeats one picture of an atlas, the shader wraps in the cell). Render
  targets used as textures are never replaced.
- **Upload:** BC2 (DXT3) with its mip chain, or RGBA8 (a texture pack's
  DDS pictures also BC1, BC3 and BC7 with the levels the file has, decoded
  to RGBA8 on a device that does not sample that format), uploaded on first
  use (one GPU texture per picture; a texture and its PS2 mip levels share
  one), at most 24 MB per presented frame (the PS2 texture is drawn until
  its turn). Given back when the registry drops the picture.
- **Sampling** (texture kind 3): normalized coordinates u / TW, v / TH, so
  a picture of any size maps onto the PS2 UV space; trilinear with 8x
  anisotropic filtering whatever the game's filter, the mip level from the
  derivatives of the unclamped UV; REPEAT / CLAMP by the sampler,
  REGION_CLAMP, a REGION_REPEAT cell and the primitive's texel range
  clamped half a replacement texel in. The CLUT is not used; TFX, TCC, TEXA (16-bit CLUTs: the alpha
  bit from alpha >= 0.5), alpha test, fog and blending are as for the PS2
  texture. Alpha 1.0 is the PS2 palette's largest alpha (0x7F in
  Aggressive Inline, which keeps the skater shadow's counter in the frame
  buffer's alpha; 0x80 for movie frames).
- **Diagnostics:** `ZX_TRACE_TEXTURE_REPLACE=1` (pictures found, turned
  off by a second palette or a region repeat; 2: every indexed and RGB
  decode with its hash; 3: palettes), `ZX_GS_REPL_TINT=1` (replaced
  surfaces red; 2: indexed textures without a picture blue),
  `ZX_GS_REPL_NO_MIPS=1`, `ZX_GS_REPL_LOD_BIAS=<f>`,
  `ZX_GS_REPL_NO_ANISO=1`; `ZX_GS_STATS=1` adds replaced primitives,
  bytes uploaded, the pictures held on the GPU, the draws found by a note
  and the notes held. `ZX_TRACE_TEXTURE_REPLACE=3` also logs each note.
- **Texture packs** (`runtime/texture_pack.h`, docs/SETTINGS.md "Texture packs"): a draw's
  texture is named as PCSX2 names it (XXH3-64 of its GS memory blocks or of its texels read
  out, of its palette as 32-bit entries, PSM/TW/TH/TEXA, a region from CLAMP; a mipmapped one
  by each base level alone and with the levels below it folded in, as PCSX2 with hardware
  mipmapping names a draw from its nearest level), once per decode and palette, and a pack's
  picture of that name is drawn as above (kind 3, its alpha the PS2's: `f1.w` 255; a region's
  picture over that rectangle: `f2`). It comes before the Xbox pictures (the texture is then
  decoded first). Not named: textures read from a render target, levels in one (but a level
  0 at block 0, DM2's level textures', which is skipped), palettes held on the GPU. With no
  pack and no dump folder nothing is hashed; with one, a name is hashed only when some pack has
  a picture of its shape (bits and region), nearest base first. Pack pictures upload at most
  6 MB per presented frame (`ZX_TEXTURE_PACK_UPLOAD_MB`), so a level's first frames take
  them a few at a time. `ZX_GS_STATS=1` adds `pack=lookups/hashes` and their time.
- The CPU renderer ignores the Xbox replacements (it is the bit-exact reference); a texture
  pack's pictures it draws at the PS2's size, box-filtered into the texture's colours (the
  fast path's colour builds; the reference rasterizer's few draws keep the PS2 texture).

### Drawing

Primitives are converted on the CPU into triangle lists (sprites become two
triangles, points a 1x1 sprite, lines a GPU line list), with flat shading
resolved per vertex, and appended to the open render pass.

2D rectangles drawn as triangles: the front end, HUD panels and text are
quads of two triangles whose edges often lie on a pixel's sample point
(x = 639.5, y = 223.75; pixels are sampled at x + 0.5 like the CPU
renderer). The CPU renderer covers such a pixel (inclusive edges). The GPU
would not, and upscaled it would cover part of it: a thin dark line at the
right and bottom of 2D screens. A triangle with one Z (and one Q) whose legs are
horizontal and vertical (within 1/16 pixel) gets its corners moved to the
edges of the native pixels the CPU renderer covers (left/top edge
ceil(e - 0.5), right/bottom edge floor(e - 0.5) + 1) and every attribute
(colour, STQ/UV, fog) extended along its plane, so the samples inside still
see the original triangle's values; both halves of a quad share the same
corners. This is PCSX2's "align sprite" idea applied by coverage instead of
a global half-pixel offset, which would move every 3D edge off the CPU
renderer's. 3D triangles (varying Z) are left alone. Sprites already cover whole native
pixels (integer corners, as in the CPU renderer). Consecutive
primitives with the same pipeline, uniforms, textures, scissor and blend
constants become one draw call. A pass covers one colour target (and its Z
target); it is recorded when something needs the GPU in order (a different
target, a copy, a load, a write-back, presentation). Vertex data and texture
uploads go into one copy pass in front of each render pass.

Pipeline state:

- Z test NEVER (draws nothing), ALWAYS, GEQUAL, GREATER; ZMSK. Z is written
  from the fragment shader, rounded to the integer the GS stores, so equal
  Z compare like on the GS (exact for Z16 and Z24, Z32 loses its low bits).
- Alpha test, all eight ATST methods; AFAIL KEEP, FB_ONLY, ZB_ONLY and
  RGB_ONLY by drawing the primitive twice (passing pixels with all writes,
  failing pixels with the AFAIL masks).
- FBMSK: whole channels with the colour write mask; partial masks in the
  shader (below). FBA. RGB-only writes keep the destination alpha.
- Alpha blending: `(A - B) * C >> 7 + D` with A, B, D from Cs, Cd, 0 and C
  from As or FIX is mapped onto the blend unit: the shader computes the
  source term exactly (`Cs * C >> 7`, `Cs + ...`, `Cs - ...`) and the blend
  unit adds, subtracts or reverse-subtracts the destination term with factor
  1, As/128 (shader alpha) or FIX/128 (blend constant). The stored alpha stays
  the source alpha through the alpha blend constant 128/255.
- COLCLAMP=0 (sums wrap): for a flat untextured "Cs * C + Cd" (the shadow
  volumes add 8 and 248) the term is added, or subtracted as 256 - term, which
  is exact while the sum does not wrap; anything else goes to the shader path.
  The term needs the primitive's colour and alpha exactly: see "Shadows" under
  Native draws.
- Shader path ("slow"): PABE, C = Ad, the `(Cd - Cs) * C + Cd` and
  `Cd * C + Cd` forms, FIX > 128, FBA with an As factor, partial FBMSK
  (the shadow counter's 0x80808080), DATE/DATM, wrapping sums. The target is
  copied first and the shader reads the destination, evaluates the GS blend,
  COLCLAMP, PABE, DATE, FBMSK and FBA with integer arithmetic and writes the
  result without hardware blending. Primitives that do not overlap share one
  copy and one pass; overlapping ones start a new pass each.
- 16-bit frame buffers are RGBA8 targets quantised when read (as texture, at
  write-back and presentation); blending runs at 8-bit precision.
- Frame formats other than CT32/CT24/CT16/CT16S, and Z formats that are not
  Z formats, fall back to the CPU rasterizer (write back everything, draw on
  the CPU, mark the pages written). Not seen in Aggressive Inline.

`ClearFramebuffer` (the runtime's HLE clears) is drawn as a sprite with the
same semantics as the CPU version (scissor area, FBMSK, FBA, no Z).

### Native draws

`GSRasterBackend::SubmitNative` (gs/gs_native.h) draws a mesh a
game override hands the GS whole, in place of the VU1 work that would have
kicked its vertices (each game's `games/<game>/overrides/`
`aggressive_inline_native.cpp`, `dave_mirra_native.cpp`, `bmx_xxx_native.cpp`, and the engine's draw records
through `engine/host/native_draws.h`). `GS::drawNative` calls it at the draw's place in the
stream with the GS state of that point (active context, PRIM's attributes,
TEXA, FOG), so everything about the draw except its geometry comes from the
game's own GS registers, as for a primitive.

- **Geometry:** `GSNativeMesh`, a triangle list of `GSNativeVertex`
  (object-space position, S/T, RGBA8, the triangle's S/T range, an
  object-space normal), uploaded once into a vertex buffer of its own on
  first use (at most 48 MB per presented frame; a draw over the budget goes
  the CPU way below), given back once the mesh is gone (weak reference,
  swept at presentation).
- **Vertex shader** `gs_native_vs`: `[x y z 1] * transform` (object to GS
  window space, row vectors: X, Y = x'/w', y'/w' in GS pixels, Z = z'/w' the
  GS depth value, clamped at 0 as the VU programs do), then the same mapping
  as `gs_vs` (XYOFFSET in whole pixels, the target's NDC) but in clip space
  with the real w: the GPU clips what the GS could not take. Pipelines of
  native draws (`PipeKey::native`) have depth clipping on: z_ndc = Z /
  2^bits, so the near plane is where Z reaches 2^16, the plane the game's
  clipping programs cut at (w = 1.25). STQ = (s, t, 1) / w', colour and STQ
  `center_no_perspective` / `noperspective` as GS Gouraud and STQ are (the
  GS-path vertices have w = 1, so their interpolation is unchanged). Per
  draw: colour in perspective instead (`perspectiveColour`, colour * Q
  interpolated, divided by Q in the fragment shader: what the game's
  clipper approximates for meshes crossing the near plane), a vertex alpha
  override, an offset along the normal, a UV map from (s, t, 1) or from the
  normal (the terrain program's grass shells, scrolling water and sphere
  map), X and Y clamped to the GS window (the sky), STQ = (s, t, 1)
  (`affineUV`: the skinned programs' affine textures), and the triangles
  a microprogram would leave out whole: `dropFlags` against the VU CLIP
  flags of `[x y z 1] * clipTransform`, a clip distance of -1 at a flagged
  vertex and 0 elsewhere, so nothing of such a triangle but an edge is kept
  (the sky's triangles behind the near side, the skinned ones outside the
  guard band or the depth range).
- **Skinned meshes** (`GSNativeMesh::skin`, `GSNativeDraw::skin`): a
  second vertex stream of `GSNativeSkinWeights` (four bones as palette
  rows, weights in sixteenths) in the same buffer after the vertices,
  pipelines `PipeKey::native` 2 with `gs_native_skin_vs`, and per draw
  `GSNativeSkinning` (the 128-row bone palette, three light directions,
  their colours and the ambient, the lights in the native uniforms) pushed
  as the vertex uniforms (2.4 KB). The shader skins and lights in the VU1
  program's order of operations (`gsNativeSkinVertex` is the CPU version).
  The target box is the scissor (bones move the mesh).
- **Lit rigid meshes** (`GSNativeDraw::lights`, BMX XXX's rigid parts with
  normals drawn with their node's lights): `gs_native_vs` lights the vertex
  from its normal with the native uniforms' lights, scale and alpha in
  place of its colour (`gsNativeLightVertex` is the CPU version).
- **Fragment stage, blending, tests, textures, targets:** the primitive
  path's (`drawPrepared`, shared with `submitPrimitive`); the box for target
  bookkeeping is the mesh's bounds projected (the scissor if a corner is
  behind the eye). A draw that would need shader blending (the destination
  read once per pass, wrong for a mesh overlapping itself), a COLCLAMP=0
  wrap, a render target sampled while drawn into, or the CPU rasterizer is
  declined: `GS::drawNative` then sends its triangles as a PACKED GIF packet
  (ST, RGBAQ, XYZ2, each triangle a strip of its own with ADC on its first
  two vertices, so PRIM stays as the game set it), transformed and clipped
  at the near plane on the CPU as the VU1 program would (skinned, lit,
  dropped and clamped as the shaders do; a node the rigid clipping program
  draws, `GSNativeClip::NearSplit`, with that program's triangles: each split
  in four at its midpoints, cut where it leaves the clip volume, X and Y
  clamped per vertex, as the PS2's picture has them; the sky at Z 1). The CPU renderer always gets
  that. GS traces record native draws as such (`kNativeMesh` once per mesh,
  `kNativeDraw`; gs/gs_record.h), not those packets, so
  `zx-gsreplay` draws them natively or through its backend's fallback.
- **Shadows** (gs_quality `Shadows`, `display.shadows`; sharp by default):
  Aggressive Inline's skater shadow stays on the GS path, drawn by the
  game's own VU1 0x1C volumes into its CT16S counter at the internal
  resolution and composited from that target (render to texture), so it is
  as sharp as the scale. The volumes are fans of long thin flat triangles
  from the light (front faces add 248 * As / 128, back faces 8,
  COLCLAMP=0), and the count must be exact. The shader interpolates a flat
  colour like a Gouraud one and truncates it; on such triangles the float
  error gives 247 and alpha 0x7F at some pixels, so the 5-bit count misses
  and the shadow comes out in pale fragments with the fan's edges showing
  (strict and native alike, at every scale; the CPU renderer is right).
  Sharp: a one-colour primitive (flat, a sprite, or the same colour at every
  vertex) that adds into a wrapping sum, in any game, gets its colour
  rounded instead (`F_EXACT_COLOR`), which is the constant exactly. Original:
  the interpolated, truncated colour.
- Diagnostics: `ZX_NATIVE_TINT=1` tints native draws green,
  `ZX_NATIVE_GPU=0` sends them all through the CPU way; `ZX_GS_STATS=1`
  adds native draws / vertices, declined draws, mesh bytes uploaded and the
  meshes held.

### GPU objects made off the GS worker

Creating a texture or a buffer costs 0.2-1 ms on Direct3D 12 (a committed
resource) and about 0.02 ms on Metal. A new area of a level asks for tens of
textures in one frame (its palettes, decodes, mip copies, Xbox pictures)
and, on the native path, a vertex buffer per mesh met for the first time.
So the backend makes them ahead and keeps them:

- **Pool.** A sampler texture given back (a re-decoded or moved texture, a
  palette, a mip copy, a replacement picture) is kept by shape (size,
  format, levels, usage) and handed to the next texture of that shape. Its
  first upload cycles it (SDL_GPU: draws recorded before keep its old
  texels). At most 64 MB is kept; textures unused for 30 s are released.
- **Stock.** A thread of its own makes textures ahead, per shape: the first
  texture of a shape is made where it is asked for, and the shape's stock
  is then kept topped up (8, then twice as many after each miss, at most
  64 or 16 MB a shape, 96 MB in all). Palettes, power-of-two indexed and
  RGBA textures and BC2 pictures with their mip chains are seeded when the
  renderer starts. A shape not asked for in 30 s gives its stock back.
  SDL_GPU's Direct3D 12 and Metal texture creation is safe on any thread
  (a committed resource and a descriptor taken under the pool's lock;
  `newTexture`).
- **Pipelines.** SDL_GPU passes the driver no pipeline cache, so a
  pipeline's first use compiles it on the thread that records the pass (the
  GS worker, or the window thread when a presentation closes it). On an
  Android handheld's Vulkan driver that stalls the frame for over 100 ms;
  Metal keeps its own shader cache. With a pipeline file (`ZX_GS_PIPELINE_CACHE`)
  the key of every pipeline a game makes is added to it (in first-use
  order), and at the next start a thread of its own makes those pipelines
  again while the game boots; a draw that needs one being made waits for
  it instead of making it twice. A file of another key layout starts over.
- **Mesh chunks.** Native meshes live in 16 MB vertex buffers (a larger
  mesh gets one of its own size), each mesh a 256-byte-aligned range found
  first-fit; a swept mesh's range is free again after the next submit (any
  copy into it comes after the draws recorded from it, on the same queue).
  Draws bind the chunk at the mesh's offset.

`ZX_GS_STATS=1` counts the textures reused, taken from the stock and
made, and the mesh chunks. None of the three changes the picture:
`zx-gsreplay --gpu --picture-hash` gives the same hash with and without
them.

### Picture quality beyond the PS2

`gs/gs_quality.h`: options of the GPU renderer that go past what the
console could do, each a setting (settings.ini `[graphics]`, a Display row,
an environment variable for the runner and the tests; read per draw, so they
apply at once). The CPU renderer ignores them and the strict render path
(`ZX_RENDER_PATH=strict`) draws as the console whatever they are; the
game's memory and timing never depend on them.

| Option | settings.ini | Environment | Default | What it does |
|---|---|---|---|---|
| Anti-Aliasing | `anti_aliasing = off \| fxaa` | `ZX_GS_AA` | fxaa | Aggressive Inline: FXAA 3.11 (quality) over each view's 3D picture where the game's fog planes come (after the view's meshes, shadows and particles, before the HUD); a window much smaller than the picture is filled from a box-filtered mipmap level of it |
| Texture Filtering | `texture_filtering = off \| 2x \| 4x \| 8x \| 16x` | `ZX_GS_ANISO` | 16x | anisotropic filtering of native draws' textures |
| Fog | `fog = original \| smooth` | `ZX_NATIVE_FOG` | smooth | Aggressive Inline: per-pixel fog from the Z buffer in place of the game's eight fog planes |
| Shadows | `shadows = original \| sharp` | `ZX_NATIVE_SHADOWS` | sharp | one-colour primitives that add into a wrapping sum (Aggressive Inline's skater shadow volumes) get their colour exactly (`F_EXACT_COLOR`, see "Shadows" under Native draws) |
| Depth Precision | `depth_precision = original \| high` | `ZX_GS_PRECISE_DEPTH` | high | native meshes keep the GPU's float depth (z'/w' / 2^bits) instead of the GS's 16-bit steps |
| Perspective Correction | `perspective_correct = false \| true` | `ZX_NATIVE_PERSPECTIVE` | true | native meshes' vertex colours interpolated in perspective (the GS interpolates on screen) and skinned models' textures mapped in perspective (their programs give Q = 1.0, affine) |
| Character Lighting | `character_lighting = ps2 \| xbox` | `ZX_NATIVE_LIGHTING` | xbox | lit skinned models (skaters, riders, people) with the Xbox release's lights instead of the PS2's, in a game whose Xbox release has them (Aggressive Inline, BMX XXX: `SkinnedDrawForm::xboxLights`) |

Smooth fog and the FXAA pass work from the fog planes, and only Aggressive
Inline's override notes them (`GS::noteFogPlanes`): in DM2 and BMX XXX
smooth fog has no planes to replace and FXAA no view to run over (the
Anti-Aliasing setting still gives the presented picture its mipmap levels
for a small window).

- **Fog planes.** Aggressive Inline draws a view's fog as eight flat
  full-viewport planes of the fog colour (FUN_001110C0, in the 2D packet):
  untextured triangles at evenly spaced view distances between fog start and
  end, blended (Cs - Cd) As + Cd, Z test GREATER
  without Z write (alpha test NEVER, AFAIL FB_ONLY), so a pixel behind n
  planes gets F + (S - F)(1 - As/128)^n: eight visible bands. The game
  override wraps FUN_001110C0 (calling it) and tells the GS, in stream order
  before the planes, their distances (`GS::noteFogPlanes`, `GSFogPlanes`,
  recorded in GS traces as `kFogPlanes`). The backend holds the plane
  triangles as they arrive (flat, untextured, blended, covering the
  scissor), fits the view's depth mapping GS Z = A / w + B to the planes'
  own Z values at their distances (with Draw Distance Farthest the Z mapping
  is not the projection's; a set that does not fit within 2.5 Z is drawn as
  planes), and in their place draws one pass over the viewport that reads the Z buffer: n = (w - nearest) / step +
  1/2, clamped to 0..8 (through the middle of each step), colour blended with
  alpha 1 - (1 - As/128)^n, and the planes' alpha written where at least one
  plane covers the pixel. Anything else in the set's form (another blend, a
  Z write, masks) draws the planes as they are. With high depth precision
  the fog is smooth to the last bit; with the 16-bit steps it changes in
  steps of one Z (27 units at 1500 with Farthest).
- **Texture filtering** (native draws only; GS-path draws, 2D and the HUD
  sample as the GS does). Replacement pictures get the option's anisotropy
  (Off: trilinear). A PS2 texture without a picture is drawn from a
  mipmapped RGBA copy (`gs/gs_texture_mips.h`): the indices read
  through the draw's palette on the CPU with TEXA applied per texel, the
  GS's own levels kept from the first one outside a render target (DM2's
  distant models keep level 0 at TBP 0, in the frame buffer: the GS's LOD
  never reads it, the copy's would, with ZX_GS_MIP_TARGET_LEVELS=memory
  too), the rest box-filtered (colour weighted by alpha
  where TCC uses it, so cut-out edges do not darken), keyed by content
  (texels, palette, TEXA, size) so a texture the game's cache flushes and
  uploads again finds its copy; at most half a million texels read or built
  per presented frame (`ZX_GS_MIP_BUDGET`), the texture drawn as the GS
  does until its turn. Textures the game magnifies NEAREST keep the nearest
  texel where a pixel covers at most one. For every native picture the
  clamps follow the mip level the filter reads (half a texel of that level
  in from the primitive's range, REGION_CLAMP and REGION_REPEAT cells), so
  lower levels do not mix in atlas neighbours (seams down palm fronds and
  sign edges at a distance). `ZX_GS_MIP_INSET=0` clamps half a level 0
  texel in instead.
- **Character lighting.** VU1 0x08 lights a skinned vertex with three
  directional lights and an ambient (`GSNativeSkinning`). Both of Aggressive
  Inline's releases keep one static light block for characters: the PS2's
  at 0x2732F0 (directions the world axes, colours 0.3, ambient 0.5), the
  Xbox's at 0x26E450 in its RAM (directions (1, 0, -0.3), (0, -1, 0.3),
  (-0.7, 0, 0.5), colours 0.6, ambient 0.5 as the PS2's). Both turn the
  block's directions into object space the same way (the Xbox shader dots
  the normal with -L, VU1 with the negated block). BMX XXX's Xbox release
  has the same block in its XBE, so its riders get the same lights
  (`bmx_xxx_native.cpp`). With Xbox (`zaxis::xboxCharacterLights`) a skinned draw whose
  upload carries the PS2's default block gets the Xbox's directions (U
  times the normalised Xbox directions as columns) and colours, and its
  colour saturates at 128 as the Xbox's vertex colour does at 1.0 (VU1 keeps
  up to 255: `GSNativeSkinning::maxColour`, GS traces' lit byte bit 1).
  Nodes with lights of their own are left alone. The material colours
  (MATI) do not light characters in either release.
- **FXAA** runs at the same place (with or without smooth fog), on a copy
  of the view's rectangle, RGB only (the game keeps counters in the frame
  buffer's alpha). Frames without fog planes (front end, menus) are not
  anti-aliased.
- **Depth precision.** Native draws then use the hardware's interpolated
  depth (no `GS_DEPTH_OUT` rounding), so far surfaces sort among themselves
  in float precision; GS-path draws (particles, shadows, fog planes) still
  write the GS's integers and compare against those values.

### Presentation

`Present` follows `GSCpuBackend::PresentFromLocalMemory`: DISPFB/DISPLAY of
both read circuits (with the DISPFB x/y offsets), PMODE merge (ALP or
per-pixel alpha, SLBG, BGCOLOR), the preferred copy source, field mode line
doubling (odd/even field from the vsync count). It reads the targets
directly at the internal resolution into a presentation texture of
DISPLAY's size times the scale. An existing target shorter than DISPLAY
is grown to its height (reloaded from local memory) only when no other
target has drawing there that memory lacks; else it is shown as it is (the
next buffer often follows it). Aggressive Inline's loading screens need the
growth: their 448-line picture is uploaded in 64-line strips, each a
transfer at its own DBP, over the front end's 224-line buffers, and without
it the lines past the target's end would repeat its last line (vertical
streaks). Not copied: the CPU's fallback that looks for another frame when a
black frame 0 is displayed.

Where the picture goes from there:

- In the game (`GSGpuBackendOptions::hostDevice`, set by
  `createGpuRasterBackendFromEnvironment`): the backend draws on the host
  window's `SDL_GPUDevice`, taken at `Initialize()` (the GS makes its
  backend before the window exists). `Present()` submits the presentation
  pass and returns a frame without pixels (`PresentationFrame::onGpu`);
  `GSRasterBackend::PresentedGpuPicture()` hands the texture through
  `GS::latchedHostPresentationGpuPicture()` to the runner, which passes it
  to `ps2_host::present()` as `Frame::gpuTexture`. The host layer blits it
  to the swapchain with the same rules as a CPU frame (4:3, or 16:9 while
  the widescreen override marks frames; integer or smooth scaling; Retina
  pixels). No read-back and no GPU wait per frame. Screenshots
  (`ZX_SHOT_AT`, ZX_CONTROL `shot`) read the texture back
  (`ReadPresentedGpuPicture`) and are written at 640x480 (854x480 wide)
  times the scale. The host layer runs `addGpuDeviceReleaseHook` hooks
  before it destroys the device; the backend gives everything back there.
  Without a window it takes a private device with read-back; without any
  device the CPU renderer inside the backend draws.
- Offline (`zx-gsreplay --gpu`, `readback = true`): the picture is read
  back; `LastPresentedScaled()` gives it at the internal resolution and
  `Present()` returns the usual 640-wide frame at native resolution
  (centre sample of each GS pixel).

### Frame interpolation

The Display setting Frame Interpolation (`[display] frame_interpolation`,
`ZX_INTERPOLATION=on`; off by default) shows pictures between the game's
frames on a display faster than the game (120, 144 Hz, ProMotion), drawn by
this renderer alone: the game's logic, RAM, GS stream and timing are as
without it. The arithmetic is `gs/gs_interpolate.h` (unit tests
`GSInterpolate`), the window loop's side `runtime/frame_interpolation.h`.

- **What is kept.** While it is on, each game frame (from one frame-queue
  Present to the next, `GSPresentationRequest::frame`) is held as it is
  recorded: every closed pass (its targets, vertices and draw calls), the
  smooth fog and FXAA passes and the presentation, with each native draw's
  motion (`GSNativeMotion`: its node and view, its world and the view's
  camera matrix, given by the producers) and the targets' pixels from before
  the frame first used them (a GPU copy per target per frame). The newest
  three frames are held; a texture a held frame drew with is not written or
  given back while it is held: a write marks the frame not replayable.
- **A picture between** frames A and B at t: B's steps recorded again into
  the replay's own targets, started from B's copies. A native draw found in
  A (by key) is drawn with `world(t) x camera(t) x rest(t)`: the world by
  polar decomposition (rotation slerped, stretch and translation lerped),
  the camera's own transform (the view's inverse) the same way, the rest
  (projection and the program's column changes) lerped; skinned bone
  palettes per bone the same way, light directions lerped. GS-path 3D
  primitives (Z tested GEQUAL or GREATER, not sprites, points or fog planes:
  particles, trails, shadow volumes, declined native draws) are moved per
  vertex from B's camera to t's (`reproject`, w from the window position);
  2D, sprites, the fog and the previous-frame blend (which reads the other
  buffer's copy: the last real frame) are as B has them. The picture goes to
  one of two textures of its own, shown by the window as a presented one.
- **DM2 and BMX XXX** draw their riders (and BMX XXX every mesh) natively
  only from the engine's draw records: with interpolation on, their
  meshes are drawn from the records (`native_draws::attach`'s
  followInterpolation) as with `render_path = records`, unless the
  render path is strict. DM2 shows the buffer it drew a frame before: the
  picture between replays the steps that drew what the frame shows
  (`PresentStep::ownDrawing`).
- **Not made** (the window shows a frame as it is, counted by reason,
  `gs_interpolate::Skip`): a camera cut (the engine's cameraCut rule: more
  than 7 units or 20 degrees in a frame), nothing moved (menus, movies,
  pause), a frame that loaded a target from local memory, drew on the CPU
  rasterizer, resized a target or had a texture written while held, the last
  picture between before the last still on the GPU (budget: one may be in flight), the next
  frame not made in time, no frame before. A draw without a match in A or
  whose node moved more than 7 units is drawn where B has it.
- **Checks.** `zx-gsreplay TRACE OUT --gpu --between 0.5,1` replays a trace as
  the frame queue makes it and after each frame the pictures between it and
  the one before: at 1 the replay of the frame's own steps must give the
  frame's picture exactly (`ZX_INTERP_FORCE=1` makes them at cuts and
  still frames too; `ZX_INTERP_MATH_AT_ONE=1` runs the matrices at 1 as
  well). `ZX_INTERP_TIMING=1` times each picture between alone on the GPU
  (the queue idle first). The runner's `ZX_PERF_LOG` prints `[interp]`
  lines: pictures between, the reasons, refresh intervals and how long after
  a frame was made it was first seen and seen whole.

### Full height

Many PS2 games draw field frames. Aggressive Inline does: the display is
640x448 but the game sets up its buffers, projection and screen offsets for
half the height (bit 0 of 0x231900, FUN_00100FB8 / sceGsSetDefDBuffDc at
0x100404: 640x224 in the front end, 512x224 in levels) and the TV shows
each line twice. At "3x" without full height the picture is 1920x672
(1536x672 in a level) stretched to 4:3: half the detail vertically.
Rendering the game in 448-line frames instead would need about 1.7 MB more
GS memory than the game leaves free (two 640x448 colour buffers and a Z
buffer), so it is done in the renderer, for whichever game runs (the Full
Height setting is shared by the three): with `fullHeight` every target is `scaleX` times wider and
`scaleY = 2 * scaleX` times taller than in GS pixels. Primitives are drawn
through the viewport, so geometry, textures and fog get the extra lines for
free; copies, write-backs (centre sample per axis), loads, the depth load
pass, render-to-texture sampling (the previous-frame blend, the skater
shadow) and presentation take the two scales. Targets that are already
448 lines (loading screens, the legal screen) are drawn at twice the lines
they need, harmless. The shaders
carry the vertical scale in `FSUniforms.f1.z`, `BlitUniforms.f.y` and
`PresentUniforms.out.z`; the HLSL has the same three (`outSize.z` there).

### Write-backs, threads and locks

- Write-backs: a write-back records the download and does its bookkeeping
  at once, but local memory gets the pixels only when the GPU is done
  (`OwedWriteBack`; `ZX_GS_GPU_DEFER=0` waits at once); until then those
  pages are owed and whatever reads or writes them on the CPU waits for them
  first. CLUT loads and 4- and 8-bit textures over owed pages are read from
  the target on the GPU instead (gather passes, PCSX2's
  palette-from-render-target path), with the last gather reused while the
  target is unchanged. This matters where the game reads the frame being
  drawn several times a frame (Aggressive Inline's split screen at draw
  distance far).
- Threading: SDL_GPU uses a command buffer only on the thread that
  acquired it. On Metal (which does not mind) the GS worker and the window
  thread share the backend's command buffer under the mutex. On other
  drivers (`threadOwnedCommands`, `ZX_GS_GPU_THREAD_OWNED=0/1` to force)
  every public call runs in a `CallScope`: on entry it waits (up to 20 ms)
  until no other thread holds an open command buffer; the owner submits at
  the end of its current call when someone waits, a thread other than the
  drawing one always submits what it recorded before it leaves, and the GS
  worker submits when its queue runs dry (`GSRasterBackend::WorkerIdle` from
  the `Ps2AsyncStage` idle hook). After the 20 ms the waiter takes the
  buffer over and says so once in the log (only expected with the GS on the
  game thread, `ZX_VU1_ASYNC=0`).
- Locks: the GS worker holds the frontend's state lock for each GIF packet
  and the backend's for the same packet (`GSRasterBackend::BeginPacket` /
  `EndPacket`: one `CallScope` over the packet, so a primitive takes no
  lock), and takes them again at once for the next. A mutex that does not
  hand over (Windows' SRW locks) would wake the window thread's presentation
  latch after every one, at a kernel call each on the worker, while the
  latch spins; so other threads count themselves as waiting
  (`GS::lockForHost`, `CallScope`) and the worker lets them in before its
  next packet or call (at most a millisecond): inside a packet it ends the
  packet's scope for them (submitting its command buffer to a thread waiting
  for it) and starts a new one after.
- Per draw: a pass keeps its vertex and draw storage across passes, and
  texture objects, replacement pictures, targets and pipeline keys keep
  their last lookup.

### Switching while running

`PS2Machine::requestRasterBackendReload()` (the Renderer setting,
ZX_CONTROL `renderer cpu|gpu [n]`) makes the backend again from
`ZX_GS_BACKEND` / `ZX_GS_SCALE` between two host frames.
`GS::switchRasterBackend` waits until no host-to-local transfer is half
done (at most 60 host frames), writes the old backend's targets back
(`setRasterBackend`: Sync, SnapshotVram), and carries the CLUT buffer over;
the new backend loads its targets and textures from local memory.

## Shaders on Metal, Direct3D 12 and Vulkan

Metal: hand-written MSL (`gs_gpu_shaders_msl.h`) compiled by SDL at run
time (`SDL_GPU_SHADERFORMAT_MSL`); variants come from `#define`s.

Direct3D 12: `gs_gpu_shaders.hlsl`, a line by line translation of the MSL
(keep the two in step), compiled at build time by the Windows SDK's `dxc`
(Shader Model 6.0, HLSL 2021) into DXIL byte arrays:
`ps2/gs/CompileGsShadersDxil.cmake` writes `gs_gpu_shaders_dxil.h` in the
build tree, one array per entry point and `GS_SLOW`/`GS_DEPTH_OUT` variant.
Bindings follow `SDL_gpu.h` for HLSL: vertex uniforms `b0, space1`;
fragment textures and samplers `t0-3`/`s0-3, space2`, fragment uniforms
`b0, space3`; vertex attribute n is `TEXCOORDn`. Differences from the MSL:
`round()` is half to even in HLSL, so a `roundAway()` gives MSL's half away
from zero; `select()` instead of `?:` on vectors; `linear` is a keyword.
Vulkan (Android): the same HLSL compiled to SPIR-V by the NDK's `glslc`
(`-DGLSLC=`, the same script; `gs_gpu_spirv`, `GS_SPIRV=1`). The backend
takes MSL if the device does, else DXIL, else SPIR-V (`builtShaderFormats`);
without `dxc` at build time a Windows build has no GPU renderer (the CPU
renderer draws). Desktop builds have no SPIR-V, so no Vulkan device there.

## How the picture differs from the CPU renderer's

`zx-gsreplay TRACE OUT --gpu --compare` compares each frame with the CPU
renderer at 1x: the mean absolute per-channel difference (0-255), the
largest difference, and the share of pixels off by more than 16. Frame 0 of
a trace is the buffer the trace starts with (no GPU drawing yet), so it
matches exactly where the game did not draw into the shown buffer. The
differences to expect:

- Triangle edges: the CPU renderer covers pixels on an edge from both sides
  (an epsilon), the GPU once (top-left rule). Shows as single columns in
  HUD bars and specks on small distant geometry.
- The last line of a frame drawn with Aggressive Inline's previous-frame
  blend (the Tutorial, the Movie Lot): the blend reads one line past the
  frame buffer; the CPU gets whatever memory follows (the Z buffer), the
  GPU clamps to the target.
- One-off rounding of the hardware blend (`Cs*As + Cd*(1-As)` in float
  instead of the GS's integer shift).

## Known gaps

- Aggressive Inline's movies (intros, the menu FMV window) are 512x256
  frames uploaded into a texture and drawn as one quad with the game's own bilinear filter, so at
  n x they are a smooth bilinear upscale of the native frame (no blocks);
  there is no more detail to get without decoding at a higher resolution.
- Upscaled bilinear texture sampling differs from the CPU renderer's native
  samples on fine textures (the menu background's seams), as any upscale
  does; `--compare` at 3x (centre sample) shows it.
- No Vulkan on desktop builds (SPIR-V is built for Android only).

## Out of scope (not implemented)

Bit-exactness with the CPU renderer; dithering (DTHE); SCANMSK; AA1 antialiasing; wide or upscaled
lines (lines are one pixel at the internal resolution); textures read with
a different layout than the target holds them in (8/4-bit reads of 32-bit
targets, 16-bit reads of 32-bit targets, depth as colour) other than through
memory; render targets with frame formats other than CT32/CT24/CT16/CT16S;
the black-frame display fallback; multiple GPU devices; threading inside the
backend (one mutex serialises it).
