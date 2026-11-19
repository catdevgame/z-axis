#!/bin/bash
# build-llvm.sh HOST OUT_DIR [JOBS]: the app's bundled compiler (llvm.lock) for HOST, from the pinned source release,
# into OUT_DIR/<version>-<HOST>-<recipe>.tar.xz (and its SHA-256), to be kept in the workshop's archive store and pinned in llvm.lock.
#   HOST: macos-arm64 | macos-x86_64 (on a Mac), linux-x64 (on Linux), android-arm64 (on Linux or a Mac, with
#   ANDROID_NDK set: the NDK's clang cross-compiles it for the Thor).
# What it builds: clang and lld (clang++, ld64.lld, ld.lld and lld-link are their names; LLVM's one-program driver build
# makes an empty tool list for 23.1.5 in this configuration), for the AArch64 and X86 targets alone, with no zlib, zstd, libxml2 or terminfo, statically linked where the
# host allows; and clang's resource headers that a game module reaches (game_module_flags.txt: intrinsics, stdint.h and
# kin). Run at low priority; JOBS default 8.
set -euo pipefail
HOST=$1; OUT=$2; JOBS=${3:-8}
HERE=$(cd "$(dirname "$0")" && pwd)
lock() { awk -v k="$1" '$1 == k { print $2 }' "$HERE/llvm.lock"; }
VERSION=$(lock version); URL=$(lock source); SHA=$(lock source_sha256)
WORK=$OUT/work-$HOST
mkdir -p "$WORK" "$OUT"
SRC=$OUT/llvm-project-$VERSION.src
if [ ! -d "$SRC" ]; then
  TAR=$OUT/llvm-project-$VERSION.src.tar.xz
  if [ ! -f "$TAR" ]; then curl -fL --retry 3 -o "$TAR.part" "$URL"; mv "$TAR.part" "$TAR"; fi
  GOT=$( (shasum -a 256 "$TAR" 2>/dev/null || sha256sum "$TAR") | cut -d' ' -f1)
  [ "$GOT" = "$SHA" ] || { echo "build-llvm.sh: $TAR is $GOT, llvm.lock pins $SHA" >&2; exit 1; }
  tar -xJf "$TAR" -C "$OUT"
fi
# What decides the programs: CFG. Its hash (with HOST) is the archive's <recipe>, so a change to the rest of this file
# (the link jobs, the packing) keeps the name of an archive that comes out the same.
CFG=(-DCMAKE_BUILD_TYPE=Release
  -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD="AArch64;X86"
  -DLLVM_DISTRIBUTION_COMPONENTS="clang;lld;clang-resource-headers"
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF
  -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
  -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF
  -DCLANG_PLUGIN_SUPPORT=OFF -DCLANG_DEFAULT_LINKER=lld -DLLVM_ENABLE_PLUGINS=OFF)
EXTRA=()
case $HOST in
  macos-arm64) CFG+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0) ;;
  macos-x86_64) CFG+=(-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0) ;;
  linux-x64) CFG+=(-DLLVM_STATIC_LINK_CXX_STDLIB=ON) ;;
  android-arm64)
    : "${ANDROID_NDK:?ANDROID_NDK: the NDK to cross-compile with}"
    # (the host tools LLVM runs while it builds: tblgen, built natively first by LLVM's own NATIVE build)
    CFG+=(-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DLLVM_HOST_TRIPLE=aarch64-linux-android33
          -DLLVM_STATIC_LINK_CXX_STDLIB=ON -DANDROID_NDK_REVISION="$(awk -F' = ' '$1 == "Pkg.Revision" { print $2 }' "$ANDROID_NDK/source.properties")")
    EXTRA=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake") ;;
  *) echo "build-llvm.sh: no host $HOST" >&2; exit 2 ;;
esac
RECIPE=$( { echo "$HOST"; printf '%s\n' "${CFG[@]}"; } | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-8)
nice -n 19 cmake -G Ninja -S "$SRC/llvm" -B "$WORK/build" -DCMAKE_INSTALL_PREFIX="$WORK/install" "${CFG[@]}" ${EXTRA[@]+"${EXTRA[@]}"} \
  -DLLVM_PARALLEL_LINK_JOBS=2
nice -n 19 ninja -C "$WORK/build" -j "$JOBS" install-distribution-stripped
NAME=$VERSION-$HOST-$RECIPE
rm -rf "$WORK/$NAME"; mkdir -p "$WORK/$NAME"
cp -a "$WORK/install/bin" "$WORK/install/lib" "$WORK/$NAME/"
cp "$SRC/llvm/LICENSE.TXT" "$WORK/$NAME/LICENSE.TXT"
tar -C "$WORK" -cJf "$OUT/$NAME.tar.xz" "$NAME"
( cd "$OUT" && { shasum -a 256 "$NAME.tar.xz" 2>/dev/null || sha256sum "$NAME.tar.xz"; } ) | tee "$OUT/$NAME.tar.xz.sha256"
du -sh "$WORK/$NAME" "$OUT/$NAME.tar.xz"
