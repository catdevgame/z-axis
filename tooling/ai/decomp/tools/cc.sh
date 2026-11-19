#!/bin/sh
# Compile one C file to an object with the original toolchain:
#   Sony ee-gcc 2.9-ee-991111 (cc1 + GNU as 2.9-ee-991111), Linux i386 binaries,
#   run inside a persistent linux/386 container (Docker Desktop) named ai-eegcc.
# usage: tools/cc.sh "<cflags>" src.c out.o
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONTAINER=ai-eegcc
CC=tools/compilers/ee-gcc2.9-991111
if [ -z "$(docker ps -q -f name=^${CONTAINER}$)" ]; then
    # parallel ninja jobs: only one of them starts the container
    LOCK="${TMPDIR:-/tmp}/${CONTAINER}.lock"
    while ! mkdir "$LOCK" 2>/dev/null; do sleep 0.2; done
    trap 'rmdir "$LOCK" 2>/dev/null' EXIT
    if [ -z "$(docker ps -q -f name=^${CONTAINER}$)" ]; then
        docker rm -f $CONTAINER >/dev/null 2>&1 || true
        docker run -d --name $CONTAINER --platform linux/386 -v "$ROOT":/w debian:bookworm-slim sleep infinity >/dev/null
    fi
    rmdir "$LOCK"; trap - EXIT
fi
mkdir -p "$(dirname "$3")"
docker exec -w /w $CONTAINER $CC/bin/ee-gcc -B/w/$CC/lib/gcc-lib/ee/2.9-ee-991111/ \
    -c $1 -Iinclude -Isrc -o "$3" "$2"
