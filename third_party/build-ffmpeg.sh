#!/usr/bin/env bash
# Builds the minimal FFmpeg the runtime needs, as static libraries: for the
# game's PSS movies libavcodec with the MPEG-2 video decoder and the MPEG
# video parser, libswscale (YUV to RGBA) and libavutil; for a custom
# soundtrack (Mods/Music) libavformat reading local files with the Ogg,
# FLAC and MP3 demuxers and the Vorbis, FLAC and MP3 decoders (Opus would
# need swresample); for the Xbox movies (tools/modding/xbox_pack.py) the Bink
# demuxer and the Bink video and audio decoders, and for the other
# releases' movies the games play as their discs have them (BMX XXX's
# level previews, logo movie and trailers: collection/extras) the XMV
# demuxer, the WMV2 decoder and the Xbox ADPCM decoder (a trailer's
# sound); for BMX XXX's Xbox songs, which the setup copies as the disc has
# them, the ASF demuxer and the WMA 2 decoder. No
# programs, no avdevice/avfilter/swresample, no network, no external or
# autodetected libraries (no VideoToolbox, iconv, zlib, ...), LGPL only (no
# --enable-gpl / --enable-nonfree).
#
#   third_party/build-ffmpeg.sh      # prints the install prefix on stdout
#
# The release tarball is pinned by version and SHA-256 and cached with the
# build under games/aggressive/workspace/third_party-cache/ (a data link); a second run just prints the
# prefix. Pass the prefix to CMake as -DZX_FFMPEG_PREFIX=<prefix>.
#
# No path of the machine it is built on is left in the libraries: FFmpeg's assert messages name their files by
# __FILE__, which the compiler is told to name from the source folder (-ffile-prefix-map; MSVC's -d1trimfile). It
# builds in the cache's real folder (pwd -P), the one the compiler is run in, so the map matches it when the cache is
# reached through a link (1.3.1: a Windows clone's third_party-cache junction to another checkout's data, whose
# prebuilt FFmpeg had been built through a link and trimmed nothing: its .exe named three of that machine's folders).
# After the build no library may name the source folder (else no prefix is written). PATHS, the revision of that, is
# part of the cache key: a library built before it is never reused.
#
# Environment:
#   FFMPEG_CACHE   cache dir (default: <checkout>/games/aggressive/workspace/third_party-cache)
#   MIN_MACOS      macOS deployment target (default 11.0, the first macOS on Apple Silicon)
#   ARCH           target arch (default: the host's; arm64/aarch64 or x86_64)
#   JOBS           parallel make jobs
#
# Written for macOS; Linux works as is (MIN_MACOS is ignored). Windows x64:
# run it from an MSYS2 shell (bash, make, nasm) started from a Visual Studio
# x64 developer environment (cl.exe and link.exe on PATH, MSYS2_PATH_TYPE=
# inherit); it then builds with MSVC (--toolchain=msvc) against the static
# CRT (/MT), as .lib files (platform/windows/make-windows.ps1 does all of this).
set -euo pipefail

FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
FFMPEG_URL=https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz

ROOT=$(cd "$(dirname "$0")/.." && pwd)   # (the checkout)
CACHE=${FFMPEG_CACHE:-$ROOT/games/aggressive/workspace/third_party-cache}
# (2: built in the cache's real folder and checked to name no build folder; 1, unnamed: MSVC's -d1trimfile of the
# folder as reached, which a link left untrimmed)
PATHS=2
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
TRIM='-ffile-prefix-map=<source>/='   # (the compiler flag that cuts the source folder from __FILE__; its form is in the key)
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
  # named this machine's folders (winbuild1). -d1trimfile cuts it to libavcodec/... (cl matches / and \ alike), when
  # it is the folder cl runs in: the real one (the build step below), not one reached through a junction (1.3.1).
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
  --enable-demuxer=ogg,flac,mp3,bink,xmv,asf
  --enable-decoder=vorbis,flac,mp3,mp3float
  --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,wmv2
  --enable-decoder=adpcm_ima_xbox,wmav2
  --enable-parser=vorbis,flac,mpegaudio
  --enable-static --disable-shared --enable-pic
  --arch=$FF_ARCH --cc=$CC
)
KEY=$(printf '%s\n' "$FFMPEG_VERSION" "paths $PATHS" "$CFLAGS$TRIM" "$LDFLAGS" "${CONFIGURE[@]}" ${CROSS[@]+"${CROSS[@]}"} "$EXTRA_CONFIGURE" \
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
# (in the cache's real folder, links resolved: the folder the compiler runs in and names files by, which the map cuts)
mkdir -p "$CACHE/build"
WORK=$(cd "$CACHE/build" && pwd -P)/ffmpeg-$FFMPEG_VERSION-$TAG-$KEY
rm -rf "$WORK" "$PREFIX"
mkdir -p "$WORK"
say "extracting into $WORK"
tar -xJf "$TARBALL" -C "$WORK"
SRC=$WORK/ffmpeg-$FFMPEG_VERSION
# (the licence every app ships as FFMPEG-LICENSE.txt is the tree's copy of this release's: a new release with another
# text updates it)
cmp -s "$SRC/COPYING.LGPLv2.1" "$ROOT/third_party/licenses/FFmpeg-LGPL-2.1.txt" \
  || die "FFmpeg $FFMPEG_VERSION's COPYING.LGPLv2.1 is not third_party/licenses/FFmpeg-LGPL-2.1.txt, the one the apps ship"
SRC_NATIVE=$SRC
[[ $OS == Windows ]] && SRC_NATIVE=$(cygpath -m "$SRC")
CFLAGS="$CFLAGS ${TRIM/<source>/$SRC_NATIVE}"
say "configuring ($TAG)"
# shellcheck disable=SC2086
(cd "$SRC" && ./configure --prefix="$PREFIX" "${CONFIGURE[@]}" ${CROSS[@]+"${CROSS[@]}"} $EXTRA_CONFIGURE \
  --extra-cflags="$CFLAGS" --extra-ldflags="$LDFLAGS" >"$WORK/configure.log" 2>&1) \
  || { tail -30 "$WORK/configure.log" >&2; [[ -f $SRC/ffbuild/config.log ]] && tail -30 "$SRC/ffbuild/config.log" >&2; die "configure failed"; }
grep -q '^License: LGPL' "$WORK/configure.log" || die "configuration is not LGPL (see $WORK/configure.log)"
# (the configure line the libraries keep, avutil_configuration() and the others', without its folders: the prefix and the
# map name this machine's)
CONF_TEXT="${CONFIGURE[*]}${CROSS[*]+ ${CROSS[*]}}${EXTRA_CONFIGURE:+ $EXTRA_CONFIGURE}"
sed -i.orig "s|^#define FFMPEG_CONFIGURATION .*|#define FFMPEG_CONFIGURATION \"$CONF_TEXT\"|" "$SRC/config.h" && rm -f "$SRC/config.h.orig"
grep -q "^#define FFMPEG_CONFIGURATION \"$CONF_TEXT\"$" "$SRC/config.h" || die "config.h has no FFMPEG_CONFIGURATION to replace"
say "building with $JOBS jobs"
make -C "$SRC" -j"$JOBS" >"$WORK/build.log" 2>&1 || { tail -40 "$WORK/build.log" >&2; die "build failed"; }
make -C "$SRC" install >>"$WORK/build.log" 2>&1 || die "install failed"
# (no library names the cache's folders, in any of their forms, in what a program linked with it can carry: __FILE__ cut,
# the configure line without them, or the prefix is not written. Left aside: MSVC's record of each object's own path
# (its .debug$S S_OBJNAME, a printable run that ends in the object's name, .o), which cl writes even without -Z7 and
# a link without -DEBUG drops: the package's own check, tools/checks/release_check.py, reads the .exe)
FORMS=("$WORK" "$CACHE" "$(cd "$CACHE" && pwd -P)")
[[ $OS == Windows ]] && for f in "${FORMS[@]}"; do FORMS+=("$(cygpath -m "$f")" "$(cygpath -w "$f")"); done
for lib in "$PREFIX"/lib/*.a "$PREFIX"/lib/*.lib; do
  [[ -f $lib ]] || continue
  TEXT=$(LC_ALL=C tr -c '[:print:]' '\n' <"$lib" | LC_ALL=C grep -v -i -E '\.(o|obj)$' || true)
  for form in "${FORMS[@]}"; do
    if LC_ALL=C grep -i -F -q -- "$form" <<<"$TEXT"; then
      die "$(basename "$lib") names the build machine's folder $form: $(LC_ALL=C grep -i -F -m1 -- "$form" <<<"$TEXT" | cut -c1-200) (__FILE__ not cut by $TRIM, or the configure line)"
    fi
  done
done
cp "$SRC/COPYING.LGPLv2.1" "$PREFIX/" 2>/dev/null || true
# The pkg-config files name their folder relative to themselves (${pcfiledir}), never the path the build ran under: a
# prefix reached through a worktree's data link was written with that worktree's path, and every build failed once
# the worktree was removed (2026-11-23).
for pc in "$PREFIX"/lib/pkgconfig/*.pc; do
  [[ -f $pc ]] || continue
  sed -i.bak -E 's#^prefix=.*#prefix=${pcfiledir}/../..#; s#^exec_prefix=.*#exec_prefix=${prefix}#; s#^libdir=.*#libdir=${prefix}/lib#; s#^includedir=.*#includedir=${prefix}/include#' "$pc" && rm -f "$pc.bak"
done
printf '%s\n' "ffmpeg $FFMPEG_VERSION sha256 $FFMPEG_SHA256" "paths $PATHS: no library names the build folder" "cflags: $CFLAGS" "configure: ${CONFIGURE[*]} ${CROSS[*]+${CROSS[*]}} $EXTRA_CONFIGURE" >"$PREFIX/BUILDINFO"
touch "$PREFIX/.done"
rm -rf "$WORK"
say "installed $PREFIX"
echo "$PREFIX"
