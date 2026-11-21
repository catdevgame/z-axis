# Controls

Keyboard and mouse are mapped onto the DualShock 2 each game expects, through a
plain config file per game. A gamepad keeps working alongside (see below). All of this
is host-side (z-axis `runtime/src/lib/ps2_host_input.cpp`, `ps2_pad.cpp`); the game
code is unchanged, except for one hook per game on its follow camera that
does mouse-look (below). On Android the app expects a gamepad (the handheld's own or a connected one); touches go
to the bottom screen (the collection menu's there, or the game's panel).

## Default bindings

Aggressive Inline's; Dave Mirra BMX 2's and BMX XXX's are the same layout under their own action names (below).

| Game action (PS2 button) | Keyboard / mouse |
|---|---|
| Forward / Left / Brake / Right (left stick) | W / A / S / D |
| Same, half push (gentler steering) | hold Left Alt (Option) with W/A/S/D |
| Camera: mouse-look | mouse motion (cursor captured) |
| Camera back behind the skater | R, middle mouse button |
| Camera look-around (right stick; zoom in the park editor) | keypad 8 / 2 / 4 / 6, Page Up / Page Down for up / down |
| Park editor camera zoom | mouse wheel (or the right stick keys up / down) |
| Jump / Accelerate (Cross) | Space, K |
| Grabs and Tricks (Square) | left mouse button, J |
| Grind (Triangle) | right mouse button, I |
| Action: talk, vault, skitch (Circle) | F, L |
| Rotate left / right (L1 / R1) | Q / E |
| Cess slide left / right (L2 / R2) | Z / C |
| Pause (Start) | Escape, Enter |
| Challenges (Select) | Tab |
| D-pad (menus; in a level same as the left stick) | arrow keys |
| Restart (PC shortcut: the challenge, lesson, level or run again) | Backspace; gamepad: L3+R3 (both sticks pressed in) |
| Hide HUD (PC shortcut, on and off) | F9 |
| Screenshot (PC shortcut, PNG to Pictures/Z-Axis Collection/<game>) | F12 |
| Photo mode (PC shortcut: the game stops, a free camera; again to leave) | P |
| Switch game (PC shortcut: back to the collection menu) | F8 |

The left stick also moves the menu cursors, so WASD works in menus too.
I/J/K/L form the PS face-button diamond for keyboard-only play.

Mouse cursor: a click in the window captures it (that click is not passed to
the game). Escape (which also pauses) or the backquote key releases it, and so
does switching away from the window. While the window has no focus, keyboard
and mouse input is dropped (held keys are released); the gamepad keeps
working.

## Rebinding in the game

The game's Settings screen (SETTINGS.md "Where it is") has a Controls section: Button Prompts, Gamepad,
Stick Dead Zone, Connected Gamepads, Test Rumble, Swap Pads 1 and 2, Mouse Look (Free Look or Right Stick),
Mouse Sensitivity, Invert Mouse Y, a row per action with its keys (also Restart, Hide HUD, Screenshot and Photo
Mode; Switch Game has no row: change it in `controls.ini`), a row per game button with its gamepad button, and
Reset Controls. Cross on an action row waits for a key or
mouse button (Esc cancels, Backspace or Delete unbinds; a gamepad's Circle
or Start cancels): it becomes the action's first binding and is taken off
any other action. Cross on a "Pad:" row waits for a gamepad button and
swaps it with the action that had it. Nothing reaches the game while it
waits. Every change is written to `controls.ini` at once, keeping the
file's comments and layout, and applies immediately.

## Button prompts and the mouse in menus

On-screen prompts follow the device last used: a key, mouse button or
clear mouse motion shows keyboard and mouse prompts (the first key bound
to each game button: Space for Jump/Cross, a mouse with its right button
lit for Grind/Triangle, ...); a gamepad button or a stick push shows that
gamepad's (PlayStation, Xbox-style or Nintendo, by SDL's gamepad type,
following `[gamepad]` remaps: a remapped PlayStation pad shows the
picture of the button that presses each game button). While the game is not in play (front end,
pause menu, tip and message boxes) the mouse wheel presses up/down, a left
click presses Cross (select) and the right button is Triangle (back) as
usual. Enter on the main menu selects; Escape there asks "Quit to
Desktop?" (in the collection: "Exit to Collection?"). `ZX_PROMPTS=keyboard|playstation|xbox|nintendo` fixes the
prompts (testing). Settings > Controls > Button Prompts (`[gamepad]
prompts = auto|keyboard|xbox|playstation|nintendo`) shows one style always,
whatever is used: for Steam Input, which presents every controller as an
Xbox pad, or a PlayStation pad through a remapper. `SETTINGS.md`
has the details.

The Tutorial's loading screens (the controller with arrows) follow the
device too: with the keyboard they show the keys you bound (and the mouse)
beside each action; with an Xbox-style or Nintendo pad a drawn pad of that
kind, its arrows following `[gamepad]` remaps. The Xbox release's own
screens show its old layout (Rotate on the triggers), which the port's
default bindings do not use, so they only appear if `[gamepad]` puts
Rotate on the triggers and Cess slide on the shoulders. A PlayStation pad
keeps the PS2 picture.

Level Select, Exit Game and Quit to Desktop in the pause menus (also the
Tutorial's and split screen's) ask "Are you sure?" first, as on the Xbox:
No is preselected, Triangle (right click) goes back.

## Photo mode

Photo Mode in the pause menu (Aggressive Inline's Career, Freeskate, Timed Run, the
Tutorial's lesson list and split screen, where it moves player 1's view; the
PHOTO MODE line in Dave Mirra BMX 2's and BMX XXX's), or the photo
key (P) anywhere in a level, stops the game (it stays paused) and hides
everything on screen but the level; a free camera takes over and the help
shows top left (not in the picture). Leaving puts the game back as it was
(from the pause menu: back to it; from the key: back to play).

| Photo mode | Keyboard and mouse | Gamepad |
|---|---|---|
| Look | mouse | right stick |
| Move | W / A / S / D | left stick |
| Up / down | Space / Ctrl | R2 / L2 (RT / LT, ZR / ZL) |
| Roll | Q / E | L1 / R1 |
| Zoom (field of view 15-110) | mouse wheel, Z / X | d-pad up / down |
| Speed | - / =, hold Shift (faster) or Alt (slower) | d-pad left / right, hold R3 (faster) |
| Filter: None, Letterbox, Vignette, Black & White, Cinema, Noir | F | Triangle (Y, X) |
| Reset the camera | R | Select (Back, -) |
| Save a picture (full resolution, with the filter) | the Screenshot key (F12) | Cross (A, B) |
| Hide or show the help | H | Square (X, Y) |
| Back | Esc, the photo key | Circle (B, A), Start |

The camera stays within about 40 m of where it started (the game only
keeps the part of the level near you).

Window keys (not game buttons): Alt+Enter toggles full screen everywhere,
F11 too on Windows (Cmd+Ctrl+F and Cmd+F are the macOS menu shortcuts);
Cmd+, opens the settings on macOS (on Windows: the game's own Settings
item). F1 shows the debug panel on Mac and Windows (with Settings > Game > Developer Menu on, the same press
opens the game's own debug menu too). Do not bind these to game buttons. Android has none of these: the app is
always full screen.

## Watching a recording

Settings > Recordings plays one of your recordings back (SETTINGS.md). The game plays exactly as you did, so
its own menus do not take your input while it runs; the app's do:

| | Keyboard | Gamepad | Second screen (Thor) |
|---|---|---|---|
| Pause | Space, Escape | Start | Pause, at the foot of the bottom screen |
| While paused: go on | Space, Enter | Start, Cross | Resume |
| While paused: leave | Escape | Circle | Leave |
| Leave at any time | F8 (Switch Game) | | Leave |

The line at the top left shows where it is ("Playback 1:23 / 5:00") for a few seconds after it starts or
resumes, and all the time while paused; the Thor's strip shows it always. At its end, or when you
leave, the Recordings page is back, on the recording you watched.

## The config file

Each game has its own controls.ini, with its own defaults (its keys on
that game's actions, named as the game names them) and its own action
names on the Controls rows (`runtime/src/lib/ps2_game_controls.cpp`): rebinding one
game leaves the others alone. The defaults keep one layout for the whole
collection (Cross on Space, Square and Triangle on the mouse buttons, WASD
on the left stick); Dave Mirra BMX 2 and BMX XXX get their Normal /
Configuration A captions (Start / Jump, Trick, Grind / Stall, Modifier,
Spin / Slide, ...) and the same PC shortcuts. Where they differ from Aggressive Inline's: Dave Mirra BMX 2's
right stick has no keys and its d-pad is Pedal, Brake and Steer; BMX XXX's R3 is the middle mouse button and V
(Camera), it has no recentre key, and its mouse-look is off by default (`mode = stick`, `motion = none`: Settings > Controls > Mouse Look >
Free Look turns it on).

- The collection: `<support>/games/<id>/controls.ini` (`ai`, `dm2`, `bmx`);
  the user's `gamecontrollerdb.txt` stays at `<support>` for every game. A
  shared `<support>/controls.ini` of an earlier version became Aggressive
  Inline's once; the other games started from their own defaults.
- The runner and a single-game app, macOS:
  `~/Library/Application Support/<game>/controls.ini` (Windows
  `%APPDATA%\<game>\controls.ini`), `<game>` the game's title.
- `ZX_CONTROLS_INI=<file>` overrides the path (automated runs use this, so
  they never touch the real file); `ZX_GAMEPAD_MAPPINGS=<file>` the
  gamepad mappings'.

It is written with the game's documented defaults on first run; the Settings
screen writes its changes into it; edit it by hand while the game is not
running.
A file written before mouse-look (with `mode = offset`) gets mouse-look:
`offset` / `velocity` are now `stick_mode` values; set `mode = stick` for
the old behaviour.
A line you delete keeps its default, an empty value (`l3 =`) unbinds.
A file written before the right stick had default keys has `right_up =`
... `right_right =` empty, so they stay unbound: delete those four lines
(or Reset Controls in the Settings screen) to get them. Bad
names and values are reported in the log (`[input] ...`) and skipped.

```ini
[buttons]          # cross square triangle circle l1 r1 l2 r2 start select up down left right l3 r3
cross  = SPACE, K
r2     = C*0.5     # NAME*v: pressure v (DualShock 2 pressure buttons; default full)

[sticks]           # left_up left_down left_left left_right right_up ... right_right
left_up = W        # NAME*v: deflection v
modifier = LEFT_ALT
modifier_amount = 0.5

[mouse]
mode = look               # look (mouse-look) | stick (the mouse pushes a stick)
look_speed = 0.2          # degrees per pixel (times sensitivity_x / _y)
look_up_limit = 15        # how far the view may look up / down from horizontal
look_down_limit = 75
recentre = R, MOUSE_MIDDLE
auto_recentre = 0         # seconds without mouse motion, then back behind the skater; 0 = never
look_snap = true          # the camera follows the mouse at once (false: with the game's easing)
sensitivity_x = 1.0       # look: times look_speed; stick: 1.0 = 200 px for a full push
sensitivity_y = 1.0
invert_x = false          # look: mouse right turns the view right
invert_y = true           # mouse up looks up (in both modes)
motion = right_stick      # stick mode: right_stick | left_stick | none
stick_mode = offset       # stick mode: offset | velocity
decay = 0.85              # stick mode: push kept per frame once the mouse stops (0 = recentre at once, 1 = never)
deadzone = 0.27           # stick mode: smallest push sent (the game ignores less than ~0.26)
capture = click           # click | always | never
release = ESCAPE, GRAVE

[gamepad]
enabled = true
stick_threshold = 0.2     # a gamepad stick pushed past this wins over keys/mouse (and below it is ignored)
cross = SOUTH             # gamepad buttons per game button (default: each its own)
prompts = auto            # auto | keyboard | xbox | playstation | nintendo

[hotkeys]                 # PC shortcuts, not game buttons
restart    = BACKSPACE    # Career: the challenge again; Tutorial: the lesson; Freeskate: the level; Timed Run: the run
hide_hud   = F9
screenshot = F12
photo_mode = P
switch_game = F8         # back to the collection menu
restart_pad    = L3+R3    # gamepad buttons held together (by position, joined with +)
hide_hud_pad   =
screenshot_pad =
photo_mode_pad =
switch_game_pad =
```

A file written before these existed gets them with their defaults (a
missing line keeps its default). Binding a shortcut's key in the Settings
screen takes it off the game controls and the other way round.

Gamepad button names (by position): `SOUTH`, `EAST`, `WEST`, `NORTH`, `L1`,
`R1`, `L2`, `R2`, `START`, `SELECT` (Back), `L3`, `R3`, `DPAD_UP`,
`DPAD_DOWN`, `DPAD_LEFT`, `DPAD_RIGHT`; aliases `A B X Y LB RB LT RT BACK`
(Xbox names: A = south) and `CROSS CIRCLE SQUARE TRIANGLE`.

Key names: `A`-`Z`, `0`-`9`, `F1`-`F12`, `SPACE`, `ENTER`, `ESCAPE`, `TAB`,
`BACKSPACE`, arrows `UP DOWN LEFT RIGHT`, `LEFT_SHIFT`, `RIGHT_SHIFT`,
`LEFT_CONTROL`, `RIGHT_CONTROL`, `LEFT_ALT` (alias `OPTION`), `RIGHT_ALT`,
`LEFT_SUPER` (alias `CMD`), `GRAVE`, `MINUS`, `EQUAL`, `LEFT_BRACKET`,
`RIGHT_BRACKET`, `BACKSLASH`, `SEMICOLON`, `APOSTROPHE`, `COMMA`, `PERIOD`,
`SLASH`, `KP_0`-`KP_9` and the other `KP_` keys, `INSERT`, `DELETE`, `HOME`,
`END`, `PAGE_UP`, `PAGE_DOWN`, `CAPS_LOCK`; mouse `MOUSE_LEFT`, `MOUSE_RIGHT`,
`MOUSE_MIDDLE`, `MOUSE_SIDE`, `MOUSE_EXTRA`. Case does not matter.

## Gamepad

Every connected gamepad SDL3 sees (its gamepad API: Xbox, PlayStation,
Switch and most other controllers) is used. The first one connected is
player 1, together with keyboard and mouse; the second is player 2 in split
screen, with its own buttons, sticks and vibration (the same `[gamepad]`
remaps and stick threshold). With one pad (or none) the second port is
empty, as on a PS2 with one controller: the games then hide or grey their
multiplayer (Aggressive Inline skips the MULTIPLAYER icon, Dave Mirra BMX
2 greys Multiplayer and Controller 2); plug a second pad in for split
screen. A pad keeps its player while connected; if a player's pad goes
away, a spare pad takes its place, or the pads after it move up, so the
last remaining pad is always player 1; a pad plugged in later becomes the
next player. Player lights show the number. Settings > Controls has
Connected Gamepads (the pads in player order; Cross writes their details to the log),
Test Rumble (each pad's large motor, then its small one, through the
game's vibration path) and Swap Pads 1 and 2. Extra controller mappings: a
`gamecontrollerdb.txt` (the community format) next to controls.ini or next
to the game; SDL also reads `SDL_GAMECONTROLLERCONFIG` and
`SDL_GAMECONTROLLERCONFIG_FILE`. Each pad read: face buttons (bottom = Cross,
right = Circle, left = Square, top = Triangle), shoulders, triggers (past
10 % = L2/R2), sticks, Start/Select (Back), L3/R3. It keeps working while
the window is in the background. Its buttons add to
the keyboard's (at full pressure); a gamepad stick pushed past
`stick_threshold` replaces the keyboard/mouse value for that stick. So
keyboard, mouse and gamepad can be mixed.

## The sticks and the camera

The game reads each stick with a dead zone of about 0.26 of a full push
(hence the mouse `deadzone`). The left stick is the same as the d-pad for
skating and moves the menu cursors; skating speed is not analog (a half
push accelerates like a full one). The right stick is a look-around, not a
free camera: its position swings the follow camera up to 60 degrees to the
side and 40 up or down, and the camera eases back behind the skater when
it is let go.

With `mode = stick` the mouse drives the right stick in `offset` mode:
moving the mouse pushes the stick like a thumb (200 px = full, so 60
degrees), keeping the mouse moving holds it, and it springs back when the
mouse stops (about half a second with `decay = 0.85`, plus the game's own
easing). It works as a quick glance, not as free mouse-look; with
`decay = 1` the glance stays until the mouse moves back. In this mode
mouse right swings the camera to the skater's right, so the view turns
left (that is what the game's stick does).

## Mouse-look (`mode = look`, the default in AI and DM2)

Moving the mouse turns the view around the skater, like a PC third-person
game: all the way round left and right (no 60 degree limit), up and down
within `look_up_limit` / `look_down_limit` of horizontal (15 up, 75 down).
Looking up brings the camera down towards the floor; there it moves in
closer to the skater instead of into the floor.
0.2 degrees per pixel by default, so 900 px turn the view right round to
the front. The view follows the mouse with no lag, and it stays where you
put it: with the mouse still, the camera keeps its angle to the skater. R
or the middle mouse button puts it back behind the skater (the game swings
it there with its own easing); `auto_recentre = <seconds>` does that on its
own after that long without mouse motion (off by default).

The angle is relative to the skater, not to the world: it is kept
relative to the direction the game's camera wants, the skater's direction
of motion. Skate through a turn with the view at 90 degrees and it stays at
the skater's side. The game's own follow behaviour (lagging a little behind
fast turns, rising with speed, pulled in front of walls) stays as it is,
with the mouse angle on top.

- Normal skating, grinds, manuals, jumps: the follow camera with the mouse
  angle (on a rail too).
- The game's special camera states (where the right stick does nothing in
  the original either): the game's camera runs unchanged and mouse motion
  is dropped; after one that lasts more than about half a second the view
  comes back behind the skater.
- Scripted cameras (the Movie Lot fly-through, Tutorial lesson intros,
  level start, loading): the mouse cannot fight them; motion is dropped and
  the view is put back behind the skater.
- Pause menu, tip boxes, hints, the save dialog: mouse motion is dropped,
  and the view keeps its angle when play resumes.
- Gamepad: pushing the gamepad's right stick hands the camera back to the
  game (its own 60/40 degree look-around) and recentres the mouse angle.
  Keys bound to `right_*` stick directions only work while the mouse angle
  is centred.

With no mouse motion the game runs bit for bit as without mouse-look
(logiccheck). In Aggressive Inline mouse motion does not change play either:
the game decides what is in view by a hidden follow camera without it, and
only the picture follows the mouse (docs/ENHANCEMENTS.md). How it is done: `runtime/src/game_overrides/aggressive_inline_slus20327.cpp`
(`dave_mirra_mouselook.cpp`, `bmx_xxx_mouselook.cpp` the same way in the other games); where the
game's camera reads the stick: its follow camera's update each frame (`runtime/src/game/aggressive/camera.h`,
`updateFollowCamera`); scripted host input for
tests: docs/DEVTOOLS.md ("Test runs").
