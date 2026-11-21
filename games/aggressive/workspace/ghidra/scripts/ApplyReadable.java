// The readable write-back: applies what the readable rewrite knows (function names, C++
// signatures as Ghidra prototypes, the readable structs, typed globals) from the file
// tools/workdb/readable_ghidra.py writes, then reads every item back and fails on any
// mismatch. The file's SHA-256 (and its header's) go into the program's Program Information, so
// an export says which input it was made with.
// Signatures use the compiler spec's default convention, or the one a record names ("cc": the Xbox
// builds keep the convention Ghidra's analysis found); the read-back checks each parameter's
// storage against the file's (EABI registers on the PS2 and GameCube, ECX/EDX or the stack on x86).
// Args: <readable-BUILD.jsonl> [verify]
//   verify: apply nothing, only read back (for a -readOnly check of a project already applied)
// The header named in the file's meta line (readable-BUILD.h, beside it) is parsed with Ghidra's
// C parser into the category /readable. Renamed functions keep their old name in the string
// property map "ReadableOriginalName" (ExportXbe.java writes it as the function's name, so the
// matching tools see the same names as before). A name a loader, Function ID or the SCE scan gave
// (the libraries' own) stays; the readable name becomes a secondary label there. Run it on a copy
// of an analysed project (tools/workdb/ghidra/readable_pass.sh): a program that already
// holds a different input's hash is refused.
// @category Z-Axis
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.cmd.function.FunctionRenameOption;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.framework.options.Options;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.lang.CompilerSpec;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.util.PropertyMapManager;
import ghidra.program.model.util.StringPropertyMap;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;

public class ApplyReadable extends GhidraScript {
    static final String CAT = "/readable";
    static final String ORIG = "ReadableOriginalName";
    int mismatches = 0;
    List<String> notes = new ArrayList<>();

    void mismatch(String s) {
        mismatches++;
        if (mismatches <= 60) printerr("ApplyReadable mismatch: " + s);
    }

    static String sha256(byte[] b) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (byte x : MessageDigest.getInstance("SHA-256").digest(b)) sb.append(String.format("%02x", x));
        return sb.toString();
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        if (a.length < 1) throw new IllegalArgumentException("usage: ApplyReadable.java <readable-BUILD.jsonl> [verify]");
        boolean verify = a.length > 1 && a[1].equals("verify");
        File in = new File(a[0]);
        byte[] raw = Files.readAllBytes(in.toPath());
        String sha = sha256(raw);
        List<JsonObject> recs = new ArrayList<>();
        for (String ln : new String(raw, StandardCharsets.UTF_8).split("\n"))
            if (!ln.isBlank()) recs.add(JsonParser.parseString(ln).getAsJsonObject());
        JsonObject meta = recs.get(0);
        if (!meta.get("k").getAsString().equals("meta")) throw new IllegalStateException("no meta line in " + in);
        boolean types = meta.get("types").getAsBoolean();
        File hdr = new File(in.getParentFile(), meta.get("header").getAsString());
        byte[] hraw = Files.readAllBytes(hdr.toPath());
        String hsha = sha256(hraw);
        if (!hsha.equals(meta.get("header_sha256").getAsString()))
            throw new IllegalStateException("header " + hdr + " is not the one the file was made with");

        Options info = currentProgram.getOptions(Program.PROGRAM_INFO);
        String had = info.getString("Readable input sha256", null);
        if (!verify && had != null && !had.equals(sha))
            throw new IllegalStateException("this program already holds readable input " + had + "; start from a fresh copy");
        if (verify && !sha.equals(had)) mismatch("program records input " + had + ", file is " + sha);

        DataTypeManager dtm = currentProgram.getDataTypeManager();
        Map<String, DataType> protos = new HashMap<>(), typedefs = new HashMap<>();
        if (types) {
            if (!verify) parseHeader(new String(hraw, StandardCharsets.UTF_8), dtm);
            for (DataType dt : dtm.getCategory(new CategoryPath(CAT)) == null ? new DataType[0]
                    : dtm.getCategory(new CategoryPath(CAT)).getDataTypes()) {
                if (dt instanceof FunctionDefinition) protos.put(dt.getName(), dt);
                else if (dt instanceof TypeDef) typedefs.put(dt.getName(), dt);
            }
        }
        PropertyMapManager pm = currentProgram.getUsrPropertyManager();
        StringPropertyMap orig = pm.getStringPropertyMap(ORIG);
        if (orig == null && !verify) orig = pm.createStringPropertyMap(ORIG);

        int nStruct = 0, nFn = 0, nRenamed = 0, nSame = 0, nKept = 0, nNoFn = 0, nSig = 0, nSigSkip = 0,
            nGlob = 0, nGlobTyped = 0, nGlobFail = 0;
        Map<Long, String> names = new HashMap<>();
        // structs: read back every field the file lists
        for (JsonObject r : recs) {
            if (!r.get("k").getAsString().equals("struct")) continue;
            DataType dt = dtm.getDataType(new CategoryPath(CAT), r.get("n").getAsString());
            if (!(dt instanceof Composite c)) { mismatch("struct " + r.get("n") + " missing"); continue; }
            if (c.getLength() != r.get("size").getAsInt())
                mismatch("struct " + c.getName() + " is " + c.getLength() + " bytes, wants " + r.get("size"));
            for (JsonElement fe : r.getAsJsonArray("fields")) {
                JsonArray f = fe.getAsJsonArray();
                int off = f.get(0).getAsInt(), size = f.get(3).getAsInt();
                String fname = f.get(1).getAsString();
                DataTypeComponent dc = null;
                for (DataTypeComponent x : c.getDefinedComponents())
                    if (x.getOffset() == off && fname.equals(x.getFieldName())) dc = x;
                if (dc == null) mismatch("struct " + c.getName() + " has no " + fname + " at +0x" + Integer.toHexString(off));
                else if (dc.getLength() != size)
                    mismatch("struct " + c.getName() + "." + fname + " is " + dc.getLength() + " bytes, wants " + size);
            }
            nStruct++;
        }
        // function names
        SymbolTable st = currentProgram.getSymbolTable();
        Namespace global = currentProgram.getGlobalNamespace();
        for (JsonObject r : recs) {
            if (!r.get("k").getAsString().equals("fn")) continue;
            Address ad = toAddr(r.get("a").getAsLong());
            String want = r.get("n").getAsString();
            Function f = getFunctionAt(ad);
            if (f == null) { nNoFn++; notes.add("no function at " + ad + " for " + want); continue; }
            nFn++;
            names.put(r.get("a").getAsLong(), want);
            // a name a loader, Function ID or the SCE scan gave (IMPORTED: the libraries' own names)
            // stays the function's name; the readable one becomes a secondary label
            boolean keep = !f.getName().equals(want) && (f.getSymbol().getSource() == SourceType.IMPORTED
                || f.getSymbol().getSource() == SourceType.ANALYSIS && named(f.getName()));
            if (!verify) {
                String old = f.getName(true);
                if (old.equals(want)) { nSame++; }
                else if (keep) {
                    nKept++;
                    if (st.getGlobalSymbol(want, ad) == null) st.createLabel(ad, want, SourceType.USER_DEFINED);
                }
                else {
                    if (orig.getString(ad) == null) orig.add(ad, old);
                    f.getSymbol().setNameAndNamespace(want, global, SourceType.USER_DEFINED);
                    nRenamed++;
                }
                String ev = "readable: " + want + " (" + r.get("src").getAsString()
                    + (r.has("ai") && r.get("ai").getAsLong() != r.get("a").getAsLong()
                        ? String.format(", AI PS2 0x%X %s", r.get("ai").getAsLong(), r.get("conf").getAsString()) : "")
                    + (r.has("via") ? " via " + r.get("via").getAsString() : "")
                    + "; " + r.get("ev").getAsString() + ")";
                String pc = f.getComment();
                f.setComment(pc == null ? ev : pc + "\n" + ev);
            }
            Function g = getFunctionAt(ad);
            if (keep) {
                if (verify) nKept++;
                if (st.getGlobalSymbol(want, ad) == null) mismatch("function " + ad + " (" + g.getName() + ") has no label " + want);
            } else if (!g.getName().equals(want)) mismatch("function " + ad + " is " + g.getName() + ", wants " + want);
        }
        // signatures
        for (JsonObject r : recs) {
            if (!r.get("k").getAsString().equals("sig")) continue;
            if (r.has("skip")) { nSigSkip++; continue; }
            Address ad = toAddr(r.get("a").getAsLong());
            Function f = getFunctionAt(ad);
            if (f == null) continue;
            DataType pd = protos.get(r.get("proto_name").getAsString());
            if (!(pd instanceof FunctionDefinition fd)) { mismatch("prototype " + r.get("proto_name") + " not parsed"); continue; }
            if (!verify) {
                // the build's own convention where the file names one (the Xbox: the one Ghidra's
                // analysis found), else the compiler spec's default (the EE's or the PowerPC
                // EABI), not the parser's "unknown"
                if (r.has("cc")) fd.setCallingConvention(r.get("cc").getAsString());
                else if (fd.getCallingConventionName().equals(CompilerSpec.CALLING_CONVENTION_unknown))
                    fd.setCallingConvention(currentProgram.getCompilerSpec().getDefaultCallingConvention().getName());
                ApplyFunctionSignatureCmd cmd = new ApplyFunctionSignatureCmd(ad, fd, SourceType.USER_DEFINED, false,
                    FunctionRenameOption.NO_CHANGE);
                if (!cmd.applyTo(currentProgram, monitor)) { mismatch("signature at " + ad + ": " + cmd.getStatusMsg()); continue; }
            }
            nSig++;
            checkSignature(f, fd, r);
        }
        // globals: the larger first, so a field's own global inside a struct global stays a label
        List<JsonObject> globs = new ArrayList<>();
        for (JsonObject r : recs) if (r.get("k").getAsString().equals("global") && r.has("a")) globs.add(r);
        globs.sort((x, y) -> Integer.compare(y.has("size") ? y.get("size").getAsInt() : 0, x.has("size") ? x.get("size").getAsInt() : 0));
        for (JsonObject r : globs) {
            Address ad = toAddr(r.get("a").getAsLong());
            if (!currentProgram.getMemory().contains(ad)) { notes.add("global " + r.get("n") + " at " + ad + ": not in memory"); continue; }
            String want = r.get("n").getAsString();
            nGlob++;
            DataType dt = r.has("tname") ? typedefs.get(r.get("tname").getAsString()) : null;
            if (dt instanceof TypeDef td) dt = td.getDataType();
            if (!verify) {
                Symbol s = st.getGlobalSymbol(want, ad);
                if (s == null) s = st.createLabel(ad, want, SourceType.USER_DEFINED);
                s.setPrimary();
                if (r.has("aliases"))
                    for (JsonElement al : r.getAsJsonArray("aliases"))
                        if (st.getGlobalSymbol(al.getAsString(), ad) == null) st.createLabel(ad, al.getAsString(), SourceType.USER_DEFINED);
                Data cont = getDataContaining(ad);
                boolean inside = cont != null && !cont.getAddress().equals(ad) && cont.getDataType().getPathName().startsWith(CAT);
                if (dt != null && !inside && getInstructionContaining(ad) == null) {
                    try {
                        DataUtilities.createData(currentProgram, ad, dt, -1, DataUtilities.ClearDataMode.CLEAR_ALL_CONFLICT_DATA);
                    } catch (Exception e) {
                        nGlobFail++;
                        notes.add("global " + want + " at " + ad + " not typed: " + e.getMessage());
                    }
                }
            }
            Symbol p = st.getPrimarySymbol(ad);
            if (p == null || !p.getName().equals(want)) mismatch("global " + ad + " is " + (p == null ? "unnamed" : p.getName()) + ", wants " + want);
            Data d = getDataAt(ad);
            if (dt != null && d != null && d.getDataType().isEquivalent(dt)) nGlobTyped++;
        }
        if (!verify) {
            info.setString("Readable input", in.getName());
            info.setString("Readable input sha256", sha);
            info.setString("Readable header sha256", hsha);
            info.setString("Readable sources", meta.get("readable_commit").getAsString() + " (" + meta.get("readable_digest").getAsString() + ")");
            info.setString("Readable workdb built", String.valueOf(meta.get("workdb_built")));
        }
        for (String n : notes.subList(0, Math.min(notes.size(), 40))) println("ApplyReadable note: " + n);
        println(String.format("ApplyReadable %s: %s %s, structs %d, functions %d (renamed %d, already so %d, loader/SCE names kept %d, " +
            "no function at %d), signatures %d (skipped %d), globals %d (typed %d, type refused %d), notes %d, mismatches %d, input sha256 %s",
            verify ? "verified" : "applied", meta.get("build").getAsString(), in.getName(), nStruct, nFn, nRenamed, nSame, nKept, nNoFn,
            nSig, nSigSkip, nGlob, nGlobTyped, nGlobFail, notes.size(), mismatches, sha));
        if (mismatches > 0) throw new IllegalStateException("ApplyReadable: " + mismatches + " mismatches");
    }

    void parseHeader(String text, DataTypeManager dtm) throws Exception {
        // the parser takes no comments outside a preprocessor run: drop them
        text = text.replaceAll("(?s)/\\*.*?\\*/", "");
        CParser p = new CParser(dtm, true, null);
        p.setParseFileName("readable.h");
        p.parse(text);
        String msg = p.getParseMessages();
        if (msg != null && !msg.isBlank()) println("ApplyReadable parser: " + msg.trim());
        // into one category
        CategoryPath cat = new CategoryPath(CAT);
        List<DataType> all = new ArrayList<>();
        all.addAll(p.getComposites().values());
        all.addAll(p.getFunctions().values());
        all.addAll(p.getTypes().values());
        for (DataType dt : all) {
            DataType have = dtm.getDataType(dt.getDataTypePath());
            if (have == null) have = dtm.resolve(dt, DataTypeConflictHandler.REPLACE_HANDLER);
            if (!have.getCategoryPath().equals(cat)) have.setCategoryPath(cat);
        }
    }

    // workdb.py's is_named: a name that says more than Ghidra's placeholders
    static boolean named(String n) {
        String s = n.substring(n.lastIndexOf(':') + 1);
        for (String p : new String[] {"FUN_", "thunk_FUN_", "LAB_", "entry_", "switchD", "caseD", "SUB_"})
            if (s.startsWith(p)) return false;
        return true;
    }

    static String regOf(Parameter p) {
        VariableStorage vs = p.getVariableStorage();
        if (vs.isRegisterStorage()) return vs.getRegister().getName();
        if (vs.isStackStorage()) return "stack";
        return vs.toString();
    }

    void checkSignature(Function f, FunctionDefinition fd, JsonObject r) {
        JsonArray ps = r.getAsJsonArray("params"), store = r.getAsJsonArray("storage");
        Parameter[] got = f.getParameters();
        if (r.has("cc") && !f.getCallingConventionName().equals(r.get("cc").getAsString()))
            mismatch(f.getName() + " is " + f.getCallingConventionName() + ", wants " + r.get("cc").getAsString());
        if (got.length != ps.size()) { mismatch(f.getName() + ": " + got.length + " parameters, wants " + ps.size()); return; }
        ParameterDefinition[] want = fd.getArguments();
        for (int i = 0; i < got.length; i++) {
            if (!got[i].getDataType().isEquivalent(want[i].getDataType()))
                mismatch(f.getName() + " parameter " + i + " is " + got[i].getDataType().getName() + ", wants " + want[i].getDataType().getName());
            if (!got[i].getName().equals(want[i].getName()))
                mismatch(f.getName() + " parameter " + i + " is named " + got[i].getName() + ", wants " + want[i].getName());
            String reg = regOf(got[i]), wr = store.get(i).getAsString();
            // the R5900's 128-bit GPRs: the language names their low 32 or 64 bits a0_lo and so on
            if (!(reg.equals(wr) || reg.startsWith(wr + "_")))
                mismatch(f.getName() + " parameter " + i + " (" + got[i].getName() + ") in " + reg + ", the convention puts it in " + wr);
        }
        if (!f.getReturnType().isEquivalent(fd.getReturnType()))
            mismatch(f.getName() + " returns " + f.getReturnType().getName() + ", wants " + fd.getReturnType().getName());
    }
}
