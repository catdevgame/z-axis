cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ZX_ENABLE_RUNNER_UNITY_BUILD "Build zx-runner with CMake unity build" ON)
set(ZX_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for zx-runner")
option(ZX_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for zx-runner" ON)
option(ZX_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(ZX_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(ZX_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" ON)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(ZX_ENABLE_FUNCTION_LOG_TRACKER "Trace every generated function entry/exit to ps2_log.txt (very slow; needs ZX_ENABLE_AGRESSIVE_LOGS)" OFF)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(ZX_SHOW_WINDOWS_CONSOLE "Show a console window for zx-runner on Windows release builds" ON)
option(ZX_ENABLE_DEBUG_UI "Build the runtime debug UI (Mac and Windows)" ON)

if(ZX_ENABLE_SCCACHE)
    find_program(ZX_SCCACHE_PROGRAM sccache)

    if(ZX_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${ZX_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${ZX_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${ZX_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(ZX_IS_ANDROID OFF)

if(ANDROID)
    set(ZX_IS_ANDROID ON)
endif()

# The build's flavour. ON is the dev build: the checkers' machinery (the recorder and the traced originals,
# diffcall, the cycle and tail-call profilers, the guest-call trace, the stack fills, zx-fncheck) is built in, and
# the hooks a checker sets are tested on every call. OFF is the app's flavour (make-app.sh, make-windows.ps1 and the
# Android build): none of it is compiled or linked, and the hooks are constant nulls (ee/abi.h), so a call pays
# nothing for them. What the game computes is the same in both: the frame table, the held table, the register saves
# and EE time stay (they are behaviour under the PS2 clock). The checks run the dev flavour; the app's is proven
# against it by the inter-binary A/B (tooling/checks/flavour_ab.py). Android ships the app alone.
if(ZX_IS_ANDROID)
    set(_zaxis_dev_checks_default OFF)
else()
    set(_zaxis_dev_checks_default ON)
endif()
option(ZX_DEV_CHECKS "Build the checkers' machinery into the runner (OFF: the app's flavour)" ${_zaxis_dev_checks_default})
# One header says which, so every file that includes the framework agrees and the files that do not (the
# recompiled code) compile the same in both flavours (a cache hit in the other's build).
if(ZX_DEV_CHECKS)
    set(_zaxis_dev_checks 1)
else()
    set(_zaxis_dev_checks 0)
endif()
# ZX_GUEST_MAP_CHECK: every access of the readable game (ee::at<T>, game/ee/ee.h) also resolved as before the
# guest map (runtime/guest_map.h), and an address the two resolve differently, or one outside RAM's own view, said
# by the code that made it (ee/guest_map_check.cpp): the evidence of where the special addresses occur. A tool build.
option(ZX_GUEST_MAP_CHECK "Check every readable access against the resolution before the guest map (a tool build)" OFF)
if(ZX_GUEST_MAP_CHECK)
    set(_zaxis_guest_map_check 1)
else()
    set(_zaxis_guest_map_check 0)
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/flavour/build_flavour.h"
    CONTENT "#pragma once\n// Made by runtime/CMakeLists.txt: ZX_DEV_CHECKS 1 the dev build, 0 the app's flavour; ZX_GUEST_MAP_CHECK 1 a\n// build that checks the guest map's every readable access.\n#define ZX_DEV_CHECKS @_zaxis_dev_checks@\n#define ZX_GUEST_MAP_CHECK @_zaxis_guest_map_check@\n"
    @ONLY)
message(STATUS "flavour: ${_zaxis_dev_checks_default} by default, ZX_DEV_CHECKS=${ZX_DEV_CHECKS}")
# ZX_LANE_WATCH_BUILD: every guest load and store of the recompiled code and every readable access (ee::at<T>) also told
# to the lane watch (runtime/lane_watch.h, ZX_LANE_WATCH: who reads or writes a watched word), for wlane.py's proofs
# that a leftover vector lane is read by nothing. A tool build: the macros cost a test per access.
option(ZX_LANE_WATCH_BUILD "Count every access of watched words by the code that made it (a tool build)" OFF)

set(ZX_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

# Window, input, audio and SDL_GPU presentation.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/zx_sdl3.cmake")
# The shipped font's rasteriser (collection/vector_font.cpp).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/zx_stb.cmake")
# zstd: the compressed recordings (src/game/ee/fnrec_file.h, the checkers' machinery, the dev flavour's alone) and
# the GameCube discs' RVZ images the extras' setup reads (src/collection/extras, both flavours).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/zx_zstd.cmake")
# bcdec, the BC1/BC2/BC3/BC7 block decoder of texture packs' DDS pictures (runtime/ps2_texture_dds.h).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/zx_bcdec.cmake")

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
add_library(zx_xxhash INTERFACE)
target_include_directories(zx_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")

if(ZX_ENABLE_DEBUG_UI)
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

    set(ZX_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

    # Dear ImGui with its SDL3 platform and SDL_GPU renderer backends.
    add_library(imgui STATIC
        "${ZX_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${ZX_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${ZX_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${ZX_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
        "${ZX_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        "${ZX_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${ZX_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp"
    )
    target_include_directories(imgui PUBLIC "${ZX_IMGUI_SOURCE_DIR}" "${ZX_IMGUI_SOURCE_DIR}/backends")
    target_link_libraries(imgui PUBLIC zx::sdl3)
endif()

add_library(zx_host_backend INTERFACE)

set(ZX_DEFAULT_BOOT_ELF_DEFINE "")

if(ZX_DEFAULT_BOOT_ELF)
    set(ZX_DEFAULT_BOOT_ELF_DEFINE "${ZX_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" ZX_DEFAULT_BOOT_ELF_DEFINE "${ZX_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" ZX_DEFAULT_BOOT_ELF_DEFINE "${ZX_DEFAULT_BOOT_ELF_DEFINE}")
endif()

target_link_libraries(zx_host_backend INTERFACE zx::sdl3)

if(ZX_IS_ANDROID)
    set(ZX_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(ZX_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(ZX_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${ZX_ENABLE_FFMPEG_DEFAULT})

# libavformat: custom soundtrack files other than WAV (runtime/ps2_audio_file.h).
set(ZX_HAS_AVFORMAT OFF)
set(ZX_FFMPEG_PREFIX "" CACHE PATH
    "Install prefix of a static minimal FFmpeg (tools/build-ffmpeg.sh); empty uses the system's via pkg-config (Windows: a prebuilt shared FFmpeg)")
if(NOT ZX_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32 AND ZX_FFMPEG_PREFIX)
    # The static MSVC build (tools/build-ffmpeg.sh from MSYS2): its .pc files
    # hold MSYS paths, so the libraries are named directly.
    add_library(ffmpeg INTERFACE)
    foreach(_zx_ff_lib IN ITEMS avformat avcodec swscale avutil)
        if(NOT EXISTS "${ZX_FFMPEG_PREFIX}/lib/${_zx_ff_lib}.lib")
            message(FATAL_ERROR "ZX_FFMPEG_PREFIX=${ZX_FFMPEG_PREFIX} has no lib/${_zx_ff_lib}.lib")
        endif()
        target_link_libraries(ffmpeg INTERFACE "${ZX_FFMPEG_PREFIX}/lib/${_zx_ff_lib}.lib")
    endforeach()
    target_include_directories(ffmpeg INTERFACE "${ZX_FFMPEG_PREFIX}/include")
    target_link_libraries(ffmpeg INTERFACE bcrypt ole32 user32)
    set(ZX_HAS_AVFORMAT ON)
    message(STATUS "FFmpeg: static minimal build in ${ZX_FFMPEG_PREFIX}")
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

    set(ZX_HAS_AVFORMAT ON)
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

    set(ZX_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(ZX_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    # MPEG.cpp only uses libavcodec (MPEG-2 video decoder and parser),
    # libswscale (YUV to RGBA) and libavutil.
    find_package(PkgConfig REQUIRED)
    if(ZX_FFMPEG_PREFIX)
        if(NOT EXISTS "${ZX_FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc")
            message(FATAL_ERROR "ZX_FFMPEG_PREFIX=${ZX_FFMPEG_PREFIX} has no lib/pkgconfig/libavcodec.pc")
        endif()
        # Search only the given prefix, so a system FFmpeg cannot be mixed in.
        set(_zx_saved_pc_libdir "$ENV{PKG_CONFIG_LIBDIR}")
        set(_zx_saved_pc_path "$ENV{PKG_CONFIG_PATH}")
        set(ENV{PKG_CONFIG_LIBDIR} "${ZX_FFMPEG_PREFIX}/lib/pkgconfig")
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
    if(ZX_FFMPEG_PREFIX)
        set(ENV{PKG_CONFIG_LIBDIR} "${_zx_saved_pc_libdir}")
        set(ENV{PKG_CONFIG_PATH} "${_zx_saved_pc_path}")
        message(STATUS "FFmpeg: static minimal build in ${ZX_FFMPEG_PREFIX} (${FFMPEG_libavcodec_VERSION})")
    else()
        message(STATUS "FFmpeg: system (pkg-config, libavcodec ${FFMPEG_libavcodec_VERSION})")
    endif()

    # pkg-config reports "-framework X" as two words; CMake de-duplicates link
    # options and would drop the repeated "-framework", so keep each pair whole.
    get_target_property(_zx_ffmpeg_link_opts PkgConfig::FFMPEG INTERFACE_LINK_OPTIONS)
    if(_zx_ffmpeg_link_opts)
        set(_zx_ffmpeg_fixed_opts "")
        set(_zx_ffmpeg_pending_framework OFF)
        foreach(_zx_opt IN LISTS _zx_ffmpeg_link_opts)
            if(_zx_ffmpeg_pending_framework)
                list(APPEND _zx_ffmpeg_fixed_opts "SHELL:-framework ${_zx_opt}")
                set(_zx_ffmpeg_pending_framework OFF)
            elseif(_zx_opt STREQUAL "-framework")
                set(_zx_ffmpeg_pending_framework ON)
            else()
                list(APPEND _zx_ffmpeg_fixed_opts "${_zx_opt}")
            endif()
        endforeach()
        set_target_properties(PkgConfig::FFMPEG PROPERTIES INTERFACE_LINK_OPTIONS "${_zx_ffmpeg_fixed_opts}")
    endif()

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
    if(ZX_IS_ANDROID AND ZX_FFMPEG_PREFIX)
        # (the NDK toolchain's find_library does not look outside its sysroot)
        target_link_directories(ffmpeg INTERFACE "${ZX_FFMPEG_PREFIX}/lib")
    endif()
    if(FFMPEG_FORMAT_FOUND)
        set(ZX_HAS_AVFORMAT ON)
        target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG_FORMAT)
        message(STATUS "FFmpeg: libavformat ${FFMPEG_FORMAT_VERSION} (custom soundtrack in OGG/FLAC/MP3)")
    else()
        message(STATUS "FFmpeg: no libavformat (custom soundtrack in WAV only)")
    endif()
endif()

function(zx_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT ZX_FFMPEG_BIN_DIR OR NOT ZX_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${ZX_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${ZX_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

# The bottom screen (the collection's: include/collection/panel.h): plain C++,
# no platform or game dependency, so the tests and the Mac harness run the
# phone's code. A game's state reaches it as a snapshot (include/runtime/
# panel_snapshot.h, the runtime's contract between a game's capture and the
# panel).
add_library(zx_panel STATIC
    src/collection/panel.cpp
    src/collection/panel_canvas.cpp
    src/collection/panel_file.cpp
    src/collection/panel_view.cpp
)
target_include_directories(zx_panel PUBLIC include)
# The harness: the snapshot registry and each game's capture compiled in
# (Aggressive Inline's by default, --game dm2 / bmx the others').
add_executable(zx-panelshot EXCLUDE_FROM_ALL tools/panel_shot.cpp
    src/lib/panel_snapshot.cpp
    src/game_overrides/aggressive_inline_panel.cpp)
target_link_libraries(zx-panelshot PRIVATE zx_panel)

add_library(zx_runtime STATIC
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
    src/lib/ps2_texture_dds.cpp
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
    # The player's recordings: the list and the playback's boot (Settings > Recordings).
    src/collection/recordings.cpp
    src/collection/playback.cpp
    src/collection/settings.cpp
    src/collection/offered.cpp
    src/collection/disc_check.cpp
    src/collection/game_setup.cpp
    # The search for every game's discs in a folder (the first run's Find Discs).
    src/collection/disc_finder.cpp
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
    src/collection/extras/lzo1x.cpp
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
set(ZX_BUILD_COMMIT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_commit/play_recording_commit.h")
add_custom_target(zaxis_build_commit
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${ZX_BUILD_COMMIT_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildCommit.cmake
    BYPRODUCTS ${ZX_BUILD_COMMIT_HEADER}
    VERBATIM)
add_dependencies(zx_runtime zaxis_build_commit)
if(APPLE)
    # play_recording.cpp asks Foundation for Low Power Mode (through the Objective-C runtime).
    target_link_libraries(zx_runtime PUBLIC objc "-framework Foundation")
endif()
set_source_files_properties(src/lib/play_recording.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/build_commit"
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON)

if(ZX_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(zx_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(ZX_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(zx_runtime PUBLIC
        AGRESSIVE_LOGS=1
    )
endif()

# Per-function enter/exit tracing in generated code writes and flushes a line
# to ps2_log.txt for every guest call, which slows games down by orders of
# magnitude, so it is separate from the other verbose logs.
if(ZX_ENABLE_AGRESSIVE_LOGS AND ZX_ENABLE_FUNCTION_LOG_TRACKER)
    target_compile_definitions(zx_runtime PUBLIC
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(zx_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(zx_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(zx_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

if(ZX_LANE_WATCH_BUILD)
    target_compile_definitions(zx_runtime PUBLIC ZX_LANE_WATCH_BUILD=1)
endif()

target_compile_definitions(zx_runtime PRIVATE
    ZX_HAS_FFMPEG=$<BOOL:${ZX_ENABLE_FFMPEG}>
    ZX_HAS_AVFORMAT=$<BOOL:${ZX_HAS_AVFORMAT}>
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(zx_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

set(ZX_RUNNER_GENERATED_DIR "" CACHE PATH
    "Directory with AI's recompiler output (.cpp + headers): its game module (cmake/game_module.cmake), the runtime's default code")

# Each game's generated code is a game module (cmake/game_module.cmake): linked into the runner or a shared library
# beside it. The runner itself has only the global table (src/runner: empty, the fallback for an ELF no module names).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/game_module.cmake")
file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)
if(ZX_RUNNER_GENERATED_DIR)
    zx_add_game_module(ai "${ZX_RUNNER_GENERATED_DIR}")
endif()

# Per-game override modules (PS2_REGISTER_GAME_OVERRIDE). They are keyed by ELF
# metadata, so linking all of them into every runner is harmless. They are
# compiled into the executable rather than zx_runtime so the static
# registration objects are never dropped by the linker.
file(GLOB ZX_GAME_OVERRIDE_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/game_overrides/*.cpp"
)
list(APPEND RUNNER_SRC_FILES ${ZX_GAME_OVERRIDE_SRC_FILES})

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

if(ZX_IS_ANDROID)
    add_library(zx-runner SHARED
        ${RUNNER_SRC_FILES}
    )

    # SDLActivity loads this library and calls SDL_main (android/build-apk.sh).
    # (-Bsymbolic: FFmpeg's assembly addresses its tables directly)
    target_link_options(zx-runner PRIVATE "-Wl,--undefined=JNI_OnLoad" "-Wl,-Bsymbolic")
    set_source_files_properties("${ROOT_MAIN_CPP}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
else()
    add_executable(zx-runner
        ${RUNNER_SRC_FILES}
    )
endif()

if(ZX_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(zx-runner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${ZX_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(ZX_ENABLE_RUNNER_PCH)
    set(ZX_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    list(APPEND ZX_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(zx-runner PRIVATE ${ZX_RUNNER_PCH_HEADERS})
endif()

# More games in the same runner: recompiler output made with [general]
# namespace = "<id>", one directory each. Every set is an object library of
# its own (its own generated headers on the include path) whose tables
# register by ELF name (ps2_generated in ps2_runtime.h); the runtime picks
# the set of the ELF it boots, else the set of ZX_RUNNER_GENERATED_DIR.
set(ZX_RUNNER_EXTRA_GENERATED_DIRS "" CACHE STRING
    "Semicolon list of namespaced recompiler output directories built into the runner as well")
foreach(_zx_extra_dir IN LISTS ZX_RUNNER_EXTRA_GENERATED_DIRS)
    if(NOT EXISTS "${_zx_extra_dir}/register_functions.cpp")
        message(FATAL_ERROR "ZX_RUNNER_EXTRA_GENERATED_DIRS: ${_zx_extra_dir} has no register_functions.cpp")
    endif()
    # (the module's name is the recompiler's namespace, as its registration names it)
    file(STRINGS "${_zx_extra_dir}/register_functions.cpp" _zx_registrar REGEX "Registrar g_ps2GeneratedCodeRegistrar_[A-Za-z0-9_]+\\{" LIMIT_COUNT 1)
    if(NOT _zx_registrar MATCHES "Registrar g_ps2GeneratedCodeRegistrar_([A-Za-z0-9_]+)\\{")
        message(FATAL_ERROR "ZX_RUNNER_EXTRA_GENERATED_DIRS: ${_zx_extra_dir} is not namespaced recompiler output")
    endif()
    zx_add_game_module(${CMAKE_MATCH_1} "${_zx_extra_dir}")
endforeach()
zx_link_game_modules(zx-runner)

if(ZX_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(zx-runner PRIVATE
        ZX_DEFAULT_BOOT_ELF="${ZX_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(zx-runner PRIVATE /FS)
endif()
# The game thread runs all guest code, so its stack depth follows the guest
# call depth: give threads Linux's 8 MiB (Windows defaults to 1 MiB, and
# std::thread takes the executable's default).
if(WIN32)
    if(MSVC)
        target_link_options(zx-runner PRIVATE "/STACK:8388608")
    elseif(MINGW)
        target_link_options(zx-runner PRIVATE "-Wl,--stack,8388608")
    endif()
endif()

# VU arithmetic rounds the product and the sum separately (as PCSX2 does);
# do not let the compiler fuse a*b+c into one FMA, which also made the
# results depend on the host architecture. clang-cl (MSVC in CMake) takes
# the clang options after /clang:; cl does not contract under /fp:precise.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(ZX_FP_NO_CONTRACT "/clang:-ffp-contract=off")
    set(ZX_FP_ROUNDING_MATH "/clang:-frounding-math")
elseif(MSVC)
    set(ZX_FP_NO_CONTRACT "/fp:precise")
    set(ZX_FP_ROUNDING_MATH "/fp:strict")
else()
    set(ZX_FP_NO_CONTRACT "-ffp-contract=off")
    set(ZX_FP_ROUNDING_MATH "-frounding-math")
endif()
set_source_files_properties(
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_interpreter.cpp
    src/lib/vu/ps2_vu1_decoder.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    PROPERTIES COMPILE_OPTIONS "${ZX_FP_NO_CONTRACT}")
# The VU1 ahead-of-time generator's key: a hash of what it writes from (the generator, the VU1 decoder and core under
# src/lib/vu, include/runtime/ps2_vu1*.h). The build's own VU1 code (ZX_VU1_AOT_DIR) and a game module's made at
# first run (collection/game_code.cpp) are made by the generator with this key.
file(GLOB _zx_vu1_generator CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/vu1_aot.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*" "${CMAKE_CURRENT_SOURCE_DIR}/include/runtime/ps2_vu1*.h")
list(SORT _zx_vu1_generator)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_zx_vu1_generator})
set(_zx_vu1_generator_text "")
foreach(_zx_file IN LISTS _zx_vu1_generator)
    file(SHA256 "${_zx_file}" _zx_hash)
    get_filename_component(_zx_name "${_zx_file}" NAME)
    string(APPEND _zx_vu1_generator_text "${_zx_name} ${_zx_hash}\n")
endforeach()
string(SHA256 ZX_VU1_GENERATOR_KEY "${_zx_vu1_generator_text}")
string(SUBSTRING "${ZX_VU1_GENERATOR_KEY}" 0 16 ZX_VU1_GENERATOR_KEY)
set_property(SOURCE src/lib/vu/ps2_vu1_aot_generator.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             "ZX_VU1_GENERATOR_KEY=\"${ZX_VU1_GENERATOR_KEY}\"")
# Estate's level picture and icon (build time): the same bytes on every host.
set_source_files_properties(src/lib/ps2_zaxis_estate_icon.cpp PROPERTIES COMPILE_OPTIONS "${ZX_FP_NO_CONTRACT}")
# The map converter computes as the tooling's Python reference does (each
# product and sum rounded on its own): the same files on every host.
file(GLOB ZX_LEVELS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/levels/*.cpp")
set_source_files_properties(${ZX_LEVELS_SOURCES} PROPERTIES COMPILE_OPTIONS "${ZX_FP_NO_CONTRACT}")
# The extras' converters too (collection/extras): the bytes the tooling's Python tools write, on every host.
file(GLOB ZX_EXTRAS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/extras/*.cpp")
set_source_files_properties(${ZX_EXTRAS_SOURCES} PROPERTIES COMPILE_OPTIONS "${ZX_FP_NO_CONTRACT}")

target_include_directories(zx_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
    ${CMAKE_CURRENT_BINARY_DIR}/flavour
)

target_link_libraries(zx_runtime PUBLIC zx_host_backend ffmpeg ps2_iop)
# (the game file system merges mods' single archive entries into the .ZAL the game reads: runtime/ps2_game_files.h)
target_link_libraries(zx_runtime PRIVATE zx::zal)
# stb_truetype's one header, for collection/vector_font.cpp only.
set_source_files_properties(src/collection/vector_font.cpp PROPERTIES INCLUDE_DIRECTORIES "${ZX_STB_DIR}" SKIP_UNITY_BUILD_INCLUSION ON)
# (the recorder and the checkers read and write compressed recordings: ee/fnrec_file.cpp, in every game's instance)
# (and the extras' setup reads RVZ images: collection/extras/rvz_image.cpp, in both flavours)
if(ZX_DEV_CHECKS)
    target_link_libraries(zx_runtime PUBLIC zx::zstd)
else()
    target_link_libraries(zx_runtime PRIVATE zx::zstd)
endif()
target_link_libraries(zx_runtime PRIVATE zx_xxhash)
# bcdec's one header, for lib/ps2_texture_dds.cpp only (its implementation is compiled there).
set_source_files_properties(src/lib/ps2_texture_dds.cpp PROPERTIES INCLUDE_DIRECTORIES "${ZX_BCDEC_DIR}" SKIP_UNITY_BUILD_INCLUSION ON)

# zx_write_if_changed(PATH TEXT): PATH holds TEXT, rewritten only when it differs: a header or resource
# made at configure time keeps its time when the configure changes nothing, so its users are not compiled
# and the runner is not linked again (each rewrite cost make-windows' rerun of an unchanged tree a ThinLTO
# link: about 6 minutes on a Windows build).
function(zx_write_if_changed path text)
    if(EXISTS "${path}")
        file(READ "${path}" _zx_old)
        if(_zx_old STREQUAL text)
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
zx_write_if_changed("${CMAKE_CURRENT_BINARY_DIR}/miniz/miniz_export.h"
    "#pragma once\n// Made by runtime/CMakeLists.txt: miniz built as a static library.\n#define MINIZ_EXPORT\n#define MINIZ_NO_EXPORT\n")
add_library(zx_miniz STATIC
    "${miniz_SOURCE_DIR}/miniz.c"
    "${miniz_SOURCE_DIR}/miniz_zip.c"
    "${miniz_SOURCE_DIR}/miniz_tinfl.c"
    "${miniz_SOURCE_DIR}/miniz_tdef.c")
target_include_directories(zx_miniz PUBLIC "${miniz_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miniz")
target_compile_definitions(zx_miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(zx_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zx_runtime PRIVATE zx_miniz)

# Hardware GS renderer on SDL_GPU (prototype, src/lib/gs/GPU_BACKEND.md):
# a separate library linked only by what uses it.
if(ZX_GS_GPU AND TARGET zx::sdl3)
    add_library(zx_gs_gpu STATIC src/lib/gs/gs_gpu_backend.cpp)
    target_link_libraries(zx_gs_gpu PUBLIC zx_runtime zx::sdl3)
    target_compile_definitions(zx_gs_gpu PRIVATE ZX_HAS_GS_GPU=1)
    # Direct3D 12: the HLSL shaders compiled to DXIL at build time with the
    # Windows SDK's DXC (Metal compiles the MSL source at run time).
    if(WIN32)
        set(_zx_pf86 "ProgramFiles(x86)")
        file(GLOB _zx_sdk_bins LIST_DIRECTORIES true "$ENV{${_zx_pf86}}/Windows Kits/10/bin/10.*")
        list(SORT _zx_sdk_bins COMPARE NATURAL ORDER DESCENDING)
        set(_zx_dxc_hints "")
        if(DEFINED ENV{WindowsSdkVerBinPath})
            list(APPEND _zx_dxc_hints "$ENV{WindowsSdkVerBinPath}/x64")
        endif()
        foreach(_zx_bin IN LISTS _zx_sdk_bins)
            list(APPEND _zx_dxc_hints "${_zx_bin}/x64")
        endforeach()
        find_program(ZX_DXC dxc HINTS ${_zx_dxc_hints})
        if(ZX_DXC)
            set(_zx_dxil_dir "${CMAKE_CURRENT_BINARY_DIR}/gs_gpu_dxil")
            set(_zx_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gs/gs_gpu_shaders.hlsl")
            set(_zx_dxil_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileGsShadersDxil.cmake")
            add_custom_command(
                OUTPUT "${_zx_dxil_dir}/gs_gpu_shaders_dxil.h"
                COMMAND "${CMAKE_COMMAND}" "-DDXC=${ZX_DXC}" "-DSRC=${_zx_hlsl}"
                        "-DOUT=${_zx_dxil_dir}/gs_gpu_shaders_dxil.h" -P "${_zx_dxil_script}"
                DEPENDS "${_zx_hlsl}" "${_zx_dxil_script}"
                COMMENT "Compiling the GS GPU shaders to DXIL"
                VERBATIM)
            target_sources(zx_gs_gpu PRIVATE "${_zx_dxil_dir}/gs_gpu_shaders_dxil.h")
            target_include_directories(zx_gs_gpu PRIVATE "${_zx_dxil_dir}")
            target_compile_definitions(zx_gs_gpu PRIVATE ZX_GS_GPU_DXIL=1)
            message(STATUS "GS GPU backend: Direct3D 12 shaders compiled with ${ZX_DXC}")
        else()
            message(WARNING "No dxc (Windows SDK) found: the GPU renderer has no Direct3D 12 shaders")
        endif()
    endif()
    # Android (Vulkan): the same HLSL to SPIR-V with the NDK's glslc.
    if(ZX_IS_ANDROID)
        file(GLOB _zx_glslc_hints "${ANDROID_NDK}/shader-tools/*")
        find_program(ZX_GLSLC glslc HINTS ${_zx_glslc_hints} REQUIRED)
        set(_zx_spirv_dir "${CMAKE_CURRENT_BINARY_DIR}/gs_gpu_spirv")
        set(_zx_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gs/gs_gpu_shaders.hlsl")
        set(_zx_dxil_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileGsShadersDxil.cmake")
        add_custom_command(
            OUTPUT "${_zx_spirv_dir}/gs_gpu_shaders_spirv.h"
            COMMAND "${CMAKE_COMMAND}" "-DGLSLC=${ZX_GLSLC}" "-DSRC=${_zx_hlsl}"
                    "-DOUT=${_zx_spirv_dir}/gs_gpu_shaders_spirv.h" -P "${_zx_dxil_script}"
            DEPENDS "${_zx_hlsl}" "${_zx_dxil_script}"
            COMMENT "Compiling the GS GPU shaders to SPIR-V"
            VERBATIM)
        target_sources(zx_gs_gpu PRIVATE "${_zx_spirv_dir}/gs_gpu_shaders_spirv.h")
        target_include_directories(zx_gs_gpu PRIVATE "${_zx_spirv_dir}")
        target_compile_definitions(zx_gs_gpu PRIVATE ZX_GS_GPU_SPIRV=1)
    endif()
endif()

# The readable game and its checker, zx-fncheck (src/game).
include("${CMAKE_CURRENT_SOURCE_DIR}/src/game/game.cmake")

# The game overrides' symbols (tools/override_symbols.py): an override names its game's functions and data
# (sym::dm2::g_cameraSettings, sym::bmx::..., sym:: for Aggressive Inline's) from the readable code's table and
# the bind tables, in <build>/symbols/overrides/sym/game_overrides/<source>.h, written only when their text
# changes.
set(ZX_OVERRIDE_SYMBOLS_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/override_symbols.py")
set(ZX_OVERRIDE_SYMBOLS_DIR "${ZX_SYMBOLS_DIR}/overrides")
set(_zx_override_symbols_args headers --out "${ZX_OVERRIDE_SYMBOLS_DIR}")
set(_zx_override_symbols_depends ${ZX_GAME_OVERRIDE_SRC_FILES} "${ZX_OVERRIDE_SYMBOLS_TOOL}"
    "${ZX_SYMBOLS_TOOL}" "${ZX_SYMBOLS_TABLE}")
file(GLOB ZX_GAME_OVERRIDE_HEADER_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game_overrides/*.h")
list(APPEND _zx_override_symbols_depends ${ZX_GAME_OVERRIDE_HEADER_FILES})
# (and the extras' converters, which name the data tables they read from a game's ELF the same way)
list(APPEND _zx_override_symbols_depends ${ZX_EXTRAS_SOURCES})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ZX_OVERRIDE_SYMBOLS_TOOL}")
if(ZX_BIND_DIR)
    list(APPEND _zx_override_symbols_args --bind-dir "${ZX_BIND_DIR}")
    foreach(_zx_override_game dm2 bmx)
        foreach(_zx_override_input "${ZX_BIND_DIR}/${_zx_override_game}.tsv"
                                     "${ZX_GAME_DIR}/${_zx_override_game}/bound.tsv")
            if(EXISTS "${_zx_override_input}")
                list(APPEND _zx_override_symbols_depends "${_zx_override_input}")
            endif()
        endforeach()
    endforeach()
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${ZX_OVERRIDE_SYMBOLS_TOOL}" ${_zx_override_symbols_args}
                RESULT_VARIABLE _zx_override_symbols_result)
if(NOT _zx_override_symbols_result EQUAL 0)
    message(FATAL_ERROR "the game overrides' symbol headers failed")
endif()
file(GLOB_RECURSE _zx_override_symbols_headers "${ZX_OVERRIDE_SYMBOLS_DIR}/sym/*.h")
add_custom_command(
    OUTPUT "${ZX_SYMBOLS_DIR}/overrides.stamp"
    BYPRODUCTS ${_zx_override_symbols_headers}
    COMMAND "${Python3_EXECUTABLE}" -B "${ZX_OVERRIDE_SYMBOLS_TOOL}" ${_zx_override_symbols_args}
    COMMAND "${Python3_EXECUTABLE}" -B "${ZX_OVERRIDE_SYMBOLS_TOOL}" lint
    COMMAND "${CMAKE_COMMAND}" -E touch "${ZX_SYMBOLS_DIR}/overrides.stamp"
    DEPENDS ${_zx_override_symbols_depends}
    COMMENT "the game overrides' symbol headers, their literal lint"
    VERBATIM)
add_custom_target(zx_override_symbols DEPENDS "${ZX_SYMBOLS_DIR}/overrides.stamp")
add_dependencies(zx-runner zx_override_symbols)
target_include_directories(zx-runner PRIVATE "${ZX_OVERRIDE_SYMBOLS_DIR}")
add_dependencies(zx_runtime zx_override_symbols)
target_include_directories(zx_runtime PRIVATE "${ZX_OVERRIDE_SYMBOLS_DIR}")
# The panel harness reads DM2's RAM with DM2's own capture (--game dm2), by its symbols.
target_sources(zx-panelshot PRIVATE src/game_overrides/dave_mirra_panel.cpp)
# And BMX XXX's RAM with BMX XXX's (--game bmx).
target_sources(zx-panelshot PRIVATE src/game_overrides/bmx_xxx_panel.cpp)
add_dependencies(zx-panelshot zx_override_symbols)
target_include_directories(zx-panelshot PRIVATE "${ZX_OVERRIDE_SYMBOLS_DIR}")

# Offline GS trace replay (traces come from ZX_GS_DUMP, see gs_record.h).
if(NOT ZX_IS_ANDROID)
    add_executable(zx-gsreplay tools/gs_replay.cpp)
    target_link_libraries(zx-gsreplay PRIVATE zx_runtime)
    if(TARGET zx_gs_gpu)
        target_link_libraries(zx-gsreplay PRIVATE zx_gs_gpu)
        target_compile_definitions(zx-gsreplay PRIVATE ZX_HAS_GS_GPU=1)
    endif()
    # Offline VU1 corpus check (corpora come from ZX_VU1_RECORD). It loads game modules (--module: their VU1 code)
    # as the runner does, so it has the runner's (empty) global function table.
    add_executable(zx-vu1check tools/vu1_check.cpp src/runner/register_functions.cpp)
    target_link_libraries(zx-vu1check PRIVATE zx_runtime)
    # Ahead-of-time compiled VU1 microprograms (see src/lib/vu/ps2_vu1_aot.h).
    add_executable(zx-vu1aot tools/vu1_aot.cpp)
    target_link_libraries(zx-vu1aot PRIVATE zx_runtime)
    # Aggressive Inline's Zoo and Ape made at build time from the user's Xbox
    # files (runtime/ps2_zaxis_zoo.h, ps2_zaxis_ape.h; tools/make-app.sh and
    # make-windows.ps1 run it).
    add_executable(zx-xbox-convert tools/xbox_convert.cpp)
    target_link_libraries(zx-xbox-convert PRIVATE zx_runtime)
    # TUT/Estate, the GameCube disc's earlier Tutorial, made at build time
    # from the files tools/gc_disc.py extracts (runtime/ps2_zaxis_estate.h).
    add_executable(zx-gc-convert tools/gc_convert.cpp)
    target_link_libraries(zx-gc-convert PRIVATE zx_runtime)
    # The collection's extras (collection/extras) on the command line: the
    # Xbox and GameCube disc images read, for the checks against the
    # tooling's Python tools (the reference).
    add_executable(zx-extras tools/extras_tool.cpp src/collection/extras/xbox_model_maker.cpp
        # (the engine's Xbox model converter, as zx-native-assets builds it)
        src/game/zge/native/texture_library.cpp src/game/zge/native/native_textures.cpp
        src/game/zge/native/disc_files.cpp src/game/zge/native/geo_file.cpp src/game/zge/native/geo_meshes.cpp
        src/game/zge/native/xbox_models.cpp)
    target_link_libraries(zx-extras PRIVATE zx_runtime)
    target_include_directories(zx-extras PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
    # The collection's map converter on the command line (collection/levels):
    # its files written to a folder and its records read back, for the checks
    # against the tooling's Python converters (the reference).
    add_executable(zx-map-convert tools/map_convert.cpp)
    target_link_libraries(zx-map-convert PRIVATE zx_runtime)
endif()

# ZX_VU1_AOT_DIR, ZX_VU1_AOT_EXTRA_DIRS: folders of zx-vu1aot output, each with the micro memory images
# it was made from (<hash>.vu1code), built into the runner and zx-vu1check. They register themselves by micro
# memory hash, so they go into the executables (a static library member nothing refers to would be dropped).
# The code follows this tree's generator: the generator key is a hash of what zx-vu1aot writes from (its
# source, the VU1 decoder under src/lib/vu and include/runtime/ps2_vu1*.h) and of the folder's images. A folder
# whose vu1_aot.key holds the key is built as it is; else the build makes this tree's code from the folder's
# images with its own zx-vu1aot, into the build dir (vu1-aot/<key>: generated output never goes beside the
# input, which may sit in a source checkout; cmake/vu1_aot_generate.cmake makes it aside and renames it into
# place, never written over). A build that cannot run its own zx-vu1aot (a cross build: Android) builds the
# generator alone for the build machine (tools/vu1_aot_host), or takes ZX_VU1_AOT_TOOL, a host's zx-vu1aot.
set(ZX_VU1_AOT_DIR "" CACHE PATH "Directory of zx-vu1aot output (ahead-of-time compiled VU1 microprograms)")
# More games: their AOT sources too (they register by micro memory hash).
set(ZX_VU1_AOT_EXTRA_DIRS "" CACHE STRING "Semicolon list of more zx-vu1aot output directories")
set(ZX_VU1_AOT_TOOL "" CACHE FILEPATH "A host zx-vu1aot for a build that cannot run its own (cross builds)")
if(ZX_VU1_AOT_DIR OR ZX_VU1_AOT_EXTRA_DIRS)
    # (the generator's text, above: the key adds the folder's images)
    set(_zx_vu1_generate "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vu1_aot_generate.cmake")
    set(ZX_VU1_AOT_SOURCES "")
    set(_zx_vu1_keys "")
    foreach(_zx_aot_dir IN ITEMS ${ZX_VU1_AOT_DIR} ${ZX_VU1_AOT_EXTRA_DIRS})
        string(REGEX REPLACE "/+$" "" _zx_aot_dir "${_zx_aot_dir}")
        file(GLOB _zx_images CONFIGURE_DEPENDS "${_zx_aot_dir}/*.vu1code")
        list(SORT _zx_images)
        set(_zx_key_text "${_zx_vu1_generator_text}")
        foreach(_zx_image IN LISTS _zx_images)
            file(SHA256 "${_zx_image}" _zx_hash)
            string(APPEND _zx_key_text "image ${_zx_hash}\n")
        endforeach()
        string(SHA256 _zx_key "${_zx_key_text}")
        string(SUBSTRING "${_zx_key}" 0 16 _zx_key)
        set(_zx_out "${CMAKE_BINARY_DIR}/vu1-aot/${_zx_key}")
        if(_zx_key IN_LIST _zx_vu1_keys)
            continue() # (another folder with the same images: the same code, built once)
        endif()
        list(APPEND _zx_vu1_keys "${_zx_key}")
        set(_zx_stamp "")
        if(EXISTS "${_zx_aot_dir}/vu1_aot.key")
            file(READ "${_zx_aot_dir}/vu1_aot.key" _zx_stamp)
            string(STRIP "${_zx_stamp}" _zx_stamp)
        endif()
        if(_zx_stamp STREQUAL _zx_key)
            file(GLOB _zx_aot_sources CONFIGURE_DEPENDS "${_zx_aot_dir}/vu1_aot_*.cpp")
            list(APPEND ZX_VU1_AOT_SOURCES ${_zx_aot_sources})
            message(STATUS "VU1 ahead-of-time code: ${_zx_aot_dir} (generator key ${_zx_key})")
            continue()
        endif()
        if(NOT _zx_images)
            message(FATAL_ERROR "${_zx_aot_dir}: no micro memory image (*.vu1code) to make its VU1 code from with "
                                "this tree's zx-vu1aot (generator key ${_zx_key})")
        endif()
        set(_zx_tool_depends "")
        if(ZX_VU1_AOT_TOOL)
            set(_zx_tool "${ZX_VU1_AOT_TOOL}")
        elseif(TARGET zx-vu1aot)
            set(_zx_tool "$<TARGET_FILE:zx-vu1aot>")
        else()
            # A cross build (Android) cannot run its own: the generator alone built for the build machine
            # (tools/vu1_aot_host, the host's compiler), once for every folder.
            if(NOT TARGET zx_vu1_aot_host)
                include(ExternalProject)
                set(_zx_host_dir "${CMAKE_BINARY_DIR}/vu1-aot-host")
                set(_zx_host_tool "${_zx_host_dir}/build/zx-vu1aot${CMAKE_HOST_EXECUTABLE_SUFFIX}")
                ExternalProject_Add(zx_vu1_aot_host
                    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/vu1_aot_host"
                    BINARY_DIR "${_zx_host_dir}/build"
                    PREFIX "${_zx_host_dir}"
                    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DZX_VU1_GENERATOR_KEY=${ZX_VU1_GENERATOR_KEY}"
                    BUILD_ALWAYS ON
                    INSTALL_COMMAND ""
                    BUILD_BYPRODUCTS "${_zx_host_tool}")
            endif()
            set(_zx_tool "${_zx_host_tool}")
            set(_zx_tool_depends zx_vu1_aot_host)
            message(STATUS "VU1 ahead-of-time code: the generator built for the build machine (tools/vu1_aot_host)")
        endif()
        # (zx-vu1aot writes a header, the dispatcher and 32 chunk sources per image: tools/vu1_aot.cpp)
        set(_zx_outputs "${_zx_out}/vu1_aot.key")
        set(_zx_aot_sources "")
        foreach(_zx_image IN LISTS _zx_images)
            get_filename_component(_zx_stem "${_zx_image}" NAME_WE)
            list(APPEND _zx_outputs "${_zx_out}/vu1_aot_${_zx_stem}.h")
            list(APPEND _zx_aot_sources "${_zx_out}/vu1_aot_${_zx_stem}.cpp")
            foreach(_zx_chunk RANGE 0 31)
                if(_zx_chunk LESS 10)
                    set(_zx_chunk "0${_zx_chunk}")
                endif()
                list(APPEND _zx_aot_sources "${_zx_out}/vu1_aot_${_zx_stem}_c${_zx_chunk}.cpp")
            endforeach()
        endforeach()
        string(REPLACE ";" "|" _zx_image_list "${_zx_images}") # (one argument: the script splits it)
        add_custom_command(
            OUTPUT ${_zx_outputs} ${_zx_aot_sources}
            COMMAND "${CMAKE_COMMAND}" "-DTOOL=${_zx_tool}" "-DOUT=${_zx_out}" "-DKEY=${_zx_key}"
                    "-DIMAGES=${_zx_image_list}" -P "${_zx_vu1_generate}"
            DEPENDS ${_zx_images} "${_zx_vu1_generate}" ${_zx_tool_depends}
            COMMENT "VU1 ahead-of-time code: ${_zx_out}"
            VERBATIM)
        list(APPEND ZX_VU1_AOT_SOURCES ${_zx_aot_sources})
        message(STATUS "VU1 ahead-of-time code: ${_zx_out} (made by the build: generator key ${_zx_key})")
    endforeach()
    set_source_files_properties(${ZX_VU1_AOT_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${ZX_FP_NO_CONTRACT};${ZX_FP_ROUNDING_MATH}"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu")
    target_sources(zx-runner PRIVATE ${ZX_VU1_AOT_SOURCES})
    if(TARGET zx-vu1check)
        target_sources(zx-vu1check PRIVATE ${ZX_VU1_AOT_SOURCES})
    endif()
endif()
target_link_libraries(zx-runner
    PRIVATE
    zx_runtime
)
# Opt-in GPU renderer in the runner: ZX_GS_BACKEND=gpu.
if(TARGET zx_gs_gpu)
    target_sources(zx-runner PRIVATE src/lib/gs/gs_gpu_register.cpp)
    set_source_files_properties(src/lib/gs/gs_gpu_register.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_link_libraries(zx-runner PRIVATE zx_gs_gpu)
endif()

if(ZX_ENABLE_DEBUG_UI AND NOT ZX_IS_ANDROID)
    target_sources(zx-runner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(zx-runner PRIVATE imgui)
    target_compile_definitions(zx-runner PRIVATE ZX_ENABLE_DEBUG_UI=1)
endif()

# The game overrides install their panel captures (aggressive_inline_panel.cpp,
# dave_mirra_panel.cpp, bmx_xxx_panel.cpp); on Android the panel's sources are
# the runner's own (below).
if(NOT ZX_IS_ANDROID)
    target_link_libraries(zx-runner PRIVATE zx_panel)
endif()

zx_stage_ffmpeg_runtime_dlls(zx-runner)

# Single-game app front end (tools/make-app.sh in the game repo sets these):
# started without an ELF argument, the runner checks the game files bundled
# with it (Resources/disc, extracted from the disc image when the app is
# built), runs the game from them and keeps settings, saves and logs in the
# per-user folders.
# Empty ZX_APP_NAME: the developer runner only.
set(ZX_APP_NAME "" CACHE STRING "Build the runner as this single-game app (window title, folder names)")
set(ZX_APP_GAME_TITLE "" CACHE STRING "Game title shown in the app's messages, e.g. Aggressive Inline (USA)")
set(ZX_APP_ELF "" CACHE STRING "Boot ELF the app expects in its game files, e.g. SLUS_203.27")
set(ZX_APP_ELF_SHA256 "" CACHE STRING "SHA-256 of the ELF the generated code was recompiled from (empty: not checked)")
set(ZX_APP_SAVE_DIR "" CACHE STRING "The game's memory card save folder, for importing a PCSX2 save")
set(ZX_APP_VERSION "${PROJECT_VERSION}" CACHE STRING "App version shown in the log")
set(ZX_APP_ICON "" CACHE FILEPATH "Windows: .ico for the app's executable and window")

# Several games in one app (a collection named ZX_APP_NAME): one entry per
# game, "id|name|title|ELF|ELF SHA-256|PCSX2 save folder" (empty fields
# allowed). The app then picks the game (--game <id> or a picker) and keeps
# each game's files in <support>/games/<id> (src/collection/app.cpp).
set(ZX_APP_GAMES "" CACHE STRING "Semicolon list of the games of a collection app (id|name|title|elf|sha256|savedir)")

if(ZX_APP_NAME)
    if(NOT ZX_APP_ELF AND NOT ZX_APP_GAMES)
        message(FATAL_ERROR "ZX_APP_NAME needs ZX_APP_ELF (or ZX_APP_GAMES)")
    endif()
    if(ZX_APP_GAMES)
        set(_zx_games_h "${CMAKE_CURRENT_BINARY_DIR}/zx_app_games.h")
        set(_zx_games_text "// Generated by CMake from ZX_APP_GAMES.\nnamespace zx_app\n{\n    constexpr GameProfile kGames[] = {\n")
        foreach(_zx_game IN LISTS ZX_APP_GAMES)
            string(REPLACE "|" ";" _zx_fields "${_zx_game}")
            list(LENGTH _zx_fields _zx_field_count)
            if(NOT _zx_field_count EQUAL 6)
                message(FATAL_ERROR "ZX_APP_GAMES entry needs 6 fields: ${_zx_game}")
            endif()
            list(GET _zx_fields 0 _id)
            list(GET _zx_fields 1 _name)
            list(GET _zx_fields 2 _title)
            list(GET _zx_fields 3 _elf)
            list(GET _zx_fields 4 _sha)
            list(GET _zx_fields 5 _save)
            string(APPEND _zx_games_text "        {\"${_id}\", \"${_name}\", \"${_title}\", \"${_elf}\", \"${_sha}\", \"${_save}\"},\n")
        endforeach()
        string(APPEND _zx_games_text "    };\n}\n")
        zx_write_if_changed("${_zx_games_h}" "${_zx_games_text}")
        if(NOT ZX_APP_ELF)
            list(GET ZX_APP_GAMES 0 _zx_first)
            string(REPLACE "|" ";" _zx_first "${_zx_first}")
            list(GET _zx_first 3 ZX_APP_ELF)
        endif()
        message(STATUS "zx-runner: collection app ${ZX_APP_NAME}: ${ZX_APP_GAMES}")
    endif()
    message(STATUS "zx-runner: app front end for ${ZX_APP_NAME} (${ZX_APP_ELF})")
    set(ZX_APP_SOURCES src/collection/app.cpp)
    if(APPLE)
        list(APPEND ZX_APP_SOURCES src/collection/platform_macos.mm)
        # Objective-C++ through the C++ compiler (no OBJCXX language for the
        # project).
        set_source_files_properties(src/collection/platform_macos.mm PROPERTIES
            LANGUAGE CXX
            SKIP_PRECOMPILE_HEADERS ON
            COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
        target_link_libraries(zx-runner PRIVATE "-framework AppKit")
    elseif(WIN32)
        list(APPEND ZX_APP_SOURCES src/collection/platform_windows.cpp)
        target_link_libraries(zx-runner PRIVATE user32)
        # The executable's icon (resource 1, which SDL also uses for the
        # window): an .ico made by tools/make-windows.ps1 from local files.
        if(ZX_APP_ICON)
            enable_language(RC)
            file(TO_CMAKE_PATH "${ZX_APP_ICON}" _zx_icon)
            zx_write_if_changed("${CMAKE_CURRENT_BINARY_DIR}/zx_app_icon.rc" "1 ICON \"${_zx_icon}\"\n")
            target_sources(zx-runner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/zx_app_icon.rc")
            target_compile_definitions(zx-runner PRIVATE ZX_APP_HAS_ICON=1)
        endif()
    elseif(ZX_IS_ANDROID)
        # (its documents' JNI side: android_documents.cpp, below)
        list(APPEND ZX_APP_SOURCES src/collection/platform_android.cpp)
    else()
        list(APPEND ZX_APP_SOURCES src/collection/platform_stub.cpp)
    endif()
    target_sources(zx-runner PRIVATE ${ZX_APP_SOURCES})
    set_source_files_properties(${ZX_APP_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_compile_definitions(zx-runner PRIVATE
        ZX_APP_NAME="${ZX_APP_NAME}"
        ZX_APP_GAME_TITLE="$<IF:$<BOOL:${ZX_APP_GAME_TITLE}>,${ZX_APP_GAME_TITLE},${ZX_APP_NAME}>"
        ZX_APP_ELF="${ZX_APP_ELF}"
        ZX_APP_ELF_SHA256="${ZX_APP_ELF_SHA256}"
        ZX_APP_SAVE_DIR="${ZX_APP_SAVE_DIR}"
        ZX_APP_VERSION="${ZX_APP_VERSION}"
    )
    if(ZX_APP_GAMES)
        target_compile_definitions(zx-runner PRIVATE ZX_APP_GAMES_HEADER="${_zx_games_h}")
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(zx_runtime)
    EnableFastReleaseMode(zx-runner)

    if(WIN32 AND NOT ZX_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(zx-runner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(zx-runner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

if(ZX_IS_ANDROID)
    target_sources(zx-runner PRIVATE src/collection/android_app.cpp src/collection/android_documents.cpp
                                     src/collection/android_assets.cpp)
    set_source_files_properties(src/collection/android_app.cpp src/collection/android_documents.cpp
                                src/collection/android_assets.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON)
    target_link_libraries(zx-runner PRIVATE android log)
    # The second screen's panel (no platform or game dependency).
    file(GLOB ZX_PANEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/collection/panel*.cpp")
    target_sources(zx-runner PRIVATE ${ZX_PANEL_SOURCES})
    set_source_files_properties(${ZX_PANEL_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endif()

install(TARGETS zx_runtime zx-runner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND ZX_ENABLE_FFMPEG AND NOT ZX_FFMPEG_PREFIX)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
