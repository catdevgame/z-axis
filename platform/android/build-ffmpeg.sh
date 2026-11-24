#!/bin/sh
# The minimal static FFmpeg of third_party/build-ffmpeg.sh (main repo), same version
# and the same configure line, for Android arm64 with the NDK. Pass the prefix
# to CMake as -DZX_ENABLE_FFMPEG=ON -DZX_FFMPEG_PREFIX=<prefix>.
#
#   platform/android/build-ffmpeg.sh <ffmpeg-9.0.2.tar.xz> <work dir> <prefix>
#
# ANDROID_HOME: the SDK, with its NDK. API level 33.
#
# As third_party/build-ffmpeg.sh: no path of the machine it is built on in the libraries (__FILE__ cut by
# -ffile-prefix-map in the work dir's real folder, the configure line kept without its folders, and each library
# checked); a prefix already made by this revision of it (its BUILDINFO's "paths 2" line) is kept as it is, any other
# made again. Its pkg-config files name their folder relative to themselves (${pcfiledir}), as
# third_party/build-ffmpeg.sh's: never the path the build ran under, which a later build through another path (a
# worktree's data link, removed with the worktree) would not find; a kept prefix's are made so too.
set -eu
TARBALL=$1
WORK=$2
PREFIX=$3
PATHS=2
relative_pc() {
    for pc in "$PREFIX"/lib/pkgconfig/*.pc; do
        [ -f "$pc" ] || continue
        sed -i.bak -E 's#^prefix=.*#prefix=${pcfiledir}/../..#; s#^exec_prefix=.*#exec_prefix=${prefix}#; s#^libdir=.*#libdir=${prefix}/lib#; s#^includedir=.*#includedir=${prefix}/include#' "$pc"
        rm -f "$pc.bak"
    done
}
if [ -f "$PREFIX/BUILDINFO" ] && grep -qx "paths $PATHS" "$PREFIX/BUILDINFO"; then
    relative_pc
    echo "$PREFIX"
    exit 0
fi
NDK=$ANDROID_HOME/ndk/$(ls "$ANDROID_HOME/ndk" | sort -V | tail -1)
BIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin

rm -rf "$WORK" "$PREFIX"
mkdir -p "$WORK"
tar -xJf "$TARBALL" -C "$WORK"
cd "$WORK"/ffmpeg-*
SRC=$(pwd -P)
CONF="--disable-everything --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect \
--disable-avdevice --disable-avfilter --disable-swresample --enable-avcodec --enable-avutil --enable-swscale --enable-avformat \
--enable-decoder=mpeg2video --enable-parser=mpegvideo --enable-protocol=file --enable-demuxer=ogg,flac,mp3,bink,xmv,asf \
--enable-decoder=vorbis,flac,mp3,mp3float --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,wmv2 \
--enable-decoder=adpcm_ima_xbox,wmav2 --enable-parser=vorbis,flac,mpegaudio --enable-static --disable-shared --enable-pic"
# shellcheck disable=SC2086
./configure --prefix="$PREFIX" \
    --enable-cross-compile --target-os=android --arch=aarch64 \
    --cc="$BIN/aarch64-linux-android33-clang" --cxx="$BIN/aarch64-linux-android33-clang++" \
    --ar="$BIN/llvm-ar" --ranlib="$BIN/llvm-ranlib" --nm="$BIN/llvm-nm" --strip="$BIN/llvm-strip" \
    --extra-cflags="-O3 -ffile-prefix-map=$SRC/=" \
    $CONF >"$WORK/configure.log" 2>&1
grep -q '^License: LGPL' "$WORK/configure.log"
sed -i.orig "s|^#define FFMPEG_CONFIGURATION .*|#define FFMPEG_CONFIGURATION \"$CONF --target-os=android --arch=aarch64\"|" config.h
rm -f config.h.orig
make -j"${JOBS:-6}" >"$WORK/build.log" 2>&1
make install >>"$WORK/build.log" 2>&1
for lib in "$PREFIX"/lib/*.a; do
    for form in "$WORK" "$SRC" "$(cd "$WORK" && pwd -P)" "$PREFIX"; do
        if LC_ALL=C grep -a -F -q -- "$form" "$lib"; then
            echo "build-ffmpeg: $(basename "$lib") names the build machine's folder $form" >&2
            exit 1
        fi
    done
done
relative_pc
printf '%s\n' "ffmpeg (Android arm64) from $(basename "$TARBALL")" "paths $PATHS" >"$PREFIX/BUILDINFO"
echo "$PREFIX"
