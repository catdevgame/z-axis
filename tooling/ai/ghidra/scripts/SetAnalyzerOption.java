// Sets program analysis options before auto-analysis. Args: "<option name>=<value>" ...
// e.g. "Non-Returning Functions - Discovered=false"
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
public class SetAnalyzerOption extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            int i = a.lastIndexOf('=');
            setAnalysisOption(currentProgram, a.substring(0, i), a.substring(i + 1));
            println("analysis option " + a.substring(0, i) + " = " + a.substring(i + 1));
        }
    }
}
