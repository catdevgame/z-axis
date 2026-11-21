// The game save (gamehand14; were the sweep's drafts of saveRegionAllocate ..
// setFrameTime): the game's client of the engine's save system (zsaveapi; its
// region, methods, prepare and registration), and the menus' and the pause
// screen's save and loads, each setting the client's mode (gameSavePrepare
// reads it) and starting the save system with the client, and its load
// (restoreGameSave). The globals file before it (debugMenuSecondHook .. saveRegionAllocate) is not
// rewritten; saveCareerLevel, the same module's, is level/load.cpp's (beside
// restoreCareerLevel).
//

#include "zge/memory/dmm.h"
#include "zge/memory/regions.h"
#include "aggressive/career.h"
#include "aggressive/game_state.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/platform/platform.h"
#include "zge/text/cstring.h"
#include "zge/platform/save_system.h"
#include "zge/zgui/widget.h"
#include "aggressive/game_save.h"
#include "aggressive/game_start.h"
#include "aggressive/level/load.h"
#include "aggressive/level/objectives.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/game_save.cpp.h"

namespace zge::zgui { int32_t variableToText(ee::Ptr<Node>, ee::Ptr<char>); }

namespace aggressive { int32_t startGameSilentLoad(); }

namespace aggressive
{
    namespace
    {
        enum class GameSaveMode : int32_t
        {
            Load = 0,
            Save = 1,
            AutoLoad = 2,
            SilentLoad = 3,
            Idle = 4, // registered, nothing asked
        };
        inline ee::Global<uint32_t, sym::g_gameSaveClient> g_gameSaveClient; // registerGameSaveClient's
        inline ee::Global<GameSaveMode, sym::g_gameSaveMode> g_gameSaveMode;
        inline ee::Global<uint8_t, sym::g_gameLoadPending> g_gameLoadPending; // 1 once a load starts

        // How long each save's and load's job waits before it starts (the
        // save API's delay; the silent load's 0).
        constexpr uint32_t kJobDelayMilliseconds = 750;
    }

    // unusedGameSaveHook: empty (no caller and no reference in the game).
    void unusedGameSaveHook()
    {
    }
    READABLE_FUNCTION(unusedGameSaveHook, Verified);

    // startGameSave (inlineMenuSaveGame's and inlinePauseSave's tail call):
    // the game saved.
    int32_t startGameSave()
    {
        *g_gameSaveMode = GameSaveMode::Save;
        zge::startSaveSystem();
        return game::tail(zge::saveApiSave, *g_gameSaveClient, kJobDelayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startGameSave, Verified);

    // startGameLoad: the game loaded.
    int32_t startGameLoad()
    {
        *g_gameSaveMode = GameSaveMode::Load;
        zge::startSaveSystem();
        const uint32_t client = *g_gameSaveClient;
        *g_gameLoadPending = 1;
        return game::tail(zge::saveApiLoad, client, kJobDelayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startGameLoad, Verified);

    // startGameSilentLoad: a load without the save API's job value [inferred: the quiet one].
    int32_t startGameSilentLoad()
    {
        *g_gameSaveMode = GameSaveMode::SilentLoad;
        zge::startSaveSystem();
        const uint32_t client = *g_gameSaveClient;
        *g_gameLoadPending = 1;
        return game::tail(zge::saveApiSilentLoad, client, 0);
    }
    READABLE_FUNCTION_WAITS(startGameSilentLoad, Verified);

    // startGameAutoLoad: the automatic load [inferred: at boot].
    int32_t startGameAutoLoad()
    {
        *g_gameSaveMode = GameSaveMode::AutoLoad;
        zge::startSaveSystem();
        const uint32_t client = *g_gameSaveClient;
        *g_gameLoadPending = 1;
        return game::tail(zge::saveApiAutoLoad, client, kJobDelayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startGameAutoLoad, Verified);

    namespace
    {
        constexpr uint32_t kDmmRelease = sym::dmmRelease;     // (region, handle)
        constexpr uint32_t kAllocateMemory = sym::allocateMemory; // (bytes) -> memory
        constexpr uint32_t kStopSaveSystem = sym::stopSaveSystem;
        constexpr uint32_t kOpenSaveLoadMenu = sym::openSaveUi;  // (kind: 0 save, 1 load, 2 the quiet loads) "SaveLoad.SaveLoadMenu"
        constexpr uint32_t kShowSaveMessage = sym::showSaveMessage, kShowSaveChoice = sym::showSaveChoice, kShowSaveSlotMenu = sym::showSaveSlotMenu;
        constexpr uint32_t kSaveRegionAllocate = sym::saveRegionAllocate; // saveRegionAllocate below
        constexpr uint32_t kRestoreGameSave = sym::restoreGameSave;   // the client's load (not registered: no recording)
        constexpr uint32_t kGameSaveTitle = sym::gameSaveTitle2;     // "Aggressive Inline Skaters"
        constexpr uint32_t kGameSaveFileName = sym::gameSaveFileName2;  // "BASLUS-20327CEAIGAME"
        constexpr uint32_t kTestSaveFileName = sym::testSaveFileName2;  // "TESTGAM.SAV"
        constexpr uint32_t kSaveIconPath = sym::saveIconPath;      // "assets\icon.icn"
        constexpr uint32_t kSaveRegionBytes = 0x7D000;
        constexpr uint32_t kGameSaveBytes = 0x70400;         // the save's buffer (a save file's bytes after its byte sum)
        // The save's layout: the unlocks (g_unlockedSkaters, 0xF74 bytes), then each
        // skater record but the first: its first 0x2A8 bytes and its first
        // ten career levels' six words and blocks (career.h).
        constexpr uint32_t kUnlocks = sym::g_unlockedSkaters, kUnlocksBytes = 0xF74;
        constexpr uint32_t kSkaterHeadBytes = sizeof(SkaterProfile);
        constexpr uint32_t kSkaters = 20, kSavedLevels = 10, kLevelWords = 6;

        inline ee::Global<uint32_t, sym::g_gameSaveData> g_gameSaveData;
        inline ee::Global<uint32_t, sym::g_gameSaveDataBytes> g_gameSaveDataBytes;
        inline ee::Global<uint32_t, sym::g_gameSaveIcon> g_gameSaveIcon;
        inline ee::Global<uint32_t, sym::g_gameSaveIconSize> g_gameSaveIconSize;
        inline ee::Global<uint8_t, sym::g_gameSaveRan> g_gameSaveRan;      // a save or load ran (its menu's end at shutdown)
        inline ee::Global<ee::Ptr<zge::DmmRegion>, sym::g_saveRegion> g_saveRegion; // the save's DMM region
        inline ee::Global<uint32_t, sym::g_saveRegionFirst> g_saveRegionFirst; // 16 bytes taken at the start; nothing reads them

        // The client the engine's save system calls (registerSaveClient).
        struct SaveClient
        {
            uint32_t listBufferBytes; // +0x00 the original never sets it: see registerGameSaveClient
            uint32_t dataSize;     // +0x04
            uint32_t data;         // +0x08
            uint32_t title;        // +0x0C
            uint32_t fileName;     // +0x10
            uint32_t iconSize;     // +0x14
            uint32_t icon;         // +0x18
            uint32_t singleFile;   // +0x1C
            uint32_t prepare;      // +0x20
            uint32_t restore;      // +0x24
            uint32_t _28;          // left as the stack had it
            uint32_t showMessage;  // +0x2C
            uint32_t showChoice;   // +0x30
            uint32_t message13;    // 0
            uint32_t showSlotMenu; // +0x38
            uint32_t showFileMenu; // 0
        };
        static_assert(offsetof(SaveClient, prepare) == 0x20 && offsetof(SaveClient, showSlotMenu) == 0x38);
    }

    // saveRegionAllocate (bytes): a locked block of the save's region.
    uint32_t saveRegionAllocate(uint32_t bytes)
    {
        const uint32_t handle = zge::dmmAllocate(*g_saveRegion, bytes);
        return zge::zmemDMMLock(*g_saveRegion, handle);
    }
    READABLE_FUNCTION(saveRegionAllocate, Verified);

    namespace
    {
        namespace tail
        {
        uint32_t dmmRelease(ee::Ptr<zge::DmmRegion> region, uint32_t index) { return ee::tailCall<uint32_t>(kDmmRelease, region, index); }
        }
    }

    // saveRegionFree (memory): a block of the save's region back.
    uint32_t saveRegionFree(ee::Address memory)
    {
        const uint32_t handle = zge::dmmHandleOf(*g_saveRegion, memory);
        return tail::dmmRelease(*g_saveRegion, handle);
    }
    READABLE_FUNCTION(saveRegionFree, Verified);

    // The client's methods.
    uint32_t gameSaveDataSize() { return kGameSaveBytes; } // gameSaveDataSize
    READABLE_FUNCTION(gameSaveDataSize, Verified);
    uint32_t gameSaveSingleFile() { return 1; } // gameSaveSingleFile (the park's answers 0)
    READABLE_FUNCTION(gameSaveSingleFile, Verified);
    uint32_t gameSaveData() { return *g_gameSaveData; } // gameSaveData
    READABLE_FUNCTION(gameSaveData, Verified);
    uint32_t gameSaveTitle() { return kGameSaveTitle; } // gameSaveTitle
    READABLE_FUNCTION(gameSaveTitle, Verified);
    uint32_t gameSaveFileName() { return kGameSaveFileName; } // gameSaveFileName
    READABLE_FUNCTION(gameSaveFileName, Verified);
    uint32_t testSaveFileName() { return kTestSaveFileName; } // testSaveFileName (no caller)
    READABLE_FUNCTION(testSaveFileName, Verified);

    int32_t openSaveUi(uint32_t displayMode);

    // gameSavePrepare: the client's prepare: the save's buffer (when the
    // save's size fits it: the unlocks and every skater's records and
    // career levels, all fourteen counted), then in a save the unlocks and
    // skaters 1 .. 19 written into it (only ten levels each: the original's;
    // skater 0 is not saved), then the save/load menu for the mode.
    uint32_t gameSavePrepare()
    {
        // The size: the unlocks, each skater's head and fourteen levels' words
        // and blocks.
        uint32_t bytes = kUnlocksBytes;
        for (uint32_t skater = 0; skater < kSkaters; ++skater)
        {
            bytes += kSkaterHeadBytes;
            for (uint32_t level = 0; level < kCareerLevels; ++level)
            {
                const ee::Ptr<CareerLevelState> state = careerLevelState(skater, level);
                bytes = bytes + kLevelWords * 4 - 4 + state->objectStatesSize + state->scriptVariablesSize + state->goalsSize + state->propsSize;
                bytes += state->visibilityCount;
            }
        }
        if (bytes <= kGameSaveBytes - 1)
        {
            *g_gameSaveDataBytes = kGameSaveBytes;
            *g_gameSaveData = zge::allocateMemory(kGameSaveBytes);
        }

        if (*g_gameSaveMode == GameSaveMode::Save)
        {
            const uint32_t data = *g_gameSaveData;
            uint32_t cursor = data + kUnlocksBytes;
            zge::memoryCopy(data, kUnlocks, kUnlocksBytes);
            const auto write = [&](ee::Address from, uint32_t size) {
                zge::memoryCopy(cursor, from, size);
                cursor += size;
            };
            const auto writeBlock = [&](ee::Address block, uint32_t size) {
                if (size)
                    write(block, size);
            };
            for (uint32_t skater = 1; skater < kSkaters; ++skater)
            {
                write(skater * kSkaterRecordSize + kSkaterRecords, kSkaterHeadBytes);
                for (uint32_t level = 0; level < kSavedLevels; ++level)
                {
                    // The level's six words, then its blocks.
                    const CareerLevelState &state = *careerLevelState(skater, level);
                    write(&state.objectStatesSize, 4);
                    write(&state.scriptVariablesSize, 4);
                    write(&state.goalsSize, 4);
                    write(&state.propsSize, 4);
                    write(&state.visibilityCount, 4);
                    write(&state.completion, 4);
                    writeBlock(state.objectStates, state.objectStatesSize);
                    writeBlock(state.scriptVariables, state.scriptVariablesSize);
                    writeBlock(state.goals, state.goalsSize);
                    writeBlock(state.props, state.propsSize);
                    writeBlock(state.visibility, state.visibilityCount);
                }
            }
        }

        switch (*g_gameSaveMode)
        {
        case GameSaveMode::Load:
            openSaveUi(1);
            break;
        case GameSaveMode::AutoLoad:
        case GameSaveMode::SilentLoad:
            openSaveUi(2);
            break;
        case GameSaveMode::Save:
            openSaveUi(0);
            break;
        default:
            break;
        }
        *g_gameSaveRan = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(gameSavePrepare, Verified);

    uint32_t gameSaveIconSize() { return *g_gameSaveIconSize; } // gameSaveIconSize
    READABLE_FUNCTION(gameSaveIconSize, Verified);
    uint32_t gameSaveIcon() { return *g_gameSaveIcon; } // gameSaveIcon
    READABLE_FUNCTION(gameSaveIcon, Verified);

    // registerGameSaveClient (the game's setup): the save's region and the game's
    // save client registered (idle), its icon read from assets\icon.icn.
    void registerGameSaveClient()
    {
        ee::Local<SaveClient> client;
        // The original never sets listBufferBytes: the save list takes what its
        // stack held there (a word engineMain left: the display set-up's
        // upper half) as a save's size and allocates that.
        client->listBufferBytes = sym::g_displaySetupFlags & 0xFFFF0000u;
        *g_saveRegion = zge::newDmmRegion(kSaveRegionBytes);
        *g_saveRegionFirst = saveRegionAllocate(0x10);
        client->showChoice = kShowSaveChoice;
        client->showMessage = kShowSaveMessage;
        client->dataSize = sym::gameSaveDataSize;
        client->data = sym::gameSaveData;
        client->title = sym::gameSaveTitle;
        client->singleFile = sym::gameSaveSingleFile;
        client->iconSize = sym::gameSaveIconSize;
        client->icon = sym::gameSaveIcon;
        client->fileName = sym::gameSaveFileName;
        client->restore = kRestoreGameSave;
        client->prepare = sym::gameSavePrepare;
        client->showSlotMenu = kShowSaveSlotMenu;
        client->showFileMenu = 0;
        client->message13 = 0;
        *g_gameSaveClient = zge::registerSaveClient(client);
        *g_gameSaveMode = GameSaveMode::Idle;
        *g_gameSaveData = 0;
        *g_gameSaveDataBytes = 0;
        const ee::Ptr<zge::ZFile> file = zge::openFile(kSaveIconPath, zge::kFileModeRead, 0).file();
        zge::fileSize(file, sym::g_gameSaveIconSize, 0);
        const uint32_t icon = zge::heapAllocate(*g_gameSaveIconSize);
        const uint32_t iconBytes = *g_gameSaveIconSize;
        *g_gameSaveIcon = icon;
        zge::fileRead(file, icon, iconBytes, 0);
        zge::closeFile(file, 0);
    }
    READABLE_FUNCTION_WAITS(registerGameSaveClient, Verified);

    // shutdownGameSave (the game's shutdown): the save's region freed, the
    // save system stopped after a load, and the save/load menu's end when
    // one ran.
    void shutdownGameSave()
    {
        zge::freeDmmRegion(*g_saveRegion);
        uint8_t ran = *g_gameSaveRan;
        if (*g_gameLoadPending)
        {
            zge::stopSaveSystem();
            *g_gameLoadPending = 0;
            ran = *g_gameSaveRan;
        }
        if (ran)
        {
            playSaveShutdown();
            *g_gameSaveRan = 0;
        }
    }
    READABLE_FUNCTION_WAITS(shutdownGameSave, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_levelNumber> g_currentLevel;
    }

    // saveCareerProgress: the player's career progress at a level's end, back
    // into its save record: the level's completion in percent (its goals
    // 85%, its collectibles 15%; the Tutorial's goals are all of it), the
    // skater's level, and over the career levels the mean completion, the
    // points a second and the rollovers. The Tutorial keeps only its level
    // records.
    void saveCareerProgress(ee::Ptr<skater::Player> player)
    {
        ee::Float kept = ee::Float(0.0f);
        const ee::Ptr<SkaterProfile> profile = skater::skaterProfile(player);
        const uint32_t skaterIndex = profile->index & 0x1Fu;
        const ee::Ptr<CareerLevelState> state = careerLevelState(skaterIndex, static_cast<uint32_t>(*g_currentLevel));
        ee::Float &completion = state->completion;
        completion = ee::Float(0.0f);
        if (aggressive::level::objectivesComplete() != 0)
        {
            const int32_t level = *g_currentLevel;
            ee::Float goalShare;
            kept = ee::toFloat(profile->levels[level].goalsDone);
            if (level == static_cast<int32_t>(kLevelTutorial))
                goalShare = kept / ee::toFloat(aggressive::level::objectivesComplete());
            else
                goalShare = kept / ee::toFloat(aggressive::level::objectivesComplete()) * ee::Float(0.85f);
            completion = completion + goalShare;
        }
        const int32_t level = *g_currentLevel;
        if (level != static_cast<int32_t>(kLevelTutorial))
        {
            const LevelRecord &record = profile->levels[level];
            if (record.collectibles != 0)
                completion = completion + ee::toFloat(static_cast<int32_t>(record.collected)) / ee::toFloat(static_cast<int32_t>(record.collectibles)) * ee::Float(0.15f);
        }
        const ee::Float percent = completion * ee::Float(100.0f);
        completion = percent;
        if (ee::Float(99.0f) < percent)
            completion = ee::Float(100.0f);

        aggressive::skater::computeLevel(profile);
        profile->pointsPerSecond = ee::Float(0.0f);
        profile->rollovers = 0;
        profile->continues = 0;
        profile->completion = ee::Float(0.0f);
        for (uint32_t careerLevel = 0;;)
        {
            const LevelRecord &record = profile->levels[careerLevel];
            if (ee::Float(0.0f) < record.secondsPlayed)
                profile->pointsPerSecond = profile->pointsPerSecond + ee::toFloat(static_cast<int32_t>(record.score)) / profile->timePlayed;
            profile->rollovers += record.rollovers;
            profile->continues += record.continues;
            profile->completion = profile->completion + careerLevelState(skaterIndex, careerLevel)->completion / ee::Float(7.0f);
            if (++careerLevel >= kCareerLevelCount)
                break;
        }
        const uint32_t record = kSkaterRecords + skaterIndex * kSkaterRecordSize;
        if (*g_currentLevel != static_cast<int32_t>(kLevelTutorial))
            zge::memoryCopy(record, profile, sizeof(SkaterProfile));
        else
            zge::memoryCopy(record + offsetof(SkaterProfile, levels), &profile->levels, kProfileLevels * sizeof(LevelRecord));
        game::tail(aggressive::level::saveCareerLevel);
    }
    READABLE_FUNCTION_WAITS(saveCareerProgress, Verified);

    namespace
    {
        constexpr uint32_t kSetVariable = sym::setVariable;         // (widget, name, value *), by address: hooked
        constexpr uint32_t kRunScript = sym::runScript;           // (widget, text, 0), where its result is ours

        constexpr uint32_t kSkaterRecordHeads = sym::g_skaterRecords;
        inline ee::Global<uint8_t[3], sym::g_masterVolume> g_volumes;  // master, music, sound
        constexpr uint32_t kSavedSkaterLevels = 10;            // ten of the fourteen levels are saved
        constexpr uint32_t kGoodLoad = 2;                      // the load's result
        constexpr uint32_t kZguiGlobals = sym::g_zgui;

        constexpr uint32_t kFrontEndNode = sym::g_textFrontEnd;        // "FrontEnd"
        constexpr ee::Ptr<const char> kMovieNameName{sym::g_textMovieName};       // "MovieName"
        constexpr ee::Ptr<const char> kLoopMovieName{sym::g_textLoopMovie};       // "LoopMovie"
        constexpr uint32_t kBackgroundMovie = sym::g_textAssetsMoviesBackgrndPsm;     // "assets\\Movies\\BACKGRND.PSM"
        constexpr ee::Ptr<const char> kMovieEndScriptName{sym::g_textOnMovieEndScript};  // "OnMovieEndScript"
        constexpr uint32_t kMovieEndScript = sym::movieEndScript;      // ""
        constexpr uint32_t kPlayMovieScript = sym::g_textCallInlineMenuPlayMovieMovie;     // "CALL inlineMenuPlayMovie Movie"
        constexpr uint32_t kFocusOptionsScript = sym::g_textSetfocusBackgroundOptionsMenu;  // "SETFOCUS Background.Options.Menu.MemCard.Options"
        constexpr uint32_t kBackToBeginScript = sym::g_textPlayFrontEndBackgroundToBegin;   // "PLAY FrontEnd.Background.ToBegin.KeySniffer.OnStart"
        constexpr uint32_t kFocusGameOverScript = sym::g_textSetfocusInGameUIPauseMenus2; // "SETFOCUS InGameUI.PauseMenus.Death.GameOver.Menu"
        constexpr uint32_t kFocusPauseScript = sym::g_textSetfocusInGameUIPauseMenus3;    // "SETFOCUS InGameUI.PauseMenus.Frame.Pause.Menu"
        constexpr uint32_t kUnlockInputScript = sym::g_textUnlockinput2;   // "UNLOCKINPUT 0"
        constexpr uint32_t kLockInputScript = sym::g_textLockinput5;     // "LOCKINPUT 1"

        constexpr uint32_t kSaveLoadNode = sym::g_textSaveLoad;        // "SaveLoad"
        constexpr uint32_t kSaveLoadFile = sym::g_textSaveLoadZsc;        // "SaveLoad.zsc"
        constexpr uint32_t kSaveUiFrontEnd = sym::g_textFrontEnd2, kSaveUiInGame = sym::g_textInGame, kSaveUiParkEditor = sym::g_textParkEditor;
        constexpr uint32_t kSaveLoadWidthName = sym::saveLoadWidthName;   // "SaveLoad.ScreenWidth"
        constexpr uint32_t kSaveLoadHeightName = sym::g_textSaveLoadScreenHeight;
        constexpr uint32_t kSaveLoadGameModeName = sym::g_textSaveLoadGameMode;
        constexpr uint32_t kSaveLoadDisplayModeName = sym::g_textSaveLoadDisplayMode;
        constexpr uint32_t kOnInitScript = sym::g_textPlaySaveLoadOnInit;        // "PLAY SaveLoad.OnInit"
        constexpr uint32_t kMenuOnInitScript = sym::g_textPlaySaveLoadSaveLoadMenuOnInit;    // "PLAY SaveLoad.SaveLoadMenu.OnInit"
        constexpr uint32_t kMemsetLibc = sym::memset;

        inline ee::Global<int32_t, sym::g_topStateNext> g_topStateNext;
        inline ee::Global<uint32_t, sym::g_topState> g_topStateRecord;
    }

    // restoreGameSave: the client's finish: a read save copied into the game
    // (modes Load and SilentLoad): the unlocks, then skaters 1 .. 19's heads
    // and ten levels' words and heap blocks, and the sound told the volumes.
    // Then the save's memory given back and the way out by the top state.
    int32_t restoreGameSave(uint32_t result)
    {
        ee::Local<int32_t> value;
        const auto run = [&](ee::Ptr<zgui::Node> widget, uint32_t script) {
            zgui::runScript(widget, ee::Ptr<char>(script), 0);
        };

        const GameSaveMode mode = *g_gameSaveMode;
        bool playShutdown = true;
        if ((mode == GameSaveMode::Load || mode == GameSaveMode::SilentLoad) && result == kGoodLoad)
        {
            uint32_t cursor = *g_gameSaveData;
            const auto read = [&](ee::Address to, uint32_t size) {
                zge::memoryCopy(to, cursor, size);
                cursor += size;
            };
            // A block of the level's state; a size of 0 frees what was there.
            const auto readBlock = [&](auto &memory, uint32_t bytes) {
                if (memory)
                {
                    saveRegionFree(memory);
                    memory = {};
                }
                if (bytes != 0)
                {
                    memory = std::remove_reference_t<decltype(memory)>(saveRegionAllocate(bytes));
                    read(memory, bytes);
                }
            };

            read(kUnlocks, kUnlocksBytes);
            for (uint32_t skater = 1; skater < kSkaters; ++skater)
            {
                read(skater * kSkaterRecordSize + kSkaterRecordHeads, kSkaterHeadBytes);
                for (uint32_t level = 0; level < kSavedSkaterLevels; ++level)
                {
                    CareerLevelState &state = *careerLevelState(skater, level);
                    read(&state.objectStatesSize, 4);
                    read(&state.scriptVariablesSize, 4);
                    read(&state.goalsSize, 4);
                    read(&state.propsSize, 4);
                    read(&state.visibilityCount, 4);
                    read(&state.completion, 4);
                    readBlock(state.objectStates, state.objectStatesSize);
                    readBlock(state.scriptVariables, state.scriptVariablesSize);
                    readBlock(state.goals, state.goalsSize);
                    readBlock(state.props, state.propsSize);
                    readBlock(state.visibility, state.visibilityCount);
                }
            }
            // The sound told the three volumes.
            for (uint32_t volume = 0; volume < 3; ++volume)
                soundEvent(0, volume, (*g_volumes)[volume], 0, 0);
        }

        zge::freeMemory(*g_gameSaveData);
        *g_gameSaveData = 0;
        *g_gameSaveDataBytes = 0;
        zge::stopSaveSystem();
        const int32_t next = *g_topStateNext;
        *g_gameLoadPending = 0;

        if (next == kTopStateFrontEnd)
        {
            switch (*g_gameSaveMode)
            {
            case GameSaveMode::Load:
            case GameSaveMode::Save:
            {
                *value = 1;
                const ee::Ptr<zgui::Node> frontEnd = zgui::findNode(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<const char>(kFrontEndNode), 1);
                zgui::setVariable(frontEnd, kMovieNameName, kBackgroundMovie);
                zgui::setVariable(frontEnd, kLoopMovieName, value);
                zgui::setVariable(frontEnd, kMovieEndScriptName, kMovieEndScript);
                run(frontEnd, kPlayMovieScript);
                run(frontEnd, kFocusOptionsScript);
                run(frontEnd, kUnlockInputScript);
                run(frontEnd, kLockInputScript);
                break;
            }
            case GameSaveMode::AutoLoad:
                startGameSilentLoad();
                playShutdown = false;
                break;
            case GameSaveMode::SilentLoad:
                run(*zgui::g_root, kBackToBeginScript);
                run(*zgui::g_root, kUnlockInputScript);
                run(*zgui::g_root, kLockInputScript);
                break;
            default:
                break;
            }
        }
        else if (next == kTopStateLevel)
        {
            const GameSaveMode modeNow = *g_gameSaveMode;
            if (modeNow == GameSaveMode::Load)
            {
                if (result == kGoodLoad)
                {
                    setPendingStateAction(0);
                    setPendingStateDelay(1);
                    switchGameState(kTopStateWait);
                }
                else
                    run(*zgui::g_root, kFocusGameOverScript);
            }
            else if (modeNow == GameSaveMode::Save)
                run(*zgui::g_root, kFocusPauseScript);
            run(*zgui::g_root, kUnlockInputScript);
            run(*zgui::g_root, kLockInputScript);
        }

        if (*g_gameSaveRan == 0)
            return playShutdown ? 1 : 0;
        if (!playShutdown)
            return 0;
        const int32_t answer = playSaveShutdown();
        *g_gameSaveRan = 0;
        return answer;
    }
    READABLE_FUNCTION_WAITS(restoreGameSave, Verified);

    namespace
    {
        void memset(ee::Ptr<uint8_t> m, uint32_t c, uint32_t n) { ee::game(kMemsetLibc, m, c, n); }
    }

    namespace
    {
        int32_t runRootScript(uint32_t script) { return ee::game<int32_t>(kRunScript, 0, script, 0); }
    }

    // openSaveUi: the save and load screens over whatever is running: its
    // script, the screen's size and mode for it, the menus' script functions.
    int32_t openSaveUi(uint32_t displayMode)
    {
        ee::Local<uint8_t[0x20]> modeName;
        ee::Local<uint32_t[1]> width;
        ee::Local<uint32_t[1]> height;
        ee::Local<uint32_t[1]> display;
        *display = displayMode;
        *width = 512;
        *height = 448;
        memset(modeName, 0, 0x20);
        const ee::Ptr<zgui::Node> saveLoad = zgui::findNode(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<const char>(kSaveLoadNode), 1);
        if (!saveLoad)
            zge::zgui::loadZguiScript(kSaveLoadFile, *g_uiRoot);
        const auto wideScreen = [&](uint32_t name) {
            zge::stringCopy(modeName, name);
            *height = 448;
            *width = 640;
        };
        const int32_t topState = *g_topStateNext;
        if (topState == kTopStateFrontEnd)
            wideScreen(kSaveUiFrontEnd);
        else if (topState == kTopStateLevel)
        {
            const int32_t levelMode = currentLevel()->mode->id;
            if (levelMode == kModePaused)
            {
                zge::stringCopy(modeName, kSaveUiInGame);
                *width = 512;
                *height = 640;
            }
            else if (levelMode == kModeParkEditor)
                wideScreen(kSaveUiParkEditor);
        }
        const auto set = [&](uint32_t name, uint32_t value) {
            zgui::setVariable(*g_uiRoot, name, value);
        };
        set(kSaveLoadWidthName, width.address());
        set(kSaveLoadHeightName, height.address());
        set(kSaveLoadGameModeName, modeName.address());
        set(kSaveLoadDisplayModeName, display.address());
        const auto native = [&](uint32_t name, uint32_t function) {
            zge::zgui::registerScriptFunction(name, function);
        };
        native(sym::g_textInlineMemCardOnUserCancel, sym::cancelParkSaveMenu); // "InlineMemCard.OnUserCancel"
        native(sym::g_textInlineMemCardOnUserSelection, sym::answerSaveMenuFromVariable); // "InlineMemCard.OnUserSelection"
        native(sym::g_textInlineMemCardOnUserFileSelection, sym::inlineMemCardOnUserFileSelection); // "InlineMemCard.OnUserFileSelection"
        native(sym::g_textInlineMemCardOnUserSetDescription, sym::inlineMemCardOnUserSetDescription); // "InlineMemCard.OnUserSetDescription"
        native(sym::g_textInlineMemCardOnUserSetName, sym::inlineMemCardOnUserSetName); // "InlineMemCard.OnUserSetName"
        int32_t result = runRootScript(kOnInitScript);
        if (!saveLoad)
            result = runRootScript(kMenuOnInitScript);
        return result;
    }
    READABLE_FUNCTION_WAITS(openSaveUi, Verified);

    namespace
    {
        constexpr uint32_t kTextPlaySaveLoadOnShutdown = sym::g_textPlaySaveLoadOnShutdown;
        inline ee::Global<int32_t, sym::g_parkSaveClient> g_parkSaveClient;
        inline ee::Global<int32_t, sym::g_parkSaveBuffer> g_parkSaveBuffer;
        inline ee::Global<int32_t, sym::g_parkSaveUnderway> g_parkSaveUnderway; // g_parkSaveUnderway; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_parkSaving> g_parkFileMenuShown; // g_parkFileMenuShown; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_parkSaveFileCount> g_parkSaveFileCount; // g_parkSaveFileCount; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_parkFileList> g_parkFileList;
        inline ee::Global<int32_t, sym::g_parkSaveIcon> g_parkSaveIcon;
        inline ee::Global<int32_t, sym::g_parkSaveIconSize> g_parkSaveIconSize; // g_parkSaveIconSize; its name is a low-confidence guess (readable_names.tsv)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(runScriptTailint32_t, ee::tailCall<int32_t>, kRunScript);
            GAME_TABLE_ROUTE(runScriptint32_t, ee::game<int32_t>, kRunScript);
        }
    }

    int32_t playSaveShutdown()
    {
        return routed::runScriptTailint32_t(0, kTextPlaySaveLoadOnShutdown, 0);
    }
    READABLE_FUNCTION_WAITS(playSaveShutdown, Verified);

    int32_t stopParkSaveSystem()
    {
        return game::tail(zge::stopSaveSystem);
    }
    READABLE_FUNCTION_WAITS(stopParkSaveSystem, Verified);

    int32_t cancelParkSaveMenu()
    {
        return game::tail(zge::cancelSaveMenu);
    }
    READABLE_FUNCTION(cancelParkSaveMenu, Verified);

    int32_t freeParkSaveIcon()
    {
        const int32_t heapFreeResult = zge::heapFree(*g_parkSaveIcon);
        *g_parkSaveIcon = 0;
        *g_parkSaveIconSize = 0;
        return heapFreeResult;
    }
    READABLE_FUNCTION_WAITS(freeParkSaveIcon, Verified);
}
