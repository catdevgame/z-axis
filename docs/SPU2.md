# The emulated sound path: SPU2 on the IOP

What runs when the game's sound goes the PS2 way instead of the native driver (docs/NATIVE.md), and how to use it.
The native ZAudio driver is the default in all three games. This path is a fallback and a reference for sound
comparisons.

## What runs

- `ZX_SOUND=spu2` runs the game's own sound modules on the emulated IOP: ZAUDIO (the game's driver; RPC SID 0x47 in
  Aggressive Inline), LIBSD and SDRDRV, over PCSX2's SPU2 ported into iop/ (`ps2/spu2/`; its README.md
  maps the PCSX2 files). The game also takes this path for its movies' sound when the movies run in their PS2 form
  (`ZX_MOVIES=hle`): the libsdr calls are forwarded to SDRDRV.
- The IOP is clocked from EE cycles (36.864 MHz; 768 IOP cycles per 48 kHz sample). DMA4/7 and the SPU2 interrupt
  are emulated. The mixed output goes to the host through an SDL3 audio stream (`ps2/runtime/spu2_audio.cpp`).
- Tests: `ps2_iop_spu2_tests` (ADPCM vectors, registers, DMA, voices, interrupts, ADMA), built with
  `-DPS2X_IOP_BUILD_TESTS=ON` and run with `ctest --test-dir <build>/iop`.

## Switches (environment; all off by default but the stats line)

| Variable | Effect |
|---|---|
| `ZX_SPU2_WAV=<file>` (`ZX_SPU2_WAV_SECONDS`, default 120) | the exact mixed output as a 48 kHz 16-bit stereo WAV, valid even if the run is killed |
| `ZX_SPU2_AUDIO=0` | no host audio device (headless runs) |
| `ZX_AUDIO=off` | the mix plays to SDL's dummy device (test runs, unpaced runs; DEVTOOLS.md) |
| `ZX_SPU2_STATS=<s>` | level, buffer, underruns and emulation speed every s emulated seconds (default 30; 0 turns them off) |
| `ZX_SPU2_LATENCY_MS=<ms>` | the host ring's target latency (default 60) |
| `ZX_TRACE_SPU2=<n>` | the first n SPU2 register writes, DMA starts and ends, interrupts |
| `ZX_SPU2_ADMA_DUMP=<file>`, `ZX_SPU2_RAM_DUMP=<prefix>` | raw ADMA input blocks; SPU2 RAM and the voice table every 5 emulated seconds |
| `ZX_TRACE_SDR=1`, `ZX_SDR_HLE=1` | each forwarded sceSdRemote call; the libsdr HLE in place of the forwarding to SDRDRV (the movies are then silent) |

## Known gaps

- Reverb uses PCSX2's scalar reference filters, so it can differ in the low bits from PCSX2's SIMD path.
- No PS1 mode, no CDDA or PCM24 input on core 1, no SPDIF bitstream.
- IOP DMA timing follows PCSX2's model, on an IOP that charges one cycle per instruction.
- The host output cannot hide an EE slower than real time (no time-stretching).
