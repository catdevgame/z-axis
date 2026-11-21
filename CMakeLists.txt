cmake_minimum_required(VERSION 3.21)

# The collection's version, x.y.z: the one file every build and package reads it from (VERSION.txt; not VERSION, which a
# case-insensitive disk finds for the C++ header <version> on the root include path), the app's menu and
# log (ZX_APP_VERSION's default), the Mac app's Info.plist, the Windows .exe's version resource and the Android
# manifest (platform/mac/make-app.sh, platform/windows/make-windows.ps1, platform/android/build-apk.sh).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" ZX_VERSION LIMIT_COUNT 1)
if(NOT ZX_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION.txt holds \"${ZX_VERSION}\", not a version x.y.z")
endif()
project(ZAxisCollection VERSION ${ZX_VERSION})

# SDL3 (third_party/cmake/zx_sdl3.cmake) has Objective-C sources; a language
# enabled in two sibling directories breaks the second, so enable it once here.
if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The repository's root: what a folder's CMake names outside its own folder is named from here.
set(ZX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
# The readable code's seven folders, in key order (games/game.cmake, tools/checks/zlayout.py READABLE), and the
# include roots its keys are found under: #include "zge/..." (engine/), "ee/..." (ps2/), "aggressive/..." (games/).
set(ZX_ENGINE_DIR "${ZX_ROOT}/engine")
set(ZX_PS2_DIR "${ZX_ROOT}/ps2")
set(ZX_GAMES_DIR "${ZX_ROOT}/games")
set(ZX_READABLE_DIRS "${ZX_GAMES_DIR}/aggressive" "${ZX_GAMES_DIR}/bmx" "${ZX_GAMES_DIR}/dm2" "${ZX_PS2_DIR}/ee"
    "${ZX_ENGINE_DIR}/host" "${ZX_PS2_DIR}/sce" "${ZX_ENGINE_DIR}/zge")
set(ZX_READABLE_ROOTS "${ZX_ENGINE_DIR}" "${ZX_PS2_DIR}" "${ZX_GAMES_DIR}")

# ccache, when installed, so build directories in other worktrees reuse the
# objects of identical compiles (the generated game code above all). Set
# base_dir and hash_dir=false in ccache's own config for hits across paths.
option(ZX_CCACHE "Compile through ccache when it is installed" ON)
if(ZX_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(ZX_CCACHE_PROGRAM ccache)
    if(ZX_CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${ZX_CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${ZX_CCACHE_PROGRAM}")
        set(CMAKE_OBJC_COMPILER_LAUNCHER "${ZX_CCACHE_PROGRAM}")
    endif()
endif()

option(ZX_BUILD_RUNTIME "Build the runtime and the games" ON)
option(ZX_BUILD_TEST "Build the unit tests (zx-tests)" ON)
# The recompiler that makes each game's generated code (and its own tests) is a
# tool: it lives in recompiler, built here unless ZX_BUILD_RECOMPILER
# is OFF (the app build and the workers' builds never run it).
option(ZX_BUILD_RECOMPILER "Build recompiler (when it is there)" ON)

# On Android the recompiler is built only for the APK that makes each game's code on the device (platform/android/README.md).
option(ZX_ANDROID_RECOMPILER "Android: build the recompiler too (the APK's, for the game code made on the device)" OFF)
if(ANDROID)
    message(STATUS "Android target detected, building runtime only")
    set(ZX_BUILD_TEST OFF)
    if(NOT ZX_ANDROID_RECOMPILER)
        set(ZX_BUILD_RECOMPILER OFF)
    endif()
endif()

set(ZX_IS_ARM_TARGET OFF)
set(ZX_IS_AARCH64_TARGET OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(ZX_IS_ARM_TARGET ON)
    set(ZX_IS_AARCH64_TARGET ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm|^ARM")
    set(ZX_IS_ARM_TARGET ON)
endif()

if(ZX_IS_ARM_TARGET)
    message(STATUS "ARM target detected, fetching sse2neon")

    include(FetchContent)
    FetchContent_Declare(
        sse2neon
        # v1.9.1: the release's source archive, checked by SHA-256 (no git clone).
        URL https://github.com/DLTcollab/sse2neon/archive/refs/tags/v1.9.1.tar.gz
        URL_HASH SHA256=6b70e7cb8c5ce4641002b85deaafe97efdf9ade9b49884edeaf678b35f0e132f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sse2neon)

    include_directories(${sse2neon_SOURCE_DIR})
    add_compile_definitions(USE_SSE2NEON)

    if(ZX_IS_AARCH64_TARGET AND APPLE)
        # macOS ARM64 already uses optimal defaults
        message(STATUS "macOS ARM64 - using default compiler flags")
    elseif(ZX_IS_AARCH64_TARGET AND MSVC)
        # Windows ARM64 - MSVC already uses optimal defaults
        message(STATUS "Windows ARM64 (MSVC) - using default compiler flags")
    elseif(ZX_IS_AARCH64_TARGET)
        # Linux ARM64 - add NEON flags
        message(STATUS "Non-Apple ARM64 - adding NEON compiler flags")
        add_compile_options(-march=armv8-a+fp+simd)

        # Try to enable crypto and CRC extensions if supported
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=armv8-a+fp+simd+crypto+crc" COMPILER_SUPPORTS_CRYPTO_CRC)
        if(COMPILER_SUPPORTS_CRYPTO_CRC)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+fp+simd+crypto+crc")
            message(STATUS "Crypto and CRC extensions enabled")
        else()
            add_compile_options(-march=armv8-a+fp+simd)
        endif()
    endif()
endif()

# x86-64: the runtime's SIMD code uses SSE4.1 and SSSE3 intrinsics natively
# (sse2neon is only for ARM). Clang and GCC need those target features
# enabled (x86-64-v2: SSE4.2, POPCNT); MSVC's cl allows the intrinsics
# without. clang-cl counts as MSVC in CMake and takes clang options after
# /clang:.
# clang-cl follows MSVC and turns type-based alias analysis off; clang and
# GCC elsewhere (the macOS build included) have it on, and the VU1 and
# recompiled code need it to keep guest state in registers (about 3x on the
# VU1 worker).
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(/clang:-fstrict-aliasing)
endif()

set(ZX_X64_LEVEL "x86-64-v2" CACHE STRING "x86-64 target: x86-64-v2 (SSE4.2) or x86-64-v3 (AVX2, FMA, BMI2)")
set(ZX_IS_X64_TARGET OFF)
if(NOT ZX_IS_ARM_TARGET AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    set(ZX_IS_X64_TARGET ON)
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(/clang:-march=${ZX_X64_LEVEL})
    elseif(NOT MSVC)
        add_compile_options(-march=${ZX_X64_LEVEL})
    endif()
endif()

# SDL_GPU GS backend (docs/GPU_BACKEND.md). SDL3 itself
# comes with the host layer (third_party/cmake/zx_sdl3.cmake).
option(ZX_GS_GPU "Build the SDL_GPU GS backend" ON)

# The build dirs keep their names (<build>/runtime/zx-runner, <build>/tests/zx-tests, <build>/iop/zx-disc): the tools
# and the remote workers find the binaries there, whatever folder their sources are in. The build is one scope,
# ps2/CMakeLists.txt's: the PS2 machine (ps2/), the engine (engine/), the games (games/game.cmake), the collection
# and the platforms, and the tools built from them.
# Our libraries, each in its folder: the disc image reader (ps2/disc), the games' .ZAL archives (engine/zal) and the
# SPU2 (ps2/spu2). Every build has the first two (seconds): the recompiler's (ZX_BUILD_RUNTIME OFF) reads each game's
# ELF from its disc image with zx-disc (tools/setup/recompile.sh), and with everything else OFF a build is zx-disc and
# zal alone (docs/MODS.md "Archives").
add_subdirectory("ps2/disc" "${CMAKE_BINARY_DIR}/disc")
add_subdirectory("engine/zal" "${CMAKE_BINARY_DIR}/zal")
if(ZX_BUILD_RUNTIME)
    add_subdirectory("ps2/spu2" "${CMAKE_BINARY_DIR}/spu2")
    add_subdirectory("iop" "${CMAKE_BINARY_DIR}/iop")
    add_subdirectory("ps2" "${CMAKE_BINARY_DIR}/runtime")
endif()

# (The unit tests drive the checkers' hooks: the dev flavour's alone, ps2/CMakeLists.txt ZX_DEV_CHECKS.)
if(ZX_BUILD_TEST AND ZX_BUILD_RUNTIME AND NOT ZX_DEV_CHECKS)
    message(STATUS "tests: not built in the app's flavour (ZX_DEV_CHECKS OFF)")
elseif(ZX_BUILD_TEST)
    add_subdirectory("tests" "${CMAKE_BINARY_DIR}/tests")
endif()

if(ZX_BUILD_RECOMPILER AND EXISTS "${CMAKE_SOURCE_DIR}/recompiler/CMakeLists.txt")
    add_subdirectory("recompiler" "${CMAKE_BINARY_DIR}/recompiler")
endif()
