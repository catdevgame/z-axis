The C library headers a game module's compile reaches through the compiler's intrinsics headers (sse2neon.h on ARM,
mm_malloc.h): declarations only, on the module's include path alone (cmake/game_module.cmake). A module calls none of these
functions: anything that did would be an undefined symbol at the module's link, which fails the build.
