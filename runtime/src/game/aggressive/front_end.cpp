// The front end: the zgui menu's game functions (the "inlineMenu..." names its
// scripts call), the set-up that registers them and the menu's movie.

#include "zge/movie/movie.h"
#include "aggressive/camera.h"
#include "aggressive/career.h"
#include "aggressive/game_save.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/volume_menu.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/callback.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/input/pads.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/assets/file.h"
#include "zge/render/display.h"
#include "zge/render/texture.h"
#include "zge/scene/scene_database.h"
#include "zge/zgui/widget_parts.h"
#include "sce/libgcc.h"
#include "aggressive/pads.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/particles.h"
#include "aggressive/game_start.h"
#include "aggressive/level/props.h"
#include "aggressive/level/load.h"
#include "aggressive/level/park_buffer.h"
#include "aggressive/level/park_save.h"

#include <cstdint>
#include <cstring>
#include "sym/aggressive/front_end.cpp.h"

namespace zge::zgui { int32_t variableIntValueAgain(ee::Ptr<Node>, ee::Ptr<int32_t>); }

namespace aggressive
{
    namespace
    {
        using zgui::Node;

        // A UI asset, as far as the front end reads one.
        constexpr uint32_t kSetVariable = sym::setVariable;         // (widget, name, value *), by address: a game override hooks it
        constexpr uint32_t kRunScript = sym::runScript;           // (widget, text, 0), by address where its result is ours
        constexpr uint32_t kReleaseFollowCameras = sym::releaseFollowCameras;
        constexpr uint32_t kExitDestination = sym::exitDestination;
        constexpr uint32_t kInlineMenuStopMovie = sym::inlineMenuStopMovie;
        constexpr uint32_t kFindSkaterIndexByName = sym::findSkaterIndexByName;
        constexpr uint32_t kButtonLayoutLabel = sym::buttonLayoutLabel;
        constexpr uint32_t kMemset = sym::memset;              // libc
        constexpr uint32_t kFloatToDouble = sym::fptodp;       // libgcc

        // The script functions' names and handlers registered once .
        struct MenuFunction
        {
            uint32_t name, function, returnsTo;
        };
        constexpr MenuFunction kMenuFunctions[] = {
            {sym::g_textInlineMenuSync, sym::inlineMenuSync, points::registerMenuFunctions::afterRegisterScriptFunction}, // "inlineMenuSync"
            {sym::g_textInlineMenuPopulateSkaterSelectMenu, sym::inlineMenuPopulateSkaterSelectMenu, points::registerMenuFunctions::afterRegisterScriptFunction2}, // "inlineMenuPopulateSkaterSelectMenu"
            {sym::g_textInlineMenuPopulateSkaterStats, sym::inlineMenuPopulateSkaterStats, points::registerMenuFunctions::afterRegisterScriptFunction3}, // "inlineMenuPopulateSkaterStats"
            {sym::g_textInlineMenuNameSkater, sym::inlineMenuNameSkater, points::registerMenuFunctions::afterRegisterScriptFunction4}, // "inlineMenuNameSkater"
            {sym::g_textInlineMenuPopulateLevelData, sym::inlineMenuPopulateLevelData, points::registerMenuFunctions::afterRegisterScriptFunction5}, // "inlineMenuPopulateLevelData"
            {sym::g_textInlineMenuPopulateCheatList, sym::inlineMenuPopulateCheatList, points::registerMenuFunctions::afterRegisterScriptFunction6}, // "inlineMenuPopulateCheatList"
            {sym::g_textInlineMenuStartGame, sym::inlineMenuStartGame, points::registerMenuFunctions::afterRegisterScriptFunction7}, // "inlineMenuStartGame"
            {sym::g_textInlineMenuPlayOnChangeSound, sym::inlineMenuPlayOnChangeSound, points::registerMenuFunctions::afterRegisterScriptFunction8}, // "inlineMenuPlayOnChangeSound"
            {sym::g_textInlineMenuVibrateController, sym::inlineMenuVibrateController, points::registerMenuFunctions::afterRegisterScriptFunction9}, // "inlineMenuVibrateController"
            {sym::g_textInlineMenuGetControllerConfig, sym::inlineMenuGetControllerConfig, points::registerMenuFunctions::afterRegisterScriptFunction10}, // "inlineMenuGetControllerConfig"
            {sym::g_textInlineMenuSetControllerConfig, sym::inlineMenuSetControllerConfig, points::registerMenuFunctions::afterRegisterScriptFunction11}, // "inlineMenuSetControllerConfig"
            {sym::g_textInlineMenuRefreshControllerConfigScreen, sym::inlineMenuRefreshControllerConfigScreen, points::registerMenuFunctions::afterRegisterScriptFunction12}, // "inlineMenuRefreshControllerConfigScreen"
            {sym::g_textInlineMenuSetSFXVolume, sym::inlineMenuSetSFXVolume, points::registerMenuFunctions::afterRegisterScriptFunction13}, // "inlineMenuSetSFXVolume"
            {sym::g_textInlineMenuSetSpeechVolume, sym::inlineMenuSetSpeechVolume, points::registerMenuFunctions::afterRegisterScriptFunction14}, // "inlineMenuSetSpeechVolume"
            {sym::g_textInlineMenuSetMusicVolume, sym::inlineMenuSetMusicVolume, points::registerMenuFunctions::afterRegisterScriptFunction15}, // "inlineMenuSetMusicVolume"
            {sym::g_textInlineMenuGetSFXVolume, sym::inlineMenuGetSFXVolume, points::registerMenuFunctions::afterRegisterScriptFunction16}, // "inlineMenuGetSFXVolume"
            {sym::g_textInlineMenuGetSpeechVolume, sym::inlineMenuGetSpeechVolume, points::registerMenuFunctions::afterRegisterScriptFunction17}, // "inlineMenuGetSpeechVolume"
            {sym::g_textInlineMenuGetMusicVolume, sym::inlineMenuGetMusicVolume, points::registerMenuFunctions::afterRegisterScriptFunction18}, // "inlineMenuGetMusicVolume"
            {sym::g_textInlineMenuStartMusic, sym::inlineMenuStartMusic, points::registerMenuFunctions::afterRegisterScriptFunction19}, // "inlineMenuStartMusic"
            {sym::g_textInlineMenuStopMusic, sym::inlineMenuStopMusic, points::registerMenuFunctions::afterRegisterScriptFunction20}, // "inlineMenuStopMusic"
            {sym::g_textInlineMenuSaveGame, sym::inlineMenuSaveGame, points::registerMenuFunctions::afterRegisterScriptFunction21}, // "inlineMenuSaveGame"
            {sym::g_textInlineMenuLoadGame, sym::inlineMenuLoadGame, points::registerMenuFunctions::afterRegisterScriptFunction22}, // "inlineMenuLoadGame"
            {sym::g_textInlineMenuAutoLoadGame, sym::inlineMenuAutoLoadGame, points::registerMenuFunctions::afterRegisterScriptFunction23}, // "inlineMenuAutoLoadGame"
            {sym::g_textInlineMenuLoadPark, sym::inlineMenuLoadPark, points::registerMenuFunctions::afterRegisterScriptFunction24}, // "inlineMenuLoadPark"
            {sym::g_textInlineMenuPlayPark, sym::inlineMenuPlayPark, points::registerMenuFunctions::afterRegisterScriptFunction25}, // "inlineMenuPlayPark"
            {sym::g_textInlineMenuCheat, sym::inlineMenuCheat, points::registerMenuFunctions::afterRegisterScriptFunction26}, // "inlineMenuCheat"
            {sym::g_textInlineMenuPopulateMediaViewerMenu, sym::inlineMenuPopulateMediaViewerMenu, points::registerMenuFunctions::afterRegisterScriptFunction27}, // "inlineMenuPopulateMediaViewerMenu"
            {sym::g_textInlineMenuPrepareMediaMovie, sym::inlineMenuPrepareMediaMovie, points::registerMenuFunctions::afterRegisterScriptFunction28}, // "inlineMenuPrepareMediaMovie"
            {sym::g_textInlineMenuGetCredits, sym::inlineMenuGetCredits, points::registerMenuFunctions::afterRegisterScriptFunction29}, // "inlineMenuGetCredits"
            {sym::g_textInlineMenuPlayMovie, sym::inlineMenuPlayMovie, points::registerMenuFunctions::afterRegisterScriptFunction30}, // "inlineMenuPlayMovie"
            {sym::g_textInlineMenuStopMovie, sym::inlineMenuStopMovie, points::registerMenuFunctions::afterRegisterScriptFunction31}, // "inlineMenuStopMovie"
        };

        // Strings.
        constexpr uint32_t kMainLoadPicture = sym::mainLoadPicture;     // "ASSETS\Loading\MAINLOAD.RAW"
        constexpr uint32_t kFrontEndScript = sym::frontEndScript;      // "FrontEnd.zsc"
        constexpr uint32_t kSkipToMainMenu = sym::skipToMainMenu;      // "PLAY FrontEnd.SkipToMainMenu"
        constexpr uint32_t kSkipToLevelSelect = sym::skipToLevelSelect;   // "PLAY FrontEnd.SkipToLevelSelect"
        constexpr uint32_t kGameTypeVariable = sym::gameTypeVariable;    // "FrontEnd.GameType"
        constexpr uint32_t kSkaterVariable = sym::skaterVariable;      // "FrontEnd.Skater"
        constexpr uint32_t kSkater2Variable = sym::skater2Variable;     // "FrontEnd.Skater2"
        constexpr uint32_t kLevelVariable = sym::levelVariable;       // "FrontEnd.Level"
        constexpr uint32_t kManyControllersVariable = sym::manyControllersVariable; // "FrontEnd.MoreThanOneContollerIsAttached"
        constexpr uint32_t kShowLoading = sym::showLoading;         // "SET FrontEnd.Loading.Visible {1}"
        constexpr uint32_t kDestroyFrontEnd = sym::destroyFrontEnd;     // " DESTROY FrontEnd"
        constexpr uint32_t kMovieNameVariable = sym::g_textFrontEndMovieName;   // "FrontEnd.MovieName"
        constexpr uint32_t kLoopMovieVariable = sym::loopMovieVariable;   // "FrontEnd.LoopMovie"
        constexpr uint32_t kMovieMaterial = sym::movieMaterial;       // "FrontEnd.MovieMaterial"
        constexpr uint32_t kStillBackground = sym::stillBackground;     // "FrontEnd.still_bkg_01"
        constexpr uint32_t kSetMaterialFormat = sym::setMaterialFormat;   // "SET Material {\"%s\"}"
        constexpr uint32_t kMovieEndVariable = sym::movieEndVariable;    // "FrontEnd.OnMovieEndScript"
        constexpr uint32_t kEmptyString = sym::emptyString;         // ""
        constexpr uint32_t kPlayFormat = sym::playFormat;          // "PLAY %s"
        constexpr uint32_t kUnknownGameType = sym::unknownGameType;     // "Could not find the game type specified by the GUI: '%s'."
        constexpr uint32_t kDefaultSkater = sym::defaultSkater;       // "CE"
        constexpr uint32_t kSkaterSelectorItems = sym::skaterSelectorItems; // "FrontEnd.Background.CharacterSelect.Menu.SkaterSelector.Items"
        constexpr uint32_t kSkaterSelector = sym::skaterSelector;      // "FrontEnd.Background.CharacterSelect.Menu.SkaterSelector"
        constexpr uint32_t kSelectedText = sym::selectedText;        // "SelectedText"
        constexpr uint32_t kNameInputString = sym::nameInputString;     // "NameInput.String"
        constexpr uint32_t kLineBreak = sym::lineBreak2;           // "\n" (two characters: the UI script's escape)
        constexpr uint32_t kCommentMark = sym::commentMark;         // "!!"
        constexpr uint32_t kClearText = sym::clearText;           // "SET Text {\"\"}"
        constexpr uint32_t kDoubleLineBreak = sym::doubleLineBreak;     // "\n\n"
        constexpr uint32_t kCheatLineFormat = sym::cheatLineFormat;     // "STRCAT Text {\"%s%s%s\n%s%s\"}"
        constexpr uint32_t kNoCheatsFormat = sym::noCheatsFormat;      // "SET Text {\"%sNO CHEATS UNLOCKED YET.\"}"
        constexpr uint32_t kCheatNameColour = sym::cheatNameColour;     // "#RGB{83 130 186}"
        constexpr uint32_t kCheatTextColour = sym::cheatTextColour;     // "#RGB{192 192 192}"
        constexpr uint32_t kCurValue = sym::curValue3, kPlayOnChange = sym::playOnChange; // "CurValue", "PLAY OnChange"
        constexpr uint32_t kSelectedIndex = sym::g_textSelectedIndex3, kPreviousIndex = sym::previousIndex; // "SelectedIndex", "PreviousIndex"
        constexpr uint32_t kVibrationIsOn = sym::vibrationIsOn;       // "Vibration.isOn"
        constexpr uint32_t kVibrationRefresh = sym::vibrationRefresh;    // "PLAY Vibration.Refresh"
        constexpr uint32_t kParkDialogMoveIn = sym::parkDialogMoveIn;    // "PLAY Root.FrontEnd.Background.LevelSelect.ParkEditorDialog.MoveIn"
        constexpr uint32_t kLevelSelectMoveIn = sym::levelSelectMoveIn;   // "PLAY Root.FrontEnd.Background.LevelSelect.MoveIn"
        constexpr uint32_t kCreditsFile = sym::creditsFile;         // "ASSETS\Credit.txt"
        constexpr uint32_t kTextVariable = sym::textVariable;        // "Text"

        // What the original's saved registers keep loaded.
        constexpr uint32_t kGameSettings = sym::g_debugDisplay;        // globals.h: the skaters at +1, the type at +3, the level at +4
        constexpr uint32_t kZguiGlobals = sym::g_zgui;
        constexpr uint32_t kPage24 = ee::high(sym::g_zgui), kPage25 = ee::high(sym::g_debugDisplay), kPage26 = ee::high(sym::cheatNameColour), kPage27 = ee::high(sym::emptyString);

        // The game's tables and state.
        constexpr uint32_t kPlayerSkaters = sym::g_playerSkaters;
        constexpr uint32_t kSkaterCodes = sym::skaterCodes;         // 20 two-letter skater codes
        inline ee::Global<uint32_t[20], sym::skaterCodes> g_skaterCodes;
        inline ee::Global<int32_t[20], sym::skaterOrder> g_skaterOrder;
        constexpr uint32_t kGameTypeNames = sym::gameTypeNames;       // by GameType
        constexpr uint32_t kSkaterOrder = sym::skaterOrder;         // the select screen's order, 20 words
        constexpr uint32_t kCheats = sym::cheats;              // 10 Cheat records
        constexpr uint32_t kSkaterProfiles = sym::g_skaterRecords, kSkaterRecordBytes = 0x510u;
        constexpr uint64_t kSkaterNamed = 0x20;                // a skater record's first flags
        constexpr uint32_t kPadConnected = 0x20u;
        constexpr int32_t kNoLevel = 14;                       // kLevelFromName of no level

        struct Cheat
        {
            uint32_t bit;          // +0x00 in g_careerCheats
            char name[0x20];       // +0x04
            char description[0x20]; // +0x24
        };
        static_assert(sizeof(Cheat) == 0x44);
        inline ee::Global<Cheat[10], sym::cheats> g_cheats;

        // Where the front end was left for: exitDestination's values.
        constexpr uint32_t kExitToTitle = 0, kExitToMainMenu = 1, kExitToLevelSelect = 2;
        // The buttons a layout's screen shows, bit by bit (padLayoutOption).
        constexpr int32_t kFirstButton = 0x10, kLastButton = 0x80;
        // soundEvent's volume events: get or set, the channel in `a`.
        constexpr uint32_t kSetVolume = 0, kGetVolume = 1;
        // A park's card data.
        constexpr int32_t kParkBytes = 0x2000;
        constexpr uint32_t kParkLoaded = 2;                    // kLoadFromCard's result
        constexpr int32_t kFirstParkLevel = 10;
        constexpr uint32_t kParkLevelWord = 1;                 // the park's level, from kFirstParkLevel

        inline ee::Global<ee::Float, sym::g_defaultFieldOfView> g_defaultFieldOfView;
        inline ee::Global<ee::Float, sym::g_defaultAspect> g_defaultAspect;
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_editCameraObject> g_editCameraObject;
        inline ee::Global<ee::Callback<int32_t()>, sym::g_changeSoundHook> g_changeSoundHook;
        // A skater record's first flags say it has been given a name.
        uint64_t setSkaterNamed(ee::Ptr<SkaterProfile> profile)
        {
            profile->flagsWord |= kSkaterNamed;
            return profile->flagsWord;
        }
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelNumber;       // a word here; globals.h's g_levelIndex is its low byte
        inline ee::Global<uint32_t, sym::g_vibrateControllerPort> g_vibrateControllerPort;
        inline ee::Global<uint32_t, sym::g_unlockedSkaters> g_unlockedSkaters;  // the low word of g_unlocks
        inline ee::Global<uint32_t, sym::g_careerCheats> g_careerCheats;
        inline ee::Global<uint32_t, sym::g_gameplayCheats> g_gameplayCheats;
        inline ee::Global<uint32_t, sym::g_parkPlayState> g_parkPlayState;    // 1 from the load menu, 2 played from the level select
        inline ee::Global<ee::Ptr<level::ParkBuffer>, sym::g_parkBuffer> g_parkBuffer;
        inline ee::Global<uint64_t, sym::g_sceneBoundsMin> g_sceneBoundsMin;
        inline ee::Global<uint64_t, sym::g_sceneBoundsMinHigh> g_sceneBoundsMinHigh;
        inline ee::Global<uint32_t, sym::g_frontEndMovie> g_frontEndMovie;
        inline ee::Global<uint32_t, sym::g_exitDestination> g_exitDestination;
        inline ee::Global<uint32_t, sym::g_menuFunctionsPending> g_menuFunctionsPending; // 1 until the script functions are registered
        constexpr uint32_t kFirstCameraBytes = 0x280, kScreenHeight = 0x1C0; // 640 x 448
    }

    uint32_t exitDestination();

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
        int32_t runScriptResult(ee::Address widget, uint32_t script) { return ee::game<int32_t>(kRunScript, widget, script, 0); }
    }

    namespace
    {
        constexpr uint32_t kReservedBlockCount = 6;
        inline ee::Global<uint32_t[6], sym::gameTypeNames> g_gameTypeNames;

        // A camera object's camera (sceneOf: the scene word holds a camera).
        ee::Ptr<zge::Camera> cameraOf(ee::Ptr<zge::SceneObject> object) { return zge::sceneOf(object).cast<zge::Camera>(); }

        // The menu's camera: the edit camera object's.
        ee::Ptr<zge::Camera> menuCamera()
        {
            return cameraOf(*g_editCameraObject);
        }
    }

    // registerMenuFunctions: the front end built: six blocks taken from the heap (the
    // sizes keep FrontEnd.zsc's allocations above them) and given back after
    // it loads, the first camera on the menu's scene, the script functions
    // registered the first time, then, by what the game left for, the main
    // menu or the level select.
    uint32_t registerMenuFunctions()
    {
        ee::Local<uint32_t[8]> blockSizes;
        ee::Local<uint32_t[8]> blocks;
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> lookAt;
        ee::Local<uint64_t[2]> sceneDefaults;
        ee::Local<zge::Vec4[2]> sceneExtent;
        ee::Local<ee::Float[4]> viewport;

        if (!*g_menuFunctionsPending)
        {
            zge::resetCameraViewports(kFirstCameraBytes, kScreenHeight);
            zge::showRawImage(kMainLoadPicture, kFirstCameraBytes, kScreenHeight, 24);
        }
        zge::releaseAllEmptyPoolBlocks();

        constexpr uint32_t kReservedBlocks[kReservedBlockCount] = {(1u << 20), 0x00031C80u, (1u << 20), 0x2010u, 0x0005000Cu, 0x00091768u};
        for (uint32_t index = 0; index < kReservedBlockCount; ++index)
            blockSizes[index] = kReservedBlocks[index];
        for (uint32_t index = 0; index < kReservedBlockCount; ++index)
            blocks[index] = zge::heapAllocate(blockSizes[index]);

        // The front end's block, made the first time.
        const ee::Ptr<TopState> topState = g_gameState->topState;
        if (!topState->frontEnd)
            topState->frontEnd = ee::Ptr<FrontEndState>(zge::heapAllocate(sizeof(FrontEndState)));
        topState->frontEnd->clearedAtStart = 0;
        setupFollowCameras(0);
        setFollowCameraMode(&g_gameState->cameras[0], 6);
        *g_parkPlayState = 0;

        // The menu's camera looks 500 units along z in a 200-unit cube.
        memset(eye, 0, 0x10);
        memset(lookAt, 0, 0x10);
        const uint64_t defaultsHigh = *g_sceneBoundsMinHigh;
        sceneDefaults[0] = *g_sceneBoundsMin;
        sceneDefaults[1] = defaultsHigh;
        lookAt->z = ee::Float(500.0f);
        sceneExtent[0].x = ee::Float(-100.0f);
        sceneExtent[0].y = ee::Float(-100.0f);
        sceneExtent[0].z = ee::Float(-100.0f);
        sceneExtent[1].x = ee::Float(100.0f);
        sceneExtent[1].y = ee::Float(100.0f);
        sceneExtent[1].z = ee::Float(100.0f);
        zge::enterUnlistedScene(sceneDefaults.address(), sceneExtent.address());
        *g_editCameraObject = g_gameState->cameras[0].node;
        zge::lookFromAt(g_gameState->cameras[0].node, eye, lookAt, 0);
        if (*g_menuFunctionsPending)
        {
            for (const MenuFunction &function : kMenuFunctions)
                zge::zgui::registerScriptFunction(function.name, function.function);
            *g_menuFunctionsPending = 0;
        }
        zge::movieInit();

        zge::zgui::loadZguiScript(kFrontEndScript, *g_uiRoot);
        for (uint32_t index = 0; index < kReservedBlockCount; ++index)
            zge::heapFree(blocks[index]);

        const uint32_t destination = exitDestination();
        if (destination == kExitToMainMenu)
        {
            setGameMode(1);
            zgui::runScript(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<char>(kSkipToMainMenu), 0);
        }
        else if (destination == kExitToTitle)
            setGameMode(0);
        else if (destination == kExitToLevelSelect)
        {
            setGameMode(1);
            const ee::Ptr<zgui::Node> root = *g_uiRoot;
            zgui::setVariable(root, kGameTypeVariable, (*g_gameTypeNames)[static_cast<uint8_t>(g_gameState->type)]);
            zgui::setVariable(root, kSkaterVariable, (*g_skaterCodes)[g_gameState->playerSkaters[0]]);
            zgui::setVariable(root, kSkater2Variable, (*g_skaterCodes)[g_gameState->playerSkaters[1]]);
            zgui::runScript(ee::Ptr<Node>(root), ee::Ptr<char>(kSkipToLevelSelect), 0);
        }
        aggressive::fx::alwaysZero();
        viewport[0] = 0.0f;
        viewport[1] = 0.0f;
        viewport[2] = ee::Float(640.0f);
        viewport[3] = ee::Float(448.0f);
        zge::resetCameraViewports(kFirstCameraBytes, kScreenHeight);
        zge::setCameraViewport(menuCamera(), viewport.address());
        zge::setCameraScales(menuCamera(), *g_defaultFieldOfView, *g_defaultAspect);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerMenuFunctions, Verified);

    // checkSecondController: tells the scripts whether a second pad is plugged in.
    uint32_t checkSecondController()
    {
        ee::Local<int32_t> many;
        *many = 0;
        if (!currentLevel())
            return 0;
        int32_t connected = 0;
        for (const skater::PadState &pad : g_gameState->pads)
            if (pad.options & kPadConnected)
                ++connected;
        *many = connected >= 2;
        zgui::setVariable(*g_uiRoot, kManyControllersVariable, many);
        return 1;
    }
    READABLE_FUNCTION_WAITS(checkSecondController, Verified);

    void inlineMenuStopMovie();

    namespace
    {
        namespace tail
        {
            // releaseFollowCameras is void: the original returns what it leaves in the result register.
            int32_t releaseFollowCameras() { return ee::tailCall<int32_t>(kReleaseFollowCameras); }
        }
    }

    // leaveFrontEnd: the front end left: the movie stopped, the loading sign
    // on, the UI destroyed.
    int32_t leaveFrontEnd()
    {
        inlineMenuStopMovie();
        zge::movieShutdown();
        zgui::runScript(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<char>(kShowLoading), 0);
        zgui::runScript(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<char>(kDestroyFrontEnd), 0);
        zge::leaveScene();
        return tail::releaseFollowCameras();
    }
    READABLE_FUNCTION_WAITS(leaveFrontEnd, Verified);

    // setExitDestination, exitDestination: where the next front end starts.
    void setExitDestination(uint32_t destination)
    {
        *g_exitDestination = destination;
    }
    READABLE_FUNCTION(setExitDestination, Verified);

    uint32_t exitDestination()
    {
        return *g_exitDestination;
    }
    READABLE_FUNCTION(exitDestination, Verified);

    // inlineMenuPlayMovie: FrontEnd.MovieName played (looping if FrontEnd.LoopMovie)
    // into the movie's material, which every widget of the zero-ended list
    // gets. A movie that will not open leaves them on the still background.
    // Returns what the last call left.
    int32_t inlineMenuPlayMovie(ee::Ptr<uint32_t> widgets)
    {
        ee::Local<char[0x400]> name;
        ee::Local<char[0x100]> command;
        ee::Local<int32_t> loop;
        ee::Local<uint32_t> picture;

        // The material's command run on each widget of the list, 0 at its end.
        const auto runOnWidgets = [&](int32_t result) {
            for (ee::Ptr<uint32_t> widget = widgets; *widget; ++widget)
            {
                zgui::runScript(ee::Ptr<Node>(*widget), ee::Ptr<char>(command.address()), 0);
                result = 0;
            }
            return result;
        };

        memset(name, 0, 0x400);
        const ee::Ptr<zgui::Node> root = *g_uiRoot;
        *loop = 0;
        zgui::copyVariable(ee::Ptr<Node>(root), ee::Ptr<const char>(kMovieNameVariable), ee::Ptr<char>(name));
        zgui::copyVariable(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<const char>(kLoopMovieVariable), ee::Ptr<char>(loop.address()));
        if (const uint32_t playing = *g_frontEndMovie)
        {
            zge::movieStop(playing);
            zge::movieClose(*g_frontEndMovie);
            *g_frontEndMovie = 0;
        }
        int32_t result = zge::movieOpen(name.address());
        *g_frontEndMovie = static_cast<uint32_t>(result);
        if (result)
        {
            zge::setMovieEndCallback(static_cast<uint32_t>(result), sym::runMovieEndScript, 0);
            result = zge::moviePlay(*g_frontEndMovie, *loop);
            if (!widgets)
                return result;
            memset(command, 0, 0x100);
            const ee::Ptr<zgui::UiAsset> asset = zge::zgui::findGuiAsset(kMovieMaterial, 0);
            const ee::Ptr<uint32_t> texture = zge::movieTexture(*g_frontEndMovie, picture);
            zge::setMaterialPicture(ee::Ptr<zge::Material>(asset->object), *texture, 0);
            return runOnWidgets(zge::formatString(command.address(), kSetMaterialFormat, kMovieMaterial));
        }
        if (widgets)
        {
            memset(command, 0, 0x100);
            zge::formatString(command.address(), kSetMaterialFormat, kStillBackground);
            runOnWidgets(0);
            result = zge::movieClose(*g_frontEndMovie);
            *g_frontEndMovie = 0;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(inlineMenuPlayMovie, Verified);

    // inlineMenuStopMovie: the movie stopped and closed. With none, the result
    // is what the caller had.
    void inlineMenuStopMovie()
    {
        if (const uint32_t movie = *g_frontEndMovie)
        {
            *g_frontEndMovie = 0;
            if (zge::movieIsPlaying())
                zge::movieStop(movie);
            zge::movieClose(movie);
        }
    }
    READABLE_FUNCTION_WAITS(inlineMenuStopMovie, Verified);

    // runMovieEndScript: the movie's end callback: at a real end (*status 0) the
    // script FrontEnd.OnMovieEndScript names, played. 1, or 0 if it was
    // stopped.
    uint32_t runMovieEndScript(uint32_t status)
    {
        ee::Local<char> m;
        memset(m, 0, 0x400);
        zgui::copyVariable(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<const char>(kMovieEndVariable), ee::Ptr<char>(m.address()));
        uint32_t result = 0;
        if (!*ee::Ptr<int32_t>(status))
        {
            if (zge::compareStrings(m, kEmptyString))
            {
                const uint32_t command = zge::formatScratch(kPlayFormat, m);
                zgui::runScript(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<char>(command), 0);
            }
            result = 1;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(runMovieEndScript, Verified);

    uint32_t findSkaterIndexByName(ee::Address name);

    // inlineMenuSync: the UI's choices copied to the game: the game type, both
    // skaters (no name: "CE") and the level (unless it names none). Returns
    // the settings' address (what the original leaves as its result).
    int32_t inlineMenuSync()
    {
        ee::Local<uint8_t[0x100]> text;
        ee::Local<uint8_t[0x1A0]> out;
        memset(text, 0, 0x100);
        zgui::copyVariable(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<const char>(kGameTypeVariable), ee::Ptr<char>(text.address()));

        bool found = false;
        for (int32_t gameType = 1; gameType < 6 && !found; ++gameType)
        {
            if (!zge::compareStrings((*g_gameTypeNames)[gameType], text))
            {
                found = true;
                g_gameState->type = static_cast<GameType>(gameType);
            }
        }
        if (!found)
            zge::formatString(out, kUnknownGameType, text);

        for (int32_t player = 0; player < 2; ++player)
        {
            const uint32_t variable = player == 0 ? kSkaterVariable : kSkater2Variable;
            zgui::copyVariable(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<const char>(variable), ee::Ptr<char>(text.address()));
            if (!zge::compareStrings(kEmptyString, text))
                zge::stringCopy(text, kDefaultSkater);
            g_gameState->playerSkaters[player] = static_cast<uint8_t>(findSkaterIndexByName(text));
        }
        zgui::copyVariable(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<const char>(kLevelVariable), ee::Ptr<char>(text.address()));
        const int32_t level = aggressive::level::levelFromName(text);
        if (level != kNoLevel)
            *g_levelNumber = level;
        return kGameSettings;
    }
    READABLE_FUNCTION_WAITS(inlineMenuSync, Verified);

    // findSkaterIndexByName: the skater with the two-letter code (0 if none has it or
    // there is no name).
    uint32_t findSkaterIndexByName(ee::Address name)
    {
        if (!name)
            return 0;
        for (uint32_t index = 0; index < kSkaterRecordCount; ++index)
            if (!zge::compareStrings((*g_skaterCodes)[index], name))
                return index;
        return 0;
    }
    READABLE_FUNCTION(findSkaterIndexByName, Verified);

    // inlineMenuPopulateSkaterSelectMenu: the skater selector's Items: every code from the select
    // screen's order, locked ones as comment lines (the menu's "!!"). The
    // order's first entry is never listed: the loop starts at its second.
    // The first 13 skaters are always open.
    int32_t inlineMenuPopulateSkaterSelectMenu()
    {
        constexpr int32_t kAlwaysOpen = 13, kSkaterCount = 20;
        constexpr uint32_t kUnlockBits = 0x000FFFFFu;
        ee::Local<int32_t[kSkaterCount]> order;
        ee::Local<char[0x190]> items;

        for (int32_t index = 0; index < kSkaterCount; ++index)
            order[index] = (*g_skaterOrder)[index];

        // A line per skater but the first of the order; a locked one is a
        // comment. Every piece is two characters.
        uint32_t end = items.address();
        items[0] = 0;
        const auto append = [&](uint32_t text) {
            zge::appendString(end, text);
            end += 2;
        };
        for (int32_t index = 1; index < kSkaterCount; ++index)
        {
            const int32_t skater = order[index];
            const bool open = static_cast<uint32_t>(skater) < kAlwaysOpen || ((*g_unlockedSkaters & kUnlockBits) >> (skater & 0x1F) & 1);
            if (index != 1)
                append(kLineBreak);
            if (!open)
                append(kCommentMark);
            append((*g_skaterCodes)[skater]);
        }
        *ee::Ptr<char>(end) = 0;
        return zgui::setVariable(*g_uiRoot, kSkaterSelectorItems, items.address());
    }
    READABLE_FUNCTION_WAITS(inlineMenuPopulateSkaterSelectMenu, Verified);

    // inlineMenuPopulateSkaterStats: the skater selected on the select screen: its name, its
    // picture and every line of its stats panel, among them the game's
    // jokes (lightning strikes, rainfall, snowfall).
    namespace
    {
        constexpr uint64_t kCheaterFlag = 0x40; // a skater record's first flags
        inline ee::Global<ee::Float, sym::g_millimetresPerInch> g_millimetresPerInch; // the rain and snow are kept in millimetres

        uint64_t profileFlags(ee::Ptr<SkaterProfile> profile) { return profile->flagsWord; }
        ee::Ptr<char> nameOf(ee::Ptr<SkaterProfile> profile) { return profile->name; }
        ee::Ptr<const char> nameOf(const BestRun &run) { return run.name; }
    }

    int32_t inlineMenuPopulateSkaterStats()
    {
        constexpr int32_t kStatColumn = 12;
        ee::Local<uint32_t[kStatCount]> statNames;
        ee::Local<uint32_t[6]> rankNames;
        ee::Local<uint32_t[kSkaterRecordCount]> shots;
        ee::Local<char[0x100]> commandText;
        ee::Local<char[0x80]> playTimeText;
        ee::Local<int32_t> named;
        const uint32_t command = commandText.address(), playTime = playTimeText.address();
        const ee::Float noTime(0.0f);

        for (uint32_t index = 0; index < kStatCount; ++index)
            statNames[index] = ee::Ptr<uint32_t>(sym::g_statNames)[index];
        for (uint32_t index = 0; index < 6; ++index)
            rankNames[index] = ee::Ptr<uint32_t>(sym::g_rankNames)[index];
        for (uint32_t index = 0; index < kSkaterRecordCount; ++index)
            shots[index] = ee::Ptr<uint32_t>(sym::shotTable)[index];

        memset(command, 0, 0x100);
        const ee::Ptr<Node> selector = zgui::findNode(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<const char>(kSkaterSelector), 1);
        *named = 0;
        zgui::copyVariable(selector, ee::Ptr<const char>(kSelectedText), ee::Ptr<char>(command));
        const int32_t skater = static_cast<int32_t>(findSkaterIndexByName(command));
        const ee::Ptr<SkaterProfile> profile(skater * kSkaterRecordBytes + kSkaterProfiles);
        *named = (profileFlags(profile) >> 5) & 1;
        aggressive::skater::computeLevel(profile);
        zgui::setVariable(selector, sym::g_textHasBeenNamed, named);
        const ee::Ptr<char> name = nameOf(profile);
        zgui::setVariable(selector, sym::g_textSkatersName, name);
        const uint32_t shot = zge::formatScratch(sym::g_textSETCharacterShotMaterialS, shots[skater]);
        zgui::runScript(selector, ee::Ptr<char>(shot), 0);

        const ee::Ptr<Node> stats = zgui::findNode(selector, ee::Ptr<const char>(sym::g_textStats), 1);
        // A line of the panel: a value between the two colours.
        const auto showLine = [&]() {
            zgui::runScript(stats, ee::Ptr<char>(command), 0);
        };
        const auto addLine = [&](uint32_t format, uint64_t value) {
            zge::formatString(command, format, kCheatNameColour, value, kCheatTextColour);
        };
        zge::formatString(command, sym::g_textSETTextSSStatsN, name);
        showLine();

        for (uint32_t stat = 0; stat < kStatCount; ++stat)
        {
            const uint32_t text = statNames[stat];
            const int32_t width = kStatColumn - zge::stringLength(text);
            zge::formatString(command, sym::g_textSTRCATTextSSDS10N, text, kCheatNameColour, width, profile->stats[stat].total, kCheatTextColour);
            showLine();
        }
        addLine(sym::g_textSTRCATTextNAttributeTotalSDSN, profile->statTotal);
        showLine();
        aggressive::skater::computeLevel(profile);
        addLine(sym::g_textSTRCATTextNJuiceLevelSDSN, profile->level);
        showLine();

        // Ranking: which of the six names the completion percentage reaches.
        const ee::Float rankPosition = profile->completion * ee::Float(6.0f) / ee::Float(100.0f);
        addLine(sym::g_textSTRCATTextNRankingNSSSN, rankNames[ee::toInt(rankPosition)]);
        showLine();
        const bool cheater = (profileFlags(profile) & kCheaterFlag) != 0;
        addLine(sym::g_textSTRCATTextNCheaterSSSN, cheater ? sym::g_textYes : sym::g_textNo);
        showLine();

        // The time played in hours, minutes and seconds.
        ee::Float seconds = profile->timePlayed;
        memset(playTime, 0, 0x80);
        ee::Float hours = noTime, minutes = noTime;
        const ee::Float secondsPerHour(3600.0f), secondsPerMinute(60.0f), one(1.0f);
        while (secondsPerHour < seconds)
        {
            seconds = seconds - secondsPerHour;
            hours = hours + one;
        }
        while (secondsPerMinute < seconds)
        {
            seconds = seconds - secondsPerMinute;
            minutes = minutes + one;
        }
        if (noTime < hours)
            zge::formatString(playTime, sym::g_textDHours, ee::toInt(hours));
        const auto appendCount = [&](uint32_t format, int32_t count) {
            zge::appendString(playTime, zge::formatScratch(format, count));
        };
        if (noTime < minutes)
            appendCount(sym::g_textDMinutes, ee::toInt(minutes));
        if (noTime < seconds)
            appendCount(sym::g_textDSeconds, ee::toInt(seconds));
        else if (!playTimeText[0])
            zge::stringCopy(playTime, sym::g_textNone2);
        addLine(sym::g_textSTRCATTextNTotalTimeInGameSSSN, playTime);
        const ee::Float millimetresPerInch = *g_millimetresPerInch;
        showLine();

        const auto toDouble = [&](ee::Float value) {
            return sce::libgcc::fptodp(value);
        };
        addLine(sym::g_textSTRCATTextNPtsSecondS4fSN, toDouble(profile->pointsPerSecond));
        showLine();
        addLine(sym::g_textSTRCATTextNRolloversSDSN, static_cast<uint32_t>(profile->rollovers));
        showLine();
        addLine(sym::g_textSTRCATTextNBestTrickStringSDptsSN, static_cast<uint32_t>(profile->bestTrickString));
        showLine();
        addLine(sym::g_textSTRCATTextNCrashCountSDSN, static_cast<uint32_t>(profile->crashCount));
        showLine();
        addLine(sym::g_textSTRCATTextNContinueCountSDSN, static_cast<uint32_t>(profile->continues));
        showLine();
        addLine(sym::g_textSTRCATTextNSaveCountSDSN, static_cast<uint32_t>(profile->saveCount));
        showLine();
        addLine(sym::g_textSTRCATTextNCrittersExterminatedSDSN, static_cast<uint32_t>(profile->crittersExterminated));
        showLine();
        addLine(sym::g_textSTRCATTextNLightningStrikesS2fGWSN, toDouble(profile->lightning));
        showLine();
        addLine(sym::g_textSTRCATTextNRainfallS2finSN, toDouble(profile->rainfall / millimetresPerInch));
        showLine();
        addLine(sym::g_textSTRCATTextNSnowfallS2finSN, toDouble(profile->snowfall / millimetresPerInch));
        return runScriptResult(stats, command);
    }
    READABLE_FUNCTION_WAITS(inlineMenuPopulateSkaterStats, Verified);

    // inlineMenuNameSkater: the name typed for the selected skater into its save
    // record, which is then marked named.
    uint64_t inlineMenuNameSkater()
    {
        ee::Local<char> m;
        const ee::Ptr<Node> selector = zgui::findNode(ee::Ptr<Node>(*g_uiRoot), ee::Ptr<const char>(kSkaterSelector), 1);
        memset(m, 0, 0x20);
        zgui::copyVariable(selector, ee::Ptr<const char>(kSelectedText), ee::Ptr<char>(m.address()));
        const ee::Ptr<SkaterProfile> profile(static_cast<int32_t>(findSkaterIndexByName(m)) * kSkaterRecordBytes + kSkaterProfiles);
        zgui::copyVariable(selector, ee::Ptr<const char>(kNameInputString), ee::Ptr<char>(profile->name));
        return setSkaterNamed(profile);
    }
    READABLE_FUNCTION_WAITS(inlineMenuNameSkater, Verified);

    // inlineMenuPopulateLevelData: the level select's panel for the level chosen: locked or
    // not, its percent complete, the power-up and key the skater can win in
    // it (career only: their images tinted until won) and, in a career or a
    // timed run, the best runs table. A split-screen level is open if either
    // player has opened it.
    namespace
    {
        // A line of the level's best runs on the level select.
        struct StatRow
        {
            uint32_t label;
            int32_t unit;     // into the units' names
            int32_t isFloat;
            int32_t timedOnly; // a timed run's row, or the career's
        };
        static_assert(sizeof(StatRow) == 0x10);

        template <typename T, size_t N>
        void copyWords(const ee::Local<T[N]> &to, uint32_t from)
        {
            for (size_t index = 0; index < N; ++index)
                to[index] = ee::Ptr<T>(from)[index];
        }
    }

    int32_t inlineMenuPopulateLevelData()
    {
        constexpr uint32_t kGameTypeParkEditor = 5;
        constexpr int32_t kNoPowerUp = 7, kFirstPark = 10, kNoStats = 7, kPaddedWidth = 17, kBestRunBytes = 0x14, kBestRunsBytes = 0x118;
        constexpr uint32_t kRefreshPercent = sym::refreshPercent, kPlayRefresh = sym::playRefresh2;
        constexpr ee::Ptr<const char> kPercentVariable{sym::percentVariable};
        constexpr ee::Ptr<const char> kIsLockedVariable{sym::isLockedVariable};
        constexpr uint32_t kRefresh = sym::refresh2;
        constexpr ee::Ptr<const char> kVisible{sym::visible};
        constexpr ee::Ptr<const char> kMaterial{sym::material};
        constexpr uint32_t kTopLeftColor = sym::topLeftColor2, kTopRightColor = sym::topRightColor2, kBottomLeftColor = sym::bottomLeftColor2, kBottomRightColor = sym::bottomRightColor2;
        constexpr uint32_t kAttributeIcon = sym::attributeIcon;       // "AttributeIcons.AttGrind"
        constexpr uint32_t kResetLines = sym::resetLines;
        constexpr ee::Ptr<const char> kScrollBarActive{sym::scrollBarActive};
        constexpr uint32_t kFloatStatFormat = sym::floatStatFormat, kIntStatFormat = sym::intStatFormat;
        constexpr int32_t kDegreesPerHalfTurn = 0xB4;
        constexpr int32_t kUnitDegrees = 4;                    // kUnits' index for an integer row kept in half turns
        const ee::Float metresToFeet(3.28084f);

        ee::Local<uint32_t[14]> keyImages;
        ee::Local<uint32_t[28]> keyBits;     // a doubleword per level
        ee::Local<int32_t[14]> powerUps;     // by level
        ee::Local<uint32_t[7]> powerUpImages;
        ee::Local<uint32_t[14]> powerUpBits; // a doubleword per power-up
        ee::Local<char[0x100]> selectedLevel;
        ee::Local<uint8_t[0x10]> unwonTint;
        ee::Local<uint8_t[0x10]> wonTint;
        ee::Local<int32_t[4]> parkBits;
        ee::Local<StatRow[kBestRunsPerLevel]> statRows;
        ee::Local<uint32_t[5]> units;
        ee::Local<char[0x100]> commandText;
        ee::Local<int32_t> locked;
        ee::Local<int32_t> percent;
        ee::Local<int32_t> powerUpShown;
        ee::Local<int32_t> powerUpHidden;
        ee::Local<int32_t> keyShown;
        ee::Local<int32_t> keyHidden;
        ee::Local<int32_t> statsShown;
        ee::Local<int32_t> statsHidden;

        copyWords(keyImages, sym::g_keyImagePaths);
        copyWords(keyBits, sym::g_keyBits);
        copyWords(powerUps, sym::g_powerUpsOfLevel);
        copyWords(powerUpImages, sym::g_powerUpImagePaths);
        copyWords(powerUpBits, sym::g_inlineMenuPopulateLevelDataInitializer1);

        const auto findChild = [&](ee::Ptr<Node> parent, uint32_t name) {
            return zgui::findNode(parent, ee::Ptr<const char>(name), 1);
        };

        const ee::Ptr<Node> levelSelect = findChild(ee::Ptr<Node>(*g_uiRoot), sym::g_textFrontEndBackgroundLevelSelect);
        *locked = 1;
        *percent = 0;
        memset(selectedLevel, 0, 0x100);
        memset(unwonTint, 0, 0x10);
        for (uint32_t index = 0; index < 0x10; ++index)
            wonTint[index] = ee::Ptr<uint8_t>(sym::g_levelDataMask)[index];
        unwonTint[3] = 255;
        const ee::Ptr<Node> levelData = findChild(levelSelect, sym::g_textLevelData);
        const ee::Ptr<Node> data = findChild(levelData, sym::g_textData2);
        const ee::Ptr<Node> statsWidget = findChild(data, sym::g_textStats);
        const uint32_t skater = g_gameState->playerSkaters[0];
        const ee::Ptr<SkaterProfile> profile(skater * kSkaterRecordBytes + kSkaterProfiles);
        zgui::copyVariable(levelSelect, ee::Ptr<const char>(sym::g_textSelectedLevel), ee::Ptr<char>(selectedLevel.address()));
        const int32_t level = aggressive::level::levelFromName(selectedLevel.address());

        // Locked unless the skater's record has the level's bit; in split
        // screen either player's opens it. Parks have bits of their own.
        const auto opens = [](ee::Ptr<SkaterProfile> skaterProfile, int32_t bit) {
            return ((skaterProfile->openLevels >> (bit & 0x1F)) & 1) != 0;
        };
        const auto lockedUnlessBit = [&](int32_t bit) {
            *locked = opens(profile, bit) ? 0 : 1;
        };
        bool isPark = false;
        const GameType gameType = g_gameState->type;
        if (gameType == GameType::SplitScreen)
        {
            const ee::Ptr<SkaterProfile> other(g_gameState->playerSkaters[1] * kSkaterRecordBytes + kSkaterProfiles);
            *locked = opens(profile, level) || opens(other, level) ? 0 : 1;
        }
        else if (static_cast<uint32_t>(gameType) == kGameTypeParkEditor)
        {
            copyWords(parkBits, sym::g_parkBits);
            isPark = true;
            lockedUnlessBit(parkBits[level - kFirstPark]);
        }
        else if (static_cast<uint32_t>(gameType) > kGameTypeParkEditor || static_cast<uint32_t>(gameType) < 2 || level < kFirstPark || level >= kNoLevel)
            lockedUnlessBit(level);
        else
        {
            *locked = 0;
            isPark = true;
        }

        zgui::setVariable(data, kIsLockedVariable, locked);
        zgui::runScript(data, ee::Ptr<char>(kPlayRefresh), 0);
        *percent = ee::toInt(careerLevelState(skater, level)->completion);
        zgui::setVariable(data, kPercentVariable, percent);
        zgui::runScript(data, ee::Ptr<char>(kRefreshPercent), 0);

        // The image's corner colours, stopping at the first refused.
        const auto setCorners = [&](ee::Ptr<Node> image, uint32_t colour) {
            for (const uint32_t corner : {kTopLeftColor, kTopRightColor, kBottomLeftColor, kBottomRightColor})
                if (zgui::setVariable(image, corner, colour) == 0)
                    return false;
            return true;
        };
        const auto won = [&](uint32_t bit) { return ((profile->lootFlags >> (bit & 0x3F)) & 1) != 0; };

        // The level's power-up and key, a career's only: bright once found.
        const int32_t powerUp = powerUps[level];
        if (static_cast<uint32_t>(powerUp) < static_cast<uint32_t>(kNoPowerUp) && g_gameState->type == GameType::Career)
        {
            *powerUpShown = static_cast<int32_t>(GameType::Career);
            const ee::Ptr<Node> icon = findChild(data, sym::g_textPowerUp);
            zgui::setVariable(icon, kVisible, powerUpShown);
            const ee::Ptr<Node> image = findChild(icon, sym::g_textImage);
            const bool found = won(powerUpBits[powerUp * 2]);
            if (setCorners(image, found ? wonTint.address() : unwonTint.address()))
                zgui::setVariable(image, kMaterial, found ? powerUpImages[powerUp] : kAttributeIcon);
            zgui::runScript(image, ee::Ptr<char>(kRefresh), 0);
        }
        else
        {
            *powerUpHidden = 0;
            zgui::setVariable(data, sym::g_textPowerUpVisible, powerUpHidden);
        }

        if (keyImages[level] && g_gameState->type == GameType::Career)
        {
            *keyShown = static_cast<int32_t>(GameType::Career);
            const ee::Ptr<Node> icon = findChild(data, sym::g_textKey);
            zgui::setVariable(icon, kVisible, keyShown);
            const ee::Ptr<Node> image = findChild(icon, sym::g_textImage);
            zgui::setVariable(image, kMaterial, keyImages[level]);
            setCorners(image, won(keyBits[level * 2]) ? wonTint.address() : unwonTint.address());
            zgui::runScript(image, ee::Ptr<char>(kRefresh), 0);
        }
        else
        {
            *keyHidden = 0;
            zgui::setVariable(data, sym::g_textKeyVisible, keyHidden);
        }

        // The best runs, for an open level of a career or a timed run.
        const GameType typeNow = g_gameState->type;
        const bool careerOrTimed = typeNow == GameType::TimedRun || typeNow == GameType::Career;
        if (!careerOrTimed || *locked || isPark || level == kNoStats)
        {
            *statsHidden = 0;
            zgui::setVariable(statsWidget, kVisible, statsHidden);
            return zgui::setVariable(statsWidget, kScrollBarActive, statsHidden);
        }

        copyWords(statRows, sym::g_levelStatRows);
        copyWords(units, sym::g_statUnits);
        *statsShown = 1;
        const uint32_t command = commandText.address();
        memset(command, 0, 0x100);
        zgui::setVariable(statsWidget, kVisible, statsShown);
        zgui::setVariable(statsWidget, kScrollBarActive, statsShown);
        zgui::runScript(statsWidget, ee::Ptr<char>(kResetLines), 0);
        zgui::runScript(statsWidget, ee::Ptr<char>(kClearText), 0);

        bool first = true;
        for (uint32_t row = 0; row < kBestRunsPerLevel; ++row)
        {
            const StatRow &stat = statRows[row];
            const bool timedRun = *g_gameType == GameType::TimedRun;
            if ((stat.timedOnly != 0) != timedRun)
                continue;
            const BestRun &run = (*g_bestRuns)[level][row];
            const uint32_t unitName = units[stat.unit];
            const uint32_t between = first ? kEmptyString : kLineBreak;
            const ee::Ptr<const char> holder = nameOf(run);
            const int32_t width = kPaddedWidth - zge::stringLength(holder);
            if (stat.isFloat)
            {
                // Heights and distances are metres shown in feet.
                const bool inFeet = static_cast<uint32_t>(stat.unit - 1) < 2u;
                const ee::Float shown = inFeet ? run.measure * metresToFeet : run.measure;
                zge::formatString(command, kFloatStatFormat, between, stat.label, holder, width, sce::libgcc::fptodp(shown), unitName);
            }
            else
            {
                // A row in degrees is kept in half turns.
                const int32_t value = stat.unit == kUnitDegrees ? run.value * kDegreesPerHalfTurn : run.value;
                zge::formatString(command, kIntStatFormat, between, stat.label, holder, width, value, unitName);
            }
            first = false;
            zgui::runScript(statsWidget, ee::Ptr<char>(command), 0);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(inlineMenuPopulateLevelData, Verified);

    // inlineMenuPopulateCheatList: (widget,): the Text of the cheat list: every cheat earned,
    // its name in capitals and its description, or a line saying there are
    // none.
    int32_t inlineMenuPopulateCheatList(uint32_t arguments)
    {
        ee::Local<uint8_t[0x80]> command;
        ee::Local<uint8_t[0xC0]> name;
        bool listed = false;

        memset(command, 0, 0x80);
        memset(name, 0, 0x20);
        const uint32_t widget = *ee::Ptr<uint32_t>(arguments);
        zgui::runScript(ee::Ptr<Node>(widget), ee::Ptr<char>(kClearText), 0);

        for (Cheat &cheat : *g_cheats)
        {
            if (!((*g_careerCheats >> (cheat.bit & 0x1F)) & 1))
                continue;
            zge::stringCopyN(name, cheat.name, 31);
            zge::stringToUpper(name);
            const uint32_t separator = listed ? kDoubleLineBreak : kEmptyString;
            listed = true;
            zge::formatString(command, kCheatLineFormat, separator, kCheatNameColour, name, kCheatTextColour, cheat.description);
            zgui::runScript(ee::Ptr<Node>(widget), ee::Ptr<char>(command.address()), 0);
        }
        if (listed)
            return 0;
        zge::formatString(command, kNoCheatsFormat, kCheatNameColour);
        return runScriptResult(widget, command.address());
    }
    READABLE_FUNCTION_WAITS(inlineMenuPopulateCheatList, Verified);

    // inlineMenuStartGame: the choices made: the game starts next frame.
    int32_t inlineMenuStartGame()
    {
        setPendingStateAction(0);
        setPendingStateDelay(1);
        return game::tail(switchGameState, 2);
    }
    READABLE_FUNCTION_WAITS(inlineMenuStartGame, Verified);

    // inlineMenuPlayOnChangeSound: zgui's change sound, for the scripts.
    int32_t inlineMenuPlayOnChangeSound()
    {
        if (!*g_changeSoundHook)
            return 0;
        return (*g_changeSoundHook)();
    }
    READABLE_FUNCTION(inlineMenuPlayOnChangeSound, Verified);

    namespace
    {
        namespace tail
        {
        int padVibrate(ee::Ptr<zge::Pad> pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag) { return game::tail(zge::padVibrate, pad, smallMotor, bigMotor, time, tag); }
        }
    }

    // inlineMenuVibrateController: a short buzz on the port being set up.
    int32_t inlineMenuVibrateController()
    {
        const uint32_t smallMotor = sce::libgcc::fptoui(ee::toFloat(255));
        const uint32_t bigMotor = sce::libgcc::fptoui(ee::toFloat(65000));
        return tail::padVibrate(ee::Ptr<zge::Pad>(*g_vibrateControllerPort), smallMotor, bigMotor, 300, 0);
    }
    READABLE_FUNCTION(inlineMenuVibrateController, Verified);

    namespace
    {
        // The controller screens' script arguments on the frame: the port
        // from the first argument's value (1 or 2 to the script).
        // The script arguments hold the port's widget, then the config widget.
        void readPort(uint32_t widget, ee::Ptr<int32_t> port)
        {
            zgui::variableIntValue(ee::Ptr<Node>(widget), port);
        }
    }

    // inlineMenuGetControllerConfig: (variable, widget): the port's layout and vibration set
    // on the options widget.
    int32_t inlineMenuGetControllerConfig(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> port;
        ee::Local<uint32_t> layout;
        ee::Local<uint32_t> vibration;
        *vibration = 1;
        *port = 0;
        *layout = 0;
        readPort(arguments[0], port);
        *port = *port - 1;
        const uint32_t widget = arguments[1];
        *layout = padLayoutOption(static_cast<uint8_t>(*port));
        zgui::setVariable(widget, kSelectedIndex, layout);
        zgui::setVariable(widget, kPreviousIndex, layout);
        *vibration = padVibrationOption(static_cast<uint8_t>(*port));
        zgui::setVariable(widget, kVibrationIsOn, vibration);
        return runScriptResult(widget, kVibrationRefresh);
    }
    READABLE_FUNCTION_WAITS(inlineMenuGetControllerConfig, Verified);

    // inlineMenuSetControllerConfig: (variable, widget): the options widget's layout and
    // vibration stored for the port.
    uint64_t inlineMenuSetControllerConfig(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> port;
        ee::Local<uint32_t> layout;
        ee::Local<int32_t> vibration;
        *vibration = 1;
        *port = 0;
        *layout = 0;
        readPort(arguments[0], port);
        const uint32_t widget = arguments[1];
        *port = *port - 1;
        zgui::copyVariable(ee::Ptr<Node>(widget), ee::Ptr<const char>(kSelectedIndex), ee::Ptr<char>(layout.address()));
        zgui::copyVariable(ee::Ptr<Node>(widget), ee::Ptr<const char>(kVibrationIsOn), ee::Ptr<char>(vibration.address()));
        setControllerConfigLayout(static_cast<uint8_t>(*port), static_cast<uint8_t>(*layout));
        return setControllerConfigVibration(static_cast<uint8_t>(*port), *vibration);
    }
    READABLE_FUNCTION_WAITS(inlineMenuSetControllerConfig, Verified);

    // buttonLayoutLabel: a button's action on the controller screen, its word.
    uint32_t buttonLayoutLabel(uint32_t action)
    {
        if (action == 4)
            return sym::g_textAction3;
        if (static_cast<int32_t>(action) < 5)
        {
            if (action == 3)
                return sym::g_textJump;
            return 0;
        }
        if (action == 5)
            return sym::g_textGrind;
        if (action == 6)
            return sym::g_textTrick;
        return 0;
    }
    READABLE_FUNCTION(buttonLayoutLabel, Verified);

    // inlineMenuRefreshControllerConfigScreen: (variable, layout widget, screen widget): the action
    // buttons' words shown on the controller screen for the layout selected,
    // in variables named after the button: Triangle, Square and Circle; the
    // loop ends before X (button 0x80), whose case is never reached.
    // Bug kept: the loop stops before the last button, so X (0x80) never gets a
    // label here.
    int32_t inlineMenuRefreshControllerConfigScreen(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x40]> name;
        ee::Local<int32_t> port;
        ee::Local<int32_t> layout;
        *port = 0;
        *layout = 0;
        readPort(arguments[0], port);
        const uint32_t screen = arguments[2];
        zgui::copyVariable(ee::Ptr<Node>(arguments[1]), ee::Ptr<const char>(kSelectedIndex), ee::Ptr<char>(layout.address()));

        int32_t result = 0;
        for (int32_t button = kFirstButton; button < kLastButton; button <<= 1)
        {
            const uint32_t label = buttonLayoutLabel(buttonLayoutIndex(button, *layout));
            memset(name, 0, 0x40);
            if (button == 0x10)
                zge::stringCopy(name, sym::g_textTriangle);
            else if (button == 0x20)
                zge::stringCopy(name, sym::g_textSquare);
            else if (button == 0x40)
                zge::stringCopy(name, sym::g_textCircle);
            zge::appendString(name, sym::g_textText5);
            result = zgui::setVariable(screen, name.address(), label);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(inlineMenuRefreshControllerConfigScreen, Verified);

    // The three sliders' shape, front end and pause menu (volume_menu.h;
    // `function` is the menu function's own name, which nothing reads).
    // setChannelVolume (slider, test): the slider's value (90 if it has
    // none), kept to 0 .. 127, as the channel's volume, with a test sound
    // unless the second argument says no (soundEvent 0).
    int32_t setChannelVolume(uint32_t /*function*/, uint32_t arguments, uint32_t channel)
    {
        ee::Local<ee::Float> value;
        ee::Local<int32_t> playTest;
        const ee::Ptr<uint32_t> argument(arguments);
        *value = ee::Float(90.0f);
        *playTest = 1;
        zgui::variableFloatValue(ee::Ptr<Node>(argument[0]), ee::Ptr<ee::Float>(value));
        zgui::variableIntValueAgain(ee::Ptr<Node>(argument[1]), ee::Ptr<int32_t>(playTest));
        const int32_t slider = ee::toInt(*value);
        const int32_t volume = slider <= 0 ? 0 : slider < 127 ? slider : 127;
        return static_cast<int32_t>(soundEvent(kSetVolume, channel, volume, *playTest, 0));
    }

    // showChannelVolume (slider,): the channel's volume (soundEvent 1),
    // rounded up to even and at most 128, set as the slider's value, then
    // the slider's on-change script run.
    int32_t showChannelVolume(uint32_t /*function*/, uint32_t arguments, uint32_t channel, uint32_t currentValueName, uint32_t onChangeScript)
    {
        ee::Local<ee::Float> shown;
        const ee::Ptr<uint32_t> argument(arguments);
        const int32_t volume = static_cast<int32_t>(soundEvent(kGetVolume, channel, 0, 0, 0));
        const int32_t even = (volume & 1) ? volume + 1 : volume;
        const uint32_t slider = argument[0];
        *shown = ee::toFloat(static_cast<uint32_t>(even) > 128u ? 128 : even);
        zgui::setVariable(slider, currentValueName, shown.address());
        return runScriptResult(argument[0], onChangeScript);
    }

    // inlineMenuSetSFXVolume .. inlineMenuSetMusicVolume: (value, test): the three volumes set.
    int32_t inlineMenuSetSFXVolume(uint32_t arguments)
    {
        return setChannelVolume(sym::inlineMenuSetSFXVolume, arguments, kVolumeSfx);
    }
    READABLE_FUNCTION_WAITS(inlineMenuSetSFXVolume, Verified);

    int32_t inlineMenuSetSpeechVolume(uint32_t arguments)
    {
        return setChannelVolume(sym::inlineMenuSetSpeechVolume, arguments, kVolumeSpeech);
    }
    READABLE_FUNCTION_WAITS(inlineMenuSetSpeechVolume, Verified);

    int32_t inlineMenuSetMusicVolume(uint32_t arguments)
    {
        return setChannelVolume(sym::inlineMenuSetMusicVolume, arguments, kVolumeMusic);
    }
    READABLE_FUNCTION_WAITS(inlineMenuSetMusicVolume, Verified);

    // inlineMenuGetSFXVolume .. inlineMenuGetMusicVolume: (slider,): the three volumes shown.
    int32_t inlineMenuGetSFXVolume(uint32_t arguments)
    {
        return showChannelVolume(sym::inlineMenuGetSFXVolume, arguments, kVolumeSfx, kCurValue, kPlayOnChange);
    }
    READABLE_FUNCTION_WAITS(inlineMenuGetSFXVolume, Verified);

    int32_t inlineMenuGetSpeechVolume(uint32_t arguments)
    {
        return showChannelVolume(sym::inlineMenuGetSpeechVolume, arguments, kVolumeSpeech, kCurValue, kPlayOnChange);
    }
    READABLE_FUNCTION_WAITS(inlineMenuGetSpeechVolume, Verified);

    int32_t inlineMenuGetMusicVolume(uint32_t arguments)
    {
        return showChannelVolume(sym::inlineMenuGetMusicVolume, arguments, kVolumeMusic, kCurValue, kPlayOnChange);
    }
    READABLE_FUNCTION_WAITS(inlineMenuGetMusicVolume, Verified);

    namespace
    {
        namespace tail
        {
        int setGameMode(uint32_t mode) { return game::tail(aggressive::setGameMode, mode); }
        }
    }

    // inlineMenuStartMusic, inlineMenuStopMusic: despite their names, the game mode: the
    // front end's music follows it.
    uint32_t inlineMenuStartMusic()
    {
        return tail::setGameMode(1);
    }
    READABLE_FUNCTION_WAITS(inlineMenuStartMusic, Verified);

    uint32_t inlineMenuStopMusic()
    {
        return tail::setGameMode(0);
    }
    READABLE_FUNCTION_WAITS(inlineMenuStopMusic, Verified);

    namespace
    {
        namespace tail
        {
        int startGameSave() { return game::tail(aggressive::startGameSave); }
        }
    }

    // inlineMenuSaveGame .. inlineMenuAutoLoadGame: the save dialogs.
    int32_t inlineMenuSaveGame()
    {
        return tail::startGameSave();
    }
    READABLE_FUNCTION_WAITS(inlineMenuSaveGame, Verified);

    namespace
    {
        namespace tail
        {
        int startGameLoad() { return game::tail(aggressive::startGameLoad); }
        }
    }

    int32_t inlineMenuLoadGame()
    {
        return tail::startGameLoad();
    }
    READABLE_FUNCTION_WAITS(inlineMenuLoadGame, Verified);

    namespace
    {
        namespace tail
        {
        int startGameAutoLoad() { return game::tail(aggressive::startGameAutoLoad); }
        }
    }

    int32_t inlineMenuAutoLoadGame()
    {
        return tail::startGameAutoLoad();
    }
    READABLE_FUNCTION_WAITS(inlineMenuAutoLoadGame, Verified);

    // parkLoadedStartGame: the card's dialog is over: a loaded park starts on its
    // level; anything else goes back where the load was asked from and frees
    // the buffer.
    int32_t parkLoadedStartGame(uint32_t result)
    {
        if (result == kParkLoaded)
        {
            const uint32_t data = aggressive::level::parkBufferData(*g_parkBuffer);
            *g_levelNumber = ee::Ptr<int32_t>(data)[kParkLevelWord] + kFirstParkLevel;
            return game::tail(aggressive::inlineMenuStartGame);
        }
        zgui::runScript(ee::Ptr<Node>(0u), ee::Ptr<char>(*g_parkPlayState == 1 ? kParkDialogMoveIn : kLevelSelectMoveIn), 0);
        const ee::Ptr<level::ParkBuffer> block = *g_parkBuffer;
        *g_parkPlayState = 0;
        const int32_t freed = aggressive::level::freeParkBuffer(block);
        *g_parkBuffer = {};
        return freed;
    }
    READABLE_FUNCTION_WAITS(parkLoadedStartGame, Verified);

    namespace
    {
        int32_t loadPark(uint32_t state, uint32_t newReturnsTo, uint32_t kindReturnsTo)
        {
            *g_parkPlayState = state;
            const ee::Ptr<level::ParkBuffer> block = aggressive::level::newParkBuffer(kParkBytes);
            *g_parkBuffer = block;
            aggressive::level::setParkBufferClient(block, 0);
            return game::tail(aggressive::level::startParkLoad, *g_parkBuffer, sym::parkLoadedStartGame);
        }
    }

    // inlineMenuLoadPark, inlineMenuPlayPark: a park loaded from the card, from the park
    // editor's dialog or to play.
    int32_t inlineMenuLoadPark()
    {
        namespace here = points::inlineMenuLoadPark;
        return loadPark(1, here::afterNewParkBuffer, here::afterSetParkBufferSlot);
    }
    READABLE_FUNCTION_WAITS(inlineMenuLoadPark, Verified);

    int32_t inlineMenuPlayPark()
    {
        namespace here = points::inlineMenuPlayPark;
        return loadPark(2, here::afterNewParkBuffer, here::afterSetParkBufferSlot);
    }
    READABLE_FUNCTION_WAITS(inlineMenuPlayPark, Verified);

    // inlineMenuGetCredits: (widget,): the credits file into its Text. Must run with
    // the file there: the original does not test loadFile's result. With no
    // widget the result is what the caller had.
    void inlineMenuGetCredits(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> bytesOut;
        if (!arguments || !*arguments)
            return;
        const uint32_t file = zge::loadFile(kCreditsFile, bytesOut);
        *(ee::Ptr<uint8_t>(file) + *bytesOut) = 0;
        zgui::setVariable(*arguments, kTextVariable, file);
        zge::freeLoadedFile(file);
    }
    READABLE_FUNCTION_WAITS(inlineMenuGetCredits, Verified);

    // ---- the media viewer -----------------------------------------------------

    namespace
    {
        // A movie the viewer lists (the table at mediaMovies).
        struct MediaMovie
        {
            uint8_t _00[4];
            uint32_t variableValue; // +0x04 what FrontEnd.MovieName is set to
            uint32_t title;         // +0x08
            uint8_t _0c[4];
            uint32_t unlockBit;     // +0x10 of g_mediaUnlocks; kAlwaysUnlocked: every game has it
            uint32_t needsWatched;  // +0x14 non-zero: also that bit of g_mediaWatched
        };
        static_assert(sizeof(MediaMovie) == 0x18);
        constexpr uint32_t kMediaMovies = sym::mediaMovies, kMediaMovieCount = 8;
        constexpr uint32_t kAlwaysUnlocked = 14;
        constexpr uint32_t kMediaViewerMovies = sym::mediaViewerMovies;
        constexpr uint32_t kClearItems = sym::g_textSetItems2;         // "SET Items {\"\"}"
        constexpr uint32_t kMediaItem = sym::g_textStrcatItems4;          // "STRCAT Items {\"%s%s%s\"}"
        constexpr uint32_t kEmptyText = sym::emptyString;          // ""
        constexpr uint32_t kHighlight = sym::highlight;          // "!": the zgui's locked look
        constexpr uint32_t kStringsPage = ee::high(sym::cheatNameColour);        // the strings' page (upper half), as the original holds it
        inline ee::Global<uint16_t, sym::g_mediaUnlocks> g_mediaUnlocks;
        inline ee::Global<uint16_t, sym::g_mediaWatched> g_mediaWatched;
    }

    // mediaViewerMovies: the movie table and its size.
    void mediaViewerMovies(uint32_t /*unused*/, ee::Ptr<uint32_t> movies, ee::Ptr<uint32_t> count)
    {
        *movies = kMediaMovies;
        *count = kMediaMovieCount;
    }
    READABLE_FUNCTION(mediaViewerMovies, Verified);

    // inlineMenuPopulateMediaViewerMenu: (a widget, the list,): a line per movie; the ones not
    // yet unlocked are highlighted, the first has no line break.
    int32_t inlineMenuPopulateMediaViewerMenu(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char> line;
        ee::Local<uint32_t> movies;
        ee::Local<uint32_t> count;
        memset(line, 0, 0x100);
        const uint32_t list = arguments[1];
        *movies = 0;
        *count = 0;
        mediaViewerMovies(arguments[0], movies, count);
        zgui::runScript(ee::Ptr<Node>(list), ee::Ptr<char>(kClearItems), 0);
        if (*count == 0)
            return 0;

        int32_t result = 0;
        for (uint32_t index = 0; index < *count; ++index)
        {
            const ee::Ptr<MediaMovie> movie(*movies);
            const uint32_t bit = movie->unlockBit & 0x1F;
            uint32_t unlocked = movie->unlockBit == kAlwaysUnlocked || ((static_cast<int32_t>(*g_mediaUnlocks) >> bit) & 1) ? 1 : 0;
            if (unlocked && movie->needsWatched)
                unlocked = *g_mediaWatched & (1u << bit);
            const uint32_t lineBreak = index == 0 ? kEmptyText : kLineBreak;
            const uint32_t highlight = unlocked ? kEmptyText : kHighlight;
            zge::formatString(line, kMediaItem, lineBreak, highlight, movie->title);
            zgui::runScript(ee::Ptr<Node>(list), ee::Ptr<char>(line.address()), 0);
            result = static_cast<int32_t>(*movies + sizeof(MediaMovie));
            *movies = static_cast<uint32_t>(result);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(inlineMenuPopulateMediaViewerMenu, Verified);

    // inlineMenuPrepareMediaMovie: (a widget, the list,): the selected movie for the player.
    int32_t inlineMenuPrepareMediaMovie(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<uint32_t> movies;
        ee::Local<uint32_t> count;
        ee::Local<uint32_t> selected;
        *movies = 0;
        *count = 0;
        *selected = 0;
        mediaViewerMovies(arguments[0], movies, count);
        zgui::copyVariable(ee::Ptr<Node>(arguments[1]), ee::Ptr<const char>(kSelectedIndex), ee::Ptr<char>(selected.address()));
        const ee::Ptr<MediaMovie> movie = ee::Ptr<MediaMovie>(*movies) + *selected;
        return zgui::setVariable(*g_uiRoot, kMovieNameVariable, movie->variableValue);
    }
    READABLE_FUNCTION_WAITS(inlineMenuPrepareMediaMovie, Verified);

    // inlineMenuCheat: the cheat box: what was typed against each code (their
    // meanings are in docs/CHEATS.md). The first three unlock; the rest
    // change the tuning table and mark the career as cheated (g_gameplayCheats).
    namespace
    {
        // A tuning value set: `category` is the global holding the category's
        // block, `field` the value's place in it.
        void setTuning(uint32_t category, uint32_t field, ee::Float value)
        {
            *ee::Ptr<ee::Float>(*ee::Ptr<uint32_t>(category) + field) = value;
        }
    }

    int32_t inlineMenuCheat(uint32_t arguments)
    {
        ee::Local<uint8_t[0x80]> typed;
        ee::Local<int32_t> successful;
        constexpr uint32_t kEmpty = sym::emptyString;
        constexpr uint32_t kString = sym::g_textString4, kCheatSuccessful = sym::cheatSuccessful;
        constexpr uint32_t kTuningPhysics = sym::tuningPhysics, kTuningJuice = sym::tuningJuice, kTuningGrindWalls = sym::tuningGrindWalls; // the categories' tables
        constexpr uint32_t kAirRotate = 0x650, kMaxJuiceDegradation = 0xB0, kJuiceLostOnCrash = 0x80, kWallRideGravity = 0x830, kGrindBalanceFactor = 0xB0,
                           kGrindBalanceInit = 0x110, kStallBalanceFactor = 0x2C0, kStallBalanceInit = 0x320, kManualBalanceFactor = 0x3B0, kManualBalanceInit = 0x410;
        constexpr uint32_t kCheatSound = 73;
        const auto isCode = [&](uint32_t code) {
            return zge::compareStrings(typed, code) == 0;
        };
        const auto succeed = [&](uint32_t bit) {
            *successful = 1;
            *g_gameplayCheats |= bit;
        };

        *successful = 0;
        memset(typed, 0, 0x80);
        const uint32_t node = *ee::Ptr<uint32_t>(arguments);
        zgui::copyVariable(ee::Ptr<zgui::Node>(node), ee::Ptr<const char>(kString), ee::Ptr<char>(typed.address()));
        if (zge::compareStrings(typed, kEmpty) != 0)
        {
            if (isCode(sym::g_textWwssadadBABA))
            {
                // Every skater's record maxed.
                for (uint32_t skater = 0; skater < kSkaterRecordCount; ++skater)
                {
                    const ee::Ptr<SkaterProfile> profile(sym::g_skaterRecords + skater * kSkaterRecordSize);
                    profile->openLevels = 0xFFFFFFFFu;
                    profile->experienceMultiplier = 10;
                }
                succeed(1);
            }
            if (isCode(sym::g_textSddsasasddd)) // every skater and level unlocked
            {
                *g_unlocks |= 0xFFFFFu;
                succeed(1);
            }
            if (isCode(sym::g_textSKELETON))
            {
                level::cheatGiveLevelKeys();
                succeed(1);
            }
            if (isCode(sym::g_textWswsadadABABS))
            {
                setTuning(kTuningGrindWalls, kWallRideGravity, ee::Float(3.0f));
                succeed(2);
            }
            if (isCode(sym::g_textAaddadswwsAI))
            {
                setTuning(kTuningJuice, kMaxJuiceDegradation, ee::Float(-0.2f));
                succeed(4);
            }
            if (isCode(sym::g_textKHUFU))
            {
                setTuning(kTuningJuice, kJuiceLostOnCrash, ee::Float(0.0f));
                succeed(8);
            }
            if (isCode(sym::g_textBIGUPYASELF)) // perfect grind balance
            {
                setTuning(kTuningGrindWalls, kGrindBalanceFactor, ee::Float(0.0f));
                setTuning(kTuningGrindWalls, kGrindBalanceInit, ee::Float(0.0f));
                succeed(0x10);
            }
            if (isCode(sym::g_textJUSTINBAILEY)) // perfect stall balance
            {
                setTuning(kTuningGrindWalls, kStallBalanceFactor, ee::Float(0.0f));
                setTuning(kTuningGrindWalls, kStallBalanceInit, ee::Float(0.0f));
                succeed(0x20);
            }
            if (isCode(sym::g_textAaaaddddadadw))
            {
                setTuning(kTuningPhysics, kAirRotate, ee::Float(640.0f));
                succeed(0x40);
            }
            if (isCode(sym::g_textQUEZDONTSLEEP)) // perfect manual balance
            {
                setTuning(kTuningGrindWalls, kManualBalanceFactor, ee::Float(0.0f));
                setTuning(kTuningGrindWalls, kManualBalanceInit, ee::Float(0.0f));
                succeed(0x80);
            }
            if (isCode(sym::g_textBAKABAKA))
                succeed(0x100);
        }
        zgui::setVariable(node, kCheatSuccessful, successful);
        return soundEvent(kCheatSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(inlineMenuCheat, Verified);
}
