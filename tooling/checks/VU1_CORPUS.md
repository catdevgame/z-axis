# VU1 run corpus and the bit-exact interpreter check

Any change to `VU1Interpreter` (`runtime/src/lib/vu/`) must keep the output of every
recorded microprogram run identical, unless the change is a deliberate
correctness fix (then record new corpora with the fixed build).

## Recording

```sh
ZX_VU1_RECORD=<file.vu1r> [ZX_VU1_RECORD_FROM=<guest vsync>] \
  [ZX_VU1_RECORD_VSYNCS=<n, default 120>] [ZX_VU1_RECORD_RUNS=<max runs>] \
  zx-runner ...
```

Every MSCAL and MSCNT run is written with its micro memory (when it
changed), registers and flags in and out, data memory in and out (sparse
qword deltas) and every PATH1 packet it kicked. Format in
`runtime/include/runtime/ps2_vu1_record.h`. `[run:tick]` log lines show the guest
vsync count, which is what `ZX_VU1_RECORD_FROM` uses (in Aggressive
Inline's level one game frame is about one guest vsync while the game runs
slower than real time).

The path is opened from the runner's working directory (`recomp/run` with
`run.sh`), so pass an absolute path.

## Checking

```sh
zx-vu1check <corpus.vu1r>... [--stop] [--quiet] [--time] [--repeat n] [--profile] [--dump seq prefix]
             [--poke qword lane hex] [--reads qword]
```

replays each run through a fresh `VU1Interpreter` state taken from the
recording and compares registers, flags, the cycle count, all 16 KiB of data
memory and every PATH1 packet bit for bit. Exit status 0 when every run
matches. `--time` reports thread CPU time per VU cycle (best of `--repeat`),
`--profile` runs, cycles, packets and time per start PC, `--dump` writes one
run as `.code/.data-in/.data-out/.regs-in` (the `ZX_VU1_DUMP` layout, for
`tools/vudis.py`) plus `.state-in/.state-out/.gif`. `--reads` counts the loads of a
data qword by pc and lanes; `--poke` starts every run with a value in a lane of a
data qword and expects it there at the end: a run that differs read it.

`tooling/ai/tools/vu1corpus.py` reads corpora from Python (programs, runs,
GIF vertices).

## The corpora

Each game's corpora are in its workspace's `recomp/traces/vu1` (AI_WORKSPACE, DM2_WORKSPACE, BMX_DATA;
data, never committed). `tooling/checks/vu1_corpus_check.sh CHECKER [DIR|FILE ...]` replays all of them (default: the
three folders) with the ahead-of-time code and with `ZX_VU1_AOT=0`; CHECKER is a zx-vu1check built with the
three games' AOT folders, as the collection build has them. Exit 0 when every run matches on both sides; a
corpus folder gone, or an image the checker has no AOT code for ("NO AOT"), fails it too.

`vu1_corpus_check.sh --changed REPO BASE COMMIT` lists the VU1 files (the interpreter, the AOT generator and its
build step, the checker) that differ between two commits: a change there is what needs the check.

- Aggressive Inline: the level (four views), the menus, split-screen multiplayer.
- Dave Mirra BMX 2 (one micro memory image, 836c2d1891f4ba03, in every session): the front end (vsync 2400 +
  2000), 60 vsyncs about 675 into play on Woodward, Trainyard, Airport (Free Ride), Swampy (Session), Galloon
  (ProQuest), the Xbox levels Venice and Greenville (ZX_DM2_XBOX_LEVELS), 120 vsyncs of the park editor
  (playbot/dm2park.py build) and two custom maps in the park slot (tut-dm2, root-dm2): playbot
  sessions, the CPU renderer, about 1,100 runs a vsync in play.
- BMX XXX: the menus, the Secret Level, TST_FLAT, TST_GRND and TST_VERT (two windows each for GRND
  and VERT), the Bronx, CMP_RMPG (two), the custom maps tut-bmx and woodward-bmx.
