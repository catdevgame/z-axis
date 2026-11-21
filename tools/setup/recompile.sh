#!/bin/zsh
# recompile.sh [--ai IMAGE] [--dm2 IMAGE] [--bmx IMAGE]: each game's code made from your own disc, before the
# first app build (README "Building"). For each game given (default: each whose image is where make-app.sh
# looks for it, under paths.env's GAME_DISCS):
#   1. its boot ELF taken from the disc image (any game in any format: an ISO, a BIN/CUE dump by its CUE or BIN, a
#      CHD, a CSO or a ZSO, read by the app's own reader, zx-disc) into its workspace's extracted/
#      (games/aggressive/workspace, games/dm2/workspace), and checked to be the USA release the code is for;
#   2. its recompiler config made (recomp/tools/make-config.py) and the recompiler run: recomp/generated (AI),
#      recomp/generated-ns (DM2, the collection's namespaced code), and its VU1 micro memory image into
#      recomp/generated-vu1 (the tree's tools/setup/vu1_image.py). Seconds a game.
#   BMX XXX's is its workspace's one step, games/bmx/workspace/recomp/recompile.py: its ELF from the disc, the configs, the
#   namespaced code, its VU0 upload and VU1 micro memory image (both read from the ELF), into paths.env's BMX_RECOMP.
# ps2_recomp and zx-disc are built first when BUILD (default <checkout>/out/build-recomp) has them not: CMake,
# Ninja, the compiler (README), network once for the fetched sources. NINJA_JOBS (default 6).
emulate -L zsh
setopt err_return pipe_fail
source ${0:A:h:h}/paths.sh
say() { print -r -- "[recompile] $*" }
die() { print -r -- "[recompile] error: $*" >&2; exit 1 }

typeset -A IMAGE
while (( $# )); do
  case $1 in
    --ai|--dm2|--bmx) [[ -n ${2:-} ]] || die "$1 needs a disc image"; IMAGE[${1#--}]=$2; shift 2 ;;
    -h|--help) sed -n '2,14p' $0 | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument $1 (--ai IMAGE, --dm2 IMAGE, --bmx IMAGE)" ;;
  esac
done
if (( ${#IMAGE} == 0 )); then
  # make-app.sh's defaults
  [[ -f "$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso" ]] && IMAGE[ai]="$GAME_DISCS/aggressive inline/Aggressive Inline-ps2.iso"
  for f in "$GAME_DISCS/Dave Mirra Freestyle BMX 2 (USA)/Dave Mirra Freestyle BMX 2 (USA).cue" $DM2_WORKSPACE/dmbmx2.iso; do
    [[ -z ${IMAGE[dm2]:-} && -f $f ]] && IMAGE[dm2]=$f
  done
  [[ -f "$GAME_DISCS/BMX XXX (USA)/BMX XXX-ps2.iso" ]] && IMAGE[bmx]="$GAME_DISCS/BMX XXX (USA)/BMX XXX-ps2.iso"
  (( ${#IMAGE} )) || die "no disc image given, and none in $GAME_DISCS (--ai IMAGE, --dm2 IMAGE, --bmx IMAGE)"
fi

# The game: its workspace, ELF, the ELF's SHA-256 of the USA release, and the config the recompiler runs.
typeset -A WS ELF SHA CFG
WS=(ai $AI_WORKSPACE dm2 $DM2_WORKSPACE)
ELF=(ai SLUS_203.27 dm2 SLUS_201.59 bmx SLUS_204.15)
SHA=(ai 1b5e170b7750487b29afd9ac9a5012e41a4e67db9c6c90fcba91511e4c688a1c
     dm2 7c89b999729aeaf36140248be80616cd5738b8eb79cf7bc7c90474c2d7bbdc92)
CFG=(ai config.game.toml dm2 config.ns.toml)

BUILD=${BUILD:-$ZAXIS/out/build-recomp}
RECOMP=$BUILD/recompiler/ps2xRecomp/ps2_recomp
DISC=$BUILD/iop/zx-disc
if [[ ! -x $RECOMP || ! -x $DISC ]]; then
  say "building the recompiler and the disc reader in $BUILD (the first time fetches their sources)"
  mkdir -p ${BUILD:h}
  cmake -S $ZAXIS -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DZX_BUILD_RUNTIME=OFF -DZX_BUILD_TEST=OFF >$BUILD.log 2>&1 \
    || die "configure failed: $BUILD.log"
  ninja -C $BUILD -j${NINJA_JOBS:-6} ps2_recomp zx-disc >>$BUILD.log 2>&1 || die "the recompiler did not build: $BUILD.log"
fi

for game in ai dm2 bmx; do
  image=${IMAGE[$game]:-}
  [[ -n $image ]] || continue
  if [[ $game == bmx ]]; then
    [[ -f $image ]] || die "bmx: no disc image $image"
    # (its ELF through zx-disc, as the others', so any format reads, into BMX_RECOMP's extracted/, where
    # make-app.sh looks for it; recompile.py checks its SHA-256)
    elf=${ELF[bmx]} ex=${BMX_RECOMP:h}/extracted
    mkdir -p $ex && rm -f $ex/$elf
    $DISC extract "$image" $ex $elf && [[ -f $ex/$elf ]] || die "bmx: no $elf on ${image:t}: not its PS2 disc?"
    chmod u+rw $ex/$elf
    python3 -I $BMX_WORKSPACE/recomp/recompile.py --elf $ex/$elf --recompiler $RECOMP \
      | sed -u 's/^recompile: /[recompile] bmx: /' || die "bmx: recomp/recompile.py failed"
    continue
  fi
  [[ -f $image ]] || die "$game: no disc image $image"
  ws=${WS[$game]} elf=${ELF[$game]}
  mkdir -p $ws/extracted
  rm -f $ws/extracted/$elf
  $DISC extract "$image" $ws/extracted $elf && [[ -f $ws/extracted/$elf ]] \
    || die "$game: no $elf on ${image:t}: not its PS2 disc?"
  chmod u+rw $ws/extracted/$elf
  sha=$(shasum -a 256 $ws/extracted/$elf | cut -d' ' -f1)
  [[ $sha == ${SHA[$game]} ]] || die "$game: ${image:t}'s $elf is not the USA release's (SHA-256 $sha)"
  say "$game: $elf from ${image:t}, the USA release"
  (cd $ws/recomp && python3 -I tools/make-config.py >/dev/null) || die "$game: make-config.py failed"
  start=$SECONDS
  (cd $ws/recomp && $RECOMP ${CFG[$game]} >$BUILD/recompile-$game.log 2>&1) || die "$game: the recompiler failed: $BUILD/recompile-$game.log"
  out=$(sed -n 's/^output *= *"\(.*\)"/\1/p' $ws/recomp/${CFG[$game]} | head -1)
  say "$game: code in recomp/$out ($(ls $ws/recomp/$out | wc -l | tr -d ' ') files, $((SECONDS - start)) s)"
  # (its VU1 micro memory image, as BMX XXX's recompile.py makes BMX's: the tree's tools/setup/vu1_image.py walks the
  # microprograms' chain the symbol and bind tables name, into recomp/generated-vu1, which make-app.sh and
  # make-windows.ps1 build the VU1 code ahead of time from; a tree from before it has none, and its VU1 is interpreted)
  if [[ -f $ZAXIS/tools/setup/vu1_image.py ]]; then
    python3 -I $ZAXIS/tools/setup/vu1_image.py --game $game --elf $ws/extracted/$elf --out-dir $ws/recomp/generated-vu1 \
      | sed -u "s/^vu1_image: /[recompile] /" || die "$game: vu1_image.py failed"
  fi
done
