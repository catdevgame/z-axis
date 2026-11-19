cmake_minimum_required(VERSION 3.21)

project(ps2xTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(gs_cache)
endif()

# The guest function table the runtime tests register (and tooling/recompiler/tests too).
add_library(ps2_test_function_table OBJECT
    src/test_function_table.cpp
)

target_include_directories(ps2_test_function_table PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/iop/include
)

add_library(ps2_test_lib STATIC
    src/pad_input_tests.cpp
    src/host_input_tests.cpp
    src/ps2_runtime_io_tests.cpp
    src/ps2_runtime_kernel_tests.cpp
    src/ps2_runtime_interrupt_tests.cpp
    src/ps2_runtime_lifecycle_tests.cpp
    src/ps2_memory_tests.cpp
    src/ps2_async_graphics_tests.cpp
    src/ps2_vu1_tests.cpp
    src/ps2_vu_tests.cpp
    src/ps2_gs_tests.cpp
    src/ps2_iop_tests.cpp
    src/ps2_sif_rpc_tests.cpp
    src/ps2_sif_dma_tests.cpp
    src/ps2_logic_check_tests.cpp
    src/ps2_debug_control_tests.cpp
    src/ps2_disc_image_tests.cpp
    src/ps2_disc_formats_tests.cpp
    src/ps2_app_support_tests.cpp
    src/ps2_collection_tests.cpp
    src/ps2_front_end_tests.cpp
    src/ps2_game_setup_tests.cpp
    src/ps2_settings_tests.cpp
    src/zge_settings_page_tests.cpp
    src/ps2_host_pacing_tests.cpp
    # The games' volume rows (a change read as made until the game's frame has passed it).
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/zge_volumes.cpp
    src/ps2_boot_config_tests.cpp
    src/play_recording_tests.cpp
    src/ps2_session_tests.cpp
    src/ps2_game_module_tests.cpp
    src/ps2_gs_quality_tests.cpp
    src/ps2_gs_interpolate_tests.cpp
    src/ps2_gpu_texture_check_tests.cpp
    src/ps2_glyph_art_tests.cpp
    src/ps2_panel_tests.cpp
    # (Aggressive Inline's capture of its RAM for the panel: the tests feed it a made-up RAM)
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/aggressive_inline_panel.cpp
    src/ps2_photo_tests.cpp
    src/ps2_save_store_tests.cpp
    src/ps2_game_files_tests.cpp
    src/ps2_xbox_disc_tests.cpp
    src/ps2_extras_tests.cpp
    src/ps2_zaxis_xbox_tests.cpp
    src/ps2_zaxis_zoo_tests.cpp
    src/ps2_zaxis_estate_tests.cpp
    src/ps2_zaxis_mesh_tests.cpp
    src/ps2_zaxis_text_tests.cpp
    src/ps2_texture_replace_tests.cpp
    src/ps2_texture_pack_tests.cpp
    src/ps2_gs_texture_audit_tests.cpp
    src/ps2_pss_movie_tests.cpp
    src/ps2_spu_voice_tests.cpp
    # The library ports against AI's recompiled originals (library_ports.tsv; the originals below).
    src/ps2_library_tests.cpp
    src/ps2_readable_tests.cpp
    # A traced original of two entry_ chunks, as game.cmake builds one (ee/fncheck_traced.h).
    src/ps2_fncheck_chunks_traced.cpp
    src/ps2_readable_script_tests.cpp
    src/ps2_career_scripts_tests.cpp
    # The career scripts setting's script edit (and its install, unused here).
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/aggressive_inline_careerscripts.cpp
    # DM2's unlock cheats and Create Player's riders (the pure part).
    src/ps2_unlock_tests.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/dave_mirra_cheats.cpp
    # The movies' form and the sound driver that goes with it.
    src/zge_movie_form_tests.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/zge_movie_form.cpp
    # Exit to Collection's menu edits in DM2 and BMX XXX (and their installs, unused here).
    src/ps2_exit_menu_tests.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/zge_exit.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/dave_mirra_exit.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/dave_mirra_restart.cpp
    # (its pause menu's Photo Mode line starts photo mode, its Settings line shows the Settings page)
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/dave_mirra_photo.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/dave_mirra_settings.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/zge_settings.cpp
    # (the app's Settings... request the pause hooks consume)
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/zge_settings_open.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/zge_photo.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/zge_sites.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/bmx_xxx_exit.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/bmx_xxx_restart.cpp
    # (its pause menu's Photo Mode line starts photo mode, its Settings line shows the Settings page, as DM2's)
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/bmx_xxx_photo.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides/bmx_xxx_settings.cpp
    # The readable game's framework (its game code is tested by ps2xFnCheck).
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/abi.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/held.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/fiber_posix.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/fiber_windows.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/fncheck.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/fnrec_file.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/profile.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/tailcall_stats.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/cycletrace.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/readable.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/guest_calls.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/diffcall.cpp
    # The level scripts' machine, run on a handler written for the test.
    ${CMAKE_SOURCE_DIR}/runtime/src/game/aggressive/script/vm.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/aggressive/script/objects.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/aggressive/script/natives.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/text/strings.cpp
    # The string tables call the C string functions by name (and those libc).
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/text/cstring.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libc.cpp
    # The string tables' memory comes from the heap (heapAllocate by name), the heap's regions from pools, which keep lists and pointer arrays (utilities.cpp, with the matrix and angle math its other functions call).
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/memory/regions.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/memory/pool.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/utilities.cpp
    # (its box sweep tells the sweep's sweepPassLow by name)
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/collision/sweep.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/container/array.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/math/matrix.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/math/angle.cpp
    # The matrix math copies rotations with the platform's matrixCopyRotation by name (and the platform's clock
    # is the cycle counter's).
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/platform/ps2_platform.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/debug/cycle_counter.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libgcc.cpp
    # The other library ports, for ps2_library_tests.cpp.
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libm.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/mprec.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libc_strtod.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libc_printf.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libc_scanf.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libc_stdio.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/sce/libc_malloc.cpp
    # GEO data read on the host (the collision chunks' reader).
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/native/geo_file.cpp
    # The level setup's load-time words made on the host (and the closest
    # point it uses).
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/native/collision_setup.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/aggressive/level/collision_setup_host.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/zge/collision/closest.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/game/ee/suspend.cpp
)

# The game sources above that are plain sources (plain_sources.txt) are compiled from a copy that differs from the
# source only where the AI build's copy (readable_entries.py, game.cmake) makes a version's own body (_OWN)
# READABLE_OWN_INLINE: inline here as there, so this build emits nothing of it either, and an own body that calls its
# game's _ONLY functions (DM2's scaleMatrixDiagonal: copyRotationAndTranslation, which no build but DM2's instance
# has) leaves no unresolved call. Their plain calls stay direct, as written: this binary has no function table.
# But the libraries' (sce/): those are compiled from the build's own copy (each registered body's entry, as
# game.cmake's), so that a port's calls go where the game's do (game::call: a function on directly, in its own
# frame, any other through the table), which the library ports' tests (ps2_library_tests.cpp) need: their ports'
# callees are the originals in the table.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_ps2x_test_game_dir "${CMAKE_SOURCE_DIR}/runtime/src/game")
set(_ps2x_test_entries_tool "${CMAKE_SOURCE_DIR}/runtime/tools/readable_entries.py")
file(STRINGS "${_ps2x_test_game_dir}/plain_sources.txt" _ps2x_test_plain_prefixes REGEX "^[a-z]")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ps2x_test_game_dir}/plain_sources.txt")
get_target_property(_ps2x_test_sources ps2_test_lib SOURCES)
set(_ps2x_test_compiled "")
foreach(_source IN LISTS _ps2x_test_sources)
    set(_plain FALSE)
    string(FIND "${_source}" "${_ps2x_test_game_dir}/" _at)
    if(_at EQUAL 0)
        file(RELATIVE_PATH _relative "${_ps2x_test_game_dir}" "${_source}")
        foreach(_prefix IN LISTS _ps2x_test_plain_prefixes)
            string(LENGTH "${_prefix}" _length)
            string(SUBSTRING "${_relative}" 0 ${_length} _head)
            if(_head STREQUAL _prefix)
                set(_plain TRUE)
            endif()
        endforeach()
    endif()
    if(NOT _plain)
        list(APPEND _ps2x_test_compiled "${_source}")
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
        COMMAND "${Python3_EXECUTABLE}" -B "${_ps2x_test_entries_tool}" ${_mode} --line-root "${CMAKE_BINARY_DIR}" "${_source}" "${_copy}"
        DEPENDS "${_source}" "${_ps2x_test_entries_tool}" "${CMAKE_SOURCE_DIR}/runtime/tools/readable_entry_reads.tsv"
        COMMENT "readable: the unit tests' copy of ${_relative}"
        VERBATIM)
    get_filename_component(_dir "${_source}" DIRECTORY)
    # (its own folder's headers by -iquote, as game.cmake's copies: never a system header's name on -I)
    if(MSVC)
        set_source_files_properties("${_copy}" PROPERTIES GENERATED TRUE INCLUDE_DIRECTORIES "${_dir}")
    else()
        set_source_files_properties("${_copy}" PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-iquote;${_dir}")
    endif()
    list(APPEND _ps2x_test_compiled "${_copy}")
endforeach()
set_property(TARGET ps2_test_lib PROPERTY SOURCES ${_ps2x_test_compiled})

# (the generated code's headers fncheck_traced.h includes: empty, this binary has no generated code)
set_source_files_properties(src/ps2_fncheck_chunks_traced.cpp TARGET_DIRECTORY ps2_test_lib
                            PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/traced_stub")

target_include_directories(ps2_test_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/ps2xRuntime/symbols/ai
    ${CMAKE_BINARY_DIR}/ps2xRuntime/symbols/overrides
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/runtime/src/lib
    ${CMAKE_SOURCE_DIR}/runtime/src/game
    ${CMAKE_SOURCE_DIR}/runtime/src/game_overrides
)

target_link_libraries(ps2_test_lib PRIVATE
    ps2_runtime
    ps2_iop
    ps2x_miniz # (the tests write zips: src/test_zip_builder.h; and CSO images' deflate blocks)
    ps2x::lz4 # (ZSO and CSO v2 images' LZ4 blocks: src/ps2_disc_formats_tests.cpp)
)
# (the override sources it builds name their game's symbols: runtime/tools/override_symbols.py)
add_dependencies(ps2_test_lib ps2_override_symbols)

# The GPU renderer's tests (skipped without an SDL_GPU device).
if(TARGET ps2_gs_gpu)
    target_link_libraries(ps2_test_lib PRIVATE ps2_gs_gpu)
    target_compile_definitions(ps2_test_lib PRIVATE PS2X_HAS_GS_GPU=1)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(ps2_test_lib PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(ps2x_tests
    src/main.cpp
    $<TARGET_OBJECTS:ps2_test_function_table>
)

# The readable tests run ps2xFnCheck's own test of records that stop at a call (--self-test-stops: the right version
# passes, the broken ones fail) where this build makes the checker.
if(TARGET ps2xFnCheck)
    target_compile_definitions(ps2_test_lib PRIVATE "PS2X_FNCHECK_EXE=\"$<TARGET_FILE:ps2xFnCheck>\"")
    add_dependencies(ps2x_tests ps2xFnCheck)
endif()

option(PRINT_GENERATED_CODE "Print generated code in tests" OFF)
if(PRINT_GENERATED_CODE)

    target_compile_definitions(ps2x_tests PRIVATE PRINT_GENERATED_CODE)
    target_compile_definitions(ps2_test_lib PRIVATE PRINT_GENERATED_CODE)
endif()

target_include_directories(ps2x_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/runtime/include
)

# The whole test library: a static archive's member that nothing references is left out by the linker (macOS's ld
# dropped libc_malloc.cpp's object, and with it its registrations, so ps2_library_tests' suite found no port for
# __malloc_lock and did not run on the Mac; cov4). Every member's static registrations must be in the binary.
target_link_libraries(ps2x_tests PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,ps2_test_lib>"
    ps2_panel
    ps2_runtime
)

# Two game modules of the tests' own (game_module/), built and linked as the runner's are with
# PS2X_GAME_MODULES=static: ps2_game_module_tests.cpp checks each registers its own code in one program.
if(COMMAND ps2x_add_game_module AND PS2X_GAME_MODULES STREQUAL "static")
    set(PS2X_GAME_MODULE_TARGETS "")
    set(PS2X_GAME_MODULE_IDS "")
    ps2x_add_game_module(zxa "${CMAKE_CURRENT_SOURCE_DIR}/game_module/zxa")
    ps2x_add_game_module(zxb "${CMAKE_CURRENT_SOURCE_DIR}/game_module/zxb")
    ps2x_link_game_modules(ps2x_tests)
    target_compile_definitions(ps2x_tests PRIVATE PS2X_TEST_LINKED_MODULES=1)
endif()

# The library ports' recompiled originals (ps2_library_tests.cpp): in a build with AI's generated code, every
# function of src/library_ports.tsv (a port, or what one calls) built from its generated file with fncheck's
# tracing and block markers, as game.cmake builds the traced originals (its _ps2x_traced_sources, here into this
# folder, and tools/fncheck_traced_gen.py for the markers), and registered as AI's traced originals. Their data
# is AI's ELF (PS2X_TEST_AI_ELF, or the environment's at run time: a build-time input, never copied into the
# tree); without generated code or the ELF the tests say they did not run.
set(PS2X_TEST_AI_ELF "" CACHE FILEPATH "AI's ELF (SLUS_203.27) for the library ports' tests (default: disc-root/SLUS_203.27 beside PS2X_RUNNER_GENERATED_DIR)")
set(_ps2x_library_elf "${PS2X_TEST_AI_ELF}")
if(NOT _ps2x_library_elf AND PS2X_RUNNER_GENERATED_DIR AND EXISTS "${PS2X_RUNNER_GENERATED_DIR}/../disc-root/SLUS_203.27")
    get_filename_component(_ps2x_library_elf "${PS2X_RUNNER_GENERATED_DIR}/../disc-root/SLUS_203.27" ABSOLUTE)
endif()
set(_ps2x_library_out "${CMAKE_CURRENT_BINARY_DIR}/library_ports")
set(_ps2x_library_rows "")
set(_ps2x_library_names "")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/library_ports.tsv" _ps2x_library_lines REGEX "^[A-Za-z_]")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/library_ports.tsv")
foreach(_line IN LISTS _ps2x_library_lines)
    if(_line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)\t(port|callee|system|data)(\t([^\t]+))?$")
        set(_ps2x_library_role_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        set(_ps2x_library_file_${CMAKE_MATCH_1} "${CMAKE_MATCH_4}")
        list(APPEND _ps2x_library_names "${CMAKE_MATCH_1}")
    endif()
endforeach()
# Each name's address in AI, from the symbol table (AI's rows: no game column).
file(STRINGS "${CMAKE_SOURCE_DIR}/runtime/tools/readable_symbols.tsv" _ps2x_symbol_rows REGEX "^(function|data)\t[A-Za-z_][A-Za-z0-9_]*\t0x[0-9A-Fa-f]+\t\t$")
set(_ps2x_library_traced_addresses "")
foreach(_row IN LISTS _ps2x_symbol_rows)
    if(_row MATCHES "^(function|data)\t([A-Za-z_][A-Za-z0-9_]*)\t0x([0-9A-Fa-f]+)\t")
        set(_name "${CMAKE_MATCH_2}")
        if(DEFINED _ps2x_library_role_${_name} AND NOT DEFINED _ps2x_library_address_${_name})
            string(TOLOWER "${CMAKE_MATCH_3}" _hex)
            math(EXPR _addr "0x${_hex}" OUTPUT_FORMAT HEXADECIMAL)
            string(TOLOWER "${_addr}" _addr)
            set(_ps2x_library_address_${_name} "${_addr}")
        endif()
    endif()
endforeach()
foreach(_name IN LISTS _ps2x_library_names)
    if(NOT DEFINED _ps2x_library_address_${_name})
        message(FATAL_ERROR "tests/src/library_ports.tsv: ${_name} has no AI row in runtime/tools/readable_symbols.tsv")
    endif()
    set(_role "${_ps2x_library_role_${_name}}")
    string(APPEND _ps2x_library_rows "    {\"${_name}\", ${_ps2x_library_address_${_name}}u, LibraryRole::${_role}, \"${_ps2x_library_file_${_name}}\"},\n")
    if(_role STREQUAL "port" OR _role STREQUAL "callee")
        list(APPEND _ps2x_library_traced_addresses "${_ps2x_library_address_${_name}}")
    endif()
endforeach()
file(CONFIGURE OUTPUT "${_ps2x_library_out}/library_ports_table.h" CONTENT
"// Generated by tests/CMakeLists.txt from src/library_ports.tsv and runtime/tools/readable_symbols.tsv:
// each name, its address in AI, its role and a port's source.
#pragma once
inline constexpr LibrarySymbol kLibrarySymbols[] = {
${_ps2x_library_rows}};
" @ONLY)
target_include_directories(ps2_test_lib PRIVATE "${_ps2x_library_out}")
# The game sources here compiled as game.cmake compiles the readable game (ps2_game_readable): no fused
# multiply-adds around the EE's float helpers, wrapping 32-bit arithmetic. The library ports are judged as the game
# builds them.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(ps2_test_lib PRIVATE /clang:-ffp-contract=off /clang:-fwrapv)
elseif(MSVC)
    target_compile_options(ps2_test_lib PRIVATE /fp:precise)
else()
    target_compile_options(ps2_test_lib PRIVATE -ffp-contract=off -fwrapv)
endif()
if(PS2X_RUNNER_GENERATED_DIR AND EXISTS "${PS2X_RUNNER_GENERATED_DIR}/register_functions.cpp" AND COMMAND _ps2x_traced_sources)
    # (_ps2x_traced_sources reads these from game.cmake's scope)
    set(PS2X_SYMBOLS_TABLE "${CMAKE_SOURCE_DIR}/runtime/tools/readable_symbols.tsv")
    set(_ps2x_game_number_ "0")
    set(PS2X_TRACED_BLOCK_LIST "")
    set(PS2X_TRACED_BLOCK_FILES "")
    set(PS2X_TRACED_GENERATED_FILES "")
    _ps2x_traced_sources(_ps2x_library_traced "" "${PS2X_RUNNER_GENERATED_DIR}" ${_ps2x_library_traced_addresses})
    file(CONFIGURE OUTPUT "${_ps2x_library_out}/traced_blocks.list" CONTENT "${PS2X_TRACED_BLOCK_LIST}" @ONLY)
    add_custom_command(
        OUTPUT "${_ps2x_library_out}/traced_blocks.stamp"
        BYPRODUCTS ${PS2X_TRACED_BLOCK_FILES} "${_ps2x_library_out}/stack_args_unused.cpp"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/runtime/tools/fncheck_traced_gen.py"
                --list "${_ps2x_library_out}/traced_blocks.list" --stack-args "${_ps2x_library_out}/stack_args_unused.cpp"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_ps2x_library_out}/traced_blocks.stamp"
        DEPENDS "${CMAKE_SOURCE_DIR}/runtime/tools/fncheck_traced_gen.py" "${_ps2x_library_out}/traced_blocks.list"
                ${PS2X_TRACED_GENERATED_FILES}
        COMMENT "library ports' tests: block markers in the recompiled originals"
        VERBATIM)
    add_custom_target(ps2_library_traced_blocks DEPENDS "${_ps2x_library_out}/traced_blocks.stamp")
    # (with the frame table game.cmake makes from the same generated code: readable functions enter their frames
    # by it, ee::enterFrame, as in the game)
    set(_ps2x_library_frames "${CMAKE_BINARY_DIR}/ps2xRuntime/fncheck_build/readable_frames.cpp")
    set_source_files_properties("${_ps2x_library_frames}" PROPERTIES GENERATED TRUE)
    add_library(ps2_library_originals OBJECT ${_ps2x_library_traced} "${_ps2x_library_frames}")
    add_dependencies(ps2_library_originals ps2_library_traced_blocks ps2_game_traced_blocks)
    target_link_libraries(ps2_library_originals PUBLIC ps2_runtime)
    target_include_directories(ps2_library_originals PRIVATE "${PS2X_RUNNER_GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/runtime/src/game")
    target_sources(ps2x_tests PRIVATE $<TARGET_OBJECTS:ps2_library_originals>)
    list(LENGTH _ps2x_library_traced _ps2x_library_count)
    target_compile_definitions(ps2_test_lib PRIVATE PS2X_LIBRARY_ORIGINALS=${_ps2x_library_count})
    message(STATUS "ps2x_tests: ${_ps2x_library_count} recompiled originals for the library ports' tests (AI's ELF: ${_ps2x_library_elf})")
endif()
if(_ps2x_library_elf)
    target_compile_definitions(ps2_test_lib PRIVATE "PS2X_TEST_AI_ELF=\"${_ps2x_library_elf}\"")
endif()

# VU/GS tests intentionally instantiate large interpreter state.
# Windows executables default to a much smaller stack than Linux, and
# sanitizer/debug instrumentation can push these test frames over 1 MiB.
if(MSVC)
    target_link_options(ps2x_tests PRIVATE "/STACK:8388608")
elseif(MINGW)
    target_link_options(ps2x_tests PRIVATE "--stack,8388608")
endif()

if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2x_tests)
endif()

include("${CMAKE_SOURCE_DIR}/runtime/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_test_lib)
    EnableFastReleaseMode(ps2x_tests)
endif()
