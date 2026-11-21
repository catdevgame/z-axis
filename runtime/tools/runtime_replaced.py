#!/usr/bin/env python3
"""The functions of a generated folder that the runtime replaces: their generated body is only a call of the
runtime's own implementation, with no instruction of the game's left. Two forms:

  - a library function the runtime is: only a call of ps2_stubs::NAME or ps2_syscalls::NAME, as the recompiler
    writes it (libdma's sceDmaSend, libkernl's EnableDmac);
  - a kernel call wrapper, the runtime being the kernel: only the call number loaded, the syscall instruction and
    the return (`addiu $v1, $zero, N; syscall; jr $ra`, libkernl's SetSyscall, its TLB set-up's GetEntryAddress),
    which the generated code runs as runtime->handleSyscall (sce/not_ported.tsv, "kernel call wrapper").

    runtime_replaced.py GENERATED_DIR [--names]   one address a line (0x%08x), sorted; --names adds the callee
                                                  (ps2_stubs::NAME, ps2_syscalls::NAME or syscall 0xN)

game.cmake leaves these out of a corpus game's record-only registrations (ZX_GAME_CORPUS): such a function is
not the game's code, has no readable form (sce/not_ported.tsv, "the runtime replaces it", "the runtime is the
kernel"), and its replay would run the runtime's code on a recording that never saw what it reads or writes: the
stub's and the kernel's guest accesses are the host's, outside the trace, and the kernel's state (the syscall
table's handlers, threads, semaphores) is the runtime's, not guest RAM. zx-fncheck's runtime has no memory of its
own (BMX XXX's sceDmaSend crashed the checker on 36 files) nor the game's kernel state (BMX XXX's SetSyscall wrote
its table's mirror outside the recording on 74 files, the TLB set-up's syscall 0x5B read it on 37). A caller's
recording keeps what such a call changed as its callee's effects, so the callers are judged as before.
"""
import os
import re
import sys

FILE = re.compile(r'_0x([0-9a-f]+)\.cpp$')
# The whole body: the return address taken, then the one call (an optional _DEBUG log entry before them).
BODY = re.compile(r'\{\s*(?:#ifdef _DEBUG\s+PS_LOG_ENTRY\("[^"]*"\);\s+#endif\s+)?'
                  r'ctx->pc = getRegU32\(ctx, 31\);\s*'
                  r'(ps2_(?:stubs|syscalls)::\w+)\(rdram, ctx, runtime\);\s*\}')
DEFINITION = re.compile(r'\bvoid \w+\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\)\s*')
# A kernel call wrapper's instructions, as the generated code's comments give them (address: word, then the
# disassembly), and its one call of the runtime's kernel.
INSTRUCTION = re.compile(r'^\s*// 0x([0-9a-f]+): 0x[0-9a-f]+\s+(.*?)\s*$', re.M)
LOAD_NUMBER = re.compile(r'addiu\s+\$v1, \$zero, (-?0x[0-9A-Fa-f]+|-?\d+)$')
KERNEL_CALL = re.compile(r'\bruntime->handleSyscall\(rdram, ctx, 0x0u\);')
RUNTIME_CODE = re.compile(r'\bps2_stubs::|\bps2_syscalls::|\bruntime->')
SMALL = 4096  # bytes: a replaced function's file is under 1 KB; a function with instructions is larger or has them


def kernel_call_number(text):
    """The call number of a kernel call wrapper's body, else None."""
    instructions = {}
    for m in INSTRUCTION.finditer(text):
        instructions.setdefault(int(m.group(1), 16), ' '.join(m.group(2).split()))
    words = [instructions[a] for a in sorted(instructions)]
    if len(words) != 3 or words[1] != 'syscall 0' or words[2] != 'jr $ra':
        return None
    number = LOAD_NUMBER.match(words[0])
    if not number or len(KERNEL_CALL.findall(text)) != 1:
        return None
    # No other runtime code than the kernel call (and the return's dispatch, PS2X_STRICT_RETURN_DIAGNOSTICS).
    others = [m for m in RUNTIME_CODE.finditer(text)
              if not text.startswith('runtime->handleSyscall(', m.start())
              and not text.startswith('runtime->dispatchGuestBranch(', m.start())]
    if others:
        return None
    return int(number.group(1), 0)


def replaced(folder):
    out = []
    for name in os.listdir(folder):
        m = FILE.search(name)
        if not m:
            continue
        path = os.path.join(folder, name)
        if os.path.getsize(path) > SMALL:
            continue
        with open(path, errors='replace') as f:
            text = f.read()
        definitions = DEFINITION.findall(text)
        if len(definitions) != 1:
            continue
        body = BODY.search(text, text.index(definitions[0]))
        if body:
            out.append((int(m.group(1), 16), body.group(1)))
            continue
        number = kernel_call_number(text)
        if number is not None:
            out.append((int(m.group(1), 16), 'syscall %s0x%X' % ('-' if number < 0 else '', abs(number))))
    return sorted(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    names = '--names' in sys.argv[2:]
    for address, callee in replaced(sys.argv[1]):
        print('0x%08x%s' % (address, '\t' + callee if names else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
