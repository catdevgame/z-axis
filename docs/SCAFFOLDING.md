# Temporary scaffolding

Forms the readable source uses for now, because the full picture is not in yet: a callee still in its generated
form, a value whose source is not named, a frame that must stay where the original kept it while the checkers compare
word by word. Each is correct today (the gates prove it) and each is meant to go. This file is the list: what each
form is, why it is there, and what removes it.

Rules:

- A new temporary form gets an entry here before it merges, and a `// Scaffolding (NAME): ...` comment where it is
  defined (the helper, the header, the wrapper), so `grep -rn "Scaffolding (" runtime/src` finds every
  definition. A use of an existing form needs no comment of its own: the entry's `find` pattern counts it.
- When an entry's count reaches 0, delete the form and its entry in the same change.
- `python3 -I tooling/checks/scaffolding.py` counts every entry by folder from the `find` lines below
  (`--json` for a machine-readable copy). It reads this file; an entry without a `find` line is counted by hand.

Not here: what is permanent. The profiler's clock words a gate counts apart (they are COP0 COUNT and differ by
design), the dead-code manifests (docs/DEAD-CODE.md), `ee::FunctionPtr` callback tables and `ee::Global` records
(the game's own pointers and globals, typed).

## t0-leftover

The engine's scale macro brings its factor into VU0 through t0, so t0's upper half (what the last 128-bit move
through it left) lands in VU0's work register, where later vector code reads it. Where no vector can be named for it,
`zge::scaleLeftover(zge::LeftBy::caller | zge::LeftBy::lastCall)` (zge/math/scale_leftover.h, pinned, framework to the
lint) reads it as the original's code path leaves it. The user's decision ("name it in the engine", t0carry1).

- Why for now: DM2's only 128-bit writers of t0 are library code (sceVu0CopyMatrix's third row, the string routines,
  the IPU copies) reached through callbacks; until those are readable the leftover has no name as data.
- Removed by: those library bodies readable, so each site passes the vector it came from (AI's `upperHalf`), or a
  proof per site that nothing reads the lanes.
- find: `zge::scaleLeftover\(`

## vu0-argument-upper-half

BMX XXX's angleSin and angleCos hand VU0 the angle by QMTC2, which moves all 128 bits of a0, so the microprogram
runs its z and w lanes on a0's upper half (whatever the last 128-bit move through it left) and later vector code
stores those lanes into game memory. `ee::vu0::firstArgumentUpperHalf()` (ee/vu0.h) reads it as the caller left it,
for zge/math/angle.cpp's VU0 helper.

- Why for now: the upper half is no value the game names (string bytes, pointers, 0 in the recordings).
- Removed by: a proof that nothing reads the program's z and w lanes after an angle call, or each caller readable
  with the vector its a0 came from.
- find: `firstArgumentUpperHalf\(`

## guest-call-kept

A call that must stay a call into the generated code (`ee::game(sym::...)` in a small typed wrapper such as
dm2/rider/actions/rails.cpp `negateByCall`, `rayHitsByCall`, `edgeEndByCall`), because a `LeftBy::lastCall` read after
it needs the registers that call leaves.

- Why for now: a plain C++ call leaves no guest registers behind.
- Removed by: t0-leftover gone at that site.
- find: `[a-z][A-Za-z]*ByCall\(`

## register-read

A body reading a register as the caller or a callee left it, written directly (`ee::callerRegisterHigh`,
`ee::leftByCallHigh`, `ee::leftByCall`, `ee::callerRegister`). Readable functions may not have these (the lint fails
them); the remaining ones are in functions still being converted.

- Removed by: the function's conversion (a named form or the value's real source).
- find: `ee::(callerRegisterHigh|leftByCallHigh|leftByCall|callerRegister)\b`

## left-by-caller

`ee::LeftByCaller<T>` (ee/plain.h): a value a caller leaves in a callee-saved register that the original reads
without setting (AI's initZgui and finishUiImageLoad: the user's "checker supplies it", leftreg1). The replay checker
supplies the word from the recording (tools/readable_entry_reads.tsv).

- Why for now: the original reads a register its callers set for their own reasons.
- Removed by: each caller readable, so the value is passed as an argument.
- find: `ee::LeftByCaller<`

## address-call

A call into a function by address (`ee::call`, `ee::game(sym::...)` with any result type, `ee::game<ee::Ptr<T>>`
too, and the form a `GAME_TABLE_ROUTE` names; `sym::` callees through typed declarations) because the callee has no
readable body in that game yet: DM2's and BMX XXX's level-1 engine functions (their code differs from AI's, AI's
signature unproven there), their own game functions not yet written, and calls whose result registers the caller
reads (a void or float callee's v0).

- Removed by: the callee readable in that game (src2plan D4, D5, D8; B4, B5, B7), then a plain call.
- find: `\bee::(call|game)\b(?=\s*[<(,])`

## upper-half-argument

An argument that carries a register's upper half (`withHigh(...)`, `ee::Qword`) because the callee reads all 128 bits
of an integer register.

- Removed by: the callee readable, taking a vector or a named 128-bit value.
- find: `withHigh\(`

## frame-exact

Locals kept at the original's stack-frame offsets (`ee::Local<T>` with rows in tools/readable_locals*.tsv,
`StackFrame` sizes equal to the frame table's), and frame stores kept that nothing reads afterwards, so RAM compares
word by word against the original (the A/B and the gates).

- Why for now: the gates compare every RAM word, stack included; a leftover stack value can carry into later code
  (stale-stack reads the replay cannot see).
- Removed by: the stale-stack reads proven absent for that function's callers, after which locals are plain C++.
  Keep while a gate compares that function's frames word by word.
- find: `\bStackFrame\b`

## unnamed-field

Fields and values the code uses whose meaning is not settled yet: `_NNN` padding-named fields, raw offsets read through
a helper (BMX XXX's and DM2's riderField are gone), and flags named by their value (`kFlag8000000`). The find counts
the last two; the rest carry a `// Scaffolding (unnamed-field)` comment where they are defined.

- Removed by: a name from evidence (the game's code and strings, the other builds, the lineage); never a guess.
- find: `riderField<|\bkFlag[0-9A-F]+\b`

## call-leftover-chain (framework, no code yet)

In game mode `ee::call` restores the whole context after a guest call, so a guest callee never sees the previous guest
call's caller-saved temporaries (t0's upper half among them), where the PS2 passes them on. DM2's bailMove is the one
known function that depends on the chain; its gate passes, so no word it reaches differs today.

- Removed by: ee::callGuest putting the callee's caller-saved temporaries back from `t_left` after `restoreSaves` (at
  least t0's 128 bits), if a gate ever shows the chain matters; or the chain's functions all readable.
- Counted by hand: 1 known site (dm2/rider bailMove).

## held-place-as

`ee::held::placeAs(game, target, standsFor)` (ee/held.h): a call a host form makes where the original made another is
placed by the held-register table (ee/held.cpp: the callee-saved registers the original holds at each of its calls) as
that other call. render_path = records (zge/host/class_draws.cpp): the strict encoder without its VU1 work begins the
draw's DMA tag (`sce::vif1PacketCnt`) where the original calls the part's packet (`sce::vif1PacketCall`); without the
placing the table loses its place there, and the class renderer's next callee with a frame (requestTextures) saves
entry values into stack the game copies later (BMX XXX's health bar shards' texture coordinates, s-muly2's heap word
of nativedef1). A runtime registration: the generated held tables are not changed by it.

- Why for now: the gates compare every RAM word with the held table on, and the records path shares the readable
  encoder, whose calls the table follows through the original's sites.
- Removed by: the held table no longer needed for the class renderers (their callees' frames proven unread, frame-exact
  gone there), or the records path's draw tag made without a guest call the original does not make.
- find: `held::placeAs\(`

## misnamed-row

DM2's own symbol row named `matrixMultiply` (DM2 0x1128D8) is a point transform, a different function from AI's
matrixMultiply (DM2 0x128660). Callers use it by address under that row's name. DM2's own row `rowsFromMatrix`
(DM2 0x1285B0) is ZGE2's matrix copy (out, m: the rotation's and the translation's x, y, z), which ZGE3 inlines;
zge/collision's copyBodyMatrix calls it under that name. The engine's row `pathFirstPoint` reads a record's word
+0x0C, which nothing calls in AI; BMX XXX's camera objects call it on a follower, whose +0x0C is its end mode (the
function sits just before setFollowerEndMode): game/bmx/level/camera_objects.cpp's followerEndMode calls it by address.

- Removed by: the row renamed from evidence in readable_symbols.tsv and its callers updated.
- Counted by hand: 3 rows.
