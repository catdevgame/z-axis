"""A recording's file (<address>.fnrec) in either form it is kept in: plain, or compressed with zstd
(runtime/src/game/ee/fnrec_file.h, the format; zx-fncheck and the recorder read and write it there).

The compressed form is a zstd stream whose decompressed bytes are the plain file. It begins with a skippable frame
holding the plain file's header and size (the head), then frames of whole records with their checksums. The head
is read without zstd (plain bytes), so the size every tool keys a file by is the plain one wherever it is known:

    key_size(path)          the plain file's size (a compressed file's head names it), else the file's own size
                            (a compressed file the recorder wrote: its head does not know it)
    plain_size(path)        the plain file's size, decompressing to count it when the head does not know it
    head(path)              {'packed', 'plain_bytes', 'header'}: header the plain file's first 24 bytes, or None
    open_plain(path)        a binary file object over the plain bytes (read, tell, forward seek)
    pack(src, dst)          the converter's compression of one plain file (fnrec_pack.py): records grouped into
                            frames of at least PACK_FRAME bytes at PACK_LEVEL, the head naming the plain size

The library is zstd's (1.5.7, as runtime/cmake/zx_zstd.cmake pins): Python 3.14's compression.zstd where there is
one (the Mac's Python), else the zstandard module (the Windows worker's Python 3.12); only open_plain and pack need it.
"""
import io
import os
import struct

PACKED_MAGIC = 0x184D2A50            # fnrec_file.h kPackedMagic (a zstd skippable frame)
ZSTD_MAGIC = 0xFD2FB528
HEAD = struct.Struct('<II4sIQ24s')   # skippable magic, its size (40), 'FNRZ', head version, plain bytes, header
HEAD_TAG = b'FNRZ'
HEADER = struct.Struct('<8sIIII')    # fncheck::FileHeader: magic, version, address, context size, game
RECORD_MAGIC = 0x4C4C4143            # "CALL"
LINE = 24                            # fncheck::Line
PACK_LEVEL = 9                       # fnrec_file.h kPackLevel
PACK_FRAME = 64 << 10                # a converted file's frames: whole records, at least this many bytes (a
                                     # record's lookup decompresses at most this much before it; 1 MiB: 2% smaller)

try:
    from compression import zstd as _zstd   # Python 3.14 on
    _zstandard = None
except ImportError:
    _zstd = None
    try:
        import zstandard as _zstandard
    except ImportError:
        _zstandard = None


def library():
    """The zstd library in use: (module name, zstd version), or None."""
    if _zstd is not None:
        return ('compression.zstd', '.'.join(map(str, _zstd.zstd_version_info)))
    if _zstandard is not None:
        return ('zstandard', _zstandard.ZSTD_VERSION if isinstance(_zstandard.ZSTD_VERSION, str)
                else '.'.join(map(str, _zstandard.ZSTD_VERSION)))
    return None


def _need_library():
    if _zstd is None and _zstandard is None:
        raise RuntimeError('fnrec_io: a compressed recording needs zstd: Python 3.14 (compression.zstd) or the '
                           'zstandard module')


def is_packed(first):
    """True if the bytes begin a compressed recording (a zstd frame or a skippable one)."""
    if len(first) < 4:
        return False
    magic = struct.unpack_from('<I', first)[0]
    return magic == ZSTD_MAGIC or (magic & 0xFFFFFFF0) == 0x184D2A50


def parse_head(first):
    """head() of a file's first HEAD.size bytes."""
    if not is_packed(first):
        return {'packed': False, 'plain_bytes': None, 'header': first[:HEADER.size] if len(first) >= HEADER.size else None}
    if len(first) >= HEAD.size:
        magic, size, tag, _, plain, header = HEAD.unpack_from(first)
        if magic == PACKED_MAGIC and size == HEAD.size - 8 and tag == HEAD_TAG:
            return {'packed': True, 'plain_bytes': plain or None, 'header': header}
    return {'packed': True, 'plain_bytes': None, 'header': None}


def head(path):
    with open(path, 'rb') as f:
        return parse_head(f.read(HEAD.size))


def key_size(path, st=None):
    """The size a file is keyed and compared by (the verdict cache, the corpus syncs): a .fnrec's plain size where
    known, so a file compressed in place (its mtime kept) keys as it did; any other file's own size."""
    st = st or os.stat(path)
    if not path.endswith('.fnrec') or st.st_size < 4:
        return st.st_size
    try:
        h = head(path)
    except OSError:
        return st.st_size
    return h['plain_bytes'] if h['plain_bytes'] else st.st_size


class _Plain(io.RawIOBase):
    """The plain bytes of a compressed file: read, tell, and seek forward (by reading) or back (from the start)."""

    def __init__(self, path):
        self.path = path
        self._open()

    def _open(self):
        self.raw = open(self.path, 'rb')
        if _zstd is not None:
            self.reader = _zstd.ZstdFile(self.raw, 'rb')
        else:
            self.reader = _zstandard.ZstdDecompressor().stream_reader(self.raw, read_across_frames=True)
        self.pos = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def readinto(self, b):
        data = self.reader.read(len(b))
        n = len(data)
        b[:n] = data
        self.pos += n
        return n

    def tell(self):
        return self.pos

    def seek(self, offset, whence=io.SEEK_SET):
        target = offset if whence == io.SEEK_SET else self.pos + offset if whence == io.SEEK_CUR else None
        if target is None:
            raise io.UnsupportedOperation('fnrec_io: no seek from the end of a compressed recording')
        if target < self.pos:
            self.close_files()
            self._open()
        while self.pos < target:
            got = self.reader.read(min(target - self.pos, 1 << 20))
            if not got:
                self.pos = target   # (past the end, as a plain file's seek: the next read gives nothing)
                break
            self.pos += len(got)
        return self.pos

    def close_files(self):
        self.reader.close()
        self.raw.close()

    def close(self):
        if not self.closed:
            self.close_files()
        super().close()


def plain_size(path):
    """The plain file's size: a plain file's own, a compressed one's head's, or (a head that does not know it, the
    recorder's) the bytes it decompresses to."""
    h = head(path)
    if not h['packed']:
        return os.path.getsize(path)
    if h['plain_bytes']:
        return h['plain_bytes']
    n = 0
    with open_plain(path) as f:
        while True:
            got = f.read(8 << 20)
            if not got:
                return n
            n += len(got)


def open_plain(path, buffering=1 << 16):
    """A recording's plain bytes as a binary file (an open() for either form)."""
    with open(path, 'rb') as f:
        packed = is_packed(f.read(4))
    if not packed:
        return open(path, 'rb', buffering=buffering)
    _need_library()
    return io.BufferedReader(_Plain(path), buffer_size=buffering)


def _frame_compressor(level):
    """A function compressing one frame from pieces: frame(size, pieces) -> the frame's bytes, pieces an iterable
    of bytes that add up to size (the frame names its content size and has its checksum)."""
    _need_library()
    if _zstd is not None:
        options = {_zstd.CompressionParameter.compression_level: level, _zstd.CompressionParameter.checksum_flag: 1,
                   _zstd.CompressionParameter.content_size_flag: 1}

        def frame(size, pieces):
            c = _zstd.ZstdCompressor(options=options)
            c.set_pledged_input_size(size)
            out = [c.compress(p) for p in pieces]
            out.append(c.flush(_zstd.ZstdCompressor.FLUSH_FRAME))
            return b''.join(out)
    else:
        z = _zstandard.ZstdCompressor(level=level, write_checksum=True, write_content_size=True)

        def frame(size, pieces):
            c = z.compressobj(size=size)
            out = [c.compress(p) for p in pieces]
            out.append(c.flush())
            return b''.join(out)
    return frame


def pack(src, dst, level=PACK_LEVEL, frame_bytes=PACK_FRAME, piece=8 << 20):
    """Compresses the plain recording src into dst: the head (plain size, header), then the records grouped into
    frames of at least frame_bytes (a cut-short tail goes in the last as it is), read in pieces of at most `piece`
    bytes (memory stays flat whatever the file's size). Returns (plain bytes, packed bytes)."""
    frame = _frame_compressor(level)
    size = os.path.getsize(src)
    with open(src, 'rb') as f:
        header = f.read(HEADER.size)
        if len(header) < HEADER.size or not header.startswith(b'FNREC1'):
            raise ValueError('%s: not a plain recording' % src)
        _, version, _, ctx, _ = HEADER.unpack(header)
        ends = record_ends(f, size, version, ctx)
    packed = 0
    with open(src, 'rb') as f, open(dst, 'wb') as out:
        out.write(HEAD.pack(PACKED_MAGIC, HEAD.size - 8, HEAD_TAG, 1, size, header))
        packed += HEAD.size
        start = 0
        for end in ends + [size]:
            if (end - start < frame_bytes and end != size) or end <= start:
                continue

            def pieces(left=end - start):
                while left > 0:
                    data = f.read(min(left, piece))
                    if not data:
                        raise IOError('%s: changed while it was read' % src)
                    left -= len(data)
                    yield data
            blob = frame(end - start, pieces())
            out.write(blob)
            packed += len(blob)
            start = end
    return size, packed


def record_ends(f, size, version, ctx):
    """Where each whole record of a plain file ends (its last byte + 1), read from f positioned after the header
    (fncheck.h's record layout, as fnrec_index.py reads it); stops at the first record it cannot follow (a tail cut
    short by a kill stays in the last frame as it is)."""
    ends = []
    head_bytes = 28 + (8 if version >= 3 else 0)
    call_bytes = (12 if version >= 2 else 8) + 64 + (64 if version >= 5 else 0) + 32 + ctx

    def u32():
        raw = f.read(4)
        if len(raw) < 4:
            raise EOFError
        return struct.unpack('<I', raw)[0]

    def lines():
        n = u32()
        f.seek(n * LINE, 1)

    pos = HEADER.size
    try:
        while pos + head_bytes <= size:
            raw = f.read(8)
            if len(raw) < 8:
                break
            magic, flags = struct.unpack('<II', raw)
            if magic != RECORD_MAGIC:
                break
            f.seek(head_bytes - 8 + 2 * ctx, 1)
            lines()
            lines()
            for _ in range(u32()):
                f.seek(call_bytes, 1)
                lines()
                if version >= 7:
                    lines()
            if version >= 5 and flags & 2:
                lines()
            end = f.tell()
            if end > size:
                break
            ends.append(end)
            pos = end
    except EOFError:
        pass
    return ends
