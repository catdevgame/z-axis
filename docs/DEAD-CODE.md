# Dead code: what it is and what it is for later

The functions of the games that no game path reaches: what they are, and which of them are cut or unfinished
features worth bringing back (the "Game content" lists below). None of it is gibberish: every one is the game's own
machine code. A dead flag means no reference was found, not that the function is proven unreachable: the scans
miss computed calls, and a route that reaches a flagged function is always right over the flag.

The flags live in the work database for all three games: one note a function (topic `cut`, "Flagged KIND in the
sweep manifest (LAYER layer, STATUS)", with the row's reason), in AI's from the flags the sweep manifests last held (the
manifests are no longer in the tree: every function is in its named file, or a library's in `not_ported.tsv`) and the
verification lanes' scans since, in DM2's and BMX XXX's from those scans.
- `tools/readable/dead_code.py` makes each game's table of flagged functions from them (AI's `dead-code.tsv`,
  `dead-code-dm2-flags.tsv`, `dead-code-bmx-flags.tsv`); `--table` prints the groups' table below, a row per game and
  group.
- DM2's whole-ELF table: `games/dm2/tools/dead_code_dm2.py` makes it from the ELF and the work database (every
  function nothing reaches, flagged or not; below).

Both write to `$ZAXIS_SCRATCH/dead-code/`: the tables hold strings from the games' ELFs, so they stay out of the
repository.

## Aggressive Inline

### The flags

| Flag | Meaning | How it is decided |
|---|---|---|
| `dead` | no caller and no pointer to it in AI's PS2 ELF, or called only from such code | a scan of the ELF |
| `port-dead` | live on PS2, but reached only through code the PC runtime replaces for good (printf, libmpeg and the IPU, the kernel, SIF, CD, GS setup) | the same scan |
| `script-dead` | its only way in is a script command or menu item that no shipped AI script uses | scans of AI's scripts |
| `data-dead` | nothing in AI's PS2 data reaches it, but another game's or release's data does: it stays a target | scans of DM2's, BMX XXX's and AI's Xbox and GameCube data |

### The three groups, per game (1,519 functions, 211 KB)

| Game | Group | Layer | Functions | Bytes | Also in another game |
|---|---|---|---|---|---|
| Aggressive Inline | Sony SDK and libraries | sce | 443 (250 dead, 193 port-dead) | 74 KB | 414 |
| Aggressive Inline | Engine (NgEngine / ZGE) | zge | 628 (577 dead, 1 port-dead, 44 data-dead, 6 script-dead) | 88 KB | 480 |
| Aggressive Inline | Game content | aggressive | 264 (209 dead, 32 data-dead, 23 script-dead) | 34 KB | 77 |
| Dave Mirra BMX 2 | Engine (NgEngine / ZGE) | zge | 179 (178 dead, 1 port-dead) | 15 KB | 179 |
| BMX XXX | Engine (NgEngine / ZGE) | zge | 5 (4 dead, 1 port-dead) | 0 KB | 5 |

DM2's and BMX XXX's rows are the flags of the engine functions whose readable body stays Draft there (verzge1);
DM2's whole-ELF table below counts every function nothing reaches.

A scan of the nine releases' data flagged 63 more rows (the reason column names the game and file). The last
column of the table above (also in DM2 or BMX XXX) does not count them.

- `script-dead`, 7: the zgui script commands STRCMP, LOADZSC, USE_STR, ROTQUATX, ROTQUATZ and
  CANCELLOADLIB (the command table 0x245448..0x2457D8: name and handler) and the menu command
  inlinePauseRestartFromLoad: no UI script of any release uses them.
- `data-dead`, 36: the commands STREXTRACT, STRTOKEN (BMX XXX), ROTQUATY (AI's leftover
  MENU/MAIN.ZSC, DM2), REPORTLIB, LOADMODEL, LOADIMAGE (DM2), FORMAT (DM2, BMX XXX) and the five
  functions only they call; the chunk loaders NAME and ZFFD (BMX XXX's rider creator archives
  CREATE/*.ZAL), WSMC and WSMO (BMX XXX's level GEOs, the same eight levels as the emitters),
  ZLIT and LITO (DM2's CLOVERLE.DAT), UNAD (AI's leftover MENU/ANIMS.ZIF, DM2's UILIB.ZAL), TTGA
  (xboxButtons.Zif and gcButtons.Zif inside AI's own UILIB.ZAL, which the PS2 build never loads:
  it loads ps2Buttons.Zif); the TrafficLight object type's 14 (DM2's COMMERCI level has
  TrafficLight objects on all three releases; AI's levels only Prop_TrafficLight props).
- `dead`, 20, decided by data: the DCON constraint types 1, 3 and 4 (every DCON of every release is
  type 0), the chunk loaders CLEN, DBNC, SDRD, LITH, PVSH, ZPDF, ZPPT, ZPLM, ZPCT, ZPVL, ZPDN (and
  two functions only ZPDN's calls) and SDTD (no file of any release has the chunk), and CameraAnim's 3
  (no level of any release has a CameraAnim object).

Not flagged, but no shipped data reaches them either: updateDynamics' per-body force list
(+0x388: FUN_001568D0, FUN_001565A0) is filled only by FUN_0015BB30 and a body's parent slerp flag
(+0x398: FUN_0015A400) set only by FUN_0015B528, both `dead`; the decompressor (FUN_0013EA50 and its
parts) runs for a ZDIR with CLEN (none on any release) or from FUN_001DEF30, which no route reaches.

Not flagged, with no way in from the game's own code (test routes reach some of them):
- airTrickOnButtonReleased (0x1EE290): the air trick class registers a ButtonReleased handler, but
  ButtonReleased is sent only to a pole object (0x18C11C).
- zfile's create (FUN_0013FAF0) calls device slot +0x50, past the 20 methods of every device table
  (dvd0, pcfs, mc00, rd00): a null call. The device API wrappers 0x13F528..0x13F998 are
  called only by the watcher task and the dead ZAxis saves (FUN_001D8550 / FUN_001D8780 / FUN_001D8290,
  mc00:\ZAxis\globals.dat, career.dat, levels.dat), so the mc00 device's 19 methods and their helpers
  have no live caller (`cov-devapi` drives the dvd0, pcfs and rd00 ones by guest call).
- The engine's pad recorder start FUN_0014C768 (record or play a pad file) has no caller; `cov-padrec` starts it
  by guest call.
- Text trick objects (kind 5) are made only by tellGoalsOver for the bonus goals, whose list only
  setBonusObjectiveState fills (called only in TUT/MOVIE.BIN, LOCAL/LOCALT.BIN and MP/LOCAL, which no
  menu loads), and by the native addBonusTrickToTrickQueue, which no script calls.
- AI's TrafficLight object type (registerTrafficLightType 0x1E61A0) is registered by no shipped code.
  Registered by guest call in DM2's COMMERCI level converted to AI's formats, the two lights were made and
  updated, 10 of the 18 functions. Their path tags found no path record in the converted level, so
  setSegmentTrafficLight and the segment light states stayed unreached. `cov-gh23-z1` reaches them on a
  Movie Lot car's scene object made a light by guest calls, on made-up data.

Reached only by test routes (guest calls: docs/DEVTOOLS.md; nothing in the game gets there; the `cov-*` folders of
the corpus):
- The mc00 device (the card as a zfile device): its 19 methods, the Ps2MemCard completions and the poll task, driven
  through the device wrappers and openFile/fileRead/fileWrite, and directly with a completion block (`cov-mc00`); needs
  `ZX_MC_FD_BASE0=1` (this runtime numbers libmc's descriptors from 1, FUN_00121138 takes only an open that returns 0).
  Left of the card code: formatMemoryCard, findMemoryCard, endSaveJob, memoryCardTestWrite, appendRequiredSpaceText,
  appendClientRequiredSpace, memoryCardFormatted (the NEEDFORMAT dialog).
- zgui's type setters (uint8SetFloat .. vectorSetColor, zfloatSetInt): in the type tables' setFloat/setInt/setVector slots,
  which only operateFloats calls, for a ZFloat destination (`cov-zguiset`).
- zcompres (decompressFileData, lzDecompress, decodeFileBlock and their copies) on token streams made for it
  (`cov-zcompres`; no release ships a compressed library entry).
- Engine API with no caller: asset library selectors, zfile's small API, the chunk reader's hooks and trace, the
  scene objects' attachments (the emitter objects' only user), the scene database's entry walk, animation record makers
  and frees, the animator copies and event queries, the token reader's calls, the timers' tick and running flag, model
  hooks, the profiler's bars, camera accessors, parts and streams, libc stdio over a descriptor, libm's rarer functions,
  the script machine's multiply, divide, or, and and shift opcodes (none is in a disc script).
- Trick handlers: the input recorder's save and load of a skitch, pole, vault and wall trick (`cov-replay4`), the manual's
  TrickStartHolding and StoppedHolding (no manual animation has the event), ButtonReleased to an air trick, the pole's
  ButtonReleased (a press of the grab button on the pole: `games/aggressive/tools/routes.py polebutton`).

Still unreached, with no caller and no data word (45 by the call graph and every data word of the ELF): the
emitter objects' effect functions (no asset has an emitter; deleting an emitter object crashes: a null call in
destroyParticleEmitterObject), the particle system group, the scene database's animation library functions
(loadLibraryModel, loadAnimationLibrary ...), zobjModel's library loaders, the animation stream reader, the CLEN and NAME chunk
handlers, FUN_0013FAF0 (create), and the libkernl/libc stubs the runtime replaces.

What reaching the `data-dead` rows would take in AI (nothing ported):
- BMX XXX's particle emitters, WSMC/WSMO: one of its level GEOs (CHL_DAM and the others) through
  AI's level loader, converted at build time.
- NAME, ZFFD: BMX XXX's rider creator archives (CREATE/*.ZAL) loaded: content of a rider creator.
- ZLIT/LITO: a level with those chunks (DM2's CLOVERLE converted to AI's formats and loaded with the developer start
  reached readZlitChunk and readLitoChunk).
- TrafficLight: a level with TrafficLight objects and the type registered (see above).
- The zgui commands, guiIdleFunc, the Animate action: a menu that uses them (DM2's menus, or a UI
  script patch such as the settings menus' in-memory ones; AI's own leftovers MENU/MAIN.ZSC and
  MENU/ANIMS.ZIF are on its disc and loadable by the LOADLIB and LOADZSC commands).
- TTGA: loading xboxButtons.Zif (or gcButtons.Zif) instead of ps2Buttons.Zif: Xbox or GameCube
  button prompts, which the PC game with an Xbox pad would want anyway.
- The Gap List: a pause menu item that opens AI's own GapList panel (a UI script patch).

### Sony SDK and libraries: nothing to do

Library functions linked into the ELF that AI never calls, and the parts the runtime replaces.
By library: libmpeg 125, libkernl 111, libdev 41 (VIF and VU debug calls), libvu0 29 (matrix and
vector helpers), libc 28, libgcc 12, libcdvd 8, and a few from libmc, libsdr, libipu, libpad and
others; 70 with no library match. Libraries are never rewritten: they are ported from
their pinned versions' source, so none of this needs readable work, and DM2 and BMX XXX link the
same SDK.

### Engine: material for a ZGE superset

The engine is a shared library: 411 of its 544 dead functions also exist in DM2 or BMX XXX, which
link the same NgEngine. They are the parts of the engine AI's build does not use, and the first
place to look when the engine has to run DM2's and BMX XXX's content as well as AI's.

- Known modules: zglTerrainRender 42, ps2Movie 23, ps2Chan (sound channels) 23, zprofile (the
  profiler, "EngCPU", "App") 19, zobjParticleEmitter 16, zdebug (printing vectors and matrices) 8,
  zobjModel 7, zfile 6, zTimer 5, Ps2MemCard 4, and single functions of zscnpts, zobj (pool
  statistics), zgui, zfxmgr, action and XBoxGL.
- `data-dead` here: the 19 particle-emitter loaders (0x15E8F8 to 0x16B490) that BMX XXX's levels
  use on all three releases (emitters in the GEO files of CHL_DAM, CHL_MNCO, CHL_MULY, CHL_NASA,
  CHL_SHEP, CHL_SYRA, CMP_RMPG, CMP_ROOT, and TST_VERT on GameCube).
- Also present: a BMP and TGA image loader, particle types (billboard, spark, poly), parser
  functions BMX XXX's Xbox build names zparseParse.
- 384 engine functions have no module (no module map entry, no counterpart source file and no
  string). DM2's ELF keeps the `.rodata` lines of its link map, which give the engine's object
  files in link order: that is the way to place them.
- 118 are in AI's PS2 build only and 11 in AI's Xbox and GameCube builds only: engine code no
  other game kept.

### Game content: cut and unfinished features to play with later

AI's own code, 149 of the 210 in no other game or release. By module where known: the editors'
tool set (editToolSet) 27, zgui types 14, inlineFx 7.

- A ragdoll crash: every player loads `edwards.zad`, a ragdoll of 10 bodies and 9 constraints and
  the only dynamics file with constraints on any AI disc. Its stepper (FUN_001A4968) has no caller
  and nothing attaches it to the skater. Route `cov-ragdoll` attaches it by hand (test only).
- The lens flare: the game sets a sun position for three levels, but on PS2 never starts the
  flare. Route `cov-fxclasses` (test only).
- The Gap List: a pause-menu panel in AI's UI data (UILIB.ZAL, all three releases) that no pause
  item opens (`data-dead`).
- The input replay: a recorder and player of the players' input, which saves and restores their
  game objects. Reached only by a test call (routes `cov-replay`, `cov-replay2`); starting a
  recording hangs Boardwalk's free skate, and stopping within 50 vsyncs of a start hangs the game.
- Named from the strings they use (readable_names.tsv): the memory-card save of
  `mc00:\ZAxis\globals.dat` (openGlobalsSave, createGlobalsFile, readGlobalsDirectory,
  writeGlobalsEntry, closeGlobalsSave: 0x1D7F88 to 0x1D8780) and `TESTGAM.SAV`
  (testSaveFileName), the `FrontEnd.ShowAll` command (showAllFrontEnd, with hideAllFrontEnd), the
  `Tang.TopLeftColor` UI functions (setTangTopLeftColor, tangTopLeftColor,
  setPotentialTangTopLeftColor, potentialTangTopLeftColor), the two "Score" functions
  (playerPanelScore, pointsCounterScore) and the Park Editor's `OnPlayPause` handler
  (playPlayPauseScript).
- A judged competition: 7 of 13 rivals picked at random, and each judge scoring the player's run against the
  round's target (pickCompetitionRivals, judgePlayerRun, scoreCompetitionRound: 0x1AE530 to 0x1AECC0). Nothing in
  AI reaches it.
- `script-dead`: the challenge objectives and traffic parts no level script uses (setVehicleVolume
  0x1E5A50 among them) and the inlinesound UI commands.
- `data-dead`: the Animate action's methods, used by AI's main menu script on all three releases
  and by DM2's menus, and guiIdleFunc, which DM2's menus use (`OBJECT "guiIdleFunc" "DemoMode"`).

The 44 also in DM2 or BMX XXX are game-side code the three games share.

## Dave Mirra BMX 2 (874 functions, 107 KB)

DM2's table is made from the ELF and the work database: `games/dm2/tools/dead_code_dm2.py` writes
`$ZAXIS_SCRATCH/dead-code/dm2-dead-code.tsv` (strings from the ELF: it stays out of the repo). A row is a function
with:
- no call or jump from another function;
- no code reference to its address (a lui/addiu pair or a loaded word);
- no data word equal to its address (tables, vtables, state tables);
- not the ELF's entry;
- no recording in DM2's corpus (by its file index).

Kinds: `dead` (no reference at all: 702) and `dead-callers` (referenced only by other rows, to a fixed point: 172).
The evidence is in its columns: callers (all/live), code references, data words, the AI counterpart with its class
and AI's flag, the BMX XXX class (the DM2 to BMX XXX pairs), DM2 Xbox and GameCube counterparts, module, strings. As
with AI's `dead`, it means no reference was found: a computed call is not seen.

| Group | Functions (dead / dead-callers) | Bytes | AI counterpart dead in AI / live in AI / none | In BMX XXX |
|---|---|---|---|---|
| Libraries (layer sdk) | 369 (306 / 63) | 46 KB | 228 (3 port-dead) / 100 / 41 | 36 |
| Engine (ZGE2, below 0x163280) | 387 (319 / 68) | 42 KB | 249 / 78 / 60 | 73 |
| Game (DM2's own) | 118 (77 / 41) | 19 KB | 5 / 6 / 107 | 9 |

- Libraries: listed, never rewritten. Kernel call wrappers 95, libkernl 65,
  libcdvd 42, libdev 41, libvu0 34, libc 23, libmpeg 18, libgcc 12, libpad 11, libmc 9, libdma 8,
  libsdr 5; 302 are AI's code (identical), whose treatment applies (the ports in ps2/sce, or not_ported.tsv). The
  versions are in games/dm2/libraries.tsv.
- Engine: written as AI's are. 321 have an AI readable body (by AI's file: sound 70, render 48,
  scene 39, assets 37, debug 24, collision 24, text 19, math 14, platform 13); 78 of those are live in
  AI. 60 are ZGE2's own (no AI counterpart).
- Game: written as AI's are. By the nearest source path: physics.c 34 (15 of them the
  PlayerCreator menu: ASSETS\CREATEPL.DAT, InitializePlayerCreatorMenu, the outfit and geometry option
  counts: a rider creator DM2's PS2 build never opens), editor/editGui.c 26, zgeif/zoverlay.c 18,
  game.c 15, SpecialFX/zfxMgr.c 6, camera.c 5, fx.c 3; 11 have an AI counterpart; 51 have a DM2 Xbox
  counterpart.
- The 173 functions from 0x1C2728 to 0x1CC800 (the level object types, EditObj_GapMarker to SpecialFX/zfxMgr.c)
  are DM2's game layer. The SDK scan's match at 0x1C2728 is DM2's own code, not the library's
  (tools/workdb/workdb.py SCE_FALSE).
