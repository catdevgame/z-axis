# Aggressive Inline mods for PCSX2 (and real PS2 cheat devices)

`SLUS-20327_67DB3ED8.pnach` brings what we learned building the native PC
port of Aggressive Inline (USA, SLUS-20327) to the original game: an
improved widescreen patch (HUD at the screen edges, 2D kept in shape, 16:10
and 21:9), camera and assist options, the game's own cheats as switches,
Unlock Everything, the hidden developer debug menu, Quick Restart and a Hide
HUD toggle. Every mod is a patch of the game's EE memory (data writes and
small MIPS hooks), so it runs on the unmodified disc in PCSX2. The same
codes are in `raw-codes.txt` for cheat devices on a real PS2 (not tested
there, see below).

Credits: the 3D part of the widescreen sections is BloodRaynare's patch
(based on ElHecht's PAL patch), as bundled with PCSX2. Everything else:
Aggressive Inline PC port.

## Install in PCSX2

PCSX2 2.x (tested with 2.8.2 on macOS):

1. Copy `SLUS-20327_67DB3ED8.pnach` into PCSX2's `patches` folder (in PCSX2:
   Tools > Open Data Directory, then `patches`; or the folder set under
   Settings > Folders).
2. Right-click the game in the game list > Properties > Patches, and tick the
   sections you want. They apply at the next boot of the game (Quick Restart,
   the debug menu and Hide HUD use button combos once on).
3. Widescreen: tick one of our Widescreen sections and leave PCSX2's built-in
   "Widescreen 16:9" patch (listed in the same tab) and the global
   "Enable Widescreen Patches" setting off. Ours includes the same 3D patch.
   16:9 sets PCSX2's aspect ratio by itself; for 16:10 and 21:9 set
   Settings > Graphics > Aspect Ratio to Stretch and play in a window or
   full screen of that shape (PCSX2 has no 16:10 or 21:9 setting).

The file only has labelled sections, so nothing is on until you tick it.

## The mods

Status: **tested** = turned on in PCSX2 2.8.2 and its effect seen in the game
(screenshots, or the game's state read over PINE); **partly tested** = the
patched values were read back from the running game's memory, but the
effect in play was not tried. Nothing was tested on a real PS2.

| Section | What it does | Status |
|---|---|---|
| Widescreen 16:9 (HUD fix) | 16:9 3D (BloodRaynare's patch), HUD (juice, score, timer, trick bonus, game-over score) moved to the screen edges, pause menus, tip boxes and other in-level 2D squeezed back to their 4:3 shape; the front end and the movies show as 4:3 between bars instead of stretched | tested: Tutorial, Movie Lot (Career, Freeskate, Timed Run), split screen, pause, game over, tips, debug menu, front end, intro movie |
| Widescreen 16:10 (HUD fix) | the same for 16:10 | tested (PCSX2 Stretch, captured and resampled to 16:10) |
| Widescreen 21:9 (HUD fix) | the same for 21:9 (2.33:1) | tested (PCSX2 Stretch, resampled to 21:9) |
| Camera Distance 150% / 200% | the follow camera further back (also in grinds and on poles) | tested |
| Camera Height 150% / 200% | the follow camera higher | tested |
| Field of View 90 / 105 | wider view than the game's 75 (split screen FOVs scaled too); from the next level start | tested |
| Motion Blur Off / Low | no or half-strength motion trail; from the next level start | tested (the blur's switch and alpha read in memory: 65 original, 32 Low, off) |
| Split Screen Draw Distance Far / Farthest | split screen draws 2.5x as far (250, Museum 375) or as far as single player (1000), fog moved out with it; single player is already at the game's maximum | tested (Movie Lot split screen: far clip 100 / 250 / 1000, the warehouse background drawn instead of black) |
| Quick Restart (L3+R3) | click both sticks in during play: the level loads again from the start (Career progress is committed as when you exit the level); in a Timed Run the game's own Play Again (new 2:00 timer). Not in split screen | tested: Tutorial (Career), Freeskate, Timed Run |
| Developer Debug Menu (L3+SELECT) | hold L3, press SELECT during play: the developers' hidden menu (Resume, Art Viewer free camera, Tuning editor for the game's 405 tuning values, Respawn, Toggle Debug info). Details in `docs/DEVTOOLS.md` | tested (menu, tuning editor, back to play) |
| Hide HUD Toggle (R3+SELECT) | hold R3, press SELECT during play: hide or show the HUD, for screenshots and videos | tested (single player and split screen) |
| Keep Skating When Out of Juice | an empty juice meter no longer ends a Career run | tested (default: "Game Over, you ran out of Juice"; with it: still skating 20 s later) |
| Juice Drain 50% | the meter empties half as fast | tested |
| No Juice Drain | the meter only goes down when you crash | tested |
| Juice Lost on Crash 50% | crashes take half as much juice | partly tested |
| Cheat: Juice Regeneration | the game's cheat: the meter fills up by itself | tested |
| Cheat: Low Gravity Wall Ride, Invulnerability, Perfect Grind, Perfect Handplant, Perfect Manual, Super Spin | the game's own cheats, always on, with the values the game's cheat codes write (`inlineMenuCheat` 0x001AA628), without the cheat bits the codes leave in the career data | partly tested |
| Unlock Everything | all levels, skaters, park editor pieces and the Media Viewer cutscenes (the game's two unlock codes plus the cutscene bits). It writes the career data, so saving keeps the unlocks after you turn the patch off | tested (a fresh career: Civic Center and every level icon open) |

Combos are read from player 1's pad during play only (not in menus or
paused), on the frame the second button goes down. The game uses neither
L3 nor R3 in play, and the combos take SELECT out of that frame's presses,
so the Challenges list does not open with them (checked in Movie Lot
Career, where SELECT alone opens it). Hide HUD stays hidden until toggled
again, also in the next level.

Do not combine: two Widescreen sections; two values of the same setting
(Camera Distance 150% and 200%, ...); Juice Regeneration with Juice Drain
50% or No Juice Drain; Invulnerability with Juice Lost on Crash 50%. These
write the same addresses, so one of them wins. Everything else can be on
together (tested: 19 sections at once, from boot through the menus into a
level and a Quick Restart).

Known limits: the level loading screen is drawn outside the 2D path and
stays stretched in widescreen; the split-screen HUD keeps its shape but is
not moved to the edges; Farthest split-screen draw distance draws much more
and may slow down on a real PS2.

## How it works

Addresses and findings come from the native port (`docs/BRINGUP.md`,
`docs/DEVTOOLS.md`, PS2Recomp `game_overrides/aggressive_inline_*.cpp`).

- **Tuning table writes.** Camera, FOV, motion blur, draw distance, assists
  and cheats write floats in the developer tuning table (0x00246138, entries
  `{name[32], value, max, min, step}`), which the game reads while it plays.
  FOV, Normal Blur and the split-screen far clips are copied when a level
  starts. Split-screen fog is two `lui` constants in the level camera setup
  (0x001903A0, 0x001903B8).
- **Unlock Everything** uses bitwise-OR codes (`7aaaaaaa`) on the career
  unlock word 0x0024CBB8, so other progress bits are kept, plus the 20 skater
  records at 0x0024DB30.
- **Hooks** (`tools/ai_mods_pnach.s`), placed in libkernl's file-system
  functions (sceMkdir ... sceReadlink, 0x00219A80-0x0021B5B7), which the game
  never calls (no call, jump, pointer or address pair in the whole ELF refers
  to them):
  - Part L replaces the play update (game mode 2) in the mode table
    (0x0024B844): the combos, then the game's update. Restart sets the
    loader's target and delay and switches to the loader (0x001BCD38), or
    calls Play Again (0x001A7460) in a Timed Run; the debug menu is game
    mode 6 (0x001A70F0).
  - Part G hooks the zgui widget draw (0x00171CA8): skips InGameUI's /
    MultiplayerUI's Overlay and OverlayToggle when the HUD is hidden, and
    moves HUD widgets by width / 6 (16:9) while they are drawn.
  - Part S hooks sceVif1PkCloseUpkCode (0x00210F28): each finished 2D
    primitive of the screen-space packet (0x0027A848) has its x squeezed
    about the centre by the widescreen scale; in a level, primitives over
    the whole width (fades, menu dimming, letterbox) stay as they are.
  - Variables (section switches, the HUD state, the widescreen scale and
    shift) live in the zero padding after .rodata, 0x002728C0-0x002728DB,
    next to BloodRaynare's frustum code at 0x00272894. A section that is off
    leaves its switch at 0, so sections can share parts: they write identical
    words.

Rebuild after changing the assembly or the sections:

    python3 tools/build_mods_pnach.py          # writes the pnach and raw-codes.txt
    python3 tools/build_mods_pnach.py --listing   # plus the disassembly

It needs the decomp's PS2 binutils (`decomp/tools/binutils`) and, only for
the original words quoted in comments, the game ELF (`extracted/SLUS_203.27`,
not in git). `test-status.txt` holds each section's test status, which ends
up in its description.

## Real PS2 (cheat devices, OPL)

`raw-codes.txt` has every section as unencrypted raw codes
(`2aaaaaaa vvvvvvvv` 32-bit writes, `0aaaaaaa 000000vv` 8-bit writes,
`7aaaaaaa 00Z0vvvv` OR), the format of PS2RD and OPL's cheat engine (OPL
`.cht` files: a quoted name line followed by its codes). **None of it was
tried on a real PS2**, and:

- your device or OPL needs a master code (hook) for this game; none is
  given here (no verified one was available to us);
- the data-only sections are a few codes each and are the safest;
- the hook sections write MIPS code (62 to 288 codes each). Cheat engines
  usually cope with code patches, but code written while the game runs can
  meet a stale instruction cache, and some devices limit the number of
  codes. Try the data sections first.

## What stays PC-only, and why

These port features are host-side (C++ in the runtime around the
recompiled game), not changes the PS2 itself could run:

- **Renderer and display:** the GPU renderer, resolution scaling, full
  448-line rendering (the game renders 224-line fields and leaves too little
  GS memory for full frames; PCSX2 has its own deinterlacing and upscaling),
  per-screen aspect switching (the port shows the front end at 4:3 and
  levels at 16:9; on PCSX2 the output is one aspect, hence the pillarboxed
  front end here), screenshots (PCSX2 has its own).
- **Input:** keyboard and mouse, mouse-look, rebinding, prompts for Xbox /
  Nintendo pads and remapped buttons, mouse in menus, the park editor's mouse
  wheel zoom (PCSX2 has its own bindings).
- **Files and loading:** native save files, the PCSX2 save import, fast
  loads, native movies and sound, the native disc layer, custom soundtracks
  and their titles, `Mods/` file replacement and `tuning.ini` (the debug
  menu's Tuning editor does the same job live).
- **The Settings pages and Quit to Desktop:** the in-game Settings screen is
  UI script patches (UILIB.ZAL) bound to native code that changes host
  settings; on a PS2 there is nothing for them to change. The Restart Level
  pause item needs a new script action backed by native code; the L3+R3
  combo does the same job here.
- **Language:** the disc's other string tables only cover memory card texts
  (mostly untranslated); not worth a patch.

No disc patch (xdelta) is offered: every change that made sense on a PS2
fits in memory patches, and the menu-script changes of the port only make
sense with its native code behind them. Game data and anything derived
from the disc stay out of this repository.

## How it was tested

PCSX2 2.8.2 with a private data folder (BIOS and settings copied from the
reference setup, its own PINE slot), driven by `tools/pcsx2pad.py` (scripted
pad input over PINE, see `tools/PCSX2PAD.md`) with savestates in the
Tutorial, Movie Lot Career, Freeskate, Timed Run and split screen. For split
screen the test copy of the pad hook also fed player 2. Each section's words
were read back from EE RAM with every section on and with groups of
sections on, and the effects were captured as screenshots (FOV, camera,
widescreen in three aspects, menus, restart before and after, debug menu,
hidden HUD, juice meter over time, game over, unlocks, split-screen draw
distance) or read from the game's state (motion blur switch and alpha,
split-screen far clip, unlock bits).
