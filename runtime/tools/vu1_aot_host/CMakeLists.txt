# zx-vu1aot for the build machine alone: the VU1 generator, its decoder and the core (src/lib/vu), with nothing of the
# runtime besides. A cross build (Android) builds its VU1 code with it (runtime/CMakeLists.txt builds this project with
# the host's compiler), so it needs no host build's zx-vu1aot (ZX_VU1_AOT_TOOL). Its output is zx-vu1aot's of the
# runtime build, byte for byte (the same sources).
cmake_minimum_required(VERSION 3.20)
project(zx_vu1aot_host CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(ZX_VU1_GENERATOR_KEY "" CACHE STRING "The generator's key (the runtime build's)")
if(NOT ZX_VU1_GENERATOR_KEY)
    message(FATAL_ERROR "ZX_VU1_GENERATOR_KEY: the runtime build's generator key")
endif()
set(_runtime "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(zx-vu1aot
    "${_runtime}/tools/vu1_aot.cpp"
    "${_runtime}/src/lib/vu/ps2_vu1_aot_generator.cpp"
    "${_runtime}/src/lib/vu/ps2_vu1_decoder.cpp"
    "${_runtime}/src/lib/vu/ps2_vu1_upper.cpp"
    "${_runtime}/src/lib/vu/ps2_vu1_lower.cpp"
    "${_runtime}/src/lib/vu/ps2_vu1_core.cpp"
    "${_runtime}/src/lib/vu/ps2_vu1_record.cpp")
target_include_directories(zx-vu1aot PRIVATE "${_runtime}/include" "${_runtime}/src/lib/vu")
target_compile_definitions(zx-vu1aot PRIVATE "ZX_VU1_GENERATOR_KEY=\"${ZX_VU1_GENERATOR_KEY}\"")
if(MSVC)
    target_compile_options(zx-vu1aot PRIVATE /fp:precise)
else()
    target_compile_options(zx-vu1aot PRIVATE -ffp-contract=off)
endif()
