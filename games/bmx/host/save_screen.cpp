// The save screen as the host's saves show it (zge/host/save_jobs.cpp): the
// boot's quiet loads open none (on the host they take no time, and the PS2's
// untitled screen with "Checking..." and "Loading..." would be up for ~4 s);
// every other one is the game's (openSaveUi, save/game_save.cpp). Installed with
// the save jobs' host forms, by the native saves' switch.

#include "ee/ee.h"
#include "ee/host_form.h"
#include "host/save_files.h"

#include <cstdint>
#include "sym/bmx/host/save_screen.cpp.h"

namespace game::bmx::host
{
    namespace
    {
        constexpr uint32_t kQuietLoadScreen = 2; // openSaveUi's display mode for the boot's loads
        // Set by the client's prepare once it opened the screen; the client's
        // finish then plays the screen's shutdown.
        inline ee::Global<uint8_t, sym::g_gameSaveRan> g_gameSaveRan;

        void screenNotOpened() { *g_gameSaveRan = 0; }
    }

    void openSaveUiOnHost(uint32_t displayMode)
    {
        if (displayMode == kQuietLoadScreen)
            return ::game::host::saves::bootScreenSkipped(&screenNotOpened);
        ::game::host::continueInPs2Form(sym::openSaveUi);
    }

    HOST_FORM(saves, sym::openSaveUi, openSaveUiOnHost);
}
