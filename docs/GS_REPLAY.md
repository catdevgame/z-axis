# GS trace replay and the bit-exact renderer check

Traces are recorded in the runner with `ZX_GS_DUMP=<file>
ZX_GS_DUMP_FRAME=<host frame> [ZX_GS_DUMP_COUNT=n]` (format in
`ps2/gs/gs_record.h`) and replayed without the game:

```sh
zx-gsreplay <trace.gsr> <out-prefix> [first-frame] [--no-png] [--hash]
             [--picture-hash] [--dump-vram] [--time] [--repeat n]
             [--gpu [--scale n] [--compare] [--between a,b,...]]
```

`--gpu` replays on the SDL_GPU hardware renderer instead (headless, PNGs at
the internal resolution, `--scale n` for n times the GS resolution);
`--compare` also replays on the CPU renderer and prints per frame the mean
and largest per-channel difference and writes `<out-prefix>-<n>-cmp.png`
(CPU | GPU | difference x4). `--between` (with `--gpu`) also makes frame interpolation's
pictures between frames ("Frame interpolation" there). See `docs/GPU_BACKEND.md`.

Any change to the GS CPU backend must keep the output identical. Before the
change, record a reference with the old build; after it, check:

```sh
source tools/paths.sh   # AI_WORKSPACE
T=$AI_WORKSPACE/recomp/traces
REF=$AI_WORKSPACE/recomp/logs/gs-reference
tools/checks/gs_replay_check.sh record <old build>/runtime/zx-gsreplay $T $REF
tools/checks/gs_replay_check.sh check  <new build>/runtime/zx-gsreplay $T $REF
```

`check` compares, per frame, a hash of the full 4 MiB of GS local memory and of
the presented picture, and the pictures again from a replay without the VRAM
snapshots (so they are taken with draws still queued, as in the runner). On a
difference, `KEEP=<dir>` writes the local memory of the failing traces and
`tools/checks/gs_vram_diff.py <ref>.vram <new>.vram` lists the GS pages that differ.
Run it in the modes below too (e.g. `ZX_GS_THREADS=1 tools/checks/gs_replay_check.sh check ...`).

The randomized differential tests (`tests/gs_cache/gs_fast_path_tests.cpp`,
`ctest --test-dir <build>/tests -R fast`) compare the fast path with the
reference rasterizer on cases the traces do not cover (render to texture,
stale texture cache, all formats, queued presentation).

## Switches (environment)

| Variable | Effect |
|---|---|
| `ZX_GS_REFERENCE=1` | only the reference rasterizer (no queue, no threads) |
| `ZX_GS_THREADS=n` | raster threads (default: all cores, at most 32; 1 = no tile workers) |
| `ZX_GS_ASYNC=0` | draw batches on the calling thread instead of the raster thread |
| `ZX_GS_DEFER=0` | runner/replay: draw every primitive before the GS call returns |
| `ZX_GS_SIMD=0` | scalar bilinear filtering instead of NEON or SSE4.1 |
| `ZX_GS_STATS=1` | per presented frame: queued/serial draws, batches, texture decodes |
