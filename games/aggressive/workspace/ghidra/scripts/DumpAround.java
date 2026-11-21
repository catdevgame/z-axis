// Debug helper: print instructions around addresses (args: addr [before] [after]).
// @category Z-Axis
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
public class DumpAround extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        int before = Integer.parseInt(a[1]), after = Integer.parseInt(a[2]);
        for (String s : a[0].split(":")) {
            Address c = toAddr(Long.decode(s));
            println("==== " + c);
            for (long o = -4L * before; o <= 4L * after; o += 4) {
                Address x = c.add(o);
                Instruction in = getInstructionAt(x);
                Function f = getFunctionAt(x);
                println((f != null ? "[" + f.getName() + "] " : "") + x + "  " + (in == null ? "??" : in.toString()) +
                    (in != null && in.getReferencesFrom().length > 0 ? "   -> " + in.getReferencesFrom()[0].getToAddress() : ""));
            }
        }
    }
}
