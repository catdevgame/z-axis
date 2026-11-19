# Aggressive Inline (USA, SLUS_203.27) matching decomp

Groundwork for a matching decompilation of the PS2 game *Aggressive Inline*
(Z-Axis, ZGE3 engine, May 2002). Retail ELF: `SLUS_203.27`,
SHA1 `af35b83f0d773e613c91f21fa1e00a45efbbfec7`.

Status:
- splat splits the ELF into asm and data. The build reassembles and links it, then
  reproduces **both the flat load image and the full retail ELF byte-for-byte**
  (both SHA1s are checked by `ninja`).
- The original compiler and flags are identified. 13 of 14 test functions match
  exactly. Four of them are already compiled from C inside the real build
  (`src/cod/`) and the ELF still hashes to the retail SHA1.

## Toolchain findings

| Item | Finding | Evidence |
|---|---|---|
| Compiler (codegen) | **Sony ee-gcc 2.9-ee-991111** (cc1). The `991111`, `991111-01` and Windows `991111b/r4` builds give identical code. | 13/14 exact matches (table below). `990721` is ruled out by func_00116C98. SN ProDG 2.95.x builds save `$ra` with 128-bit `sq`/`lq` (2.95.3 v1.36 excepted) where retail uses `sd`/`ld`. 2.96 and 3.x schedule differently. |
| Flags | **`-O2 -G16 -fno-strict-aliasing`** | `-O1` loses delay-slot/branch-likely filling. `-G16` is required: 16-byte objects in `.sbss` are gp-relative and `.sdata` holds string literals up to 16 bytes. Without `-fno-strict-aliasing`, func_00116C98 keeps a pointer in a register that retail reloads every loop iteration. |
| Assembler | **GNU as 2.9-ee-991111** (Sony SDK `ee-as`, the Linux build in the decomp.me `ee-gcc2.9-991111` package) | Only this `as` inserts the `mtc1` to FPU-use hazard `nop` seen in retail (func_001C8F90), and it doesn't add EE short-loop nops. The `as.exe` (2.9-ee-991111b) in the Windows/SN packages omits the hazard nop, SN's `ps2eeas` differs more, and modern binutils expands `move` to `or` (retail uses `daddu`). Float constants are `li.s` expansions (`lui $at` + `mtc1`, or `.lit4` loads), so the source was assembled with GNU as. |
| Linker | Sony dvp-aware GNU ld (`ee-ld`) | `.DVP.overlay..*`, `.DVP.ovlytab`, `.DVP.ovlystrtab` sections and the `.vutext`/`.vudata`/`.vubss` markers are written by Cygnus/Sony dvp binutils. Single PT_LOAD at file offset 0x1000. `e_flags` 0x20924001 = mips3, MACH_5900, EABI64, noreorder. |
| Sony SDK (libsce) | **Runtime Library 2.4.3** (`SCE_LIBRARY_VERSION` 0x2430) | The game prints `PS2 LIBRARY VERSION:%x` with 0x2430 (call at 0x0011770C). libsce module tags in `.data`: `PsIIlibkernl2430`, `PsIIlibmc   2430`, `PsIIlibcdvd 2420`, `PsIIlibmpeg 2420`, `PsIIlibpad  2410`, `PsIIlibgraph2400`, `PsIIlibdma  2400`, `PsIIlibipu  2400`, `PsIIlibpkt  2400`, `PsIIlibpc   2400`, `PsIIlibsdr  2400`. The disc's IRX modules are dated 2001-11-28 (2.4 base) and 2002-03-23 (2.4.x updates). |
| ZGE engine | 01.02.49 | `ZGE LIBRARY  VERSION:%02x.%02x.%02x` printed with 1, 2, 0x49 |

How the version strings were read (`tools/xref.py`): the function at 0x001176C8
(engine startup) calls the printf-like `func_00148798` three times:

```
001176EC  lui a0,0x26 ; addiu a1,zero,2 ; addiu a0,a0,0x2528 ; jal func_00148798 ; addiu a2,zero,0x5F
          -> "GNU COMPILER VERSION:%d.%d", 2, 95
00117708  lui a0,0x26 ; addiu a1,zero,0x2430 ; jal func_00148798 ; addiu a0,a0,0x2548
          -> "PS2 LIBRARY  VERSION:%x", 0x2430
00117720  ... a1=1, a2=2, a3=0x49  -> "ZGE LIBRARY  VERSION:%02x.%02x.%02x"
```

**Open question: why "2.95"?** All Sony 2.9-ee drivers define `__GNUC_MINOR__=9`
(2.96 defines 96), so the string points at a driver that reports 2.95. The SN
ProDG `ee-gcc.exe` 2.95.x drivers do: they identify themselves as
"gcc driver version 2.9-ee-991111b/r4 executing gcc version 2.95.2-EE". The
generated code, though, is unambiguously Sony's 2.9-ee-991111 cc1, not any SN cc1
on decomp.me. The most likely explanation is a ProDG-style driver or specs
(defining `-D__GNUC_MINOR__=95`) in front of Sony's 2.9-ee-991111 cc1 and
`ee-as`, but this is unconfirmed. Nothing in the matching pipeline depends on it.

## Address map (approximate)

| Range | Contents |
|---|---|
| 0x00100000-0x00100100 | crt0 (`_start` at 0x00100008, handwritten) |
| ~0x00100100-0x00171000 | ZGE3 engine (`../ngengine/ZGE3/...`: zobj, zfile, zdebug, zprofile, ps2Movie, Ps2MemCard, ps2Chan, ZgeMods/...) |
| ~0x00171000-0x00202800 | game code (`./src/com/inlineFx.c`, `inlineTextureFX.c`, ...), then the park editor (`../GameTools/ParkEditor/*`) ending near 0x00202800 |
| ~0x00202800-0x0022DB34 | libraries: libm (`acosf`), libgraph (from 0x00204E88), libmpeg/libipu, libdma, libpad, libcdvd, libkernl, libmc, libsdr, libc (`printf` tables near 0x00225000) |
| 0x0022DB40-0x002318B0 | `.vutext`: VU1 microcode (kept as binary) |

## Repository layout

```
SLUS_203.27.yaml      splat config (segments; C subsegments for decompiled functions)
configure.py          runs splat, writes build/link.ld and build.ninja
include/              splat macros (macro.inc, include_asm.h) + common.h
src/cod/*.c           decompiled, matching C
matchtest/*.c         feasibility functions (compiled stand-alone by tools/matchtest.py)
tools/setup.sh        fetches the toolchain into tools/ (gitignored) and creates .venv
tools/cc.sh           C -> .o with Sony ee-gcc 2.9-ee-991111 (cc1 + as) in a linux/386 container
tools/elf_pack.py     rebuilds the retail ELF wrapper (ee-ld layout, DVP overlay tables) around our link
tools/matchtest.py    compiler x flags matrix, masked byte compare against retail
tools/asmdiff.py      side-by-side retail vs compiled disassembly
tools/xref.py         find a string and the code that references it
tools/funcs.py        function list (address, size, call count, source-path strings)
tools/showfunc.sh     a function's split asm
tools/check_sha1.sh   SHA1 check used by ninja
```

Never committed (see `.gitignore`): the ELF, the `.rom`, `asm/`, `assets/`, `build/`,
`tools/compilers/`, `tools/binutils/`, `tools/wibo`, `.venv/`, `logs/`.

## Setup (macOS arm64)

Requirements: `uv`, `ninja`, Docker Desktop (already installed here and used only to
run the 32-bit Linux compiler binaries in a `linux/386` `debian:bookworm-slim`
container of about 120 MB), Rosetta 2 (for wibo).

```sh
cd tooling/ai/decomp
ln -s ../extracted/SLUS_203.27 SLUS_203.27   # the retail ELF (not committed)
tools/setup.sh            # or: tools/setup.sh --all  (also the comparison compilers)
```

What it installs (about 150 MB in `tools/` plus the 120 MB image):
- `.venv`: splat64 0.50.1, spimdisasm 1.42.4, rabbitizer 1.16.2 (R5900 mode is selected by splat's `platform: ps2`)
- decompals `binutils-mips-ps2-decompals` v0.10 (macOS arm64): as/ld/objcopy for the split asm and the link
- decomp.me compiler packages: `ee-gcc2.9-991111` (Linux i386, the matching toolchain) and `ee-gcc2.9-991111b-r4` (Windows, same cc1 codegen)
- `wibo` 1.2.0 macOS build: runs the Windows compiler builds natively. The harness uses it for the SN/991111b packages. The final pipeline doesn't need it, because the matching `as` exists only as a Linux binary.

## Build and verify

```sh
.venv/bin/python configure.py   # runs splat (writes asm/, assets/), build/link.ld, build.ninja
ninja                           # assemble, compile C, link, objcopy, elf_pack, check both SHA1s
```

Expected tail:
```
build/SLUS_203.27.rom: OK (f46a8b71b6a481428d34e6f4f64e3bb283ce336e)
build/SLUS_203.27: OK (af35b83f0d773e613c91f21fa1e00a45efbbfec7)
```

- `build/SLUS_203.27.rom` is the flat load image (vaddr 0x00100000-0x00279158,
  identical to `objcopy -O binary` of the retail ELF). It is what really proves the
  code and data match.
- `build/SLUS_203.27` is the full retail-format ELF. `tools/elf_pack.py` takes
  every loadable section from our linked ELF and regenerates the ee-ld wrapper:
  the header, the PT_LOAD at 0x1000, `.reginfo` (gp 0x0027A8F0), the empty
  `.vudata`/`.vubss`, the eight zero-filled `.DVP.overlay` placeholders, and the
  `.DVP.ovlytab`/`.DVP.ovlystrtab` and `.shstrtab` tables. Its constants are linker
  metadata only, not game bytes.
- Our linker script pins the retail output-section addresses (`.text` 0x00100000,
  `.vutext` 0x0022DB40, `.data` 0x00231900, `.rodata` 0x00261E80, `.lit4`
  0x00272900, `.sdata` 0x00273280, `.sbss` 0x00279180, `.bss` 0x00279980) because
  ee-ld put each section on a 0x80 boundary. splat's own `SLUS_203.27.ld` is used
  only for the object order.

## Decompiling a function

1. Find it: `tools/showfunc.sh func_XXXXXXXX` (asm) and `.venv/bin/python tools/funcs.py > logs/funcs.tsv`.
2. Write `matchtest/func_XXXXXXXX.c` and iterate:
   ```sh
   .venv/bin/python tools/matchtest.py --cc ee-gcc2.9-991111 --flags "-O2 -G16 -fno-strict-aliasing" matchtest/func_XXXXXXXX.c
   .venv/bin/python tools/asmdiff.py "build/matchtest/func_XXXXXXXX.ee-gcc2.9-991111.-O2-G16-fno-strict-aliasing.o"
   ```
   The compare masks relocated fields (HI16/LO16/GPREL16/26-bit), so symbol
   addresses don't matter.
3. Move it into the build: add a `c` subsegment at the function's ROM offset
   (vaddr - 0x00100000) followed by an `asm` subsegment at the next function in
   `SLUS_203.27.yaml`, put the C in `src/cod/<name>.c`, then
   `.venv/bin/python configure.py && ninja`. Old `as` emits `.text` unpadded with
   8-byte alignment, so single functions can be carved out without knowing the
   original file boundaries. Data and string literals still live in the data
   splits, so for now carve only functions whose literals are not
   duplicated. Real translation-unit boundaries need recovering before data can be
   migrated.

Matching quirks found so far:
- `switch` on an **enum** selector: gcc 2.95 balances the case tree with a
  character-cost table for non-enum selectors (with `int`, case 10 `'\n'` becomes
  the root). Enums also compare unsigned (`sltiu`). See func_00134018.
- **128-bit copies through `$t0`** (`lq $t0` / `sq $t0`) come from inline asm, as do
  the VU0 macro-mode sequences (`lqc2`, `vmul`, `qmfc2.ni $t0`) used by about 370
  engine/game functions. See func_001C8F90.
- Unrotated loops (`beq` exit at the top, `b` at the bottom) come from
  `while (1) { if (...) return ...; }`, not from `for`/`while (cond)`.
- Small objects of 16 bytes or less are gp-relative (`-G16`). Objects reached with
  `lui`/`addiu` are larger than 16 bytes (or are string literals); declare them with
  a size over 16.

## Feasibility results

14 functions (7 leaf, 7 non-leaf; engine and game regions). The table shows the
number of differing instruction words (relocation fields masked), with OK meaning
an exact match. Full logs: `logs/matchtest_final.tsv`. Selected rows:

| compiler | flags | exact | 116C98 | 134018 | 147E40 | 148BD0 | 14D310 | 14ED88 | 16EFF8 | 18C588 | 1A9E80 | 1C8F90 | 1E5488 | 1EDF50 | 1F9910 | 1FC098 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ee-gcc2.9-991111 | -O2 -G16 -fno-strict-aliasing | 13/14 | 5 | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK |
| ee-gcc2.9-991111 | -O2 -G16 | 13/14 | 18 | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK |
| ee-gcc2.9-990721 | -O2 -G16 -fno-strict-aliasing | 13/14 | 9 | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK | OK |
| ee-gcc2.9-991111 | -O2 -G8 | 12/14 | 18 | OK | OK | OK | OK | OK | OK | OK | OK | 12 | OK | OK | OK | OK |
| ee-gcc2.95.3-114 (SN) | -O2 -G16 | 6/14 | 9 | OK | 10 | 6 | OK | 5 | 10 | OK | OK | OK | 11 | 4 | 6 | OK |
| ee-gcc2.95.3-136 (SN) | -O2 -G16 | 6/14 | 9 | OK | 9 | 6 | OK | 3 | 9 | OK | OK | OK | 10 | 4 | 4 | OK |
| ee-gcc2.9-991111 | -O1 -G16 | 3/14 | 13 | OK | 12 | 4 | 9 | 6 | 8 | 5 | OK | 16 | OK | 14 | 2 | 7 |
| ee-gcc2.96 | -O2 -G16 | 2/14 | 17 | 7 | 12 | 16 | OK | 7 | 15 | 17 | 24 | 4 | 7 | 9 | 5 | OK |

All rows use Sony `as` 2.9-ee-991111. The only miss, func_00116C98, has an
identical loop body. It differs only in the order of five struct stores before
the loop plus the `blez` delay slot. No permutation of the source store order
fixes it (best is 4 words), so the original source does something slightly
different there.

## Size of a full decomp

- 6,324 functions in `.text` (0x12DB34 bytes). About 5,390 are game/engine code
  (below 0x00202800) and about 930 are libsce/libc/libm. The libraries are
  prebuilt Sony objects, best identified and kept as asm or matched against SDK
  2.4.3 libraries.
- Sizes: 2,610 functions are 0x40 bytes or less, 2,468 are 0x41-0x100, 1,096 are
  0x101-0x400 and 150 are larger. 4,217 make calls, about 1,300 use floats, 101
  use jump tables, about 370 game/engine functions use VU0 macro instructions
  (inline asm) and 20 use MMI.
- Difficulty: **moderate**. A single identified compiler and flag set covers the
  game code, matching is very deterministic (13/14 on the first serious attempt),
  and there are no debug symbols or `.mdebug`, but there are many source-path and
  assert strings. The main costs are (1) recovering struct layouts and names with
  no symbols, (2) inline-asm VU0/128-bit code that has to be reproduced
  textually, and (3) translation-unit boundaries, needed before `.data`/`.rodata`/
  `.sdata` can move into C. The tiny getters and setters (over 40% of functions)
  go fast; the large engine routines are the long tail.
