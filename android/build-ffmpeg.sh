#!/bin/sh
# The minimal static FFmpeg of tools/build-ffmpeg.sh (main repo), same version
# and the same configure line, for Android arm64 with the NDK. Pass the prefix
# to CMake as -DPS2X_ENABLE_FFMPEG=ON -DPS2X_FFMPEG_PREFIX=<prefix>.
#
#   android/build-ffmpeg.sh <ffmpeg-9.0.2.tar.xz> <work dir> <prefix>
#
# ANDROID_HOME: the SDK, with its NDK. API level 33.
set -eu
TARBALL=$1
WORK=$2
PREFIX=$3
NDK=$ANDROID_HOME/ndk/$(ls "$ANDROID_HOME/ndk" | sort -V | tail -1)
BIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xJf "$TARBALL" -C "$WORK"
cd "$WORK"/ffmpeg-*
./configure --prefix="$PREFIX" \
    --enable-cross-compile --target-os=android --arch=aarch64 \
    --cc="$BIN/aarch64-linux-android33-clang" --cxx="$BIN/aarch64-linux-android33-clang++" \
    --ar="$BIN/llvm-ar" --ranlib="$BIN/llvm-ranlib" --nm="$BIN/llvm-nm" --strip="$BIN/llvm-strip" \
    --extra-cflags="-O3" \
    --disable-everything \
    --disable-programs --disable-doc --disable-debug \
    --disable-network --disable-autodetect \
    --disable-avdevice --disable-avfilter --disable-swresample \
    --enable-avcodec --enable-avutil --enable-swscale --enable-avformat \
    --enable-decoder=mpeg2video \
    --enable-parser=mpegvideo \
    --enable-protocol=file \
    --enable-demuxer=ogg,flac,mp3,bink,xmv \
    --enable-decoder=vorbis,flac,mp3,mp3float \
    --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,wmv2 \
    --enable-parser=vorbis,flac,mpegaudio \
    --enable-static --disable-shared --enable-pic >"$WORK/configure.log" 2>&1
grep -q '^License: LGPL' "$WORK/configure.log"
make -j"${JOBS:-6}" >"$WORK/build.log" 2>&1
make install >>"$WORK/build.log" 2>&1
echo "$PREFIX"
