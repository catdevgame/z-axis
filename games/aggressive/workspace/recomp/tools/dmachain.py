#!/usr/bin/env python3
"""Walk an EE VIF1 DMA source chain (TTE on) in a RAM dump and decode the VIF stream.
usage: dmachain.py <ram.bin> <tadr hex> [max_tags] [-v]"""
import struct, sys
ram = open(sys.argv[1], 'rb').read()
tadr = int(sys.argv[2], 16)
maxtags = int(sys.argv[3]) if len(sys.argv) > 3 else 200
verbose = '-v' in sys.argv
def mem(a, n): a &= 0x1FFFFFF; return ram[a:a+n]
NAMES = {0:'NOP',1:'STCYCL',2:'OFFSET',3:'BASE',4:'ITOP',5:'STMOD',6:'MSKPATH3',7:'MARK',0x10:'FLUSHE',0x11:'FLUSH',0x13:'FLUSHA',0x14:'MSCAL',0x15:'MSCALF',0x17:'MSCNT',0x20:'STMASK',0x30:'STROW',0x31:'STCOL',0x4A:'MPG',0x50:'DIRECT',0x51:'DIRECTHL'}
counts = {}
def note(k): counts[k] = counts.get(k, 0) + 1
pending = 0  # bytes still owed to a command spanning DMA packets
def decode(stream, label):
    global pending
    p = 0; out = []
    if pending:
        take = min(pending, len(stream)); p += take; pending -= take
    while p + 4 <= len(stream):
        c = struct.unpack_from('<I', stream, p)[0]; p += 4
        cmd = (c >> 24) & 0x7F; num = (c >> 16) & 0xFF; imm = c & 0xFFFF
        if cmd >= 0x60:
            vn = (cmd >> 2) & 3; vl = cmd & 3; n = num or 256
            size = ((32 >> vl) * (vn + 1) * n + 31) // 32 * 4
            name = 'UNPACK V%d-%d' % (vn + 1, 32 >> vl); note('UNPACK')
            out.append('%s n=%d addr=0x%x%s' % (name, n, imm & 0x3FF, ' +tops' if imm & 0x8000 else ''))
        elif cmd == 0x4A:
            n = num or 256; size = n * 8; note('MPG'); out.append('MPG n=%d addr=0x%x' % (n, imm))
        elif cmd in (0x50, 0x51):
            n = imm or 65536; p = ((p + 8 + 15) & ~15) - 8; size = n * 16; note(NAMES[cmd])
            if p + 16 <= len(stream):
                lo, hi = struct.unpack_from('<QQ', stream, p)
                out.append('%s qwc=%d giftag nloop=%d eop=%d pre=%d prim=0x%x flg=%d nreg=%d regs=0x%x' % (NAMES[cmd], n, lo & 0x7FFF, (lo >> 15) & 1, (lo >> 46) & 1, (lo >> 47) & 0x7FF, (lo >> 58) & 3, (lo >> 60) & 15, hi))
            else:
                out.append('%s qwc=%d' % (NAMES[cmd], n))
        elif cmd in (0x30, 0x31): size = 16; note(NAMES[cmd]); out.append(NAMES[cmd])
        elif cmd == 0x20: size = 4; note('STMASK'); out.append('STMASK')
        else:
            size = 0; nm = NAMES.get(cmd, 'cmd%02x' % cmd); note(nm)
            if cmd != 0: out.append('%s imm=0x%x' % (nm, imm))
        if p + size > len(stream):
            pending = p + size - len(stream); p = len(stream)
        else:
            p += size
    return out
ids = {0:'refe',1:'cnt',2:'next',3:'ref',4:'refs',5:'call',6:'ret',7:'end'}
stack = []
for i in range(maxtags):
    lo, hi = struct.unpack_from('<II', mem(tadr, 8))
    qwc = lo & 0xFFFF; tid = (lo >> 28) & 7
    data = tadr + 16 if tid in (1, 2, 5, 6, 7) else hi
    stream = mem(tadr + 8, 8) + mem(data, qwc * 16)
    out = decode(stream, '')
    if verbose or i < 40:
        print('%3d @0x%x %s qwc=%d addr=0x%x' % (i, tadr, ids[tid], qwc, hi))
        for x in out[:8]: print('        ' + x)
        if len(out) > 8: print('        ... %d more' % (len(out) - 8))
    if tid == 1: tadr = data + qwc * 16
    elif tid == 2: tadr = hi
    elif tid in (3, 4): tadr += 16
    elif tid == 5: stack.append(data + qwc * 16); tadr = hi
    elif tid == 6:
        if not stack: break
        tadr = stack.pop()
    else: break
print('tags walked:', i + 1, 'end id:', ids[tid])
print('VIF command counts:', counts)
