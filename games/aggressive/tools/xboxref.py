#!/usr/bin/env python3
"""Xbox reference shots of the Zoo: the real Xbox release in xemu, for comparing
the port's converted Zoo against ground truth (as PCSX2 is for the PS2 levels).

    python3 -I tools/xboxref.py setup  [--host win|mac]   install/check xemu, config, data copies
    python3 -I tools/xboxref.py start  [--host ...] [--snapshot zoo-start]
    python3 -I tools/xboxref.py shots  [--host ...] [--views a,b] [--tag T] [--out DIR]
    python3 -I tools/xboxref.py view NAME | look FX FZ TX TZ | goto X Y Z YAW   [--host ...]
    python3 -I tools/xboxref.py shot NAME | pos | fps | views | status          [--host ...]
    python3 -I tools/xboxref.py keys "A,~500,Right*300" | savevm NAME | loadvm NAME | stop

Hosts. win (default): xemu 0.8.139 on the Windows worker (over ssh), full
speed (60 fps at every viewpoint), keyboard input (SendInput in the logged-on
session, scheduled task) and xemu's own F12 screenshot (the guest frame buffer,
640x480). mac: xemu on this Mac (27 to 47 fps by viewpoint), window capture only, no input
(it never sends keys to your desktop); it relies on the snapshot.

How. Both HDD copies hold the snapshot `zoo-start` (Free Skate, Zoo, skater at
the spawn point, every level unlocked), so `start` boots xemu and `loadvm`s it.
Viewpoints are set by writing the skater's position, velocity and heading
through xemu's GDB stub (physical memory; the skater object is at 0x897740 in
that snapshot, found again by signature if not), then the camera settles, the VM
is paused (QMP stop), the shot taken, the VM resumed. QMP and GDB listen on
127.0.0.1 only (Windows: reached through an ssh tunnel).

Coordinates are the level's (zoo.INS): y points down (ground about 0, statues
reach -14), heading 0 faces +z, 90 faces +x. The fountain (the pond) is at the
origin, the elephant-lined paths with their trellis arches (pergolas) and roller
ramps run along the axes, quarter pipes at z 122 and 187.

Menu path from a cold boot (what made the snapshot; keyboard on win: Return =
Start, A = A, B = B, X = X, arrows = d-pad; `keys` plays them): title, Return;
main menu (cursor on CAREER), Right (FREESKATE), A; skater, A; name entry, A
(cursor on END); level select, Right x7 (giraffe = ZOO), A; about 10 s of
loading and the intro camera. On a fresh save every Free Skate level but the
Movie Lot is locked. The game's level unlock code (OPTIONS = main menu Right x5,
A; padlock = Right x5, A; on-screen keyboard bottom row: d R R d L d L d R R R,
END) was entered but left them locked for the chosen skater; what unlocks them
is the Konami code's skater-record writes, done here over GDB (`unlock`, at the
main menu, then enter Free Skate again): each record at 0x1EB208 + n*0x510 gets
+1 = 10, +4 = 0xFFFFFFFF. Then `savevm NAME` keeps the state.

Files (never commit any of them, nor any shot): the user's own console files
(MCPX boot ROM, flash BIOS, EEPROM, HDD) are copied from
$GAME_DISCS/XEMU into the scratch data folders below and only
the copies are used. Shots go to /tmp/ai-xboxref/shots by default.
"""
import argparse
import json
import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env: the discs, the Xbox data, the Windows worker's folder)

XEMU_VERSION = "0.8.139"
USER_XEMU = os.path.join(zpaths.GAME_DISCS, "XEMU")
USER_FILES = {  # name in the data folder: path under USER_XEMU
    "mcpx_1.0.bin": "Boot ROM Image/mcpx_1.0.bin",
    "Complex_4627.bin": "BIOS/Complex_4627.bin",
    "eeprom.bin": "Modified EEPROM/eeprom.bin",
    "xbox_hdd.qcow2": "Xbox Hard Disk Image/xbox_hdd.qcow2",
}
MCPX_MD5 = "d49c52a4102f6df7bcf8d0617ac475ed"
OUT_DEFAULT = "/tmp/ai-xboxref/shots"

MAC_BASE = os.path.join(zpaths.XBOX_DATA, "xboxref")
MAC_APP = MAC_BASE + "/app/xemu.app/Contents/MacOS/xemu"
MAC_ISO = os.path.join(zpaths.GAME_DISCS, "aggressive inline", "Aggressive Inline-xbox.iso")
MAC_QMP, MAC_GDB = 24455, 21234

WIN_HOST = "desktop"
WIN_ROOT = zpaths.PATHS.get("WIN_ROOT") or r"C:\z-axis"   # the Windows host's own folder (tools, scratch, iso)
WIN_XEMU = WIN_ROOT + r"\tools\xemu" + "\\" + XEMU_VERSION
WIN_DATA = WIN_ROOT + r"\tools\xemu\data"
WIN_S = WIN_ROOT + r"\scratch\xboxref"
WIN_ISO = WIN_ROOT + r"\iso\Aggressive Inline (USA).xiso.iso"
WIN_QMP, WIN_GDB = 4455, 1234          # on the Windows host's 127.0.0.1
TUN_QMP, TUN_GDB = 14455, 11234        # local ends of the ssh tunnel

# Skater object (physical address in the zoo-start snapshot): position xyz at
# +0, velocity at +0x10, heading rows at +0x20/+0x30/+0x40 (+0x34 = -1.0), the
# level's spawn point at +0x50.
SKATER = 0x897740
ZOO_SPAWN = (-13.133045, -3.899451, 103.096779)
FRAME_COUNTER = 0x1D3324               # virtual; +1 per game frame
SKATER_RECORDS = (0x1EB208, 0x510, 0x1F1C59)

# name: (from x, from z, target x, target z, what it shows). None = the spawn point as loaded.
VIEWS = {
    "start": None,
    "pond-south": (0, -40, 0, 0, "the fountain pond from the entrance side, seals, elephant statues behind"),
    "pond-diagonal": (-32, -32, 0, 0, "the fountain pond from the south-west, pergola arches and city behind"),
    "pergolas-east": (95, 0, 61.5, -16.6, "trellis arches (pergolas) over the east elephant path, pond beyond"),
    "pergolas-west": (-30, 0, -63, 0, "west elephant path under the pergolas, roller ramp, elephant pair"),
    "ramps-north": (0, 30, 0, 110, "north roller ramp between elephant statues, pergolas, quarter pipe"),
    "ramps-quarterpipe": (0, 95, 0, 125, "quarter pipe at z 122 below the planter mound and signposts"),
    "ramps-roller": (-50, 0, 0, 0, "west roller (funbox) ramp seen along its length, pond statue behind"),
    "elephant": (45, -30, 61.6, -16.6, "elephant statue on its pedestal over a roller ramp"),
}


def say(*a):
    print("[xboxref]", *a, flush=True)


# ---------------------------------------------------------------- QMP and GDB

class QMP:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.f = self.s.makefile("rwb")
        json.loads(self.f.readline())
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg).encode() + b"\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "event" not in r:
                break
        if "error" in r:
            raise RuntimeError(f"QMP {name}: {r['error'].get('desc')}")
        return r.get("return")

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

    def close(self):
        self.s.close()


class GDB:
    """Minimal GDB remote protocol client for xemu's stub (memory only).
    Connecting halts the guest; close() detaches, which resumes it."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.buf = b""
        self.cmd("qSupported")

    def _send(self, p):
        d = p.encode()
        self.s.sendall(b"$" + d + b"#" + ("%02x" % (sum(d) & 0xFF)).encode())

    def _recv(self):
        while True:
            while b"#" not in self.buf or len(self.buf) < self.buf.index(b"#") + 3:
                chunk = self.s.recv(65536)
                if not chunk:
                    raise IOError("gdb stub closed")
                self.buf += chunk
            self.buf = self.buf.lstrip(b"+")
            if not self.buf.startswith(b"$"):
                i = self.buf.find(b"$")
                self.buf = self.buf[i:] if i >= 0 else b""
                continue
            j = self.buf.index(b"#")
            pkt, self.buf = self.buf[1:j], self.buf[j + 3:]
            self.s.sendall(b"+")
            return pkt.decode(errors="replace")

    def cmd(self, p):
        self._send(p)
        return self._recv()

    def phys(self, on):
        self.cmd("Qqemu.PhyMemMode:%d" % (1 if on else 0))

    def read(self, addr, n):
        out = b""
        while n:
            k = min(n, 0x800)
            r = self.cmd("m%x,%x" % (addr, k))
            if r.startswith("E"):
                raise IOError("read %x: %s" % (addr, r))
            out += bytes.fromhex(r)
            addr += k
            n -= k
        return out

    def write(self, addr, data):
        r = self.cmd("M%x,%x:%s" % (addr, len(data), data.hex()))
        if r != "OK":
            raise IOError("write %x: %s" % (addr, r))

    def close(self):
        try:
            self.cmd("D")
        except Exception:
            pass
        self.s.close()


# ---------------------------------------------------------------- PNG (stdlib)

def png_read(path):
    """8-bit RGB/RGBA non-interlaced PNG -> (w, h, channels, rows of bytes)."""
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    i, idat, w = 8, b"", 0
    while i < len(d):
        n, t = struct.unpack(">I4s", d[i:i + 8])
        body = d[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ctype in (2, 6) and inter == 0, "unsupported PNG"
            ch = 4 if ctype == 6 else 3
        elif t == b"IDAT":
            idat += body
        i += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * ch, [], bytearray(w * ch)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b, c = prev[x], (prev[x - ch] if x >= ch else 0)
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, ch, rows


def png_write(path, w, h, rows_rgb):
    raw = b"".join(b"\x00" + bytes(r) for r in rows_rgb)
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def crop_mac_window(src, dst):
    """screencapture -l keeps the title bar: keep the bottom 4:3 area, halve Retina."""
    w, h, ch, rows = png_read(src)
    ch_h = w * 3 // 4
    rows = rows[h - ch_h:] if ch_h < h else rows
    step = 2 if w >= 2560 else 1
    out = []
    for y in range(0, len(rows), step):
        r = rows[y]
        out.append(bytes(b for x in range(0, w, step) for b in r[x * ch:x * ch + 3]))
    png_write(dst, w // step, len(out), out)
    return w // step, len(out)


# ---------------------------------------------------------------- Windows helpers (deployed by setup)

PS = {}
PS["xr-it.ps1"] = r'''# xr-it.ps1 -Script <ps1> [-ArgLine "..."] [-Wait] [-Name n]: runs a script in the
# logged-on user's interactive session (scheduled task with an Interactive principal),
# where windows, SendInput and screen copies work. Output: run\it-<name>.out.
param([string]$Script, [string]$ArgLine = "", [switch]$Wait, [string]$Name = "it")
$S = "__ROOT__\scratch\xboxref"
$out = "$S\run\it-$Name.out"
New-Item -ItemType Directory -Force "$S\run" | Out-Null
Remove-Item $out -ErrorAction SilentlyContinue
$user = (Get-Process explorer -IncludeUserName | Select -First 1).UserName
$task = "xboxref-$Name"
$cmd = "& '$Script' $ArgLine *> '$out'"
$enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -EncodedCommand $enc"
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 4) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep -Milliseconds 500
if ($Wait) { while ((Get-ScheduledTask -TaskName $task).State -eq "Running") { Start-Sleep -Milliseconds 200 } }
Unregister-ScheduledTask -TaskName $task -Confirm:$false
if ($Wait -and (Test-Path $out)) { Get-Content $out }
'''
PS["xr-win.ps1"] = r'''# xr-win.ps1: window, keyboard and capture helpers (dot-sourced).
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class XR {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr x);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int sz);
  public static List<IntPtr> TopWindows(uint pid) {
    var r = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) r.Add(h); return true; }, IntPtr.Zero);
    return r; }
  public static uint Key(ushort vk, bool up) {
    var i = new INPUT[1]; i[0].type = 1; i[0].ki.wVk = vk; i[0].ki.wScan = (ushort)MapVirtualKey(vk, 0);
    uint f = 0; if (vk >= 0x21 && vk <= 0x2E) f |= 1; if (up) f |= 2; i[0].ki.dwFlags = f;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))); }
}
"@
function Get-XRWindow([int]$ProcId) {
  $best = $null; $area = 0
  foreach ($h in [XR]::TopWindows([uint32]$ProcId)) { $r = New-Object XR+RECT; [XR]::GetWindowRect($h, [ref]$r) | Out-Null; $a = ($r.R-$r.L)*($r.B-$r.T); if ($a -gt $area) { $area = $a; $best = $h } }
  $best }
function Focus-XR($h) {
  if ([XR]::GetForegroundWindow() -eq $h) { return $true }
  [XR]::ShowWindow($h, 9) | Out-Null
  [XR]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [XR]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
  [XR]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 200
  return ([XR]::GetForegroundWindow() -eq $h) }
'''
PS["xr-keys.ps1"] = r'''# xr-keys.ps1 -Seq "<tokens>": focuses xemu and plays keys (SendInput). Tokens, comma
# separated: KEY = tap (150 ms), KEY*MS = hold, K1+K2*MS = chord, ~MS = wait. KEY is a
# .NET Keys name (Return, Up, Left, A, B, X, F12, Back, ...).
param([string]$Seq, [string]$PidFile = "__ROOT__\scratch\xboxref\run\xemu.pid")
. __ROOT__\scratch\xboxref\xr-win.ps1
Add-Type -AssemblyName System.Windows.Forms
$h = Get-XRWindow ([int](Get-Content $PidFile))
if (-not $h) { "no window"; exit 1 }
if (-not (Focus-XR $h)) { "could not focus"; exit 1 }
foreach ($t in ($Seq -split "[\s,]+" | ? { $_ })) {
  if ($t.StartsWith("~")) { Start-Sleep -Milliseconds ([int]$t.Substring(1)); continue }
  $ms = 150; $k = $t
  if ($t -match "^(.+)\*(\d+)$") { $k = $matches[1]; $ms = [int]$matches[2] }
  $vks = $k -split "\+" | % { [int][System.Windows.Forms.Keys]::Parse([System.Windows.Forms.Keys], $_, $true) }
  if ([XR]::GetForegroundWindow() -ne $h) { Focus-XR $h | Out-Null }
  foreach ($v in $vks) { [XR]::Key([uint16]$v, $false) | Out-Null }
  Start-Sleep -Milliseconds $ms
  foreach ($v in $vks) { [XR]::Key([uint16]$v, $true) | Out-Null }
  Start-Sleep -Milliseconds 60
}
"ok"
'''
PS["xr-screen.ps1"] = r'''# xr-screen.ps1 -Out <png>: brings xemu to the front, copies its client area from the screen.
param([string]$Out, [string]$PidFile = "__ROOT__\scratch\xboxref\run\xemu.pid")
. __ROOT__\scratch\xboxref\xr-win.ps1
$h = Get-XRWindow ([int](Get-Content $PidFile))
if (-not $h) { "no window"; exit 1 }
Focus-XR $h | Out-Null; Start-Sleep -Milliseconds 200
$cr = New-Object XR+RECT; [XR]::GetClientRect($h, [ref]$cr) | Out-Null
$o = New-Object XR+POINT; [XR]::ClientToScreen($h, [ref]$o) | Out-Null
$w = $cr.R - $cr.L; $hh = $cr.B - $cr.T
$b = New-Object System.Drawing.Bitmap $w, $hh
[System.Drawing.Graphics]::FromImage($b).CopyFromScreen($o.X, $o.Y, 0, 0, (New-Object System.Drawing.Size $w, $hh))
$b.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out"
'''
PS["xr-xemu.ps1"] = r'''# xr-xemu.ps1: starts xemu (portable: xemu.toml next to xemu.exe) with QMP and the GDB
# stub on 127.0.0.1; run in the interactive session. Pid in run\xemu.pid.
param([string]$Exe = "__ROOT__\tools\xemu\__VER__\xemu.exe")
$S = "__ROOT__\scratch\xboxref"
New-Item -ItemType Directory -Force "$S\run", "$S\shots" | Out-Null
$a = "-qmp tcp:127.0.0.1:__QMP__,server=on,wait=off -gdb tcp:127.0.0.1:__GDB__"
$p = Start-Process -FilePath $Exe -ArgumentList $a -WorkingDirectory (Split-Path $Exe) -RedirectStandardError "$S\run\xemu.log" -RedirectStandardOutput "$S\run\xemu.out" -PassThru
"$($p.Id)" | Set-Content -NoNewline "$S\run\xemu.pid"
"started $($p.Id)"
'''.replace("__VER__", XEMU_VERSION).replace("__QMP__", str(WIN_QMP)).replace("__GDB__", str(WIN_GDB))
PS = {name: text.replace("__ROOT__", WIN_ROOT) for name, text in PS.items()}   # (the host's folder: WIN_ROOT)


def xemu_toml(files_dir, iso, shots_dir, mac):
    q = (lambda p: "'" + p + "'")
    sep = "/" if mac else "\\"
    return f"""# xemu.toml for the Aggressive Inline Xbox reference runs (written by tools/xboxref.py).
# sys.files are copies of the user's own console files: never commit them.
[general]
show_welcome = false
skip_boot_anim = true
screenshot_dir = {q(shots_dir)}

[general.updates]
check = false

[input]
auto_bind = true

[input.bindings]
port1_driver = 'usb-xbox-gamepad'
port1 = 'keyboard'

[display]
renderer = 'OPENGL'
{"" if mac else "setup_nvidia_profile = false"}

[display.quality]
surface_scale = 1

[display.window]
startup_size = '1280x960'
vsync = true

[display.ui]
show_notifications = false
show_menubar = false

[audio]
volume_limit = 0.0

[perf]
cache_shaders = {"false" if mac else "true"}

[sys]
mem_limit = '64'
avpack = 'hdtv'

[sys.files]
bootrom_path = {q(files_dir + sep + "mcpx_1.0.bin")}
flashrom_path = {q(files_dir + sep + "Complex_4627.bin")}
eeprom_path = {q(files_dir + sep + "eeprom.bin")}
hdd_path = {q(files_dir + sep + "xbox_hdd.qcow2")}
dvd_path = {q(iso)}
"""


# ---------------------------------------------------------------- hosts

class Host:
    qmp_port = gdb_port = 0

    def qmp(self):
        return QMP(self.qmp_port)

    def gdb(self):
        return GDB(self.gdb_port)

    def alive(self):
        try:
            self.qmp().close()
            return True
        except OSError:
            return False

    def close(self):
        pass

    def start(self, snapshot):
        if not self.alive():
            self.launch()
            for _ in range(120):
                if self.alive():
                    break
                time.sleep(0.5)
            else:
                raise SystemExit("xemu did not open QMP; see its log")
            time.sleep(3)
        if snapshot:
            q = self.qmp()
            if snapshot not in q.hmp("info snapshots"):
                raise SystemExit(f"no snapshot {snapshot} in the HDD copy (see the docstring's menu path)")
            r = q.hmp(f"loadvm {snapshot}")
            if r.strip():
                say("loadvm:", r.strip())
            q.cmd("cont")
            q.close()
            time.sleep(2)
            say("loaded", snapshot)

    def stop(self):
        if self.alive():
            try:
                self.qmp().cmd("quit")
            except (OSError, ValueError):
                pass
        time.sleep(2)
        self.kill_if_running()


class Mac(Host):
    name = "mac"
    qmp_port, gdb_port = MAC_QMP, MAC_GDB
    pidfile = MAC_BASE + "/run/xemu.pid"

    def setup(self):
        for d in ("app", "data", "run", "home", "shots", "bin", "download"):
            os.makedirs(f"{MAC_BASE}/{d}", exist_ok=True)
        if not os.path.exists(MAC_APP):
            z = f"{MAC_BASE}/download/xemu-{XEMU_VERSION}-macos-universal.zip"
            url = f"https://github.com/xemu-project/xemu/releases/download/v{XEMU_VERSION}/xemu-{XEMU_VERSION}-macos-universal.zip"
            say("downloading", url)
            subprocess.run(["curl", "-sSL", "-o", z, url], check=True)
            subprocess.run(["ditto", "-x", "-k", z, MAC_BASE + "/app"], check=True)
        copy_user_files(MAC_BASE + "/data")
        open(MAC_BASE + "/xemu.toml", "w").write(xemu_toml(MAC_BASE + "/data", MAC_ISO, MAC_BASE + "/shots", True))
        self.winid_tool()
        say("mac ready:", MAC_APP, "config", MAC_BASE + "/xemu.toml")

    def winid_tool(self):
        exe = MAC_BASE + "/bin/winid"
        if not os.path.exists(exe):
            src = MAC_BASE + "/bin/winid.swift"
            open(src, "w").write(
                "import CoreGraphics\nlet pid = Int(CommandLine.arguments[1]) ?? 0\n"
                "let l = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as! [[String: Any]]\n"
                "var best = 0, area = 0.0\n"
                "for w in l where (w[kCGWindowOwnerPID as String] as? Int) == pid && (w[kCGWindowLayer as String] as? Int) == 0 {\n"
                "  let b = w[kCGWindowBounds as String] as? [String: Double] ?? [:]\n"
                "  let a = (b[\"Width\"] ?? 0) * (b[\"Height\"] ?? 0)\n"
                "  if a > area { area = a; best = w[kCGWindowNumber as String] as? Int ?? 0 } }\nprint(best)\n")
            subprocess.run(["swiftc", "-O", "-o", exe, src], check=True)
        return exe

    def launch(self):
        if not os.path.exists(MAC_BASE + "/xemu.toml"):
            self.setup()
        env = dict(os.environ, HOME=MAC_BASE + "/home")
        log = open(MAC_BASE + "/run/xemu.log", "wb")
        p = subprocess.Popen([MAC_APP, "-config_path", MAC_BASE + "/xemu.toml",
                              "-qmp", f"tcp:127.0.0.1:{MAC_QMP},server=on,wait=off", "-gdb", f"tcp:127.0.0.1:{MAC_GDB}"],
                             cwd=MAC_BASE + "/run", env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        open(self.pidfile, "w").write(str(p.pid))
        say("started xemu pid", p.pid)

    def pid(self):
        try:
            return int(open(self.pidfile).read())
        except (OSError, ValueError):
            return 0

    def kill_if_running(self):
        pid = self.pid()
        if pid:
            try:
                os.kill(pid, 0)
                os.kill(pid, 15)
                say("terminated pid", pid)
            except OSError:
                pass

    def capture(self, dst):
        wid = subprocess.run([self.winid_tool(), str(self.pid())], capture_output=True, text=True).stdout.strip()
        if not wid or wid == "0":
            raise SystemExit("no xemu window on screen")
        tmp = dst + ".raw.png"
        subprocess.run(["screencapture", "-x", "-o", "-l" + wid, tmp], check=True)
        w, h = crop_mac_window(tmp, dst)
        os.remove(tmp)
        return f"{w}x{h} window capture"

    def keys(self, seq):
        raise SystemExit("mac: no input (it would type into your desktop); use --host win, or a snapshot")

    def pmemsave(self, n):
        path = MAC_BASE + "/run/ram.bin"
        q = self.qmp()
        q.cmd("pmemsave", val=0, size=n, filename=path)
        q.close()
        return open(path, "rb").read()


class Win(Host):
    name = "win"
    qmp_port, gdb_port = TUN_QMP, TUN_GDB

    def __init__(self):
        self.tunnel = None

    def ssh(self, ps, check=True):
        r = subprocess.run(["ssh", WIN_HOST, ps], capture_output=True, text=True)
        if check and r.returncode:
            raise SystemExit(f"ssh failed: {r.stderr.strip()[:500]}")
        return r.stdout

    def it(self, script, args="", name="it"):
        """Runs a deployed helper in the interactive session and returns its output."""
        arg = f" -ArgLine '{args}'" if args else ""
        return self.ssh(f"powershell -NoProfile -ExecutionPolicy Bypass -File {WIN_S}\\xr-it.ps1 "
                        f"-Script {WIN_S}\\{script}{arg} -Wait -Name {name}")

    def open_tunnel(self):
        if self.tunnel:
            return
        self.tunnel = subprocess.Popen(["ssh", "-N", "-o", "ExitOnForwardFailure=yes",
                                        "-L", f"{TUN_QMP}:127.0.0.1:{WIN_QMP}", "-L", f"{TUN_GDB}:127.0.0.1:{WIN_GDB}", WIN_HOST])
        time.sleep(1.5)

    def qmp(self):
        self.open_tunnel()
        return QMP(TUN_QMP)

    def gdb(self):
        self.open_tunnel()
        return GDB(TUN_GDB)

    def alive(self):
        out = self.ssh(f"$p = Get-Content {WIN_S}\\run\\xemu.pid -ErrorAction SilentlyContinue; "
                       "if ($p -and (Get-Process -Id $p -ErrorAction SilentlyContinue)) { 'up' } else { 'down' }", check=False)
        return out.strip() == "up" and Host.alive(self)

    def close(self):
        if self.tunnel:
            self.tunnel.terminate()
            self.tunnel.wait()
            self.tunnel = None

    def setup(self):
        tmp = tempfile.mkdtemp(prefix="xboxref-")
        for name, body in PS.items():
            open(os.path.join(tmp, name), "w", newline="\r\n").write(body)
        open(os.path.join(tmp, "xemu.toml"), "w").write(
            xemu_toml(WIN_DATA, WIN_ISO, WIN_S + "\\shots", False))
        self.ssh(f"New-Item -ItemType Directory -Force {WIN_S}\\run, {WIN_S}\\shots, {WIN_DATA}, {WIN_XEMU} | Out-Null")
        subprocess.run(["scp", "-q"] + [os.path.join(tmp, n) for n in PS] + [f"{WIN_HOST}:{WIN_S.replace(chr(92), '/')}/"], check=True)
        exe = self.ssh(f"Test-Path {WIN_XEMU}\\xemu.exe").strip()
        if exe != "True":
            z = f"xemu-{XEMU_VERSION}-windows-x86_64.zip"
            self.ssh(f"New-Item -ItemType Directory -Force {WIN_ROOT}\\tools\\xemu\\_download | Out-Null; "
                     f"curl.exe -sSL -o {WIN_ROOT}\\tools\\xemu\\_download\\{z} "
                     f"https://github.com/xemu-project/xemu/releases/download/v{XEMU_VERSION}/{z}; "
                     f"Expand-Archive -Force {WIN_ROOT}\\tools\\xemu\\_download\\{z} {WIN_XEMU}")
        subprocess.run(["scp", "-q", os.path.join(tmp, "xemu.toml"), f"{WIN_HOST}:{WIN_XEMU.replace(chr(92), '/')}/xemu.toml"], check=True)
        have = self.ssh(f"Get-ChildItem {WIN_DATA} | % Name").split()
        missing = [n for n in USER_FILES if n not in have]
        if missing:
            local = MAC_BASE + "/data"
            copy_user_files(local)
            subprocess.run(["scp", "-q"] + [f"{local}/{n}" for n in missing] + [f"{WIN_HOST}:{WIN_DATA.replace(chr(92), '/')}/"], check=True)
        shutil.rmtree(tmp)
        say("win ready:", WIN_XEMU + "\\xemu.exe", "helpers in", WIN_S)

    def launch(self):
        say(self.it("xr-xemu.ps1", name="launch").strip())

    def kill_if_running(self):
        self.ssh(f"$p = Get-Content {WIN_S}\\run\\xemu.pid -ErrorAction SilentlyContinue; "
                 "if ($p -and (Get-Process -Id $p -ErrorAction SilentlyContinue)) { Stop-Process -Id $p -Force; 'killed ' + $p }", check=False)

    def keys(self, seq):
        return self.it("xr-keys.ps1", f"-Seq {seq}", name="keys").strip()

    def capture(self, dst):
        """xemu's F12 screenshot (guest frame buffer); screen copy if it does not appear."""
        n0 = self.ssh(f"(Get-ChildItem {WIN_S}\\shots -Filter xemu-*.png).Count").strip()
        for _ in range(3):
            self.keys("F12*250,~500")
            out = self.ssh(f"$f = Get-ChildItem {WIN_S}\\shots -Filter xemu-*.png | sort LastWriteTime | select -Last 1; "
                           f"if ((Get-ChildItem {WIN_S}\\shots -Filter xemu-*.png).Count -gt {n0 or 0}) {{ "
                           f"Move-Item -Force $f.FullName {WIN_S}\\shots\\last.png; 'ok' }}").strip()
            if out == "ok":
                how = "F12 frame buffer"
                break
        else:
            self.it("xr-screen.ps1", f"-Out {WIN_S}\\shots\\last.png", name="screen")
            how = "screen copy"
        subprocess.run(["scp", "-q", f"{WIN_HOST}:{WIN_S.replace(chr(92), '/')}/shots/last.png", dst], check=True)
        self.ssh(f"Remove-Item {WIN_S}\\shots\\last.png -ErrorAction SilentlyContinue", check=False)
        return how

    def pmemsave(self, n):
        path = WIN_S + "\\run\\ram.bin"
        q = self.qmp()
        q.cmd("pmemsave", val=0, size=n, filename=path.replace("\\", "/"))
        q.close()
        local = tempfile.mktemp(suffix=".bin")
        subprocess.run(["scp", "-q", f"{WIN_HOST}:{path.replace(chr(92), '/')}", local], check=True)
        d = open(local, "rb").read()
        os.remove(local)
        return d


def copy_user_files(dst):
    """Copies the user's console files into a scratch data folder (originals untouched)."""
    os.makedirs(dst, exist_ok=True)
    for name, rel in USER_FILES.items():
        target = os.path.join(dst, name)
        if os.path.exists(target):
            continue
        src = os.path.join(USER_XEMU, rel)
        if not os.path.exists(src):
            raise SystemExit(f"missing {src}: the MCPX boot ROM, flash BIOS, EEPROM and HDD come from your own console")
        shutil.copyfile(src, target)
        os.chmod(target, 0o644)
        say("copied", name)


# ---------------------------------------------------------------- game

def is_skater(b):
    """Skater object: heading row 2 is (0, -1, 0), the level's spawn point at +0x50."""
    return struct.unpack_from("<f", b, 0x34)[0] == -1.0 and \
        all(abs(u - v) < 1e-3 for u, v in zip(struct.unpack_from("<3f", b, 0x50), ZOO_SPAWN))


def find_skater(host, g):
    g.phys(True)
    b = g.read(SKATER, 0x60)
    if is_skater(b):
        return SKATER
    g.phys(False)
    g.close()
    say("skater not at", hex(SKATER), "- scanning RAM for the spawn point")
    q = host.qmp()
    q.cmd("stop")
    q.close()
    try:
        ram = host.pmemsave(48 << 20)
    finally:
        q = host.qmp()
        q.cmd("cont")
        q.close()
    spawn = struct.pack("<3f", *ZOO_SPAWN)
    i = 0
    while True:
        i = ram.find(spawn, i)
        if i < 0:
            raise SystemExit("skater not found (is a Zoo free skate running?)")
        if i >= 0x50 and is_skater(ram[i - 0x50:i + 0x10]):
            say("skater at", hex(i - 0x50))
            return i - 0x50
        i += 1


def place(host, x, y, z, yaw):
    g = host.gdb()
    base = find_skater(host, g)
    if g.s.fileno() < 0:
        g = host.gdb()
    g.phys(True)
    t = math.radians(yaw)
    s, c = math.sin(t), math.cos(t)
    g.write(base, struct.pack("<3f", x, y, z))
    g.write(base + 0x10, struct.pack("<3f", 0, 0, 0))
    g.write(base + 0x20, struct.pack("<3f", c, 0, -s))
    g.write(base + 0x30, struct.pack("<3f", 0, -1, 0))
    g.write(base + 0x40, struct.pack("<3f", s, 0, c))
    g.phys(False)
    g.close()


def look(host, fx, fz, tx, tz, y=-0.5):
    place(host, fx, y, fz, math.degrees(math.atan2(tx - fx, tz - fz)))


def position(host):
    g = host.gdb()
    base = find_skater(host, g)
    if g.s.fileno() < 0:
        g = host.gdb()
    g.phys(True)
    v = struct.unpack("<3f", g.read(base, 12))
    g.phys(False)
    g.close()
    return v


def shoot(host, path, pause=True):
    q = host.qmp()
    if pause:
        q.cmd("stop")
    try:
        how = host.capture(path)
    finally:
        if pause:
            q.cmd("cont")
        q.close()
    return how


def unlock(host):
    """The Konami code's writes: every skater record gets all levels (the Zoo included)."""
    g = host.gdb()
    start, step, end = SKATER_RECORDS
    a, n = start, 0
    while a + 1 < end:
        g.write(a + 1, bytes([10]))
        g.write(a + 4, b"\xff\xff\xff\xff")
        a += step
        n += 1
    g.close()
    say("unlocked levels for", n, "skater records")


def fps(host, seconds):
    def rd():
        g = host.gdb()
        v = struct.unpack("<I", g.read(FRAME_COUNTER, 4))[0]
        g.close()
        return time.time(), v
    t0, a = rd()
    time.sleep(seconds)
    t1, b = rd()
    return (b - a) / (t1 - t0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--host", choices=("win", "mac"), default="win")
    ap.add_argument("--snapshot", default="zoo-start")
    ap.add_argument("--views", default="all")
    ap.add_argument("--tag", default="")
    ap.add_argument("--out", default=OUT_DEFAULT)
    ap.add_argument("--settle", type=float, default=2.2, help="seconds for the camera to settle after a move")
    ap.add_argument("--keep", action="store_true", help="leave xemu running after shots")
    a = ap.parse_args()
    host = Win() if a.host == "win" else Mac()
    os.makedirs(a.out, exist_ok=True)
    c = a.command
    try:
        if c == "setup":
            host.setup()
        elif c == "views":
            for k, v in VIEWS.items():
                print(f"{k:18s} " + ("spawn point as loaded" if v is None else f"from ({v[0]}, {v[1]}) to ({v[2]}, {v[3]}): {v[4]}"))
        elif c == "status":
            say(host.name, "xemu running" if host.alive() else "xemu not running")
            if host.alive():
                say("snapshots:", host.qmp().hmp("info snapshots").strip())
        elif c == "start":
            host.start(a.snapshot)
        elif c == "stop":
            host.stop()
        elif c == "loadvm":
            say(host.qmp().hmp("loadvm " + a.args[0]) or "loaded")
        elif c == "savevm":
            say(host.qmp().hmp("savevm " + a.args[0]) or "saved")
        elif c == "keys":
            say(host.keys(a.args[0]))
        elif c == "unlock":
            unlock(host)
        elif c == "pos":
            say("skater at x %.2f y %.2f z %.2f" % position(host))
        elif c == "fps":
            say("game frames per second: %.1f" % fps(host, float(a.args[0]) if a.args else 5))
        elif c == "goto":
            place(host, *map(float, a.args[:4]))
        elif c == "look":
            look(host, *map(float, a.args[:4]))
        elif c == "view":
            v = VIEWS[a.args[0]]
            if v is None:
                host.start(a.snapshot)
            else:
                look(host, *v[:4])
        elif c == "shot":
            path = os.path.join(a.out, (a.args[0] if a.args else "shot") + ".png")
            say(path, shoot(host, path))
        elif c == "shots":
            names = list(VIEWS) if a.views == "all" else a.views.split(",")
            was_up = host.alive()
            host.start(a.snapshot)
            say("game fps: %.1f" % fps(host, 3))
            for n in names:
                v = VIEWS[n]
                if v is None:
                    host.start(a.snapshot)
                else:
                    look(host, *v[:4])
                time.sleep(a.settle if v is not None else 1.0)
                path = os.path.join(a.out, f"xbox-zoo-{n}{'-' + a.tag if a.tag else ''}-{host.name}.png")
                say(n, "->", path, f"({shoot(host, path)})")
            host.start(a.snapshot)
            if not was_up and not a.keep:
                host.stop()
        else:
            raise SystemExit(f"unknown command {c}")
    finally:
        host.close()


if __name__ == "__main__":
    main()
