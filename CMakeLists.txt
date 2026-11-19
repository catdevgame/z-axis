cmake_minimum_required(VERSION 3.21)

project(ZAxisCollection)

# SDL3 (runtime/cmake/ps2x_sdl3.cmake) has Objective-C sources; a language
# enabled in two sibling directories breaks the second, so enable it once here.
if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ccache, when installed, so build directories in other worktrees reuse the
# objects of identical compiles (the generated game code above all). Set
# base_dir and hash_dir=false in ccache's own config for hits across paths.
option(PS2X_CCACHE "Compile through ccache when it is installed" ON)
if(PS2X_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(PS2X_CCACHE_PROGRAM ccache)
    if(PS2X_CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_CCACHE_PROGRAM}")
        set(CMAKE_OBJC_COMPILER_LAUNCHER "${PS2X_CCACHE_PROGRAM}")
    endif()
endif()

option(PS2X_BUILD_RUNTIME "Build the runtime and the games" ON)
option(PS2X_BUILD_TEST "Build the unit tests (ps2x_tests)" ON)
# The recompiler that makes each game's generated code (and its own tests) is a
# tool: it lives in tooling/recompiler, built here unless PS2X_BUILD_RECOMPILER
# is OFF (the app build and the workers' builds never run it).
option(PS2X_BUILD_RECOMPILER "Build tooling/recompiler (when it is there)" ON)

if(ANDROID)
    message(STATUS "Android target detected, building runtime only")
    set(PS2X_BUILD_TEST OFF)
    set(PS2X_BUILD_RECOMPILER OFF)
endif()

set(PS2X_IS_ARM_TARGET OFF)
set(PS2X_IS_AARCH64_TARGET OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(PS2X_IS_ARM_TARGET ON)
    set(PS2X_IS_AARCH64_TARGET ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm|^ARM")
    set(PS2X_IS_ARM_TARGET ON)
endif()

if(PS2X_IS_ARM_TARGET)
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

    if(PS2X_IS_AARCH64_TARGET AND APPLE)
        # macOS ARM64 already uses optimal defaults
        message(STATUS "macOS ARM64 - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET AND MSVC)
        # Windows ARM64 - MSVC already uses optimal defaults
        message(STATUS "Windows ARM64 (MSVC) - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET)
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

set(PS2X_X64_LEVEL "x86-64-v2" CACHE STRING "x86-64 target: x86-64-v2 (SSE4.2) or x86-64-v3 (AVX2, FMA, BMI2)")
set(PS2X_IS_X64_TARGET OFF)
if(NOT PS2X_IS_ARM_TARGET AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    set(PS2X_IS_X64_TARGET ON)
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(/clang:-march=${PS2X_X64_LEVEL})
    elseif(NOT MSVC)
        add_compile_options(-march=${PS2X_X64_LEVEL})
    endif()
endif()

# SDL_GPU GS backend (runtime/src/lib/gs/GPU_BACKEND.md). SDL3 itself
# comes with the host layer (runtime/cmake/ps2x_sdl3.cmake).
option(PS2X_GS_GPU "Build the SDL_GPU GS backend" ON)

# The build dirs keep their old names (<build>/ps2xRuntime/ps2EntryRunner,
# <build>/ps2xTest/ps2x_tests): the tools and the remote workers find the
# binaries there, and they change with the binaries' own names.
if(PS2X_BUILD_RUNTIME)
    add_subdirectory("iop" "${CMAKE_BINARY_DIR}/ps2xIOP")
    add_subdirectory("runtime" "${CMAKE_BINARY_DIR}/ps2xRuntime")
elseif(PS2X_BUILD_RECOMPILER)
    # The disc reader's tool beside the recompiler (tooling/recompile.sh: each game's ELF from its disc image).
    set(PS2X_IOP_DISC_ONLY ON)
    add_subdirectory("iop" "${CMAKE_BINARY_DIR}/ps2xIOP")
endif()

# (The unit tests drive the checkers' hooks: the dev flavour's alone, runtime/CMakeLists.txt ZAXIS_DEV_CHECKS.)
if(PS2X_BUILD_TEST AND PS2X_BUILD_RUNTIME AND NOT ZAXIS_DEV_CHECKS)
    message(STATUS "tests: not built in the app's flavour (ZAXIS_DEV_CHECKS OFF)")
elseif(PS2X_BUILD_TEST)
    add_subdirectory("tests" "${CMAKE_BINARY_DIR}/ps2xTest")
endif()

if(PS2X_BUILD_RECOMPILER AND EXISTS "${CMAKE_SOURCE_DIR}/tooling/recompiler/CMakeLists.txt")
    add_subdirectory("tooling/recompiler" "${CMAKE_BINARY_DIR}/recompiler")
endif()
