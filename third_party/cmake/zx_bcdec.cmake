# bcdec v0.985 (Sergii Kudlai's, MIT or the Unlicense, iOrange/bcdec at
# 80859ed), the block decoder of texture packs' DDS pictures (BC1, BC2,
# BC3, BC7: runtime/texture_dds.h) for the CPU renderer and for a GPU
# without that format. It decodes as Direct3D's block compression is
# specified, as the GPU PCSX2 draws them on samples them (PCSX2's own CPU
# decoder, common/TextureDecompress, which it uses only to measure alpha,
# makes BC1's transparent texels opaque black and reads BC2's colours in
# BC1's three-colour mode). The one header, unchanged, fetched once into the
# build folder and checked against its hash.
set(ZX_BCDEC_COMMIT "80859ed3b7afb1c527a2a99d70c61457bea72d0c")
set(ZX_BCDEC_SHA256 "134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189")
set(ZX_BCDEC_DIR "${CMAKE_BINARY_DIR}/_deps/bcdec-${ZX_BCDEC_COMMIT}")
if(NOT EXISTS "${ZX_BCDEC_DIR}/bcdec.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/iOrange/bcdec/${ZX_BCDEC_COMMIT}/bcdec.h"
        "${ZX_BCDEC_DIR}/bcdec.h"
        EXPECTED_HASH "SHA256=${ZX_BCDEC_SHA256}"
        TLS_VERIFY ON
        STATUS ZX_BCDEC_STATUS)
    list(GET ZX_BCDEC_STATUS 0 ZX_BCDEC_CODE)
    if(NOT ZX_BCDEC_CODE EQUAL 0)
        file(REMOVE "${ZX_BCDEC_DIR}/bcdec.h")
        message(FATAL_ERROR "bcdec.h could not be fetched: ${ZX_BCDEC_STATUS}")
    endif()
endif()
