#!/bin/sh
# release-check.sh --mac-app APP | --mac-zip ZIP | --windows-zip ZIP | --windows-dir DIR | --apk APK [options]:
# a package checked as players get it, in seconds (release_check.py beside this script: what it checks and its options).
exec python3 -I "$(dirname "$0")/release_check.py" "$@"
