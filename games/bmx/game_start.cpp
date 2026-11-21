// BMX XXX's start and end as a game (game_start.h): BMX XXX's own code (bmx.c).

#include "game_start.h"

#include "bmx/create/create_a_model.h"
#include "bmx/create/part_locks.h"
#include "bmx/input/pads.h"
#include "bmx/objectives/objectives.h"
#include "bmx/profile/players.h"
#include "bmx/profile/records.h"
#include "bmx/save/game_save.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/states/game_state.h"
#include "bmx/ui/strings.h"
#include "bmx/ui/ui_system.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/file.h"
#include "zge/platform/platform.h"
#include "zge/text/strings.h"
#include "zge/platform/tasks.h"
#include "zge/render/display.h"
#include "zge/scene/animator.h"
#include "zge/scene/cut_scenes.h"
#include "zge/scene/fast_paths.h"
#include "zge/scene/model_records.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/bmx/game_start.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The loading picture, 512 by 448 at 32 bits a pixel.
        constexpr ee::Ptr<const char> kLoadingPicture{sym::g_textLoadingPicture}; // "assets\\loadingN.raw"
        constexpr uint32_t kPictureWidth = 512, kPictureHeight = 448, kPictureBits = 32;

        // A level's records as a new game has them.
        constexpr ee::Ptr<LevelSavedRecords> kNewLevelRecords{sym::g_newLevelRecords};
        // The movies: each one's name, and whether a new game has it open
        // (bit 0 of its flags).
        struct MovieRow
        {
            ee::Ptr<const char> name;
            uint32_t flags;
        };
        inline ee::Global<MovieRow[kSavedMovies], sym::g_movieRows> g_movieRows;
        // A new game's volumes: effects, music, speech.
        constexpr uint8_t kNewSfxVolume = 0x80, kNewMusicVolume = 0x40, kNewSpeechVolume = 0x80;
        constexpr uint32_t kNewPlayerName = sym::bmx::g_textNewPlayerName; // "player"

        // The level scripts' commands the game adds to the engine's.
        struct ScriptCommand
        {
            uint32_t name, function;
        };
        constexpr ScriptCommand kScriptCommands[] = {
            {sym::g_textHideObjCommand, sym::hideObjectCommand},            // "HIDE_OBJ"
            {sym::g_textShowObjCommand, sym::showObjectCommand},            // "SHOW_OBJ"
            {sym::g_textParticleCommand, sym::particleCommand},             // "PARTICLE"
            {sym::g_textHideCategoryCommand, sym::hideCategoryCommand},     // "HIDE_CATEGORY"
            {sym::g_textShowCategoryCommand, sym::showCategoryCommand},     // "SHOW_CATEGORY"
            {sym::g_textEntityEnableCommand, sym::entityEnableCommand},     // "ENTITY_ENABLE"
            {sym::g_textEntityDisableCommand, sym::entityDisableCommand},   // "ENTITY_DISABLE"
            {sym::g_textEntityActivateCommand, sym::entityActivateCommand}, // "ENTITY_ACTIVATE"
            {sym::g_textSubtitleCommand, sym::subtitleEvent},               // "SUBTITLE"
            {sym::g_textVibrationCommand, sym::vibrationEvent},             // "VIBRATION"
        };

        // The state the game is in (states/game_state.cpp) and the states'
        // exits.
        inline ee::Global<uint8_t, sym::g_currentGameState> g_currentGameState;
        inline ee::Global<ee::FunctionPtr<void()>[kGameStates], sym::g_gameStateExit> g_gameStateExit;
        // The task that draws the profiler's backdrop.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_profilerBackdropTask> g_profilerBackdropTask;
        constexpr uint32_t kBackdropTaskStage = 1;
        constexpr uint32_t kSourceFile = sym::g_textBmxSourceFile; // "./src/com/bmx.c"
        constexpr uint32_t kBackdropTaskLine = 900;
        // The riders' animation archive (the intro's loading opens it).
        inline ee::Global<int32_t, sym::g_riderAnimations> g_riderAnimations;

        // The C library's memset, by address (ee/plain.h).
        // Scaffolding (address-call): a library function, never rewritten.
        void memset(ee::Address dst, uint32_t byte, uint32_t bytes) { ee::game(sym::memset, dst, byte, bytes); }
    }

    namespace
    {
        // The riders' model definitions: the player's ("def.Player" in the
        // create-a-model's defs.zal), the lattices and body shapes of
        // lattice.zal, and the model a character falls back on.
        constexpr ee::Ptr<const char> kDefsLibrary{sym::g_textCreateDefsLibrary};       // "assets\create\defs.zal"
        constexpr uint32_t kPlayerModelDef = sym::g_textPlayerModelDef;                   // "def.Player"
        constexpr ee::Ptr<const char> kLatticeLibrary{sym::g_textCreateLatticeLibrary}; // "assets\create\lattice.zal"
        constexpr uint32_t kLatticeNames[] = {
            sym::g_textFatLattice,    // "fatLattice"
            sym::g_textNormalLattice, // "normalLattice"
            sym::g_textSkinnyLattice, // "skinnyLattice"
            sym::g_textSmallBreast,   // "smallBreast"
            sym::g_textLargeBreast,   // "largeBreast"
            sym::g_textSmallCrotch,   // "smallCrotch"
            sym::g_textLargeCrotch,   // "largeCrotch"
        };
        constexpr ee::Ptr<const char> kDefaultModel{sym::g_textDefaultMaleModel};       // "Default Male"
        // A rider character's model name (the riders' table, by character).
        struct CharacterModelRow
        {
            uint8_t _00[0x10];
            ee::Ptr<const char> modelName; // +0x10 its model script's NAME
        };
        static_assert(sizeof(CharacterModelRow) == 0x14);
        const ee::Ptr<CharacterModelRow> kCharacterModelRows(sym::g_characterNames);

        // Scaffolding (address-call): a library's asset loaded for the
        // models (the engine's loadLibraryAsset, below).
    }
    namespace calls
    {
        inline uint32_t loadLibraryAsset(ee::Ptr<const char> library, ee::Ptr<const char> asset)
        {
            return ee::game<uint32_t>(sym::loadLibraryAsset, library, asset);
        }
    }

    // loadRiderModels: the player's model script read from the defs library,
    // the lattices loaded, each rider character's model found by its name
    // ("Default Male" when there is none), and the parts' lock tables read.
    void loadRiderModels()
    {
        ee::Local<char[0x40]> name;
        const uint32_t defs = zge::openAssetLibrary(kDefsLibrary);
        zge::formatString(name, kPlayerModelDef);
        if (zge::findAsset(defs, zge::nameHash(name)))
        {
            const uint32_t bytes = zge::currentAssetLength(defs);
            const uint32_t script = zge::allocateMemory(bytes);
            zge::readAsset(defs, script, 0);
            const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(script, bytes);
            zge::loadModelScript(ee::Address(file));
            zge::closeFile(file, 0);
            zge::freeMemory(script);
        }
        zge::closeAssetLibrary(defs);
        for (const uint32_t lattice : kLatticeNames)
            calls::loadLibraryAsset(kLatticeLibrary, ee::Ptr<const char>(lattice));
        for (uint32_t character = 0; character < kRiderCharacters; ++character)
        {
            ee::Ptr<zge::ModelRecord> &model = g_savedGame->characters[character].model;
            model = zge::findModelByName((kCharacterModelRows + character)->modelName);
            if (!model)
                model = zge::findModelByName(kDefaultModel);
        }
        loadPartLockTables();
    }
    READABLE_FUNCTION_WAITS(loadRiderModels, Verified);

    // The engine's closer queries' start, which has no declaration yet.
    // Scaffolding (address-call): until it has one (the engine's, in zge).
    namespace calls
    {
        inline void initCloserQueries() { ee::game(sym::initCloserQueries); }
    }

    void initGame()
    {
        ee::Local<uint8_t[4]> black;
        memset(black, 0, 4);
        black[3] = 0xFF;
        zge::resetCameraViewports(kPictureWidth, kPictureHeight);
        zge::showRawImage(kLoadingPicture, kPictureWidth, kPictureHeight, kPictureBits);
        zge::setClearColour(ee::Ptr<uint8_t>(black.address()));

        // A new saved game.
        SavedGame &saved = *g_savedGame;
        zge::memoryFill(sym::g_savedGame, 0, sizeof(SavedGame));
        for (uint32_t level = 0; level < kSavedRecordLevels; ++level)
            zge::memoryCopy(ee::Ptr<LevelSavedRecords>(sym::g_savedRecords) + level, kNewLevelRecords, sizeof(LevelSavedRecords));
        for (uint32_t character = 0; character < kRiderCharacters; ++character)
            resetRiderCharacter(character);
        saved.teamRewards = 1;
        for (uint32_t i = 0; i < 7; ++i)
            saved.teamMembers[i] = 0xFF;
        loadRiderModels();
        for (uint32_t movie = 0; movie < kSavedMovies; ++movie)
            saved.movies[movie] = (saved.movies[movie] & ~kMovieRewardBit0) | ((*g_movieRows)[movie].flags & kMovieRewardBit0);
        saved.editorThemes = 1;
        saved.features = 0;
        saved.channelVolumes[1] = kNewMusicVolume;
        saved.channelVolumes[2] = kNewSpeechVolume;
        saved._00005 = 0;
        saved.channelVolumes[0] = kNewSfxVolume;
        saved.language = static_cast<uint8_t>(defaultLanguage());

        for (uint32_t player = 0; player < kMaxPlayers; ++player)
        {
            PlayerSetup &setup = (*g_playerSetups)[player];
            setup.controllerLayout = 0;
            setup.cameraType = 0;
            setup.flags = setup.flags | kSetupVibration;
            setup.character = 0;
            zge::formatString(ee::Ptr<char>(setup.name), kNewPlayerName);
        }

        zge::initAnimators();
        calls::initCloserQueries();
        zge::initFastPaths();
        for (const ScriptCommand &command : kScriptCommands)
            zge::registerAnimationEventHandler(ee::Ptr<const char>(command.name), command.function, 0);
        objectives::setUpObjectives();
        *g_currentPlayer = 0;
        *g_playerCount = 1;
        initPads();
        setUpUi();
        initCreateAModel();
        *g_currentGameState = kNoGameState;
        setGameState(kGameStateIntro);
        *g_profilerBackdropTask = zge::newTask(kBackdropTaskStage, sym::drawProfilerBackdrop, 0, 1, kSourceFile, kBackdropTaskLine);
    }
    READABLE_FUNCTION_WAITS(initGame, Verified);

    void shutDownGame()
    {
        zge::deleteTask(*g_profilerBackdropTask);
        if (*g_currentGameState < kGameStates)
            (*g_gameStateExit)[*g_currentGameState]();
        for (uint32_t character = 0; character < kRiderCharacters; ++character)
        {
            if (const ee::Ptr<zge::ModelRecord> model = g_savedGame->characters[character].model)
                zge::freeModelRecord(model);
        }
        *g_currentGameState = kNoGameState;
        zge::freeAnimationLibrary(static_cast<uint32_t>(*g_riderAnimations));
        shutDownUi();
        shutdownSound();
        freePads();
        stopParkSaveSystem();
        shutdownGameSave();
        objectives::shutDownObjectives();
        shutDownCreateAModel();
        zge::shutdownAnimators();
        zge::shutDownCutScenes();
        game::tail(zge::shutDownFastPaths);
    }
    READABLE_FUNCTION_WAITS(shutDownGame, Verified);
}
