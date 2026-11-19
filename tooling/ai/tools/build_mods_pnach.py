#!/usr/bin/env python3
"""Build the PCSX2 pnach (and raw cheat-device codes) of the Aggressive Inline mods.

Output (committed):
  mods/pcsx2/SLUS-20327_67DB3ED8.pnach   PCSX2 2.x patch file, one [section] per mod
  mods/pcsx2/raw-codes.txt               the same as raw codes (2aaaaaaa vvvvvvvv)

Inputs: tools/ai_mods_pnach.s (the hooks' MIPS code), assembled with the
decomp's PS2 binutils (decomp/tools/binutils, read-only), and the game's ELF
(only to print the original words in the comments; give it with --elf, the
default is extracted/SLUS_203.27). No game data ends up in the output apart
from the original instruction words quoted in comments, as is usual in pnach
files.

    python3 tools/build_mods_pnach.py            # write both files
    python3 tools/build_mods_pnach.py --install DIR   # also copy the pnach to DIR
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BINUTILS = os.path.join(REPO, "decomp", "tools", "binutils", "mips-ps2-decompals-")
SOURCE = os.path.join(HERE, "ai_mods_pnach.s")
OUT_DIR = os.path.join(REPO, "mods", "pcsx2")
PNACH = "SLUS-20327_67DB3ED8.pnach"
CAVE = 0x00219A80          # sceMkdir..sceReadlink (libkernl), never called by the game
CAVE_END = 0x0021B5B8
VARS = 0x002728C0          # .rodata padding (zero), after BloodRaynare's 0x272894-0x2728AF
AUTHOR = "Aggressive Inline PC port"
WS_AUTHOR = "BloodRaynare (based on ElHecht's PAL patch); HUD/2D fix and aspect variants: Aggressive Inline PC port"


# ---- assembling -------------------------------------------------------------
def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        obj, elf, binf = (os.path.join(tmp, n) for n in ("m.o", "m.elf", "m.bin"))
        subprocess.run([BINUTILS + "as", "-EL", "-march=r5900", "-mabi=eabi", "-o", obj, SOURCE], check=True)
        subprocess.run([BINUTILS + "ld", "-EL", "-Ttext", "0x%08X" % CAVE, "-e", "level_update", "-o", elf, obj],
                       check=True)
        subprocess.run([BINUTILS + "objcopy", "-O", "binary", "-j", ".text", elf, binf], check=True)
        nm = subprocess.run([BINUTILS + "nm", elf], check=True, capture_output=True, text=True).stdout
        code = open(binf, "rb").read()
    syms = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    if CAVE + len(code) > CAVE_END:
        raise SystemExit("code does not fit the cave")
    return code, syms


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read() if path and os.path.exists(path) else None

    def word(self, va):
        if not self.data:
            return None
        return struct.unpack_from("<I", self.data, 0x1000 + va - 0x100000)[0]


def f2h(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def h2f(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


def jump(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


# ---- the mods -----------------------------------------------------------------
# Tuning table value addresses (entry + 0x20; docs/DEVTOOLS.md). Each entry is
# {name[32], value, max, min, step}; the game reads most of them live.
T = {
    "Death by Juice": 0x00246248,
    "Distance": 0x00246578,
    "Distance HPole": 0x002465A8,
    "Height": 0x002465D8,
    "FOV": 0x002466C8,
    "MP Boardwalk FOV": 0x00246758,
    "MP Civic FOV": 0x00246788,
    "MP Movie FOV": 0x002467B8,
    "MP Museum FOV": 0x002467E8,
    "MP Brdwalk Far Clip": 0x00246818,
    "MP Civic Far Clip": 0x00246848,
    "MP Movie Far Clip": 0x00246878,
    "MP Museum Far Clip": 0x002468A8,
    "Grind Height": 0x00246C68,
    "Grind Distance": 0x00246C98,
    "Normal Blur": 0x00246D58,
    "Air Rotate": 0x002473B8,
    "Juice Lost/Level on crash": 0x00247BC8,
    "Max Juice Degradation/sec": 0x00247BF8,
    "Grind Balance Factor": 0x00249C68,
    "Grind Balance Init": 0x00249CC8,
    "Stall Balance Factor": 0x00249E78,
    "Stall Balance Init": 0x00249ED8,
    "Manual Balance Factor": 0x00249F68,
    "Manual Balance Init": 0x00249FC8,
    "Wall Ride Gravity": 0x0024A3E8,
}
DEFAULTS = {  # the ELF's values (for comments and scaling)
    "Death by Juice": 1, "Distance": 4, "Distance HPole": 6, "Height": 0.45, "FOV": 75,
    "MP Boardwalk FOV": 52, "MP Civic FOV": 60, "MP Movie FOV": 60, "MP Museum FOV": 54,
    "MP Brdwalk Far Clip": 100, "MP Civic Far Clip": 100, "MP Movie Far Clip": 100, "MP Museum Far Clip": 150,
    "Grind Height": 1.8, "Grind Distance": 3.5, "Normal Blur": 65, "Air Rotate": 320,
    "Juice Lost/Level on crash": 0.5, "Max Juice Degradation/sec": 2,
    "Grind Balance Factor": 0.0001, "Grind Balance Init": 0.004, "Stall Balance Factor": 0.00012,
    "Stall Balance Init": 0.003, "Manual Balance Factor": 0.00012, "Manual Balance Init": 0.01,
    "Wall Ride Gravity": 12,
}


class Section:
    def __init__(self, name, description, author=AUTHOR, extra=None, notes=None):
        self.name, self.description, self.author = name, description, author
        self.extra = extra or []          # e.g. gsaspectratio=16:9
        self.notes = notes or []          # comment lines at the top of the section
        self.lines = []                   # (kind, text) kind: "comment" | "patch"

    def comment(self, text):
        self.lines.append(("comment", text))

    def word(self, addr, value, note=""):
        self.lines.append(("patch", (addr, "word", value, note)))

    def ext(self, code, value, note=""):
        self.lines.append(("patch", (code, "extended", value, note)))

    def tune(self, name, value, note=""):
        default = DEFAULTS[name]
        text = "%s = %g (game: %g)" % (name, value, default)
        self.word(T[name], f2h(value), text + ("; " + note if note else ""))


def blob(sec, code, syms, start, end, title):
    a, b = syms[start], syms[end]
    sec.comment("%s: 0x%08X-0x%08X (%d words, in the unused sceMkdir..sceReadlink code)"
                % (title, a, b - 4, (b - a) // 4))
    for addr in range(a, b, 4):
        off = addr - CAVE
        sec.word(addr, struct.unpack_from("<I", code, off)[0])


def make_sections(code, syms, elf):
    def orig(addr):
        w = elf.word(addr)
        return "" if w is None else "was %08X" % w

    L = lambda s: blob(s, code, syms, "level_update", "level_update_end", "Part L, play update hook")
    G = lambda s: blob(s, code, syms, "gui_draw", "gui_draw_end", "Part G, zgui widget draw hook")
    S = lambda s: blob(s, code, syms, "squeeze", "squeeze_end", "Part S, 2D squeeze hook")

    def hook_level(s):
        s.comment("Game mode 2 (play) update pointer in the mode table 0x0024B810")
        s.word(0x0024B844, syms["level_update"], "was 001ACA68 (play update) -> Part L")

    def hook_gui(s):
        s.comment("zgui widget draw 0x00171CA8: j Part G")
        s.word(0x00171CA8, jump(syms["gui_draw"]), orig(0x00171CA8) + " (addiu sp,sp,-48)")
        s.word(0x00171CAC, 0, orig(0x00171CAC) + " (li v1,1)")

    def hook_squeeze(s):
        s.comment("sceVif1PkCloseUpkCode 0x00210F28: j Part S")
        s.word(0x00210F28, jump(syms["squeeze"]), orig(0x00210F28) + " (lw v0,0(a0))")
        s.word(0x00210F2C, 0, orig(0x00210F2C) + " (lui v1,0xffff)")

    out = []

    # ---- widescreen -----------------------------------------------------------
    def widescreen(label, aspect_text, lui_hi, ratio_note, gs):
        scale = h2f(lui_hi << 16)
        shift = 0.5 * (1.0 / scale - 1.0)
        s = Section("Widescreen %s (HUD fix)" % label,
                    "%s 3D with the HUD at the screen edges and in-game 2D (menus, text boxes) kept in shape; "
                    "the front end and movies show as 4:3 between bars. Use instead of the built-in "
                    "Widescreen 16:9 patch, and turn on only one widescreen section." % aspect_text,
                    author=WS_AUTHOR, extra=["gsaspectratio=%s" % gs] if gs else [],
                    notes=[ratio_note] if ratio_note else [])
        s.comment("3D: BloodRaynare's patch (horizontal scale %.4f, view frustum widened by the same)" % scale)
        s.word(0x00101130, 0x3C010000 | lui_hi, "lui at,0x%04X: horizontal scale %.4f, %s (lui at,0x3f80)"
               % (lui_hi, scale, orig(0x00101130)))
        s.word(0x00272894, 0x3C090000 | lui_hi, "frustum code cave: lui t1,0x%04X (rodata padding, was 0)" % lui_hi)
        s.word(0x00101714, 0x0809CA25, "j 0x00272894, " + orig(0x00101714) + " (div.s f1,f1,f0)")
        s.word(0x00101718, 0x00000000, orig(0x00101718) + " (swc1 f0,0x4E8(s1))")
        s.word(0x0027289C, 0x4489F000, "mtc1 t1,f30")
        s.word(0x002728A0, 0x461E0843, "div.s f1,f1,f30: half width / scale")
        s.word(0x002728A4, 0x46000843, "div.s f1,f1,f0 (the replaced instruction)")
        s.word(0x002728A8, 0xE62004E8, "swc1 f0,0x4E8(s1) (the replaced instruction)")
        s.word(0x002728AC, 0x080405C6, "j 0x00101718")
        s.comment("2D and HUD (Aggressive Inline PC port): squeeze 2D by the scale, move HUD widgets out by width * shift")
        s.word(VARS + 20, f2h(scale), "squeeze scale %.6f" % scale)
        s.word(VARS + 24, f2h(shift), "HUD shift %.6f of the width" % shift)
        G(s)
        S(s)
        hook_gui(s)
        hook_squeeze(s)
        out.append(s)

    widescreen("16:9", "16:9", 0x3F40, None, "16:9")
    widescreen("16:10", "16:10", 0x3F55,
               "PCSX2 has no 16:10 output setting: set Aspect Ratio to Stretch and use a 16:10 window or screen.", None)
    widescreen("21:9", "21:9 (2.33:1)", 0x3F12,
               "PCSX2 has no 21:9 output setting: set Aspect Ratio to Stretch and use a 21:9 window or screen.", None)

    # ---- camera -----------------------------------------------------------------
    for pct in (150, 200):
        s = Section("Camera Distance %d%%" % pct,
                    "The follow camera sits %s further behind the skater (grinds and poles too)."
                    % ("1.5x" if pct == 150 else "2x"))
        for n in ("Distance", "Grind Distance", "Distance HPole"):
            s.tune(n, DEFAULTS[n] * pct / 100)
        out.append(s)
    for pct in (150, 200):
        s = Section("Camera Height %d%%" % pct,
                    "The follow camera sits %s higher above the skater (grinds too)."
                    % ("1.5x" if pct == 150 else "2x"))
        for n in ("Height", "Grind Height"):
            s.tune(n, DEFAULTS[n] * pct / 100)
        out.append(s)
    for fov in (90, 105):
        s = Section("Field of View %d" % fov,
                    "Wider camera view: FOV %d instead of 75 (split screen scaled the same). "
                    "Takes effect when a level starts." % fov)
        s.tune("FOV", fov)
        for n in ("MP Boardwalk FOV", "MP Civic FOV", "MP Movie FOV", "MP Museum FOV"):
            s.tune(n, round(DEFAULTS[n] * fov / 75.0, 1), "split screen")
        out.append(s)

    # ---- image ------------------------------------------------------------------
    s = Section("Motion Blur Off", "No motion trail (the previous frame blended over each new one). "
                "Takes effect when a level starts.")
    s.tune("Normal Blur", 0, "blend alpha; below 8 the blur is off")
    out.append(s)
    s = Section("Motion Blur Low", "Half-strength motion trail. Takes effect when a level starts.")
    s.tune("Normal Blur", 32, "blend alpha")
    out.append(s)

    for label, far, fog0_hi, fog1_hi, desc in (
            ("Far", (250, 250, 250, 375), 0x4322, 0x4361,
             "2.5x the draw distance and fog in split screen (single player is already at the game's maximum)."),
            ("Farthest", (1000, 1000, 1000, 1000), 0x4422, 0x4461,
             "Split screen draws as far as single player (1000, fog 648-900). May slow down on a real PS2.")):
        s = Section("Split Screen Draw Distance %s" % label, desc + " Takes effect when a level starts.")
        for n, v in zip(("MP Brdwalk Far Clip", "MP Civic Far Clip", "MP Movie Far Clip", "MP Museum Far Clip"), far):
            s.tune(n, v)
        s.comment("Split-screen fog in the level camera setup 0x001901C0")
        s.word(0x001903A0, 0x3C010000 | fog0_hi, "lui at,0x%04X: fog start %g, %s (65)"
               % (fog0_hi, h2f(fog0_hi << 16), orig(0x001903A0)))
        s.word(0x001903B8, 0x3C010000 | fog1_hi, "lui at,0x%04X: fog end %g, %s (90)"
               % (fog1_hi, h2f(fog1_hi << 16), orig(0x001903B8)))
        out.append(s)

    # ---- hooks: restart, debug menu, hide HUD ------------------------------------
    s = Section("Quick Restart (L3+R3)",
                "Click both sticks in during play: the level starts again from the beginning (Career, Freeskate; "
                "Career progress is kept as when you exit the level). In a Timed Run it restarts the run. "
                "Not in split screen.")
    L(s)
    hook_level(s)
    s.word(VARS + 0, 1, "Quick Restart on")
    out.append(s)

    s = Section("Developer Debug Menu (L3+SELECT)",
                "Hold L3 (left stick in) and press SELECT during play: the developers' hidden debug menu "
                "(Resume, Art Viewer free camera, Tuning editor for the game's 405 tuning values, Respawn, "
                "Toggle Debug info).")
    L(s)
    hook_level(s)
    s.word(VARS + 4, 1, "Debug Menu on")
    out.append(s)

    s = Section("Hide HUD Toggle (R3+SELECT)",
                "Hold R3 (right stick in) and press SELECT during play to hide or show the HUD "
                "(score, juice meter, trick names, balance meters, timer), for screenshots and videos.")
    L(s)
    G(s)
    hook_level(s)
    hook_gui(s)
    s.word(VARS + 8, 1, "Hide HUD toggle on")
    out.append(s)

    # ---- assists ------------------------------------------------------------------
    s = Section("Keep Skating When Out of Juice",
                "An empty juice meter no longer ends a Career run.")
    s.tune("Death by Juice", 0)
    out.append(s)
    s = Section("Juice Drain 50%", "The juice meter empties half as fast.")
    s.tune("Max Juice Degradation/sec", 1.0)
    out.append(s)
    s = Section("No Juice Drain", "The juice meter only goes down when you crash.")
    s.tune("Max Juice Degradation/sec", 0.0)
    out.append(s)
    s = Section("Juice Lost on Crash 50%", "A crash takes half as much juice.")
    s.tune("Juice Lost/Level on crash", 0.25)
    out.append(s)

    # ---- the game's cheats, always on ------------------------------------------------
    cheats = [
        ("Low Gravity Wall Ride", "Wall rides last longer.", [("Wall Ride Gravity", 3.0)]),
        ("Juice Regeneration", "The juice meter fills up by itself.", [("Max Juice Degradation/sec", -0.2)]),
        ("Invulnerability", "Crashes cost no juice.", [("Juice Lost/Level on crash", 0.0)]),
        ("Perfect Grind", "Grinds never lose balance.",
         [("Grind Balance Factor", 0.0), ("Grind Balance Init", 0.0)]),
        ("Perfect Handplant", "Hand plants (stalls) never lose balance.",
         [("Stall Balance Factor", 0.0), ("Stall Balance Init", 0.0)]),
        ("Perfect Manual", "Manuals never lose balance.",
         [("Manual Balance Factor", 0.0), ("Manual Balance Init", 0.0)]),
        ("Super Spin", "Spins twice as fast in the air.", [("Air Rotate", 640.0)]),
    ]
    for name, desc, writes in cheats:
        s = Section("Cheat: %s" % name, desc + " The game's own cheat, always on, without its code "
                    "(and without setting the cheat bits the codes leave in the career data).")
        for n, v in writes:
            s.tune(n, v, "as the game's cheat code sets it (0x001AA628)")
        out.append(s)

    s = Section("Unlock Everything",
                "All levels, skaters, park editor pieces and Media Viewer cutscenes. Writes the career data: "
                "saving the career keeps the unlocks after the patch is turned off.")
    s.comment("Unlock bits of the career data at 0x0024CBB8 (64-bit), ORed so other progress is kept")
    s.ext(0x7024CBB8, 0x0010FFFF, "16-bit OR 0xFFFF: bits 0-15 (levels, skaters, park pieces)")
    s.ext(0x7024CBBA, 0x0000000F, "8-bit OR 0x0F: bits 16-19")
    s.ext(0x7024CBBE, 0x0000007F, "8-bit OR 0x7F: bits 48-54 (Media Viewer cutscenes)")
    s.comment("The 20 skater records at 0x0024DB30 + n * 0x510: +1 = 10, +4 = all levels (as the game's codes)")
    for n in range(20):
        rec = 0x0024DB30 + n * 0x510
        s.ext(0x00000000 | (rec + 1), 0x0000000A, "skater %d: 8-bit write 10" % n)
        s.word(rec + 4, 0xFFFFFFFF, "skater %d: level mask" % n)
    out.append(s)
    return out


UNTESTED_MARK = "UNTESTED"


def load_test_status():
    path = os.path.join(OUT_DIR, "test-status.txt")
    status = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.rstrip("\n")
            if not line or line.startswith("#") or "\t" not in line:
                continue
            name, state = line.split("\t", 1)
            status[name] = state
    return status


def render_pnach(sections):
    status = load_test_status()
    out = [
        "gametitle=Aggressive Inline (USA) (SLUS-20327)",
        "",
        "// Mods for the original game, from the research behind the Aggressive Inline native",
        "// PC port. Each [section] is one mod: turn it on in PCSX2 under Game Properties >",
        "// Patches. See README.md next to this file. Generated by tools/build_mods_pnach.py",
        "// from tools/ai_mods_pnach.s; do not edit by hand.",
        "//",
        "// Shared code lives in libkernl's unused file-system functions (0x00219A80-0x0021B5B7)",
        "// and shared variables in the .rodata padding 0x002728C0-0x002728DB. Sections that",
        "// share a hook write identical words there, so any combination can be on, except",
        "// two Widescreen sections, two values of the same setting (e.g. Camera Distance",
        "// 150% and 200%), Juice Regeneration with Juice Drain 50% / No Juice Drain, and",
        "// Invulnerability with Juice Lost on Crash 50% (they write the same value).",
        "",
    ]
    for s in sections:
        out.append("[%s]" % s.name)
        out.append("author=%s" % s.author)
        state = status.get(s.name, UNTESTED_MARK)
        out.append("description=%s (%s)" % (s.description, state))
        out.extend(s.extra)
        for n in s.notes:
            out.append("// " + n)
        for kind, item in s.lines:
            if kind == "comment":
                out.append("// " + item)
            else:
                addr, typ, value, note = item
                line = "patch=1,EE,%08X,%s,%08X" % (addr, typ, value)
                out.append(line + (" // " + note if note else ""))
        out.append("")
    return "\n".join(out)


def render_raw(sections):
    status = load_test_status()
    out = [
        "Aggressive Inline (USA) SLUS-20327 (ELF CRC 0xA8BE0B5B, PCSX2 CRC 67DB3ED8)",
        "Raw codes (unencrypted PS2RD / OPL cheat format) of the mods in SLUS-20327_67DB3ED8.pnach.",
        "Generated by tools/build_mods_pnach.py. Not tested on a real PS2: see README.md.",
        "2aaaaaaa vvvvvvvv = 32-bit write, 0aaaaaaa 000000vv = 8-bit write,",
        "7aaaaaaa 00Z0vvvv = bitwise OR (Z=0 8-bit, Z=1 16-bit).",
        "Cheat devices need their own master code (hook) for this game; none is given here.",
        "",
    ]
    for s in sections:
        out.append('"%s"' % s.name)
        for kind, item in s.lines:
            if kind != "patch":
                continue
            addr, typ, value, _ = item
            if typ == "word":
                out.append("%08X %08X" % (0x20000000 | addr, value))
            else:
                out.append("%08X %08X" % (addr, value))
        out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default=os.path.join(REPO, "extracted", "SLUS_203.27"),
                    help="game ELF, for the original words in comments (optional)")
    ap.add_argument("--install", metavar="DIR", help="also copy the pnach into this PCSX2 patches folder")
    ap.add_argument("--listing", action="store_true", help="print the disassembly of the code")
    args = ap.parse_args()
    code, syms = assemble()
    elf = Elf(args.elf)
    sections = make_sections(code, syms, elf)
    os.makedirs(OUT_DIR, exist_ok=True)
    pnach = os.path.join(OUT_DIR, PNACH)
    open(pnach, "w").write(render_pnach(sections))
    open(os.path.join(OUT_DIR, "raw-codes.txt"), "w").write(render_raw(sections))
    print("wrote %s (%d sections, code %d bytes at 0x%08X)" % (pnach, len(sections), len(code), CAVE))
    for k in ("level_update", "level_update_end", "gui_draw", "gui_draw_end", "squeeze", "squeeze_end"):
        print("  %-18s 0x%08X" % (k, syms[k]))
    if args.install:
        shutil.copy(pnach, os.path.join(args.install, PNACH))
        print("installed into", args.install)
    if args.listing:
        with tempfile.TemporaryDirectory() as tmp:
            obj = os.path.join(tmp, "m.o")
            elf_out = os.path.join(tmp, "m.elf")
            subprocess.run([BINUTILS + "as", "-EL", "-march=r5900", "-mabi=eabi", "-o", obj, SOURCE], check=True)
            subprocess.run([BINUTILS + "ld", "-EL", "-Ttext", "0x%08X" % CAVE, "-e", "level_update", "-o",
                            elf_out, obj], check=True)
            print(subprocess.run([BINUTILS + "objdump", "-d", "-mmips:5900", elf_out], check=True,
                                 capture_output=True, text=True).stdout)


if __name__ == "__main__":
    sys.exit(main())
