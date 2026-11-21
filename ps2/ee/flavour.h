#pragma once

// The build's flavour (ps2/CMakeLists.txt makes build_flavour.h): ZX_DEV_CHECKS 1 in the dev build (the
// checkers' machinery and hooks), 0 in the app's; ZX_GUEST_MAP_CHECK 1 in a tool build that checks every readable
// access against the resolution before the guest map. A compile with the source tree's flags alone (a syntax check
// against an older build's compile commands) has no build_flavour.h and is the dev build's.
#if __has_include("build_flavour.h")
#include "build_flavour.h"
#else
#define ZX_DEV_CHECKS 1
#define ZX_GUEST_MAP_CHECK 0
#endif
