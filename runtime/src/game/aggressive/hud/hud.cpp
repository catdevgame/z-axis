// A skater's HUD widgets (hud.h): the score counter, the trick list and
// the two balance meters. The widgets are the HUD script's (assets\ui);
// their properties and animations go through zgui's setVariable /
// copyVariable / runScript calls, by name.
//

#include "hud.h"

#include "aggressive/globals.h"
#include "aggressive/objects.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/timers.h"
#include "zge/memory/regions.h"
#include "zge/math/random.h"

#include <cstring>
#include "sym/aggressive/hud/hud.cpp.h"

namespace aggressive::hud
{
    namespace
    {
        // zgui (engine side of the UI, not ported).
        constexpr uint32_t kGetProperty = sym::copyVariable; // (widget, name, value*) -> found
        constexpr uint32_t kCommand = sym::runScript;     // (widget, command, 0) e.g. "PLAY SetScore"
        constexpr uint32_t kFormatScore = sym::formatScore; // (score, text*) the score with its digit groups (inferred)

        // The message line's queue and its slots (0x80 bytes).
        inline ee::Global<uint32_t, sym::g_messageQueue> g_messageQueue;
        constexpr uint32_t kMemset = sym::memoryFill;          // (dst, byte, bytes)
        // Timers: (callback, data, tick callback, tick data, seconds,
        // interval) -> handle (script/challenge.cpp).
        constexpr uint32_t kShowQueuedMessage = sym::showQueuedMessage; // the timer's callback: the slot shown
        inline constexpr ee::Float kMinSeconds = 0.5f;
        inline constexpr ee::Float kRandomScale = 2.3283064e-10f; // 2^-32
        // The shortest delay before the message shows.
        inline ee::Global<ee::Float, sym::g_minMessageDelay2> g_minMessageDelay;

        // Property and command names.
        constexpr uint32_t kText = sym::text2;            // "Text"
        constexpr ee::Ptr<const char> kScore{sym::g_textScore};           // "Score"
        constexpr uint32_t kStopSetScore = sym::stopSetScore;    // "STOP SetScore"
        constexpr uint32_t kPlaySetScore = sym::playSetScore;    // "PLAY SetScore"
        constexpr ee::Ptr<const char> kCurValue{sym::curValue};        // "CurValue"
        constexpr uint32_t kAbortSetScore = sym::abortSetScore;   // "ABORT SetScore"
        constexpr ee::Ptr<const char> kLabelText{sym::labelText};       // "TextLabel.Text"
        constexpr uint32_t kRefreshLabel = sym::refreshLabel;    // "REFRESH TextLabel"
        constexpr ee::Ptr<const char> kLabelWidth{sym::labelWidth};      // "TextLabel.Width"
        constexpr ee::Ptr<const char> kBackgroundWidth{sym::backgroundWidth}; // "Background.Width"
        inline constexpr ee::Float kBackgroundMargin = 24.0f;
    }

    void showMessage(ee::Address text, ee::Float seconds)
    {
        const ee::Float shown = ee::max(seconds, kMinSeconds);
        const ee::Ptr<MessageSlot> slot(aggressive::poolTake(*g_messageQueue));
        zge::memoryFill(slot, 0, sizeof(MessageSlot));
        zge::stringCopyN(slot->text, text, 0x77);
        slot->seconds = shown;
        slot->text[0x77] = 0;
        // A random delay in [minimum, 1] seconds; no repeat (-1).
        const ee::Float random = ee::toFloat(zge::randomNumber()) * kRandomScale;
        const ee::Float delay = ee::max(ee::min(random, 1.0f), *g_minMessageDelay);
        slot->timer = aggressive::createTimer(kShowQueuedMessage, slot, 0, 0, delay, ee::Float(-1.0f));
    }
    READABLE_FUNCTION(showMessage, Verified);

    void setTrickText(ee::Ptr<SkaterHud> hud, ee::Address text)
    {
        game::tail(zge::zgui::setVariable, hud->potentialScore, kText, text);
    }
    READABLE_FUNCTION(setTrickText, Verified);

    void showScore(ee::Ptr<skater::Player> player, int32_t score, int32_t animate)
    {
        ee::Local<uint8_t[0x40]> out;
        ee::Local<int32_t> value;
        ee::Local<ee::Float> width;
        const ee::Ptr<zgui::Node> counter = player->hud->score;
        *value = score;
        zgui::setVariable(counter, kScore, value);
        if (animate)
        {
            zgui::runScript(counter, ee::Ptr<char>(kStopSetScore), 0);
            zgui::runScript(counter, ee::Ptr<char>(kPlaySetScore), 0);
            return;
        }
        *width = 0.0f;
        formatScore(*value, out);
        zgui::setVariable(counter, kCurValue, value);
        zgui::runScript(counter, ee::Ptr<char>(kAbortSetScore), 0);
        zgui::setVariable(counter, kLabelText, out);
        zgui::runScript(counter, ee::Ptr<char>(kRefreshLabel), 0);
        zgui::copyVariable(counter, kLabelWidth, width);
        *width = *width + kBackgroundMargin;
        zgui::setVariable(counter, kBackgroundWidth, width);
    }
    READABLE_FUNCTION(showScore, Verified);

    // playerPanelScore: the score the player's HUD counter holds (its
    // "Score"); 0 if it has none. No caller in AI.
    int32_t playerPanelScore(ee::Ptr<skater::Player> player)
    {
        ee::Local<int32_t> score;
        *score = 0;
        zgui::copyVariable(ee::Ptr<SkaterHud>(player->hud)->score, ee::Ptr<const char>(kScore), score);
        return *score;
    }
    READABLE_FUNCTION_WAITS(playerPanelScore, Draft);

}

namespace aggressive::hud
{
    namespace
    {
        constexpr uint32_t kRunScript = sym::runScript;   // (widget, text, 0), by address where its result is ours
        int32_t runScriptResult(ee::Address widget, uint32_t script) { return ee::game<int32_t>(kRunScript, widget, script, 0); }
        constexpr uint32_t kMemsetLibc = sym::memset;  // libc (dst, byte, bytes)
        // This file's functions, called by address as the original calls them.
        constexpr uint32_t kSetCornerColors = sym::setCornerColors;
        constexpr uint32_t kObjectName = sym::objectName;
        constexpr uint32_t kClearTrickList = sym::clearTrickList;
        // (player) -> the half turns of the jump in progress (score.cpp's
        // counters: Player::spinsCounterClockwise, spinsClockwise).

        // (The object message asking for a name, g_msgGetName, and its
        // NameRequest {out, size}: aggressive/objects.h. The result says
        // whether it answered.)

        // Widget paths, properties and commands (.data and .sdata).
        constexpr uint32_t kPlayerOnePane = sym::playerOnePane;          // "MultiplayerUI.Overlay.PlayerOne"
        constexpr uint32_t kPlayerOneTogglePane = sym::playerOneTogglePane;    // "MultiplayerUI.OverlayToggle.PlayerOne"
        constexpr uint32_t kPlayerTwoPane = sym::playerTwoPane;          // "MultiplayerUI.Overlay.PlayerTwo"
        constexpr uint32_t kPlayerTwoTogglePane = sym::playerTwoTogglePane;    // "MultiplayerUI.OverlayToggle.PlayerTwo"
        constexpr uint32_t kInGameOverlay = sym::inGameOverlay;          // "InGameUI.Overlay"
        constexpr uint32_t kInGameOverlayToggle = sym::inGameOverlayToggle;    // "InGameUI.OverlayToggle"
        constexpr uint32_t kScoreName = sym::g_textScore;              // "Score"
        constexpr uint32_t kHorzBalanceMeter = sym::horzBalanceMeter;       // "HorzBalanceMeter"
        constexpr uint32_t kVertBalanceMeter = sym::vertBalanceMeter;       // "VertBalanceMeter"
        constexpr uint32_t kTrickListName = sym::trickListName;          // "TrickList"
        constexpr uint32_t kPotentialScoreName = sym::potentialScoreName;     // "PotentialScore"
        constexpr uint32_t kCompletedTrickBuffer = sym::completedTrickBuffer;   // "CompletedTrickBuffer"
        constexpr uint32_t kTopLeftColor = sym::topLeftColor;           // "TopLeftColor"
        constexpr uint32_t kTopRightColor = sym::topRightColor;          // "TopRightColor"
        constexpr uint32_t kBottomLeftColor = sym::bottomLeftColor;        // "BottomLeftColor"
        constexpr uint32_t kBottomRightColor = sym::bottomRightColor;       // "BottomRightColor"
        constexpr ee::Ptr<const char> kVisibleName{sym::visibleName};            // "Visible"
        constexpr ee::Ptr<const char> kTextName{sym::text2};               // "Text"
        constexpr uint32_t kNoText = sym::noText3;                 // ""
        constexpr ee::Ptr<const char> kSafeDirection{sym::safeDirection};          // "SafeDirection"
        constexpr uint32_t kFirstSegment = sym::firstSegment;           // "First"
        constexpr uint32_t kNextSegment = sym::nextSegment;            // "Next"
        constexpr uint32_t kPlaySetBalance = sym::playSetBalance;         // "PLAY SetBalance"
        constexpr ee::Ptr<const char> kIsActivated{sym::isActivated};            // "isActivated"
        constexpr uint32_t kSpinClockwise = sym::spinClockwise;          // "SET Text {\"%d CW  \"}"
        constexpr uint32_t kSpinCounterClockwise = sym::spinCounterClockwise;   // "SET Text {\"%d CCW  \"}"
        constexpr uint32_t kSpinBoth = sym::spinBoth;               // "SET Text {\"%d CCW  %d CW  \"}"
        constexpr uint32_t kSpinNone = sym::spinNone;               // "SET Text {\"\"}"
        constexpr uint32_t kAppendParentText = sym::appendParentText;       // "STRCAT .Text Text"
        constexpr uint32_t kAppendFirstTrick = sym::appendFirstTrick;       // "STRCAT Text {\"%s\"}"
        constexpr uint32_t kAppendNextTrick = sym::appendNextTrick;        // "STRCAT Text {\" + %s\"}"
        constexpr uint32_t kPlayFadeIn = sym::playFadeIn;             // "PLAY FadeIn"
        constexpr uint32_t kPlayFadeOut = sym::playFadeOut;            // "PLAY FadeOut"
        constexpr uint32_t kStopFadeOut = sym::stopFadeOut;            // "STOP FadeOut"
        constexpr uint32_t kPlayRefresh = sym::playRefresh;            // "PLAY Refresh"
        constexpr uint32_t kPlayParentRefresh = sym::playParentRefresh;      // "PLAY .Refresh"
        constexpr uint32_t kZeroText = sym::zeroText;               // "0"
        constexpr int32_t kDegreesPerSpin = 180;
        constexpr uint32_t kTrickNameBytes = 0x100, kCommandBytes = 0x120;
        constexpr uint32_t kZguiGlobals = sym::g_zgui; // zgui's globals (the root at +0x0C), kept in s0

        ee::Ptr<zgui::Node> findWidget(ee::Ptr<zgui::Node> parent, uint32_t path)
        {
            return zgui::findNode(parent, ee::Ptr<const char>(path), uint64_t{1});
        }
        ee::Ptr<SkaterHud> hudOf(ee::Ptr<skater::Player> player) { return player->hud; }
    }

    void setupSkaterHud(ee::Ptr<skater::Player> player)
    {
        ee::Ptr<SkaterHud> &hud = player->hud;
        // The panes the widgets are in: the score's and the meters' (the
        // toggle pane); a third player has none (found from no root).
        ee::Ptr<zgui::Node> pane;
        ee::Ptr<zgui::Node> togglePane;
        hud = ee::Ptr<SkaterHud>(zge::heapAllocate(sizeof(SkaterHud)));
        zge::memoryFill(hud, 0, sizeof(SkaterHud));
        if (*g_gameType == GameType::SplitScreen)
        {
            const uint8_t number = player->number;
            if (number == 0 || number == 1)
            {
                uint32_t togglePath;
                if (number == 0)
                {
                    pane = findWidget(*zgui::g_root, kPlayerOnePane);
                    togglePath = kPlayerOneTogglePane;
                }
                else
                {
                    pane = findWidget(*zgui::g_root, kPlayerTwoPane);
                    togglePath = kPlayerTwoTogglePane;
                }
                togglePane = findWidget(*zgui::g_root, togglePath);
            }
        }
        else
        {
            pane = findWidget(*zgui::g_root, kInGameOverlay);
            togglePane = findWidget(*zgui::g_root, kInGameOverlayToggle);
        }
        hudOf(player)->score = findWidget(pane, kScoreName);
        hudOf(player)->grindMeter = findWidget(togglePane, kHorzBalanceMeter);
        hudOf(player)->balanceMeter = findWidget(togglePane, kVertBalanceMeter);
        hudOf(player)->trickList = findWidget(togglePane, kTrickListName);
        hudOf(player)->potentialScore = findWidget(hudOf(player)->trickList, kPotentialScoreName);
        hudOf(player)->completedTricks = findWidget(hudOf(player)->trickList, kCompletedTrickBuffer);
    }
    READABLE_FUNCTION_WAITS(setupSkaterHud, Verified);

    void freeSkaterHud(ee::Ptr<skater::Player> player)
    {
        ee::Ptr<SkaterHud> &hud = player->hud;
        if (hud)
        {
            zge::heapFree(hud);
            hud = {};
        }
    }
    READABLE_FUNCTION_WAITS(freeSkaterHud, Verified);

    int32_t setCornerColors(ee::Ptr<zgui::Node> widget, ee::Address color)
    {
        // (The first corner's answer is tested by its low byte, as the original's.)
        bool set = (zgui::setVariable(widget, kTopLeftColor, color) & 0xFF) != 0;
        if (set)
            set = zgui::setVariable(widget, kTopRightColor, color) != 0;
        if (set)
            set = zgui::setVariable(widget, kBottomLeftColor, color) != 0;
        if (set)
            set = zgui::setVariable(widget, kBottomRightColor, color) != 0;
        return set;
    }
    READABLE_FUNCTION_WAITS(setCornerColors, Verified);

    void objectName(ee::Ptr<GameObject> object, ee::Address out, uint32_t size)
    {
        ee::Local<NameRequest> request;
        ee::Local<int32_t> answered;
        *answered = 0;
        if (out)
        {
            if (!size)
                return;
            const ee::Ptr<char> name(out);
            *name = 0;
            if (!object)
                return;
            request->size = size;
            request->out = out;
            const int32_t sent = sendObjectMessage(object, *g_msgGetName, request, answered);
            if (sent && *answered)
                return;
            *name = 0;
        }
    }
    READABLE_FUNCTION_WAITS(objectName, Verified);

    // ---- the balance meters -------------------------------------------------
    namespace
    {
        // A meter's three segments ("First", its "Next" and that one's) are
        // coloured by how far the balance leans: green the safe way (the
        // meter's SafeDirection) or under the segment's warning mark, yellow
        // past it, red past its danger mark; each segment also grows and
        // moves with the lean.
        struct BalanceMarks
        {
            ee::Float danger;  // red past it
            ee::Float warning; // yellow past it
        };
        // The meter's colours, ZRGBA_PAL values as the corners take them:
        // each with no palette name (0), so its RGBA is used as it is.
        struct alignas(16) BalanceColors
        {
            zgui::Color red, yellow, green;
        };
        static_assert(sizeof(BalanceMarks[3]) == 0x18 && sizeof(BalanceColors) == 0x30);
        inline ee::Global<BalanceMarks[3], sym::balanceMarks> g_balanceMarks;   // (0.7, 0.4), (0.7, 0.4), (0.8, 0.5)
        inline ee::Global<BalanceColors, sym::balanceColors> g_balanceColors; // red, yellow, green
        constexpr uint32_t kSegments = 3;
        inline constexpr ee::Float kSegmentShift = 24.0f, kSecondSegmentGrowth = 0.5f; // (the lean is clamped to -1 .. 1)

        // The grind meter (horizontal) and the balance meter (vertical).
        struct BalanceMeter
        {
            ee::Ptr<zgui::Node> SkaterHud::*widget;
            uint32_t scaleName;    // "Scale.y" / "Scale.x": grows with the lean
            uint32_t positionName; // "Position.x" / "Position.y": moves with it
        };
        constexpr BalanceMeter kGrindMeter = {&SkaterHud::grindMeter, sym::g_textScaleY, sym::g_textPositionX};
        constexpr BalanceMeter kVerticalMeter = {&SkaterHud::balanceMeter, sym::g_textScaleX, sym::g_textPositionY};

        // The meter's segments shown for `balance` (-1 .. 1, clamped).
        // Returns the segments done (3).
        int32_t showBalanceOn(const BalanceMeter &meter, ee::Ptr<skater::Player> player, ee::Float balance)
        {
            ee::Local<ee::Ptr<zgui::Node>[kSegments]> segments;
            ee::Local<BalanceMarks[kSegments]> marks;
            ee::Local<BalanceColors> colors;
            ee::Local<int32_t> safeDirection; // the meter's SafeDirection
            ee::Local<ee::Float> scale;
            ee::Local<ee::Float> position;
            *safeDirection = 0;
            for (uint32_t index = 0; index < kSegments; ++index)
                marks[index] = (*g_balanceMarks)[index];
            *colors = *g_balanceColors;

            ee::Float limit(-1.0f);
            bool clamp = balance < limit;
            if (!clamp)
            {
                limit = ee::Float(1.0f);
                clamp = limit < balance;
            }
            if (clamp)
                balance = limit;
            const auto widget = [&] { return hudOf(player).get()->*meter.widget; };
            zgui::copyVariable(widget(), ee::Ptr<const char>(kSafeDirection), ee::Ptr<char>(safeDirection.address()));
            segments[0] = findWidget(widget(), kFirstSegment);
            segments[1] = findWidget(widget(), kNextSegment);
            segments[2] = findWidget(segments[1], kNextSegment);
            const ee::Float lean = ee::abs(balance);

            for (uint32_t s = 0; s < kSegments; ++s)
            {
                const ee::Float direction = ee::toFloat(*safeDirection);
                bool safe = ee::Float(0.0f) < direction && ee::Float(0.0f) < balance;
                if (!safe)
                    safe = direction < ee::Float(0.0f) && balance < ee::Float(0.0f);
                zgui::Color *color = &colors->green;
                if (!safe)
                {
                    if (marks[s].danger < lean)
                        color = &colors->red;
                    else if (marks[s].warning < lean)
                        color = &colors->yellow;
                }
                setCornerColors(segments[s], color);
                // The first segment grows with the lean, the second half as much.
                if (s == 0)
                {
                    *scale = lean + ee::Float(1.0f);
                    zgui::setVariable(segments[0], meter.scaleName, scale.address());
                }
                else if (s == 1)
                {
                    *scale = lean * kSecondSegmentGrowth + ee::Float(1.0f);
                    zgui::setVariable(segments[1], meter.scaleName, scale.address());
                }
                // The second and third move with it.
                if (s == 1 || s == 2)
                {
                    *position = balance * kSegmentShift;
                    zgui::setVariable(segments[s], meter.positionName, position.address());
                }
            }
            return static_cast<int32_t>(kSegments);
        }

        // The meter's safe direction (SafeDirection) set, and its "PLAY
        // SetBalance" run.
        int32_t setMeterSafeDirection(ee::Ptr<zgui::Node> SkaterHud::*meter, ee::Ptr<skater::Player> player, int32_t direction)
        {
            ee::Local<int32_t> value;
            *value = direction;
            const auto widget = [&] { return hudOf(player).get()->*meter; };
            zgui::setVariable(widget(), kSafeDirection, value.address());
            return runScriptResult(widget(), kPlaySetBalance);
        }
    }

    int32_t setGrindBalance(ee::Ptr<skater::Player> player, ee::Float balance) { return showBalanceOn(kGrindMeter, player, balance); }
    READABLE_FUNCTION_WAITS(setGrindBalance, Verified);

    int32_t setBalance(ee::Ptr<skater::Player> player, ee::Float balance) { return showBalanceOn(kVerticalMeter, player, balance); }
    READABLE_FUNCTION_WAITS(setBalance, Verified);

    int32_t setGrindSafeDirection(ee::Ptr<skater::Player> player, int32_t direction) { return setMeterSafeDirection(&SkaterHud::grindMeter, player, direction); }
    READABLE_FUNCTION_WAITS(setGrindSafeDirection, Verified);

    int32_t setSafeDirection(ee::Ptr<skater::Player> player, int32_t direction) { return setMeterSafeDirection(&SkaterHud::balanceMeter, player, direction); }
    READABLE_FUNCTION_WAITS(setSafeDirection, Verified);
}

namespace aggressive::hud
{
    namespace
    {
        // A meter's "Visible" set.
        int32_t setMeterVisible(ee::Ptr<zgui::Node> SkaterHud::*meter, ee::Ptr<skater::Player> player, int32_t visible)
        {
            ee::Local<int32_t> value;
            const ee::Ptr<zgui::Node> widget = hudOf(player).get()->*meter;
            *value = visible;
            return zgui::setVariable(widget, kVisibleName, value.address());
        }
        // A meter hidden ("Visible" 0); its caller then clears its safe
        // direction (setGrindSafeDirection or setSafeDirection).
        void hideMeter(ee::Ptr<zgui::Node> SkaterHud::*meter, ee::Ptr<skater::Player> player)
        {
            ee::Local<int32_t> value;
            const ee::Ptr<zgui::Node> widget = hudOf(player).get()->*meter;
            *value = 0;
            zgui::setVariable(widget, kVisibleName, value.address());
        }
    }

    int32_t showGrindMeter(ee::Ptr<skater::Player> player) { return setMeterVisible(&SkaterHud::grindMeter, player, 1); }
    READABLE_FUNCTION_WAITS(showGrindMeter, Verified);

    int32_t hideGrindMeter(ee::Ptr<skater::Player> player)
    {
        hideMeter(&SkaterHud::grindMeter, player);
        return setGrindSafeDirection(player, 0);
    }
    READABLE_FUNCTION_WAITS(hideGrindMeter, Verified);

    int32_t showBalanceMeter(ee::Ptr<skater::Player> player) { return setMeterVisible(&SkaterHud::balanceMeter, player, 1); }
    READABLE_FUNCTION_WAITS(showBalanceMeter, Verified);

    int32_t hideBalanceMeter(ee::Ptr<skater::Player> player)
    {
        hideMeter(&SkaterHud::balanceMeter, player);
        return setSafeDirection(player, 0);
    }
    READABLE_FUNCTION_WAITS(hideBalanceMeter, Verified);

    // ---- the trick list ------------------------------------------------------

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemsetLibc, m, c, n); }
    }

    namespace
    {
        // Whether the trick line's text has anything in it yet.
        bool lineHasText(ee::Ptr<zgui::Node> line)
        {
            const ee::Ptr<char> text(*ee::Ptr<uint32_t>(zgui::variableValue(line, ee::Ptr<const char>(kTextName))));
            return text && *text != 0;
        }
    }

    int32_t showSpinText(ee::Ptr<skater::Player> player)
    {
        ee::Local<uint8_t[kTrickNameBytes]> trickName;
        ee::Local<uint8_t[kCommandBytes]> commandText;
        ee::Local<int32_t> activated;
        const uint32_t command = commandText.address();
        memset(trickName, 0, kTrickNameBytes);
        const ee::Ptr<SkaterHud> hud = hudOf(player);
        const ee::Ptr<zgui::Node> completed = hud->completedTricks;
        const ee::Ptr<zgui::Node> trickList = hud->trickList;
        const ee::Ptr<GameObject> current = skater::skaterCurrentObject(player);
        objectName(current, trickName, kTrickNameBytes);
        const int32_t counterClockwise = aggressive::skater::counterClockwiseSpins(player);
        const int32_t clockwise = aggressive::skater::clockwiseSpins(player);
        *activated = 0;
        memset(command, 0, kCommandBytes);

        // The spin in degrees ("SET Text {...}" for the list).
        if (counterClockwise + clockwise != 0)
        {
            bool both = false;
            uint32_t format = kSpinClockwise;
            int32_t degrees = clockwise * kDegreesPerSpin;
            if (counterClockwise != 0)
            {
                both = clockwise != 0;
                format = kSpinCounterClockwise;
                degrees = counterClockwise * kDegreesPerSpin;
            }
            if (both)
                zge::formatString(command, kSpinBoth, counterClockwise * kDegreesPerSpin, clockwise * kDegreesPerSpin);
            else
                zge::formatString(command, format, degrees);
        }
        else
            zge::stringCopy(command, kSpinNone);
        zgui::runScript(trickList, ee::Ptr<char>(command), 0);
        zgui::runScript(completed, ee::Ptr<char>(kAppendParentText), 0);
        zgui::copyVariable(trickList, ee::Ptr<const char>(kIsActivated), ee::Ptr<char>(activated.address()));
        if (!*activated)
            zgui::runScript(trickList, ee::Ptr<char>(kPlayFadeIn), 0);

        // The current trick's name after the line's (" + " between them).
        if (lineHasText(completed) && trickName[0] != 0)
            zge::formatString(command, kAppendNextTrick, trickName);
        else
            zge::formatString(command, kAppendFirstTrick, trickName);
        zgui::runScript(trickList, ee::Ptr<char>(command), 0);
        return runScriptResult(trickList, kPlayRefresh);
    }
    READABLE_FUNCTION_WAITS(showSpinText, Verified);

    int32_t addCompletedTrick(ee::Ptr<skater::Player> player, ee::Ptr<GameObject> trick)
    {
        ee::Local<uint8_t[0x100]> trickName;
        ee::Local<uint8_t[0x150]> command;
        memset(trickName, 0, kTrickNameBytes);
        const ee::Ptr<zgui::Node> completed = hudOf(player)->completedTricks;
        objectName(trick, trickName, kTrickNameBytes);
        if (trickName[0] == 0)
            return 0;
        memset(command, 0, kCommandBytes);
        if (lineHasText(completed))
            zge::formatString(command, kAppendNextTrick, trickName);
        else
            zge::formatString(command, kAppendFirstTrick, trickName);
        zgui::runScript(completed, ee::Ptr<char>(command.address()), 0);
        return runScriptResult(completed, kPlayParentRefresh);
    }
    READABLE_FUNCTION_WAITS(addCompletedTrick, Verified);

    int32_t clearTrickList(ee::Ptr<skater::Player> player)
    {
        ee::Local<int32_t> value;
        const ee::Ptr<SkaterHud> hud = hudOf(player);
        const ee::Ptr<zgui::Node> trickList = hud->trickList;
        const ee::Ptr<zgui::Node> completed = hud->completedTricks;
        *value = 0;
        zgui::setVariable(completed, kTextName, kNoText);
        zgui::setVariable(trickList, kIsActivated, value);
        return runScriptResult(trickList, kPlayFadeOut);
    }
    READABLE_FUNCTION_WAITS(clearTrickList, Verified);

    namespace
    {
        namespace tail
        {
        void runScript(ee::Ptr<zgui::Node> widget, uint32_t script) { game::tail(zgui::runScript, widget, ee::Ptr<char>(script), 0); }
        }
    }

    void resetTrickList(ee::Ptr<skater::Player> player)
    {
        const ee::Ptr<zgui::Node> trickList = hudOf(player)->trickList;
        clearTrickList(player);
        zgui::setVariable(trickList, kTextName, kNoText);
        tail::runScript(trickList, kStopFadeOut);
    }
    READABLE_FUNCTION_WAITS(resetTrickList, Verified);

    int32_t formatScore(int32_t score, ee::Address out)
    {
        ee::Local<uint8_t[0xB0]> reversed;
        memset(reversed, 0, 0x80);
        const bool negative = score < 0;
        int32_t rest = negative ? -score : score;
        if (rest == 0)
        {
            const int32_t result = static_cast<int32_t>(zge::stringCopy(out, kZeroText));
            return result;
        }
        // The digits from the last, a comma before every fourth.
        int32_t length = 0;
        int32_t inGroup = 0;
        while (rest > 0)
        {
            const int32_t digit = rest % 10;
            ++inGroup;
            if (inGroup >= 4)
            {
                reversed[length++] = ',';
                inGroup = 1;
            }
            reversed[length++] = static_cast<uint8_t>('0' + digit);
            rest = (rest - digit) / 10;
        }
        if (negative)
            reversed[length++] = '-';

        // Copied out in reading order. Returns the last character copied.
        ee::Ptr<uint8_t> text(out);
        int32_t lastCopied = length == 0 ? 1 : 0;
        for (int32_t index = length - 1; index >= 0; --index)
        {
            lastCopied = reversed[index];
            *text = reversed[index];
            ++text;
        }
        *text = 0;
        return lastCopied;
    }
    READABLE_FUNCTION(formatScore, Verified);
}
