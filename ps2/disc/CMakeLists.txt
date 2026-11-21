# The disc image reader, zx_disc / zx::disc: ISO9660 over the image a file holds (ISO, BIN/CUE, CHD, CSO, ZSO:
# disc/disc_image_file.h). The app sets a game up from the player's disc with it, the IOP's CDVD reads through it,
# and zx-disc (tools/setup/disc_tool.cpp: list | extract | to-iso) is its command line.
if(NOT DEFINED ZX_ROOT)
    get_filename_component(ZX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
include("${ZX_ROOT}/third_party/cmake/zx_chdr.cmake")
include("${ZX_ROOT}/third_party/cmake/zx_lz4.cmake")
add_library(zx_disc STATIC
    iso9660.cpp
    cue_sheet.cpp
    disc_image_file.cpp
    disc_image_chd.cpp
    disc_image_cso.cpp
)
target_compile_features(zx_disc PUBLIC cxx_std_20)
# (its headers are named from ps2/: "disc/iso9660.h")
target_include_directories(zx_disc PUBLIC "${ZX_ROOT}/ps2")
target_link_libraries(zx_disc PRIVATE zx::chdr zx::lz4)
# (disc_image_cso.cpp inflates with libchdr's miniz, under its names)
zx_chdr_miniz_names(zx_disc)
set_target_properties(zx_disc PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(zx::disc ALIAS zx_disc)

# zx-disc list | extract | to-iso: a disc image's files, whatever its format. (In <build>/iop, where
# tools/setup/recompile.sh and tools/modding/disc_image.py find it.)
add_executable(zx-disc "${ZX_ROOT}/tools/setup/disc_tool.cpp")
target_link_libraries(zx-disc PRIVATE zx_disc)
set_target_properties(zx-disc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/iop")
