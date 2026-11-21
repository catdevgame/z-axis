cmake_minimum_required(VERSION 3.20)

project(PS2Recomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    elfio
    # Release_3.12: the tag's source archive, checked by SHA-256 (no git clone).
    URL https://github.com/serge1/ELFIO/archive/refs/tags/Release_3.12.tar.gz
    URL_HASH SHA256=e4ebc9ce3d6916461bc3e7765bb45e6210f0a9b93978bf91e59b05388c024489
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(elfio)

FetchContent_Declare(
    toml11
    # v4.4.0: the tag's source archive, checked by SHA-256 (no git clone, so none of its test and docs submodules).
    URL https://github.com/ToruNiina/toml11/archive/refs/tags/v4.4.0.tar.gz
    URL_HASH SHA256=815bfe6792aa11a13a133b86e7f0f45edc5d71eb78f5fb6686c49c7f792b9049
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(toml11)

FetchContent_Declare(
    fmt
    # 12.1.0: the tag's source archive, checked by SHA-256 (no git clone).
    URL https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz
    URL_HASH SHA256=ea7de4299689e12b6dddd392f9896f08fb0777ac7168897a244a6d6085043fea
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(fmt)
 
FetchContent_Declare(
    libdwarf
    # v2.2.0: the tag's source archive, checked by SHA-256 (no git clone).
    URL https://github.com/davea42/libdwarf-code/archive/refs/tags/v2.2.0.tar.gz
    URL_HASH SHA256=379bc309a86a713ca0227c95e4f7f79e5c463200741e315feaa804b43a33d2d9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(BUILD_DWARFDUMP    OFF CACHE BOOL "" FORCE)
set(BUILD_DWARFEXAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_DWARFGEN     OFF CACHE BOOL "" FORCE)

set(BUILD_SHARED   OFF CACHE BOOL "" FORCE)
# (no compressed debug sections: a PS2 ELF has none, and without them the recompiler links no zlib or zstd; the
# app bundles it, so it takes nothing outside the system: Homebrew's zstd it found before was a player's missing library)
set(ENABLE_DECOMPRESSION OFF CACHE BOOL "" FORCE)
set(BUILD_NON_SHARED ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libdwarf)

set(LIBDWARF_INCLUDE_DIR "${libdwarf_SOURCE_DIR}/src/lib/libdwarf")

FetchContent_Declare(
    rabbitizer
    # 1.14.3: the tag's source archive, checked by SHA-256 (no git clone).
    URL https://github.com/Decompollaborate/rabbitizer/archive/refs/tags/1.14.3.tar.gz
    URL_HASH SHA256=cda4b90f68914e86f6d134c3edec944be09f8ba1327358fa217e99a0fb2e3e26
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(rabbitizer)

if (NOT TARGET rabbitizer)
    file(GLOB_RECURSE RABBITIZER_SOURCES CONFIGURE_DEPENDS
        "${rabbitizer_SOURCE_DIR}/src/analysis/*.c"
        "${rabbitizer_SOURCE_DIR}/src/common/*.c"
        "${rabbitizer_SOURCE_DIR}/src/instructions/*.c"
        "${rabbitizer_SOURCE_DIR}/src/instructions/*/*.c"
    )

    add_library(rabbitizer STATIC ${RABBITIZER_SOURCES})

    target_include_directories(rabbitizer PUBLIC
        "${rabbitizer_SOURCE_DIR}/include"
        "${rabbitizer_SOURCE_DIR}/tables"
        "${rabbitizer_SOURCE_DIR}/tables/tables"
    )
endif()

file(GLOB_RECURSE PS2RECOMP_LIB_SOURCES CONFIGURE_DEPENDS
    src/lib/*.cpp
)

file(GLOB_RECURSE PS2RECOMP_HEADERS CONFIGURE_DEPENDS
    include/*.h
    include/*.hpp
)

add_library(ps2_recomp_lib STATIC ${PS2RECOMP_LIB_SOURCES} ${PS2RECOMP_HEADERS})

# The recompiler's source id for the output manifest (cmake/source_id.cmake), made again at every build.
set(PS2RECOMP_SOURCE_ID_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ps2recomp_source_id.h")
add_custom_target(ps2recomp_source_id
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -DRUNTIME_INCLUDE=${ZAXIS_RUNTIME_DIR}/include
            "-DBUILT_WITH=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_SYSTEM_NAME}"
            -DOUT=${PS2RECOMP_SOURCE_ID_HEADER} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/source_id.cmake
    BYPRODUCTS ${PS2RECOMP_SOURCE_ID_HEADER}
    COMMENT "ps2recomp source id"
    VERBATIM)
add_dependencies(ps2_recomp_lib ps2recomp_source_id)
target_include_directories(ps2_recomp_lib PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_compile_definitions(ps2_recomp_lib
PUBLIC
    LIBDWARF_STATIC
PRIVATE
    LIBDWARF_STATIC
)

target_include_directories(ps2_recomp_lib
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${elfio_SOURCE_DIR}
    ${LIBDWARF_INCLUDE_DIR}

PRIVATE
    ${ZAXIS_RUNTIME_DIR}/include
)

target_link_libraries(ps2_recomp_lib
PUBLIC
    fmt::fmt
    toml11::toml11 
    dwarf
    rabbitizer
)

file(GLOB_RECURSE PS2RECOMP_EXE_SOURCES CONFIGURE_DEPENDS
    src/runner/*.cpp
)

add_executable(ps2_recomp ${PS2RECOMP_EXE_SOURCES})

target_link_libraries(ps2_recomp
PRIVATE
    ps2_recomp_lib
)

install(TARGETS ps2_recomp ps2_recomp_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include
)

include("${ZAXIS_RUNTIME_DIR}/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_recomp_lib)
    EnableFastReleaseMode(ps2_recomp)
endif()
