# Ghidra function map for Aggressive Inline (SLUS_203.27)

Builds the recompiler's function map (`recomp/ghidra-functions.csv`) and config (`recomp/config.toml`)
from `extracted/SLUS_203.27` with Ghidra 12.1.4 running headless.

## Processor language

Stock Ghidra 12.1.4 has no Emotion Engine language. `Ghidra/Processors/MIPS/data/languages` only has
the generic mips32/mips64 (plus R6, micro, 16e) specs. Those decode MMI, 128-bit `lq`/`sq` and COP2
macro-mode instructions wrongly. So the pipeline uses:

- **ghidra-emotionengine-reloaded v2.1.38**, the prebuilt release for Ghidra 12.1.4
  (`downloads/ghidra_12.1.4_PUBLIC_20261004_ghidra-emotionengine-reloaded.zip`, from
  github.com/chaoticgd/ghidra-emotionengine-reloaded).
- Language **`r5900:LE:32:default`** ("Emotion Engine (Play Station 2) R5900 MIPS Core"), compiler spec `default`.
- The extension is installed in a **project-local Ghidra settings dir**, so nothing outside
  `ghidra/` changes: `settings/ghidra/ghidra_12.1.4_PUBLIC/Extensions/ghidra-emotionengine-reloaded/`.
  `scripts/run-ghidra.sh` points Ghidra at it with
  `GHIDRA_HEADLESS_JAVA_OPTIONS=-Dapplication.settingsdir=$PWD/settings`. Ghidra compiles `r5900.sla` on first use.
  To use the same project in the GUI, either start Ghidra with the same `-Dapplication.settingsdir`
  (e.g. `GHIDRA_JAVA_OPTIONS`), or install the zip through File > Install Extensions.

## Reproduce everything

```bash
cd tooling/ai/ghidra

# one-time: fetch + install the EE extension into the local settings dir
gh release download v2.1.38 -R chaoticgd/ghidra-emotionengine-reloaded \
   -p 'ghidra_12.1.4_PUBLIC_*' -D downloads
mkdir -p settings/ghidra/ghidra_12.1.4_PUBLIC/Extensions
unzip -q -o downloads/ghidra_12.1.4_PUBLIC_*.zip -d settings/ghidra/ghidra_12.1.4_PUBLIC/Extensions/

# full pipeline (about 3 minutes; resumable: scripts/pipeline.sh <stage>)
nohup scripts/pipeline.sh 1 > logs/pipeline.out 2>&1 &
tail -f logs/pipeline.out
```

`scripts/pipeline.sh` runs these steps. Each stage saves the project and writes its own log
`logs/pipeline-NN-<stage>.log`. If a run is interrupted, restart it from the failed stage:

| stage | what it does |
|---|---|
| 0 | `sce_scan.py extract` pulls `symbols.json`/`tree.json` out of the analyzer's embedded `sce_symbol_database_data.h` (`tooling/recompiler/ps2xAnalyzer`, read-only) into `sce-db/`. `sce_scan.py scan` matches SDK functions in `.text` and writes `sce-db/sce-matches.csv`. Then the project is recreated |
| 1 | import with `-processor r5900:LE:32:default -cspec default`. `PrepareEEProgram.java` clears the execute bit on every block except `.text`: `.vutext` and the 8 `.DVP.overlay.*` VU microcode sections, plus the extension's `vu0.code`/`vu1.code`/`iop_ram` placeholders. VU code is never disassembled as EE code and never exported |
| 2 | `SetAnalyzerOption.java "Shared Return Calls=false"`, then `ApplySceSymbols.java`, then full auto-analysis |
| 3 | turn off "Non-Returning Functions - Discovered", then `FixNoReturn.java` and `FixSwitchTables.java`, then re-analysis |
| 4 | `FixNoReturn.java`, then `GapFill.java`, then re-analysis |
| 5 | `FixNoReturn`, `FixSwitchTables`, `MergeFragments`, `GapFill`, `FixDetachedChunks`, then re-analysis |
| 6 | verify: dry runs of all the fixers (all must report 0 left to do; 6 dead branch stubs are expected) |
| 7 | `MapQualityReport.java` writes `reports/` |
| 8 | `ExportPS2FunctionsHeadless.java` writes `recomp/config.toml` and `recomp/ghidra-functions.csv` |
| 9 | the readable write-back on a copy, `project-readable/` (the project stays as stage 8 exported it): `tooling/dm2/tools/readable_ghidra.py gen ai_ps2 readable` writes `readable/readable-ai_ps2.jsonl` and `.h` from the work database and the readable sources, `tooling/dm2/tools/ghidra/readable_pass.sh` applies them with `ApplyReadable.java`, checks them read-only and writes `readable/AggressiveInline-decomp.c` (`tooling/dm2/MATCH3.md` section 7). Skipped when `DM2_REPO` (default `tooling/dm2`) has no `readable_pass.sh` |

To run a single script against the saved project:

```bash
scripts/run-ghidra.sh -process SLUS_203.27 -noanalysis -readOnly \
  -scriptPath $PWD/scripts -postScript MapQualityReport.java $PWD/reports
```

## Scripts (all in `scripts/`)

- `run-ghidra.sh`: wrapper around `$(brew --prefix ghidra)/libexec/support/analyzeHeadless` with
  JAVA_HOME=openjdk@21, 6 GB heap, the local settings dir and project `project/AggressiveInline`.
- `ExportPS2FunctionsHeadless.java`: copy of `tooling/recompiler/ps2xRecomp/tools/ghidra/ExportPS2Functions.java`
  with two changes. (1) Script args `<config.toml> <functions.csv> [input_elf] [output_dir]` replace the
  `askFile` dialogs and set `[general].input`/`output`. (2) Non-function code labels (`caseD_*`, `switchD_*`,
  `default`) are exported as `entry_XXXXXXXX`. The upstream script exports them under their Ghidra label
  names. Those names repeat (756 duplicates) and the recompiler only treats `entry_*` names as nested
  synthetic entries, so upstream turned every switch case into a fake top-level function.
- `sce_scan.py`: a Python port of `tooling/recompiler/ps2xAnalyzer/src/sce_symbol_scanner.cpp`: match tree, relocation masking,
  SHA-1, at least 256 static bits, one identity per address.
- `ApplySceSymbols.java`: SCE names become function entry points before analysis, so the export's
  name-based stub classification sees them.
- `ApplyReadable.java`: the readable write-back (stage 9, and `tooling/dm2/tools/ghidra`'s pipelines for the
  other builds): parses the generated header with Ghidra's C parser into `/readable`, renames the
  functions (the old name kept in the `ReadableOriginalName` property map, a loader or SCE name as a
  secondary label), applies the signatures (default convention, the EE's EABI), types the globals,
  then reads every item back (struct sizes and fields, names, parameter types and registers,
  returns) and fails on any mismatch. The input's SHA-256 goes into Program Information;
  `verify` re-checks a saved program read-only.
- `FixSwitchTables.java`: completes GCC jump tables (`sltiu`/`lui`/`addiu`/`sll`/`addu`/`lw`/`jr`). It reads
  the table address from the `lw` reference and the bound from the `sltiu` on the index register. If there
  is no `sltiu`, it infers the bound from consecutive in-function code pointers. It adds `COMPUTED_JUMP` refs
  and removes functions that were wrongly created on case blocks.
- `GapFill.java`: GCC lays functions out back to back, so a gap that starts right after a terminated function
  (`jr`/`j`/`b`/`eret`) is a function. It recovers pointer-only functions: vtable methods, callbacks, empty
  `jr ra` stubs. It skips unreferenced dead `b`/`j` stubs.
- `MergeFragments.java`: merges blocks that are only reached by branches from one earlier function back into
  that function. This is -O0 code around 0x166b88. It also drops unreferenced dead `b`/`j` stub functions,
  which Ghidra otherwise treats as thunks.
- `FixDetachedChunks.java`: GCC cross-jumping makes functions branch to a far `jr ra; nop` (or another
  function's body). This script splits such remote body ranges into their own functions, so each exported
  `[start, end)` range is contiguous.
- `FixNoReturn.java`: clears noreturn on functions that end in `jr ra`, plus bogus `CALL_RETURN` overrides on
  `jal`. It is a guard; the only wrong case, FUN_001672d8, went away once the analyzer was turned off.
- `MapQualityReport.java`: coverage, gaps, overlaps, jump tables, bookmarks, and source-path grouping.
- Debug helpers: `DumpAround.java`, `DumpBodies.java`, `ListNoReturn.java`.

## Outputs

- `../recomp/ghidra-functions.csv`: the recompiler's map, `Name,Start,End,Size`. It has 6324 Ghidra functions
  plus 19936 `entry_*` synthetic labels. The labels are branch targets and address-taken code; this is
  upstream script behaviour, and the recompiler treats nested `entry_*` labels as wrappers.
- `../recomp/config.toml`: `input` = the ELF, `output` = `recomp/generated`, `ghidra_output` = the CSV.
  It lists 327 `stubs` (`name@addr`, runtime-handled SDK/libc functions found mostly via the SCE scan) and
  80 `untracked_stubs`.
- `reports/quality.txt`, `gaps.csv`, `jumptables.csv`, `error-bookmarks.csv`
- `reports/source-paths.json` / `source-paths.csv`: every `*.c/*.cpp/*.h/...` path string with its addresses
  and the functions that reference it. `reports/function-source.csv` is the same data inverted (function to paths).
- `sce-db/sce-matches.csv`: 582 SCE SDK matches (address, size, name, library).

## Results

- 6324 functions. 1,223,620 of 1,235,764 `.text` bytes are inside functions (99.02%).
  The remainder is 12,096 B of zero/nop padding (3010 ranges) and 48 B of dead `b` stubs (6 of them, in the -O0
  zsaveapi code around 0x166c18 to 0x167240).
- 0 instructions outside functions, 0 data defined in `.text`, 0 overlapping bodies, 0 error bookmarks.
  Nothing from `.vutext` or the overlays is in the map.
- 124 computed `jr` instructions; all now have targets (12 tables completed by `FixSwitchTables`).
- 582 SCE matches are applied (libkernl 141, libmpeg 110, libc 82, libgcc 38, libdev 31, libvu0 30, libpad 29,
  libmc 27, libgraph 18, ...). All 582 are function starts in the map. 6 Ghidra bodies are shorter than the
  SDK size (exception handlers/eret code and sceCdMmode).
- Source paths: 19 distinct strings, all referenced from code, attributing 23 functions. The anchors' addresses
  rise in link order (ps2/ to com/ to ZGE3 core to ZgeMods to game src/ to GameTools), so they bound where each
  object file sits in `.text`.

## Known caveats

- `GapFill` assumes contiguous GCC layout. About 560 recovered functions are 8 bytes or less: mostly
  `jr ra; nop` empty methods and `j` tail-call stubs, which is plausible for C++-style vtables.
- Two jump tables (in FUN_00103a40, 56 entries, and FUN_0016e578, 65 entries) have no `sltiu` bound. Their
  length was inferred.
- 20 duplicate names in the CSV: SDK statics such as `setD3_CHCR`/`kCopy`, kernel syscall stubs named by the
  EE extension (`RFU*`), and `thunk_FUN_*`. The stub selectors use `name@address`, so they are unambiguous.
- Turning off "Shared Return Calls" means `j`/`b` tail jumps are plain jumps. `FixDetachedChunks` (52 splits)
  keeps the ranges contiguous.
