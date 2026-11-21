# zstd, pinned: the compressed recordings (ps2/ee/fnrec_file.h) and the GameCube RVZ disc images the
# setup reads extras from (collection/extras/rvz_image.cpp). The release tarball is checked by SHA-256 and
# its library sources (lib/common, lib/compress, lib/decompress) are built as they ship, static, without zstd's own
# CMake project (which enables ASM, programs and tests): no legacy formats, no worker threads, and the portable C
# Huffman decoder (ZSTD_DISABLE_ASM), so every host builds the same code. The Python tools read the same library
# (tools/checks/fnrec_io.py: Python 3.14's compression.zstd or the zstandard module, both zstd 1.5.7).
#
#   include("${CMAKE_SOURCE_DIR}/third_party/cmake/zx_zstd.cmake")
#   target_link_libraries(<target> PRIVATE zx::zstd)
#
# FETCHCONTENT_SOURCE_DIR_ZSTD=<dir> uses an already extracted copy (no network).
include_guard(GLOBAL)

set(ZX_ZSTD_VERSION "1.5.7")
set(ZX_ZSTD_SHA256 "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3")

if(NOT TARGET zx_zstd)
    enable_language(C)
    include(FetchContent)
    FetchContent_Declare(
        zstd
        URL "https://github.com/facebook/zstd/releases/download/v${ZX_ZSTD_VERSION}/zstd-${ZX_ZSTD_VERSION}.tar.gz"
        URL_HASH "SHA256=${ZX_ZSTD_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(zstd) # (populated only: its root has no CMakeLists.txt)
    file(GLOB _zx_zstd_sources CONFIGURE_DEPENDS
        "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c" "${zstd_SOURCE_DIR}/lib/decompress/*.c")
    add_library(zx_zstd STATIC ${_zx_zstd_sources})
    target_include_directories(zx_zstd SYSTEM PUBLIC "${zstd_SOURCE_DIR}/lib")
    target_compile_definitions(zx_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=ZSTD_)
    set_target_properties(zx_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    add_library(zx::zstd ALIAS zx_zstd)
endif()
