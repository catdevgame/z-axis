# iop

`iop` runs original IRX modules on an R3000A interpreter, with a virtual
IOP kernel providing imports without a PS2 BIOS. The C++20 static library
`ps2_iop` / `ps2x::iop` is linked into the runtime (`runtime/`). The games run
with the IOP stopped by default: their disc, movie, sound and save modules are
replaced by native code (`docs/NATIVE.md`), and this path is the fallback and
the reference for comparisons (`ZX_IOP=1`, `ZX_SOUND=spu2`, ...).

It also holds the disc image reader, `zx_disc` / `zx::disc` (ISO 9660 over
an ISO, a BIN/CUE dump, a CHD, a CSO or a ZSO), which the app uses to set a
game up from the player's disc, and its command line, `zx-disc list | extract
| to-iso` (`tools/disc_tool.cpp`; `-DZX_IOP_DISC_ONLY=ON` builds it alone,
as `tooling/recompile.sh` does). Beside them, `zx_zal` / `zx::zal` reads and
writes the games' `.ZAL` archives byte for byte as the discs have them
(`include/zx/zal_archive.h`: the game file system puts mods' single
entries in them), and `zal list | unpack | repack | verify`
(`tools/zal_tool.cpp`) is its command line for modders (`docs/MODS.md`,
"Archives").

## Execution policy

Game-specific IOP code executes from IRX modules. There is no game-profile
selection or native profile-plugin loader. A physical IRX RPC server is
authoritative for its SID.

Generic HLE services remain available when no loaded IRX provides an endpoint:

| Service | SID | Activation |
| --- | --- | --- |
| MCSERV | `0x80000400`, `0x80000480` | Recognized module load |
| LIBSD | `0x80000701` | Recognized module load |
| DBCMAN | `0x80001300` | Recognized module load |

These services are dormant before module load and after reset or the final
module stop. Unknown modules fail to load; unknown RPC SIDs remain unhandled.
A game needs its own IRX modules and support for the imports and hardware they
use.

## SPU2

The SPU2 sound processor (registers at `0x1F900000`, IOP DMA channels 4/7,
interrupts 9, 0x24, 0x28) is emulated in `src/emulator/spu2`, ported from
PCSX2's SPU2 (GPL-3.0+); see [src/emulator/spu2/README.md](src/emulator/spu2/README.md).
Mixed 48 kHz stereo is delivered through `IopHost::audioSamples()`.

## Lifecycle and transport

- `reset()` clears loaded modules, HLE service state and emulator state.
- `loadModule(...)` / `loadModuleBuffer(...)` load and start an IRX.
- `stopModule(...)` releases a module and its owned state.
- `runEeCycles(...)` advances the IOP from EE cycle accounting.
- `selectRpcAbi(...)`, `handleRpc(...)` and `onSifTransfer(...)` connect SIF transport.

IOP RAM is separate from EE RAM. The transport copies data through the IOP
memory accessors; SIF notifications do not mirror bytes into equal-numbered EE
addresses. `RpcResult` describes completion and dispatch actions for the runtime.

Link with `target_link_libraries(my_runtime PRIVATE ps2x::iop)`. The public API
is [iop_subsystem.h](include/ps2x/iop/iop_subsystem.h); `PS2Runtime` owns its
subsystem and host adapter.

## Diagnostics and tests

`debugSnapshot()` exposes emulator cycle/instruction counts, loaded module,
thread and RPC-server counts, generic service metrics and load diagnostics.
The runtime debugger renders these in the **IOP/SIF** tab.

Build standalone tests with:

```sh
cmake -S iop -B out/build/iop-tests -DPS2X_IOP_BUILD_TESTS=ON
cmake --build out/build/iop-tests
ctest --test-dir out/build/iop-tests --output-on-failure
```

The suites cover IRX execution, RPC, imports, version resolution and generic
HLE compatibility. `zx-tests` also covers runtime SIF RPC/DMA integration.
