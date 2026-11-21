# Writes OUT (play_recording_commit.h: ZX_BUILD_COMMIT, the source tree's
# git commit, "+changes" when tracked files differ from it) for the play
# recordings' header (runtime/play_recording.h). Rewritten only when the
# text changes, so a build without a new commit recompiles nothing.
set(commit "unknown")
execute_process(COMMAND git -C "${SRC}" rev-parse HEAD
                OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
if(rc EQUAL 0 AND NOT head STREQUAL "")
    set(commit "${head}")
    execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no --ignore-submodules
                    OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT dirty STREQUAL "")
        string(APPEND commit "+changes")
    endif()
endif()
set(text "#define ZX_BUILD_COMMIT \"${commit}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
