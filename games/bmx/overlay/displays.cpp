// BMX XXX's in-game overlay (displays.h): the display table's functions and
// the displays with little to them (the timer's set-up, the replay's song
// info, the cut scene's subtitle, the handlers that do nothing). BMX XXX's
// own code (./src/com/action/overlay.c).

#include "displays.h"

#include "bmx/fx/spawn_effects.h"
#include "bmx/game/round.h"
#include "bmx/ui/strings.h"
#include "bmx/ui/trick_text.h"
#include "bmx/ui/ui_nodes.h"
#include "bmx/objectives/objectives.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/objectives.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/overlay/displays.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using namespace zge::zgui;
    // The overlay's nodes are zgui's widgets, not the scene's nodes.
    using zge::zgui::Node;

    namespace
    {
        inline ee::Global<OverlayDisplay[kOverlayDisplays], sym::g_overlayDisplays> g_overlayDisplays;
        // The overlay's widget tree, and each player's own.
        inline ee::Global<ee::Ptr<Node>, sym::g_overlayRoot> g_overlayRoot;
        inline ee::Global<ee::Ptr<Node>[kOverlayPlayers], sym::g_overlayPlayerRoots> g_overlayPlayerRoots;

        // What the displays here show.
        // The timer's: the time, all ones when there is none.
        struct TimerDisplayState
        {
            uint32_t time;
            uint8_t _04[4];
        };
        inline ee::Global<TimerDisplayState, sym::g_timerDisplay> g_timerDisplay;
        // The song info's: the song's number ("$KSTR_SONGINFO_%02d_..."), 0xFF none.
        struct SongInfoDisplayState
        {
            uint8_t song;
            uint8_t _01[0x0B];
        };
        inline ee::Global<SongInfoDisplayState, sym::g_songInfoDisplay> g_songInfoDisplay;
        constexpr uint8_t kNoSong = 0xFF;
        // The subtitle's: the line's string id, 0 none.
        struct SubtitleDisplayState
        {
            uint32_t text;
            uint8_t _04[4];
        };
        inline ee::Global<SubtitleDisplayState, sym::g_subtitleDisplay> g_subtitleDisplay;

        // A value for a player's display, as the game code sends it (the
        // score's, the health's).
        struct ValueMessage
        {
            uint8_t player;  // +0x00
            uint8_t flags;   // +0x01 kValueAtOnce
            uint8_t _02[2];
            ee::Float value; // +0x04
        };
        static_assert(sizeof(ValueMessage) == 0x08);
        // The value is shown as it is, not counted or slid towards.
        constexpr uint8_t kValueAtOnce = 0x1;
        // What a display that moves towards its value keeps for a player:
        // the value on the screen, and the last message.
        struct MovingValueState
        {
            uint8_t _00[4];
            ee::Float shown;     // +0x04
            ValueMessage wanted; // +0x08
        };
        static_assert(sizeof(MovingValueState) == 0x10 && offsetof(MovingValueState, wanted) == 0x08);
        inline ee::Global<MovingValueState[kOverlayPlayers], sym::g_scoreDisplay> g_scoreDisplay;
        inline ee::Global<MovingValueState[kOverlayPlayers], sym::g_healthDisplay> g_healthDisplay;
        // No score to show: all ones, as a float.
        const ee::Float kNoScore = ee::Float::fromBits(0x4F800000u);
        // The balance display's, per player (0x10 bytes; its message handler's).
        struct BalanceMessage
        {
            uint8_t player;   // +0x00
            uint8_t pad01;    // padding: no reader (int25: the senders never set it; only whole-message copies move it)
            uint16_t flags;   // +0x02 BalanceMessageFlags
            ee::Float value;  // +0x04 the balance: -1 .. 1, 0 upright
        };
        static_assert(sizeof(BalanceMessage) == 0x08);
        struct BalanceDisplayState
        {
            BalanceMessage shown; // +0x00 the last message drawn
            uint8_t _08[0x08];
        };
        static_assert(sizeof(BalanceDisplayState) == 0x10);
        inline ee::Global<BalanceDisplayState[kOverlayPlayers], sym::g_balanceDisplay> g_balanceDisplay;
        // OverlayDisplay::Player::flags, the balance display's own: its widget has faded in.
        constexpr uint32_t kBalanceFadedIn = 0x2;
        constexpr uint32_t kBalanceVertical = 0x100; // the meter stands (its message's flag, kept)
        constexpr uint32_t kBalanceMirrored = 0x800; // the meter's sides are swapped (the same)
        enum BalanceMessageFlags : uint16_t
        {
            kBalanceInit = 0x20,          // "Init", not faded in
            kBalanceFadeIn = 0x40,        // the meter's type and side taken, "FadeIn", and drawn
            kBalanceFadeOut = 0x80,       // "FadeOut" when faded in
            kBalanceMessageVertical = 0x100,
            kBalanceMessageMirrored = 0x800,
        };
        // The reward display's six items a player: what each is and how far
        // its widget's "State" has got.
        constexpr uint32_t kRewardItems = 6;
        struct RewardItemState
        {
            uint8_t player; // +0x00 the first eight bytes are the item's message, but for the phase
            uint8_t kind;   // +0x01 picks its text, its colour and the sound it appears with
            uint16_t phase; // +0x02 RewardPhase; its widget's "State" otherwise
            uint32_t value; // +0x04 a number (a float's bits) or a pointer, by the kind
        };
        static_assert(sizeof(RewardItemState) == 0x08);
        inline ee::Global<RewardItemState[kOverlayPlayers][kRewardItems], sym::g_rewardDisplay> g_rewardDisplay;
        inline ee::Global<ee::Ptr<Node>[kOverlayPlayers][kRewardItems], sym::g_rewardItemWidgets> g_rewardItemWidgets;
        enum RewardPhase : uint32_t
        {
            kRewardWaiting = 1, // to appear once no other item is appearing
            kRewardAppearing = 2, // this and the next: on its way in
        };
        // The objective display's, per player: `value` is what its message
        // handler last set, 0 while there is nothing to show.
        struct ObjectiveDisplayState
        {
            uint8_t player;   // +0x00 the first eight bytes are its last message's
            uint8_t pad01;    // padding: no reader (int25: the senders never set it; only whole-message copies move it)
            uint16_t visible; // +0x02 whether its widget is visible, as objectiveDisplayState last found it
            uint32_t goal;    // +0x04 the goal's number (kNoGoal: none)
            uint32_t value; // +0x08
            uint8_t _0C[0x0C];
        };
        static_assert(sizeof(ObjectiveDisplayState) == 0x18);
        inline ee::Global<ObjectiveDisplayState[kOverlayPlayers], sym::g_objectiveDisplay> g_objectiveDisplay;
        // The hint display's, per player.
        struct HintDisplayState
        {
            uint8_t player; // +0x00 the first two bytes are its last message's
            uint8_t hint;   // +0x01 the hint's number (kNoHint: none)
            uint16_t visible; // +0x02 the low half of its widget's "Visible", as hintDisplayState last read it
            uint8_t _04[0x04];
        };
        static_assert(sizeof(HintDisplayState) == 0x08);
        inline ee::Global<HintDisplayState[kOverlayPlayers], sym::g_hintDisplay> g_hintDisplay;
        // What the hint's and the objective's message handlers are handed.
        struct HintMessage
        {
            uint8_t player;
            uint8_t hint;
            uint8_t _02[2];
        };
        struct ObjectiveMessage
        {
            uint8_t player;   // +0x00
            uint8_t _01;
            uint16_t visible; // +0x02
            uint32_t goal;    // +0x04
            uint32_t _08;
        };
        static_assert(sizeof(HintMessage) == 4 && sizeof(ObjectiveMessage) == 0xC);
        constexpr uint8_t kNoHint = 0xFF;
        constexpr uint32_t kNoGoal = 0xFFFFFFFFu;
        constexpr uint8_t kNoReward = 0xFF;
        // The game mode with no timer.
        constexpr uint32_t kNoTimerMode = 3;
        // The trick display's, per player: the line last written and the
        // next to write, its four name lines and its score label.
        struct TrickDisplayState
        {
            uint8_t line;              // +0x00
            uint8_t next;              // +0x01
            uint8_t _02[2];
            ee::Ptr<Node> lines[4];    // +0x04 "LabelName00" ..
            ee::Ptr<Node> score;       // +0x14 "LabelScore"
        };
        static_assert(sizeof(TrickDisplayState) == 0x18);
        inline ee::Global<TrickDisplayState[kOverlayPlayers], sym::g_trickDisplay> g_trickDisplay;
        // Each player's widget file by how many play ("%s.zsc", "UI_%s").
        inline ee::Global<ee::Ptr<const char>[2][2], sym::g_overlayPlayerLayouts> g_overlayPlayerLayouts;
        // Display 2's (no widget): four bytes its message sets.
        struct Display2State
        {
            uint8_t first;   // +0x00
            uint8_t _01;
            uint16_t second; // +0x02 cleared by every message
            uint8_t _04[0x04];
        };
        inline ee::Global<Display2State, sym::g_display2> g_display2;

        // The exit warning's, per player: `value` is 0xFF while there is
        // nothing to warn of.
        struct ExitWarningDisplayState
        {
            uint8_t _00;
            uint8_t value; // +0x01
        };
        inline ee::Global<ExitWarningDisplayState[kOverlayPlayers], sym::g_exitWarningDisplay> g_exitWarningDisplay;
        constexpr uint8_t kNoExitWarning = 0xFF;
        // The exit warning's message.
        struct ExitWarningMessage
        {
            uint8_t player;
            uint8_t value;
        };

        // A display's widget found in each player's own tree.
        void findPlayerWidgets(OverlayDisplay *display, uint32_t name)
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
                display->players[player].widget = zge::zgui::findNode((*g_overlayPlayerRoots)[player], name, 1);
        }

        // The timer's place when the screen is split.
        struct WidgetPosition
        {
            ee::Float x, y;
            uint32_t z;
        };

        // The level's own strings (loaded as the overlay opens), and the game's.
        inline ee::Global<ee::Ptr<StringTable>, sym::g_levelStrings> g_levelStrings;

        // A colour: red, green, blue, alpha.
        struct Rgba
        {
            uint8_t channel[4];
        };
        // A widget's colour variable: the colour, and twelve more bytes.
        struct ColourValue
        {
            Rgba colour;
            uint32_t notSet; // the original leaves what its stack held here
            uint64_t zero;
        };
        static_assert(sizeof(ColourValue) == 0x10);
        // The timer's colours: the warning one and the usual one.
        inline ee::Global<ColourValue[2], sym::g_timerColours> g_timerColours;
        // mixColours' result.
        inline ee::Global<Rgba, sym::g_mixedColour> g_mixedColour;
        // The health bar's colours: empty and full.
        inline ee::Global<Rgba[2], sym::g_healthBarColours> g_healthBarColours;
        // The fades' names, by OverlayFade.
        inline ee::Global<ee::Ptr<const char>[8], sym::g_overlayFadeNames> g_overlayFadeNames;
        // Which of the logo's materials the logo script shows next (only the first is listed).
        inline ee::Global<uint8_t, sym::g_overlayLogo> g_overlayLogo;

        namespace calls
        {
            // Scaffolding (address-call): docs/SCAFFOLDING.md. bmx.c's widget
            // actions and text wrapping, the objectives' status text and fx.c's
            // shard are other lanes' (b7bmxc, b7obj, b7game2): called by address
            // until their files are in this tree.
            // The objective of a name's hash (named here by its use).
            uint32_t objectiveNamed(uint32_t name) { return ee::game<uint32_t>(sym::objectiveByName, name); }
        }
        // The displays' own formats.
        constexpr uint32_t kOverlayDegreesFormat = sym::bmx::g_overlayDegreesFormat;
        constexpr uint32_t kOverlayDescriptionFormat = sym::bmx::g_overlayDescriptionFormat;
        constexpr uint32_t kOverlayHintFormat = sym::bmx::g_overlayHintFormat;
        constexpr uint32_t kOverlayHintTextFormat = sym::bmx::g_overlayHintTextFormat;
        constexpr uint32_t kOverlayMinutesFormat = sym::bmx::g_overlayMinutesFormat;
        constexpr uint32_t kOverlayMissingStringFormat = sym::bmx::g_overlayMissingStringFormat;
        constexpr uint32_t kOverlayNameFormat = sym::bmx::g_overlayNameFormat;
        constexpr uint32_t kOverlayNumberedNameFormat = sym::bmx::g_overlayNumberedNameFormat;
        constexpr uint32_t kOverlayRewardItemFormat = sym::bmx::g_overlayRewardItemFormat;
        constexpr uint32_t kOverlayScoreFormat = sym::bmx::g_overlayScoreFormat;
        constexpr uint32_t kOverlayScriptFileFormat = sym::bmx::g_overlayScriptFileFormat;
        constexpr uint32_t kOverlayScriptRootFormat = sym::bmx::g_overlayScriptRootFormat;
        constexpr uint32_t kOverlaySecondsFormat = sym::bmx::g_overlaySecondsFormat;
        constexpr uint32_t kOverlaySongInfoFormat = sym::bmx::g_overlaySongInfoFormat;
        constexpr uint32_t kOverlayTimerFormat = sym::bmx::g_overlayTimerFormat;
        constexpr uint32_t kOverlayTricksFormat = sym::bmx::g_overlayTricksFormat;
        // The modes that show no score (the word game) and the health bar's numbers.
        constexpr uint8_t kNoScoreMode = 6;
        constexpr uint32_t kHealthShardFlags = 5;
        constexpr int32_t kHealthShardLifetime = 1500;
    }

    // ---- the table

    void closeOverlayDisplays()
    {
        for (uint32_t i = 0; i < kOverlayDisplays; ++i)
        {
            OverlayDisplay &display = (*g_overlayDisplays)[i];
            if (display.setUp)
                display.setUp(&display, 0);
        }
        zge::zgui::destroyNode(*g_overlayRoot);
        *g_overlayRoot = ee::Ptr<Node>();
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            zge::zgui::destroyNode((*g_overlayPlayerRoots)[player]);
            (*g_overlayPlayerRoots)[player] = ee::Ptr<Node>();
        }
    }
    READABLE_FUNCTION(closeOverlayDisplays, Verified);

    void updateOverlayDisplays()
    {
        for (uint32_t i = 0; i < kOverlayDisplays; ++i)
        {
            OverlayDisplay &display = (*g_overlayDisplays)[i];
            if ((display.players[0].flags & kOverlayDisplayHidden) == 0 && display.frame)
                display.frame(&display);
        }
    }
    READABLE_FUNCTION(updateOverlayDisplays, Verified);

    // showOverlayDisplay: the first player's flag says whether it is shown
    // now; on a change the handler is told and every player's flag follows.
    void showOverlayDisplay(uint32_t index, uint32_t shown)
    {
        OverlayDisplay &display = (*g_overlayDisplays)[index];
        const uint32_t shownNow = (display.players[0].flags ^ kOverlayDisplayHidden) & kOverlayDisplayHidden;
        if (shown == shownNow)
            return;
        if (display.show)
            display.show(&display, shown);
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            if (shown != 0)
                display.players[player].flags &= ~kOverlayDisplayHidden;
            else
                display.players[player].flags |= kOverlayDisplayHidden;
        }
    }
    READABLE_FUNCTION(showOverlayDisplay, Verified);

    void overlayMessage(uint32_t index, ee::Address message)
    {
        OverlayDisplay &display = (*g_overlayDisplays)[index];
        if (display.message)
            display.message(&display, message);
    }
    READABLE_FUNCTION(overlayMessage, Verified);

    void overlayDisplayState(uint32_t index, ee::Address out)
    {
        OverlayDisplay &display = (*g_overlayDisplays)[index];
        if (display.state)
            display.state(&display, out);
    }
    READABLE_FUNCTION(overlayDisplayState, Verified);

    // ---- handlers with nothing to do (displays 2 and 8, which have no
    // widget, and the hint's show)

    void display2SetUp()
    {
        memoryFill(&*g_display2, 0, sizeof(Display2State));
        g_display2->first = 0;
    }
    READABLE_FUNCTION(display2SetUp, Verified);
    // display2Message: the message's four bytes kept, the second half cleared.
    void display2Message(OverlayDisplay *, ee::Address message)
    {
        memoryCopy(&*g_display2, message, 4);
        g_display2->second = 0;
    }
    READABLE_FUNCTION(display2Message, Verified);
    void display2Frame() {}
    READABLE_FUNCTION(display2Frame, Verified);
    void display2Show() {}
    READABLE_FUNCTION(display2Show, Verified);
    void display8SetUp() {}
    READABLE_FUNCTION(display8SetUp, Verified);
    void display8Frame() {}
    READABLE_FUNCTION(display8Frame, Verified);
    void display8Show() {}
    READABLE_FUNCTION(display8Show, Verified);
    void display8Message() {}
    READABLE_FUNCTION(display8Message, Verified);
    void hintDisplayShow() {}
    READABLE_FUNCTION(hintDisplayShow, Verified);

    // ---- the timer

    // timerDisplaySetUp: no time yet, and the widget found; with two
    // players it moves to the middle of the screen's top.
    void timerDisplaySetUp(OverlayDisplay *display)
    {
        ee::Local<WidgetPosition> position;
        memoryFill(&*g_timerDisplay, 0, sizeof(TimerDisplayState));
        const ee::Ptr<Node> widget = zge::zgui::findNode(*g_overlayRoot, sym::g_overlayTimerName, 1); // "Overlay.DisplayTimer"
        display->players[0].widget = widget;
        if (widget != 0 && *g_playerCount >= 2)
        {
            position->x = ee::Float(256.0f);
            position->y = ee::Float(53.0f);
            position->z = 0;
            zge::zgui::setVariable(widget, sym::g_overlayTextPosition, position); // "Position"
        }
    }
    READABLE_FUNCTION(timerDisplaySetUp, Verified);

    // ---- the replay's song info

    void songInfoDisplaySetUp(OverlayDisplay *display)
    {
        memoryFill(&*g_songInfoDisplay, 0, sizeof(SongInfoDisplayState));
        display->players[0].widget = zge::zgui::findNode(*g_overlayRoot, sym::g_overlaySongInfoName, 1); // "OverlayReplay.Demo.LabelSongInfo"
    }
    READABLE_FUNCTION(songInfoDisplaySetUp, Verified);

    // songInfoDisplayShow: visible only while there is a song to name.
    void songInfoDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        if (display->players[0].widget == 0)
            return;
        if (g_songInfoDisplay->song == kNoSong)
            *visible = 0;
        zge::zgui::setVariable(display->players[0].widget, sym::g_overlayTextVisible, visible); // "Visible"
    }
    READABLE_FUNCTION(songInfoDisplayShow, Verified);

    // ---- the cut scene's subtitle

    void subtitleDisplaySetUp(OverlayDisplay *display)
    {
        memoryFill(&*g_subtitleDisplay, 0, sizeof(SubtitleDisplayState));
        display->players[0].widget = zge::zgui::findNode(*g_overlayRoot, sym::g_overlaySubtitleName, 1); // "OverlayCutScene.DisplaySubtitle"
    }
    READABLE_FUNCTION(subtitleDisplaySetUp, Verified);

    // subtitleDisplayShow: visible only while there is a line.
    void subtitleDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        if (g_subtitleDisplay->text == 0)
            *visible = 0;
        zge::zgui::setVariable(display->players[0].widget, sym::g_overlayTextVisible, visible); // "Visible"
    }
    READABLE_FUNCTION(subtitleDisplayShow, Verified);

    // ---- the displays each player has: set-ups and shows

    // trickDisplayShow: every player's widget shown or hidden.
    void trickDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            if (display->players[player].widget != 0)
                zge::zgui::setVariable(display->players[player].widget, sym::g_overlayTextVisible, visible);
        }
    }
    READABLE_FUNCTION(trickDisplayShow, Verified);

    void healthDisplaySetUp(OverlayDisplay *display)
    {
        memoryFill(&*g_healthDisplay, 0, sizeof(MovingValueState[kOverlayPlayers]));
        findPlayerWidgets(display, sym::g_overlayHealthName); // "Overlay.DisplayHealth"
    }
    READABLE_FUNCTION(healthDisplaySetUp, Verified);

    // healthDisplayShow: every player's widget shown or hidden.
    void healthDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            if (display->players[player].widget != 0)
                zge::zgui::setVariable(display->players[player].widget, sym::g_overlayTextVisible, visible);
        }
    }
    READABLE_FUNCTION(healthDisplayShow, Verified);

    void objectiveDisplaySetUp(OverlayDisplay *display)
    {
        memoryFill(&*g_objectiveDisplay, 0, sizeof(ObjectiveDisplayState[kOverlayPlayers]));
        findPlayerWidgets(display, sym::g_overlayObjectiveName); // "Overlay.DisplayObjctv"
    }
    READABLE_FUNCTION(objectiveDisplaySetUp, Verified);

    // objectiveDisplayShow: a player's widget is visible only while that
    // player has something to show. Kept as it is: once one player has
    // nothing, the players after are hidden too.
    void objectiveDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<Node> widget = display->players[player].widget;
            if (widget == 0)
                continue;
            if ((*g_objectiveDisplay)[player].value == 0)
                *visible = 0;
            zge::zgui::setVariable(widget, sym::g_overlayTextVisible, visible);
        }
    }
    READABLE_FUNCTION(objectiveDisplayShow, Verified);

    // rewardDisplayShow: every player's widget shown or hidden, with no
    // check that the widget was found.
    void rewardDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        if (*g_playerCount == 0)
            return;
        uint32_t player = 0;
        do
        {
            zge::zgui::setVariable(display->players[player].widget, sym::g_overlayTextVisible, visible);
            ++player;
        } while (player < *g_playerCount);
    }
    READABLE_FUNCTION(rewardDisplayShow, Verified);

    void exitWarningDisplaySetUp(OverlayDisplay *display)
    {
        memoryFill(&*g_exitWarningDisplay, 0, sizeof(ExitWarningDisplayState[kOverlayPlayers]));
        findPlayerWidgets(display, sym::g_overlayExitWarningName); // "Overlay.DisplayExitWarn"
    }
    READABLE_FUNCTION(exitWarningDisplaySetUp, Verified);

    // exitWarningDisplayShow: a player's widget is visible only while that
    // player has a warning; as the objective's, one player without hides
    // the players after.
    void exitWarningDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<Node> widget = display->players[player].widget;
            if (widget == 0)
                continue;
            if ((*g_exitWarningDisplay)[player].value == kNoExitWarning)
                *visible = 0;
            zge::zgui::setVariable(widget, sym::g_overlayTextVisible, visible);
        }
    }
    READABLE_FUNCTION(exitWarningDisplayShow, Verified);

    // ---- state queries: `out`'s first byte names the player; the player's
    // state is copied out with whether the widget is visible now

    void hintDisplayState(OverlayDisplay *, ee::Address out)
    {
        ee::Local<uint32_t> visible;
        const uint8_t player = *ee::Ptr<uint8_t>(out);
        HintDisplayState &state = (*g_hintDisplay)[player];
        const ee::Ptr<Node> widget = zge::zgui::findNode((*g_overlayPlayerRoots)[player], sym::g_overlayHintName, 1); // "Overlay.DisplayHint"
        zge::zgui::copyVariable(widget, sym::g_overlayTextVisible, visible);
        state.visible = static_cast<uint16_t>(*visible);
        memoryCopy(out, &state, 4);
    }
    READABLE_FUNCTION(hintDisplayState, Draft);

    void objectiveDisplayState(OverlayDisplay *display, ee::Address out)
    {
        ee::Local<uint32_t> visible;
        const uint8_t player = *ee::Ptr<uint8_t>(out);
        ObjectiveDisplayState &state = (*g_objectiveDisplay)[player];
        zge::zgui::copyVariable(display->players[player].widget, sym::g_overlayTextVisible, visible);
        state.visible = *visible != 0 ? 1 : 0;
        memoryCopy(out, &state, 0x0C);
    }
    READABLE_FUNCTION(objectiveDisplayState, Verified);
    // ---- the overlay's parts and fades

    // showOverlayPart: one of the overlay's widget groups shown or hidden;
    // part 0 is every group but the demo's.
    void showOverlayPart(uint32_t part, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        const uint32_t parts = part == kOverlayAllParts ? kOverlayEveryPartButDemo : 1u << part;
        if (parts & (1u << kOverlayShared))
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayRoot, sym::g_overlayName, 1), sym::g_overlayTextVisible, visible); // "Overlay"
        if (parts & (1u << kOverlayReplay))
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayRoot, sym::g_overlayReplayName, 1), sym::g_overlayTextVisible, visible); // "OverlayReplay"
        if (parts & (1u << kOverlayReplayDemo))
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayRoot, sym::g_overlayReplayDemoName, 1), sym::g_overlayTextVisible, visible); // "OverlayReplay.Demo"
        if (parts & (1u << kOverlayCutScene))
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayRoot, sym::g_overlayCutSceneName, 1), sym::g_overlayTextVisible, visible); // "OverlayCutScene"
        if (parts & (1u << kOverlayPlayersOwn))
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
                zge::zgui::setVariable(zge::zgui::findNode((*g_overlayPlayerRoots)[player], sym::g_overlayName, 1), sym::g_overlayTextVisible, visible);
        }
    }
    READABLE_FUNCTION(showOverlayPart, Verified);

    // playOverlayFade: a fade of a player's part of the screen played, or
    // of the whole screen (kOverlayWholeScreen).
    void playOverlayFade(uint32_t player, uint32_t fade)
    {
        ee::Local<ee::Ptr<const char>[8]> names;
        for (uint32_t i = 0; i < 8; ++i)
            names[i] = (*g_overlayFadeNames)[i];
        ee::Ptr<Node> widget;
        if (player == kOverlayWholeScreen)
            widget = zge::zgui::findNode(*g_overlayRoot, sym::g_overlayFadeName, 1); // "Fade"
        else
            widget = zge::zgui::findNode((*g_overlayPlayerRoots)[player], sym::g_overlayFadeName, 1);
        playUiNode(widget, names[fade]);
    }
    READABLE_FUNCTION(playOverlayFade, Verified);

    // mixColours: the colour `amount` of the way from one to the other, each
    // channel kept within a byte. The result is one shared colour.
    ee::Ptr<Rgba> mixColours(const Rgba *from, const Rgba *to, ee::Float amount)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            const int32_t mixed = ee::toInt(ee::toFloat(from->channel[i]) + amount * (ee::toFloat(to->channel[i]) - ee::toFloat(from->channel[i])));
            g_mixedColour->channel[i] = static_cast<uint8_t>(mixed >= 0x100 ? 0xFF : mixed < 0 ? 0 : mixed);
        }
        return &*g_mixedColour;
    }
    READABLE_FUNCTION(mixColours, Verified);

    // overlayDisplayTimerUpdate: the script function the overlay registers
    // under that name: the logo image under the widget it is called for
    // given its material and faded in. It leaves no result of its own.
    void overlayDisplayTimerUpdate(ee::Ptr<ee::Ptr<Node>> arguments)
    {
        ee::Local<uint32_t[1]> materials;
        materials[0] = sym::g_overlayLogoMaterial; // "Common.Logo"
        const ee::Ptr<Node> logo = zge::zgui::findNode(arguments[0], sym::g_overlayLogoImageName, 1); // "ImageLogo"
        zge::zgui::setVariable(logo, sym::g_overlayTextMaterial, materials[*g_overlayLogo]); // "Material"
        playUiNode(logo, sym::g_overlayTextFadeIn); // "FadeIn"
        *g_overlayLogo = 0;
    }
    READABLE_FUNCTION(overlayDisplayTimerUpdate, Draft);

    // ---- the timer's show

    // timerDisplayShow: hidden whatever is asked while the mode keeps no
    // score or there is no time.
    void timerDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        if (gameModeRecord()->scored == 0)
            *visible = 0;
        if (g_timerDisplay->time == ~0u)
            *visible = 0;
        zge::zgui::setVariable(display->players[0].widget, sym::g_overlayTextVisible, visible);
    }
    READABLE_FUNCTION(timerDisplayShow, Verified);

    // ---- the cut scene's subtitle

    // subtitleDisplayMessage: the line (a string of the level's own table,
    // wrapped at fifty characters) put in the caption and shown; no line
    // empties and hides it.
    void subtitleDisplayMessage(OverlayDisplay *display, ee::Address message)
    {
        ee::Local<char[0x400]> caption;
        ee::Local<uint32_t> visible;
        const uint32_t text = *ee::Ptr<uint32_t>(message);
        g_subtitleDisplay->text = text;
        if (text != 0)
        {
            setStringTable(*g_levelStrings);
            wrapText(caption, stringTableText(g_subtitleDisplay->text), 50);
            setStringTable(*g_gameStrings);
            *visible = 1;
        }
        else
        {
            caption[0] = 0;
            *visible = 0;
        }
        zge::zgui::setVariable(zge::zgui::findNode(display->players[0].widget, sym::g_overlayCaptionName, 1), sym::g_overlayTextText, caption); // "LabelCaption", "Text"
        zge::zgui::setVariable(display->players[0].widget, sym::g_overlayTextVisible, visible);
    }
    READABLE_FUNCTION(subtitleDisplayMessage, Verified);

    // ---- the score

    // scoreDisplaySetUp: each player's widget found and its label emptied.
    void scoreDisplaySetUp(OverlayDisplay *display)
    {
        memoryFill(&*g_scoreDisplay, 0, sizeof(MovingValueState[kOverlayPlayers]));
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<Node> widget = zge::zgui::findNode((*g_overlayPlayerRoots)[player], sym::g_overlayScoreName, 1); // "Overlay.DisplayScore"
            display->players[player].widget = widget;
            if (widget != 0)
                zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlayScoreLabelName, 1), sym::g_overlayTextText, sym::g_overlayNoText); // "LabelScore", "Text", ""
        }
    }
    READABLE_FUNCTION(scoreDisplaySetUp, Verified);

    // scoreDisplayShow: each player's widget shown or hidden; hidden
    // whatever is asked in a mode that keeps no score, in the word game and
    // with no score to show. Once hidden for a player it is hidden for the
    // players after (the value is not set again).
    void scoreDisplayShow(OverlayDisplay *display, uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        *visible = shown;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            if (display->players[player].widget == 0)
                continue;
            if (gameModeRecord()->scored == 0)
                *visible = 0;
            else if (gameMode() == kNoScoreMode)
                *visible = 0;
            if ((*g_scoreDisplay)[player].shown == kNoScore)
                *visible = 0;
            zge::zgui::setVariable(display->players[player].widget, sym::g_overlayTextVisible, visible);
        }
    }
    READABLE_FUNCTION(scoreDisplayShow, Verified);

    // scoreDisplayMessage: a player's score to show (the frame counts up to
    // it). No score hides the widget; so does a mode that keeps none, and
    // the display being hidden. The word game leaves the widget as it is.
    void scoreDisplayMessage(OverlayDisplay *display, const ValueMessage *message)
    {
        ee::Local<uint32_t> visible;
        const ee::Ptr<Node> widget = display->players[message->player].widget;
        if (widget == 0)
            return;
        MovingValueState &state = (*g_scoreDisplay)[message->player];
        const ee::Float value = message->value;
        state.wanted.value = value;
        state.wanted.flags |= message->flags;
        *visible = 1;
        if (value == kNoScore)
        {
            state.shown = message->value;
            *visible = 0;
        }
        if (gameModeRecord()->scored == 0)
            *visible = 0;
        else if (gameMode() == kNoScoreMode)
            return;
        if (display->players[message->player].flags & kOverlayDisplayHidden)
            *visible = 0;
        else
            *visible &= 1;
        zge::zgui::setVariable(widget, sym::g_overlayTextVisible, visible);
    }
    READABLE_FUNCTION(scoreDisplayMessage, Verified);

    // ---- the balance meter's set-up

    // balanceDisplaySetUp: each player's widget found and, as the overlay
    // opens, its "Init" played; none has faded in yet.
    void balanceDisplaySetUp(OverlayDisplay *display, uint32_t opening)
    {
        memoryFill(&*g_balanceDisplay, 0, sizeof(BalanceDisplayState[kOverlayPlayers]));
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<Node> widget = zge::zgui::findNode((*g_overlayPlayerRoots)[player], sym::g_overlayBalanceName, 1); // "Overlay.DisplayBalance"
            display->players[player].widget = widget;
            if (widget != 0 && opening != 0)
                playUiNode(widget, sym::g_overlayTextInit); // "Init"
            display->players[player].flags &= ~kBalanceFadedIn;
        }
    }
    READABLE_FUNCTION(balanceDisplaySetUp, Verified);

    // ---- the health bar

    // healthDisplayFrame: each player's bar slid towards the health last
    // sent, at most 2.125 a frame: its height is the health's share (of 255)
    // of the border's height less 4, its colour between the empty and the
    // full one. While it drops, shards of that colour fall from the bar's
    // top, three for every four points lost this frame.
    void healthDisplayFrame(OverlayDisplay *display)
    {
        ee::Local<ShardTriangle> shard;
        ee::Local<Vec4> barTop;
        ee::Local<Vec4> corner;
        ee::Local<ColourValue> colourValue;
        ee::Local<Rgba[2]> colours;
        ee::Local<ee::Float> borderHeight;
        ee::Local<ee::Float> barHeight;
        ee::Local<ee::Float> barWidth;
        const ee::Float kShare = ee::Float::fromBits(0x3B808081u); // 1 / 255
        colours[0] = (*g_healthBarColours)[0];
        colours[1] = (*g_healthBarColours)[1];
        for (int32_t player = 0; player < static_cast<int32_t>(*g_playerCount); ++player)
        {
            MovingValueState &state = (*g_healthDisplay)[player];
            ee::Float step = state.wanted.value - state.shown;
            if (step == ee::Float(0.0f))
                continue;
            if (ee::Float(0.0f) < step)
                step = ee::min(step, ee::Float(2.125f));
            else
                step = ee::max(step, ee::Float(-2.125f));
            state.shown = state.shown + step;
            const ee::Ptr<Node> widget = display->players[player].widget;
            *barTop = widget->screenPosition;
            zge::zgui::copyVariable(zge::zgui::findNode(widget, sym::g_overlayBorderName, 1), sym::g_overlayTextHeight, borderHeight); // "Border", "Height"
            const ee::Float inside = *borderHeight - ee::Float(4.0f);
            const ee::Float bottom = barTop->y + *borderHeight;
            *borderHeight = inside;
            barTop->y = bottom;
            *barHeight = state.shown * (inside * kShare);
            const ee::Ptr<Node> bar = zge::zgui::findNode(widget, sym::g_overlayBarName, 1); // "Bar"
            zge::zgui::setVariable(bar, sym::g_overlayTextHeight, barHeight);
            zge::zgui::copyVariable(bar, sym::g_overlayTextWidth, barWidth); // "Width"
            barTop->y = barTop->y - *barHeight;
            const Rgba colour = *mixColours(&colours[0], &colours[1], state.shown * kShare);
            colourValue->colour = colour;
            colourValue->zero = 0;
            zge::zgui::setVariable(bar, sym::g_overlayTextTopLeftColor, colourValue);
            zge::zgui::setVariable(bar, sym::g_overlayTextTopRightColor, colourValue);
            zge::zgui::setVariable(bar, sym::g_overlayTextBottomLeftColor, colourValue);
            zge::zgui::setVariable(bar, sym::g_overlayTextBottomRightColor, colourValue);
            if (!(step < ee::Float(0.0f)))
                continue;
            shard->material = {};
            std::memcpy(&shard->colour, colour.channel, 4);
            shard->lifetime = kHealthShardLifetime;
            shard->flags = kHealthShardFlags;
            const int32_t shards = ee::toInt(-step * ee::Float(0.75f));
            for (int32_t i = 0; i < shards; ++i)
            {
                // Thrown down, and up to three quarters to either side.
                const int32_t side = static_cast<int32_t>(randomNumber() & 0xFF) - 0x80;
                shard->velocity.x = ee::toFloat(side) * ee::Float(0.005859375f);
                shard->velocity.y = ee::Float(-1.5f);
                shard->velocity.z = ee::Float(0.0f);
                *corner = *barTop;
                shard->corners[0].position = *corner;
                corner->x = corner->x - ee::Float(12.0f) + ee::Float(1.0f);
                shard->corners[1].position = *corner;
                corner->x = corner->x - ee::Float(6.0f);
                corner->z = corner->z + ee::Float(10.0f);
                shard->corners[2].position = *corner;
                spawnShardFromTriangle(shard);
            }
        }
    }
    READABLE_FUNCTION(healthDisplayFrame, Verified);

    // healthDisplayMessage: a player's health to show; sent "at once" the
    // bar starts from nothing. The widget shows unless the health is 0 sent
    // at once, or the display is hidden.
    void healthDisplayMessage(OverlayDisplay *display, const ValueMessage *message)
    {
        ee::Local<uint32_t> visible;
        ValueMessage &wanted = (*g_healthDisplay)[message->player].wanted;
        memoryCopy(&wanted, message, sizeof(ValueMessage));
        if (wanted.flags & kValueAtOnce)
            (*g_healthDisplay)[wanted.player].shown = ee::Float(0.0f);
        *visible = 0;
        if (!(wanted.value == ee::Float(0.0f)) || (wanted.flags & kValueAtOnce) == 0)
            *visible = 1;
        if (display->players[wanted.player].flags & kOverlayDisplayHidden)
            *visible = 0;
        else
            *visible &= 1;
        zge::zgui::setVariable(display->players[wanted.player].widget, sym::g_overlayTextVisible, visible);
    }
    READABLE_FUNCTION(healthDisplayMessage, Verified);

    // ---- the objective's status

    // objectiveDisplayFrame: each player's objective's status text (in the
    // level's own strings) put in the widget's "Status".
    void objectiveDisplayFrame(OverlayDisplay *display)
    {
        ee::Local<char[0x100]> status;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<Node> widget = display->players[player].widget;
            if (widget == 0)
                continue;
            const ObjectiveDisplayState &state = (*g_objectiveDisplay)[player];
            if (state.value == 0)
                continue;
            setStringTable(*g_levelStrings);
            objectiveStatusText(ee::Ptr<Objective>(state.value), status);
            setStringTable(*g_gameStrings);
            zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlayStatusName, 1), sym::g_overlayTextText, status); // "Status", "Text"
        }
    }
    READABLE_FUNCTION(objectiveDisplayFrame, Verified);

    // ---- the rewards

    // rewardDisplayFrame: each player's reward items, one appearing at a
    // time: an item waiting fades in, with the sound of its kind, once no
    // other item of the player is on its way in; any other item's phase
    // follows its widget's "State".
    void rewardDisplayFrame(OverlayDisplay *)
    {
        ee::Local<uint32_t> widgetState;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            for (uint32_t item = 0; item < kRewardItems; ++item)
            {
                RewardItemState &state = (*g_rewardDisplay)[player][item];
                const ee::Ptr<Node> widget = (*g_rewardItemWidgets)[player][item];
                if (state.phase != kRewardWaiting)
                {
                    zge::zgui::copyVariable(widget, sym::g_overlayTextState, widgetState); // "State"
                    state.phase = static_cast<uint16_t>(*widgetState);
                    continue;
                }
                uint32_t other = 0;
                for (; other < kRewardItems; ++other)
                {
                    if (other == item)
                        continue;
                    zge::zgui::copyVariable((*g_rewardItemWidgets)[player][other], sym::g_overlayTextState, widgetState);
                    if (*widgetState - kRewardAppearing < 2)
                        break;
                }
                if (other != kRewardItems)
                    continue;
                playUiNode(widget, sym::g_overlayTextFadeIn);
                // The sound events are known by number only.
                switch (state.kind)
                {
                case 0: soundEvent(0x3D, 0, 0, 0, 0); break;
                case 1: soundEvent(0x3E, 0, 0, 0, 0); break;
                case 2: break;
                case 3: soundEvent(0x3E, 0, 0, 0, 0); break;
                case 5: soundEvent(0x40, 0, 0, 0, 0); break;
                case 6: soundEvent(0x40, 0, 0, 0, 0); break;
                case 7: soundEvent(0x43, 0, 0, 0, 0); break;
                default: soundEvent(0x42, 0, 0, 0, 0); break;
                }
                state.phase = kRewardAppearing;
            }
        }
    }
    READABLE_FUNCTION(rewardDisplayFrame, Verified);

    // ---- the exit warning's message

    // exitWarningDisplayMessage: a player's warning (0xFF: none). A warning
    // fades the widget in unless it shows already or the display is hidden;
    // none stops its fades and hides it.
    void exitWarningDisplayMessage(OverlayDisplay *display, const ExitWarningMessage *message)
    {
        ee::Local<uint32_t> visible;
        const ee::Ptr<Node> widget = display->players[message->player].widget;
        if (widget == 0)
            return;
        (*g_exitWarningDisplay)[message->player].value = message->value;
        if (message->value != kNoExitWarning)
        {
            zge::zgui::copyVariable(widget, sym::g_overlayTextVisible, visible);
            if (*visible != 0)
                return;
            if (display->players[message->player].flags & kOverlayDisplayHidden)
                return;
            playUiNode(widget, sym::g_overlayTextFadeIn);
            return;
        }
        stopUiNode(widget, sym::g_overlayTextFadeIn);
        stopUiNode(widget, sym::g_overlayTextFadeOut); // "FadeOut"
        *visible = 0;
        zge::zgui::setVariable(widget, sym::g_overlayTextVisible, visible);
    }
    READABLE_FUNCTION(exitWarningDisplayMessage, Verified);

    // openOverlayDisplays: the displays' widgets loaded (the shared tree, and
    // a tree per player: the full screen's for one player, the left and the
    // right half's for two), the logo's script function registered, every
    // part hidden, and every display set up and shown.
    void openOverlayDisplays()
    {
        ee::Local<char[0x40]> name;
        ee::Local<ee::Ptr<const char>[2][2]> layouts; // by player count, then player
        for (uint32_t i = 0; i < 4; ++i)
            layouts[i / 2][i % 2] = (*g_overlayPlayerLayouts)[i / 2][i % 2];
        zge::zgui::loadZguiScript(sym::g_overlayDisplaysFile, g_zgui->root); // "overDisp_com.zsc"
        *g_overlayRoot = zge::zgui::findNode(g_zgui->root, sym::g_overlayDisplaysRootName, 1); // "UI_overDisp_com"
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            formatString(name, kOverlayScriptFileFormat, layouts[*g_playerCount - 1][player]); // "%s.zsc"
            zge::zgui::loadZguiScript(name, g_zgui->root);
            formatString(name, kOverlayScriptRootFormat, layouts[*g_playerCount - 1][player]); // "UI_%s"
            (*g_overlayPlayerRoots)[player] = zge::zgui::findNode(g_zgui->root, name.address(), 1);
        }
        zge::zgui::registerScriptFunction(sym::g_overlayTimerUpdateScriptName, sym::overlayDisplayTimerUpdate); // "bmxoverlayDisplayTimerUpdate"
        showOverlayPart(kOverlayAllParts, 0);
        for (uint32_t i = 0; i < kOverlayDisplays; ++i)
        {
            OverlayDisplay &display = (*g_overlayDisplays)[i];
            if (display.setUp)
                display.setUp(&display, 1);
            for (uint32_t player = 0; player < kOverlayPlayers; ++player)
                display.players[player].flags = kOverlayDisplayHidden;
            showOverlayDisplay(i, 1);
        }
    }
    READABLE_FUNCTION(openOverlayDisplays, Verified);

    // timerDisplayMessage: the time (in sixtieths of a second, rounded up to
    // the second) put in the timer's labels: minutes, seconds, and a colon
    // that blinks off for the first half of each second. Five seconds or
    // less (301 ticks) shows in the warning colour. Game mode 3 hides the
    // timer instead. The message's first word is the time.
    void timerDisplayMessage(OverlayDisplay *display, const uint32_t *message)
    {
        ee::Local<char[0x10]> whole;   // formatted and never shown (kept)
        ee::Local<char[0x10]> minutesText;
        ee::Local<char[0x10]> secondsText;
        ee::Local<ColourValue[2]> colours; // the warning colour, the usual one (0x20 bytes copied from g_timerColours)
        ee::Local<uint32_t> visible;
        colours[0] = (*g_timerColours)[0];
        colours[1] = (*g_timerColours)[1];
        if (gameMode() == kNoTimerMode) // 3
        {
            *visible = 0;
            zge::zgui::setVariable(display->players[0].widget, sym::g_overlayTextVisible, visible);
            return;
        }
        uint32_t ticks = *message + 59;
        uint32_t minutes = 0;
        uint32_t seconds = 0;
        while (ticks >= 3601)
        {
            ticks -= 3600;
            ++minutes;
        }
        while (ticks >= 61)
        {
            ticks -= 60;
            ++seconds;
        }
        const uint32_t blinker = ticks < 30 ? sym::g_overlayTimerBlinkOff : sym::g_overlayTimerBlinkOn; // " ", ":"
        formatString(whole, kOverlayTimerFormat, minutes, 0x20, seconds); // "%1ld%c%02ld"
        formatString(minutesText, kOverlayMinutesFormat, minutes);        // "%1ld"
        formatString(secondsText, kOverlaySecondsFormat, seconds);        // "%02ld"
        const ee::Address colour = 301 < *message ? &colours[1] : &colours[0];
        const ee::Ptr<Node> widget = display->players[0].widget;
        zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlayMinutesName, 1), sym::g_overlayTextColor, colour); // "LabelMin", "Color"
        zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlaySecondsName, 1), sym::g_overlayTextColor, colour); // "LabelSec"
        zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlayBlinkerName, 1), sym::g_overlayTextColor, colour); // "LabelBlinker"
        zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlayMinutesName, 1), sym::g_overlayTextText, minutesText);
        zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlaySecondsName, 1), sym::g_overlayTextText, secondsText);
        zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlayBlinkerName, 1), sym::g_overlayTextText, blinker);
        g_timerDisplay->time = *message;
    }
    READABLE_FUNCTION(timerDisplayMessage, Verified);

    // songInfoDisplayMessage: the song (its number names the four strings of
    // its text) shown, fading in; 0xFF empties the label.
    void songInfoDisplayMessage(OverlayDisplay *display, ee::Address message)
    {
        ee::Local<char[0x400]> text;
        memoryCopy(&*g_songInfoDisplay, message, sizeof(SongInfoDisplayState));
        const ee::Ptr<Node> widget = display->players[0].widget;
        if (widget == 0)
            return;
        const uint8_t song = g_songInfoDisplay->song;
        if (song != kNoSong)
        {
            formatString(text, kOverlaySongInfoFormat, song, song, song, song); // "$KSTR_SONGINFO_%02d_ARTIST\n..."
            stopUiNode(display->players[0].widget, sym::g_overlayTextWait);    // "Wait"
            stopUiNode(display->players[0].widget, sym::g_overlayTextFadeOut);
            playUiNode(display->players[0].widget, sym::g_overlayTextFadeIn);
        }
        else
        {
            text[0] = 0;
        }
        zge::zgui::setVariable(display->players[0].widget, sym::g_overlayTextText, text);
    }
    READABLE_FUNCTION(songInfoDisplayMessage, Verified);

    // scoreDisplayFrame: each player's score label. A mode that keeps no
    // score and the word game (mode 6) empty it. Modes 9 to 11 and 14 to 16
    // show a distance in feet and inches, mode 12 a count of tricks, mode 13
    // half turns as degrees: each rewrites the label only for a value sent
    // "at once" or a changed one, and EMPTIES it for an unchanged one (kept).
    // Every other mode counts the score up towards the value sent, a tenth
    // of what is left plus ten a frame; sent "at once" it jumps there.
    void scoreDisplayFrame(OverlayDisplay *display)
    {
        ee::Local<char[0x30]> text;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            if (display->players[player].widget == 0)
                continue;
            MovingValueState &state = (*g_scoreDisplay)[player];
            text[0] = 0;
            if (gameModeRecord()->scored != 0)
            {
                const uint8_t mode = gameMode();
                if (mode == 6)
                {
                    // nothing: the label is emptied
                }
                else if ((mode >= 9 && mode <= 11) || (mode >= 14 && mode <= 16) || mode == 12 || mode == 13)
                {
                    const ee::Float value = state.wanted.value;
                    if ((state.wanted.flags & kValueAtOnce) != 0 || !(value == state.shown))
                    {
                        state.shown = value;
                        if (mode == 12)
                            formatString(text, kOverlayTricksFormat, sce::libgcc::fptoui(value));         // "%ld tricks"
                        else if (mode == 13)
                            formatString(text, kOverlayDegreesFormat, sce::libgcc::fptoui(value) * 180u); // "%ld degrees"
                        else
                            formatFeetInches(text, value);
                    }
                }
                else
                {
                    if (state.wanted.flags & kValueAtOnce)
                    {
                        state.shown = state.wanted.value;
                        state.wanted.flags &= 0xFE;
                    }
                    else
                    {
                        const ee::Float wanted = state.wanted.value;
                        const ee::Float shown = state.shown;
                        if (!(shown < wanted))
                            continue; // the label is left as it is
                        const ee::Float step = ee::toFloat(ee::toInt((wanted - shown) * ee::Float(0.1f)));
                        const ee::Float next = shown + step + ee::Float(10.0f);
                        state.shown = next < wanted ? next : wanted;
                    }
                    formatString(text, kOverlayScoreFormat, sce::libgcc::fptoui(state.shown)); // "%ld"
                }
            }
            zge::zgui::setVariable(zge::zgui::findNode(display->players[player].widget, sym::g_overlayScoreLabelName, 1), sym::g_overlayTextText, text);
        }
    }
    READABLE_FUNCTION(scoreDisplayFrame, Verified);

    // trickDisplaySetUp: each player's widget found; as the overlay opens,
    // its four name lines and its score label found and "Init" played; as it
    // closes, "Shutdown".
    void trickDisplaySetUp(OverlayDisplay *display, uint32_t opening)
    {
        ee::Local<char[0x80]> name;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            formatString(name, kOverlayNameFormat, sym::g_overlayTrickName); // "%s", "Overlay.DisplayTrick"
            const ee::Ptr<Node> widget = zge::zgui::findNode((*g_overlayPlayerRoots)[player], name.address(), 1);
            display->players[player].widget = widget;
            if (widget == 0)
                continue;
            TrickDisplayState &state = (*g_trickDisplay)[player];
            if (opening != 0)
            {
                for (uint32_t line = 0; line < 4; ++line)
                {
                    formatString(name, kOverlayNumberedNameFormat, sym::g_overlayTrickLineName, line); // "%s%02d", "LabelName"
                    state.lines[line] = zge::zgui::findNode(widget, name.address(), 1);
                }
                formatString(name, kOverlayNameFormat, sym::g_overlayScoreLabelName); // "LabelScore"
                state.score = zge::zgui::findNode(widget, name.address(), 1);
                playUiNode(widget, sym::g_overlayTextInit);
            }
            else
            {
                playUiNode(widget, sym::g_overlayTextShutdown); // "Shutdown"
            }
        }
    }
    READABLE_FUNCTION(trickDisplaySetUp, Verified);

    // hintDisplayMessage: a player's hint (its number names the string;
    // 0xFF: none) put in the hint widget, fading in. HintDisplayState's
    // first two bytes are the message's: the player and the hint.
    void hintDisplayMessage(OverlayDisplay *, const HintMessage *message) // { uint8_t player; uint8_t hint; uint8_t _02[2]; }
    {
        ee::Local<char[0x100]> text;
        HintDisplayState &state = (*g_hintDisplay)[message->player];
        memoryCopy(&state, message, 4);
        const ee::Ptr<Node> widget = zge::zgui::findNode((*g_overlayPlayerRoots)[message->player], sym::g_overlayHintName, 1);
        if (message->hint != kNoHint) // 0xFF
        {
            stopUiNode(widget, sym::g_overlayTextFadeIn);
            stopUiNode(widget, sym::g_overlayTextFadeOut);
            playUiNode(widget, sym::g_overlayTextFadeIn);
            formatString(text, kOverlayHintFormat, state.hint & 0x7F); // "$KSTR_HINT%d"
            state.visible = 1;
        }
        else
        {
            state.visible = 0;
            text[0] = 0;
        }
        zge::zgui::setVariable(widget, sym::g_overlayTextText, text);
    }
    READABLE_FUNCTION(hintDisplayMessage, Verified);

    // objectiveDisplayMessage: a player's objective (a goal's number;
    // all ones: none) put in the objective widget: its description and hint
    // (the level's strings named "<string id>_DESC" and "_HINT") and its
    // status; the widget fades in unless the display is hidden.
    // ObjectiveDisplayState's first twelve bytes are the message's: the
    // player (+0), visible (+2), the goal (+4); +8 is the objective found.
    void objectiveDisplayMessage(OverlayDisplay *display, const ObjectiveMessage *message)
    {
        ee::Local<char[0x100]> description;
        ee::Local<char[0x100]> hint;
        ee::Local<char[0x100]> status;
        ObjectiveDisplayState &state = (*g_objectiveDisplay)[message->player];
        memoryCopy(&state, message, 0xC);
        if (display->players[message->player].widget == 0)
            return;
        setStringTable(*g_levelStrings);
        description[0] = 0;
        hint[0] = 0;
        status[0] = 0;
        if (state.goal != kNoGoal) // 0xFFFFFFFF
        {
            // goalNameHash is called with the goal still in the second argument register from the load above.
            state.value = calls::objectiveNamed(objectives::goalNameHash(state.player, state.goal));
            const ee::Ptr<ObjectiveInfo> info = objectiveInfo(ee::Ptr<Objective>(state.value));
            if (info->stringId[0] != 0)
            {
                formatString(description, kOverlayDescriptionFormat, info->stringId); // "%s_DESC"
                formatString(hint, kOverlayHintTextFormat, info->stringId);            // "%s_HINT"
            }
            else
            {
                formatString(description, kOverlayMissingStringFormat, info->name);   // "Missing STRINGID for \"%s\""
            }
            objectiveStatusText(ee::Ptr<Objective>(state.value), status);
            state.visible = 1;
        }
        else
        {
            state.visible = 0;
            state.value = 0;
        }
        zge::zgui::setVariable(display->players[message->player].widget, sym::g_overlayTextText, ee::Address(stringTableText(nameHash(description))));
        const ee::Ptr<Node> hintLabel = zge::zgui::findNode(display->players[message->player].widget, sym::g_overlayHintLabelName, 1); // "Hint"
        zge::zgui::setVariable(hintLabel, sym::g_overlayTextText, ee::Address(stringTableText(nameHash(hint))));
        zge::zgui::setVariable(zge::zgui::findNode(display->players[message->player].widget, sym::g_overlayStatusName, 1), sym::g_overlayTextText, status);
        if ((display->players[message->player].flags & kOverlayDisplayHidden) == 0)
        {
            stopUiNode(display->players[message->player].widget, sym::g_overlayTextFadeIn);
            stopUiNode(display->players[message->player].widget, sym::g_overlayTextSwitch1); // "Switch1"
            stopUiNode(display->players[message->player].widget, sym::g_overlayTextSwitch2); // "Switch2"
            stopUiNode(display->players[message->player].widget, sym::g_overlayTextFadeOut);
            if (state.value != 0)
                playUiNode(display->players[message->player].widget, sym::g_overlayTextFadeIn);
        }
        setStringTable(*g_gameStrings);
    }
    READABLE_FUNCTION(objectiveDisplayMessage, Verified);

    // rewardDisplaySetUp: each player's widget found (not tested); as the
    // overlay opens its six items are found and emptied (no kind) and "Init"
    // played; as it closes, "Shutdown".
    void rewardDisplaySetUp(OverlayDisplay *display, uint32_t opening)
    {
        ee::Local<char[0x40]> name;
        memoryFill(&*g_rewardDisplay, 0, sizeof(RewardItemState[kOverlayPlayers][kRewardItems]));
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<Node> widget = zge::zgui::findNode((*g_overlayPlayerRoots)[player], sym::g_overlayRewardName, 1); // "Overlay.DisplayReward"
            display->players[player].widget = widget;
            if (opening != 0)
            {
                for (uint32_t item = 0; item < kRewardItems; ++item)
                {
                    formatString(name, kOverlayRewardItemFormat, item); // "Item%02d"
                    (*g_rewardItemWidgets)[player][item] = zge::zgui::findNode(display->players[player].widget, name.address(), 1);
                    (*g_rewardDisplay)[player][item].phase = 0;
                    (*g_rewardDisplay)[player][item].kind = kNoReward; // 0xFF
                }
                playUiNode(display->players[player].widget, sym::g_overlayTextInit);
            }
            else
            {
                playUiNode(widget, sym::g_overlayTextShutdown);
            }
        }
    }
    READABLE_FUNCTION(rewardDisplaySetUp, Verified);

    // ---- the trick display's messages

    namespace
    {
        using rider::ScoreBlock;
        struct TrickMessage
        {
            uint8_t player;            // +0x00
            uint8_t kind;              // +0x01 TrickMessageKind
            uint8_t _02[2];
            ee::Ptr<ScoreBlock> score; // +0x04 the rider's score block
        };
        enum TrickMessageKind : uint8_t
        {
            kTrickStart = 0,     // "Init": every line hidden
            kTrickLineAdded = 1, // the lines move up and the next one fades in
            kTrickLineText = 2,  // the newest line's text, and the points
            kTrickLineOut = 3,   // the newest line fades out
            kTrickScoreOut = 4,  // the points fade out
            kTrickCrash = 5,     // the newest line and the points play "Crash"
        };
        constexpr uint32_t kTrickLines = 4;
        constexpr uint32_t kTrickLineWidth = 24; // characters a line of the display holds
        constexpr uint32_t kOverlayTrickJoinFormat = sym::bmx::g_overlayTrickJoinFormat; // "#RGB{192 192 192}TO#RGB{255 153 0} "
        constexpr uint32_t kOverlayPointsFormat = sym::bmx::g_overlayPointsFormat;       // "%ld points"
    }

    // trickDisplayMessage: what the rider's scoring tells the trick
    // display (TrickMessageKind). The display has four lines used in
    // turn: `line` is the newest one, `next` the one the next trick takes.
    // The points show only in the game modes that score by points.
    void trickDisplayMessage(OverlayDisplay *display, const TrickMessage *message)
    {
        ee::Local<char[0x200]> text;
        ee::Local<char[0x200]> wrapped;
        ee::Local<uint32_t> visible;
        const ee::Ptr<ScoreBlock> score = message->score;
        TrickDisplayState &state = (*g_trickDisplay)[message->player];
        if (display->players[message->player].widget == 0)
            return;
        const ee::Ptr<Node> scoreLabel = state.score;
        switch (message->kind)
        {
        case kTrickStart:
            playUiNode(display->players[message->player].widget, sym::g_overlayTextInit);
            state.next = 1;
            state.line = 0;
            *visible = 0;
            for (uint32_t line = 0; line < kTrickLines; ++line)
                zge::zgui::setVariable(state.lines[line], sym::g_overlayTextVisible, visible);
            break;

        case kTrickLineAdded:
        {
            if (score->lineCount == 0)
                break;
            if (score->lineCount >= 2)
            {
                // Every line showing moves up, from the newest back to the
                // one about to be taken; the newest fades out.
                uint32_t line = state.line;
                do
                {
                    const ee::Ptr<Node> widget = state.lines[line];
                    if (widget != 0)
                    {
                        zge::zgui::copyVariable(widget, sym::g_overlayTextVisible, visible);
                        if (*visible != 0)
                            playUiNode(widget, sym::g_overlayTextMove); // "Move"
                    }
                    line = line == 0 ? kTrickLines - 1 : line - 1;
                } while (line != state.next);
                const ee::Ptr<Node> newest = state.lines[state.line];
                if (newest != 0)
                    playUiNode(newest, sym::g_overlayTextFadeOut);
            }
            const ee::Ptr<Node> taken = state.lines[state.next];
            if (taken != 0)
            {
                playUiNode(taken, sym::g_overlayTextFadeIn);
                state.line = state.next;
                state.next = static_cast<uint8_t>((state.next + 1u) % kTrickLines);
            }
            if (score->lineCount == 1)
            {
                stopUiNode(scoreLabel, sym::g_overlayTextFadeOut);
                stopUiNode(scoreLabel, sym::g_overlayTextFadeOut2); // "FadeOut2"
                playUiNode(scoreLabel, sym::g_overlayTextFadeIn);
            }
            else
            {
                playUiNode(scoreLabel, sym::g_overlayTextFlashOn); // "FlashOn"
            }
            break;
        }

        case kTrickLineText:
        {
            if (score->lineCount == 0)
                break;
            const ee::Ptr<Node> widget = state.lines[state.line];
            if (widget == 0)
                break;
            // The newest trick line's name in capitals, broken into lines;
            // from the second line on it follows a grey "TO".
            text[0] = 0;
            trickText(text, &score->lines[score->lineCount - 1], 1);
            stringToUpper(text);
            wrapText(wrapped, text, kTrickLineWidth);
            if (score->lineCount >= 2)
            {
                // (The format takes nothing; the line's number is passed all the same.)
                formatString(text, kOverlayTrickJoinFormat, state.line);
                appendString(text, wrapped);
                zge::zgui::setVariable(widget, sym::g_overlayTextText, text);
            }
            else
            {
                zge::zgui::setVariable(widget, sym::g_overlayTextText, wrapped);
            }
            text[0] = 0;
            if (gameModeRecord()->scored != 0)
            {
                const uint32_t mode = gameMode();
                if (mode <= 2 || mode == 4 || mode == 5 || mode == 7 || mode > 16)
                    formatString(text, kOverlayPointsFormat, static_cast<int32_t>(score->linePoints));
            }
            zge::zgui::setVariable(scoreLabel, sym::g_overlayTextText, text);
            break;
        }

        case kTrickLineOut:
        {
            if (score->lineCount == 0)
                break;
            const ee::Ptr<Node> widget = state.lines[state.line];
            if (widget != 0)
                playUiNode(widget, sym::g_overlayTextFadeOut);
            break;
        }

        case kTrickScoreOut:
            playUiNode(scoreLabel, sym::g_overlayTextFadeOut);
            break;

        case kTrickCrash:
        {
            const ee::Ptr<Node> widget = state.lines[state.line];
            if (widget != 0)
                playUiNode(widget, sym::g_overlayTextCrash); // "Crash"
            playUiNode(scoreLabel, sym::g_overlayTextCrash);
            break;
        }

        default:
            break;
        }
    }
    READABLE_FUNCTION(trickDisplayMessage, Verified);

    // ---- the balance meter's messages

    namespace
    {
        // The meter's border by the way it lies ("H", "V"), the names of
        // its four corners, and the colour fields' colours from a level
        // meter to a tipped one.
        inline ee::Global<ee::Ptr<const char>[2], sym::g_balanceSideNames> g_balanceSideNames;
        inline ee::Global<ee::Ptr<const char>[4], sym::g_overlayCornerNames> g_overlayCornerNames; // "Corner_TL", TR, BL, BR
        constexpr uint32_t kBalanceColours = 5;
        inline ee::Global<ColourValue[kBalanceColours], sym::g_balanceColours> g_balanceColours;
        constexpr uint32_t kOverlayBorderFormat = sym::bmx::g_overlayBorderFormat;         // "Border.%s.%s"
        constexpr uint32_t kOverlayColorFieldFormat = sym::bmx::g_overlayColorFieldFormat; // "ColorField.%s"
        constexpr ee::Float kLeastBalanceField = ee::Float(0.2f);
    }

    // balanceDisplayMessage: the rider's balance for a player's meter
    // (BalanceMessageFlags). A changed balance (or a fade in) is drawn: the
    // indicator goes the balance's part of the border's size (times the
    // widget's "RadiusScalar") along the meter, and the four colour fields,
    // one from the middle towards each corner, are sized by how far the
    // balance is out (a fifth at least) and coloured by it; only the fields
    // on the balance's side show.
    void balanceDisplayMessage(OverlayDisplay *display, const BalanceMessage *message)
    {
        ee::Local<char[0x40]> name;
        ee::Local<Vec4> position;
        ee::Local<ee::Ptr<const char>[2]> sides;
        ee::Local<ColourValue[kBalanceColours]> colours;
        ee::Local<uint32_t> type;
        ee::Local<ee::Float> width;
        ee::Local<ee::Float> height;
        ee::Local<ee::Float> radiusScalar;
        ee::Local<ee::Float> fieldWidth;
        ee::Local<ee::Float> fieldHeight;
        ee::Local<uint32_t> visible;
        sides[0] = (*g_balanceSideNames)[0];
        sides[1] = (*g_balanceSideNames)[1];
        for (uint32_t i = 0; i < kBalanceColours; ++i)
            colours[i] = (*g_balanceColours)[i];
        const ee::Ptr<Node> widget = display->players[message->player].widget;
        const uint16_t flags = message->flags;
        if (flags & kBalanceInit)
        {
            playUiNode(widget, sym::g_overlayTextInit);
            display->players[message->player].flags &= ~kBalanceFadedIn;
            return;
        }
        if (flags & kBalanceFadeIn)
        {
            uint32_t &kept = display->players[message->player].flags;
            if (flags & kBalanceMessageVertical)
            {
                kept |= kBalanceVertical;
                *type = 1;
            }
            else
            {
                kept &= ~kBalanceVertical;
                *type = 0;
            }
            zge::zgui::setVariable(widget, sym::g_overlayTextType, type); // "Type"
            if (message->flags & kBalanceMessageMirrored)
                display->players[message->player].flags |= kBalanceMirrored;
            else
                display->players[message->player].flags &= ~kBalanceMirrored;
            display->players[message->player].flags |= kBalanceFadedIn;
            playUiNode(widget, sym::g_overlayTextFadeIn);
        }
        else if (flags & kBalanceFadeOut)
        {
            if ((display->players[message->player].flags & kBalanceFadedIn) == 0)
                return;
            playUiNode(widget, sym::g_overlayTextFadeOut);
            display->players[message->player].flags &= ~kBalanceFadedIn;
            return;
        }

        BalanceDisplayState &state = (*g_balanceDisplay)[message->player];
        if (message->value == state.shown.value && (message->flags & kBalanceFadeIn) == 0)
            return;

        const bool vertical = (display->players[message->player].flags & kBalanceVertical) != 0;
        formatString(name, kOverlayBorderFormat, sides[vertical ? 1 : 0], (*g_overlayCornerNames)[0]);
        const ee::Ptr<Node> border = zge::zgui::findNode(widget, name.address(), 1);
        zge::zgui::copyVariable(border, sym::g_overlayTextWidth, width);
        zge::zgui::copyVariable(border, sym::g_overlayTextHeight, height);
        zge::zgui::copyVariable(widget, sym::g_overlayTextRadiusScalar, radiusScalar); // "RadiusScalar"
        const ee::Ptr<Node> indicator = zge::zgui::findNode(widget, sym::g_overlayIndicatorName, 1); // "Indicator"
        *position = kZero;
        if (display->players[message->player].flags & kBalanceVertical)
            position->y = message->value * (*height * *radiusScalar);
        else
            position->x = message->value * (*width * *radiusScalar);
        if (display->players[message->player].flags & kBalanceMirrored)
            vec3Negate(&position->xyz(), &position->xyz());
        zge::zgui::setVariable(indicator, sym::g_overlayTextPosition, position);

        const ee::Float out = ee::abs(message->value) <= ee::Float(1.0f) ? ee::abs(message->value) : ee::Float(1.0f);
        const ee::Float size = ee::max(out, kLeastBalanceField);
        *fieldWidth = ee::toFloat(ee::toInt(size * (*width * *radiusScalar) + ee::Float(0.5f)));
        *fieldHeight = ee::toFloat(ee::toInt(size * (*height * *radiusScalar) + ee::Float(0.5f)));
        for (uint32_t corner = 0; corner < 4; ++corner)
        {
            formatString(name, kOverlayColorFieldFormat, (*g_overlayCornerNames)[corner]);
            const ee::Ptr<Node> field = zge::zgui::findNode(widget, name.address(), 1);
            // The left fields (and the top ones) start their own size before the middle.
            position->x = (corner & 1) ? ee::Float(0.0f) : -*fieldWidth;
            position->y = (corner & 2) ? ee::Float(0.0f) : -*fieldHeight;
            const ColourValue *colour = &colours[ee::toInt(out * ee::Float(4.0f))];
            // A standing meter shows its top or bottom pair, a lying one
            // its left or right pair: by the balance's sign.
            const uint32_t far = (display->players[message->player].flags & kBalanceVertical) ? ((corner >> 1) ^ 1) & 1 : (corner ^ 1) & 1;
            *visible = (ee::Float(0.0f) < message->value ? 1u : 0u) ^ far;
            if (display->players[message->player].flags & kBalanceMirrored)
                *visible ^= 1;
            zge::zgui::setVariable(field, sym::g_overlayTextPosition, position);
            zge::zgui::setVariable(field, sym::g_overlayTextWidth, fieldWidth);
            zge::zgui::setVariable(field, sym::g_overlayTextHeight, fieldHeight);
            zge::zgui::setVariable(field, sym::g_overlayTextTopLeftColor, colour);
            zge::zgui::setVariable(field, sym::g_overlayTextTopRightColor, colour);
            zge::zgui::setVariable(field, sym::g_overlayTextBottomLeftColor, colour);
            zge::zgui::setVariable(field, sym::g_overlayTextBottomRightColor, colour);
            zge::zgui::setVariable(field, sym::g_overlayTextVisible, visible);
        }
        memoryCopy(&state, message, sizeof(BalanceMessage));
    }
    READABLE_FUNCTION(balanceDisplayMessage, Verified);

    // ---- the reward display's messages

    namespace
    {
        struct RewardMessage
        {
            uint8_t player; // +0x00
            uint8_t kind;   // +0x01 kNoReward: the player's items are cleared
            uint8_t _02[2];
            uint32_t value; // +0x04 by the kind: a text, an objective's record, a number (a float's bits)
        };
        static_assert(sizeof(RewardMessage) == 0x08);
        // The kinds with a text of their own; the others show the game's
        // string "$KSTR_REWARD<kind>", and kinds 19 to 26 add a distance.
        enum RewardKind : uint8_t
        {
            kRewardText = 0,              // the message's own text
            kRewardGoalDone = 1,          // the objective's "<string id>_COMPLETE" and the level's goals done
            kRewardGoalTimeOut = 2,       // the objective's "<string id>_TIMEOUT"
            kRewardNamed = 4,             // what the objectives' reward text says of the value
            kRewardGapDone = 5,           // this and the next: the kind's string with the level's gaps done
            kRewardGapDone2 = 6,
            kRewardCountDash = 7,         // "<number>-" and the kind's string
            kRewardCount = 8,             // "<number> " and the kind's string
            kRewardFall = 11,             // the fall's string by its number, in red
            kRewardFirstDistance = 19,
            kRewardLastDistance = 26,
        };
        // What an item of each kind looks like: the character its widget
        // shows, how long it stays (the "FadeOut" action's wait) and its colour.
        struct RewardLook
        {
            int8_t character;   // +0x00
            uint8_t _01[3];
            ee::Float seconds;  // +0x04
            Rgba colour;        // +0x08
        };
        static_assert(sizeof(RewardLook) == 0x0C);
        constexpr uint32_t kRewardKinds = 27;
        inline ee::Global<RewardLook[kRewardKinds], sym::g_rewardLooks> g_rewardLooks;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        constexpr uint64_t kAnimationNode = static_cast<uint64_t>(NodeKind::Animation);
        // The game's strings used here, by their names' hashes: a goal done
        // whose level has no text for it, and "<done> of <goals>" (it takes the two counts).
        constexpr uint32_t kStringGoalDone = zge::nameHashOf("KSTRSP_OBJCTV_GENERIC_COMPLETE");
        constexpr uint32_t kStringGoalsCount = 0x07337175;
        constexpr uint32_t kOverlayCompleteFormat = sym::bmx::g_overlayCompleteFormat;       // "%s_COMPLETE"
        constexpr uint32_t kOverlayTwoLinesFormat = sym::bmx::g_overlayTwoLinesFormat;       // "%s\n%s"
        constexpr uint32_t kOverlayTimeoutFormat = sym::bmx::g_overlayTimeoutFormat;         // "%s_TIMEOUT"
        constexpr uint32_t kOverlayGenericTimeout = sym::bmx::g_overlayGenericTimeout;       // "$KSTRSP_OBJCTV_GENERIC_TIMEOUT"
        constexpr uint32_t kOverlayRewardFormat = sym::bmx::g_overlayRewardFormat;           // "$KSTR_REWARD%02d"
        constexpr uint32_t kOverlayRewardNameFormat = sym::bmx::g_overlayRewardNameFormat;   // "KSTR_REWARD%02d"
        constexpr uint32_t kOverlayRewardCountDashFormat = sym::bmx::g_overlayRewardCountDashFormat; // "%d-$KSTR_REWARD%02d"
        constexpr uint32_t kOverlayRewardCountFormat = sym::bmx::g_overlayRewardCountFormat; // "%d $KSTR_REWARD%02d"
        constexpr uint32_t kOverlayFallFormat = sym::bmx::g_overlayFallFormat;               // "#RGB{200 32 32}$KSTR_FALL_%02d"
        constexpr uint32_t kOverlaySeparator = sym::bmx::g_overlaySeparator;                 // " : "
        constexpr uint32_t kOverlayCharacterFormat = sym::bmx::g_overlayCharacterFormat;     // "%c"
        // An animation of a widget, found by its name.
        ee::Ptr<Animation> animationNamed(ee::Ptr<Node> widget, uint32_t name)
        {
            return ee::Ptr<Animation>(ee::Address(zge::zgui::findNode(widget, name, kAnimationNode)));
        }
    }

    // rewardDisplayMessage: a reward (or a notice like one) for a player's
    // list: the first of the six items not in use takes it, with its text
    // by kind, its colour and character, and how long it stays, and waits
    // to appear (rewardDisplayFrame lets one in at a time). With all six
    // in use the message is dropped. kNoReward hides the six and frees them.
    void rewardDisplayMessage(OverlayDisplay *, const RewardMessage *message)
    {
        ee::Local<char[0x400]> text;
        ee::Local<char[0x400]> counts;
        ee::Local<ColourValue> colour;
        ee::Local<RewardLook[kRewardKinds]> looks;
        ee::Local<uint32_t> visible;
        for (uint32_t i = 0; i < kRewardKinds; ++i)
            looks[i] = (*g_rewardLooks)[i];
        const uint32_t player = message->player;
        if (message->kind == kNoReward)
        {
            *visible = 0;
            for (uint32_t item = 0; item < kRewardItems; ++item)
            {
                zge::zgui::setVariable((*g_rewardItemWidgets)[message->player][item], sym::g_overlayTextVisible, visible);
                (*g_rewardDisplay)[message->player][item].phase = 0;
            }
            return;
        }
        uint32_t item = 0;
        while ((*g_rewardDisplay)[player][item].phase != 0)
        {
            if (++item >= kRewardItems)
                return;
        }
        RewardItemState &state = (*g_rewardDisplay)[player][item];
        const ee::Ptr<Node> widget = (*g_rewardItemWidgets)[player][item];
        memoryCopy(&state, message, sizeof(RewardMessage));
        const uint32_t kind = state.kind;
        switch (kind)
        {
        case kRewardText:
            stringCopy(text, message->value);
            break;
        case kRewardGoalDone:
        {
            // Both objective kinds ask for the level's goals before telling
            // which they are; this one asks again and keeps the answer.
            playerLevelGoals(message->player, *g_currentLevel);
            const ee::Ptr<ObjectiveInfo> objective(message->value);
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(message->player, *g_currentLevel);
            setStringTable(*g_levelStrings);
            const char *done = nullptr;
            if (objective->stringId[0] != 0)
            {
                formatString(text, kOverlayCompleteFormat, objective->stringId);
                done = stringTableText(nameHash(text));
            }
            if (done == nullptr)
                done = stringTableText(kStringGoalDone);
            setStringTable(*g_gameStrings);
            formatString(counts, ee::Address(stringTableText(kStringGoalsCount)), goals->goalsDone, goals->goals);
            formatString(text, kOverlayTwoLinesFormat, ee::Address(done), counts.address());
            break;
        }
        case kRewardGoalTimeOut:
        {
            playerLevelGoals(message->player, *g_currentLevel);
            const ee::Ptr<ObjectiveInfo> objective(message->value);
            setStringTable(*g_levelStrings);
            formatString(text, kOverlayTimeoutFormat, objective->stringId);
            const char *timeOut = stringTableText(nameHash(text));
            if (timeOut != nullptr)
                stringCopy(text, ee::Address(timeOut));
            else
                formatString(text, kOverlayGenericTimeout);
            setStringTable(*g_gameStrings);
            break;
        }
        case kRewardNamed:
            if (objectives::rewardText(message->player, ee::Ptr<objectives::Reward>(message->value), text) == 0)
                return;
            break;
        case kRewardGapDone:
        case kRewardGapDone2:
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(message->player, *g_currentLevel);
            formatString(text, kOverlayRewardNameFormat, state.kind);
            formatString(text, ee::Address(stringTableText(nameHash(text))), goals->gapsDone, goals->gaps);
            break;
        }
        case kRewardCountDash:
            formatString(text, kOverlayRewardCountDashFormat, sce::libgcc::fptoui(ee::Float::fromBits(state.value)), kind);
            break;
        case kRewardCount:
            formatString(text, kOverlayRewardCountFormat, sce::libgcc::fptoui(ee::Float::fromBits(state.value)), kind);
            break;
        case kRewardFall:
            formatString(text, kOverlayFallFormat, sce::libgcc::fptoui(ee::Float::fromBits(state.value)));
            break;
        default:
            formatString(text, kOverlayRewardFormat, kind);
            break;
        }
        if (state.kind >= kRewardFirstDistance && state.kind <= kRewardLastDistance)
        {
            formatString(text.address() + stringLength(text), kOverlaySeparator);
            formatFeetInches(text.address() + stringLength(text), ee::Float::fromBits(state.value));
        }
        zge::zgui::setVariable(zge::zgui::findNode(widget, sym::g_overlayDescName, 1), sym::g_overlayTextText, text); // "Desc"
        colour->colour = looks[state.kind].colour;
        colour->zero = 0;
        zge::zgui::setVariable(widget, sym::g_overlayTextColor, colour);
        formatString(text, kOverlayCharacterFormat, static_cast<int32_t>(looks[state.kind].character));
        zge::zgui::setVariable(widget, sym::g_overlayTextText, text);
        // The item stays its kind's time before it fades out.
        animationNamed(widget, sym::g_overlayTextFadeOut)->delay = looks[state.kind].seconds;
        state.phase = kRewardWaiting;
    }
    READABLE_FUNCTION(rewardDisplayMessage, Verified);
}
