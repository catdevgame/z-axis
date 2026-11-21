"""Minimal ELF32 LE parser for PS2 EE executables (data only)."""
import struct, sys

class Elf:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        d = self.data
        assert d[:4] == b'\x7fELF'
        (self.entry,) = struct.unpack_from('<I', d, 0x18)
        phoff, shoff = struct.unpack_from('<II', d, 0x1c)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x2a)
        self.phdrs = []
        for i in range(phnum):
            p = struct.unpack_from('<8I', d, phoff + i * phentsize)
            self.phdrs.append(dict(type=p[0], off=p[1], vaddr=p[2], filesz=p[4], memsz=p[5], flags=p[6]))
        self.sections = []
        raw = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
        if shnum:
            stro = raw[shstrndx][4]
            for s in raw:
                name = d[stro + s[0]: d.index(b'\0', stro + s[0])].decode('latin1')
                self.sections.append(dict(name=name, type=s[1], flags=s[2], addr=s[3], off=s[4], size=s[5]))

    def section(self, name):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None

    def read(self, addr, n):
        for p in self.phdrs:
            if p['type'] == 1 and p['vaddr'] <= addr < p['vaddr'] + p['filesz']:
                o = p['off'] + addr - p['vaddr']
                return self.data[o:o + n]
        return None

    def words(self, start, end):
        b = self.read(start, end - start)
        return list(struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4]))

if __name__ == '__main__':
    for path in sys.argv[1:]:
        e = Elf(path)
        print('==', path, 'entry 0x%x' % e.entry)
        for p in e.phdrs:
            print('  PH type %d off 0x%x vaddr 0x%x filesz 0x%x memsz 0x%x flags %d' % (p['type'], p['off'], p['vaddr'], p['filesz'], p['memsz'], p['flags']))
        for s in e.sections:
            print('  %-24s type %2d addr 0x%08x off 0x%06x size 0x%06x' % (s['name'], s['type'], s['addr'], s['off'], s['size']))
