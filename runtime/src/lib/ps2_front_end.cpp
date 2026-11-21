// The front end a packaged build starts (runtime/ps2_front_end.h).
#include "runtime/ps2_front_end.h"

namespace zx::front_end
{
    namespace
    {
        // Constant-initialised: in place before the installers' static
        // initialisation, whatever order the files' initialisers run in.
        Prepare g_prepare = nullptr;
        App g_app{};
        WindowMade g_windowMade = nullptr;
        Vsync g_vsync = nullptr;
    }

    bool installPrepare(Prepare prepare)
    {
        g_prepare = prepare;
        return true;
    }

    bool installApp(App app)
    {
        g_app = app;
        return true;
    }

    bool installWindowMade(WindowMade windowMade)
    {
        g_windowMade = windowMade;
        return true;
    }

    bool installVsync(Vsync vsync)
    {
        g_vsync = vsync;
        return true;
    }

    Prepare prepare() { return g_prepare; }

    const App &app() { return g_app; }

    WindowMade windowMade() { return g_windowMade; }

    Vsync vsync() { return g_vsync; }
}
