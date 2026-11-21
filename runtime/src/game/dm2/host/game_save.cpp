// DM2's game save as the host's native saves keep it: the memory card frame
// (setUiFrame, uiFrameUpdate: action/ui_frame.cpp, the PS2 form), asked to
// save or load the game, does the work at once on a plain file
// (game/host/save_files.h's WholeSave: the card file's bytes, <saves>/
// <profile>.sav) and shows no frame. It keeps the frame's bookkeeping (what
// it is for, its step, the request), and half a second later tells the
// request the frame is over (its onClosed, as the frame's last step does),
// after the menu that asked has played its own frame's DeInit. A park load
// with a host park file (save_files.h's configureHostPark: Custom Freeplay's
// map; FREERIDE's level 8 and the park menu's LOAD PARK ask for it) is done
// the same way from that file into the request's park buffer, and tells the
// request it is done (its onDone): the level select goes on to load the
// park's level. Other parks, replays, the start check and name entry stay the
// game's frame on the memory card. ZX_SAVES=card: none of this (the frame
// on the emulated card, as on the console).

#include "dm2/action/action.h"
#include "dm2/action/card_frame.h"
#include "dm2/action/ui_frame.h"
#include "dm2/game/memory_card.h"
#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "ee/ee.h"
#include "ee/host_form.h"
#include "host/kernel.h"
#include "host/save_files.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include "sym/dm2/host/game_save.cpp.h"

namespace game::dm2::host
{
    namespace
    {
        namespace files = ::game::host::saves;
        namespace platform = ::game::host;

        // The frame's step before its first (setUiFrame leaves it so until its
        // run picks one), and the frames until the request is told: the menu
        // that asked plays its frame's DeInit first (Options: about a third of
        // a second), and the request's callback must come after it.
        constexpr uint8_t kNoStep = 0xFF;
        constexpr int kFinishDelayFrames = 30;

        // The kind of file the card layer works on (memory_card.cpp's), and
        // what gameState() and singlePlayerMode() read.
        inline ee::Global<uint8_t, sym::g_cardFileKind> g_cardFileKind;
        inline ee::Global<uint8_t, sym::g_nextGameState> g_gameState;
        inline ee::Global<uint8_t, sym::g_singlePlayerMode> g_singlePlayerMode;

        // As the card layer's save (startCardSave): in a ProQuest level the
        // player's goals restarted first, then the saved game and records.
        void saveGame()
        {
            if (*g_gameState == kGameStateLevel && *g_singlePlayerMode == kLevelGoals)
                platform::callGameFunction(sym::restartLevelGoals, {*g_currentPlayer});
            std::vector<uint8_t> file(sizeof(GameSaveFile), 0u);
            GameSaveFile &save = *reinterpret_cast<GameSaveFile *>(file.data());
            save.magic = kGameSaveMagic;
            save.bytes = sizeof(GameSaveFile);
            std::memcpy(save.game, ee::at<uint8_t>(sym::g_savedGame), kSavedGameBytes);
            std::memcpy(save.records, ee::at<uint8_t>(sym::g_savedRecords), kSavedRecordsBytes);
            files::writeWholeSave(file);
        }

        // As the card layer's load's end: the saved game and records back.
        void loadGame()
        {
            std::vector<uint8_t> file;
            if (!files::loadWholeSave(file))
                return;
            const GameSaveFile &save = *reinterpret_cast<const GameSaveFile *>(file.data());
            std::memcpy(ee::at<uint8_t>(sym::g_savedGame), save.game, kSavedGameBytes);
            std::memcpy(ee::at<uint8_t>(sym::g_savedRecords), save.records, kSavedRecordsBytes);
        }

        // The host park file's park into the request's park buffer: true when
        // read.
        bool loadPark(uint32_t buffer)
        {
            std::vector<uint8_t> park(kParkBytes, 0u);
            if (!files::loadHostPark(park))
                return false;
            std::memcpy(ee::at<uint8_t>(buffer), park.data(), park.size());
            return true;
        }
    }

    // setUiFrameOnHost: a game's save or load done at once, and a park load
    // from the host park file; any other frame the game's (a park load too
    // when the file cannot be read).
    void setUiFrameOnHost(uint32_t on, uint32_t kind, ee::Ptr<UiFrameRequest> request)
    {
        const uint32_t work = kind & 0xFFu;
        if (on != 0 && work == kUiFrameLoadPark && request && files::hostPark())
        {
            if (!loadPark(request->data))
                return platform::continueInPs2Form(sym::setUiFrame);
            *g_uiFrameKind = static_cast<uint8_t>(work);
            *g_uiFrameStep = kNoStep;
            *g_cardFileKind = kCardFilePark;
            *g_uiFrameRequest = *request;
            files::job().framesToFinish = kFinishDelayFrames;
            return;
        }
        if (on == 0 || (work != kUiFrameSaveGame && work != kUiFrameLoadGame))
            return platform::continueInPs2Form(sym::setUiFrame);
        *g_uiFrameKind = static_cast<uint8_t>(work);
        *g_uiFrameStep = kNoStep;
        *g_cardFileKind = kCardFileGame;
        if (request)
            *g_uiFrameRequest = *request;
        if (work == kUiFrameSaveGame)
            saveGame();
        else
            loadGame();
        files::job().framesToFinish = kFinishDelayFrames;
    }

    // uiFrameUpdateOnHost: after a save or load, the frames counted down and
    // the request told (a game's save or load: its onClosed, as the card
    // frame's last step; a host park loaded: its onDone); else the game's
    // frame. Always 0.
    uint32_t uiFrameUpdateOnHost()
    {
        int &frames = files::job().framesToFinish;
        if (frames == 0)
        {
            platform::continueInPs2Form(sym::uiFrameUpdate);
            return 0;
        }
        if (--frames != 0)
            return 0;
        const UiFrameRequest &request = *g_uiFrameRequest;
        const UiFrameCallback tell = *g_uiFrameKind == kUiFrameLoadPark ? request.onDone : request.onClosed;
        if (tell)
            platform::callGameFunction(tell.address, {request.data, request.dataHandle});
        return 0;
    }

    HOST_FORM(saves, sym::setUiFrame, setUiFrameOnHost);
    HOST_FORM(saves, sym::uiFrameUpdate, uiFrameUpdateOnHost);
}
