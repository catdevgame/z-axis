// BMX XXX's game code (bmx.c's park and game saves): Aggressive Inline's games/aggressive/game_save.cpp, copied for
// BMX XXX: the functions the two games have word for word. The game's save and load started, and the save
// menus' ends. The save client itself (its data, its registration, the save menu's
// opening) is BMX XXX's own, and so is what the save holds (writeGameSave, readGameSave: the saved game
// with the riders' models, and the saved records).

#include "bmx/save/game_save.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "bmx/profile/players.h"
#include "bmx/save/park_save.h"
#include "bmx/overlay/screens.h"
#include "bmx/sound/events.h"
#include "bmx/states/game_state.h"
#include "bmx/ui/menu.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/platform/save_system.h"
#include "zge/scene/model_records.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/save/game_save.cpp.h"

namespace game::bmx
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

        inline ee::Global<int32_t, sym::g_parkSaveIcon> g_parkSaveIcon;
        inline ee::Global<int32_t, sym::g_parkSaveIconSize> g_parkSaveIconSize;

        // Scaffolding (address-call): BMX XXX's game passes saveApiLoad a third word (1) that the engine's shared
        // declaration does not have yet (an engine version difference for the engine lanes).
        int32_t saveApiLoadTail(uint32_t client, uint32_t delayMilliseconds, uint32_t one) { return ee::tailCall<int32_t>(sym::saveApiLoad, client, delayMilliseconds, one); }
        // A root script run where the original hands on what runScript (void) left in its result register: called
        // at its address, as Aggressive Inline's openSaveUi and playSaveShutdown do.
        constexpr uint32_t kRunScript = sym::runScript; // (widget, text, 0)
        int32_t runScriptTail(uint32_t widget, uint32_t text, uint32_t argument) { return ee::tailCall<int32_t>(kRunScript, widget, text, argument); }
        int32_t runRootScript(uint32_t text) { return ee::game<int32_t>(kRunScript, 0, text, 0); }
        // A script run on a widget.
        void runScript(ee::Ptr<zge::zgui::Node> widget, uint32_t text) { zge::zgui::runScript(widget, ee::Ptr<char>(text), 0); }
        void fillBytes(ee::Address memory, uint32_t value, uint32_t bytes) { ee::game(sym::memset, memory, value, bytes); }
        inline ee::Global<uint32_t, sym::g_uiRoot> g_uiRoot;

        // Scaffolding (address-call): the original's tail jump to the menu machine's message sending
        // (bmxuiHandler), with the word it passes.
        int32_t sendMenuMessageTail() { return ee::tailCall<int32_t>(sym::bmxuiHandler, 0); }
        // The menu machine's message, set before its sending.
        inline ee::Global<int32_t, sym::g_msgType> g_menuMessage;
        constexpr int32_t kMenuCameIn = 11; // the menu machine's message: the coming in is over
        constexpr int32_t kMenuMessageNone = 0;
        constexpr uint32_t kScreenAfterLoad = 0x16; // which screen: not named yet

        // The kinds, and each kind's archive ("assets\create\parts.zal", lattice, skeleton, textureA .. D).
        constexpr uint32_t kModelParts = 0, kModelLattices = 1, kModelSkeletons = 2, kModelTextureA = 3, kModelTextureB = 4,
                           kModelTextureC = 5, kModelTextureD = 6, kModelKinds = 7;
        inline ee::Global<ee::Ptr<const char>[kModelKinds], sym::g_modelKindFiles> g_modelKindFiles;

        constexpr uint32_t kSavedRecordsBytes = 0x1180;
        // The characters' table of the game (g_characterNames), a row a character: the name of
        // its model is the row's last word.
        struct CharacterRow
        {
            uint8_t _00[0x10];
            ee::Ptr<const char> modelName; // +0x10 "Jr. Shabadoo"
        };
        static_assert(sizeof(CharacterRow) == 0x14);
        inline ee::Global<CharacterRow[kRiderCharacters], sym::g_characterNames> g_characterRows;

        // A save's buffer: its version, the saved game, the saved records.
        constexpr uint8_t kGameSaveVersion = 2;
        struct GameSaveFile
        {
            uint8_t version;                    // +0x00000
            uint8_t _00001[3];
            SavedGame game;                     // +0x00004
            uint8_t records[kSavedRecordsBytes]; // +0x22000
        };
        static_assert(offsetof(GameSaveFile, records) == 0x22000 && sizeof(GameSaveFile) == 0x23180);

        // soundEvent's volume event, and its channels.
        constexpr uint32_t kSetVolume = 0;
        constexpr uint32_t kVolumeSfx = 0, kVolumeMusic = 1, kVolumeSpeech = 2;
    }

    namespace
    {
        constexpr uint32_t kGameSaveBytes = 0x23180; // the save's buffer (a save file's bytes after its byte sum)
        inline ee::Global<uint32_t, sym::g_gameSaveData> g_gameSaveData;
        inline ee::Global<uint32_t, sym::g_gameSaveIcon> g_gameSaveIcon;
        inline ee::Global<uint32_t, sym::g_gameSaveIconSize> g_gameSaveIconSize;
        inline ee::Global<uint8_t, sym::g_gameSaveRan> g_gameSaveRan; // a save or load ran (its menu's end at shutdown)

        // The save screens' display modes (openSaveUi).
        constexpr uint32_t kSaveDisplay = 0, kLoadDisplay = 1, kQuietLoadDisplay = 2;
        constexpr uint32_t kSaveUiWidth = 640, kSaveUiHeight = 448;
    }

    // ---- the save screens ------------------------------------------------------

    // openSaveUi: the save and load screens over whatever is running: their script read if it
    // is not there yet, the screen's size and the mode for it, the menus' script functions.
    int32_t openSaveUi(uint32_t displayMode)
    {
        ee::Local<char[0x20]> modeName; // empty: BMX XXX's screens have one look
        ee::Local<uint32_t> width;
        ee::Local<uint32_t> height;
        ee::Local<uint32_t> display;
        *display = displayMode;
        *width = kSaveUiWidth;
        *height = kSaveUiHeight;
        fillBytes(modeName, 0, 0x20);
        const ee::Ptr<zge::zgui::Node> saveLoad = zge::zgui::findNode(ee::Ptr<zge::zgui::Node>(*g_uiRoot), sym::g_textSaveLoad, 1);
        if (!saveLoad)
            zge::zgui::loadZguiScript(sym::g_textSaveLoadZsc, ee::Ptr<zge::zgui::Node>(*g_uiRoot));
        zge::zgui::setVariable(*g_uiRoot, sym::saveLoadWidthName, width);
        zge::zgui::setVariable(*g_uiRoot, sym::g_textSaveLoadScreenHeight, height);
        zge::zgui::setVariable(*g_uiRoot, sym::g_textSaveLoadGameMode, modeName);
        zge::zgui::setVariable(*g_uiRoot, sym::g_textSaveLoadDisplayMode, display);
        zge::zgui::registerScriptFunction(sym::g_textInlineMemCardOnUserCancel, sym::cancelParkSaveMenu); // "parkEditorMemCardOnUserCancel"
        zge::zgui::registerScriptFunction(sym::g_textInlineMemCardOnUserSelection, sym::answerSaveMenuFromVariable);
        zge::zgui::registerScriptFunction(sym::g_textInlineMemCardOnUserFileSelection, sym::inlineMemCardOnUserFileSelection);
        zge::zgui::registerScriptFunction(sym::g_textInlineMemCardOnUserSetDescription, sym::inlineMemCardOnUserSetDescription);
        zge::zgui::registerScriptFunction(sym::g_textInlineMemCardOnUserSetName, sym::inlineMemCardOnUserSetName);
        int32_t result = runRootScript(sym::g_textPlaySaveLoadOnInit);
        if (!saveLoad)
            result = runRootScript(sym::g_textPlaySaveLoadSaveLoadMenuOnInit);
        return result;
    }
    READABLE_FUNCTION_WAITS(openSaveUi, Verified);

    // ---- the game's save client --------------------------------------------------

    // The client's methods.
    uint32_t gameSaveDataSize() { return kGameSaveBytes; }

    uint32_t writeGameSave(ee::Address buffer)
    {
        const ee::Ptr<GameSaveFile> file(buffer);
        for (uint32_t character = 0; character < kRiderCharacters; ++character)
        {
            RiderCharacter &rider = g_savedGame->characters[character];
            SavedRiderModel &saved = rider.savedModel;
            zge::memoryFill(&saved, 0, sizeof(SavedRiderModel));
            if (!rider.model)
                continue;
            zge::listModelKindEntries(rider.model, saved.partNames[0], saved.partNumbers, kModelParts);
            zge::listModelKindEntries(rider.model, saved.latticeNames[0], saved.latticeNumbers, kModelLattices);
            zge::listModelKindEntries(rider.model, saved.skeletonNames[0], saved.skeletonNumbers, kModelSkeletons);
            zge::listModelKindEntries(rider.model, saved.textureANames[0], saved.textureANumbers, kModelTextureA);
            zge::listModelKindEntries(rider.model, saved.textureBNames[0], saved.textureBNumbers, kModelTextureB);
            zge::listModelKindEntries(rider.model, saved.textureCNames[0], saved.textureCNumbers, kModelTextureC);
            zge::listModelKindEntries(rider.model, saved.textureDNames[0], saved.textureDNumbers, kModelTextureD);
            for (uint8_t weight = 0; weight < kModelWeights; ++weight)
                saved.weights[weight] = zge::modelWeight(rider.model, weight);
        }
        file->version = kGameSaveVersion;
        zge::memoryCopy(&file->game, sym::g_savedGame, sizeof(SavedGame));
        zge::memoryCopy(file->records, sym::g_savedRecords, kSavedRecordsBytes);
        return 1;
    }
    READABLE_FUNCTION_WAITS(writeGameSave, Verified);

    uint32_t readGameSave(ee::Address buffer)
    {
        const ee::Ptr<GameSaveFile> file(buffer);
        if (file->version != kGameSaveVersion)
            return 0;
        zge::memoryCopy(sym::g_savedGame, &file->game, sizeof(SavedGame));
        zge::memoryCopy(sym::g_savedRecords, file->records, kSavedRecordsBytes);
        // Each rider's model found by its name and made what the save says.
        for (uint32_t character = 0; character < kRiderCharacters; ++character)
        {
            const ee::Ptr<zge::ModelRecord> model = zge::findModelByName((*g_characterRows)[character].modelName);
            RiderCharacter &rider = g_savedGame->characters[character];
            SavedRiderModel &saved = rider.savedModel;
            rider.model = model;
            zge::setModelKindEntries(model, saved.partNames[0], saved.partNumbers, kModelParts, (*g_modelKindFiles)[kModelParts], kSavedParts);
            zge::setModelKindEntries(rider.model, saved.latticeNames[0], saved.latticeNumbers, kModelLattices, (*g_modelKindFiles)[kModelLattices], kSavedLattices);
            zge::setModelKindEntries(rider.model, saved.skeletonNames[0], saved.skeletonNumbers, kModelSkeletons, (*g_modelKindFiles)[kModelSkeletons], kSavedSkeletons);
            zge::setModelKindEntries(rider.model, saved.textureANames[0], saved.textureANumbers, kModelTextureA, (*g_modelKindFiles)[kModelTextureA], kSavedTextures);
            zge::setModelKindEntries(rider.model, saved.textureBNames[0], saved.textureBNumbers, kModelTextureB, (*g_modelKindFiles)[kModelTextureB], kSavedTextures);
            zge::setModelKindEntries(rider.model, saved.textureCNames[0], saved.textureCNumbers, kModelTextureC, (*g_modelKindFiles)[kModelTextureC], kSavedTextures);
            zge::setModelKindEntries(rider.model, saved.textureDNames[0], saved.textureDNumbers, kModelTextureD, (*g_modelKindFiles)[kModelTextureD], kSavedTextures);
            for (uint8_t weight = 0; weight < kModelWeights; ++weight)
                zge::setModelWeight(rider.model, weight, saved.weights[weight]);
        }
        // The saved volumes go to the sound channels (no test sound).
        soundEvent(kSetVolume, kVolumeSfx, g_savedGame->channelVolumes[kVolumeSfx], 0, 0);
        soundEvent(kSetVolume, kVolumeSpeech, g_savedGame->channelVolumes[kVolumeSpeech], 0, 0);
        soundEvent(kSetVolume, kVolumeMusic, g_savedGame->channelVolumes[kVolumeMusic], 0, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readGameSave, Verified);
    READABLE_FUNCTION(gameSaveDataSize, Verified);
    uint32_t gameSaveSingleFile() { return 1; } // (the park's answers 0)
    READABLE_FUNCTION(gameSaveSingleFile, Verified);
    uint32_t gameSaveData() { return *g_gameSaveData; }
    READABLE_FUNCTION(gameSaveData, Verified);
    uint32_t gameSaveTitle() { return sym::g_textGameSaveTitle; } // "BMX XXX Save"
    READABLE_FUNCTION(gameSaveTitle, Verified);
    uint32_t gameSaveFileName() { return sym::g_textGameSaveFileName; } // "BASLUS-20415BMXXXXGM"
    READABLE_FUNCTION(gameSaveFileName, Verified);
    uint32_t gameSaveIconSize() { return *g_gameSaveIconSize; }
    READABLE_FUNCTION(gameSaveIconSize, Verified);
    uint32_t gameSaveIcon() { return *g_gameSaveIcon; }
    READABLE_FUNCTION(gameSaveIcon, Verified);

    // gameSavePrepare: the client's prepare: the save's buffer, in a save filled with the
    // game's records; then the save screens for the mode. In the menus' state the menu machine
    // is told its menu has come in.
    int32_t gameSavePrepare()
    {
        const uint32_t data = zge::allocateMemory(kGameSaveBytes);
        *g_gameSaveData = data;
        if (*g_gameSaveMode == GameSaveMode::Save)
            writeGameSave(data);
        switch (*g_gameSaveMode)
        {
        case GameSaveMode::Load:
            openSaveUi(kLoadDisplay);
            break;
        case GameSaveMode::AutoLoad:
        case GameSaveMode::SilentLoad:
            openSaveUi(kQuietLoadDisplay);
            break;
        case GameSaveMode::Save:
            openSaveUi(kSaveDisplay);
            break;
        default:
            break;
        }
        *g_gameSaveRan = 1;
        const uint8_t state = gameState();
        if (state != kGameStateFrontEnd)
            return state;
        *g_menuMessage = kMenuCameIn;
        return sendMenuMessageTail();
    }
    READABLE_FUNCTION_WAITS(gameSavePrepare, Verified);

    // restoreGameSave: the client's finish: a save that was read goes into the game (the
    // loads asked for by the player and the quiet one), the buffer is freed and the save
    // system stopped. Then by where the game is: in the menus' state an automatic load goes on
    // to the quiet load, and a save or load from the menus gives the menu its input back and
    // tells the menu machine; in a level the menu is dropped and the screen after a load
    // shown. The save screens' shutdown plays unless a load follows.
    void restoreGameSave(int32_t result)
    {
        ee::Local<Menu> noMenu;
        bool ended = true;
        const GameSaveMode endedMode = *g_gameSaveMode;
        if ((endedMode == GameSaveMode::Load || endedMode == GameSaveMode::SilentLoad) && result == zge::kMessageDone)
            readGameSave(*g_gameSaveData);
        zge::freeMemory(*g_gameSaveData);
        *g_gameSaveData = 0;
        zge::stopSaveSystem();
        *g_gameLoadPending = 0;
        const uint8_t state = gameState();
        if (state == kGameStateFrontEnd)
        {
            const GameSaveMode mode = *g_gameSaveMode;
            if (mode == GameSaveMode::AutoLoad)
            {
                ended = false;
                startGameSilentLoad();
            }
            else if (mode == GameSaveMode::Load || mode == GameSaveMode::Save)
            {
                const ee::Ptr<zge::zgui::Node> menu = zge::zgui::findNode(ee::Ptr<zge::zgui::Node>(*g_uiRoot), sym::g_textUiMenu, 1); // "UI_menu"
                runScript(menu, sym::g_textKillFocus);     // "KILLFOCUS"
                runScript(menu, sym::g_textUnlockInput0);  // "UNLOCKINPUT 0"
                runScript(menu, sym::g_textLockInput1);    // "LOCKINPUT 1"
            }
            if (ended)
            {
                *g_menuMessage = kMenuMessageNone;
                bmxuiHandler();
            }
        }
        else if (state == kGameStateLevel)
        {
            noMenu->data = 0;
            noMenu->handler = 0;
            noMenu->id = kNoMenu;
            setMenu(noMenu);
            menuFrame();
            showGameScreen(kScreenAfterLoad);
        }
        if (*g_gameSaveRan && ended)
        {
            playSaveShutdown();
            *g_gameSaveRan = 0;
        }
    }
    READABLE_FUNCTION_WAITS(restoreGameSave, Verified);

    // registerGameSaveClient (the game's start): the game's save client registered (idle),
    // its icon read from assets\icon.dat into the heap.
    void registerGameSaveClient()
    {
        // The original never sets listBufferBytes or saveFound: they stay what the stack held.
        ee::Local<zge::SaveClient> client;
        ee::Local<uint32_t> iconBytes;
        client->dataSize = sym::gameSaveDataSize;
        client->showChoice = sym::showSaveChoice;
        client->showMessage = sym::showSaveMessage;
        client->data = sym::gameSaveData;
        client->title = sym::gameSaveTitle;
        client->singleFile = sym::gameSaveSingleFile;
        client->iconSize = sym::gameSaveIconSize;
        client->icon = sym::gameSaveIcon;
        client->fileName = sym::gameSaveFileName;
        client->finished = sym::restoreGameSave;
        client->prepare = sym::gameSavePrepare;
        client->showSlotMenu = sym::showSaveSlotMenu;
        zge::clearSaveClientLeadWord(*client);
        client->showFileMenu = 0;
        client->message13 = 0;
        *g_gameSaveClient = zge::registerSaveClient(client);
        *g_gameSaveMode = GameSaveMode::Idle;
        *g_gameSaveData = 0;
        const uint32_t file = zge::loadFile(sym::g_textGameSaveIconPath, iconBytes);
        *g_gameSaveIconSize = *iconBytes;
        const uint32_t icon = zge::heapAllocate(*iconBytes);
        *g_gameSaveIcon = icon;
        zge::memoryCopy(icon, file, *iconBytes);
        zge::freeLoadedFile(file);
    }
    READABLE_FUNCTION_WAITS(registerGameSaveClient, Verified);

    // shutdownGameSave (the game's shutdown): the save system stopped after a load, and the
    // save screens' end when one ran.
    void shutdownGameSave()
    {
        if (*g_gameLoadPending)
        {
            zge::stopSaveSystem();
            *g_gameLoadPending = 0;
        }
        if (*g_gameSaveRan)
        {
            playSaveShutdown();
            *g_gameSaveRan = 0;
        }
    }
    READABLE_FUNCTION_WAITS(shutdownGameSave, Verified);

    // ---- a save or load started ------------------------------------------------

    // startGameSave: the game saved.
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
        return saveApiLoadTail(client, kJobDelayMilliseconds, 1);
    }
    READABLE_FUNCTION_WAITS(startGameLoad, Verified);

    // startGameSilentLoad: a load without the save API's job value.
    int32_t startGameSilentLoad()
    {
        *g_gameSaveMode = GameSaveMode::SilentLoad;
        zge::startSaveSystem();
        const uint32_t client = *g_gameSaveClient;
        *g_gameLoadPending = 1;
        return game::tail(zge::saveApiSilentLoad, client, 0);
    }
    READABLE_FUNCTION_WAITS(startGameSilentLoad, Verified);

    // startGameAutoLoad: the automatic load.
    int32_t startGameAutoLoad()
    {
        *g_gameSaveMode = GameSaveMode::AutoLoad;
        zge::startSaveSystem();
        const uint32_t client = *g_gameSaveClient;
        *g_gameLoadPending = 1;
        return game::tail(zge::saveApiAutoLoad, client, kJobDelayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startGameAutoLoad, Verified);

    // The SaveLoad pages' shutdown script played.
    int32_t playSaveShutdown()
    {
        return runScriptTail(0, sym::g_textPlaySaveLoadOnShutdown, 0);
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
