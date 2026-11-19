#!/usr/bin/env python3
"""Compile matchtest/func_XXXXXXXX.c with many compiler/flag combinations and compare
each function against the retail bytes (relocated fields masked).

usage: matchtest.py [--cc NAME ...] [--flags "-O2 -G8" ...] [files...]

Windows compilers (SN ProDG builds, 991111b) run through tools/wibo; Linux i386
builds (Sony 2.9-ee, 2.96) run in a linux/386 Docker container (started on demand,
named ai-eegcc). The .s is assembled with Sony's GNU as 2.9-ee-991111 (the retail
assembler) unless --bundled-as or --modern-as is given.
"""
import argparse
import glob
import os
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC_DIR = os.path.join(ROOT, "tools/compilers")
AS = os.path.join(ROOT, "tools/binutils/mips-ps2-decompals-as")
OUT = os.path.join(ROOT, "build/matchtest")
CONTAINER = "ai-eegcc"

WIN = ["ee-gcc2.95.2-273a", "ee-gcc2.95.2-274", "ee-gcc2.95.3-107", "ee-gcc2.95.3-114",
       "ee-gcc2.95.3-136", "ee-gcc2.9-991111b-r4"]
LINUX = ["ee-gcc2.9-990721", "ee-gcc2.9-991111", "ee-gcc2.9-991111-01", "ee-gcc2.96"]
FLAGS = ["-O2 -G16 -fno-strict-aliasing", "-O2 -G16", "-O2 -G8", "-O2 -G0", "-O1 -G16"]


def ensure_container():
    r = subprocess.run(["docker", "ps", "-q", "-f", f"name=^{CONTAINER}$"], capture_output=True, text=True)
    if r.stdout.strip():
        return
    subprocess.run(["docker", "rm", "-f", CONTAINER], capture_output=True)
    subprocess.run(["docker", "run", "-d", "--name", CONTAINER, "--platform", "linux/386",
                    "-v", f"{ROOT}:/w", "debian:bookworm-slim", "sleep", "infinity"],
                   check=True, capture_output=True)


def compile_c(cc, flags, src, out_s, mode="-S"):
    rel_src = os.path.relpath(src, ROOT)
    rel_out = os.path.relpath(out_s, ROOT)
    if cc in WIN:
        cmd = [os.path.join(ROOT, "tools/wibo"), f"tools/compilers/{cc}/bin/ee-gcc.exe",
               mode, *flags.split(), "-Imatchtest", "-o", rel_out, rel_src]
    else:
        ensure_container()
        libdir = glob.glob(os.path.join(CC_DIR, cc, "lib/gcc-lib/ee/*"))[0]
        b = "/w/" + os.path.relpath(libdir, ROOT) + "/"
        cmd = ["docker", "exec", "-w", "/w", CONTAINER, f"/w/tools/compilers/{cc}/bin/ee-gcc",
               f"-B{b}", mode, *flags.split(), "-Imatchtest", "-o", rel_out, rel_src]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return r.returncode == 0, r.stderr


SONY_AS = "tools/compilers/ee-gcc2.9-991111/ee/bin/as"


def assemble_sony(flags, src_s, out_o):
    """Sony SDK GNU as 2.9-ee-991111 (Linux i386) in the container, flags as ee-gcc passes them."""
    ensure_container()
    g = [f for f in flags.split() if f.startswith("-G")] or ["-G8"]
    r = subprocess.run(["docker", "exec", "-w", "/w", CONTAINER, SONY_AS, *g, "-O2", "-mabi=eabi",
                        "-o", os.path.relpath(out_o, ROOT), os.path.relpath(src_s, ROOT)],
                       capture_output=True, text=True)
    return r.returncode == 0, r.stderr


def assemble(flags, src_s, out_o):
    g = [f for f in flags.split() if f.startswith("-G")] or ["-G8"]
    r = subprocess.run([AS, "-EL", "-march=r5900", "-mabi=eabi", *g, "-no-pad-sections",
                        "-o", out_o, src_s], capture_output=True, text=True)
    return r.returncode == 0, r.stderr


def func_from_obj(path, name):
    elf = ELFFile(open(path, "rb"))
    text = elf.get_section_by_name(".text")
    sym = [s for s in elf.get_section_by_name(".symtab").iter_symbols() if s.name == name][0]
    data = text.data()
    start = sym["st_value"]
    size = sym["st_size"] or (len(data) - start)
    words = list(struct.unpack_from(f"<{size // 4}I", data, start))
    mask = [0xFFFFFFFF] * len(words)
    for sec in elf.iter_sections():
        if isinstance(sec, RelocationSection) and sec.name in (".rel.text", ".rela.text"):
            for rel in sec.iter_relocations():
                off = rel["r_offset"] - start
                if 0 <= off < size:
                    t = rel["r_info_type"]
                    mask[off // 4] = 0xFC000000 if t == 4 else 0xFFFF0000  # R_MIPS_26 else 16-bit
    return words, mask


def retail_func(name, nwords):
    elf = ELFFile(open(os.path.join(ROOT, "SLUS_203.27"), "rb"))
    text = elf.get_section_by_name(".text")
    addr = int(name.split("_")[1], 16)
    off = text["sh_offset"] + addr - text["sh_addr"]
    raw = open(os.path.join(ROOT, "SLUS_203.27"), "rb").read()
    return list(struct.unpack_from(f"<{nwords}I", raw, off))


_GLABELS = None


def retail_size(name):
    """Size from splat's 'nonmatching NAME, SIZE'; for functions already moved to C,
    the distance to the next function in the split asm (trailing nops are trimmed later)."""
    import re
    global _GLABELS
    pat = re.compile(r"nonmatching %s, 0x([0-9A-Fa-f]+)" % name)
    files = sorted(glob.glob(os.path.join(ROOT, "asm/cod/*.s")))
    for path in files:
        for line in open(path):
            m = pat.match(line)
            if m:
                return int(m.group(1), 16)
    if _GLABELS is None:
        _GLABELS = sorted(int(m.group(1), 16) for path in files for m in
                          re.finditer(r"^glabel func_([0-9A-F]{8})", open(path).read(), re.M))
    addr = int(name.split("_")[1], 16)
    return min(a for a in _GLABELS if a > addr) - addr


def compare(name, words, mask):
    want_n = retail_size(name) // 4
    ref = retail_func(name, want_n)
    # retail sizes from splat include trailing alignment nops; trim them
    while want_n > len(words) and ref[want_n - 1] == 0:
        want_n -= 1
    ref = ref[:want_n]
    n = max(len(ref), len(words))
    diff = 0
    for i in range(n):
        a = ref[i] if i < len(ref) else None
        b = words[i] if i < len(words) else None
        m = mask[i] if i < len(mask) else 0xFFFFFFFF
        if a is None or b is None or (a & m) != (b & m):
            diff += 1
    return diff, len(ref), len(words)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", action="append")
    ap.add_argument("--flags", action="append")
    ap.add_argument("--modern-as", action="store_true", help="assemble with decompals binutils instead")
    ap.add_argument("--sony-as", action="store_true", help="(default) assemble with Sony SDK as 2.9-ee-991111")
    ap.add_argument("--bundled-as", action="store_true", help="assemble with each compiler package's own as")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    files = a.files or sorted(glob.glob(os.path.join(ROOT, "matchtest/func_*.c")))
    ccs = a.cc or (WIN + LINUX)
    flagsets = a.flags or FLAGS
    os.makedirs(OUT, exist_ok=True)
    print("function\tcompiler\tflags\tresult\tdiff_words/ref_words(ours)")
    for src in files:
        name = os.path.basename(src)[:-2]
        for cc in ccs:
            for fl in flagsets:
                tag = f"{name}.{cc}.{fl.replace(' ', '')}"
                s = os.path.join(OUT, tag + ".s")
                o = os.path.join(OUT, tag + ".o")
                ok, err = compile_c(cc, fl, src, s)
                if not ok:
                    print(f"{name}\t{cc}\t{fl}\tCC-ERROR\t{err.strip().splitlines()[-1] if err.strip() else ''}")
                    continue
                if a.modern_as:
                    ok, err = assemble(fl, s, o)
                elif a.bundled_as:  # the compiler package's own assembler, via the driver
                    ok, err = compile_c(cc, fl, src, o, "-c")
                else:  # default: Sony SDK GNU as 2.9-ee-991111, as used for the retail build
                    ok, err = assemble_sony(fl, s, o)
                if not ok:
                    print(f"{name}\t{cc}\t{fl}\tAS-ERROR\t{err.strip().splitlines()[-1] if err.strip() else ''}")
                    continue
                words, mask = func_from_obj(o, name)
                d, nref, nours = compare(name, words, mask)
                print(f"{name}\t{cc}\t{fl}\t{'MATCH' if d == 0 else 'diff'}\t{d}/{nref}({nours})", flush=True)


if __name__ == "__main__":
    main()
