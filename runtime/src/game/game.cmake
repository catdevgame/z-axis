# The readable game (src/game, see src/game/ee/readable.h): hand-written C++
# replacing recompiled functions one at a time, built into the runner, and
# ps2xFnCheck, which checks each one against the recompiled original on
# recorded calls (src/game/ee/fncheck.h).
#
# Included from runtime/CMakeLists.txt after the runner target.

set(PS2X_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
# The comparison machinery (docs/READABLE.md "The gate"): the recorder and its traced originals, diffcall, the
# cycle and tail-call profilers, the guest-call trace, ps2xFnCheck. The dev flavour builds them; the app's
# (ZAXIS_DEV_CHECKS OFF, runtime/CMakeLists.txt) compiles and links none, and ee/readable.cpp's install() has
# nothing of theirs to install.
set(PS2X_DEV_CHECK_SOURCES "${PS2X_GAME_DIR}/ee/fncheck.cpp" "${PS2X_GAME_DIR}/ee/fnrec_file.cpp" "${PS2X_GAME_DIR}/ee/diffcall.cpp"
    "${PS2X_GAME_DIR}/ee/cycletrace.cpp" "${PS2X_GAME_DIR}/ee/tailcall_stats.cpp" "${PS2X_GAME_DIR}/ee/guest_calls.cpp")
file(GLOB_RECURSE PS2X_GAME_SOURCES CONFIGURE_DEPENDS "${PS2X_GAME_DIR}/*.cpp")
list(REMOVE_ITEM PS2X_GAME_SOURCES "${PS2X_GAME_DIR}/ee/fncheck_main.cpp"
    "${PS2X_GAME_DIR}/zge/native/native_assets_main.cpp"
    # build time only (the packaging step's converter): not in the game
    "${PS2X_GAME_DIR}/zge/native/xbox_models.cpp")
# The games beside AI (ee/game_id.h's names, each with its ELF): each has an instance of its own below.
set(PS2X_GAME_OTHER_GAMES "dm2|SLUS_201.59;bmx|SLUS_204.15")
# One engine, three games: AI's instance is zge, sce and game/aggressive. Another game's folder (game/dm2,
# game/bmx: its own code) is compiled in that game's instance only (PS2X_GAME_FOLDER_SOURCES_<game>).
# PS2X_GAME_SOURCES keeps every source, for the steps that read them all (symbols, digests, lint).
set(PS2X_GAME_AI_SOURCES ${PS2X_GAME_SOURCES})
foreach(_entry IN LISTS PS2X_GAME_OTHER_GAMES)
    string(REGEX REPLACE "\\|.*" "" _game "${_entry}")
    set(PS2X_GAME_FOLDER_SOURCES_${_game} ${PS2X_GAME_SOURCES})
    list(FILTER PS2X_GAME_FOLDER_SOURCES_${_game} INCLUDE REGEX "^${PS2X_GAME_DIR}/${_game}/")
    list(FILTER PS2X_GAME_AI_SOURCES EXCLUDE REGEX "^${PS2X_GAME_DIR}/${_game}/")
endforeach()

# Every header of the readable game, for the steps that read them (symbols, the ratchet).
file(GLOB_RECURSE _ps2x_game_headers CONFIGURE_DEPENDS "${PS2X_GAME_DIR}/*.h")

# Symbols (tools/readable_symbols.py, docs/READABLE.md "Symbols"): the names
# a source uses, as constants, in one header per source and game, written only
# when their text changes. AI's come from tools/readable_symbols.tsv alone.
# Another game's from its bind table, tools/bind/<game>.tsv, made from the
# work database (`readable_symbols.py bind --write`) and committed, so a
# commit builds with its own table on every host. PS2X_BIND_DIR set: the
# build has the other games' instances, from this tree's tables whatever
# folder it names (a build dir configured before, with the shared table of
# the scratch folder, takes them at its next configure); empty: no
# instance of another game, and the build is AI's as before.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PS2X_BIND_DIR "" CACHE PATH "The per-game symbol tables: this tree's tools/bind when set (readable_symbols.py bind)")
set(PS2X_BIND_TABLES "${CMAKE_CURRENT_SOURCE_DIR}/tools/bind")
if(PS2X_BIND_DIR AND NOT PS2X_BIND_DIR STREQUAL PS2X_BIND_TABLES)
    message(STATUS "readable: PS2X_BIND_DIR ${PS2X_BIND_DIR}: the commit's own bind tables are used, ${PS2X_BIND_TABLES}")
    set(PS2X_BIND_DIR "${PS2X_BIND_TABLES}" CACHE PATH "The per-game symbol tables: this tree's tools/bind when set (readable_symbols.py bind)" FORCE)
endif()
set(PS2X_SYMBOLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/symbols")
set(PS2X_SYMBOLS_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_symbols.py")
set(PS2X_SYMBOLS_TABLE "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_symbols.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_SYMBOLS_TOOL}" "${PS2X_SYMBOLS_TABLE}")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_SYMBOLS_TOOL}" headers --out "${PS2X_SYMBOLS_DIR}/ai"
                RESULT_VARIABLE _ps2x_symbols_result)
if(NOT _ps2x_symbols_result EQUAL 0)
    message(FATAL_ERROR "readable: the symbol headers failed")
endif()
# A shared source's layout check (sym/<source>.layout.h, included at its end: the fields it uses at the offsets
# that game's machine code uses) is written for another game's instance from its bind table; in AI's build the
# layouts are the headers' own, so the file checks nothing.
foreach(_ps2x_source IN LISTS PS2X_GAME_SOURCES)
    file(STRINGS "${_ps2x_source}" _ps2x_layout_include REGEX "^#include \"sym/.*\\.layout\\.h\"")
    if(_ps2x_layout_include)
        file(RELATIVE_PATH _ps2x_relative "${PS2X_GAME_DIR}" "${_ps2x_source}")
        set(_ps2x_layout "${PS2X_SYMBOLS_DIR}/ai/sym/${_ps2x_relative}.layout.h")
        if(NOT EXISTS "${_ps2x_layout}")
            file(WRITE "${_ps2x_layout}" "// AI's build: ${_ps2x_relative}'s layouts are its headers' own.\n")
        endif()
    endif()
endforeach()
# The headers are the step's byproducts, so that Ninja looks at them again once it has run (restat): a source whose
# header the step rewrote (a status of symbols_status.tsv or of a registration in another source, a level) is
# rebuilt in the same build. Unlisted, the first build after such a change wrote the headers but kept the objects
# that include them, and only a second build compiled them (verify2, verify3: a promotion's runner under-counted
# Verified). The list is the configure step's headers (written just above); a new source reconfigures.
file(GLOB_RECURSE _ps2x_symbols_headers "${PS2X_SYMBOLS_DIR}/ai/sym/*.h")
list(FILTER _ps2x_symbols_headers EXCLUDE REGEX "\\.layout\\.h$")
add_custom_command(
    OUTPUT "${PS2X_SYMBOLS_DIR}/ai.stamp"
    BYPRODUCTS ${_ps2x_symbols_headers}
    COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_SYMBOLS_TOOL}" headers --out "${PS2X_SYMBOLS_DIR}/ai"
    COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_SYMBOLS_TOOL}" lint
    COMMAND "${CMAKE_COMMAND}" -E touch "${PS2X_SYMBOLS_DIR}/ai.stamp"
    DEPENDS ${PS2X_GAME_SOURCES} ${_ps2x_game_headers} "${PS2X_SYMBOLS_TOOL}" "${PS2X_SYMBOLS_TABLE}"
    COMMENT "readable: symbol headers (ai), the literal lint"
    VERBATIM)
add_custom_target(ps2_game_symbols DEPENDS "${PS2X_SYMBOLS_DIR}/ai.stamp")

# Plain sources (plain_sources.txt, tools/readable_entries.py): a source whose
# functions are defined and called by their plain names is compiled from a copy
# in the build dir that renames each registered definition NAME_ and defines
# NAME as its entry (game::detail::Route: the replay's answers, hooks, the
# switch, fibers), so a plain call goes where a game function's call goes.
# The source itself is never changed; the copy keeps its lines (#line).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PS2X_ENTRIES_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_entries.py")
# (where an ee::LeftByCaller's value is: the copy gives each its register from this table)
set(PS2X_ENTRY_READS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_entry_reads.tsv")
set(_ps2x_plain_list "${PS2X_GAME_DIR}/plain_sources.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ps2x_plain_list}")
file(STRINGS "${_ps2x_plain_list}" _ps2x_plain_prefixes REGEX "^[a-z]")
set(_ps2x_entries_out "${CMAKE_CURRENT_BINARY_DIR}/readable_entries")
# ps2x_plain_source(<source> <out variable>): the path to compile for a game source.
function(ps2x_plain_source _source _out)
    file(RELATIVE_PATH _relative "${PS2X_GAME_DIR}" "${_source}")
    set(_plain FALSE)
    foreach(_prefix IN LISTS _ps2x_plain_prefixes)
        string(LENGTH "${_prefix}" _length)
        string(SUBSTRING "${_relative}" 0 ${_length} _head)
        if(_head STREQUAL _prefix)
            set(_plain TRUE)
        endif()
    endforeach()
    if(NOT _plain)
        set(${_out} "${_source}" PARENT_SCOPE)
        return()
    endif()
    set(_copy "${_ps2x_entries_out}/${_relative}")
    get_property(_made GLOBAL PROPERTY PS2X_ENTRIES_MADE)
    if(NOT _copy IN_LIST _made)
        add_custom_command(
            OUTPUT "${_copy}"
            COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_ENTRIES_TOOL}" --line-root "${CMAKE_BINARY_DIR}" "${_source}" "${_copy}"
            DEPENDS "${_source}" "${PS2X_ENTRIES_TOOL}" "${PS2X_ENTRY_READS}"
            COMMENT "readable: entries of ${_relative}"
            VERBATIM)
        set_property(GLOBAL APPEND PROPERTY PS2X_ENTRIES_MADE "${_copy}")
    endif()
    get_filename_component(_dir "${_source}" DIRECTORY)
    # The copy finds its own folder's headers ("strings.h") by -iquote, not -I: on -I, a folder's header
    # named as a system one (zge/text/strings.h) is also what glibc's string.h includes as <strings.h>, inside
    # its extern "C" (a Linux build of a game's instance, which has no precompiled header to come first).
    if(MSVC)
        set_source_files_properties("${_copy}" PROPERTIES GENERATED TRUE INCLUDE_DIRECTORIES "${_dir}")
    else()
        set_source_files_properties("${_copy}" PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-iquote;${_dir}")
    endif()
    set(${_out} "${_copy}" PARENT_SCOPE)
endfunction()
set(_ps2x_compiled_sources "")
foreach(_source IN LISTS PS2X_GAME_AI_SOURCES)
    ps2x_plain_source("${_source}" _compiled)
    list(APPEND _ps2x_compiled_sources "${_compiled}")
endforeach()

if(NOT ZAXIS_DEV_CHECKS)
    list(REMOVE_ITEM _ps2x_compiled_sources ${PS2X_DEV_CHECK_SOURCES})
endif()
# An object library: the registrations (static objects) must reach both
# executables, which a static library would let the linker drop.
add_library(ps2_game_readable OBJECT ${_ps2x_compiled_sources})
add_dependencies(ps2_game_readable ps2_game_symbols)
target_link_libraries(ps2_game_readable PUBLIC ps2_runtime)
target_include_directories(ps2_game_readable PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PS2X_GAME_DIR}")
target_include_directories(ps2_game_readable PRIVATE "${PS2X_SYMBOLS_DIR}/ai")
# EE float arithmetic is done by the ps2_f* helpers; no fused multiply-adds
# around them.
target_compile_options(ps2_game_readable PRIVATE ${PS2X_FP_NO_CONTRACT})
# The EE's 32-bit integer arithmetic wraps; readable code (drafts above all:
# a register's value as int32_t, `s3 + s5` stepping past 0x7FFFFFFF in
# fn_10D6B8's loop) is written as the original computes, so signed overflow
# wraps here too instead of being undefined (drafter21).
target_compile_options(ps2_game_readable PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fwrapv>)
# clang-cl ignores a bare -fwrapv ("unknown argument ignored in clang-cl"):
# it takes clang's options after /clang:, as PS2X_FP_NO_CONTRACT does. Without
# it the Windows build's fn_10D6B8 passed angleSin 0x8000 for the original's
# 0xFFFF8000 (remote1: the corpus on the Windows worker).
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(ps2_game_readable PRIVATE /clang:-fwrapv)
endif()
# The runtime's headers precompiled: three quarters of a readable source's
# parse time is ps2_runtime.h, which every one includes through ee/ee.h. Only
# the runtime's headers, never the game's, so a change to a game header does
# not rebuild the precompiled one (ps2_game_headers, which checks that each
# game header compiles on its own, has none).
option(PS2X_ENABLE_GAME_PCH "Precompile the runtime headers for the readable game" ON)
if(PS2X_ENABLE_GAME_PCH)
    target_precompile_headers(ps2_game_readable PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h")
    # (it sets the feature macros <ucontext.h> needs before any system header)
    set_source_files_properties("${PS2X_GAME_DIR}/ee/fiber_posix.cpp" TARGET_DIRECTORY ps2_game_readable
                                PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:ps2_game_readable>)
include("${PS2X_GAME_DIR}/game_headers.cmake")

# The recompiled originals of the registered functions, built again with
# fncheck's tracing (src/game/ee/fncheck_traced.h). The addresses come from
# READABLE_FUNCTION / READABLE_PLANNED in the sources (Aggressive Inline's,
# from PS2X_RUNNER_GENERATED_DIR) and from READABLE_PLANNED_IN(<game>, ...) /
# READABLE_FUNCTION_IN(<game>, ...) / _WAITS_IN / _VU0_IN (another game's, from its namespaced
# generated code in PS2X_RUNNER_EXTRA_GENERATED_DIRS; ee/game_id.h).
#
# _ps2x_traced_sources(<out list> <game or ""> <generated dir> <addresses>...):
# one source per address. For another game the generated file defines its
# function inside the game's namespace (it opens `namespace <game>` itself),
# so the #define renames that definition to <game>::fncheck_traced_<game>_<hex>,
# and the registration names the game; tail calls and extents as AI's.
#
# The traced source includes blocks/<stem>.cpp, the generated file with block
# markers for path coverage (tools/fncheck_traced_gen.py at build time, from
# the list each call appends to: game number, address, generated file, copy;
# ee/fncheck_build.h).
set(_ps2x_game_number_ "0")
set(_ps2x_game_number_dm2 "1")
set(_ps2x_game_number_bmx "2")
set(PS2X_TRACED_BLOCK_LIST "")
set(PS2X_TRACED_BLOCK_FILES "")
set(PS2X_TRACED_GENERATED_FILES "")
function(_ps2x_traced_sources _outVar _game _genDir)
    set(_sources "")
    set(_blockList "${PS2X_TRACED_BLOCK_LIST}")
    set(_blockFiles "${PS2X_TRACED_BLOCK_FILES}")
    set(_genFiles "${PS2X_TRACED_GENERATED_FILES}")
    # The generated folder (26,000 files) listed once, each file by its
    # address, rather than globbed once per registered function.
    file(GLOB _ps2x_generated_files "${_genDir}/*_0x*.cpp")
    foreach(_file IN LISTS _ps2x_generated_files)
        if(_file MATCHES "_(0x[0-9a-f]+)\\.cpp$")
            if(DEFINED _ps2x_gen_${CMAKE_MATCH_1})
                set(_ps2x_gen_${CMAKE_MATCH_1} "AMBIGUOUS")
            else()
                set(_ps2x_gen_${CMAKE_MATCH_1} "${_file}")
            endif()
        endif()
    endforeach()
    if(_game STREQUAL "")
        set(_folder "fncheck_traced")
        set(_prefix "fncheck_traced_")
        set(_what "")
    else()
        set(_folder "fncheck_traced_${_game}")
        set(_prefix "fncheck_traced_${_game}_")
        set(_what "${_game} ")
    endif()
    foreach(_addr IN LISTS ARGN)
        set(_ps2x_registered_${_addr} 1)
    endforeach()
    foreach(_addr IN LISTS ARGN)
        set(_gen "${_ps2x_gen_${_addr}}")
        if(NOT _gen OR _gen STREQUAL "AMBIGUOUS")
            message(WARNING "readable: no single generated ${_what}function for ${_addr}")
            continue()
        endif()
        get_filename_component(_stem "${_gen}" NAME_WE)
        string(SUBSTRING "${_addr}" 2 -1 _hex)
        set(_traced "${CMAKE_CURRENT_BINARY_DIR}/${_folder}/traced_${_hex}.cpp")
        set(_blocks "${CMAKE_CURRENT_BINARY_DIR}/${_folder}/blocks/${_stem}.cpp")
        file(STRINGS "${_gen}" _range REGEX "^// Address: 0x[0-9a-f]+ - 0x[0-9a-f]+" LIMIT_COUNT 1)
        # A function the recompiler wrote as several entry_ chunks (the Ghidra
        # map has it inside another function's range: AI's bindTexture,
        # entry_1049d0 .. entry_104abc): its later chunks are the entry_ files
        # that each start where the one before ends, up to a registered
        # address, a FUN_ file or a function of the game's symbol table. Its
        # traced original is then the whole function (fncheck_traced.h
        # FNCHECK_TRACED_CHUNKS: the chunks run in turn, as the scheduler
        # dispatches them in the game), one block table over its extent.
        set(_chunkGens "")
        set(_end "")
        if(_range MATCHES "^// Address: (0x[0-9a-f]+) - (0x[0-9a-f]+)")
            set(_end "${CMAKE_MATCH_2}")
        endif()
        if(_stem MATCHES "^entry_" AND _end)
            while(TRUE)
                set(_next "${_ps2x_gen_${_end}}")
                if(NOT _next OR _next STREQUAL "AMBIGUOUS" OR DEFINED _ps2x_registered_${_end})
                    break()
                endif()
                get_filename_component(_nextStem "${_next}" NAME_WE)
                if(NOT _nextStem MATCHES "^entry_")
                    break()
                endif()
                string(SUBSTRING "${_end}" 2 -1 _endHex)
                string(TOUPPER "${_endHex}" _endHex)
                file(STRINGS "${PS2X_SYMBOLS_TABLE}" _named
                     REGEX "^function\t[^\t]*\t0x0*${_endHex}\t[^\t]*\t${_game}$" LIMIT_COUNT 1)
                if(_named)
                    break()
                endif()
                file(STRINGS "${_next}" _nextRange REGEX "^// Address: 0x[0-9a-f]+ - 0x[0-9a-f]+" LIMIT_COUNT 1)
                if(NOT _nextRange MATCHES "^// Address: (0x[0-9a-f]+) - (0x[0-9a-f]+)")
                    break()
                endif()
                list(APPEND _chunkGens "${_next}")
                set(_end "${CMAKE_MATCH_2}")
            endwhile()
        endif()
        set(_chunkColumn "")
        set(_chunkStems "")
        if(_chunkGens)
            list(JOIN _chunkGens "|" _chunkColumn)
            set(_chunkColumn "\t${_chunkColumn}")
            foreach(_chunkGen IN LISTS _chunkGens)
                get_filename_component(_chunkStem "${_chunkGen}" NAME_WE)
                list(APPEND _chunkStems "${_chunkStem}")
            endforeach()
            list(LENGTH _chunkGens _chunkCount)
            math(EXPR _chunkCount "${_chunkCount} + 1")
            list(JOIN _chunkStems " " _chunkNames)
            message(STATUS "readable: ${_what}${_addr}: ${_chunkCount} entry_ chunks, to ${_end} (${_stem} ${_chunkNames})")
        endif()
        string(APPEND _blockList "${_ps2x_game_number_${_game}}\t${_addr}\t${_gen}\t${_blocks}${_chunkColumn}\n")
        list(APPEND _blockFiles "${_blocks}")
        list(APPEND _genFiles "${_gen}" ${_chunkGens})
        # Tail calls go to fncheck::tailCallTo: code generated since round 11
        # calls runtime->tailCall (TracedRuntime's), whose recompiled target
        # is named only as a fallback the traced build never takes, so it
        # names a placeholder instead (ps2xFnCheck has only the registered
        # functions); older code makes a direct call of the target, then
        # returns, which a macro redirects.
        set(_tailDefines "")
        # (each #define undone at the end, so that the next source of a
        # unity batch, PS2X_TRACED_UNITY, sees its own names only)
        set(_undefines "#undef ${_stem}\n#undef g_fncheckBlocks\n#undef kFncheckBlockStarts\n")
        foreach(_tailFile IN ITEMS "${_gen}" ${_chunkGens})
            file(STRINGS "${_tailFile}" _tails REGEX "^ *([A-Za-z_][A-Za-z0-9_]*_0x[0-9a-f]+\\(rdram, ctx, runtime\\)|runtime->tailCall\\(.*&[A-Za-z_][A-Za-z0-9_]*_0x[0-9a-f]+\\)); return;")
            foreach(_tail IN LISTS _tails)
                if(_tail MATCHES "&([A-Za-z_][A-Za-z0-9_]*_0x[0-9a-f]+)\\); return;")
                    set(_tailSymbol "${CMAKE_MATCH_1}")
                    string(FIND "${_tailDefines}" "#define ${_tailSymbol} " _already)
                    if(NOT _tailSymbol STREQUAL _stem AND NOT _tailSymbol IN_LIST _chunkStems AND _already EQUAL -1)
                        string(APPEND _tailDefines "#define ${_tailSymbol} ::fncheck::tailCallPlaceholder\n")
                        string(APPEND _undefines "#undef ${_tailSymbol}\n")
                    endif()
                elseif(_tail MATCHES "([A-Za-z_][A-Za-z0-9_]*_0x([0-9a-f]+))\\(rdram, ctx, runtime\\)")
                    set(_tailSymbol "${CMAKE_MATCH_1}")
                    set(_tailAddress "${CMAKE_MATCH_2}")
                    string(FIND "${_tailDefines}" "#define ${_tailSymbol}(" _already)
                    if(NOT _tailSymbol STREQUAL _stem AND NOT _tailSymbol IN_LIST _chunkStems AND _already EQUAL -1)
                        string(APPEND _tailDefines "#define ${_tailSymbol}(r, c, rt) ::fncheck::tailCallTo(0x${_tailAddress}u, r, c, rt)\n")
                        string(APPEND _undefines "#undef ${_tailSymbol}\n")
                    endif()
                endif()
            endforeach()
        endforeach()
        if(_chunkGens)
            # The chunks' traced names (<function>_<chunk>), and the function
            # that runs them, under the function's traced name.
            if(_game STREQUAL "")
                set(_symbol "fncheck_traced_${_hex}")
                set(_open "")
                set(_close "")
                set(_registration "FNCHECK_TRACED(${_addr}u, ${_symbol})
FNCHECK_TRACED_EXTENT(${_addr}u, ${_end}u, ${_symbol})")
            else()
                set(_symbol "${_prefix}${_hex}")
                set(_open "namespace ${_game}\n{\n")
                set(_close "}\n")
                set(_registration "FNCHECK_TRACED_IN(${_game}, ${_addr}u, ${_game}::${_symbol})
FNCHECK_TRACED_EXTENT_IN(${_game}, ${_addr}u, ${_end}u, ${_game}::${_symbol})")
            endif()
            set(_chunkDefines "")
            set(_chunkRows "")
            foreach(_chunkGen IN ITEMS "${_gen}" ${_chunkGens})
                get_filename_component(_chunkStem "${_chunkGen}" NAME_WE)
                file(STRINGS "${_chunkGen}" _chunkRange REGEX "^// Address: 0x[0-9a-f]+ - 0x[0-9a-f]+" LIMIT_COUNT 1)
                string(REGEX REPLACE "^// Address: (0x[0-9a-f]+) - (0x[0-9a-f]+).*$" "\\1;\\2" _chunkRange "${_chunkRange}")
                list(GET _chunkRange 0 _chunkStart)
                list(GET _chunkRange 1 _chunkEnd)
                string(SUBSTRING "${_chunkStart}" 2 -1 _chunkHex)
                string(APPEND _chunkDefines "#define ${_chunkStem} ${_symbol}_${_chunkHex}\n")
                if(NOT _chunkRows STREQUAL "")
                    string(APPEND _chunkRows ",\n")
                endif()
                string(APPEND _chunkRows "                      {${_chunkStart}u, ${_chunkEnd}u, &${_symbol}_${_chunkHex}}")
                if(NOT _chunkStem STREQUAL _stem)
                    string(APPEND _undefines "#undef ${_chunkStem}\n")
                endif()
            endforeach()
            file(CONFIGURE OUTPUT "${_traced}" CONTENT
"// Generated by src/game/game.cmake: ${_what}${_addr}'s ${_chunkCount} entry_ chunks with fncheck tracing, run as
// one function (fncheck_traced.h, FNCHECK_TRACED_CHUNKS).
#include \"ee/fncheck_traced.h\"
${_tailDefines}${_chunkDefines}#define g_fncheckBlocks g_fncheckBlocks_${_hex}
#define kFncheckBlockStarts kFncheckBlockStarts_${_hex}
#include \"blocks/${_stem}.cpp\"
${_open}FNCHECK_TRACED_CHUNKS(${_symbol},
${_chunkRows})
${_close}${_registration}
${_undefines}" @ONLY)
            list(APPEND _sources "${_traced}")
            continue()
        endif()
        # The function's extent, for jumps through a register within it
        # (fncheck::TracedExtent).
        set(_extent "")
        if(_game STREQUAL "")
            if(_range MATCHES "^// Address: (0x[0-9a-f]+) - (0x[0-9a-f]+)")
                set(_extent "FNCHECK_TRACED_EXTENT(${CMAKE_MATCH_1}u, ${CMAKE_MATCH_2}u, fncheck_traced_${_hex})\n")
            endif()
            file(CONFIGURE OUTPUT "${_traced}" CONTENT
"// Generated by src/game/game.cmake: ${_stem} with fncheck tracing.
#include \"ee/fncheck_traced.h\"
${_tailDefines}#define ${_stem} fncheck_traced_${_hex}
#define g_fncheckBlocks g_fncheckBlocks_${_hex}
#define kFncheckBlockStarts kFncheckBlockStarts_${_hex}
#include \"blocks/${_stem}.cpp\"
FNCHECK_TRACED(${_addr}u, fncheck_traced_${_hex})
${_extent}${_undefines}" @ONLY)
        else()
            if(_range MATCHES "^// Address: (0x[0-9a-f]+) - (0x[0-9a-f]+)")
                set(_extent "FNCHECK_TRACED_EXTENT_IN(${_game}, ${CMAKE_MATCH_1}u, ${CMAKE_MATCH_2}u, ${_game}::${_prefix}${_hex})\n")
            endif()
            file(CONFIGURE OUTPUT "${_traced}" CONTENT
"// Generated by src/game/game.cmake: ${_game}'s ${_stem} with fncheck tracing
// (its generated file defines it in namespace ${_game}).
#include \"ee/fncheck_traced.h\"
${_tailDefines}#define ${_stem} ${_prefix}${_hex}
#define g_fncheckBlocks g_fncheckBlocks_${_hex}
#define kFncheckBlockStarts kFncheckBlockStarts_${_hex}
#include \"blocks/${_stem}.cpp\"
FNCHECK_TRACED_IN(${_game}, ${_addr}u, ${_game}::${_prefix}${_hex})
${_extent}${_undefines}" @ONLY)
        endif()
        list(APPEND _sources "${_traced}")
    endforeach()
    set(${_outVar} "${_sources}" PARENT_SCOPE)
    set(PS2X_TRACED_BLOCK_LIST "${_blockList}" PARENT_SCOPE)
    set(PS2X_TRACED_BLOCK_FILES "${_blockFiles}" PARENT_SCOPE)
    set(PS2X_TRACED_GENERATED_FILES "${_genFiles}" PARENT_SCOPE)
endfunction()

# PS2X_TRACED_UNITY=N: the traced originals compiled about N to a translation
# unit (0: one each). Nine tenths of a traced source's compile time is the
# runtime's headers, the same for all of them, so batches cut a cold build of
# the traced originals by about ten. A source's batch is chosen by a hash of
# its address, among a power of two of batches, so a registration added or
# removed recompiles its own batch only, not every batch after it.
set(PS2X_TRACED_UNITY 32 CACHE STRING "Traced originals per unity translation unit (0: one each)")
function(_ps2x_traced_unity _target)
    if(NOT PS2X_TRACED_UNITY OR PS2X_TRACED_UNITY LESS 2)
        return()
    endif()
    list(LENGTH ARGN _count)
    math(EXPR _wanted "(${_count} + ${PS2X_TRACED_UNITY} - 1) / ${PS2X_TRACED_UNITY}")
    set(_batches 1)
    while(_batches LESS _wanted)
        math(EXPR _batches "${_batches} * 2")
    endwhile()
    foreach(_source IN LISTS ARGN)
        get_filename_component(_name "${_source}" NAME_WE)
        string(MD5 _hash "${_name}")
        string(SUBSTRING "${_hash}" 0 6 _hash)
        math(EXPR _batch "0x${_hash} % ${_batches}")
        list(APPEND _ps2x_unity_${_batch} "${_source}")
    endforeach()
    math(EXPR _last "${_batches} - 1")
    foreach(_batch RANGE ${_last})
        if(_ps2x_unity_${_batch})
            set_source_files_properties(${_ps2x_unity_${_batch}} TARGET_DIRECTORY ${_target}
                                        PROPERTIES UNITY_GROUP "traced_${_batch}")
        endif()
    endforeach()
    set_target_properties(${_target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
endfunction()

# The headers every traced original includes first (ee/fncheck_traced.h's),
# precompiled: they are four fifths of a traced source's compile time
# (ps2_runtime.h through ps2_runtime_macros.h alone 3 of its 4.6 s). The
# generated headers by name (<...>), so that another game's target gets its
# own folder's (its include path puts that first).
option(PS2X_ENABLE_TRACED_PCH "Precompile the runtime headers for the traced originals" ON)
function(_ps2x_traced_pch _target)
    if(NOT PS2X_ENABLE_TRACED_PCH)
        return()
    endif()
    target_precompile_headers(${_target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "<ps2_recompiled_functions.h>"
        "<ps2_recompiled_stubs.h>"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/runtime/ee_scheduler.h")
endfunction()

# PS2X_GAME_NO_AI_TRACED: no traced originals of AI, for a build that only records and replays
# another game's corpus (6,000 objects fewer; AI's own corpus then replays nothing).
option(PS2X_GAME_NO_AI_TRACED "No traced originals of Aggressive Inline (another game's corpus build)" OFF)
# Each game's VU0 upload (ee/fncheck.h Vu0Upload): the VIF0 stream its boot sends VU0, vu0_upload.vif in its
# generated code's folder (runtime/tools/vu0_upload.py writes it from the game's ELF), built into ps2xFnCheck
# so its replays start microprograms on the code the game had uploaded. GAME|FILE.
# A game whose boot uploads VU0 code (vu0_upload.py's UPLOADS: BMX XXX's angleSin and angleCos start its
# microprogram) must have its upload, made for the recompile in that folder: vu0_upload.recomp holds the SHA-1 of
# the register_functions.cpp it was made beside. Without it, or after a recompile, the configure stops (the replays
# would run VCALLMS on empty or stale code memory and judge every call of those functions wrongly). Only where
# ps2xFnCheck is built (the dev flavour, not Android): the app's build does not use it.
set(PS2X_FNCHECK_VU0_UPLOADS "")
set(PS2X_VU0_UPLOAD_GAMES bmx)
function(_ps2x_vu0_upload _game _dir)
    set(_remake "python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/vu0_upload.py --game ${_game} --elf <the game's ELF> --out ${_dir}/vu0_upload.vif")
    if(_game STREQUAL "bmx")
        # (BMX XXX's recompile makes its upload in the same step)
        set(_remake "python3 -I ${CMAKE_SOURCE_DIR}/tooling/bmx/recomp/recompile.py, or ${_remake}")
    endif()
    if(_game IN_LIST PS2X_VU0_UPLOAD_GAMES AND ZAXIS_DEV_CHECKS AND NOT PS2X_IS_ANDROID)
        if(NOT EXISTS "${_dir}/vu0_upload.vif" OR NOT EXISTS "${_dir}/vu0_upload.recomp")
            message(FATAL_ERROR "readable: ${_game}: the generated code in ${_dir} has no VU0 upload (vu0_upload.vif and vu0_upload.recomp): ${_remake}")
        endif()
        file(SHA1 "${_dir}/register_functions.cpp" _recompile)
        file(STRINGS "${_dir}/vu0_upload.recomp" _made LIMIT_COUNT 1)
        if(NOT _made STREQUAL _recompile)
            message(FATAL_ERROR "readable: ${_game}: ${_dir}/vu0_upload.vif was made for another recompile of the game (vu0_upload.recomp ${_made}, register_functions.cpp ${_recompile}): ${_remake}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dir}/vu0_upload.recomp" "${_dir}/register_functions.cpp")
    endif()
    if(EXISTS "${_dir}/vu0_upload.vif")
        set(PS2X_FNCHECK_VU0_UPLOADS ${PS2X_FNCHECK_VU0_UPLOADS} "${_game}|${_dir}/vu0_upload.vif" PARENT_SCOPE)
        message(STATUS "readable: ${_game}: VU0 upload ${_dir}/vu0_upload.vif")
    endif()
endfunction()
set(PS2X_GAME_TRACED_SOURCES "")
if(PS2X_RUNNER_GENERATED_DIR)
    _ps2x_vu0_upload(ai "${PS2X_RUNNER_GENERATED_DIR}")
endif()
if(PS2X_RUNNER_GENERATED_DIR AND NOT PS2X_GAME_NO_AI_TRACED)
    set(_ps2x_game_addresses "")
    # Every registration (the headers' list).
    file(STRINGS "${PS2X_SYMBOLS_DIR}/ai/registered.tsv" _lines)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^ai\t(0x[0-9A-Fa-f]+)")
            math(EXPR _addr "${CMAKE_MATCH_1}" OUTPUT_FORMAT HEXADECIMAL)
            string(TOLOWER "${_addr}" _addr)
            list(APPEND _ps2x_game_addresses "${_addr}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _ps2x_game_addresses)
    _ps2x_traced_sources(PS2X_GAME_TRACED_SOURCES "" "${PS2X_RUNNER_GENERATED_DIR}" ${_ps2x_game_addresses})
    list(LENGTH PS2X_GAME_TRACED_SOURCES _count)
    message(STATUS "readable: ${_count} recompiled functions built with fncheck tracing")
endif()

if(PS2X_GAME_TRACED_SOURCES AND ZAXIS_DEV_CHECKS)
    add_library(ps2_game_traced OBJECT ${PS2X_GAME_TRACED_SOURCES})
    _ps2x_traced_unity(ps2_game_traced ${PS2X_GAME_TRACED_SOURCES})
    _ps2x_traced_pch(ps2_game_traced)
    add_dependencies(ps2_game_traced ps2_game_traced_blocks)
    target_link_libraries(ps2_game_traced PUBLIC ps2_runtime)
    target_include_directories(ps2_game_traced PRIVATE "${PS2X_RUNNER_GENERATED_DIR}" "${PS2X_GAME_DIR}")
    # (recording and ps2xFnCheck, neither of which runs on Android)
    if(NOT PS2X_IS_ANDROID)
        target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:ps2_game_traced>)
    endif()
endif()

# Other games' traced originals: each game's registrations (the _IN macros)
# from its folder in PS2X_RUNNER_EXTRA_GENERATED_DIRS, the one whose
# register_functions.cpp registers the game's ELF (ps2_generated::Registrar
# {"<namespace>", "<ELF>", ...}). Game names and namespaces as ee/game_id.h
# (PS2X_GAME_OTHER_GAMES, at the top).
# PS2X_GAME_CORPUS: the games whose recording corpus this build records and
# replays: every function of game/<game>/functions.tsv built traced and
# registered Planned, as AI's every function is, so PS2X_FNREC and ps2xFnCheck
# reach them. Off in an ordinary build: 4,500 more traced objects.
set(PS2X_GAME_CORPUS "" CACHE STRING "Games with a recording corpus, e.g. dm2 (game/<game>/functions.tsv)")
# The shared sources each game has an instance of: every source here is clean
# of address literals (readable_symbols.py lint), and what it calls is here too
# (a plain call of a function in a source left out is an undefined zge_<game>
# symbol at the link).
# DM2's and BMX XXX's are the engine each has, in every build with its bind table: every shared
# source of game/<game>/instance_sources.txt (its corpus build's list too). game/dm2 and game/bmx
# call that engine by name.
foreach(_game dm2 bmx)
    file(STRINGS "${PS2X_GAME_DIR}/${_game}/instance_sources.txt" PS2X_GAME_INSTANCE_SOURCES_${_game} REGEX "^[a-z]")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_GAME_DIR}/${_game}/instance_sources.txt")
endforeach()
# A corpus build has an instance of every source in game/<game>/instance_sources.txt: the
# shared engine, so its functions can be proven at this game's addresses.
foreach(_game IN LISTS PS2X_GAME_CORPUS)
    if(EXISTS "${PS2X_GAME_DIR}/${_game}/instance_sources.txt")
        file(STRINGS "${PS2X_GAME_DIR}/${_game}/instance_sources.txt" _ps2x_instance_rows REGEX "^[a-z]")
        set(PS2X_GAME_INSTANCE_SOURCES_${_game} ${_ps2x_instance_rows})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_GAME_DIR}/${_game}/instance_sources.txt")
    endif()
endforeach()
set(PS2X_GAME_INSTANCE_TARGETS "")
set(PS2X_GAME_TRACED_OTHER_TARGETS "")
set(PS2X_GAME_TRACED_ARCHIVES "")
foreach(_entry IN LISTS PS2X_GAME_OTHER_GAMES)
    string(REPLACE "|" ";" _entry "${_entry}")
    list(GET _entry 0 _game)
    list(GET _entry 1 _elf)
    set(_addresses "")
    file(STRINGS "${PS2X_SYMBOLS_DIR}/ai/registered.tsv" _lines)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^${_game}\t(0x[0-9A-Fa-f]+)")
            math(EXPR _addr "${CMAKE_MATCH_1}" OUTPUT_FORMAT HEXADECIMAL)
            string(TOLOWER "${_addr}" _addr)
            list(APPEND _addresses "${_addr}")
        endif()
    endforeach()
    # This game's instance (PS2X_GAME_INSTANCE, ee/readable.h): its own folder (game/<game>, which no other
    # instance compiles) and the shared sources compiled again with its symbols, zge and sce renamed so that
    # every function, inline and constant in them is its own, and each shared function registered at its
    # level (readable_symbols.py headers). The shared sources need the game's bind table; a folder that
    # names only its own addresses (its game's rows) is built with or without one. The game's generated code
    # is not needed: the instance reaches it through the function table at run time, so a build with none
    # (the app, which makes each game's code from the player's disc) has the same readable code as one with
    # it (the frame and held-register tables come from the committed ones then, below).
    set(_instance_sources ${PS2X_GAME_INSTANCE_SOURCES_${_game}})
    set(_folder_sources ${PS2X_GAME_FOLDER_SOURCES_${_game}})
    set(_bind_table "${PS2X_BIND_DIR}/${_game}.tsv")
    set(_bound FALSE)
    if(_instance_sources AND PS2X_BIND_DIR AND EXISTS "${_bind_table}")
        set(_bound TRUE)
    endif()
    # A folder's plain sources (plain_sources.txt) call the game's engine by name: they are compiled from
    # their entries' copies (readable_entries.py), and only with the instance that defines those names. A
    # build with no bind table has none of this game: its originals run.
    set(_folder_compiled "")
    set(_folder_plain FALSE)
    foreach(_source IN LISTS _folder_sources)
        ps2x_plain_source("${_source}" _compiled)
        list(APPEND _folder_compiled "${_compiled}")
        if(NOT _compiled STREQUAL _source)
            set(_folder_plain TRUE)
        endif()
    endforeach()
    if(_folder_plain AND NOT _bound)
        list(LENGTH _folder_sources _count)
        message(STATUS "readable: ${_game}: game/${_game} (${_count} sources) calls its engine by name: not built without ${_game}'s bind table")
        set(_folder_sources "")
        set(_folder_compiled "")
    endif()
    set(_instance_symbols "${PS2X_SYMBOLS_DIR}/${_game}")
    set(_instance_symbols_target ps2_game_symbols_${_game})
    set(_instance_paths "")
    if(_bound OR _folder_sources)
        # The game's own headers: its folder's names at its own addresses (and a registration there at level 2,
        # the status it is written with), the shared sources' through the bind table when there is one.
        set(_symbols_args headers --game ${_game} --status "${PS2X_GAME_DIR}/${_game}/symbols_status.tsv"
                          --out "${PS2X_SYMBOLS_DIR}/${_game}")
        set(_symbols_depends ${PS2X_GAME_SOURCES} "${PS2X_SYMBOLS_TOOL}" "${PS2X_SYMBOLS_TABLE}"
                             "${PS2X_GAME_DIR}/${_game}/symbols_status.tsv")
        # (the levels read close.tsv, and bound.tsv's rows extend the bind table)
        foreach(_levels_input close.tsv bound.tsv)
            if(EXISTS "${PS2X_GAME_DIR}/${_game}/${_levels_input}")
                list(APPEND _symbols_depends "${PS2X_GAME_DIR}/${_game}/${_levels_input}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_GAME_DIR}/${_game}/${_levels_input}")
            endif()
        endforeach()
        if(_bound)
            list(APPEND _symbols_args --bind "${_bind_table}" --instance ${_instance_sources})
            list(APPEND _symbols_depends "${_bind_table}")
            if(_game IN_LIST PS2X_GAME_CORPUS)
                list(APPEND _symbols_args --try-close)
            endif()
            # (a table's change regenerates at configure time, so the files whose headers it changes
            # are seen as out of date in the same build)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bind_table}")
            if(EXISTS "${PS2X_BIND_DIR}/${_game}.layout.tsv")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_BIND_DIR}/${_game}.layout.tsv")
            endif()
        endif()
        execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_SYMBOLS_TOOL}" ${_symbols_args}
                        RESULT_VARIABLE _ps2x_symbols_result)
        if(NOT _ps2x_symbols_result EQUAL 0)
            message(FATAL_ERROR "readable: ${_game}'s symbol headers failed")
        endif()
        # (the headers as byproducts, as AI's above: a status or level change recompiles their sources in one build)
        file(GLOB_RECURSE _symbols_headers "${PS2X_SYMBOLS_DIR}/${_game}/sym/*.h")
        add_custom_command(
            OUTPUT "${PS2X_SYMBOLS_DIR}/${_game}.stamp"
            BYPRODUCTS ${_symbols_headers}
            COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_SYMBOLS_TOOL}" ${_symbols_args}
            COMMAND "${CMAKE_COMMAND}" -E touch "${PS2X_SYMBOLS_DIR}/${_game}.stamp"
            DEPENDS ${_symbols_depends}
            COMMENT "readable: symbol headers (${_game})"
            VERBATIM)
        add_custom_target(ps2_game_symbols_${_game} DEPENDS "${PS2X_SYMBOLS_DIR}/${_game}.stamp")
    endif()
    if(_bound)
        foreach(_relative IN LISTS _instance_sources)
            ps2x_plain_source("${PS2X_GAME_DIR}/${_relative}" _compiled)
            list(APPEND _instance_paths "${_compiled}")
        endforeach()
    endif()
    if(_bound OR _folder_sources)
        add_library(ps2_game_readable_${_game} OBJECT ${_instance_paths} ${_folder_compiled})
        add_dependencies(ps2_game_readable_${_game} ${_instance_symbols_target} ps2_game_traced_blocks)
        target_link_libraries(ps2_game_readable_${_game} PUBLIC ps2_runtime)
        target_include_directories(ps2_game_readable_${_game} BEFORE PRIVATE "${_instance_symbols}")
        target_include_directories(ps2_game_readable_${_game} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PS2X_GAME_DIR}")
        # (sym and points too: their constants are `inline`, and one passed by reference would be
        # a weak symbol the link shares between the games)
        string(TOUPPER "${_game}" _game_upper)
        set(_instance_definitions PS2X_GAME_INSTANCE=${_game} PS2X_GAME_${_game_upper}=1
                                  zge=zge_${_game} sce=sce_${_game}
                                  sym=sym_${_game} points=points_${_game})
        target_compile_definitions(ps2_game_readable_${_game} PRIVATE ${_instance_definitions})
        target_compile_options(ps2_game_readable_${_game} PRIVATE ${PS2X_FP_NO_CONTRACT}
                               $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fwrapv>)
        if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(ps2_game_readable_${_game} PRIVATE /clang:-fwrapv)
        endif()
        target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:ps2_game_readable_${_game}>)
        list(APPEND PS2X_GAME_INSTANCE_TARGETS ps2_game_readable_${_game})
        # The folder's headers, each compiled on its own in the instance (game_headers.cmake's check).
        ps2x_game_header_units(ps2_game_headers_${_game} "${_instance_symbols}" ${PS2X_GAME_FOLDER_HEADERS_${_game}})
        if(TARGET ps2_game_headers_${_game})
            target_compile_definitions(ps2_game_headers_${_game} PRIVATE ${_instance_definitions})
            add_dependencies(ps2_game_headers_${_game} ${_instance_symbols_target})
            add_dependencies(ps2_game_readable_${_game} ps2_game_headers_${_game})
        endif()
        list(LENGTH _folder_sources _count)
        message(STATUS "readable: ${_game}: instance of game/${_game} (${_count} sources)")
    endif()
    if(_bound)
        file(STRINGS "${PS2X_SYMBOLS_DIR}/${_game}/levels.tsv" _lines)
        set(_body 0)
        set(_known 0)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^[^\t]+\t(0x[0-9A-Fa-f]+)\t([12])")
                math(EXPR _addr "${CMAKE_MATCH_1}" OUTPUT_FORMAT HEXADECIMAL)
                string(TOLOWER "${_addr}" _addr)
                list(APPEND _addresses "${_addr}")
                if(CMAKE_MATCH_2 EQUAL 2)
                    math(EXPR _body "${_body} + 1")
                else()
                    math(EXPR _known "${_known} + 1")
                endif()
            endif()
        endforeach()
        message(STATUS "readable: ${_game}: instance of ${_instance_sources}: ${_body} bodies, ${_known} addresses only")
    endif()
    list(REMOVE_DUPLICATES _addresses)
    set(_dir "")
    foreach(_extra IN LISTS PS2X_RUNNER_EXTRA_GENERATED_DIRS)
        file(STRINGS "${_extra}/register_functions.cpp" _registrar REGEX "Registrar g_ps2GeneratedCodeRegistrar_${_game}\\{\"${_game}\", \"${_elf}\"")
        if(_registrar)
            set(_dir "${_extra}")
        endif()
    endforeach()
    if(_game IN_LIST PS2X_GAME_CORPUS AND EXISTS "${PS2X_GAME_DIR}/${_game}/functions.tsv")
        # The functions the runtime replaces (tools/runtime_replaced.py: a generated body that is only a call of
        # ps2_stubs or ps2_syscalls, or a kernel call wrapper: the call number, the syscall and the return) are not
        # recorded: not the game's code, no readable form, and a replay of the runtime's code on a recording that
        # never saw what it reads or writes, nor the kernel's state (BMX XXX's sceDmaSend crashed the checker; its
        # SetSyscall wrote the syscall table's mirror outside the recording).
        if(_dir)
            execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_replaced.py" "${_dir}"
                            OUTPUT_VARIABLE _replaced_out RESULT_VARIABLE _replaced_rc)
            if(NOT _replaced_rc EQUAL 0)
                message(FATAL_ERROR "readable: ${_game}: tools/runtime_replaced.py failed on ${_dir}")
            endif()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_replaced.py")
            string(REGEX MATCHALL "0x[0-9a-f]+" _replaced_hex "${_replaced_out}")
            foreach(_hex IN LISTS _replaced_hex)
                math(EXPR _addr "${_hex}" OUTPUT_FORMAT HEXADECIMAL)
                string(TOLOWER "${_addr}" _addr)
                set(_ps2x_replaced_${_game}_${_addr} 1)
            endforeach()
        endif()
        set(_left_out 0)
        file(STRINGS "${PS2X_GAME_DIR}/${_game}/functions.tsv" _lines REGEX "^0x")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_GAME_DIR}/${_game}/functions.tsv")
        set(_rows "#include \"ee/readable.h\"\n// Generated by game.cmake from ${_game}/functions.tsv: registered to record, no C++ of its own.\n")
        set(_record_only 0)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^(0x[0-9A-Fa-f]+)\t[^\t]+\t[^\t]+\t([^\t]+)$")
                set(_label "${CMAKE_MATCH_2}")
                math(EXPR _addr "${CMAKE_MATCH_1}" OUTPUT_FORMAT HEXADECIMAL)
                string(TOLOWER "${_addr}" _addr)
                if(DEFINED _ps2x_replaced_${_game}_${_addr} AND NOT _addr IN_LIST _addresses)
                    math(EXPR _left_out "${_left_out} + 1")
                elseif(NOT _addr IN_LIST _addresses)
                    if(_label STREQUAL "-")
                        string(SUBSTRING "${_addr}" 2 -1 _hex)
                        set(_label "${_game}_${_hex}")
                    endif()
                    string(APPEND _rows "READABLE_PLANNED_IN(${_game}, ${_addr}u, \"${_label}\");\n")
                    list(APPEND _addresses "${_addr}")
                    math(EXPR _record_only "${_record_only} + 1")
                endif()
            endif()
        endforeach()
        set(_record_source "${CMAKE_CURRENT_BINARY_DIR}/fncheck_traced_${_game}/record_only.cpp")
        file(CONFIGURE OUTPUT "${_record_source}" CONTENT "${_rows}" @ONLY)
        add_library(ps2_game_record_${_game} OBJECT "${_record_source}")
        target_link_libraries(ps2_game_record_${_game} PUBLIC ps2_runtime)
        target_include_directories(ps2_game_record_${_game} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PS2X_GAME_DIR}")
        if(ZAXIS_DEV_CHECKS)
            target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:ps2_game_record_${_game}>)
        endif()
        list(APPEND PS2X_GAME_INSTANCE_TARGETS ps2_game_record_${_game})
        message(STATUS "readable: ${_game}: ${_record_only} functions registered to record only (${_left_out} the runtime replaces left out)")
    endif()
    list(LENGTH _addresses _registered)
    if(NOT _dir)
        if(_registered)
            message(STATUS "readable: ${_game}: ${_registered} registrations, no traced originals (no ${_elf} folder in PS2X_RUNNER_EXTRA_GENERATED_DIRS)")
        endif()
        continue()
    endif()
    _ps2x_vu0_upload(${_game} "${_dir}")
    _ps2x_traced_sources(_traced_sources "${_game}" "${_dir}" ${_addresses})
    list(LENGTH _traced_sources _count)
    message(STATUS "readable: ${_game}: ${_count} recompiled functions built with fncheck tracing from ${_dir}")
    if(_traced_sources AND ZAXIS_DEV_CHECKS)
        add_library(ps2_game_traced_${_game} OBJECT ${_traced_sources})
        _ps2x_traced_unity(ps2_game_traced_${_game} ${_traced_sources})
        _ps2x_traced_pch(ps2_game_traced_${_game})
        add_dependencies(ps2_game_traced_${_game} ps2_game_traced_blocks)
        target_link_libraries(ps2_game_traced_${_game} PUBLIC ps2_runtime)
        # The game's folder first: <ps2_recompiled_functions.h> is its own.
        target_include_directories(ps2_game_traced_${_game} BEFORE PRIVATE "${_dir}")
        target_include_directories(ps2_game_traced_${_game} PRIVATE "${PS2X_GAME_DIR}")
        if(_game IN_LIST PS2X_GAME_CORPUS)
            # (4,500 more objects would pass the link line's limit: one archive, linked whole for
            # the objects' registrations)
            add_library(ps2_game_traced_all_${_game} STATIC $<TARGET_OBJECTS:ps2_game_traced_${_game}>)
            target_link_libraries(ps2EntryRunner PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,ps2_game_traced_all_${_game}>")
            list(APPEND PS2X_GAME_TRACED_ARCHIVES ps2_game_traced_all_${_game})
        else()
            target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:ps2_game_traced_${_game}>)
            list(APPEND PS2X_GAME_TRACED_OTHER_TARGETS ps2_game_traced_${_game})
        endif()
    endif()
endforeach()

# The traced originals' block markers and the callee stack-argument table
# (tools/fncheck_traced_gen.py), and the readable functions' source digests
# (tools/readable_digest.py), for ps2xFnCheck (ee/fncheck_build.h). Files are
# written only when their text changes (the stamps are the outputs).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_ps2x_fncheck_build "${CMAKE_CURRENT_BINARY_DIR}/fncheck_build")
file(CONFIGURE OUTPUT "${_ps2x_fncheck_build}/traced_blocks.list" CONTENT "${PS2X_TRACED_BLOCK_LIST}" @ONLY)
set(_ps2x_scan_args "")
set(_ps2x_scan_depends "")
if(PS2X_RUNNER_GENERATED_DIR)
    list(APPEND _ps2x_scan_args --scan "0=${PS2X_RUNNER_GENERATED_DIR}")
    list(APPEND _ps2x_scan_depends "${PS2X_RUNNER_GENERATED_DIR}/register_functions.cpp")
endif()
foreach(_entry IN LISTS PS2X_GAME_OTHER_GAMES)
    string(REPLACE "|" ";" _entry "${_entry}")
    list(GET _entry 0 _game)
    list(GET _entry 1 _elf)
    foreach(_extra IN LISTS PS2X_RUNNER_EXTRA_GENERATED_DIRS)
        file(STRINGS "${_extra}/register_functions.cpp" _registrar REGEX "Registrar g_ps2GeneratedCodeRegistrar_${_game}\\{\"${_game}\", \"${_elf}\"")
        if(_registrar)
            list(APPEND _ps2x_scan_args --scan "${_ps2x_game_number_${_game}}=${_extra}")
            list(APPEND _ps2x_scan_depends "${_extra}/register_functions.cpp")
        endif()
    endforeach()
endforeach()
# The frame table (the original's frame size and register saves of every
# function, ee::frameLayout, and each callee's call site) and the held-register
# table (tools/readable_held_gen.py: the value of each callee-saved register at
# each call site of the original, for ee/held.h) go into every readable build:
# the runner and ps2xFnCheck enter readable functions with them. A build with
# the generated code makes them from it, as before (its registrations and its
# corpus' decide which functions are whole functions of several entry_ chunks, so
# a corpus build's differ). A build with none (the app before the player's disc
# gives it its code, a source build) makes them from the committed tables,
# runtime/tools/frames/<game>.tsv and runtime/tools/held/<game>.txt.xz, which are
# what the app's configuration makes: every build of that configuration checks
# them for the games it scans (ps2_frames_check, ps2_held_check: a stale table
# stops the build), and ps2_frames_write / ps2_held_write write them (the
# integration lane's, as readable_symbols.py bind --write).
set(_ps2x_frames_dir "${CMAKE_CURRENT_SOURCE_DIR}/tools/frames")
set(_ps2x_held_dir "${CMAKE_CURRENT_SOURCE_DIR}/tools/held")
file(GLOB _ps2x_frames_tables CONFIGURE_DEPENDS "${_ps2x_frames_dir}/*.tsv")
file(GLOB _ps2x_held_data CONFIGURE_DEPENDS "${_ps2x_held_dir}/*.txt.xz")
# (the games whose tables this build's configuration makes as the app's does: not a game whose corpus it records
# (every function of it registered), and AI only with its traced originals)
set(_ps2x_check_args "")
if(PS2X_RUNNER_GENERATED_DIR AND NOT PS2X_GAME_NO_AI_TRACED AND NOT "ai" IN_LIST PS2X_GAME_CORPUS)
    list(APPEND _ps2x_check_args --scan "0=${PS2X_RUNNER_GENERATED_DIR}")
endif()
foreach(_entry IN LISTS PS2X_GAME_OTHER_GAMES)
    string(REPLACE "|" ";" _entry "${_entry}")
    list(GET _entry 0 _game)
    list(GET _entry 1 _elf)
    if(_game IN_LIST PS2X_GAME_CORPUS)
        continue()
    endif()
    foreach(_extra IN LISTS PS2X_RUNNER_EXTRA_GENERATED_DIRS)
        file(STRINGS "${_extra}/register_functions.cpp" _registrar REGEX "Registrar g_ps2GeneratedCodeRegistrar_${_game}\\{\"${_game}\", \"${_elf}\"")
        if(_registrar)
            list(APPEND _ps2x_check_args --scan "${_ps2x_game_number_${_game}}=${_extra}")
        endif()
    endforeach()
endforeach()
set(_ps2x_frames_out "")
set(_ps2x_frames_args "")
if(_ps2x_scan_args)
    set(_ps2x_frames_out "${_ps2x_fncheck_build}/readable_frames.cpp")
    set(_ps2x_frames_args --frames "${_ps2x_fncheck_build}/readable_frames.cpp")
endif()
add_custom_command(
    OUTPUT "${_ps2x_fncheck_build}/traced_blocks.stamp"
    BYPRODUCTS ${PS2X_TRACED_BLOCK_FILES} "${_ps2x_fncheck_build}/fncheck_stack_args.cpp" ${_ps2x_frames_out}
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/fncheck_traced_gen.py"
            --list "${_ps2x_fncheck_build}/traced_blocks.list"
            --stack-args "${_ps2x_fncheck_build}/fncheck_stack_args.cpp" ${_ps2x_frames_args} ${_ps2x_scan_args}
    COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_fncheck_build}/traced_blocks.stamp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/fncheck_traced_gen.py" "${_ps2x_fncheck_build}/traced_blocks.list"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_indirect_calls.tsv" "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_args_dead.tsv"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_locals.tsv" "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_locals_dm2.tsv"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_args_dead_dm2.tsv"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_locals_bmx.tsv"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_args_dead_bmx.tsv"
            ${PS2X_TRACED_GENERATED_FILES} ${_ps2x_scan_depends}
    COMMENT "readable: block markers in the traced originals, the stack-argument table"
    VERBATIM)
add_custom_target(ps2_game_traced_blocks DEPENDS "${_ps2x_fncheck_build}/traced_blocks.stamp")
if(NOT _ps2x_scan_args)
    add_custom_command(
        OUTPUT "${_ps2x_fncheck_build}/readable_frames.cpp"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_frames.py" cpp
                --dir "${_ps2x_frames_dir}" --out "${_ps2x_fncheck_build}/readable_frames.cpp"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_fncheck_build}/readable_frames.cpp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_frames.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/fncheck_traced_gen.py"
                ${_ps2x_frames_tables}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_locals.tsv" "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_locals_dm2.tsv"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_locals_bmx.tsv"
        COMMENT "readable: the frame table, from tools/frames"
        VERBATIM)
endif()
set_source_files_properties("${_ps2x_fncheck_build}/readable_frames.cpp" PROPERTIES GENERATED TRUE)
target_sources(ps2_game_readable PRIVATE "${_ps2x_fncheck_build}/readable_frames.cpp")
add_dependencies(ps2_game_readable ps2_game_traced_blocks)
# Its programs are generated as code beside the held table (tools/readable_held_code.py, ee/held_program.h):
# _ps2x_held_chunks sources, always all of them, compiled in parallel. Made from the scanned folders, its depfile names
# every generated function it read, so a body changed in place makes it again; readable_held_unknown.tsv lists what
# it cannot give.
set(_ps2x_held_chunks 16) # (readable_held_code.py CHUNKS)
set(_ps2x_held_programs)
math(EXPR _ps2x_held_last "${_ps2x_held_chunks} - 1")
foreach(_k RANGE 0 ${_ps2x_held_last})
    if(_k LESS 10)
        set(_k "0${_k}")
    endif()
    list(APPEND _ps2x_held_programs "${_ps2x_fncheck_build}/readable_held_programs_${_k}.cpp")
endforeach()
if(_ps2x_scan_args)
    add_custom_command(
        OUTPUT "${_ps2x_fncheck_build}/readable_held.cpp"
        BYPRODUCTS "${_ps2x_fncheck_build}/readable_held_unknown.tsv" ${_ps2x_held_programs}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_gen.py"
                --list "${_ps2x_fncheck_build}/traced_blocks.list"
                --out "${_ps2x_fncheck_build}/readable_held.cpp"
                --unknown "${_ps2x_fncheck_build}/readable_held_unknown.tsv"
                --chunks ${_ps2x_held_chunks}
                --depfile "${_ps2x_fncheck_build}/readable_held.d" --jobs 6 ${_ps2x_scan_args}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_fncheck_build}/readable_held.cpp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_gen.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_code.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/fncheck_traced_gen.py"
                "${_ps2x_fncheck_build}/traced_blocks.list" ${_ps2x_scan_depends}
        DEPFILE "${_ps2x_fncheck_build}/readable_held.d"
        COMMENT "readable: the held-register table and its programs"
        VERBATIM)
else()
    add_custom_command(
        OUTPUT "${_ps2x_fncheck_build}/readable_held.cpp"
        BYPRODUCTS ${_ps2x_held_programs}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_gen.py"
                --from "${_ps2x_held_dir}" --out "${_ps2x_fncheck_build}/readable_held.cpp" --chunks ${_ps2x_held_chunks}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_fncheck_build}/readable_held.cpp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_gen.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_code.py"
                ${_ps2x_held_data}
        COMMENT "readable: the held-register table and its programs, from tools/held"
        VERBATIM)
endif()
if(_ps2x_check_args)
    add_custom_command(
        OUTPUT "${_ps2x_fncheck_build}/readable_frames_check.stamp"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_frames.py" check
                --list "${_ps2x_fncheck_build}/traced_blocks.list" --dir "${_ps2x_frames_dir}" ${_ps2x_check_args}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_fncheck_build}/readable_frames_check.stamp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_frames.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/fncheck_traced_gen.py"
                "${_ps2x_fncheck_build}/traced_blocks.list" ${_ps2x_frames_tables} ${_ps2x_scan_depends}
        COMMENT "readable: the committed frame tables against the generated code"
        VERBATIM)
    add_custom_command(
        OUTPUT "${_ps2x_fncheck_build}/readable_held_check.stamp"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_gen.py"
                --list "${_ps2x_fncheck_build}/traced_blocks.list" --check "${_ps2x_held_dir}"
                --depfile "${_ps2x_fncheck_build}/readable_held_check.d" --jobs 6 ${_ps2x_check_args}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_fncheck_build}/readable_held_check.stamp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_gen.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_code.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/fncheck_traced_gen.py"
                "${_ps2x_fncheck_build}/traced_blocks.list" ${_ps2x_held_data} ${_ps2x_scan_depends}
        DEPFILE "${_ps2x_fncheck_build}/readable_held_check.d"
        COMMENT "readable: the committed held data against the generated code"
        VERBATIM)
    add_custom_target(ps2_game_tables_check DEPENDS "${_ps2x_fncheck_build}/readable_frames_check.stamp"
                                                    "${_ps2x_fncheck_build}/readable_held_check.stamp")
    add_dependencies(ps2_game_readable ps2_game_tables_check)
    # (the integration lane's: the checked games' tables written into the tree, as readable_symbols.py bind --write)
    add_custom_target(ps2_frames_write
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_frames.py" write
                --list "${_ps2x_fncheck_build}/traced_blocks.list" --dir "${_ps2x_frames_dir}" ${_ps2x_check_args}
        VERBATIM)
    add_custom_target(ps2_held_write
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_held_gen.py"
                --list "${_ps2x_fncheck_build}/traced_blocks.list" --write "${_ps2x_held_dir}" --jobs 6 ${_ps2x_check_args}
        VERBATIM)
endif()
set_source_files_properties("${_ps2x_fncheck_build}/readable_held.cpp" ${_ps2x_held_programs} PROPERTIES GENERATED TRUE)
target_sources(ps2_game_readable PRIVATE "${_ps2x_fncheck_build}/readable_held.cpp" ${_ps2x_held_programs})
set(_ps2x_digest_inputs "${PS2X_GAME_DIR}")
# (the registrations' addresses: the symbol table, and a game folder's of AI's names through the bind tables)
set(_ps2x_digest_depends ${PS2X_GAME_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_symbols.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_symbols.tsv")
foreach(_ps2x_digest_game IN ITEMS dm2 bmx)
    foreach(_ps2x_digest_bind "${PS2X_BIND_DIR}/${_ps2x_digest_game}.tsv" "${PS2X_GAME_DIR}/${_ps2x_digest_game}/bound.tsv")
        if(PS2X_BIND_DIR AND EXISTS "${_ps2x_digest_bind}")
            list(APPEND _ps2x_digest_depends "${_ps2x_digest_bind}")
        endif()
    endforeach()
endforeach()
add_custom_command(
    OUTPUT "${_ps2x_fncheck_build}/readable_digests.stamp"
    BYPRODUCTS "${_ps2x_fncheck_build}/readable_digests.cpp"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_digest.py"
            --cpp "${_ps2x_fncheck_build}/readable_digests.cpp" "--bind-dir=${PS2X_BIND_DIR}" ${_ps2x_digest_inputs}
    COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_fncheck_build}/readable_digests.stamp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_digest.py" ${_ps2x_digest_depends}
    COMMENT "readable: the readable functions' source digests"
    VERBATIM)
add_custom_target(ps2_game_readable_digests DEPENDS "${_ps2x_fncheck_build}/readable_digests.stamp")
# The readable ratchet (tools/readable_lint.py, the user's bar function by
# function): a function that passed may not stop passing, so the build fails.
add_custom_command(
    OUTPUT "${_ps2x_fncheck_build}/readable_lint.stamp"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_lint.py" --game "${PS2X_GAME_DIR}"
            --ratchet --stamp "${_ps2x_fncheck_build}/readable_lint.stamp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_lint.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_lint_baseline.txt"
            ${PS2X_GAME_SOURCES} ${_ps2x_game_headers}
    COMMENT "readable: the ratchet"
    VERBATIM)
add_custom_target(ps2_game_readable_lint DEPENDS "${_ps2x_fncheck_build}/readable_lint.stamp")

if(NOT PS2X_IS_ANDROID AND ZAXIS_DEV_CHECKS)
    # The VU0 uploads (above) as fncheck::vu0Uploads(), written only when its text changes.
    set(_ps2x_vu0_arrays "")
    set(_ps2x_vu0_rows "")
    foreach(_vu0_entry IN LISTS PS2X_FNCHECK_VU0_UPLOADS)
        string(REPLACE "|" ";" _vu0_entry "${_vu0_entry}")
        list(GET _vu0_entry 0 _vu0_game)
        list(GET _vu0_entry 1 _vu0_file)
        file(READ "${_vu0_file}" _hex HEX)
        string(LENGTH "${_hex}" _digits)
        math(EXPR _quads "${_digits} % 8")
        if(_digits EQUAL 0 OR NOT _quads EQUAL 0)
            message(FATAL_ERROR "${_vu0_file}: not a VIF0 stream (whole 32-bit words, at least one)")
        endif()
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
        string(SUBSTRING "${_vu0_game}" 0 1 _first)
        string(SUBSTRING "${_vu0_game}" 1 -1 _rest)
        string(TOUPPER "${_first}" _first)
        string(APPEND _ps2x_vu0_arrays "        // ${_vu0_game}'s (vu0_upload.vif)\n        const uint8_t k${_first}${_rest}Upload[] = {${_bytes}};\n")
        string(APPEND _ps2x_vu0_rows "            {game::Game::${_first}${_rest}, k${_first}${_rest}Upload, sizeof(k${_first}${_rest}Upload)},\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vu0_file}")
    endforeach()
    if(_ps2x_vu0_rows)
        set(_ps2x_vu0_body "        static const Vu0Upload uploads[] = {\n${_ps2x_vu0_rows}        };\n        return uploads;\n")
    else()
        set(_ps2x_vu0_body "        return {};\n")
    endif()
    file(CONFIGURE OUTPUT "${_ps2x_fncheck_build}/vu0_uploads.cpp" CONTENT
"// Each game's VU0 upload (game.cmake, ee/fncheck.h Vu0Upload): generated, do not edit.
#include \"ee/fncheck.h\"

namespace fncheck
{
    namespace
    {
${_ps2x_vu0_arrays}    }

    std::span<const Vu0Upload> vu0Uploads()
    {
${_ps2x_vu0_body}    }
}
" @ONLY)
    add_executable(ps2xFnCheck "${PS2X_GAME_DIR}/ee/fncheck_main.cpp" $<TARGET_OBJECTS:ps2_game_readable>
        "${_ps2x_fncheck_build}/fncheck_stack_args.cpp" "${_ps2x_fncheck_build}/readable_digests.cpp"
        "${_ps2x_fncheck_build}/vu0_uploads.cpp")
    add_dependencies(ps2xFnCheck ps2_game_traced_blocks ps2_game_readable_digests)
    if(NOT PS2X_GAME_CORPUS) # (AI's ratchet is AI's builds' to run)
        add_dependencies(ps2xFnCheck ps2_game_readable_lint)
    endif()
    target_include_directories(ps2xFnCheck PRIVATE "${PS2X_SYMBOLS_DIR}/ai")
    # Hand-written or drafted functions that leave out an entry block a resume
    # can reach: a line in the build's output, not a failure (readable_marks.py).
    add_custom_command(TARGET ps2xFnCheck POST_BUILD
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/readable_marks.py"
                --check "${CMAKE_BINARY_DIR}" --tree "${CMAKE_SOURCE_DIR}" --warn
        VERBATIM)
    if(TARGET ps2_game_traced)
        target_sources(ps2xFnCheck PRIVATE $<TARGET_OBJECTS:ps2_game_traced>)
    endif()
    foreach(_target IN LISTS PS2X_GAME_TRACED_OTHER_TARGETS PS2X_GAME_INSTANCE_TARGETS)
        target_sources(ps2xFnCheck PRIVATE $<TARGET_OBJECTS:${_target}>)
    endforeach()
    foreach(_target IN LISTS PS2X_GAME_TRACED_ARCHIVES)
        target_link_libraries(ps2xFnCheck PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_target}>")
    endforeach()
    target_link_libraries(ps2xFnCheck PRIVATE ps2_runtime)
    target_include_directories(ps2xFnCheck PRIVATE "${PS2X_GAME_DIR}")
    target_compile_options(ps2xFnCheck PRIVATE ${PS2X_FP_NO_CONTRACT})
    # lld-link folds identical functions with /OPT:REF (its default in a
    # release link) even when their address is taken: readable functions are
    # registered and called by host address, so only safe folding (as the
    # runner, cmake/ReleaseMode.cmake). Without it 241 of the sweep's
    # registrations shared a host function in ps2xFnCheck.exe.
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_link_options(ps2xFnCheck PRIVATE /OPT:SAFEICF)
    endif()
    # A replayed call runs the readable body and the traced original on the
    # host stack, so a deep guest recursion is a deep host one: Linux's 8 MiB,
    # as the runner (Windows' 1 MiB default overflowed on DM2 searchTreeWalk's
    # cov3-frontend and cov3-fr-woodward recordings: 0xC00000FD).
    if(WIN32)
        if(MSVC)
            target_link_options(ps2xFnCheck PRIVATE "/STACK:8388608")
        elseif(MINGW)
            target_link_options(ps2xFnCheck PRIVATE "-Wl,--stack,8388608")
        endif()
    endif()
endif()

# The extras' setup makes AI's Xbox skaters with the engine's model converter
# (collection/extras/ai_xbox_skaters.h engineXboxModelMaker): built into the
# app, beside AI's game code, and not into the game (ps2_game_readable).
target_sources(ps2EntryRunner PRIVATE "${PS2X_GAME_DIR}/zge/native/xbox_models.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/extras/xbox_model_maker.cpp")
set_source_files_properties("${PS2X_GAME_DIR}/zge/native/xbox_models.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/extras/xbox_model_maker.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    INCLUDE_DIRECTORIES "${PS2X_GAME_DIR}")

# The native asset readers checked against the build tools' output.
if(NOT PS2X_IS_ANDROID)
    add_executable(ps2xNativeAssets "${PS2X_GAME_DIR}/zge/native/native_assets_main.cpp"
        "${PS2X_GAME_DIR}/zge/native/texture_library.cpp" "${PS2X_GAME_DIR}/zge/native/native_textures.cpp"
        "${PS2X_GAME_DIR}/zge/native/disc_files.cpp" "${PS2X_GAME_DIR}/zge/native/geo_file.cpp"
        "${PS2X_GAME_DIR}/zge/native/geo_meshes.cpp" "${PS2X_GAME_DIR}/zge/native/xbox_models.cpp")
    target_link_libraries(ps2xNativeAssets PRIVATE ps2_runtime)
    target_include_directories(ps2xNativeAssets PRIVATE "${PS2X_GAME_DIR}")
endif()
