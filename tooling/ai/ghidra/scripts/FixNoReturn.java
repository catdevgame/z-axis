// Undo wrong "non-returning" marks: any function flagged noreturn whose body contains a
// "jr ra" is cleared, its call sites get their fallthrough back (also any other jal/jalr
// carrying a CALL_RETURN/terminator flow override to a returning function), and fragment functions
// that were created behind those calls (no CALL references, reachable from the caller by
// plain flow) are removed so the caller's body is recomputed in one piece.
// Args: [dry]
// @category PS2Recomp

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

import java.util.*;

public class FixNoReturn extends GhidraScript {

    private boolean hasCallRef(Address a) {
        for (Reference r : getReferencesTo(a)) {
            if (r.getReferenceType().isCall()) {
                return true;
            }
        }
        return false;
    }

    private static boolean anonymous(String n) {
        return n.startsWith("FUN_") || n.startsWith("thunk_FUN_") || n.startsWith("caseD_") ||
            n.startsWith("switchD_") || n.startsWith("LAB_");
    }

    @Override
    public void run() throws Exception {
        boolean dry = getScriptArgs().length > 0 && getScriptArgs()[0].equals("dry");
        Set<Function> callers = new LinkedHashSet<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.hasNoReturn()) {
                continue;
            }
            boolean returns = false;
            InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
            while (ii.hasNext()) {
                Instruction in = ii.next();
                if (in.getMnemonicString().replace("_", "").equals("jr") &&
                    in.getDefaultOperandRepresentation(0).equals("ra")) {
                    returns = true;
                    break;
                }
            }
            if (!returns) {
                println("keep noreturn " + f.getName());
                continue;
            }
            println((dry ? "WOULD CLEAR" : "CLEAR") + " noreturn on " + f.getName());
            if (!dry) {
                f.setNoReturn(false);
            }
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                if (!r.getReferenceType().isCall()) {
                    continue;
                }
                Instruction call = getInstructionAt(r.getFromAddress());
                if (call == null) {
                    continue;
                }
                println("  call site " + call.getAddress() + " flowOverride=" + call.getFlowOverride() +
                    " fallthrough=" + call.getFallThrough());
                if (!dry) {
                    if (call.getFlowOverride() != FlowOverride.NONE) {
                        call.setFlowOverride(FlowOverride.NONE);
                    }
                    if (call.isFallThroughOverridden()) {
                        call.clearFallThroughOverride();
                    }
                }
                Function c = getFunctionContaining(call.getAddress());
                if (c != null) {
                    callers.add(c);
                }
            }
        }
        // any other call instruction whose flow was overridden although its target returns
        int overrides = 0;
        InstructionIterator all = currentProgram.getListing().getInstructions(true);
        while (all.hasNext()) {
            Instruction in = all.next();
            if (in.getFlowOverride() == FlowOverride.NONE) {
                continue;
            }
            String m = in.getMnemonicString().replace("_", "");
            if (!(m.equals("jal") || m.equals("jalr"))) {
                continue;
            }
            Function target = null;
            for (Reference r : in.getReferencesFrom()) {
                if (r.getReferenceType().isCall()) {
                    target = getFunctionAt(r.getToAddress());
                }
            }
            if (target != null && target.hasNoReturn() && !dry) {
                continue;
            }
            println("  " + (dry ? "would clear" : "clear") + " flow override " + in.getFlowOverride() + " on " +
                in.getAddress() + " " + in + " (target " + (target == null ? "?" : target.getName()) + ")");
            overrides++;
            if (!dry) {
                in.setFlowOverride(FlowOverride.NONE);
                Function c = getFunctionContaining(in.getAddress());
                if (c != null) {
                    callers.add(c);
                }
            }
        }
        int removed = 0;
        for (Function owner : callers) {
            Address boundary = currentProgram.getMemory().getBlock(".text").getEnd();
            Function nf = getFunctionAfter(owner.getEntryPoint());
            while (nf != null) {
                if (hasCallRef(nf.getEntryPoint()) || !anonymous(nf.getName())) {
                    boundary = nf.getEntryPoint();
                    break;
                }
                nf = getFunctionAfter(nf.getEntryPoint());
            }
            Deque<Address> work = new ArrayDeque<>();
            work.push(owner.getEntryPoint());
            Set<Address> seen = new HashSet<>();
            while (!work.isEmpty()) {
                Address a = work.pop();
                if (!seen.add(a) || a.compareTo(owner.getEntryPoint()) < 0 || a.compareTo(boundary) >= 0) {
                    continue;
                }
                Function f = getFunctionAt(a);
                if (f != null && !f.equals(owner) && !hasCallRef(a)) {
                    println("  " + (dry ? "would remove" : "remove") + " fragment " + f.getName() + " of " +
                        owner.getName());
                    if (!dry) {
                        removeFunction(f);
                        removed++;
                    }
                }
                Instruction in = getInstructionAt(a);
                if (in == null) {
                    continue;
                }
                boolean isJ = in.getMnemonicString().replace("_", "").equals("j");
                if (!in.getFlowType().isCall()) {
                    for (Address t : in.getFlows()) {
                        if (!(isJ && getFunctionAt(t) != null)) {
                            work.push(t);
                        }
                    }
                }
                Address ft = in.getFallThrough();
                if (ft == null && in.getFlowType().isCall() && !dry) {
                    ft = in.getMaxAddress().add(1 + 4); // past the delay slot
                }
                if (!in.isInDelaySlot() && ft != null) {
                    work.push(ft);
                }
            }
            if (!dry) {
                CreateFunctionCmd.fixupFunctionBody(currentProgram, owner, monitor);
            }
        }
        println("FixNoReturn: call overrides cleared " + overrides + " (plus noreturn call sites), callers repaired " +
            callers.size() + ", fragments removed " + removed);
    }
}
