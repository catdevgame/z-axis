# The games' .ZAL archives, zx_zal / zx::zal (ZDIR archives and zfile libraries: zal/zal_archive.h), read and written
# byte for byte as the discs have them: the game file system merges mods' single entries into them with it. zal
# (tools/modding/zal_tool.cpp: list | unpack | repack | verify, docs/MODS.md "Archives") is its command line.
if(NOT DEFINED ZX_ROOT)
    get_filename_component(ZX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
add_library(zx_zal STATIC
    zal_archive.cpp
    zal_folder.cpp
)
target_compile_features(zx_zal PUBLIC cxx_std_20)
# (its header is named from engine/: "zal/zal_archive.h")
target_include_directories(zx_zal PUBLIC "${ZX_ROOT}/engine")
set_target_properties(zx_zal PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(zx::zal ALIAS zx_zal)

# zal list | unpack | repack | verify: an archive as a folder of its entries and back, for modders (in <build>/iop,
# beside zx-disc).
add_executable(zal "${ZX_ROOT}/tools/modding/zal_tool.cpp")
target_link_libraries(zal PRIVATE zx_zal)
set_target_properties(zal PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/iop")
