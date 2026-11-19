// Read-only quality report for the PS2Recomp function map, plus source-file grouping.
// Args: <report_dir>
// Writes: <report_dir>/quality.txt, gaps.csv, error-bookmarks.csv, jumptables.csv,
//         source-paths.csv, source-paths.json, function-source.csv
// @category PS2Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.File;
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class MapQualityReport extends GhidraScript {

    private static final Pattern SRC_PATH =
        Pattern.compile("[A-Za-z0-9_.\\-/\\\\:]*[A-Za-z0-9_\\-]\\.(?:c|cc|cpp|cxx|h|hpp|s|S|asm|vsm|dsm)\\b");

    private static String hex(long v) {
        return String.format("0x%08X", v);
    }

    private static String json(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') {
                b.append('\\').append(c);
            } else if (c < 0x20) {
                b.append(String.format("\\u%04x", (int) c));
            } else {
                b.append(c);
            }
        }
        return b.append('"').toString();
    }

    private static String csv(String s) {
        if (s.contains(",") || s.contains("\"") || s.contains("\n")) {
            return "\"" + s.replace("\"", "\"\"") + "\"";
        }
        return s;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0] : "reports");
        dir.mkdirs();
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        MemoryBlock text = mem.getBlock(".text");
        AddressSet textSet = new AddressSet(text.getStart(), text.getEnd());
        long textSize = text.getSize();

        // ---- functions, coverage, overlaps
        AddressSet covered = new AddressSet();
        long bodySum = 0;
        int fnCount = 0, fnOutsideText = 0, thunks = 0, defaultNamed = 0, sceNamed = 0, overlapping = 0;
        List<String> overlapLines = new ArrayList<>();
        for (Function f : listing.getFunctions(true)) {
            fnCount++;
            AddressSetView body = f.getBody();
            bodySum += body.getNumAddresses();
            if (!textSet.contains(body)) {
                fnOutsideText++;
            }
            if (f.isThunk()) {
                thunks++;
            }
            if (f.getName().startsWith("FUN_")) {
                defaultNamed++;
            } else {
                sceNamed++;
            }
            AddressSet inter = covered.intersect(body);
            if (!inter.isEmpty()) {
                overlapping++;
                if (overlapLines.size() < 200) {
                    overlapLines.add(f.getName() + " @ " + f.getEntryPoint() + " shares " + inter.getNumAddresses() +
                        " bytes, first " + inter.getMinAddress());
                }
            }
            covered.add(body);
        }
        AddressSet coveredText = covered.intersect(textSet);

        // ---- instructions / data / undefined in .text
        long instrBytes = 0, instrOutsideFn = 0;
        InstructionIterator ii = listing.getInstructions(textSet, true);
        AddressSet instrSet = new AddressSet();
        while (ii.hasNext()) {
            Instruction in = ii.next();
            instrBytes += in.getLength();
            instrSet.add(in.getMinAddress(), in.getMaxAddress());
        }
        instrOutsideFn = instrSet.subtract(covered).getNumAddresses();
        long dataBytes = 0;
        List<String> dataLines = new ArrayList<>();
        for (Data d : listing.getDefinedData(textSet, true)) {
            dataBytes += d.getLength();
            if (dataLines.size() < 300) {
                dataLines.add(d.getAddress() + "," + d.getLength() + "," + csv(d.getDataType().getName()));
            }
        }

        // ---- gaps in .text not covered by functions
        AddressSet gaps = textSet.subtract(coveredText);
        int gapCount = 0, zeroGaps = 0, codeLikeGaps = 0;
        long zeroGapBytes = 0, codeLikeGapBytes = 0, otherGapBytes = 0;
        try (PrintWriter w = new PrintWriter(new File(dir, "gaps.csv"))) {
            w.println("Start,End,Size,Kind,HasInstructions,PrevFunction");
            for (AddressRange r : gaps) {
                gapCount++;
                long len = r.getLength();
                byte[] bytes = new byte[(int) len];
                mem.getBytes(r.getMinAddress(), bytes);
                boolean allZero = true;
                for (byte b : bytes) {
                    if (b != 0) {
                        allZero = false;
                        break;
                    }
                }
                boolean hasInstr = instrSet.intersects(r.getMinAddress(), r.getMaxAddress());
                String kind;
                if (allZero) {
                    kind = "zero-padding";
                    zeroGaps++;
                    zeroGapBytes += len;
                } else if (hasInstr || looksLikeCode(bytes)) {
                    kind = "code-like";
                    codeLikeGaps++;
                    codeLikeGapBytes += len;
                } else {
                    kind = "data-or-unknown";
                    otherGapBytes += len;
                }
                Function prev = getFunctionBefore(r.getMinAddress());
                w.println(r.getMinAddress() + "," + r.getMaxAddress() + "," + len + "," + kind + "," + hasInstr +
                    "," + (prev == null ? "" : prev.getName()));
            }
        }

        // ---- error bookmarks (bad disassembly etc.)
        int errBookmarks = 0;
        try (PrintWriter w = new PrintWriter(new File(dir, "error-bookmarks.csv"))) {
            w.println("Address,Category,Comment");
            Iterator<Bookmark> it = currentProgram.getBookmarkManager().getBookmarksIterator(BookmarkType.ERROR);
            while (it.hasNext()) {
                Bookmark b = it.next();
                errBookmarks++;
                w.println(b.getAddress() + "," + csv(b.getCategory()) + "," + csv(b.getComment()));
            }
        }
        int warnBookmarks = 0;
        Iterator<Bookmark> wit = currentProgram.getBookmarkManager().getBookmarksIterator(BookmarkType.WARNING);
        while (wit.hasNext()) {
            wit.next();
            warnBookmarks++;
        }

        // ---- computed jumps / jump tables
        int computedJumps = 0, resolvedJumps = 0;
        try (PrintWriter w = new PrintWriter(new File(dir, "jumptables.csv"))) {
            w.println("Address,Function,Resolved,TargetCount");
            ii = listing.getInstructions(textSet, true);
            while (ii.hasNext()) {
                Instruction in = ii.next();
                String m = in.getMnemonicString();
                if (!m.equals("jr") && !m.equals("_jr")) {
                    continue;
                }
                String op = in.getDefaultOperandRepresentation(0);
                if (op.equals("ra")) {
                    continue;
                }
                computedJumps++;
                int targets = 0;
                for (Reference r : in.getReferencesFrom()) {
                    if (r.getReferenceType().isJump() || r.getReferenceType().isComputed()) {
                        targets++;
                    }
                }
                if (targets > 0) {
                    resolvedJumps++;
                }
                Function f = getFunctionContaining(in.getAddress());
                w.println(in.getAddress() + "," + (f == null ? "" : f.getName()) + "," + (targets > 0) + "," +
                    targets);
            }
        }

        // ---- source path strings
        Map<String, List<Long>> pathToStringAddrs = new TreeMap<>();
        Map<Long, String> stringText = new HashMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || !b.isLoaded() || b.isExecute() || b.getName().equals(".vutext")) {
                continue;
            }
            byte[] bytes = new byte[(int) b.getSize()];
            mem.getBytes(b.getStart(), bytes);
            int i = 0;
            while (i < bytes.length) {
                int j = i;
                while (j < bytes.length && bytes[j] >= 0x20 && bytes[j] < 0x7f) {
                    j++;
                }
                if (j < bytes.length && bytes[j] == 0 && j - i >= 3) {
                    String s = new String(bytes, i, j - i, "US-ASCII");
                    Matcher m = SRC_PATH.matcher(s);
                    while (m.find()) {
                        String p = m.group();
                        if (p.indexOf('.') == 0 && !p.startsWith("./") && !p.startsWith("../")) {
                            continue;
                        }
                        long addr = b.getStart().getOffset() + i;
                        pathToStringAddrs.computeIfAbsent(p, k -> new ArrayList<>()).add(addr);
                        stringText.put(addr, s);
                    }
                }
                i = j + 1;
            }
        }
        Map<String, Set<Function>> pathToFns = new TreeMap<>();
        Map<Function, Set<String>> fnToPaths = new HashMap<>();
        Map<String, Integer> pathRefCount = new HashMap<>();
        for (Map.Entry<String, List<Long>> e : pathToStringAddrs.entrySet()) {
            Set<Function> fns = new TreeSet<>(Comparator.comparing(f -> f.getEntryPoint()));
            int refs = 0;
            for (long sa : e.getValue()) {
                Address a = toAddr(sa);
                ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(a);
                while (ri.hasNext()) {
                    Reference r = ri.next();
                    refs++;
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) {
                        // pointer in data: follow one level (e.g. a table of __FILE__ pointers)
                        ReferenceIterator r2 = currentProgram.getReferenceManager()
                            .getReferencesTo(r.getFromAddress());
                        while (r2.hasNext()) {
                            Function f2 = getFunctionContaining(r2.next().getFromAddress());
                            if (f2 != null) {
                                fns.add(f2);
                            }
                        }
                        continue;
                    }
                    fns.add(f);
                }
            }
            pathRefCount.put(e.getKey(), refs);
            pathToFns.put(e.getKey(), fns);
            for (Function f : fns) {
                fnToPaths.computeIfAbsent(f, k -> new TreeSet<>()).add(e.getKey());
            }
        }
        try (PrintWriter w = new PrintWriter(new File(dir, "source-paths.csv"))) {
            w.println("SourcePath,StringAddresses,RefCount,FunctionCount,Functions");
            for (String p : pathToFns.keySet()) {
                StringBuilder sa = new StringBuilder();
                for (long a : pathToStringAddrs.get(p)) {
                    sa.append(sa.length() == 0 ? "" : " ").append(hex(a));
                }
                StringBuilder fs = new StringBuilder();
                for (Function f : pathToFns.get(p)) {
                    fs.append(fs.length() == 0 ? "" : " ").append(f.getName()).append("@")
                        .append(hex(f.getEntryPoint().getOffset()));
                }
                w.println(csv(p) + "," + sa + "," + pathRefCount.get(p) + "," + pathToFns.get(p).size() + "," +
                    csv(fs.toString()));
            }
        }
        try (PrintWriter w = new PrintWriter(new File(dir, "source-paths.json"))) {
            w.println("{");
            w.println("  \"program\": " + json(currentProgram.getName()) + ",");
            w.println("  \"source_paths\": [");
            int n = 0;
            for (String p : pathToFns.keySet()) {
                w.println("    {");
                w.println("      \"path\": " + json(p) + ",");
                w.print("      \"strings\": [");
                int k = 0;
                for (long a : pathToStringAddrs.get(p)) {
                    w.print((k++ == 0 ? "" : ", ") + "{\"address\": " + json(hex(a)) + ", \"text\": " +
                        json(stringText.get(a)) + "}");
                }
                w.println("],");
                w.println("      \"ref_count\": " + pathRefCount.get(p) + ",");
                w.print("      \"functions\": [");
                k = 0;
                for (Function f : pathToFns.get(p)) {
                    w.print((k++ == 0 ? "" : ", ") + "{\"name\": " + json(f.getName()) + ", \"start\": " +
                        json(hex(f.getEntryPoint().getOffset())) + ", \"size\": " + f.getBody().getNumAddresses() +
                        "}");
                }
                w.println("]");
                w.println("    }" + (++n < pathToFns.size() ? "," : ""));
            }
            w.println("  ]");
            w.println("}");
        }
        int ambiguous = 0;
        try (PrintWriter w = new PrintWriter(new File(dir, "function-source.csv"))) {
            w.println("Function,Start,Size,SourcePaths");
            List<Function> fl = new ArrayList<>(fnToPaths.keySet());
            fl.sort(Comparator.comparing(f -> f.getEntryPoint()));
            for (Function f : fl) {
                Set<String> ps = fnToPaths.get(f);
                if (ps.size() > 1) {
                    ambiguous++;
                }
                w.println(f.getName() + "," + hex(f.getEntryPoint().getOffset()) + "," +
                    f.getBody().getNumAddresses() + "," + csv(String.join(" ", ps)));
            }
        }
        int pathsWithFns = 0;
        for (Set<Function> s : pathToFns.values()) {
            if (!s.isEmpty()) {
                pathsWithFns++;
            }
        }

        // ---- summary
        try (PrintWriter w = new PrintWriter(new File(dir, "quality.txt"))) {
            w.println("Program: " + currentProgram.getName() + "  language " + currentProgram.getLanguageID());
            w.println(".text: " + text.getStart() + "-" + text.getEnd() + " size " + textSize);
            w.println("Functions (Ghidra): " + fnCount + "  (non-FUN_ named: " + sceNamed + ", thunks: " + thunks +
                ", bodies outside .text: " + fnOutsideText + ")");
            w.println("Sum of function body bytes: " + bodySum);
            w.printf("Unique .text bytes inside functions: %d (%.2f%%)%n", coveredText.getNumAddresses(),
                100.0 * coveredText.getNumAddresses() / textSize);
            w.printf("Instruction bytes in .text: %d (%.2f%%); instructions outside any function: %d bytes%n",
                instrBytes, 100.0 * instrBytes / textSize, instrOutsideFn);
            w.println("Defined data inside .text: " + dataBytes + " bytes");
            w.println("Functions whose body overlaps an earlier function: " + overlapping);
            w.println("Gaps in .text not covered by functions: " + gapCount + " ranges; zero padding " + zeroGaps +
                " (" + zeroGapBytes + " B), code-like " + codeLikeGaps + " (" + codeLikeGapBytes +
                " B), data/unknown " + otherGapBytes + " B");
            w.println("Error bookmarks: " + errBookmarks + "  Warning bookmarks: " + warnBookmarks);
            w.println("Computed jumps (jr not ra): " + computedJumps + ", with resolved targets: " + resolvedJumps);
            w.println("Source path strings: " + pathToFns.size() + " distinct, " + pathsWithFns +
                " referenced from functions; functions attributed: " + fnToPaths.size() + " (" + ambiguous +
                " with more than one path)");
            w.println();
            w.println("== overlaps (first 200)");
            for (String l : overlapLines) {
                w.println(l);
            }
            w.println();
            w.println("== defined data in .text (first 300): address,length,type");
            for (String l : dataLines) {
                w.println(l);
            }
        }
        println("Report written to " + dir.getAbsolutePath());
    }

    // crude: a word-aligned run looks like code if many words decode to common EE opcodes
    private static boolean looksLikeCode(byte[] b) {
        int words = b.length / 4, hits = 0;
        if (words == 0) {
            return false;
        }
        for (int i = 0; i + 4 <= b.length; i += 4) {
            long w = (b[i] & 0xffL) | ((b[i + 1] & 0xffL) << 8) | ((b[i + 2] & 0xffL) << 16) |
                ((b[i + 3] & 0xffL) << 24);
            int op = (int) (w >>> 26);
            int rs = (int) ((w >>> 21) & 31);
            int rt = (int) ((w >>> 16) & 31);
            if ((op == 0x09 && rs == 29 && rt == 29) || op == 0x03 || (op == 0 && (w & 0x3f) == 8 && rs == 31) ||
                op == 0x1f || op == 0x1e || op == 0x0f || op == 0x23 || op == 0x2b || op == 0x3f || op == 0x37) {
                hits++;
            }
        }
        return hits * 3 >= words;
    }
}
