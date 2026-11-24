# Compiles the GS GPU backend's HLSL (ps2/gs/gs_gpu_shaders.hlsl) to DXIL
# for Direct3D 12 with DXC and writes one header of byte arrays
# (gs_gpu_shaders_dxil.h). Run by the build: cmake -DDXC=<dxc> -DSRC=<hlsl>
# -DOUT=<header> -P CompileGsShadersDxil.cmake
#
# With -DGLSLC=<glslc> instead of -DDXC: SPIR-V for Vulkan (Android), the same
# header but for the namespace (gs_gpu_spirv).
#
# Each entry: name, entry point, profile, GS_SLOW, GS_DEPTH_OUT.
set(_zx_shaders
    "gs_vs;gs_vs;vs_6_0;0;0"
    "gs_native_vs;gs_native_vs;vs_6_0;0;0"
    "gs_native_skin_vs;gs_native_skin_vs;vs_6_0;0;0"
    "gs_fs_00;gs_fs;ps_6_0;0;0"
    "gs_fs_01;gs_fs;ps_6_0;0;1"
    "gs_fs_10;gs_fs;ps_6_0;1;0"
    "gs_fs_11;gs_fs;ps_6_0;1;1"
    "blit_vs;blit_vs;vs_6_0;0;0"
    "depth_load_fs;depth_load_fs;ps_6_0;0;0"
    "depth_store_fs;depth_store_fs;ps_6_0;0;0"
    "present_fs;present_fs;ps_6_0;0;0"
    "gather_rgba_fs;gather_rgba_fs;ps_6_0;0;0"
    "gather_index_fs;gather_index_fs;ps_6_0;0;0"
    "fog_fs;fog_fs;ps_6_0;0;0"
    "fog_alpha_fs;fog_alpha_fs;ps_6_0;0;0"
    "fxaa_fs;fxaa_fs;ps_6_0;0;0"
    "warp_fs;warp_fs;ps_6_0;0;0"
)

get_filename_component(_zx_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_zx_out_dir}")
set(_zx_namespace gs_gpu_dxil)
if(GLSLC)
    set(_zx_namespace gs_gpu_spirv)
endif()
set(_zx_text "// Generated from gs_gpu_shaders.hlsl by CompileGsShadersDxil.cmake. Do not edit.\n#pragma once\n\n#include <cstddef>\n\nnamespace ${_zx_namespace}\n{\n")
set(_zx_table "")
# The list above holds 5-field records separated by ';' inside quotes, which
# CMake flattens: read them back five at a time.
list(LENGTH _zx_shaders _zx_n)
math(EXPR _zx_last "${_zx_n} - 1")
foreach(_zx_i RANGE 0 ${_zx_last} 5)
    math(EXPR _i1 "${_zx_i} + 1")
    math(EXPR _i2 "${_zx_i} + 2")
    math(EXPR _i3 "${_zx_i} + 3")
    math(EXPR _i4 "${_zx_i} + 4")
    list(GET _zx_shaders ${_zx_i} _name)
    list(GET _zx_shaders ${_i1} _entry)
    list(GET _zx_shaders ${_i2} _profile)
    list(GET _zx_shaders ${_i3} _slow)
    list(GET _zx_shaders ${_i4} _depth)
    set(_bin "${_zx_out_dir}/${_name}.dxil")
    if(GLSLC)
        # SDL_GPU's Vulkan sets are the HLSL's spaces (-fhlsl-iomap).
        set(_bin "${_zx_out_dir}/${_name}.spv")
        set(_stage vertex)
        if(_profile MATCHES "^ps_")
            set(_stage fragment)
        endif()
        execute_process(
            COMMAND "${GLSLC}" -x hlsl -fshader-stage=${_stage} -fentry-point=${_entry} -fhlsl-iomap
                    --target-env=vulkan1.1 -O -DGS_SPIRV=1 -DGS_SLOW=${_slow} -DGS_DEPTH_OUT=${_depth}
                    -o "${_bin}" "${SRC}"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
    else()
    execute_process(
        COMMAND "${DXC}" -nologo -T ${_profile} -E ${_entry} -HV 2021 -O3 -Qstrip_debug
                -D GS_SLOW=${_slow} -D GS_DEPTH_OUT=${_depth} -Fo "${_bin}" "${SRC}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    endif()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${_name} (${_entry}, ${_profile}) failed:\n${_out}${_err}")
    endif()
    file(READ "${_bin}" _hex HEX)
    string(LENGTH "${_hex}" _hexlen)
    math(EXPR _size "${_hexlen} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    string(APPEND _zx_text "static const unsigned char k_${_name}[${_size}] = {\n    ${_bytes}\n};\n")
    string(APPEND _zx_table "    {\"${_entry}\", ${_slow}, ${_depth}, k_${_name}, sizeof(k_${_name})},\n")
endforeach()
string(APPEND _zx_text "\nstruct Shader\n{\n    const char *entry;\n    int slow;\n    int depthOut;\n    const unsigned char *code;\n    size_t size;\n};\n\nstatic const Shader kShaders[] = {\n${_zx_table}};\n}\n")
file(WRITE "${OUT}.tmp" "${_zx_text}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
