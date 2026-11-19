// Merges "fragment" functions back into their owner. Ghidra sometimes starts a function at
// a basic block that is only reached by PC-relative branches (b/beq/bne...) from one other
// function, typically the shared epilogue / loop head of -O0 code (s8 frame pointer). For
// recompilation such a block must stay inside its owner.
// A function F is merged into O when: F has an anonymous name, no CALL and no DATA
// references, and every branch into F's body from outside comes from O (ignoring
// unreferenced dead-code functions) via a non-"j" branch instruction.
// Args: [dry]
// @category PS2Recomp

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.util.*;

public class MergeFragments extends GhidraScript {

    private static boolean anonymous(String n) {
        return n.startsWith("FUN_") || n.startsWith("thunk_FUN_") || n.startsWith("caseD_") ||
            n.startsWith("switchD_") || n.startsWith("LAB_");
    }

    private boolean isDead(Function f) {
        return getReferencesTo(f.getEntryPoint()).length == 0 &&
            !currentProgram.getSymbolTable().isExternalEntryPoint(f.getEntryPoint());
    }

    // true if a function starts at a and is called or has its address taken
    private boolean calledOrTaken(Address a) {
        if (getFunctionAt(a) == null) {
            return false;
        }
        for (Reference r : getReferencesTo(a)) {
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
        int merged = 0;
        for (int pass = 1; pass <= 20; pass++) {
            int passMerged = 0;
            List<Function> fns = new ArrayList<>();
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                fns.add(f);
            }
            // unreferenced 8-byte "b/j back into the previous function" dead-code stubs: Ghidra makes
            // them thunks, which forces a function at the branch target and splits the real owner
            for (Function f : fns) {
                if (!anonymous(f.getName()) || !isDead(f) || f.getBody().getNumAddresses() != 8 ||
                    pointedToByData(f.getEntryPoint())) {
                    continue;
                }
                Instruction in = getInstructionAt(f.getEntryPoint());
                Function before = getFunctionBefore(f.getEntryPoint());
                if (in == null || before == null) {
                    continue;
                }
                String m = in.getMnemonicString().replace("_", "");
                if (!(m.equals("b") || m.equals("j"))) {
                    continue;
                }
                Address[] fl = in.getFlows();
                if (fl.length == 1 && fl[0].compareTo(before.getEntryPoint()) > 0 &&
                    fl[0].getOffset() < f.getEntryPoint().getOffset() + 0x1000 && !calledOrTaken(fl[0])) {
                    println((dry ? "WOULD REMOVE" : "REMOVE") + " dead branch stub " + f.getName() + " -> " + fl[0]);
                    passMerged++;
                    if (!dry) {
                        removeFunction(f);
                    }
                }
            }
            for (Function f : fns) {
                if (f.isDeleted() || !anonymous(f.getName()) || pointedToByData(f.getEntryPoint()) ||
                    currentProgram.getSymbolTable().isExternalEntryPoint(f.getEntryPoint())) {
                    continue;
                }
                boolean disqualified = false;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    Function src = getFunctionContaining(r.getFromAddress());
                    if (src != null && !src.equals(f) && isDead(src) && !r.getReferenceType().isCall()) {
                        continue; // e.g. an unreferenced "b <f>" thunk left behind as dead code
                    }
                    if (r.getReferenceType().isCall() || !r.getReferenceType().isFlow() ||
                        getInstructionAt(r.getFromAddress()) == null) {
                        disqualified = true; // called, or address taken (data / pointer)
                        break;
                    }
                }
                if (disqualified) {
                    continue;
                }
                AddressSetView body = f.getBody();
                Set<Function> owners = new HashSet<>();
                List<Reference> refsIn = new ArrayList<>();
                AddressIterator dests = currentProgram.getReferenceManager().getReferenceDestinationIterator(body, true);
                while (dests.hasNext()) {
                    for (Reference r : getReferencesTo(dests.next())) {
                        refsIn.add(r);
                    }
                }
                int branchRefs = 0;
                for (Reference r : refsIn) {
                    if (disqualified) {
                        break;
                    }
                    Address from = r.getFromAddress();
                    if (body.contains(from) || !r.getReferenceType().isFlow()) {
                        continue;
                    }
                    if (r.getReferenceType().isCall()) {
                        disqualified = true;
                        break;
                    }
                    Instruction in = getInstructionAt(from);
                    Function o = getFunctionContaining(from);
                    if (in == null || o == null) {
                        continue;
                    }
                    if (isDead(o)) {
                        continue;
                    }
                    String m = in.getMnemonicString().replace("_", "");
                    if (m.equals("j") || m.equals("jr")) {
                        disqualified = true; // tail call or computed jump: leave alone
                        break;
                    }
                    owners.add(o);
                    branchRefs++;
                }
                if (disqualified || owners.size() != 1 || branchRefs == 0) {
                    continue;
                }
                Function o = owners.iterator().next();
                if (o.equals(f) || o.getEntryPoint().compareTo(f.getEntryPoint()) > 0) {
                    continue; // fragments are blocks placed after their owner's entry
                }
                println((dry ? "WOULD MERGE " : "MERGE ") + f.getName() + " (" + body.getNumAddresses() +
                    " B) into " + o.getName());
                passMerged++;
                if (!dry) {
                    removeFunction(f);
                    CreateFunctionCmd.fixupFunctionBody(currentProgram, o, monitor);
                }
            }
            merged += passMerged;
            if (passMerged == 0 || dry) {
                break;
            }
        }
        println("MergeFragments: merged " + merged + " fragment functions");
    }
}
