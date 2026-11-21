# recompiler_tests: the recompiler's and the analyzer's unit tests, and the runtime semantics checked through code
# they generate (64-bit branches, float semantics, the runtime's expansions). The test framework (MiniTest.h) and
# the guest function table are z-axis's tests/ (ZAXIS_TESTS_DIR, ../CMakeLists.txt).
add_executable(recompiler_tests
    src/main.cpp
    src/code_generator_tests.cpp
    src/r5900_decoder_tests.cpp
    src/elf_analyzer_tests.cpp
    src/ps2_recompiler_tests.cpp
    src/ps2_runtime_expansion_tests.cpp
    src/ps2_float_semantics_tests.cpp
    src/ps2_branch64_tests.cpp
    $<TARGET_OBJECTS:zx_test_function_table>
)
target_include_directories(recompiler_tests PRIVATE
    ${ZAXIS_TESTS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../ps2xRecomp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../ps2xAnalyzer/include
    ${ZAXIS_RUNTIME_DIR}/include
    ${ZAXIS_RUNTIME_DIR}/runtime
)
target_link_libraries(recompiler_tests PRIVATE ps2_recomp_lib ps2_analyzer_lib zx_runtime ps2_iop)
if(MSVC)
    target_link_options(recompiler_tests PRIVATE "/STACK:8388608")
elseif(MINGW)
    target_link_options(recompiler_tests PRIVATE "--stack,8388608")
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(recompiler_tests PRIVATE ${CMAKE_DL_LIBS})
endif()
