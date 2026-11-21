# SDL3, pinned: the host layer (window, input, audio) and SDL_GPU (Metal on
# macOS, D3D12/Vulkan elsewhere). Self-contained so every target that needs
# SDL3 (the runtime, the GS replay tool, a GPU renderer) includes this same
# file and gets the same copy:
#
#   include("${CMAKE_SOURCE_DIR}/third_party/cmake/zx_sdl3.cmake")
#   target_link_libraries(<target> PRIVATE zx::sdl3)
#
# Built static (no dylib to bundle) with the project's CMAKE_OSX_DEPLOYMENT_TARGET.
# The release tarball is checked by SHA-256. FETCHCONTENT_SOURCE_DIR_SDL3=<dir>
# uses an already extracted copy (no network).
include_guard(GLOBAL)

set(ZX_SDL3_VERSION "3.4.18")
set(ZX_SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")

function(zx_fetch_sdl3)
    include(FetchContent)
    # Normal variables in this function's scope only: SDL3's options read
    # them (CMP0077) without touching the cache, so another SDL in the same
    # build (ps2xStudio's SDL2) keeps its own settings.
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_INSTALL OFF)
    set(SDL_UNINSTALL OFF)
    set(SDL_CAMERA OFF)
    set(SDL_DISABLE_INSTALL ON)
    FetchContent_Declare(
        sdl3
        URL "https://github.com/libsdl-org/SDL/releases/download/release-${ZX_SDL3_VERSION}/SDL3-${ZX_SDL3_VERSION}.tar.gz"
        URL_HASH "SHA256=${ZX_SDL3_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sdl3)
endfunction()

if(NOT TARGET SDL3::SDL3-static)
    # SDL3's Cocoa/Metal sources are Objective-C. enable_language only works
    # at file scope (not under the function above), and only once per build
    # tree when other subprojects need it too: the top-level CMakeLists
    # should enable OBJC itself (the top-level CMakeLists.txt does).
    if(APPLE AND NOT CMAKE_OBJC_COMPILER_LOADED)
        enable_language(OBJC)
    endif()
    zx_fetch_sdl3()
endif()

if(NOT TARGET zx::sdl3)
    add_library(zx_sdl3 INTERFACE)
    target_link_libraries(zx_sdl3 INTERFACE SDL3::SDL3-static)
    add_library(zx::sdl3 ALIAS zx_sdl3)
endif()
