// Debug helper: the listing from an address on (x86, variable length), with the function each
// instruction is in. Args: <addr>[:<addr>...] <instructions>
// @category Z-Axis
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
public class XbeDump extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        int n = Integer.parseInt(a[1]);
        for (String s : a[0].split(":")) {
            Address c = toAddr(Long.decode(s));
            println("==== " + c);
            Instruction in = getInstructionContaining(c);
            if (in == null) in = getInstructionAfter(c);
            for (int i = 0; i < n && in != null; i++, in = in.getNext()) {
                Function f = getFunctionContaining(in.getAddress());
                println(in.getAddress() + " " + (f == null ? "-" : f.getName()) + "  " + in);
            }
        }
    }
}
