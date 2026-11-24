# Controls

In all three games, keyboard and mouse are mapped onto the DualShock 2 the game expects, through a plain config
file per game, and a gamepad keeps working alongside (below). This is all host-side (z-axis
`ps2/runtime/host_input.cpp`, `pad.cpp`); the game code is unchanged, except for one hook per game on
its follow camera for mouse-look (below). On Android the app expects a gamepad (the handheld's own or a connected
one); touches go to the bottom screen (the collection menu's there, or the game's panel).

## Default bindings

The table names Aggressive Inline's actions. Dave Mirra BMX 2 and BMX XXX use the same layout under their own action
names, with a few differences ("The config file" below).

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
| Restart (PC shortcut: the challenge, lesson, level, run or turn again) | Backspace; gamepad: L3+R3 (both sticks pressed in) |
| Hide HUD (PC shortcut, on and off) | F9 |
| Screenshot (PC shortcut, PNG to Pictures/Z-Axis Collection/<game>; Android: the Gallery, the same folder) | F12 |
| Photo mode (PC shortcut: the game stops, a free camera; again to leave) | P |
| Switch game (PC shortcut: back to the collection menu) | F8 |
| Developer menu (with Settings > Game > Developer Menu on: the game's debug menu or display, in a level) | F1; gamepad: Select+L3 |
| Previous / next track (PC shortcut: the soundtrack's song before or after the one playing, in a level) | F5 / F6; gamepad: L3+L1 / L3+R1 |
| Shuffle (PC shortcut, on and off; also Settings > Sound > Shuffle) | F7 |

The left stick also moves the menu cursors, so WASD works in menus too. I/J/K/L form the PS face-button diamond for
keyboard-only play.

Mouse cursor: a click in the window captures it (that click is not passed to the game). Escape (which also pauses)
or the backquote key releases it, and so does switching away from the window. While the window has no focus,
keyboard and mouse input is dropped (held keys are released); the gamepad keeps working.

## Rebinding in the game

The game's Settings screen (docs/SETTINGS.md "Where it is") has a Controls section: Button Prompts, Gamepad, Stick Dead
Zone, Connected Gamepads, Test Rumble, Swap Pads 1 and 2, Mouse Look (Free Look or Right Stick), Mouse Sensitivity,
Invert Mouse Y, a row per action with its keys (also Restart, Hide HUD, Screenshot and Photo Mode; Switch Game has no
row: change it in `controls.ini`), Gamepad Layout, a row per game button with its gamepad button, and Reset Controls.
L2 and R2 jump between the parts of a section: in Controls the top rows, KEYS and BUTTONS (in Gameplay CAMERA,
ASSISTS and CHEATS).

- Gamepad Layout sets every gamepad button at once: PS2 Original (each game button on its own gamepad button, the
  default) or Modern Triggers (L1 / R1 on the triggers, L2 / R2 on the shoulder buttons; Aggressive Inline's Xbox
  layout, so its Xbox Tutorial pictures of the controller are right). Buttons of your own show as Custom.

- Cross on an action row waits for a key or mouse button (Esc cancels, Backspace or Delete unbinds; a gamepad's
  Circle or Start cancels). It becomes the action's first binding and is taken off any other action.
- Cross on a "Pad:" row waits for a gamepad button and swaps it with the action that had it.

Nothing reaches the game while it waits. Every change is written to `controls.ini` at once, keeping the file's
comments and layout, and applies immediately.

## Button prompts and the mouse in menus

In all three games the on-screen prompts follow the device last used. A key, mouse button or clear mouse motion
shows keyboard and mouse prompts (the first key bound to each game button: Space for Jump/Cross, a mouse with its
right button lit for Grind/Triangle, ...). A gamepad button or a stick push shows that gamepad's (PlayStation,
Xbox-style or Nintendo, by SDL's gamepad type, following `[gamepad]` remaps: a remapped PlayStation pad shows the
picture of the button that presses each game button). Settings > Controls > Button Prompts (`[gamepad] prompts =
auto|keyboard|xbox|playstation|nintendo`) shows one style always, whatever is used: for Steam Input, which presents
every controller as an Xbox pad, or a PlayStation pad through a remapper. `ZX_PROMPTS=keyboard|playstation|xbox|
nintendo` fixes the prompts for testing.

In Aggressive Inline:

- While the game is not in play (front end, pause menu, tip and message boxes) the mouse wheel presses up/down, a
  left click presses Cross (select) and the right button is Triangle (back). Enter on the main menu selects; Escape
  there asks "Exit to Collection?" ("Quit to Desktop?" in a single-game app).
- The Tutorial's loading screens (the controller with arrows) follow the device too: with the keyboard they show the
  keys you bound (and the mouse) beside each action; with an Xbox-style or Nintendo pad a drawn pad of that kind,
  its arrows following `[gamepad]` remaps. A PlayStation pad keeps the PS2 picture. The Xbox release's own screens
  show its layout (Rotate on the triggers), which the default bindings do not use, so they appear only if
  `[gamepad]` puts Rotate on the triggers and Cess slide on the shoulders.
- Level Select, Exit Game and Quit to Desktop in the pause menus (also the Tutorial's and split screen's) ask "Are
  you sure?" first, as on the Xbox: No is preselected, Triangle (right click) goes back.

## Photo mode

Photo mode is in all three games: Photo Mode in Aggressive Inline's pause menus (Career, Freeskate, Timed Run, the
Tutorial's lesson list and split screen, where it moves player 1's view), the PHOTO MODE line in Dave Mirra BMX 2's
and BMX XXX's pause menus, or the photo key (P) anywhere in a level. It stops the game (it stays paused) and hides
everything on screen but the level; a free camera takes over and the help shows top left (not in the picture).
Leaving puts the game back as it was (from the pause menu: back to it; from the key: back to play).

| Photo mode | Keyboard and mouse | Gamepad |
|---|---|---|
| Look | mouse | right stick |
| Move | W / A / S / D | left stick |
| Up / down | Space / Ctrl | R2 / L2 (RT / LT, ZR / ZL) |
| Roll | Q / E | L1 / R1 |
| Level the horizon (the roll back to 0) | Q + E together | L1 + R1 together |
| Zoom (field of view 15-110) | mouse wheel, Z / X | d-pad up / down |
| Speed | - / =, hold Shift (faster) or Alt (slower) | d-pad left / right, hold R3 (faster) |
| Filter: None, Letterbox (2.39:1), Vignette, Black & White, Cinema (letterbox and vignette), Noir (all three) | F | Triangle (Y, X) |
| Reset the camera | R | Select (Back, -) |
| Rule-of-thirds grid on or off (over the window only: never in a saved picture) | G | L2 + R2 together |
| Next Frame: one frame of play, then paused again (a trick's apex) | . (period) | L3 |
| Save a picture (full resolution, with the filter) | the Screenshot key (F12) | Cross (A, B) |
| Hide or show the help | H | Square (X, Y) |
| Back | Esc, the photo key | Circle (B, A), Start |

The camera stays within 40 units of where it started. The grid stays on from one photo mode to the next, as the
filter does; with the letterbox it marks the thirds between the bars.

On Android the help and the grid do not show: the app has no overlay for notices yet (it comes in 1.4). The
controls work the same.

Window keys (not game buttons): Alt+Enter toggles full screen everywhere, F11 too on Windows (Cmd+Ctrl+F and Cmd+F are
the macOS menu shortcuts); Cmd+, opens the settings on macOS (on Windows: Settings... in the window's system menu,
Alt+Space then T). F1 shows the debug panel (with Settings > Game > Developer Menu on, the same press opens the game's
own debug menu or display too: the developer_menu shortcut, Select+L3 on a gamepad). Do not bind these to game
buttons. Android has none of the window keys: the app is always full screen. Where there is no F1 key (a handheld),
Settings > Controls > Gamepad Test (with the Developer Menu on) shows the debug panel at its PAD tab, what the game
reads from each pad; the same row, F1 or the window's close button hides it.

The bottom screen on one screen: in a level, M (L3+R2 on a gamepad; the bottom_screen shortcut) shows a
dual-screen handheld's bottom screen, the level's map with the goals and the tricks, in the window's bottom right
corner over the game, and hides it again. A click, a drag or the wheel over it (a finger on a touch screen) works it
as a touch works the handheld's; the game gets none of them and plays on. L3 does nothing in the three games; the R2
of the chord reaches the game as R2 alone would, as the other L3 chords' shoulder buttons do. On a handheld whose
second screen shows the bottom screen already (the Thor), the shortcut does nothing.

## Watching a recording

Settings > Recordings plays one of your recordings back (docs/SETTINGS.md). The game plays exactly as you did, so its
own menus do not take your input while it runs; the app's do:

| | Keyboard | Gamepad | Bottom screen (Android) |
|---|---|---|---|
| Pause | Space, Escape | Start | Pause, at the foot of the bottom screen |
| While paused: go on | Space, Enter | Start, Cross | Resume |
| While paused: leave | Escape | Circle | Leave |
| Leave at any time | F8 (Switch Game) | | Leave |

The line at the top left shows where it is ("Playback 1:23 / 5:00") for a few seconds after it starts or resumes,
and all the time while paused; on Android the bottom screen's strip shows it always. At its end, or when you leave,
the Recordings page is back, on the recording you watched.

## The config file

Each game has its own controls.ini, with its own defaults (its keys on that game's actions, named as the game names
them) and its own action names on the Controls rows (`ps2/runtime/game_controls.cpp`): rebinding one game
leaves the others alone. The defaults keep one layout for the whole collection (Cross on Space, Square and Triangle
on the mouse buttons, WASD on the left stick) and the same PC shortcuts. Dave Mirra BMX 2 and BMX XXX get their
Normal / Configuration A captions (Start / Jump, Trick, Grind / Stall, Modifier, Spin / Slide, ...). Where they
differ from Aggressive Inline's:

- Dave Mirra BMX 2: the right stick has no keys (the game has no camera stick), and the d-pad is Pedal, Brake and
  Steer.
- BMX XXX: R3 is the middle mouse button and V (Camera), there is no recentre key, the right stick keys are the park
  editor's camera zoom, and on Android mouse-look is off by default (`mode = stick`, `motion = none`: Settings >
  Controls > Mouse Look > Free Look turns it on). On the Mac and Windows it is on, as in the other two games; a BMX
  XXX `controls.ini` written by 1.2.0 keeps its `mode = stick` until you change it.

Where the file is:

- The collection: `<support>/games/<id>/controls.ini` (`ai`, `dm2`, `bmx`); your `gamecontrollerdb.txt` stays at
  `<support>` for every game.
- The runner and a single-game app, macOS: `~/Library/Application Support/<game>/controls.ini` (Windows
  `%APPDATA%\<game>\controls.ini`), `<game>` the game's title.
- `ZX_CONTROLS_INI=<file>` overrides the path (automated runs use this, so they never touch the real file);
  `ZX_GAMEPAD_MAPPINGS=<file>` the gamepad mappings'.

It is written with the game's defaults on first run, and the Settings screen writes its changes into it. Edit it by
hand while the game is not running. A line you delete keeps its default; an empty value (`l3 =`) unbinds. A missing
line or section gets its default, and Reset Controls in the Settings screen puts every line back. Bad names and
values are reported in the log (`[input] ...`) and skipped.

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
auto_recentre = 0         # seconds without mouse motion, then back behind the player; 0 = never
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
restart    = BACKSPACE    # AI: Career the challenge again, Tutorial the lesson, Freeskate the level, Timed Run the run
hide_hud   = F9
screenshot = F12
photo_mode = P
switch_game = F8         # back to the collection menu
developer_menu = F1      # with the Developer Menu on: the game's debug menu (AI) or debug display (DM2, BMX XXX)
previous_track = F5      # the soundtrack's song before or after the one playing (in a level)
next_track     = F6
shuffle        = F7      # the songs in a random order, on and off (Settings > Sound > Shuffle)
bottom_screen = M        # the bottom screen (map, goals, tricks) over the game on one screen, shown and hidden
restart_pad    = L3+R3    # gamepad buttons held together (by position, joined with +)
hide_hud_pad   =
screenshot_pad =
photo_mode_pad =
switch_game_pad =
developer_menu_pad = SELECT+L3
previous_track_pad = L3+L1  # L3 is no game's button; the shoulder button reaches the game too
next_track_pad     = L3+R1
shuffle_pad        =
bottom_screen_pad = L3+R2
```

Binding a shortcut's key in the Settings screen takes it off the game controls and the other way round.

Gamepad button names (by position): `SOUTH`, `EAST`, `WEST`, `NORTH`, `L1`, `R1`, `L2`, `R2`, `START`, `SELECT`
(Back), `L3`, `R3`, `DPAD_UP`, `DPAD_DOWN`, `DPAD_LEFT`, `DPAD_RIGHT`; aliases `A B X Y LB RB LT RT BACK` (Xbox
names: A = south) and `CROSS CIRCLE SQUARE TRIANGLE`.

Key names: `A`-`Z`, `0`-`9`, `F1`-`F12`, `SPACE`, `ENTER`, `ESCAPE`, `TAB`, `BACKSPACE`, arrows `UP DOWN LEFT
RIGHT`, `LEFT_SHIFT`, `RIGHT_SHIFT`, `LEFT_CONTROL`, `RIGHT_CONTROL`, `LEFT_ALT` (alias `OPTION`), `RIGHT_ALT`,
`LEFT_SUPER` (alias `CMD`), `GRAVE`, `MINUS`, `EQUAL`, `LEFT_BRACKET`, `RIGHT_BRACKET`, `BACKSLASH`, `SEMICOLON`,
`APOSTROPHE`, `COMMA`, `PERIOD`, `SLASH`, `KP_0`-`KP_9` and the other `KP_` keys, `INSERT`, `DELETE`, `HOME`,
`END`, `PAGE_UP`, `PAGE_DOWN`, `CAPS_LOCK`; mouse `MOUSE_LEFT`, `MOUSE_RIGHT`, `MOUSE_MIDDLE`, `MOUSE_SIDE`,
`MOUSE_EXTRA`. Case does not matter.

## Gamepad

Every connected gamepad SDL3 sees (its gamepad API: Xbox, PlayStation, Switch and most other controllers) is used.
The first one connected is player 1, together with keyboard and mouse; the second is player 2 in split screen, with
its own buttons, sticks and vibration (the same `[gamepad]` remaps and stick threshold). With one pad (or none) the
second port is empty, as on a PS2 with one controller: the games then hide or grey their multiplayer (Aggressive
Inline skips the MULTIPLAYER icon, Dave Mirra BMX 2 greys Multiplayer and Controller 2, BMX XXX greys MULTIPLAYER); plug a second pad in for
split screen.

A pad keeps its player while connected. If a player's pad goes away, a spare pad takes its place, or the pads after
it move up, so the last remaining pad is always player 1; a pad plugged in later becomes the next player. Player
lights show the number. Settings > Controls has Connected Gamepads (the pads in player order; Cross writes their
details to the log), Test Rumble (each pad's large motor, then its small one, through the game's vibration path) and
Swap Pads 1 and 2.

Extra controller mappings: a `gamecontrollerdb.txt` (the community format) next to controls.ini or next to the game;
SDL also reads `SDL_GAMECONTROLLERCONFIG` and `SDL_GAMECONTROLLERCONFIG_FILE`. Each pad reads: face buttons (bottom
= Cross, right = Circle, left = Square, top = Triangle), shoulders, triggers (past 10 % = L2/R2), sticks,
Start/Select (Back), L3/R3. It keeps working while the window is in the background. Its buttons add to the
keyboard's (at full pressure); a gamepad stick pushed past `stick_threshold` replaces the keyboard/mouse value for
that stick. So keyboard, mouse and gamepad can be mixed.

## The sticks and the camera

The game reads each stick with a dead zone of about 0.26 of a full push (hence the mouse `deadzone`). In Aggressive
Inline the left stick is the same as the d-pad for skating and moves the menu cursors; skating speed is not analog
(a half push accelerates like a full one). Its right stick is a look-around, not a free camera: its position swings
the follow camera up to 60 degrees to the side and 40 up or down, and the camera eases back behind the skater when
it is let go.

With `mode = stick` the mouse drives a stick instead (`motion`: the right stick, the left one, or none). In
Aggressive Inline, in `offset` mode, moving the mouse pushes the right stick like a thumb (200 px = full, so 60
degrees), keeping the mouse moving holds it, and it springs back when the mouse stops (about half a second with
`decay = 0.85`, plus the game's own easing). It works as a quick glance, not as free mouse-look; with `decay = 1`
the glance stays until the mouse moves back. Mouse right swings the camera to the skater's right, so the view turns
left (that is what the game's stick does). Dave Mirra BMX 2 has no camera stick, so stick mode leaves its camera to
the game; in BMX XXX the right stick is the park editor's zoom.

## Mouse-look (`mode = look`, the default in all three games, but BMX XXX's on Android)

All three games have mouse-look. Moving the mouse turns the view around the player, like a PC third-person game:
all the way round left and right, and up and down within `look_up_limit` / `look_down_limit` of horizontal (15 up,
75 down). The game's camera stays clear of the ground and walls: in Aggressive Inline looking up brings the camera
down towards the floor, and there it moves in closer instead of into the floor; in Dave Mirra BMX 2, near the
ground, looking up goes only a little way. 0.2 degrees per pixel by default, so 900 px turn the view right round to
the front. The view follows the mouse with no lag, and it stays where you put it: with the mouse still, the camera
keeps its angle to the player. A recentre key (R or the middle mouse button; BMX XXX has none) puts it back behind
the player, with the game's own easing; `auto_recentre = <seconds>` does that on its own after that long without
mouse motion (off by default).

The angle is relative to the player, not to the world: it is kept relative to the direction the game's camera
wants, the player's direction of motion. Ride through a turn with the view at 90 degrees and it stays at the
player's side. The game's own follow behaviour (lagging a little behind fast turns, rising with speed, pulled in
front of walls) stays as it is, with the mouse angle on top.

- Riding, grinds, manuals, jumps: the follow camera with the mouse angle (on a rail too).
- The game's other cameras (scripted ones such as Aggressive Inline's Movie Lot fly-through, Tutorial lesson
  intros, level start and loading; BMX XXX's rider's eyes, first-person view, bail, view from above and swings): the
  game's camera runs unchanged, mouse motion is dropped and the view comes back behind the player.
- Pause menu and the other in-level menus (in Aggressive Inline also tip boxes, hints, the save dialog): mouse
  motion is dropped, and the view keeps its angle when play resumes.
- In Dave Mirra BMX 2 and BMX XXX only player 1's camera turns: keyboard and mouse are port 0. Aggressive Inline's
  mouse-look picks no player: it applies to each player's follow camera alike.
- Aggressive Inline with a gamepad: pushing the right stick hands the camera back to the game (its own 60/40 degree
  look-around) and recentres the mouse angle. Keys bound to `right_*` stick directions only work while the mouse
  angle is centred.

With no mouse motion the game runs bit for bit as without mouse-look, in all three games. In Aggressive Inline mouse
motion does not change play either: the game decides what is in view by a hidden follow camera without it, and only
the picture follows the mouse (docs/ENHANCEMENTS.md). `ZX_MOUSELOOK=0` leaves the camera alone. How it is done:
`games/aggressive/overrides/aggressive_inline_slus20327.cpp`, `dave_mirra_mouselook.cpp`, `bmx_xxx_mouselook.cpp`;
in Aggressive Inline the game's camera reads the stick in its follow camera's update each frame
(`games/aggressive/camera.h`, `updateFollowCamera`). Scripted host input for tests: docs/DEVTOOLS.md
("Test runs").
