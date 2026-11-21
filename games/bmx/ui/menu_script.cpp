// The menus' script functions (menu_script.h): BMX XXX's own code.

#include "menu_script.h"
#include "records_frame.h"

#include "bmx/create/menu_model.h"
#include "bmx/profile/players.h"
#include "bmx/game/player_riders.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/ui/strings.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/ui/menu_movie.h"
#include "bmx/ui/ui_calls.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/assets/file.h"
#include "zge/input/pads.h"
#include "zge/memory/pool.h"
#include "zge/render/display.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/ui/menu_script.cpp.h"

namespace game::bmx
{
    namespace
    {
        // A label as the signed text measureText takes.
        ee::Ptr<const int8_t> labelText(ee::Ptr<char> label) { return label.cast<const int8_t>(); }
        using ui::calls::Node;

        // The C library's memset, by address (ee/plain.h).
        // Scaffolding (address-call): a library function, never rewritten.
        void memset(ee::Address dst, uint32_t byte, uint32_t bytes) { ee::game(sym::memset, dst, byte, bytes); }

        // The sound channels whose volumes the saved game keeps
        // (SavedGame::channelVolumes), as the volume event numbers them.
        constexpr uint32_t kSfxChannel = 0, kMusicChannel = 1, kSpeechChannel = 2;
        constexpr int32_t kLoudest = 127;
        // A slider's top.
        constexpr uint32_t kSliderTop = 128;

        constexpr ee::Ptr<const char> kSelectedIndex{sym::g_textMenuSelectedIndex};       // "SelectedIndex"
        constexpr ee::Ptr<const char> kVibrationOn{sym::g_textMenuVibrationOn};           // "Vibration.isOn"
        constexpr ee::Ptr<char> kVibrationRefresh{sym::g_textMenuVibrationRefresh}; // "PLAY Vibration.Refresh"
        constexpr ee::Ptr<const char> kCurValue{sym::g_textMenuCurValue};                 // "CurValue"
        constexpr ee::Ptr<char> kPlayOnChange{sym::g_textMenuPlayOnChange};         // "PLAY OnChange"

        // The test shake: the small motor full on, the big motor's level,
        // and how long (ms).
        constexpr float kShakeSmallMotor = 255.0f;
        inline ee::Global<ee::Float, sym::g_menuShakeBigMotor> g_menuShakeBigMotor; // 65000
        constexpr uint32_t kShakeTime = 300;

        // A track's label.
        constexpr ee::Ptr<const char> kMenuFont{sym::g_textMenuFont};           // "Common.font_normal"
        constexpr uint32_t kFontAsset = 1;
        constexpr ee::Ptr<const char> kLabelSeparator{sym::g_textTrackSeparator}; // " - "
        constexpr ee::Ptr<const char> kEllipsis{sym::g_textTrackEllipsis};       // "..."
        inline ee::Global<char, sym::g_emptyTrackTitle> g_emptyTrackTitle;      // an empty text's first byte
        inline ee::Global<char[0x100], sym::g_trackLabel> g_trackLabel;
        constexpr uint32_t kLabelWidth = 951; // a label this wide or wider is cut
        // The track widget's text: its scale, and how far apart its lines are.
        inline ee::Global<zge::Vec4, sym::g_trackLabelScale> g_trackLabelScale; // 0.75, 0.75, 1, 0
        constexpr float kTrackLineSpacing = 1.25f;
        constexpr uint32_t kSetTrackText = sym::bmx::g_textSetTrackText;         // "SET Text {\"%s\"}"
        constexpr ee::Ptr<const char> kNoEnabledTracks{sym::g_textNoEnabledTracks}; // "SET Text {\"$KSTR_NO_ENABLED_TRACKS\"}"

        // Set until the menus' script functions are registered (the first
        // front end's loading registers them).
        inline ee::Global<uint32_t, sym::g_menuScriptsToRegister> g_menuScriptsToRegister;
        // The script functions the menus' scripts call, each under its own
        // name (the text is the function's name), in the order the game
        // registers them.
        struct ScriptFunctionRow
        {
            uint32_t name, function;
        };
        constexpr ScriptFunctionRow kMenuScriptFunctions[] = {
            {sym::g_textBmxmenuscriptRecordsUpdate, sym::bmxmenuscriptRecordsUpdate},
            {sym::g_textBmxmenuscriptGetLanguage, sym::bmxmenuscriptGetLanguage},
            {sym::g_textBmxmenuscriptSetLanguage, sym::bmxmenuscriptSetLanguage},
            {sym::g_textBmxmenuscriptPlayOnChangeSound, sym::bmxmenuscriptPlayOnChangeSound},
            {sym::g_textBmxmenuscriptPlayOnCancelSound, sym::bmxmenuscriptPlayOnCancelSound},
            {sym::g_textBmxmenuscriptPlayOnSelectSound, sym::bmxmenuscriptPlayOnSelectSound},
            {sym::g_textBmxmenuscriptVibrateController, sym::bmxmenuscriptVibrateController},
            {sym::g_textBmxmenuscriptGetControllerConfig, sym::bmxmenuscriptGetControllerConfig},
            {sym::g_textBmxmenuscriptSetControllerConfig, sym::bmxmenuscriptSetControllerConfig},
            {sym::g_textBmxmenuscriptSetSFXVolume, sym::bmxmenuscriptSetSFXVolume},
            {sym::g_textBmxmenuscriptSetSpeechVolume, sym::bmxmenuscriptSetSpeechVolume},
            {sym::g_textBmxmenuscriptSetMusicVolume, sym::bmxmenuscriptSetMusicVolume},
            {sym::g_textBmxmenuscriptGetSFXVolume, sym::bmxmenuscriptGetSFXVolume},
            {sym::g_textBmxmenuscriptGetSpeechVolume, sym::bmxmenuscriptGetSpeechVolume},
            {sym::g_textBmxmenuscriptGetMusicVolume, sym::bmxmenuscriptGetMusicVolume},
            {sym::g_textBmxmenuscriptEnableDisableTrack, sym::bmxmenuscriptEnableDisableTrack},
            {sym::g_textBmxmenuscriptPrevTrack, sym::bmxmenuscriptPrevTrack},
            {sym::g_textBmxmenuscriptNextTrack, sym::bmxmenuscriptNextTrack},
            {sym::g_textBmxmenuscriptPopulatePlaylist, sym::bmxmenuscriptPopulatePlaylist},
            {sym::g_textBmxmenuscriptStartMusic, sym::bmxmenuscriptStartMusic},
            {sym::g_textBmxmenuscriptStopMusic, sym::bmxmenuscriptStopMusic},
            {sym::g_textBmxmenuscriptSetVideoModeToPAL, sym::bmxmenuscriptSetVideoModeToPAL},
            {sym::g_textBmxmenuscriptSetVideoModeToNTSC, sym::bmxmenuscriptSetVideoModeToNTSC},
            {sym::g_textBmxmenuscriptCheat, sym::bmxmenuscriptCheat},
            {sym::g_textBmxmenuscriptPopulateCheatList, sym::bmxmenuscriptPopulateCheatList},
            {sym::g_textBmxmenuscriptPopulateMediaViewerMenu, sym::bmxmenuscriptPopulateMediaViewerMenu},
            {sym::g_textBmxmenuscriptPopulateSpecialModeMenu, sym::bmxmenuscriptPopulateSpecialModeMenu},
            {sym::g_textBmxmenuscriptGetSelectedMediaFileName, sym::bmxmenuscriptGetSelectedMediaFileName},
            {sym::g_textBmxmenuscriptGetLevelNames, sym::bmxmenuscriptGetLevelNames},
            {sym::g_textBmxmenuscriptGetOptionsMenuItems, sym::bmxmenuscriptGetOptionsMenuItems},
            {sym::g_textBmxmenuscriptGetCharacterNames, sym::bmxmenuscriptGetCharacterNames},
            {sym::g_textBmxmenuscriptGetCharacterInfo, sym::bmxmenuscriptGetCharacterInfo},
            {sym::g_textBmxmenuscriptGetCharacterLvlInfo, sym::bmxmenuscriptGetCharacterLvlInfo},
            {sym::g_textBmxmenuscriptGetTeamNames, sym::bmxmenuscriptGetTeamNames},
            {sym::g_textBmxmenuscriptSetCharacterNum, sym::bmxmenuscriptSetCharacterNum},
            {sym::g_textBmxmenuscriptGetCharacterNum, sym::bmxmenuscriptGetCharacterNum},
            {sym::g_textBmxmenuscriptGetTeamMemCharNum, sym::bmxmenuscriptGetTeamMemCharNum},
            {sym::g_textBmxmenuscriptGetPlayerNum, sym::bmxmenuscriptGetPlayerNum},
            {sym::g_textBmxmenuscriptIsPlayerCharCreated, sym::bmxmenuscriptIsPlayerCharCreated},
            {sym::g_textBmxmenuscriptGetGameActionMode, sym::bmxmenuscriptGetGameActionMode},
            {sym::g_textBmxmenuscriptGetCredits, sym::bmxmenuscriptGetCredits},
            {sym::g_textBmxmenuscriptPlayMovie, sym::bmxmenuscriptPlayMovie},
            {sym::g_textBmxmenuscriptStopMovie, sym::bmxmenuscriptStopMovie},
            {sym::g_textBmxmenuscriptSelectedDisabledMenuItem, sym::bmxmenuscriptSelectedDisabledMenuItem},
        };

        // The video modes' screens: PAL 50 Hz, 640 by 512; NTSC 60 Hz, 640
        // by 448.
        constexpr uint32_t kPalHz = 50, kNtscHz = 60;
        constexpr uint32_t kScreenWidth = 640, kPalHeight = 512, kNtscHeight = 448;
        // The first character a player creates (the rider characters before
        // it are the game's).
        constexpr uint8_t kFirstCreatedCharacter = 13;
        // A menu item whose text starts with '!' is disabled.
        constexpr ee::Ptr<const char> kSelectedText{sym::g_textMenuSelectedText}; // "SelectedText"
        constexpr char kDisabledMark = '!';

        // The engine's hook the video mode functions call with their rate
        // (its body is empty in this build: named for its callers).
        // Scaffolding (address-call): until each has its body.
        namespace calls
        {
            void videoModeHook(uint32_t hz) { ee::game(sym::videoModeHook, hz); }
        }

        // The end of the track buttons: the track picked starts and the
        // widget shows its label; with none on, the widget says so.
        void showPickedTrack(ee::Ptr<char> line, ee::Ptr<zge::Vec4> scale, ee::Ptr<uint32_t> arguments, int32_t track, uint32_t on)
        {
            if (on)
            {
                *scale = *g_trackLabelScale;
                const ee::Ptr<char> label = trackLabel(static_cast<uint32_t>(track), scale, 0, 0.0f, kTrackLineSpacing);
                zge::formatString(line, kSetTrackText, label);
                soundEvent(kSoundPlayTrack, static_cast<uint32_t>(track), 0, 0, 0);
            }
            else
                zge::stringCopy(line, kNoEnabledTracks);
            zge::zgui::runScript(Node(arguments[0]), line, 0);
        }

        void setChannelVolume(ee::Ptr<uint32_t> arguments, uint32_t channel)
        {
            ee::Local<ee::Float> value;
            ee::Local<int32_t> playTest;
            *value = 90.0f;
            *playTest = 1;
            zge::zgui::variableFloatValue(Node(arguments[0]), value);
            zge::zgui::variableIntValueAgain(Node(arguments[1]), playTest);
            const int32_t slider = ee::toInt(*value);
            const int32_t volume = slider <= 0 ? 0 : slider < kLoudest ? slider : kLoudest;
            g_savedGame->channelVolumes[channel] = static_cast<uint8_t>(volume);
            soundEvent(kSoundSetVolume, channel, static_cast<uint8_t>(volume), *playTest, 0);
        }

        void showChannelVolume(ee::Ptr<uint32_t> arguments, uint32_t channel)
        {
            ee::Local<ee::Float> shown;
            const uint32_t volume = g_savedGame->channelVolumes[channel];
            const uint32_t even = volume + (volume & 1);
            *shown = ee::toFloat(static_cast<int32_t>(even <= kSliderTop ? even : kSliderTop));
            zge::zgui::setVariable(Node(arguments[0]), kCurValue, shown);
            zge::zgui::runScript(Node(arguments[0]), kPlayOnChange, 0);
        }
    }

    void bmxmenuscriptPlayMovie(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x100]> name;
        ee::Local<int32_t> loop;
        memset(name, 0, sizeof(char[0x100]));
        *loop = 0;
        zge::zgui::variableToText(Node(arguments[0]), name);
        zge::zgui::variableIntValueAgain(Node(arguments[1]), loop);
        playMenuMovie(name, static_cast<uint32_t>(*loop));
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPlayMovie, Verified);

    void bmxmenuscriptStopMovie()
    {
        game::tail(stopMenuMovie);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptStopMovie, Verified);

    void bmxmenuscriptPlayOnChangeSound()
    {
        if (g_bmxZgui->changeSound)
            g_bmxZgui->changeSound();
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPlayOnChangeSound, Verified);

    void bmxmenuscriptPlayOnCancelSound()
    {
        if (g_bmxZgui->cancelSound)
            g_bmxZgui->cancelSound();
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPlayOnCancelSound, Verified);

    void bmxmenuscriptPlayOnSelectSound()
    {
        if (g_bmxZgui->selectSound)
            g_bmxZgui->selectSound();
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPlayOnSelectSound, Verified);

    void bmxmenuscriptVibrateController()
    {
        const rider::PadInput *input = rider::padInput(currentPlayer());
        const uint32_t smallMotor = static_cast<uint32_t>(ee::toInt(ee::Float(kShakeSmallMotor)));
        const uint32_t bigMotor = static_cast<uint32_t>(ee::toInt(*g_menuShakeBigMotor));
        game::tail(zge::padVibrate, input->pad, smallMotor, bigMotor, kShakeTime, 0);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptVibrateController, Verified);

    void bmxmenuscriptGetControllerConfig(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> player;
        ee::Local<uint32_t> config;
        ee::Local<uint32_t> vibration;
        *vibration = 1;
        *player = 0;
        *config = 0;
        // (The first argument is read, made a player index, and not used.)
        zge::zgui::variableIntValue(Node(arguments[0]), player);
        *player = *player - 1;
        const Node screen(arguments[1]);
        *config = (*g_playerSetups)[currentPlayer()].controllerLayout;
        zge::zgui::setVariable(screen, kSelectedIndex, config);
        *vibration = (*g_playerSetups)[currentPlayer()].flags & kSetupVibration;
        zge::zgui::setVariable(screen, kVibrationOn, vibration);
        zge::zgui::runScript(screen, kVibrationRefresh, 0);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetControllerConfig, Verified);

    void bmxmenuscriptSetControllerConfig(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> player;
        ee::Local<uint32_t> config;
        ee::Local<uint32_t> vibration;
        *vibration = 1;
        *player = 0;
        *config = 0;
        zge::zgui::variableIntValue(Node(arguments[0]), player);
        const Node screen(arguments[1]);
        *player = *player - 1;
        zge::zgui::copyVariable(screen, kSelectedIndex, config);
        zge::zgui::copyVariable(screen, kVibrationOn, vibration);
        // Both players get the settings, whoever asked.
        const uint8_t on = static_cast<uint8_t>(*vibration) & kSetupVibration;
        PlayerSetup &first = (*g_playerSetups)[0];
        PlayerSetup &second = (*g_playerSetups)[1];
        first.flags = static_cast<uint8_t>((first.flags & ~kSetupVibration) | on);
        second.flags = static_cast<uint8_t>((second.flags & ~kSetupVibration) | on);
        second.controllerLayout = static_cast<uint8_t>(*config);
        first.controllerLayout = static_cast<uint8_t>(*config);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptSetControllerConfig, Verified);

    void bmxmenuscriptSetSFXVolume(ee::Ptr<uint32_t> arguments)
    {
        setChannelVolume(arguments, kSfxChannel);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptSetSFXVolume, Verified);

    void bmxmenuscriptSetSpeechVolume(ee::Ptr<uint32_t> arguments)
    {
        setChannelVolume(arguments, kSpeechChannel);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptSetSpeechVolume, Verified);

    void bmxmenuscriptSetMusicVolume(ee::Ptr<uint32_t> arguments)
    {
        setChannelVolume(arguments, kMusicChannel);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptSetMusicVolume, Verified);

    void bmxmenuscriptGetSFXVolume(ee::Ptr<uint32_t> arguments)
    {
        showChannelVolume(arguments, kSfxChannel);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetSFXVolume, Verified);

    void bmxmenuscriptGetSpeechVolume(ee::Ptr<uint32_t> arguments)
    {
        showChannelVolume(arguments, kSpeechChannel);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetSpeechVolume, Verified);

    void bmxmenuscriptGetMusicVolume(ee::Ptr<uint32_t> arguments)
    {
        showChannelVolume(arguments, kMusicChannel);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetMusicVolume, Verified);

    ee::Ptr<char> trackLabel(uint32_t track, const zge::Vec4 *scale, uint32_t monospaced, ee::Float angle, ee::Float lineSpacing)
    {
        ee::Local<char[0x100]> title;
        ee::Local<zge::TextBounds> bounds;
        ee::Local<zge::TextStyle> style;
        title[0] = *g_emptyTrackTitle;
        memset(title.address() + 1, 0, sizeof(char[0x100]) - 1);
        const ee::Ptr<zge::zgui::UiAsset> fontAsset = zge::zgui::findGuiAsset(kMenuFont, kFontAsset);
        zge::stringCopy(title, soundEvent(kSoundTrackArtist, track, 0, 0, 0));
        if (title[0] != 0)
            zge::appendString(title, kLabelSeparator);
        zge::appendString(title, soundEvent(kSoundTrackTitle, track, 0, 0, 0));

        zge::memoryFill(style, 0, sizeof(zge::TextStyle));
        style->angle = angle;
        style->monospaced = monospaced & 1;
        style->lineSpacing = lineSpacing;
        style->scaleX = scale->x;
        style->scaleY = scale->y;
        style->scaleExtra[0] = scale->z;
        style->scaleExtra[1] = scale->w;
        const ee::Ptr<char> label(sym::g_trackLabel);
        zge::expandStringKeys(label, title);
        const ee::Ptr<zge::Font> font(fontAsset->object);
        zge::measureText(font, style, labelText(label), bounds, ee::Ptr<int32_t>());
        if (sce::libgcc::fptoui(bounds->right - bounds->left) >= kLabelWidth)
        {
            // The last three characters become "...", then a character at a
            // time goes from before the dots until the label fits.
            const int32_t length = zge::stringLength(label);
            zge::stringCopy(label + (length - 3), kEllipsis);
            ee::Ptr<char> end = label + (length - 1);
            do
            {
                end[-3] = '.';
                end[0] = 0;
                zge::measureText(ee::Ptr<zge::Font>(fontAsset->object), style, labelText(label), bounds, ee::Ptr<int32_t>());
                end = end - 1;
            } while (sce::libgcc::fptoui(bounds->right - bounds->left) >= kLabelWidth);
        }
        return label;
    }
    READABLE_FUNCTION_WAITS(trackLabel, Verified);

    void bmxmenuscriptEnableDisableTrack(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> track;
        *track = 0;
        zge::zgui::variableIntValue(Node(arguments[0]), track);
        if (soundEvent(kSoundTrackIsOn, static_cast<uint32_t>(*track), 0, 0, 0))
            soundEvent(kSoundTrackOff, static_cast<uint32_t>(*track), 0, 0, 0);
        else
            soundEvent(kSoundTrackOn, static_cast<uint32_t>(*track), 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptEnableDisableTrack, Verified);

    void bmxmenuscriptNextTrack(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x200]> line;
        ee::Local<zge::Vec4> scale;
        memset(line, 0, sizeof(char[0x200]));
        const int32_t start = static_cast<int32_t>(soundEvent(kSoundCurrentTrack, 0, 0, 0, 0));
        const int32_t count = static_cast<int32_t>(soundEvent(kSoundTrackCount, 0, 0, 0, 0));
        int32_t track = start;
        uint32_t on = 0;
        if (count > 0)
        {
            do
            {
                track = track + 1 < count ? track + 1 : 0;
                on = soundEvent(kSoundTrackIsOn, static_cast<uint32_t>(track), 0, 0, 0);
            } while (!on && track != start);
        }
        showPickedTrack(line, scale, arguments, track, on);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptNextTrack, Verified);

    void bmxmenuscriptPrevTrack(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x200]> line;
        ee::Local<zge::Vec4> scale;
        memset(line, 0, sizeof(char[0x200]));
        const int32_t start = static_cast<int32_t>(soundEvent(kSoundCurrentTrack, 0, 0, 0, 0));
        const int32_t count = static_cast<int32_t>(soundEvent(kSoundTrackCount, 0, 0, 0, 0));
        int32_t track = start;
        uint32_t on = 0;
        if (count > 0)
        {
            do
            {
                track = track - 1 > -1 ? track - 1 : count - 1;
                on = soundEvent(kSoundTrackIsOn, static_cast<uint32_t>(track), 0, 0, 0);
            } while (!on && track != start);
        }
        showPickedTrack(line, scale, arguments, track, on);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPrevTrack, Verified);

    int32_t registerMenuScriptFunctions()
    {
        zge::releaseAllEmptyPoolBlocks();
        if (*g_menuScriptsToRegister)
        {
            for (const ScriptFunctionRow &row : kMenuScriptFunctions)
                zge::zgui::registerScriptFunction(row.name, row.function);
            *g_menuScriptsToRegister = 0;
        }
        return 1;
    }
    READABLE_FUNCTION(registerMenuScriptFunctions, Verified);

    // The menu block's frame and exit hooks, empty (inferred names: the
    // front end's frame and exit call them). The frame's is always 1.
    int32_t unusedFrontEndFrameHook()
    {
        return 1;
    }
    READABLE_FUNCTION(unusedFrontEndFrameHook, Verified);

    void unusedFrontEndExitHook()
    {
    }
    READABLE_FUNCTION(unusedFrontEndExitHook, Verified);

    // The menus' language (the saved game's), and setting it.
    void bmxmenuscriptGetLanguage(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<uint32_t> answered;
        *answered = g_savedGame->language;
        zge::zgui::setVariableValue(Node(arguments[0]), answered);
    }
    READABLE_FUNCTION(bmxmenuscriptGetLanguage, Draft);

    void bmxmenuscriptSetLanguage(ee::Ptr<uint32_t> arguments)
    {
        // (The value keeps what the stack held when the argument has none,
        // as the original's.)
        ee::Local<int32_t> language;
        zge::zgui::variableIntValue(Node(arguments[0]), language);
        setLanguage(static_cast<uint8_t>(*language));
    }
    READABLE_FUNCTION(bmxmenuscriptSetLanguage, Draft);

    // (level, frame): the records screen's rows of a level.
    void bmxmenuscriptRecordsUpdate(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> level;
        zge::zgui::variableIntValue(Node(arguments[0]), level);
        fillRecordsFrame(Node(arguments[1]), static_cast<uint8_t>(*level));
    }
    READABLE_FUNCTION(bmxmenuscriptRecordsUpdate, Verified);

    // The front end's music on (the sound's front end mode) and off.
    int32_t bmxmenuscriptStartMusic()
    {
        return game::tail(setGameMode, kFrontEndMode);
    }
    READABLE_FUNCTION(bmxmenuscriptStartMusic, Verified);

    int32_t bmxmenuscriptStopMusic()
    {
        return game::tail(setGameMode, kSoundModeNone);
    }
    READABLE_FUNCTION(bmxmenuscriptStopMusic, Verified);

    // The video mode: its rate, and the screen's size for the cameras.
    void bmxmenuscriptSetVideoModeToPAL()
    {
        calls::videoModeHook(kPalHz);
        game::tail(zge::resetCameraViewports, kScreenWidth, kPalHeight);
    }
    READABLE_FUNCTION(bmxmenuscriptSetVideoModeToPAL, Draft);

    void bmxmenuscriptSetVideoModeToNTSC()
    {
        calls::videoModeHook(kNtscHz);
        game::tail(zge::resetCameraViewports, kScreenWidth, kNtscHeight);
    }
    READABLE_FUNCTION(bmxmenuscriptSetVideoModeToNTSC, Verified);

    // The current player's rider character, whether it is one the player
    // created, and the current player.
    void bmxmenuscriptGetCharacterNum(ee::Ptr<uint32_t> arguments)
    {
        const Node variable(arguments[0]);
        ee::Local<uint32_t> answered;
        *answered = (*g_playerSetups)[currentPlayer()].character;
        zge::zgui::setVariableValue(variable, answered);
    }
    READABLE_FUNCTION(bmxmenuscriptGetCharacterNum, Verified);

    void bmxmenuscriptIsPlayerCharCreated(ee::Ptr<uint32_t> arguments)
    {
        const Node variable(arguments[0]);
        ee::Local<uint32_t> answered;
        *answered = (*g_playerSetups)[currentPlayer()].character >= kFirstCreatedCharacter ? 1 : 0;
        zge::zgui::setVariableValue(variable, answered);
    }
    READABLE_FUNCTION(bmxmenuscriptIsPlayerCharCreated, Verified);

    void bmxmenuscriptGetPlayerNum(ee::Ptr<uint32_t> arguments)
    {
        const Node variable(arguments[0]);
        ee::Local<uint32_t> answered;
        *answered = currentPlayer();
        zge::zgui::setVariableValue(variable, answered);
    }
    READABLE_FUNCTION(bmxmenuscriptGetPlayerNum, Verified);

    // What is being played (the session's kind: the scripts' "game action
    // mode").
    void bmxmenuscriptGetGameActionMode(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<uint32_t> answered;
        *answered = *g_sessionKind;
        zge::zgui::setVariableValue(Node(arguments[0]), answered);
    }
    READABLE_FUNCTION(bmxmenuscriptGetGameActionMode, Verified);

    // (member, out): the team member's rider character.
    void bmxmenuscriptGetTeamMemCharNum(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> member;
        ee::Local<uint32_t> character;
        const Node out(arguments[1]);
        zge::zgui::variableIntValue(Node(arguments[0]), member);
        *character = g_savedGame->teamMembers[*member];
        zge::zgui::setVariableValue(out, character);
    }
    READABLE_FUNCTION(bmxmenuscriptGetTeamMemCharNum, Draft);

    // (out, item): 1 when the item's selected text marks it disabled.
    void bmxmenuscriptSelectedDisabledMenuItem(ee::Ptr<uint32_t> arguments)
    {
        const Node out(arguments[0]);
        const ee::Ptr<ee::Ptr<const char>> text(zge::zgui::variableValue(Node(arguments[1]), kSelectedText));
        ee::Local<uint32_t> answered;
        *answered = (*text)[0] == kDisabledMark ? 1 : 0;
        zge::zgui::setVariableValue(out, answered);
    }
    READABLE_FUNCTION(bmxmenuscriptSelectedDisabledMenuItem, Verified);

    namespace
    {
        // The cheat list's two texts: emptied, then the none-yet line.
        constexpr ee::Ptr<char> kClearText{sym::g_textMenuClearText};   // "SET Text {\"\"}"
        constexpr ee::Ptr<char> kNoCheatsText{sym::g_textMenuNoCheatsText}; // "SET Text {\"$KSTR_NO_CHEATS_UNLOCKED_YET\"}"
        // The options menu's items, with two marks the format takes before
        // the second and sixth items: none, and "!!" (greyed out).
        constexpr uint32_t kOptionsItems = sym::g_textOptionsMenuItems; // "$KSTR_COM_OPTIONS_OPT_00\n%s$KSTR_..._01 .. %s.._05"
    }

    // bmxmenuscriptPopulateCheatList (list): the list emptied and given the
    // no-cheats-yet line (this build opens no cheat from the list).
    void bmxmenuscriptPopulateCheatList(ee::Ptr<uint32_t> arguments)
    {
        const Node list(arguments[0]);
        zge::zgui::runScript(list, kClearText, 0);
        game::tail(zge::zgui::runScript, list, kNoCheatsText, 0);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPopulateCheatList, Draft);

    // bmxmenuscriptGetOptionsMenuItems (list): the options menu's six items
    // as the list's value.
    void bmxmenuscriptGetOptionsMenuItems(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x400]> items;
        if (!arguments)
            return;
        const Node list(arguments[0]);
        if (!list)
            return;
        items[0] = 0;
        zge::formatString(items, kOptionsItems, ee::Ptr<const char>(sym::g_textMenuScriptEmpty), ee::Ptr<const char>(sym::g_textMenuScriptGreyMark));
        zge::zgui::setVariableValue(list, items);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetOptionsMenuItems, Verified);

    namespace
    {
        // The menus' loading script played while the rider object changes.
        constexpr uint32_t kStartLoading = sym::g_textMenuStartLoading; // "PLAY UI_Menu.StartLoading"
        // The characters' list: a line each, "FIRST LAST" in capitals, a
        // locked character's marked disabled.
        constexpr uint32_t kFullName = sym::g_textCharacterFullName;       // "%s %s"
        constexpr uint32_t kListLine = sym::g_textCharacterListLine;       // "%s%s%s"
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textMenuScriptBreak}; // "\\n" (the script's line break)
        constexpr ee::Ptr<const char> kNoText{sym::g_textMenuScriptEmpty};    // ""
        constexpr ee::Ptr<const char> kDisabled{sym::g_textMenuScriptDisabled}; // "!"
    }

    // bmxmenuscriptSetCharacterNum (character): the current player rides the
    // character; the menus' rider object is hidden, the loading played and
    // the object dressed in the character's model, then shown.
    void bmxmenuscriptSetCharacterNum(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<int32_t> character;
        zge::zgui::variableIntValue(Node(arguments[0]), character);
        (*g_playerSetups)[currentPlayer()].character = static_cast<uint8_t>(*character);
        showMenuModel(0, 0);
        zge::zgui::runScript(Node(), ee::Ptr<char>(kStartLoading), 0);
        setMenuModel(playerCharacter(currentPlayer())->model);
        showMenuModel(1, 0);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptSetCharacterNum, Verified);

    // bmxmenuscriptGetCharacterNames (list): the seventeen characters' names
    // as the list's value.
    void bmxmenuscriptGetCharacterNames(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x80]> name;
        ee::Local<char[0x480]> names;

        const Node list(arguments[0]);
        names[0] = 0;
        name[0] = 0;
        for (uint32_t character = 0; character < kRiderCharacters; ++character)
        {
            RiderCharacter &record = g_savedGame->characters[character];
            zge::formatString(name, kFullName, ee::Ptr<const char>(record.firstName), ee::Ptr<const char>(record.lastName));
            zge::stringToUpper(name);
            const int32_t end = zge::stringLength(names);
            zge::formatString(names.address() + static_cast<uint32_t>(end), kListLine, end != 0 ? kLineBreak : kNoText,
                              (record.flags & kCharacterLocked) != 0 ? kDisabled : kNoText, name);
        }
        zge::zgui::setVariableValue(list, names);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetCharacterNames, Verified);

    namespace
    {
        // The level select's levels by item (front_end_menu.cpp's), and the
        // eight the select lists: the six challenge levels and the two
        // competitions.
        constexpr uint32_t kLevelSelectItems = 12, kListedLevels = 8;
        inline ee::Global<uint8_t[kLevelSelectItems], sym::g_levelSelectLevels> g_levelSelectLevels;
        const ee::Ptr<LevelInfo> kLevelInfos(sym::g_levels);
        // The level's counts for the select: goals done of all, then gaps.
        constexpr uint32_t kLevelCounts = sym::g_textLevelGoalCounts; // "%d/%d\n%d/%d"
        // The two competitions (levels 6 and 7) show no counts; the last
        // level a character keeps a record of.
        constexpr uint8_t kFirstCompetitionLevel = 6, kCompetitionLevels = 2;
        constexpr int32_t kLastRecordLevel = 12;
    }

    // bmxmenuscriptGetLevelNames (list): the level select's eight levels'
    // titles as the list's value, a level no player has open marked
    // disabled.
    void bmxmenuscriptGetLevelNames(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x480]> names;

        const Node list(arguments[0]);
        names[0] = 0;
        for (uint32_t item = 0; item < kListedLevels; ++item)
        {
            const uint8_t level = (*g_levelSelectLevels)[item];
            bool open = false;
            const int32_t end = zge::stringLength(names);
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if ((playerLevelGoals(player, level)->flags & kLevelGoalsOpen) != 0)
                {
                    open = true;
                    break;
                }
            }
            zge::formatString(names.address() + static_cast<uint32_t>(end), kListLine, end != 0 ? kLineBreak : kNoText,
                              open ? kNoText : kDisabled, (kLevelInfos + level)->title);
        }
        zge::zgui::setVariableValue(list, names);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetLevelNames, Verified);

    // bmxmenuscriptGetCharacterLvlInfo (out, item): the current player's
    // character's goals and gaps done of all, for the level of the level
    // select's item (an item past the twelve is the last), as the out
    // variable's value; nothing for a competition.
    void bmxmenuscriptGetCharacterLvlInfo(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x400]> text;
        ee::Local<int32_t> item;

        const Node out(arguments[0]);
        zge::zgui::variableIntValue(Node(arguments[1]), item);
        int32_t index = *item;
        if (index >= static_cast<int32_t>(kLevelSelectItems))
            index = kLevelSelectItems - 1;
        if (index <= -1)
            index = 0;
        int32_t level = (*g_levelSelectLevels)[index];
        const ee::Ptr<RiderCharacter> character = playerCharacter(currentPlayer());
        if (!out || !character)
            return;
        if (level > kLastRecordLevel)
            level = kLastRecordLevel;
        if (level <= -1)
            level = 0;
        if (static_cast<uint32_t>(level - kFirstCompetitionLevel) < kCompetitionLevels)
        {
            text[0] = 0;
        }
        else
        {
            const LevelGoals &goals = character->levels[level];
            zge::formatString(text, kLevelCounts, goals.goalsDone, goals.goals, goals.gapsDone, goals.gaps);
        }
        zge::zgui::setVariableValue(out, text);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetCharacterLvlInfo, Verified);

    namespace
    {
        // The cheat codes the cheats menu takes, and what each opens in the
        // saved game: a level, a movie, a character or a feature (or every
        // one), or a character's bikes.
        enum CheatOpens : uint8_t
        {
            kCheatLevels = 0x01,     // the level opened for every character
            kCheatMovies = 0x02,     // the movie won
            kCheatCharacters = 0x04, // the character unlocked
            kCheatBikes = 0x08,      // the character given every bike
            kCheatFeatures = 0x10,   // the feature opened and turned on
        };
        struct CheatCode
        {
            ee::Ptr<const char> code; // +0x00 ("XXX RATED CHEAT", "BRONXCHEER", ..)
            uint8_t opens;            // +0x04 CheatOpens
            int8_t which;             // +0x05 the level, movie, character or feature; kCheatEveryOne: all
            uint8_t _06[2];
        };
        static_assert(sizeof(CheatCode) == 0x08 && offsetof(CheatCode, which) == 0x05);
        constexpr uint32_t kCheatCodes = 54;
        inline ee::Global<CheatCode[kCheatCodes], sym::g_cheatCodes> g_cheatCodes;
        constexpr int8_t kCheatEveryOne = -1;
        constexpr uint8_t kEveryBike = 5;
        constexpr uint32_t kFeatures = 6;
        constexpr ee::Ptr<const char> kCheatString{sym::g_textCheatString};         // "String"
        constexpr ee::Ptr<const char> kCheatSuccessful{sym::g_textCheatSuccessful}; // "CheatSuccessful"
        // The sound of a code taken (the menus' blocked sound for one not).
        constexpr uint32_t kSoundCheatTaken = 0x42;

        bool cheatOpens(int8_t which, uint32_t index) { return which == kCheatEveryOne || index == static_cast<uint32_t>(which); }
    }

    // bmxmenuscriptCheat (entry): the code typed in the entry ("String")
    // looked up among the cheat codes; the first that matches opens what it
    // opens. The entry's "CheatSuccessful" is set to whether one matched,
    // and a sound says so.
    void bmxmenuscriptCheat(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x80]> typed;
        ee::Local<int32_t> successful;

        const Node entry(arguments[0]);
        *successful = zge::zgui::copyVariable(entry, kCheatString, typed);
        for (uint32_t row = 0; row < kCheatCodes; ++row)
        {
            const CheatCode &cheat = (*g_cheatCodes)[row];
            if (zge::compareStrings(typed, cheat.code) != 0)
                continue;
            if ((cheat.opens & kCheatLevels) != 0)
            {
                for (uint32_t character = 0; character < kRiderCharacters; ++character)
                {
                    for (uint32_t level = 0; level < kLevels; ++level)
                    {
                        if (cheatOpens(cheat.which, level))
                            g_savedGame->characters[character].levels[level].flags |= kLevelGoalsOpen;
                    }
                }
            }
            if ((cheat.opens & kCheatMovies) != 0)
            {
                for (uint32_t movie = 0; movie < kSavedMovies; ++movie)
                {
                    if (cheatOpens(cheat.which, movie))
                        g_savedGame->movies[movie] |= kMovieRewardBit0;
                }
            }
            if ((cheat.opens & kCheatCharacters) != 0)
            {
                for (uint32_t character = 0; character < kRiderCharacters; ++character)
                {
                    if (cheatOpens(cheat.which, character))
                        g_savedGame->characters[character].flags &= static_cast<uint16_t>(~kCharacterLocked);
                }
            }
            if ((cheat.opens & kCheatBikes) != 0)
            {
                for (uint32_t character = 0; character < kRiderCharacters; ++character)
                {
                    if (cheatOpens(cheat.which, character))
                        g_savedGame->characters[character].bikes = kEveryBike;
                }
            }
            if ((cheat.opens & kCheatFeatures) != 0)
            {
                uint16_t features = g_savedGame->features;
                for (uint32_t feature = 0; feature < kFeatures; ++feature)
                {
                    if (cheatOpens(cheat.which, feature))
                    {
                        const uint16_t bit = static_cast<uint16_t>(1u << feature);
                        features = static_cast<uint16_t>(features | bit);
                        g_savedGame->featuresOn = static_cast<uint16_t>(g_savedGame->featuresOn | bit);
                    }
                }
                g_savedGame->features = features;
            }
            *successful = 1;
            *successful = zge::zgui::setVariable(entry, kCheatSuccessful, successful);
            soundEvent(kSoundCheatTaken, 0, 0, 0, 0);
            return;
        }
        *successful = 0;
        *successful = zge::zgui::setVariable(entry, kCheatSuccessful, successful);
        soundEvent(kSoundMenuBlocked, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptCheat, Verified);

    namespace
    {
        // The rider select's counts are the six challenge levels' (the
        // levels before the competitions).
        constexpr uint32_t kChallengeLevels = 6;
    }

    // characterGoalsDone, characterGoals, characterGapsDone, characterGaps
    // (inferred names): a character's goals done, goals, gaps done and gaps
    // over the six challenge levels.
    uint32_t characterGoalsDone(const RiderCharacter *character)
    {
        uint32_t sum = 0;
        for (uint32_t level = 0; level < kChallengeLevels; ++level)
            sum += character->levels[level].goalsDone;
        return sum;
    }
    READABLE_FUNCTION(characterGoalsDone, Verified);

    uint32_t characterGoals(const RiderCharacter *character)
    {
        uint32_t sum = 0;
        for (uint32_t level = 0; level < kChallengeLevels; ++level)
            sum += character->levels[level].goals;
        return sum;
    }
    READABLE_FUNCTION(characterGoals, Verified);

    uint32_t characterGapsDone(const RiderCharacter *character)
    {
        uint32_t sum = 0;
        for (uint32_t level = 0; level < kChallengeLevels; ++level)
            sum += character->levels[level].gapsDone;
        return sum;
    }
    READABLE_FUNCTION(characterGapsDone, Verified);

    uint32_t characterGaps(const RiderCharacter *character)
    {
        uint32_t sum = 0;
        for (uint32_t level = 0; level < kChallengeLevels; ++level)
            sum += character->levels[level].gaps;
        return sum;
    }
    READABLE_FUNCTION(characterGaps, Verified);

    namespace
    {
        // The rider select's info: the counts, the stats as bars (the
        // rider's and the bike's, eight of the nine), the bike's name.
        constexpr uint32_t kInfoCounts = sym::g_textCharacterInfoCounts; // "%d/%d\\n%d/%d\\n%d/%d\\n%d/%d\\n"
        constexpr uint32_t kInfoStats = sym::g_textCharacterInfoStats;   // "%s%s%s%s\\n" eight times, then " %s"
        constexpr uint32_t kBikeName = sym::g_textCharacterBikeName;     // "$KSTR_BIKENAME_%1d_%1d"
        constexpr uint32_t kRiderColour = sym::g_textStatRiderColour;    // "#RGB{50 50 50}"
        constexpr uint32_t kBikeColour = sym::g_textStatBikeColour;      // "#RGB{125 75 15}"
        // A stat's bar: "" .. ">>>>>>>>" by its value (0 .. 8).
        constexpr uint8_t kLongestBar = 8;
        struct StatBars
        {
            ee::Ptr<const char> bar[kLongestBar + 1];
        };
        inline ee::Global<StatBars, sym::g_statBars> g_statBars;
        // The stats by character (the riders' table, a new record's too),
        // and by bike: a line's five bikes, for each of the seven lines.
        constexpr uint32_t kStats = 9, kBikeLines = 7, kBikesInLine = 5;
        inline ee::Global<uint8_t[kRiderCharacters][kStats], sym::g_characterStats> g_characterStats;
        inline ee::Global<uint8_t[kBikeLines][kBikesInLine][kStats], sym::g_bikeStats> g_bikeStats;
        // The bikes' count's whole, and the bike parts' (four bikes of six).
        constexpr uint32_t kAllBikes = 5, kAllBikeParts = kCharacterBikes * kBikeParts;
    }

    // bmxmenuscriptGetCharacterInfo (counts, stats, bike): the current
    // player's character's goals and gaps done of all, its bikes of five,
    // its rider's and bike's stats as bars and its bike's name, as the three
    // variables' values. (A character, line or bike past the tables' last is
    // the last.)
    void bmxmenuscriptGetCharacterInfo(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x400]> text;
        ee::Local<StatBars> bars;

        const Node bikeOut(arguments[2]);
        const Node countsOut(arguments[0]);
        const Node statsOut(arguments[1]);
        *bars = *g_statBars;
        if (!countsOut || !statsOut || !bikeOut)
            return;
        uint32_t index = (*g_playerSetups)[currentPlayer()].character;
        if (index > kRiderCharacters - 1)
            index = kRiderCharacters - 1;
        RiderCharacter *const character = &g_savedGame->characters[index];

        const uint32_t goalsDone = characterGoalsDone(character);
        const uint32_t goals = characterGoals(character);
        const uint32_t gapsDone = characterGapsDone(character);
        const uint32_t gaps = characterGaps(character);
        zge::formatStringWithStackWords(text, kInfoCounts, goalsDone, goals, gapsDone, gaps, character->bikePartsFound, kAllBikeParts, character->bikes,
                          kAllBikes);
        zge::zgui::setVariableValue(countsOut, text);

        const uint32_t line = character->bike < kBikeLines - 1 ? character->bike : kBikeLines - 1;
        const uint32_t bike = character->bikeChoice < kBikesInLine - 1 ? character->bikeChoice : kBikesInLine - 1;
        const auto riderBar = [&](uint32_t stat) {
            const uint8_t value = (*g_characterStats)[index][stat];
            return bars->bar[value < kLongestBar ? value : kLongestBar];
        };
        const auto bikeBar = [&](uint32_t stat) {
            const uint8_t value = (*g_bikeStats)[line][bike][stat];
            return bars->bar[value < kLongestBar ? value : kLongestBar];
        };
        // (stat 3 is not shown)
        zge::formatStringWithStackWords(text, kInfoStats,
                          kRiderColour, riderBar(0), kBikeColour, bikeBar(0),
                          kRiderColour, riderBar(1), kBikeColour, bikeBar(1),
                          kRiderColour, riderBar(2), kBikeColour, bikeBar(2),
                          kRiderColour, riderBar(4), kBikeColour, bikeBar(4),
                          kRiderColour, riderBar(5), kBikeColour, bikeBar(5),
                          kRiderColour, riderBar(6), kBikeColour, bikeBar(6),
                          kRiderColour, riderBar(7), kBikeColour, bikeBar(7),
                          kRiderColour, riderBar(8), kBikeColour, bikeBar(8),
                          kRiderColour);
        zge::zgui::setVariableValue(statsOut, text);
        zge::formatString(text, kBikeName, line, bike);
        zge::zgui::setVariableValue(bikeOut, text);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetCharacterInfo, Verified);

    namespace
    {
        // The credits' text file.
        constexpr ee::Ptr<const char> kCreditsFile{sym::g_textCreditsFile}; // "ASSETS\\credits.txt"
        constexpr ee::Ptr<const char> kCreditsText{sym::g_textCreditsText}; // "Text"
        constexpr uint32_t kFileOk = 1;
    }

    // bmxmenuscriptGetCredits (text): the credits file read whole (its size
    // asked first, the file closed), its last byte made the end, and set as
    // the text's "Text"; nothing when the file will not open, size or close.
    void bmxmenuscriptGetCredits(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<uint32_t> bytes;

        if (!arguments || arguments[0] == 0)
            return;
        const ee::Ptr<zge::ZFile> file = zge::openFile(kCreditsFile, zge::kFileModeRead, 0).file();
        if (!file)
            return;
        if (zge::fileSize(file, bytes, 0) != kFileOk)
            return;
        if (zge::closeFile(file, 0) != kFileOk)
            return;
        const uint32_t text = zge::loadFileBytes(kCreditsFile, *bytes);
        if (text == 0)
            return;
        const ee::Ptr<char> loaded(text);
        loaded[*bytes - 1] = 0;
        zge::zgui::setVariable(arguments[0], kCreditsText, text);
        zge::freeLoadedFile(text);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetCredits, Verified);

    namespace
    {
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        // The playlist: the list's lines ("STRCAT Items"), the checkbox
        // column beside it, and the track playing under it.
        constexpr ee::Ptr<const char> kActiveCheckboxes{sym::g_textActiveCheckboxes}; // "ActiveCheckboxes"
        constexpr ee::Ptr<const char> kCurrentTrack{sym::g_textCurrentTrack};         // "CurrentTrack"
        constexpr ee::Ptr<char> kClearItems{sym::g_textSetItems};                     // "SET Items {\"\"}"
        constexpr uint32_t kPlaylistLine = sym::g_textPlaylistLine;                   // "STRCAT Items {\"%s %s\"}"
        constexpr uint32_t kPlaylistCheckbox = sym::g_textPlaylistCheckbox;           // "STRCAT Items {\"%s%s\"}"
        constexpr ee::Ptr<const char> kCheckGlyph{sym::checkGlyph};                   // "~": the font's tick
        constexpr ee::Ptr<const char> kCrossGlyph{sym::crossGlyph};                   // "`": the font's cross
        // The list's labels' scale (1, 1, 1, 0), and their line spacing; the
        // track playing has the track widget's (trackLabel).
        inline ee::Global<zge::Vec4, sym::g_playlistLabelScale> g_playlistLabelScale;
        constexpr float kPlaylistLineSpacing = 1.0f;
        // The list takes at most 127 tracks.
        constexpr uint32_t kPlaylistTracks = 127;
    }

    // bmxmenuscriptPopulatePlaylist (list): the playlist's lines, a track's
    // label each, with its checkbox (on or off) in the column beside, then
    // the track playing under them; when that one is off, the next one on
    // is picked as bmxmenuscriptNextTrack does.
    void bmxmenuscriptPopulatePlaylist(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x400]> line;
        ee::Local<zge::Vec4> scale;
        ee::Local<zge::Vec4> currentScale;
        ee::Local<uint32_t[2]> currentArguments;

        memset(line, 0, sizeof(char[0x400]));
        const uint32_t tracks = soundEvent(kSoundTrackCount, 0, 0, 0, 0);
        const Node list(arguments[0]);
        const Node checkboxes = zge::zgui::findNode(list, kActiveCheckboxes, kFrameNode);
        const Node currentTrack = zge::zgui::findNode(list, kCurrentTrack, kFrameNode);
        zge::zgui::runScript(list, kClearItems, 0);
        zge::zgui::runScript(checkboxes, kClearItems, 0);
        const uint32_t listed = tracks < kPlaylistTracks + 1 ? tracks : kPlaylistTracks;
        for (uint32_t track = 0; track < listed; ++track)
        {
            *scale = *g_playlistLabelScale;
            const ee::Ptr<const char> separator = track == 0 ? kNoText : kLineBreak;
            const ee::Ptr<char> label = trackLabel(track, scale, 0, 0.0f, kPlaylistLineSpacing);
            zge::formatString(line, kPlaylistLine, separator, label);
            zge::zgui::runScript(list, line, 0);
            const bool on = soundEvent(kSoundTrackIsOn, track, 0, 0, 0) != 0;
            zge::formatString(line, kPlaylistCheckbox, separator, on ? kCheckGlyph : kCrossGlyph);
            zge::zgui::runScript(checkboxes, line, 0);
        }
        const uint32_t current = soundEvent(kSoundCurrentTrack, 0, 0, 0, 0);
        if (soundEvent(kSoundTrackIsOn, current, 0, 0, 0))
        {
            *currentScale = *g_trackLabelScale;
            const ee::Ptr<char> label = trackLabel(current, currentScale, 0, 0.0f, kTrackLineSpacing);
            zge::formatString(line, kSetTrackText, label);
            zge::zgui::runScript(currentTrack, line, 0);
        }
        else
        {
            currentArguments[0] = ee::Address(currentTrack);
            currentArguments[1] = 0;
            bmxmenuscriptNextTrack(ee::Ptr<uint32_t>(currentArguments.address()));
        }
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPopulatePlaylist, Draft);

    namespace
    {
        // A movie the media viewer lists (the table at mediaMovies): its
        // file, its title, and the saved game's movie record that opens it.
        struct MediaMovie
        {
            uint8_t _00[4];
            ee::Ptr<const char> file;  // +0x04 "ASSETS\\Movies\\<name>.pss"
            ee::Ptr<const char> title; // +0x08 a string table key
            uint8_t _0c[4];
            uint8_t savedMovie;        // +0x10 its SavedGame::movies row (read as a byte)
            uint8_t _11[7];
        };
        static_assert(sizeof(MediaMovie) == 0x18);
        constexpr uint32_t kMediaMovieCount = 19;
        constexpr uint32_t kMediaItem = sym::g_textMediaItem;                           // "STRCAT Items {\"%s%s%s\"}"
    }

    // mediaViewerMovies: the movie table and its size (AI's
    // mediaViewerMovies, with BMX XXX's table; the first word is not read).
    void mediaViewerMovies(uint32_t /*unused*/, ee::Ptr<uint32_t> movies, ee::Ptr<uint32_t> count)
    {
        *movies = sym::mediaMovies;
        *count = kMediaMovieCount;
    }
    READABLE_FUNCTION(mediaViewerMovies, Verified);

    // bmxmenuscriptPopulateMediaViewerMenu (list, list): a line per movie,
    // its title, a movie its saved record has not opened marked disabled
    // (FrameMovies's script passes its menu twice; the first goes to
    // mediaViewerMovies, which does not read it).
    void bmxmenuscriptPopulateMediaViewerMenu(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x100]> line;
        ee::Local<uint32_t> movies;
        ee::Local<uint32_t> count;

        memset(line, 0, sizeof(char[0x100]));
        const Node list(arguments[1]);
        *movies = 0;
        *count = 0;
        mediaViewerMovies(arguments[0], movies, count);
        zge::zgui::runScript(list, kClearItems, 0);
        for (uint32_t index = 0; index < *count; ++index)
        {
            const ee::Ptr<MediaMovie> movie(*movies);
            const bool open = (g_savedGame->movies[movie->savedMovie] & kMovieRewardBit0) != 0;
            zge::formatString(line, kMediaItem, index == 0 ? kNoText : kLineBreak, open ? kNoText : kDisabled, movie->title);
            zge::zgui::runScript(list, line, 0);
            *movies = *movies + sizeof(MediaMovie);
        }
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPopulateMediaViewerMenu, Verified);

    // bmxmenuscriptGetSelectedMediaFileName (out, list, list): the file of
    // the movie the list has selected as the out variable's value
    // (FramePlayMovie's movieFileName; the script passes the list twice).
    void bmxmenuscriptGetSelectedMediaFileName(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<uint32_t> movies;
        ee::Local<uint32_t> count;
        ee::Local<uint32_t> selected;

        const Node list(arguments[2]);
        const Node out(arguments[0]);
        *movies = 0;
        *count = 0;
        *selected = 0;
        mediaViewerMovies(arguments[1], movies, count);
        zge::zgui::copyVariable(list, kSelectedIndex, selected);
        const ee::Ptr<MediaMovie> movie = ee::Ptr<MediaMovie>(*movies) + *selected;
        zge::zgui::setVariableValue(out, ee::Address(movie->file));
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetSelectedMediaFileName, Verified);

    namespace
    {
        // The special modes (the features a reward opens, SavedGame::features
        // and featuresOn): their names' list and their settings' list.
        constexpr uint32_t kSpecialModes = 5;
        constexpr uint32_t kFeatureName = sym::g_textFeatureName;               // "$KSTR_FEATURENAME_%02d"
        constexpr uint32_t kFeatureNameUnknown = sym::g_textFeatureNameUnknown; // "$KSTR_FEATURENAME_UNKNOWN"
        constexpr ee::Ptr<const char> kModeOn{sym::g_textFeatureOn};            // "$KSTR_ON"
        constexpr ee::Ptr<const char> kModeOff{sym::g_textFeatureOff};          // "$KSTR_OFF"
        constexpr ee::Ptr<const char> kListItems{sym::g_textSpecialModeItems};  // "Items"
        constexpr ee::Ptr<const char> kListVisibleCount{sym::g_textSpecialModeVisibleCount}; // "VisibleCount"
    }

    // bmxmenuscriptPopulateSpecialModeMenu (names, settings; FrameSpecialModes's
    // MenuName and MenuValue): the five
    // special modes, a line each: in the first list its name (an unopened
    // one's the unknown name), in the second whether it is on; an unopened
    // mode's lines are marked disabled. Each list shows all five.
    void bmxmenuscriptPopulateSpecialModeMenu(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x200]> names;
        ee::Local<char[0x200]> settings;
        ee::Local<char[0x40]> name;
        ee::Local<uint32_t> mode;

        const Node settingsList(arguments[1]);
        const Node namesList(arguments[0]);
        names[0] = 0;
        settings[0] = 0;
        for (*mode = 0; *mode < kSpecialModes; *mode = *mode + 1)
        {
            const bool open = ((static_cast<int32_t>(g_savedGame->features) >> *mode) & 1) != 0;
            if (open)
                zge::formatString(name, kFeatureName, *mode);
            else
                zge::formatString(name, kFeatureNameUnknown);
            const ee::Ptr<const char> mark = open ? kNoText : kDisabled;
            zge::formatString(names.address() + static_cast<uint32_t>(zge::stringLength(names)), kListLine,
                              *mode != 0 ? kLineBreak : kNoText, mark, name);
            const bool on = ((static_cast<int32_t>(g_savedGame->featuresOn) >> *mode) & 1) != 0;
            zge::formatString(settings.address() + static_cast<uint32_t>(zge::stringLength(settings)), kListLine,
                              *mode != 0 ? kLineBreak : kNoText, mark, on ? kModeOn : kModeOff);
        }
        zge::zgui::setVariable(namesList, kListItems, names);
        zge::zgui::setVariable(namesList, kListVisibleCount, mode);
        zge::zgui::setVariable(settingsList, kListItems, settings);
        zge::zgui::setVariable(settingsList, kListVisibleCount, mode);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptPopulateSpecialModeMenu, Verified);

    namespace
    {
        // The team's list: a member's "FIRST LAST" in capitals, a place no
        // team reward has filled yet the locked line.
        constexpr uint32_t kTeamPlaces = 7;
        constexpr uint32_t kTeamLine = sym::g_textTeamLine;               // "%s%s"
        constexpr uint32_t kTeamLockedLine = sym::g_textTeamLockedLine;   // "%s"
        constexpr ee::Ptr<const char> kTeamLocked{sym::g_textTeamMemberLocked}; // "\\n!$KSTR_TEAM_MEM_NAME_LOCKED"
    }

    // bmxmenuscriptGetTeamNames (list): the seven team places' names as the
    // list's value; each place past the team rewards' count is locked (its
    // line starts with the script's line break, even as the first). No UI
    // script of UILIB.ZAL calls it.
    void bmxmenuscriptGetTeamNames(ee::Ptr<uint32_t> arguments)
    {
        ee::Local<char[0x80]> name;
        ee::Local<char[0x480]> names;

        const Node list(arguments[0]);
        names[0] = 0;
        name[0] = 0;
        for (uint32_t place = 0; place < kTeamPlaces; ++place)
        {
            if (place < g_savedGame->teamRewards)
            {
                const RiderCharacter &member = g_savedGame->characters[g_savedGame->teamMembers[place]];
                zge::formatString(name, kFullName, ee::Ptr<const char>(member.firstName), ee::Ptr<const char>(member.lastName));
                zge::stringToUpper(name);
                const int32_t end = zge::stringLength(names);
                zge::formatString(names.address() + static_cast<uint32_t>(end), kTeamLine, end != 0 ? kLineBreak : kNoText, name);
            }
            else
            {
                const int32_t end = zge::stringLength(names);
                zge::formatString(names.address() + static_cast<uint32_t>(end), kTeamLockedLine, kTeamLocked);
            }
        }
        zge::zgui::setVariableValue(list, names);
    }
    READABLE_FUNCTION_WAITS(bmxmenuscriptGetTeamNames, Draft);
}
