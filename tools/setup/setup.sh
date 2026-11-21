#!/bin/zsh
# setup.sh [--check]: this checkout made ready. Safe to re-run.
#   1. every PATH in data-links.txt (in a game's workspace/ or tools/) made a link to its folder under AI_DATA or
#      DM2_DATA (paths.env): the tools find the data where they always did, outside git.
#      A data folder the tools write into (recomp/dumps, logs, run, traces, output; and what building the app
#      makes: extracted/ for the ELF, recomp/generated, generated-ns, generated-vu1, dist) is made if missing;
#      the rest is the workshop's own data (Ghidra projects, the decomp's build, PCSX2 baselines, reference runs),
#      not needed to build or play: counted, listed with VERBOSE=1, left unlinked.
#   2. in the workshop's main checkout, CLAUDE.md -> workshop/CLAUDE.md (the agents' rules, versioned there; the
#      lanes' worktrees in $ZAXIS_WORKTREES sit below it, so it applies to them too). None in a public checkout.
# --check: report only, change nothing. Exit 1 if a path is a real file or folder (move it into the
# data first) or a link points elsewhere (--check) .
emulate -L zsh
CHECK=; [[ $1 == --check ]] && CHECK=1
source ${0:A:h:h}/paths.sh
ok=0 made=0 missing=0 bad=0
# (lpath: zsh's path is PATH)
while read -r lpath var rel; do
  [[ -z $lpath || $lpath == \#* ]] && continue
  D=${(P)var}
  [[ -n $D ]] || { echo "setup: $lpath: no data folder $var (paths.env)"; bad=$((bad+1)); continue; }
  link=$ZAXIS/$lpath; target=$D/$rel
  if [[ -L $link ]]; then
    [[ $(readlink $link) == $target ]] && { ok=$((ok+1)); continue; }
    [[ -n $CHECK ]] && { echo "setup: $lpath -> $(readlink $link), not $target"; bad=$((bad+1)); continue; }
    rm $link
  elif [[ -e $link ]]; then
    echo "setup: $lpath is a real file or folder: move it to $target first"; bad=$((bad+1)); continue
  fi
  if [[ ! -e $target ]]; then
    case $rel in
      recomp/(dumps|logs|run|traces|output|generated|generated-ns|generated-vu1)|extracted|dist) [[ -z $CHECK ]] && mkdir -p $target ;;
      *) missing=$((missing+1)); [[ -n $VERBOSE ]] && echo "setup: no $target: $lpath not linked"; continue ;;
    esac
  fi
  [[ -n $CHECK ]] && { echo "setup: $lpath not linked"; bad=$((bad+1)); continue; }
  mkdir -p ${link:h}; ln -s $target $link; made=$((made+1))
done < ${0:A:h}/data-links.txt
if [[ $ZAXIS == $ZAXIS_MAIN && -f $ZAXIS/workshop/CLAUDE.md ]]; then
  if [[ -L $ZAXIS/CLAUDE.md && $(readlink $ZAXIS/CLAUDE.md) == workshop/CLAUDE.md ]]; then :
  elif [[ -e $ZAXIS/CLAUDE.md || -L $ZAXIS/CLAUDE.md ]]; then echo "setup: $ZAXIS/CLAUDE.md is not the link to workshop/CLAUDE.md"; bad=$((bad+1))
  elif [[ -n $CHECK ]]; then echo "setup: no $ZAXIS/CLAUDE.md"; bad=$((bad+1))
  else ln -s workshop/CLAUDE.md $ZAXIS/CLAUDE.md; fi
fi
echo "setup: data $AI_DATA, $DM2_DATA: $ok links as they were, $made made, $bad wrong; $missing of the workshop's own data folders not there (not needed to build or play the games: VERBOSE=1 lists them)"
[[ $bad -eq 0 ]]
