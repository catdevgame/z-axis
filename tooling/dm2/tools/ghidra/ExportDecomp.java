// Writes the decompiled C of every function (or the given entry addresses) to one file.
// Args: <out.c> [addr ...]
// A program the readable stage wrote into (ApplyReadable.java) gets a first line naming that input
// and its SHA-256 (before the first //// FUNCTION line, so readers of the functions skip it).
// @category PS2Recomp
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        List<Function> fs = new ArrayList<>();
        if (a.length > 1) {
            for (int i = 1; i < a.length; i++) {
                Function f = getFunctionContaining(toAddr(Long.decode(a[i])));
                if (f != null) fs.add(f);
            }
        } else {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) fs.add(f);
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(a[0])) {
            ghidra.framework.options.Options info = currentProgram.getOptions(ghidra.program.model.listing.Program.PROGRAM_INFO);
            String rsha = info.getString("Readable input sha256", null);
            if (rsha != null)
                w.println("// readable write-back: " + info.getString("Readable input", "?") + " sha256 " + rsha
                    + ", readable sources " + info.getString("Readable sources", "?"));
            int n = 0;
            for (Function f : fs) {
                if (monitor.isCancelled()) break;
                DecompileResults r = d.decompileFunction(f, 30, monitor);
                w.println("//// FUNCTION " + f.getEntryPoint() + " " + f.getName());
                if (r != null && r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
                else w.println("// decompile failed");
                if (++n % 500 == 0) println("decompiled " + n);
            }
        }
        d.dispose();
        println("ExportDecomp wrote " + fs.size() + " functions");
    }
}
