// Pre-analysis fixups for SLUS_203.27 (Aggressive Inline, PS2 EE):
//  - .vutext and the .DVP.overlay.* sections hold VU microcode, not EE code, and the EE
//    extension adds vu0/vu1/iop_ram placeholder blocks. Clear the execute bit on every block but .text so auto-analysis (and the PS2Recomp export, which
//    walks executable blocks) never disassembles them as R5900 instructions.
//  - Report the memory map so the log documents what was imported.
// @category PS2Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class PrepareEEProgram extends GhidraScript {

    // Only .text holds EE code. Everything else that is marked executable is VU microcode
    // (.vutext, .DVP.overlay.*) or a placeholder map added by the EE extension (vu0.code,
    // vu1.code, iop_ram), none of which should be disassembled as R5900.
    private static boolean isVuBlock(String name) {
        return !name.equals(".text");
    }

    @Override
    public void run() throws Exception {
        println("Language: " + currentProgram.getLanguageID() + " / " +
            currentProgram.getCompilerSpec().getCompilerSpecID());
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (isVuBlock(block.getName()) && block.isExecute()) {
                block.setExecute(false);
                // Also make sure nothing was already laid down there by the loader.
                currentProgram.getListing().clearCodeUnits(block.getStart(), block.getEnd(), false);
                println("Cleared execute flag on non-EE block " + block.getName());
            }
            println(String.format("BLOCK %-40s %s-%s size=0x%x r=%b w=%b x=%b init=%b loaded=%b",
                block.getName(), block.getStart(), block.getEnd(), block.getSize(), block.isRead(),
                block.isWrite(), block.isExecute(), block.isInitialized(), block.isLoaded()));
        }
    }
}
