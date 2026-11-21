# The emulated sound path: SPU2 on the IOP

What runs, and how to use it, when the game's sound goes the PS2 way instead of the native driver
(docs/NATIVE.md). The native ZAudio driver is the default; this path is a fallback and a
reference for sound comparisons.

## What runs

- `ZX_SOUND=spu2` runs the game's own sound modules on the emulated IOP: ZAUDIO (the game's
  driver, RPC SID 0x47), LIBSD and SDRDRV, over PCSX2's SPU2 ported into iop/
  (`iop/src/emulator/spu2/`, its README.md maps the PCSX2 files). `ZX_MOVIES=hle` also takes
  this path for the movies' sound (the libsdr calls forwarded to SDRDRV).
- The IOP is clocked from EE cycles (36.864 MHz; 768 IOP cycles per 48 kHz sample). DMA4/7 and the
  SPU2 interrupt are emulated; the mixed output goes to the host through an SDL3 audio stream
  (`runtime/src/lib/ps2_spu2_audio.cpp`).
- Tests: `ps2_iop_spu2_tests` (ADPCM vectors, registers, DMA, voices, interrupts, ADMA), built with
  `-DPS2X_IOP_BUILD_TESTS=ON` and run with `ctest --test-dir <build>/iop`.

## Switches (environment, off by default but the stats line)

| Variable | Effect |
|---|---|
| `ZX_SPU2_WAV=<file>` (`ZX_SPU2_WAV_SECONDS`, default 120) | the exact mixed output as a 48 kHz 16-bit stereo WAV, valid even if the run is killed |
| `ZX_SPU2_AUDIO=0` | no host audio device (headless runs) |
| `ZX_AUDIO=off` | the mix plays to SDL's dummy device (test runs, unpaced runs; DEVTOOLS.md) |
| `ZX_SPU2_STATS=<s>` | level, buffer, underrun and emulation speed every s emulated seconds (default 30; 0 turns them off) |
| `ZX_SPU2_LATENCY_MS=<ms>` | the host ring's target latency (default 60) |
| `ZX_TRACE_SPU2=<n>` | the first n SPU2 register writes, DMA starts and ends, interrupts |
| `ZX_SPU2_ADMA_DUMP=<file>`, `ZX_SPU2_RAM_DUMP=<prefix>` | raw ADMA input blocks; SPU2 RAM and the voice table every 5 emulated seconds |
| `ZX_TRACE_SDR=1`, `ZX_SDR_HLE=1` | each forwarded sceSdRemote call; the old libsdr HLE (silent movies) |

## Known gaps

- Reverb uses PCSX2's scalar reference filters (bit differences from PCSX2's SIMD path); the game
  leaves reverb off in the runs measured.
- No PS1 mode, no CDDA or PCM24 input on core 1, no SPDIF bitstream.
- IOP DMA timing follows PCSX2's model on an IOP that charges one cycle per instruction.
- The host output cannot hide an EE slower than real time (no time-stretching).
