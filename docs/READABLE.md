# Readable C++: replacing recompiled functions one at a time

This is a guide to the current method and state, not a log. Read the part you
need, never the whole file. What each round did, and every per-function
finding, lives elsewhere (see "Where knowledge lives"). A docs patch changes
this file only where the method or the state changed; it gets no round
sections.

## The goal

A readable, deobfuscated C++ version of Aggressive Inline that loads native
assets where possible, as part of this repository's public source. The recompiled game (`recomp/generated`, never
committed) stays the reference and the fallback: functions are replaced one at a
time by hand-written C++ (or by a draft the generator lifts from the original),
each proven equal to the recompiled one, and the whole stays bit-identical to
PCSX2 at logiccheck's 21 checkpoints. Readable means real names, real types and
comments that say what the code is for, never a register-by-register
transliteration. The direction: one engine for the collection, with
AI as the baseline and DM2 and BMX XXX as version differences; libraries are
ported from their pinned versions' source, never rewritten).

All code is in z-axis `runtime/src/game` unless a path says otherwise.

## Where knowledge lives

- The work database (`tooling/dm2/tools/workdb.py`, DuckDB at
  `$ZAXIS_SCRATCH/workdb/work.duckdb`, rebuilt in seconds, opened
  read-only by everyone but `build`). Look a function up there before any doc:

  ```sh
  W=$WORKDB_PY; PY=$ZAXIS_SCRATCH/venv/bin/python   # (paths.sh's names)
  $PY -I $W card 0x0018D678          # or a name: counterparts in all nine builds, strings, callers, status,
                                     # recordings, path coverage, and the function's notes
  $PY -I $W notes 0x0018D678         # its notes: purpose, cut, bug-kept, trigger, route, layout, evidence, caveat, history
  $PY -I $W notes cut                # every note of one topic; `notes` alone counts them per topic
  $PY -I $W query "SELECT * FROM stale_verified"      # tables and views: `tables`, `schema TABLE`
  ```

- Per-function findings go into the database as `note` results
  (`workdb.py emit FILE note addr=0x... topic=bug-kept text="..." run=NAME generator=NAME agent=NAME source=PATH_OR_COMMIT`
  into `$ZAXIS_SCRATCH/workdb/results/<name>/`; the next `build` ingests them,
  `check-results` validates them).
- The progress page (rebuilt by each batch push: the database, then the page, published to the project's
  progress site) shows each game's functions by what runs for them (readable and proven, AI's body tried,
  generated only) per layer and per folder, the source plan's steps (edited when work
  merges or starts) with each step's measured number, the checks of every batch, the scaffolding counts and the
  trends. `STATUS_NO_BUILD=1` keeps the database as it is; `status.py OUT_DIR --no-history` builds without pushing.
- `docs/DEAD-CODE.md`: the functions no game path reaches and what they are for.
  `docs/MANUAL-TESTS.md`: checks no agent can reach in 30 minutes.

## Statuses

Set in the registration (`READABLE_FUNCTION(0x00148408, stringTableText, Tested);`,
`READABLE_FUNCTION_WAITS`, `READABLE_FUNCTION_VU0`, `READABLE_PLANNED`):

| Status | Means |
|---|---|
| `Planned` | recorded only: the registration makes the recorder keep its calls, nothing runs |
| `Draft` | written (by hand or by the generator), not yet proven on every recorded call |
| `Tested` | identical on every recorded call (`ps2xFnCheck`, all verdicts pass) |
| `Verified` | Tested, and logiccheck 21/21 MATCH with it on; on by default |

Verified needs real recordings: the game's own routes, or guest calls on real game objects in states the game itself produces. Made-up data or generated calls earn Tested at most (`resolveBodyContact`: a route's holds from the dead `newBodyContactRecord`, on a real ragdoll, are made-up data).

A Draft no route reaches can also carry generated-call evidence (`diffcall.py`, the `evidence` notes, the coverage page): the original against the readable on generated arguments from real game states. It is not a status: nothing is promoted or turned on by it. A pass there is weak evidence: on the code before quar1's fixes the generated calls of `trickRelease` and `__svfscanf` all passed, and neither of those two real bugs showed.

`PS2X_READABLE` changes what runs: `off`, `tested`, `all`, names or addresses,
`-name` to leave one out, `on,0x..,0x..` adds addresses to the defaults,
`tested,-0x..` takes them away (see `ee/readable.h`). Record with
`PS2X_READABLE=off`. A function stays Draft for a reason, and the reason is a note
(`notes ADDR`, topic caveat) and the sweep's newest failure class (`card`).
Defaults must not change game logic; new behaviour goes behind a setting.

Current numbers: the database's `readable` table counts registrations by status
(`SELECT status, count(*) FROM readable GROUP BY 1`), `carry_by_layer` and
`carry_by_status` size them by layer and code size, `coverage_latest` has path
coverage, `stale_verified` the Verified functions whose source changed since their
last pass. `runtime/tools/readable_coverage.py [--profile file.csv]` counts the
game's functions (the Ghidra map without its entry_* rows), the named ones and the
readable ones per address area, and with a `PS2X_FNPROF` profile lists the functions
that take EE time. `tooling/checks/readable_names_count.py [--json] [--by-file]`
counts the machine-shaped symbols left (fn_XXXX, kFunXXXX, g_XXXX, field_0x, param_N,
register-named locals) in the swept drafts and in the hand-written sources. The
coverage page graphs both over git history. The layers by function: SDK 948,
engine 2,648, game 2,728 (of 6,324).

## Layout

```
src/game/
  game.cmake, game_headers.cmake   built into the runner and ps2xFnCheck; every header compiled on its own
  ee/        the framework: ee.h (EE RAM: ee::at, the guest map's base + address (runtime/guest_map.h: RAM at every
             address the runtime folds onto it, the scratchpad at its two), ee::Ptr<T>, ee::Global<T, address>,
             ee::Float), abi.h/.cpp (the
             calling convention: ee::entry<&fn>, ee::call<R>(address, ...), ee::StackFrame, ee::chargeCycles),
             suspend.cpp + fiber*.cpp (suspendable calls on host fibers), vu0.h (VU0 macro-mode lane arithmetic),
             readable.h/.cpp (the registry, PS2X_READABLE, install), fncheck*.h/.cpp (call recording, the recording
             format, traced originals, block tables, digests), fncheck_main.cpp (ps2xFnCheck), io.h (I/O registers
             from readable code), dispatch.h (tableJump), profile.cpp (PS2X_FNPROF), cycletrace.cpp (PS2X_FNCYCLES)
  zge/       the Z-Axis ZGE3 engine (0x100100-0x171000): math, text (strings, fonts, the script token reader), memory
             (record pools, small heaps, handle regions), containers, assets (IFF, GEO/TXR/MATI loaders, geometry objects, tag tables, images, zfile, the RAM disk), sound, native (assets read on the host:
             textures, GEO meshes, collision parts), render (the display's frame loop and video mode, the renderer,
             parts, materials and their kinds and MATI states, texture levels and cache, draw submission and its draw
             records, class renderers, shadow volumes; render/ps2 the strict encoders the PS2 draws with: the DMA
             channels, the frame packets and their fix-ups, the display's GS side and libgraph's buffers, the class
             draws' VU1 work, the part and primitive packets, the material packet and its GS state words, the textures'
             GS levels and uploads, the texture request, the motion blur and the shadow pass),
             scene (nodes, cameras and their records, skeletons, animation, messages, objects, level objects, followers and paths, the animate system), collision
             (world grid, face tree, queries, the rigid bodies' box contacts and constraint solver with its matrices), debug (the profiler
             overlay and its cycle timers, the debug line font, the debug primitives and strings), input (the pads:
             libpad's set-up and reads, the pad recorder), platform (the PS2 layer: start-up and the main loop, the
             engine's systems up and down and the frame's step, memory by kind and the main region, the clock, debug output, the interrupt handlers' register saves, IOP RPC, the save system and, in memory_card/, the memory card: the save jobs' PS2 form and its file device), movie (the engine's movies
             a game plays and, in ps2/, the player's PS2 form over Sony's MPEG stream sample, whose own sources stay the
             library's), host (the host platform's forms of the PS2 parts, as ZGE kept XBox/ beside ps2/: the disc device
             on the host's files, the save jobs on plain files, the movie stream decoded by the host, the class draws
             made natively from their draw records with render_path = records)
  host/      the host's services to zge/host (game files, save files, movie decoding, the kernel objects and waits a host form makes,
             native draws at their place in the GS stream)
  aggressive/  the game itself (0x171000-0x202800): globals, follow camera, tuning table, NPC types and pickups,
             pads, camera, game_frame, effects, replay; fx/ (particles); level/ (scene objects, traffic, props, bodies,
             dynamics, park editor); sound/; script/ (the level scripts' stack machine); zgui/ (zgui.c's start and end and file tokens, the UI tree and its
             script interpreter); skater/ (the player, the state table, every state's files, tricks, score); hud/
  sce/       libraries linked into the ELF, one file per library, each ported from its pinned version's source: newlib
             1.8.2's libm and libc (libc, libc_stdio, libc_malloc, libc_strtod, libc_scanf, libc_printf, mprec; newlib.h
             has its records), GCC 2.95's libgcc (with __main). What the EE toolchain changed and no source on disk has
             is ported from the pinned source with the change read from the machine code, each saying where: libc's
             quadword step in front of string/*.c's word loops, fp-bit's unpack_d with no denormals. Sony's libraries
             have no source on disk and are never written here: the runtime's where it replaces them, else the
             recompiled originals. Records of theirs the engine keeps are layouts in a header (packet_records.h:
             libpkt's packets); the engine calls the library by name (packet_calls.h). not_ported.tsv lists every
             library function with no C++ here and why: the runtime replaces it (kernel call wrappers, libmpeg, CD, what
             only those reach), it is dead in all three games, or it is Sony's and runs as the recompiled original
             (libpkt, libvu0, libkernl's start-up and the rest; zge/not_ported.tsv the same for Sony's MPEG stream
             sample in the engine's range). AI links Sony's release 2.4 (libkernl and libmc
             2430, libcdvd and libmpeg 2420, libpad 2410, the rest 2400); BMX XXX the same release and code (plus
             libscf), DM2 release 2.2 (every library 2200); all three the same newlib 1.8.2 and libgcc code
             (game/dm2/libraries.tsv, game/bmx/libraries.tsv). game/dm2/not_ported.tsv and game/bmx/not_ported.tsv
             do the same for each game: every library function there that is neither readable nor the runtime's
             (runtime_replaced.py), with its reason read from that game's call graph, data and corpus
runtime/tools/   what the build runs and reads: readable_draft.py, fncheck_traced_gen.py, readable_digest.py,
                     readable_symbols.py (+ .tsv), readable_entries.py, readable_lint.py, readable_marks.py,
                     readable_coverage.py, readable_names.py (+ .tsv) and their tables, runtime_replaced.py (the functions the runtime replaces, its stubs' and kernel call wrappers, left out of a corpus game's record-only registrations); the checks and analysis tools
                     (readable_corpus.py, fncheck_corpus.py, fnrec_index.py, readable_sweep.py, readable_draft_check.py,
                     readable_args.py, readable_names_count.py, readable_tablejumps.py, readable_saves.py,
                     aggressive_script_dis.py, ...) are the tooling's, tooling/checks
tests/src/ps2_readable_tests.cpp, ps2_readable_script_tests.cpp   unit tests of the framework and the script machine
```

A host form (ee/host_form.h) is an engine function as the host platform runs it, in place of
the PS2 form the readable game proves (zge/host/disc_device.cpp's dvdOpenOnHost for
zge/assets/dvd_device.cpp's dvdOpen): readable C++ at the game's symbols, one body in every
game's instance, registered with `HOST_FORM(system, sym::fn, fnOnHost)` and installed by its
system's own switch (`[host] disc = native`, the default), never by PS2X_READABLE, so the A/B's
two sides run the same form. It is not recorded or replayed: the gate's runs prove it; the PS2
form keeps its replay proof under `[host] disc = ps2`.

Struct headers lay the game's data out as it is in EE RAM, with `static_assert`s on
every offset that is known; unknown bytes stay as padding (`_0a4[...]`), inferred
fields say so. A struct's pointers are `ee::Ptr<T>` (32-bit EE addresses), its floats
`ee::Float`. Bit fields of a 64-bit type are read and written with LD / SD, which take the next
word along (a camera's change flags and its object, a profile bar's flags and its id): lay them
out as the 32-bit fields they are, the same bytes either way.

## Writing a readable function

The rules:

- A form kept only until more is known (a callee still generated, a value with no name yet, a frame kept where the
  original had it) is temporary scaffolding: it has an entry in `docs/SCAFFOLDING.md` (what it is, why for now, what
  removes it) and a `// Scaffolding (NAME):` comment where it is defined. `tooling/checks/scaffolding.py` counts
  what is left of each kind.

- The C++ signature is the game function's: integers, enums and pointers from a0-a7,
  floats from f12-f19; the result in v0 or f0 (`ee::entry<&fn>` adapts it).
- Floats are `ee::Float`: `+ - *` round towards zero with the R5900's operand
  alignment, `/` and `sqrt` to nearest, no Inf or NaN. Results must be bit-identical, so
  the order of operations is the compiled code's.
- A local whose address goes to game code is `ee::Local<T>` (ee/plain.h): it reads like the variable and lives on
  the guest stack, in declaration order from the frame's bottom; `tools/readable_locals.tsv` names the place of
  one the original keeps elsewhere, by ordinal: the count of the function's Locals alive where it is made (a Local
  of a block that has ended counts no more, so sibling blocks share an ordinal; a lambda's or a plain helper's count
  on from where it is called), each Local with its own row. `tools/readable_locals_order.py` (lanecheck, merge-check)
  checks every row against the source. A local no game code sees is a plain C++ local. Vector arithmetic is
  zge/math/vec.h: each operator runs on VU0, so what it leaves is what the original leaves. A vector operation loads the
  aligned quadword its operand sits in, as VU0 does. A store is plain C++ and does not: where the destination may sit off a
  16-byte boundary write `aligned(p) = ...`, and read data of another type as a vector with `quadwordOf(p)`, only where the
  original loads it with LQC2 or LQ: a plain word load stays a field read.
- Game code calls a game function by its name, in the original's order with its arguments: the checker requires the
  same calls. In a plain source (`src/game/plain_sources.txt`: sce, zge/math, container, memory, text, input, assets,
  platform, sound, movie, collision, render, scene, zgui, debug, zge/utilities.cpp, and game/aggressive, its level/ folder file by file) a function
  is defined, declared and called by its plain name with its own types, `vec3Length(&v->xyz())`, and registered
  `READABLE_FUNCTION(name, Verified)`. The build compiles a copy of the file (`tools/readable_entries.py`,
  `<build>/readable_entries/`) where the definition is `name_` and `name` is its entry
  (`game::detail::Route<&name_>::at<sym::name>`: the replay's answers, a hook, a function switched off, a fiber, the
  callee's frame), so the source never says any of that. Arguments are what the parameters say: an object in game
  memory goes as `&object`, a Vec4 to a Vec3 parameter as `&v.xyz()`, an `ee::Local` or `ee::Ptr` converts to the
  pointer itself; a parameter that takes any record's or text's address (`heapFree`, `memoryCopy`,
  `pointerArrayAdd`'s item) is an `ee::Address`, which takes all of those. The original's tail call is
  `game::tail(name, ...)`. What the original leaves in the result register is the result (addTagTableReference
  returns its table); a caller that reads what a void callee left keeps the address call. A helper the original
  computes inline stays inline (a shared C++ helper, not the entry). A file joins the list once its bodies have no
  underscore and its callers' types agree; `tooling/checks/readable_plaincalls.py --check` counts the calls in every other
  form, by folder, and only lets them go down.
  Its `to_plain`, always 0, is a plain function's `&name` handed on (`game::call<&name>`, `step<&name>()`, a value): the entry, unknown to the tables.
  `tooling/checks/readable_localscan.py` fails on an `ee::Local` a plain call hands on as its value, unless `readable_localscan_explained.txt` lists it.
  Elsewhere, a readable function the caller's file declares has its body marked
  with a trailing underscore, is registered `READABLE_FUNCTION(sym::name, name_, Verified)`, and has
  `GAME_CALL(name);` after its declaration (ee/readable.h): `orthonormalize(a, b, c)`, or `zge::heapFree(p)` from
  another namespace, does what `game::call` does, so hooks, a function switched off and the replay's answers all
  hold; the function's name everywhere else (PS2X_READABLE, the tables) has no underscore. Anything else (a
  recompiled callee, a readable one the file does not declare) has one typed line in `namespace { }` above its
  first caller, over `ee::game` or `game::call`; `ee::Address` is a parameter several kinds of address reach. A
  tail call keeps its form (`tail::name` when the plain name is taken). A call whose callee's register leftovers
  are read after it stays `ee::call`. `name_(...)` called directly skips all of that: only where the original's
  was no call. Several registrations with identical code fold into one host function on lld-link: they keep
  `game::callAt<&fn, sym::name>`.
- A game function held as data (a callback, a table's entry, a class's hook) is `ee::FunctionPtr<R(A...)>` (ee/function_ptr.h): 32 bits as the game stores it, 0 is none, and `pool->allocate(bytes, mask)` is the original's jump through the pointer. Its parameters say what the callee takes. `readable_args.py` scans for `ee::call`, so it does not see these calls.
- COP0 reads: `ee::io::cop0Count()` is `mfc0 rt, Count` (the EE cycle counter, as the recompiled
  instruction reads it), `cop0Status()`, `cop0Perf()`, and `cop0Register(rd)` / `setCop0Register(rd,
  value)` for the rest, with the recompiler's masks. Recordings keep Count at the call (record
  flag `kEntryCount`) and the replays start Count there; older ones start it at 0, so a function
  that reads Count does not replay as recorded until it is recorded again.
- A pointer to a game function kept in game memory (a class table's method, a handler, a callback) is an `ee::Callback<R(A...)>` member (ee/callback.h): four bytes, called like the function, `.tail(...)` for the original's tail jump. It goes through `ee::game`, so what a jalr does holds. A function that calls itself calls itself by name, as a typed call (recursion; see "The gate"): `ee::selfCall` is scaffolding and fails the lint.
- Index guest arrays through `ee::Ptr` (`ptr[i]` is guest address arithmetic, wrapping in 32 bits
  as the original's addressing does, and stays in guest RAM). `ee::at<T>(address)[i]` and a raw
  `T *` index the host pointer: a wrapped uint32 index faults the host where the original reads a
  wrapped address.
- Kernel calls: `ee::kernelCall(number)` is `addiu v1, N; syscall` (the arguments as the
  function's caller left them, the result is v0). It runs through a table slot of its own, so a
  call that waits leaves the fiber as a guest callee does; drafts that make one are `_WAITS`.
- Parallel instructions on whole registers are `ee::mmi::` (ee/mmi.h: the generated code's
  `PS2_*` forms on `ee::Qword`, SA, HI:LO).
- A register the original only hands on unchanged (to a callee or a tail jump: a variadic
  one's a3-t3) is not a parameter: pass the caller's on with `ee::callerRegister(ee::reg::t0)`.
  A parameter too many makes every drafted caller pass a leftover (zgehand10's formatString
  with eight made newAppInstanceNode's draft take t0-t3, and spawnDebris failed the check).
  That holds for a register nothing reads. One a table's method reads (a jalr shows the drafter no arguments) is a named parameter, and pass it: once a row is
  hand-written, drafted callers call it as `game::call<&fn>` with the signature's arguments, `ee::callerRegister` and the context then hold the caller's own, and the replay
  answers such calls from the recording, so only the game shows it (gamehand25: activateEditTool's a1, updateEditTool's f12; the editor A/B hung).
- A local the original never initialises (a kept bug: initZgui's and finishUiImageLoad's material flags) holds what
  the caller left in the register its compiler gave it: `const ee::LeftByCaller<int32_t> flags;` (ee/plain.h), read
  like the value it had at entry. The register is the function's row of `tools/readable_entry_reads.tsv`, which the
  build's copy (`readable_entries.py`) gives it; game code never names it, and the lint fails one that does. The
  replay starts every call from the recording's registers at entry, so it hands over what the original's caller left.
- Pointers a function stores keep their EE address (`ee::Ptr` arithmetic; the uncached
  mirror 0x20000000 | address must survive). A local whose address goes to a callee lives in the
  original's frame: `ee::StackFrame`. Every readable function enters in its
  original's frame (the frame table) and writes its register saves. What the original holds in
  its callee-saved registers at each call (s0-s7, fp, f20-f31: a pointer it walks, a count, a
  float it loaded), which the callee's prologue saves into its frame, the runtime gives the
  callee from the held-register table (ee/held.h, `tools/readable_held_gen.py` at build time):
  per function and call site of the original, one expression per call made before it, over the
  entry registers and frame, the call's arguments, the last call's results, what the last call
  got, loads with no store or call since, entry loads (memory the original reads before its
  first store outside its frame and its first call, read as the function is entered), and the
  original's own operations, a branch's two values included (a select on its condition, the
  FPU's compares too), a word the function stored in its own frame and read back, and the code
  between two calls run as a small program where it loops (a value only when the run went the
  original's way to the site). Game
  code has no part in it. A value it cannot give (a loop that stores, an operation not
  modelled: `<build>/fncheck_build/readable_held_unknown.tsv`) is the entry value, the replay's
  `saves_differ` / `held_differ` notes; a draft's `ee::calleeSaves` still wins. That is no proof
  of the value: the original may have changed the register, and the entry value is right only
  while every readable level above hands on what the original held (DM2's showGameScreen's s4,
  a select since held2, went to actionStart as runScript's). Of several sites a call can be, the
  case's checks (the call's arguments as the original makes them, loads included, read only to
  choose, and in the game only) pick one, and so do the branch conditions its path met since the
  previous call; a load that cannot be read rules its case out. A value
  the site alone does not prove (its cases give it differently, a case carrying what an earlier
  site set compared by that value) is given only when one of the
  case's checks was worked out and held, else the entry value. The table's programs and each case's values
  and checks are also generated as code from the same nodes (`tools/readable_held_code.py`, `ee/held_program.h`:
  one meaning per node for both), which the runtime runs; `PS2X_READABLE_HELD_CODE=0` works the rows and nodes
  instead, `=check` both, compared. `PS2X_READABLE_HELD=0` leaves the
  table out. AI's app runs without it (`PS2X_READABLE_LEAN=1`, its boot's default there: a third of the EE thread in
  AI's levels; DM2 and BMX XXX keep it until the words it alone gives them are proven unused); the A/B, the replay and the logic checks keep it, so what it alone gives (the bits a guest
  function takes from a register it never set, stack lanes a record copies) stays the original's there.
- A callee that can wait (a file read, a semaphore) or yield: register with
  `READABLE_FUNCTION_WAITS`; the function runs on a host fiber and its calls are real guest calls.
- EE time: generated code charges 32 cycles at every backward branch and 8 for every call it
  dispatches, and emulated time decides when vsyncs and interrupts fall. A readable body charges
  the 8 of each call it makes but none of the original's loop cycles, so at the defaults EE time,
  and with it the vsync a read lands on, can differ from the original's (b131's strcpy moved AI's
  boot's first prompt read from vsync 4 to 3). The gate does not compare time (it judges logic at
  4096x); a recording replays with its own build's readable set (`replay.py run`, DEVTOOLS.md). The
  few functions that still mark the original's entry blocks (`ee::block`, `ee::loopBack`,
  `ee::tableJump` for a jump table's case outside an entry block) leave at its safe points when a
  checkpoint is due. `readable_tablejumps.py BUILD ADDR...` prints the
  lines a hand-written function with such a JR needs. Marks are owed only from a place a resume can
  reach: a loop head, a call's return that is an entry block (ee::block(L, true), or ee::afterCall
  when other paths join there) and what follows them; a mark before that does nothing, so leave it
  out. `readable_marks.py --check BUILD` lists what is owed.
  Where the drafter does not finish (`Planned`, "the generator does not finish"), write it from
  the listing (decomp/asm) with the entry blocks recomp/generated's `entry_*` files name.
  After a drafter change, `readable_sweep.py manifest-redraft --build DIR --target ps2_game_readable`
  drafts every Planned row again with no run state (a draft that does not compile goes back to
  Planned with the error). `--add --only @LIST --why WHY` adds functions in no manifest.
- Verified code holds no open item: a draft with a `TODO`, an inline `/* TODO ... */` or an
  unknown expression (an unmodelled value) does not qualify. `promotable` leaves it out,
  `manifest-verify` refuses it, and both report any already Verified, with hand-written source;
  `readable_sweep.py manifest-todos` is the gate alone. What a draft says about itself that is
  settled is a `// Note:` line.
- A function used from another source is declared once, in a header, as it is defined: a
  return type that differs links on the Mac but not with MSVC or clang-cl, which mangle it
  (setNodeMaterialSet). A struct type in a readable function's signature is written qualified
  (`ee::Ptr<aggressive::EditToolSet>`): the drafter copies the signature into its batches' declarations and
  qualifies an unqualified type with the function's own namespace, which does not compile.
- Names come from the strings a function passes, the tuning table, the nine builds' counterparts
  (the card), the string tables and the level scripts; never invent a name the evidence does not
  support. Libraries are pinned to an exact version and ported from its source.

## Symbols: names, never addresses

A guest address is a row of a table, never a number in readable code. `tools/readable_symbols.py` does the work.

```cpp
ee::call<uint32_t>(sym::memoryFill, dst, byte, bytes);       // a callee
ee::Global<uint32_t, sym::g_scratchBuffer> g_scratchBuffer;  // a global
namespace here = points::stringFindAny;                      // a function's resume points
ee::block(here::loop);  ee::calleeSaves({{31, here::afterStrcat}});
ee::calleeSaves({{18, ee::high(sym::g_tuning)}});            // a %hi the original keeps in a register
*ee::at<uint32_t>(ee::uncached(sym::g_fioClient));           // the uncached mirror
READABLE_FUNCTION(memoryFill, Verified);                     // the address is the symbol's
READABLE_FUNCTION_IN(dm2, sym::dm2::riderFrame, riderFrame, Verified);  // DM2's own, outside game/dm2
```

- `tools/readable_symbols.tsv`: kind, name, address, function (a point's), game (empty: AI's). Names and addresses only, the same kind of data as `readable_names.tsv`; committed so AI builds without the work database. `generate` makes it: AI's functions and data from the work database (registrations, `readable_names.tsv`, the sources' constants and globals), each resume point from the machine code; a name a source uses stays as it is (`rename` changes one, in the table and the sources), so only an address no source names takes the database's name as it stands. `generate --drafts <build>/ps2xRuntime/readable_sweep --check` says the committed file is what the database gives. Rows of game `dm2` (`bmx`) are addresses of that game's own: in its folder (game/dm2, compiled in DM2's instance only, its sources including `sym/dm2/...`) they are plain `sym::name`, `points::fn` and `READABLE_FUNCTION(fn, Verified)`, which the tools read as DM2's (`literal_view` gives `READABLE_FUNCTION_IN(dm2, ...)`); anywhere else `sym::dm2::name`. `convert` makes them, `generate` keeps them and adds the database's DM2 function names.
- A new symbol: a name for an unnamed datum goes in `readable_names.tsv` (`datum 0xADDRESS name confidence evidence`, for this table only; `global` rows also drive the drafter's output; `part 0xADDRESS name+0x10 ...` for an address inside a named object, written `(sym::name + 0x10)`), for anything else `readable_symbols.py add function NAME 0xADDRESS`, `add data ...`, `add point NAME 0xADDRESS FUNCTION`, `--game dm2`; or write the code with literals and run `convert FILE`, which names what it can and lists the rest. `rename OLD NEW` follows a rename through the table and the sources.
- A point's name is a pure function of the code: `afterStrcat`, `loop`, `join`, `taken`, `notTaken`, `done`, `next`, numbered in address order over all the function's instructions, so a name does not change with which points a source uses. These are role names; a person replaces them, or marks leave the source, and they go with them.
- `runtime/tools/bind/<game>.tsv`, the game's bind table, and `<game>.layout.tsv` beside it: generated and committed, so a commit builds with its own tables on every host and a lane changes them in its own branch. `readable_symbols.py bind --write` makes both from the work database as its last `workdb.py build` left it, the two ELFs (paths.env's workspaces) and the tree's sources, and writes the ones that differ (the venv's python, which it starts itself; a second); `bind --check`, which lanecheck runs, fails when a committed one is not what they make: a work database built since with other pairs, or a level-2 function whose fields changed. A table's first line names the database build it was made from. Every build of the commit uses these (game.cmake: `PS2X_BIND_DIR` set, whatever it names, is this folder; rcheck's jobs build from the checkout's), never a shared copy; a commit from before tools39 still builds from paths.env's `GAME_BIND_LEGACY`. The table (`table --game dm2 --elf-game ELF --out FILE`, what `bind` runs) walks every pair the database calls identical word by word (a pair that fails is an `entry`), and each pair's `jal`, `lui`/low and gp-relative words prove where the game has AI's callees and data. `body` rows rebase every address inside the function, `entry` rows only its start. A pair the match got wrong, read in both games' machine code, is a `pair` result (`workdb.py emit FILE pair addr=AI build=ai_ps2 b_build=dm2_ps2 b_addr=GAME method=read cls=identical|close|drifted confidence=high ...`, with its evidence in a note; `b_addr=0`: no counterpart): the next `build` takes it in place of the match's.
- `game/<game>/bound.tsv`: AI's symbols a game has where no pair of the table names them (a callee a drifted caller reaches, a datum only close code uses, a function inside another's body), one row each with the evidence read in that game's machine code (an identical pair's walk the table did not take, the same words found once in each ELF, a recording's call, the same bytes): kind, AI's name, the game's address, the evidence. A close function's resume point is a `point` row named `FUNCTION::NAME` (the source's `points::FUNCTION::NAME`, `here::NAME`), its function bound and the point after its start. `Bind` adds them to the table; a row the table names otherwise stops it. An address with no such evidence stays unbound.
- Pairs settle against each other (`settle_votes`): an identical pair is only proven by the words a link does not change, so a row its callers contradict is corrected from the call sites, and a pair that contradicts a sure row (a pair read in the machine code, a long identical pair, or most callers) stays an entry and shares no body; a read that the game has no counterpart is sure too, so no call site or alignment gives that function a row (BMX XXX's nodeNameHash). Every pair of equal size is walked, moves carry a `lui`'s high half, and a `lui` hoisted over a branch is followed into both arms. What a close pair's instructions line up with (runs of 6, matched by kind) gives `aligned` rows for addresses no walk names, and corrects a position-found row that no walk names (BMX XXX's freeMaterial: a drifted pair's alignment named another function where an identical caller's walk names the right one). Where close pairs disagree on a datum, an alignment that makes it an address an identical walk names as another AI datum's loses (BMX XXX's g_animalCarrierTree had no row while a mispaired function voted it the animators' tree); and two globals in another order, used in that order, walk word for word: where at least two close pairs align a datum to an address the walk names as another datum, and outnumber its walkers, those walkers stay entries (BMX XXX's carrier and attachment pools, freed in the other order by its freeCarriedAnimals).
- Each source includes its own header, `#include "sym/zge/text/cstring.cpp.h"`: generated at build time per source and game (`headers`), written only when its text changes, so a new symbol recompiles the files that name it. The constants are compile-time: AI's objects come out byte for byte.
- A game's instance: its own folder (game/dm2, game/bmx), which no other instance compiles (AI's is zge, sce and game/aggressive), and `PS2X_GAME_INSTANCE_SOURCES_<game>` in game.cmake, the shared sources compiled again for that game (`PS2X_BIND_DIR` set; DM2's is every source of `game/dm2/instance_sources.txt`, the engine it has, in every build with its bind table), with its headers and `zge`, `sce`, `sweep` renamed (`-Dzge=zge_dm2`), so every function, inline and constant is that game's own. Each registered function gets a level: 2 its body runs there (identical word by word, every symbol it names bound, every readable callee at least level 1), 1 only its address is known (registered Planned: `game::call<&fn>` reaches the recompiled function), 0 no such function there. A game folder that is a plain source (game/dm2 in `plain_sources.txt`) calls its engine by name (`zge::vec3Negate(...)`, its instance's entries) and is built only with that instance: a build with no bind table has none of that game's code. `<build>/symbols/<game>/levels.tsv` lists the instance's functions with the reason. The instance renames `sym` and `points` too: their constants are `inline`, and one passed by reference would be a weak symbol shared with AI's objects. A status is per game, in `game/<game>/symbols_status.tsv`: nothing is on in another game because it is on in AI; it is proven on that game's recordings first. A shared body a game cannot run is a `Planned` row there with its reason (its level is 1), never a registration under `#if` in zge.
- Layouts: `layouts` takes the struct fields a level-2 function uses and the offsets the game's own machine code uses; the instance has them as `static_assert`s (`sym/<source>.layout.h`, included at the end of the source). A field that differs in a game fails there; the struct then gets a per-game difference, which an instance can have at compile time.
- Per-game differences: `game::readable::perGame(ai, dm2)` for a constant, `game::readable::kIsDm2` for a step, `#if PS2X_GAME_DM2` for a struct's fields (an instance's macro; each field at the offset that game's machine code uses). `game/<game>/close.tsv`: functions whose words differ and which were read as the same logic, one row each with what differs; they get level 2 and are proven on that game's recordings like any other. An address two AI functions share (DM2 calls `heapAllocate` where AI calls `allocateSized`) registers one body. A field only one game's struct has (ZGE4's emitter burst, DM2's primitives switch) is reached through an accessor named for what it returns (`emitterBurst(data).rate`, `primitivesOnWord(words)`), defined in the header in a `#if PS2X_GAME_BMX` block beside the fields with the other games' stubs in its `#else` (pool.h's clearMostInUse is the pattern): the block holds accessors and fields only, never logic, and the shared body uses the accessor under `kIsBmx`, so it compiles in every game's build and no `#if` enters a .cpp.
- A function only one engine version has, with no AI function to bind (ZGE2's `lengthSquared`, which ZGE3 and ZGE4 square inline): `READABLE_FUNCTION_ONLY(dm2, lengthSquared, Draft);` (`_WAITS_ONLY`, `_VU0_ONLY`; ee/readable_waits.h) in the shared file of the code around it, its body defined `inline` there (`headers` stops otherwise) and its symbol the game's own row (`sym::dm2::x`, `sym::bmx::x`). AI's build registers nothing and emits nothing of it, so AI's objects are what they were; that game's instance binds it at level 2 (1 when something it names is not bound there) with its `symbols_status.tsv` status, another instance not at all. The tools read it as that game's `_IN` registration (the lint counts it with the game's folder); it is not `reg_in`. Callers reach it through its symbol, as the original's `jal` does.
- A version's own implementation under the shared symbol (different logic): `READABLE_FUNCTION_OWN(fn, Verified, dm2, ownName[, bmx, ownName2]);` (ee/readable_waits.h), the own body named for what it does in the same file. It is its game's alone: the build copy makes it inline elsewhere and its entry that game's (`READABLE_ONLY_DEFINE`), so AI's objects have none of it; that game's code calls it by its name, with its own parameter list (DM2's `shapeBoundsOverlapBox`, ZGE2's queryOverlapsBox).
- A shared function one version has far from the others, whose own body is that game's (game/bmx/debug's drawColouredDebugText for drawDebugText): `READABLE_EXCEPT(bmx, READABLE_FUNCTION(drawDebugText, Verified));` (ee/readable_waits.h) registers it in every build but that game's instance, which registers only the game's own body at the address (its row, `READABLE_FUNCTION(drawColouredDebugText, Draft)` in the game folder, or an _ONLY function the bind's pair put the shared name on); `headers` gives the shared name level 0 there ("not registered here"), and the shared name's entry calls what the game has at the address. Without it the instance has two registrations at one address and `readable::find` takes whichever links first.
- One body whose versions take other words (ee/readable_waits.h): a parameter only some versions have is written after the one before it, with no comma between, and its value in the other versions' bodies, `void skinnedColourWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t vertex READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))` (`READABLE_PARAM_EXCEPT(dm2, uint32_t flags = kNodeKeepsRegion)` for all versions but one; a header's declaration names it without the value); a call passes it the same way, `skinnedColourWord(packet, part, vertex READABLE_PARAM_ONLY(bmx, colourShift))`; a result only some versions have is `READABLE_RESULT_ONLY(dm2, uint32_t) addRigidColours(...)` (void in the others) with `return READABLE_RETURN_ONLY(dm2, vuAddress);`. AI's build sees exactly its own words. A first parameter one version has not, another order of the first words, or other logic is an own body. The tools read a game's view through `readable_symbols.version_view`; `readable_layering` counts each form (`per_game_word`).
- The layering: one engine, three games. zge holds the engine once, whichever games use a function: a game that lacks one does not call it (or its instance leaves the file out), and a difference goes inside the one body (`perGame`, struct fields side by side in the header, `if constexpr` for a step one game skips; never an `#if` in a .cpp). A game folder holds its game's code, and only game code may have a second copy (BMX XXX's park editor beside AI's). Engine or game is a question of evidence: each original links the engine library (`../ngengine/ZGE*`) first and the game's own sources (`./src/com`, `../GameTools`) after, so where a function sits in each game's ELF and where its counterparts sit in the others' says which it is. `tooling/checks/readable_layering.py --check` counts, by folder, game conditionals (`pp_game`; their lines in a .cpp, `pp_lines`, and in a header, `layout_lines`), `kIsDm2`-style branches, `perGame` values and layout checks, another game's registrations outside its folder (`reg_in`), game/aggressive functions a zge file names (`up_ref`), game code in zge (`game_in_zge`), engine code in a game folder (`engine_in_game`), and functions whose evidence disagrees, not yet judged (`split_in_zge`, `split_in_game`). The evidence is `readable_layering_games.tsv` (`readable_layering.py games`, from the work database: each AI function's block, its counterparts' classes and blocks); a verdict given one function at a time goes in `readable_layering_judged.txt` with its evidence. Where the engine calls into the game (engineMain's `gameStartup`, the scene database's packed library reader), `zge/game.h` declares the function in the engine's namespace and each game defines it: AI in game/aggressive (a plain source, so the call is its entry), DM2 and BMX XXX in their folders (`game_main.cpp`: their originals by address until each has a body); `up_ref` does not count those. Counts only go down (`--rebaseline` refuses a rise), except once when a new game joins ee/game_id.h. The collection (the third layer: `runtime/src/collection`, `include/collection`: the app and its game picker, the shared settings, the bottom screen, mods) is counted over `runtime/src` and `runtime/include` by the headers each file includes: `collection_in_engine` and `collection_in_game` (an engine or game file including `collection/`: never), `collection_reach` (a collection file including an engine's or a game's header that is not an interface: `zge/native/*.h`, the engine's readers made for the host, and `zge/game.h`; what the collection needs of a game goes through the runtime's contracts, such as the settings registry and `runtime/panel_snapshot.h`, which a game's override fills and the panel reads) and `collection_in_runtime` (the runtime reading the collection's game table: a ratchet).
- Game overrides (`runtime/src/game_overrides`, the host code that hooks a game) name its functions and data the same way, from the same table: a file that includes `#include "sym/game_overrides/<file>.h"` writes `sym::dm2::g_cameraSettings`, `sym::bmx::drawWidget`, `points::dm2::showPauseScreen::itemsSet` (a game's own resume point, a return address), `sym::name` for AI's. Every game's overrides are built into one runner (a unity build joins several), so another game's names are always qualified. `tools/override_symbols.py headers` writes them into `<build>/ps2xRuntime/symbols/overrides` (the game's own row first, else AI's name where the game's bind table and `bound.tsv` put it; a name the table does not bind fails the build, and a build with no bind table gets UNBOUND, as the readable sources do), and its `lint` fails an address literal in a file that includes its header. It is not a checker: it reads `readable_symbols.py`'s table and `Bind` and changes neither.
- Drafts: `readable_draft.py --manifest` writes its batch files with symbols, by the same code and with no ELF: the words come from the generated sources' comments, data from the table. A row the table has not got goes to `<build>/ps2xRuntime/readable_sweep/symbols-extra.tsv`; `generate --drafts <that folder>` commits it. Data with no row stays a literal and fails the lint: name it.
- Register mirrors: `readable_pass.py run mirrors` takes out a read of a callee-saved register the original never reads as it found it; `tools/readable_entry_reads.tsv` lists the ones it does. A result no caller reads in the ELF is `void`; a variadic function's words are `ee::VarArg` parameters, defaulted in its declaration (`ee::VarArg first = {}`, ee/plain.h), so each call names the words its format reads and only those are passed. `readable_lint.py` fails a guest address in any comment.
- `lint` runs in every build at zero: any address in code (a number in the image's range, its uncached mirror, or a `fn_123456`-shaped name) fails it, in the sources and the drafts, and so does a `FUN_`, `DAT_`, `LAB_` or `fn_` reference in a comment, reported apart, and a source's own `#include "sym/..."` under a preprocessor conditional (the other platforms would have no symbols; `convert` moves it). A number that only looks like an address is written another way (`1u << 20`) or says so on its line (`// not an address`). Addresses in comments are not linted; game/dm2's cite both games and other builds.
- Tools that scan sources for addresses read them through `readable_symbols.literal_view()`: registrations, marks, constants and digests look as they did, whatever a symbol's called, with `ee::high`, `ee::uncached`, parts and number forms folded back to the number. A digest does not change with a rename. A game folder's registration of an AI name is written at the game's address its bind table gives (as its instance registers it), never AI's; with no bind table it is left as written.
- `ee::high` and `ee::uncached` are `consteval`, so the object is the literal's: a `constexpr` call folded late gives other code.
- The manifests' `srcref` flag keeps a draft's return value for the 17 functions a source's comment used to name by address (`source_mentions`).

## How a function is proven

1. Record real calls: `PS2X_FNREC=<names, addresses, @file, or all> PS2X_FNREC_DIR=<scratch>` with
   `PS2X_READABLE=off`. The registered functions are built a second time from `recomp/generated`
   with every load, store and call traced (`fncheck_traced.h`). A recorded call keeps the registers
   at entry and return, every byte it read as it was at entry, every byte it or its callees wrote,
   each call it made (target, arguments, returned registers, RAM changes), I/O register accesses in
   order, and the EE cycles its own code charged. Sampling caps calls per function and per vsync
   (`PS2X_FNREC_MAX`, `_PER_VSYNC`, `_MIN_CALLS`, `_FROM`, `_ATOMIC`, `_WAITS`); callee writes are
   found by page protection. A function that never returns (a card job's end that deletes its own
   thread, a thread's endless loop) is recorded to a stop with `PS2X_FNREC_STOPS=1`: the functions of
   the table in `fncheck_stops.h`, each call kept up to the callee that never comes back (the thread
   ended in it, or the first wait `_STOP_FRAMES` vsyncs in), RAM and registers as at that call. The
   checker replays such a record to that call, checks it as any call and judges RAM and the calls
   made there. Stop recordings go in route folders of their own (`stop-ROUTE`). Recordings are game
   data: scratch only, never a repository. Files are compressed with zstd, a frame a record
   (`ee/fnrec_file.h`; `PS2X_FNREC_PLAIN=1` writes plain ones); every reader takes either form, and
   `zstd -d` gives the plain file back. `tooling/checks/fnrec_pack.py CORPUS` compresses a corpus's
   plain files in place, keeping each one's name, mtime and verdict-cache key.
   Readable code off is enforced: the recorder refuses (the run ends, exit 2) unless `PS2X_READABLE=off`
   or a test's `PS2X_FNREC_ALLOW_READABLE=1`. `PS2X_FNREC_NEW_BLOCKS=3` keeps the capped sample as
   unset does and, past it, every call that runs a block of the original no kept call ran (nor
   `PS2X_FNREC_COVERED`'s), so a route keeps its late branches; 1 keeps only those.
2. Check: `ps2xFnCheck <recordings>` replays each call from its entry state in an empty RAM against
   the traced original (which must give the recording back) and against the readable version twice
   (unrecorded memory filled with 0xA5, then 0x5A). A call passes when the result by its C++ return
   type, the RAM afterwards byte for byte, the callee-saved registers, the calls made (same order,
   same arguments) and, with `--cycles`, the EE cycles all equal the recording. `--semantic` is the
   verdict merges are held to (below, "The gate"). Flags:
   `--only fn`, `--limit n`, `--show n`, `--list`, `--semantic`, `--cycles`, `--markers`, `--safepoints [p [c]]`,
   `--call-blocks`, `--call-timeout s`, `--max-mem-mb mb` (default 2048), `--runaway k` (default 64),
   `--generate fn n` (a pure vector routine, generated inputs: every path, not only recorded ones).
3. Safe points: with `--safepoints 32 3` the replay interrupts the call at the original's safe
   points and checks the readable function resumes and charges as the original does; `--markers`
   checks the entry blocks it passes. Differences show as safe-point differences per function.
4. Path coverage and digests: the traced originals carry a marker at every basic block, so a
   function's verdict also says how many blocks the recordings ran (`coverage_latest`,
   `fncheck_corpus.py coverage`). Every result line ends with `impl` (the readable source digest),
   `orig`, `rec` and `checker`, so a pass counts only for the code it was made against
   (`stale_verified`). Verified does not mean every path ran.
5. Corpus: `readable_corpus.py` records every registered function over a fixed set of routes
   (`tools/routes.py`, the logiccheck and careercheck inputs, the cov- routes for paths no other route
   reaches), once per recorder version, into `$ZAXIS_SCRATCH/reccorpus/v<N>/<route>/`.
   `fncheck_corpus.py run --incremental` replays every recording against the build with a verdict
   cache and replays only what changed; `compare`, `same`, `coverage`, `saves`, `emit` (workdb
   lines), `prune-plan`. It caps its own memory (`--max-mem-gb`, `--jobs auto`). On the Windows
   worker the same replay runs (DEVTOOLS.md, `rcheck.sh`).
   DM2 has its own corpus, `reccorpus/dm2-v1` (`tools/dm2corpus.py` in its repository, routes in `dm2routes.py`): a
   build with `PS2X_GAME_CORPUS=dm2` (and `PS2X_BIND_DIR`) has all of DM2's 4,527 functions (`game/dm2/functions.tsv`)
   traced and registered Planned, and an instance of every shared engine source (`game/dm2/instance_sources.txt`), the
   level 1 bodies registered Draft for the replay to judge (`headers --try-close`). `readable_locals_dm2.tsv` and `readable_args_dead_dm2.tsv` hold its rows of word-identical functions. `dm2cov.py` says what the routes
   reach (`PS2X_FNPROF` needs no traced build), `dm2proof.py` what a replay proves by level. Its route folders live on
   the Linux worker: `dm2corpus.py ship`, then `rcheck.sh` replays them there (`--dm2-corpus --fncheck-arg=--semantic`).
6. Integrate: `tools/lc-when-free.sh NAME`, 21/21 MATCH with the defaults and with
   `PS2X_READABLE=tested`, each against `off` at the same clock; the career A/B (`tooling/checks/career_ab.py`:
   nine inputs unpaced with the null GS, defaults against `off` at 4096x, always readable off on one side:
   every state field at every tick equal, 0 RAM words outside dead stack and `ab_allow.txt`, the scratchpad's hash
   equal, the streams equal but for the words nothing reads; `PS2X_FNCYCLES`, `_WATCH`, `_VU0` and `PS2X_TRACE_EE` find the first divergence when not;
   DEVTOOLS.md); `dm2logic` 6/6 for DM2, and `career_ab.py gate --game dm2` (its routes, `ab_allow_dm2.txt`). A function that passes everything is
   promoted (`readable_sweep.py promotable`, `manifest-verify`), and the unit tests (`ps2x_tests`
   from the z-axis worktree root) pass.

What the check cannot see: registers that are dead after
the return; a caller's outgoing stack arguments (except the callees the build lists); VU0 state of
functions not registered `_VU0` (a note on the result line, `fncheck_corpus.py vu0`: a
lane left wrong is live, a later VMULx.xyz keeps it and an SQC2 stores it, see below; the note sees what
is left at the return, not what a callee finds in the middle); an uninitialised
stack the code reads later (the rows' w of a matrix quaternionToMatrix fills on a caller's stack: the
stack's words reach node matrices and VU1 0x3F3.w, and a readable function does not hold what the
original spilled there, such as updatePads' quadword temporaries; a local the original keeps in its frame
goes in `ee::StackFrame`); a callee that waits (the
game and logiccheck see it); a callee's reads that are not in its recording (a call is answered from the
recording, so a callee that lost an argument still returns the recorded result: `readable_args.py`, below),
beyond what `callee_read` reaches (below); and every path no recording ran.

What a callee reads at a call is compared where the replay can tell it (`callee_read`): in the original's
replay each call's callee runs first as its traced original in a sandbox (its changes undone), from the original's
state there, following its calls, until the first byte the replay does not hold, a function that runs the runtime's
own code (`g_untracedCode`: a library stub, a syscall), an I/O access or a cap; when the folder has the callee's own
recording of that very call (all general registers and the vsync match), it runs on that record to its return instead.
The bytes read on the way that hold the original's memory at the call must be the same at the readable version's call
(bugs1's contact record, dead stack after the return). It is a lower bound: the result line's `callee_probes` ...
`callee_buckets` say how far it got (`--no-callee-reads` turns it off, `PS2X_FNCHECK_PROBES=1` prints each call).
A probe stops at the callee's first global the caller's recording does not hold, so a record the caller builds on its
stack for a callee is read by the recording instead (version 7, tools44): the recorder keeps, per call, the bytes of the
caller's frame the callee read before anything wrote them (fncheck.h `Call::reads`). A recorded callee runs traced at
every call that hands it a pointer into a recorded caller's frame, kept or not, and one it hands the pointer on to adds
its reads. Those bytes the original's replay holds with the recorded value must be the same at the readable version's
call (`callee_read`, "in the caller's frame, as the recording has the callee read it"; `callee_frame_read_calls`,
`_bytes` count them); there is no run again, so read is enough. Older recordings have none, and a callee the recorder
does not record or a pointer left in memory gives none (portbmx4's beginObjectiveBlock: a handler in the wrong slot of
the set-up registerBlockClass copies).

What a function leaves in its own frame is compared too: its working values there outlive the return, and a
later function that reads a local it never set reads them (closestPointOnTriangle's edges came back as BMX XXX's camera
matrix's w). After each readable replay the frame at the return, `[deadLow, entry sp)`, must hold the original's bytes;
a difference is a note (`frame_residue_differs`). The frames already hold the original's register saves (the frame
table), so what differs is working values. Whether a later function reads it comes from the folder's index of leftover
reads (`fncheck_corpus.py run --frame-index DIR`: first every recording's original alone, `--frame-reads-out`, keeping
each call's reads of its own frame before it writes them, and its callees' through a pointer into it, probed; each
call replayed again with its leftovers turned over, which says whether it acts on them). A differing byte the folder's
next leftover read after the call (by vsync, then the EE cycle at entry) finds with the original's value, by a call that
acts on it, fails (`frame_residue_read`; `frame_residue_read_value` where the bytes read are not all zero); read by one
that does not act on it, a note (`frame_residue_masked`). Only recorded readers and recorded calls count, AI's older
recordings have no entry cycle (the order within a vsync is not known), and the index is cached per folder under its
recordings, their originals and the checker.

A call is only as good as the registers it hands over. `ee::call` sets the arguments it names; `game::call`
reaches its callee as C++ and sets none, so a function that leaves a register for a callee (`ee::call(method)`
with a0 still the caller's, f12 still `seconds`) is right when a table call enters it and wrong from a
`game::call`, with every replay passing. gamehand25's activateEditTool hung the EE that way at tick 4480.
`ps2xFnCheck` follows where each argument register's value comes from: `args_left` (another value than the
original's) fails, `entry_left` (an entry value left in place) fails a function some `game::call` names;
`readable_args.py --check --build BUILD`
looks at every call site, recorded or not, against what the listing says the callee reads, through indirect
calls too (readable_indirect_calls.tsv, from the recordings: `readable_args.py indirect CORPUS` after new routes),
and is run at every merge. Pass what the callee reads by name; a hit that is not one goes in readable_args_ok.tsv
with the reason, a read the code never uses in readable_args_dead.tsv.

### The gate

What a merge has to pass (`merge-check.sh`, `batchS.sh`). Verified says a function works, `readable_lint.py`
says a person can read it; they are reported apart and neither implies the other.

- The pins first: every file that decides a verdict is pinned
  by hash outside the tree (the replay, the corpus compare, the A/B, its allow list and inputs, logiccheck, the
  arguments check and its exception lists, the lint and its baseline, the state probe). A file not as pinned
  stops the merge; the maintainers read its diff and re-pin it. An agent that needs a
  check changed says so in its report.
- The ratchet, in the build: `readable_lint.py --ratchet` against `readable_lint_baseline.txt`, by address. A
  function that passed may not stop passing. The lint follows helpers: a function with no registration of its own, in the
  same file or a thin wrapper in an included header, answers for its raw addresses and offsets, VU0 work, selfCall and
  scaffolding, and so does every function that reaches it (`via NAME: LABEL`); ee/ and zge/math/vec.h are not charged. A
  register in a function's comments or a local named for one fails it too. When a rule gets stricter, the orchestrator
  alone lowers the file: `--rebaseline REASON`, its own commit, the reason and every function dropped kept in its comments.
- `readable_args.py --check` and the unit tests. A variadic call's jal is told by its first argument (a text's address,
  read from the code), and a `dropped` row in readable_args_ok.tsv names its jal, so another site that loses an argument
  is a new hit. A wrapper or helper in another file counts for its callers. A call through ee::FunctionPtr or ee::Callback
  is an indirect call like any other, its type says which words are floats, and a value the original leaves for the jalr
  in its block counts whoever is recorded; a helper that hands on a pack is checked at each place it is called.
- The semantic replay of the whole corpus: `fncheck_corpus.py run --semantic` (`rcheck.sh --fncheck-arg=--semantic`),
  which exits 1 if the readable side failed or faulted on any recording,
  then `compare PREV NEW --no-worse --semantic`: no pass may become a fail. A call passes on what it does: results by
  type, RAM byte for byte outside the dead stack, callee-saved registers, every call with its target and arguments,
  stack arguments, `args_left`, `entry_left` for a function some `game::call` names, the memory each callee reads at
  its call as far as the probe reaches (`callee_read`, above), with `--frame-index` the frame it leaves where a later
  function reads it (`frame_residue_read`, above), I/O in order, and the VU0 lanes below. Time, marks and interruption do not count. A function that calls itself runs its body directly (recordings hold
  no call for that, generated code runs it inline), so a recursive call is written as a typed call and what it does inside
  is checked against the recording in order.
- Logiccheck at the gate's clock: `PS2X_EE_SCALE=4096`, `off` first, then the defaults and `PS2X_READABLE=tested` with
  `LC_OFF=<the off run>`: a checkpoint passes if it matches the reference, or differs from it exactly as `off` at
  the same clock does (`logiccheck.py compare NAME --off OFFNAME`; such a row says so). Nothing else is let through.
  The same three runs again with the app's menus (`PS2X_GAME_SETTINGS_MENUS=1`, logiccheck's menus route: the same
  input, the zgui panels at their menus-on addresses), so the gate judges what the app runs too.
- The A/B at that clock: `career_ab.py gate`, every input, `off` against the defaults, same runner. At 4096x no frame
  overruns its vsync, so what differs is logic: every state field at every tick, the scratchpad, the streams, VU0,
  RAM. RAM's exceptions are two: a word below its thread's sp in both runs (the dump's `.sp` file) is dead stack,
  counted apart; `ab_allow.txt` names the rest, each with its proof and as narrow as that proof (clock words by the
  size of the difference, a stack only while no thread holds it, a record's w by its offset). A word of a heap record
  has no address: an `owner=` line names a walk of the game's own lists (`OWNERS` in career_ab.py), run on each side at
  each dump, and a word the walk does not find is compared. A record left in dead memory after its owner went has no list:
  that rule tests the record's shape, and is named as such. A stack's frames: the `frame` lines are a table of
  functions, the chain of saved return addresses is walked through it to the stack's top, and only the slots listed count.
  DM2's and BMX XXX's `menus` input runs the app's Settings page in the game's menus (PS2X_GAME_SETTINGS_MENUS=1 on both
  sides) from Options and from the pause menu, Right then Left on every row: its keys read back and are the same on both
  sides (career_ab.py PAGE_INPUTS). A change made on that page reaches the game before the game goes on
  (ps2_settings.h, the FromGame calls), so the run repeats exactly whenever the window thread runs.
- dm2logic.

Byte match (`decomp/`, the original's compiler) is a tier above Verified for rows no route reaches, not a gate.

A value left on the stack has no gate of its own: the replay answers calls and the A/B sees only dumps and streams.
`PS2X_STACK_POISON=1` fills the stack below each readable function's sp with 0x5A: a run with it against a run without,
same runner, shows every read of a value left there on an input's path, by the function or by a callee through a pointer
into its frame. Lanes nothing reads move too (a vector's w, 0x24C9EC). `career_ab.py stale` is that compare over every
input: fields, scratchpad, GIF, VU0 and every voice call's arguments; a quad's w lane, the VIF1 stream and RAM are counted,
out of the verdict, so a stale value that reaches only VU1 or a record stays unseen. `gate --voices` first in the same
folder saves it one side. `survivors.py` lists the registers read from the caller.

What it does not see, since time is not compared: a change of time alone; the order of entry blocks where a wrong
branch has the same effects on every recorded input (`--markers`); a function interrupted between two calls; the
A/B tick for tick at the normal clock.

The two halves see different things. The replay alone sees a path no A/B input reaches and a wrong word that does
not reach a dump (drawNodeTree's, `__svfscanf`'s). The A/B alone sees data a callee reads through a pointer into its
caller's frame where the recording has no frame reads for it (above: older recordings, a callee not recorded). Neither sees a path nothing runs, or a value
that is wrong only in a state no input reaches (a dropped store of a value that is already there).

VU0 between functions: the engine's macros multiply with `.xyz` and store with SQC2, so 224 functions
store a lane they never set, mostly vf6's or vf9's w. Those lanes reach the skater's position and
velocity, the camera and the particles, and in the original the last VU0 user before them decides
them (the `-1` segmentFaceTree leaves is what keeps the skater's position w at 1). So a readable
function that does VU0 work leaves what the original leaves: on `ee::vu0::Machine` (the context's
registers), or, for a result worked out by value, through `ee::vu0::leftInVf6` / `leftInVf9`. A local
copy of the stale lane is not enough: the next function reads the context. Which lanes cross a boundary is
derived per game from its code (`fncheck_traced_gen.py` vu0_lanes, a dataflow over every function's blocks: a call
reads what its callee reads and sets what the callee sets on every path), and `--semantic` fails a lane unlike the
original's that the callee reads at a call or that is live at the return. In AI: vf6.w and vf9.w, vf2.w and vf3.w
(the sphere's w drawScene and drawNodeTree store into a NodeView), and the view's culling registers vf16 to vf27,
which loadViewCulling sets and drawScene and drawNodeTree read. In DM2: vf6.w and vf9.w. A QMFC2 that an MTC1 takes
the low word of reads x only, VSUB x, x, x (orthonormalize's vf6.w) reads nothing. To find a function that leaves vf6 or vf9 alone in the middle of a frame: `PS2X_FNCYCLES_VU0=6,9`
with every readable function traced, on both sides, and the first event where a register differs that only
one side has.

## Drafts and the sweep

- `readable_draft.py ADDR...` lifts a function from the generated code as a draft in the house style
  (signature from the registers the code reads, calls in order, EE time charged, frame as
  `ee::StackFrame` or variables, memory through `ee::at`, control flow as if/loops where blocks nest,
  names from the readable sources and Ghidra). `readable_draft_check.py` builds drafts and checks them
  against the corpus. The author's part: names, types, structure where gotos remain.
- The sweep (`readable_sweep.py`) drafts, builds, checks and registers a whole layer from manifests
  `*/sweep/manifest.txt` (status, flags, reason); its results go to the work database (`sweep_latest`,
  `sweep_failure_classes`). Flags: `dead`, `port-dead`, `script-dead`, `data-dead` (DEAD-CODE.md) and
  the failure classes in `card`. AI has none left: every function it swept is in its named file or, a
  library's, in not_ported.tsv, and the build no longer generates drafts (game.cmake has no sweep step;
  a new manifest needs that step back).
- A draft gets committed source with `readable_adopt.py` (`ee/route.h` for the calls a plain name cannot make, `readable_place.py` for its file): `move` carries it to the end of its named file's namespace and takes its row out of the manifest, `readable_pass.py run ... --only NAMES --files FILES --units` takes it through the passes (`--only` leaves other bodies alone), `routed`, `records` (a struct of the layout table no header defines, from what the functions read) and `typed` go on from there, and what is still below the bar goes `back` to generated. From then on the source is edited by hand. `hand FILE` replaces functions by name. Run it on a full staged tree and finish from a snapshot of it: a function that goes back takes the calls to it with it.
- Hand-written rounds replace drafts with readable code in order of live-play self time
  (`PS2X_FNPROF`, the coverage page): engine in `zge/`, game in `aggressive/`. A hand-written function is
  checked like a draft; its notes (purpose, cut, bug-kept) go to the database.
- Names: `readable_names.py` and its tables name the swept drafts at build time from the evidence above;
  `readable_names_count.py` measures what is left. `readable_names.tsv` holds `function`, `global`,
  `param`, `ptype` (a parameter's struct), `gtype` (a table global holding a pointer to a struct: its
  loads typed) and `record ADDRESS:ELEMENT:COUNT` rows (an array or record global no source declares:
  the addresses inside are `g_name<index><Field>`); a global's name is unique (check refuses one a
  hand-written global at another address has: the drafter's duplicate-name pass renames by text);
  `readable_names_layout.tsv` holds struct fields (`STRUCT OFFSET field`) and `base`, `name` (the
  variables of one name are that struct), `size` and `type` rows (`STRUCT type FIELD=TYPE`: a layout
  field that is an embedded struct, so `kEditParkCellSize + offsetof(zge::Vec3, y)`); an offset
  inside an embedded member with no member of its own there (a matrix's words) is written as the
  member plus the rest. Every row has a confidence and its
  evidence; low ones are applied and marked in the draft. A local is a web (the drafter's split of a
  register's lifetimes) named from all its definitions (a call's result, a global, a field or stack
  slot loaded, a copy), then its uses (a field stored to, a callee's named parameter), then its role
  (loop index, count, walk over a named value or list, the result, its type's name), last what its
  expression says over named variables (`flagsBit4`, `speedBelowLimit`, `heightMinusOffset`,
  `guiScrollViewByte` for a load no field names yet: a description, replaced when the field gets its
  name). A parameter nothing names takes its use (`callback`, `flags`, `limit`, `index`, `enabled`);
  a label what follows it (`done`, `setHeight`, `join`). Types reach a
  variable from `ee::Ptr<T>`, `ptype`, `rtype` (a function's result) and `vtype` (a local, by its
  draft's name) rows, a typed callee parameter or result, a typed field (a layout `type` row's
  `FIELD=TYPE*` for a pointer) or global (every definition the same type), a copy, a list walk, what
  is returned or stored to a pointer field, an array element by stride, or its name. A local with a
  role's name takes its struct's. Accesses through a register holding a page's high half are written
  at their address and named. New structs come from `readable_names_recover.py <sweep> out.json`
  (classes of pointers across functions; `--cards out.json ID...` prints one) and
  `readable_names_specs.py out.json spec...` (a struct's fields and classes into rows). A class is a
  lead: read its functions before naming it; binding by a hand-written label alone measured about one
  in four wrong; a `vtype` row is keyed by the local's first name (draft with `NAMES_DUMP_RENAMES=FILE`, then
  `readable_names_rekey.py FILE`), so later naming rules do not orphan it; the EE's hardware registers
  are constants (`kD4Chcr`) from the module's table; a C string's address has a `g_text<Words>` global
  row (its words, so `nameHash(kTextChest)` is `chestHash`); a float kept in a register is named for
  its value (`half`, `float0_9f`); infer types a parameter handed on unchanged to a callee
  parameter whose type its own code or an explicit row gives (worked to a fixed point each run). `readable_names.py infer <sweep>` rewrites the rows it owns (`inferred:`, `callers:`, `own:`,
  `uses:`) from a set of drafts: a name moves one call level a run, so redraft and rerun it until no
  row's name changes (three or four runs from a large change), with drafts made under `NAMES_NO_ROLES=1` (an expression's name is
  no parameter's), then `readable_names.py check`. `python3 -I readable_names.py sync-hand` after a merge gives
  function, global and parameter rows of hand-written code the sources' names; a name another
  function has is a CLASH line and exit 1: names are unique across layers (`ee::readable` and
  `PS2X_READABLE` find functions by name), so rename one of the two where it is written. A layout `result` row (`STRUCT result CALLEE:N=VALUE`) types
  and names a call's variable by an argument (the park editor's `editObjectPart(object,
  "EditRailTool")`: an object's part per class). A float read from .lit4 is written as its literal:
  the build knows the words the manifests' `elf` column pins (`readable_names.py pin-literals
  <sweep>` once a sweep adds functions); `RUNTIME_WRITTEN` keeps the words the runtime writes (the
  DM2 host's split tolerances) as reads. Param rows key a declared parameter by its number
  (`ADDRESS:2`) and one no signature declares by the drafter's register name (`ADDRESS:arg1`).

## Other builds and assets

- Other games on the engine: readable functions are keyed by name with addresses per game (DM2, BMX XXX);
  the three PS2 builds, three Xbox and three GameCube builds are paired in the work database (`card`
  counterparts, confidence high and medium dependable, low a lead; MATCH3.md in `tooling/dm2`). A game's instance of the shared sources is "Symbols" above;
  DM2's corpus and checks are "How a function is proven" item 5.
- BMX XXX (SLUS-20415) is game 2 of the collection runner (`game/bmx`, `game_overrides/bmx_xxx.cpp`): its `generated-ns` (27,112 files from the match3 Ghidra map, namespace `bmx`, made by `tooling/recompile.sh --bmx` (`tooling/bmx/recomp/recompile.py`) into paths.env's `BMX_RECOMP`) in `PS2X_RUNNER_EXTRA_GENERATED_DIRS`, its instance of the shared sources with `PS2X_BIND_DIR` (`runtime/tools/bind/bmx.tsv`), its differences as `kIsBmx` / `perGame(ai, dm2, bmx)` and `PS2X_GAME_BMX` layouts. What runs readable is its status file's Verified rows; it is played by `tools/playbot/bmx.py` and ships in the collection app (`GAMES="ai dm2 bmx"` make-app.sh).
- Native assets: texture libraries of both releases, GEO meshes and collision parts are made on the host from
  the files at build time (`zge/native`); Xbox textures are the HD default. See `readable-native-assets.md`.
- Suspendable calls (`readable-suspendable.md`): `READABLE_FUNCTION_WAITS` runs a function on a host fiber
  (a register-only switch on arm64 and x86-64, ucontext elsewhere, the Fibers API on Windows). A readable
  callee of a guest call runs on its caller's fiber, with the same steps and states: only a generated callee
  leaves it, so a level load makes about 100,000 fibers for 3.5 million calls.

## Tools and where each lives

| Tool | Where | What |
|---|---|---|
| `ps2xFnCheck` | z-axis build dir | the differential checker (above) |
| `logiccheck.py`, `lc-when-free.sh` | `tools/` | 21 checkpoints against the PCSX2 reference run; `compare NAME` |
| `careercheck.py` | `tools/` | the career input for the A/B runs |
| `run-game.sh NAME SECS [RUNNER]` | `tools/` | a scratch-HOME unpaced run; `STOP_TICK=<vsync>` |
| `routes.py`, `nctl.py`, `sweep.py` | `tools/` | closed-loop routes over `PS2X_CONTROL` (DEVTOOLS.md) |
| `diffcall.py`, `diffcall_search.py` | `tooling/checks/` | generated calls from real game states, a coverage-guided search over them, for what no recording reaches (DEVTOOLS.md): evidence below Tested, never Verified |
| `readable_lint.py`; `readable_place.py`, `readable_adopt.py` | `runtime/tools/`; `tooling/checks/` | the user's bar per function, the ratchet in the build, reported apart from Verified; every swept row's named file; a draft into committed source |
| `readable_corpus.py`, `fncheck_corpus.py` | `tooling/checks/` | record the corpus; replay it with verdicts and coverage |
| `readable_symbols.py` | `runtime/tools/` | names for addresses: `generate`, the tables, the headers, `convert`, `comments`, `rename`, `layouts`, `lint`, `census` ("Symbols") |
| `readable_sweep.py` | `tooling/checks/` | draft, build, check, register, promote, demote, manifests |
| `readable_draft.py`; `readable_draft_check.py` | `runtime/tools/`; `tooling/checks/` | drafts and their check |
| `readable_digest.py`, `fncheck_traced_gen.py` | `runtime/tools/` | source digests; traced originals, block tables, frame table |
| `readable_coverage.py`; `readable_names_count.py` | `runtime/tools/`; `tooling/checks/` | counts per area; symbols left |
| `workdb.py` | `tooling/dm2/tools/` | the database: `build`, `card`, `notes`, `query`, `emit`, `check-results` |
| `dead_code.py` | `tools/` | the dead-flagged functions by group (DEAD-CODE.md) |

Runtime knobs for readable work: `PS2X_READABLE`, `PS2X_FNREC*`, `PS2X_FNPROF` (`_FROM`, `_FOR`),
`PS2X_FNCYCLES` (`_FROM`, `_ARGS`), `PS2X_TRACE_EE`, `PS2X_SAFEPOINT_TRACE`, `PS2X_GUEST_CALL`,
`PS2X_UNPACED`. Test-run helpers (`PS2X_DEV_UNLOCK_ALL`, `PS2X_DEV_START_LEVEL`, routes) are in DEVTOOLS.md.

## What to read for what

A function: `workdb.py card ADDR`, `workdb.py notes ADDR`: the facts of every round about it are notes in the work
database (prefer `notes` to grep).
