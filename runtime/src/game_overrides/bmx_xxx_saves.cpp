// BMX XXX (SLUS-20415): native save files instead of the memory card: the
// engine's save jobs on the host (zge/host/save_jobs.cpp, one body with
// Aggressive Inline's) at BMX XXX's save system, which is AI's (zsaveapi.c;
// game/bmx/save/game_save.cpp, park_save.cpp). This is the game's part: where
// its saves are.
//
// The game save is the client's buffer (g_gameSaveData, made by its prepare,
// gameSavePrepare, and filled there for a save; restoreGameSave reads a
// loaded one back): 0x23180 bytes, its version byte, the saved game and the
// saved records (game_save.cpp GameSaveFile). Its card folder is
// BASLUS-20415BMXXXXGM (gameSaveFileName). The boot's automatic load
// (startGameAutoLoad, then the quiet load startGameSilentLoad: display mode 2)
// opens no save screen (game/bmx/host/save_screen.cpp). Parks are the park
// buffer's (g_parkSaveBuffer: 0x2000 bytes, the first 0x48 its header, which
// the list reads: park_save.cpp ParkSaveHeader), in folders named
// BASLUS-20415PARK and a number.
//
// Files: <saves>/<profile>.sav (the game save, 143,748 bytes: the byte sum,
// then the buffer, as the card's data file) and <saves>/Parks/Park NNN.sav
// (game/host/save_files.h). The card folders' saves are copied in once; the
// card folders are left as they are. ZX_SAVES=card: the memory card
// dialogs, as on the console.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_saves.cpp.h"
#include "zge_saves.h"

namespace bmx_xxx
{
    namespace
    {
        constexpr uint32_t kGameSaveBytes = 0x23180;
        constexpr uint32_t kParkSaveBytes = 0x2000;
    }

    bool installNativeSaves(PS2Runtime &runtime)
    {
        return zge_host::saves::install(runtime, game::Game::Bmx,
                                        {.gameSaveBuffer = sym::bmx::g_gameSaveData,
                                         .gameSaveBytes = kGameSaveBytes,
                                         .gameSaveCardFolder = "BASLUS-20415BMXXXXGM",
                                         .parkBuffer = sym::bmx::g_parkSaveBuffer,
                                         .parkBytes = kParkSaveBytes,
                                         .parkCardFolderPrefix = "BASLUS-20415PARK",
                                         .completeSave = &keepLevelRecordInSave,
                                         .parkSaved = &releasesParkSaved});
    }
}
