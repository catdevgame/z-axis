#!/bin/bash
# Wrapper: runs Ghidra headless with the project-local settings dir (where the
# ghidra-emotionengine-reloaded extension is installed). Args are passed to analyzeHeadless.
G=$(cd "$(dirname "$0")/.." && pwd -P)
export JAVA_HOME=/opt/homebrew/opt/openjdk@21
export GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-6G}
export GHIDRA_HEADLESS_JAVA_OPTIONS="-Dapplication.settingsdir=$G/settings"
exec "$(brew --prefix ghidra)/libexec/support/analyzeHeadless" "$G/project" AggressiveInline "$@"
