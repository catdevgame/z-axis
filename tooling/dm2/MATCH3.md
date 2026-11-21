# The three-way function match: Aggressive Inline, Dave Mirra BMX 2, BMX XXX

What this file is for: how the cross-build function match is made and what the work database built on it holds
(sources, tables, views, results, the card, the write-back). `tools/workdb.py`'s docstring is its usage; section 6
below is the database's reference. Counts quoted here are from when each part was made: `workdb.py tables` and the
views give today's.

Three PS2 games on consecutive versions of Z-Axis's engine: Dave Mirra BMX 2 `SLUS_201.59` (ZGE2, Aug 2001), Aggressive Inline `SLUS_203.27` (ZGE3,
Apr 2002), BMX XXX `SLUS_204.15` (ZGE4, Oct 2002; it was "bmx3"). The aim: say how much of the
readable C++ written for AI (`runtime/src/game`) can switch on in DM2 and BMX XXX,
and map AI's functions to the engine's modules. Keyed on AI's functions throughout.

Nothing here is game data: the tools are committed, their outputs (function maps, the match, the
module map, decompiler output, everything with the games' addresses) are written to a scratch
folder outside the checkout (`$ZAXIS_SCRATCH/match3`) and never committed.

## Tools (`tools/`, Python run with `-I`, disc files read as data)

| tool | what it does |
|---|---|
| `ghidra/pipeline.sh` | The carry-over study's Ghidra pipeline for any of the engine's executables: `pipeline.sh <name> <elf copy> <work dir>`. The scripts are AI's (`tooling/ai/ghidra/scripts`, copied into the work dir) plus `ghidra/ExportDecomp.java`; it installs the R5900 extension into the work dir's own Ghidra settings. Stages 1-8 as before, 9 = full decompiler output |
| `xbe_paths.py` | `extract`: default.xbe out of an Xbox image (read only, with `tooling/ai/tools/xbox_files.py`); `paths`: the __FILE__ strings in an XBE and the x86 instructions that push them |
| `fmatch3.py` | the three-way match (below); writes `match3.json` |
| `readable_carry.py` | every readable registration with its counterpart in DM2 and BMX XXX and what differs; writes `readable-carry.json`, `--list` prints the identical ones |
| `modmap.py` | AI's functions by module (below); writes `modmap.json`, `--readable` adds the readable functions per block |
| `ghidra/xbe_pipeline.sh`, `xmatch.py`, `xpair.py` | the Xbox build as a second decompilation, paired with the PS2's (section 4) |
| `readable_ghidra.py`, `ghidra/readable_pass.sh` | the readable write-back: the readable rewrite's names, signatures, structs and globals applied to a copy of a Ghidra project, then the exports again (section 7) |
| `exports.py` | where each build's decompilation and function export are, as analysed and readable; the tools that read them take the readable ones by default (section 7) |
| `dm2bmx.py` | DM2 -> BMX XXX PS2: fmatch3's match, then again seeded with its pairs and the triangle's; writes the work database's `pair` results (section 6, "DM2 and BMX XXX") |
| `lineage_gc.py` | the engine changes the GameCube builds date, as `history` notes (section 6, "The engine's lineage") |
| `dead_code_dm2.py` | DM2's dead-code table (docs/DEAD-CODE.md, "Dave Mirra BMX 2") |

The run, from the scratch folder (inputs: copies of the three ELFs in `elf/`; AI's and DM2's
function maps from the earlier pipeline runs in `tooling/dm2/ghidra-ai` and
`recomp/ghidra-functions.csv`; BMX XXX's from `pipeline.sh`; the XBEs in `xbe/`):

```
tools/ghidra/pipeline.sh BmxXXX elf/SLUS_204.15 ghidra-bmxxxx
python3 -I tools/xbe_paths.py extract "<BMX XXX-xbox.iso>" xbe/bmxxxx.xbe   # and DM2's, AI's
python3 -I tools/xbe_paths.py paths xbe/bmxxxx.xbe xbe/bmxxxx-paths.json
python3 -I tools/fmatch3.py elf/SLUS_203.27 <ai.csv> elf/SLUS_201.59 <dm2.csv> elf/SLUS_204.15 ghidra-bmxxxx/out/ghidra-functions.csv match3.json
python3 -I tools/readable_carry.py match3.json <the six inputs> <z-axis>/runtime/src/game readable-carry.json --list
python3 -I tools/modmap.py match3.json --readable readable-carry.json <ELF, functions, sce-matches of each game> modmap.json xbe/*-paths.json
```

Ghidra on BMX XXX: 5,621 functions, 99.13 % of .text (1,300,928 of 1,312,288 bytes; 4 bytes of
instructions outside a function), 0 overlaps, 0 error bookmarks, 192 computed jumps all resolved,
423 SCE signature matches (DM2 576, AI 582: BMX XXX's link leaves out unreferenced functions, see below), 27 source path
strings. About 2.5 minutes for stages 1-8, 30 s for the decompiler output.

## 1. The match (fmatch3.py)

Per pair (AI to DM2, AI to BMX XXX): fmatch.py's passes (address-masked exact hash, unique string
sets, call-graph propagation through aligned call sites) plus equal-hash groups bracketed by
matched neighbours, an order pass (an unmatched function between two matched neighbours is
compared with the unmatched functions between their counterparts: link order is mostly kept),
and a call-graph neighbour pass. Similarity for accepting a pair: 0.4 masked-token ratio + 0.4
opcode-class ratio + 0.2 shared matched callees. Classes by the masked-token ratio r: identical
(masked bodies equal), close (r >= 0.85), drifted (0.5 to 0.85), weak (matched by call graph or
order, r < 0.5), none.

Checks:
- The carry-over study's 38 hand-checked AI/DM2 pairs: 35 found, 3 unmatched, none paired wrongly (the
  three: the UI script runner, which DM2 rewrote, the front-end registration and display setup).
- Triangle: DM2 matched to BMX XXX the same way; of 2,298 AI functions matched in both games,
  2,135 give the same pair, 47 a different one, 116 DM2 functions unmatched there. By the worse of
  the two classes: identical 1,284 / 10 / 13, close 234 / 3 / 4, drifted 394 / 23 / 36, weak 223 /
  11 / 63 (agree / disagree / unmatched). So identical and close pairs are about 99 % consistent;
  weak ones about 75 %.

AI's 6,324 functions by layer (AI address: sdk = an SCE-scanned name, crt0, or 0x2021E0 up;
engine 0x100100-0x171000; game 0x171000-0x2021E0):

| layer | AI fns | DM2 identical | close | drifted | weak | none | BMX XXX identical | close | drifted | weak | none | identical in both |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Sony SDK | 948 | 711 | 46 | 42 | 46 | 103 | 581 | 22 | 3 | 0 | 342 | 426 |
| engine | 2,648 | 1,201 | 206 | 316 | 149 | 776 | 1,532 | 292 | 236 | 62 | 526 | 832 |
| game | 2,728 | 76 | 18 | 91 | 91 | 2,452 | 672 | 138 | 171 | 54 | 1,693 | 49 |
| all | 6,324 | 1,988 | 270 | 449 | 286 | 3,331 | 2,785 | 452 | 410 | 116 | 2,561 | 1,307 |

By code size (instruction words) the identical share is: SDK 62.7 % DM2 / 70.2 % BMX XXX, engine
25.8 % / 45.3 %, game 1.3 % / 10.4 %; identical or close: engine 37.9 % / 66.6 %. 1,024 engine
functions are identical or close in both games.

Findings:
- BMX XXX is much closer to AI than DM2 is, in the engine (1,532 identical against 1,201) and in
  the game layer: 672 of AI's game functions are identical in BMX XXX, 413 of them at AI
  0x1F0000-0x1FFFFF (the park editor, `GameTools/ParkEditor/editToolSet.c`, which BMX XXX
  inherited) and 162 at 0x170000-0x18FFFF (zgui and the UI script interpreter).
- BMX XXX's SDK is AI's release, not a newer one: its library version strings are
  AI's (PsIIlibkernl2430 ... PsIIlibpkt 2400, plus libscf 2400), and every library function that
  pairs is AI's code. The 342 of AI's SDK functions with no counterpart are ones BMX XXX's link
  left out (nothing references them: its ELF has 7 unreferenced functions, DM2's 702). DM2 links
  release 2.2 (every version string 2200). `runtime/src/game/dm2/libraries.tsv`, `game/bmx/libraries.tsv`
  pin each library.
- DM2's game layer shares almost nothing with AI's (76 identical, mostly small helpers).

## 2. The readable functions (readable_carry.py)

Every readable registration with its counterpart in DM2 and BMX XXX and what differs. For identical pairs, "table
ok" means every call goes to the matched counterpart of AI's callee and every global maps as the rest of the match
maps it (so a per-game address table is all the function needs); "+ call site" also allows callees the match left
unmatched but whose call sites all name one function (they need that callee's semantics checked: e.g. AI's sin/cos
pair against DM2's). For drifted pairs it lists what differs (code, calls, constants, registers, call sites, struct
offsets, stack frame, globals). The work database's `carry_by_status` view gives the same by status, today.

When this was first measured, about 18 % of the readable functions carried to DM2 as they were and 28 % to BMX
XXX, almost all engine leaves (chunk loaders, collision face trees, vector, matrix and quaternion math, IFF chunks,
lists, libvifpk) and, in BMX XXX, zgui and the UI scripts; the skater, the level scripts and the follow camera had
no counterpart in either BMX game.

## 3. The module map (modmap.py)

Evidence, strongest first: a __FILE__ path string used by the function in AI or by its DM2 or
BMX XXX counterpart (asserts and error reports: 20 paths in AI's PS2 build, 22 in DM2's, 27 in
BMX XXX's); the SCE scan's library (SDK region only: small SCE hits inside the engine are chance);
blocks; contiguity between two functions of one module (an object's .text is contiguous).

Blocks: two functions are in one object when they use the same data item (.data, .sdata, .bss,
.sbss, .rodata) or one calls the other with every user within 0x3000 bytes of text (statics and a
module's own globals; farther apart means an extern); adjacent blocks whose local data is out of
link order are one object too. Computed in each game and carried to AI through the match
(counterparts in one block link their AI interval: 185 links). Checks: at 0x3000 no block holds
two modules' anchors in AI or DM2 (at 0x4000 DM2's ps2Movie and Ps2MemCard merge); in AI 4
clashes remain, three between adjacent SDK libraries and one between the park editor and the
game's zfxMgr. AI has 763 blocks, 74 of 2+ functions in the engine and 82 in the game.

Blocks are one object or a few neighbouring objects tied by calls; the zglTerrainRender block
(0x12C0F8-0x135860, 272 functions) also holds the GEO chunk loaders and part of collision. The
readable code's own folders agree with the blocks: most blocks that hold readable functions hold
one folder (zge/math, zge/assets, aggressive/script, aggressive/skater ...).

DM2's link-map fragment (the ELF header padding): 33 objects in link order, zobj zscene zcompres
zfile zassetLib zanim zstring ziff zdebug zinput zquat znad zcollide ztoken zlist zprofile zbinHeap
zUtil zEventLoop zik zmovie zdynamic zscnpts zobjModel zobjCamera zobjLight zobjPEmit zTimer zFont
zPath zEntity zRamDisk zmathGenMatrix (the first line is cut: `zmathUtil.o` before zobj). AI's
anchored modules keep that order (8 of them are in the list). Its sizes are another build's:
DM2's retail .rodata holds far less (zobj's path string to zfile's is 0xB8 bytes, the fragment
says zobj+zscene+zcompres alone are 0x5E8), so only the order is used: a gap between two listed
modules gets the objects between them as candidates (309 engine functions).

The Xbox builds' __FILE__ paths (AI 21, DM2 24, BMX XXX 27) are the same modules plus the Xbox
platform files (XBoxGL, XBoxTexture, XBoxMovie, xboxChan ...), DM2's zglCamera.c and
Entities/mover.c, BMX XXX's ztexturefx.c and its game files (action/{action, camera, fx, game,
motview, overlay, physics, weapon}.c, bmx.c). Their .rdata order is not the PS2 link order, so
they only add names. (Section 4: they are function anchors too; the engine's error
reports store __FILE__ with `mov [eax+0x14], imm32`, which xbe_paths.py finds.)

Coverage of AI's functions:

| layer | fns | file | sdk | block | between | unplaced |
|---|---|---|---|---|---|---|
| SDK | 948 | 0 | 584 | 82 | 121 | 161 |
| engine | 2,648 | 20 | 0 | 869 | 0 | 1,759 |
| game | 2,728 | 5 | 0 | 124 | 0 | 2,599 |

By code size: SDK 97.8 %, engine 29.9 %, game 8.9 % placed in a named module. Named engine and
game modules (AI functions placed): ps2Chan 60, ps2Movie 171, Ps2MemCard 87, zglTerrainRender
272, zobj 37, zfile 47, zdebug 29, zprofile 67, zscnpts 15, zobjModel 38, zobjPEmit 1,
zobjParticleEmitter 18, zTimer 8, zsaveapi 18, zfxmgr 10, zgui 65, inlineFx 66,
inlineTextureFX 8, editToolSet 1, zfxMgr 1; SDK libraries libm, libgraph, libmpeg, libdma,
libdev, libpkt, libpad, libvu0, libcdvd, libkernl, libmc, libsdr, libgcc, libc. Of the 465
readable functions, 65 are in a named module; all are in one of 85 blocks (49 in one-function
blocks), the list `modmap.py --readable` prints.

## 4. The Xbox build beside the MIPS

AI's Xbox release (`default.xbe`, the PDB path
`C:\inline\src\Xbox\Final_LTCG\InlineXBox.exe`, XDK 4627, MSVC with link-time code generation,
D3D8LTCG) run through Ghidra as a second decompilation, its functions paired with AI's PS2
functions, and the pair printed side by side for the readable work. Outputs in
`$ZAXIS_SCRATCH/aixbox` (decompilations, the pairing, the per-function files: disc
derived, never committed).

Tools:

| tool | what it does |
|---|---|
| `ghidra/xbe_pipeline.sh` | `xbe_pipeline.sh <name> <default.xbe copy> <work dir>`: 1 import with XboxDev's ghidra-xbe loader, full analysis (its XbSymbolDatabase analyzer names XDK functions; Function ID names CRT ones), "Non-Returning Functions - Discovered" off, "Decompiler Parameter ID" on; 2 gap fill; 3 export (`<work>/out/<name>-xbe.jsonl`); 4 decompiler output (`<work>/<name>-decomp.c`). About 12 minutes |
| `ghidra/XbeGapFill.java` | functions reached only through pointers in data (vtables, state and callback tables) and code after the alignment padding; not after code that falls through (that is the rest of a function) |
| `ghidra/ExportXbe.java` | per function: calls in order (tail jumps too), data references with the instruction, immediates, displacements; all defined strings |
| `ghidra/XbeDump.java` | debug: the listing from an address |
| `xmatch.py` | the pairing (below); `--holdout` the precision check |
| `xpair.py` | a PS2 function beside its Xbox counterpart, annotated; `--slices FILE` one file per Planned registration and per address in FILE's "Next slices, in order" section (docs/READABLE.md no longer has one: Planned only) |
| `xbe_paths.py` | also finds `mov [r32+d], imm32` uses of a path |

The run (from `$ZAXIS_SCRATCH/aixbox`; XBE_EXT_ZIP is the ghidra-xbe release zip,
github.com/XboxDev/ghidra-xbe `build-202610161023`, `ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip`):

```
python3 -I tools/xbe_paths.py extract "<Aggressive Inline-xbox.iso>" xbe/ai.xbe
XBE_EXT_ZIP=downloads/ghidra-xbe/ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip \
  tools/ghidra/xbe_pipeline.sh AiXbox xbe/ai.xbe $PWD/ghidra-aixbox
python3 -I tools/xmatch.py elf/SLUS_203.27 <ai.csv> <ai sce-matches.csv> xbe/ai.xbe \
  ghidra-aixbox/out/AiXbox-xbe.jsonl xmatch.json --readable <z-axis>/runtime/src/game
python3 -I tools/xpair.py xmatch.json <ai-decomp.c> ghidra-aixbox/AiXbox-decomp.c 0x18D678 [--side]
python3 -I tools/xpair.py xmatch.json <ai-decomp.c> ghidra-aixbox/AiXbox-decomp.c \
  --slices <z-axis>/docs/READABLE.md --out slices
```

(`<ai.csv>`, `<ai sce-matches.csv>`: `ghidra-ai/out/ghidra-functions.csv` and
`ghidra-ai/sce-db/sce-matches.csv`; `<ai-decomp.c>`: this workspace's `ai-decomp.c` (a data link), the PS2
pipeline's decompiler output.)

### Ghidra on the XBE

5,686 functions: 3,917 in `.text` (95.0 % of its 1,024,496 bytes; the rest is alignment padding
and 28 KB of jump tables and code that does not start a function: 202 gaps), 1,769 in the XDK's
own sections (D3D, D3DX, XGRPH, DSOUND, XPP, WMADEC, BINK*). 252 names from XbSymbolDatabase and
the kernel imports (XAPILIB, D3D8LTCG, DSOUND, D3D8, xboxkrnl), about 125 from Function ID (the CRT:
`_strncmp`, `__ftol` ...). The game and engine are `.text` 0x11000-0xF12AF (3,263 unnamed
functions); XAPI and the CRT follow. LTCG shows: register arguments (`in_EAX`, `unaff_ESI`:
8,766 such names in the high and medium pairs' Xbox output against 1,193 on the PS2), small
functions inlined everywhere (strcpy/strcat loops, list walks, vector math). The gap fill
matters: without it Ghidra finds 2,634 `.text` functions (79.6 %); with the discovered-noreturn
analyzer on, the same run made 473 more, fragments of functions cut after calls it took as not
returning.

### The pairing (xmatch.py)

Each function becomes a set of tokens the two builds share: strings (`__FILE__` paths by file
name), float constants (MIPS lui/mtc1 and lwc1; x87 loads, doubles narrowed), integer immediates,
memory displacements off a register (struct offsets), and, once paired, callees and globals;
weighted by rarity, only tokens seen in both builds counting. Passes: names (libc), rare strings,
call sites (callees between two paired callees aligned in call order, a small callee's own calls
looked through, as either build may have inlined it; callers the same), link order (inside a
module the Xbox keeps the PS2's function order; between two pairs about as far apart in both,
the functions in between are aligned by tokens and size), folded duplicates. Confidence: high,
medium, low (the docstring has the rules).

| layer | AI fns | high | medium | low | inlined | none | paired by size (high / medium / low) |
|---|---|---|---|---|---|---|---|
| Sony SDK | 948 | 7 | 0 | 0 | 8 | 933 | 1.0 % (libc by name; the rest has no counterpart) |
| engine | 2,648 | 352 | 175 | 123 | 332 | 1,666 | 39.4 % (24.4 / 10.9 / 4.1) |
| game | 2,728 | 733 | 293 | 254 | 303 | 1,145 | 77.5 % (50.9 / 18.4 / 8.3) |

1,937 pairs (2 many-to-one), on the Xbox side 1,928 of the 3,263 game and engine functions
(68.6 % of their instructions). "Inlined": no standalone counterpart, and the Xbox counterparts
of its paired callers hold its tokens (398) or have no call left in its place (237). The engine's
unpaired third is mostly PS2-only code: 0x100000-0x138000 (the VIF/VU1 render path, GEO/TXR
loaders, ps2Movie, ps2Chan, the memory card) pairs at 0-9 % by size per 16 KB, 0x13C000-0x160000
(zobj, zfile, zscene, zgui's engine half) at 50-73 %.

Checks:
- Hand check, pairs drawn at random from engine and game, read side by side: high 11 of 11
  right, medium 12 of 12, low 29, about half right
  (the wrong ones: a size match beside a paired call with nothing else shared). Medium is not
  always right: PS2 0x138370 (a 16-byte aligned bump allocator) is paired with the Xbox's
  ziffChunkReadData (0xAA370), whose PS2 counterpart is readChunk 0x149428 (paired low with
  0xAA2F0). So high is dependable, medium nearly, low is a lead to read, not a fact.
- `--holdout`: every second of the 138 pairs made by a string unique in both builds is hidden
  (strings removed on both sides, the string pass skips it) and the other passes run: of 69, 48
  paired the same, 16 unpaired, 5 differently; 3 of those 5 the hidden "truth" was wrong, the
  unique string had moved with inlining (PS2 0x1910B8, 2,479 instructions, holds a bone-name table
  the Xbox keeps as its own small function; setSkaterScore's "Rollover!" is "$KSTR_ROLLOVER!" on
  the Xbox and the old text sits in an Xbox function that inlined it). So at most 2 of the 53
  pairs made are wrong (a low and a medium one, not read).
- Not checked: the pairs against running code (nothing was run), and the engine's low pairs
  beyond the sample.

### Reading the Xbox code beside the MIPS

`xpair.py --slices` wrote 103 files (the slices' addresses and every Planned registration): 28
high, 12 medium, 20 low, 1 inlined, 42 with no counterpart (the packet rebuilds FUN_00134AB0 .. FUN_00134D28,
the VIF1 handler FUN_001085B0, the libvifpk/libvu0 ones, half the script natives: they are inlined
into the script VM's dispatch or dropped). The Xbox output is annotated with the PS2 names of its
callees and globals and both sides with string texts and float values.

Over the 1,553 high and medium pairs, Ghidra's Xbox output is about as long (engine 21,722 body
lines against 22,209, game 54,159 against 55,104) with fewer gotos (683 / 859), fewer
`undefined` types (12,566 / 17,548) and fewer calls (10,946 / 13,323: inlining), but seven times
the register-argument names (8,766 / 1,193). Where it helps: the 201 pairs whose PS2 side uses
VU0 macro code (8,031 `_lqc2`/`_vsub`/`_vmulbc` lines): the Xbox side is plain float arithmetic
and 25 % shorter (19,778 lines against 26,373). The follow camera
(fn_18D678: 640 MIPS, 737 x86) reads as C on the Xbox: `1/SQRT(x*x+y*y+z*z)` normalisations,
the collision query with the same arguments, the same constants (2, 1.5, 20).

Where it differs:
- Inlining: LTCG inlines small callees (string copies become byte loops, list walks, getters,
  vector math, the script stack's pop in every native), so a short PS2 function calling helpers
  can be a long Xbox one (the level path builder 0x1A4D78: 70 MIPS, 240 x86). GCC inlined some
  the Xbox kept (the bone-name table).
- Float order and precision: x87 computes in 80 bits (`float10`) and MSVC reorders sums
  (vec3Length is `z*z + y*y + x*x` on the Xbox, `x*x + y*y + z*z` in VU0 order on the PS2). The
  Xbox code shows what is computed, never the PS2's order or rounding: ee::Float and the MIPS
  stay the reference.
- Struct layout: the engine's and zgui's structures keep their offsets (zgui widgets +0xF0 ..
  +0x164 on both), the skater's Player does not: +0x74 is +0x84, +0x214 +0x224, +0x1E8 +0x1F8,
  +0x434 +0x484, +0x550 +0x5C0 (fields grow by 0x10 to 0x70 through the struct). Offsets read on
  the Xbox side have to be mapped back.
- 64-bit fields: the PS2 loads some fields as 64-bit (`ld`, Ghidra's `long`), the Xbox as two
  32-bit words (+0x110/+0x114).
- Calling: LTCG's register arguments make Ghidra miss parameters (`in_EAX`); the PS2 side has the
  ABI's arguments.

### Names the Xbox build has and the PS2 lacks

- Function names in messages the PS2 build dropped: zfileRegisterErrorMessage (PS2 0x13F1D8),
  zguiScript_Load and zguiScript_LoadLib (0x17EB20), zmemDMMLock (inlined into four Xbox
  functions, the counterparts of 0x1C3BC0, 0x1D8CC0, 0x139760 and 0x1D9838), zmachineSetObjectState, zglSceneFindTextureFromHash,
  zsndSourceReset, zdebugPrint, zdebugRemoveStatsFunction (0x13B990), ziffChunkReadData (readChunk
  0x149428, see the checks),
  inlineAirTrickCreateJumpAirTrick (0x1B2E28), inlineObjectiveTrickCreateInstance (0x1EB2D0),
  inlineCommandProccessGrind (0x18AF00); Xbox-side XBUtil_FindMediaFile, XBoxFile, XBFont.
- Type and enum names: EzsceneObjType (0x13CEE0's message "ZSCENECLOSE: Object not attached to
  scene hasn't been freed:EzsceneObjType=%d"), kzobjType, module tags ZIFF, ZTOKEN, ZOBJ,
  ZGLMODEL, ZGLTEXTURE, ZGLMAT, ZSCENECLOSE; the property types of zgui's scripts (Bool, Sint32,
  ZFloat, Z3DVector, ZRGBA_PAL, String, ZAssetID, Event).
- 1,116 of the 1,942 strings the Xbox code references are not in the PS2 executable: engine
  diagnostics ("***INFO*** World size:x=%dm", "High number of instances (%d > %d) in sector",
  "Didn't find topology id"), the game's ("Double Grind Edge: No match found!", "Memory leak in
  ..."), the vertex and pixel shader sources with their comments, localised UI strings ($KSTR_*).
- Module paths: the 21 __FILE__ paths, now with their uses (28 functions; 14 paired, two placing
  a PS2 function no PS2 path placed: zdebug 0x13B990, XBoxLoadScreen's counterpart 0x110E68).
- The release: XDK 4627 libraries (XAPILIB, D3D8LTCG, D3DX8, XGRAPHC, DSOUND, LIBCMT, LIBCPMT),
  Bink, WMA decoding.

`xmatch.json` keeps them per Xbox function (`names`, with whether the PS2 build has the
identifier).

Installed: nothing system-wide. ghidra-xbe (XboxDev, `build-202610161023`, for Ghidra 12.1.4) is
unpacked into the work dir's own Ghidra settings by the pipeline; its XbSymbolDatabaseCLI ships
for mac_x86_64 only and runs under Rosetta (copied to `os/mac_arm_64`).

Not done: the Player's offset map as a table (from the paired functions' displacements); the low
pairs checked; the engine's PS2-only code has no Xbox counterpart by design.

### DM2's and BMX XXX's Xbox builds

BMX XXX (`c:\bmx3\src\XBox\Bmx3XBox\Final\Bmx3XBox.exe`) through `xbe_pipeline.sh`, in
`$ZAXIS_SCRATCH/workdb/bmxxbox`; about 3 minutes. 6,116 functions, 4,134 in
`.text` (96.4 % of its 1,162,368 bytes; 769 gaps after code that falls through), 376 named by
XbSymbolDatabase and Function ID. DM2's is in `$ZAXIS_SCRATCH/dm2xbox-ghidra`.

`xmatch.py --layers ENGINE:GAME:SDK` gives the other games' PS2 builds their own layer bounds:
with AI's, BMX XXX's game code from 0x2021E0 to its SDK at 0x21DAA8 was out of the pool, and DM2's
unnamed Sony libraries (0x1C2728 up) were in it. Bounds, from where AI's engine, game and SDK
functions' identical and close counterparts fall: DM2 100100:150000:1C2728, BMX XXX
100100:170000:21DAA8.

| PS2 build to its Xbox build | fns | high | medium | low | inlined | none | paired by size |
|---|---|---|---|---|---|---|---|
| BMX XXX engine | 2,495 | 463 | 217 | 151 | 444 | 1,220 | 53.0 % |
| BMX XXX game | 2,516 | 898 | 258 | 204 | 338 | 818 | 84.5 % |
| DM2 engine | 2,058 | 0 | 0 | 6 | 55 | 1,997 | 0.3 % |
| DM2 game | 1,346 | 520 | 117 | 138 | 45 | 526 | 67.3 % |

(SDK: BMX XXX 6 high, DM2 3, libc by name.) Holdout (`--holdout`, every second unique-string pair
hidden): BMX XXX 82 paired the same, 4 differently (all high: 1 call, 3 order), 14 unpaired of
100; DM2 41 the same, 2 differently (a low and a medium), 33 unpaired of 76. DM2's Xbox engine
does not pair: its Xbox release was built on a different engine (0.3 % by size, against 53 % for
BMX XXX).

## 5. The GameCube builds

The three games' GameCube releases: `main.dol`, PowerPC Gekko, Metrowerks
CodeWarrior, no symbols and no `__FILE__` paths, but the engine's and game's messages the PS2
builds dropped are there (zguiScript_Load's, the "Memory leak in ..." reports) and the UI code
spells out its `$KSTR_*` keys. Extracted from the discs (read only) with
`tooling/ai/tools/gc_disc.py extract <rvz> --out <dir> sys/main.dol --sys`; outputs in
`$ZAXIS_SCRATCH/workdb/gc` (disc derived, never committed).

Tools:

| tool | what it does |
|---|---|
| `ghidra/gc_pipeline.sh` | `gc_pipeline.sh <name> <main.dol copy> <work dir>`: 1 import with Cuyler36's Ghidra-GameCube-Loader (DOL loader, the Gekko/Broadway language; its "(GameCube/Wii) Program Analyzer" turned on: it finds the r2/r13 small-data bases and resolves the references off them, 13,600 more in AI's build), full analysis, "Non-Returning Functions - Discovered" off; 2 export (`ExportXbe.java`, r1/r2/r13 left out of the struct offsets); 3 decompiler output. About 1.5 minutes each |
| `gcmatch.py` | the pairing: xmatch.py's tokens, passes and confidence rules with a GameCube side (Ghidra's functions, calls and references; floats from lfs/lfd/psq_l references; integers, struct offsets and lis+ori/addi constants decoded from the DOL's words as the MIPS side does it); `--layers`, `--holdout` as in xmatch.py |

The run (from `$ZAXIS_SCRATCH/workdb/gc`; GC_EXT_ZIP is the loader's release zip,
github.com/Cuyler36/Ghidra-GameCube-Loader release 1.3.1,
`GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip`):

```
GC_EXT_ZIP=downloads/gcloader/GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip \
  tools/ghidra/gc_pipeline.sh AiGc dol/ai.dol $PWD/ghidra-ai          # Dm2Gc dm2.dol, BmxGc bmx.dol
python3 -I tools/gcmatch.py <ai.elf> <ai.csv> <ai sce-matches.csv> dol/ai.dol \
  ghidra-ai/out/AiGc-gc.jsonl ai-gcmatch.json
python3 -I tools/gcmatch.py <dm2.elf> ... dol/dm2.dol ghidra-dm2/out/Dm2Gc-gc.jsonl dm2-gcmatch.json \
  --layers 100100:150000:1C2728
python3 -I tools/gcmatch.py <bmx.elf> ... dol/bmx.dol ghidra-bmx/out/BmxGc-gc.jsonl bmx-gcmatch.json \
  --layers 100100:170000:21DAA8
```

Ghidra: AI 5,280 functions (98.7 % of its 1,362,272 bytes of text), DM2 3,912 (98.5 % of
1,083,104), BMX XXX 5,027 (98.6 % of 1,505,952); 3,172 to 3,332 strings each. The loader's
extension.properties says 12.1; the pipeline sets it to the running Ghidra's version and compiles
its SLEIGH language with Ghidra's own `support/sleigh` inside the work dir.

| PS2 build to its GameCube build | fns | high | medium | low | inlined | none | paired by size |
|---|---|---|---|---|---|---|---|
| AI engine | 2,648 | 970 | 184 | 196 | 73 | 1,225 | 56.6 % |
| AI game | 2,728 | 1,890 | 170 | 138 | 22 | 508 | 94.0 % |
| DM2 engine | 2,058 | 851 | 137 | 157 | 50 | 863 | 58.6 % |
| DM2 game | 1,346 | 994 | 59 | 56 | 9 | 228 | 92.4 % |
| BMX XXX engine | 2,495 | 1,244 | 219 | 299 | 64 | 669 | 73.0 % |
| BMX XXX game | 2,516 | 1,506 | 106 | 72 | 23 | 809 | 87.5 % |

(The Sony SDK has no GameCube counterpart: 0 pairs.) Holdout: AI 123 of 127 paired the same, 0
differently, 4 unpaired; DM2 72 the same, 2 differently (both low, order pass), 8 unpaired of 82;
BMX XXX 124 the same, 1 differently (low), 9 unpaired of 134. So high and medium are dependable
here as on the Xbox, and the GameCube pairs much more of the code: CodeWarrior inlines far less
than MSVC's LTCG, the source is the same, and only the platform layer (the PS2's VIF/VU1 render
path, ps2Movie, ps2Chan, the memory card) has no counterpart. PowerPC against MIPS instruction
counts: log ratio mean 0.14 to 0.25, sd 0.24 to 0.39 over the unique-string pairs (gcmatch.py
uses 0.18 / 0.35). Of the readable registrations' Verified 424 (SDK aside), the AI GameCube build
has a high or medium counterpart for 340 (the Xbox 175).

What it gives the readable work: a third decompilation of the same source, closer to the PS2's
than the Xbox's (fewer inlined callees, the same argument passing in registers, floats as single
precision), the GameCube-only messages (zguiScript_Load's are in all three GameCube builds),
`$KSTR_*` names by function, and struct layouts per build (the structure zguiScript_Load, 0x17EB20, reads: AI PS2 +0x28 ..
+0x13C as in DM2's PS2 build, AI GC +0x48 .. +0x15C as in BMX XXX's builds, so AI's GameCube
release looks like a later version of that code than its PS2 release).

Installed: nothing system-wide. Downloaded into `$ZAXIS_SCRATCH/workdb/gc/downloads/gcloader`:
`GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip` (sha256
9892f28fc1e7f19bb3fcec7cfb4ad2ab7504503b571d93de8b142a6f7bec8ed0), unpacked by the pipeline into
each work dir's own Ghidra settings.

## 6. The work database (tools/workdb.py)

One DuckDB file, `$ZAXIS_SCRATCH/workdb/work.duckdb`, with everything above keyed on AI's
PS2 functions, for the readable rewrite's sweep (docs/READABLE.md "Drafts and the sweep") and the readable passes. It
is a cache: `workdb.py build` makes it from the sources (about 5 s), nothing edits it, everyone
else opens it read-only, and a build writes a new file and renames it over the old one. Run with
`$ZAXIS_SCRATCH/venv/bin/python` (duckdb 2.0.0; any other Python re-runs the script with
it); the database is disc derived and stays in scratch.

```
python3 -I tools/workdb.py build                       # rebuild (sources: `tables` lists them)
python3 -I tools/workdb.py tables                      # tables and views, row counts, when built
python3 -I tools/workdb.py query "SELECT * FROM carry_by_layer"   [--csv|--json]
python3 -I tools/workdb.py card 0x18D678 [NAME ...] [--build B] [--json] [--prefix '// '] [--width 100]
python3 -I tools/workdb.py notes [TOPIC|ADDR|NAME] [--build B] [--json]
python3 -I tools/workdb.py emit FILE KIND key=value ...   # one sweep result line
python3 -I tools/workdb.py check-results [FILE ...]
duckdb -readonly $ZAXIS_SCRATCH/workdb/work.duckdb   # or the CLI
```

`query` turns `0x1A2B` outside quotes into its value (DuckDB has no hex literals); in SQL,
`hexa(addr)` prints an address and `addr_of('0x18D678')` reads one.

Sources (each overridable, see the docstring): the three PS2 ELF copies with their Ghidra
function CSVs and SCE scans; `match3.json` and `modmap.json`; the three XBE exports with their
xmatch pairings and `__FILE__` uses; the three DOL exports with their gcmatch pairings; the
readable sources (`runtime/src/game`: registrations, `ee::Global` and `k...` address
constants, the `// FUN_xxxxxxxx:` doc comments, lines per file); every `index.json` of the
recording corpus; the ZX_FNPROF profile; main's commit messages ("N of M ... on"); the string
tables (AI and DM2, PS2 and Xbox) with the `KSTR_*` names the builds and the UI files spell out;
the sweep's result files.

Tables (layer 1):

| table | what |
|---|---|
| builds | the nine builds: platform, CPU, functions, which build each is paired to, layer bounds |
| functions | per build: address, end, size, instructions, Ghidra's name, layer (other builds: their AI counterpart's, else by bounds), section, masked hash (PS2), floats, integer constants, struct offsets (lists) |
| calls | caller, byte offset, callee per build |
| func_strings, func_globals | strings (with is_path) and other data each function references |
| file_anchors | `__FILE__` paths by function: PS2 and Xbox strings, xbe_paths.py's uses |
| modules, blocks | AI's module map (module, how, block, candidates) |
| matches | direct pairs between builds: method, class (identical .. weak, or high .. low), confidence, score, votes, the triangle check, source file |
| inlined | functions with no counterpart that a counterpart of their callers holds |
| counterparts | per AI function and build: the counterpart, direct or through DM2's or BMX XXX's PS2 build (confidence the lower of the two links), or the functions it is inlined into |
| readable, readable_symbols, readable_files | registrations (status, file, line, WAITS/VU0, doc comment); named addresses; C++ lines |
| corpus, corpus_routes, corpus_uncovered | per corpus version, route and function: calls kept, callee calls, vsyncs |
| profile, progress | the ZX_FNPROF profile; Verified counts over main's history |
| tuning | AI's tuning table (category, name, value, range, step) |
| string_keys, key_names | string table keys with their text and name; KSTR names by hash and where seen |
| names | name candidates per build and function with source, kind, confidence and evidence |
| best_names | one name per function (AI's own, else its counterpart's) for listings |

Name sources: `readable` (registration), `sce` (the SCE scan), `ghidra`/`xdk`/`function_id`
(Ghidra's names on the PS2, XbSymbolDatabase, Function ID), `message` (identifiers in a
function's messages, xmatch.py's rule, in every build: `function` when the message starts with
`name(` or `name:`), `file` (the `__FILE__` path's module), `modmap` (the module map),
`tuning` (the tuning entries it reads: directly, or through a category's entries pointer and an
entry offset), `string_key` (a `KSTR_*` key: a name hash constant on the PS2, a `$KSTR_*` string
elsewhere, with the English text), `readable_doc` (the doc comment of an unregistered address).

Views: `ai_functions` (status, module, routes, counterparts at a glance), `counterparts_wide`
(one column per build), `card` (one row per function with lists), `ai_names` (every candidate
from every build through the counterparts, with the pair's confidence), `uncovered` (non-SDK
functions no route of the newest corpus reaches), `carry_by_layer`
and `carry_by_status` (counterparts by confidence per layer or readable status, with shares by
size), `sweep_latest`, `sweep_failure_classes`, `engine_lineage` (below).

Carry-over of AI's functions when the database was first built (best counterpart; size = share of the layer's
instructions with a high or medium counterpart; `carry_by_layer` gives today's):

| build | engine high / medium / low | engine size | game high / medium / low | game size |
|---|---|---|---|---|
| DM2 PS2 | 1,407 / 316 / 149 | 55.9 % | 94 / 91 / 91 | 6.4 % |
| BMX XXX PS2 | 1,824 / 236 / 62 | 80.4 % | 810 / 171 / 54 | 27.5 % |
| AI Xbox | 352 / 175 / 123 | 35.3 % | 733 / 293 / 254 | 69.2 % |
| DM2 Xbox (via DM2 PS2) | 0 / 1 / 8 | 0.0 % | 11 / 27 / 34 | 1.8 % |
| BMX XXX Xbox (via BMX PS2) | 281 / 230 / 174 | 33.2 % | 214 / 128 / 102 | 18.8 % |
| AI GC | 970 / 184 / 196 | 53.2 % | 1,890 / 170 / 138 | 92.4 % |
| DM2 GC (via DM2 PS2) | 552 / 273 / 236 | 30.7 % | 50 / 71 / 84 | 4.8 % |
| BMX XXX GC (via BMX PS2) | 829 / 315 / 295 | 51.0 % | 249 / 107 / 47 | 17.7 % |

### The sweep's results (layer 2)

Sweep jobs and agents never write the database. Each appends JSON lines to its own file under
`$ZAXIS_SCRATCH/workdb/results/` (any depth, `*.jsonl`); the next build ingests them,
rejected lines go to `ingest_errors` with the reason. A line:

```
{"kind": "check", "addr": "0x0018D678", "run": "sweep-a", "generator": "readable_draft.py <commit>",
 "ts": "2026-10-24T13:00:00", "route": "lc", "result": "pass", "calls": 100, "failed": 0}
```

`kind`, `addr`, `run` (the sweep run), `generator` (the draft generator's version) and `ts` are
required; `agent` is optional; fields a kind does not know are kept in `extra` (JSON).

| kind | table | required | values | optional |
|---|---|---|---|---|
| draft | sweep_draft | result | drafted, failed, skipped | reason, file, lines, gotos, name |
| build | sweep_build | result | ok, compile_error, link_error, set_aside | error, build_dir |
| check | sweep_check | route, result | pass, fail, no_recording, crash, timeout, skipped | calls, failed, first_failure, cycles_equal, game, impl, orig, rec, checker |
| coverage | sweep_coverage | blocks, hit | counts | game, calls, files, uncovered (list), corpus, orig, checker |
| failure | sweep_failure | stage, failure_class | stage: draft, build, check, register; class free | detail |
| status | sweep_status | status | Draft, Tested, Verified, Planned | registered, note |
| map | func_map | build, role | build: one of the nine | name, port, confidence, module, covered, size, callers, callees, note, counterparts (JSON) |
| pair | func_pairs | build, b_build, b_addr, confidence | builds: the nine; high, medium, low | method, cls, ratio |
| note | func_notes | topic, text | topic: purpose, cut, bug-kept, trigger, route, layout, evidence, caveat, history | build (default ai_ps2; any of the nine), agent, source |

`map` and `pair` lines are keyed by their own `build` and `addr` (any build, not only AI's PS2):
a function mapped by hand for a port (the DM2 rider code,
`results/riderplan/rider-map.jsonl` by `tools/ridermap.py`) and pairs the match does not make
(`results/riderplan/dm2-bmx-pairs.jsonl`: DM2 -> BMX XXX PS2, 2,674; superseded by `dm2bmx.py`'s, below). The views
`func_map_latest` and `func_pairs_latest` give the newest line per function (schema version 2).

### DM2 and BMX XXX

`tools/dm2bmx.py OUT.json <workdb>/results/d1data --run d1data` makes the DM2 -> BMX XXX PS2 match:
fmatch3.match_pair on the two ELFs (as `ridermap.py pair`, which gave the same 2,674 pairs on today's
function maps), then a second run seeded with those pairs and the triangle's (a DM2 and a BMX XXX
function the first left unpaired whose AI counterparts are one AI function, identical or close on
both sides: 6), which grows them to 2,687: identical 1,446, close 291, drifted 591, weak 359. Each
line keeps fmatch3's method, ratio, opcode ratio, callee consistency and the triangle's verdict
(agree 2,141, disagree 48, none 498). `build` copies the newest run's pairs into `matches`
(a_build dm2_ps2, b_build bmx_ps2, source `results:d1data`, triangle true/false), after AI's
counterparts, so no AI chain goes through them. By class of the two sides (DM2's engine end
0x163280, BMX XXX's 0x190340): engine to engine 1,637, game to game 488, library to library 546,
16 across; of DM2's 1,309 game functions 498 pair; of BMX XXX's 2,047 493, 390 of the 1,319 with no
AI body. Of the 332 BMX XXX game functions whose masked body equals a DM2 function's, 164 pair: the
rest are small bodies (under 6 instructions, or in equal-hash groups) the match leaves alone.

DM2's library bound is 0x1CC800 (sceGsResetGraph), not 0x1C2728: the SCE scan's `_InitSys` there is
a game function (SCE_FALSE in workdb.py), and the 173 functions between are DM2's level object types
(EditObj_GapMarker .. SpecialFX/zfxMgr.c). The Xbox and GameCube pairings were made with the old bound.

### The engine's lineage

View `engine_lineage`: one row per engine function across ZGE2 (DM2 PS2, Aug 2001), ZGE3 (AI PS2,
Apr 2002) and ZGE4 (BMX XXX PS2, its build string Oct 16 2002), from the three games' direct links
(a version a direct link misses is taken through the third game's): the three addresses, the name,
each step's class (identical when the masked bodies are equal, else the direct pair's class), a
lineage, the instruction counts and AI's readable file. Engine = below each ELF's engine end (DM2
0x163280, AI 0x185D20, BMX XXX 0x190340) and not sdk, or an AI function in zge/. The card prints it.
A version "not in an ELF" is not linked there (BMX XXX's link drops unreferenced functions), which
does not say its source lost it.

| lineage | rows |
|---|---|
| the same in all three | 855 |
| changed in ZGE3, kept in ZGE4 | 395 |
| changed in ZGE3 and in ZGE4 | 322 |
| kept in ZGE3, changed in ZGE4 | 116 |
| changed in ZGE3, ZGE4 has the ZGE2 body | 0 |
| new in ZGE3, kept in ZGE4 / changed in ZGE4 | 450 / 293 |
| kept in ZGE3 / changed in ZGE3, not in ZGE4's ELF | 258 / 82 |
| not in ZGE3's ELF, kept / changed in ZGE4 | 9 / 9 |
| ZGE2 only / ZGE3 only / ZGE4 only | 268 / 218 / 506 |
| all (ZGE2 2,314, ZGE3 2,989, ZGE4 2,955 functions) | 3,781 |

Dating (`tools/lineage_gc.py`): the GameCube builds are one compiler for AI and BMX XXX (of the
pairs identical on PS2, 630 of 800 are the same masked PowerPC code on GameCube) and not for DM2 and
AI (65 of 427), so only an equal GameCube pair is evidence. Of 429 ZGE3 -> ZGE4 changes with both
GameCube counterparts, 67 have AI's GameCube code equal to BMX XXX's: ZGE4's form was in the engine
of AI's GameCube release (built after its Dolphin OS kernel of June 5 2002), after AI's PS2 build or
in PS2-only code; of 433 ZGE2 -> ZGE3 changes, 3 are dated so. They are `history` notes on AI's
functions (`results/d1data/lineage-notes.jsonl`, 70). The Xbox builds are not used: DM2's Xbox
engine does not pair (section 4) and MSVC's inlining hides the bodies. Build dates in the
executables: DM2 Xbox XBE Nov 9 2001; DM2 GameCube Dolphin OS Sep 8 2001; AI GameCube Dolphin OS
Jun 5 2002; AI Xbox XBE Jul 2 2002; BMX XXX PS2 Oct 16 2002, Xbox Oct 22 2002, GameCube Nov 3 2002.

A `check` line names what the pass was made against (zx-fncheck's result line,
docs/READABLE.md "How a function is proven"): `impl` the readable function's source digest
(`runtime/tools/readable_digest.py`), `orig` the generated original's, `rec` the recording's
recorder version and corpus folder (`6:v5/lc`), `checker` the zx-fncheck executable's. A
`coverage` line (`tooling/checks/fncheck_corpus.py emit`) gives a function's path coverage over a
corpus: the original's basic blocks, how many the replayed calls ran, the uncovered blocks'
addresses. The build also runs readable_digest.py on the readable sources (source
`readable_digest`, next to `game_src`) into `readable_digests`: each registration's current digest.
Views (schema version 3): `coverage_latest` (the newest coverage line per function with its share),
`stale_verified` (Verified registrations whose current digest has no passing check: never checked
with a digest, or edited since their last pass).

A `note` line is a fact about a function that is not a measurement, keyed by `build` (default
`ai_ps2`) and `addr`, with a `topic` and the fact as `text` (a sentence or two, self-contained, with
the names and addresses it needs), `source` a doc path with a line, or a commit. Topics: purpose
(what it does), cut (a feature the original has and the port drops, or dead code and what it was
for), bug-kept (an original bug the readable code keeps), trigger (what calls it or makes it run),
route (how a recording or play reaches it, or that none does), layout (field, struct or table
evidence), evidence (how a fact was established), caveat (a trap, or why it stays Draft), history
(a past change worth knowing). Notes are appended, never edited; the same text twice is shown once.
They go to `func_notes`; `workdb.py card` shows the first twelve under the function and
`workdb.py notes TOPIC|ADDR|NAME [--build B]` lists them all (`notes` alone counts them per topic).
Schema version 4. The notes' text, keyed by function name (no addresses of its own, the sources left out), is exported to
`tools/workdb_notes.jsonl` (`workdb.py export-notes`); `card` and `notes` read that export when there is no
database or no duckdb module, or with `WORKDB_PUBLIC=1` (`tools/workdb_notes.py`).

`workdb.py emit FILE KIND key=value ...` writes one checked line (values parsed as JSON when they
parse, `ts` filled in); from Python, `workdb.append_result(path, record)` (a locked append of one
whole line). `sweep_latest` gives per function the newest draft, build, checks per route (pass /
fail / other), failure and status; `sweep_failure_classes` counts functions per newest failure
class (the sweep's loop: fix the generator for the biggest class, redraft).

### The card (`workdb.py card`)

Everything known about one AI function across the nine builds, as comment lines a draft's header
can hold: layer, size, module and block; the registration and its doc comment; corpus routes with
calls kept (or why there are none); the sweep's newest results; the counterpart in each of the
eight other builds with confidence, method, the link it went through and its size (or the
functions it is inlined into); name candidates grouped by name and source with the builds that
give them and a sample message; string keys with their text; tuning entries; `__FILE__` paths;
strings with the builds that have them; float constants; struct offsets per build (the lists
side by side show where a layout moved: the skater's Player on Xbox and GameCube); globals with
their readable names; callers and callees (in call order) with their best names. An address
inside a function finds the function; a name is looked up in every build's candidates. About
25 ms a card (100 cards in 2.4 s in one process).

`runtime/tools/readable_draft.py` puts a function's card in each draft's header, after its "Draft by
readable_draft.py" line and before Ghidra's decompilation comment (`--workdb PATH`, default
`tooling/dm2/tools/workdb.py` or WORKDB_PY; `--no-workdb`; with the database missing, or an address that is not an
AI function, it drafts without the card). The card names the database's build time; it is a comment, so
zx-fncheck results do not depend on it.

Not checked: the GameCube pairs read side by side by hand (the holdout is the only precision measure); the tuning
candidates read through the category pointer (a displacement in a function that also reads the category could be
another struct's); the string-key candidates beyond their hash matching a named key.

## 7. The readable write-back

The decompilations read with the readable rewrite's names, types and struct fields: what the
readable code knows is written into a copy of each analysed Ghidra project, read back, and the
exports are made again from the copy. Everything it writes holds the games' addresses:
scratch only (`$ZAXIS_SCRATCH/ghidranames`).

| tool | what it does |
|---|---|
| `readable_ghidra.py` | the input. `scan`: clang over the readable translation units (every `runtime/src/game` source in a `compile_commands.json` under the checkout's `out/`, with its flags; cached by a digest of the sources): `-fdump-record-layouts-complete` for every struct's offsets and size, and the JSON AST of the namespaces aggressive, zge and sce for the registrations (address, the registered function's C++ signature and parameter names), the ee::Global variables, enums and typedefs. `gen BUILD DIR`: `readable-BUILD.h` (the guest structs, packed with every gap spelled out so the offsets are clang's; the prototypes; the globals' types) and `readable-BUILD.jsonl` (a meta line with the header's SHA-256, the readable commit and the work database's build time; then the expected layouts, the names, the signatures with their EABI registers, the globals). `all DIR`: the nine builds. `stats BEFORE.c AFTER.c`: counts over two decompilations |
| `tooling/ai/ghidra/scripts/ApplyReadable.java` | the stage. The header goes through Ghidra's C parser into the category `/readable`. Function names: the analysis name is kept in the `ReadableOriginalName` property map; a name a loader, Function ID or the SCE scan gave (the libraries' own) stays, and the readable one becomes a label beside it. Signatures: ApplyFunctionSignatureCmd with the compiler spec's default convention. Globals: typed data, larger first (a global inside a struct global stays a label). Each function gets a plate comment saying where its name came from. Then everything is read back: struct sizes and each field's offset, name and size; names; parameter types, names and registers against the EABI (a0-a3, t0-t3, f12-f19); return types. Any mismatch fails the stage. The input's and the header's SHA-256 go into Program Information. `verify` runs the read-back alone (read only) |
| `ghidra/readable_pass.sh` | `readable_pass.sh <ps2\|xbe\|gc> <name> <work dir> <program> <readable-BUILD.jsonl>`: copies `<work>/project` to `<work>/project-readable` and applies there. The base project stays as analysed: its CSV names the recompiled functions and its jsonl feeds the pairings. Then, read only: verify, the export again (xbe/gc) and the decompilation, into `<work>/readable/` |
| `ExportXbe.java`, `ExportDecomp.java` | on a readable copy, ExportXbe writes the analysis name as `"n"` (so xmatch, gcmatch and workdb read what they always read) and the readable one as `"rn"`. ExportDecomp starts with a line that names the input and its SHA-256 |
| `xpair.py` | a call to a function that the readable stage named on the Xbox side gets its PS2 counterparts, as a FUN_ call does |

The pipelines run it as their last stage when `READABLE_BUILD` names the build:
`pipeline.sh` stage 10, `xbe_pipeline.sh` stage 5, `gc_pipeline.sh` stage 4. AI's own
pipeline (`tooling/ai/ghidra/scripts/pipeline.sh`) runs it as stage 9.

What each build takes:
- AI PS2 gets everything: the registrations, the readable constants that name a function
  (`kSetEye` read as `setEye`), the Planned names, the signatures, the structs and the globals.
- DM2 and BMX XXX PS2 get the same through identical matches only (the same machine code, so the
  same signature and offsets).
- AI Xbox and GameCube get names only, through high and medium counterparts. LTCG and PowerPC
  pass arguments their own ways, and struct layouts can differ by platform.

No two AI names fold into one counterpart in these five builds (the generator would apply
neither). A name used twice gets the address (`allocate_00117AC0`).

On copies of the five projects every apply and every read-only verify reports
0 mismatches. Before = a fresh ExportDecomp of each base copy, byte-identical to the decompilations
in use (`ai-decomp.c`, `dm2-decomp.c`, `BmxXXX-decomp.c`, `AiXbox-decomp.c`, `AiGc-decomp.c`).

| build | names applied (renamed, library names kept) | signatures | structs | typed globals | FUN_ refs | raw `*(T *)(x + 0xN)` | `->field` | `param_N` |
|---|---|---|---|---|---|---|---|---|
| AI PS2 | 883 (864, 13; 6 already so) | 507 (2 skipped: ee::Qword) | 176 (135 with static_asserted offsets) | 272 of 287 | 26,851 -> 15,664 | 19,585 -> 14,556 | 213 -> 8,073 | 36,352 -> 32,760 |
| DM2 PS2 | 205 (190, 11) | 105 | 176 | - | 20,157 -> 15,770 | 12,681 -> 12,434 | 212 -> 1,102 | 26,284 -> 25,503 |
| BMX XXX PS2 | 300 (283, 12) | 156 | 176 | - | 30,199 -> 23,404 | 21,643 -> 20,419 | 211 -> 2,135 | 35,564 -> 34,421 |
| AI Xbox | 301 (301; 171 high, 130 medium) | - | - | - | 25,806 -> 21,981 | unchanged | unchanged | unchanged |
| AI GameCube | 646 (646; 561 high, 85 medium) | - | - | 29,613 -> 23,410 | unchanged (+1) | unchanged | unchanged |

AI PS2's names: 509 registrations, 301 readable constants and 73 Planned. 310 of the 507
signatures take a pointer to a readable struct (51 structs used). AI PS2's `DAT_` references fall
from 17,467 to 12,910. DM2's rise by 184 and BMX XXX's by 434. The rise sits in a few functions,
such as DM2 0x10B5F0 and BMX XXX 0x10D518, where a callee's new prototype ended Ghidra's "type
propagation algorithm not settling" and the output got fuller.

Examples (AI PS2 unless noted):
- `void FUN_0018e858(float param_1,undefined8 param_2)` becomes
  `void updateFollowCamera(FollowCamera *camera,float seconds)`, and in it
  `if (*(int *)(iVar5 + 4) == 0)` reads `if (camera->player == (Player *)0x0)`. Likewise
  `*(undefined4 *)(iVar2 + 0x1ec) = *(undefined4 *)(iVar2 + 0x1e8)` reads
  `pPVar2->previousSpeed = pPVar2->speed`, and `FUN_00136f00(*(int *)(iVar5 + 4) + 0xa0)` reads
  `vec3Length((Vec3 *)&camera->player->velocity)`. Ghidra had listed the float first (f12
  before a0) and left the camera an undefined8.
- `void FUN_0018cc50(float param_1,float param_2,undefined8 param_3,undefined1 (*param_4) [16])`
  becomes `int turnCameraView(FollowCamera *camera,uint direction,float seconds,float distance)`.
- railFilter 0x187C90 (named from `kRailFilter`): `*(float *)(PTR_s_Min_Grind_Angle_00249bb7_1_0024af58 + 0x950)`
  reads `g_tuningCategories[0xe].entries[0x31].value`.
- DM2 0x11C890: `bool FUN_0011c890(undefined8 param_1,undefined4 *param_2)` becomes
  `int loadTextureHeaderChunk(ChunkReader *reader,TextureLoadContext *context)` (identical to AI
  0x1282E0), with `readChunkData`, `findTexture` and `nameTexture` for three FUN_ calls and
  `context->texture` for `*param_2`.
- AI Xbox 0x32BB0 is `turnCameraView` (medium), and its calls read `quaternionToMatrix`,
  `matrixToQuaternion`. AI GameCube 0x8007C210 is `updateFollowCamera`, with `updateCameraLookAt`
  for `FUN_80079ebc`.

Checks on the new exports:
- The readable Xbox and GameCube jsonl equal the base exports line for line once `"rn"` is
  dropped (5,687 and 5,281 lines; 301 and 646 `"rn"`).
- Two scratch work databases were built from the same sources, one with those two exports
  swapped in: all 33 tables (meta and sources aside) hold the same rows.
- `xpair.py` reads the readable PS2 and Xbox decompilations; on 0x18CC50 it annotates the same
  8 counterpart calls as on the base files, and its base output is unchanged.
- `readable_draft.py --decomp <readable AI decomp>` reads it. Its draft of
  0x18E858 names the parameters `camera` and `seconds` (`param_2` and `param_1` from the base
  file).
- Not checked: dmatch.py on the readable files; Ghidra's GUI on a readable project.

Rerun:

```
python3 -I tools/readable_ghidra.py all $ZAXIS_SCRATCH/ghidranames/gen     # or gen BUILD DIR
AI_SCRIPTS=<folder with ApplyReadable.java> tools/ghidra/readable_pass.sh ps2 AggressiveInlineCopy \
  <work dir> SLUS_203.27 $ZAXIS_SCRATCH/ghidranames/gen/readable-ai_ps2.jsonl
# in a pipeline, from its last stage: READABLE_BUILD=bmx_ps2 tools/ghidra/pipeline.sh BmxXXX elf/SLUS_204.15 ghidra-bmxxxx 10
python3 -I tools/readable_ghidra.py stats <base decomp.c> <work dir>/readable/<name>-decomp.c
```

The scratch folder holds the copies (`<build>/project`, `project-readable`, `readable/`, `base/`),
`run-all.sh` (all five, one Ghidra at a time; about 5 minutes) and `base-decomp.sh`.

### The other four builds, and types on the Xbox and GameCube

DM2's and BMX XXX's Xbox and GameCube builds take names through their high and medium
counterparts: AI's function, its pair in the game's PS2 build, and that one's pair in the Xbox or
GameCube build (the work database's `counterparts` with `via`; the confidence is the lower of the
two links). All six Xbox and GameCube builds also take structs and signatures, where shown:

- **Struct layouts, per struct and build** (`readable_ghidra.py layouts BUILD [--json]`). AI PS2's
  readable decompilation spells each field access (`camera->player->speed`): per function, the
  struct and the displacement its instruction uses (field offsets summed up to each
  dereference; only displacements AI PS2's own instructions hold count). The counterpart on the
  build (high or medium) has its own instructions' displacements (the work database's `disps`).
  A struct is *same* when no informative displacement (0x10 and up) fails in most of two or more
  counterparts or shows up consistently elsewhere, at least 80 % of its observations hold, and
  there are two or more; *small* when it is under 0x10 bytes and every displacement holds twice
  or more (0 not counted on x86, where `[reg]` has none); *differs* or *unknown* otherwise. Only
  same and small structs go into the build's header; a pointer to any other struct becomes
  `void *` (the parameter keeps its name), a struct holding one by value is left out.
  The check finds the known Xbox Player: +0x74 is at +0x84 in 30 of 32 pairs, +0x550 at +0x5C0
  in 7 of 7, +0x214 at +0x224. The GameCube Player differs too (+0x74 at +0x68 in 30 pairs).
  Player is skipped on both; a per-build Player layout is not made (the shifts are known only
  where a field is used, see "Not done").
  Run on DM2's and BMX XXX's PS2 builds as a control, the check finds there the same structs
  differing as on their Xbox and GameCube builds (BMX XXX's ChunkReader +0x50 at +0x54 in 9
  pairs, its TextWidget and zge_Node; DM2's CollisionWorld): those are the games' own versions,
  not the platforms'. (The PS2 builds take structs through identical functions only, whose
  offsets are AI's.)
- **Signatures**: high counterparts only, and only where Ghidra's own analysis of the counterpart
  (its decompilation's prototype) takes the same arguments in the same places. GameCube: the
  PowerPC EABI (integers and pointers r3-r10, floating point f1-f8; floats as `double`, which is
  what the FPRs hold), the integer and float counts equal to Ghidra's. Xbox: the convention
  Ghidra found is kept (`__cdecl`, `__stdcall`: all on the stack; `__fastcall`: the first two
  integers in ECX, EDX); `__thiscall` and LTCG's own register conventions are skipped. Wide
  arguments (8-byte integers, x86 doubles) are skipped, and a float return against an integer
  one. ApplyReadable.java takes the record's `cc` and reads it back with each parameter's
  storage.

`tools/exports.py` says where each build's exports are: the base ones (as analysed) and the
readable ones in one folder, `$ZAXIS_SCRATCH/readable-exports/<build>/` (`decomp.c`,
`export.jsonl` for Xbox and GameCube, `source.json` with where they came from and their
SHA-256; `exports.py install BUILD WORKDIR` after a readable pass). The tools take the readable
files by default:

| tool | default | the originals |
|---|---|---|
| `workdb.py build` | the six readable Xbox/GameCube exports (they name the analysis name `"n"`, so the tables read as before; `meta.exports` says which) | `--base-exports` |
| `xpair.py` | `xpair.py ai_xbox\|dm2_xbox\|bmx_xbox ADDR ...`: the pairing and both readable decompilations; any decompilation argument may be a build name | `--base` |
| `dmatch.py` | build names as arguments; a call by a readable name counts as the call to its address | `--base` |
| `show.py` | build names; `--callers` finds a named function's callers by its name | `--base` |
| `datzip.py` | the base files (it pairs `DAT_` addresses; the readable AI PS2 file names its globals) | `--readable` for the readable ones |
| `ridermap.py`, `readable_ghidra.py gen` | read the work database only (ridermap), or AI PS2's readable decompilation for the layout check | - |

READABLE_EXPORTS=base in the environment turns every default back. A build with no readable
file falls back to its base file with a warning. `readable_draft.py --decomp` still defaults to
`tooling/dm2/ai-decomp.c`; the readable one is `python3 -I tools/exports.py path ai_ps2 decomp`.

On copies of the six Xbox and GameCube projects every apply and read-only verify reports 0 mismatches.
Before = the base decompilation each pipeline wrote.

| build | names (renamed, library kept) | structs written (layout check: same, small, differs, unknown) | signatures (of high readable ones) | FUN_ refs | raw `*(T *)(x + 0xN)` | `->field` | `param_N` |
|---|---|---|---|---|---|---|---|
| AI Xbox | 301 (301, 0) | 22 (16, 8, 3, 149) | 35 of 105 | 25,806 -> 21,941 | 25,442 -> 24,974 | 139 -> 475 | 53,002 -> 52,152 |
| AI GameCube | 646 (646, 0) | 36 (21, 17, 4, 134) | 267 of 328 | 29,613 -> 23,410 | 22,366 -> 18,376 | 0 -> 1,053 | 40,350 -> 36,523 |
| DM2 Xbox | 6 (5, 1) | 0 (no evidence) | 0 | 30,068 -> 30,034 | unchanged | unchanged | unchanged |
| DM2 GameCube | 148 (148, 0) | 9 (5, 7, 1, 163) | 43 of 44 | 25,147 -> 23,326 | 15,107 -> 14,994 | 0 -> 166 | 28,943 -> 28,484 |
| BMX XXX Xbox | 116 (116, 0) | 14 (9, 6, 3, 158) | 11 of 50 | 31,069 -> 28,435 | 29,338 -> 29,185 | 139 -> 168 | 55,542 -> 55,243 |
| BMX XXX GameCube | 243 (243, 0) | 23 (14, 10, 4, 148) | 73 of 105 | 33,980 -> 30,108 | 25,783 -> 24,671 | 0 -> 513 | 34,379 -> 33,445 |

Names only (the four new builds before types, `readable-names/`): the FUN_ counts above, every
other count unchanged. Structs that differ: AI Xbox Player, Font, ZgePart; AI GameCube Player,
FollowCamera, SphereQuery, zge_Node; DM2 GameCube CollisionWorld; BMX XXX Xbox ChunkReader,
TextWidget, ZgePart; BMX XXX GameCube ChunkReader, SphereQuery, TextWidget, zge_Node. Signatures
skipped (AI Xbox): 31 for the convention (`__thiscall` or LTCG's own), 29 arguments placed
otherwise than Ghidra's analysis, 8 returns (7 of them the EE's 64-bit long), 1 wide argument, 1
with no prototype; AI GameCube: 45 arguments, 7 returns, 6 wide, 3 no prototype.

Examples:
- AI GameCube 0x8005392C, `undefined4 FUN_8005392c(undefined4 param_1,int param_2)` becomes
  `int loadMaterialTableChunk(void *reader,GeoLoadContext *ctx)`, with
  `ctx->materials = pvVar2` for `*(int *)(param_2 + 0x1c) = iVar2` and `attachMaterialTable`,
  `chunkDataSize`, `enterChunk`, `parseChunks` for four FUN_ calls.
- AI GameCube 0x8007C210, `void FUN_8007c210(double param_1,int param_2)` becomes
  `void updateFollowCamera(void *camera,double seconds)`: FollowCamera's layout differs there
  (+0x72 at +0x5E), so the camera stays untyped, and the arguments are in the source's order.

Checks:
- The readable Xbox and GameCube exports equal the base ones line for line without `"rn"` (all
  six; 301, 646, 5, 116, 148, 243 `"rn"`).
- Two scratch work databases (`workdb.py build --base-exports` and the default, the readable
  exports): all 33 tables (meta and sources aside) hold the same rows. No name changes: the
  database reads the analysis name `"n"`; the readable one is AI's name, which `best_names`
  already gives through the counterparts.
- `xpair.py ai_xbox 0x18CC50 0x18E858` and `xpair.py bmx_xbox 0x122528 0x1243D8`: the same
  counterpart annotations on the readable files as with `--base` (42 and 12).
- `dmatch.py ai_ps2 dm2_ps2` on five AI functions: the same best DM2 candidate on the readable
  files as on the base ones (scores within 0.01, loadTextureHeaderChunk and vec3Length at 1.0).
- Not checked: Ghidra's GUI on a readable project; a recomputation of xmatch or gcmatch from the
  readable exports (they read `"n"`, and the files are otherwise equal).

Rerun:

```
python3 -I tools/readable_ghidra.py all <gen dir>
python3 -I tools/readable_ghidra.py layouts ai_gc          # the per-struct evidence
AI_SCRIPTS=<ApplyReadable.java with the cc change> tools/ghidra/readable_pass.sh gc AiGc <work dir> ai.dol <gen dir>/readable-ai_gc.jsonl
python3 -I tools/exports.py install ai_gc <work dir>
```

The scratch folder holds `run-all.sh` (the six, one Ghidra at a time, about 13 minutes),
`after.sh` (install, counts, export equality) and `workdb-check/check.sh`.

## Not done, and next

- Names for the unnamed blocks: the fragment's candidates plus the readable folders would name
  most engine blocks by hand (zmath*, zquat, ziff, zstring, zlist, zcollide ...); not done.
- A finer block rule (local calls tie neighbouring objects; a call-only link could be dropped where
  the data says otherwise) and a decompiler-text pass (dmatch.py) for the 776 engine functions
  with no DM2 counterpart.
- The work database's next layers: struct offsets per build as a table of field pairs (the
  Player's on Xbox and GameCube), the GameCube pairs read by hand, DM2's and BMX XXX's string
  tables from their discs, the corpus per-function caps the sweep needs.
- The readable write-back (section 7): DM2's and BMX XXX's own globals (their addresses through
  the data matches); per-build layouts for the structs that differ (the Player on Xbox and
  GameCube: the layout check gives each used field's shift, e.g. Xbox +0x74 -> +0x84,
  +0x550 -> +0x5C0, GameCube +0x74 -> +0x68, but not the fields no counterpart uses, nor the
  size); `__thiscall` and LTCG register conventions on the Xbox; medium counterparts' signatures;
  the 134-164 structs per build with too little evidence (most are used by no paired function);
  DM2 Xbox (6 names: its pairing with DM2 PS2 holds 523 high pairs, 53 of them with an AI
  counterpart); readable_draft.py's `--decomp` default.
- The three-way match is static. "Identical" means the masked machine code is equal, not that a
  readable function was checked there: that is each game's own recording corpus and zx-fncheck
  (docs/READABLE.md "How a function is proven").
