// GCC cross-jumping makes some functions branch to a "jr ra; nop" epilogue that sits
// inside/after an unrelated function far away. Ghidra then puts that remote chunk in
// the caller's body, so the PS2Recomp export range [entry, max+1) would swallow every
// function in between. This script detaches body ranges that are separated from the
// entry's range by another function's entry point and turns each into its own function
// (a tail-jump target), keeping each exported range contiguous.
// Args: [dry]
// @category Z-Axis

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;

import java.util.ArrayList;
import java.util.List;

public class FixDetachedChunks extends GhidraScript {
    @Override
    public void run() throws Exception {
        boolean dry = getScriptArgs().length > 0 && getScriptArgs()[0].equals("dry");
        List<Function> fns = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            fns.add(f);
        }
        int detached = 0;
        List<Address> newEntries = new ArrayList<>();
        for (Function f : fns) {
            AddressSetView body = f.getBody();
            if (body.getNumAddressRanges() < 2) {
                continue;
            }
            AddressRange entryRange = body.getRangeContaining(f.getEntryPoint());
            AddressSet remove = new AddressSet();
            for (AddressRange r : body) {
                if (r.equals(entryRange)) {
                    continue;
                }
                Address lo = r.getMinAddress().compareTo(entryRange.getMaxAddress()) > 0 ? entryRange.getMaxAddress()
                    : r.getMaxAddress();
                Address hi = r.getMinAddress().compareTo(entryRange.getMaxAddress()) > 0 ? r.getMinAddress()
                    : entryRange.getMinAddress();
                Function between = getFunctionAfter(lo);
                if (between != null && between.getEntryPoint().compareTo(hi) < 0) {
                    remove.add(r);
                    println((dry ? "WOULD DETACH " : "DETACH ") + r + " from " + f.getName() + " (" +
                        between.getName() + " lies in between)");
                }
            }
            if (remove.isEmpty() || dry) {
                detached += remove.getNumAddressRanges();
                continue;
            }
            f.setBody(body.subtract(remove));
            for (AddressRange r : remove) {
                newEntries.add(r.getMinAddress());
                detached++;
            }
        }
        for (Address a : newEntries) {
            if (getFunctionAt(a) == null) {
                CreateFunctionCmd cmd = new CreateFunctionCmd(a);
                if (!cmd.applyTo(currentProgram, monitor)) {
                    println("FAILED to create function at " + a + ": " + cmd.getStatusMsg());
                }
            }
        }
        println("FixDetachedChunks: detached " + detached + " remote body ranges");
    }
}
