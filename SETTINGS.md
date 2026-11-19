# PC settings in the game's own menus (Aggressive Inline, SLUS-20327)

Design note for the settings screens of the Aggressive Inline port: PC
settings (display, sound, controls, game) shown by the game's own front end,
in its fonts, menu widgets, highlight, sounds and navigation, instead of a
panel drawn over the game.

## How the game builds its menus

The whole UI is data. `ASSETS/UILIB.ZAL` (Z-Axis archive) holds the texture
libraries (`*.zif`) and five UI scripts, plain text written by Z-Axis'
"UIBuilder": `FrontEnd.zsc` (title, main menu, Options), `InGame.zsc` (HUD,
pause menu and its pages), `SaveLoad.zsc`, `Multiplayer.zsc`,
`ParkEditor.zsc`. The ELF names them (`uilib:FrontEnd.zsc` etc.).

Archive layout (little endian):

    ZDIR <size>  DIDF 8: <count> 0
                 GRID 4*count: name hash per entry
                 DLEN 4*count: 0x50000000 | byte length
                 OFST 4*count: offset from the start of the file
    entries, each at a 16-byte aligned offset (zif entries are ULIB chunks;
    the scripts are bare text)

A script is a tree of widgets with properties and event handlers:

    OBJECT "guiToggler" "SFXVolume"
    {
        MinValue 0  MaxValue 128  IncValue 2  CurValue 128
        OnChange { ... PLAY Bar.Refresh
                   CALL inlineMenuSetSFXVolume CurValue .playSoundsOnChange }
        OBJECT "guiLabel" "Label" { Charset "CommonFonts.fnt_ovr_trick" Text "SFX Volume:" }
    }

Widgets: guiScreenSpace, guiGroup, guiFrame (the window frame from
CommonWindow), guiImage, guiLabel, guiMenu (vertical or horizontal list,
`Items`, `SelectedIndex`, `OnItemSelect`, `OnChange`, focus colours),
guiToggler (a value changed with left/right: `CurValue`, `MinValue`,
`MaxValue`, `IncValue`, `WrapAround`, `OnChange`, `OnSelect`, `OnCancel`),
guiKeySniffer (pad events: `OnUpKey`, `OnDownKey`, `OnFire1-4`, `OnStart`,
...), guiTextBox, guiScrollView, guiKeyboard. The language has
`DECLARE`/`SET`/`ADD`/`MULT`/`DIV`, `IF ... GOTO`, labels, `PLAY <action>`,
`ABORT`, `SETFOCUS`/`KILLFOCUS` and `CALL <native> <args>`. Actions are
`ACTION "Interpolate"` (timed, `LINEAR` over `.Ratio`) and `ACTION
"Script"`. Widgets are addressed by path (`.Sibling`, `..Parent.Child`,
`Root.FrontEnd...`).

Natives are C functions registered by name with `func_170820(name, fn)`
(0x170820: hashes the name with 0x148888, copies it, stores fn in the table
at 0x240CD0). The front end registers the `inlineMenu*` set in FUN_001A7660,
the pause menu the `inlinePause*` set in FUN_001BBC30. A native gets a0 =
argv, an array of value handles, one per argument: a widget, a property
(`CurValue`) or a declared variable. Helpers used by the natives:

| Address | Use |
|---|---|
| 0x17FD00 | `int getInt(handle, int *out)` |
| 0x17FD50 | `float getFloat(handle, float *out)` |
| 0x17FDA0 | `bool getString(handle, char *out)` |
| 0x17FDF0 | `bool getBool(handle, int *out)` |
| 0x1802A8 | `setProperty(widget, "Name", void *value)` (float for CurValue, char* for Text) |
| 0x1822B8 | `runScript(widget or 0, "PLAY OnChange", 0)`: runs script text on a widget |

A widget's name is at +0x14, its parent at +0x08 (as used by the widescreen
HUD hook).

The existing option pages: the front end's Options is a horizontal icon menu
(`Background.Options.Menu`: Save & Load, Sound, Controller, Media Viewer,
Credits, Cheats) whose items open sub pages; Sound is a guiKeySniffer with
three guiTogglers (SFX, Music, Speech volume, bars and arrows), Controller is
the PS2 pad layout (CONFIG A/B/C) and vibration. The pause menu
(`InGameUI.PauseMenus...Menu`) is a vertical text guiMenu (Resume Game,
Challenges, ..., Sound Options, Level Select, Exit; set per game mode with
`SET Menu.Items`), and its Sound Options page is the same three togglers.
The intro movies are a chain of script actions on `FrontEnd.Movie`
(StartAKAMovie, StartZAxisMovie, StartIntroMovie, then ReturnToMainMenu);
START plays the next one.

## Hook points

1. **Script patch at load.** When the IOP opens `ASSETS/UILIB.ZAL`
   (`PS2IopHostAdapter::openHostFile`), the runtime serves a copy in memory
   with `FrontEnd.zsc` and `InGame.zsc` changed by text edits (insert pages,
   add menu items, rename), the directory rebuilt (lengths, offsets). The
   disc folder and the user's data stay untouched; nothing of the game is
   committed (the patch is code that edits the user's own file, and an edit
   that does not find its anchor is skipped and logged).
2. **Host natives.** New names (`pcSettings*`) registered through
   `func_170820` when the game registers its own (a wrapper of FUN_001A7660 /
   FUN_001BBC30), each pointing at a spare guest address bound to a host
   function with `PS2Runtime::registerFunction`. A native identifies its
   setting by the calling widget's name, reads and writes values with the
   helpers above, and updates labels with `runScript`.
3. **Guest calls from host code** run with EE safe points held off
   (`PS2Runtime::callGuestFunction`), so a nested call cannot unwind to the
   scheduler halfway; they are short helpers (`runScript`, the native table
   insert). Hooks and natives return through ra like generated code: a
   thread preempted just before the call is resumed by calling the hook
   directly, and a pc left at its entry would call it again.
4. **Gotchas found on the way.** The engine silently refuses a script
   command longer than about 256 characters (hence one label per row, each
   set with a short `SET`). `func_1822B8`'s names resolve relative to the
   widget passed; in a handler `.X` is a sibling and `.` the parent.

## The settings model

`ps2x::settings` (runtime library, no UI): a registry of settings, each with
a key (`display.renderer`), label, kind (on/off, choice, range), current
value, the text for a value, whether it applies live or on restart, and
set/get callbacks. The app registers the settings.ini ones (and keeps the
View menu items working through the same calls, so the menu bar and the
in-game pages show the same state), the input layer registers the
controls.ini ones and the rebinding entries, and the audio output its
master volume. The game natives only talk to this registry, so another
front end (the menu bar, a fallback panel) uses the same values and apply
logic.

Each game has its own options (`ps2x::app::GameSettingsProfile`,
src/collection/settings.cpp): its settings.ini header, the keys of its
own file and the Game rows, and which of the app's shared values it acts
on (widescreen, motion blur, draw distance, movie volume, skip intro
movies; their rows and menu items show only for such a game). A game lists
what any of its releases offers once the collection has it for that game;
a new option gets a `GameOption` and goes into that game's list. In the
collection, the root's settings.ini holds the app's settings (display,
graphics, sound, pause in background, skip intro movies, log file) for
every game, games/<id>/settings.ini the game's options; the input layer
the game's controls (src/lib/ps2_game_controls.cpp).

## What was built

- **Front end:** Options > the Controller icon (relabelled "Settings")
  opens a full Settings screen laid out like the Controller screen it
  replaces: window frame, rotated side banner, the game's fonts, the red
  scratch highlight, gold values, a section selector at the bottom (L1/R1,
  Q/E by default) and button prompts. Sections: Display, Sound, Controls,
  Camera, Assists, Cheats, Game. Up/down picks a row, left/right changes it, Cross changes a toggle
  or starts a rebind, Triangle goes back. Changes apply at once.
- **Pause menu:** a Settings item after Sound Options opens the same rows
  as a page of the pause frame (title "SETTINGS"; the frame is widened and
  centred while it shows). The same in the split screen's pause menu
  (Multiplayer.zsc: its fixed item list is picked by index, so the labels
  after Sound Options are renumbered) and before Level Select in the
  Tutorial's own pause menu (its lesson list, `Frame.Tutorial`). The
  page's Back shows again the menu it came from: each menu's Show calls
  `pcSettingsPauseShown` with its group, and the page's Hide calls
  `pcSettingsPauseBack`, which plays that group's Show.
- **Settings... / Cmd+,** (app menu; `settings` on PS2X_CONTROL): by the
  engine's own path in all three games (`zge_settings_open.h`): no pad
  press is made up for the player and no clock is waited on. In a level
  with play running, the request is a flag the game's play frame consumes
  on the EE thread: where the game's own Start would pause (AI
  `checkPauseButtons` with the level's mode playing; DM2 `actionFrame`
  once play's Start wait is over; BMX XXX `playStateFrame` past its first
  frames) the frame does what Start does (AI: the level's stop
  reason cleared and `enterGameMode(paused)`; DM2 and BMX XXX: the
  sound's paused mode and `showGameScreen(pause)`, the frame ending with
  1) and the pause menu the game builds gets the Settings page in its
  place, as its SETTINGS line does (AI: `pcSettingsPauseShown` plays
  GoToPcSettings; DM2: after `showPauseScreen`; BMX XXX: the pause
  handler's enter). With the pause menu, the page, a tip or a message box
  up, or before play has started, the notice "Settings: in the pause
  menu" and nothing else. Outside a level, DM2 and BMX XXX show
  "Settings: in the game's Options"; AI's front end, from the main menu
  or Options, opens Options and its Settings page (the main menu's and
  Options' OnGetFocus/OnLostFocus call natives that say which has the
  focus; a looping action calls `pcFrontEndPoll` each frame to act on a
  request). From player or level select (and the park editor's dialog)
  it presses Back (Triangle) once per screen until the main menu has the
  focus, then goes on as from there (those menus' focus handlers call
  `pcBackFocus` / `pcMenuBlur`).
- **Rows** come from `ps2x::settings`: Display (display mode window / full
  screen / borderless, renderer, GPU resolution, full height (GPU),
  widescreen, wide aspect (16:9, 16:10, 21:9, 32:9, window), motion blur
  (off, low, original), draw distance (each game's own, its settings.ini
  `[gameplay] draw_distance`: original, far, farthest; AI: split
  screen's far clip 100 -> 250 or 1000, and with farthest every level's
  1000 -> 2500, past the game's cap, and every camera's far clip the
  game does not set again (the 2D camera's all session, a new camera's
  until the level sets its own) 200 -> 500; the fog moves out by the same
  ratio, and the models' LOD switch distances too, on every render path,
  so trees and buildings keep their detailed parts as far out. In DM2
  and BMX XXX every level's far clip moves, single player's too. In all
  three it is the game's own state, so it is Original by default, comes
  with the boot and applies at the next start, and its row says it
  changes the game. An earlier version's app-wide pick went to every
  game's file, unless it was Far, that version's default), Xbox
  textures and Xbox movies (only when the app was built with your Xbox
  disc, see below), scaling,
  vsync, variable refresh, frame interpolation (pictures between the game's frames on a
  display faster than 60 Hz, GPU renderer, off by default: runtime/src/lib/gs/GPU_BACKEND.md
  "Frame interpolation"), frame rate counter, pause in background), Sound (master volume,
  mute in background, movie volume, then music, effects and speech volume:
  the game's own Options > Sound volumes, read from and set through the
  game, so both pages show the same values), Controls (mouse sensitivity, invert mouse Y, mouse
  camera, stick dead zone, gamepad on/off, then a row per action for keys
  and one per game button for the gamepad, reset to defaults), Game (skip
  intro movies, applies at the next start).
- **Rebinding:** Cross on a key row makes the input layer take the next key
  or mouse button (Esc cancels, Backspace/Delete unbinds; a gamepad's
  Circle or Start cancels) and write it to controls.ini, replacing the
  first binding and taking it away from other actions. Gamepad rows take
  the next gamepad button and swap with the action that had it
  (`[gamepad] cross = SOUTH`, ...). Nothing reaches the game meanwhile, nor
  until the keys are released.
- **Overlay:** only the frame rate counter (Dear ImGui, top right).
- **Where it is on:** in the app (it sets `PS2X_GAME_SETTINGS_MENUS=1`).
  The developer runner leaves the game's scripts and native table alone
  unless `PS2X_GAME_SETTINGS_MENUS=1`: the added widgets and natives move
  the game's heap, and the logic/replay checks (logiccheck, careercheck,
  inputcheck's Career replay) anchor on heap addresses. Logiccheck has a
  route for the menus too (it takes it when `PS2X_GAME_SETTINGS_MENUS=1`:
  the zgui panels at their menus-on addresses), which the gate runs.

## Button prompts, Quit to Desktop, mouse in menus

(`game_overrides/aggressive_inline_prompts.cpp`, on with the same switch.)

**Which device.** The input layer (`PSPadBackend::activeDevice`) follows
the last device used: a key, mouse button or clear mouse motion picks the
keyboard; a gamepad button or a stick past half picks the gamepad, as
PlayStation, Xbox or Nintendo by SDL's gamepad type (unknown pads count
as Xbox). At start: the gamepad if one is connected. `PS2X_PROMPTS=
keyboard|playstation|xbox|nintendo` fixes it, as does `prompts <device>`
on PS2X_CONTROL (`auto` follows the input again).
`PSPadBackend::promptGeneration` changes with the device and with any
binding, so a rebind shows at once.

**Pictures.** The game shows PlayStation buttons as textures
(`ps2Buttons.zif`: `select_button` cross, `cancel_button` triangle,
`secondary_button` circle, `Tird_Button` square, `shoulder_button` R1,
`DPad_Button`; `ParkEditorButton.zif`: `Button1-5`, `ButtonL1/R1Up/Down`)
and as characters of two fonts (`fnt_ovr_small`: `\` cross, `` ` ``
square, `|` triangle, `~` circle, used by the Tutorial's tips;
`fnt_ps2buttons`: `b x y l L r R s S` in the trick lists). The TXRC chunk
handler 0x128240 is wrapped: the object it gets from 0x128468 (0x560
bytes; name at +0x10, pixels +0x40 with their quadword count at +0x44,
height << 16 | width +0x4C, the upload DMA chain +0x60, CLUT +0x4A8 and its
count, its chain +0x4C0) is noted if its name is one of those, with a copy
of its pixels and CLUT; 0x128590 (free) forgets it. The game uploads such
a texture to GS memory once and keeps it there, so on a device change the
RAM copy is redrawn and the game's own two chains (CNT/REF/RET tags) are
sent through the GIF again (`PS2Memory::processGIFPacket`, TRXPOS reset
first), from the text hooks below (EE thread, between the game's own
transfers). CLUTs are RGBA with alpha 0-0x80 and, for 256 colours, the
CSM1 order (entries 8-15 and 16-23 of each 32 swapped). Whole textures get
a new palette (median cut); in the font atlases only the button cells
change, coloured with palette entries the rest of the atlas does not use.

What is drawn (`runtime/ps2_glyph_art.h`): keyboard key caps with the
first key bound to the button in controls.ini (single characters large;
Space, Enter, Backspace and arrows as symbols; long names smaller), a mouse
with the bound button lit, the arrow keys for the d-pad; for an Xbox-style
pad the face buttons are the disc's own `xboxButtons.zif` pictures (A, B,
X, Y, d-pad; read from UILIB.ZAL when it is opened, by position: the
button in the south place is A whatever the game button), shoulders and
Start/Back as labelled dark buttons (LB RB LT RT START BACK); Nintendo
pads get round labelled buttons (B A Y X by position, L R ZL ZR + -). The
labels use the game's `fnt_14pt` (FrontEndFonts), also read from the
disc. PlayStation pads get the original pictures back, except for game
buttons that `[gamepad]` remaps to another button: those show the
picture of the button that presses them (ps2Buttons.zif's own pictures,
read from UILIB.ZAL like the Xbox ones; L1 L2 R2 START SELECT as labels),
and "L1"/"R1"/... in text follow (in one pass, so swapped buttons stay
swapped). Nothing of the game is stored: everything comes from the
user's UILIB.ZAL at run time.

**Park editor and Nintendo pictures (uipolish).** The park editor's
L1/L2/R1/R2 at the top of its screen are characters of `fnt_lvl_ed`
(ParkEditor.Icons: "1" L1, "2" L2, "3" R1, "4" R2, and "6" "7" "5" "8"
while held); those cells are redrawn like the other button cells: the
bound key with the keyboard, LB LT RB RT on an Xbox-style pad, grey L ZL R
ZR keys on a Nintendo-style one, the pressing button's name on a remapped
PlayStation pad, tinted blue while held. Nintendo-style pads get pictures
drawn in the game's look (`ps2_glyph_art.h`: dark round 3D face buttons
lettered by position, B south, A east, Y west, X north; grey shoulder
keys; round + and -; a grey d-pad); small labels are drawn slightly bold.
The Tutorial's loading screens (LOADING/TUT/TUT0-3.RAW) are composed in
memory for a Nintendo-style pad and an Xbox-style one: the PS2 file's LOADING bar is kept (rows 340
down), above it the old picture is blurred into a background, and a pad
drawn from shapes (`ps2_pad_art.h`, no console maker's art) gets the
screen's captions in the game's marker font fnt_ovr_small (the four
screens' wording, TUT1/TUT2 swaps, no Challenges on TUT3; translated with
the language), with arrows to the controls that press each game button
(`[gamepad]` remaps followed). The game shows these screens at half
height, so their labels are drawn bold. The Xbox release's own screens
(Rotate L / R on the triggers, Cess slide on the white button) are shown
only when `[gamepad]` puts Rotate on the triggers and Cess slide on the
shoulders with the face buttons and Start as they are; with the port's
default bindings (Rotate LB / RB, Cess slide LT / RT) they would mislabel
them, so the drawn screen is the default. With the keyboard the screens
are drawn with the bound keys: each caption beside the key caps of its
game button (the first key in controls.ini, a mouse button as the mouse
with it lit), the movement keys as a cross with Forward / Left / Right /
Brake around it, and the mouse for the camera with mouse-look (else the
right stick's keys); redrawn when a binding changes. PlayStation pads keep
the PS2 picture. With the settings menus on, Options > Controller opens
the port's Settings page, so the game's own pad picture
(FrontEnd.controller) never shows.

**Gamepad rows (uipolish).** Controls > Gamepads lists the connected pads
in player order: one pad's name, or "1 <pad>, 2 <pad>" (the styles when the
names are too long), or None; it is read when the page redraws (a key on
the page). Cross on it writes each pad's details to the log. Test Rumble
runs each pad's large motor, then its small one, through the same per-slot
path as the game's vibration. Swap Pads 1 and 2 exchanges which pad is
player 1 and which is player 2 (split screen).

**Words.** Every text the game draws goes through two primitives:
0x160B90 (font, position, text: one line; a thunk into 0x160B98 with a3 =
0) and 0x161628 (wrapped text, text in a2). Both are replaced by hooks
that change a2 and run the original in the same frame (they can yield to
the scheduler while flushing GS packets, so a nested guest call does not
work). The changed text goes to one of eight 2 KB guest buffers. Rules:
with the keyboard, "the \ button" (a button character) becomes the key
("Space", "F", "the right mouse button"), "the R1 button" and "R1" the
bound key, "directional button" "arrow keys", "left analog stick" the
WASD keys, "right analog stick" "the mouse", "Press any button" "Press any
key"; with an Xbox/Nintendo pad the button characters stay (their cells
show A/B/X/Y), "directional button" becomes "D-pad", the sticks "left
stick"/"right stick", L1/R1/L2/R2 LB/RB/LT/RT (L/R/ZL/ZR). Text in
`fnt_ps2buttons` (all pictures) is left alone. The memory card texts of
STRING.EUS are never shown with native saves; the legal screen
(LEGAL1.RAW) is a title splash, kept.

**Quit to Desktop.** Pause menus (Career, Freeskate, Timed Run, and the
Tutorial's lesson list) get a last item that calls `pcQuitToDesktop`
(`ps2_host::requestQuit`: the same path as closing the window, so the
shutdown hooks run and the career is autosaved). On the main menu (a
guiToggler, which has no Cancel or Start handlers) a key sniffer that is
active while the menu has the focus catches Back (Triangle, right click)
and Start: Back, or Start from the Escape key, shows "Quit to Desktop?"
(No / Yes, a small guiMenu over the movie window; it takes the focus a few
frames later so the same press does not answer it); Start from Enter or a
gamepad presses Cross (select). The footer's Back prompt reads Quit there.

**Mouse in menus.** While the game is not in play (game mode 0x276FE4 !=
2: the front end, pause menus, tip and message boxes), a wheel step
presses up or down and a left click Cross (the left button's own Square
is held back); right click is Triangle (back) by default anyway. Pointing
at an item does not select it: the menu engine has no pointer support at
all (no mouse events or hit testing in zgui; item rectangles only exist
inside each widget type's draw code), so hover would mean reconstructing
every widget type's layout.

**Park editor zoom.** The park editor (its editor object at 0x278808,
non-zero while it runs; game mode 2) zooms its camera with the right
stick's Y axis (up in, down out; X does nothing). There the wheel pushes
that axis fully for 6 pad reads per step (`PSPadBackend::setWheelStick`),
and its keyboard help reads "Mouse wheel = zoom camera" instead of
"Right analog stick = zoom camera". The right stick keys default to
keypad 8/2/4/6, with Page Up/Page Down also on up/down.
`PS2X_HOST_INPUT_SCRIPT` takes `<t>:WHEEL=<steps>` for tests.

**Custom soundtrack titles.** The playlist the pause menu's Sound
Options shows (title and artist per song) is filled once at start by
0x170090(list, index, title, artist) into 0x80-byte entries (title at
list + 0x4C + 0x80 index, artist 0x40 after it; the list pointer at
0x277010, index = MUSIC.ZSD wave). With a custom song for an index the
entry gets the file's title and artist tags (libavformat), else its file
name without the number; quotes and braces are replaced (the caption is
set with a script command). The hook is only installed when a custom
song exists.

## Gameplay pages, Restart, Hide HUD, Screenshot

(`game_overrides/aggressive_inline_gameplay.cpp`, on with the same switch;
kept in `PS2X_GAMEPLAY_INI`, the app's `gameplay.ini`.)

**Camera, Assists, Cheats** are rows of the settings registry like the
others (sections `camera.`, `assist.`, `cheat.`). They change entries of
the game's developer tuning table (docs/DEVTOOLS.md), written at VBlank
on the EE thread while they differ from what the rows ask; an entry at its
default is never written (one this code changed is put back once). Camera
Distance scales Distance, Grind Distance and Distance HPole, Camera Height
Height and Grind Height (read live by the follow camera). The FOV is only
read when a camera mode is set: the getter 0x18C828 (Camera FOV, or the
level's MP FOV in split screen) is wrapped to scale its result, and the
setter 0x18FCA0 (camera +0x30) to know the camera, whose +0x30 is rewritten
when the row changes, unless it holds another value then (the R1 camera).
Out of Juice writes Death by Juice 0 (the Career check at 0x1B4964 skips
the run's end), Juice Drain and Juice Lost on Crash scale Max Juice
Degradation/sec and Juice Lost/Level on crash. The cheats are the game's
list (0x2541F0) with the writes of their codes (inlineMenuCheat 0x1AA628):
Wall Ride Gravity 3, Max Juice Degradation -0.2, Juice Lost 0, Grind /
Stall / Manual Balance Factor and Init 0, Air Rotate 640. The codes also
set a bit of 0x24CBC8 (in the career save; any bit makes the next level
flag its players, 0x1A60D8); the rows do not. Unlock Everything (two steps:
"Sure? Again", then "Unlocked") ORs bits 0-19 (the D R R D L D L D R R R
code) and 48-54 (Media Viewer cutscenes) into 0x24CBB8 and sets each of
the 20 skater records' +0x1181 = 10 and level mask +0x1184 (the Konami
code): the career save, written at the next autosave.

**Restart** (`game_overrides/aggressive_inline_restart.cpp`). The game's
top-level states (0x24C850: 0 level, 1 front end, 2 loader) switch through
0x1BCD38(state), the old one's exit then the new one's enter; the loader
waits the byte at 0x276DD5 frames, then enters the state in 0x276DD8. The
menus' Start sets 0 and 1 (inlineMenuStartGame), the pause menu's Exit 1
and 3. So Restart Level writes 0 and 1 and tail-calls the switch as
inlinePauseExit does: the level's own exit runs (career commit, native save
autosave) and the same level, skaters and mode load (split screen: back to
its game menu). Game type 0x24C9B3: 1 Career, 2 Freeskate, 3 Timed Run, 4
split screen; level 0x24C9B4 (7 the Tutorial).

- Pause items. InGame.zsc: Restart Level after Resume Game in Career and
  Freeskate (an ACTION "Script" next to ExitGame calling `pcRestartLevel`,
  with the same 0.00001 s delay); Restart Run in Timed Run (`PLAY .Hide`,
  `inlinePauseModeExit`, `inlinePauseRestartTimedRun`: the game's own Play
  Again, 0x1A7460); Retry Challenge above Restart Level in Career while a
  challenge can be given again (the Show's Career list is followed by
  `pcPauseCareerItems Menu`, which sets the list with it). The Tutorial's
  own menu: Restart Lesson after Resume Tutorial (the lesson items, one row
  down, jump through `pcJumpToLesson .Menu`: SelectedIndex less two; the
  completed column gets a blank row, `pcTutorialCheckboxes`). Multiplayer.zsc:
  Restart Run and Restart Level after Resume Game (items 1 and 2, the
  others' index labels two further).
- Retry Challenge (Career). A giver is a level script object; the engine's
  state change is 0x1E0798(object hash, state hash) (exit, handlers, enter),
  current states in the table at 0x2798C0 ({hash, word offset}, -1 ended;
  the object's slot into the words at 0x2798BC). Pressing Action by a giver
  sends it ACTION (0x196BF8 -> 0x152910, handled synchronously); the state
  change of that same object during the handler is "given": from, to and
  the skater's matrix and position are kept. Again: the skater respawned
  there (the game's named respawn 0x193EB0 with a locator name only the
  hooked lookup 0x187800 answers), the object put back in from (if it is
  not there) and into to through 0x1E0798: the attempt's exit releases its
  timer and tells the other givers to stop waiting, its enter makes a new
  timer and challenge score. Kept while running or failed back to the
  giver; dropped when the giver's objective is set to complete (0x1BE1A8,
  the record at 0x2795A0 + 0x2C n names the giver at +0x14), the object
  reaches complete/done, another challenge is given, or out of the level.
  Parts other objects track (rails of a multi-rail goal) are not reset.
- Restart Lesson (Tutorial). inlinePauseJumpToTutorial sends "GoToLesson" +
  letter to ScriptObj_PlayerOne; the current lesson is scriptObj_controller's
  state (LessonA..LessonW).
- Split screen Restart Run: the running game (level+2: 0 Most Points .. 4
  Animal Rescue; level+8 its timer) ended without its results (Twenty One
  0x1AED80; Animal Rescue 0x196690 per player and the parrots 0x1C5FB0(0)),
  the game menu's SelectedIndex set to it, then the game's
  inlinePauseMultiplayerStartEnd (0x1BB738: setup, Play Again for both).
- The key, in place of one level update (0x1ACA68): Tutorial the lesson
  (the level between lessons), Timed Run the run, split screen the game (not
  before one is chosen), Career the challenge if one can be retried, else
  the level. Pause items that run in play ask for the next update too.
  `inlinePauseRestartFromLoad` (0x1BB4D8 -> 0x1D9630) is the career load
  from the save, not a restart. `PS2X_TRACE_RESTART=1` logs state changes.
- `PS2Runtime::callGuestFunction` follows tail jumps and branches into split
  off blocks to the return (StartEnd ends in Play Again, which ends in a
  loop over such blocks).

**Hide HUD** skips InGameUI's and MultiplayerUI's Overlay and
OverlayToggle groups in zgui's widget draw 0x171CA8 (score, juice meter,
trick list, balance meters, timer, messages); it shows again when the front
end comes back. **Screenshot** is the app's, for every game (src/collection/app.cpp
onHostFrame): it asks the runtime for the next presented
picture (`PS2Runtime::requestScreenshot`, the PS2X_CONTROL "shot" path:
displayed aspect at the renderer's resolution) as
`<game> <date> <time>.png` in `PS2X_SCREENSHOT_DIR`, and
`ps2x::settings::notify` shows "Screenshot saved" in the overlay (top left,
next to where the frame counter goes; not in the picture).

**Keys.** `controls.ini` `[hotkeys]`: `restart` (Backspace), `hide_hud`
(F9), `screenshot` (F12) and `<name>_pad` gamepad chords (`restart_pad =
L3+R3`). The input layer counts presses (`PSPadBackend::hotkeyPresses`);
the override acts on a new count at VBlank (Restart only in play, past the
level's 30th frame). Their rows sit at the end of the key rows; binding one
takes its key off the game controls and the other way round. `[gamepad]
prompts = auto|keyboard|xbox|playstation|nintendo` (Controls > Button
Prompts) fixes the prompt style like `PS2X_PROMPTS`, which wins. For tests,
`set hotkey.restart 1` (`hide_hud`, `screenshot`, `photo_mode`) on
PS2X_CONTROL counts a press, and `set` on an action row runs it.

## "Are you sure?" in the pause menus

(`game_overrides/aggressive_inline_settings.cpp`, `patchConfirm`.) As the
Xbox release does (its AreYouSure frame in InGameUI and MultiplayerUI,
"$KSTR_ARE_YOU_SURE" Yes / No), Level Select and Exit Game ask "Are you
sure?" first, in the Career / Freeskate / Timed Run pause menu, the
Tutorial's lesson list and the split screen pause menu; Quit to Desktop
there too (the Xbox asks before its Dashboard). A guiFrame PcConfirm is
added to each UI root: Yes / No, No preselected, Triangle is No (back to
the menu it came from, the item still selected). The Xbox frame keeps the
script to run and the menu to return to in String variables played
indirectly; here two Sint32 (Action: 1 Level Select, 2 Exit Game, 3 Quit
to Desktop; From: 0 the pause menu, 1 the Tutorial's) and Yes does what the
item did: the pause menu's ExitGameToLevelSelect / ExitGame actions after
inlinePauseTimedRunEndRun, the Tutorial's inlinePauseExit, split screen's
inlinePauseExitToLevelSelect / inlinePauseExit, pcQuitToDesktop. The UI is
512 units wide; the frame is centred on the pause frame (x 269). Texts are
English in the script and translated by the text hooks ("Are you sure?" is
KSTR_ARE_YOU_SURE in the Xbox tables). The PS2 already asks in the park
editor (clear, load, leave) and, with card saves, before saving and loading;
the native saves ask nothing (design: no card dialogs).

## Photo mode

(`game_overrides/aggressive_inline_photo.cpp`, `runtime/ps2_photo.h`; on
with the same switch.)

**Where.** Photo Mode in the pause menu (Career, Freeskate, Timed Run:
before Sound Options; the Tutorial's lesson list: before Settings; split
screen: before Sound Options, the later items one further down; back to
the pause menu after) and the `photo_mode`
key (`[hotkeys]`, P by default; `photo_mode_pad` for a chord, none by
default) anywhere in a level, the Tutorial and split screen too (the first
player's view): from play it pauses first (a Start press) and resumes after
(a Start press once the pause menu has the focus).

**Freezing.** Photo mode runs inside the game's own pause (game mode 3 of
the mode table 0x24B810: enter 0x1BBC30, update 0x1BC280, exit 0x1BC300),
which already holds the level, the skaters, the timers and the sound. The
pause update only sets each camera's mode (0x18FD68 with the tuning table's
Camera Type); in photo mode the free camera runs in its place. Every port
reads an idle pad (`PSPadBackend::setFreeInput`; the keyboard, mouse and pad
go to `takeFreeInput`, the cursor is captured), so the pause menu does not
move, and what is still held when it ends stays away from the game until
released. Checked by RAM: see BRINGUP.

**Camera.** The game's frame (0x1BCC58) runs the mode update, then the
camera update 0x18FA00 for the player's camera at 0x24C9D0 (+0x00 its scene
node, +0x10 eye, +0x20 look-at point, +0x30 FOV in degrees, +0x71 mode).
Camera mode 5 (the developers' Art Viewer's) only looks from the eye at the
point (0x15E188: 0x13A260 builds the matrix with the world's up, y down;
the node gets the position and a quaternion) and sets the FOV, so the free
camera (`ps2x::photo::FreeCamera`) writes eye, point and FOV and sets mode
5. 0x13A260 ignores its roll argument; it is wrapped to turn the matrix's
right and up rows about the view axis when called for this camera. The
camera's 0x80 bytes are kept at the start and put back at the end (the pause
update sets its mode again the next frame). The camera stays within 40
units of where it started (the game only has the level near the player).

**Controls.** Keyboard and mouse: mouse look, W A S D move, Space / Ctrl up
and down, Q / E roll, wheel or Z / X zoom (FOV 15-110), Shift faster, Alt
slower, - / = speed, F filter, R reset the camera, H hide the help, the
Screenshot key (F12) saves, Esc or the photo key goes back. Gamepad: left
stick move, right stick look, R2 / L2 up and down, L1 / R1 roll, d-pad up
and down zoom, left and right speed, R3 faster, Triangle filter, Select
reset, Cross saves, Square help, Circle or Start back (named for the pad in
the help). The help and short notes ("Field of View 60", "Filter: Cinema")
are the overlay's notice, top left, not in the picture; translated with
the port's words.

**Filters.** None, Letterbox (2.39:1 bars), Vignette, Black & White, Cinema
(letterbox and vignette), Noir (all three), through
`PS2Runtime::setPictureFilter`: while a filter is on, the presented picture
is read back from the GPU renderer (or copied from the CPU one), filtered
on the window thread and shown as an upload, and screenshots get the same
pixels (full resolution: 1920x1440 at 3x). A few milliseconds a frame at
3x; nothing at all without a filter.

## The Zoo (Settings > Game > Zoo Level)

`game.zoo` (settings.ini `[extras] zoo`, default on; `PS2X_ZOO=1` for the
runner, where it is off by default) adds the level cut from the PS2
release to the free skate and timed run level select
(`game_overrides/aggressive_inline_zoo.cpp`): a UILIB.ZAL patch on
FrontEnd.zsc's `FreeskateOnChange` (the giraffe icon the level-select font
already has, between the Museum and the Editor Level, always unlocked, nine
icons), files of ASSETS/LEVELS/ZOO from a provider (runtime/ps2_game_files.h:
the Xbox release's level made into the PS2's at build time when the app was
built with an Xbox image, else the PS2 disc's prototype), and extended
memory. Like
the developer menu it applies at the next start (the UI library is read
once). Details: the bring-up log, "Zoo, the cut level".

With the Xbox files the Zoo also has its ambient NPCs (kelp, sharks, the
willows' swaying foliage): the Xbox executable's six extra NPC types,
SCENE.TXT's models for the ZOO and MPZOO scenes, and the models from
`assets/npc/zoo` converted (`runtime/ps2_zaxis_xbox.h` convertModel), the
Xbox pictures drawn by the GPU renderer.

**Made at build time.** Nothing of the Zoo is converted while the game runs.
`ps2xXboxConvert` (tools/xbox_convert.cpp, `runtime/ps2_zaxis_zoo.h`; run by
the main repo's tools/make-app.sh and make-windows.ps1 from the extracted
Xbox disc and the PS2 disc) writes the level's files, split screen's
ZOO.TXR / ZOO.GEO / ZOO.DAT / ZOO.BIN, the NPC models and the Xbox originals
of their textures (`textures/<path>.xtp`) into Resources/xbox-converted
(Windows: xbox-converted\ next to disc\); the app sets
`PS2X_XBOX_CONVERTED` to it and the override only reads those files
(`aggressive_inline_converted.h`). Developer runs: `ps2xXboxConvert --xbox
<extracted Xbox disc> --ps2 <extracted PS2 disc> --out <dir>`, then
`PS2X_XBOX_CONVERTED=<dir>`; without it the runner's Zoo is the PS2 disc's
prototype. The conversion's developer switches are the tool's options now:
`--split-screen-texture-side N` (was `PS2X_ZOO_MP_TEXTURE_SIDE`),
`--group-entries N` (`PS2X_ZOO_GROUP`), `--hide a,b` (`PS2X_ZOO_HIDE`),
`--split-screen-eggs "x,y,z;..."` (`PS2X_ZOO_MP_EGGS`),
`--keep-willow-twins` (`PS2X_ZOO_MP_TWINS`). The split-screen level select gets
the Zoo as a fifth entry: MP\ZOO is the single-player level with smaller
textures (64 a side), a player two start, the free skate script without its
intro camera (doors open, willows sway), the split-screen games' props
(Egg Hunt's eggs, Twenty One's cards; from the PS2 disc's MP\MOVIE) and egg
spots below the respawn points, each willow once, its NPCs animated, and the
Movie Lot's split-screen FOV.
Developer variables: `PS2X_ZOO_NPCS=0` (no ambient NPCs),
`PS2X_ZOO_MP_SCRIPT=empty|free` (split screen with the empty script, or the
free skate one as it is: no timer), `PS2X_ZOO_MP_NPC_ANIMATION=0` (split
screen's NPCs animate only as the game decides: hardly ever),
`PS2X_TRACE_ZOO=1` (also the NPC type lookups), `PS2X_TRACE_ZOO_NPCS=1`
(each Xbox NPC type lookup, counted by type and caller: the NPCs a level
makes).

## Estate (Settings > Game > Estate Level)

`game.estate` (settings.ini `[extras] estate`, default off; shown only in an
app built with a GameCube image; `PS2X_ESTATE=1` and `PS2X_ESTATE_DIR=<dir>`
for the runner) adds TUT/Estate, an earlier export of the Tutorial site left
on the GameCube disc, to the free skate level select as "Estate"
(`game_overrides/aggressive_inline_estate.cpp`): a UILIB.ZAL patch on
FrontEnd.zsc's `FreeskateOnChange` after the Zoo's (its
own icon, before the Editor Level, always unlocked, the Editor Level's still
picture), the Tutorial's level index 7 for its SelectedLevel "Estate" (the
name lookup func_1A8258 wrapped), and its files for ASSETS/LEVELS/TUT from a
provider while it is the level picked and the game is not a career (the
career's Tutorial is always the retail one). Estate's own scripts run (its
25-lesson tutorial); its givers answer Circle (the NPC constructor's free
skate hiding, func_13BD48 at 0x1E36AC, skipped in Estate as in the Zoo); its
pause menu is the Tutorial's lesson list, whose 23 lesson items (the same
topics) start Estate's lessons 1-8, 10-20 and 22-25, Restart Lesson the
current one, the checkboxes Estate's lessons done since the level started;
lesson 9, which has no giver, rail or respawn point in the level, hands over
to lesson 10 when it starts; it keeps its own records beside the career save
(`<saves>/<profile>.estaterecords`; every native save gets the Tutorial's own
row, `PS2X_ESTATE_RECORDS=0` shares the Tutorial's row as before); the Xbox
texture pack is kept off its textures. Applies at the next start.

**Made at build time.** `ps2xGcConvert` (tools/gc_convert.cpp,
`runtime/ps2_zaxis_estate.h`; run by the main repo's tools/make-app.sh and
make-windows.ps1 on what tools/gc_disc.py extracted from the GameCube image)
writes TUT.ZIT, TUT.GEO, TUT.INS, TUT.TXR (its one 24-bit texture made
8-bit), TUT.DAT, TUT.BIN, TUTT.BIN, MOTIONS.ZAL, TEXTURES.TXT (the
textures' keys), VISIBILITY.TXT and ICONS.UFNT (its level-select icon, 'n' /
'N', from a picture of the level drawn from its own files; `--picture
<file.bmp>` writes the picture too) into Resources/estate (Windows: estate\ next to disc\); the
app sets `PS2X_ESTATE_DIR` to it. Developer runs: `python3 -I tools/gc_disc.py
extract <gc image> --out <gc> assets/levels/TUT/Estate`, `ps2xGcConvert --gc
<gc> --out <dir>`, then `PS2X_ESTATE=1 PS2X_ESTATE_DIR=<dir>`;
`PS2X_DEV_START_LEVEL=ESTATE` starts in it. Developer variables:
`PS2X_TRACE_ESTATE=1` (each file served, a line every 10 s in the level),
`PS2X_ESTATE_PROBE=<points>:<out>` (the game's collision under each point
of ai_level.py ground's grid; `PS2X_ESTATE_PROBE_ANY=1` in any level, for a
control), `PS2X_ESTATE_GRINDS=<edges>:<out>` (the game's rail search at
each of ai_level.py edges' grind edges), `PS2X_ESTATE_LESSONS=<out>` (the
lesson list's 23 lessons in turn, each started by its jump, player one put
by its giver and Circle pressed through the pad; a line per lesson and a
summary), `PS2X_ESTATE_ACTION=1` (the Action move's target search traced:
the nearest talker asked, the ACTION messages sent; at frame 150 the nearest
talker at each giver).

## The Ape, Xbox tuning, Xbox scripts, Zoo unlock, camera (Settings > Game, Camera)

These rows take what the Xbox release has beyond the PS2's (details: the
bring-up log, "Xbox-only content" and "Xbox leftovers"). The Game rows
apply at the next start; the runner has them off unless its variable is
set.

- `game.ape` "Ape Skater" (settings.ini `[extras] ape`, default on;
  `PS2X_APE=1`; needs GOR.ZIT made at build time in Resources/xbox-converted
  and the Xbox file `uilib.zal`): the Xbox's secret skater, last in the skater select
  (`game_overrides/aggressive_inline_ape.cpp`). The Ape takes skater slot
  0, the "None" row every PS2 skater table has (the Xbox's index 20 would
  run past the PS2 tables): code AP, the Xbox's stats (the Tinman's), the
  default name APE, the cha_ape picture from the Xbox uilib.zal, the model
  made from the Xbox GOR.ZIT at build time (`ps2xXboxConvert`,
  `runtime/ps2_zaxis_ape.h`: 256 a side; the GPU renderer draws the 512
  originals; nothing converted while the game runs). Locked until the Mummy is unlocked (the Museum's hidden
  challenges, the all-skaters code, Unlock Everything), which is when the
  Xbox unlocks it. Its career is skater record 0, which the game never
  saves: the native saves write it after the game's data in the same
  file ("XAPE" block; the file stays a PS2 save, the game reads only its
  own part). The converted model is the file
  xbox-converted/ASSETS/SKATERS/GORILLA/GOR.ZIT (`PS2X_APE_DUMP` is gone).
- `game.tuning` "Gameplay Tuning" PS2 / Xbox (settings.ini `[gameplay]
  tuning = ps2|xbox`, default ps2; `PS2X_TUNING=xbox`;
  `game_overrides/aggressive_inline_tuning.cpp`): the Xbox balance. Its
  tuning table is the PS2's value for value; what differs is experience
  (special tricks +300,000, the eight key items nothing instead of
  +500,000) and Civic Center's 350,000 point trick (exactly 350,000
  passes). With ps2 nothing is hooked (logic checks against PCSX2).
- `game.xbox_free_skate` "Xbox Free Skate" (settings.ini `[extras]
  xbox_free_skate`, default on; `PS2X_XBOX_FREESKATE=1`;
  `game_overrides/aggressive_inline_freeskate.cpp`): the Boardwalk's and
  the Cannery's free skate scripts from the Xbox files (`levels/brdwlk/
  brdwlkt.bin`, `levels/cannery/canneryt.bin`): their doors open as in the
  career, coaster cars, fan, boulder loop, the Cannery's cutter, idle hints.
- `game.career_scripts` "Career Scripts" PS2 / Xbox (settings.ini
  `[gameplay] career_scripts = xbox|ps2`, default xbox;
  `PS2X_CAREER_SCRIPTS=xbox|ps2`; `aggressive_inline_careerscripts.cpp`;
  the Xbox files `levels/<level>/<level>.bin`, `levels/civic/motions.zal`):
  the career's level scripts from the Xbox release, the PS2's with the
  Xbox's bug fixes (challenges that could stick or be offered again,
  wrong previews, texts that say what the code checks). The Tutorial stays
  the PS2's. Two PS2 texts the Xbox scripts dropped are put back (in
  memory, only into the exact US Xbox files, checked by size and hash):
  the Movie Lot's first-start hint "Use the select button to view the
  current challenges." and the Hangar's "Turret challenge complete!"
  (shown as the Xbox completes the challenge). A skater's saved progress
  in a level holds that script's variables, whose layout differs in four
  levels, so a level the skater has played keeps the script it was played
  with; the setting is for the levels not played yet.
- `game.zoo_unlock` "Zoo Unlock" Museum / Always (settings.ini `[extras]
  zoo_unlock = museum|always`, default always; `PS2X_ZOO_UNLOCK`;
  `aggressive_inline_zoo.cpp`): Museum is the Xbox's rule, the Museum's
  eighth challenge (challenge points) unlocks the Zoo for that skater
  ("New level unlocked!"), the free skate select showing the game's own
  lock until then; the all-levels code and Unlock Everything open it too.
  Always (the default): open whatever the career, as before.
- `camera.style` "Camera" PS2 / Xbox (Settings > Camera; gameplay.ini
  `[camera] style = 0|1`, default 0, live): the Xbox release's field of
  view. Its follow camera is the PS2's (same code, tuning, collision probe
  and smoothing); its projection takes the vertical FOV as FOV x 1.1 /
  aspect, so it keeps about the same width and crops the top and bottom in
  widescreen, where the port's widescreen widens the sides: at 16:9 the
  skater 1.34x as tall as with PS2 (the FOV given to the game 59.5 for 75),
  at 4:3 0.96x (77.3). Single player only (the Xbox fixes 50 in split
  screen; the PS2's per-level FOV stays). Field of View applies on top.

## Languages from the Xbox release

(`game_overrides/aggressive_inline_language.cpp`; runtime
`ps2_zaxis_text.h`, `ps2_xbox_audio.h`.) The US PS2 disc is English only;
the Xbox release has French, German and Spanish. Its files are a build-time
input: tools/make-app.sh (`XBOX_ISO`) and make-windows.ps1 (`-XboxIso`)
extract the user's Xbox image and bundle what tools/xbox-files.txt lists
into Resources/xbox (`PS2X_XBOX_DIR`, read through `ps2_xbox_disc`); the app
never reads an image.

**Rows** (only when the bundle has them): Game > Language (English,
Français, Deutsch, Español; next start; settings.ini `language =
english|french|german|spanish`) and Sound > French Voices (live;
`french_voices = auto|true|false`, auto = on with French). Changing the
language puts French Voices back to auto.

**Text.** The scripts keep their English: the menus jump by item text
(`Goto SelectedText` with labels like `:Resume Game`), and the port's own
natives and edits find widgets and items by their English text. So the
language is applied where text is drawn, in the two text primitives the
prompts already hook (0x160B90, 0x161628): the text goes through an English
-> language table, then the prompt words. The Xbox engine does the same
with its `$KSTR_*` keys, resolved at draw time. The table is made at the
first UILIB.ZAL open from:
- the Xbox string tables, key by key (`string.eus` English with
  `string.fre/.ger/.spa`; 438 keys; menus, HUD messages, records, pause
  menu, the ELF's literal messages like "Stop playing in traffic!");
- the Xbox level scripts (`levels/<L>/<l>.bni` English and `.bnf/.bng/.bns`):
  the script bodies refer to their strings by offset in the string block,
  and two translations of one script have the same body apart from those
  offsets, so each English string is paired with the string its body
  positions point at in the translation (by majority). Then the PS2's own
  `.BIN` is paired with the `.bni` the same way where the bodies line up,
  for the few PS2 wordings that differ. Challenge names and descriptions,
  the Tutorial's tips (the PS2 variant `tut.bn?`, which keeps the PS2
  button characters, not `tut_xbox`);
- the port's rows (labels, values, sections, added menu items) and the
  Settings rows' help lines, written in the override (for the rows the
  game's own words win where both have one; the help lines are the port's).
Lookups: exact; ASCII case-insensitive (an all-caps text gets the
translation in capitals, ASCII letters only, as the game does); spaces kept;
words before a colon ("Jump: 3/10"; the words after it too when they have a
translation, "Pad: Jump"), in brackets ("< Off >"), after or before a number
("0% Complete", "Continue for 0 pts"), a known ending after a name
("Jaren's Stats", "JAREN's Stats:" -> "JAREN Stats:" as the Xbox composes
it); sentence by sentence when every sentence has one (a help line with the
restart note, "Start at the title screen. Applies at next start.");
multi-line texts line by line and between the
engine's inline markup (`#RGB{r g b}`, `#SCALE{x y z}`), a longer word
taking padding spaces from its line so stat columns do not wrap. Single
characters are never translated (key names; the Xbox German table even has
"SAUVEGARDER" under "a"). `PS2X_TRACE_LANGUAGE=1` lists texts drawn with no
translation, `=2` every text with its translation.

**string.eus** is served (disc_patch) with the language's text for each PS2
key whose English the Xbox table has under the same key (trick names, which
the game composes itself; most stay English in the Xbox translations).

**Fonts.** The PS2 fonts are ASCII only. When UILIB.ZAL is opened, each
font of a PS2 library (CommonFonts, InGameFonts, FrontEndFonts, the park
editor's) that the Xbox library of the same name hash also has is replaced
by the Xbox one, chunk and all (`zaxis::replaceFonts`; same UFNT/HFNT/BMP8
layout, 28-62 more characters, some atlases twice as tall; +170 KB of guest
heap). This happens before the prompts note the fonts, so the button cells
they redraw are found in the Xbox atlases. The game looks glyphs up by the
16-bit character, so Latin-1 bytes draw as is. The Xbox fonts do not have
every accented letter (no capital E acute in fnt_14pt and fnt_ovr_small,
the Settings help lines' fonts; the Xbox texts write "Etes-vous" for that):
a character the font being drawn with has no glyph for is drawn as the
letter without its accent (the glyphs from the game's font object: u16
count at +2, 12-byte glyphs at the pointer at +0x0C; its atlas name is at
+4 of the page the pointer at +0x08 points to).

**Fitting.** The Xbox release fits its longer texts by narrowing the widgets
that hold them: the Tutorial's lesson list in the pause menu at 0.8 of its
width ("Engager la conversation"), the Trick List and Sound Options titles
at 1.3 and 1.2 for 1.5, the park editor's pause menu at 0.9. The pause frame
and the panels have the same sizes in both releases' scripts (only the
screen groups are placed for 640 wide), so when UILIB.ZAL is opened every
text widget (label, menu, text box) of a PS2 script whose Xbox counterpart
(same script, same object path, same type) is narrower takes the Xbox's
width scale, its own height (`zaxis::narrowLike`; logged). The Settings
help line is drawn narrower when it is longer than fits (about 74
characters on the front end's page, 44 on the pause page), in English too.
The port's longest pause item is short in French ("Rejouer le niveau").

**Skater names** stay as they are: the Xbox release does not translate
them either (its player select sets the same literal names, "Jaren\nGrob",
"The\nBride"; no string table key holds one). Only the "'s Stats" after the
name is its string table's (" Stats", " Statistiken", " Estads").

**French voices.** SPEECH.ZSD streams through the native sound driver; the
Xbox `speech01.zsd` is a French dub of the same 388 lines in the same order
(84-byte wave entries with names, Xbox IMA ADPCM: 36-byte mono blocks of 64
samples). When a speech stream's source is a PS2 line (by its sector) and
French voices are on, the Xbox line is decoded (once) and played through
the custom-soundtrack path at the game's pitch (both banks have the same
rates); its end and progress come from the decoded length.

**Xbox controller loading screens.** The Tutorial's loading screens
(LOADING/TUT/TUT0-3.RAW, 640x448 RGB) picture the pad. With an Xbox-style
pad in use (`PSPadBackend::activeDevice`) the Xbox release's pictures of its
controller are served instead (a `game_files` provider, asked at each
open); keyboard, PlayStation and Nintendo pads keep the PS2 picture.

**English** changes nothing: no file is replaced, the table is not made,
and the text hooks pass the text through as before.

## Xbox textures and movies

When the app was built with your own Xbox disc of the game (`XBOX_ISO` or
`XBOX_DIR` for tools/make-app.sh, `-XboxIso` or `-XboxDir` for
make-windows.ps1; the build turns it into `Resources/xbox-pack`, the app
never reads a disc), the Xbox release's textures are what every level shows
on the GPU renderer: wherever the Xbox has a counterpart of a PS2 texture
(the same texture, as large or larger, and alike), its picture is drawn;
the PS2 picture only where there is none. The Zoo draws the Xbox originals
of its converted textures (up to 512, DXT3), not the 128-texel copies the
PS2 renderer's texture cache gets. The Display page has two more rows, both
on by default and applied at once; off is the PS2 release's look (and what
the CPU renderer, logic checks and PCSX2 comparisons always use):

| Row | settings.ini | What it does |
|---|---|---|
| Xbox Textures | `[display] xbox_textures = true` | The GPU renderer draws the Xbox release's larger, redrawn textures for levels, skaters, park editor pieces and the multiplayer levels in place of the PS2 ones (the game's memory and logic are unchanged; the CPU renderer shows the PS2 ones) |
| Xbox Movies | `[display] xbox_movies = true` | The Xbox release's 640x480 Bink movies (cutscenes, intro, logos, flythroughs re-recorded with the Xbox textures) play in place of the PS2 ones; read at the start of each movie |

Environment (developer runner, tests): `PS2X_XBOX_PACK=<pack folder>`,
`PS2X_XBOX_TEXTURES=0/1`, `PS2X_XBOX_MOVIES=0/1`; `set
display.xbox_textures 0` on PS2X_CONTROL. Which texture a draw shows is
told by the game's texture cache (its texture objects), not guessed from GS
memory (`runtime/src/lib/gs/GPU_BACKEND.md`, "Replacement pictures"). Details: docs/BRINGUP.md
"Xbox textures and movies" in the bring-up workspace.

## Texture packs

A texture pack is a mod (the Mods page) with a `Textures` folder of PNG
pictures that are drawn in place of the game's own textures, at their
own size: a larger picture is drawn at its resolution on the GPU
renderer (with its mip levels, trilinear and the Display page's
anisotropic filtering). The game itself never sees a pack (its memory and
logic are the same with one or without); the CPU renderer draws the
pictures at the PS2's resolution (each box-filtered to its texture's
size), so edits show there too, extra detail does not.

- Layout: `<mod>/Textures/<game>/**/*.png`, `<game>` the collection's id
  (`ai`, `dm2`, `bmx`) or the disc's serial (`SLUS-20327` and so on); a
  mod for one game alone can put them in `Textures` itself. Later mods win
  on the same picture; the Xbox textures sit under every pack.
- Names: PCSX2's, so a pack made for PCSX2 (its `textures/<serial>/replacements`
  folder) and its dump tooling work unchanged: `<texture hash>-<palette
  hash>-<bits>.png`, `<texture hash>-<bits>.png` without a palette,
  `-r<w>x<h>` for a region of a texture, `-mip<n>` for a picture's own
  mip level (optional: made from the picture otherwise). PNG only (DDS
  not yet). A picture's alpha is the PS2's: 128 (0x80) is opaque, as in
  PCSX2's dumps. `.xtp` files there load as the Xbox pack's do (keyed by
  the texture's indices and name: Aggressive Inline's).
- Making one: dump mode (`PS2X_TEXTURE_DUMP=<folder>`, docs/DEVTOOLS.md)
  writes every texture the game draws under that name; edit or upscale
  the files and put them in a pack.
- Memory: the pictures are read on loader threads when the game starts
  (all of them while they fit 1 GB on Mac and Windows, 256 MB on Android;
  the rest when first drawn, the PS2 texture until then).

| Environment | What it does |
|---|---|
| `PS2X_TEXTURE_PACKS=<folder>[:<folder>...]` (`;` on Windows) | the packs' folders, lowest first (the app sets it from the game's mods) |
| `PS2X_TEXTURE_DUMP=<folder>` | dump mode |
| `PS2X_TEXTURE_PRELOAD=0` | pictures read when first drawn only |
| `PS2X_TEXTURE_PACK_MB=<n>` | the pictures' memory budget |

## Native meshes (render path)

On the GPU renderer the game's level meshes are drawn by the GPU from
vertex buffers made once from the game's own mesh data, not by its VU1
microprograms vertex by vertex: the rigid prelit meshes (render type 4),
the terrain with its grass shells (type 0x20), the sky (0x18) and the
skaters and other skinned characters (8 and 0xC: bones and lights in a
vertex shader) in every level, split screen included. The game itself runs
exactly as before (its memory and timing do not change); the meshes look as
they do through VU1, a little smoother where they cross the near plane.
Shadows, particles and 2D still go the PS2 way.

| settings.ini | Environment | What it does |
|---|---|---|
| `[graphics] render_path = native` (default) | `PS2X_RENDER_PATH=auto` | native meshes in each game's form: Aggressive Inline's packets substituted on the GPU renderer (strict on the CPU one); DM2's and BMX XXX's from the draw records, with either renderer (as `records` below; DM2's riders and bikes too) |
| `render_path = strict` | `PS2X_RENDER_PATH=strict` | every mesh through VIF1, VU1 and the GS as on the console (the logic checks' and PCSX2 comparisons' mode) |
| | `PS2X_RENDER_PATH=native` | the packet substitution, also with the CPU renderer (the same triangles sent to it as GS primitives); DM2's without the records (developers), BMX XXX's the records |
| `render_path = records` | `PS2X_RENDER_PATH=records` | native meshes made by the engine's class renderers from their draw records (zge/host/class_draws.cpp): no VU1 work in the mesh packet (its texture requests stay), with either renderer, in all three games (DM2's and BMX XXX's default); a draw it does not make goes through VU1 (`PS2X_TRACE_NATIVE=1` counts why) |

The game's files are read straight from the host (the game's mods over its
own, "Mods" below) by the engine's disc device's host form; the PS2's CD/DVD
client over the emulated IOP is still there:

| settings.ini | Environment | What it does |
|---|---|---|
| `[host] disc = native` (default) | `ZAXIS_DISC=native` | the game's files from the host, ZCDDVD.IRX not loaded |
| `disc = ps2` | `ZAXIS_DISC=ps2` (or `PS2X_DISC=cdvd`) | the game's CD/DVD client over the emulated IOP and ZCDDVD, as on the console |

The developer runner has a Display row (`set display.native_meshes 0/1`
on PS2X_CONTROL) to switch while running. Debugging:
`PS2X_NATIVE_CLASSES=4,0x20,0x18,8` (render types drawn natively; 8 or 0xC
for the skinned ones), `PS2X_NATIVE_LEVELS=zoo`,
`PS2X_NATIVE_TINT=1`, `PS2X_NATIVE_GPU=0`, `PS2X_TRACE_NATIVE=1`; design
in docs/NATIVE.md and `runtime/src/lib/gs/GPU_BACKEND.md` ("Native draws").

## Mods

The players' guide is docs/MODS.md. Mods are folders or zips (read in place, never unpacked: `runtime/ps2_zip_archive.h`,
miniz 3.0.2) in the collection's Mods folder, `<support>/Mods`, each with a `mod.ini` (`[mod]` id, name, version,
author, games, needs, conflicts); what a mod holds is told by its layout:

| In a mod's folder | What it replaces |
|---|---|
| `ASSETS/...`, `MOVIES/...`, any disc path (names compared without case) | that file of the game's disc, through the engine's file system (`runtime/ps2_game_files.h`; the game's own in-memory patches, such as the Settings menus' scripts, still apply on top) |
| `Music/NN.ogg` (`.wav`, `.flac`, `.mp3`) | song NN of the soundtrack (`zge_sound.cpp`) |
| `tuning.ini` | the tuning overrides (`docs/DEVTOOLS.md`) |
| `Textures/<game>/` (or the disc serial, or `Textures/` for a mod of one game) | the GPU renderer's replacement pictures (`PS2X_TEXTURE_PACKS`) |
| `[map]` in `mod.ini`, or `map.ini` | a custom map (Custom Freeplay, below), read by no other boot |

The loader (`runtime/include/collection/mods.h`, `collection/mods.cpp`) gives each game, as `PS2X_MODS_DIR` (the
platform's path-list separator between them, `;` on Windows and `:` elsewhere; the developer runner takes one folder
there as before), the game's own Mods folder (`<support>/games/<id>/Mods`; the single-game app's `<support>/Mods`),
then the mods it reads in the order, a later mod's file over an earlier one's: the mods of the game's own Mods folder
(the layout before manifests: each folder or zip there is a mod of that game, by its name; files loose there are read
under every mod; a folder named like one of the game's own top-level disc folders, or Music, is that loose layout's;
a folder with a `map.ini` is no mod), then the collection's that name the game. The mods' texture folders for the game
go as `PS2X_TEXTURE_PACKS` (lowest first, unset with none). With no mods the boot's values are as before. The boot's
log lists each mod (`[mods] ai: Mods/hd.zip 1.2 read`, or why not) and, where two have the same file, song, tuning.ini
or texture, which one's the game reads (`[mods] ai: Mods/b over Mods/a: 3 things (ASSETS/X.ZAL, ...)`); `[disc] files
from ..., mods from ...` lists the folders.

The collection's menu has a Mods page (Settings > Mods): every mod, numbered in the order, each with its state; a
mod's page has its switch in each game it is for, Read earlier and Read later, what it wins and loses, and why a game
does not read it. It lists the folders again each time it opens. A switch is kept in the game's `settings.ini` and
applies at the game's next start; the order in the collection's root `settings.ini`:

| settings.ini | What it does |
|---|---|
| `[mods] <key> = off` (the game's file) | the mod is not read by this game; the key is the manifest's id, or for a mod without one its folder's name as it is (the case kept; a name with `=` in it ends at the last one); `= on`, or no line, is on |
| `[mod order] <n> = <place>` (the root file) | the order, first read first, each mod by where it is (`Mods/hd.zip`, `games/ai/Mods/10-park`); mods not named come after, each game's own folder's first, then the collection's, by name. None: that order alone |

A mod is not read by a game while it is switched off there, while a mod it needs is missing or not read, while a mod
it conflicts with is read, or when its id is one a mod before it has (the page says which); a mod is read after the
mods it needs. The single-game app has no menu: its `settings.ini` takes the same `[mods]` lines.

## Custom Freeplay (Settings > Custom Freeplay)

A fourth tile on the collection's menu, off by default (`settings.ini`
`[general] custom_freeplay`, the app's, in the collection's root file; the
Settings page's Custom Freeplay row; with it off the menu, the boots and the
games are as before). The tile opens two pages in the menu's own look: the
player (the three games' tiles again: an inline skater from Aggressive Inline,
a BMX rider from Dave Mirra Freestyle BMX 2 or BMX XXX), then a custom map for
that game: the one list of every map, each startable by every player. Enter on
a map boots the player's game and starts it straight into free play on the
map: the game's own player select comes up first (its own
skaters or riders, as the game lists them), then the level loads with no more
menus. The design (docs/CUSTOM-LEVELS.md section 4): the player type picks the
game, so each game keeps its physics, tricks and camera; the mix is in the
menu and in the content.

A custom map is a mod in the collection's Mods folder, `<support>/Mods/<map>/` (a
folder or a zip), or a folder in the maps folder, `<support>/Maps/<map>/` (as
before mods had manifests): the level files of the game it was made in, laid
out as that game's slot has them (below), and a description, a `[map]` section
in its `mod.ini` or a `map.ini`. The map page lists both places, each map once.
No boot but its own reads a map: a folder with a `map.ini` in a game's Mods
folder is not a mod either, whatever its line in `[mods]` says.

```ini
[map]
name = Test Park        ; shown in the menu
game = ai               ; the game it was made in (ai, dm2, bmx): its level formats
slot = TUT              ; that game's slot its files are laid out in (below)
preview = preview.bmp   ; a picture in the folder, optional (a BMP of 4, 8, 24 or 32 bits)
author = ...            ; optional
npcs = none             ; none (the default) | slot, below
```

A map's people, riders and traffic are those its own files place (its level
objects, its scripts). Where a game keeps a level's NPCs outside the level's
folder, by the level's number (BMX XXX's NPC riders: the executable's records,
the rides in `assets/replay.zal`), a map leaves its slot's out, unless it says
`npcs = slot`: a map made from the slot's own level, which keeps its riders.

The map page lists the maps folder's maps, read again each time the page
opens. Each game lends its free play one slot (`collection/maps.h`
`mapSlots`); a map in the slot its own game lends starts for every player.
The player of the game it was made in reads its folder as it is; any other
player's boot gets it converted into its own game's level files for its slot
when it starts (a moment; the page says so while it converts), held in memory
for the boot and read over the mods: nothing is written, and the map's folder
is not read at that boot. A map that does not convert for a game (over the
game's budgets: too many parts drawn with no visibility zones, too many
texels, level files larger than the game's largest stock level's, which
would not fit its memory) is listed with the converter's reason; it never
starts. Each game's own start route (its devstart override) takes the
boot from the title to the game's own player select, waits for the player's
pick there, then picks the level and loads (`PS2X_DEV_START_LEVEL`,
`PS2X_DEV_START_SKATER=ask`, the boot's values):

- Aggressive Inline: `TUT`, the Tutorial's level 7 (`ASSETS/LEVELS/TUT`),
  reached through the Estate's free skate entry (the boot gets
  `PS2X_ESTATE=1` and `PS2X_DEV_START_LEVEL=ESTATE`;
  `aggressive_inline_devstart.cpp`; the app's bundled estate folder is what
  gives the entry).
- Dave Mirra Freestyle BMX 2: `WOODWARD`, Woodward Camp's level
  (`ASSETS/LEVELS/WOODWARD`, as `dm2_level_convert.py --level WOODWARD`
  writes a level), reached as FREERIDE's level select value 0
  (`dave_mirra_devstart.cpp`: FREERIDE, the rider and bike choices the
  player's, the modifiers, the level). The one row open from the start, and
  the one with the fewest extras of its own: a FREERIDE level has no NPCs,
  its gaps fire on its own faces' tags (a map's have none), it has no tree
  fade (Swampy Trails' and Galloon's rows have), and its far clip is the
  game's table's, 187 m. (The park slot also holds a park theme's editor
  objects: Woodward with them ran DM2's memory out.) Its level-point camera
  (the instructions, game over and after-replay screens) looks from the
  map's own camera points, not Woodward Camp's in the executable's table:
  the boot gets `PS2X_DM2_LEVEL_CAMERA_POINTS=1`, DM2's option for a level
  whose scene has its own "CameraPos" idle points (off: the table, as on
  the disc; a level without its own keeps the table either way).
- BMX XXX: `CHL_MULY`, The Bronx's level (`ASSETS/LEVELS/CHL_MULY`, the
  level select's first row), reached as a HARDCORE TOUR row
  (`bmx_xxx_devstart.cpp`: the rider select the player's, the row picked; the
  level is opened in the saved game in memory for the pick and the bits put
  back once the level has begun, so a save made after keeps the player's own
  progress). A challenge level has no clock: the ride lasts as long as the
  player likes. The map's `OBJCTV.TXT` gives it no goals, and as the game
  counts a level's goals into the player's record of it, the start keeps
  The Bronx's counts and puts them back on the screens a ride ends on (game
  over, where it saves) and in the menus (`PS2X_BMX_KEEP_LEVEL_GOALS`, which
  the boot gets). The Bronx's three NPC riders are the level number's, not
  its folder's: the boot gets `PS2X_BMX_LEVEL_NPCS=0` (`bmx_xxx_npcs.cpp`:
  the game sets up no NPC riders, as in multiplayer), or `1` with
  `npcs = slot` (a map made in BMX XXX, started by BMX XXX's player).

At a boot into a map the map's files are read last of the mods (they win),
the other maps are left out, and the mods are read as the Mods page has them. The custom map's
files are the player's: nothing is bundled with the app.

Developer runs: `PS2X_MENU_SCRIPT` drives the pages like the others (the
tile is the fourth, then Enter, the player's tile, Enter, the map's row,
Enter); `--game <id> --map <folder>` (or `PS2X_APP_GAME`, `PS2X_APP_MAP`)
starts the maps folder's map of that folder name for that game's player, no
menu first, converted as the map page does it; `PS2X_DEV_START_SKATER=<n>` in
the environment wins over the boot's `ask` for an unattended run;
`[app] custom freeplay: ...` in the log names the map, the game it was made
in, its slot, the start, the mods the boot was given and, for a converted
map, the conversion's time and the files held in memory. `ps2xMapConvert`
(a build tool) writes a conversion to a folder.

## Picture quality (GPU renderer)

Now that the meshes are drawn by the GPU, the PC version need not keep the
PS2's limits. Seven Display rows, each in settings.ini `[graphics]`, all on by
default; each applies at once. They change only how the GPU renderer draws
(the game itself runs exactly as before); the CPU renderer and the strict
render path (the logic checks' and PCSX2 comparisons' mode) ignore them.

| Row | settings.ini | Values (default first) | What it does |
|---|---|---|---|
| Anti-Aliasing (GPU) | `anti_aliasing` | fxaa, off | Smooths jagged edges of the 3D picture (FXAA, before the HUD is drawn, so text stays sharp); a window smaller than the picture is filled from a filtered half-size copy |
| Texture Filtering (GPU) | `texture_filtering` | 16x, 8x, 4x, 2x, off | Anisotropic filtering: ground and walls at a grazing angle stay sharp |
| Fog (GPU) | `fog` | smooth, original | Per-pixel fog instead of the PS2's eight fog bands |
| Shadows (GPU) | `shadows` | sharp, original | The skater's shadow drawn by the GPU at full resolution |
| Depth Precision (GPU) | `depth_precision` | high, original | Far surfaces stop flickering through each other (the PS2's 16-bit depth, one step 49 units at 2000 with Draw Distance Farthest) |
| Perspective Correction (GPU) | `perspective_correct` | true, false | Shading without the PS2's screen-space warping (and its jump in brightness where the ground meets the camera), skater and character textures without affine warping |
| Character Lighting (GPU) | `character_lighting` | xbox, ps2 | Skaters and people lit by the Xbox release's lights (brighter, from other angles: the Ape's fur shows as on the Xbox) instead of the PS2's |

Always on the GPU renderer, by design: surfaces crossing the near plane are drawn whole, where the PS2's clipping
program splits them and clamps their corners to the GS window, which bends their textures at the bottom of the screen
(Industrial's grating floor; the CPU renderer draws the PS2's triangles).

Environment (developer runner, tests): `PS2X_GS_AA`, `PS2X_GS_ANISO`,
`PS2X_NATIVE_FOG`, `PS2X_NATIVE_SHADOWS`, `PS2X_GS_PRECISE_DEPTH`,
`PS2X_NATIVE_PERSPECTIVE`, `PS2X_NATIVE_LIGHTING` (a value name or its index); on PS2X_CONTROL
`set display.anti_aliasing 0` and so on. Design:
`runtime/src/lib/gs/GPU_BACKEND.md` ("Picture quality beyond the PS2").

## Design (as planned)

### Pages

- Front end Options: the Controller item becomes **Controls** (the console
  pad layout and vibration are replaced by the keyboard/mouse/gamepad
  bindings) and a **Display** page is added; **Sound** gets Master Volume
  and Mute in Background rows. Pause menu: **Display** and **Controls**
  items next to Sound Options, the same pages.
- Rebinding is a guiMenu of actions whose texts a native fills ("Jump  SPACE,
  K"); selecting one makes the input layer capture the next key, mouse
  button or gamepad button (game input held back meanwhile); it is written
  to controls.ini.
- Skip intro movies: the front end's Movie.Start asks a native, which plays
  ReturnToMainMenu at once instead of StartAKAMovie (the same path the
  movies take when they end).
- Anything the menu engine cannot show (the frame rate counter) is drawn by
  the host over the picture; that is the only overlay.
