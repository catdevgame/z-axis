// Debug helper: list functions marked non-returning, with call counts and their last instruction.
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
public class ListNoReturn extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.hasNoReturn()) {
                continue;
            }
            int calls = 0;
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                if (r.getReferenceType().isCall()) {
                    calls++;
                }
            }
            println("NORET " + f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses() +
                " calls " + calls + " last " + getInstructionContaining(f.getBody().getMaxAddress()));
        }
    }
}
