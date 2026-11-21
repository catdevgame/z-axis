# The recompiler: it makes each game's generated code (the literal translation of the original program that
# runs until a readable body takes a function's place), from the game's ELF and its config.game.toml. A tool, not
# part of the app: built only from z-axis's top-level CMakeLists.txt when this tooling checkout is present
# (ZX_BUILD_RECOMPILER), never by the app or the lanes' builds.
#
#   ps2xRecomp     the recompiler (ps2_recomp, and ps2_recomp_lib for the tests)
#   ps2xAnalyzer   the ELF analyzer that writes a game's first config.game.toml
#   tests          their unit tests, and the runtime semantics they check through generated code
#                  (recompiler_tests; z-axis's own unit tests are tests/' zx-tests)
#
# z-axis's folders the recompiler's tests read.
# (the PS2 runtime's folder: its include/ holds what generated code and the recompiler include)
set(ZAXIS_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/ps2")
set(ZAXIS_TESTS_DIR "${CMAKE_SOURCE_DIR}/tests")
add_subdirectory(ps2xRecomp)
add_subdirectory(ps2xAnalyzer)
if(ZX_BUILD_TEST AND TARGET zx_runtime)
    add_subdirectory(tests)
endif()
