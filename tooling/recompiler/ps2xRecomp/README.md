# ps2xRecomp: the recompiler and its configuration

The build-time tool that makes each game's generated code: the code every function runs as until its readable body
takes over (README.md at the top of the checkout says how the collection uses it). Paths below are relative to the
checkout's root.

## Modules

* `tooling/recompiler/ps2xAnalyzer`: scans ELF/functions and writes TOML config (`stubs`, instruction patches).
* `tooling/recompiler/ps2xRecomp`: reads TOML + ELF, decodes R5900 instructions, and generates C++ output.
* `runtime`: hosts memory, function registration, syscall dispatch, and hardware stubs.
* `iop`: R3000A IRX execution, a virtual IOP kernel, and generic HLE fallbacks.

## Features

* Translates MIPS R5900 instructions to C++ code
* PS2-specific MMI and VU0 macro support.
* Single-file or multi-file output.
* Configurable stubs, skips, and instruction patches.
* Instruction-driven syscall handling.

## How It Works
The recompiler works by:

* Parsing a PS2 ELF file to extract functions, symbols, and relocations
* Decoding the MIPS R5900 instructions in each function
* Translating those instructions to equivalent C++ code
* Generating a runtime that can execute the recompiled code

The translated code is very literal, with each MIPS instruction mapping to a C++ operation. For example, `addiu $a0, $a0, 0x20` becomes `SET_GPR_S32(ctx, 4, (int32_t)ADD32(GPR_U32(ctx, 4), 32));`.

## Current Behavior

* `stubs` entries generate wrappers that call known runtime syscall/stub handlers by name.
* `stubs` also supports address bindings with `handler@0xADDRESS` for stripped games (for example `sceCdRead@0x00123456`).
* Address bindings also support generic return handlers for triage: `ret0`, `ret1`, `reta0`.
* The recompiler tries relocation-symbol auto-binding at callsites (`J/JAL`) before raw address dispatch; when relocation symbol is known (for example `sceCdRead`), it can call runtime handlers without manual address mapping.
* Recompiler discovers additional internal static entry targets and emits `entry_...` wrappers for those addresses.
* For unresolved static `J/JAL` sites, generated code falls back to `runtime->dispatchGuestBranch(...)` (the runtime's function table by address).
* `skip` entries are not recompiled and generate explicit `ps2_stubs::TODO_NAMED(...)` wrappers.
* Recompiled `SYSCALL` calls `runtime->handleSyscall(...)` with the encoded syscall immediate.
* Runtime syscall dispatch tries encoded syscall ID first, then falls back to `$v1`.

## Usage

Preferred workflow for retail or stripped games:

1. Open the ELF in Ghidra.
2. Run `tooling/recompiler/ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (headless, as the games' workspaces do:
   `tooling/ai/ghidra/NOTES.md`).
3. Use the exported TOML and CSV map.
4. Recompile with the exported TOML:

```bash
./ps2_recomp config.toml
```

Fallback workflow for quick local experiments or ELFs with debug symbols:

```bash
./ps2_analyzer your_game.elf config.toml
```

See the [Ghidra workflow](../ps2xAnalyzer/Readme.md#4-ghidra-integration) for Ghidra instructions. For the three
games, `tooling/recompile.sh` makes each game's config (`recomp/tools/make-config.py`; BMX XXX's
`tooling/bmx/recomp/recompile.py`) and runs the recompiler.

Then build generated output and link with the runtime (`runtime/`).

## Configuration

Main fields in `config.toml`:

* `general.input`: source ELF path.
* `general.ghidra_output`: recommended function map CSV exported from Ghidra.
* `general.output`: generated C++ output folder.
* `general.single_file_output`: one combined cpp or one file per function.
* `general.low_memory_mode`: reduce peak output-generation memory by avoiding retained disassembly strings and forcing serial output generation. Generated instruction comments are still emitted; disassembly text is produced while writing each output file instead of being kept in memory.
* `general.output_worker_threads`: number of output-generation workers (clamped to nproc * 2). A positive value uses exactly that many workers. `0` uses `nproc - 1` when at least two hardware threads are available, otherwise serial output generation. `1` forces serial output generation.
* `general.patch_syscalls`: apply configured patches to `SYSCALL` instructions (`false` recommended).
* `general.patch_cop0`: apply configured patches to COP0 instructions.
* `general.patch_cache`: apply configured patches to CACHE instructions.
* `general.stubs`: names to force as stubs. Also accepts `handler@0xADDRESS` to bind a stripped function address directly to a runtime syscall/stub handler. Includes generic handlers `ret0`, `ret1`, `reta0`.
* `general.skip`: names to force as skipped wrappers.
* `general.recompile`: names (exact, `prefix*` or `name@0xADDR`) recompiled from the ELF even when the runtime has a
  handler of the same name.
* `general.entry_points`: guest functions without runtime handlers that may be reached by address.
* `general.namespace`: wrap the generated code in this C++ namespace and register its function table with the runtime
  by name instead of defining the global table (several games in one executable: DM2's and BMX XXX's
  `generated-ns`).
* `patches.instructions`: raw instruction replacements by address.
* `[vu1] microprogram_chain`: the address of the DMA chain the game's renderer sends VIF1 its VU1 microprograms with
  (`microprogram_chain_tte`: whether that DMA has CHCR.TTE, default true). With a chain the recompiler writes the
  micro memory image it leaves, `<hash>.vu1code`, beside the generated code (`runtime/tools/VU1_AOT.md`).
* `[mmio]`, `[jump_tables]`: per-instruction MMIO addresses and jump tables, as the analyzer writes them.

Address binding for stripped ELFs:

* Use `handler@0xADDRESS` inside `general.stubs` to map a stripped function start directly to a runtime handler.
* Example: `sceCdRead@0x00123456` binds function start `0x00123456` to `ps2_stubs::sceCdRead(...)`.
* Generic temporary handlers are available: `ret0@0xADDR`, `ret1@0xADDR`, `reta0@0xADDR`.
* Before manual binding, prefer recompilation from a Ghidra-exported TOML/CSV first. The extra boundaries and synthetic entry points are usually more important than manual early triage.
* The address must be the function start in that exact ELF build.
* Addresses are not portable across different games/regions/builds.
* The handler name must exist in runtime call lists (`PS2_SYSCALL_LIST` or `PS2_STUB_LIST`).

Example:

```toml
# stripped function binding by address:
stubs = ["sceCdRead@0x00123456", "SifLoadModule@0x00127890"]
# temporary return handlers:
stubs = ["ret0@0x001D9410", "ret1@0x001D5BC8", "reta0@0x0024B7C0"]
# mixed example:
stubs = ["printf", "sceCdRead@0x00123456", "SifLoadModule@0x00127890"] 
```

## Runtime

To execute the recompiled code.

`runtime` currently provides:

* Guest memory model and function dispatch table.
* Some syscall dispatcher with common kernel IDs.
* Basic GS/VU/file/system stubs.
* Foundation to expand and port your game.
* `iop` execution of original IRX modules with generic HLE fallbacks.

See [IOP emulation](../../../iop/README.md) for module execution and the service boundary.

## Game Override Hooks

Game overrides are runtime-side, build-scoped patch modules.

A game override is C++ code that runs during `loadELF` and can replace EE function bindings by address for one specific game build. IOP RPC/DMA behavior is handled by the `iop` emulator and its runtime transport. This is separate from recompilation output and separate from global runtime stubs/syscalls.

API:

* Header: `runtime/include/game_overrides.h`
* Register macro: `PS2_REGISTER_GAME_OVERRIDE(name, elfName, entry, crc32, applyFn)`
* Direct bind helper: `ps2_game_overrides::bindAddressHandler(runtime, addr, "handler")`

Use Game Override modules when:

* You need per-game/per-build routing or patches without polluting global behavior.
* You need to bind many addresses, or install custom replacement logic for a specific title.

### Recommended Iteration Loop

1. Run with minimal config and no aggressive skipping.
2. Fix hard blockers first (`function not found`, syscall TODO, critical IO stubs).
3. Use temporary return stubs only to classify call importance.
4. Promote temporary fixes to real implementations.
5. Move per-game hacks into game overrides keyed by ELF metadata.
6. Re-test from cold boot after each batch.
