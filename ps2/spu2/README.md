# SPU2 (PS2 sound processor)

This directory emulates the PS2 SPU2 for the IOP emulator (iop/), as its own library,
`zx_spu2` (its interface `spu2/spu2.h`). The sound code is a port of
the SPU2 emulation from [PCSX2](https://github.com/PCSX2/pcsx2), copyright
2002-2026 PCSX2 Dev Team, licensed GPL-3.0+ (the collection is GPL-3.0). Thanks to the
PCSX2 team and to the people credited in the original sources (Dr. Hell,
nocash, Near, Ryphecha and others).

Ported from PCSX2 commit `091ab68708a555198d80a7216965481c7ddd0d98`:

| file | PCSX2 source |
|---|---|
| `spu2_internal.h` | `pcsx2/SPU2/defs.h`, `regs.h` |
| `spu2_sys.cpp` | `spu2sys.cpp`, `RegTable.cpp`, register/reset parts of `spu2.cpp` |
| `spu2_mixer.cpp` | `Mixer.cpp`, `ReadInput.cpp`, `spu2Output()` from `spu2.cpp` |
| `spu2_adsr.cpp` | `ADSR.cpp` |
| `spu2_reverb.cpp` | `Reverb.cpp`, `ReverbResample.cpp` (portable reference filters) |
| `spu2_dma.cpp` | `Dma.cpp`, DMA4/7 callbacks in `spu2.cpp`, SPU2 part of `pcsx2/IopDma.cpp` |
| `spu2_interpolate_table.h` | `interpolate_table.h` (unchanged) |
| `spu2.h`, `spu2.cpp` | new: the interface used by `IopMemory` and `IopEmulator` |

What changed relative to PCSX2:

- All global state is held in one `spu2::Spu2System` per IOP so several
  emulators can exist at once. The template table of register write handlers
  became an address decoder calling the same handler bodies.
- Dropped: savestates, debug/visual debugger, wave dumps, logging, PS1 mode,
  CDDA/PCM24 input, multi-ISA SIMD paths (the reverb filters use PCSX2's
  scalar reference versions) and the host AudioStream/SndOut backends.
- The IOP DMA channel 4/7 registers live here; transfers copy from/to IOP RAM
  by byte address with wrap-around. Completion and SPU2 IRQs are reported as
  IOP interrupt numbers 0x24, 0x28 and 9 and dispatched by the IOP's intrman (iop/).
- `psxCounters[6]`/`CounterUpdate` is replaced by `Spu2::nextEventCycle()`,
  which the IOP scheduler uses to bound idle skips. Time is IOP cycles; one
  48 kHz sample is mixed every 768 cycles, as in PCSX2.
- Output is interleaved signed 16-bit stereo, passed to
  `IopHost::audioSamples()`; the runtime decides how to play it.

Tests: `iop/tests/iop_spu2_tests.cpp` (`ps2_iop_spu2_tests`).
