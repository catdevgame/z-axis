// Debug helper: print body ranges and flow into far chunks for the given function entry addresses.
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
public class DumpBodies extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Function f = getFunctionAt(toAddr(Long.decode(a)));
            println("FN " + f.getName() + " body " + f.getBody().getNumAddresses());
            for (AddressRange r : f.getBody()) {
                println("  range " + r);
                Instruction first = getInstructionAt(r.getMinAddress());
                for (Reference ref : getReferencesTo(r.getMinAddress())) {
                    println("    ref from " + ref.getFromAddress() + " " + ref.getReferenceType() + " in " +
                        getFunctionContaining(ref.getFromAddress()));
                }
                Instruction last = getInstructionContaining(r.getMaxAddress());
                println("    first " + first + " | last " + last);
            }
        }
    }
}
