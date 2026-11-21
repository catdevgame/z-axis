// Each game's controls (runtime/ps2_host_input.h, GameControls): the
// documented controls.ini written on its first run, with the keys put where
// that game's own actions are, and the names of those actions on the
// Controls rows of the settings pages.
//
// The parts every game shares (key names, pressure, the stick modifier, the
// mouse's tuning, gamepads) are written once here; a game gives its title,
// its [buttons] and stick lines with what they do, what the mouse does with
// its camera, and the PC shortcuts it acts on.
#include "runtime/ps2_host_input.h"

#include <string>
#include <vector>

namespace ps2_host_input
{
    namespace
    {
        struct ControlsText
        {
            std::string title;
            // How to rebind: comment lines.
            std::string rebind;
            // [buttons]: comment and binding lines.
            std::string buttons;
            // [sticks] up to the modifier: comment and binding lines.
            std::string sticks;
            // [mouse] mode: comment, then "mode = ...".
            std::string mouseMode;
            // Who the camera follows ("skater", "rider").
            std::string character;
            // [mouse] recentre keys and stick-mode motion.
            std::string recentre = "R, MOUSE_MIDDLE";
            std::string motion = "right_stick";
            // [hotkeys]: comment and binding lines (keys, then *_pad chords).
            std::string hotkeys;
        };

        std::string compose(const ControlsText &t)
        {
            const std::string &who = t.character;
            return "# " + t.title + " controls (keyboard and mouse).\n"
                   "#\n" +
                   t.rebind +
                   "#\n"
                   "# Bindings are comma-separated key names. Keys: A-Z, 0-9, F1-F12, SPACE,\n"
                   "# ENTER, ESCAPE, TAB, BACKSPACE, UP, DOWN, LEFT, RIGHT, LEFT_SHIFT,\n"
                   "# RIGHT_SHIFT, LEFT_CONTROL, RIGHT_CONTROL, LEFT_ALT (Option), RIGHT_ALT,\n"
                   "# LEFT_SUPER (Command), GRAVE (`), MINUS, EQUAL, LEFT_BRACKET, RIGHT_BRACKET,\n"
                   "# BACKSLASH, SEMICOLON, APOSTROPHE, COMMA, PERIOD, SLASH, KP_0-KP_9, KP_ENTER,\n"
                   "# KP_ADD, KP_SUBTRACT, KP_MULTIPLY, KP_DIVIDE, KP_DECIMAL, INSERT, DELETE,\n"
                   "# HOME, END, PAGE_UP, PAGE_DOWN, CAPS_LOCK. Mouse buttons: MOUSE_LEFT,\n"
                   "# MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_SIDE, MOUSE_EXTRA.\n"
                   "#\n"
                   "# NAME*0.5 presses at half strength: for buttons the pressure the game\n"
                   "# reads (DualShock 2 pressure-sensitive buttons, full by default), for stick\n"
                   "# directions the deflection.\n"
                   "#\n"
                   "# A gamepad keeps working alongside: its buttons add to these, and a\n"
                   "# gamepad stick that is pushed wins over keys and mouse.\n"
                   "\n"
                   "[buttons]\n" +
                   t.buttons +
                   "\n"
                   "[sticks]\n" +
                   t.sticks +
                   "# Hold for a partial push of the key-driven stick directions (gentler\n"
                   "# steering), to modifier_amount of full.\n"
                   "modifier        = LEFT_ALT\n"
                   "modifier_amount = 0.5\n"
                   "\n"
                   "[mouse]\n" +
                   t.mouseMode +
                   "# Degrees the view turns per pixel of mouse motion (times sensitivity_x/y).\n"
                   "look_speed = 0.2\n"
                   "# How far the view may look up / down from horizontal, in degrees.\n"
                   "look_up_limit = 15\n"
                   "look_down_limit = 75\n"
                   "# Keys that put the camera back behind the " + who + ".\n"
                   "recentre = " + t.recentre + "\n"
                   "# Seconds without mouse motion after which the camera goes back behind the\n"
                   "# " + who + " on its own; 0 = never.\n"
                   "auto_recentre = 0\n"
                   "# true: the camera follows the mouse at once; false: with the game's easing.\n"
                   "look_snap = true\n"
                   "# Multiply the motion; in stick mode 1.0 means 200 pixels push the stick\n"
                   "# from centre to full.\n"
                   "sensitivity_x = 1.0\n"
                   "sensitivity_y = 1.0\n"
                   "# invert_x = false: mouse right turns the view right (look mode) or pushes\n"
                   "# the stick right, which swings the camera to the " + who + "'s right (stick\n"
                   "# mode). invert_y = true: mouse up looks up in both modes (the camera stick\n"
                   "# raises the view when pushed down, airplane style).\n"
                   "invert_x = false\n"
                   "invert_y = true\n"
                   "# Stick mode: what mouse motion drives: right_stick (the camera),\n"
                   "# left_stick, or none.\n"
                   "motion = " + t.motion + "\n"
                   "# Stick mode. offset: the mouse pushes the stick like a thumb would; moving\n"
                   "#   keeps it pushed, and it springs back to centre when the mouse stops (the\n"
                   "#   game's camera stick is a look-around that returns when released).\n"
                   "# velocity: the push follows the mouse speed.\n"
                   "stick_mode = offset\n"
                   "# Stick mode: fraction of the push kept per frame (1/60 s) while the mouse\n"
                   "# is still: 0 recentres at once, 1 never recentres.\n"
                   "decay = 0.85\n"
                   "# Stick mode: smallest push sent for any motion. The game ignores stick\n"
                   "# pushes below about 0.26, so small mouse motions would do nothing with 0.\n"
                   "deadzone = 0.27\n"
                   "# click: a click in the window captures the cursor (that click is not sent\n"
                   "#   to the game); always: captured whenever the window has focus; never.\n"
                   "# The cursor is released by the keys below and when the window loses focus.\n"
                   "capture = click\n"
                   "release = ESCAPE, GRAVE\n"
                   "\n"
                   "[gamepad]\n"
                   "enabled = true\n"
                   "# A gamepad stick pushed further than this wins over keys and mouse (and a\n"
                   "# push below it is ignored: the stick dead zone).\n"
                   "stick_threshold = 0.2\n"
                   "# Gamepad buttons can be rebound like keys: <game button> = <gamepad\n"
                   "# buttons>, named by position: SOUTH, EAST, WEST, NORTH, L1, R1, L2, R2,\n"
                   "# START, SELECT (Back), L3, R3, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT.\n"
                   "# Each game button is pressed by its own gamepad button unless set here,\n"
                   "# e.g. \"cross = SOUTH\" (South = A on Xbox pads, Cross on PlayStation ones).\n"
                   "# Button prompts: auto follows the device in use; keyboard, xbox,\n"
                   "# playstation or nintendo shows that one always (Steam Input and remappers\n"
                   "# can make a pad look like another kind).\n"
                   "prompts = auto\n"
                   "\n"
                   "[hotkeys]\n" +
                   t.hotkeys;
        }

        // ---- Aggressive Inline ------------------------------------------------

        GameControls aggressiveInline()
        {
            ControlsText t;
            t.title = "Aggressive Inline";
            t.character = "skater";
            t.rebind = "# Rebind in the game: Options > Settings > Controls (or Settings in the\n"
                       "# pause menu) saves here at once. Or edit this file while the game is not\n"
                       "# running; delete it to get these defaults back. A key you remove keeps its\n"
                       "# default; an empty value (\"l3 =\") unbinds it.\n";
            t.buttons = "# Game controls: Cross = Jump / Accelerate, Square = Grabs and Tricks,\n"
                        "# Triangle = Grind, Circle = Action (talk, vault, skitch), L1/R1 = Rotate\n"
                        "# left/right, L2/R2 = Cess slide, Start = Pause, Select = Challenges.\n"
                        "cross    = SPACE, K\n"
                        "square   = MOUSE_LEFT, J\n"
                        "triangle = MOUSE_RIGHT, I\n"
                        "circle   = F, L\n"
                        "l1       = Q\n"
                        "r1       = E\n"
                        "l2       = Z\n"
                        "r2       = C\n"
                        "start    = ESCAPE, ENTER\n"
                        "select   = TAB\n"
                        "# D-pad (menus; in a level it is Forward / Left / Right / Brake like the left stick).\n"
                        "up       = UP\n"
                        "down     = DOWN\n"
                        "left     = LEFT\n"
                        "right    = RIGHT\n"
                        "l3       =\n"
                        "r3       =\n";
            t.sticks = "# Keys that push the analog sticks. Left stick = Forward / Left / Right /\n"
                       "# Brake; right stick = Camera (look around; zoom in the park editor, where\n"
                       "# the mouse wheel zooms too).\n"
                       "left_up     = W\n"
                       "left_down   = S\n"
                       "left_left   = A\n"
                       "left_right  = D\n"
                       "right_up    = KP_8, PAGE_UP\n"
                       "right_down  = KP_2, PAGE_DOWN\n"
                       "right_left  = KP_4\n"
                       "right_right = KP_6\n";
            t.mouseMode = "# look: mouse-look. Moving the mouse turns the view around the skater: all\n"
                          "#   the way round left and right, and up and down within the limits below.\n"
                          "#   The view keeps its angle to the skater (it turns when they turn) until\n"
                          "#   you move it back, press a recentre key or auto_recentre runs out. A\n"
                          "#   gamepad's right stick still works the game's own way and recentres it.\n"
                          "#   Only the view turns: the game plays as without mouse-look.\n"
                          "# stick: the mouse pushes a stick instead (see motion and stick_mode): the\n"
                          "#   game's own look-around, which swings back behind the skater.\n"
                          "mode = look\n";
            t.hotkeys = "# PC shortcuts, not game buttons. restart: the challenge you were given\n"
                        "# again (Career; else the level), the lesson (Tutorial), the level\n"
                        "# (Freeskate) or the run (Timed Run, split screen). hide_hud: the score,\n"
                        "# juice meter and the rest of the in-level display off and on.\n"
                        "# screenshot: the picture as a PNG (Pictures/<app>/Aggressive Inline).\n"
                        "# photo_mode: the game stops and a free camera takes over (in a level).\n"
                        "# switch_game: in an app with several games, back to its menu.\n"
                        "restart    = BACKSPACE\n"
                        "hide_hud   = F9\n"
                        "screenshot = F12\n"
                        "photo_mode = P\n"
                        "switch_game = F8\n"
                        "# Gamepad buttons held together for a shortcut, named by position as in\n"
                        "# [gamepad] and joined with +, e.g. \"restart_pad = L3+R3\" (both sticks pressed in).\n"
                        "restart_pad    = L3+R3\n"
                        "hide_hud_pad   =\n"
                        "screenshot_pad =\n"
                        "photo_mode_pad =\n"
                        "switch_game_pad =\n";
            GameControls controls;
            controls.id = "ai";
            controls.title = t.title;
            controls.defaultIni = compose(t);
            controls.labels = {
                {"left_up", "Forward"},
                {"left_down", "Brake"},
                {"left_left", "Turn Left"},
                {"left_right", "Turn Right"},
                {"modifier", "Half Push"},
                {"cross", "Jump"},
                {"square", "Grab and Trick"},
                {"triangle", "Grind"},
                {"circle", "Action"},
                {"l1", "Rotate Left"},
                {"r1", "Rotate Right"},
                {"l2", "Cess Slide Left"},
                {"r2", "Cess Slide Right"},
                {"start", "Pause"},
                {"select", "Challenges"},
                {"recentre", "Camera Behind"},
                {"up", "Menu Up"},
                {"down", "Menu Down"},
                {"left", "Menu Left"},
                {"right", "Menu Right"},
                {"restart", "Restart"},
                {"hide_hud", "Hide HUD"},
                {"screenshot", "Screenshot"},
                {"photo_mode", "Photo Mode"},
            };
            return controls;
        }

        // ---- Dave Mirra Freestyle BMX 2 ------------------------------------------
        // The rider reads its buttons through a move table per controller
        // configuration (Options > Controller: Normal, the default, Beginner,
        // Expert); the names are the Normal configuration's captions in the
        // game's controller screen (UILIB.ZAL ControllerFrame). The Xbox and
        // GameCube releases have the same three configurations and roles (the
        // Xbox spins on its two triggers only, where the PS2 takes L1 or L2
        // and R1 or R2), so the PS2's layout is the default.

        GameControls daveMirra2()
        {
            ControlsText t;
            t.title = "Dave Mirra Freestyle BMX 2";
            t.character = "rider";
            t.rebind = "# Edit this file while the game is not running; delete it to get these\n"
                       "# defaults back. A key you remove keeps its default; an empty value\n"
                       "# (\"l3 =\") unbinds it.\n";
            t.buttons = "# Game controls, as the game's Normal configuration names them: Cross =\n"
                        "# Start / Jump (hold to crouch, let go to jump), Square = Trick, Triangle =\n"
                        "# Grind / Stall, Circle = Modifier, L1 or L2 and R1 or R2 = Spin / Slide,\n"
                        "# Start = Pause. Options > Controller in the game has Beginner (Cross\n"
                        "# pedals too) and Expert (pedal with Up only).\n"
                        "cross    = SPACE, K\n"
                        "square   = MOUSE_LEFT, J\n"
                        "triangle = MOUSE_RIGHT, I\n"
                        "circle   = F, L\n"
                        "l1       = Q\n"
                        "r1       = E\n"
                        "l2       = Z\n"
                        "r2       = C\n"
                        "start    = ESCAPE, ENTER\n"
                        "select   = TAB\n"
                        "# D-pad: Up = Pedal (a tap turns the bike round), Down = Brake, Left / Right =\n"
                        "# Steer, as the left stick; in menus, up, down, left and right.\n"
                        "up       = UP\n"
                        "down     = DOWN\n"
                        "left     = LEFT\n"
                        "right    = RIGHT\n"
                        "l3       =\n"
                        "r3       =\n";
            t.sticks = "# Keys that push the analog sticks. Left stick = Pedal / Steer / Brake, as\n"
                       "# the d-pad. The rider reads the left stick only: the camera is the mouse's.\n"
                       "left_up     = W\n"
                       "left_down   = S\n"
                       "left_left   = A\n"
                       "left_right  = D\n"
                       "right_up    =\n"
                       "right_down  =\n"
                       "right_left  =\n"
                       "right_right =\n";
            t.mouseMode = "# look: mouse-look. Moving the mouse turns the view around the rider (any\n"
                          "#   camera type): all the way round left and right, and up and down within\n"
                          "#   the limits below. The view keeps its angle to the rider (it turns when\n"
                          "#   they turn) until you move it back, press a recentre key or auto_recentre\n"
                          "#   runs out. The game's camera stays clear of the ground and walls, so near\n"
                          "#   the ground looking up goes only a little way.\n"
                          "# stick: the mouse pushes a stick instead (see motion and stick_mode). The\n"
                          "#   game has no camera stick, so this leaves its camera to the game.\n"
                          "mode = look\n";
            t.hotkeys = "# PC shortcuts, not game buttons. restart: the run again from its start\n"
                        "# (a free ride's or a timed one's). hide_hud: the score, the clock and\n"
                        "# the rest of the in-level display off and on. screenshot: the picture\n"
                        "# as a PNG (Pictures/<app>/Dave Mirra Freestyle BMX 2). photo_mode: the\n"
                        "# game stops and a free camera takes over (in a level). switch_game: in\n"
                        "# an app with several games, back to its menu.\n"
                        "restart    = BACKSPACE\n"
                        "hide_hud   = F9\n"
                        "screenshot = F12\n"
                        "photo_mode = P\n"
                        "switch_game = F8\n"
                        "# Gamepad buttons held together for a shortcut, named by position as in\n"
                        "# [gamepad] and joined with +, e.g. \"restart_pad = L3+R3\" (both sticks pressed in).\n"
                        "restart_pad    = L3+R3\n"
                        "hide_hud_pad   =\n"
                        "screenshot_pad =\n"
                        "photo_mode_pad =\n"
                        "switch_game_pad =\n";
            GameControls controls;
            controls.id = "dm2";
            controls.title = t.title;
            controls.defaultIni = compose(t);
            controls.labels = {
                {"left_up", "Pedal"},
                {"left_down", "Brake"},
                {"left_left", "Steer Left"},
                {"left_right", "Steer Right"},
                {"modifier", "Half Push"},
                {"cross", "Start / Jump"},
                {"square", "Trick"},
                {"triangle", "Grind / Stall"},
                {"circle", "Modifier"},
                {"l1", "Spin / Slide (L1)"},
                {"r1", "Spin / Slide (R1)"},
                {"l2", "Spin / Slide (L2)"},
                {"r2", "Spin / Slide (R2)"},
                {"start", "Pause"},
                {"select", "Select"},
                {"recentre", "Camera Behind"},
                {"up", "Pedal (D-Pad)"},
                {"down", "Brake (D-Pad)"},
                {"left", "Steer Left (D-Pad)"},
                {"right", "Steer Right (D-Pad)"},
                {"restart", "Restart"},
                {"hide_hud", "Hide HUD"},
                {"screenshot", "Screenshot"},
                {"photo_mode", "Photo Mode"},
            };
            return controls;
        }

        // ---- BMX XXX ------------------------------------------------------------
        // As DM2: a move table per configuration (Options: Configuration A, the
        // default, B with Trick and Modifier swapped, C with Modifier and
        // Grind / Stall swapped); the names are the game's own option texts
        // (STRING.ZAL KSTR_OPTIONS_*, KSTR_PBTN_*). The Xbox and GameCube
        // releases have the same configurations, so the PS2's layout is the
        // default.

        GameControls bmxXxx()
        {
            ControlsText t;
            t.title = "BMX XXX";
            t.character = "rider";
            t.rebind = "# Edit this file while the game is not running; delete it to get these\n"
                       "# defaults back. A key you remove keeps its default; an empty value\n"
                       "# (\"l3 =\") unbinds it.\n";
            t.buttons = "# Game controls, as the game's Configuration A names them: Cross = Jump /\n"
                        "# Pedal, Square = Trick, Circle = Modifier, Triangle = Grind / Stall (also\n"
                        "# talk, skitch, grab an edge), L1 / R1 = Spins / Skid (both: Ghost Ride),\n"
                        "# R3 = Camera, Start = Pause, Select = Challenge List. The game's Options\n"
                        "# have Configurations B and C, which swap the face buttons around.\n"
                        "cross    = SPACE, K\n"
                        "square   = MOUSE_LEFT, J\n"
                        "triangle = MOUSE_RIGHT, I\n"
                        "circle   = F, L\n"
                        "l1       = Q\n"
                        "r1       = E\n"
                        "l2       = Z\n"
                        "r2       = C\n"
                        "start    = ESCAPE, ENTER\n"
                        "select   = TAB\n"
                        "# D-pad: Steer / Pedal / Brake, as the left stick; in menus, up, down, left\n"
                        "# and right.\n"
                        "up       = UP\n"
                        "down     = DOWN\n"
                        "left     = LEFT\n"
                        "right    = RIGHT\n"
                        "l3       =\n"
                        "r3       = MOUSE_MIDDLE, V\n";
            t.sticks = "# Keys that push the analog sticks. Left stick = Steer / Pedal / Brake, as\n"
                       "# the d-pad; right stick = the park editor's camera zoom.\n"
                       "left_up     = W\n"
                       "left_down   = S\n"
                       "left_left   = A\n"
                       "left_right  = D\n"
                       "right_up    = KP_8, PAGE_UP\n"
                       "right_down  = KP_2, PAGE_DOWN\n"
                       "right_left  = KP_4\n"
                       "right_right = KP_6\n";
            t.mouseMode = "# look: mouse-look. Moving the mouse turns the follow camera around the\n"
                          "#   rider: all the way round left and right, and up and down within the\n"
                          "#   limits below; the game's easing and its keeping clear of walls stay.\n"
                          "#   The rider's-eyes and first-person views, a bail, the view from above\n"
                          "#   and the swings stay the game's. The view keeps its angle until you move\n"
                          "#   it back or auto_recentre runs out (recentre has no key here: R3, the\n"
                          "#   middle button, is the game's camera button).\n"
                          "# stick (the default, with motion = none): the mouse moves nothing;\n"
                          "#   motion = right_stick makes it push the right stick (the park editor's\n"
                          "#   zoom). Settings > Controls > Mouse Look: Free Look is look, Right\n"
                          "#   Stick is stick.\n"
                          "mode = stick\n";
            t.recentre = "";
            t.motion = "none";
            t.hotkeys = "# PC shortcuts, not game buttons. restart: the turn again from its start\n"
                        "# while one runs. hide_hud: the score, the clock and the rest of the\n"
                        "# in-level display off and on. screenshot: the picture as a PNG\n"
                        "# (Pictures/<app>/BMX XXX). photo_mode: the game stops and a free camera\n"
                        "# takes over (in a level). switch_game: in an app with several games,\n"
                        "# back to its menu.\n"
                        "restart    = BACKSPACE\n"
                        "hide_hud   = F9\n"
                        "screenshot = F12\n"
                        "photo_mode = P\n"
                        "switch_game = F8\n"
                        "# Gamepad buttons held together for a shortcut, named by position as in\n"
                        "# [gamepad] and joined with +, e.g. \"restart_pad = L3+R3\" (both sticks pressed in).\n"
                        "restart_pad    = L3+R3\n"
                        "hide_hud_pad   =\n"
                        "screenshot_pad =\n"
                        "photo_mode_pad =\n"
                        "switch_game_pad =\n";
            GameControls controls;
            controls.id = "bmx";
            controls.title = t.title;
            controls.defaultIni = compose(t);
            controls.labels = {
                {"left_up", "Pedal"},
                {"left_down", "Brake"},
                {"left_left", "Steer Left"},
                {"left_right", "Steer Right"},
                {"modifier", "Half Push"},
                {"cross", "Jump / Pedal"},
                {"square", "Trick"},
                {"circle", "Modifier"},
                {"triangle", "Grind / Stall"},
                {"l1", "Spin Left / Skid"},
                {"r1", "Spin Right / Skid"},
                {"l2", "L2"},
                {"r2", "R2"},
                {"r3", "Camera"},
                {"start", "Pause"},
                {"select", "Challenge List"},
                {"up", "Pedal (D-Pad)"},
                {"down", "Brake (D-Pad)"},
                {"left", "Steer Left (D-Pad)"},
                {"right", "Steer Right (D-Pad)"},
                {"restart", "Restart"},
                {"hide_hud", "Hide HUD"},
                {"screenshot", "Screenshot"},
                {"photo_mode", "Photo Mode"},
            };
            return controls;
        }

        const std::vector<GameControls> &games()
        {
            static const std::vector<GameControls> all = {aggressiveInline(), daveMirra2(), bmxXxx()};
            return all;
        }
    }

    const GameControls &gameControls(const std::string &id)
    {
        for (const GameControls &game : games())
            if (game.id == id)
                return game;
        return games().front();
    }

    std::vector<const GameControls *> allGameControls()
    {
        std::vector<const GameControls *> out;
        for (const GameControls &game : games())
            out.push_back(&game);
        return out;
    }
}
