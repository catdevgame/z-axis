#pragma once

// The front end a packaged build starts in place of the developer runner,
// and what it is told while a game runs. The runtime knows no front end:
// one that is built in installs its parts here when the program starts
// (before main), and the runtime asks. The collection's app
// (collection/app.cpp) installs its start; the Android build's start
// (collection/android_app.cpp) installs its preparation, its window's
// background pause and the second screen's capture. A build with neither
// (the developer runner, the tests) runs the ELF it is given, as before.

#include <cstdint>

class PS2Machine;

namespace zx::front_end
{
    // First in main, after only the log's redirection: Android's files
    // folder, the environment the Activity cannot give, argv.
    using Prepare = void (*)(int &argc, char **&argv);
    // The game's host window was made (PS2Machine::initialize).
    using WindowMade = void (*)(PS2Machine &runtime);
    // Every vsync, on the EE's thread, with the guest's main RAM.
    using Vsync = void (*)(const uint8_t *rdram);

    struct App
    {
        // This start is the app's (no ELF argument, a Finder launch, --game).
        bool (*startedAsApp)(int argc, char *argv[]) = nullptr;
        // The app to its end: the process's exit status.
        int (*run)(int argc, char *argv[]) = nullptr;
    };

    // Each returns true, for the installing file's namespace-scope
    // `const bool installed = zx::front_end::installApp(...)`: set before
    // main runs. At most one of each in a build.
    bool installPrepare(Prepare prepare);
    bool installApp(App app);
    bool installWindowMade(WindowMade windowMade);
    bool installVsync(Vsync vsync);

    // What was installed (nullptr, an App of nullptrs: none).
    Prepare prepare();
    const App &app();
    WindowMade windowMade();
    Vsync vsync();
}
