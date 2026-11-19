#!/usr/bin/env python3
"""A PS2 disc image as the build's tools read it: an ISO of 2048-byte sectors.

Any game's disc may come in any of the usual formats; which one, its bytes tell (never its name):
- an ISO: as it is;
- a BIN/CUE dump (Redump's form of the CD games), by its CUE or its BIN: raw 2352-byte sectors, which bsdtar and
  Windows' tar do not read. This makes the 2048-byte ISO from the CUE's data track (a multi-track dump's audio
  tracks left out): each sector's user data (Mode 1 at byte 16, Mode 2 Form 1 at byte 24), the same bytes the app's
  own reader (iop Iso9660Image) takes;
- a CHD, CSO or ZSO (compressed): the app's own reader makes the ISO, ps2x_disc (built with the recompiler by
  tooling/recompile.sh; PS2X_DISC=<path> names another).

    disc_image.py is-raw IMAGE        exit 0 when IMAGE is no ISO tar reads (BIN/CUE, CHD, CSO, ZSO), 1 for an ISO
    disc_image.py to-iso IMAGE OUT    OUT: the 2048-byte ISO of IMAGE
"""
import os
import subprocess
import sys

RAW = 2352
USER = 2048
SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])
PVD_SECTOR = 16
COMPRESSED = (b'MComprHD', b'CISO', b'ZISO')
CUE_COMMANDS = {'CATALOG', 'CDTEXTFILE', 'FILE', 'FLAGS', 'INDEX', 'ISRC', 'PERFORMER', 'POSTGAP', 'PREGAP', 'REM',
                'SONGWRITER', 'TITLE', 'TRACK'}
CHECKOUT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def cue_lines(path):
    """A CUE sheet's lines (by its text, whatever its name), or None for any other file."""
    if os.path.getsize(path) > 256 * 1024:
        return None
    with open(path, 'rb') as f:
        data = f.read()
    if b'\0' in data:
        return None
    text = data.decode('utf-8', errors='replace').lstrip('﻿')
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    commands = [line.split()[0].upper() for line in lines]
    if not lines or any(c not in CUE_COMMANDS for c in commands) or 'FILE' not in commands or 'TRACK' not in commands:
        return None
    return lines


def bin_of_cue(cue, lines):
    """The file a CUE sheet's first data track (MODE1/..., MODE2/...) is in, beside it."""
    name = None
    for line in lines:
        command = line.split()[0].upper()
        if command == 'FILE':
            rest = line[4:].strip()
            if rest.startswith('"'):
                name = rest[1:rest.index('"', 1)] if '"' in rest[1:] else rest[1:]
            else:
                name = rest.split(' ')[0]
        elif command == 'TRACK' and name is not None:
            parts = line.split()
            if len(parts) >= 3 and parts[2].upper().startswith(('MODE1', 'MODE2')):
                path = os.path.join(os.path.dirname(os.path.abspath(cue)), name)
                if not os.path.isfile(path):
                    sys.exit('disc_image: %s names %s, which is not beside it' % (cue, name))
                return path
    sys.exit('disc_image: %s names no data track' % cue)


def data_file(image):
    lines = cue_lines(image)
    return bin_of_cue(image, lines) if lines is not None else image


def compressed(path):
    with open(path, 'rb') as f:
        head = f.read(8)
    return head.startswith(COMPRESSED)


def raw_layout(path):
    """The user data's offset in each 2352-byte sector (16 or 24), or None for a 2048-byte image."""
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        f.seek(PVD_SECTOR * USER)
        if f.read(6)[1:6] == b'CD001':
            return None
        if size % RAW or size < (PVD_SECTOR + 1) * RAW:
            return None
        f.seek(PVD_SECTOR * RAW)
        head = f.read(16)
        if head[:12] != SYNC or head[15] not in (1, 2):
            return None
        offset = 16 if head[15] == 1 else 24
        f.seek(PVD_SECTOR * RAW + offset)
        return offset if f.read(6)[1:6] == b'CD001' else None


def ps2x_disc():
    tool = os.environ.get('PS2X_DISC') or os.path.join(CHECKOUT, 'out', 'build-recomp', 'ps2xIOP',
                                                       'ps2x_disc.exe' if os.name == 'nt' else 'ps2x_disc')
    if not os.path.isfile(tool):
        sys.exit('disc_image: a compressed image (CHD, CSO, ZSO) needs ps2x_disc (%s): run tooling/recompile.sh first, '
                 'or PS2X_DISC=<path>' % tool)
    return tool


def to_iso(image, out):
    path = data_file(image)
    if compressed(path):
        subprocess.run([ps2x_disc(), 'to-iso', path, out], check=True)
        return
    offset = raw_layout(path)
    if offset is None:
        sys.exit('disc_image: %s is not a raw (2352-byte sector) image' % image)
    tmp = out + '.partial'
    with open(path, 'rb') as src, open(tmp, 'wb') as dst:
        while True:
            block = src.read(RAW * 512)
            if not block:
                break
            dst.write(b''.join(block[i + offset:i + offset + USER] for i in range(0, len(block), RAW)))
    os.replace(tmp, out)
    print('disc_image: %s -> %s (%d sectors)' % (os.path.basename(path), out, os.path.getsize(out) // USER))


def main(argv):
    if len(argv) == 3 and argv[1] == 'is-raw':
        path = data_file(argv[2])
        return 0 if compressed(path) or raw_layout(path) is not None else 1
    if len(argv) == 4 and argv[1] == 'to-iso':
        to_iso(argv[2], argv[3])
        return 0
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
