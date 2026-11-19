cmake_minimum_required(VERSION 3.21)

project(ps2xIOP LANGUAGES CXX)

option(PS2X_IOP_BUILD_TESTS "Build ps2xIOP emulator smoke tests" OFF)
# The disc reader and its tool alone (the recompiler's build, PS2X_BUILD_RUNTIME OFF: tooling/recompile.sh takes each
# game's ELF from its disc image with ps2x_disc).
option(PS2X_IOP_DISC_ONLY "Build only the disc image reader and ps2x_disc" OFF)

# The disc image reader: ISO9660 over the image a file holds (ISO, BIN, CHD, CSO, ZSO: ps2x/iop/disc_image_file.h).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2x_chdr.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2x_lz4.cmake")
add_library(ps2_disc STATIC
    src/iso9660.cpp
    src/cue_sheet.cpp
    src/disc_image_file.cpp
    src/disc_image_chd.cpp
    src/disc_image_cso.cpp
)
target_compile_features(ps2_disc PUBLIC cxx_std_20)
target_include_directories(ps2_disc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(ps2_disc PRIVATE ps2x::chdr ps2x::lz4)
# (disc_image_cso.cpp inflates with libchdr's miniz, under its names)
ps2x_chdr_miniz_names(ps2_disc)
set_target_properties(ps2_disc PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(ps2x::disc ALIAS ps2_disc)

# ps2x_disc list | extract | to-iso: a disc image's files, whatever its format (tools/ps2x_disc.cpp).
add_executable(ps2x_disc tools/ps2x_disc.cpp)
target_link_libraries(ps2x_disc PRIVATE ps2_disc)

if(PS2X_IOP_DISC_ONLY)
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

target_link_libraries(ps2_iop PUBLIC ps2_disc)

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
