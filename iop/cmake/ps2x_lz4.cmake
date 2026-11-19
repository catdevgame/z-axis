# LZ4 1.10.0, pinned: ZSO disc images and CSO v2's LZ4 blocks (ps2x/iop/disc_image_file.h). BSD-2-Clause (lib/).
# The release tarball is checked by SHA-256 and its block format alone (lib/lz4.c) is built, static, without LZ4's
# own CMake project (which builds its programs).
#
#   include("${CMAKE_SOURCE_DIR}/iop/cmake/ps2x_lz4.cmake")
#   target_link_libraries(<target> PRIVATE ps2x::lz4)
#
# FETCHCONTENT_SOURCE_DIR_LZ4=<dir> uses an already extracted copy (no network).
include_guard(GLOBAL)

set(PS2X_LZ4_VERSION "1.10.0")
set(PS2X_LZ4_SHA256 "537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b")

if(NOT TARGET ps2x_lz4)
    enable_language(C)
    include(FetchContent)
    FetchContent_Declare(
        lz4
        URL "https://github.com/lz4/lz4/releases/download/v${PS2X_LZ4_VERSION}/lz4-${PS2X_LZ4_VERSION}.tar.gz"
        URL_HASH "SHA256=${PS2X_LZ4_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR not-added
    )
    FetchContent_MakeAvailable(lz4) # (fetched, not added)
    add_library(ps2x_lz4 STATIC "${lz4_SOURCE_DIR}/lib/lz4.c")
    target_include_directories(ps2x_lz4 SYSTEM PUBLIC "${lz4_SOURCE_DIR}/lib")
    set_target_properties(ps2x_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    add_library(ps2x::lz4 ALIAS ps2x_lz4)
endif()
