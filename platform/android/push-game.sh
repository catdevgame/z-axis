#!/bin/sh
# Puts the games' files on an Android device, in the app's external files folder:
# the folders of the desktop app's Contents/Resources, and the bottom screen's
# level files (panel/*.pmap), made here from those folders by panel_map.py. The
# running game converts nothing. All of this is game data: never commit it.
#
#   ANDROID_SERIAL=<serial> [APP_ID=<id>] platform/android/push-game.sh [--panel-only] <Resources folder> <work folder>
#
# A collection's Resources (games/<id>/disc, ...) go to files/games/<id>/, as
# the app reads them (collection/app.cpp gameResources); a single game's
# (disc, xbox, ...) to files/ itself. The panel's files are each game's own,
# games/<id>/panel: Aggressive Inline's and Dave Mirra BMX 2's (panel_map.py
# --game dm2, its Xbox levels' from games/dm2/xbox-levels) and BMX XXX's
# (panel_map.py --game bmx, from its disc). --panel-only makes and pushes the panels alone: what a change to the
# panel's tools or its file format needs. The APK must be installed first
# (Android makes the folder for it); <work folder> keeps the made files
# between runs.
set -eu

PANEL_ONLY=0
if [ "${1:-}" = "--panel-only" ]; then
    PANEL_ONLY=1
    shift
fi
HERE=$(cd "$(dirname "$0")" && pwd)
RES=$1
WORK=$2
# (APP_ID: the APK's application id, build-apk.sh's)
FILES=/sdcard/Android/data/${APP_ID:-io.github.catdevgame.zaxiscollection}/files
SINGLE_FOLDERS="disc xbox xbox-converted estate dm2-levels xbox-pack xbox-levels"

# (panel_map.py is the packaging's, in tools/modding: this checkout's or its main checkout's)
PANEL_MAP=$HERE/../../tools/modding/panel_map.py
if [ ! -f "$PANEL_MAP" ]; then
    COMMON=$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir)
    PANEL_MAP=${COMMON%/.git}/tools/modding/panel_map.py
fi

# adb push cannot make subfolders in the app's folder (secure_mkdirs), so tar.
send() { # <device folder under files> <folder holding them> <names...>
    TO=$FILES/$1
    FROM=$2
    shift 2
    adb shell "mkdir -p $TO && cd $TO && rm -rf $*"
    COPYFILE_DISABLE=1 tar -cf - --no-xattrs --no-mac-metadata -C "$FROM" "$@" | adb exec-in "tar -xf - -C $TO"
}

# The same step as make-app.sh's, into the work folder: Resources may come from
# an app built before the panel or its tools last changed.
make_panel() { # <game id> <that game's Resources folder> <work folder for it>
    GAME=$1
    GRES=$2
    OUT=$3
    rm -rf "$OUT/panel"
    mkdir -p "$OUT"
    set -- --disc "$GRES/disc" --out "$OUT/panel"
    case $GAME in
    dm2)
        set -- "$@" --game dm2
        [ -d "$GRES/xbox-levels" ] && set -- "$@" --dm2-xbox "$GRES/xbox-levels"
        ;;
    ai)
        [ -d "$GRES/xbox" ] && set -- "$@" --xbox "$GRES/xbox"
        [ -d "$GRES/xbox-converted/ASSETS/LEVELS/ZOO" ] && set -- "$@" --zoo "$GRES/xbox-converted/ASSETS/LEVELS/ZOO"
        [ -d "$GRES/estate" ] && set -- "$@" --estate "$GRES/estate"
        ;;
    bmx)
        set -- "$@" --game bmx
        ;;
    *)
        return 1 # no panel for this game yet
        ;;
    esac
    PYTHONDONTWRITEBYTECODE=1 python3 -I "$PANEL_MAP" build "$@" | grep -v ' faces, '
}

adb shell "[ -d $FILES ]" || { echo "no $FILES: install the APK and start it once" >&2; exit 1; }

if [ -d "$RES/games" ]; then
    # A collection: each game's folders to files/games/<id>, its panel from the work folder.
    if [ "$PANEL_ONLY" = 0 ]; then
        # The single-game layout's folders, if a game was pushed that way before, are stale.
        adb shell "cd $FILES && rm -rf $SINGLE_FOLDERS panel"
    fi
    for GRES in "$RES"/games/*; do
        GAME=$(basename "$GRES")
        [ -d "$GRES/disc" ] || { echo "no $GRES/disc" >&2; exit 1; }
        if [ "$PANEL_ONLY" = 0 ]; then
            send games "$RES/games" "$GAME"
        fi
        if make_panel "$GAME" "$GRES" "$WORK/games/$GAME"; then
            send "games/$GAME" "$WORK/games/$GAME" panel
        fi
    done
else
    # One game, its folders in files/ itself (an app of one game, Aggressive Inline's or DM2's).
    [ -d "$RES/disc" ] || { echo "no $RES/disc" >&2; exit 1; }
    GAME=ai
    [ -f "$RES/disc/SLUS_201.59" ] && GAME=dm2
    if [ "$PANEL_ONLY" = 0 ]; then
        HAVE=
        for F in $SINGLE_FOLDERS; do [ -d "$RES/$F" ] && HAVE="$HAVE $F"; done
        send . "$RES" $HAVE
    fi
    make_panel "$GAME" "$RES" "$WORK"
    send . "$WORK" panel
fi
adb shell "du -s $FILES"
