cmake_minimum_required(VERSION 3.21)

project(zx_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(gs_cache)
endif()

# The guest function table the runtime tests register (and recompiler/tests too).
add_library(zx_test_function_table OBJECT
    src/test_function_table.cpp
)

target_include_directories(zx_test_function_table PRIVATE
    ${CMAKE_SOURCE_DIR}/ps2/include
    ${CMAKE_SOURCE_DIR}/ps2
    ${CMAKE_SOURCE_DIR}/iop/include
)

add_library(zx_test_lib STATIC
    src/pad_input_tests.cpp
    src/host_input_tests.cpp
    src/io_tests.cpp
    src/kernel_tests.cpp
    src/interrupt_tests.cpp
    src/machine_lifecycle_tests.cpp
    src/memory_tests.cpp
    src/async_graphics_tests.cpp
    src/vu1_tests.cpp
    src/vu0_math_tests.cpp
    src/gs_tests.cpp
    src/iop_tests.cpp
    src/sif_rpc_tests.cpp
    src/sif_dma_tests.cpp
    src/logic_check_tests.cpp
    src/debug_control_tests.cpp
    src/disc_image_tests.cpp
    src/disc_formats_tests.cpp
    src/app_support_tests.cpp
    src/collection_tests.cpp
    src/updater_tests.cpp
    src/front_end_tests.cpp
    src/game_setup_tests.cpp
    src/settings_tests.cpp
    src/controls_defaults_tests.cpp
    src/zge_settings_page_tests.cpp
    src/host_pacing_tests.cpp
    src/host_window_tests.cpp
    # The games' volume rows (a change read as made until the game's frame has passed it).
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_volumes.cpp
    src/boot_config_tests.cpp
    src/play_recording_tests.cpp
    src/recordings_tests.cpp
    src/ghost_rider_tests.cpp
    src/session_tests.cpp
    src/game_module_tests.cpp
    src/gs_quality_tests.cpp
    src/gs_interpolate_tests.cpp
    src/gpu_texture_check_tests.cpp
    src/glyph_art_tests.cpp
    src/panel_tests.cpp
    # (Aggressive Inline's capture of its RAM for the panel: the tests feed it a made-up RAM)
    ${CMAKE_SOURCE_DIR}/games/aggressive/overrides/aggressive_inline_panel.cpp
    # (DM2's capture of its RAM for the panel, fed a made-up RAM the same way)
    ${CMAKE_SOURCE_DIR}/games/dm2/overrides/dave_mirra_panel.cpp
    # (and BMX XXX's)
    ${CMAKE_SOURCE_DIR}/games/bmx/overrides/bmx_xxx_panel.cpp
    src/photo_tests.cpp
    src/save_store_tests.cpp
    src/game_files_tests.cpp
    src/zal_tests.cpp
    src/xbox_files_tests.cpp
    src/extras_tests.cpp
    src/zaxis_xbox_tests.cpp
    src/zaxis_zoo_tests.cpp
    src/zaxis_estate_tests.cpp
    src/zaxis_mesh_tests.cpp
    src/zaxis_text_tests.cpp
    src/texture_replace_tests.cpp
    src/texture_pack_tests.cpp
    src/texture_dds_tests.cpp
    src/gs_texture_audit_tests.cpp
    src/pss_movie_tests.cpp
    src/spu_voice_tests.cpp
    # The library ports against AI's recompiled originals (library_ports.tsv; the originals below).
    src/library_tests.cpp
    src/readable_tests.cpp
    # A traced original of two entry_ chunks, as game.cmake builds one (ee/fncheck_traced.h).
    src/fncheck_chunks_traced.cpp
    src/readable_script_tests.cpp
    src/career_scripts_tests.cpp
    # The career scripts setting's script edit (and its install, unused here).
    ${CMAKE_SOURCE_DIR}/games/aggressive/overrides/aggressive_inline_careerscripts.cpp
    # DM2's unlock cheats and Create Player's riders (the pure part).
    src/dm2_cheats_tests.cpp
    ${CMAKE_SOURCE_DIR}/games/dm2/overrides/dave_mirra_cheats.cpp
    # The movies' form and the sound driver that goes with it.
    src/zge_movie_form_tests.cpp
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_movie_form.cpp
    # Exit to Collection's menu edits in DM2 and BMX XXX (and their installs, unused here).
    src/exit_menu_tests.cpp
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_exit.cpp
    ${CMAKE_SOURCE_DIR}/games/dm2/overrides/dave_mirra_exit.cpp
    ${CMAKE_SOURCE_DIR}/games/dm2/overrides/dave_mirra_restart.cpp
    ${CMAKE_SOURCE_DIR}/games/dm2/overrides/dave_mirra_practice.cpp
    # (its pause menu's Photo Mode line starts photo mode, its Settings line shows the Settings page)
    ${CMAKE_SOURCE_DIR}/games/dm2/overrides/dave_mirra_photo.cpp
    ${CMAKE_SOURCE_DIR}/games/dm2/overrides/dave_mirra_settings.cpp
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_settings.cpp
    # (the app's Settings... request the pause hooks consume)
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_settings_open.cpp
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_photo.cpp
    # Practice's restart point and timer (the games' keys, the tries' clock).
    src/zge_practice_tests.cpp
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_practice.cpp
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_practice_rider.cpp
    ${CMAKE_SOURCE_DIR}/engine/overrides/zge_sites.cpp
    ${CMAKE_SOURCE_DIR}/games/bmx/overrides/bmx_xxx_exit.cpp
    ${CMAKE_SOURCE_DIR}/games/bmx/overrides/bmx_xxx_restart.cpp
    ${CMAKE_SOURCE_DIR}/games/bmx/overrides/bmx_xxx_practice.cpp
    # (its pause menu's Photo Mode line starts photo mode, its Settings line shows the Settings page, as DM2's)
    ${CMAKE_SOURCE_DIR}/games/bmx/overrides/bmx_xxx_photo.cpp
    ${CMAKE_SOURCE_DIR}/games/bmx/overrides/bmx_xxx_settings.cpp
    # The readable game's framework (its game code is tested by zx-fncheck).
    ${CMAKE_SOURCE_DIR}/ps2/ee/abi.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/held.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/fiber_posix.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/fiber_windows.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/fncheck.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/fnrec_file.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/profile.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/tailcall_stats.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/cycletrace.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/readable.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/guest_calls.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/diffcall.cpp
    # The level scripts' machine, run on a handler written for the test.
    ${CMAKE_SOURCE_DIR}/games/aggressive/script/vm.cpp
    ${CMAKE_SOURCE_DIR}/games/aggressive/script/objects.cpp
    ${CMAKE_SOURCE_DIR}/games/aggressive/script/natives.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/text/strings.cpp
    # The string tables call the C string functions by name (and those libc).
    ${CMAKE_SOURCE_DIR}/engine/zge/text/cstring.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/libc.cpp
    # The string tables' memory comes from the heap (heapAllocate by name), the heap's regions from pools, which keep lists and pointer arrays (utilities.cpp, with the matrix and angle math its other functions call).
    ${CMAKE_SOURCE_DIR}/engine/zge/memory/regions.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/memory/pool.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/utilities.cpp
    # (its box sweep tells the sweep's sweepPassLow by name)
    ${CMAKE_SOURCE_DIR}/engine/zge/collision/sweep.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/container/array.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/math/matrix.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/math/angle.cpp
    # The matrix math copies rotations with the platform's matrixCopyRotation by name (and the platform's clock
    # is the cycle counter's).
    ${CMAKE_SOURCE_DIR}/engine/zge/platform/platform.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/debug/cycle_counter.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/libgcc.cpp
    # The other library ports, for library_tests.cpp.
    ${CMAKE_SOURCE_DIR}/ps2/sce/libm.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/mprec.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/libc_strtod.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/libc_printf.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/libc_scanf.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/libc_stdio.cpp
    ${CMAKE_SOURCE_DIR}/ps2/sce/libc_malloc.cpp
    # GEO data read on the host (the collision chunks' reader).
    ${CMAKE_SOURCE_DIR}/engine/zge/native/geo_file.cpp
    # The level setup's load-time words made on the host (and the closest
    # point it uses).
    ${CMAKE_SOURCE_DIR}/engine/zge/native/collision_setup.cpp
    ${CMAKE_SOURCE_DIR}/games/aggressive/level/collision_setup_host.cpp
    ${CMAKE_SOURCE_DIR}/engine/zge/collision/closest.cpp
    ${CMAKE_SOURCE_DIR}/ps2/ee/suspend.cpp
)

# The game sources above that are plain sources (plain_sources.txt) are compiled from a copy that differs from the
# source only where the AI build's copy (readable_entries.py, game.cmake) makes a version's own body (_OWN)
# READABLE_OWN_INLINE: inline here as there, so this build emits nothing of it either, and an own body that calls its
# game's _ONLY functions (DM2's scaleMatrixDiagonal: copyRotationAndTranslation, which no build but DM2's instance
# has) leaves no unresolved call. Their plain calls stay direct, as written: this binary has no function table.
# But the libraries' (sce/): those are compiled from the build's own copy (each registered body's entry, as
# game.cmake's), so that a port's calls go where the game's do (game::call: a function on directly, in its own
# frame, any other through the table), which the library ports' tests (library_tests.cpp) need: their ports'
# callees are the originals in the table.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_zx_test_entries_tool "${CMAKE_SOURCE_DIR}/tools/readable/readable_entries.py")
file(STRINGS "${ZX_GAMES_DIR}/plain_sources.txt" _zx_test_plain_prefixes REGEX "^[a-z]")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ZX_GAMES_DIR}/plain_sources.txt")
get_target_property(_zx_test_sources zx_test_lib SOURCES)
set(_zx_test_compiled "")
foreach(_source IN LISTS _zx_test_sources)
    set(_plain FALSE)
    # (a readable source: one of games/game.cmake's seven folders, known by its key; not a game's overrides)
    set(_relative "")
    foreach(_zx_test_dir IN LISTS ZX_READABLE_DIRS)
        string(FIND "${_source}" "${_zx_test_dir}/" _at)
        if(_at EQUAL 0 AND NOT _source MATCHES "^${ZX_GAMES_DIR}/[^/]+/(overrides|tools|docs|workspace)/")
            zx_readable_key(_relative "${_source}")
        endif()
    endforeach()
    if(_relative)
        foreach(_prefix IN LISTS _zx_test_plain_prefixes)
            string(LENGTH "${_prefix}" _length)
            string(SUBSTRING "${_relative}" 0 ${_length} _head)
            if(_head STREQUAL _prefix)
                set(_plain TRUE)
            endif()
        endforeach()
    endif()
    if(NOT _plain)
        list(APPEND _zx_test_compiled "${_source}")
        continue()
    endif()
    set(_copy "${CMAKE_CURRENT_BINARY_DIR}/own_inline/${_relative}")
    set(_mode --own-inline)
    if(_relative MATCHES "^sce/")
        set(_copy "${CMAKE_CURRENT_BINARY_DIR}/readable_entries/${_relative}")
        set(_mode "")
    endif()
    add_custom_command(
        OUTPUT "${_copy}"
        COMMAND "${Python3_EXECUTABLE}" -B "${_zx_test_entries_tool}" ${_mode} --line-root "${CMAKE_BINARY_DIR}" "${_source}" "${_copy}"
        DEPENDS "${_source}" "${_zx_test_entries_tool}" "${CMAKE_SOURCE_DIR}/tools/readable/readable_entry_reads.tsv"
        COMMENT "readable: the unit tests' copy of ${_relative}"
        VERBATIM)
    get_filename_component(_dir "${_source}" DIRECTORY)
    # (its own folder's headers by -iquote, as game.cmake's copies: never a system header's name on -I)
    if(MSVC)
        set_source_files_properties("${_copy}" PROPERTIES GENERATED TRUE INCLUDE_DIRECTORIES "${_dir}")
    else()
        set_source_files_properties("${_copy}" PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-iquote;${_dir}")
    endif()
    list(APPEND _zx_test_compiled "${_copy}")
endforeach()
set_property(TARGET zx_test_lib PROPERTY SOURCES ${_zx_test_compiled})

# (the generated code's headers fncheck_traced.h includes: empty, this binary has no generated code)
set_source_files_properties(src/fncheck_chunks_traced.cpp TARGET_DIRECTORY zx_test_lib
                            PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/traced_stub")

target_include_directories(zx_test_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/runtime/symbols/ai
    ${CMAKE_BINARY_DIR}/runtime/symbols/overrides
    ${CMAKE_SOURCE_DIR}/ps2/include
    ${CMAKE_SOURCE_DIR}/ps2/runtime
    ${ZX_READABLE_ROOTS}
)

target_link_libraries(zx_test_lib PRIVATE
    zx_runtime
    ps2_iop
    zx_miniz # (the tests write zips: src/test_zip_builder.h; and CSO images' deflate blocks)
    zx::lz4 # (ZSO and CSO v2 images' LZ4 blocks: src/disc_formats_tests.cpp)
    zx::zal # (the .ZAL archives: src/zal_tests.cpp)
)
# (the override sources it builds name their game's symbols: tools/readable/override_symbols.py)
add_dependencies(zx_test_lib zx_override_symbols)

# The tree's resources (the shipped font) by the source tree's path: a Ninja build writes __FILE__ relative to the
# build folder, so a path made from it depended on the folder the tests were started in.
target_compile_definitions(zx_test_lib PRIVATE "ZX_TEST_RESOURCES_DIR=\"${CMAKE_SOURCE_DIR}/collection/resources\"")

# The GPU renderer's tests (skipped without an SDL_GPU device).
if(TARGET zx_gs_gpu)
    target_link_libraries(zx_test_lib PRIVATE zx_gs_gpu)
    target_compile_definitions(zx_test_lib PRIVATE ZX_HAS_GS_GPU=1)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(zx_test_lib PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(zx-tests
    src/main.cpp
    $<TARGET_OBJECTS:zx_test_function_table>
)

# The readable tests run zx-fncheck's own test of records that stop at a call (--self-test-stops: the right version
# passes, the broken ones fail) where this build makes the checker.
if(TARGET zx-fncheck)
    target_compile_definitions(zx_test_lib PRIVATE "ZX_FNCHECK_EXE=\"$<TARGET_FILE:zx-fncheck>\"")
    add_dependencies(zx-tests zx-fncheck)
endif()

option(PRINT_GENERATED_CODE "Print generated code in tests" OFF)
if(PRINT_GENERATED_CODE)

    target_compile_definitions(zx-tests PRIVATE PRINT_GENERATED_CODE)
    target_compile_definitions(zx_test_lib PRIVATE PRINT_GENERATED_CODE)
endif()

target_include_directories(zx-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ps2/include
)

# The whole test library: a static archive's member that nothing references is left out by the linker (macOS's ld
# dropped libc_malloc.cpp's object, and with it its registrations, so ps2_library_tests' suite found no port for
# __malloc_lock and did not run on the Mac; cov4). Every member's static registrations must be in the binary.
target_link_libraries(zx-tests PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,zx_test_lib>"
    zx_panel
    zx_runtime
)

# Two game modules of the tests' own (game_module/), built and linked as the runner's are with
# ZX_GAME_MODULES=static: game_module_tests.cpp checks each registers its own code in one program.
if(COMMAND zx_add_game_module AND ZX_GAME_MODULES STREQUAL "static")
    set(ZX_GAME_MODULE_TARGETS "")
    set(ZX_GAME_MODULE_IDS "")
    zx_add_game_module(zxa "${CMAKE_CURRENT_SOURCE_DIR}/game_module/zxa")
    zx_add_game_module(zxb "${CMAKE_CURRENT_SOURCE_DIR}/game_module/zxb")
    zx_link_game_modules(zx-tests)
    target_compile_definitions(zx-tests PRIVATE ZX_TEST_LINKED_MODULES=1)
endif()

# The library ports' recompiled originals (library_tests.cpp): in a build with AI's generated code, every
# function of src/library_ports.tsv (a port, or what one calls) built from its generated file with fncheck's
# tracing and block markers, as game.cmake builds the traced originals (its _zx_traced_sources, here into this
# folder, and tools/readable/fncheck_traced_gen.py for the markers), and registered as AI's traced originals. Their data
# is AI's ELF (ZX_TEST_AI_ELF, or the environment's at run time: a build-time input, never copied into the
# tree); without generated code or the ELF the tests say they did not run.
set(ZX_TEST_AI_ELF "" CACHE FILEPATH "AI's ELF (SLUS_203.27) for the library ports' tests (default: disc-root/SLUS_203.27 beside ZX_RUNNER_GENERATED_DIR)")
set(_zx_library_elf "${ZX_TEST_AI_ELF}")
if(NOT _zx_library_elf AND ZX_RUNNER_GENERATED_DIR AND EXISTS "${ZX_RUNNER_GENERATED_DIR}/../disc-root/SLUS_203.27")
    get_filename_component(_zx_library_elf "${ZX_RUNNER_GENERATED_DIR}/../disc-root/SLUS_203.27" ABSOLUTE)
endif()
set(_zx_library_out "${CMAKE_CURRENT_BINARY_DIR}/library_ports")
set(_zx_library_rows "")
set(_zx_library_names "")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/library_ports.tsv" _zx_library_lines REGEX "^[A-Za-z_]")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/library_ports.tsv")
foreach(_line IN LISTS _zx_library_lines)
    if(_line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)\t(port|callee|system|data)(\t([^\t]+))?$")
        set(_zx_library_role_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        set(_zx_library_file_${CMAKE_MATCH_1} "${CMAKE_MATCH_4}")
        list(APPEND _zx_library_names "${CMAKE_MATCH_1}")
    endif()
endforeach()
# Each name's address in AI, from the symbol table (AI's rows: no game column).
file(STRINGS "${CMAKE_SOURCE_DIR}/tools/readable/readable_symbols.tsv" _zx_symbol_rows REGEX "^(function|data)\t[A-Za-z_][A-Za-z0-9_]*\t0x[0-9A-Fa-f]+\t\t$")
set(_zx_library_traced_addresses "")
foreach(_row IN LISTS _zx_symbol_rows)
    if(_row MATCHES "^(function|data)\t([A-Za-z_][A-Za-z0-9_]*)\t0x([0-9A-Fa-f]+)\t")
        set(_name "${CMAKE_MATCH_2}")
        if(DEFINED _zx_library_role_${_name} AND NOT DEFINED _zx_library_address_${_name})
            string(TOLOWER "${CMAKE_MATCH_3}" _hex)
            math(EXPR _addr "0x${_hex}" OUTPUT_FORMAT HEXADECIMAL)
            string(TOLOWER "${_addr}" _addr)
            set(_zx_library_address_${_name} "${_addr}")
        endif()
    endif()
endforeach()
foreach(_name IN LISTS _zx_library_names)
    if(NOT DEFINED _zx_library_address_${_name})
        message(FATAL_ERROR "tests/src/library_ports.tsv: ${_name} has no AI row in tools/readable/readable_symbols.tsv")
    endif()
    set(_role "${_zx_library_role_${_name}}")
    string(APPEND _zx_library_rows "    {\"${_name}\", ${_zx_library_address_${_name}}u, LibraryRole::${_role}, \"${_zx_library_file_${_name}}\"},\n")
    if(_role STREQUAL "port" OR _role STREQUAL "callee")
        list(APPEND _zx_library_traced_addresses "${_zx_library_address_${_name}}")
    endif()
endforeach()
file(CONFIGURE OUTPUT "${_zx_library_out}/library_ports_table.h" CONTENT
"// Generated by tests/CMakeLists.txt from src/library_ports.tsv and tools/readable/readable_symbols.tsv:
// each name, its address in AI, its role and a port's source.
#pragma once
inline constexpr LibrarySymbol kLibrarySymbols[] = {
${_zx_library_rows}};
" @ONLY)
target_include_directories(zx_test_lib PRIVATE "${_zx_library_out}")
# The game sources here compiled as game.cmake compiles the readable game (zx_game_readable): no fused
# multiply-adds around the EE's float helpers, wrapping 32-bit arithmetic. The library ports are judged as the game
# builds them.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(zx_test_lib PRIVATE /clang:-ffp-contract=off /clang:-fwrapv)
elseif(MSVC)
    target_compile_options(zx_test_lib PRIVATE /fp:precise)
else()
    target_compile_options(zx_test_lib PRIVATE -ffp-contract=off -fwrapv)
endif()
if(ZX_RUNNER_GENERATED_DIR AND EXISTS "${ZX_RUNNER_GENERATED_DIR}/register_functions.cpp" AND COMMAND _zx_traced_sources)
    # (_zx_traced_sources reads these from game.cmake's scope)
    set(ZX_SYMBOLS_TABLE "${CMAKE_SOURCE_DIR}/tools/readable/readable_symbols.tsv")
    set(_zx_game_number_ "0")
    set(ZX_TRACED_BLOCK_LIST "")
    set(ZX_TRACED_BLOCK_FILES "")
    set(ZX_TRACED_GENERATED_FILES "")
    _zx_traced_sources(_zx_library_traced "" "${ZX_RUNNER_GENERATED_DIR}" ${_zx_library_traced_addresses})
    file(CONFIGURE OUTPUT "${_zx_library_out}/traced_blocks.list" CONTENT "${ZX_TRACED_BLOCK_LIST}" @ONLY)
    add_custom_command(
        OUTPUT "${_zx_library_out}/traced_blocks.stamp"
        BYPRODUCTS ${ZX_TRACED_BLOCK_FILES} "${_zx_library_out}/stack_args_unused.cpp"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/readable/fncheck_traced_gen.py"
                --list "${_zx_library_out}/traced_blocks.list" --stack-args "${_zx_library_out}/stack_args_unused.cpp"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_zx_library_out}/traced_blocks.stamp"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/readable/fncheck_traced_gen.py" "${_zx_library_out}/traced_blocks.list"
                ${ZX_TRACED_GENERATED_FILES}
        COMMENT "library ports' tests: block markers in the recompiled originals"
        VERBATIM)
    add_custom_target(zx_library_traced_blocks DEPENDS "${_zx_library_out}/traced_blocks.stamp")
    # (with the frame table game.cmake makes from the same generated code: readable functions enter their frames
    # by it, ee::enterFrame, as in the game)
    set(_zx_library_frames "${CMAKE_BINARY_DIR}/runtime/fncheck_build/readable_frames.cpp")
    set_source_files_properties("${_zx_library_frames}" PROPERTIES GENERATED TRUE)
    add_library(zx_library_originals OBJECT ${_zx_library_traced} "${_zx_library_frames}")
    add_dependencies(zx_library_originals zx_library_traced_blocks zx_game_traced_blocks)
    target_link_libraries(zx_library_originals PUBLIC zx_runtime)
    target_include_directories(zx_library_originals PRIVATE "${ZX_RUNNER_GENERATED_DIR}" ${ZX_READABLE_ROOTS})
    target_sources(zx-tests PRIVATE $<TARGET_OBJECTS:zx_library_originals>)
    list(LENGTH _zx_library_traced _zx_library_count)
    target_compile_definitions(zx_test_lib PRIVATE ZX_LIBRARY_ORIGINALS=${_zx_library_count})
    message(STATUS "zx-tests: ${_zx_library_count} recompiled originals for the library ports' tests (AI's ELF: ${_zx_library_elf})")
endif()
if(_zx_library_elf)
    target_compile_definitions(zx_test_lib PRIVATE "ZX_TEST_AI_ELF=\"${_zx_library_elf}\"")
endif()

# VU/GS tests intentionally instantiate large interpreter state.
# Windows executables default to a much smaller stack than Linux, and
# sanitizer/debug instrumentation can push these test frames over 1 MiB.
if(MSVC)
    target_link_options(zx-tests PRIVATE "/STACK:8388608")
elseif(MINGW)
    target_link_options(zx-tests PRIVATE "--stack,8388608")
endif()

if(COMMAND zx_stage_ffmpeg_runtime_dlls)
    zx_stage_ffmpeg_runtime_dlls(zx-tests)
endif()

include("${CMAKE_SOURCE_DIR}/ps2/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(zx_test_lib)
    EnableFastReleaseMode(zx-tests)
endif()
