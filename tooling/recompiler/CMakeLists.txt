# The recompiler: it makes each game's generated code (the literal translation of the original program that
# runs until a readable body takes a function's place), from the game's ELF and its config.game.toml. A tool, not
# part of the app: built only from z-axis's top-level CMakeLists.txt when this tooling checkout is present
# (PS2X_BUILD_RECOMPILER), never by the app or the lanes' builds.
#
#   ps2xRecomp     the recompiler (ps2_recomp, and ps2_recomp_lib for the tests)
#   ps2xAnalyzer   the ELF analyzer that writes a game's first config.game.toml
#   tests          their unit tests, and the runtime semantics they check through generated code
#                  (recompiler_tests; z-axis's own unit tests are tests/' ps2x_tests)
#
# z-axis's folders as the tree names them: runtime/ and tests/, or ps2xRuntime/ and ps2xTest/ in a tree from before
# tools17's renames (a build of an older commit).
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/runtime" AND EXISTS "${CMAKE_SOURCE_DIR}/ps2xRuntime")
    set(ZAXIS_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/ps2xRuntime")
    set(ZAXIS_TESTS_DIR "${CMAKE_SOURCE_DIR}/ps2xTest")
else()
    set(ZAXIS_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/runtime")
    set(ZAXIS_TESTS_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()
add_subdirectory(ps2xRecomp)
add_subdirectory(ps2xAnalyzer)
if(PS2X_BUILD_TEST AND TARGET ps2_runtime)
    add_subdirectory(tests)
endif()
