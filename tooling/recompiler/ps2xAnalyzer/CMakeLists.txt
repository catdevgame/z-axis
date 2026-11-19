cmake_minimum_required(VERSION 3.20)
project(PS2Analyzer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    # v3.11.3: the release's json.tar.xz (the library's headers and CMake files, as its README gives for
    # FetchContent), checked by SHA-256 (no git clone).
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(PS2ANALYZER_LIB_SOURCES
    src/analysis_passes.cpp
    src/elf_analysis_context.cpp
    src/elf_analyzer.cpp
    src/function_classifier.cpp
    src/sce_symbol_scanner.cpp
    src/toml_generator.cpp
)

add_library(ps2_analyzer_lib STATIC ${PS2ANALYZER_LIB_SOURCES})

target_include_directories(ps2_analyzer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../ps2xRecomp/include
    ${ZAXIS_RUNTIME_DIR}/include
)

target_link_libraries(ps2_analyzer_lib PUBLIC
    ps2_recomp_lib
    nlohmann_json::nlohmann_json
)

add_executable(ps2_analyzer
    src/analyzer_main.cpp
)

target_link_libraries(ps2_analyzer PRIVATE
    ps2_analyzer_lib
)

install(TARGETS ps2_analyzer ps2_analyzer_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

include("${ZAXIS_RUNTIME_DIR}/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_analyzer_lib)
    EnableFastReleaseMode(ps2_analyzer)
endif()
