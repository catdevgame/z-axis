# Game modules: each game's recompiled code (a folder of ps2xRecomp output) built as a module of its own, against
# the module's view of the runtime with no library header (include/game_module/, cmake/game_module_flags.txt: the
# app builds the same from the player's disc). PS2X_GAME_MODULES:
#   static  the modules' objects linked into the runner (the workshop's default: one program for the gates, the
#           checkers and the remote hosts);
#   shared  one shared library each, in the runner's game-code folder, loaded at boot (runtime/game_module.h).
# Either way the module's code is compiled the same, without LTO (the boundary is the same in both forms).
set(PS2X_GAME_MODULES "static" CACHE STRING "Game modules linked into the runner (static) or shared libraries (shared)")
set_property(CACHE PS2X_GAME_MODULES PROPERTY STRINGS static shared)
if(NOT PS2X_GAME_MODULES MATCHES "^(static|shared)$")
    message(FATAL_ERROR "PS2X_GAME_MODULES: static or shared, not ${PS2X_GAME_MODULES}")
endif()
if(PS2X_GAME_MODULES STREQUAL "shared" AND (ZAXIS_LANE_WATCH OR PS2X_ENABLE_FUNCTION_LOG_TRACKER))
    message(FATAL_ERROR "PS2X_GAME_MODULES=shared: the lane watch and the function log need the runtime linked to the "
                        "generated code (PS2X_GAME_MODULES=static)")
endif()

set(PS2X_GAME_MODULE_FLAGS_FILE "${CMAKE_CURRENT_LIST_DIR}/game_module_flags.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_GAME_MODULE_FLAGS_FILE}")
file(STRINGS "${PS2X_GAME_MODULE_FLAGS_FILE}" _ps2x_module_flag_lines REGEX "^[a-z0-9_]+ ")
if(PS2X_IS_AARCH64_TARGET OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(_ps2x_module_arch arm64)
else()
    set(_ps2x_module_arch x86_64)
endif()
set(PS2X_GAME_MODULE_COMPILE_FLAGS "")
foreach(_line IN LISTS _ps2x_module_flag_lines)
    string(REGEX MATCH "^([a-z0-9_]+) (.*)$" _ "${_line}")
    if(CMAKE_MATCH_1 STREQUAL "all" OR CMAKE_MATCH_1 STREQUAL _ps2x_module_arch)
        if(MSVC)
            # (clang-cl: clang's own options after /clang:)
            list(APPEND PS2X_GAME_MODULE_COMPILE_FLAGS "/clang:${CMAKE_MATCH_2}")
        else()
            list(APPEND PS2X_GAME_MODULE_COMPILE_FLAGS "${CMAKE_MATCH_2}")
        endif()
    endif()
endforeach()
if(MSVC)
    # (clang-cl's INCLUDE folders, the Windows SDK's and the C runtime's, left out as -nostdlibinc leaves the rest)
    list(APPEND PS2X_GAME_MODULE_COMPILE_FLAGS /X /FS)
endif()

# The module's include path: its generated folder, the module's headers (include/: ps2_generated.h, game_module/,
# runtime/guest_cpu.h and the address map), the declarations for the intrinsics headers, sse2neon.
set(PS2X_GAME_MODULE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(sse2neon_SOURCE_DIR)
    list(APPEND PS2X_GAME_MODULE_INCLUDE_DIRS "${sse2neon_SOURCE_DIR}")
endif()
set(PS2X_GAME_MODULE_SYSTEM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/game_module/libc")
set(PS2X_GAME_MODULE_ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/game_module/module_entry.cpp")
set(PS2X_GAME_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/game-code")

# (the settings above, for the functions below when another directory calls them: the tests' own modules)
foreach(_setting ENTRY_SOURCE COMPILE_FLAGS INCLUDE_DIRS SYSTEM_INCLUDE_DIR DIR)
    set_property(GLOBAL PROPERTY PS2X_GAME_MODULE_${_setting} "${PS2X_GAME_MODULE_${_setting}}")
endforeach()
macro(_ps2x_game_module_settings)
    foreach(_setting ENTRY_SOURCE COMPILE_FLAGS INCLUDE_DIRS SYSTEM_INCLUDE_DIR DIR)
        get_property(PS2X_GAME_MODULE_${_setting} GLOBAL PROPERTY PS2X_GAME_MODULE_${_setting})
    endforeach()
endmacro()

set(PS2X_GAME_MODULE_TARGETS "")
set(PS2X_GAME_MODULE_IDS "")

# ps2x_add_game_module(<id> <generated folder>): the module of one game's code (id: ai, dm2, bmx).
function(ps2x_add_game_module _id _dir)
    _ps2x_game_module_settings()
    if(NOT EXISTS "${_dir}/register_functions.cpp")
        message(FATAL_ERROR "game module ${_id}: ${_dir} has no register_functions.cpp")
    endif()
    file(STRINGS "${_dir}/register_functions.cpp" _preamble LIMIT_COUNT 1)
    if(NOT _preamble STREQUAL "#include \"ps2_generated.h\"")
        message(FATAL_ERROR "game module ${_id}: ${_dir} was made by a recompiler from before game modules (its "
                            "files include the runtime's headers, not ps2_generated.h): use a folder made by this "
                            "tree's ps2xRecomp")
    endif()
    file(GLOB _sources CONFIGURE_DEPENDS "${_dir}/*.cpp")
    set(_target ps2_game_module_${_id})
    if(PS2X_GAME_MODULES STREQUAL "shared")
        add_library(${_target} SHARED ${_sources} "${PS2X_GAME_MODULE_ENTRY_SOURCE}")
        target_compile_definitions(${_target} PRIVATE PS2X_GAME_MODULE_SHARED=1)
        set_target_properties(${_target} PROPERTIES
            OUTPUT_NAME "zx-${_id}"
            PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY "${PS2X_GAME_MODULE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${PS2X_GAME_MODULE_DIR}"
            LINKER_LANGUAGE CXX)
        # Nothing from any library: an undefined symbol fails the link here, not the load.
        if(MSVC)
            target_link_options(${_target} PRIVATE /NOENTRY /NODEFAULTLIB /DLL)
            set_property(TARGET ${_target} PROPERTY LINK_LIBRARIES "")
        elseif(APPLE)
            # (Apple's ld links no dylib without libSystem; the module takes nothing from it, its own memcpy and
            # the rest are the module's, and an undefined symbol still fails. The app's ld64.lld needs none.)
            target_link_options(${_target} PRIVATE -nostdlib -lSystem -Wl,-undefined,error)
        else()
            target_link_options(${_target} PRIVATE -nostdlib -Wl,-z,defs)
        endif()
    else()
        add_library(${_target} OBJECT ${_sources} "${PS2X_GAME_MODULE_ENTRY_SOURCE}")
    endif()
    target_compile_definitions(${_target} PRIVATE PS2X_GAME_MODULE_ID=${_id})
    if(PS2X_STRICT_RETURN_DIAGNOSTICS)
        target_compile_definitions(${_target} PRIVATE PS2X_STRICT_RETURN_DIAGNOSTICS=1)
    endif()
    if(ZAXIS_LANE_WATCH)
        target_compile_definitions(${_target} PRIVATE ZAXIS_LANE_WATCH=1)
    endif()
    target_compile_options(${_target} PRIVATE ${PS2X_GAME_MODULE_COMPILE_FLAGS})
    target_include_directories(${_target} BEFORE PRIVATE "${_dir}" ${PS2X_GAME_MODULE_INCLUDE_DIRS})
    target_include_directories(${_target} SYSTEM PRIVATE "${PS2X_GAME_MODULE_SYSTEM_INCLUDE_DIR}")
    set_target_properties(${_target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    set_source_files_properties("${PS2X_GAME_MODULE_ENTRY_SOURCE}" TARGET_DIRECTORY ${_target}
                                PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    # The function table's filler: only stores, run once at load; unoptimized (optimized, a compiler folds the
    # constructor into the table and takes over 8 GB for AI's). The app's build compiles it the same
    # (collection/game_code.cpp).
    if(MSVC)
        set(_ps2x_fill_opt "/Od")
    else()
        set(_ps2x_fill_opt "-O0")
    endif()
    set_source_files_properties("${_dir}/register_functions.cpp" TARGET_DIRECTORY ${_target}
                                PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON COMPILE_OPTIONS "${_ps2x_fill_opt}")
    if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
        set_target_properties(${_target} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}")
    endif()
    list(LENGTH _sources _count)
    message(STATUS "game module ${_id} (${PS2X_GAME_MODULES}): ${_count} generated sources from ${_dir}")
    set(PS2X_GAME_MODULE_TARGETS ${PS2X_GAME_MODULE_TARGETS} ${_target} PARENT_SCOPE)
    set(PS2X_GAME_MODULE_IDS ${PS2X_GAME_MODULE_IDS} ${_id} PARENT_SCOPE)
endfunction()

# ps2x_link_game_modules(<runner>): the runner gets the static modules' objects and their entries
# (game_modules_linked.cpp), or depends on the shared ones and looks for them in its game-code folder.
function(ps2x_link_game_modules _runner)
    _ps2x_game_module_settings()
    if(PS2X_GAME_MODULES STREQUAL "shared")
        # (the loader is the runtime's: lib/game_module.cpp; the build's own folder only when it builds modules: a
        # build with none, the app's, takes only the code it makes for the player's disc, and names no build folder)
        if(PS2X_GAME_MODULE_TARGETS)
            add_dependencies(${_runner} ${PS2X_GAME_MODULE_TARGETS})
            target_compile_definitions(ps2_runtime PRIVATE PS2X_GAME_CODE_DIR="${PS2X_GAME_MODULE_DIR}")
        endif()
        return()
    endif()
    set(_text "// Written by cmake/game_module.cmake: the game modules linked into this program.\n")
    string(APPEND _text "#include \"runtime/game_module.h\"\n\n")
    foreach(_id IN LISTS PS2X_GAME_MODULE_IDS)
        string(APPEND _text "extern \"C\" const ZxGameModule *zx_game_module_entry_${_id}(const ZxRuntimeApi *api);\n")
    endforeach()
    string(APPEND _text "\nnamespace\n{\n")
    foreach(_id IN LISTS PS2X_GAME_MODULE_IDS)
        string(APPEND _text "    const ps2x::game_module::Linked g_${_id}{\"${_id}\", &zx_game_module_entry_${_id}};\n")
    endforeach()
    string(APPEND _text "}\n")
    set(_linked "${CMAKE_CURRENT_BINARY_DIR}/game_modules_linked.cpp")
    file(CONFIGURE OUTPUT "${_linked}" CONTENT "${_text}")
    target_sources(${_runner} PRIVATE "${_linked}")
    set_source_files_properties("${_linked}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    foreach(_target IN LISTS PS2X_GAME_MODULE_TARGETS)
        target_sources(${_runner} PRIVATE $<TARGET_OBJECTS:${_target}>)
    endforeach()
endfunction()
