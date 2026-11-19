// Recovers EE functions that auto-analysis missed (typically only reached through
// function pointers / vtables). GCC lays functions out back to back in .text, so a
// gap that starts right after a terminated function (jr/j/b + delay slot, possibly
// followed by zero padding) and disassembles cleanly is treated as a new function.
// Gaps that follow a non-terminated function are only logged (possible bad
// non-returning call or unresolved jump table), not converted.
// Args: [dry]   -> only log the decisions
// @category PS2Recomp

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;

public class GapFill extends GhidraScript {

    private boolean isTerminal(Address delaySlotAddr) {
        // eret (no delay slot) or similar terminal instruction as the last instruction
        Instruction last = getInstructionAt(delaySlotAddr);
        if (last != null && last.getFlowType().isTerminal() && last.getDelaySlotDepth() == 0) {
            return true;
        }
        // instruction before the delay slot must be an unconditional, non-call transfer
        Instruction br = getInstructionAt(delaySlotAddr.subtract(4));
        if (br == null) {
            return false;
        }
        FlowType ft = br.getFlowType();
        if (ft.isTerminal()) {
            return true;
        }
        return ft.isJump() && ft.isUnConditional() && !ft.isCall();
    }

    // true if a function starts at a and is called or has its address taken
    private boolean calledOrTaken(Address a) {
        if (getFunctionAt(a) == null) {
            return false;
        }
        for (ghidra.program.model.symbol.Reference r : getReferencesTo(a)) {
            if (r.getReferenceType().isCall() || getInstructionAt(r.getFromAddress()) == null) {
                return true;
            }
        }
        return false;
    }


    // every aligned 32-bit word stored in initialized, non-executable memory (raw pointer candidates,
    // e.g. vtables Ghidra did not turn into references)
    private java.util.Set<Long> dataWords;

    private boolean pointedToByData(Address a) throws Exception {
        if (dataWords == null) {
            dataWords = new java.util.HashSet<>();
            for (ghidra.program.model.mem.MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isInitialized() || !b.isLoaded() || b.isExecute()) {
                    continue;
                }
                byte[] bytes = new byte[(int) b.getSize()];
                b.getBytes(b.getStart(), bytes);
                long base = b.getStart().getOffset();
                for (int i = (int) ((4 - (base & 3)) & 3); i + 4 <= bytes.length; i += 4) {
                    dataWords.add((bytes[i] & 0xffL) | ((bytes[i + 1] & 0xffL) << 8) |
                        ((bytes[i + 2] & 0xffL) << 16) | ((bytes[i + 3] & 0xffL) << 24));
                }
            }
        }
        return dataWords.contains(a.getOffset());
    }

    @Override
    public void run() throws Exception {
        boolean dry = getScriptArgs().length > 0 && getScriptArgs()[0].equals("dry");
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        int created = 0, tails = 0, failed = 0, dead = 0;
        for (int pass = 1; pass <= 40; pass++) {
            AddressSet covered = new AddressSet();
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                covered.add(f.getBody());
            }
            AddressSet gaps = new AddressSet(text.getStart(), text.getEnd()).subtract(covered);
            int passCreated = 0;
            for (AddressRange r : gaps) {
                Address s = r.getMinAddress();
                if (s.getOffset() % 4 != 0) {
                    continue;
                }
                // skip zero padding / nops
                while (s.compareTo(r.getMaxAddress()) < 0 && getInt(s) == 0) {
                    s = s.add(4);
                }
                if (s.compareTo(r.getMaxAddress()) > 0 || getInt(s) == 0) {
                    continue;
                }
                Address before = r.getMinAddress().subtract(4);
                Function prev = getFunctionContaining(before);
                boolean afterTerminal = prev != null && isTerminal(before);
                if (prev == null && r.getMinAddress().equals(text.getStart())) {
                    afterTerminal = true;
                }
                if (!afterTerminal) {
                    if (pass == 1) {
                        tails++;
                        println("TAIL? gap " + r.getMinAddress() + "-" + r.getMaxAddress() + " after " +
                            (prev == null ? "<no function>" : prev.getName()) + " last insn " +
                            getInstructionAt(before));
                    }
                    continue;
                }
                // dead code: a lone "b"/"j" back into the previous function (left behind by -O0 code
                // after an unconditional branch). Creating a function there makes Ghidra treat it as a
                // thunk and re-split the previous function at the branch target, so leave it alone.
                long w = getInt(s) & 0xffffffffL;
                long tgt = -1;
                if ((w >>> 16) == 0x1000L) { // beq zero,zero,off == b off
                    tgt = s.getOffset() + 4 + (((short) (w & 0xffff)) << 2);
                } else if ((w >>> 26) == 2) { // j
                    tgt = (s.getOffset() & 0xf0000000L) | ((w & 0x3ffffffL) << 2);
                }
                if (tgt >= 0 && prev != null && tgt > prev.getEntryPoint().getOffset() && tgt < s.getOffset() + 0x1000 &&
                    !calledOrTaken(toAddr(tgt)) && !pointedToByData(s)) {
                    if (pass == 1) {
                        dead++;
                        println("DEAD branch stub at " + s + " -> " + Long.toHexString(tgt) + " (after " +
                            prev.getName() + ")");
                    }
                    s = s.add(8); // skip the stub and its delay slot, then any padding
                    while (s.compareTo(r.getMaxAddress()) < 0 && getInt(s) == 0) {
                        s = s.add(4);
                    }
                    if (s.compareTo(r.getMaxAddress()) > 0 || getInt(s) == 0) {
                        continue;
                    }
                }
                if (dry) {
                    println("NEW? " + s + " (gap " + r.getLength() + " B after " +
                        (prev == null ? "-" : prev.getName()) + ")");
                    continue;
                }
                if (getInstructionAt(s) == null) {
                    DisassembleCommand dc = new DisassembleCommand(s, new AddressSet(s, r.getMaxAddress()), true);
                    dc.applyTo(currentProgram, monitor);
                    if (getInstructionAt(s) == null) {
                        failed++;
                        println("FAILED disassembly at " + s);
                        continue;
                    }
                }
                CreateFunctionCmd cmd = new CreateFunctionCmd(s);
                if (cmd.applyTo(currentProgram, monitor)) {
                    passCreated++;
                    Function nf = getFunctionAt(s);
                    println("CREATED " + nf.getName() + " size " + nf.getBody().getNumAddresses() + " (gap " +
                        r.getLength() + " B)");
                } else {
                    failed++;
                    println("FAILED createFunction at " + s + ": " + cmd.getStatusMsg());
                }
            }
            created += passCreated;
            println("pass " + pass + ": created " + passCreated);
            if (passCreated == 0 || dry) {
                break;
            }
        }
        println("GapFill done: created " + created + ", tails left alone " + tails + ", dead branch stubs left alone " +
            dead + ", failed " + failed);
    }
}
