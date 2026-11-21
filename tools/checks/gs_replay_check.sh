#!/bin/sh
# Bit-exact regression check for the GS renderer, using recorded GS traces
# (ZX_GS_DUMP, see zx-gsreplay).
#
#   gs_replay_check.sh record <zx-gsreplay> <trace-dir> <ref-dir>
#   gs_replay_check.sh check  <zx-gsreplay> <trace-dir> <ref-dir>
#
# record: replays every <trace-dir>/*.gsr and stores, per trace, the hash of
#         the full 4 MiB GS local memory and of the presented picture after
#         every frame (<ref-dir>/<trace>.hash), plus the raw local memory per
#         frame (<ref-dir>/<trace>-<n>.vram) for gs_vram_diff.py.
# check:  replays again and compares against <ref-dir>. Prints OK or DIFF per
#         trace and the replay time; exits 1 on any difference. A second
#         replay without the per-frame VRAM snapshots (--picture-hash) must
#         present the same pictures, with draws still queued as in the runner. With
#         KEEP=<dir>, the local memory of differing traces is written there
#         so it can be compared with
#         gs_vram_diff.py <ref-dir>/<trace>-<n>.vram <dir>/<trace>-<n>.vram
#
# Record the reference with the renderer before a change, check after it.
set -u
mode=${1:-}
replay=${2:-}
traces=${3:-}
ref=${4:-}
if [ -z "$mode" ] || [ -z "$replay" ] || [ -z "$traces" ] || [ -z "$ref" ]; then
    sed -n '2,17p' "$0"
    exit 2
fi

status=0
mkdir -p "$ref"
for trace in "$traces"/*.gsr; do
    name=$(basename "$trace" .gsr)
    case "$mode" in
    record)
        "$replay" "$trace" "$ref/$name" --no-png --hash --dump-vram --time > "$ref/$name.log" 2>&1
        grep -E '^frame [0-9]+ vram=' "$ref/$name.log" > "$ref/$name.hash"
        echo "$name: $(wc -l < "$ref/$name.hash" | tr -d ' ') frames recorded, $(grep -E '^[0-9]+ events' "$ref/$name.log" | sed 's/.*frames, //')"
        ;;
    check)
        tmp=$(mktemp -d)
        "$replay" "$trace" "$tmp/$name" --no-png --hash --time > "$tmp/$name.log" 2>&1
        grep -E '^frame [0-9]+ vram=' "$tmp/$name.log" > "$tmp/$name.hash"
        time=$(grep -E '^[0-9]+ events' "$tmp/$name.log" | sed 's/.*frames, //')
        "$replay" "$trace" "$tmp/$name" --no-png --picture-hash > "$tmp/$name.plog" 2>&1
        grep -E '^frame [0-9]+ pixels=' "$tmp/$name.plog" > "$tmp/$name.pictures"
        sed 's/ vram=[0-9a-f]*//' "$ref/$name.hash" > "$tmp/$name.refpictures"
        if ! cmp -s "$tmp/$name.refpictures" "$tmp/$name.pictures"; then
            echo "DIFF $name pictures without VRAM snapshots"
            diff "$tmp/$name.refpictures" "$tmp/$name.pictures" | sed 's/^/     /'
            status=1
        fi
        if cmp -s "$ref/$name.hash" "$tmp/$name.hash" && [ -s "$tmp/$name.hash" ]; then
            echo "OK   $name ($time)"
        else
            echo "DIFF $name ($time)"
            diff "$ref/$name.hash" "$tmp/$name.hash" | sed 's/^/     /'
            status=1
            if [ -n "${KEEP:-}" ]; then
                mkdir -p "$KEEP"
                "$replay" "$trace" "$KEEP/$name" --no-png --dump-vram > /dev/null 2>&1
            fi
        fi
        rm -rf "$tmp"
        ;;
    *)
        echo "unknown mode $mode" >&2
        exit 2
        ;;
    esac
done
exit $status
