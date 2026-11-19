// Resolves GCC MIPS jump tables that Ghidra's switch analysis left unresolved:
//   sltiu vX, idx, N ; beq vX, zero, default ; lui/addiu table ; sll ; addu ; lw rT, 0(..) ; jr rT
// For each unresolved "jr reg" (reg != ra) the table address is taken from the lw's
// memory reference and the bound N from the preceding sltiu. COMPUTED_JUMP references
// are added from the jr to every table target, functions that GapFill wrongly created
// on case blocks (no CALL references, inside the owner's region) are removed, and the
// owner's body is recomputed.
// Args: [dry]
// @category PS2Recomp

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.util.*;

public class FixSwitchTables extends GhidraScript {

    private MemoryBlock text;

    private boolean hasCallRef(Address a) {
        for (Reference r : getReferencesTo(a)) {
            if (r.getReferenceType().isCall()) {
                return true;
            }
        }
        return false;
    }

    private boolean inText(Address a) {
        return a != null && text.contains(a);
    }

    @Override
    public void run() throws Exception {
        boolean dry = getScriptArgs().length > 0 && getScriptArgs()[0].equals("dry");
        text = currentProgram.getMemory().getBlock(".text");
        List<Instruction> todo = new ArrayList<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(
            new AddressSet(text.getStart(), text.getEnd()), true);
        while (it.hasNext()) {
            Instruction in = it.next();
            String m = in.getMnemonicString();
            if (!(m.equals("jr") || m.equals("_jr")) || in.getDefaultOperandRepresentation(0).equals("ra")) {
                continue;
            }
            todo.add(in);
        }
        int fixed = 0, partial = 0, viaFallback = 0, onlyKnown = 0, noTable = 0;
        for (Instruction jr : todo) {
            Set<Address> known = new LinkedHashSet<>();
            for (Reference r : jr.getReferencesFrom()) {
                if (r.getReferenceType().isJump() || r.getReferenceType().isComputed()) {
                    known.add(r.getToAddress());
                }
            }
            String reg = jr.getDefaultOperandRepresentation(0);
            Address table = null;
            String idxReg = null;
            Long bound = null;
            Instruction p = jr;
            for (int i = 0; i < 24 && p != null; i++) {
                p = p.getPrevious();
                if (p == null) {
                    break;
                }
                String pm = p.getMnemonicString().replace("_", "");
                if (table == null) {
                    if (pm.equals("lw") && p.getDefaultOperandRepresentation(0).equals(reg)) {
                        for (Reference r : p.getReferencesFrom()) {
                            // Ghidra may attach several refs (base + a resolved entry); the table base is the lowest
                            if (r.isMemoryReference() && !r.isStackReference() &&
                                (table == null || r.getToAddress().compareTo(table) < 0)) {
                                table = r.getToAddress();
                            }
                        }
                        if (table == null) {
                            break;
                        }
                    }
                    continue;
                }
                if (idxReg == null && pm.equals("sll")) {
                    Scalar sh = p.getScalar(2);
                    if (sh != null && sh.getUnsignedValue() == 2) {
                        idxReg = p.getDefaultOperandRepresentation(1);
                    }
                    continue;
                }
                if (idxReg != null && pm.equals("sltiu") && p.getDefaultOperandRepresentation(1).equals(idxReg)) {
                    Scalar sc = p.getScalar(2);
                    if (sc != null) {
                        bound = sc.getUnsignedValue();
                    }
                    break;
                }
            }
            Function owner = getFunctionContaining(jr.getAddress());
            List<Address> targets = new ArrayList<>();
            if (table != null && owner != null && bound != null && bound > 0 && bound <= 1024) {
                for (int i = 0; i < bound; i++) {
                    long v = getInt(table.add(4L * i)) & 0xffffffffL;
                    Address t = toAddr(v);
                    if (!inText(t) || v % 4 != 0 || getInstructionAt(t) == null) {
                        targets.clear();
                        break;
                    }
                    targets.add(t);
                }
            } else if (table != null && owner != null) {
                // no bound check found: take entries while they point at code near the owner and
                // until another reference marks the start of different data
                long lo = owner.getEntryPoint().getOffset(), hi = lo + 0x8000;
                for (int i = 0; i < 256; i++) {
                    Address ea = table.add(4L * i);
                    if (i > 0 && (getReferencesTo(ea).length > 0)) {
                        break;
                    }
                    long v = getInt(ea) & 0xffffffffL;
                    if (v % 4 != 0 || v < lo || v >= hi || getInstructionAt(toAddr(v)) == null) {
                        break;
                    }
                    targets.add(toAddr(v));
                }
                if (!targets.isEmpty()) {
                    viaFallback++;
                }
            }
            if (targets.isEmpty()) {
                if (known.isEmpty()) {
                    noTable++;
                    println("SKIP " + jr.getAddress() + " (no table) table=" + table + " idx=" + idxReg +
                        " bound=" + bound);
                } else {
                    onlyKnown++;
                    println("KEEP " + jr.getAddress() + " Ghidra targets " + known.size() + " (table=" + table +
                        " idx=" + idxReg + " bound=" + bound + ")");
                    cleanup(jr, new ArrayList<>(known), dry, false);
                }
                continue;
            }
            Set<Address> missing = new LinkedHashSet<>(targets);
            missing.removeAll(known);
            if (!known.isEmpty() && !missing.isEmpty()) {
                partial++;
            }
            if (!dry) {
                for (Address t : missing) {
                    currentProgram.getReferenceManager().addMemoryReference(jr.getAddress(), t,
                        RefType.COMPUTED_JUMP, SourceType.USER_DEFINED, 0);
                }
            }
            if (!missing.isEmpty()) {
                println("FIX " + jr.getAddress() + " in " + owner.getName() + " table " + table + " entries " +
                    targets.size() + " (Ghidra had " + known.size() + ", added " + missing.size() + ")" +
                    (bound == null ? " [bound inferred]" : ""));
                fixed++;
            }
            Set<Address> all = new LinkedHashSet<>(targets);
            all.addAll(known);
            cleanup(jr, new ArrayList<>(all), dry, !missing.isEmpty());
        }
        println("FixSwitchTables: " + todo.size() + " computed jumps; added targets to " + fixed + " (" + partial +
            " were partially resolved by Ghidra, " + viaFallback + " with inferred bound); " + onlyKnown +
            " kept Ghidra's targets; " + noTable + " without a recognisable table; removed " + removedFns +
            " fake functions");
    }

    private int removedFns = 0;

    private void cleanup(Instruction jr, List<Address> targets, boolean dry, boolean refsAdded) throws Exception {
        boolean changed = refsAdded;
        Function owner = getFunctionContaining(jr.getAddress());
        // region boundary: first function after the jr that is really called from somewhere
        Address boundary = text.getEnd();
        Function nf = getFunctionAfter(jr.getAddress());
        while (nf != null) {
            String nn = nf.getName();
            boolean anon = nn.startsWith("FUN_") || nn.startsWith("thunk_FUN_") || nn.startsWith("caseD_") ||
                nn.startsWith("switchD_");
            if (hasCallRef(nf.getEntryPoint()) || !anon) {
                boundary = nf.getEntryPoint();
                break;
            }
            nf = getFunctionAfter(nf.getEntryPoint());
        }
        if (owner == null) {
            return;
        }
        // flood fill from the case targets (no calls, stay below boundary and after owner entry)
        Deque<Address> work = new ArrayDeque<>(targets);
        Set<Address> seen = new HashSet<>();
        while (!work.isEmpty()) {
            Address a = work.pop();
            if (!seen.add(a) || a.compareTo(owner.getEntryPoint()) < 0 || a.compareTo(boundary) >= 0) {
                continue;
            }
            Function f = getFunctionAt(a);
            if (f != null && !f.equals(owner) && !hasCallRef(a)) {
                println("  " + (dry ? "would remove" : "remove") + " fake function " + f.getName() +
                    " (case block of " + owner.getName() + " jr " + jr.getAddress() + ")");
                if (dry) {
                    continue;
                }
                removeFunction(f);
                removedFns++;
                changed = true;
            }
            Instruction in = getInstructionAt(a);
            if (in == null) {
                continue;
            }
            boolean isJ = in.getMnemonicString().replace("_", "").equals("j");
            for (Address t : in.getFlows()) {
                // "j" into an existing function entry is a tail call, not a case block
                if (!in.getFlowType().isCall() && !(isJ && getFunctionAt(t) != null)) {
                    work.push(t);
                }
            }
            // delay slots are never function entries and their own fallthrough would run past a
            // terminating jr/j, so they are not followed; a branch's getFallThrough() already skips them
            if (!in.isInDelaySlot() && in.getFallThrough() != null) {
                work.push(in.getFallThrough());
            }
        }
        if (changed && !dry) {
            CreateFunctionCmd.fixupFunctionBody(currentProgram, owner, monitor);
        }
    }
}
