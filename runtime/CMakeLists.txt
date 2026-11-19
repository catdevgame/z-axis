cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" ON)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_ENABLE_FUNCTION_LOG_TRACKER "Trace every generated function entry/exit to ps2_log.txt (very slow; needs PS2X_ENABLE_AGRESSIVE_LOGS)" OFF)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the runtime debug UI (Mac and Windows)" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

# The build's flavour. ON is the dev build: the checkers' machinery (the recorder and the traced originals,
# diffcall, the cycle and tail-call profilers, the guest-call trace, the stack fills, ps2xFnCheck) is built in, and
# the hooks a checker sets are tested on every call. OFF is the app's flavour (make-app.sh, make-windows.ps1 and the
# Android build): none of it is compiled or linked, and the hooks are constant nulls (ee/abi.h), so a call pays
# nothing for them. What the game computes is the same in both: the frame table, the held table, the register saves
# and EE time stay (they are behaviour under the PS2 clock). The checks run the dev flavour; the app's is proven
# against it by the inter-binary A/B (tooling/checks/flavour_ab.py). Android ships the app alone.
if(PS2X_IS_ANDROID)
    set(_zaxis_dev_checks_default OFF)
else()
    set(_zaxis_dev_checks_default ON)
endif()
option(ZAXIS_DEV_CHECKS "Build the checkers' machinery into the runner (OFF: the app's flavour)" ${_zaxis_dev_checks_default})
# One header says which, so every file that includes the framework agrees and the files that do not (the
# recompiled code) compile the same in both flavours (a cache hit in the other's build).
if(ZAXIS_DEV_CHECKS)
    set(_zaxis_dev_checks 1)
else()
    set(_zaxis_dev_checks 0)
endif()
# ZAXIS_GUEST_MAP_CHECK: every access of the readable game (ee::at<T>, game/ee/ee.h) also resolved as before the
# guest map (runtime/guest_map.h), and an address the two resolve differently, or one outside RAM's own view, said
# by the code that made it (ee/guest_map_check.cpp): the evidence of where the special addresses occur. A tool build.
option(ZAXIS_GUEST_MAP_CHECK "Check every readable access against the resolution before the guest map (a tool build)" OFF)
if(ZAXIS_GUEST_MAP_CHECK)
    set(_zaxis_guest_map_check 1)
else()
    set(_zaxis_guest_map_check 0)
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/flavour/build_flavour.h"
    CONTENT "#pragma once\n// Made by runtime/CMakeLists.txt: ZAXIS_DEV_CHECKS 1 the dev build, 0 the app's flavour; ZAXIS_GUEST_MAP_CHECK 1 a\n// build that checks the guest map's every readable access.\n#define ZAXIS_DEV_CHECKS @_zaxis_dev_checks@\n#define ZAXIS_GUEST_MAP_CHECK @_zaxis_guest_map_check@\n"
    @ONLY)
message(STATUS "flavour: ${_zaxis_dev_checks_default} by default, ZAXIS_DEV_CHECKS=${ZAXIS_DEV_CHECKS}")
# ZAXIS_LANE_WATCH: every guest load and store of the recompiled code and every readable access (ee::at<T>) also told
# to the lane watch (runtime/lane_watch.h, PS2X_LANE_WATCH: who reads or writes a watched word), for wlane.py's proofs
# that a leftover vector lane is read by nothing. A tool build: the macros cost a test per access.
option(ZAXIS_LANE_WATCH "Count every access of watched words by the code that made it (a tool build)" OFF)

set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

# Window, input, audio and SDL_GPU presentation.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2x_sdl3.cmake")
# The shipped font's rasteriser (collection/vector_font.cpp).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2x_stb.cmake")
# zstd: the compressed recordings (src/game/ee/fnrec_file.h, the checkers' machinery, the dev flavour's alone) and
# the GameCube discs' RVZ images the extras' setup reads (src/collection/extras, both flavours).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2x_zstd.cmake")

# xxHash 0.8.3 (BSD 2-Clause), header only: texture packs name textures by
# PCSX2's XXH3-64 hashes (runtime/ps2_texture_pack.h), PCSX2's own copy is
# this release. The tarball is checked by SHA-256.
FetchContent_Declare(
    xxhash
    URL "https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz"
    URL_HASH "SHA256=aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# (no CMakeLists.txt at its top: populated only, nothing added)
FetchContent_MakeAvailable(xxhash)
add_library(ps2x_xxhash INTERFACE)
target_include_directories(ps2x_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")

if(PS2X_ENABLE_DEBUG_UI)
    FetchContent_Declare(
        imgui
        # v1.92.7-docking: the tag's source archive, checked by SHA-256 (no git clone).
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.7-docking.tar.gz
        URL_HASH SHA256=2c58e28c957497eba0ed01c48a0bc5f118ec5f10a1c3721ba9436253a623bd72
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(imgui)

    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

    # Dear ImGui with its SDL3 platform and SDL_GPU renderer backends.
    add_library(imgui STATIC
        "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp"
    )
    target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}" "${PS2X_IMGUI_SOURCE_DIR}/backends")
    target_link_libraries(imgui PUBLIC ps2x::sdl3)
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

target_link_libraries(ps2_host_backend INTERFACE ps2x::sdl3)

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

# libavformat: custom soundtrack files other than WAV (runtime/ps2_audio_file.h).
set(PS2X_HAS_AVFORMAT OFF)
set(PS2X_FFMPEG_PREFIX "" CACHE PATH
    "Install prefix of a static minimal FFmpeg (tools/build-ffmpeg.sh); empty uses the system's via pkg-config (Windows: a prebuilt shared FFmpeg)")
if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32 AND PS2X_FFMPEG_PREFIX)
    # The static MSVC build (tools/build-ffmpeg.sh from MSYS2): its .pc files
    # hold MSYS paths, so the libraries are named directly.
    add_library(ffmpeg INTERFACE)
    foreach(_ps2x_ff_lib IN ITEMS avformat avcodec swscale avutil)
        if(NOT EXISTS "${PS2X_FFMPEG_PREFIX}/lib/${_ps2x_ff_lib}.lib")
            message(FATAL_ERROR "PS2X_FFMPEG_PREFIX=${PS2X_FFMPEG_PREFIX} has no lib/${_ps2x_ff_lib}.lib")
        endif()
        target_link_libraries(ffmpeg INTERFACE "${PS2X_FFMPEG_PREFIX}/lib/${_ps2x_ff_lib}.lib")
    endforeach()
    target_include_directories(ffmpeg INTERFACE "${PS2X_FFMPEG_PREFIX}/include")
    target_link_libraries(ffmpeg INTERFACE bcrypt ole32 user32)
    set(PS2X_HAS_AVFORMAT ON)
    message(STATUS "FFmpeg: static minimal build in ${PS2X_FFMPEG_PREFIX}")
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        URL_HASH SHA256=b985887dbca8c0a9a4c47f9d2bac7ec3ab994335b1b7fe7000bdb1dcd772ac68
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    set(PS2X_HAS_AVFORMAT ON)
    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    # MPEG.cpp only uses libavcodec (MPEG-2 video decoder and parser),
    # libswscale (YUV to RGBA) and libavutil.
    find_package(PkgConfig REQUIRED)
    if(PS2X_FFMPEG_PREFIX)
        if(NOT EXISTS "${PS2X_FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc")
            message(FATAL_ERROR "PS2X_FFMPEG_PREFIX=${PS2X_FFMPEG_PREFIX} has no lib/pkgconfig/libavcodec.pc")
        endif()
        # Search only the given prefix, so a system FFmpeg cannot be mixed in.
        set(_ps2x_saved_pc_libdir "$ENV{PKG_CONFIG_LIBDIR}")
        set(_ps2x_saved_pc_path "$ENV{PKG_CONFIG_PATH}")
        set(ENV{PKG_CONFIG_LIBDIR} "${PS2X_FFMPEG_PREFIX}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_PATH} "")
    endif()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavutil
        libswscale
    )
    # Optional: the minimal static build has it with the audio demuxers and
    # decoders a custom soundtrack needs (tools/build-ffmpeg.sh).
    pkg_check_modules(FFMPEG_FORMAT IMPORTED_TARGET GLOBAL libavformat)
    if(PS2X_FFMPEG_PREFIX)
        set(ENV{PKG_CONFIG_LIBDIR} "${_ps2x_saved_pc_libdir}")
        set(ENV{PKG_CONFIG_PATH} "${_ps2x_saved_pc_path}")
        message(STATUS "FFmpeg: static minimal build in ${PS2X_FFMPEG_PREFIX} (${FFMPEG_libavcodec_VERSION})")
    else()
        message(STATUS "FFmpeg: system (pkg-config, libavcodec ${FFMPEG_libavcodec_VERSION})")
    endif()

    # pkg-config reports "-framework X" as two words; CMake de-duplicates link
    # options and would drop the repeated "-framework", so keep each pair whole.
    get_target_property(_ps2x_ffmpeg_link_opts PkgConfig::FFMPEG INTERFACE_LINK_OPTIONS)
    if(_ps2x_ffmpeg_link_opts)
        set(_ps2x_ffmpeg_fixed_opts "")
        set(_ps2x_ffmpeg_pending_framework OFF)
        foreach(_ps2x_opt IN LISTS _ps2x_ffmpeg_link_opts)
            if(_ps2x_ffmpeg_pending_framework)
                list(APPEND _ps2x_ffmpeg_fixed_opts "SHELL:-framework ${_ps2x_opt}")
                set(_ps2x_ffmpeg_pending_framework OFF)
            elseif(_ps2x_opt STREQUAL "-framework")
                set(_ps2x_ffmpeg_pending_framework ON)
            else()
                list(APPEND _ps2x_ffmpeg_fixed_opts "${_ps2x_opt}")
            endif()
        endforeach()
        set_target_properties(PkgConfig::FFMPEG PROPERTIES INTERFACE_LINK_OPTIONS "${_ps2x_ffmpeg_fixed_opts}")
    endif()

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
    if(PS2X_IS_ANDROID AND PS2X_FFMPEG_PREFIX)
        # (the NDK toolchain's find_library does not look outside its sysroot)
        target_link_directories(ffmpeg INTERFACE "${PS2X_FFMPEG_PREFIX}/lib")
    endif()
    if(FFMPEG_FORMAT_FOUND)
        set(PS2X_HAS_AVFORMAT ON)
        target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG_FORMAT)
        message(STATUS "FFmpeg: libavformat ${FFMPEG_FORMAT_VERSION} (custom soundtrack in OGG/FLAC/MP3)")
    else()
        message(STATUS "FFmpeg: no libavformat (custom soundtrack in WAV only)")
    endif()
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

# The bottom screen (the collection's: include/collection/panel.h): plain C++,
# no platform or game dependency, so the tests and the Mac harness run the
# phone's code. A game's state reaches it as a snapshot (include/runtime/
# panel_snapshot.h, the runtime's contract between a game's capture and the
# panel).
add_library(ps2_panel STATIC
    src/collection/panel.cpp
    src/collection/panel_canvas.cpp
    src/collection/panel_file.cpp
    src/collection/panel_view.cpp
)
target_include_directories(ps2_panel PUBLIC include)
# The harness: the snapshot registry and each game's capture compiled in
# (Aggressive Inline's by default, --game dm2 / bmx the others').
add_executable(ps2xPanelShot EXCLUDE_FROM_ALL tools/panel_shot.cpp
    src/lib/panel_snapshot.cpp
    src/game_overrides/aggressive_inline_panel.cpp)
target_link_libraries(ps2xPanelShot PRIVATE ps2_panel)

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/ps2_spu2_audio.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_quality.cpp
    src/lib/gs/gs_interpolate.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/gs/gs_null_backend.cpp
    src/lib/gs/gs_cpu_fast.cpp
    src/lib/gs/gs_record.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_disc_patch.cpp
    src/lib/ps2_game_files.cpp
    src/lib/ps2_zip_archive.cpp
    src/lib/ps2_games.cpp
    src/lib/ps2_front_end.cpp
    src/lib/ps2_xbox_disc.cpp
    src/lib/ps2_zaxis_estate.cpp
    src/lib/ps2_zaxis_estate_icon.cpp
    src/lib/ps2_zaxis_xbox.cpp
    src/lib/ps2_zaxis_zoo.cpp
    src/lib/ps2_zaxis_ape.cpp
    src/lib/ps2_zaxis_mesh.cpp
    src/lib/ps2_zaxis_shadow.cpp
    src/lib/ps2_audio_mix.cpp
    src/lib/ps2_spu_voice.cpp
    src/lib/ps2_audio_file.cpp
    src/lib/ps2_pss_movie.cpp
    src/lib/ps2_texture_replace.cpp
    src/lib/ps2_texture_pack.cpp
    src/lib/ps2_gs_texture_audit.cpp
    src/lib/ps2_gs_shots.cpp
    src/lib/ps2_zaxis_ui.cpp
    src/lib/ps2_zaxis_text.cpp
    src/lib/ps2_xbox_audio.cpp
    src/lib/ps2_glyph_art.cpp
    src/lib/ps2_pad_art.cpp
    src/lib/ps2_photo.cpp
    src/lib/ps2_settings.cpp
    src/lib/ps2_boot_config.cpp
    src/lib/ps2_boot_sequence.cpp
    src/lib/ps2_session.cpp
    src/lib/ps2_memory.cpp
    src/lib/guest_map.cpp
    src/lib/guest_heap.cpp
    src/lib/heap_trace.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_pad_script.cpp
    src/lib/ps2_host_input.cpp
    src/lib/ps2_game_controls.cpp
    src/lib/ps2_host_sdl.cpp
    src/lib/gs/gs_gpu_texture_check.cpp
    src/lib/ps2_state_probe.cpp
    src/lib/ps2_thread_cpu.cpp
    src/lib/ps2_frame_trace.cpp
    src/lib/ps2_debug_control.cpp
    src/lib/ps2_lane_watch.cpp
    src/lib/ps2_play_link.cpp
    src/lib/ps2_spot_fork.cpp
    src/lib/play_recording.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_save_store.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_runtime.cpp
    src/lib/game_module.cpp
    src/lib/ps2_frame_interpolation.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_interpreter.cpp
    src/lib/vu/ps2_vu1_decoder.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/vu/ps2_vu1_record.cpp
    src/lib/vu/ps2_vu1_aot_generator.cpp
    src/lib/games_database.cpp
    src/lib/panel_snapshot.cpp
    # The collection (src/collection): what spans the games, built into this
    # library for the runner and the tests to link (the runtime's own code
    # includes none of it: it knows its games by runtime/ps2_games.h).
    src/collection/menu.cpp
    src/collection/mods.cpp
    src/collection/maps.cpp
    src/collection/settings.cpp
    src/collection/disc_check.cpp
    src/collection/game_setup.cpp
    src/collection/save_transfer.cpp
    src/collection/card_import.cpp
    src/collection/game_code.cpp
    # The front end: the games' menu sound, pictures and movies from their
    # discs, and the Thor's bottom screen in the menu.
    src/collection/game_order.cpp
    src/collection/sound_bank.cpp
    src/collection/menu_audio.cpp
    src/collection/front_art.cpp
    src/collection/menu_bottom.cpp
    src/collection/menu_scene.cpp
    # The font the app ships (resources/fonts, OFL 1.1) drawn with stb_truetype.
    src/collection/vector_font.cpp
    # The map converter (collection/levels): a map made in one game's level
    # formats into another's, in memory, when Custom Freeplay starts it.
    src/collection/levels/chunks.cpp
    src/collection/levels/geometry.cpp
    src/collection/levels/scene.cpp
    src/collection/levels/model.cpp
    src/collection/levels/export.cpp
    src/collection/levels/text.cpp
    src/collection/levels/arithmetic.cpp
    src/collection/levels/textures.cpp
    src/collection/levels/strips.cpp
    src/collection/levels/convert.cpp
    src/collection/levels/convert_dm2.cpp
    src/collection/levels/convert_bmx.cpp
    # The extras (collection/extras): the other releases' discs the player
    # adds, read and made into the files the games load.
    src/collection/extras/disc_image.cpp
    src/collection/extras/rvz_image.cpp
    src/collection/extras/xbox_disc.cpp
    src/collection/extras/gamecube_disc.cpp
    src/collection/extras/source_files.cpp
    src/collection/extras/xbox_texture_pack.cpp
    src/collection/extras/sequence_matcher.cpp
    src/collection/extras/bmx_releases.cpp
    src/collection/extras/dm2_files.cpp
    src/collection/extras/dm2_xbox.cpp
    src/collection/extras/dm2_xbox_levels.cpp
    src/collection/extras/dm2_xbox_themes.cpp
    src/collection/extras/dm2_xbox_career.cpp
    src/collection/extras/ai_xbox_files.cpp
    src/collection/extras/ai_converted.cpp
    src/collection/extras/ai_xbox_skaters.cpp
    src/collection/extras/setup.cpp
    src/collection/extras/panel_maps.cpp
    src/collection/extras/panel_maps_ai.cpp
    src/collection/extras/panel_maps_dm2.cpp
    src/collection/extras/panel_maps_bmx.cpp
    src/collection/extras/panel_raster.cpp
    src/collection/extras/panel_goals.cpp
    src/collection/extras/panel_ui.cpp
)

# The build's commit for the play recordings' header (cmake/BuildCommit.cmake).
set(ZAXIS_BUILD_COMMIT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_commit/play_recording_commit.h")
add_custom_target(zaxis_build_commit
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${ZAXIS_BUILD_COMMIT_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildCommit.cmake
    BYPRODUCTS ${ZAXIS_BUILD_COMMIT_HEADER}
    VERBATIM)
add_dependencies(ps2_runtime zaxis_build_commit)
if(APPLE)
    # play_recording.cpp asks Foundation for Low Power Mode (through the Objective-C runtime).
    target_link_libraries(ps2_runtime PUBLIC objc "-framework Foundation")
endif()
set_source_files_properties(src/lib/play_recording.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/build_commit"
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON)

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
    )
endif()

# Per-function enter/exit tracing in generated code writes and flushes a line
# to ps2_log.txt for every guest call, which slows games down by orders of
# magnitude, so it is separate from the other verbose logs.
if(PS2X_ENABLE_AGRESSIVE_LOGS AND PS2X_ENABLE_FUNCTION_LOG_TRACKER)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

if(ZAXIS_LANE_WATCH)
    target_compile_definitions(ps2_runtime PUBLIC ZAXIS_LANE_WATCH=1)
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
    PS2X_HAS_AVFORMAT=$<BOOL:${PS2X_HAS_AVFORMAT}>
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

set(PS2X_RUNNER_GENERATED_DIR "" CACHE PATH
    "Directory with AI's recompiler output (.cpp + headers): its game module (cmake/game_module.cmake), the runtime's default code")

# Each game's generated code is a game module (cmake/game_module.cmake): linked into the runner or a shared library
# beside it. The runner itself has only the global table (src/runner: empty, the fallback for an ELF no module names).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/game_module.cmake")
file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)
if(PS2X_RUNNER_GENERATED_DIR)
    ps2x_add_game_module(ai "${PS2X_RUNNER_GENERATED_DIR}")
endif()

# Per-game override modules (PS2_REGISTER_GAME_OVERRIDE). They are keyed by ELF
# metadata, so linking all of them into every runner is harmless. They are
# compiled into the executable rather than ps2_runtime so the static
# registration objects are never dropped by the linker.
file(GLOB PS2X_GAME_OVERRIDE_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/game_overrides/*.cpp"
)
list(APPEND RUNNER_SRC_FILES ${PS2X_GAME_OVERRIDE_SRC_FILES})

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

if(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    # SDLActivity loads this library and calls SDL_main (android/build-apk.sh).
    # (-Bsymbolic: FFmpeg's assembly addresses its tables directly)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=JNI_OnLoad" "-Wl,-Bsymbolic")
    set_source_files_properties("${ROOT_MAIN_CPP}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

# More games in the same runner: recompiler output made with [general]
# namespace = "<id>", one directory each. Every set is an object library of
# its own (its own generated headers on the include path) whose tables
# register by ELF name (ps2_generated in ps2_runtime.h); the runtime picks
# the set of the ELF it boots, else the set of PS2X_RUNNER_GENERATED_DIR.
set(PS2X_RUNNER_EXTRA_GENERATED_DIRS "" CACHE STRING
    "Semicolon list of namespaced recompiler output directories built into the runner as well")
foreach(_ps2x_extra_dir IN LISTS PS2X_RUNNER_EXTRA_GENERATED_DIRS)
    if(NOT EXISTS "${_ps2x_extra_dir}/register_functions.cpp")
        message(FATAL_ERROR "PS2X_RUNNER_EXTRA_GENERATED_DIRS: ${_ps2x_extra_dir} has no register_functions.cpp")
    endif()
    # (the module's name is the recompiler's namespace, as its registration names it)
    file(STRINGS "${_ps2x_extra_dir}/register_functions.cpp" _ps2x_registrar REGEX "Registrar g_ps2GeneratedCodeRegistrar_[A-Za-z0-9_]+\\{" LIMIT_COUNT 1)
    if(NOT _ps2x_registrar MATCHES "Registrar g_ps2GeneratedCodeRegistrar_([A-Za-z0-9_]+)\\{")
        message(FATAL_ERROR "PS2X_RUNNER_EXTRA_GENERATED_DIRS: ${_ps2x_extra_dir} is not namespaced recompiler output")
    endif()
    ps2x_add_game_module(${CMAKE_MATCH_1} "${_ps2x_extra_dir}")
endforeach()
ps2x_link_game_modules(ps2EntryRunner)

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()
# The game thread runs all guest code, so its stack depth follows the guest
# call depth: give threads Linux's 8 MiB (Windows defaults to 1 MiB, and
# std::thread takes the executable's default).
if(WIN32)
    if(MSVC)
        target_link_options(ps2EntryRunner PRIVATE "/STACK:8388608")
    elseif(MINGW)
        target_link_options(ps2EntryRunner PRIVATE "-Wl,--stack,8388608")
    endif()
endif()

# VU arithmetic rounds the product and the sum separately (as PCSX2 does);
# do not let the compiler fuse a*b+c into one FMA, which also made the
# results depend on the host architecture. clang-cl (MSVC in CMake) takes
# the clang options after /clang:; cl does not contract under /fp:precise.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(PS2X_FP_NO_CONTRACT "/clang:-ffp-contract=off")
    set(PS2X_FP_ROUNDING_MATH "/clang:-frounding-math")
elseif(MSVC)
    set(PS2X_FP_NO_CONTRACT "/fp:precise")
    set(PS2X_FP_ROUNDING_MATH "/fp:strict")
else()
    set(PS2X_FP_NO_CONTRACT "-ffp-contract=off")
    set(PS2X_FP_ROUNDING_MATH "-frounding-math")
endif()
set_source_files_properties(
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_interpreter.cpp
    src/lib/vu/ps2_vu1_decoder.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    PROPERTIES COMPILE_OPTIONS "${PS2X_FP_NO_CONTRACT}")
# The VU1 ahead-of-time generator's key: a hash of what it writes from (the generator, the VU1 decoder and core under
# src/lib/vu, include/runtime/ps2_vu1*.h). The build's own VU1 code (PS2X_VU1_AOT_DIR) and a game module's made at
# first run (collection/game_code.cpp) are made by the generator with this key.
file(GLOB _ps2x_vu1_generator CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/vu1_aot.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*" "${CMAKE_CURRENT_SOURCE_DIR}/include/runtime/ps2_vu1*.h")
list(SORT _ps2x_vu1_generator)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ps2x_vu1_generator})
set(_ps2x_vu1_generator_text "")
foreach(_ps2x_file IN LISTS _ps2x_vu1_generator)
    file(SHA256 "${_ps2x_file}" _ps2x_hash)
    get_filename_component(_ps2x_name "${_ps2x_file}" NAME)
    string(APPEND _ps2x_vu1_generator_text "${_ps2x_name} ${_ps2x_hash}\n")
endforeach()
string(SHA256 PS2X_VU1_GENERATOR_KEY "${_ps2x_vu1_generator_text}")
string(SUBSTRING "${PS2X_VU1_GENERATOR_KEY}" 0 16 PS2X_VU1_GENERATOR_KEY)
set_property(SOURCE src/lib/vu/ps2_vu1_aot_generator.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             "PS2X_VU1_GENERATOR_KEY=\"${PS2X_VU1_GENERATOR_KEY}\"")
# Estate's level picture and icon (build time): the same bytes on every host.
set_source_files_properties(src/lib/ps2_zaxis_estate_icon.cpp PROPERTIES COMPILE_OPTIONS "${PS2X_FP_NO_CONTRACT}")
# The map converter computes as the tooling's Python reference does (each
# product and sum rounded on its own): the same files on every host.
file(GLOB PS2X_LEVELS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/levels/*.cpp")
set_source_files_properties(${PS2X_LEVELS_SOURCES} PROPERTIES COMPILE_OPTIONS "${PS2X_FP_NO_CONTRACT}")
# The extras' converters too (collection/extras): the bytes the tooling's Python tools write, on every host.
file(GLOB PS2X_EXTRAS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/extras/*.cpp")
set_source_files_properties(${PS2X_EXTRAS_SOURCES} PROPERTIES COMPILE_OPTIONS "${PS2X_FP_NO_CONTRACT}")

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
    ${CMAKE_CURRENT_BINARY_DIR}/flavour
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)
# stb_truetype's one header, for collection/vector_font.cpp only.
set_source_files_properties(src/collection/vector_font.cpp PROPERTIES INCLUDE_DIRECTORIES "${PS2X_STB_DIR}" SKIP_UNITY_BUILD_INCLUSION ON)
# (the recorder and the checkers read and write compressed recordings: ee/fnrec_file.cpp, in every game's instance)
# (and the extras' setup reads RVZ images: collection/extras/rvz_image.cpp, in both flavours)
if(ZAXIS_DEV_CHECKS)
    target_link_libraries(ps2_runtime PUBLIC ps2x::zstd)
else()
    target_link_libraries(ps2_runtime PRIVATE ps2x::zstd)
endif()
target_link_libraries(ps2_runtime PRIVATE ps2x_xxhash)

# ps2x_write_if_changed(PATH TEXT): PATH holds TEXT, rewritten only when it differs: a header or resource
# made at configure time keeps its time when the configure changes nothing, so its users are not compiled
# and the runner is not linked again (each rewrite cost make-windows' rerun of an unchanged tree a ThinLTO
# link: about 6 minutes on a Windows build).
function(ps2x_write_if_changed path text)
    if(EXISTS "${path}")
        file(READ "${path}" _ps2x_old)
        if(_ps2x_old STREQUAL text)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${text}")
endfunction()

# miniz 3.0.2 (MIT licence, github.com/richgel999/miniz, the tag's commit):
# zip mods read in place (runtime/ps2_zip_archive.h). Its own CMakeLists asks
# for CMake 3.0, which CMake 4 refuses, so its four sources are built here
# (SOURCE_SUBDIR names a folder it does not have: fetched, not added), with
# the export header its CMakeLists would generate for a static library.
FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.tar.gz # commit 293d4db1b
    URL_HASH SHA256=c4b4c25a4eb81883448ff8924e6dba95c800094a198dc9ce66a292ac2ef8e018
    SOURCE_SUBDIR not-added
)
FetchContent_MakeAvailable(miniz)
ps2x_write_if_changed("${CMAKE_CURRENT_BINARY_DIR}/miniz/miniz_export.h"
    "#pragma once\n// Made by runtime/CMakeLists.txt: miniz built as a static library.\n#define MINIZ_EXPORT\n#define MINIZ_NO_EXPORT\n")
add_library(ps2x_miniz STATIC
    "${miniz_SOURCE_DIR}/miniz.c"
    "${miniz_SOURCE_DIR}/miniz_zip.c"
    "${miniz_SOURCE_DIR}/miniz_tinfl.c"
    "${miniz_SOURCE_DIR}/miniz_tdef.c")
target_include_directories(ps2x_miniz PUBLIC "${miniz_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miniz")
target_compile_definitions(ps2x_miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(ps2x_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ps2_runtime PRIVATE ps2x_miniz)

# Hardware GS renderer on SDL_GPU (prototype, src/lib/gs/GPU_BACKEND.md):
# a separate library linked only by what uses it.
if(PS2X_GS_GPU AND TARGET ps2x::sdl3)
    add_library(ps2_gs_gpu STATIC src/lib/gs/gs_gpu_backend.cpp)
    target_link_libraries(ps2_gs_gpu PUBLIC ps2_runtime ps2x::sdl3)
    target_compile_definitions(ps2_gs_gpu PRIVATE PS2X_HAS_GS_GPU=1)
    # Direct3D 12: the HLSL shaders compiled to DXIL at build time with the
    # Windows SDK's DXC (Metal compiles the MSL source at run time).
    if(WIN32)
        set(_ps2x_pf86 "ProgramFiles(x86)")
        file(GLOB _ps2x_sdk_bins LIST_DIRECTORIES true "$ENV{${_ps2x_pf86}}/Windows Kits/10/bin/10.*")
        list(SORT _ps2x_sdk_bins COMPARE NATURAL ORDER DESCENDING)
        set(_ps2x_dxc_hints "")
        if(DEFINED ENV{WindowsSdkVerBinPath})
            list(APPEND _ps2x_dxc_hints "$ENV{WindowsSdkVerBinPath}/x64")
        endif()
        foreach(_ps2x_bin IN LISTS _ps2x_sdk_bins)
            list(APPEND _ps2x_dxc_hints "${_ps2x_bin}/x64")
        endforeach()
        find_program(PS2X_DXC dxc HINTS ${_ps2x_dxc_hints})
        if(PS2X_DXC)
            set(_ps2x_dxil_dir "${CMAKE_CURRENT_BINARY_DIR}/gs_gpu_dxil")
            set(_ps2x_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gs/gs_gpu_shaders.hlsl")
            set(_ps2x_dxil_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileGsShadersDxil.cmake")
            add_custom_command(
                OUTPUT "${_ps2x_dxil_dir}/gs_gpu_shaders_dxil.h"
                COMMAND "${CMAKE_COMMAND}" "-DDXC=${PS2X_DXC}" "-DSRC=${_ps2x_hlsl}"
                        "-DOUT=${_ps2x_dxil_dir}/gs_gpu_shaders_dxil.h" -P "${_ps2x_dxil_script}"
                DEPENDS "${_ps2x_hlsl}" "${_ps2x_dxil_script}"
                COMMENT "Compiling the GS GPU shaders to DXIL"
                VERBATIM)
            target_sources(ps2_gs_gpu PRIVATE "${_ps2x_dxil_dir}/gs_gpu_shaders_dxil.h")
            target_include_directories(ps2_gs_gpu PRIVATE "${_ps2x_dxil_dir}")
            target_compile_definitions(ps2_gs_gpu PRIVATE PS2X_GS_GPU_DXIL=1)
            message(STATUS "GS GPU backend: Direct3D 12 shaders compiled with ${PS2X_DXC}")
        else()
            message(WARNING "No dxc (Windows SDK) found: the GPU renderer has no Direct3D 12 shaders")
        endif()
    endif()
    # Android (Vulkan): the same HLSL to SPIR-V with the NDK's glslc.
    if(PS2X_IS_ANDROID)
        file(GLOB _ps2x_glslc_hints "${ANDROID_NDK}/shader-tools/*")
        find_program(PS2X_GLSLC glslc HINTS ${_ps2x_glslc_hints} REQUIRED)
        set(_ps2x_spirv_dir "${CMAKE_CURRENT_BINARY_DIR}/gs_gpu_spirv")
        set(_ps2x_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gs/gs_gpu_shaders.hlsl")
        set(_ps2x_dxil_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileGsShadersDxil.cmake")
        add_custom_command(
            OUTPUT "${_ps2x_spirv_dir}/gs_gpu_shaders_spirv.h"
            COMMAND "${CMAKE_COMMAND}" "-DGLSLC=${PS2X_GLSLC}" "-DSRC=${_ps2x_hlsl}"
                    "-DOUT=${_ps2x_spirv_dir}/gs_gpu_shaders_spirv.h" -P "${_ps2x_dxil_script}"
            DEPENDS "${_ps2x_hlsl}" "${_ps2x_dxil_script}"
            COMMENT "Compiling the GS GPU shaders to SPIR-V"
            VERBATIM)
        target_sources(ps2_gs_gpu PRIVATE "${_ps2x_spirv_dir}/gs_gpu_shaders_spirv.h")
        target_include_directories(ps2_gs_gpu PRIVATE "${_ps2x_spirv_dir}")
        target_compile_definitions(ps2_gs_gpu PRIVATE PS2X_GS_GPU_SPIRV=1)
    endif()
endif()

# The readable game and its checker, ps2xFnCheck (src/game).
include("${CMAKE_CURRENT_SOURCE_DIR}/src/game/game.cmake")

# The game overrides' symbols (tools/override_symbols.py): an override names its game's functions and data
# (sym::dm2::g_cameraSettings, sym::bmx::..., sym:: for Aggressive Inline's) from the readable code's table and
# the bind tables, in <build>/symbols/overrides/sym/game_overrides/<source>.h, written only when their text
# changes.
set(PS2X_OVERRIDE_SYMBOLS_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/override_symbols.py")
set(PS2X_OVERRIDE_SYMBOLS_DIR "${PS2X_SYMBOLS_DIR}/overrides")
set(_ps2x_override_symbols_args headers --out "${PS2X_OVERRIDE_SYMBOLS_DIR}")
set(_ps2x_override_symbols_depends ${PS2X_GAME_OVERRIDE_SRC_FILES} "${PS2X_OVERRIDE_SYMBOLS_TOOL}"
    "${PS2X_SYMBOLS_TOOL}" "${PS2X_SYMBOLS_TABLE}")
file(GLOB PS2X_GAME_OVERRIDE_HEADER_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game_overrides/*.h")
list(APPEND _ps2x_override_symbols_depends ${PS2X_GAME_OVERRIDE_HEADER_FILES})
# (and the extras' converters, which name the data tables they read from a game's ELF the same way)
list(APPEND _ps2x_override_symbols_depends ${PS2X_EXTRAS_SOURCES})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2X_OVERRIDE_SYMBOLS_TOOL}")
if(PS2X_BIND_DIR)
    list(APPEND _ps2x_override_symbols_args --bind-dir "${PS2X_BIND_DIR}")
    foreach(_ps2x_override_game dm2 bmx)
        foreach(_ps2x_override_input "${PS2X_BIND_DIR}/${_ps2x_override_game}.tsv"
                                     "${PS2X_GAME_DIR}/${_ps2x_override_game}/bound.tsv")
            if(EXISTS "${_ps2x_override_input}")
                list(APPEND _ps2x_override_symbols_depends "${_ps2x_override_input}")
            endif()
        endforeach()
    endforeach()
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_OVERRIDE_SYMBOLS_TOOL}" ${_ps2x_override_symbols_args}
                RESULT_VARIABLE _ps2x_override_symbols_result)
if(NOT _ps2x_override_symbols_result EQUAL 0)
    message(FATAL_ERROR "the game overrides' symbol headers failed")
endif()
file(GLOB_RECURSE _ps2x_override_symbols_headers "${PS2X_OVERRIDE_SYMBOLS_DIR}/sym/*.h")
add_custom_command(
    OUTPUT "${PS2X_SYMBOLS_DIR}/overrides.stamp"
    BYPRODUCTS ${_ps2x_override_symbols_headers}
    COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_OVERRIDE_SYMBOLS_TOOL}" ${_ps2x_override_symbols_args}
    COMMAND "${Python3_EXECUTABLE}" -B "${PS2X_OVERRIDE_SYMBOLS_TOOL}" lint
    COMMAND "${CMAKE_COMMAND}" -E touch "${PS2X_SYMBOLS_DIR}/overrides.stamp"
    DEPENDS ${_ps2x_override_symbols_depends}
    COMMENT "the game overrides' symbol headers, their literal lint"
    VERBATIM)
add_custom_target(ps2_override_symbols DEPENDS "${PS2X_SYMBOLS_DIR}/overrides.stamp")
add_dependencies(ps2EntryRunner ps2_override_symbols)
target_include_directories(ps2EntryRunner PRIVATE "${PS2X_OVERRIDE_SYMBOLS_DIR}")
add_dependencies(ps2_runtime ps2_override_symbols)
target_include_directories(ps2_runtime PRIVATE "${PS2X_OVERRIDE_SYMBOLS_DIR}")
# The panel harness reads DM2's RAM with DM2's own capture (--game dm2), by its symbols.
target_sources(ps2xPanelShot PRIVATE src/game_overrides/dave_mirra_panel.cpp)
# And BMX XXX's RAM with BMX XXX's (--game bmx).
target_sources(ps2xPanelShot PRIVATE src/game_overrides/bmx_xxx_panel.cpp)
add_dependencies(ps2xPanelShot ps2_override_symbols)
target_include_directories(ps2xPanelShot PRIVATE "${PS2X_OVERRIDE_SYMBOLS_DIR}")

# Offline GS trace replay (traces come from PS2X_GS_DUMP, see gs_record.h).
if(NOT PS2X_IS_ANDROID)
    add_executable(ps2xGsReplay tools/gs_replay.cpp)
    target_link_libraries(ps2xGsReplay PRIVATE ps2_runtime)
    if(TARGET ps2_gs_gpu)
        target_link_libraries(ps2xGsReplay PRIVATE ps2_gs_gpu)
        target_compile_definitions(ps2xGsReplay PRIVATE PS2X_HAS_GS_GPU=1)
    endif()
    # Offline VU1 corpus check (corpora come from PS2X_VU1_RECORD). It loads game modules (--module: their VU1 code)
    # as the runner does, so it has the runner's (empty) global function table.
    add_executable(ps2xVu1Check tools/vu1_check.cpp src/runner/register_functions.cpp)
    target_link_libraries(ps2xVu1Check PRIVATE ps2_runtime)
    # Ahead-of-time compiled VU1 microprograms (see src/lib/vu/ps2_vu1_aot.h).
    add_executable(ps2xVu1Aot tools/vu1_aot.cpp)
    target_link_libraries(ps2xVu1Aot PRIVATE ps2_runtime)
    # Aggressive Inline's Zoo and Ape made at build time from the user's Xbox
    # files (runtime/ps2_zaxis_zoo.h, ps2_zaxis_ape.h; tools/make-app.sh and
    # make-windows.ps1 run it).
    add_executable(ps2xXboxConvert tools/xbox_convert.cpp)
    target_link_libraries(ps2xXboxConvert PRIVATE ps2_runtime)
    # TUT/Estate, the GameCube disc's earlier Tutorial, made at build time
    # from the files tools/gc_disc.py extracts (runtime/ps2_zaxis_estate.h).
    add_executable(ps2xGcConvert tools/gc_convert.cpp)
    target_link_libraries(ps2xGcConvert PRIVATE ps2_runtime)
    # The collection's extras (collection/extras) on the command line: the
    # Xbox and GameCube disc images read, for the checks against the
    # tooling's Python tools (the reference).
    add_executable(ps2xExtras tools/extras_tool.cpp src/collection/extras/xbox_model_maker.cpp
        # (the engine's Xbox model converter, as ps2xNativeAssets builds it)
        src/game/zge/native/texture_library.cpp src/game/zge/native/native_textures.cpp
        src/game/zge/native/disc_files.cpp src/game/zge/native/geo_file.cpp src/game/zge/native/geo_meshes.cpp
        src/game/zge/native/xbox_models.cpp)
    target_link_libraries(ps2xExtras PRIVATE ps2_runtime)
    target_include_directories(ps2xExtras PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
    # The collection's map converter on the command line (collection/levels):
    # its files written to a folder and its records read back, for the checks
    # against the tooling's Python converters (the reference).
    add_executable(ps2xMapConvert tools/map_convert.cpp)
    target_link_libraries(ps2xMapConvert PRIVATE ps2_runtime)
endif()

# PS2X_VU1_AOT_DIR, PS2X_VU1_AOT_EXTRA_DIRS: folders of ps2xVu1Aot output, each with the micro memory images
# it was made from (<hash>.vu1code), built into the runner and ps2xVu1Check. They register themselves by micro
# memory hash, so they go into the executables (a static library member nothing refers to would be dropped).
# The code follows this tree's generator: the generator key is a hash of what ps2xVu1Aot writes from (its
# source, the VU1 decoder under src/lib/vu and include/runtime/ps2_vu1*.h) and of the folder's images. A folder
# whose vu1_aot.key holds the key is built as it is; else the build makes this tree's code from the folder's
# images with its own ps2xVu1Aot, into the build dir (vu1-aot/<key>: generated output never goes beside the
# input, which may sit in a source checkout; cmake/vu1_aot_generate.cmake makes it aside and renames it into
# place, never written over). A build that cannot run its own ps2xVu1Aot (a cross build: Android) builds the
# generator alone for the build machine (tools/vu1_aot_host), or takes PS2X_VU1_AOT_TOOL, a host's ps2xVu1Aot.
set(PS2X_VU1_AOT_DIR "" CACHE PATH "Directory of ps2xVu1Aot output (ahead-of-time compiled VU1 microprograms)")
# More games: their AOT sources too (they register by micro memory hash).
set(PS2X_VU1_AOT_EXTRA_DIRS "" CACHE STRING "Semicolon list of more ps2xVu1Aot output directories")
set(PS2X_VU1_AOT_TOOL "" CACHE FILEPATH "A host ps2xVu1Aot for a build that cannot run its own (cross builds)")
if(PS2X_VU1_AOT_DIR OR PS2X_VU1_AOT_EXTRA_DIRS)
    # (the generator's text, above: the key adds the folder's images)
    set(_ps2x_vu1_generate "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vu1_aot_generate.cmake")
    set(PS2X_VU1_AOT_SOURCES "")
    set(_ps2x_vu1_keys "")
    foreach(_ps2x_aot_dir IN ITEMS ${PS2X_VU1_AOT_DIR} ${PS2X_VU1_AOT_EXTRA_DIRS})
        string(REGEX REPLACE "/+$" "" _ps2x_aot_dir "${_ps2x_aot_dir}")
        file(GLOB _ps2x_images CONFIGURE_DEPENDS "${_ps2x_aot_dir}/*.vu1code")
        list(SORT _ps2x_images)
        set(_ps2x_key_text "${_ps2x_vu1_generator_text}")
        foreach(_ps2x_image IN LISTS _ps2x_images)
            file(SHA256 "${_ps2x_image}" _ps2x_hash)
            string(APPEND _ps2x_key_text "image ${_ps2x_hash}\n")
        endforeach()
        string(SHA256 _ps2x_key "${_ps2x_key_text}")
        string(SUBSTRING "${_ps2x_key}" 0 16 _ps2x_key)
        set(_ps2x_out "${CMAKE_BINARY_DIR}/vu1-aot/${_ps2x_key}")
        if(_ps2x_key IN_LIST _ps2x_vu1_keys)
            continue() # (another folder with the same images: the same code, built once)
        endif()
        list(APPEND _ps2x_vu1_keys "${_ps2x_key}")
        set(_ps2x_stamp "")
        if(EXISTS "${_ps2x_aot_dir}/vu1_aot.key")
            file(READ "${_ps2x_aot_dir}/vu1_aot.key" _ps2x_stamp)
            string(STRIP "${_ps2x_stamp}" _ps2x_stamp)
        endif()
        if(_ps2x_stamp STREQUAL _ps2x_key)
            file(GLOB _ps2x_aot_sources CONFIGURE_DEPENDS "${_ps2x_aot_dir}/vu1_aot_*.cpp")
            list(APPEND PS2X_VU1_AOT_SOURCES ${_ps2x_aot_sources})
            message(STATUS "VU1 ahead-of-time code: ${_ps2x_aot_dir} (generator key ${_ps2x_key})")
            continue()
        endif()
        if(NOT _ps2x_images)
            message(FATAL_ERROR "${_ps2x_aot_dir}: no micro memory image (*.vu1code) to make its VU1 code from with "
                                "this tree's ps2xVu1Aot (generator key ${_ps2x_key})")
        endif()
        set(_ps2x_tool_depends "")
        if(PS2X_VU1_AOT_TOOL)
            set(_ps2x_tool "${PS2X_VU1_AOT_TOOL}")
        elseif(TARGET ps2xVu1Aot)
            set(_ps2x_tool "$<TARGET_FILE:ps2xVu1Aot>")
        else()
            # A cross build (Android) cannot run its own: the generator alone built for the build machine
            # (tools/vu1_aot_host, the host's compiler), once for every folder.
            if(NOT TARGET ps2x_vu1_aot_host)
                include(ExternalProject)
                set(_ps2x_host_dir "${CMAKE_BINARY_DIR}/vu1-aot-host")
                set(_ps2x_host_tool "${_ps2x_host_dir}/build/ps2xVu1Aot${CMAKE_HOST_EXECUTABLE_SUFFIX}")
                ExternalProject_Add(ps2x_vu1_aot_host
                    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/vu1_aot_host"
                    BINARY_DIR "${_ps2x_host_dir}/build"
                    PREFIX "${_ps2x_host_dir}"
                    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DPS2X_VU1_GENERATOR_KEY=${PS2X_VU1_GENERATOR_KEY}"
                    BUILD_ALWAYS ON
                    INSTALL_COMMAND ""
                    BUILD_BYPRODUCTS "${_ps2x_host_tool}")
            endif()
            set(_ps2x_tool "${_ps2x_host_tool}")
            set(_ps2x_tool_depends ps2x_vu1_aot_host)
            message(STATUS "VU1 ahead-of-time code: the generator built for the build machine (tools/vu1_aot_host)")
        endif()
        # (ps2xVu1Aot writes a header, the dispatcher and 32 chunk sources per image: tools/vu1_aot.cpp)
        set(_ps2x_outputs "${_ps2x_out}/vu1_aot.key")
        set(_ps2x_aot_sources "")
        foreach(_ps2x_image IN LISTS _ps2x_images)
            get_filename_component(_ps2x_stem "${_ps2x_image}" NAME_WE)
            list(APPEND _ps2x_outputs "${_ps2x_out}/vu1_aot_${_ps2x_stem}.h")
            list(APPEND _ps2x_aot_sources "${_ps2x_out}/vu1_aot_${_ps2x_stem}.cpp")
            foreach(_ps2x_chunk RANGE 0 31)
                if(_ps2x_chunk LESS 10)
                    set(_ps2x_chunk "0${_ps2x_chunk}")
                endif()
                list(APPEND _ps2x_aot_sources "${_ps2x_out}/vu1_aot_${_ps2x_stem}_c${_ps2x_chunk}.cpp")
            endforeach()
        endforeach()
        string(REPLACE ";" "|" _ps2x_image_list "${_ps2x_images}") # (one argument: the script splits it)
        add_custom_command(
            OUTPUT ${_ps2x_outputs} ${_ps2x_aot_sources}
            COMMAND "${CMAKE_COMMAND}" "-DTOOL=${_ps2x_tool}" "-DOUT=${_ps2x_out}" "-DKEY=${_ps2x_key}"
                    "-DIMAGES=${_ps2x_image_list}" -P "${_ps2x_vu1_generate}"
            DEPENDS ${_ps2x_images} "${_ps2x_vu1_generate}" ${_ps2x_tool_depends}
            COMMENT "VU1 ahead-of-time code: ${_ps2x_out}"
            VERBATIM)
        list(APPEND PS2X_VU1_AOT_SOURCES ${_ps2x_aot_sources})
        message(STATUS "VU1 ahead-of-time code: ${_ps2x_out} (made by the build: generator key ${_ps2x_key})")
    endforeach()
    set_source_files_properties(${PS2X_VU1_AOT_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${PS2X_FP_NO_CONTRACT};${PS2X_FP_ROUNDING_MATH}"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu")
    target_sources(ps2EntryRunner PRIVATE ${PS2X_VU1_AOT_SOURCES})
    if(TARGET ps2xVu1Check)
        target_sources(ps2xVu1Check PRIVATE ${PS2X_VU1_AOT_SOURCES})
    endif()
endif()
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)
# Opt-in GPU renderer in the runner: PS2X_GS_BACKEND=gpu.
if(TARGET ps2_gs_gpu)
    target_sources(ps2EntryRunner PRIVATE src/lib/gs/gs_gpu_register.cpp)
    set_source_files_properties(src/lib/gs/gs_gpu_register.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_link_libraries(ps2EntryRunner PRIVATE ps2_gs_gpu)
endif()

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

# The game overrides install their panel captures (aggressive_inline_panel.cpp,
# dave_mirra_panel.cpp, bmx_xxx_panel.cpp); on Android the panel's sources are
# the runner's own (below).
if(NOT PS2X_IS_ANDROID)
    target_link_libraries(ps2EntryRunner PRIVATE ps2_panel)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

# Single-game app front end (tools/make-app.sh in the game repo sets these):
# started without an ELF argument, the runner checks the game files bundled
# with it (Resources/disc, extracted from the disc image when the app is
# built), runs the game from them and keeps settings, saves and logs in the
# per-user folders.
# Empty PS2X_APP_NAME: the developer runner only.
set(PS2X_APP_NAME "" CACHE STRING "Build the runner as this single-game app (window title, folder names)")
set(PS2X_APP_GAME_TITLE "" CACHE STRING "Game title shown in the app's messages, e.g. Aggressive Inline (USA)")
set(PS2X_APP_ELF "" CACHE STRING "Boot ELF the app expects in its game files, e.g. SLUS_203.27")
set(PS2X_APP_ELF_SHA256 "" CACHE STRING "SHA-256 of the ELF the generated code was recompiled from (empty: not checked)")
set(PS2X_APP_SAVE_DIR "" CACHE STRING "The game's memory card save folder, for importing a PCSX2 save")
set(PS2X_APP_VERSION "${PROJECT_VERSION}" CACHE STRING "App version shown in the log")
set(PS2X_APP_ICON "" CACHE FILEPATH "Windows: .ico for the app's executable and window")

# Several games in one app (a collection named PS2X_APP_NAME): one entry per
# game, "id|name|title|ELF|ELF SHA-256|PCSX2 save folder" (empty fields
# allowed). The app then picks the game (--game <id> or a picker) and keeps
# each game's files in <support>/games/<id> (src/collection/app.cpp).
set(PS2X_APP_GAMES "" CACHE STRING "Semicolon list of the games of a collection app (id|name|title|elf|sha256|savedir)")

if(PS2X_APP_NAME)
    if(NOT PS2X_APP_ELF AND NOT PS2X_APP_GAMES)
        message(FATAL_ERROR "PS2X_APP_NAME needs PS2X_APP_ELF (or PS2X_APP_GAMES)")
    endif()
    if(PS2X_APP_GAMES)
        set(_ps2x_games_h "${CMAKE_CURRENT_BINARY_DIR}/ps2x_app_games.h")
        set(_ps2x_games_text "// Generated by CMake from PS2X_APP_GAMES.\nnamespace ps2x_app\n{\n    constexpr GameProfile kGames[] = {\n")
        foreach(_ps2x_game IN LISTS PS2X_APP_GAMES)
            string(REPLACE "|" ";" _ps2x_fields "${_ps2x_game}")
            list(LENGTH _ps2x_fields _ps2x_field_count)
            if(NOT _ps2x_field_count EQUAL 6)
                message(FATAL_ERROR "PS2X_APP_GAMES entry needs 6 fields: ${_ps2x_game}")
            endif()
            list(GET _ps2x_fields 0 _id)
            list(GET _ps2x_fields 1 _name)
            list(GET _ps2x_fields 2 _title)
            list(GET _ps2x_fields 3 _elf)
            list(GET _ps2x_fields 4 _sha)
            list(GET _ps2x_fields 5 _save)
            string(APPEND _ps2x_games_text "        {\"${_id}\", \"${_name}\", \"${_title}\", \"${_elf}\", \"${_sha}\", \"${_save}\"},\n")
        endforeach()
        string(APPEND _ps2x_games_text "    };\n}\n")
        ps2x_write_if_changed("${_ps2x_games_h}" "${_ps2x_games_text}")
        if(NOT PS2X_APP_ELF)
            list(GET PS2X_APP_GAMES 0 _ps2x_first)
            string(REPLACE "|" ";" _ps2x_first "${_ps2x_first}")
            list(GET _ps2x_first 3 PS2X_APP_ELF)
        endif()
        message(STATUS "ps2EntryRunner: collection app ${PS2X_APP_NAME}: ${PS2X_APP_GAMES}")
    endif()
    message(STATUS "ps2EntryRunner: app front end for ${PS2X_APP_NAME} (${PS2X_APP_ELF})")
    set(PS2X_APP_SOURCES src/collection/app.cpp)
    if(APPLE)
        list(APPEND PS2X_APP_SOURCES src/collection/platform_macos.mm)
        # Objective-C++ through the C++ compiler (no OBJCXX language for the
        # project).
        set_source_files_properties(src/collection/platform_macos.mm PROPERTIES
            LANGUAGE CXX
            SKIP_PRECOMPILE_HEADERS ON
            COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
        target_link_libraries(ps2EntryRunner PRIVATE "-framework AppKit")
    elseif(WIN32)
        list(APPEND PS2X_APP_SOURCES src/collection/platform_windows.cpp)
        target_link_libraries(ps2EntryRunner PRIVATE user32)
        # The executable's icon (resource 1, which SDL also uses for the
        # window): an .ico made by tools/make-windows.ps1 from local files.
        if(PS2X_APP_ICON)
            enable_language(RC)
            file(TO_CMAKE_PATH "${PS2X_APP_ICON}" _ps2x_icon)
            ps2x_write_if_changed("${CMAKE_CURRENT_BINARY_DIR}/ps2x_app_icon.rc" "1 ICON \"${_ps2x_icon}\"\n")
            target_sources(ps2EntryRunner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ps2x_app_icon.rc")
            target_compile_definitions(ps2EntryRunner PRIVATE PS2X_APP_HAS_ICON=1)
        endif()
    elseif(PS2X_IS_ANDROID)
        # (its documents' JNI side: android_documents.cpp, below)
        list(APPEND PS2X_APP_SOURCES src/collection/platform_android.cpp)
    else()
        list(APPEND PS2X_APP_SOURCES src/collection/platform_stub.cpp)
    endif()
    target_sources(ps2EntryRunner PRIVATE ${PS2X_APP_SOURCES})
    set_source_files_properties(${PS2X_APP_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_APP_NAME="${PS2X_APP_NAME}"
        PS2X_APP_GAME_TITLE="$<IF:$<BOOL:${PS2X_APP_GAME_TITLE}>,${PS2X_APP_GAME_TITLE},${PS2X_APP_NAME}>"
        PS2X_APP_ELF="${PS2X_APP_ELF}"
        PS2X_APP_ELF_SHA256="${PS2X_APP_ELF_SHA256}"
        PS2X_APP_SAVE_DIR="${PS2X_APP_SAVE_DIR}"
        PS2X_APP_VERSION="${PS2X_APP_VERSION}"
    )
    if(PS2X_APP_GAMES)
        target_compile_definitions(ps2EntryRunner PRIVATE PS2X_APP_GAMES_HEADER="${_ps2x_games_h}")
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/collection/android_app.cpp src/collection/android_documents.cpp)
    set_source_files_properties(src/collection/android_app.cpp src/collection/android_documents.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON)
    target_link_libraries(ps2EntryRunner PRIVATE android log)
    # The second screen's panel (no platform or game dependency).
    file(GLOB PS2X_PANEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/panel*.cpp")
    target_sources(ps2EntryRunner PRIVATE ${PS2X_PANEL_SOURCES})
    set_source_files_properties(${PS2X_PANEL_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG AND NOT PS2X_FFMPEG_PREFIX)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
