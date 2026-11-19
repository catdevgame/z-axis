# Aggressive Inline (USA, SLUS_203.27): the split assembly and the original toolchain

The retail ELF (`SLUS_203.27`, SHA1 `af35b83f0d773e613c91f21fa1e00a45efbbfec7`) split by splat into assembly and data,
and the toolchain that built it. Two things here are used by the rest of the tooling:

- the assembly listing (`asm/cod/*.s`, made by `configure.py`): `tooling/checks/readable_frame_stores.py` reads it,
  and a readable function the drafter does not finish is written from it (docs/READABLE.md);
- the original assembler and binutils (`tools/binutils`, fetched by `tools/setup.sh`): the PCSX2 reference runs'
  pad hooks are assembled with them (`tooling/ai/tools/build_hook.py`, `tooling/dm2/tools/dm2_build_hook.py`).

The build reassembles and links the split and reproduces both the flat load image and the full retail ELF byte for
byte (`ninja` checks both SHA1s), which proves the listing is the whole program.

## Toolchain findings

| Item | Finding | Evidence |
|---|---|---|
| Compiler (codegen) | **Sony ee-gcc 2.9-ee-991111** (cc1). The `991111`, `991111-01` and Windows `991111b/r4` builds give identical code. | `990721` is ruled out by func_00116C98's code. SN ProDG 2.95.x builds save `$ra` with 128-bit `sq`/`lq` (2.95.3 v1.36 excepted) where retail uses `sd`/`ld`. 2.96 and 3.x schedule differently. |
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
SLUS_203.27.yaml      splat config (segments)
configure.py          runs splat, writes build/link.ld and build.ninja
include/              splat macros (macro.inc, labels.inc)
tools/setup.sh        fetches the toolchain into tools/ (gitignored) and creates .venv
tools/cc.sh           C -> .o with Sony ee-gcc 2.9-ee-991111 (cc1 + as) in a linux/386 container
tools/elf_pack.py     rebuilds the retail ELF wrapper (ee-ld layout, DVP overlay tables) around our link
tools/xref.py         find a string and the code that references it
tools/funcs.py        function list (address, size, call count, source-path strings)
tools/showfunc.sh     a function's split asm
tools/check_sha1.sh   SHA1 check used by ninja
```

Never committed (`.gitignore`, `tooling/.gitignore`): the ELF, the `.rom`, `asm/`, `assets/`, `build/`,
`tools/compilers/`, `tools/binutils/`, `tools/wibo`, `.venv/`, `logs/`. `tooling/setup.sh` links them into the
data folder (`tooling/data-links.txt`).

## Setup (macOS arm64)

Requirements: `uv`, `ninja`, Docker Desktop (used only to run the 32-bit Linux compiler
binaries in a `linux/386` `debian:bookworm-slim` container of about 120 MB), Rosetta 2
(for wibo).

```sh
cd tooling/ai/decomp
tools/setup.sh            # or: tools/setup.sh --all  (also the comparison compilers)
```

`tools/setup.sh` installs (about 150 MB in `tools/` plus the 120 MB image):
- `.venv`: splat64 0.50.1, spimdisasm 1.42.4, rabbitizer 1.16.2 (R5900 mode is selected by splat's `platform: ps2`)
- decompals `binutils-mips-ps2-decompals` v0.10 (macOS arm64): as/ld/objcopy for the split asm and the link
- decomp.me compiler packages: `ee-gcc2.9-991111` (Linux i386, the matching toolchain) and `ee-gcc2.9-991111b-r4` (Windows, same cc1 codegen)
- `wibo` 1.2.0 macOS build: runs the Windows compiler builds natively; the build does not need it.

The retail ELF goes in as `SLUS_203.27` here (after `tooling/setup.sh` that name is a link into the data folder,
`$AI_DATA/decomp/SLUS_203.27`): a copy of `../extracted/SLUS_203.27`, which `tooling/recompile.sh` takes from the disc.

## Build and verify

splat adds to `asm/` and never removes a file, so after a change to the segments remove `asm/` first (a stale
split file would give `readable_frame_stores.py` a function twice).

```sh
.venv/bin/python configure.py   # runs splat (writes asm/, assets/), build/link.ld, build.ninja
ninja                           # assemble, link, objcopy, elf_pack, check both SHA1s
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
