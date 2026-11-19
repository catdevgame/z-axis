#!/usr/bin/env python3
"""The game's Settings page as the player sees it with no extras discs and with every one: pictures.

    settings_shots.py APP SCRATCH GAMES_FROM [--games ai,dm2,bmx] [--secs 240] [--placeholders]

APP: the collection app's executable (a copy of make-app.sh's, e.g. "Z-Axis Collection.app/Contents/MacOS/Z-Axis
Collection"). SCRATCH: a scratch folder (the runs' support folders, logs and HOME; pictures in SCRATCH/shots).
GAMES_FROM: a support folder where the games are set up (its games/<id>/: the PS2 files from Settings > Games, and
for the full setup its extras/ from Games > Add Xbox Disc / Add GameCube Disc). It is only read: each run's
games/<id> is a clone of it (cp -c), so nothing the game writes lands there.

Two setups per game, each a run of the app with its own support folder:
  none   the game's folder without its extras/; when the app bundles extras of its own (Resources/games/<id>/xbox,
         estate, releases ...: a workshop build made with the discs) the run uses a clone of the app without them
  full   the game's folder with its extras/; --placeholders (or a game whose GAMES_FROM folder has no extras/):
         files where collection/offered.h's findInstalled looks for each need instead (one byte each: the rows
         show, the content itself is not there, so only the Settings page is to be looked at)
Each run boots the game from the menu (PS2X_MENU_SCRIPT game:<id>, Enter), presses START until the game's Settings
page opens through the control channel (PS2X_CONTROL "settings", as Cmd+,), and writes
  shots/<game>-<setup>-display.png   the Display section (Display Mode, Map Window)
  shots/<game>-<setup>-game.png      the Game section (L1 from the first: the last), EXTRAS at its foot
  shots/<game>-<setup>-game-end.png  the same, moved down to its last rows
then quits the app (by its PID). Every run stops after --secs seconds whatever happened (exit 1 then).
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

# What collection/offered.h's findInstalled looks for, by the extras folder the setup makes (relative paths).
PLACEHOLDERS = {
    "ai": ["xbox/media/assets/string.fre", "xbox/media/assets/audio/speech01.zsd", "xbox/media/assets/uilib.zal",
           "xbox/media/assets/levels/CIVIC/CIVIC.bin", "xbox/media/assets/levels/BRDWLK/BRDWLKT.bin",
           "xbox-converted/ASSETS/SKATERS/GORILLA/GOR.ZIT", "xbox-pack/textures/.keep", "xbox-pack/models/.keep",
           "xbox-pack/movies/.keep", "estate/.keep"],
    "dm2": ["xbox-levels/levels.txt", "xbox-levels/themes/.keep", "xbox-levels/movies/ASSETS/MOVIE/RETRO.PSS"],
    "bmx": ["releases/strings/.keep", "releases/challenges/.keep", "releases/editor/OBJECT.OBJ",
            "releases/movies/ASSETS/MOVIES/MENU/BRONX.PSM", "releases/movies/ASSETS/MOVIES/ZAXIS.PSS",
            "releases/levels/ASSETS/LEVELS/.keep"],
}
# The folders an app bundles extras in (collection/settings.cpp kReleaseFolders).
BUNDLED = ["xbox", "xbox-converted", "xbox-pack", "estate", "xbox-levels", "releases"]


def clone(src, dst):
    """A copy-on-write clone where the file system has them (APFS), else a copy."""
    if sys.platform == "darwin" and subprocess.call(["cp", "-cR", src, dst]) == 0:
        return
    if os.path.isdir(src):
        shutil.copytree(src, dst, symlinks=True)
    else:
        shutil.copy2(src, dst)


def app_bundle(app):
    """The .app folder an executable is in (None: not a macOS bundle)."""
    path = os.path.abspath(app)
    while path != os.path.dirname(path):
        if path.endswith(".app"):
            return path
        path = os.path.dirname(path)
    return None


def app_without_extras(app, scratch, game):
    """APP, or a clone of its bundle without the extras it bundles for `game`."""
    bundle = app_bundle(app)
    if not bundle:
        return app
    resources = os.path.join(bundle, "Contents", "Resources", "games", game)
    if not any(os.path.isdir(os.path.join(resources, f)) for f in BUNDLED):
        return app
    copy = os.path.join(scratch, "app-none-" + game, os.path.basename(bundle))
    if not os.path.isdir(copy):
        os.makedirs(os.path.dirname(copy), exist_ok=True)
        clone(bundle, copy)
        for f in BUNDLED:
            shutil.rmtree(os.path.join(copy, "Contents", "Resources", "games", game, f), ignore_errors=True)
    return os.path.join(copy, os.path.relpath(os.path.abspath(app), bundle))


class Control:
    """The runtime's control channel (runtime/include/runtime/ps2_debug_control.h): a command per line of cmd,
    its reply on the line of out that starts with its number."""

    def __init__(self, folder, proc):
        self.folder, self.proc, self.sent = folder, proc, 0
        os.makedirs(folder, exist_ok=True)
        for f in ("cmd", "out"):
            open(os.path.join(folder, f), "w").close()

    def do(self, command, timeout=60):
        self.sent += 1
        with open(os.path.join(self.folder, "cmd"), "a") as f:
            f.write(command + "\n")
        deadline = time.time() + timeout
        while time.time() < deadline and self.proc.poll() is None:
            with open(os.path.join(self.folder, "out")) as f:
                for line in f:
                    number, _, rest = line.partition(" ")
                    if number == str(self.sent):
                        return rest.strip()
            time.sleep(0.05)
        return None


def run(app, scratch, games_from, game, setup, secs, placeholders, shots):
    base = os.path.join(scratch, f"{game}-{setup}")
    shutil.rmtree(base, ignore_errors=True)
    support, home, logs = (os.path.join(base, d) for d in ("support", "home", "logs"))
    for d in (support, home, logs):
        os.makedirs(d)
    source = os.path.join(games_from, "games", game)
    target = os.path.join(support, "games", game)
    os.makedirs(target)
    for entry in os.listdir(source):
        if entry != "extras":
            clone(os.path.join(source, entry), os.path.join(target, entry))
    runner = app
    if setup == "none":
        runner = app_without_extras(app, scratch, game)
    elif os.path.isdir(os.path.join(source, "extras")) and not placeholders:
        clone(os.path.join(source, "extras"), os.path.join(target, "extras"))
    else:
        for rel in PLACEHOLDERS[game]:
            path = os.path.join(target, "extras", rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(b"x")
    env = dict(os.environ, HOME=home, PS2X_APP_SUPPORT_DIR=support, PS2X_APP_LOG_DIR=logs,
               PS2X_MENU_SCRIPT=f"game:{game},Enter", PS2X_CONTROL=os.path.join(base, "ctl"))
    with open(os.path.join(base, "app.out"), "w") as out:
        proc = subprocess.Popen([runner], env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    control = Control(env["PS2X_CONTROL"], proc)
    deadline = time.time() + secs
    ok = False
    try:
        # The game's Settings page opens once the game has set its handler (its front end).
        while time.time() < deadline and proc.poll() is None:
            if control.do("settings", timeout=10) == "ok":
                ok = True
                break
            control.do("press START 4 30", timeout=10)
            time.sleep(2)
        if ok:
            name = os.path.join(shots, f"{game}-{setup}")
            ok = control.do("wait 90", 30) is not None and control.do(f"shot {name}-display.png", 30) == "ok"
            control.do("press L1 4 30", 20)
            control.do("wait 60", 20)
            ok = ok and control.do(f"shot {name}-game.png", 30) == "ok"
            for _ in range(24):
                control.do("press DOWN 3 8", 10)
            control.do("wait 60", 20)
            ok = ok and control.do(f"shot {name}-game-end.png", 30) == "ok"
    finally:
        if proc.poll() is None:
            os.kill(proc.pid, signal.SIGTERM)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                os.kill(proc.pid, signal.SIGKILL)
                proc.wait()
    print(f"{game} {setup}: {'shots taken' if ok else 'no shots (see ' + base + '/app.out)'}")
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("app")
    parser.add_argument("scratch")
    parser.add_argument("games_from")
    parser.add_argument("--games", default="ai,dm2,bmx")
    parser.add_argument("--secs", type=int, default=240)
    parser.add_argument("--placeholders", action="store_true")
    args = parser.parse_args()
    scratch = os.path.abspath(args.scratch)
    shots = os.path.join(scratch, "shots")
    os.makedirs(shots, exist_ok=True)
    good = True
    for game in args.games.split(","):
        if not os.path.isdir(os.path.join(args.games_from, "games", game)):
            print(f"{game}: not set up in {args.games_from}, skipped")
            continue
        for setup in ("none", "full"):
            good = run(args.app, scratch, os.path.abspath(args.games_from), game, setup, args.secs,
                       args.placeholders, shots) and good
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
