// DM2's game save as the host's native saves keep it: the memory card frame
// (setUiFrame, uiFrameUpdate: action/ui_frame.cpp, the PS2 form), asked to
// save or load the game, does the work at once on a plain file
// (engine/host/save_files.h's WholeSave: the card file's bytes, <saves>/
// <profile>.sav) and shows no frame. It keeps the frame's bookkeeping (what
// it is for, its step, the request), and half a second later tells the
// request the frame is over (its onClosed, as the frame's last step does),
// after the menu that asked has played its own frame's DeInit. The intro's
// auto load is the same load, and tells the request its work is done (its
// onDone, as the frame does whether a card had a save or not). A park load
// with a host park file (save_files.h's configureHostPark: Custom Freeplay's
// map; FREERIDE's level 8 and the park menu's LOAD PARK ask for it) is done
// the same way from that file into the request's park buffer, and tells the
// request it is done (its onDone): the level select goes on to load the
// park's level. The start check finds nothing to tell (no card is needed).
// Replays and the other parks are the game's frame over native files a slot
// each (slot_saves.cpp). ZX_SAVES=card: none of this (the frame on the
// emulated card, as on the console).

#include "saves.h"

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
#include <utility>
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
        // The frames before a step that follows the host's work: the frame's
        // menu, shown again as the step left is played out, is up a frame
        // later (its State), and a step's message shows only once it is.
        constexpr int kStepDelayFrames = 2;

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

    void tellRequestLater(bool workDone)
    {
        files::job().framesToFinish = kFinishDelayFrames;
        files::job().stepAfter = -1;
        files::job().workDone = workDone;
    }

    void showStepLater(uint32_t step)
    {
        files::job().framesToFinish = kStepDelayFrames;
        files::job().stepAfter = static_cast<int>(step);
    }

    // setUiFrameOnHost: a game's save or load done at once (the auto load's
    // too), and a park load from the host park file; any other frame the
    // game's (a park load too when the file cannot be read).
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
            return tellRequestLater(true);
        }
        if (on == 0 || (work != kUiFrameSaveGame && work != kUiFrameLoadGame && work != kUiFrameAutoLoad))
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
        tellRequestLater(work == kUiFrameAutoLoad);
    }

    namespace
    {
        // The frame's title text, below the caller's stack while the frame's
        // run is the host's (the PS2 form keeps it on its own stack).
        constexpr uint32_t kTitleBytes = 0x40;

        // What the frame's own run does every frame besides the card's work
        // (uiFrameUpdate, action/card_frame.cpp): the top header's title set
        // to the frame's ("$KSTR_SAVE_TITLE<kind>") and shown with its notch,
        // the black banner behind it, which the menus after the frame keep up
        // (the intro's auto load is the first frame to show it: CHOOSE A BIKE's
        // banner is its); the start check's hidden.
        void showFrameTitle()
        {
            const uint32_t root = (*g_cardFrameUiRoot).address;
            const uint32_t title = platform::belowStack(kTitleBytes);
            platform::callGameFunction(sym::formatString, {title, sym::saveTitleFormat, *g_uiFrameKind});
            uint32_t header = 0;
            platform::callGameFunction(sym::findNode, {root, sym::topHeaderTitlePath, 0}, header);
            platform::callGameFunction(sym::setVariable, {header, sym::textVariableName, title});
            platform::callGameFunction(
                sym::runScript, {root, *g_uiFrameKind == kUiFrameStartCheck ? sym::titleHiddenScript : sym::titleShownScript});
        }
    }

    // uiFrameUpdateOnHost: after the host's work, the frames counted down and
    // then the frame moved on to the step that follows a replay's or a park's
    // save or load, or the request told: its onClosed after a game's save or
    // load (as the card frame's last step), or its onDone when the work is
    // done (the auto load, a park or replay loaded), with the frame then over
    // as the frame's own finish leaves it; else the game's frame. While the
    // frames count down the frame's title is shown as its own run shows it.
    // Always 0.
    uint32_t uiFrameUpdateOnHost()
    {
        int &frames = files::job().framesToFinish;
        if (frames == 0)
        {
            platform::continueInPs2Form(sym::uiFrameUpdate);
            return 0;
        }
        showFrameTitle();
        if (--frames != 0)
            return 0;
        if (const int step = std::exchange(files::job().stepAfter, -1); step >= 0)
        {
            platform::callGameFunction(sym::setUiFrameStep, {static_cast<uint32_t>(step)});
            return 0;
        }
        const UiFrameRequest &request = *g_uiFrameRequest;
        if (!files::job().workDone)
        {
            if (request.onClosed)
                platform::callGameFunction(request.onClosed.address, {request.data, request.dataHandle});
            return 0;
        }
        if (request.onDone)
            platform::callGameFunction(request.onDone.address, {request.data, request.dataHandle});
        *g_uiFrameStepResults = 0;
        *g_uiFrameStep = kStepOver;
        *g_uiFrameStepFrames = 0;
        return 0;
    }

    // checkMemoryCardsOnHost: the start check's look at the cards. The saves
    // need no card: nothing to tell (0), port 0.
    uint32_t checkMemoryCardsOnHost(ee::Ptr<uint32_t> port)
    {
        if (port)
            *port = 0;
        return 0;
    }

    HOST_FORM(saves, sym::setUiFrame, setUiFrameOnHost);
    HOST_FORM(saves, sym::uiFrameUpdate, uiFrameUpdateOnHost);
    HOST_FORM(saves, sym::checkMemoryCards, checkMemoryCardsOnHost);
}
