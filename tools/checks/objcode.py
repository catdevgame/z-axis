"""objcode: the code and data of a build's object files as atoms (one function's or one datum's bytes) and the references
between them (the relocations), read from the objects themselves: Mach-O (the Mac), ELF (Linux) and COFF (Windows,
bigobj too). fncheck_corpus.Tree keys a replay's runtime code by the atoms a function's own code can reach
(Tree.runtime_part): a change to code it cannot reach leaves its verdict, a change to code it can reach is a new key.

    objcode.py OBJECT...          the atoms, their digests and references (a look at what the keys read)

An atom runs from one symbol of its section to the next (several names at one place are one atom; a section's bytes
before its first symbol are an atom of their own). Its digest is its section's kind, its bytes and its relocations
(offset, type, what they name): a global symbol by its name, an atom of the same object by that atom's own digest (not
by its name: the assembler numbers its local labels, l_.str.138, in the file's order, so one more string renames all
after it). A relocation that names a section, not a symbol, names the atom at its target's place, and its bytes in
the digest are cleared (they are that place, which moves when another atom of the section grows). The debug information, the unwind tables and the linker's notes are read as no atoms: no replay runs them.

Where an assembler can resolve a reference without a relocation, the reference is still there: ELF and COFF resolve a
reference to a local symbol of the same section (a static function in .text without -ffunction-sections), so every
atom of a section of more than one atom refers to every local atom of that section (unclear is in). Mach-O with
subsections_via_symbols keeps every reference between atoms (the linker moves and drops atoms one by one).
"""
import bisect
import hashlib
import re
import struct
import sys


# An anonymous namespace in an MSVC-mangled name (clang-cl): ?A0x<up to 8 hex digits>@, a digest of the source file's path, so the
# same code in two checkouts or build dirs had two names (cachekey2: the Windows worker's ck2ai1 and ck2ai9 builds of one
# commit, two bases), in its symbols and in the RTTI names in its data. Read as ?A0x00000000@.
_ANON = re.compile(r'\?A0x[0-9a-fA-F]{1,8}@')
_ANON_BYTES = re.compile(rb'\?A0x[0-9a-fA-F]{1,8}@')


def sha(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode('utf-8', 'surrogateescape'))
        h.update(b'\0')
    return h.hexdigest()[:24]


class Obj:
    """One object file's atoms.

    atoms: [(section, names, digest, local)]: names a tuple (the atom's symbols, () for one with none), local True when
    no name of it is visible to other objects. refs: [set of references] per atom: ('g', NAME) a global symbol (defined
    here or not: the link resolves it), ('a', INDEX) an atom of this object. defined: {NAME: [INDEX]} of this object's
    global symbols (weak ones in weak too). inits: atoms the program's start runs (static initializers' pointers)."""

    def __init__(self):
        self.atoms = []
        self.refs = []
        self.defined = {}
        self.weak = set()
        self.inits = []
        self.format = None

    def to_json(self):
        return {'format': self.format, 'atoms': [[s, list(n), d, l] for s, n, d, l in self.atoms],
                'refs': [sorted([k, v] for k, v in r) for r in self.refs], 'defined': self.defined,
                'weak': sorted(self.weak), 'inits': self.inits}

    @classmethod
    def from_json(cls, j):
        o = cls()
        o.format = j['format']
        o.atoms = [(s, tuple(n), d, l) for s, n, d, l in j['atoms']]
        o.refs = [set((k, v) for k, v in r) for r in j['refs']]
        o.defined = {k: list(v) for k, v in j['defined'].items()}
        o.weak = set(j['weak'])
        o.inits = list(j['inits'])
        return o


class _Builder:
    """The common part of the three readers: sections with their symbols and relocations, made into atoms."""

    def __init__(self, fmt):
        self.fmt = fmt
        self.sections = []     # [{name, data (bytes or None: zero fill), size, kind, symbols: [(offset, name, global, weak)], relocs, init}]
        self.precise = False   # every reference between atoms has a relocation (Mach-O subsections_via_symbols)

    def build(self):
        o = Obj()
        o.format = self.fmt
        where = {}             # section index -> [(start, end, atom index)]
        for si, s in enumerate(self.sections):
            if s is None:
                continue
            syms = sorted(s['symbols'])
            starts = sorted(set([0] + [off for off, _, _, _ in syms if 0 <= off <= s['size']]))
            if s['size'] == 0:
                starts = [0]
            spans = []
            for k, b in enumerate(starts):
                e = starts[k + 1] if k + 1 < len(starts) else s['size']
                if e <= b and not (s['size'] == 0 and b == 0):
                    continue
                spans.append([b, e, []])
            for off, name, glob, weak in syms:
                for sp in spans:
                    if sp[0] == off or (sp[0] <= off < sp[1]):
                        sp[2].append((name, glob, weak))
                        break
            where[si] = []
            for b, e, names in spans:
                if not names and e == b:
                    continue
                idx = len(o.atoms)
                where[si].append((b, e, idx))
                o.atoms.append([si, tuple(sorted(n for n, _, _ in names)), None, not any(g for _, g, _ in names)])
                o.refs.append(set())
                for n, g, w in names:
                    if g:
                        o.defined.setdefault(n, []).append(idx)
                        if w:
                            o.weak.add(n)

        starts = {si: [b for b, _, _ in spans] for si, spans in where.items()}
        first = {}
        for spans in where.values():
            for b, _, idx in spans:
                first[idx] = b

        def atom_at(si, off):
            """The atom of section si at offset off (past the end: the last one), or None."""
            spans = where.get(si)
            if not spans:
                return None
            k = bisect.bisect_right(starts[si], off) - 1
            return spans[max(k, 0)][2]

        # The relocations: per atom its references and the parts of its digest. A reference to this object's own
        # atom names it by that atom's own digest, not its name: the assembler numbers its local labels (l_.str.138,
        # lCPI79_0) in the order of the file, so another string anywhere before renames every one after it.
        parts = [[] for _ in o.atoms]
        local_refs = [[] for _ in o.atoms]
        clear = [[] for _ in o.atoms]
        for si, s in enumerate(self.sections):
            if s is None:
                continue
            for off, rtype, target, size in s['relocs']:
                a = atom_at(si, off)
                if a is None:
                    continue
                b = first[a]
                kind, value = target
                if kind == 'sym':                       # a symbol: global by name, or this object's local atom
                    name, glob, tsi, toff = value
                    if glob or tsi is None:
                        o.refs[a].add(('g', name))
                        desc = 'g:' + name
                    else:
                        t = atom_at(tsi, toff)
                        if t is None:
                            continue
                        o.refs[a].add(('a', t))
                        local_refs[a].append(('%d %s l:%d' % (off - b, rtype, toff - first[t]), t))
                        desc = 'l'
                elif kind == 'sect':                    # a section's place: the atom there
                    tsi, toff = value
                    t = atom_at(tsi, toff) if tsi in where else None
                    if t is None:
                        for _, _, t2 in where.get(tsi, ()):
                            o.refs[a].add(('a', t2))
                        desc = 's:%s:?' % (self.sections[tsi]['name'] if tsi in where else tsi)
                    else:
                        o.refs[a].add(('a', t))
                        local_refs[a].append(('%d %s s:%s:%d' % (off - b, rtype, self.sections[tsi]['name'], toff - first[t]), t))
                        desc = 's'
                    if size:
                        clear[a].append((off - b, size))
                else:
                    continue
                parts[a].append('%d %s %s' % (off - b, rtype, desc))
        if not self.precise:
            for si, spans in where.items():
                if len(spans) < 2:
                    continue
                local = [idx for _, _, idx in spans if o.atoms[idx][3]]
                for _, _, idx in spans:
                    o.refs[idx].update(('a', t) for t in local if t != idx)
        for si, spans in where.items():
            s = self.sections[si]
            for b, e, idx in spans:
                if s['data'] is None:
                    body = b'zero %d' % (e - b)
                else:
                    body = bytearray(s['data'][b:e])
                    for at, n in clear[idx]:
                        body[at:at + n] = bytes(min(n, max(0, len(body) - at)))
                    body = bytes(body)
                    if self.fmt == 'coff':
                        named = _ANON_BYTES.sub(b'?A0x00000000@', body)
                        if named != body:
                            # (and without its zero padding: the 7-digit name's section was padded to the 8-digit one's size)
                            body = named.rstrip(b'\0')
                o.atoms[idx][2] = sha(s['kind'], body, *sorted(parts[idx]))
                if s['init']:
                    o.inits.append(idx)
        own = [a[2] for a in o.atoms]
        for idx, refs in enumerate(local_refs):
            if refs:
                o.atoms[idx][2] = sha(own[idx], *sorted('%s %s' % (text, own[t]) for text, t in refs))
        o.atoms = [(self.sections[si]['name'], names, dg, local) for si, names, dg, local in o.atoms]
        return o


# ---- Mach-O ---------------------------------------------------------------------------------------------------------

_MACHO_SKIP_SECTIONS = ('__compact_unwind', '__eh_frame')


def _macho(data):
    magic, cpu, _, _, ncmds, _, flags, _ = struct.unpack_from('<IiiIIIII', data, 0)
    b = _Builder('macho')
    b.precise = bool(flags & 0x2000)   # MH_SUBSECTIONS_VIA_SYMBOLS
    raw, syms = [], []
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x19:                # LC_SEGMENT_64
            for i in range(struct.unpack_from('<I', data, off + 64)[0]):
                s = off + 72 + 80 * i
                sect = data[s:s + 16].rstrip(b'\0').decode('latin-1')
                seg = data[s + 16:s + 32].rstrip(b'\0').decode('latin-1')
                addr, ssize, offset, _, reloff, nreloc, sflags = struct.unpack_from('<QQIIIII', data, s + 32)
                raw.append((seg, sect, addr, ssize, offset, reloff, nreloc, sflags))
        elif cmd == 0x2:               # LC_SYMTAB
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', data, off + 8)
            for i in range(nsyms):
                strx, ntype, nsect, desc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
                end = data.index(b'\0', stroff + strx)
                syms.append((data[stroff + strx:end].decode('latin-1'), ntype, nsect, desc, value))
        off += size
    for seg, sect, addr, ssize, offset, reloff, nreloc, sflags in raw:
        if seg == '__DWARF' or sect in _MACHO_SKIP_SECTIONS or (sflags & 0x02000000 and seg != '__TEXT'):
            b.sections.append(None)    # (S_ATTR_DEBUG, the unwind tables)
            continue
        stype = sflags & 0xff
        zero = stype in (0x1, 0xc, 0x12)   # S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL
        b.sections.append({'name': '%s,%s' % (seg, sect), 'data': None if zero else data[offset:offset + ssize],
                           'size': ssize, 'kind': 'zero' if zero else ('code' if sflags & 0x80000000 else 'data'),
                           'symbols': [], 'relocs': [], 'init': stype in (0x9, 0x16), 'addr': addr,
                           'raw': (reloff, nreloc)})
    for name, ntype, nsect, desc, value in syms:
        if ntype & 0xe0 or (ntype & 0x0e) != 0x0e or not 0 < nsect <= len(b.sections):
            continue                   # (a debugger's entry, or not defined in a section)
        s = b.sections[nsect - 1]
        if s is not None:
            s['symbols'].append((value - s['addr'], name, bool(ntype & 0x01), bool(desc & 0x80)))
    for s in b.sections:
        if s is None:
            continue
        reloff, nreloc = s['raw']
        for i in range(nreloc):
            address, info = struct.unpack_from('<iI', data, reloff + 8 * i)
            if address < 0:
                continue               # (a scattered relocation: 32-bit only)
            num, ext, rtype = info & 0xffffff, (info >> 27) & 1, info >> 28
            if cpu == 0x0100000c and rtype == 10:
                continue               # ARM64_RELOC_ADDEND: the next one's addend, already in the bytes' place
            if ext:
                name, ntype, nsect, _, value = syms[num]
                defined = (ntype & 0x0e) == 0x0e and 0 < nsect <= len(b.sections) and b.sections[nsect - 1] is not None
                glob = bool(ntype & 0x01) or not defined
                target = ('sym', (name, glob, nsect - 1 if defined else None,
                                  value - b.sections[nsect - 1]['addr'] if defined else 0))
                s['relocs'].append((address, rtype, target, 0))
            elif 0 < num <= len(b.sections) and b.sections[num - 1] is not None:
                t = b.sections[num - 1]
                length = 1 << ((info >> 25) & 3)
                place = None
                if rtype == 0 and length in (4, 8) and s['data'] is not None:   # an address in the data
                    place = int.from_bytes(s['data'][address:address + length], 'little') - t['addr']
                s['relocs'].append((address, rtype, ('sect', (num - 1, place if place is not None else -1)),
                                    length if place is not None else 0))
    return b.build()


# ---- ELF ------------------------------------------------------------------------------------------------------------

def _elf(data):
    if data[4] != 2 or data[5] != 1:
        raise ValueError('ELF: only 64-bit little-endian objects')
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    machine, = struct.unpack_from('<H', data, 0x12)
    heads = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    if shnum == 0 and heads == []:
        return _Builder('elf').build()

    def strz(tab, at):
        o = heads[tab][4] + at
        return data[o:data.index(b'\0', o)].decode('latin-1')
    names = [strz(shstrndx, h[0]) for h in heads]
    b = _Builder('elf')
    for i, h in enumerate(heads):
        name, stype, flags, size, offset = names[i], h[1], h[2], h[5], h[4]
        if not flags & 0x2 or name.startswith(('.debug', '.eh_frame', '.gcc_except_table', '.note', '.comment')) \
                or stype in (2, 3, 4, 9, 0x6fff4c03):
            b.sections.append(None)    # (not loaded: symbols, strings, relocations, notes; the unwind tables)
            continue
        zero = stype == 8              # SHT_NOBITS
        b.sections.append({'name': name, 'data': None if zero else data[offset:offset + size], 'size': size,
                           'kind': 'zero' if zero else ('code' if flags & 0x4 else 'data'), 'symbols': [],
                           'relocs': [], 'init': stype in (14, 16) or name.startswith(('.init_array', '.ctors'))})
    syms = []
    for i, h in enumerate(heads):
        if h[1] != 2:                  # SHT_SYMTAB
            continue
        link, off, size, ent = h[6], h[4], h[5], h[9] or 24
        for k in range(size // ent):
            st_name, info, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', data, off + k * ent)
            name = strz(link, st_name) if st_name else ''
            bind, typ = info >> 4, info & 0xf
            syms.append((name, bind, typ, shndx, value))
            if typ in (3, 4) or not 0 < shndx < len(b.sections) or b.sections[shndx] is None:
                continue               # (a section's or a file's symbol; undefined, absolute, common)
            b.sections[shndx]['symbols'].append((value, name, bind != 0, bind == 2))
    pcrel = {2, 4, 9, 41, 42} if machine == 62 else set()   # x86-64: the place is the addend and the field's size
    for i, h in enumerate(heads):
        if h[1] != 4 or not 0 <= h[7] < len(b.sections) or b.sections[h[7]] is None:   # SHT_RELA for a loaded section
            continue
        s = b.sections[h[7]]
        off, size, ent = h[4], h[5], h[9] or 24
        for k in range(size // ent):
            r_off, info, addend = struct.unpack_from('<QQq', data, off + k * ent)
            symi, rtype = info >> 32, info & 0xffffffff
            if not symi or symi >= len(syms):
                continue
            name, bind, typ, shndx, value = syms[symi]
            loaded = 0 < shndx < len(b.sections) and b.sections[shndx] is not None
            if typ == 3:               # STT_SECTION: the place in the section
                if loaded:
                    s['relocs'].append((r_off, rtype, ('sect', (shndx, value + addend + (4 if rtype in pcrel else 0))), 0))
                continue
            glob = bind != 0 or not loaded
            s['relocs'].append((r_off, rtype, ('sym', (name, glob, shndx if loaded else None, value)), 0))
    return b.build()


# ---- COFF -----------------------------------------------------------------------------------------------------------

_COFF_SKIP = ('.debug$', '.pdata', '.xdata', '.llvm_addrsig', '.drectve', '.llvm.call-graph-profile')
_COFF_SIZES = {0x8664: {1: 8, 2: 4, 3: 4, 4: 4, 5: 4, 6: 4, 7: 4, 8: 4, 9: 4, 0xa: 2, 0xb: 4}}   # (AMD64: the in-place field)


def _coff(data):
    sig1, sig2 = struct.unpack_from('<HH', data, 0)
    if sig1 == 0 and sig2 == 0xffff:   # bigobj (ANON_OBJECT_HEADER_BIGOBJ)
        machine, = struct.unpack_from('<H', data, 6)
        nsect, symptr, nsyms = struct.unpack_from('<III', data, 44)
        sect_at, symsize, secfmt = 56, 20, '<i'
    else:
        machine, nsect, _, symptr, nsyms, optsize, _ = struct.unpack_from('<HHIIIHH', data, 0)
        sect_at, symsize, secfmt = 20 + optsize, 18, '<h'
    strtab = symptr + nsyms * symsize

    def name_of(raw):
        if raw[:4] == b'\0\0\0\0':
            at = strtab + struct.unpack_from('<I', raw, 4)[0]
            return _ANON.sub('?A0x00000000@', data[at:data.index(b'\0', at)].decode('latin-1'))
        return _ANON.sub('?A0x00000000@', raw.rstrip(b'\0').decode('latin-1'))
    b = _Builder('coff')
    heads = []
    for i in range(nsect):
        h = sect_at + 40 * i
        raw = data[h:h + 8].rstrip(b'\0')
        name = raw.decode('latin-1')
        if name.startswith('/'):
            at = strtab + int(name[1:])
            name = data[at:data.index(b'\0', at)].decode('latin-1')
        _, _, rawsize, rawptr, relptr, _, nrel, _, chars = struct.unpack_from('<IIIIIIHHI', data, h + 8)
        if chars & 0x01000000 and nrel == 0xffff:   # IMAGE_SCN_LNK_NRELOC_OVFL: the count in the first relocation
            nrel = struct.unpack_from('<I', data, relptr)[0]
            relptr += 10
            nrel -= 1
        heads.append((relptr, nrel))
        if name.startswith(_COFF_SKIP) or chars & 0x800 or chars & 0x200 or not chars & 0xe0:
            b.sections.append(None)    # (LNK_REMOVE, LNK_INFO, no code or data)
            continue
        zero = bool(chars & 0x80) and not rawptr
        b.sections.append({'name': name.split('$')[0] if name.startswith('.text') else name,
                           'data': None if zero else data[rawptr:rawptr + rawsize], 'size': rawsize,
                           'kind': 'zero' if zero else ('code' if chars & 0x20 else 'data'), 'symbols': [], 'relocs': [],
                           'init': name.startswith('.CRT$XC') and name != '.CRT$XCA' and name != '.CRT$XCZ'})
    syms = {}
    k = 0
    while k < nsyms:
        at = symptr + k * symsize
        value, = struct.unpack_from('<I', data, at + 8)
        sec, = struct.unpack_from(secfmt, data, at + 12)
        sc, naux = struct.unpack_from('<BB', data, at + symsize - 2)
        name = name_of(data[at:at + 8])
        syms[k] = (name, sc, sec, value, naux)
        if 0 < sec <= nsect and b.sections[sec - 1] is not None and sc in (2, 3, 6) and not (sc == 3 and naux):
            # (EXTERNAL, STATIC, LABEL; a STATIC with an aux record is the section's own symbol)
            b.sections[sec - 1]['symbols'].append((value, name, sc == 2, False))
        k += 1 + naux
    sizes = _COFF_SIZES.get(machine, {})
    for i, (relptr, nrel) in enumerate(heads):
        s = b.sections[i]
        if s is None:
            continue
        for r in range(nrel):
            va, symi, rtype = struct.unpack_from('<IIH', data, relptr + 10 * r)
            if symi not in syms:
                continue
            name, sc, sec, value, naux = syms[symi]
            loaded = 0 < sec <= nsect and b.sections[sec - 1] is not None
            if sc == 3 and naux and loaded:   # the section's symbol: the place is in the bytes
                n = sizes.get(rtype, 4)
                place = int.from_bytes(s['data'][va:va + n], 'little', signed=n == 4) if s['data'] else 0
                if rtype in (4, 5, 6, 7, 8, 9):   # REL32 and REL32_1..5: from the field's end
                    place += 4 + (rtype - 4)
                s['relocs'].append((va, rtype, ('sect', (sec - 1, place)), n))
                continue
            glob = sc in (2, 105) or not loaded
            s['relocs'].append((va, rtype, ('sym', (name, glob, sec - 1 if loaded else None, value)), 0))
    return b.build()


def read(path):
    """The Obj of an object file (Mach-O, ELF or COFF); an empty one for what is none of them."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] == b'\xcf\xfa\xed\xfe':
        return _macho(data)
    if data[:4] == b'\x7fELF':
        return _elf(data)
    sig1, sig2 = struct.unpack_from('<HH', data, 0) if len(data) >= 4 else (None, None)
    if (sig1 == 0 and sig2 == 0xffff) or sig1 in (0x8664, 0xaa64, 0x14c):
        return _coff(data)
    o = Obj()
    o.format = 'none'
    return o


class Program:
    """The atoms of a link's objects with their references resolved by name, as the linker does: a strong definition
    first, else the weak ones (an inline function in every object that uses it: identical, so one node whatever the
    number of objects that define it). Nodes are (object index, atom index); a name no object defines is ('ext', NAME)
    (the system's libraries, pinned by the compiler and the SDK in the base)."""

    def __init__(self, objs):
        self.objs = objs                     # [(label, Obj)]
        self.by_name = {}
        weak = {}
        for oi, (_, o) in enumerate(objs):
            for name, idxs in o.defined.items():
                if name in o.weak:
                    weak.setdefault(name, []).extend((oi, i) for i in idxs)
                else:
                    self.by_name.setdefault(name, []).extend((oi, i) for i in idxs)
        for name, nodes in weak.items():
            self.by_name.setdefault(name, nodes)
        for name, nodes in self.by_name.items():
            if len(nodes) > 1:
                # (copies alike, an inline function in every object that uses it: one node for each different one)
                one = {}
                for oi, ai in nodes:
                    one.setdefault(objs[oi][1].atoms[ai][2], (oi, ai))
                self.by_name[name] = sorted(one.values())

    def refs(self, node):
        oi, ai = node
        out = []
        for kind, v in self.objs[oi][1].refs[ai]:
            if kind == 'a':
                out.append((oi, v))
            else:
                out.extend(self.by_name.get(v) or [('ext', v)])
        return out

    def label(self, node):
        """What names a node in a digest: a global symbol by its name alone (the object it is in is the link's
        choice), a local atom by its object's label, section and names."""
        if node[0] == 'ext':
            return 'ext ' + node[1]
        olabel, o = self.objs[node[0]]
        section, names, digest, local = o.atoms[node[1]]
        if not local:
            return 'g %s %s' % (','.join(n for n in names if n in o.defined), digest)
        return 'l %s %s %s' % (olabel, section, digest)   # (not its names: the assembler numbers them)

    def walk(self, roots, stop=lambda node: False, content=lambda node: True):
        """The nodes reachable from roots: stop(node) ends a path there (the node not taken), content(node) says
        whether a taken node's digest is in the result. Returns (taken nodes, their labels sorted)."""
        seen, todo, labels = set(), list(roots), []
        while todo:
            node = todo.pop()
            if node in seen:
                continue
            seen.add(node)
            if stop(node):
                continue
            if node[0] == 'ext':
                labels.append(self.label(node))
                continue
            if content(node):
                labels.append(self.label(node))
            todo.extend(n for n in self.refs(node) if n not in seen)
        return seen, sorted(set(labels))


def main(argv):
    for path in argv:
        o = read(path)
        print('%s: %s, %d atoms, %d inits' % (path, o.format, len(o.atoms), len(o.inits)))
        for i, (section, names, digest, local) in enumerate(o.atoms):
            print('  %4d %-24s %s %s %s -> %s' % (i, section, digest, 'local' if local else 'global',
                                                  ','.join(names) or '-', ' '.join(sorted('%s:%s' % r for r in o.refs[i]))[:200]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
