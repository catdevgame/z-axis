cmake_minimum_required(VERSION 3.21)

project(ps2xIOP LANGUAGES CXX)

option(PS2X_IOP_BUILD_TESTS "Build ps2xIOP emulator smoke tests" OFF)
# The disc reader and its tools alone (the recompiler's build, ZX_BUILD_RUNTIME OFF: tooling/recompile.sh takes each
# game's ELF from its disc image with zx-disc).
option(ZX_IOP_DISC_ONLY "Build only the disc image reader, zx-disc and zal" OFF)

# The disc image reader: ISO9660 over the image a file holds (ISO, BIN, CHD, CSO, ZSO: zx/disc/disc_image_file.h).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/zx_chdr.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/zx_lz4.cmake")
add_library(zx_disc STATIC
    src/iso9660.cpp
    src/cue_sheet.cpp
    src/disc_image_file.cpp
    src/disc_image_chd.cpp
    src/disc_image_cso.cpp
)
target_compile_features(zx_disc PUBLIC cxx_std_20)
target_include_directories(zx_disc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(zx_disc PRIVATE zx::chdr zx::lz4)
# (disc_image_cso.cpp inflates with libchdr's miniz, under its names)
zx_chdr_miniz_names(zx_disc)
set_target_properties(zx_disc PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(zx::disc ALIAS zx_disc)

# zx-disc list | extract | to-iso: a disc image's files, whatever its format (tools/disc_tool.cpp).
add_executable(zx-disc tools/disc_tool.cpp)
target_link_libraries(zx-disc PRIVATE zx_disc)

# The games' .ZAL archives (ZDIR archives and zfile libraries: include/zx/zal_archive.h), read and written
# byte for byte as the discs have them: the game file system merges mods' single entries into them with it.
add_library(zx_zal STATIC
    src/zal_archive.cpp
    src/zal_folder.cpp
)
target_compile_features(zx_zal PUBLIC cxx_std_20)
target_include_directories(zx_zal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(zx_zal PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(zx::zal ALIAS zx_zal)

# zal list | unpack | repack | verify: an archive as a folder of its entries and back, for modders
# (tools/zal_tool.cpp, docs/MODS.md "Archives").
add_executable(zal tools/zal_tool.cpp)
target_link_libraries(zal PRIVATE zx_zal)

if(ZX_IOP_DISC_ONLY)
    return()
endif()

add_library(ps2_iop STATIC
    src/ps2_path.cpp
    src/iop_module_manager.cpp
    src/iop_subsystem.cpp
    src/emulator/iop_emulator.cpp
    src/emulator/core/iop_cpu.cpp
    src/emulator/core/iop_kernel.cpp
    src/emulator/core/iop_memory.cpp
    src/emulator/services/iop_module_loader.cpp
    src/emulator/services/iop_rpc.cpp
    src/emulator/imports/iop_cdvd.cpp
    src/emulator/imports/iop_heaplib.cpp
    src/emulator/imports/iop_imports.cpp
    src/emulator/imports/iop_intrman.cpp
    src/emulator/imports/iop_ioman.cpp
    src/emulator/imports/iop_loadcore.cpp
    src/emulator/imports/iop_stdio.cpp
    src/emulator/imports/iop_sysclib.cpp
    src/emulator/imports/iop_sysmem.cpp
    src/emulator/imports/iop_timrman.cpp
    src/emulator/imports/iop_vblank.cpp
    src/emulator/spu2/spu2.cpp
    src/emulator/spu2/spu2_adsr.cpp
    src/emulator/spu2/spu2_dma.cpp
    src/emulator/spu2/spu2_mixer.cpp
    src/emulator/spu2/spu2_reverb.cpp
    src/emulator/spu2/spu2_sys.cpp
    src/modules/dbcman.cpp
    src/modules/libsd.cpp
    src/modules/mcserv.cpp
)

target_compile_features(ps2_iop PUBLIC cxx_std_20)

target_include_directories(ps2_iop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ps2_iop PUBLIC zx_disc)

add_library(ps2x::iop ALIAS ps2_iop)

if(PS2X_IOP_BUILD_TESTS)
    enable_testing()
    add_executable(ps2_iop_emulator_tests tests/iop_emulator_tests.cpp)
    target_link_libraries(ps2_iop_emulator_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_emulator_tests COMMAND ps2_iop_emulator_tests)

    add_executable(ps2_iop_import_tests tests/iop_import_tests.cpp)
    target_link_libraries(ps2_iop_import_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_tests COMMAND ps2_iop_import_tests)

    add_executable(ps2_iop_compatibility_tests tests/iop_compatibility_tests.cpp)
    target_link_libraries(ps2_iop_compatibility_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_compatibility_tests COMMAND ps2_iop_compatibility_tests)

    add_executable(ps2_iop_import_version_tests tests/iop_import_version_tests.cpp)
    target_link_libraries(ps2_iop_import_version_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_version_tests COMMAND ps2_iop_import_version_tests)

    add_executable(ps2_iop_spu2_tests tests/iop_spu2_tests.cpp)
    target_link_libraries(ps2_iop_spu2_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_spu2_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_spu2_tests COMMAND ps2_iop_spu2_tests)

endif()

install(TARGETS ps2_iop
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
