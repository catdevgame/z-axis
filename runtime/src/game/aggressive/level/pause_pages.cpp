// The rest of the pause menu's pages: the playlist, the death menu and its
// continue, the tutorial's checkboxes, the secret tricks, saving and leaving,
// and the pages that end a timed run or a split-screen round. Then the mode
// itself: going into it (which page opens comes from the reason play stopped)
// and out of it.

#include "aggressive/career.h"
#include "aggressive/conversions.h"
#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/hud/hud.h"
#include "aggressive/level/pause_parts.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/collision/collision.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "sce/libgcc.h"
#include "aggressive/game_save.h"
#include "aggressive/sound/events.h"
#include "aggressive/timers.h"
#include "aggressive/card_game.h"
#include "aggressive/front_end.h"
#include "aggressive/game_start.h"
#include "aggressive/play_modes.h"
#include "aggressive/session.h"
#include "aggressive/level/animal_rescue.h"
#include "aggressive/level/objectives.h"
#include "aggressive/level/props.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/pause_pages.cpp.h"

namespace aggressive
{
    namespace
    {
        using parts::currentLevel;
        using parts::levelPlayer;
        using pause::Arguments;
        using pause::copyInto;
        using pause::find;
        using pause::intValue;
        using pause::Objective;
        using pause::run;
        
        // soundEvent's events for the music's tracks.
        constexpr uint32_t kCurrentTrack = 3, kPlayTrack = 4, kTrackCount = 5, kTrackOn = 6, kTrackOff = 7, kTrackIsOn = 8, kTrackName = 9;

        constexpr uint32_t kPointsPerContinue = 6; // kTuningGeneral's "Points/Continue"

        inline ee::Global<int32_t, sym::g_lessonObjectUnhashed> g_lessonObjectUnhashed; // 1 until goToLesson has the name's hash
        inline ee::Global<int32_t, sym::g_lessonObjectHash> g_lessonObjectHash;

        // The save's table: per level, 14 best values and who set them. The
        // timed run keeps its two in the last rows.
        constexpr uint32_t kBestScoreRow = 12, kBestSplitRow = 13;

        constexpr uint32_t kSpecialTrickCount = 10;
        inline ee::Global<uint32_t[kSpecialTrickCount], sym::specialTrickBits> g_specialTrickBits;   // Player::unlockedTricks' bit of each
        inline ee::Global<uint32_t[kSpecialTrickCount], sym::specialTrickNames> g_specialTrickNames; // the names shown
        inline ee::Global<uint32_t[kSpecialTrickCount], sym::specialTrickPads> g_specialTrickPads;   // the button lines shown
        constexpr uint32_t kEmptyString = sym::emptyString2;         // ""
        constexpr uint32_t kLineBreak = sym::lineBreak3;           // "\\n"
        constexpr uint32_t kClearItems = sym::g_textSetItems;          // "SET Items {""}"
        constexpr uint32_t kCheckGlyph = sym::checkGlyph;          // "~"
        constexpr uint32_t kCrossGlyph = sym::crossGlyph;          // "`"
        constexpr uint32_t kSelectedIndex = sym::g_textSelectedIndex;       // "SelectedIndex"
        constexpr ee::Ptr<const char> kItems{sym::g_textItems};               // "Items"
        constexpr ee::Ptr<const char> kText{sym::g_textText};                // "Text"
        constexpr uint32_t kActiveCheckboxes = sym::g_textActiveCheckboxes;    // "ActiveCheckboxes"
        constexpr uint32_t kCurrentTrackWidget = sym::g_textCurrentTrack;  // "CurrentTrack"
        constexpr uint32_t kButtons = sym::g_textButtons;             // "Buttons"
        constexpr uint32_t kAppendBoth = sym::g_textStrcatItems7;          // STRCAT Items {"%s %s"}
        constexpr uint32_t kAppendPair = sym::g_textStrcatItems3;          // STRCAT Items {"%s%s"}
        constexpr uint32_t kSetText = sym::g_textSetText;             // SET Text {"%s"}
        constexpr uint32_t kNoEnabledTracks = sym::g_textSetTextNoEnabledNTracks;     // SET Text {"No Enabled\\n Tracks"}
        constexpr uint32_t kContinueLine = sym::g_textContinueForPtsNExit;        // "Continue for %s pts\\nExit"
        constexpr uint32_t kFirstCheckbox = sym::g_textSetItems3;       // SET Items {"!~"}
        constexpr uint32_t kCheckedBox = sym::g_textStrcatItemsN;          // STRCAT Items {"\\n~"}
        constexpr uint32_t kUncheckedBox = sym::g_textStrcatItemsN2;        // STRCAT Items {"\\n`"}
        constexpr ee::Ptr<char> kLastCheckboxes{sym::g_textStrcatItemsNNN};      // STRCAT Items {"\\n!~\\n!~\\n"}
        constexpr uint32_t kNoSpecialTricks = sym::g_textSetItemsNoSpecialTricksFound;     // Set Items {"!No Special Tricks found."}
        constexpr uint32_t kTimedScores = sym::g_textInGameUIPauseMenusFrameTimedGameOv;         // "InGameUI.PauseMenus.Frame.TimedGameOverStats.Scores"
        constexpr uint32_t kFourLines = sym::fourLines;           // "%s\n%s\n%s\n%s"
        constexpr uint32_t kMultiplayerStartMenu = sym::g_textMultiplayerUIPauseMenusFrame;  // "MultiplayerUI.PauseMenus.Frame.MultiplayerStart.Menu"
        constexpr uint32_t kMultiplayerWinner = sym::g_textMultiplayerUIPauseMenusFrame2;     // "MultiplayerUI.PauseMenus.Frame.MultiplayerOverStats.Winner"
        constexpr uint32_t kMultiplayerScores = sym::g_textMultiplayerUIPauseMenusFrame3;     // "MultiplayerUI.PauseMenus.Frame.MultiplayerOverStats.Scores"
        constexpr uint32_t kItsATie = sym::g_textItsATie;               // "It's A Tie!"
        constexpr uint32_t kPlayerOneWins = sym::playerOneWins;
        constexpr uint32_t kPlayerTwoWins = sym::playerTwoWins;
        constexpr uint32_t kFirstScoreGold = sym::g_textRgbaRgba;        // "#RGBA{185 151 16 255}%s\n#RGBA{255 255 255 240}%s"
        constexpr uint32_t kSecondScoreGold = sym::g_textRgbaRgba2;       // "#RGBA{255 255 255 240}%s\n#RGBA{185 151 16 255}%s"
        constexpr uint32_t kBothScoresGold = sym::g_textRgba;        // "#RGBA{185 151 16 255}%s\n%s"

        inline ee::Global<uint32_t, sym::g_uiRoot> g_uiRootNode;
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelNumber;

        // Why play stopped: the level object's reason, and the pages for it.
        constexpr uint64_t kReasonMultiplayerStart = 2, kReasonMultiplayerOver = 3, kReasonTimedStart = 4, kReasonTimedOver = 5, kReasonObjectives = 11,
                           kReasonContinue = 12, kReasonSelect = 13, kReasonDeath = 14;
        constexpr int32_t kLastCareerLevel = 5, kTutorialLevel = 7;
        constexpr uint32_t kDeathSound = 74;
        constexpr uint32_t kGameModePlaying = 2, kGameModePaused = 3;
        constexpr uint32_t kPadConnected = 0x20, kPadTakeSnapshot = 0x02; // PadState::options
        constexpr uint32_t kPadOffOnClose = 0x05;                         // from the snapshot, button 0x80 held off
        inline ee::Global<uint64_t, sym::g_enabledTracks> g_enabledTracks; // the playlist's bits; bit 33 is no track's
        constexpr uint64_t kLastLevelReached = uint64_t(1) << 33;

        constexpr uint32_t kUnlockInputZero = sym::g_textUnlockinput;      // "UNLOCKINPUT 0"
        constexpr uint32_t kUnlockInputOne = sym::g_textUnlockinput3;       // "UNLOCKINPUT 1"
        constexpr uint32_t kLockInputZero = sym::g_textLockinput2;        // "LOCKINPUT 0"
        constexpr uint32_t kLockInputOne = sym::g_textLockinput;         // "LOCKINPUT 1"
        constexpr uint32_t kMultiplayerPauseTitle = sym::g_textMultiplayerUIPauseMenusFrame4; // "MultiplayerUI.PauseMenus.Frame.Pause.Title"
        constexpr uint32_t kPausePlayerOne = sym::g_textPausePlayer;       // "Pause - Player 1"
        constexpr uint32_t kPausePlayerTwo = sym::g_textPausePlayer2;       // "Pause - Player 2"
        constexpr uint32_t kShowMultiplayerPause = sym::g_textSetMultiplayerUIPauseMenus; // SET MultiplayerUI.PauseMenus.Visible {1}
        constexpr uint32_t kShowPause = sym::g_textSetInGameUIPauseMenusVisible;            // SET InGameUI.PauseMenus.Visible {1}
        constexpr uint32_t kHideMultiplayerPause = sym::g_textSetMultiplayerUIPauseMenus2; // SET MultiplayerUI.PauseMenus.Visible {0}
        constexpr uint32_t kHidePause = sym::g_textSetInGameUIPauseMenusVisible2;            // SET InGameUI.PauseMenus.Visible {0}
        constexpr uint32_t kShowTimedStart = sym::g_textPlayInGameUIPauseMenusFrame;       // Play InGameUI.PauseMenus.Frame.TimedGameStart.Show
        constexpr uint32_t kShowTimedOver = sym::g_textPlayInGameUIPauseMenusFrame2;        // Play InGameUI.PauseMenus.Frame.TimedGameOverStats.Show
        constexpr uint32_t kShowMultiplayerStart = sym::g_textPlayMultiplayerUIPauseMenus; // Play MultiplayerUI.PauseMenus.Frame.MultiplayerStart.Show
        constexpr uint32_t kShowMultiplayerOver = sym::g_textPlayMultiplayerUIPauseMenus2;  // Play MultiplayerUI.PauseMenus.Frame.MultiplayerOverStats.Show
        constexpr uint32_t kMultiplayerXToContinue = sym::g_textPlayMultiplayerUIOverlayToggle; // PLAY MultiplayerUI.OverlayToggle.Objective.BringUpXToContinue
        constexpr uint32_t kXToContinue = sym::g_textPlayInGameUIOverlayToggleObjective;          // PLAY InGameUI.OverlayToggle.Objective.BringUpXToContinue
        constexpr uint32_t kFocusObjectives = sym::g_textSetfocusInGameUIPauseMenus;      // SETFOCUS InGameUI.PauseMenus.Frame.Objectives.Data.Objectives
        constexpr uint32_t kSkipToObjectives = sym::g_textPlayInGameUIPauseMenusFrame3;     // PLAY InGameUI.PauseMenus.Frame.Objectives.SkipDirectlyHere
        constexpr uint32_t kShowDeath = sym::g_textPlayInGameUIPauseMenusDeath;            // PLAY InGameUI.PauseMenus.Death.Show
        constexpr uint32_t kShowTutorial = sym::g_textPlayInGameUIPauseMenusFrame4;         // Play InGameUI.PauseMenus.Frame.Tutorial.Show
        constexpr uint32_t kShowMultiplayerPausePage = sym::g_textPlayMultiplayerUIPauseMenus3; // Play MultiplayerUI.PauseMenus.Frame.Pause.Show
        constexpr uint32_t kShowPausePage = sym::g_textPlayInGameUIPauseMenusFrame5;        // Play InGameUI.PauseMenus.Frame.Pause.Show
        constexpr uint32_t kCartonSize = sym::g_textCartonSize2;           // "CartonSize"
        constexpr uint32_t kStopJizzed = sym::g_textStopJizzed;           // "STOP Jizzed"

        inline ee::Global<int32_t, sym::g_pauseFunctionsPending> g_pauseFunctionsPending;

        // The zgui functions the pause scripts call.
        struct ScriptFunction
        {
            uint32_t name, function;
        };
        constexpr ScriptFunction kPauseFunctions[] = {
            {sym::g_textInlinePauseModeExit, sym::inlinePauseModeExit}, // inlinePauseModeExit
            {sym::g_textInlinePausePopulateAttributeMenu, sym::inlinePausePopulateAttributeMenu}, // inlinePausePopulateAttributeMenu
            {sym::g_textInlinePausePopulateGapMenu, sym::inlinePausePopulateGapMenu}, // inlinePausePopulateGapMenu
            {sym::g_textInlinePausePlayCancelSound, sym::inlinePausePlayCancelSound}, // inlinePausePlayCancelSound
            {sym::g_textInlinePauseSetMusicVolume, sym::inlinePauseSetMusicVolume}, // inlinePauseSetMusicVolume
            {sym::g_textInlinePauseSetSpeechVolume, sym::inlinePauseSetSpeechVolume}, // inlinePauseSetSpeechVolume
            {sym::g_textInlinePauseSetSFXVolume, sym::inlinePauseSetSFXVolume}, // inlinePauseSetSFXVolume
            {sym::g_textInlinePauseGetMusicVolume, sym::inlinePauseGetMusicVolume}, // inlinePauseGetMusicVolume
            {sym::g_textInlinePauseGetSpeechVolume, sym::inlinePauseGetSpeechVolume}, // inlinePauseGetSpeechVolume
            {sym::g_textInlinePauseGetSFXVolume, sym::inlinePauseGetSFXVolume}, // inlinePauseGetSFXVolume
            {sym::g_textInlinePausePopulatePlaylist, sym::inlinePausePopulatePlaylist}, // inlinePausePopulatePlaylist
            {sym::g_textInlinePauseEnableDisableTrack, sym::inlinePauseEnableDisableTrack}, // inlinePauseEnableDisableTrack
            {sym::g_textInlinePausePrevTrack, sym::inlinePausePrevTrack}, // inlinePausePrevTrack
            {sym::g_textInlinePauseNextTrack, sym::inlinePauseNextTrack}, // inlinePauseNextTrack
            {sym::g_textInlinePausePopulateTutorialCheckbo, sym::inlinePausePopulateTutorialCheckboxes}, // inlinePausePopulateTutorialCheckboxes
            {sym::g_textInlinePausePopulateSpecialTrickLis, sym::inlinePausePopulateSpecialTrickList}, // inlinePausePopulateSpecialTrickList
            {sym::g_textInlinePausePopulateDeathMenu, sym::inlinePausePopulateDeathMenu}, // inlinePausePopulateDeathMenu
            {sym::g_textInlinePauseUseContinue, sym::inlinePauseUseContinue}, // inlinePauseUseContinue
            {sym::g_textInlinePauseSave, sym::inlinePauseSave}, // inlinePauseSave
            {sym::g_textInlinePauseExit, sym::inlinePauseExit}, // inlinePauseExit
            {sym::g_textInlinePauseExitToLevelSelect, sym::inlinePauseExitToLevelSelect}, // inlinePauseExitToLevelSelect
            {sym::g_textInlinePauseRestartFromLoad, sym::inlinePauseRestartFromLoad}, // inlinePauseRestartFromLoad
            {sym::g_textInlinePausePopulateObjectivesMenu, sym::inlinePausePopulateObjectivesMenu}, // inlinePausePopulateObjectivesMenu
            {sym::g_textInlinePausePopulateChallengeDetail, sym::inlinePausePopulateChallengeDetail}, // inlinePausePopulateChallengeDetail
            {sym::g_textInlinePausePopulateSummaryScreen, sym::inlinePausePopulateSummaryScreen}, // inlinePausePopulateSummaryScreen
            {sym::g_textInlinePausePlayFlyThru, sym::inlinePausePlayFlyThru}, // inlinePausePlayFlyThru
            {sym::g_textInlinePauseTabulateLoot, sym::inlinePauseTabulateLoot}, // inlinePauseTabulateLoot
            {sym::g_textInlinePauseRestartTimedRun, sym::inlinePauseRestartTimedRun}, // inlinePauseRestartTimedRun
            {sym::g_textInlinePausePopulateTimedGameOverSt, sym::inlinePausePopulateTimedGameOverStats}, // inlinePausePopulateTimedGameOverStats
            {sym::g_textInlinePauseTimedRunEndRun, sym::inlinePauseTimedRunEndRun}, // inlinePauseTimedRunEndRun
            {sym::g_textInlinePauseMultiplayerStartEnd, sym::inlinePauseMultiplayerStartEnd}, // inlinePauseMultiplayerStartEnd
            {sym::g_textInlinePausePopulateMultiplayerOver, sym::inlinePausePopulateMultiplayerOverStats}, // inlinePausePopulateMultiplayerOverStats
            {sym::g_textInlinePauseMultiplayerEndRun, sym::inlinePauseMultiplayerEndRun}, // inlinePauseMultiplayerEndRun
            {sym::g_textInlinePauseJumpToTutorial, sym::goToLesson}, // inlinePauseJumpToTutorial
        };
    }

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(sym::memset, m, c, n); }
        namespace tail
        {
        int32_t runScript(ee::Ptr<zgui::Node> widget, uint32_t text, uint32_t argument) { return ee::tailCall<int32_t>(sym::runScript, widget, text, argument); }
        }

        // The continue's price: the tuning's Points/Continue, at most the
        // score, which is unsigned to the original.
        ee::Float continuePrice(int32_t score)
        {
            return ee::min(tuningValue(kTuningGeneral, kPointsPerContinue), unsignedToFloat(static_cast<uint32_t>(score)));
        }

        // The end of the track buttons: the track picked starts and its name
        // shows; with none on, the page says so.
        void showPickedTrack(ee::Ptr<char> line, Arguments arguments, int32_t track, int32_t enabled)
        {
            if (enabled)
            {
                const uint32_t name = soundEvent(kTrackName, track, 0, 0, 0);
                zge::formatString(line, kSetText, name);
                soundEvent(kPlayTrack, track, 0, 0, 0);
            }
            else
                zge::stringCopy(line, kNoEnabledTracks);
            run(arguments[0], line);
        }

        // The stats page's scores: by score in the first round kind, else by
        // the round's points.
        uint32_t playerResult(bool byScore, int32_t index)
        {
            const ee::Ptr<skater::Player> player = levelPlayer(index);
            return byScore ? skater::skaterScore(player) : skater::skaterPoints(player);
        }

        // inlinePauseMultiplayerEndRun's and the timed run's last step: the
        // run's timer goes.
        void releaseRunTimer(ee::Ptr<LevelObject> level)
        {
            if (level->runTimer)
            {
                hideVisibleTimer();
                releaseTimer(level->runTimer);
                level->runTimer = 0;
            }
        }

        // Leave for `destination` (the front end's page) after the delay.
        void leaveLevel(uint32_t destination)
        {
            setExitDestination(destination);
            setPendingStateAction(1);
            setPendingStateDelay(3);
            game::tail(aggressive::switchGameState, 2);
        }
    }

    // inlinePausePopulatePlaylist: the Playlist page: a line per track in two columns, its
    // name and whether it is on, and the track playing.
    void inlinePausePopulatePlaylist(Arguments arguments)
    {
        ee::Local<char[0x400]> line;
        memset(line, 0, 0x400);
        const uint32_t count = soundEvent(kTrackCount, 0, 0, 0, 0);
        const ee::Ptr<zgui::Node> list = arguments[0];
        const ee::Ptr<zgui::Node> checks = find(list, kActiveCheckboxes);
        const ee::Ptr<zgui::Node> currentTrackWidget = find(list, kCurrentTrackWidget);
        run(list, kClearItems);
        run(checks, kClearItems);
        for (uint32_t track = 0; track < count; ++track)
        {
            const uint32_t separator = track == 0 ? kEmptyString : kLineBreak;
            const uint32_t name = soundEvent(kTrackName, track, 0, 0, 0);
            zge::formatString(line, kAppendBoth, separator, name);
            run(list, line);
            const uint32_t on = soundEvent(kTrackIsOn, track, 0, 0, 0);
            zge::formatString(line, kAppendPair, separator, on ? kCheckGlyph : kCrossGlyph);
            run(checks, line);
        }
        const uint32_t current = soundEvent(kCurrentTrack, 0, 0, 0, 0);
        const uint32_t name = soundEvent(kTrackName, current, 0, 0, 0);
        zge::formatString(line, kSetText, name);
        run(currentTrackWidget, line);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulatePlaylist, Verified);

    // inlinePauseEnableDisableTrack: the checkbox of a track.
    void inlinePauseEnableDisableTrack(Arguments arguments)
    {
        ee::Local<int32_t> track;
        *track = 0;
        intValue(arguments[0], track);
        const int32_t on = soundEvent(kTrackIsOn, *track, 0, 0, 0);
        soundEvent(on ? kTrackOff : kTrackOn, *track, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(inlinePauseEnableDisableTrack, Verified);

    // inlinePauseNextTrack: the next track that is on, round the list, back to the
    // current one at the end.
    void inlinePauseNextTrack(Arguments arguments)
    {
        ee::Local<char[0x200]> line;
        memset(line, 0, 0x200);
        const int32_t start = soundEvent(kCurrentTrack, 0, 0, 0, 0);
        const int32_t count = soundEvent(kTrackCount, 0, 0, 0, 0);
        int32_t track = start;
        int32_t on = 0;
        if (count > 0)
        {
            do
            {
                track = track + 1 >= count ? 0 : track + 1;
                on = soundEvent(kTrackIsOn, track, 0, 0, 0);
            } while (!on && track != start);
        }
        showPickedTrack(line, arguments, track, on);
    }
    READABLE_FUNCTION_WAITS(inlinePauseNextTrack, Verified);

    // inlinePausePrevTrack: the same, backwards.
    void inlinePausePrevTrack(Arguments arguments)
    {
        ee::Local<char[0x200]> line;
        memset(line, 0, 0x200);
        const int32_t start = soundEvent(kCurrentTrack, 0, 0, 0, 0);
        const int32_t count = soundEvent(kTrackCount, 0, 0, 0, 0);
        int32_t track = start;
        int32_t on = 0;
        if (count > 0)
        {
            do
            {
                track = track - 1 < 0 ? count - 1 : track - 1;
                on = soundEvent(kTrackIsOn, track, 0, 0, 0);
            } while (!on && track != start);
        }
        showPickedTrack(line, arguments, track, on);
    }
    READABLE_FUNCTION_WAITS(inlinePausePrevTrack, Verified);

    // inlinePausePopulateDeathMenu: the death menu: what a continue costs.
    void inlinePausePopulateDeathMenu(Arguments arguments)
    {
        ee::Local<char[0x200]> line;
        ee::Local<char[0x80]> price;
        memset(line, 0, 0x200);
        memset(price, 0, 0x80);
        const ee::Ptr<zgui::Node> widget = arguments[0];
        const int32_t score = skater::skaterScore(levelPlayer(0));
        hud::formatScore(ee::toInt(continuePrice(score)), price);
        zge::formatString(line, kContinueLine, price);
        zgui::setVariable(widget, kItems, line);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateDeathMenu, Verified);

    // inlinePauseUseContinue: a continue is paid for out of the score.
    void inlinePauseUseContinue()
    {
        const ee::Ptr<skater::Player> player = levelPlayer(0);
        const int32_t score = skater::skaterScore(player);
        const int32_t paid = sce::libgcc::fptoui(continuePrice(score));
        skater::setSkaterScore(player, score - paid, 1);
        const ee::Ptr<LevelRecord> record(player->score);
        record->continues += 1;
    }
    READABLE_FUNCTION_WAITS(inlinePauseUseContinue, Verified);

    // inlinePausePopulateTutorialCheckboxes: the tutorial's checkboxes, one per objective.
    void inlinePausePopulateTutorialCheckboxes(Arguments arguments)
    {
        const ee::Ptr<Objective> objectives(aggressive::level::currentObjectives());
        const uint32_t count = aggressive::level::objectiveCount();
        const ee::Ptr<zgui::Node> widget = arguments[0];
        run(widget, kFirstCheckbox);
        for (uint32_t index = 0; index < count; ++index)
            run(widget, objectives[index].done ? kCheckedBox : kUncheckedBox);
        game::tail(zgui::runScript, widget, kLastCheckboxes, 0);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateTutorialCheckboxes, Verified);

    // inlinePausePopulateSpecialTrickList: the secret tricks the player has, their names and button
    // lines in two columns.
    void inlinePausePopulateSpecialTrickList(Arguments arguments)
    {
        ee::Local<char[0x80]> line;
        ee::Local<int32_t> playerIndex;
        int32_t found = 0;
        memset(line, 0, 0x80);
        const ee::Ptr<zgui::Node> list = arguments[0];
        *playerIndex = -1;
        const ee::Ptr<zgui::Node> pads = find(list, kButtons);
        intValue(arguments[1], playerIndex);
        const ee::Ptr<skater::Player> player = levelPlayer(*playerIndex);
        run(list, kClearItems);
        run(pads, kClearItems);
        for (uint32_t i = 0; i < kSpecialTrickCount; ++i)
        {
            if ((player->unlockedTricks >> ((*g_specialTrickBits)[i] & 0x1Fu)) & 1u)
            {
                const uint32_t separator = found ? kLineBreak : kEmptyString;
                zge::formatString(line, kAppendPair, separator, (*g_specialTrickNames)[i]);
                run(list, line);
                found = 1;
                zge::formatString(line, kAppendPair, separator, (*g_specialTrickPads)[i]);
                run(pads, line);
            }
        }
        if (!found)
            run(list, kNoSpecialTricks);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateSpecialTrickList, Verified);

    // inlinePauseSave: Save: in the career the player's progress goes into
    // the save first.
    void inlinePauseSave()
    {
        if (*g_gameType == GameType::Career)
        {
            const ee::Ptr<skater::Player> player = levelPlayer(0);
            skater::skaterProfile(player)->saveCount += 1;
            saveCareerProgress(player);
        }
        game::tail(aggressive::startGameSave);
    }
    READABLE_FUNCTION_WAITS(inlinePauseSave, Verified);

    // inlinePauseExit.
    void inlinePauseExit()
    {
        leaveLevel(1);
    }
    READABLE_FUNCTION_WAITS(inlinePauseExit, Verified);

    // inlinePauseExitToLevelSelect.
    void inlinePauseExitToLevelSelect()
    {
        leaveLevel(2);
    }
    READABLE_FUNCTION_WAITS(inlinePauseExitToLevelSelect, Verified);

    // inlinePauseRestartFromLoad: no shipped page calls it (script-dead).
    void inlinePauseRestartFromLoad()
    {
        game::tail(aggressive::startGameLoad);
    }
    READABLE_FUNCTION_WAITS(inlinePauseRestartFromLoad, Verified);

    // goToLesson: the Tutorial's list asks for a lesson: the message
    // "GoToLessonA" .. "Z", "GoToLessonAB" past 26, sent to the player's
    // script object (Estate's override hooks it for its own lessons). The
    // first time, that object's name is hashed. When nothing answers, a
    // message formatted for nothing.
    void goToLesson(Arguments arguments)
    {
        ee::Local<char[0x80]> name;
        ee::Local<char[0x10]> suffix;
        ee::Local<char[0x200]> notReceived;
        ee::Local<int32_t> number;
        constexpr uint32_t kPlayerOneName = sym::g_textScriptObjPlayerOne2;  // "ScriptObj_PlayerOne"
        constexpr uint32_t kGoToLesson = sym::g_textGoToLesson;     // "GoToLesson"
        constexpr uint32_t kNotReceived = sym::g_textTheMessageWasNotRecieved;    // "The message ('%s') was not recieved.\n"
        constexpr int32_t kAlphabet = 26;
        memset(name, 0, 0x80);
        memset(suffix, 0, 4);
        *number = -1;
        if (*g_lessonObjectUnhashed != 0)
        {
            *g_lessonObjectHash = zge::nameHash(ee::Ptr<const char>(kPlayerOneName));
            *g_lessonObjectUnhashed = 0;
        }
        const ee::Ptr<ee::Ptr<zge::SceneObject>> object = zge::findObjects(&*g_lessonObjectHash, 1);
        intValue(arguments[0], number);
        zge::stringCopy(name, kGoToLesson);
        const int32_t lesson = --*number;
        if (lesson >= kAlphabet)
        {
            suffix[2] = 0;
            suffix[1] = static_cast<char>(lesson / kAlphabet + 'A');
        }
        else
            suffix[1] = 0;
        suffix[0] = static_cast<char>(lesson % kAlphabet + 'A');
        zge::appendString(name, suffix);
        const int32_t type = static_cast<int32_t>(zge::nameHash(name));
        if (zge::sendNewMessage(0, *object, type, 0, 1) == 0)
        {
            memset(notReceived, 0, 0x200);
            zge::formatString(notReceived, kNotReceived, name);
        }
    }
    READABLE_FUNCTION_WAITS(goToLesson, Verified);

    // inlinePauseRestartTimedRun.
    void inlinePauseRestartTimedRun()
    {
        game::tail(aggressive::playAgain);
    }
    READABLE_FUNCTION_WAITS(inlinePauseRestartTimedRun, Verified);

    // inlinePausePopulateTimedGameOverStats: a timed run's end: the score, the split, and the level's
    // two best runs to beat.
    void inlinePausePopulateTimedGameOverStats()
    {
        ee::Local<char[0xD0]> line;
        ee::Local<char[0x20]> score;
        ee::Local<char[0x20]> split;
        ee::Local<char[0x20]> bestScore;
        ee::Local<char[0x20]> bestSplit;
        const ee::Ptr<zgui::Node> node = find(*zgui::g_root, kTimedScores);
        hud::formatScore(skater::skaterScore(levelPlayer(0)), score);
        hud::formatScore(skater::splitScore(levelPlayer(0)), split);
        hud::formatScore((*g_bestRuns)[*g_levelNumber][kBestScoreRow].value, bestScore);
        hud::formatScore((*g_bestRuns)[*g_levelNumber][kBestSplitRow].value, bestSplit);
        zge::formatString(line, kFourLines, score, split, bestScore, bestSplit);
        zgui::setVariable(node, kText, line);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateTimedGameOverStats, Verified);

    // inlinePauseTimedRunEndRun.
    void inlinePauseTimedRunEndRun()
    {
        const ee::Ptr<LevelObject> level = currentLevel();
        endTimedRun();
        releaseRunTimer(level);
    }
    READABLE_FUNCTION_WAITS(inlinePauseTimedRunEndRun, Verified);

    // inlinePauseMultiplayerEndRun: a split-screen round ended from the menu: Twenty One's
    // cards go or Animal Rescue's animals are dropped.
    void inlinePauseMultiplayerEndRun()
    {
        const ee::Ptr<LevelObject> level = currentLevel();
        endTimedRun();
        if (level->roundKind == kCardGameRound)
            endCardGame();
        else if (level->roundKind == kAnimalRescueRound)
        {
            dropAnimals(levelPlayer(0), 1);
            dropAnimals(levelPlayer(1), 1);
            level::setAnimalRescue(0);
        }
        releaseRunTimer(level);
    }
    READABLE_FUNCTION_WAITS(inlinePauseMultiplayerEndRun, Verified);

    // inlinePauseMultiplayerStartEnd: the split-screen start page: the round picked, set up.
    void inlinePauseMultiplayerStartEnd()
    {
        ee::Local<char> picked;
        const ee::Ptr<zgui::Node> node = find(*zgui::g_root, kMultiplayerStartMenu);
        zgui::copyVariable(node, ee::Ptr<const char>(kSelectedIndex), picked);
        const ee::Ptr<LevelObject> level = currentLevel();
        level->roundKind = static_cast<uint8_t>(*picked);
        const uint8_t kind = level->roundKind;
        const ee::Ptr<skater::Player> first = levelPlayer(0), second = levelPlayer(1);
        if (kind == kCardGameRound)
        {
            // Twenty One: each player's hand on that player's card widgets.
            first->cardHand.widgets = 0;
            second->cardHand.widgets = 1;
            resetCards(&first->cardHand);
            resetCards(&second->cardHand);
            const uint32_t now = zge::collisionClock();
            startCardGame(now);
        }
        if (kind != 0)
        {
            aggressive::skater::freezeMultiplayerScore(first);
            aggressive::skater::freezeMultiplayerScore(second);
        }
        if (kind == kAnimalRescueRound)
            level::setAnimalRescue(1);
        playAgain();
    }
    READABLE_FUNCTION_WAITS(inlinePauseMultiplayerStartEnd, Verified);

    // inlinePausePopulateMultiplayerOverStats: the split-screen stats page: who won, and both results,
    // the winner's in gold. Each comparison asks both players again; the
    // skater's calls are the original's.
    void inlinePausePopulateMultiplayerOverStats()
    {
        ee::Local<char[0xD0]> line;
        ee::Local<char[0x20]> firstText;
        ee::Local<char[0x20]> secondText;

        const ee::Ptr<zgui::Node> winner = find(*zgui::g_root, kMultiplayerWinner);
        const bool byScore = currentLevel()->roundKind == 0;
        const uint32_t first = playerResult(byScore, 0);
        const uint32_t second = playerResult(byScore, 1);
        if (second < first)
            zge::formatString(line, kPlayerOneWins);
        else
        {
            const uint32_t firstAgain = playerResult(byScore, 0);
            const uint32_t secondAgain = playerResult(byScore, 1);
            if (firstAgain < secondAgain)
                zge::formatString(line, kPlayerTwoWins);
            else
                zge::formatString(line, kItsATie);
        }
        zgui::setVariable(winner, kText, line);

        const ee::Ptr<zgui::Node> scores = find(*zgui::g_root, kMultiplayerScores);
        const bool scoresByScore = currentLevel()->roundKind == 0;
        hud::formatScore(playerResult(scoresByScore, 0), firstText);
        hud::formatScore(playerResult(scoresByScore, 1), secondText);
        const uint32_t firstResult = playerResult(scoresByScore, 0);
        const uint32_t secondResult = playerResult(scoresByScore, 1);
        uint32_t format = kFirstScoreGold;
        if (secondResult >= firstResult)
        {
            const uint32_t firstAgain = playerResult(scoresByScore, 0);
            const uint32_t secondAgain = playerResult(scoresByScore, 1);
            format = firstAgain < secondAgain ? kSecondScoreGold : kBothScoresGold;
        }
        zge::formatString(line, format, firstText, secondText);
        zgui::setVariable(scores, kText, line);
        aggressive::skater::showFinalScore(levelPlayer(0), 0);
        aggressive::skater::showFinalScore(levelPlayer(1), 0);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateMultiplayerOverStats, Verified);

    // enterPauseMode: into the pause mode: the page functions registered the
    // first time; the pads held; the menu made visible; the page that opens
    // is the reason play stopped.
    uint32_t enterPauseMode()
    {
        ee::Local<char[0x90]> titleText;
        const int32_t pending = *g_pauseFunctionsPending;
        for (uint32_t port = 0; port < 2; ++port)
        {
            skater::PadState &pad = (*skater::g_pads)[port];
            if (pad.options & kPadConnected)
                pad.options |= kPadTakeSnapshot;
            else
                pad.heldAtSnapshot = pad.pressed;
        }
        if (pending)
        {
            for (const ScriptFunction &function : kPauseFunctions)
                zge::zgui::registerScriptFunction(function.name, function.function);
            *g_pauseFunctionsPending = 0;
        }
        const ee::Ptr<LevelObject> level = currentLevel();
        const bool secondPlayer = (level->flags & kLevelStoppedBy) != 0;
        if (secondPlayer && *g_gameType == GameType::SplitScreen)
            run(*g_uiRootNode, kUnlockInputOne);
        else
            run(*g_uiRootNode, kUnlockInputZero);
        if (*g_gameType == GameType::SplitScreen)
        {
            const ee::Ptr<zgui::Node> title = find(*zgui::g_root, kMultiplayerPauseTitle);
            if ((level->flags & kLevelStoppedBy) == 0)
                zge::stringCopy(titleText, kPausePlayerOne);
            else
                zge::stringCopy(titleText, kPausePlayerTwo);
            zgui::setVariable(title, kText, titleText);
            run(*g_uiRootNode, kShowMultiplayerPause);
        }
        else
            run(*g_uiRootNode, kShowPause);

        // The page: a script for the reason; the input locks first for the
        // two pages split screen shows.
        const uint64_t reason = (level->flags & kLevelStopReason) >> kLevelStopReasonShift;
        uint32_t script = 0;
        const auto lockedPage = [&](uint32_t page) {
            run(*g_uiRootNode, kUnlockInputZero);
            run(*g_uiRootNode, kLockInputOne);
            script = page;
        };
        switch (reason)
        {
        case kReasonMultiplayerStart:
            lockedPage(kShowMultiplayerStart);
            break;
        case kReasonMultiplayerOver:
            lockedPage(kShowMultiplayerOver);
            break;
        case kReasonTimedStart:
            script = kShowTimedStart;
            break;
        case kReasonTimedOver:
            script = kShowTimedOver;
            break;
        case kReasonObjectives:
            run(*g_uiRootNode, kShowPause);
            script = kFocusObjectives;
            break;
        case kReasonContinue:
            if (*g_gameType == GameType::SplitScreen)
                script = kMultiplayerXToContinue;
            else
            {
                if (*g_levelNumber == kLastCareerLevel)
                    *g_enabledTracks |= kLastLevelReached;
                script = kXToContinue;
            }
            break;
        case kReasonSelect:
            script = kSkipToObjectives;
            break;
        case kReasonDeath:
            soundEvent(kDeathSound, 0, 0, 0, 0);
            script = kShowDeath;
            break;
        default:
            // The others in the table, and anything past it, take the pause page.
            if (*g_levelNumber == kTutorialLevel)
                script = kShowTutorial;
            else if (*g_gameType == GameType::SplitScreen)
                script = kShowMultiplayerPausePage;
            break;
        }
        run(*g_uiRootNode, script ? script : kShowPausePage);
        setGameMode(kGameModePaused);
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterPauseMode, Verified);

    void stopJizzedScript(ee::Address carton);

    // closePauseMenus: out of the pause menus, back to play. From the death
    // menu the juice carton is refilled to half.
    int32_t closePauseMenus()
    {
        run(*g_uiRootNode, kLockInputZero);
        run(*g_uiRootNode, kLockInputOne);
        for (uint32_t port = 0; port < 2; ++port)
            (*skater::g_pads)[port].options |= kPadOffOnClose;
        if ((currentLevel()->flags & kLevelStopReason) == kReasonDeath << kLevelStopReasonShift)
        {
            const ee::Ptr<skater::Player> skater = levelPlayer(0);
            ee::Local<int32_t> size;
            *size = -1;
            ee::Float cartonSize(50.0f);
            if (copyInto(skater->juiceMeter, kCartonSize, size))
                cartonSize = ee::toFloat(*size);
            skater::setJuiceMeter(skater->juiceMeter, cartonSize * ee::Float(0.5f));
            stopJizzedScript(skater->juiceMeter);
            skater::setJizzed(skater, 0);
        }
        if (*g_gameType == GameType::SplitScreen)
            run(*g_uiRootNode, kHideMultiplayerPause);
        else
            run(*g_uiRootNode, kHidePause);
        return setGameMode(kGameModePlaying);
    }
    READABLE_FUNCTION_WAITS(closePauseMenus, Verified);

    // stopJizzedScript.
    void stopJizzedScript(ee::Address carton)
    {
        game::tail(zgui::runScript, ee::Ptr<zgui::Node>(carton), ee::Ptr<char>(kStopJizzed), 0);
    }
    READABLE_FUNCTION_WAITS(stopJizzedScript, Verified);
}
