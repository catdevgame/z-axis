// Applies SCE SDK function names found by sce_scan.py (a port of PS2Recomp's
// sce_symbol_scanner) as primary labels + entry points, so auto-analysis creates
// functions there and the PS2Recomp export can classify them as stubs.
// Arg 0: path to sce-matches.csv (Address,Size,Name,Library,Hash)
// @category PS2Recomp

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

import java.io.BufferedReader;
import java.io.FileReader;

public class ApplySceSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ApplySceSymbols.java <sce-matches.csv>");
            return;
        }
        SymbolTable st = currentProgram.getSymbolTable();
        int applied = 0, renamed = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
            String line = r.readLine(); // header
            while ((line = r.readLine()) != null) {
                String[] f = line.split(",");
                if (f.length < 4) {
                    continue;
                }
                Address a = toAddr(Long.decode(f[0]));
                String name = f[2];
                Function fn = getFunctionAt(a);
                if (fn != null) {
                    fn.setName(name, SourceType.IMPORTED);
                    renamed++;
                } else {
                    Symbol s = st.createLabel(a, name, SourceType.IMPORTED);
                    s.setPrimary();
                    st.addExternalEntryPoint(a);
                    if (getInstructionAt(a) == null) {
                        new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                    }
                    createFunction(a, name);
                }
                setPlateComment(a, "SCE SDK match: " + f[3] + "::" + name + " (sce-symbol-scanner hash " +
                    (f.length > 4 ? f[4] : "?") + ")");
                applied++;
            }
        }
        println("Applied " + applied + " SCE names (" + renamed + " renamed existing functions)");
    }
}
