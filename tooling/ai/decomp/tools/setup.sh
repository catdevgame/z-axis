#!/bin/sh
# Fetch the toolchain into tools/ (gitignored) and create .venv.
#   tools/setup.sh          # matching toolchain only
#   tools/setup.sh --all    # also the other EE compilers used for the comparison
set -e
cd "$(dirname "$0")/.."
REL=https://github.com/decompme/compilers/releases/download/compilers
mkdir -p tools/compilers/dl tools/binutils

# Python env (splat, spimdisasm, rabbitizer)
[ -x .venv/bin/python ] || uv venv -q .venv --python 3.12
uv pip install -q --python .venv/bin/python -r requirements.txt

# decompals PS2 binutils (as/ld/objcopy for the split asm and the link)
if [ ! -x tools/binutils/mips-ps2-decompals-ld ]; then
    curl -sSL -o tools/compilers/dl/binutils-ps2-macos.tar.gz \
        https://github.com/decompals/binutils-mips-ps2-decompals/releases/download/v0.10/binutils-mips-ps2-decompals-macos-arm64.tar.gz
    tar xf tools/compilers/dl/binutils-ps2-macos.tar.gz -C tools/binutils
    mkdir -p tools/binutils/shim   # splat create_config expects mips-linux-gnu-* names
    for f in tools/binutils/mips-ps2-decompals-*; do
        b=$(basename "$f"); ln -sf "../$b" "tools/binutils/shim/mips-linux-gnu-${b#mips-ps2-decompals-}"
    done
fi

# wibo (runs the Windows compiler builds natively via Rosetta)
[ -x tools/wibo ] || { curl -sSL -o tools/wibo https://github.com/decompals/wibo/releases/download/1.2.0/wibo-macos; chmod +x tools/wibo; }

fetch() {  # fetch <archive name in release> <local dir>
    [ -d "tools/compilers/$2" ] && return
    curl -sSL -o "tools/compilers/dl/$1" "$REL/$1"
    mkdir -p "tools/compilers/$2" && tar xf "tools/compilers/dl/$1" -C "tools/compilers/$2"
}
# Matching compiler: Sony ee-gcc 2.9-ee-991111 (Linux i386: cc1 + GNU as 2.9-ee-991111)
fetch ee-gcc2.9-991111.tar.xz ee-gcc2.9-991111
# Same cc1 codegen as a Windows build, runnable with wibo (its bundled as.exe differs)
fetch ee-gcc2.9-991111.tar.gz ee-gcc2.9-991111b-r4

if [ "$1" = "--all" ]; then
    fetch ee-gcc2.9-990721.tar.xz ee-gcc2.9-990721
    fetch ee-gcc2.9-991111-01.tar.xz ee-gcc2.9-991111-01
    fetch ee-gcc2.96.tar.xz ee-gcc2.96
    fetch ee-gcc2.95.2-273a.tar.gz ee-gcc2.95.2-273a
    fetch ee-gcc2.95.3-114.tar.gz ee-gcc2.95.3-114
    fetch ee-gcc2.95.3-136.tar.gz ee-gcc2.95.3-136
    if [ ! -d tools/compilers/ee-gcc2.95.2-274 ]; then
        curl -sSL -o tools/compilers/dl/ps2_compilers.tar.xz "$REL/ps2_compilers.tar.xz"
        tar xf tools/compilers/dl/ps2_compilers.tar.xz -C tools/compilers ee-gcc2.95.2-274 ee-gcc2.95.3-107
    fi
fi

# linux/386 userland for the Linux compiler binaries (Docker Desktop, ~120 MB)
docker image inspect debian:bookworm-slim >/dev/null 2>&1 || docker pull -q --platform linux/386 debian:bookworm-slim
echo "toolchain ready"
