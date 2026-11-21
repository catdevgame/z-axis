# The SPU2, zx_spu2: the PS2 sound processor the IOP emulator (iop/) drives, ported from PCSX2's SPU2 (README.md).
# spu2/spu2.h is its only interface.
if(NOT DEFINED ZX_ROOT)
    get_filename_component(ZX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
add_library(zx_spu2 STATIC
    spu2.cpp
    spu2_adsr.cpp
    spu2_dma.cpp
    spu2_mixer.cpp
    spu2_reverb.cpp
    spu2_sys.cpp
)
target_compile_features(zx_spu2 PUBLIC cxx_std_20)
# (named from ps2/: "spu2/spu2.h")
target_include_directories(zx_spu2 PUBLIC "${ZX_ROOT}/ps2")
set_target_properties(zx_spu2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
