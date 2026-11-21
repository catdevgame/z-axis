"""Minimal PINE client for PCSX2 (https://github.com/GovanifY/pine).

Wire format, little-endian:
  request : [u32 total size incl. this header][u8 opcode][args]...  (several
            commands may be batched behind one size header)
  reply   : [u32 total size][u8 result 0=OK 0xFF=FAIL][data of every command]

On macOS PCSX2 listens on a Unix socket "$TMPDIR/pcsx2.sock" for the default
slot 28011 and "$TMPDIR/pcsx2.sock.<slot>" for any other slot (falls back to
/tmp when TMPDIR is unset; Linux uses XDG_RUNTIME_DIR). See pcsx2/PINE.cpp.
"""

import os
import socket
import struct

DEFAULT_SLOT = 28011

OP_READ8, OP_READ16, OP_READ32, OP_READ64 = 0, 1, 2, 3
OP_WRITE8, OP_WRITE16, OP_WRITE32, OP_WRITE64 = 4, 5, 6, 7
OP_VERSION, OP_SAVESTATE, OP_LOADSTATE = 8, 9, 0xA
OP_TITLE, OP_ID, OP_UUID, OP_GAMEVERSION, OP_STATUS = 0xB, 0xC, 0xD, 0xE, 0xF

STATUS_NAMES = {0: "running", 1: "paused", 2: "shutdown"}

# Keep batches well under PCSX2's MAX_IPC_SIZE (650000) / MAX_IPC_RETURN_SIZE
# (450000).
_MAX_BATCH_READS = 8192


class PineError(RuntimeError):
    pass


def socket_path(slot=DEFAULT_SLOT):
    base = os.environ.get("TMPDIR") if os.uname().sysname == "Darwin" else os.environ.get("XDG_RUNTIME_DIR")
    base = base or "/tmp"
    path = os.path.join(base, "pcsx2.sock")
    if slot != DEFAULT_SLOT:
        path += ".%d" % slot
    return path


class Pine:
    def __init__(self, slot=DEFAULT_SLOT, timeout=10.0):
        self.slot = slot
        self.path = socket_path(slot)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        try:
            self.sock.connect(self.path)
        except OSError as e:
            raise PineError("cannot connect to %s (is PCSX2 running with EnablePINE and PINESlot=%d?): %s"
                            % (self.path, slot, e))

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- transport ---------------------------------------------------------
    def _recv_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise PineError("PINE connection closed")
            buf += chunk
        return bytes(buf)

    def transact(self, body):
        """Send one (possibly batched) request body, return reply data."""
        self.sock.sendall(struct.pack("<I", len(body) + 4) + body)
        size = struct.unpack("<I", self._recv_exact(4))[0]
        rest = self._recv_exact(size - 4)
        if rest[0] != 0:
            raise PineError("PINE command failed (is a game running?)")
        return rest[1:]

    # -- memory ------------------------------------------------------------
    def read8(self, addr):
        return self.transact(struct.pack("<BI", OP_READ8, addr))[0]

    def read16(self, addr):
        return struct.unpack("<H", self.transact(struct.pack("<BI", OP_READ16, addr)))[0]

    def read32(self, addr):
        return struct.unpack("<I", self.transact(struct.pack("<BI", OP_READ32, addr)))[0]

    def read64(self, addr):
        return struct.unpack("<Q", self.transact(struct.pack("<BI", OP_READ64, addr)))[0]

    def write8(self, addr, v):
        self.transact(struct.pack("<BIB", OP_WRITE8, addr, v & 0xFF))

    def write16(self, addr, v):
        self.transact(struct.pack("<BIH", OP_WRITE16, addr, v & 0xFFFF))

    def write32(self, addr, v):
        self.transact(struct.pack("<BII", OP_WRITE32, addr, v & 0xFFFFFFFF))

    def write64(self, addr, v):
        self.transact(struct.pack("<BIQ", OP_WRITE64, addr, v & 0xFFFFFFFFFFFFFFFF))

    def read_words(self, addrs):
        """Batched Read32 of arbitrary addresses (one round trip)."""
        out = []
        addrs = list(addrs)
        for i in range(0, len(addrs), _MAX_BATCH_READS):
            part = addrs[i:i + _MAX_BATCH_READS]
            body = b"".join(struct.pack("<BI", OP_READ32, a) for a in part)
            data = self.transact(body)
            out.extend(struct.unpack("<%dI" % len(part), data))
        return out

    def read_block(self, addr, size):
        """Read `size` bytes starting at `addr` using batched Read64/Read8."""
        out = bytearray()
        end = addr + size
        a = addr
        # unaligned head
        while a < end and (a & 7):
            out.append(self.read8(a))
            a += 1
        while end - a >= 8:
            n = min((end - a) // 8, _MAX_BATCH_READS)
            body = b"".join(struct.pack("<BI", OP_READ64, a + 8 * i) for i in range(n))
            out += self.transact(body)
            a += 8 * n
        while a < end:
            out.append(self.read8(a))
            a += 1
        return bytes(out)

    def write_block(self, addr, data):
        body = bytearray()
        for i, b in enumerate(data):
            body += struct.pack("<BIB", OP_WRITE8, addr + i, b)
            if len(body) > 60000:
                self.transact(bytes(body))
                body = bytearray()
        if body:
            self.transact(bytes(body))

    # -- info / control ----------------------------------------------------
    def _string(self, op):
        data = self.transact(struct.pack("<B", op))
        n = struct.unpack_from("<I", data)[0]
        return data[4:4 + n].rstrip(b"\0").decode("utf-8", "replace")

    def version(self):
        return self._string(OP_VERSION)

    def title(self):
        return self._string(OP_TITLE)

    def game_id(self):
        return self._string(OP_ID)

    def uuid(self):
        return self._string(OP_UUID)

    def game_version(self):
        return self._string(OP_GAMEVERSION)

    def status(self):
        return struct.unpack("<I", self.transact(struct.pack("<B", OP_STATUS)))[0]

    def save_state(self, slot):
        self.transact(struct.pack("<BB", OP_SAVESTATE, slot))

    def load_state(self, slot):
        self.transact(struct.pack("<BB", OP_LOADSTATE, slot))
