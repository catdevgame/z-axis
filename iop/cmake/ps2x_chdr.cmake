# libchdr, pinned: CHD disc images (MAME's compressed format, ps2x/iop/disc_image_file.h). BSD-3-Clause.
# github.com/rtissera/libchdr tags no releases after v0.3.0 (2020); this is its commit of 2026-10-11, 47d7941
# ("Reject crafted CHD maps and codec payloads that crash the reader"), the archive checked by SHA-256. Its sources
# and the libraries it bundles in deps/ (the LZMA SDK 26.02, public domain; miniz 3.1.2, MIT) are built as they ship,
# static, without its own CMake project (which builds a shared library and its tests): the portable C decoders (no
# assembly, as ps2x_zstd), so every host builds the same code, and its own defaults (chdconfig.h: raw data sectors,
# subcode, each hunk's CRC checked). zstd is the project's own pinned copy (runtime/cmake/ps2x_zstd.cmake), the
# 1.5.7 libchdr bundles. Its miniz is a second copy beside the runtime's (3.0.2, the zip mods'), so its symbols take
# the prefix chdr_ (libchdr does the same for the ESP32 ROMs' miniz: cmake/EspRomMinizWorkaround.cmake), in its
# sources and in each of ours that calls it (ps2x_chdr_miniz_names).
#
#   include("${CMAKE_SOURCE_DIR}/iop/cmake/ps2x_chdr.cmake")
#   target_link_libraries(<target> PRIVATE ps2x::chdr)
#
# FETCHCONTENT_SOURCE_DIR_LIBCHDR=<dir> uses an already extracted copy (no network).
include_guard(GLOBAL)
include(FetchContent)

set(PS2X_LIBCHDR_COMMIT "47d794151b2b64f6aba60304e6480f4f5a2bc24b")
set(PS2X_LIBCHDR_SHA256 "ee1131e748a2c7d7559b7054540ebe49637433901f92071560df52e369f7c7af")

# miniz 3.1.2's external symbols as libchdr builds it (MINIZ_NO_ARCHIVE_APIS, _DEFLATE_APIS, _STDIO, _TIME).
set(PS2X_CHDR_MINIZ_SYMBOLS
    miniz_def_alloc_func miniz_def_free_func miniz_def_realloc_func mz_adler32 mz_crc32 mz_error mz_free mz_inflate
    mz_inflateEnd mz_inflateInit mz_inflateInit2 mz_inflateReset mz_uncompress mz_uncompress2 mz_version
    tinfl_decompress tinfl_decompress_mem_to_callback tinfl_decompress_mem_to_heap tinfl_decompress_mem_to_mem
    tinfl_decompressor_alloc tinfl_decompressor_free)
set(PS2X_CHDR_MINIZ_DEFINES MINIZ_NO_ARCHIVE_APIS MINIZ_NO_DEFLATE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
foreach(_ps2x_symbol IN LISTS PS2X_CHDR_MINIZ_SYMBOLS)
    list(APPEND PS2X_CHDR_MINIZ_DEFINES "${_ps2x_symbol}=chdr_${_ps2x_symbol}")
endforeach()

# Gives a target's sources libchdr's miniz under its own names.
function(ps2x_chdr_miniz_names target)
    FetchContent_GetProperties(libchdr)
    target_compile_definitions(${target} PRIVATE ${PS2X_CHDR_MINIZ_DEFINES})
    target_include_directories(${target} SYSTEM PRIVATE "${libchdr_SOURCE_DIR}/deps/miniz-3.1.2")
endfunction()

if(NOT TARGET ps2x_chdr)
    enable_language(C)
    include("${CMAKE_CURRENT_LIST_DIR}/../../runtime/cmake/ps2x_zstd.cmake")
    FetchContent_Declare(
        libchdr
        URL "https://github.com/rtissera/libchdr/archive/${PS2X_LIBCHDR_COMMIT}.tar.gz"
        URL_HASH "SHA256=${PS2X_LIBCHDR_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR not-added
    )
    FetchContent_MakeAvailable(libchdr) # (fetched, not added)
    set(_ps2x_chdr "${libchdr_SOURCE_DIR}")
    add_library(ps2x_chdr STATIC
        "${_ps2x_chdr}/src/libchdr_bitstream.c"
        "${_ps2x_chdr}/src/libchdr_cdrom.c"
        "${_ps2x_chdr}/src/libchdr_chd.c"
        "${_ps2x_chdr}/src/libchdr_codec_avhuff.c"
        "${_ps2x_chdr}/src/libchdr_codec_cdfl.c"
        "${_ps2x_chdr}/src/libchdr_codec_cdlz.c"
        "${_ps2x_chdr}/src/libchdr_codec_cdzl.c"
        "${_ps2x_chdr}/src/libchdr_codec_cdzs.c"
        "${_ps2x_chdr}/src/libchdr_codec_flac.c"
        "${_ps2x_chdr}/src/libchdr_codec_huff.c"
        "${_ps2x_chdr}/src/libchdr_codec_lzma.c"
        "${_ps2x_chdr}/src/libchdr_codec_zlib.c"
        "${_ps2x_chdr}/src/libchdr_codec_zstd.c"
        "${_ps2x_chdr}/src/libchdr_flac.c"
        "${_ps2x_chdr}/src/libchdr_huffman.c"
        "${_ps2x_chdr}/deps/lzma-26.02/src/LzmaDec.c"
        "${_ps2x_chdr}/deps/miniz-3.1.2/miniz.c")
    target_include_directories(ps2x_chdr SYSTEM PUBLIC "${_ps2x_chdr}/include")
    # (zstd through the project's copy: <zstd.h>, CHDR_SYSTEM_ZSTD)
    target_compile_definitions(ps2x_chdr PRIVATE CHDR_SYSTEM_ZSTD)
    ps2x_chdr_miniz_names(ps2x_chdr)
    if(MSVC)
        target_compile_definitions(ps2x_chdr PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(ps2x_chdr PRIVATE ps2x::zstd)
    set_target_properties(ps2x_chdr PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    add_library(ps2x::chdr ALIAS ps2x_chdr)
endif()
