// Functions Ghidra's x86 analysis leaves out of an XBE's .text: code reached only through
// pointers (vtables, callback and state tables in .data/.rdata) and code after the compiler's
// alignment padding (0x90/0xCC runs) between functions. Rounds until nothing new:
//   1. every aligned 32-bit word in a non-executable section that points into .text outside
//      any function, 16-aligned or after padding or a return, at a byte that disassembles,
//      becomes a function;
//   2. in each gap between functions, the first byte after padding becomes a function (not
//      after an instruction that falls through into it).
// A candidate is kept only when its flow disassembles without errors. Then auto-analysis runs
// over the changes. Args: [dry]
// @category PS2Recomp
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;

public class XbeGapFill extends GhidraScript {
    MemoryBlock text;

    boolean inFunction(Address a) {
        return getFunctionContaining(a) != null;
    }

    boolean tryFunction(Address a) throws Exception {
        if (inFunction(a)) return false;
        Listing L = currentProgram.getListing();
        Instruction ins = L.getInstructionAt(a);
        if (ins == null) {
            if (L.getDefinedDataContaining(a) != null) return false;
            DisassembleCommand cmd = new DisassembleCommand(a, null, true);
            if (!cmd.applyTo(currentProgram, monitor)) return false;
            ins = L.getInstructionAt(a);
            if (ins == null) return false;
            // reject flows that ran into bad bytes
            if (currentProgram.getBookmarkManager().getBookmarks(a, "Error").length > 0) return false;
        }
        Function f = createFunction(a, null);
        return f != null;
    }

    @Override
    public void run() throws Exception {
        boolean dry = getScriptArgs().length > 0 && getScriptArgs()[0].equals("dry");
        Memory mem = currentProgram.getMemory();
        text = mem.getBlock(".text");
        long t0 = text.getStart().getOffset(), t1 = text.getEnd().getOffset();
        int total = 0, skipped = 0;
        for (int round = 0; round < 20; round++) {
            int made = 0;
            // 1. pointers from data
            for (MemoryBlock b : mem.getBlocks()) {
                if (b.isExecute() || !b.isInitialized()) continue;
                long s = b.getStart().getOffset(), e = b.getEnd().getOffset();
                for (long p = (s + 3) & ~3L; p + 4 <= e; p += 4) {
                    long v = mem.getInt(toAddr(p)) & 0xFFFFFFFFL;
                    if (v < t0 || v > t1) continue;
                    Address a = toAddr(v);
                    if (inFunction(a)) continue;
                    // a function start: 16-aligned (MSVC pads functions to 16 bytes) or right
                    // after padding or a return; other words in range are mostly integers
                    // (a pointer to the section's first byte has no byte before it)
                    int prev = v > t0 ? mem.getByte(toAddr(v - 1)) & 0xFF : 0xCC;
                    if ((v & 15) != 0 && prev != 0x90 && prev != 0xCC && prev != 0xC3) continue;
                    if (dry) { made++; continue; }
                    if (tryFunction(a)) made++;
                }
            }
            // 2. after padding in gaps
            Address cur = text.getStart();
            while (cur != null && cur.getOffset() < t1) {
                Function f = getFunctionContaining(cur);
                if (f != null) {
                    cur = f.getBody().getMaxAddress().add(1);
                    continue;
                }
                int by = mem.getByte(cur) & 0xFF;
                if (by == 0x90 || by == 0xCC || by == 0x00) { cur = cur.add(1); continue; }
                // the gap's first real byte, when the code before it cannot fall through into it
                // (padding, a return or a jump; after a call it is the rest of a function whose
                // callee was taken as not returning, not a function of its own)
                Instruction before = currentProgram.getListing().getInstructionBefore(cur);
                String bm = before == null ? "" : before.getMnemonicString();
                boolean cut = before != null && before.getMaxAddress().add(1).equals(cur)
                        && !bm.startsWith("RET") && !bm.equals("JMP") && !bm.equals("INT3");
                if (cut) skipped++;
                else if (dry) made++;
                else if (tryFunction(cur)) made++;
                // skip to the next function or the next padding
                Function g = getFunctionAfter(cur);
                Address next = g == null ? null : g.getEntryPoint();
                Address a = cur.add(1);
                while (a.getOffset() < t1 && (next == null || a.compareTo(next) < 0)) {
                    int c = mem.getByte(a) & 0xFF;
                    if ((c == 0x90 || c == 0xCC) && inFunction(a) == false) break;
                    if (inFunction(a)) break;
                    a = a.add(1);
                }
                cur = a;
                if (dry && made > 100000) break;
            }
            total += made;
            println("XbeGapFill round " + round + ": " + made + " functions");
            if (made == 0 || dry) break;
            analyzeChanges(currentProgram);
        }
        println("XbeGapFill done: " + total + " functions" + (dry ? " (dry)" : "") + ", " + skipped
                + " gaps left after code that falls through");
    }
}
