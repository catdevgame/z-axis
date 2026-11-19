cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PS2GSCacheTests LANGUAGES CXX)
    include(CTest)
endif()

get_filename_component(PS2_GS_DEFAULT_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/../../runtime" ABSOLUTE)
set(PS2_GS_RUNTIME_DIR "${PS2_GS_DEFAULT_RUNTIME_DIR}" CACHE PATH "Runtime source to test")
# A build dir configured before the tree's folders were renamed (ps2xRuntime became runtime) keeps the old path in
# its cache: a cached folder that no longer holds the GS sources goes back to the default.
if(NOT EXISTS "${PS2_GS_RUNTIME_DIR}/src/lib/gs")
    set(PS2_GS_RUNTIME_DIR "${PS2_GS_DEFAULT_RUNTIME_DIR}" CACHE PATH "Runtime source to test" FORCE)
endif()
option(PS2_GS_CACHE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PS2_GS_CACHE_BUILD_MEMORY_TESTS "Build tests for the new shared cached-reader API" ON)

find_package(Threads REQUIRED)
add_library(ps2_gs_cache_backend_under_test STATIC
    "${PS2_GS_RUNTIME_DIR}/src/lib/gs/gs_cpu_backend.cpp"
    "${PS2_GS_RUNTIME_DIR}/src/lib/gs/gs_cpu_fast.cpp"
    "${PS2_GS_RUNTIME_DIR}/src/lib/gs/gs_frontend.cpp"
    "${PS2_GS_RUNTIME_DIR}/src/lib/gs/gs_null_backend.cpp"
    "${PS2_GS_RUNTIME_DIR}/src/lib/gs/gs_record.cpp"
    "${PS2_GS_RUNTIME_DIR}/src/lib/gs/ps2_gs_memory.cpp"
    "${PS2_GS_RUNTIME_DIR}/src/lib/ps2_frame_trace.cpp"
)
target_include_directories(ps2_gs_cache_backend_under_test PUBLIC "${PS2_GS_RUNTIME_DIR}/include")
target_compile_features(ps2_gs_cache_backend_under_test PUBLIC cxx_std_20)
target_compile_definitions(ps2_gs_cache_backend_under_test PRIVATE PS2_RUNTIME_LOGS=0 AGRESSIVE_LOGS=0)
target_link_libraries(ps2_gs_cache_backend_under_test PUBLIC Threads::Threads)
# The CPU renderer names and looks up texture packs' pictures (runtime/ps2_texture_pack.h, texpack1): in the
# collection's tree those come from the runtime library; a standalone build of these suites has no runtime to take
# them from.
if(TARGET ps2_runtime)
    target_link_libraries(ps2_gs_cache_backend_under_test PUBLIC ps2_runtime)
endif()
set_target_properties(ps2_gs_cache_backend_under_test PROPERTIES CXX_EXTENSIONS OFF)

if(PS2_GS_CACHE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" OR MSVC)
        message(FATAL_ERROR "PS2_GS_CACHE_SANITIZERS requires a GCC/Clang sanitizer toolchain")
    endif()
    target_compile_options(ps2_gs_cache_backend_under_test PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(ps2_gs_cache_backend_under_test PUBLIC -fsanitize=address,undefined)
endif()

function(add_gs_cache_suite target source prefix)
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE ps2_gs_cache_backend_under_test)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    foreach(test_name IN LISTS ARGN)
        add_test(NAME "gs_cache.${prefix}.${test_name}" COMMAND ${target} "${test_name}")
        set_tests_properties("gs_cache.${prefix}.${test_name}" PROPERTIES LABELS "gs;cache" TIMEOUT 60)
    endforeach()
endfunction()

add_gs_cache_suite(ps2_gs_texture_cache_tests gs_texture_cache_tests.cpp texture
    unaligned_texture unaligned_wrap stale_mirror page_alternation
    flush_visibility upload_visibility local_copy_visibility raster_visibility
    reset_and_rebind invalid_vram_size reserved_psm)

add_gs_cache_suite(ps2_gs_clut_cache_tests gs_clut_cache_tests.cpp clut
    unaligned_csm1_ct32 unaligned_csm1_ct16 unaligned_csm1_ct16s wrapped_clut unaligned_csm2
    retained_palette clut_uses_page_cache cbp0_conditional cbp1_conditional
    reserved_cld nonindexed_cld tex2_reload shared_contexts
    csa_ct32 csa_ct16 csa_ct16s texa_without_reload palette_before_filtering high_planes)

if(PS2_GS_CACHE_BUILD_MEMORY_TESTS)
    add_gs_cache_suite(ps2_gs_memory_cache_tests gs_memory_cache_tests.cpp memory
        ct32 ct24 ct16 ct16s t8 t4 t8h t4hl t4hh z32 z24 z16 z16s
        alias_lanes physical_tag_aliases)
endif()

add_gs_cache_suite(ps2_gs_fast_path_tests gs_fast_path_tests.cpp fast
    random_1 random_2 random_3 random_4 random_immediate
    random_feedback_1 random_feedback_2 random_feedback_immediate big_triangles
    render_target_1 render_target_2 render_target_immediate stale_cache_1 stale_cache_2
    render_target_3 render_target_4 stale_cache_3 huge_coordinates)
