#!/usr/bin/env python3
"""Disassembles an Aggressive Inline level script (levels\\<L>\\<L>.BIN).

The format and the opcodes are described in
games/aggressive/script/script.h. Names of objects, states and events are
name hashes (zge::nameHash); give the matching .LOG (the script compiler's
log on the disc) or a word list with --names to show them by name.

    aggressive_script_dis.py TUT.BIN [--names TUT.LOG] [--stats]
"""
import argparse
import re
import struct
import sys

OPCODES = {
    0x0A: ('push', 'imm'), 0x14: ('store', 'var'), 0x1E: ('call', 'native'),
    0x28: ('send', 'object'), 0x32: ('setState', 'object'),
    0x3C: ('load', 'var'), 0x3D: ('pushAddress', 'var'),
    0x46: ('add', 'imm'), 0x47: ('add', 'var'), 0x50: ('sub', 'imm'), 0x51: ('sub', 'var'),
    0x5A: ('mul', 'imm'), 0x5B: ('mul', 'var'), 0x64: ('div', 'imm'), 0x65: ('div', 'var'),
    0x6E: ('jumpIfNotZero', 'rel'), 0x78: ('jumpIfZero', 'rel'), 0x82: ('jumpIfNegative', 'rel'),
    0x8C: ('jumpIfPositive', 'rel'), 0x96: ('jumpIfNotPositive', 'rel'), 0xA0: ('jumpIfNotNegative', 'rel'),
    0xAA: ('jump', 'rel'), 0xB4: ('pick', 'imm'), 0xBE: ('setType', 'imm'),
    0xC8: ('or', 'imm'), 0xC9: ('or', 'var'), 0xD2: ('and', 'imm'), 0xD3: ('and', 'var'),
    0xDC: ('shl', 'imm'), 0xDD: ('shl', 'var'), 0xE6: ('shr', 'imm'), 0xE7: ('shr', 'var'),
    0x1F40: ('end', None),
}
END = 0xFFFFFFFF


def name_hash(s):
    h = 0
    for c in s.upper().encode('latin-1'):
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


def words(b):
    return list(struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('script')
    ap.add_argument('--names', action='append', default=[], help='a .LOG or a text file: identifiers to hash')
    ap.add_argument('--stats', action='store_true', help='opcode counts only')
    a = ap.parse_args()
    d = open(a.script, 'rb').read()
    version, code_words, var_words, data_bytes, string_bytes = struct.unpack_from('<fIIII', d, 0)
    at = 0x14
    strings = d[at:at + string_bytes]; at += string_bytes
    data = d[at:at + data_bytes]; at += data_bytes
    variables = words(d[at:at + var_words * 4]); at += var_words * 4
    code = words(d[at:at + code_words * 4])
    names = {}
    for path in a.names:
        for ident in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', open(path, encoding='latin-1').read()):
            names.setdefault(name_hash(ident), ident)
    for w in ('enter', 'exit'):
        names.setdefault(name_hash(w), w)
    n = lambda h: names.get(h, '%08x' % h)
    counts = {}
    out = []
    out.append('version %.2f: code %d words, variables %d words, data %d bytes, strings %d bytes'
               % (version, code_words, var_words, data_bytes, string_bytes))
    o = 0
    while code[o] != END:
        obj_hash, obj_off = code[o], code[o + 1]
        rec = obj_off
        var_index = code[rec]
        out.append('object %s (variables at word %d, initial state %s)' % (n(obj_hash), var_index, n(variables[var_index])))
        s = rec + 2
        while code[s] != END:
            st_hash, st_off = code[s], code[s + 1]
            st = rec + st_off
            out.append('  state %s' % n(st_hash))
            e = st
            while code[e] != END:
                ev_hash, ev_off = code[e], code[e + 1]
                ip = st + ev_off
                out.append('    event %s' % n(ev_hash))
                k = 0
                while True:
                    op, arg = code[ip + 2 * k], code[ip + 2 * k + 1]
                    name, kind = OPCODES.get(op, ('op%x' % op, 'imm'))
                    counts[name] = counts.get(name, 0) + 1
                    if op == 0x1F40:
                        break
                    sarg = '%d' % struct.unpack('<i', struct.pack('<I', arg))[0]
                    if kind == 'rel':
                        sarg = '-> %d' % (k + 1 + struct.unpack('<i', struct.pack('<I', arg))[0])
                    elif kind in ('object', 'native'):
                        sarg = n(arg)
                    elif kind == 'imm' and arg > 0xFFFF and arg != END:
                        sarg = '%s (%g)' % (n(arg), struct.unpack('<f', struct.pack('<I', arg))[0])
                    out.append('      %4d %-18s %s' % (k, name, sarg))
                    k += 1
                e += 2
            s += 2
        o += 2
    if a.stats:
        for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
            print('%6d %s' % (v, k))
    else:
        print('\n'.join(out))


if __name__ == '__main__':
    main()
