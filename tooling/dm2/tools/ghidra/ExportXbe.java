// Writes what the Xbox pairing (tools/xmatch.py) needs from an analysed XBE, one JSON object per
// function per line: entry, end, name, section, instruction count, and in instruction order the
// calls (offset, target, or -1 for an indirect call; tail jumps to another function count), the data references (offset, instruction
// text, target), the immediates and the memory displacements (struct offsets). Plus one "#strings"
// line: every string Ghidra defined, by address.
// Args: <out.jsonl> [registers]: registers (comma separated) whose displacements are not struct
// offsets, ESP,EBP by default (gc_pipeline.sh passes PowerPC's r1,r2,r13: stack and small data).
// A program the readable stage renamed (ApplyReadable.java) keeps the name the analysis gave as "n"
// (from its ReadableOriginalName property map, so the pairing reads what it always read) and adds
// the readable name as "rn".
// @category Z-Axis
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.util.StringPropertyMap;
import java.io.PrintWriter;

public class ExportXbe extends GhidraScript {
    static String q(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c < 0x20 || c > 0x7E) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Listing L = currentProgram.getListing();
        StringPropertyMap orig = currentProgram.getUsrPropertyManager().getStringPropertyMap("ReadableOriginalName");
        java.util.Set<String> skip = new java.util.HashSet<>(java.util.Arrays.asList(
                (a.length > 1 ? a[1] : "ESP,EBP").split(",")));
        int nf = 0;
        try (PrintWriter w = new PrintWriter(a[0])) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                if (f.isExternal() || f.isThunk() && f.getBody().getNumAddresses() == 0) continue;
                Address e = f.getEntryPoint();
                MemoryBlock blk = currentProgram.getMemory().getBlock(e);
                StringBuilder calls = new StringBuilder(), refs = new StringBuilder(),
                        imms = new StringBuilder(), disp = new StringBuilder(), mn = new StringBuilder();
                int ni = 0;
                long end = e.getOffset();
                for (Instruction ins : L.getInstructions(f.getBody(), true)) {
                    ni++;
                    long off = ins.getAddress().getOffset();
                    end = Math.max(end, off + ins.getLength());
                    String m = ins.getMnemonicString();
                    if (mn.length() > 0) mn.append(' ');
                    mn.append(m);
                    boolean isCall = ins.getFlowType().isCall();
                    boolean hadCall = false;
                    for (Reference r : ins.getReferencesFrom()) {
                        RefType t = r.getReferenceType();
                        long to = r.getToAddress().getOffset();
                        if (t.isCall()) {
                            if (calls.length() > 0) calls.append(',');
                            calls.append("[").append(off - e.getOffset()).append(',').append(to).append(']');
                            hadCall = true;
                        } else if (t.isJump() && !t.isConditional()) {
                            // a tail call: a jump to another function's entry
                            Function g = getFunctionAt(r.getToAddress());
                            if (g != null && !g.equals(f)) {
                                if (calls.length() > 0) calls.append(',');
                                calls.append("[").append(off - e.getOffset()).append(',').append(to).append(']');
                            }
                        } else if (t.isData() || t.isRead() || t.isWrite()) {
                            if (r.getToAddress().isMemoryAddress()) {
                                if (refs.length() > 0) refs.append(',');
                                refs.append("[").append(off - e.getOffset()).append(',').append(q(ins.toString())).append(',').append(to).append(']');
                            }
                        }
                    }
                    if (isCall && !hadCall) {
                        if (calls.length() > 0) calls.append(',');
                        calls.append("[").append(off - e.getOffset()).append(",-1]");
                    }
                    for (int i = 0; i < ins.getNumOperands(); i++) {
                        int ty = ins.getOperandType(i);
                        Object[] objs = ins.getOpObjects(i);
                        boolean mem = (ty & OperandType.DYNAMIC) != 0 || (ty & OperandType.INDIRECT) != 0;
                        for (Object o : objs) {
                            if (!(o instanceof Scalar)) continue;
                            long v = ((Scalar) o).getUnsignedValue() & 0xFFFFFFFFL;
                            if (mem) {
                                // [reg+disp]: a struct or stack offset (absolute addresses are refs)
                                boolean hasReg = false;
                                for (Object p : objs) if (p instanceof ghidra.program.model.lang.Register) {
                                    String rn = ((ghidra.program.model.lang.Register) p).getName();
                                    if (!skip.contains(rn)) hasReg = true;
                                }
                                if (hasReg && v < 0x100000) {
                                    if (disp.length() > 0) disp.append(',');
                                    disp.append(v);
                                }
                            } else if ((ty & OperandType.SCALAR) != 0 && !isCall) {
                                if (imms.length() > 0) imms.append(',');
                                imms.append(v);
                            }
                        }
                    }
                }
                String n = f.getName(true), on = orig == null ? null : orig.getString(e);
                w.println("{\"a\":" + e.getOffset() + ",\"e\":" + end + ",\"n\":" + q(on != null ? on : n)
                        + (on != null ? ",\"rn\":" + q(n) : "")
                        + ",\"sec\":" + q(blk == null ? "?" : blk.getName()) + ",\"ni\":" + ni
                        + ",\"calls\":[" + calls + "],\"refs\":[" + refs + "],\"imms\":[" + imms
                        + "],\"disp\":[" + disp + "],\"mn\":" + q(mn.toString()) + "}");
                nf++;
            }
            // strings
            StringBuilder sb = new StringBuilder("{\"#strings\":{");
            boolean first = true;
            for (Data d : L.getDefinedData(true)) {
                if (!StringDataInstance.isString(d)) continue;
                Object v = d.getValue();
                if (v == null) continue;
                if (!first) sb.append(',');
                first = false;
                sb.append('"').append(d.getAddress().getOffset()).append("\":").append(q(v.toString()));
            }
            w.println(sb.append("}}"));
        }
        println("ExportXbe wrote " + nf + " functions");
    }
}
