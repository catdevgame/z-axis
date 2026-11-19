# pcsx2pad: scripted controller input for PCSX2

Drives pad input into PCSX2 2.8.2 running Aggressive Inline (USA, SLUS-20327,
CRC 67DB3ED8) with no OS input events: no Accessibility permission, no
keyboard focus, no osascript. A pnach hook makes the game merge a mailbox in
EE RAM into its own pad reads, and `pcsx2pad.py` writes that mailbox over
PINE. The same PINE connection gives memory peek/poke/dump, savestates and
frame-accurate waits, so a script can drive PCSX2 to a screen and then inspect
RAM for differential debugging against the native recompiled build.

## Files

| File | What |
|---|---|
| `pcsx2pad.py` | the CLI (input, scripts, memory, savestates, launch/quit, screenshots) |
| `pine.py` | minimal PINE client (Unix socket, batched requests) |
| `pcsx2pad_hook.s` | hook source (MIPS R5900) |
| `build_hook.py` | assembles the hook with `../decomp/tools/binutils` and writes the pnach |
| `pcsx2pad.pnach` | generated pnach (same content as the installed copy) |
| `scripts/boot-to-main-menu.txt` | command script: fresh boot to main menu, with savestates and screenshots |
| `scripts/boot-to-main-menu.padscript` | the same in `PS2X_PAD_SCRIPT` syntax (`7:START,11:START,15:START`) |
| `scripts/tutorial-*.txt`, `scripts/native/` | Tutorial reference run and its native pad scripts (`REFERENCE-RUN.md`) |
| `pcsx2-test-data/` | private copy of `baseline/pcsx2-data` (a link into the data folder, `tooling/data-links.txt`; never committed) |

The test data dir is a copy of the baseline with `PINESlot = 28012` (the baseline
uses 28011), the pnach in `PCSX2/patches/SLUS-20327_67DB3ED8.pnach`, and
`Slot1_Filename` set by `launch` (default `Mcd001.ps2`), `OutputMuted = true`.
Never point the tool at the baseline dir.

## How it works

### Where the game reads the pad

- `scePadRead` is at `0x00211A30` (libpad, from `ghidra/sce-db/sce-matches.csv`).
  It has exactly one caller: `jal` at `0x001188D0` in `func_00118540`, the
  game's per-port pad state machine. That function is called for each open
  pad by `func_00118ED0`, which loops over 4 pad records of 0x44 bytes at
  `0x00282380`.
- The destination buffer is `0x00282494`, 32 bytes, shared by all ports:
  `[0]` status, `[1]` id (0x79 = DualShock 2 in pressure mode, which is what
  the game sets up in PCSX2), `[2..3]` buttons (active-low), `[4..7]` RX RY
  LX LY, `[8..19]` pressure. The game then builds `(b2<<8|b3)^0xFFFF` into
  `0x00282490`, remaps it with `func_00118158`, and uses the pressure bytes
  (through table `0x00234300`) when the pad is in pressure mode.
- Edge detection (`0x00118F7C`) compares with the previous poll, so a tap
  must be visible as held for at least one poll and then released for at
  least one.

### The hook

- `patch=1,EE,001188D0,word,0C03E000` turns that call into `jal 0x000F8000`.
- The hook at `0x000F8000` (71 words) saves a0..a2 and ra, calls the real
  `scePadRead`, and returns its v0 unchanged. Only when the call was for port 0
  slot 0 and returned more than 0 bytes, it:
  - increments the poll counter. On the first poll it also records the game's
    vsync count.
  - records the real button word.
  - if the mailbox magic is `"PADM"`, clears the requested button bits, sets
    the pressure byte of each forced button to 0xFF, and, if flag bit0 is set,
    overwrites the four stick bytes.
- Real controller input (the copied ini still binds the user's SDL pad and
  keyboard) still works. Injection can only press buttons, never release a
  button that is physically held.
- All lines are `patch=1` (applied continuously), so the hook survives
  loading savestates made without it. PCSX2 only rewrites a word when it
  differs.
- An unlabelled patch in the patches folder makes PCSX2 disable the bundled
  `patches.zip` file for this game. That file holds a single labelled
  "[Widescreen 16:9]" group, so `build_hook.py` copies it into our pnach
  verbatim. It stays off because `EnableWideScreenPatches = false`, the same
  as in the baseline. The log line `Found 2 game patches in .../SLUS-20327_67DB3ED8.pnach`
  plus `72 game patches are active` (1 call-site word and 71 hook words) is
  the expected result.

### Mailbox at `0x000F7000`

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | u32 | magic `0x4D444150` ("PADM") arms injection. The tool writes it on first use |
| +0x04 | u16 | buttons to hold, **active-high** (bit set = pressed). Pad bit order: bit0 SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT, L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, bit15 SQUARE |
| +0x06 | u8 | flags: bit0 = override sticks |
| +0x08 | u8[4] | RX, RY, LX, LY (0x00 = left/up, 0x80 = centre, 0xFF = right/down) |
| +0x0C | u32 | poll counter (port-0 reads seen by the hook) |
| +0x10 | u16 | button word the game finally saw (active-low) |
| +0x12 | u16 | button word from the real pad (active-low) |
| +0x14 | u32 | game vsync count at the first poll |

The button field is active-high, not an active-low AND mask, so zeroed or
stale memory can never hold buttons down.

### Why `0x000F7000` / `0x000F8000` are safe

EE RAM `0x00096000-0x000FFFFF` sits between the BIOS EELOAD leftovers
(`0x81E10-0x950E0`) and the ELF, which loads at `0x00100000` (one segment up to
`0x002B3B70`, heap above that, stack at the top). In PINE dumps taken during
the boot movies, at the main menu, in Player Select and Level Select, and
while a level was loading and playing, this range is all zero apart from the
hook's own bytes.

### Frame counter

`0x00231CA4` is incremented by the game's VBlank handler (`func_00100268`,
`D_00231900+0x3A4`) at 59.94 Hz from early boot. `0x00231CA8` is the same
count latched at each frame flip. All waits count vsyncs from it, and a
press additionally waits for real pad polls (+0x0C), so input is never lost
if the game skips frames or is loading. The game polls the pad once per
vsync during movies and menus.

## Usage

Run from `tools/` (the defaults are slot 28012 and `tools/pcsx2-test-data`; override with
`--slot` or the `PCSX2PAD_SLOT` / `PCSX2PAD_DATA` env vars).

```sh
python3 build_hook.py                 # (re)generate + install the pnach (only after editing the .s)
python3 pcsx2pad.py launch            # start our own PCSX2, wait for game + hook
python3 pcsx2pad.py launch --card unformatted   # same, slot 1 = fresh erased card
python3 pcsx2pad.py run scripts/boot-to-main-menu.txt
python3 pcsx2pad.py status            # PCSX2 state, hook, mailbox, counters
python3 pcsx2pad.py quit              # stops only the PID we launched
```

Input commands. Buttons are `SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1
TRIANGLE CIRCLE CROSS SQUARE` (aliases X O TRI SQ SEL); combine them with `+`:

```sh
pcsx2pad.py press START               # tap: held 6 frames/2 polls, then released 6 frames/2 polls
pcsx2pad.py press UP+CROSS 10         # tap with a 10-frame hold
pcsx2pad.py hold CROSS 30             # hold 30 frames, then release
pcsx2pad.py down R1 ; pcsx2pad.py up R1   # latch/unlatch (stays held between commands)
pcsx2pad.py stick L 0 -1 120          # left stick full up for 120 frames, then centre
pcsx2pad.py stick R 0.5 0             # right stick half right until `release`
pcsx2pad.py release                   # clear all buttons and the stick override
```

Timing and conditions:

```sh
pcsx2pad.py wait 60                   # 60 vsyncs
pcsx2pad.py waitsec 2.5
pcsx2pad.py waitpolls 1               # until the game has polled the pad at least once
pcsx2pad.py waitmem 0x2732FC 8 4 600  # until a u32 equals 8 (timeout 600 frames)
pcsx2pad.py frame                     # print vsync + poll counters
```

Scripts:

```sh
# PS2X_PAD_SCRIPT syntax, as in runtime/src/lib/ps2_pad.cpp:
# "<sec>:<BUTTON>[:<dur sec, default 0.25>],..." (unknown buttons skipped)
pcsx2pad.py script "7:START,11:START,15:START"
pcsx2pad.py script @scripts/boot-to-main-menu.padscript
pcsx2pad.py --origin now script "0.5:CROSS"   # origin: firstpoll (default) | now | boot

# command files: one CLI command per line, '#' comments
pcsx2pad.py run scripts/boot-to-main-menu.txt
```

The native runtime measures script time in wall seconds from its first pad
read. Here `--origin firstpoll` (the default) uses the vsync count that the hook
recorded at the game's first `scePadRead` (+0x14), and converts seconds to
frames at 59.94 fps. So the same string means "N seconds of game time after
the pad first came up" in both builds, and `PS2X_PAD_SCRIPT="7:START,11:START,15:START"`
reaches the main menu in both. Because origin and timing are deterministic,
the script works even if you start it late, as long as the events haven't
passed yet.

Memory and state:

```sh
pcsx2pad.py peek 0x282494 32          # hex dump
pcsx2pad.py poke 0x0027A930 1 4       # write (size 1/2/4/8, default 4)
pcsx2pad.py dump 0 0x2000000 ram.bin  # full 32 MB EE RAM, about 0.6 s
pcsx2pad.py savestate 2 [copy.p2s]    # waits for the .p2s to land, optionally copies it
pcsx2pad.py loadstate 2               # waits for the load, then clears restored input
pcsx2pad.py screenshot shot.png       # screencapture of our PCSX2 window (no focus needed)
```

Savestates go to `pcsx2-test-data/PCSX2/sstates/SLUS-20327 (67DB3ED8).NN.p2s`.
From Python, `from pcsx2pad import Pad; pad = Pad(); pad.press(0x4000); pad.p.read_block(...)`.

### Game-state waits, steering and checkpoints

Added for the Tutorial reference run; details and the game addresses they use
are in `REFERENCE-RUN.md`.

```sh
pcsx2pad.py waitval 0x276FE4 u32 == 3 600       # typed compare: u8 u16 u32 s32 f32, == != < <= > >=
pcsx2pad.py pressuntil CROSS 0x276FE4 u32 == 0  # tap until the condition holds
pcsx2pad.py tips                                # dismiss tutorial message boxes until none opens
pcsx2pad.py goto -12.3 8.37 1.5 900 SHARP       # skate to (x, z) with the D-pad, closed loop
pcsx2pad.py checkpoint 33 ref04-talk-go         # savestate + atomic EE RAM + frame thumbnail
PCSX2PAD_RECORD=/tmp/run.log pcsx2pad.py run scripts/tutorial-all.txt
pcsx2pad.py padscript /tmp/run.log              # the recorded inputs as a PS2X_PAD_SCRIPT
```

## Verified

- From a fresh `launch`, `run scripts/boot-to-main-menu.txt` reaches the main
  menu by script alone (about 27 s wall), the same in every fresh boot: the
  three movies each skipped by a scripted START, the front end's card-check
  box, then MAIN MENU with CAREER selected.
- The native script string `7:START,11:START,15:START`
  (`script @scripts/boot-to-main-menu.padscript`) reaches the main menu.
- D-pad RIGHT/LEFT and the left stick move the menu cursor; CROSS opens
  Player Select and Level Select and starts the Tutorial level; in the level
  CROSS dismisses the tutorial text and `stick L 0 -1 150` skates forward.
- `down L1` shows `game sees fbff (L1 held)` in `status`.
- After `loadstate`, input is cleared and the game keeps running.

## Limitations

- **Memory card dialog**: with the baseline's formatted cards it never shows;
  the card-check box only appears empty for a moment. With
  `launch --card unformatted` (an all-0xFF card that PCSX2 reports as
  UNFORMATTED) it did not appear on the boot path or after selecting Career
  either. So it is triggered somewhere else (probably the first save/load),
  and no savestate of the real dialog was made. When it does appear, it
  takes UP/DOWN and CROSS (TRIANGLE cancels).
- **No "screen" detector**: there is no clean RAM variable for "main
  menu active". `0x002732FC` (8 at the menu) is a load counter, not a
  screen ID. The boot script therefore uses frame timing with generous
  margins. PCSX2 timing is deterministic, and START does nothing on the
  card-check box and the main menu, so extra taps are harmless.
- **Port 0 only**: injection affects port 0 slot 0. It ORs with the real pad
  (it cannot release a physically held button). Pressure for forced buttons is
  always 0xFF.
- **Savestates include the mailbox**: loading a state made while a button was
  injected would restore it. `loadstate` clears it afterwards. Raw PINE
  users should call `release`.
- **Timing assumes the game is running**: waits stop with an error if the vsync
  counter stops (paused or crashed). If the game makes no pad polls during a
  press, the tool warns after 600 extra frames and continues.
- `loadstate` waits a fixed 1 s and then 2 vsyncs, because PINE load is
  asynchronous and gives no completion event.
- Screenshots need the PCSX2 window on screen (not minimised). The capture
  retries if the window is being recreated.
- Wall-clock speed: everything is counted in frames, so a PCSX2 slowdown
  only costs time.
