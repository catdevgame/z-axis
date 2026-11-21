// The level's overlay (hud.h): the widgets every skater shares, found once
// under InGameUI.Overlay (MultiplayerUI.Overlay in split screen): the
// message line ("Objective"), the crash message, the XP indicator, the
// split-screen points counter ("SecondaryScore") and the timer.
//

#include "aggressive/hud/hud.h"

#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/math/random.h"
#include "aggressive/timers.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/hud/overlay.cpp.h"

namespace aggressive::hud
{
    namespace
    {
        // zgui, called by address: setVariable (a game override hooks it)
        // and runScript where the original's result is the function's.
        constexpr uint32_t kSetVariable = sym::setVariable; // (widget, name, value*) -> set
        constexpr uint32_t kRunScript = sym::runScript;   // (widget, text, 0)
        // (variable, out*) the variable's value as an integer (its type's intValue).
        constexpr uint32_t kVariableInt = sym::variableIntValue;
        // (name, function) a host function scripts call by name (CALL).
        constexpr uint32_t kMemset = sym::memset;       // libc (dst, byte, bytes)
        // This file's and hud.cpp's functions, called by address as the original calls them.
        constexpr uint32_t kShowOverlayText = sym::showOverlayText;
        constexpr uint32_t kShowMessageIfFree = sym::showMessageIfFree;
        constexpr uint32_t kReleaseMessageSlot = sym::releaseMessageSlot;
        constexpr uint32_t kShowPoints = sym::showPoints;
        constexpr uint32_t kFormatScore = sym::formatScore;

        // The overlay's widgets (setupOverlay) and the message line's queue
        // of MessageSlots (hud.cpp, showMessage).
        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_messageLine> g_messageLine;    // "Objective"
        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_crashMessage> g_crashMessage;   // "CrashMessage"
        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_xpIndicator> g_xpIndicator;    // "xpIndicator"
        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_pointsCounter> g_pointsCounter;  // "SecondaryScore"
        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_timer> g_timer;          // "Timer"
        inline ee::Global<uint32_t, sym::g_messageQueue> g_messageQueue;
        constexpr uint32_t kMessageSlots = 4;
        // The shortest delay before a queued message tries again.
        inline ee::Global<ee::Float, sym::g_minMessageDelay> g_minMessageDelay;
        inline constexpr ee::Float kRandomScale = 2.3283064e-10f; // 2^-32

        // Widget paths, properties and commands (.data and .sdata).
        constexpr uint32_t kMultiplayerOverlay = sym::multiplayerOverlay;       // "MultiplayerUI.Overlay"
        constexpr uint32_t kMultiplayerOverlayToggle = sym::multiplayerOverlayToggle; // "MultiplayerUI.OverlayToggle"
        constexpr uint32_t kInGameOverlay = sym::inGameOverlay;            // "InGameUI.Overlay"
        constexpr uint32_t kInGameOverlayToggle = sym::inGameOverlayToggle;      // "InGameUI.OverlayToggle"
        constexpr uint32_t kObjectiveName = sym::objectiveName;            // "Objective"
        constexpr uint32_t kCrashMessageName = sym::crashMessageName;         // "CrashMessage"
        constexpr uint32_t kXpIndicatorName = sym::xpIndicatorName;          // "xpIndicator"
        constexpr uint32_t kSecondaryScoreName = sym::secondaryScoreName;       // "SecondaryScore"
        constexpr uint32_t kTimerName = sym::timerName;                // "Timer"
        constexpr ee::Ptr<const char> kNextXpLineName{sym::nextXpLineName};           // "inlineOverlayGetNextXPLine"
        constexpr ee::Ptr<const char> kFormatWithCommasName{sym::formatWithCommasName};     // "inlineOverlayFormatIntegerAsStringWithCommas"
        constexpr uint32_t kNextXpLineFunction = sym::nextXpLine;       // nextXpLine
        constexpr uint32_t kFormatWithCommasFunction = sym::formatIntegerWithCommas; // formatIntegerWithCommas
        constexpr uint32_t kGameModeName = sym::gameModeName;             // "InGameUI.GameMode"
        constexpr uint32_t kCareerText = sym::careerText;               // "Career"
        constexpr uint32_t kFreeskateText = sym::freeskateText;            // "Freeskate"
        constexpr uint32_t kTimedRunText = sym::timedRunText;             // "Timed Run"
        constexpr uint32_t kParkEditorText = sym::parkEditorText;           // "Park editor"
        constexpr uint32_t kData = sym::data;                     // "Data"
        constexpr ee::Ptr<const char> kWaitingText{sym::waitingText};              // "WaitingText"
        constexpr ee::Ptr<const char> kXpLabelText{sym::xpLabelText};              // "xpLabel.Text"
        constexpr uint32_t kNoText = sym::noText3;                   // ""
        constexpr ee::Ptr<const char> kVisible{sym::visibleName};                  // "Visible"
        constexpr ee::Ptr<const char> kText{sym::text2};                     // "Text"
        constexpr ee::Ptr<const char> kPoints{sym::points};                   // "Points"
        constexpr ee::Ptr<const char> kScore{sym::g_textScore};                    // "Score"
        constexpr ee::Ptr<const char> kCurValue{sym::curValue};                 // "CurValue"
        constexpr ee::Ptr<const char> kLabelText{sym::labelText};                // "TextLabel.Text"
        constexpr uint32_t kLabelWidth = sym::labelWidth;               // "TextLabel.Width"
        constexpr ee::Ptr<const char> kBackgroundWidth{sym::backgroundWidth};          // "Background.Width"
        constexpr ee::Ptr<const char> kFadeOutDelay{sym::fadeOutDelay};             // "FadeOut.StartDelay"
        constexpr uint32_t kPlayFadeIn = sym::playFadeIn;               // "PLAY FadeIn"
        constexpr uint32_t kPlayFadeOut = sym::playFadeOut;              // "PLAY FadeOut"
        constexpr uint32_t kStopSetScore = sym::stopSetScore;             // "STOP SetScore"
        constexpr uint32_t kPlaySetScore = sym::playSetScore;             // "PLAY SetScore"
        constexpr uint32_t kAbortSetScore = sym::abortSetScore;            // "ABORT SetScore"
        constexpr uint32_t kRefreshLabel = sym::refreshLabel;             // "REFRESH TextLabel"
        constexpr uint32_t kPlayAddXpReward = sym::playAddXpReward;          // "PLAY AddNewXPReward"
        constexpr uint32_t kPlayFreakOut = sym::playFreakOut;             // "PLAY FreakOut"
        constexpr uint32_t kStopFreakOut = sym::stopFreakOut;             // "STOP FreakOut"
        inline constexpr ee::Float kBackgroundMargin = 24.0f;
        // The XP indicator's waiting lines take at most this much text.
        constexpr uint32_t kXpTextBytes = 0x500, kXpTextRoom = 0x480, kXpLineBytes = 0x80;

        // The upper half of the game type's address (g_gameTypeByte), which the
        // original leaves as its result where it returns early.
        constexpr int32_t kGameTypeHigh = ee::high(sym::g_debugDisplay);
        // zgui's globals (the root at +0x0C), and the upper half of the root's
        // address.
        constexpr uint32_t kZguiGlobals = sym::g_zgui, kUiRootHigh = ee::high(sym::g_zgui);

        bool splitScreen() { return isSplitScreen(); }
        int32_t runScriptResult(ee::Address widget, uint32_t script) { return ee::game<int32_t>(kRunScript, widget, script, 0); }
        ee::Ptr<zgui::Node> findWidget(ee::Ptr<zgui::Node> parent, uint32_t path)
        {
            return zgui::findNode(parent, ee::Ptr<const char>(path), uint64_t{1});
        }
    }

    int32_t setupOverlay()
    {
        // The overlay (looked up, unused) and the toggle pane under it.
        ee::Ptr<zgui::Node> pane;
        if (splitScreen())
        {
            findWidget(*zgui::g_root, kMultiplayerOverlay);
            pane = findWidget(*zgui::g_root, kMultiplayerOverlayToggle);
        }
        else
        {
            findWidget(*zgui::g_root, kInGameOverlay);
            pane = findWidget(*zgui::g_root, kInGameOverlayToggle);
        }
        *g_messageLine = findWidget(pane, kObjectiveName);
        *g_crashMessage = findWidget(pane, kCrashMessageName);
        *g_xpIndicator = findWidget(pane, kXpIndicatorName);
        *g_pointsCounter = findWidget(pane, kSecondaryScoreName);
        *g_timer = findWidget(pane, kTimerName);
        zge::zgui::registerScriptFunction(kNextXpLineName, kNextXpLineFunction);
        zge::zgui::registerScriptFunction(kFormatWithCommasName, kFormatWithCommasFunction);

        // The game mode's name for the overlay (the original's jump table
        // by game type; split screen and the rest set none).
        const uint32_t type = static_cast<uint32_t>(*g_gameType);
        uint32_t modeText = 0;
        switch (type)
        {
        case 1:
            modeText = kCareerText;
            break;
        case 2:
            modeText = kFreeskateText;
            break;
        case 3:
            modeText = kTimedRunText;
            break;
        case 5:
            modeText = kParkEditorText;
            break;
        default:
            break;
        }
        if (modeText)
            zgui::setVariable(*g_uiRoot, kGameModeName, modeText);
        *g_messageQueue = aggressive::newPool(sizeof(MessageSlot), kMessageSlots, 0);
        return static_cast<int32_t>(*g_messageQueue);
    }
    READABLE_FUNCTION_WAITS(setupOverlay, Verified);

    int32_t showOverlayText(ee::Address text, uint32_t holdAndStop, ee::Float seconds)
    {
        ee::Local<ee::Float> fadeDelay;
        *fadeDelay = seconds;
        zgui::setVariable(*g_messageLine, kText, text);
        if (holdAndStop)
        {
            // The text stays; a level still playing stops for it.
            const ee::Ptr<Level> level = currentLevel();
            const int32_t mode = level->mode->id;
            if (mode != kModePlaying)
                return mode;
            setLevelStopReason(level, kStopOverlayText);
            return enterGameMode(kModePaused);
        }
        zgui::runScript(*g_messageLine, ee::Ptr<char>(kPlayFadeIn), 0);
        zgui::setVariable(*g_messageLine, kFadeOutDelay, fadeDelay);
        return runScriptResult(*g_messageLine, kPlayFadeOut);
    }
    READABLE_FUNCTION_WAITS(showOverlayText, Verified);

    int32_t showMessageIfFree(ee::Address text, ee::Float seconds)
    {
        ee::Local<int32_t> visible;
        zgui::copyVariable(*g_messageLine, ee::Ptr<const char>(kVisible), ee::Ptr<char>(visible.address()));
        if (*visible != 0)
            return 0;
        showOverlayText(text, 0, seconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(showMessageIfFree, Verified);

    void showQueuedMessage(uint32_t timer, uint32_t interrupted, uint32_t slot)
    {
        // (Interrupted, the result is what the caller had.)
        if (!interrupted)
        {
            const ee::Ptr<MessageSlot> waiting(slot);
            if (showMessageIfFree(slot + offsetof(MessageSlot, text), waiting->seconds))
            {
                releaseMessageSlot(slot);
                return;
            }
            // The line is busy: try again after a random delay.
            const ee::Float random = ee::toFloat(zge::randomNumber()) * kRandomScale;
            aggressive::setTimer(timer, ee::max(ee::min(random, ee::Float(1.0f)), *g_minMessageDelay));
        }
    }
    READABLE_FUNCTION_WAITS(showQueuedMessage, Verified);

    int32_t releaseMessageSlot(uint32_t slot)
    {
        uint32_t &timer = ee::Ptr<MessageSlot>(slot)->timer;
        if (timer)
        {
            aggressive::releaseTimer(timer);
            timer = 0;
        }
        return game::tail(aggressive::poolGive, *g_messageQueue, slot);
    }
    READABLE_FUNCTION(releaseMessageSlot, Verified);

    namespace
    {
        // The pool's walk, by address: identical bodies fold into one host
        // function on some linkers.
        uint32_t poolFirst(uint32_t pool) { return zge::poolFirst(ee::Ptr<zge::PoolList>(pool)); }
        uint32_t poolRecord(uint32_t link) { return zge::poolRecord(link); }
        uint32_t poolNext(uint32_t link) { return zge::poolNext(ee::Ptr<zge::PoolLink>(link)); }
    }

    // freeHudMessages: leaving a level: every queued message given back, the
    // queue freed, the HUD's widgets forgotten. (The saves override hooks it
    // to run after the level is written.)
    int32_t freeHudMessages()
    {
        for (uint32_t link = poolFirst(*g_messageQueue); link != 0;)
        {
            const uint32_t slot = poolRecord(link);
            link = poolNext(link);
            releaseMessageSlot(slot);
        }
        const int32_t result = freePool(*g_messageQueue);
        *g_messageQueue = 0;
        *g_messageLine = {};
        *g_crashMessage = {};
        *g_xpIndicator = {};
        *g_pointsCounter = {};
        *g_timer = {};
        return result;
    }
    READABLE_FUNCTION_WAITS(freeHudMessages, Verified);

    int32_t showCrashMessage(ee::Address text, ee::Float seconds)
    {
        ee::Local<int32_t> visible;
        ee::Local<ee::Float> fadeDelay;
        *fadeDelay = seconds;
        *visible = 0;
        if (splitScreen())
            return kGameTypeHigh;
        zgui::setVariable(*g_crashMessage, kText, text);
        zgui::copyVariable(*g_crashMessage, ee::Ptr<const char>(kVisible), ee::Ptr<char>(visible.address()));
        zgui::runScript(*g_crashMessage, ee::Ptr<char>(kPlayFadeIn), 0);
        zgui::setVariable(*g_crashMessage, kFadeOutDelay, fadeDelay);
        return runScriptResult(*g_crashMessage, kPlayFadeOut);
    }
    READABLE_FUNCTION_WAITS(showCrashMessage, Verified);

    namespace
    {
        void memset(ee::Ptr<uint8_t> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    int32_t showXpReward(ee::Ptr<skater::Player> /*player*/, ee::Address text, int32_t points)
    {
        ee::Local<uint8_t[0x500]> waiting;
        ee::Local<int32_t> value;
        *value = points;
        memset(waiting, 0, kXpTextBytes);
        const ee::Ptr<zgui::Node> data = findWidget(*g_xpIndicator, kData);
        if (splitScreen())
            return kGameTypeHigh;
        // Queued only while the waiting lines have room.
        zgui::copyVariable(data, ee::Ptr<const char>(kWaitingText), ee::Ptr<char>(waiting.address()));
        int32_t result = static_cast<uint32_t>(zge::stringLength(waiting)) < kXpTextRoom;
        if (result)
        {
            zgui::setVariable(*g_xpIndicator, kText, text);
            zgui::setVariable(*g_xpIndicator, kPoints, value);
            result = runScriptResult(*g_xpIndicator, kPlayAddXpReward);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(showXpReward, Verified);

    int32_t nextXpLine()
    {
        ee::Local<uint8_t[0x500]> waiting;
        ee::Local<uint8_t[0xE0]> line;
        memset(waiting, 0, kXpTextBytes);
        memset(line, 0, kXpLineBytes);
        const ee::Ptr<zgui::Node> data = findWidget(*g_xpIndicator, kData);
        zgui::copyVariable(data, ee::Ptr<const char>(kWaitingText), ee::Ptr<char>(waiting.address()));

        // The first line, to a newline or the end, into `line`.
        uint32_t length = 0;
        while (waiting[length] != '\n' && waiting[length] != 0)
        {
            line[length] = waiting[length];
            ++length;
        }
        if (length == 0)
            return '\n';

        // The line to the label, the rest (after the newline) kept waiting.
        line[length] = 0;
        zgui::setVariable(data, kXpLabelText, line);
        if (waiting[length] == 0)
            return zgui::setVariable(data, kWaitingText, kNoText);
        return zgui::setVariable(data, kWaitingText, waiting.address() + length + 1);
    }
    READABLE_FUNCTION_WAITS(nextXpLine, Verified);

    int32_t formatIntegerWithCommas(ee::Ptr<ee::Ptr<zgui::Node>> arguments)
    {
        ee::Local<uint8_t[0x80]> text;
        ee::Local<int32_t> value;
        memset(text, 0, 0x80);
        // The script's arguments: the integer variable and the text widget.
        const ee::Ptr<zgui::Node> variable = arguments[0];
        *value = -1;
        const ee::Ptr<zgui::Node> widget = arguments[1];
        zgui::variableIntValue(variable, value);
        formatScore(*value, text);
        return zgui::setVariable(widget, kText, text);
    }
    READABLE_FUNCTION_WAITS(formatIntegerWithCommas, Verified);

    // pointsCounterScore: the score the points counter ("SecondaryScore")
    // holds; 0 if it has none. No caller in AI.
    int32_t pointsCounterScore()
    {
        ee::Local<int32_t> score;
        *score = 0;
        zgui::copyVariable(*g_pointsCounter, ee::Ptr<const char>(kScore), score);
        return *score;
    }
    READABLE_FUNCTION_WAITS(pointsCounterScore, Verified);

    int32_t showPoints(ee::Ptr<skater::Player> /*player*/, int32_t points, int32_t animate)
    {
        ee::Local<char[0x40]> text;
        ee::Local<int32_t> value;
        ee::Local<ee::Float> width;
        *value = points;
        zgui::setVariable(*g_pointsCounter, kScore, value);
        if (animate)
        {
            zgui::runScript(*g_pointsCounter, ee::Ptr<char>(kStopSetScore), 0);
            return runScriptResult(*g_pointsCounter, kPlaySetScore);
        }
        // At once, the background sized to the text.
        *width = 0.0f;
        formatScore(*value, text.address());
        zgui::setVariable(*g_pointsCounter, kCurValue, value);
        zgui::runScript(*g_pointsCounter, ee::Ptr<char>(kAbortSetScore), 0);
        zgui::setVariable(*g_pointsCounter, kLabelText, text.address());
        zgui::runScript(*g_pointsCounter, ee::Ptr<char>(kRefreshLabel), 0);
        zgui::copyVariable(*g_pointsCounter, ee::Ptr<const char>(kLabelWidth), ee::Ptr<char>(width.address()));
        *width = *width + kBackgroundMargin;
        return zgui::setVariable(*g_pointsCounter, kBackgroundWidth, width);
    }
    READABLE_FUNCTION_WAITS(showPoints, Verified);

    int32_t showPointsPanel()
    {
        ee::Local<int32_t> value;
        *value = 1;
        return zgui::setVariable(*g_pointsCounter, kVisible, value);
    }
    READABLE_FUNCTION_WAITS(showPointsPanel, Verified);

    int32_t hidePointsPanel(ee::Ptr<skater::Player> player)
    {
        ee::Local<int32_t> value;
        *value = 0;
        showPoints(player, 0, 0);
        return zgui::setVariable(*g_pointsCounter, kVisible, value);
    }
    READABLE_FUNCTION_WAITS(hidePointsPanel, Verified);

    namespace
    {
        namespace tail
        {
        int32_t runScript(ee::Address widget, uint32_t script) { return ee::tailCall<int32_t>(kRunScript, widget, script, 0); }
        }
    }

    int32_t timerFreakOut(uint32_t /*data*/, uint32_t stop)
    {
        return tail::runScript(*g_timer, stop ? kStopFreakOut : kPlayFreakOut);
    }
    READABLE_FUNCTION_WAITS(timerFreakOut, Verified);

    namespace
    {
        constexpr uint32_t kTextFrontEndShowAll = sym::g_textFrontEndShowAll;
        inline ee::Global<int32_t, sym::g_uiRoot> g_uiRootWord; // g_uiRootWord ((anonymous)::g_uiRootWord, g_uiRootWord)
        constexpr uint32_t kTextFrontEndHideAll = sym::g_textFrontEndHideAll;
    }

    namespace
    {
        int copyVariable(ee::Ptr<zgui::Node> pointsCounter, ee::Ptr<const char> score, ee::Ptr<char> x0) { return aggressive::zgui::copyVariable(pointsCounter, score, x0); }
    }

    namespace
    {
        namespace tail
        {
        int32_t runScript(uint32_t uiRootWord, uint32_t textFrontEndShowAll, ee::Address value) { return ee::tailCall<int32_t>(kRunScript, uiRootWord, textFrontEndShowAll, value); }
        }
    }

    int32_t showAllFrontEnd()
    {
        return tail::runScript(*g_uiRootWord, kTextFrontEndShowAll, 0);
    }
    READABLE_FUNCTION_WAITS(showAllFrontEnd, Verified);

    int32_t hideAllFrontEnd()
    {
        return tail::runScript(*g_uiRootWord, kTextFrontEndHideAll, 0);
    }
    READABLE_FUNCTION_WAITS(hideAllFrontEnd, Verified);
}
