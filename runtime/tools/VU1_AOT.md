# Ahead-of-time compiled VU1 microprograms and the graphics workers

## Graphics workers (PS2X_VU1_ASYNC)

The runner processes VIF1 streams, VU1 microprograms, the GIF arbiter and
PATH3 in order on a VIF1 worker thread, and the GS packets they produce in
order on a GS worker thread (`ps2_async_stage.h`, `PS2Memory::setAsyncGraphics`).
The EE sees the same state as with synchronous processing:

- DMA completion is reported at kick time, as before.
- Interrupt-bit VIFcodes are found on the EE by a framing-only scan of each
  VIF1 stream (`PS2Memory::scanVif1Stream`): the stall point, INTC 5 and the
  busy VIF1 DMA are decided at kick time; the worker gets the stream up to
  the stall, and FBRST.STC queues the rest.
- The same scan applies the register effects of every VIFcode to an EE-side
  shadow, so VIF1 register and GIF_STAT reads do not wait for the worker.
  `PS2X_VIF1_SHADOW_CHECK=1` drains the worker on every read and reports
  where the shadow differs from the registers.
- Everything else the workers change is behind a drain: VU1 memory, GS
  privileged registers, the GS HLE calls (`syncCoreSubsystems`), native GIF
  chain uploads, other VIF1/GIF register writes. `PS2X_PERF_LOG` prints how
  often the EE had to wait, per reason.
- VU1 D/T stops write the EE context's VPU-STAT: with FBRST.DE/TE set the
  VIF1 work runs synchronously.

`PS2X_VU1_ASYNC=0` keeps everything on the game thread.

## Ahead-of-time compiled microprograms

The generator (`runtime/ps2_vu1_aot_generator.h`; `ps2xVu1Aot` is its
command line) decodes a VU1 micro memory image with the interpreter
(including its static hazard analysis) and writes C++: the decoded pairs as
constants and, per chunk of 128 pairs, a function that runs the interpreter's
own step (`stepPair`, `src/lib/vu/ps2_vu1_step.h`) on them, with a goto per
pair for sequential flow and a switch on pc for everything else. With the
pairs constant the compiler folds the decoding and dispatch away; the
semantics are the interpreter's code. The generated code registers itself
under the FNV-1a hash of the 16 KiB micro memory and runs whenever micro
memory matches; any other image, or a pc it has no code for, runs on the
interpreter.

The step works on VU1's core (`VU1Core`, `runtime/ps2_vu1_core.h`): the
state and pipelines, plain arrays and freestanding, with what it asks of its
runner (PATH1 packets, the pair trace, reserved instructions) through
`VU1Host`. `VU1Interpreter` is that core with the decoder, the run loop and
the registry; the generated code and the core's out-of-line pipelines
(`src/lib/vu/ps2_vu1_core.cpp`) are written against the core alone, so the
same sources build into the runner and into a game module
(`runtime/game_module.h`), which lists its microprograms
(`ZxGameModule::vu1Programs`, `ps2_vu1_aot_register.h`); the runtime
registers them when it loads the module, if the module's core has the
runtime's layout (`vu1CoreLayout`).

The app makes them on the player's machine (`collection/game_code.h`): the
recompiler writes the image the game's renderer uploads (its staged config's
`[vu1]` table names the DMA chain, `tools/vu1_image.py`; `<hash>.vu1code`
beside the generated code), the app's own generator writes the code, and the
module carries it. `ps2xVu1Check --module <path>` checks a module's code
against the corpora.

For a build from source, `tooling/recompile.sh` writes each game's image into its workspace's
`recomp/generated-vu1`, and a build given that folder (`PS2X_VU1_AOT_DIR`, more games in
`PS2X_VU1_AOT_EXTRA_DIRS`) makes the code from the images with its own `ps2xVu1Aot` into the build
dir (`vu1-aot/<key>`, keyed by the generator's sources and the images; a folder that already holds
code with that key, `vu1_aot.key`, is built as it is). A cross build that cannot run its own
generator builds one for the build machine (`tools/vu1_aot_host`) or takes `PS2X_VU1_AOT_TOOL`.
By hand:

```sh
# the micro memory images a run meets (one file per image; ps2xVu1Aot accepts corpora, *.vu1r, too)
PS2X_VU1_CODE_DUMP=$PWD/recomp/generated-vu1 recomp/run.sh 60 vu1codes
# generate (the runner logs "[vu1] micro memory <hash>: ahead-of-time code")
ps2xVu1Aot OUT_DIR recomp/generated-vu1/*.vu1code
cmake ... -DPS2X_VU1_AOT_DIR=$PWD/recomp/generated-vu1
```

### Blocks

On top of the per-pair code, every basic block (from a branch target or the
pair after a delay slot to the next branch's delay slot, an E bit's delay
slot or the next block) also gets straight-line code (`b<pc>:` labels,
`VU1AotGenerator::emitBlock`). It runs the same instructions
(`execPairOps`) but keeps the issue cycle of each pair in locals:

- Stalls are worked out from the in-block writers of what a pair reads
  (the generator folds the ones that are known statically); only the first
  pairs read the interpreter's ready arrays for writers before the block,
  and FDIV/EFU/XGKICK waits are computed at run time as in the step.
- Time passing (commits of Q/P, deferred writes) is only processed when a
  commit is due (`aotNextEvent`); every other cycle costs nothing.
- At the block's end the writes still in flight are marked in the ready
  arrays, so the per-pair code (and the next block) see exactly what the
  step would have left.
- A block runs only when nothing it cannot handle can happen inside it
  (`aotBlockEntryOk`): the cycle budget cannot end in it (the entry margin
  covers 4 cycles per pair plus the longest FDIV/EFU wait per waiting
  pair), no pair trace, no deferred writes, no branch or halt pending.
  Otherwise, and when an XGKICK wait would reach the end of the budget,
  the per-pair code takes over at that pair (`aotBlockBail`).
- FMAC results that nothing can observe on their own (no MAC/status flag
  instruction, XGKICK wait or block end within 4 pairs) do not queue a flag
  result: their sticky bits are ORed into a local that the next observable
  FMAC result takes over (`fmacSpecializedDefer`, `aotFoldSticky`).
- Lower instructions see VU1's 16 KiB data memory as a constant.
- A lone DIV/SQRT/RSQRT result commits inline when its cycle passes
  (`aotSyncTo`), without the general commit loop.

`PS2X_VU1_AOT_BLOCKS=0` runs the per-pair code only (generated code
unchanged). Blocks need the matching runtime: regenerate the AOT sources
with the `ps2xVu1Aot` of the same tree.

Other parts of the interpreter that the blocks rely on:

- PATH1 runs lazily (`syncXgkick`): it only reads VU data memory, so it is
  caught up in one step before a store and before anything reads its
  state, and waits for it step from GIF tag end to tag end.
- MAC/status and clip flag results live in two 16-byte-entry rings; a batch
  of ready FMAC results is applied as its last one (each entry carries the
  OR of the sticky bits queued since the ring was empty).

One difference from stepping every cycle remains, in error cases only: a
malformed PATH1 packet (bad GIF tag, packet over 64 KiB) is reported when
PATH1 is caught up rather than on its exact cycle, and inside a block the
run may then stop a few pairs later. Valid programs are not affected.

Speed (split-screen corpus `vu1-mp-split-60.vu1r`, 60 vsyncs of the
heaviest scene, 134k runs, 179M VU cycles; `ps2xVu1Check --time`):

| | Ryzen 9 5900X (clang-cl) | M5 Pro (Apple clang) |
|---|---|---|
| per-pair code only | 2953 ms | 779 ms |
| blocks, as now | 650 ms | about 340 ms |

What the blocks' time came from, largest first: the blocks themselves with lazy PATH1, then the flag
rings with the SSE4.1 FMAC path and a constant data size, batched flag results with PATH1 bulk copies,
deferred flags and tag-end waits, inline `max` (MSVC's `std::max({...})` is a call), the MXCSR
rounding switch with chunks split outside loops and inline Q commits, sticky bits folded into the
last FMAC with a quick block entry, and SSE4.1 MAX/MINI, ITOF and FTOI.

Chunks are about 128 pairs, split where the least (short, likely inner)
loops cross (`PS2X_VU1_AOT_FIXED_CHUNKS=1`: every 128 pairs), so a hot
loop does not leave its chunk function on every iteration. Blocks jump to
each other through `e<pc>` labels, which only check the budget and a stop.

`PS2X_VU1_AOT=0` disables the AOT code at run time. Check it like the
interpreter: `ps2xVu1Check` uses the AOT code when it is built in (it logs
which), and `PS2X_VU1_AOT=0 ps2xVu1Check ...` checks the interpreter; both
must match every corpus bit for bit.

Keep the per-pair code small. The step is inlined into every pair (about
1 KB of machine code each), and in Aggressive Inline's split-screen
multiplayer (about 2,500 runs and 3.3M VU cycles per vsync) the hot pairs
are far more than the instruction cache holds, so code size decides speed:
rare parts of the step (pair trace, write capture for a budget stop, program
end, taken-branch countdown, FMAC zero lanes) are out-of-line calls behind
`[[unlikely]]` checks, and inlining PATH1's credit step made it slower.
Measure changes on a large corpus (`ps2xVu1Check --time --repeat 3`,
interleaving the old and new binaries; the machine is shared and noisy).

The generated sources are compiled with `-ffp-contract=off -frounding-math`
(the VU runs in round-toward-zero mode, which constant folding must not
assume away).
