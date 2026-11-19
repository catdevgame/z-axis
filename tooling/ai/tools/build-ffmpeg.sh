#!/usr/bin/env bash
# Builds the minimal FFmpeg the runtime needs, as static libraries: for the
# game's PSS movies libavcodec with the MPEG-2 video decoder and the MPEG
# video parser, libswscale (YUV to RGBA) and libavutil; for a custom
# soundtrack (Mods/Music) libavformat reading local files with the Ogg,
# FLAC and MP3 demuxers and the Vorbis, FLAC and MP3 decoders (Opus would
# need swresample); for the Xbox movies (tools/xbox_pack.py) the Bink
# demuxer and the Bink video and audio decoders, and for the other
# releases' movies the games play as their discs have them (BMX XXX's
# level previews and logo movie: collection/extras) the XMV demuxer and
# the WMV2 decoder. No
# programs, no avdevice/avfilter/swresample, no network, no external or
# autodetected libraries (no VideoToolbox, iconv, zlib, ...), LGPL only (no
# --enable-gpl / --enable-nonfree).
#
#   tools/build-ffmpeg.sh            # prints the install prefix on stdout
#
# The release tarball is pinned by version and SHA-256 and cached with the
# build under third_party-cache/ (git-ignored); a second run just prints the
# prefix. Pass the prefix to CMake as -DPS2X_FFMPEG_PREFIX=<prefix>.
#
# Environment:
#   FFMPEG_CACHE   cache dir (default: <repo>/third_party-cache)
#   MIN_MACOS      macOS deployment target (default 11.0, the first macOS on Apple Silicon)
#   ARCH           target arch (default: the host's; arm64/aarch64 or x86_64)
#   JOBS           parallel make jobs
#
# Written for macOS; Linux works as is (MIN_MACOS is ignored). Windows x64:
# run it from an MSYS2 shell (bash, make, nasm) started from a Visual Studio
# x64 developer environment (cl.exe and link.exe on PATH, MSYS2_PATH_TYPE=
# inherit); it then builds with MSVC (--toolchain=msvc) against the static
# CRT (/MT), as .lib files (tools/make-windows.ps1 does all of this).
set -euo pipefail

FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
FFMPEG_URL=https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${FFMPEG_CACHE:-$ROOT/third_party-cache}
OS=$(uname -s)
ARCH=${ARCH:-$(uname -m)}
MIN_MACOS=${MIN_MACOS:-11.0}
JOBS=${JOBS:-$( (sysctl -n hw.ncpu || nproc) 2>/dev/null || echo 4)}
case $OS in MSYS*|MINGW*|CYGWIN*) OS=Windows ;; esac
EXTRA_CONFIGURE=${EXTRA_CONFIGURE:-}

say() { printf '[build-ffmpeg] %s\n' "$*" >&2; }
die() { printf '[build-ffmpeg] error: %s\n' "$*" >&2; exit 1; }

case $ARCH in
  arm64|aarch64) FF_ARCH=aarch64 ;;
  x86_64|amd64)  FF_ARCH=x86_64 ;;
  *) die "unsupported ARCH $ARCH" ;;
esac

TAG=$OS-$ARCH
CFLAGS="-O3"
LDFLAGS=""
CROSS=()
TRIM=   # (the compiler flag that cuts the source folder from __FILE__; its form is in the key)
if [[ $OS == Darwin ]]; then
  TAG=$TAG-macos$MIN_MACOS
  CFLAGS="$CFLAGS -arch $ARCH -mmacosx-version-min=$MIN_MACOS"
  LDFLAGS="-arch $ARCH -mmacosx-version-min=$MIN_MACOS"
  [[ $ARCH != $(uname -m) ]] && CROSS=(--enable-cross-compile --target-os=darwin)
  CC=${CC:-clang}
elif [[ $OS == Windows ]]; then
  # MSVC, static CRT (the runtime links /MT too); nasm for the x86 code.
  CFLAGS="-O2 -MT"
  CROSS=(--toolchain=msvc --target-os=win64)
  # MSVC writes __FILE__ (FFmpeg's assert messages) as the full path of the build folder: the release's .exe
  # named this machine's folders (winbuild1). -d1trimfile cuts it to libavcodec/... (cl matches / and \ alike).
  TRIM='-d1trimfile:<source>/'
  CC=${CC:-cl}
else
  CC=${CC:-cc}
fi
sha256() { if command -v shasum >/dev/null; then shasum -a 256 "$@"; else sha256sum "$@"; fi; }

# The configure line is part of the cache key: changing it rebuilds.
CONFIGURE=(
  --disable-everything
  --disable-programs --disable-doc --disable-debug
  --disable-network --disable-autodetect
  --disable-avdevice --disable-avfilter --disable-swresample
  --enable-avcodec --enable-avutil --enable-swscale --enable-avformat
  --enable-decoder=mpeg2video
  --enable-parser=mpegvideo
  --enable-protocol=file
  --enable-demuxer=ogg,flac,mp3,bink,xmv
  --enable-decoder=vorbis,flac,mp3,mp3float
  --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,wmv2
  --enable-parser=vorbis,flac,mpegaudio
  --enable-static --disable-shared --enable-pic
  --arch=$FF_ARCH --cc=$CC
)
KEY=$(printf '%s\n' "$FFMPEG_VERSION" "$CFLAGS$TRIM" "$LDFLAGS" "${CONFIGURE[@]}" ${CROSS[@]+"${CROSS[@]}"} "$EXTRA_CONFIGURE" \
  | sha256 | cut -c1-10)
PREFIX=$CACHE/ffmpeg-$FFMPEG_VERSION-$TAG-$KEY

if [[ -f $PREFIX/.done ]]; then
  echo "$PREFIX"
  exit 0
fi

# ---- download and verify --------------------------------------------------
mkdir -p "$CACHE/downloads"
TARBALL=$CACHE/downloads/ffmpeg-$FFMPEG_VERSION.tar.xz
check() { [[ $(sha256 "$1" | cut -d' ' -f1) == "$FFMPEG_SHA256" ]]; }
if [[ ! -f $TARBALL ]] || ! check "$TARBALL"; then
  say "downloading $FFMPEG_URL"
  curl -sSfL "$FFMPEG_URL" -o "$TARBALL.part"
  check "$TARBALL.part" || { rm -f "$TARBALL.part"; die "SHA-256 mismatch for $FFMPEG_URL"; }
  mv "$TARBALL.part" "$TARBALL"
fi

# ---- configure and build ----------------------------------------------------
WORK=$CACHE/build/ffmpeg-$FFMPEG_VERSION-$TAG-$KEY
rm -rf "$WORK" "$PREFIX"
mkdir -p "$WORK"
say "extracting into $WORK"
tar -xJf "$TARBALL" -C "$WORK"
SRC=$WORK/ffmpeg-$FFMPEG_VERSION
[[ -n $TRIM ]] && CFLAGS="$CFLAGS ${TRIM/<source>/$(cygpath -m "$SRC")}"
say "configuring ($TAG)"
# shellcheck disable=SC2086
(cd "$SRC" && ./configure --prefix="$PREFIX" "${CONFIGURE[@]}" ${CROSS[@]+"${CROSS[@]}"} $EXTRA_CONFIGURE \
  --extra-cflags="$CFLAGS" --extra-ldflags="$LDFLAGS" >"$WORK/configure.log" 2>&1) \
  || { tail -30 "$WORK/configure.log" >&2; [[ -f $SRC/ffbuild/config.log ]] && tail -30 "$SRC/ffbuild/config.log" >&2; die "configure failed"; }
grep -q '^License: LGPL' "$WORK/configure.log" || die "configuration is not LGPL (see $WORK/configure.log)"
say "building with $JOBS jobs"
make -C "$SRC" -j"$JOBS" >"$WORK/build.log" 2>&1 || { tail -40 "$WORK/build.log" >&2; die "build failed"; }
make -C "$SRC" install >>"$WORK/build.log" 2>&1 || die "install failed"
cp "$SRC/COPYING.LGPLv2.1" "$PREFIX/" 2>/dev/null || true
printf '%s\n' "ffmpeg $FFMPEG_VERSION sha256 $FFMPEG_SHA256" "cflags: $CFLAGS" "configure: ${CONFIGURE[*]} ${CROSS[*]+${CROSS[*]}} $EXTRA_CONFIGURE" >"$PREFIX/BUILDINFO"
touch "$PREFIX/.done"
rm -rf "$WORK"
say "installed $PREFIX"
echo "$PREFIX"
