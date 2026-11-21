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
Each run boots the game from the menu (ZX_MENU_SCRIPT game:<id>, Enter) straight into its first level
(ZX_DEV_START_LEVEL=0: DM2's and BMX XXX's front ends have no way to the page but their Options, so the ask is made
in play, where every game pauses as Start does and its pause menu opens the page) and asks for the game's Settings
page through the control channel (ZX_CONTROL "settings", as Cmd+,) until the page itself says it opened. The
control channel's "ok" only says the ask was taken (the game may still be in its intro or loading the level), so
the page is judged by its own state: with ZX_TRACE_SETTINGS=1 the page logs each of its
keys with the section and row it then shows (zge_settings.cpp, "[settings] page 0x... Open: Display ..."). Then
  shots/<game>-<setup>-display.png   the Display section (Display Mode, Map Window), once the page says Open there
  shots/<game>-<setup>-game.png      the Game section (L1 from the first: the last), once the page says it is there
  shots/<game>-<setup>-game-end.png  the same on its last row (Up from the first: the rows wrap), scrolled to its foot
then quits the app (by its PID). A run whose page did not open or did not show those sections takes no shot and
says why ("FAILED: ..."); every run stops after --secs seconds whatever happened. Exit 1 if any run failed.
"""
import argparse
import glob
import os
import re
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


# The page's own record of each key (zge_settings.cpp pageKey, ZX_TRACE_SETTINGS=1): the key, the section shown
# and the row chosen after it.
PAGE_LINE = re.compile(r"^\[settings\] page 0x[0-9a-f]+ (\w+): (\w+) (\S*) '")
# The sections the pictures are of (zge_settings.cpp kPage): the first and the last.
FIRST_SECTION, LAST_SECTION = "Display", "Game"


class PageLog:
    """The Settings page's keys as the run's logs have them so far (the app's log in ZX_APP_LOG_DIR, and its
    stdout and stderr)."""

    def __init__(self, base):
        self.files = [os.path.join(base, "app.out"), os.path.join(base, "logs", "*.log")]

    def events(self):
        out = []
        for pattern in self.files:
            for path in sorted(glob.glob(pattern)):
                with open(path, errors="replace") as f:
                    for line in f:
                        m = PAGE_LINE.match(line)
                        if m:
                            out.append(m.groups())  # (key, section, row)
        return out

    def placed(self):
        """Whether the dev start has put the player in the level (the games' devstart: "[devstart] placed in level")."""
        for pattern in self.files:
            for path in glob.glob(pattern):
                with open(path, errors="replace") as f:
                    if any(line.startswith("[devstart] placed in level") for line in f):
                        return True
        return False

    def wait(self, count, key, proc, timeout):
        """The first event after the first `count` with this key, within timeout seconds (None: none)."""
        deadline = time.time() + timeout
        while time.time() < deadline and proc.poll() is None:
            for event in self.events()[count:]:
                if event[0] == key:
                    return event
            time.sleep(0.25)
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
    env = dict(os.environ, HOME=home, ZX_APP_SUPPORT_DIR=support, ZX_APP_LOG_DIR=logs,
               ZX_MENU_SCRIPT=f"game:{game},Enter", ZX_CONTROL=os.path.join(base, "ctl"), ZX_TRACE_SETTINGS="1",
               ZX_DEV_START_LEVEL="0")
    with open(os.path.join(base, "app.out"), "w") as out:
        proc = subprocess.Popen([runner], env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    control = Control(env["ZX_CONTROL"], proc)
    page = PageLog(base)
    deadline = time.time() + secs
    left = lambda: max(1.0, deadline - time.time())
    name = os.path.join(shots, f"{game}-{setup}")
    for suffix in ("display", "game", "game-end"):
        if os.path.exists(f"{name}-{suffix}.png"):
            os.remove(f"{name}-{suffix}.png")
    why = None

    def shot(suffix):
        path = f"{name}-{suffix}.png"
        if control.do(f"shot {path}", 30) != "ok" or not os.path.isfile(path) or os.path.getsize(path) == 0:
            return f"the picture {path} was not written"
        return None

    try:
        # Asked once the player is in the level (an ask in the front end would take its own way while the dev start
        # goes through the menus), until the page says it opened: the game opens it in play.
        while not page.placed() and time.time() < deadline and proc.poll() is None:
            time.sleep(1)
        opened = None
        while opened is None and time.time() < deadline and proc.poll() is None:
            seen = len(page.events())
            if control.do("settings", timeout=10) == "ok":
                opened = page.wait(seen, "Open", proc, min(10.0, left()))
            if opened is None:
                time.sleep(2)
        if opened is None and not page.placed():
            why = "the dev start never reached the level" + (" (the app quit)" if proc.poll() is not None else
                                                              f" in {secs} s")
        elif opened is None:
            why = "the Settings page never opened" + (" (the app quit)" if proc.poll() is not None else
                                                      f" in {secs} s")
        elif opened[1] != FIRST_SECTION:
            why = f"the page opened on {opened[1]}, not {FIRST_SECTION}"
        if why is None:
            control.do("wait 90", 30)
            why = shot("display")
        if why is None:
            # L1 from the first section: the last.
            seen = len(page.events())
            control.do("press L1 4 30", 20)
            moved = page.wait(seen, "Prev", proc, min(20.0, left()))
            if moved is None:
                why = "L1 did not reach the page (it closed?)"
            elif moved[1] != LAST_SECTION:
                why = f"L1 showed {moved[1]}, not {LAST_SECTION}"
        if why is None:
            control.do("wait 60", 20)
            why = shot("game")
        if why is None:
            # Up from the section's first row: its last (the page's rows wrap), the page scrolled to its foot.
            seen = len(page.events())
            control.do("press UP 4 30", 20)
            step = page.wait(seen, "Up", proc, min(20.0, left()))
            if step is None:
                why = "Up did not reach the page (it closed?)"
            elif step[1] != LAST_SECTION:
                why = f"Up showed {step[1]}, not {LAST_SECTION}"
            elif step[2] == moved[2]:
                why = f"{LAST_SECTION} has one row: no end to show"
        if why is None:
            control.do("wait 60", 20)
            why = shot("game-end")
    finally:
        if proc.poll() is None:
            os.kill(proc.pid, signal.SIGTERM)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                os.kill(proc.pid, signal.SIGKILL)
                proc.wait()
    if why is None:
        print(f"{game} {setup}: the Settings page's {FIRST_SECTION} and {LAST_SECTION} sections taken")
        return True
    print(f"{game} {setup}: FAILED: {why} (see {base}/logs)", file=sys.stderr)
    return False


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
