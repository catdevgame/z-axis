#!/bin/zsh
# Builds dist/Aggressive Inline.app: the release runner with the app front end,
# statically linked against a minimal FFmpeg (third_party/build-ffmpeg.sh, cached
# under third_party-cache/), the game's files extracted from your disc
# image (Contents/Resources/disc; the app needs no disc image to run), an
# icon made from local files, and an ad-hoc signature. For your own use: the
# bundle holds game data, so do not share it (dist/ is ignored by git).
#
#   platform/mac/make-app.sh                 # configure + build + bundle
#   SKIP_BUILD=1 platform/mac/make-app.sh    # bundle the existing release build
#   FROM_BUILD=DIR platform/mac/make-app.sh  # bundle DIR's runner when it is this commit's, built with these arguments
#   BUILD_ONLY=1 platform/mac/make-app.sh    # the release build alone (no disc images, no bundle): the Mac gate's
#                                     # app flavour (remote/mac/gate.sh --flavour-ab), the binary the batch ships
#   STAMP_CHECK=1 [FROM_BUILD=DIR] platform/mac/make-app.sh
#                                     # the stamp this tree and these arguments give, and whether DIR's is it
#                                     # (exit 0: FROM_BUILD would build nothing; 1: it would build); nothing built
#
# Environment:
#   ZAXIS_TREE    z-axis checkout to build (default: the one this script is in)
#   BUILD_DIR    release build dir (default: $ZAXIS_TREE/out/build-release)
#   ELF          the ELF the code was recompiled from (default: recomp/config.game.toml's input);
#                its SHA-256 is built in, and the disc image's must match it
#   ISO          your disc image (default: $GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso),
#                read only; its files are extracted once into $DIST/.disc-cache and
#                cloned into the bundle. Every disc image (ISO, DM2_ISO, BMX_ISO) may be in any format: an ISO,
#                a BIN/CUE dump, a CHD, CSO or ZSO (tools/modding/disc_image.py makes the ISO bsdtar reads)
#   XBOX_DIR     optional: the Xbox release's files, extracted from your own Xbox disc
#                (its root, media or media/assets; default: $DIST/.xbox-cache when it
#                exists); tools/modding/xbox_pack.py makes the Xbox textures and movies from it
#                and the PS2 files into $DIST/.xbox-pack, bundled as Resources/xbox-pack
#   XBOX_MOVIES  0: Xbox textures only (no Bink movies, 358 MB less)
#   XBOX_ISO     optional: your Xbox disc image (default: $GAME_DISCS/
#                aggressive inline/Aggressive Inline-xbox.iso), read only; extracted once into
#                $DIST/.xbox-cache when XBOX_DIR is not set (tools/modding/xbox_files.py). The
#                files the app reads as they are (tools/modding/xbox-files.txt) and the Zoo's
#                flythrough (made with ffmpeg if there is one) go into Resources/xbox;
#                the Zoo and the Ape, made into the PS2's formats here
#                (zx-xbox-convert), into Resources/xbox-converted
#   DM2_XBOX_ISO in a collection with dm2: your Dave Mirra Freestyle BMX 2 Xbox disc image (default
#                $GAME_DISCS/Dave Mirra Freestyle BMX 2 (USA)/Dave Mirra Freestyle BMX 2
#                (USA)-xbox.iso), read only: DM2's Xbox-only levels, Venice and Greenville
#                (tools/modding/dm2_xbox.py), into Resources/games/dm2/xbox-levels, for its Free Ride
#                (Settings > Game > Xbox Levels), with the park editor's Desert and Highway and the
#                gallery's Retro movie (Xbox Themes, Retro Movie); DM2_XBOX_LEVELS=0 leaves them out; and its
#                levels' Xbox textures (zx-extras xbox-pack) into Resources/games/dm2/xbox-pack (Xbox Textures),
#                DM2_XBOX_PACK=0 leaves them out. The app never reads
#                the image. Each game runs on its own riders and levels: none is made into another's.
#   GC_IMAGE     optional: your Aggressive Inline GameCube disc image (RVZ or GCM/ISO; default
#                $GAME_DISCS/aggressive inline/Aggressive Inline-gc.rvz), read
#                only: tools/modding/gc_disc.py extracts its TUT/Estate (an earlier Tutorial left on that
#                disc) once into $DIST/.gc-cache, zx-gc-convert makes the files the game
#                loads (one 24-bit texture made 8-bit; its level-select icon and still drawn
#                from the level's own files) into Resources/estate, for Free Skate
#                (Settings > Game > Estate Level, off by default). ESTATE=0 leaves it out. The
#                app never reads the image.
#   NO_GAME_DATA 1: the app with no game data at all (no disc files, no Xbox or GameCube content, our own icon):
#                each game is set up from the player's own disc at first start (Settings > Games, or
#                --setup <disc image>). No disc image is read; the ELF is still needed for the SHA-256 built in
#   APP_VERSION  the app's version (CFBundleShortVersionString, the menu's; default: the tree's VERSION.txt; PUBLIC
#                takes that one only)
#   OPT_FLAGS    extra C++ flags for the release build (e.g. -O2), LTO=OFF to skip LTO
#                (the app is the build's app flavour, ZX_DEV_CHECKS=OFF: none of the checkers' machinery; the
#                batch proves it against the dev build, checks/flavour_ab.py)
#   DIST         output dir (default: games/aggressive/workspace/dist)
#   FROM_BUILD   a release build of this tree made with these same arguments (BUILD_ONLY's, or a
#                folder with its binaries and stamp: the Mac gate job's app-build/): its runner and tools
#                are bundled, nothing is configured or built. The build's .make-app-stamp (the tree of
#                CMakeLists.txt, runtime and iop as committed, with no change to them in the checkout;
#                the configure arguments, every path in them cut to its last part; and the data folders
#                they name by content) must be the one this run would write, else it builds as without
#                (pace1: the batch packs the binary its flavour A/B judged, not a second build). It
#                names no path, so a build from another checkout and tooling packs (pace2).
#   NINJA_JOBS   ninja -j for the builds (default: ninja's own)
#   DEPS_DIR     fetched third-party sources to reuse (default: $ZAXIS_TREE/out/build-macos/_deps)
#   MIN_MACOS    deployment target for everything built (default 11.0; 15.0 with PUBLIC)
#   PUBLIC       1: the app players download (the release's build): the minimal static FFmpeg (FFMPEG=static, no
#                dylib bundled), MIN_MACOS 15.0 (the oldest macOS the release supports) for the code and
#                LSMinimumSystemVersion alike, and NO_GAME_DATA=1; it stops if anything bundled or built needs
#                a newer macOS than MIN_MACOS
#   VU1_AOT_DIR  ahead-of-time compiled VU1 microprograms (default: recomp/generated-vu1
#                if it holds zx-vu1aot output; see z-axis docs/VU1_AOT.md)
#   FFMPEG       static (default: build/reuse the minimal static FFmpeg) or
#                system (pkg-config, e.g. Homebrew; its dylibs are copied in
#                and raise the minimum macOS to theirs)
#   BMX_XBOX_ISO, BMX_GC_IMAGE
#                in a collection with bmx: your BMX XXX Xbox disc image and GameCube image (RVZ or
#                GCM; defaults $GAME_DISCS/BMX XXX (USA)/BMX XXX-xbox.iso and
#                BMX XXX-gc.rvz), read only: their challenge scripts, fuller French, German and
#                Spanish, level-select previews, Z-Axis logo movie and the GameCube's Secret Level and
#                test levels (tools/modding/bmx_releases.py),
#                into Resources/games/bmx/releases, each behind its setting; BMX_RELEASES=0 leaves them out;
#                and from the Xbox image its levels' and props' Xbox textures (zx-extras xbox-pack) into
#                Resources/games/bmx/xbox-pack (Xbox Textures), BMX_XBOX_PACK=0 leaves them out
#   GAMES        several games in one app, e.g. GAMES="ai dm2 bmx" (default: Aggressive
#                Inline alone, as above). dm2 is Dave Mirra Freestyle BMX 2 from the
#                DM2 workspace (DM2_ROOT, default ../dm2 beside this ai/ folder:
#                its namespaced code recomp/generated-ns, recomp/generated-vu1, the ELF
#                extracted/SLUS_201.59) and your disc image DM2_ISO (default
#                DM2_ROOT/dmbmx2.iso, else $GAME_DISCS/Dave Mirra Freestyle BMX 2 (USA)/Dave Mirra
#                Freestyle BMX 2 (USA).cue, Redump's name). DM2's VU1 code comes from the micro memory image
#                in recomp/generated-vu1 (games/dm2/docs/BRINGUP.md) when there is one, else its VU1 runs on the
#                interpreter. bmx is BMX XXX: its namespaced code generated-ns and VU1
#                image generated-vu1 from paths.env's BMX_RECOMP (recompile.sh --bmx makes
#                them; BMX_ROOT given: BMX_ROOT/recomp), the ELF BMX_ELF (default
#                BMX_ROOT/disc/SLUS_204.15, BMX_ROOT paths.env's BMX_DATA, else the one
#                recompile.sh took, beside BMX_RECOMP) and your disc image BMX_ISO (default
#                $GAME_DISCS/BMX XXX (USA)/BMX XXX-ps2.iso). The app
#                ($COLLECTION_NAME, default "Z-Axis Collection") asks which game to run
#                (or --game ai|dm2|bmx) and keeps each
#                game's files in <support>/games/<id>; an existing Aggressive Inline
#                folder is copied in on first start. Built in out/build-release-collection.
set -euo pipefail

# (this script's checkout: its tools (TOOLS: tools/modding's makers, third_party's FFmpeg build) and Aggressive Inline's
# workspace, ROOT: recomp, dist, baseline; ZAXIS_TREE, the checkout built, is this one unless given)
HERE=${0:A:h:h:h}
ROOT=$HERE/games/aggressive/workspace
TOOLS=$HERE/tools/modding
ZAXIS_TREE=${ZAXIS_TREE:-$HERE}
BUILD_DIR=${BUILD_DIR:-$ZAXIS_TREE/out/build-release}
DIST=${DIST:-$ROOT/dist}
APP_NAME="Aggressive Inline"
GAMES=(${=GAMES:-ai})
COLLECTION=0
if [[ ${#GAMES} -gt 1 || ${GAMES[1]} != ai ]]; then
  COLLECTION=1
  APP_NAME=${COLLECTION_NAME:-Z-Axis Collection}
  BUILD_DIR=${BUILD_DIR_COLLECTION:-$ZAXIS_TREE/out/build-release-collection}
  BUNDLE_ID=rest.src.cat.z-axis-collection
fi
APP=$DIST/$APP_NAME.app
TREE_VERSION=$(head -1 $ZAXIS_TREE/VERSION.txt 2>/dev/null || true)   # (the collection's version, CMakeLists.txt)
APP_VERSION=${APP_VERSION:-$TREE_VERSION}
[[ -n ${BUNDLE_ID:-} ]] || BUNDLE_ID=rest.src.cat.aggressive-inline
BUILD_MIN_MACOS=${MIN_MACOS:-11.0} # 11.0 is the first macOS on Apple Silicon
FFMPEG=${FFMPEG:-static}
PUBLIC=${PUBLIC:-}
[[ $PUBLIC == 0 ]] && PUBLIC=
if [[ -n $PUBLIC ]]; then
  BUILD_MIN_MACOS=${MIN_MACOS:-15.0} # the oldest macOS the release supports
  NO_GAME_DATA=1
fi

say() { print -r -- "[make-app] $*"; }
die() { print -r -- "[make-app] error: $*" >&2; exit 1; }
[[ -n $APP_VERSION ]] || die "no VERSION.txt in $ZAXIS_TREE: give APP_VERSION"
[[ -n $PUBLIC && $APP_VERSION != $TREE_VERSION ]] && die "PUBLIC: APP_VERSION $APP_VERSION is not the tree's VERSION.txt (${TREE_VERSION:-none})"

# ---- the generated code: this tooling's set (paths.env GENERATED_SET: each game's folder with -<set> after it) ----
GENERATED_SET=${GENERATED_SET-$(source $HERE/tools/paths.sh && print -r -- $GENERATED_SET)}
gen() { if [[ -n $GENERATED_SET ]]; then print -r -- ${1:A}-$GENERATED_SET; else print -r -- $1; fi }

# ---- the ELF the generated code came from ---------------------------------
if [[ -z ${ELF:-} ]]; then
  ELF=$(sed -n 's/^input *= *"\(.*\)"/\1/p' $ROOT/recomp/config.game.toml | head -1)
  # (a relative input is the config's folder's, as ps2_recomp reads it)
  [[ -n $ELF && $ELF != /* ]] && ELF=$ROOT/recomp/$ELF
fi
[[ -f $ELF ]] || die "recompiler input ELF not found ($ELF); set ELF=<path to SLUS_203.27>"
ELF_SHA256=$(shasum -a 256 "$ELF" | cut -d' ' -f1)
ELF_NAME=${ELF:t}
say "ELF $ELF_NAME sha256 $ELF_SHA256"

# The discs and BMX XXX's data and recompiled code: paths.env's GAME_DISCS, BMX_DATA and BMX_RECOMP, unless set.
GAME_DISCS=${GAME_DISCS:-$(source $HERE/tools/paths.sh && print -r -- $GAME_DISCS)}
BMX_DATA=${BMX_DATA:-$(source $HERE/tools/paths.sh && print -r -- $BMX_DATA)}
BMX_RECOMP=${BMX_RECOMP:-$(source $HERE/tools/paths.sh && print -r -- $BMX_RECOMP)}
# extract_disc IMAGE DEST: the disc's files into DEST. An ISO through bsdtar; any other format (a BIN/CUE dump, a
# CHD, CSO or ZSO, which bsdtar does not read) made a 2048-byte ISO beside DEST first (tools/modding/disc_image.py), then that.
extract_disc() {
  local image=$1 dest=$2 rc
  if python3 -I $TOOLS/disc_image.py is-raw "$image"; then
    python3 -I $TOOLS/disc_image.py to-iso "$image" $dest.iso | sed 's/^/[make-app] /' || return 1
    bsdtar -xf $dest.iso -C $dest; rc=$?
    rm -f $dest.iso
    return $rc
  fi
  bsdtar -xf "$image" -C $dest
}
NO_GAME_DATA=${NO_GAME_DATA:-}
[[ $NO_GAME_DATA == 0 ]] && NO_GAME_DATA=
[[ -n $NO_GAME_DATA ]] && say "NO_GAME_DATA: no game data in the app; each game is set up from the player's disc at first start"

# ---- game files from the disc image ----------------------------------------
# Extracted once per image (size and modification time) and checked: the
# disc's boot ELF must be the one the code was recompiled from.
# (BUILD_ONLY: the build alone, which reads no disc image: each game's ELF and its SHA-256 only)
BUILD_ONLY=${BUILD_ONLY:-}
STAMP_CHECK=${STAMP_CHECK:-}; [[ -n $STAMP_CHECK ]] && BUILD_ONLY=1   # (the stamp alone reads no disc either)
if [[ -z $BUILD_ONLY && -z $NO_GAME_DATA ]]; then
ISO=${ISO:-$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso}
DISC_CACHE=$DIST/.disc-cache
[[ -f $ISO ]] || die "disc image not found ($ISO); set ISO=<path to your Aggressive Inline (USA) .iso>"
ISO_STAMP="$(stat -f '%z %m' "$ISO") ${ISO:t}"
if [[ ! -f $DISC_CACHE/.stamp || "$(<$DISC_CACHE/.stamp)" != "$ISO_STAMP" ]]; then
  say "extracting the disc image ${ISO:t}"
  rm -rf $DISC_CACHE $DISC_CACHE.tmp
  mkdir -p $DISC_CACHE.tmp
  extract_disc "$ISO" $DISC_CACHE.tmp || die "could not extract $ISO"
  chmod -R u+rwX,go+rX $DISC_CACHE.tmp
  xattr -cr $DISC_CACHE.tmp 2>/dev/null || true
  mv $DISC_CACHE.tmp $DISC_CACHE
fi
BOOT_ELF=$(tr -d '\r' < $DISC_CACHE/SYSTEM.CNF 2>/dev/null | sed -n 's/^BOOT2 *= *cdrom0:\\\(.*\);1 *$/\1/p' | head -1)
[[ -n $BOOT_ELF && -f $DISC_CACHE/$BOOT_ELF ]] || die "no boot ELF in $ISO (SYSTEM.CNF)"
DISC_SHA256=$(shasum -a 256 $DISC_CACHE/$BOOT_ELF | cut -d' ' -f1)
if [[ $BOOT_ELF != $ELF_NAME || $DISC_SHA256 != $ELF_SHA256 ]]; then
  die "$ISO boots $BOOT_ELF (sha256 $DISC_SHA256), not the recompiled $ELF_NAME"
fi
print -r -- "$ISO_STAMP" > $DISC_CACHE/.stamp
say "disc image ${ISO:t}: $BOOT_ELF matches, $(du -sh $DISC_CACHE | cut -f1) of game files"
fi  # (BUILD_ONLY)

# ---- Dave Mirra Freestyle BMX 2 (GAMES with dm2) ---------------------------
if (( ${GAMES[(Ie)dm2]} )); then
  DM2_ROOT=${DM2_ROOT:-$HERE/games/dm2/workspace}
  DM2_ELF=${DM2_ELF:-$DM2_ROOT/extracted/SLUS_201.59}
  [[ -f $DM2_ELF ]] || die "DM2 ELF not found ($DM2_ELF)"
  [[ -f $(gen $DM2_ROOT/recomp/generated-ns)/register_functions.cpp ]] || die "no namespaced DM2 code in $(gen $DM2_ROOT/recomp/generated-ns)"
  DM2_SHA256=$(shasum -a 256 "$DM2_ELF" | cut -d' ' -f1)
  if [[ -z $BUILD_ONLY && -z $NO_GAME_DATA ]]; then
  if [[ -z ${DM2_ISO:-} ]]; then
    DM2_ISO=$DM2_ROOT/dmbmx2.iso
    DM2_CUE="$GAME_DISCS/Dave Mirra Freestyle BMX 2 (USA)/Dave Mirra Freestyle BMX 2 (USA).cue"
    [[ ! -f $DM2_ISO && -f $DM2_CUE ]] && DM2_ISO=$DM2_CUE
  fi
  [[ -f $DM2_ISO ]] || die "DM2 disc image not found ($DM2_ISO)"
  DM2_CACHE=$DIST/.disc-cache-dm2
  DM2_STAMP="$(stat -f '%z %m' "$DM2_ISO") ${DM2_ISO:t}"
  if [[ ! -f $DM2_CACHE/.stamp || "$(<$DM2_CACHE/.stamp)" != "$DM2_STAMP" ]]; then
    say "extracting the disc image ${DM2_ISO:t}"
    rm -rf $DM2_CACHE $DM2_CACHE.tmp
    mkdir -p $DM2_CACHE.tmp
    extract_disc "$DM2_ISO" $DM2_CACHE.tmp || die "could not extract $DM2_ISO"
    chmod -R u+rwX,go+rX $DM2_CACHE.tmp
    xattr -cr $DM2_CACHE.tmp 2>/dev/null || true
    mv $DM2_CACHE.tmp $DM2_CACHE
  fi
  [[ "$(shasum -a 256 $DM2_CACHE/${DM2_ELF:t} | cut -d' ' -f1)" == $DM2_SHA256 ]] || die "$DM2_ISO does not boot the recompiled ${DM2_ELF:t}"
  print -r -- "$DM2_STAMP" > $DM2_CACHE/.stamp
  say "DM2 disc image ${DM2_ISO:t}: ${DM2_ELF:t} matches"
  fi  # (BUILD_ONLY)
fi

# ---- BMX XXX (GAMES with bmx) ---------------------------------------------
if (( ${GAMES[(Ie)bmx]} )); then
  [[ -n ${BMX_ROOT:-} ]] && BMX_RECOMP=$BMX_ROOT/recomp   # (a BMX_ROOT given: its recomp, as before)
  BMX_ROOT=${BMX_ROOT:-$BMX_DATA}
  # (the disc copy's ELF, else the one recompile.sh --bmx took from the disc, beside BMX_RECOMP)
  [[ -z ${BMX_ELF:-} && ! -f $BMX_ROOT/disc/SLUS_204.15 && -f ${BMX_RECOMP:h}/extracted/SLUS_204.15 ]] && BMX_ELF=${BMX_RECOMP:h}/extracted/SLUS_204.15
  BMX_ELF=${BMX_ELF:-$BMX_ROOT/disc/SLUS_204.15}
  [[ -f $BMX_ELF ]] || die "BMX XXX ELF not found ($BMX_ELF)"
  [[ -f $(gen $BMX_RECOMP/generated-ns)/register_functions.cpp ]] || die "no namespaced BMX XXX code in $(gen $BMX_RECOMP/generated-ns) (tools/setup/recompile.sh --bmx IMAGE makes it)"
  BMX_SHA256=$(shasum -a 256 "$BMX_ELF" | cut -d' ' -f1)
  if [[ -z $BUILD_ONLY && -z $NO_GAME_DATA ]]; then
  BMX_ISO=${BMX_ISO:-$GAME_DISCS/BMX XXX (USA)/BMX XXX-ps2.iso}
  [[ -f $BMX_ISO ]] || die "BMX XXX disc image not found ($BMX_ISO)"
  BMX_CACHE=$DIST/.disc-cache-bmx
  BMX_STAMP="$(stat -f '%z %m' "$BMX_ISO") ${BMX_ISO:t}"
  if [[ ! -f $BMX_CACHE/.stamp || "$(<$BMX_CACHE/.stamp)" != "$BMX_STAMP" ]]; then
    say "extracting the disc image ${BMX_ISO:t}"
    rm -rf $BMX_CACHE $BMX_CACHE.tmp
    mkdir -p $BMX_CACHE.tmp
    extract_disc "$BMX_ISO" $BMX_CACHE.tmp || die "could not extract $BMX_ISO"
    chmod -R u+rwX,go+rX $BMX_CACHE.tmp
    xattr -cr $BMX_CACHE.tmp 2>/dev/null || true
    mv $BMX_CACHE.tmp $BMX_CACHE
  fi
  [[ "$(shasum -a 256 $BMX_CACHE/${BMX_ELF:t} | cut -d' ' -f1)" == $BMX_SHA256 ]] || die "$BMX_ISO does not boot the recompiled ${BMX_ELF:t}"
  print -r -- "$BMX_STAMP" > $BMX_CACHE/.stamp
  say "BMX XXX disc image ${BMX_ISO:t}: ${BMX_ELF:t} matches"
  fi  # (BUILD_ONLY)
fi

# ---- the Xbox release's files (optional) -----------------------------------
# Extracted from your Xbox disc image once (the app never reads an image).
XBOX_ISO=${XBOX_ISO:-$GAME_DISCS/aggressive inline/Aggressive Inline-xbox.iso}
if [[ -n $BUILD_ONLY || -n $NO_GAME_DATA ]]; then
  :
elif [[ -z ${XBOX_DIR:-} && -f $XBOX_ISO ]]; then
  python3 -I $TOOLS/xbox_files.py extract --iso "$XBOX_ISO" --cache $DIST/.xbox-cache | sed 's/^/[make-app] /' \
    || die "could not extract the Xbox image $XBOX_ISO"
elif [[ -z ${XBOX_DIR:-} ]]; then
  say "no Xbox image ($XBOX_ISO): the Zoo is the PS2 disc's prototype, no Xbox textures or movies"
fi

# ---- Xbox textures and movies (optional) -----------------------------------
# From the Xbox release's files (extracted at build time, never read by the
# app): the Xbox picture for each PS2 texture it replaces, keyed by the PS2
# texture, and the Bink movie for each PS2 movie. Rebuilt when the inputs
# change (the tool keeps a stamp).
XBOX_DIR=${XBOX_DIR:-$DIST/.xbox-cache}
XBOX_PACK=$DIST/.xbox-pack
if [[ -n $BUILD_ONLY || -n $NO_GAME_DATA ]]; then
  :
elif [[ -d $XBOX_DIR ]]; then
  PACK_ARGS=(--ps2 $DISC_CACHE --xbox $XBOX_DIR --out $XBOX_PACK)
  [[ ${XBOX_MOVIES:-1} == 0 ]] && PACK_ARGS+=(--no-movies)
  python3 -I $TOOLS/xbox_pack.py $PACK_ARGS | sed 's/^/[make-app] /' || die "tools/modding/xbox_pack.py failed"
else
  rm -rf $XBOX_PACK
  say "no Xbox files ($XBOX_DIR): PS2 textures and movies only"
fi

# ---- release runner ------------------------------------------------------
# The binaries come from BUILD_DIR, or from FROM_BUILD when it holds this commit's build with these arguments
# (its .make-app-stamp, below); TOOLS_DIR is where they are.
TOOLS_DIR=$BUILD_DIR
NINJA=(ninja); [[ -n ${NINJA_JOBS:-} ]] && NINJA+=(-j$NINJA_JOBS)
# The stamp names no path, so a build made in one checkout and tooling (the Mac gate slot's, an integration lane's
# tooling copy) packs from another (main's) when the inputs are the same (pace2: b129's pack built again, its stamp
# held the lane's paths and each data file's time).
# args_print: the configure arguments with every absolute path in a value (each ;-separated item that exists: not
# a flag such as /EHsc) cut to its last part: where the inputs sit is not what they are. The fetched sources' folders are left out (CMakeLists.txt pins
# their versions); the FFmpeg prefix keeps its name, build-ffmpeg.sh's key of its version, flags and configure.
args_print() {
  local x v i out
  for x in "${CMAKE_ARGS[@]}"; do
    [[ $x == -DFETCHCONTENT_SOURCE_DIR_* ]] && continue
    if [[ $x == -D*=* ]]; then
      out=()
      for i in "${(@s:;:)${x#*=}}"; do [[ $i == /* && -e $i ]] && out+=("${i:t}") || out+=("$i"); done
      v="${x%%=*}=${(j:;:)out}"
    else
      v=$x
    fi
    print -r -- $v
  done
}
# data_print: the folders the arguments name (the generated code, the VU1 code, the bind tables: the build's inputs
# from outside the tree, made again by the recompiler and the work database), each by what it is, under the
# argument's name. A folder the recompiler made (its .ps2recomp-manifest: the recompiler's source id, the SHA-256 of
# the config, the ELF and the Ghidra map, each file it wrote and its size) is that manifest's SHA-256 when its top-level
# files are exactly the manifest's, with its sizes, and none is newer than the manifest: a listing, no file read (the
# generated folders are 26k and 22k files: winbuild1). Any other folder, or one that differs from its manifest (a
# file edited by hand, added or left from an older run), is the SHA-256 of its files' names and bytes, as before the
# manifests (its top-level files, as the build reads them: not the bind tables' old-* copies). The FFmpeg prefix is in
# args_print by its key. (make-windows.ps1's DataLine is the same.)
data_print() {
  local x
  local -a dirs
  for x in "${CMAKE_ARGS[@]}"; do
    [[ $x == -D*=* && $x != -DFETCHCONTENT_* && $x != -DZX_FFMPEG_PREFIX=* ]] || continue
    dirs+=("${x%%=*}=${x#*=}")
  done
  python3 -I - "${dirs[@]}" <<'PY'
import hashlib, os, sys
MANIFEST = '.ps2recomp-manifest'
def by_manifest(d, entries):
    """The manifest's SHA-256 when the folder is what it lists, else None (and why, on stderr)."""
    path = os.path.join(d, MANIFEST)
    try:
        with open(path, 'rb') as fh:
            data = fh.read()
        when = os.stat(path).st_mtime_ns
    except OSError:
        return None
    lines = data.decode('utf-8', 'replace').split('\n')
    listed = {}
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith('files ')) + 1
        for l in lines[start:]:
            if l:
                size, name = l.split(' ', 1)
                listed[name] = int(size)
    except (StopIteration, ValueError):
        print(f'make-app: {path}: not a manifest: hashing every file', file=sys.stderr)
        return None
    have = {e.name: e for e in entries if e.name != MANIFEST}
    why = None
    if have.keys() != listed.keys():
        extra, gone = sorted(have.keys() - listed.keys()), sorted(listed.keys() - have.keys())
        why = f'{len(extra)} files not in it ({", ".join(extra[:3])}), {len(gone)} of its gone ({", ".join(gone[:3])})'
    else:
        for name, e in have.items():
            st = e.stat()
            if st.st_size != listed[name]:
                why = f'{name} is {st.st_size} bytes, not {listed[name]}'
                break
            if st.st_mtime_ns > when:
                why = f'{name} is newer than the manifest'
                break
    if why:
        print(f'make-app: {d} is not what its manifest lists ({why}): hashing every file', file=sys.stderr)
        return None
    return 'manifest ' + hashlib.sha256(data).hexdigest()
for arg in sys.argv[1:]:
    name, _, value = arg.partition('=')
    for n, d in enumerate(value.split(';')):
        if not os.path.isdir(d):
            continue
        entries = sorted((e for e in os.scandir(d) if e.is_file() and e.name != '.DS_Store'), key=lambda e: e.name)
        key = by_manifest(d, entries) if any(e.name == MANIFEST for e in entries) else None
        if key is None:
            h = hashlib.sha256()
            for e in entries:
                with open(e.path, 'rb') as fh:
                    h.update(e.name.encode() + b'\0' + hashlib.file_digest(fh, 'sha256').digest())
            key = h.hexdigest()
        print(name, n, len(entries), key)
PY
}
stamp_key() { print -r -- "$1" | sed -E 's/ \([0-9a-f]+\)$//' }   # (the stamp but the commit it names)
if [[ -z ${SKIP_BUILD:-} ]]; then
  DEPS_ARGS=()
  DEPS=${DEPS_DIR:-$ZAXIS_TREE/out/build-macos/_deps}
  if [[ -d $DEPS ]]; then # reuse fetched sources: no network
    for n in elfio fmt imgui libdwarf miniz nlohmann_json rabbitizer sdl3 sse2neon toml11; do
      if [[ -d $DEPS/$n-src ]]; then
        DEPS_ARGS+=(-DFETCHCONTENT_SOURCE_DIR_${(U)n}=$DEPS/$n-src)
      fi
    done
  fi
  LTO_ARGS=()
  [[ ${LTO:-ON} == OFF ]] && LTO_ARGS=(-DZX_ENABLE_LTO=OFF)
  [[ -n $PUBLIC && $FFMPEG != static ]] && die "PUBLIC builds with FFMPEG=static (not $FFMPEG)"
  case $FFMPEG in
    static) FFMPEG_PREFIX=$(MIN_MACOS=$BUILD_MIN_MACOS $HERE/third_party/build-ffmpeg.sh) || die "FFmpeg build failed" ;;
    system) FFMPEG_PREFIX= ;;
    *) die "FFMPEG must be static or system" ;;
  esac
  say "FFmpeg: ${FFMPEG_PREFIX:-system (pkg-config)}"
  VU1_AOT_DIR=${VU1_AOT_DIR:-$ROOT/recomp/generated-vu1}
  VU1_AOT_ARGS=(-DZX_VU1_AOT_DIR=)
  # (its micro memory image, which recompile.sh makes and the build makes the code from, or code made from one)
  if [[ -n $(print -l $VU1_AOT_DIR/*.vu1code(N) $VU1_AOT_DIR/vu1_aot_*.cpp(N)) ]]; then
    VU1_AOT_ARGS=(-DZX_VU1_AOT_DIR=$VU1_AOT_DIR)
    say "VU1 ahead-of-time code: $VU1_AOT_DIR"
  else
    say "no VU1 ahead-of-time code in $VU1_AOT_DIR (VU1 runs on the interpreter)"
  fi
  COLLECTION_ARGS=()
  if (( COLLECTION )); then
    APP_GAMES="ai|Aggressive Inline|Aggressive Inline (USA)|$ELF_NAME|$ELF_SHA256|BASLUS-20327CEAIGAME"
    EXTRA_GENERATED=() EXTRA_VU1=()
    if (( ${GAMES[(Ie)dm2]} )); then
      APP_GAMES+=";dm2|Dave Mirra Freestyle BMX 2|Dave Mirra Freestyle BMX 2 (USA)|${DM2_ELF:t}|$DM2_SHA256|BASLUS-20159BMX2GAME/BMX2SAVE.DAT,BASLUS-20159BMX2PARK,BASLUS-20159BMX2RPLY"
      EXTRA_GENERATED+=($(gen $DM2_ROOT/recomp/generated-ns))
      # (VU1 ahead of time from DM2's micro memory image, or code made from it, when there is one: made by a run of
      # the game, games/dm2/docs/BRINGUP.md "VU1 AOT"; else DM2's VU1 runs on the interpreter)
      if [[ -n $(print -l $DM2_ROOT/recomp/generated-vu1/*.vu1code(N) $DM2_ROOT/recomp/generated-vu1/vu1_aot_*.cpp(N)) ]]; then
        EXTRA_VU1+=($DM2_ROOT/recomp/generated-vu1)
      else
        say "no DM2 VU1 micro memory image in $DM2_ROOT/recomp/generated-vu1: DM2's VU1 runs on the interpreter"
      fi
    fi
    if (( ${GAMES[(Ie)bmx]} )); then
      APP_GAMES+=";bmx|BMX XXX|BMX XXX (USA)|${BMX_ELF:t}|$BMX_SHA256|BASLUS-20415BMXXXXGM"
      EXTRA_GENERATED+=($(gen $BMX_RECOMP/generated-ns))
      # (VU1 ahead of time from BMX XXX's micro memory image, which tools/setup/recompile.sh --bmx makes from the ELF:
      # else BMX's VU1 runs on the interpreter)
      if [[ -n $(print -l $BMX_RECOMP/generated-vu1/*.vu1code(N)) ]]; then
        EXTRA_VU1+=($BMX_RECOMP/generated-vu1)
      else
        say "no BMX XXX VU1 micro memory image in $BMX_RECOMP/generated-vu1: BMX XXX's VU1 runs on the interpreter"
      fi
    fi
    (( ${#EXTRA_GENERATED} )) && COLLECTION_ARGS+=("-DZX_RUNNER_EXTRA_GENERATED_DIRS=${(j:;:)EXTRA_GENERATED}")
    (( ${#EXTRA_VU1} )) && COLLECTION_ARGS+=("-DZX_VU1_AOT_EXTRA_DIRS=${(j:;:)EXTRA_VU1}")
    COLLECTION_ARGS+=("-DZX_APP_GAMES=$APP_GAMES")
    # The other games' bind tables: without them each game's instance of the shared readable engine is not compiled, and
    # DM2 and BMX XXX run the engine as recompiled code (the batch's build-coll binds them). The checkout's own,
    # tools/readable/bind (committed since tools39; game.cmake builds with those whatever ZX_BIND_DIR names), else the
    # shared folder of a commit from before (zbind, paths.sh: paths.env GAME_BIND_LEGACY), else BIND_DIR.
    BIND_DIR=${BIND_DIR:-$(source $HERE/tools/paths.sh && zbind $ZAXIS_TREE)}
    [[ -f $ZAXIS_TREE/tools/readable/bind/dm2.tsv ]] && BIND_DIR=$ZAXIS_TREE/tools/readable/bind
    [[ -n $BIND_DIR && -d $BIND_DIR ]] || die "no bind tables for the collection (BIND_DIR, or the checkout's tools/readable/bind)"
    COLLECTION_ARGS+=("-DZX_BIND_DIR=$BIND_DIR")
    say "collection: ${GAMES[*]} (bind tables $BIND_DIR)"
  fi
  CMAKE_ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release "${COLLECTION_ARGS[@]}" \
    -DZX_RUNNER_GENERATED_DIR=$(gen $ROOT/recomp/generated) "${DEPS_ARGS[@]}" "${LTO_ARGS[@]}" "${VU1_AOT_ARGS[@]}" \
    -DCMAKE_CXX_FLAGS="${OPT_FLAGS:-}" \
    -DZX_BUILD_RECOMPILER=OFF -DZX_BUILD_TEST=OFF -DZX_DEV_CHECKS=OFF \
    -DZX_ENABLE_RUNTIME_LOGS=OFF -DZX_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$BUILD_MIN_MACOS -DZX_FFMPEG_PREFIX=$FFMPEG_PREFIX \
    "-DZX_APP_NAME=$APP_NAME" "-DZX_APP_GAME_TITLE=Aggressive Inline (USA)" \
    -DZX_APP_ELF=$ELF_NAME -DZX_APP_ELF_SHA256=$ELF_SHA256 \
    -DZX_APP_SAVE_DIR=BASLUS-20327CEAIGAME -DZX_APP_VERSION=$APP_VERSION)
  # The stamp a build with these arguments of this tree carries: the build's inputs as committed (STAMP_PATHS: the
  # app builds no tests and no recompiler, so a commit that differs only in the workshop or
  # the docs, the integration lane's judged head and the batch's merge, is the same build), none when they have an
  # uncommitted or untracked change (then nothing is taken from FROM_BUILD); the arguments with no path (args_print;
  # -S and -B are not in CMAKE_ARGS); and the data folders by content (data_print).
  STAMP=
  # (the folders the build reads: the program's sources, and the tools and tables it runs and builds from)
  STAMP_PATHS=(CMakeLists.txt engine ps2 games collection platform iop third_party tools/readable tools/setup
               tools/modding tools/checks)
  if [[ -z "$(git -C $ZAXIS_TREE status --porcelain -- $STAMP_PATHS 2>/dev/null)" ]] \
      && TREE=$(git -C $ZAXIS_TREE rev-parse ${STAMP_PATHS/#/HEAD:} 2>/dev/null); then
    STAMP="tree $(print -r -- $TREE | shasum -a 256 | cut -c1-16) ($(git -C $ZAXIS_TREE rev-parse --short HEAD))
args $(args_print | shasum -a 256 | cut -c1-16)
data $(data_print | shasum -a 256 | cut -c1-16)"
  fi
  if [[ -n $STAMP_CHECK ]]; then
    print -r -- "${STAMP:-no stamp: uncommitted changes to $STAMP_PATHS in $ZAXIS_TREE}"
    [[ -z ${FROM_BUILD:-} ]] && exit 0
    if [[ -n $STAMP && -f $FROM_BUILD/.make-app-stamp && "$(stamp_key "$(<$FROM_BUILD/.make-app-stamp)")" == "$(stamp_key "$STAMP")" ]]; then
      say "the release build in $FROM_BUILD ($(head -1 $FROM_BUILD/.make-app-stamp), these arguments): nothing built"; exit 0
    fi
    say "FROM_BUILD $FROM_BUILD is not this tree's build with these arguments (its stamp: $(tr '\n' ' ' < $FROM_BUILD/.make-app-stamp 2>/dev/null)): it would build"; exit 1
  elif [[ -n ${FROM_BUILD:-} && -n $BUILD_ONLY ]]; then
    die "FROM_BUILD and BUILD_ONLY: one or the other"
  elif [[ -n ${FROM_BUILD:-} && -n $STAMP && -f $FROM_BUILD/.make-app-stamp && "$(stamp_key "$(<$FROM_BUILD/.make-app-stamp)")" == "$(stamp_key "$STAMP")" ]]; then
    TOOLS_DIR=$FROM_BUILD
    say "the release build in $FROM_BUILD ($(head -1 $FROM_BUILD/.make-app-stamp), these arguments): nothing built"
  else
    [[ -n ${FROM_BUILD:-} ]] && say "FROM_BUILD $FROM_BUILD is not this tree's build with these arguments$([[ -z $STAMP ]] && print ' (uncommitted changes to the build here)'): building"
    rm -f $BUILD_DIR/.make-app-stamp
    cmake -S $ZAXIS_TREE -B $BUILD_DIR "${CMAKE_ARGS[@]}" >/dev/null
    # (BUILD_ONLY: the tools the bundle runs too, so FROM_BUILD needs nothing built)
    TARGETS=(zx-runner); [[ -n $BUILD_ONLY ]] && TARGETS+=(zx-native-assets zx-xbox-convert zx-gc-convert zx-extras)
    $NINJA -C $BUILD_DIR $TARGETS
    [[ -n $STAMP ]] && print -r -- "$STAMP" > $BUILD_DIR/.make-app-stamp
  fi
fi
RUNNER=$TOOLS_DIR/runtime/zx-runner
[[ -x $RUNNER ]] || die "no runner at $RUNNER"
if [[ -n $BUILD_ONLY ]]; then
  say "built $RUNNER$([[ -f $BUILD_DIR/.make-app-stamp ]] && print " ($(head -1 $BUILD_DIR/.make-app-stamp))")"
  exit 0
fi
# tool NAME: a build tool the bundle runs, from TOOLS_DIR (built in BUILD_DIR when it is BUILD_DIR's)
tool() {
  if [[ $TOOLS_DIR == $BUILD_DIR ]]; then
    $NINJA -C $BUILD_DIR $1 >/dev/null || die "$1 build failed"
  fi
  [[ -x $TOOLS_DIR/runtime/$1 ]] || die "no $1 in $TOOLS_DIR"
  print -r -- $TOOLS_DIR/runtime/$1
}

# ---- Xbox skaters (optional) ----------------------------------------------
# The Xbox release's more detailed skaters, made here into the PS2's model
# format (the PS2 file with the Xbox geometry; zx-native-assets --xbox-models,
# engine/zge/native/xbox_models.h) under the pack's models/; the
# app only loads them (Settings > Display > Xbox Skaters).
if [[ -z $NO_GAME_DATA && -d $XBOX_DIR && -d $XBOX_PACK ]]; then
  ZX_NATIVE_ASSETS=$(tool zx-native-assets)
  rm -rf $XBOX_PACK/models
  $ZX_NATIVE_ASSETS --xbox-models $XBOX_PACK --ps2 $DISC_CACHE --xbox $XBOX_DIR \
    | sed 's/^/[make-app] /' || die "zx-native-assets --xbox-models failed"
fi

# ---- the Zoo and the Ape (optional) -----------------------------------------
# The Xbox release's Zoo (level, split screen, ambient NPCs) and Ape made here
# into the PS2's formats, with the Xbox originals of their textures for the
# GPU renderer (zx-xbox-convert, runtime/zaxis_zoo.h and
# zaxis_ape.h); the app only loads them (Resources/xbox-converted).
# Without the Xbox files the Zoo is the PS2 disc's prototype and there is no Ape.
XBOX_CONVERTED=$DIST/.xbox-converted
rm -rf $XBOX_CONVERTED
if [[ -z $NO_GAME_DATA && -d $XBOX_DIR ]]; then
  ZX_XBOX_CONVERT=$(tool zx-xbox-convert)
  $ZX_XBOX_CONVERT --xbox $XBOX_DIR --ps2 $DISC_CACHE --out $XBOX_CONVERTED \
    | sed 's/^/[make-app] /' || die "zx-xbox-convert could not make the Zoo and the Ape from $XBOX_DIR"
fi

# ---- bundle layout -------------------------------------------------------
say "assembling $APP"
rm -rf $APP
mkdir -p $APP/Contents/{MacOS,Frameworks,Resources}
BIN=$APP/Contents/MacOS/$APP_NAME
cp $RUNNER $BIN
chmod u+w $BIN
FW=$APP/Contents/Frameworks

# ---- dylibs: copy every non-system dependency, point references at the bundle
# (none with the static FFmpeg; kept for FFMPEG=system)
typeset -A ORIGIN   # bundled name -> original file
queue=("$BIN")
resolve() { # dep, original path of the image that references it
  local dep=$1 from=$2
  case $dep in
    @loader_path/*) print -r -- ${from:h}/${dep#@loader_path/} ;;
    @rpath/*)       print -r -- ${from:h}/${dep#@rpath/} ;;
    *)              print -r -- $dep ;;
  esac
}
while (( ${#queue} )); do
  f=${queue[1]}; queue=("${(@)queue[2,-1]}")
  orig=${ORIGIN[${f:t}]:-$f}
  for dep in ${(f)"$(otool -L $f | tail -n +2 | awk '{print $1}')"}; do
    case $dep in
      /usr/lib/*|/System/*) continue ;;
      @executable_path/*) continue ;;
    esac
    [[ $dep == @loader_path/* && $f != $BIN && -n ${ORIGIN[${dep:t}]:-} ]] && continue
    name=${dep:t}
    [[ $name == ${f:t} ]] && continue # its own id
    if [[ -z ${ORIGIN[$name]:-} ]]; then
      src=$(resolve $dep $orig)
      [[ -f $src ]] || die "cannot find $dep (needed by ${orig})"
      src=${src:A}
      cp $src $FW/$name
      chmod u+w $FW/$name
      install_name_tool -id @rpath/$name $FW/$name 2>/dev/null
      ORIGIN[$name]=$src
      queue+=("$FW/$name")
    fi
    if [[ $f == $BIN ]]; then
      install_name_tool -change $dep @executable_path/../Frameworks/$name $f 2>/dev/null
    else
      install_name_tool -change $dep @loader_path/$name $f 2>/dev/null
    fi
  done
  # Drop Homebrew run paths.
  for rp in ${(f)"$(otool -l $f | awk '/LC_RPATH/{getline; getline; print $2}')"}; do
    [[ -n $rp && $rp != @* ]] && install_name_tool -delete_rpath $rp $f 2>/dev/null || true
  done
done
say "bundled ${#ORIGIN} dylibs"
# The app cannot run on anything older than the newest minos in it (Homebrew
# bottles are built for the macOS they were bottled on).
MIN_MACOS=$BUILD_MIN_MACOS
for f in $BIN $FW/*.dylib(N); do
  v=$(otool -l $f | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')
  [[ -n $v ]] && MIN_MACOS=$(printf '%s\n%s\n' $MIN_MACOS $v | sort -V | tail -1)
done
say "minimum macOS $MIN_MACOS"

# ---- icon (from local files only; nothing new is committed) --------------
# The local game-art icon when there is one (baseline/, never committed), else the repository's own
# (collection/resources/app-icon.png); NO_GAME_DATA always the repository's.
ICON_SRC_DIR=$ROOT/baseline/icon
ICON_PNG=$ICON_SRC_DIR/square.png
[[ -n $NO_GAME_DATA || ! -f $ICON_PNG ]] && ICON_PNG=$ZAXIS_TREE/collection/resources/app-icon.png
ICON_OK=0
TMP=$(mktemp -d)
trap 'rm -rf $TMP' EXIT
if [[ -f $ICON_PNG ]]; then
  ICONSET=$TMP/AppIcon.iconset
  mkdir -p $ICONSET
  for s in 16 32 128 256 512; do
    sips -z $s $s $ICON_PNG --out $ICONSET/icon_${s}x${s}.png >/dev/null
    sips -z $((s*2)) $((s*2)) $ICON_PNG --out $ICONSET/icon_${s}x${s}@2x.png >/dev/null
  done
  iconutil -c icns $ICONSET -o $APP/Contents/Resources/AppIcon.icns && ICON_OK=1 && say "icon: ${ICON_PNG#$ZAXIS_TREE/}"
elif [[ -f "$ROOT/baseline/Aggressive Inline.app/Contents/Resources/AppIcon.icns" ]]; then
  cp "$ROOT/baseline/Aggressive Inline.app/Contents/Resources/AppIcon.icns" $APP/Contents/Resources/AppIcon.icns && ICON_OK=1
fi
(( ICON_OK )) || say "no icon source found; building without an icon"

# ---- the app's own font (no game data) ---------------------------------------
# Michroma (collection/resources/fonts, SIL Open Font License 1.1: the TTF unchanged, its licence beside it), the
# collection menu's text before Aggressive Inline's disc is set up (collection/vector_font.h).
mkdir -p $APP/Contents/Resources/fonts
cp $ZAXIS_TREE/collection/resources/fonts/Michroma-Regular.ttf $ZAXIS_TREE/collection/resources/fonts/Michroma-OFL.txt \
  $APP/Contents/Resources/fonts/

# ---- the licences -------------------------------------------------------------
# The collection's (LICENSE, GPL-3.0), its third-party parts' (NOTICE) and the licence of the FFmpeg the runner links
# (FFMPEG-LICENSE.txt: FFmpeg's own COPYING.LGPLv2.1, third_party/licenses), as every package carries them.
cp $ZAXIS_TREE/LICENSE $ZAXIS_TREE/NOTICE $APP/Contents/Resources/
cp $ZAXIS_TREE/third_party/licenses/FFmpeg-LGPL-2.1.txt $APP/Contents/Resources/FFMPEG-LICENSE.txt

if [[ -z $NO_GAME_DATA ]]; then
# ---- game files (clones on APFS: no extra space, no copy time) -------------
# A collection: each game's files in Resources/games/<id>.
GAME_RES=$APP/Contents/Resources
(( COLLECTION )) && GAME_RES=$APP/Contents/Resources/games/ai && mkdir -p $GAME_RES
cp -cR $DISC_CACHE $GAME_RES/disc 2>/dev/null || cp -R $DISC_CACHE $GAME_RES/disc
rm -f $GAME_RES/disc/.stamp
if (( COLLECTION )) && (( ${GAMES[(Ie)dm2]} )); then
  mkdir -p $APP/Contents/Resources/games/dm2
  cp -cR $DM2_CACHE $APP/Contents/Resources/games/dm2/disc 2>/dev/null || cp -R $DM2_CACHE $APP/Contents/Resources/games/dm2/disc
  rm -f $APP/Contents/Resources/games/dm2/disc/.stamp
  # DM2's Xbox-only levels (Venice, Greenville) for its Free Ride, made from
  # its Xbox disc image (tools/modding/dm2_xbox.py; the app never reads
  # the image), their textures within dm2_xbox.py's default texture budget
  # (DM2's texture cache is AI's: it does not check that a frame's textures
  # fit GS memory). DM2_XBOX_LEVELS=0 leaves them out.
  DM2_XBOX_ISO=${DM2_XBOX_ISO:-$GAME_DISCS/Dave Mirra Freestyle BMX 2 (USA)/Dave Mirra Freestyle BMX 2 (USA)-xbox.iso}
  if [[ ${DM2_XBOX_LEVELS:-1} != 0 && -f $DM2_XBOX_ISO ]]; then
    python3 -I $TOOLS/xbox_files.py extract --iso "$DM2_XBOX_ISO" --cache $DIST/.dm2-xbox-cache | sed 's/^/[make-app] /' \
      || die "could not extract the DM2 Xbox image $DM2_XBOX_ISO"
    rm -rf $DIST/.dm2-xbox-levels
    # With them: the park editor's Desert and Highway (with the PS2 disc's
    # Beach theme's light points), the attract demo's replays the PS2
    # disc lacks and the gallery's Retro movie (with
    # ffmpeg), each behind its own setting.
    python3 -I $TOOLS/dm2_xbox.py convert --xbox $DIST/.dm2-xbox-cache --ps2 $DM2_CACHE --out $DIST/.dm2-xbox-levels \
      | sed 's/^/[make-app] /' || die "dm2_xbox.py could not make DM2's Xbox levels from $DM2_XBOX_ISO"
    cp -R $DIST/.dm2-xbox-levels $APP/Contents/Resources/games/dm2/xbox-levels
    say "DM2's Xbox levels for its Free Ride: $(du -sh $APP/Contents/Resources/games/dm2/xbox-levels | cut -f1)"
  else
    say "no DM2 Xbox image ($DM2_XBOX_ISO) or DM2_XBOX_LEVELS=0: no Venice and Greenville in DM2"
  fi
  # DM2's Xbox textures: its levels' from the Xbox disc image, made as the extras' setup makes them (zx-extras
  # xbox-pack --no-movies; collection/extras/setup.cpp), into Resources/games/dm2/xbox-pack, where the app reads a
  # bundled game's pack (Xbox Textures). DM2_XBOX_PACK=0 leaves them out.
  if [[ ${DM2_XBOX_PACK:-1} != 0 && -f $DM2_XBOX_ISO ]]; then
    ZX_EXTRAS=$(tool zx-extras)
    rm -rf $DIST/.dm2-xbox-pack
    $ZX_EXTRAS xbox-pack --ps2 $DM2_CACHE --xbox "$DM2_XBOX_ISO" --out $DIST/.dm2-xbox-pack --no-movies \
      | sed 's/^/[make-app] /' || die "zx-extras xbox-pack could not make DM2's Xbox textures from $DM2_XBOX_ISO"
    cp -cR $DIST/.dm2-xbox-pack $APP/Contents/Resources/games/dm2/xbox-pack 2>/dev/null \
      || cp -R $DIST/.dm2-xbox-pack $APP/Contents/Resources/games/dm2/xbox-pack
    say "DM2's Xbox textures: $(du -sh $APP/Contents/Resources/games/dm2/xbox-pack | cut -f1)"
  fi
fi
if (( COLLECTION )) && (( ${GAMES[(Ie)bmx]} )); then
  mkdir -p $APP/Contents/Resources/games/bmx
  cp -cR $BMX_CACHE $APP/Contents/Resources/games/bmx/disc 2>/dev/null || cp -R $BMX_CACHE $APP/Contents/Resources/games/bmx/disc
  rm -f $APP/Contents/Resources/games/bmx/disc/.stamp
  say "BMX XXX game files in Resources/games/bmx/disc: $(du -sh $APP/Contents/Resources/games/bmx/disc | cut -f1)"
  # BMX XXX's Xbox and GameCube releases' content made into its PS2 formats
  # (tools/modding/bmx_releases.py; the app never reads the images): their challenge
  # scripts, the fuller French, German and Spanish string tables, the level
  # select's preview movies, the Z-Axis logo movie, the Turok and Vexx
  # trailers and the unused songs (with ffmpeg), the GameCube's Secret Level
  # and three test levels, create-a-rider's textures and parts, each behind
  # its own setting. BMX_RELEASES=0 leaves them out.
  BMX_XBOX_ISO=${BMX_XBOX_ISO:-$GAME_DISCS/BMX XXX (USA)/BMX XXX-xbox.iso}
  BMX_GC_IMAGE=${BMX_GC_IMAGE:-$GAME_DISCS/BMX XXX (USA)/BMX XXX-gc.rvz}
  if [[ ${BMX_RELEASES:-1} != 0 && ( -f $BMX_XBOX_ISO || -f $BMX_GC_IMAGE ) ]]; then
    REL_ARGS=(--ps2 $BMX_CACHE --out $DIST/.bmx-releases)
    if [[ -f $BMX_XBOX_ISO ]]; then
      python3 -I $TOOLS/xbox_files.py extract --iso "$BMX_XBOX_ISO" --cache $DIST/.bmx-xbox-cache | sed 's/^/[make-app] /' \
        || die "could not extract the BMX XXX Xbox image $BMX_XBOX_ISO"
      REL_ARGS+=(--xbox $DIST/.bmx-xbox-cache)
    fi
    if [[ -f $BMX_GC_IMAGE ]]; then
      BMX_GC_CACHE=$DIST/.bmx-gc-cache
      # (the files taken are in the stamp: a cache of fewer is read again)
      BMX_GC_FILES=(assets/string.zal assets/editor/object.obj 'assets/levels/*/objctv.txt' assets/levels/cmp_scrt
                    assets/levels/tst_flat assets/levels/tst_grnd assets/levels/tst_vert assets/create/parts.zal
                    'assets/create/texture?.zal')
      BMX_GC_STAMP="$(stat -f '%z %m' "$BMX_GC_IMAGE") ${BMX_GC_IMAGE:t} ${BMX_GC_FILES[*]}"
      if [[ ! -f $BMX_GC_CACHE/.stamp || "$(<$BMX_GC_CACHE/.stamp)" != "$BMX_GC_STAMP" ]]; then
        rm -rf $BMX_GC_CACHE
        python3 -I $TOOLS/gc_disc.py extract "$BMX_GC_IMAGE" --out $BMX_GC_CACHE $BMX_GC_FILES \
          | sed 's/^/[make-app] /' || die "could not read the BMX XXX GameCube image $BMX_GC_IMAGE"
        print -r -- "$BMX_GC_STAMP" > $BMX_GC_CACHE/.stamp
      fi
      REL_ARGS+=(--gc $BMX_GC_CACHE)
    fi
    rm -rf $DIST/.bmx-releases
    python3 -I $TOOLS/bmx_releases.py convert $REL_ARGS | sed 's/^/[make-app] /' \
      || die "bmx_releases.py could not make BMX XXX's other releases' content"
    cp -R $DIST/.bmx-releases $APP/Contents/Resources/games/bmx/releases
    say "BMX XXX's Xbox and GameCube content: $(du -sh $APP/Contents/Resources/games/bmx/releases | cut -f1)"
  else
    say "no BMX XXX Xbox or GameCube image, or BMX_RELEASES=0: BMX XXX as its PS2 release alone"
  fi
  # BMX XXX's Xbox textures: its levels' and their props' (the ZAL archives) from the Xbox disc image, made as the
  # extras' setup makes them (zx-extras xbox-pack --no-movies --archives), into Resources/games/bmx/xbox-pack, where
  # the app reads a bundled game's pack (Xbox Textures). BMX_XBOX_PACK=0 leaves them out.
  if [[ ${BMX_XBOX_PACK:-1} != 0 && -f $BMX_XBOX_ISO ]]; then
    ZX_EXTRAS=$(tool zx-extras)
    rm -rf $DIST/.bmx-xbox-pack
    $ZX_EXTRAS xbox-pack --ps2 $BMX_CACHE --xbox "$BMX_XBOX_ISO" --out $DIST/.bmx-xbox-pack --no-movies --archives \
      | sed 's/^/[make-app] /' || die "zx-extras xbox-pack could not make BMX XXX's Xbox textures from $BMX_XBOX_ISO"
    cp -cR $DIST/.bmx-xbox-pack $APP/Contents/Resources/games/bmx/xbox-pack 2>/dev/null \
      || cp -R $DIST/.bmx-xbox-pack $APP/Contents/Resources/games/bmx/xbox-pack
    say "BMX XXX's Xbox textures: $(du -sh $APP/Contents/Resources/games/bmx/xbox-pack | cut -f1)"
  fi
fi
if [[ -d $XBOX_DIR ]]; then
  python3 -I $TOOLS/xbox_files.py bundle --cache $XBOX_DIR --out $DIST/.xbox-files | sed 's/^/[make-app] /' \
    || die "tools/modding/xbox_files.py bundle failed"
  cp -cR $DIST/.xbox-files $GAME_RES/xbox 2>/dev/null || cp -R $DIST/.xbox-files $GAME_RES/xbox
  say "Xbox files the app reads: $(du -sh $GAME_RES/xbox | cut -f1)"
fi
if [[ -d $XBOX_CONVERTED ]]; then
  cp -cR $XBOX_CONVERTED $GAME_RES/xbox-converted 2>/dev/null || cp -R $XBOX_CONVERTED $GAME_RES/xbox-converted
  say "the Zoo and the Ape made from the Xbox files: $(du -sh $GAME_RES/xbox-converted | cut -f1)"
fi
# TUT/Estate, an earlier Tutorial left on the GameCube disc, for Free Skate:
# extracted from your GameCube image (tools/modding/gc_disc.py; once per image) and
# made into the files the game loads (zx-gc-convert); the app never reads the
# image.
GC_IMAGE=${GC_IMAGE:-$GAME_DISCS/aggressive inline/Aggressive Inline-gc.rvz}
if [[ ${ESTATE:-1} != 0 && -f $GC_IMAGE ]]; then
  GC_CACHE=$DIST/.gc-cache
  GC_STAMP="$(stat -f '%z %m' "$GC_IMAGE") ${GC_IMAGE:t}"
  if [[ ! -f $GC_CACHE/.stamp || "$(<$GC_CACHE/.stamp)" != "$GC_STAMP" ]]; then
    rm -rf $GC_CACHE
    python3 -I $TOOLS/gc_disc.py extract "$GC_IMAGE" --out $GC_CACHE assets/levels/TUT/Estate | sed 's/^/[make-app] /' \
      || die "could not extract TUT/Estate from the GameCube image $GC_IMAGE"
    print -r -- "$GC_STAMP" > $GC_CACHE/.stamp
  fi
  ZX_GC_CONVERT=$(tool zx-gc-convert)
  rm -rf $DIST/.estate
  $ZX_GC_CONVERT --gc $GC_CACHE --out $DIST/.estate | sed 's/^/[make-app] /' \
    || die "zx-gc-convert could not make Estate from $GC_IMAGE"
  cp -R $DIST/.estate $GAME_RES/estate
  say "Estate (the GameCube disc's earlier Tutorial) for Free Skate: $(du -sh $GAME_RES/estate | cut -f1)"
else
  say "no GameCube image ($GC_IMAGE) or ESTATE=0: no Estate level in Free Skate"
fi

# The Thor's bottom screen (panel_map.py): each level's top-down map
# and goals, the game's fonts and icons, from the files bundled above. The
# running game reads them and converts nothing; game data, never committed.
PANEL_ARGS=()
[[ -d $GAME_RES/xbox ]] && PANEL_ARGS+=(--xbox $GAME_RES/xbox)
[[ -d $XBOX_CONVERTED/ASSETS/LEVELS/ZOO ]] && PANEL_ARGS+=(--zoo $XBOX_CONVERTED/ASSETS/LEVELS/ZOO)
[[ -d $GAME_RES/estate ]] && PANEL_ARGS+=(--estate $GAME_RES/estate)
PYTHONDONTWRITEBYTECODE=1 python3 -I $TOOLS/panel_map.py build --disc $GAME_RES/disc --out $GAME_RES/panel \
  "${PANEL_ARGS[@]}" | grep -v ' faces, ' | sed 's/^/[make-app] /' || die "panel_map.py could not make the panel's level files"
say "the Thor panel's maps and goals: $(du -sh $GAME_RES/panel | cut -f1)"
if [[ -d $XBOX_PACK ]]; then
  cp -cR $XBOX_PACK $GAME_RES/xbox-pack 2>/dev/null || cp -R $XBOX_PACK $GAME_RES/xbox-pack
  rm -f $GAME_RES/xbox-pack/.stamp
  say "Xbox textures and movies: $(du -sh $GAME_RES/xbox-pack | cut -f1)"
fi

fi  # (NO_GAME_DATA)

# ---- Info.plist ----------------------------------------------------------
BUILD_NUMBER=$(git -C $ROOT rev-list --count HEAD 2>/dev/null || echo 1)
# (PUBLIC: what players get runs on MIN_MACOS and bundles no dylib)
if [[ -n $PUBLIC ]]; then
  (( ${#ORIGIN} == 0 )) || die "PUBLIC: no dylib may be bundled, found ${(k)ORIGIN} (a build with FFMPEG=system?)"
  [[ $MIN_MACOS == $BUILD_MIN_MACOS ]] || die "PUBLIC: the app needs macOS $MIN_MACOS, not $BUILD_MIN_MACOS"
fi
cat > $APP/Contents/Info.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleShortVersionString</key><string>$APP_VERSION</string>
  <key>CFBundleVersion</key><string>$BUILD_NUMBER</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>$APP_NAME</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>LSMinimumSystemVersion</key><string>$MIN_MACOS</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.sports-games</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
  <key>GCSupportedGameControllers</key>
  <array>
    <dict><key>ProfileName</key><string>ExtendedGamepad</string></dict>
  </array>
  <key>NSHumanReadableCopyright</key><string>Native port built with PS2Recomp, with the game files of your own disc.</string>
</dict>
</plist>
EOF
plutil -lint $APP/Contents/Info.plist >/dev/null

# ---- sign (ad hoc) and check ------------------------------------------------
for lib in $FW/*.dylib(N); do codesign --force --sign - --timestamp=none $lib 2>/dev/null; done
codesign --force --sign - --timestamp=none $APP
codesign --verify --strict $APP || die "signature check failed"

if otool -L $BIN $FW/*.dylib(N) | grep -E '/opt/homebrew|/usr/local' >/dev/null; then
  otool -L $BIN $FW/*.dylib(N) | grep -E '/opt/homebrew|/usr/local' >&2
  die "the bundle still references Homebrew"
fi
if otool -l $BIN $FW/*.dylib(N) | grep -A2 LC_RPATH | grep -E '/opt/homebrew|/usr/local' >/dev/null; then
  die "the bundle still has a Homebrew run path"
fi
say "done: $APP ($(du -sh $APP | cut -f1))"
