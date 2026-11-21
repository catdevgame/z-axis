# stb_truetype (Sean Barrett's, public domain or MIT, v1.28 at nothings/stb
# 9600522), the rasteriser of the font the app ships (collection/
# vector_font.cpp): the one header, unchanged, fetched once into the build
# folder and checked against its hash.
set(ZX_STB_COMMIT "9600522de53973336fa033a289858a4354cbb04c")
set(ZX_STB_TRUETYPE_SHA256 "428cfe93520f2aed5e413a220b0db5bba5b17e2ebe5a64e4a8efa9eeeb67ec5f")
set(ZX_STB_DIR "${CMAKE_BINARY_DIR}/_deps/stb-${ZX_STB_COMMIT}")
if(NOT EXISTS "${ZX_STB_DIR}/stb_truetype.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/${ZX_STB_COMMIT}/stb_truetype.h"
        "${ZX_STB_DIR}/stb_truetype.h"
        EXPECTED_HASH "SHA256=${ZX_STB_TRUETYPE_SHA256}"
        TLS_VERIFY ON
        STATUS ZX_STB_STATUS)
    list(GET ZX_STB_STATUS 0 ZX_STB_CODE)
    if(NOT ZX_STB_CODE EQUAL 0)
        file(REMOVE "${ZX_STB_DIR}/stb_truetype.h")
        message(FATAL_ERROR "stb_truetype.h could not be fetched: ${ZX_STB_STATUS}")
    endif()
endif()
