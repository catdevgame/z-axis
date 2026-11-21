# The three-way function match: Aggressive Inline, Dave Mirra BMX 2, BMX XXX

The three games run on consecutive versions of Z-Axis's engine: Dave Mirra BMX 2 `SLUS_201.59` (ZGE2, Aug
2001), Aggressive Inline `SLUS_203.27` (ZGE3, Apr 2002) and BMX XXX `SLUS_204.15` (ZGE4, Oct 2002; it was
"bmx3"). Each also has an Xbox and a GameCube release: nine builds in all. This file says how the tools pair the
functions of the nine builds, and what the work database built on those pairs holds. The match says which of the
readable C++ written for AI (`engine/zge`, `games/aggressive`) can carry over to DM2 and BMX XXX, and which engine module each of
AI's functions belongs to. Everything is keyed on AI's PS2 functions.

Section 6 is the work database's reference; `tools/workdb/workdb.py`'s docstring is its usage. For a function's current
facts, look it up (`workdb.py card ADDR|NAME`); for current counts, `workdb.py tables` and the views.

Nothing here is game data: the tools are committed, and their outputs (function maps, the match, the module map,
decompiler output, everything with the games' addresses) go to a scratch folder outside the checkout
(`$ZAXIS_SCRATCH/match3`) and are never committed.

## Tools (`tools/workdb/`, Python run with `-I`, disc files read as data)

| tool | what it does |
|---|---|
| `ghidra/pipeline.sh` | The Ghidra pipeline for any of the engine's PS2 executables: `pipeline.sh <name> <elf copy> <work dir>`. The scripts are AI's (`games/aggressive/workspace/ghidra/scripts`, copied into the work dir) plus `ghidra/ExportDecomp.java`; it installs the R5900 extension into the work dir's own Ghidra settings. Stages 1-8 as its header lists them, 9 = full decompiler output |
| `xbe_paths.py` | `extract`: default.xbe out of an Xbox image (read only, with `tools/modding/xbox_files.py`); `paths`: the __FILE__ strings in an XBE and the x86 instructions that push them |
| `fmatch3.py` | the three-way match (below); writes `match3.json` |
| `readable_carry.py` | every readable registration with its counterpart in DM2 and BMX XXX and what differs; writes `readable-carry.json`, `--list` prints the identical ones |
| `modmap.py` | AI's functions by module (below); writes `modmap.json`, `--readable` adds the readable functions per block |
| `ghidra/xbe_pipeline.sh`, `xmatch.py`, `xpair.py` | the Xbox build as a second decompilation, paired with the PS2's (section 4) |
| `readable_ghidra.py`, `ghidra/readable_pass.sh` | the readable write-back: the readable rewrite's names, signatures, structs and globals applied to a copy of a Ghidra project, then the exports again (section 7) |
| `exports.py` | where each build's decompilation and function export are, as analysed and readable; the tools that read them take the readable ones by default (section 7) |
| `dm2bmx.py` | DM2 -> BMX XXX PS2: fmatch3's match, then again seeded with its pairs and the triangle's; writes the work database's `pair` results (section 6, "DM2 and BMX XXX") |
| `lineage_gc.py` | the engine changes the GameCube builds date, as `history` notes (section 6, "The engine's lineage") |
| `games/dm2/tools/dead_code_dm2.py` | DM2's dead-code table (docs/DEAD-CODE.md, "Dave Mirra BMX 2") |

The run, from the scratch folder. Inputs: copies of the three ELFs in `elf/`; AI's and DM2's function maps from
their Ghidra pipelines (`games/dm2/workspace/ghidra-ai`, `recomp/ghidra-functions.csv`); BMX XXX's from `pipeline.sh`; the
XBEs in `xbe/`.

```
tools/workdb/ghidra/pipeline.sh BmxXXX elf/SLUS_204.15 ghidra-bmxxxx
python3 -I tools/workdb/xbe_paths.py extract "<BMX XXX-xbox.iso>" xbe/bmxxxx.xbe   # and DM2's, AI's
python3 -I tools/workdb/xbe_paths.py paths xbe/bmxxxx.xbe xbe/bmxxxx-paths.json
python3 -I tools/workdb/fmatch3.py elf/SLUS_203.27 <ai.csv> elf/SLUS_201.59 <dm2.csv> elf/SLUS_204.15 ghidra-bmxxxx/out/ghidra-functions.csv match3.json
python3 -I tools/workdb/readable_carry.py match3.json <the six inputs> <z-axis> readable-carry.json --list
python3 -I tools/workdb/modmap.py match3.json --readable readable-carry.json <ELF, functions, sce-matches of each game> modmap.json xbe/*-paths.json
```

`pipeline.sh` on BMX XXX takes about 2.5 minutes for stages 1-8 and 30 s for the decompiler output.

## 1. The match (fmatch3.py)

Per pair (AI to DM2, AI to BMX XXX): fmatch.py's passes (address-masked exact hash, unique string
sets, call-graph propagation through aligned call sites) plus equal-hash groups bracketed by
matched neighbours, an order pass (an unmatched function between two matched neighbours is
compared with the unmatched functions between their counterparts: link order is mostly kept),
and a call-graph neighbour pass. Similarity for accepting a pair: 0.4 masked-token ratio + 0.4
opcode-class ratio + 0.2 shared matched callees. Classes by the masked-token ratio r: identical
(masked bodies equal), close (r >= 0.85), drifted (0.5 to 0.85), weak (matched by call graph or
order, r < 0.5), none.

AI's layers, by AI address: sdk is an SCE-scanned name, crt0, or 0x2021E0 up; engine is 0x100100-0x171000; game is
0x171000-0x2021E0. `carry_by_layer` (section 6) gives each layer's counterparts by class.

What the match shows about the games:
- BMX XXX is much closer to AI than DM2 is, in the engine and in the game layer. Most of AI's game functions that
  BMX XXX has unchanged are the park editor (AI 0x1F0000-0x1FFFFF, `GameTools/ParkEditor/editToolSet.c`, which BMX XXX
  inherited) and zgui with the UI script interpreter (0x170000-0x18FFFF).
- BMX XXX's SDK is AI's release, not a newer one: its library version strings are AI's (PsIIlibkernl2430 ...
  PsIIlibpkt 2400, plus libscf 2400), and every library function that pairs is AI's code. AI's SDK functions with
  no counterpart in BMX XXX are ones its link left out: nothing referenced them. DM2 links release 2.2 (every
  version string 2200). `games/dm2/libraries.tsv` and `games/bmx/libraries.tsv` pin each library.
- DM2's game layer shares almost nothing with AI's: the identical pairs are mostly small helpers.

## 2. The readable functions (readable_carry.py)

Every readable registration with its counterpart in DM2 and BMX XXX and what differs. For identical pairs, "table
ok" means every call goes to the matched counterpart of AI's callee and every global maps as the rest of the match
maps it (so a per-game address table is all the function needs); "+ call site" also allows callees the match left
unmatched but whose call sites all name one function (they need that callee's semantics checked: e.g. AI's sin/cos
pair against DM2's). For drifted pairs it lists what differs (code, calls, constants, registers, call sites, struct
offsets, stack frame, globals). The work database's `carry_by_status` view gives the same by status.

## 3. The module map (modmap.py)

Evidence, strongest first: a __FILE__ path string used by the function in AI or by its DM2 or
BMX XXX counterpart (asserts and error reports); the SCE scan's library (SDK region only: small SCE hits inside the engine are chance);
blocks; contiguity between two functions of one module (an object's .text is contiguous).

Blocks: two functions are in one object when they use the same data item (.data, .sdata, .bss,
.sbss, .rodata) or one calls the other with every user within 0x3000 bytes of text (statics and a
module's own globals; farther apart means an extern); adjacent blocks whose local data is out of
link order are one object too. Blocks are computed in each game and carried to AI through the match
(counterparts in one block link their AI interval).

A block is one object or a few neighbouring objects tied by calls: the zglTerrainRender block
(0x12C0F8-0x135860) also holds the GEO chunk loaders and part of collision. The
readable code's own folders agree with the blocks: most blocks that hold readable functions hold
one folder (zge/math, zge/assets, aggressive/script, aggressive/skater ...).

DM2's link-map fragment (the ELF header padding): 33 objects in link order, zobj zscene zcompres
zfile zassetLib zanim zstring ziff zdebug zinput zquat znad zcollide ztoken zlist zprofile zbinHeap
zUtil zEventLoop zik zmovie zdynamic zscnpts zobjModel zobjCamera zobjLight zobjPEmit zTimer zFont
zPath zEntity zRamDisk zmathGenMatrix (the first line is cut: `zmathUtil.o` before zobj). AI's
anchored modules keep that order. Its sizes are another build's (DM2's retail .rodata holds far
less), so only the order is used: a function in a gap between two listed modules gets the objects
between them as candidates.

The Xbox builds' __FILE__ paths are the same modules plus the Xbox
platform files (XBoxGL, XBoxTexture, XBoxMovie, xboxChan ...), DM2's zglCamera.c and
Entities/mover.c, BMX XXX's ztexturefx.c and its game files (action/{action, camera, fx, game,
motview, overlay, physics, weapon}.c, bmx.c). Their .rdata order is not the PS2 link order, so
they only add names. (Section 4: they are function anchors too; the engine's error
reports store __FILE__ with `mov [eax+0x14], imm32`, which xbe_paths.py finds.)

`modmap.py --readable` lists the readable functions by module and block.

## 4. The Xbox build beside the MIPS

AI's Xbox release (`default.xbe`, the PDB path
`C:\inline\src\Xbox\Final_LTCG\InlineXBox.exe`, XDK 4627, MSVC with link-time code generation,
D3D8LTCG) gives a second decompilation of the same source. The tools run it through Ghidra, pair its
functions with AI's PS2 functions, and print a pair side by side for the readable work. Outputs go to
`$ZAXIS_SCRATCH/aixbox` (decompilations, the pairing, the per-function files: disc derived, never
committed). DM2's and BMX XXX's Xbox builds go the same way (below).

Tools:

| tool | what it does |
|---|---|
| `ghidra/xbe_pipeline.sh` | `xbe_pipeline.sh <name> <default.xbe copy> <work dir>`: 1 import with XboxDev's ghidra-xbe loader, full analysis (its XbSymbolDatabase analyzer names XDK functions; Function ID names CRT ones), "Non-Returning Functions - Discovered" off, "Decompiler Parameter ID" on; 2 gap fill; 3 export (`<work>/out/<name>-xbe.jsonl`); 4 decompiler output (`<work>/<name>-decomp.c`). About 12 minutes |
| `ghidra/XbeGapFill.java` | functions reached only through pointers in data (vtables, state and callback tables) and code after the alignment padding; not after code that falls through (that is the rest of a function) |
| `ghidra/ExportXbe.java` | per function: calls in order (tail jumps too), data references with the instruction, immediates, displacements; all defined strings |
| `ghidra/XbeDump.java` | debug: the listing from an address |
| `xmatch.py` | the pairing (below); `--holdout` the precision check |
| `xpair.py` | a PS2 function beside its Xbox counterpart, annotated; `--slices FILE` one file per Planned registration and per address in FILE's "Next slices, in order" section, if it has one |
| `xbe_paths.py` | also finds `mov [r32+d], imm32` uses of a path |

The run (from `$ZAXIS_SCRATCH/aixbox`; XBE_EXT_ZIP is the ghidra-xbe release zip,
github.com/XboxDev/ghidra-xbe `build-202610161023`, `ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip`):

```
python3 -I tools/workdb/xbe_paths.py extract "<Aggressive Inline-xbox.iso>" xbe/ai.xbe
XBE_EXT_ZIP=downloads/ghidra-xbe/ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip \
  tools/workdb/ghidra/xbe_pipeline.sh AiXbox xbe/ai.xbe $PWD/ghidra-aixbox
python3 -I tools/workdb/xmatch.py elf/SLUS_203.27 <ai.csv> <ai sce-matches.csv> xbe/ai.xbe \
  ghidra-aixbox/out/AiXbox-xbe.jsonl xmatch.json --readable <z-axis>
python3 -I tools/workdb/xpair.py xmatch.json <ai-decomp.c> ghidra-aixbox/AiXbox-decomp.c 0x18D678 [--side]
python3 -I tools/workdb/xpair.py xmatch.json <ai-decomp.c> ghidra-aixbox/AiXbox-decomp.c \
  --slices <z-axis>/docs/READABLE.md --out slices
```

(`<ai.csv>`, `<ai sce-matches.csv>`: `ghidra-ai/out/ghidra-functions.csv` and
`ghidra-ai/sce-db/sce-matches.csv`; `<ai-decomp.c>`: this workspace's `ai-decomp.c` (a data link), the PS2
pipeline's decompiler output.)

### Ghidra on the XBE

The XBE's game and engine code is `.text` 0x11000-0xF12AF; XAPI and the CRT follow, and the XDK's libraries
(D3D, D3DX, XGRPH, DSOUND, XPP, WMADEC, BINK*) have sections of their own. XbSymbolDatabase and the kernel imports
name the XDK's functions and Function ID the CRT's (`_strncmp`, `__ftol` ...); the game and engine functions are
unnamed. LTCG shows in the output: register arguments (`in_EAX`, `unaff_ESI`) and small functions inlined
everywhere (strcpy/strcat loops, list walks, vector math). The gap fill matters: without it Ghidra's functions
cover about 80 % of `.text` instead of 95 %. With the discovered-noreturn analyzer on, Ghidra cuts functions after
calls it takes as not returning, which is why the pipeline turns it off.

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

"Inlined" means no standalone counterpart: the Xbox counterparts of the function's paired callers hold its
tokens, or have no call left in its place. The Sony SDK has no Xbox counterpart (libc pairs by name). Most of the
engine's unpaired code is PS2-only: 0x100000-0x138000 (the VIF/VU1 render path, GEO/TXR loaders, ps2Movie,
ps2Chan, the memory card) hardly pairs, 0x13C000-0x160000 (zobj, zfile, zscene, zgui's engine half) pairs about half to three quarters by size.

How far to trust a pair: high is dependable and medium nearly so; low is a lead to read, not a fact (about half of
the low pairs read by hand were right). A wrong medium pair: PS2 0x138370 (a 16-byte aligned bump allocator) is
paired with the Xbox's ziffChunkReadData (0xAA370), whose PS2 counterpart is readChunk 0x149428. A string unique in
both builds can still mislead where inlining moved it: PS2 0x1910B8 holds a bone-name table that the Xbox keeps as
its own small function, and setSkaterScore's "Rollover!" is "$KSTR_ROLLOVER!" on the Xbox, with the old text in
an Xbox function that inlined it. `xmatch.py --holdout` measures the passes: it hides every second pair a unique
string makes and reports how the other passes pair them.

### Reading the Xbox code beside the MIPS

`xpair.py` annotates the Xbox output with the PS2 names of its callees and globals, and both sides with string
texts and float values. Some PS2 functions have no Xbox counterpart to read: the packet rebuilds (FUN_00134AB0 ..
FUN_00134D28), the VIF1 handler FUN_001085B0, the libvifpk/libvu0 functions, and half the script natives, which are
inlined into the script VM's dispatch or dropped.

Ghidra's Xbox output is about as long as the PS2's, with fewer gotos, fewer `undefined` types and fewer calls
(inlining), but many more register-argument names. It helps most where the PS2 side uses VU0 macro code
(`_lqc2`/`_vsub`/`_vmulbc`): the Xbox side is plain float arithmetic and shorter. The follow camera (fn_18D678)
reads as C on the Xbox: `1/SQRT(x*x+y*y+z*z)` normalisations, the collision query with the same arguments, the
same constants (2, 1.5, 20).

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
  0x149428, see above),
  inlineAirTrickCreateJumpAirTrick (0x1B2E28), inlineObjectiveTrickCreateInstance (0x1EB2D0),
  inlineCommandProccessGrind (0x18AF00); Xbox-side XBUtil_FindMediaFile, XBoxFile, XBFont.
- Type and enum names: EzsceneObjType (0x13CEE0's message "ZSCENECLOSE: Object not attached to
  scene hasn't been freed:EzsceneObjType=%d"), kzobjType, module tags ZIFF, ZTOKEN, ZOBJ,
  ZGLMODEL, ZGLTEXTURE, ZGLMAT, ZSCENECLOSE; the property types of zgui's scripts (Bool, Sint32,
  ZFloat, Z3DVector, ZRGBA_PAL, String, ZAssetID, Event).
- Many strings the Xbox code references are not in the PS2 executable: engine
  diagnostics ("***INFO*** World size:x=%dm", "High number of instances (%d > %d) in sector",
  "Didn't find topology id"), the game's ("Double Grind Edge: No match found!", "Memory leak in
  ..."), the vertex and pixel shader sources with their comments, localised UI strings ($KSTR_*).
- Module paths: the __FILE__ paths with the functions that use them. Two place a PS2 function no PS2 path
  placed: zdebug 0x13B990 and XBoxLoadScreen's counterpart 0x110E68.
- The release: XDK 4627 libraries (XAPILIB, D3D8LTCG, D3DX8, XGRAPHC, DSOUND, LIBCMT, LIBCPMT),
  Bink, WMA decoding.

`xmatch.json` keeps them per Xbox function (`names`, with whether the PS2 build has the
identifier).

Nothing is installed system-wide. The pipeline unpacks ghidra-xbe (XboxDev, `build-202610161023`, for Ghidra
12.1.4) into the work dir's own Ghidra settings. Its XbSymbolDatabaseCLI ships for mac_x86_64 only and runs under
Rosetta on Apple silicon (copied to `os/mac_arm_64`).

### DM2's and BMX XXX's Xbox builds

BMX XXX's Xbox build (`c:\bmx3\src\XBox\Bmx3XBox\Final\Bmx3XBox.exe`) goes through `xbe_pipeline.sh` into
`$ZAXIS_SCRATCH/workdb/bmxxbox` (about 3 minutes). DM2's is in `$ZAXIS_SCRATCH/dm2xbox-ghidra`.

Each game's PS2 build is paired with its own Xbox build. `xmatch.py --layers ENGINE:GAME:SDK` gives the other
games' PS2 builds their own layer bounds; with AI's, BMX XXX's game code from 0x2021E0 to its SDK at 0x21DAA8
would fall out of the pool and DM2's unnamed Sony libraries (0x1C2728 up) would fall in. The bounds, from where
AI's engine, game and SDK functions' identical and close counterparts fall: DM2 100100:150000:1C2728, BMX XXX
100100:170000:21DAA8.

BMX XXX's Xbox build pairs as well as AI's or better. DM2's Xbox engine hardly pairs: its Xbox release was built on
a different engine. Its game code does pair.

## 5. The GameCube builds

Each of the three games has a GameCube release: `main.dol`, PowerPC Gekko, Metrowerks CodeWarrior, with no
symbols and no `__FILE__` paths. The engine's and game's messages the PS2 builds dropped are there
(zguiScript_Load's, the "Memory leak in ..." reports), and the UI code spells out its `$KSTR_*` keys. Extract the
DOL from a disc (read only) with `tools/modding/gc_disc.py extract <rvz> --out <dir> sys/main.dol --sys`.
Outputs go to `$ZAXIS_SCRATCH/workdb/gc` (disc derived, never committed).

Tools:

| tool | what it does |
|---|---|
| `ghidra/gc_pipeline.sh` | `gc_pipeline.sh <name> <main.dol copy> <work dir>`: 1 import with Cuyler36's Ghidra-GameCube-Loader (DOL loader, the Gekko/Broadway language; its "(GameCube/Wii) Program Analyzer" turned on: it finds the r2/r13 small-data bases and resolves the references off them), full analysis, "Non-Returning Functions - Discovered" off; 2 export (`ExportXbe.java`, r1/r2/r13 left out of the struct offsets); 3 decompiler output. About 1.5 minutes each |
| `gcmatch.py` | the pairing: xmatch.py's tokens, passes and confidence rules with a GameCube side (Ghidra's functions, calls and references; floats from lfs/lfd/psq_l references; integers, struct offsets and lis+ori/addi constants decoded from the DOL's words as the MIPS side does it); `--layers`, `--holdout` as in xmatch.py |

The run (from `$ZAXIS_SCRATCH/workdb/gc`; GC_EXT_ZIP is the loader's release zip,
github.com/Cuyler36/Ghidra-GameCube-Loader release 1.3.1,
`GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip`):

```
GC_EXT_ZIP=downloads/gcloader/GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip \
  tools/workdb/ghidra/gc_pipeline.sh AiGc dol/ai.dol $PWD/ghidra-ai          # Dm2Gc dm2.dol, BmxGc bmx.dol
python3 -I tools/workdb/gcmatch.py <ai.elf> <ai.csv> <ai sce-matches.csv> dol/ai.dol \
  ghidra-ai/out/AiGc-gc.jsonl ai-gcmatch.json
python3 -I tools/workdb/gcmatch.py <dm2.elf> ... dol/dm2.dol ghidra-dm2/out/Dm2Gc-gc.jsonl dm2-gcmatch.json \
  --layers 100100:150000:1C2728
python3 -I tools/workdb/gcmatch.py <bmx.elf> ... dol/bmx.dol ghidra-bmx/out/BmxGc-gc.jsonl bmx-gcmatch.json \
  --layers 100100:170000:21DAA8
```

The loader's extension.properties says 12.1; the pipeline sets it to the running Ghidra's version and compiles
its SLEIGH language with Ghidra's own `support/sleigh` inside the work dir.

The GameCube builds pair much more of the code than the Xbox builds, in all three games: CodeWarrior inlines far
less than MSVC's LTCG, the source is the same, and only the platform layer (the PS2's VIF/VU1 render path,
ps2Movie, ps2Chan, the memory card) has no counterpart. The Sony SDK has none either. High and medium pairs are as
dependable here as on the Xbox. gcmatch.py expects the PowerPC/MIPS instruction-count log ratio at mean 0.18, sd
0.35.

What it gives the readable work: a third decompilation of the same source, closer to the PS2's
than the Xbox's (fewer inlined callees, the same argument passing in registers, floats as single
precision), the GameCube-only messages (zguiScript_Load's are in all three GameCube builds),
`$KSTR_*` names by function, and struct layouts per build (the structure zguiScript_Load, 0x17EB20, reads: AI PS2 +0x28 ..
+0x13C as in DM2's PS2 build, AI GC +0x48 .. +0x15C as in BMX XXX's builds, so AI's GameCube
release looks like a later version of that code than its PS2 release).

Nothing is installed system-wide. Downloaded into `$ZAXIS_SCRATCH/workdb/gc/downloads/gcloader`:
`GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip` (sha256
9892f28fc1e7f19bb3fcec7cfb4ad2ab7504503b571d93de8b142a6f7bec8ed0), unpacked by the pipeline into
each work dir's own Ghidra settings.

## 6. The work database (tools/workdb/workdb.py)

One DuckDB file, `$ZAXIS_SCRATCH/workdb/work.duckdb`, with everything above keyed on AI's PS2 functions, for the
readable rewrite's sweep (docs/READABLE.md "Drafts and the sweep") and the readable passes. It is a cache:
`workdb.py build` makes it from the sources (about 5 s) and nothing edits it. Everything else opens it read-only,
and a build writes a new file and renames it over the old one. It runs with `$ZAXIS_SCRATCH/venv/bin/python`
(duckdb 2.0.0; any other Python re-runs the script with it). The database is disc derived and stays in scratch.

```
python3 -I tools/workdb/workdb.py build                       # rebuild (sources: `tables` lists them)
python3 -I tools/workdb/workdb.py tables                      # tables and views, row counts, when built
python3 -I tools/workdb/workdb.py query "SELECT * FROM carry_by_layer"   [--csv|--json]
python3 -I tools/workdb/workdb.py card 0x18D678 [NAME ...] [--build B] [--json] [--prefix '// '] [--width 100]
python3 -I tools/workdb/workdb.py notes [TOPIC|ADDR|NAME] [--build B] [--json]
python3 -I tools/workdb/workdb.py emit FILE KIND key=value ...   # one sweep result line
python3 -I tools/workdb/workdb.py check-results [FILE ...]
duckdb -readonly $ZAXIS_SCRATCH/workdb/work.duckdb   # or the CLI
```

`query` turns `0x1A2B` outside quotes into its value (DuckDB has no hex literals); in SQL,
`hexa(addr)` prints an address and `addr_of('0x18D678')` reads one.

Sources (each overridable, see the docstring): the three PS2 ELF copies with their Ghidra
function CSVs and SCE scans; `match3.json` and `modmap.json`; the three XBE exports with their
xmatch pairings and `__FILE__` uses; the three DOL exports with their gcmatch pairings; the
readable sources (zlayout.py READABLE's folders: registrations, `ee::Global` and `k...` address
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

### The sweep's results (layer 2)

Nothing but `build` writes the database. A sweep job, or anyone with a finding, appends JSON lines to its own
file under `$ZAXIS_SCRATCH/workdb/results/` (any depth, `*.jsonl`). The next build ingests them; rejected lines
go to `ingest_errors` with the reason. A line:

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

`map` and `pair` lines are keyed by their own `build` and `addr` (any build, not only AI's PS2): a function
mapped by hand for a port (the DM2 rider code, `results/riderplan/rider-map.jsonl` by `tools/workdb/ridermap.py`), and
pairs the match does not make (DM2 -> BMX XXX PS2: `dm2bmx.py`'s, below, which replace
`results/riderplan/dm2-bmx-pairs.jsonl`). The views `func_map_latest` and `func_pairs_latest` give the newest
line per function.

A `check` line names what the pass was made against (zx-fncheck's result line,
docs/READABLE.md "How a function is proven"): `impl` the readable function's source digest
(`tools/readable/readable_digest.py`), `orig` the generated original's, `rec` the recording's
recorder version and corpus folder (`6:v5/lc`), `checker` the zx-fncheck executable's. A
`coverage` line (`tools/checks/fncheck_corpus.py emit`) gives a function's path coverage over a
corpus: the original's basic blocks, how many the replayed calls ran, the uncovered blocks'
addresses. The build also runs readable_digest.py on the readable sources (source
`readable_digest`, next to `game_src`) into `readable_digests`: each registration's current digest.
Views: `coverage_latest` (the newest coverage line per function with its share),
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
The notes' text, keyed by function name (no addresses of its own, the sources left out), is exported to
`tools/workdb/workdb_notes.jsonl` (`workdb.py export-notes`); `card` and `notes` read that export when there is no
database or no duckdb module, or with `WORKDB_PUBLIC=1` (`tools/workdb/workdb_notes.py`).

`workdb.py emit FILE KIND key=value ...` writes one checked line (values parsed as JSON when they
parse, `ts` filled in); from Python, `workdb.append_result(path, record)` (a locked append of one
whole line). `sweep_latest` gives per function the newest draft, build, checks per route (pass /
fail / other), failure and status; `sweep_failure_classes` counts functions per newest failure
class (the sweep's loop: fix the generator for the biggest class, redraft).

### DM2 and BMX XXX

`tools/workdb/dm2bmx.py OUT.json <workdb>/results/d1data --run d1data` makes the DM2 -> BMX XXX PS2 match:
fmatch3.match_pair on the two ELFs (as `ridermap.py pair` does), then a second run seeded with those pairs and the
triangle's (a DM2 and a BMX XXX function the first left unpaired whose AI counterparts are one AI function,
identical or close on both sides). Each line keeps fmatch3's class (identical, close, drifted, weak), method,
ratio, opcode ratio, callee consistency and the triangle's verdict (agree, disagree, none). `build` copies the
newest run's pairs into `matches` (a_build dm2_ps2, b_build bmx_ps2, source `results:<run>`, triangle true/false)
after AI's counterparts, so no AI chain goes through them. The engine ends at DM2 0x163280 and BMX XXX 0x190340.
Many BMX XXX game functions whose masked body equals a DM2 function's stay unpaired: they are small bodies (under 6
instructions, or in equal-hash groups) the match leaves alone.

DM2's library bound is 0x1CC800 (sceGsResetGraph), not 0x1C2728: the SCE scan's `_InitSys` there is a game
function (SCE_FALSE in workdb.py), and the functions between are DM2's level object types (EditObj_GapMarker ..
SpecialFX/zfxMgr.c). DM2's Xbox and GameCube pairings use the 0x1C2728 bound (`--layers` above).

### The engine's lineage

View `engine_lineage`: one row per engine function across ZGE2 (DM2 PS2, Aug 2001), ZGE3 (AI PS2,
Apr 2002) and ZGE4 (BMX XXX PS2, its build string Oct 16 2002), from the three games' direct links
(a version a direct link misses is taken through the third game's): the three addresses, the name,
each step's class (identical when the masked bodies are equal, else the direct pair's class), a
lineage, the instruction counts and AI's readable file. Engine = below each ELF's engine end (DM2
0x163280, AI 0x185D20, BMX XXX 0x190340) and not sdk, or an AI function in zge/. The card prints it.
A version "not in an ELF" is not linked there (BMX XXX's link drops unreferenced functions), which
does not say its source lost it.

The lineage values: the same in all three; changed in ZGE3, kept in ZGE4; changed in ZGE3 and in ZGE4; kept in
ZGE3, changed in ZGE4; changed in ZGE3, ZGE4 has the ZGE2 body; new in ZGE3, kept in ZGE4 (or changed in ZGE4);
kept in ZGE3 (or changed in ZGE3), not in ZGE4's ELF; not in ZGE3's ELF, kept in ZGE4 (or changed in ZGE4); ZGE2
only, ZGE3 only, ZGE4 only.

Dating (`tools/workdb/lineage_gc.py`): AI's and BMX XXX's GameCube builds share a compiler (most of their pairs
identical on PS2 are the same masked PowerPC code on GameCube), DM2's and AI's do not. So only an equal GameCube
pair is evidence. A ZGE3 -> ZGE4 change where AI's GameCube code equals BMX XXX's dates ZGE4's form to the engine
of AI's GameCube release (built after its Dolphin OS kernel of June 5 2002): after AI's PS2 build, or in PS2-only
code. ZGE2 -> ZGE3 changes are dated the same way. The dates are `history` notes on AI's functions
(`results/d1data/lineage-notes.jsonl`). The Xbox builds are not used: DM2's Xbox engine does not pair (section 4)
and MSVC's inlining hides the bodies. Build dates in the executables: DM2 Xbox XBE Nov 9 2001; DM2 GameCube Dolphin
OS Sep 8 2001; AI GameCube Dolphin OS Jun 5 2002; AI Xbox XBE Jul 2 2002; BMX XXX PS2 Oct 16 2002, Xbox Oct 22
2002, GameCube Nov 3 2002.

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
inside a function finds the function; a name is looked up in every build's candidates. A card takes
about 25 ms.

`tools/readable/readable_draft.py` puts a function's card in each draft's header, after its "Draft by
readable_draft.py" line and before Ghidra's decompilation comment (`--workdb PATH`, default
`tools/workdb/workdb.py` or WORKDB_PY; `--no-workdb`; with the database missing, or an address that is not an
AI function, it drafts without the card). The card names the database's build time; it is a comment, so
zx-fncheck results do not depend on it.

Weigh some name candidates with care: a `tuning` candidate read through the category pointer can be wrong (a
displacement in a function that also reads the category could be another struct's), and a `string_key` candidate
rests only on its hash matching a named key.

## 7. The readable write-back

This makes the decompilations read with the readable rewrite's names, types and struct fields. What the
readable code knows is written into a copy of each analysed Ghidra project and read back, and the exports are made
again from the copy. Everything it writes holds the games' addresses, so it stays in scratch
(`$ZAXIS_SCRATCH/ghidranames`).

| tool | what it does |
|---|---|
| `readable_ghidra.py` | the input. `scan`: clang over the readable translation units (every readable source in a `compile_commands.json` under the checkout's `out/`, with its flags; cached by a digest of the sources): `-fdump-record-layouts-complete` for every struct's offsets and size, and the JSON AST of the namespaces aggressive, zge and sce for the registrations (address, the registered function's C++ signature and parameter names), the ee::Global variables, enums and typedefs. `gen BUILD DIR`: `readable-BUILD.h` (the guest structs, packed with every gap spelled out so the offsets are clang's; the prototypes; the globals' types) and `readable-BUILD.jsonl` (a meta line with the header's SHA-256, the readable commit and the work database's build time; then the expected layouts, the names, the signatures with their EABI registers, the globals). `all DIR`: the nine builds. `stats BEFORE.c AFTER.c`: counts over two decompilations |
| `games/aggressive/workspace/ghidra/scripts/ApplyReadable.java` | the stage. The header goes through Ghidra's C parser into the category `/readable`. Function names: the analysis name is kept in the `ReadableOriginalName` property map; a name a loader, Function ID or the SCE scan gave (the libraries' own) stays, and the readable one becomes a label beside it. Signatures: ApplyFunctionSignatureCmd with the compiler spec's default convention. Globals: typed data, larger first (a global inside a struct global stays a label). Each function gets a plate comment saying where its name came from. Then everything is read back: struct sizes and each field's offset, name and size; names; parameter types, names and registers against the EABI (a0-a3, t0-t3, f12-f19); return types. Any mismatch fails the stage. The input's and the header's SHA-256 go into Program Information. `verify` runs the read-back alone (read only) |
| `ghidra/readable_pass.sh` | `readable_pass.sh <ps2\|xbe\|gc> <name> <work dir> <program> <readable-BUILD.jsonl>`: copies `<work>/project` to `<work>/project-readable` and applies there. The base project stays as analysed: its CSV names the recompiled functions and its jsonl feeds the pairings. Then, read only: verify, the export again (xbe/gc) and the decompilation, into `<work>/readable/` |
| `ExportXbe.java`, `ExportDecomp.java` | on a readable copy, ExportXbe writes the analysis name as `"n"` (so xmatch, gcmatch and workdb read what they always read) and the readable one as `"rn"`. ExportDecomp starts with a line that names the input and its SHA-256 |
| `xpair.py` | a call to a function that the readable stage named on the Xbox side gets its PS2 counterparts, as a FUN_ call does |

The pipelines run it as their last stage when `READABLE_BUILD` names the build:
`pipeline.sh` stage 10, `xbe_pipeline.sh` stage 5, `gc_pipeline.sh` stage 4. AI's own
pipeline (`games/aggressive/workspace/ghidra/scripts/pipeline.sh`) runs it as stage 9.

What each build takes:
- AI PS2 gets everything: the registrations, the readable constants that name a function
  (`kSetEye` read as `setEye`), the Planned names, the signatures, the structs and the globals.
- DM2 and BMX XXX PS2 get the same through identical matches only (the same machine code, so the
  same signature and offsets).
- AI Xbox and GameCube get names only, through high and medium counterparts. LTCG and PowerPC
  pass arguments their own ways, and struct layouts can differ by platform.

When two AI names fold into one counterpart, the generator applies neither. A name used twice gets the address
(`allocate_00117AC0`).

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

To run it:

```
python3 -I tools/workdb/readable_ghidra.py all $ZAXIS_SCRATCH/ghidranames/gen     # or gen BUILD DIR
AI_SCRIPTS=<folder with ApplyReadable.java> tools/workdb/ghidra/readable_pass.sh ps2 AggressiveInlineCopy \
  <work dir> SLUS_203.27 $ZAXIS_SCRATCH/ghidranames/gen/readable-ai_ps2.jsonl
# in a pipeline, from its last stage: READABLE_BUILD=bmx_ps2 tools/workdb/ghidra/pipeline.sh BmxXXX elf/SLUS_204.15 ghidra-bmxxxx 10
python3 -I tools/workdb/readable_ghidra.py stats <base decomp.c> <work dir>/readable/<name>-decomp.c
```

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
  The check finds the Xbox Player's shifts (+0x74 at +0x84, +0x550 at +0x5C0, +0x214 at +0x224) and the
  GameCube Player's (+0x74 at +0x68), so Player is skipped on both. Some structs differ in a game's own PS2 build
  as much as on its Xbox and GameCube builds (BMX XXX's ChunkReader, +0x50 at +0x54, its TextWidget and zge_Node;
  DM2's CollisionWorld): those are the games' own versions, not the platforms'. (The PS2 builds take structs
  through identical functions only, whose offsets are AI's.)
- **Signatures**: high counterparts only, and only where Ghidra's own analysis of the counterpart
  (its decompilation's prototype) takes the same arguments in the same places. GameCube: the
  PowerPC EABI (integers and pointers r3-r10, floating point f1-f8; floats as `double`, which is
  what the FPRs hold), the integer and float counts equal to Ghidra's. Xbox: the convention
  Ghidra found is kept (`__cdecl`, `__stdcall`: all on the stack; `__fastcall`: the first two
  integers in ECX, EDX); `__thiscall` and LTCG's own register conventions are skipped. Wide
  arguments (8-byte integers, x86 doubles) are skipped, and a float return against an integer
  one. ApplyReadable.java takes the record's `cc` and reads it back with each parameter's
  storage.

`tools/workdb/exports.py` says where each build's exports are: the base ones (as analysed) and the
readable ones in one folder, `$ZAXIS_SCRATCH/readable-exports/<build>/` (`decomp.c`,
`export.jsonl` for Xbox and GameCube, `source.json` with where they came from and their
SHA-256; `exports.py install BUILD WORKDIR` after a readable pass). The tools take the readable
files by default:

| tool | default | the originals |
|---|---|---|
| `workdb.py build` | the six readable Xbox/GameCube exports (they keep the analysis name as `"n"`, so the tables are the same as from the base exports; `meta.exports` says which) | `--base-exports` |
| `xpair.py` | `xpair.py ai_xbox\|dm2_xbox\|bmx_xbox ADDR ...`: the pairing and both readable decompilations; any decompilation argument may be a build name | `--base` |
| `dmatch.py` | build names as arguments; a call by a readable name counts as the call to its address | `--base` |
| `show.py` | build names; `--callers` finds a named function's callers by its name | `--base` |
| `datzip.py` | the base files (it pairs `DAT_` addresses; the readable AI PS2 file names its globals) | `--readable` for the readable ones |
| `ridermap.py`, `readable_ghidra.py gen` | read the work database only (ridermap), or AI PS2's readable decompilation for the layout check | - |

READABLE_EXPORTS=base in the environment turns every default back. A build with no readable
file falls back to its base file with a warning. `readable_draft.py --decomp` defaults to
`games/dm2/workspace/ai-decomp.c`; the readable one is `python3 -I tools/workdb/exports.py path ai_ps2 decomp`.

Structs the layout check finds differing: AI Xbox Player, Font, ZgePart; AI GameCube Player, FollowCamera,
SphereQuery, zge_Node; DM2 GameCube CollisionWorld; BMX XXX Xbox ChunkReader, TextWidget, ZgePart; BMX XXX GameCube
ChunkReader, SphereQuery, TextWidget, zge_Node. DM2's Xbox build takes almost nothing (its engine does not pair).

Examples:
- AI GameCube 0x8005392C, `undefined4 FUN_8005392c(undefined4 param_1,int param_2)` becomes
  `int loadMaterialTableChunk(void *reader,GeoLoadContext *ctx)`, with
  `ctx->materials = pvVar2` for `*(int *)(param_2 + 0x1c) = iVar2` and `attachMaterialTable`,
  `chunkDataSize`, `enterChunk`, `parseChunks` for four FUN_ calls.
- AI GameCube 0x8007C210, `void FUN_8007c210(double param_1,int param_2)` becomes
  `void updateFollowCamera(void *camera,double seconds)`: FollowCamera's layout differs there
  (+0x72 at +0x5E), so the camera stays untyped, and the arguments are in the source's order.

To run it:

```
python3 -I tools/workdb/readable_ghidra.py all <gen dir>
python3 -I tools/workdb/readable_ghidra.py layouts ai_gc          # the per-struct evidence
AI_SCRIPTS=<folder with ApplyReadable.java> tools/workdb/ghidra/readable_pass.sh gc AiGc <work dir> ai.dol <gen dir>/readable-ai_gc.jsonl
python3 -I tools/workdb/exports.py install ai_gc <work dir>
```

## What the match does not tell you

- It is static. "Identical" means the masked machine code is equal, not that a readable function was checked in
  that game: that is each game's own recording corpus and zx-fncheck (docs/READABLE.md "How a function is
  proven").
- Most engine blocks have no module name: only blocks with an anchor (a path, an SCE library, the DM2 link-map
  order) are named.
- The write-back does not name DM2's and BMX XXX's own globals, and gives no per-build layout for a struct that
  differs by build (the Player on Xbox and GameCube: the layout check gives only the shifts of the fields a
  counterpart uses). It writes no signatures for Xbox `__thiscall` and LTCG register conventions or for medium
  counterparts, and leaves out the structs with too little evidence (most are used by no paired function).
