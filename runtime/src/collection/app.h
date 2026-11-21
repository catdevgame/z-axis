#pragma once

// The packaged single-game app (built when CMake's ZX_APP_NAME is set):
// runs the game from the files bundled with it (Resources/disc), keeps
// settings, saves and logs in the platform's per-user folders. app.cpp
// installs these two for the runner's main (runtime/ps2_front_end.h).

namespace zx_app
{
    // True when the runner was started as the app (no ELF argument), false
    // for the developer command line (runner <elf>).
    bool startedAsApp(int argc, char *argv[]);
    int runApp(int argc, char *argv[]);
}
