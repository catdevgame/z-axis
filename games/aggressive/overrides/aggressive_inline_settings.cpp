// Aggressive Inline (SLUS-20327): PC settings in the game's own menus.
//
// See docs/SETTINGS.md. The game's UI is UIBuilder script text in
// ASSETS/UILIB.ZAL; when the IOP opens that file the runtime serves a copy
// (runtime/disc_patch.h) in which FrontEnd.zsc (and InGame.zsc) gain a
// Settings screen built from the game's own widgets, fonts and materials,
// and the Options menu's Controller item opens it. The screen's rows come
// from the settings registry (runtime/settings_registry.h) through script
// natives registered next to the game's own (func_170820), so the app's
// menu bar and these pages share one model.

#include "aggressive_inline_gameplay.h"
#include "sym/game_overrides/aggressive_inline_settings.cpp.h"
#include "aggressive_inline_language.h"
#include "aggressive_inline_photo.h"
#include "aggressive_inline_restart.h"
#include "aggressive_inline_settings_open.h"
#include "aggressive_inline_settings_room.h"
#include "game_overrides.h"
#include "host_backend.h"
#include "machine.h"
#include "runtime/disc_patch.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/settings_registry.h"
#include "runtime/zaxis_text.h"
#include "runtime/zaxis_ui.h"
#include "runtime/boot_config.h"
#include "runtime/session.h"
#include "overrides/zge_settings.h"
#include "overrides/zge_settings_open.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2_aggressive_inline
{
    // Button prompts for the device in use (aggressive_inline_prompts.cpp;
    // its art is read from this patch's result).
    void installPrompts(PS2Machine &runtime);

    namespace
    {
        namespace st = zx::settings;

        // ---- the game's script engine ---------------------------------------

        constexpr uint32_t kRegisterNative = sym::registerScriptFunction; // (name, fn): hash, copy name, insert
        constexpr uint32_t kRunScript = sym::runScript;      // (widget, "script text", 0)
        // The first native of each of the game's two sets: ours are
        // registered right after it (again on each registration pass).
        const char *const kFirstGameNatives[] = {"inlineMenuSync", "inlinePauseModeExit"};

        struct Engine
        {
            PS2Machine::RecompiledFunction registerNative = nullptr;
        };
        Engine &engine() { return zx::session::state<Engine>(); }

        // The script engine as host code reaches it, and the page's rows
        // and keys: zge_settings.cpp, shared with the other two games.
        using zge_host::settings::getString;
        using zge_host::settings::literal;
        using zge_host::settings::putString;
        using zge_host::settings::readWord;
        using zge_host::settings::runScript;

        uint32_t firstArg(uint8_t *rdram, R5900Context *ctx) { return zge_host::settings::firstArgument(rdram, ctx); }

        // ---- the Settings screen --------------------------------------------

        // Where the rows are: the front end's screen (its Dialog frame:
        // origin at the left middle) or the pause menu's page (the pause
        // frame: origin at the top middle).
        using Layout = zge_host::settings::Layout;
        constexpr Layout kFrontEndLayout{100.0f, 536.0f, -128.0f, 28.8f, 0.85f, 44.0f, "1.000000 1.000000 1.000000"};
        constexpr Layout kPauseLayout{-224.0f, 224.0f, 106.0f, 24.0f, 0.8f, 44.0f, "0.850000 0.850000 1.000000"};

        std::string translatePortText(const std::string &english) { return aggressive_inline::language::translate(english); }

        // The page in AI's look: the trick font's labels in the menus' blue
        // and gold, white when selected.
        const zge_host::settings::Style kStyle{
            .visibleRows = 9,
            .frontEnd = kFrontEndLayout,
            .pause = kPauseLayout,
            .pauseDialog = "PcPause",
            .findDialog = nullptr, // the scripts pass the dialog
            .highlightAt = "SET Highlight.Position.y {%.1f}",
            .nameColor = "102 164 223 255",
            .valueColor = "176 142 21 255",
            .selectedColor = "234 234 234 255",
            .headingColor = "176 142 21 255", // the section title's gold
            .translate = &translatePortText,
            .rowsType = "guiGroup",
            .rowsProperties = "OnCreate\t\"SET Visible {0}\"\nVisible\t0\n",
            .rowCharset = "CommonFonts.fnt_ovr_trick_noborder",
            .rowScale = "0.750000 0.750000 1.000000",
            .rowLabelTail = "Angle\t0\nMonospace\t0\n",
        };
        using zge_host::settings::withRows;

        // Front end Movie.Start: the logo and intro movies, or straight to
        // the title screen (where the movies end) with Skip Intro Movies.
        void introNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t movie = firstArg(rdram, ctx);
            if (zge_host::settings::tracing())
                std::fprintf(stderr, "[settings] intro argv=0x%x movie=0x%x\n", getRegU32(ctx, 4), movie);
            if (st::value("game.skip_intro_movies", 0) != 0)
            {
                std::cerr << "[aggressive-inline] intro movies skipped (settings)" << std::endl;
                runScript(rdram, ctx, runtime, movie,
                          {"SET TopLeftColor {255 255 255 255}", "SET TopRightColor TopLeftColor",
                           "SET BottomLeftColor TopLeftColor", "SET BottomRightColor TopLeftColor",
                           "PLAY ReturnToMainMenu"});
            }
            else
            {
                runScript(rdram, ctx, runtime, movie, {"PLAY StartAKAMovie"});
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // Cmd+, / Settings...: in a level by the engine's own path
        // (zge_settings_open.h: the play frame pauses as Start does, and the
        // pause menu's Show opens its Settings page; the game's side is
        // aggressive_inline_settings_open.cpp); in the front end Options and
        // its Settings page, from the main menu or Options.
        // Which of the front end's menus has the focus (their OnGetFocus /
        // OnLostFocus), with the group the handlers pass (".").
        enum class FrontEndAt
        {
            Elsewhere,
            MainMenu,
            Options,
            // Player or level select: Back leads towards the main menu.
            Back,
        };
        struct Requests
        {
            std::atomic<int> frontEndAt{0};
            std::atomic<uint32_t> frontEndWidget{0};
            std::atomic<uint64_t> frontEndRequest{0}; // vsync deadline of a Settings... request
            std::atomic<bool> settingsAfterOptions{false};
            std::atomic<uint64_t> frontEndBackAt{0}; // vsync of the last Back pressed for a request
            PS2Machine *runtime = nullptr;
        };
        Requests &requests() { return zx::session::state<Requests>(); }

        uint64_t nowVsync()
        {
            return requests().runtime ? requests().runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
        }

        // Settings... asked outside a level (zge_settings_open's handler
        // decides that): the front end, from the main menu or Options
        // (frontEndPollNative), or player or level select (Back to the main
        // menu first). False where none of them has the focus (the intro
        // movies, the title, another Options screen): the request is not
        // kept for later, and the player is told Settings are in Options.
        bool frontEndSettingsRequested()
        {
            if (static_cast<FrontEndAt>(requests().frontEndAt.load()) == FrontEndAt::Elsewhere)
                return false;
            requests().frontEndRequest = nowVsync() + 240u;
            std::cerr << "[aggressive-inline] settings: opening Options > Settings" << std::endl;
            return true;
        }

        // The pause menu the Settings page was opened from (the pause
        // frame's Pause group, or the Tutorial's lesson list): Back returns
        // there.
        struct PauseMenu
        {
            std::atomic<uint32_t> widget{0};
        };
        PauseMenu &pauseMenuState() { return zx::session::state<PauseMenu>(); }

        // Pause.Show (or Tutorial.Show) ended: the pause menu is up. A
        // Settings... pause gets its page in the menu's place.
        void pauseShownNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t pause = firstArg(rdram, ctx);
            pauseMenuState().widget = pause;
            zge_host::settings_open::pauseMenuShown(rdram, ctx, runtime, pause);
            ctx->pc = getRegU32(ctx, 31);
        }

        // PcPause.Hide ended: back to the menu it was opened from.
        void pauseBackNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t pause = pauseMenuState().widget.load();
            if (pause != 0u)
                runScript(rdram, ctx, runtime, pause, {"PLAY Show"});
            else
                runScript(rdram, ctx, runtime, firstArg(rdram, ctx), {"PLAY .Pause.Show"});
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- the front end: Quit to Desktop, Settings... from the main menu --


        void quitToDesktopNative(uint8_t *, R5900Context *ctx, PS2Machine *)
        {
            // In the collection, back to its menu; a single-game app quits
            // (ps2_host::requestExitToMenu).
            std::cerr << "[aggressive-inline] quit to desktop (menu)" << std::endl;
            ps2_host::requestExitToMenu();
            ctx->pc = getRegU32(ctx, 31);
        }

        void mainMenuFocusNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t main = firstArg(rdram, ctx);
            requests().frontEndAt = static_cast<int>(FrontEndAt::MainMenu);
            requests().frontEndWidget = main;
            // The poll for Settings... runs while the front end is up.
            // The footer's Back prompt says Quit here.
            runScript(rdram, ctx, runtime, main,
                      {"PLAY PcPoll", "SET PcSniffer.Active {1}", "SET .Dialog.Tri.Label.Text {\"Quit\"}",
                       "PLAY .Dialog.Tri.MoveIn"});
            ctx->pc = getRegU32(ctx, 31);
        }

        void mainMenuBlurNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            requests().frontEndAt = static_cast<int>(FrontEndAt::Elsewhere);
            runScript(rdram, ctx, runtime, firstArg(rdram, ctx),
                      {"SET PcSniffer.Active {0}", "SET .Dialog.Tri.Label.Text {\"Back\"}"});
            ctx->pc = getRegU32(ctx, 31);
        }

        void optionsFocusNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t options = firstArg(rdram, ctx);
            requests().frontEndAt = static_cast<int>(FrontEndAt::Options);
            requests().frontEndWidget = options;
            if (requests().settingsAfterOptions.exchange(false))
                runScript(rdram, ctx, runtime, options, {"PLAY GoToPcSettings"});
            ctx->pc = getRegU32(ctx, 31);
        }

        void backFocusNative(uint8_t *, R5900Context *ctx, PS2Machine *)
        {
            requests().frontEndAt = static_cast<int>(FrontEndAt::Back);
            ctx->pc = getRegU32(ctx, 31);
        }

        void menuBlurNative(uint8_t *, R5900Context *ctx, PS2Machine *)
        {
            requests().frontEndAt = static_cast<int>(FrontEndAt::Elsewhere);
            ctx->pc = getRegU32(ctx, 31);
        }

        // Start on the main menu: Escape asks to quit, Enter (or a
        // gamepad's Start) selects.
        void mainMenuStartNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t main = firstArg(rdram, ctx);
            constexpr int kEscape = 256;
            if (PSPadBackend::keyPressedWithin(kEscape, 0.5))
                runScript(rdram, ctx, runtime, main, {"PLAY PcQuit.Show"});
            else
                PSPadBackend::pressButton(ps2_host_input::kCross, 2);
            ctx->pc = getRegU32(ctx, 31);
        }

        // Every frame while the front end is up: a pending Settings...
        // request opens Options, then its Settings page.
        void frontEndPollNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint64_t until = requests().frontEndRequest.load();
            if (until != 0u)
            {
                const FrontEndAt at = static_cast<FrontEndAt>(requests().frontEndAt.load());
                if (nowVsync() > until)
                {
                    requests().frontEndRequest = 0;
                }
                else if (at == FrontEndAt::MainMenu)
                {
                    requests().frontEndRequest = 0;
                    requests().settingsAfterOptions = true;
                    runScript(rdram, ctx, runtime, requests().frontEndWidget.load(),
                              {"KILLFOCUS", "SET Menu.CurValue {5}", "PLAY Menu.OnChange", "PLAY MoveToOptions"});
                }
                else if (at == FrontEndAt::Options)
                {
                    requests().frontEndRequest = 0;
                    runScript(rdram, ctx, runtime, requests().frontEndWidget.load(), {"KILLFOCUS", "PLAY GoToPcSettings"});
                }
                else if (at == FrontEndAt::Back && nowVsync() >= requests().frontEndBackAt.load() + 20u)
                {
                    // Player or level select: Back (Triangle) as the player
                    // would press it, once per screen, until the main menu
                    // has the focus.
                    requests().frontEndBackAt = nowVsync();
                    requests().frontEndRequest = nowVsync() + 240u;
                    PSPadBackend::pressButton(ps2_host_input::kTriangle, 3);
                }
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- restart items --------------------------------------------------

        std::string careerItemsRetry();

        // Pause Show, Career: the list with Retry Challenge when a challenge
        // can be given again (aggressive_inline_restart.cpp).
        void pauseCareerItemsNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t menu = firstArg(rdram, ctx);
            if (aggressive_inline::restart::challengeRetryable(rdram))
            {
                const std::string items = careerItemsRetry();
                if (!items.empty())
                    runScript(rdram, ctx, runtime, menu, {"SET Items {\"" + items + "\"}"});
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // Tutorial Show: the completed-lesson column with a blank row for
        // Restart Lesson.
        void tutorialCheckboxesNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            runScript(rdram, ctx, runtime, firstArg(rdram, ctx),
                      {"SET Items {\"" + aggressive_inline::restart::tutorialCheckboxItems(rdram) + "\"}"});
            ctx->pc = getRegU32(ctx, 31);
        }

        struct Native
        {
            const char *name;
            PS2Machine::RecompiledFunction fn;
        };
        // The page's own (zge_settings.cpp) are registered first, then these.
        const Native kNatives[] = {
            {"pcSettingsIntro", &introNative},
            {"pcSettingsPauseShown", &pauseShownNative},
            {"pcSettingsPauseBack", &pauseBackNative},
            {"pcQuitToDesktop", &quitToDesktopNative},
            {"pcMainMenuFocus", &mainMenuFocusNative},
            {"pcOptionsFocus", &optionsFocusNative},
            {"pcMenuBlur", &menuBlurNative},
            {"pcBackFocus", &backFocusNative},
            {"pcMainMenuBlur", &mainMenuBlurNative},
            {"pcMainMenuStart", &mainMenuStartNative},
            {"pcFrontEndPoll", &frontEndPollNative},
            {"pcRestartLevel", &aggressive_inline::restartLevelNative},
            {"pcRetryChallenge", &aggressive_inline::retryChallengeNative},
            {"pcRestartRun", &aggressive_inline::restartRunNative},
            {"pcRestartLesson", &aggressive_inline::restartLessonNative},
            {"pcJumpToLesson", &aggressive_inline::jumpToLessonNative},
            {"pcPauseCareerItems", &pauseCareerItemsNative},
            {"pcTutorialCheckboxes", &tutorialCheckboxesNative},
            {"pcPhotoMode", &aggressive_inline::photoModeNative},
        };
        // Every native in registration order: the page's, then AI's own.
        const std::vector<Native> &allNatives()
        {
            static const std::vector<Native> all = []
            {
                std::vector<Native> out;
                for (const zge_host::settings::Function &f : zge_host::settings::pageFunctions())
                    out.push_back({f.name, f.fn});
                out.insert(out.end(), std::begin(kNatives), std::end(kNatives));
                return out;
            }();
            return all;
        }
        // Where this boot's function table has them (0: none).
        struct NativeSlots
        {
            std::vector<uint32_t> address = std::vector<uint32_t>(allNatives().size(), 0u);
        };
        NativeSlots &nativeSlots() { return zx::session::state<NativeSlots>(); }

        // func_170820 wrapper: after the first native of one of the game's
        // sets, ours go into the same table.
        void registerNativeHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t name = getRegU32(ctx, 4);
            const uint32_t fn = getRegU32(ctx, 5);
            uint32_t result = 0;
            runtime->callGuestFunction(rdram, ctx, engine().registerNative, kRegisterNative, {name, fn}, getRegU32(ctx, 29),
                                       &result);
            const std::string registered = getString(rdram, name, 64);
            const uint32_t scratchTop = zge_host::settings::scratchTop();
            if (std::find(std::begin(kFirstGameNatives), std::end(kFirstGameNatives), registered) !=
                    std::end(kFirstGameNatives) &&
                scratchTop)
            {
                const uint32_t base = zge_host::settings::scratchStrings();
                const std::vector<Native> &natives = allNatives();
                for (size_t i = 0; i < natives.size(); ++i)
                {
                    const uint32_t address = nativeSlots().address[i];
                    if (!address || !putString(rdram, base, natives[i].name))
                        continue;
                    runtime->callGuestFunction(rdram, ctx, engine().registerNative, kRegisterNative, {base, address},
                                               scratchTop);
                }
                std::cerr << "[aggressive-inline] settings natives registered (after " << registered << ")" << std::endl;
            }
            setReturnU32(ctx, result);
            // Return like generated code (jr ra): when the scheduler resumes
            // a thread at this entry it calls the hook directly, and a pc
            // left at the entry would call it again.
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- the script patch -----------------------------------------------

        // Background.PcSettings: the Settings screen, laid out like the
        // Controller screen it replaces (frame, side banner, section
        // selector at the bottom, button prompts at the top).
        const char *const kFrontEndPage = R"ZSC(OBJECT "guiGroup" "PcSettings"
{
	//////////////////////////////
	// Property
	OnCreate	"SET Visible {0}"
	Visible	0
	Position	1180.000000 -448.000000 0.000000
	OBJECT "guiFrame" "Dialog"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	30.000000 223.000000 10.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	16
		Color	255 255 255 255
		Material_Left	"CommonWindow.window_side_lf"
		Material_Center	"CommonWindow.window_side_mid"
		Material_BottomLeft	"CommonWindow.window_btm_lf"
		Material_Bottom	"CommonWindow.window_btm_mid"
		Width	0
		Height	380
		OBJECT "guiImage" "Band"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Visible {0}"
			Visible	0
			Position	318.000000 14.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			TopLeftColor	0 0 0 160
			BottomLeftColor	0 0 0 160
			TopRightColor	0 0 0 160
			BottomRightColor	0 0 0 160
			Width	480
			Height	328
			UScale	1
			VScale	1
		}
		OBJECT "guiImage" "Highlight"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Visible {0}"
			Visible	0
			Position	318.000000 -128.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			TopLeftColor	154 28 33 255
			BottomLeftColor	154 28 33 255
			TopRightColor	154 28 33 255
			BottomRightColor	154 28 33 255
			Material	"CommonMaterials.scratch_score"
			Width	452
			Height	28
			UScale	1
			VScale	1
		}
@ROWS@		OBJECT "guiImage" "MoreUp"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Visible {0}"
			Visible	0
			Position	548.000000 -150.000000 0.000000
			Scale	0.750000 0.750000 1.000000
			Alignment	17
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"CommonMaterials.arrow_up"
			Width	32
			Height	32
			UScale	1
			VScale	1
		}
		OBJECT "guiImage" "MoreDown"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Visible {0}"
			Visible	0
			Position	548.000000 112.000000 0.000000
			Scale	0.750000 0.750000 1.000000
			Alignment	17
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"CommonMaterials.arrow_dn"
			Width	32
			Height	32
			UScale	1
			VScale	1
		}
		OBJECT "guiLabel" "Help"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Visible {0}"
			Visible	0
			Position	318.000000 134.000000 0.000000
			Scale	0.850000 0.850000 1.000000
			Alignment	17
			Spacing	1.250000
			Tracking	1
			Charset	"FrontEndFonts.fnt_14pt"
			Color	192 192 192 255
			Text	""
			Angle	0
			Monospace	0
		}
		OBJECT "guiLabel" "Help2"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Visible {0}"
			Visible	0
			Position	318.000000 152.000000 0.000000
			Scale	0.850000 0.850000 1.000000
			Alignment	17
			Spacing	1.250000
			Tracking	1
			Charset	"FrontEndFonts.fnt_14pt"
			Color	192 192 192 255
			Text	""
			Angle	0
			Monospace	0
		}
		OBJECT "guiLabel" "Section"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Visible {0}"
			Visible	0
			Position	318.000000 169.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			Spacing	0.700000
			Tracking	1
			Charset	"CommonFonts.fnt_ovr_trick_noborder"
			Color	176 142 21 255
			Text	"Display"
			Angle	0
			Monospace	0
			OBJECT "guiImage" "LeftArrow"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	-110.000000 3.000000 0.000000
				Scale	1.000000 1.000000 1.000000
				Alignment	18
				TopLeftColor	255 255 255 255
				BottomLeftColor	255 255 255 255
				TopRightColor	255 255 255 255
				BottomRightColor	255 255 255 255
				Material	"CommonMaterials.arrow_lf"
				Width	32
				Height	32
				UScale	1
				VScale	1
				ACTION "Interpolate" "Pulse"
				{
					//////////////////////////////
					// Property
					StartDelay	0
					OnUpdate	"LINEAR Scale {1.25 1.25 1.0} {1.0 1.0 1.0} Pulse.Ratio"
					InterpolateMode	0
					LoopMode	0
					IntervalMode	1
					IntervalValue	10
				}
			}
			OBJECT "guiImage" "RightArrow"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	142.000000 3.000000 0.000000
				Scale	1.000000 1.000000 1.000000
				Alignment	18
				TopLeftColor	255 255 255 255
				BottomLeftColor	255 255 255 255
				TopRightColor	255 255 255 255
				BottomRightColor	255 255 255 255
				Material	"CommonMaterials.arrow_rt"
				Width	32
				Height	32
				UScale	1
				VScale	1
				ACTION "Interpolate" "Pulse"
				{
					//////////////////////////////
					// Property
					StartDelay	0
					OnUpdate	"LINEAR Scale {1.25 1.25 1.0} {1.0 1.0 1.0} Pulse.Ratio"
					InterpolateMode	0
					LoopMode	0
					IntervalMode	1
					IntervalValue	10
				}
			}
		}
		OBJECT "guiKeySniffer" "KeySniffer"
		{
			//////////////////////////////
			// Property
			Visible	1
			Active	0
			Flags	3
			EnableAnalogDir	1
			AutoRepeat	1
			OnUpKey	{
				CALL pcSettingsUp .
				CALL inlineMenuPlayOnChangeSound
				}
			OnDownKey	{
				CALL pcSettingsDown .
				CALL inlineMenuPlayOnChangeSound
				}
			OnLeftKey	{
				CALL pcSettingsLeft .
				CALL inlineMenuPlayOnChangeSound
				}
			OnRightKey	{
				CALL pcSettingsRight .
				CALL inlineMenuPlayOnChangeSound
				}
			OnAccept	{
				CALL pcSettingsAccept .
				CALL inlineMenuPlayOnChangeSound
				}
			OnLTrig1	{
				CALL pcSettingsPrev .
				PLAY .Section.LeftArrow.Pulse
				CALL inlineMenuPlayOnChangeSound
				}
			OnRTrig1	{
				CALL pcSettingsNext .
				PLAY .Section.RightArrow.Pulse
				CALL inlineMenuPlayOnChangeSound
				}
			OnCancel	"PLAY ..GoBack"
			OnStart	"PLAY ..GoBack"
		}
		ACTION "Interpolate" "TickA"
		{
			//////////////////////////////
			// Property
			StartDelay	0
			OnEnd	"CALL pcSettingsTick .Dialog"
			InterpolateMode	0
			LoopMode	0
			IntervalMode	1
			IntervalValue	2
		}
		ACTION "Interpolate" "TickB"
		{
			//////////////////////////////
			// Property
			StartDelay	0
			OnEnd	"CALL pcSettingsTick .Dialog"
			InterpolateMode	0
			LoopMode	0
			IntervalMode	1
			IntervalValue	2
		}
		OBJECT "guiGroup" "FooterText"
		{
			//////////////////////////////
			// Property
			Visible	0
			Position	120.000000 -173.000000 0.000000
			OBJECT "guiImage" "X"
			{
				//////////////////////////////
				// Property
				Visible	1
				Scale	1.000000 1.000000 1.000000
				Alignment	17
				TopLeftColor	255 255 255 255
				BottomLeftColor	255 255 255 255
				TopRightColor	255 255 255 255
				BottomRightColor	255 255 255 255
				Material	"Buttons.select_button"
				Width	32
				Height	32
				UScale	1
				VScale	1
				OBJECT "guiLabel" "Label"
				{
					//////////////////////////////
					// Property
					Visible	1
					Position	20.000000 3.000000 0.000000
					Scale	1.000000 1.000000 1.000000
					Alignment	16
					Spacing	1.250000
					Tracking	1
					Charset	"FrontEndFonts.fnt_14pt"
					Color	255 255 255 255
					Text	"Change"
					Angle	0
					Monospace	0
				}
			}
			OBJECT "guiImage" "Tri"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	130.000000 0.000000 0.000000
				Scale	1.000000 1.000000 1.000000
				Alignment	17
				TopLeftColor	255 255 255 255
				BottomLeftColor	255 255 255 255
				TopRightColor	255 255 255 255
				BottomRightColor	255 255 255 255
				Material	"Buttons.cancel_button"
				Width	32
				Height	32
				UScale	1
				VScale	1
				OBJECT "guiLabel" "Label"
				{
					//////////////////////////////
					// Property
					Visible	1
					Position	20.000000 3.000000 0.000000
					Scale	1.000000 1.000000 1.000000
					Alignment	16
					Spacing	1.250000
					Tracking	1
					Charset	"FrontEndFonts.fnt_14pt"
					Color	255 255 255 255
					Text	"Back"
					Angle	0
					Monospace	0
				}
			}
			OBJECT "guiLabel" "Sections"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	250.000000 3.000000 0.000000
				Scale	1.000000 1.000000 1.000000
				Alignment	16
				Spacing	1.250000
				Tracking	1
				Charset	"FrontEndFonts.fnt_14pt"
				Color	255 255 255 255
				Text	"L1 R1  Section"
				Angle	0
				Monospace	0
			}
		}
		ACTION "Interpolate" "MoveIn"
		{
			//////////////////////////////
			// Property
			StartDelay	0
			OnStart	{
				SET Visible {1}
				CALL pcSettingsOpen .Dialog
				}
			OnUpdate	"LINEAR Width {0} {560} MoveIn.Ratio"
			OnEnd	{
				SET Band.Visible {1}
				SET Highlight.Visible {1}
				SET Rows.Visible {1}
				SET Help.Visible {1}
				SET Help2.Visible {1}
				SET Section.Visible {1}
				SET FooterText.Visible {1}
				CALL pcSettingsOpen .Dialog
				SET KeySniffer.Active {1}
				SETFOCUS KeySniffer
				}
			InterpolateMode	0
			LoopMode	0
			IntervalMode	1
			IntervalValue	20
		}
		ACTION "Interpolate" "MoveOut"
		{
			//////////////////////////////
			// Property
			StartDelay	0
			OnStart	{
				KILLFOCUS
				SET KeySniffer.Active {0}
				SET Band.Visible {0}
				SET Highlight.Visible {0}
				SET Rows.Visible {0}
				SET Help.Visible {0}
				SET Help2.Visible {0}
				SET Section.Visible {0}
				SET MoreUp.Visible {0}
				SET MoreDown.Visible {0}
				SET FooterText.Visible {0}
				}
			OnUpdate	"LINEAR Width {560} {0} MoveOut.Ratio"
			OnEnd	"SET Visible {0}"
			InterpolateMode	0
			LoopMode	0
			IntervalMode	1
			IntervalValue	20
		}
	}
	OBJECT "guiImage" "MenuTitle"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	47.000000 62.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	1
		TopLeftColor	255 255 255 0
		BottomLeftColor	255 255 255 0
		TopRightColor	255 255 255 0
		BottomRightColor	255 255 255 0
		Material	"FrontEnd.sidebanner_scratch"
		Width	256
		Height	318
		UScale	1
		VScale	1
		OBJECT "guiLabel" "Label"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	2.000000 18.000000 0.000000
			Scale	1.100000 1.000000 1.000000
			Alignment	16
			Spacing	1.250000
			Tracking	1
			Charset	"FrontEndFonts.fnt_sidebanner"
			Color	192 192 192 255
			Text	"SETTINGS"
			Angle	90
			Monospace	0
		}
		ACTION "Interpolate" "Appear"
		{
			//////////////////////////////
			// Property
			StartDelay	0
			OnStart	{
				SET Visible {1}
				STOP Disappear
				}
			OnUpdate	{
				LINEAR Width {256} {64} Appear.Ratio
				LINEAR TopLeftColor.alpha {0} {128} Appear.Ratio
				SET TopRightColor TopLeftColor
				SET BottomLeftColor TopLeftColor
				SET BottomRightColor TopLeftColor
				}
			InterpolateMode	0
			LoopMode	0
			IntervalMode	1
			IntervalValue	8
		}
		ACTION "Interpolate" "Disappear"
		{
			//////////////////////////////
			// Property
			StartDelay	0
			OnStart	"STOP Appear"
			OnUpdate	{
				LINEAR Width {64} {256} Disappear.Ratio
				LINEAR TopLeftColor {255 255 255 192} {255 255 255 0} Disappear.Ratio
				SET TopRightColor TopLeftColor
				SET BottomLeftColor TopLeftColor
				SET BottomRightColor TopLeftColor
				}
			OnEnd	"SET Visible {0}"
			InterpolateMode	0
			LoopMode	0
			IntervalMode	1
			IntervalValue	8
		}
	}
	ACTION "Interpolate" "MoveIn"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnStart	"SET Visible {1}"
		OnUpdate	"LINEAR .Position.x {-640} {-1180} MoveIn.Ratio"
		OnEnd	{
			PLAY Dialog.MoveIn
			PLAY MenuTitle.Appear
			}
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	45
	}
	ACTION "Interpolate" "GoBack"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnStart	{
			KILLFOCUS
			PLAY Dialog.MoveOut
			PLAY MenuTitle.Disappear
			}
		OnUpdate	"LINEAR .Position.x {-1180} {-640} GoBack.Ratio"
		OnEnd	{
			SET Visible {0}
			PLAY .Options.SideText.Appear
			PLAY .Options.Menu.MoveIn
			SETFOCUS .Options.Menu
			PLAY .Dialog.X.MoveIn
			PLAY .Dialog.Tri.MoveIn
			}
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	45
	}
}
)ZSC";


        const char *const kGoToPcSettings = R"ZSC(ACTION "Script" "GoToPcSettings"
{
	//////////////////////////////
	// Property
	StartDelay	0
	Script	{
		KILLFOCUS
		PLAY SideText.Disappear
		PLAY .FloatingIcon.Disappear
		PLAY Menu.MoveOut
		PLAY .PcSettings.MoveIn
		PLAY .Dialog.X.MoveOut
		PLAY .Dialog.Tri.MoveOut
		}
}
)ZSC";

        // InGameUI.PauseMenus.Frame.PcPause: the same screen as a page of
        // the pause menu (like its Sound Options page), the pause frame
        // widened and centred while it is shown.
        const char *const kPausePage = R"ZSC(OBJECT "guiGroup" "PcPause"
{
	//////////////////////////////
	// Property
	OnCreate	"SET Visible {0}"
	Visible	0
	OBJECT "guiLabel" "Title"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	0.000000 16.000000 0.000000
		Scale	1.500000 1.500000 1.000000
		Alignment	1
		Spacing	1.250000
		Tracking	1
		Charset	"CommonFonts.fnt_ovr_trick"
		Color	185 151 16 255
		Text	"SETTINGS"
		Angle	0
		Monospace	0
	}
	OBJECT "guiImage" "Highlight"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	0.000000 106.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		TopLeftColor	154 28 33 255
		BottomLeftColor	154 28 33 255
		TopRightColor	154 28 33 255
		BottomRightColor	154 28 33 255
		Material	"CommonMaterials.scratch_score"
		Width	466
		Height	24
		UScale	1
		VScale	1
	}
@ROWS@	OBJECT "guiImage" "MoreUp"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	232.000000 86.000000 0.000000
		Scale	0.750000 0.750000 1.000000
		Alignment	17
		TopLeftColor	255 255 255 255
		BottomLeftColor	255 255 255 255
		TopRightColor	255 255 255 255
		BottomRightColor	255 255 255 255
		Material	"CommonMaterials.arrow_up"
		Width	32
		Height	32
		UScale	1
		VScale	1
	}
	OBJECT "guiImage" "MoreDown"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	232.000000 316.000000 0.000000
		Scale	0.750000 0.750000 1.000000
		Alignment	17
		TopLeftColor	255 255 255 255
		BottomLeftColor	255 255 255 255
		TopRightColor	255 255 255 255
		BottomRightColor	255 255 255 255
		Material	"CommonMaterials.arrow_dn"
		Width	32
		Height	32
		UScale	1
		VScale	1
	}
	OBJECT "guiLabel" "Section"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	0.000000 72.000000 0.000000
		Scale	0.850000 0.850000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"CommonFonts.fnt_ovr_trick_noborder"
		Color	176 142 21 255
		Text	"Display"
		Angle	0
		Monospace	0
		OBJECT "guiImage" "LeftArrow"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	-110.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	18
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"CommonMaterials.arrow_lf"
			Width	32
			Height	32
			UScale	1
			VScale	1
			ACTION "Interpolate" "Pulse"
			{
				//////////////////////////////
				// Property
				StartDelay	0
				OnUpdate	"LINEAR Scale {1.25 1.25 1.0} {1.0 1.0 1.0} Pulse.Ratio"
				InterpolateMode	0
				LoopMode	0
				IntervalMode	1
				IntervalValue	10
			}
		}
		OBJECT "guiImage" "RightArrow"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	142.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	18
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"CommonMaterials.arrow_rt"
			Width	32
			Height	32
			UScale	1
			VScale	1
			ACTION "Interpolate" "Pulse"
			{
				//////////////////////////////
				// Property
				StartDelay	0
				OnUpdate	"LINEAR Scale {1.25 1.25 1.0} {1.0 1.0 1.0} Pulse.Ratio"
				InterpolateMode	0
				LoopMode	0
				IntervalMode	1
				IntervalValue	10
			}
		}
	}
	OBJECT "guiLabel" "Help"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	0.000000 322.000000 0.000000
		Scale	0.800000 0.800000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"InGameFonts.fnt_ovr_small"
		Color	192 192 192 255
		Text	""
		Angle	0
		Monospace	0
	}
	OBJECT "guiLabel" "Help2"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	0.000000 337.000000 0.000000
		Scale	0.800000 0.800000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"InGameFonts.fnt_ovr_small"
		Color	192 192 192 255
		Text	""
		Angle	0
		Monospace	0
	}
	OBJECT "guiKeySniffer" "KeySniffer"
	{
		//////////////////////////////
		// Property
		Visible	1
		Active	0
		Flags	3
		EnableAnalogDir	1
		AutoRepeat	1
		OnUpKey	"CALL pcSettingsUp ."
		OnDownKey	"CALL pcSettingsDown ."
		OnLeftKey	"CALL pcSettingsLeft ."
		OnRightKey	"CALL pcSettingsRight ."
		OnAccept	"CALL pcSettingsAccept ."
		OnLTrig1	{
			CALL pcSettingsPrev .
			PLAY .Section.LeftArrow.Pulse
			}
		OnRTrig1	{
			CALL pcSettingsNext .
			PLAY .Section.RightArrow.Pulse
			}
		OnCancel	{
			CALL inlinePausePlayCancelSound
			PLAY .Hide
			}
		OnStart	{
			CALL inlinePausePlayCancelSound
			PLAY .Hide
			}
	}
	ACTION "Interpolate" "TickA"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnEnd	"CALL pcSettingsTick .PcPause"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	2
	}
	ACTION "Interpolate" "TickB"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnEnd	"CALL pcSettingsTick .PcPause"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	2
	}
	OBJECT "guiGroup" "FooterText"
	{
		//////////////////////////////
		// Property
		Visible	0
		Position	0.000000 363.000000 0.000000
		OBJECT "guiImage" "Triangle"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	-220.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	16
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"Buttons.cancel_button"
			Width	32
			Height	32
			UScale	1
			VScale	1
			OBJECT "guiLabel" "Back"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	35.000000 4.000000 0.000000
				Scale	0.750000 0.750000 1.000000
				Alignment	16
				Spacing	1.250000
				Tracking	1
				Charset	"CommonFonts.fnt_ovr_trick"
				Color	102 164 223 255
				Text	"Back"
				Angle	0
				Monospace	0
			}
		}
		OBJECT "guiImage" "X"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	-84.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	16
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"Buttons.select_button"
			Width	32
			Height	32
			UScale	1
			VScale	1
			OBJECT "guiLabel" "Select"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	35.000000 4.000000 0.000000
				Scale	0.750000 0.750000 1.000000
				Alignment	16
				Spacing	1.250000
				Tracking	1
				Charset	"CommonFonts.fnt_ovr_trick"
				Color	102 164 223 255
				Text	"Change"
				Angle	0
				Monospace	0
			}
		}
		OBJECT "guiLabel" "Sections"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	92.000000 4.000000 0.000000
			Scale	0.750000 0.750000 1.000000
			Alignment	16
			Spacing	1.250000
			Tracking	1
			Charset	"CommonFonts.fnt_ovr_trick"
			Color	102 164 223 255
			Text	"L1 R1  Section"
			Angle	0
			Monospace	0
		}
	}
	ACTION "Interpolate" "Show"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnStart	{
			SET Visible {1}
			SET FooterText.Visible {1}
			CALL pcSettingsOpen .PcPause
			}
		OnUpdate	{
			LINEAR .Height {99} {380} Show.Ratio
			LINEAR .Width {384} {496} Show.Ratio
			LINEAR .Position.x {13} {0} Show.Ratio
			}
		OnEnd	{
			SET Highlight.Visible {1}
			SET Rows.Visible {1}
			SET Help.Visible {1}
			SET Help2.Visible {1}
			SET Section.Visible {1}
			CALL pcSettingsOpen .PcPause
			SET KeySniffer.Active {1}
			SETFOCUS KeySniffer
			}
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	15
	}
	ACTION "Interpolate" "Hide"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnStart	{
			KILLFOCUS
			SET KeySniffer.Active {0}
			SET Highlight.Visible {0}
			SET Rows.Visible {0}
			SET Help.Visible {0}
			SET Help2.Visible {0}
			SET Section.Visible {0}
			SET MoreUp.Visible {0}
			SET MoreDown.Visible {0}
			SET FooterText.Visible {0}
			}
		OnUpdate	{
			LINEAR .Height {380} {99} Hide.Ratio
			LINEAR .Width {496} {384} Hide.Ratio
			LINEAR .Position.x {0} {13} Hide.Ratio
			}
		OnEnd	{
			SET Visible {0}
			CALL pcSettingsPauseBack .PcPause
			}
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	15
	}
}
)ZSC";

        const char *const kPauseGoToPcSettings = R"ZSC(ACTION "Interpolate" "GoToPcSettings"
{
	//////////////////////////////
	// Property
	StartDelay	0
	OnStart	{
		SET Menu.Visible {0}
		KILLFOCUS
		}
	OnUpdate	{
		LINEAR .Height {343} {99} GoToPcSettings.Ratio 
		SET FooterText.Position.y .Height
		// The magic number
		SUB FooterText.Position.y {17}
		}
	OnEnd	{
		KILLFOCUS
		SET FooterText.Visible {0}
		SET Title.Visible {0}
		PLAY .PcPause.Show
		}
	InterpolateMode	0
	LoopMode	0
	IntervalMode	1
	IntervalValue	15
}
)ZSC";

        // The Tutorial's lesson list (InGameUI.PauseMenus.Frame.Tutorial):
        // Restart Lesson, Settings and Quit to Desktop items, the
        // GoToPcSettings action, and its Show telling the natives it is up
        // (Cmd+, and Back).
        bool patchTutorialPause(std::string &text)
        {
            zx::zaxis::Span tutorial = zx::zaxis::findObject(text, "InGameUI/PauseMenus/Frame/Tutorial");
            if (!tutorial.found() || !zx::zaxis::findObject(text, "Menu", tutorial).found() ||
                !zx::zaxis::findObject(text, "Show", tutorial).found())
                return false;
            // Later positions first: the group's end, its Show, then its menu.
            text.insert(zx::zaxis::objectEnd(text, tutorial), zx::zaxis::indentBlock(kPauseGoToPcSettings, zx::zaxis::indentAt(text, tutorial.begin) + 1));
            tutorial = zx::zaxis::findObject(text, "InGameUI/PauseMenus/Frame/Tutorial");
            zx::zaxis::Span show = zx::zaxis::findObject(text, "Show", tutorial);
            const std::string showTabs(static_cast<size_t>(zx::zaxis::indentAt(text, show.begin) + 2), '\t');
            if (!zx::zaxis::replaceIn(text, show, showTabs + "SETFOCUS Menu\r\n",
                               showTabs + "SETFOCUS Menu\r\n" + showTabs + "CALL pcSettingsPauseShown .Tutorial\r\n"))
                return false;
            // Restart Lesson's blank in the completed-lesson column.
            if (!zx::zaxis::replaceIn(text, show, "CALL inlinePausePopulateTutorialCheckboxes Menu.CompletedCheckboxes",
                               "CALL pcTutorialCheckboxes Menu.CompletedCheckboxes"))
                return false;
            zx::zaxis::Span menu = zx::zaxis::findObject(text, "Menu", tutorial);
            const std::string t(static_cast<size_t>(zx::zaxis::indentAt(text, menu.begin) + 2), '\t');
            // This menu is the lesson list, not a pause menu: Restart Lesson
            // (the current lesson again, as its item would) stays second,
            // after Resume Tutorial, where a lesson is restarted from; the
            // lesson items, one further down, jump through pcJumpToLesson
            // (SelectedIndex less two). The port's other lines are one block
            // after the game's last, Exit Game: Photo Mode, Settings, Quit to
            // Desktop.
            if (!zx::zaxis::replaceIn(text, menu, t + "Resume Tutorial\r\n", t + "Resume Tutorial\r\n" + t + "Restart Lesson\r\n") ||
                !zx::zaxis::replaceIn(text, menu, t + "IF SelectedText == {\"Resume Tutorial\"} GOTO Resume\r\n",
                               t + "IF SelectedText == {\"Resume Tutorial\"} GOTO Resume\r\n" + t +
                                   "IF SelectedText == {\"Restart Lesson\"} GOTO PcRestartLesson\r\n") ||
                !zx::zaxis::replaceIn(text, menu, t + "CALL inlinePauseJumpToTutorial SelectedIndex\r\n",
                               t + "CALL pcJumpToLesson .Menu\r\n") ||
                !zx::zaxis::replaceIn(text, menu, t + ":LevelSelect\r\n",
                               t + ":PcRestartLesson\r\n" + t + "CALL pcRestartLesson\r\n" + t + "PLAY OnCancel\r\n" + t +
                                   "GOTO End\r\n" + t + ":LevelSelect\r\n"))
                return false;
            return zx::zaxis::replaceIn(text, menu, t + ":Exit\r\n",
                                 t + ":PcSettings\r\n" + t + "PLAY .GoToPcSettings\r\n" + t + "GOTO End\r\n" + t +
                                     ":PcPhoto\r\n" + t + "CALL pcPhotoMode\r\n" + t + "GOTO End\r\n" + t +
                                     ":PcQuit\r\n" + t + "CALL pcQuitToDesktop\r\n" + t + "GOTO End\r\n" + t + ":Exit\r\n") &&
                   zx::zaxis::replaceIn(text, menu, t + "IF SelectedText == {\"Exit Game\"} GOTO Exit\r\n",
                                 t + "IF SelectedText == {\"Settings\"} GOTO PcSettings\r\n" + t +
                                     "IF SelectedText == {\"Photo Mode\"} GOTO PcPhoto\r\n" + t +
                                     "IF SelectedText == {\"Quit to Desktop\"} GOTO PcQuit\r\n" + t +
                                     "IF SelectedText == {\"Exit Game\"} GOTO Exit\r\n") &&
                   zx::zaxis::replaceIn(text, menu, t + "Exit Game\r\n",
                                 t + "Exit Game\r\n" + t + "Photo Mode\r\n" + t + "Settings\r\n" + t + "Quit to Desktop\r\n");
        }

        // The pause menu's Restart Level (Career, Freeskate: the level loaded
        // again, through an action like its Exit's, aggressive_inline_restart.cpp)
        // and Restart Run (Timed Run: the game's own Play Again), the first of
        // the port's block after the game's Exit (before Photo Mode). In the
        // Career, Retry Challenge above Restart Level while a challenge given
        // in the level can be given again: the Show's Career list is followed
        // by pcPauseCareerItems, which sets the list with it.
        const char *const kRestartAction = R"ZSC(ACTION "Script" "PcRestartGame"
{
	//////////////////////////////
	// Property
	StartDelay	0.000010
	Script	"CALL pcRestartLevel"
}
)ZSC";

        std::mutex g_careerItemsMutex;
        std::string g_careerItemsRetry; // the Career list with Retry Challenge (script literal text)

        std::string careerItemsRetry()
        {
            std::lock_guard<std::mutex> lock(g_careerItemsMutex);
            return g_careerItemsRetry;
        }

        bool patchRestartItems(std::string &text)
        {
            zx::zaxis::Span pause = zx::zaxis::findObject(text, "InGameUI/PauseMenus/Frame/Pause");
            zx::zaxis::Span menu = zx::zaxis::findObject(text, "Menu", pause);
            zx::zaxis::Span exitAction = zx::zaxis::findObject(text, "ExitGameToLevelSelect", menu);
            if (!pause.found() || !menu.found() || !exitAction.found())
                return false;
            text.insert(exitAction.end, zx::zaxis::indentBlock(kRestartAction, zx::zaxis::indentAt(text, exitAction.begin)));
            pause = zx::zaxis::findObject(text, "InGameUI/PauseMenus/Frame/Pause");
            zx::zaxis::Span show = zx::zaxis::findObject(text, "Show", pause);
            menu = zx::zaxis::findObject(text, "Menu", pause);
            // The Show action comes after the menu: edit it first. Its lists
            // as patchInGame left them each end "...Level Select\nExit\nPhoto
            // Mode\nSettings\nQuit to Desktop": Timed Run's (with End Run)
            // gets Restart Run after Exit, Freeskate's and the Career's (with
            // View Loot) Restart Level.
            const std::string tail = "\\nLevel Select\\nExit\\nPhoto Mode";
            bool ok = show.found() && menu.found() &&
                      zx::zaxis::replaceIn(text, show, "\\nEnd Run\\nTrick List\\nSound Options" + tail,
                                    "\\nEnd Run\\nTrick List\\nSound Options\\nLevel Select\\nExit\\nRestart Run\\nPhoto Mode") &&
                      zx::zaxis::replaceIn(text, show, "{\"Resume Game\\nTrick List\\nSound Options" + tail,
                                    "{\"Resume Game\\nTrick List\\nSound Options\\nLevel Select\\nExit\\nRestart Level\\nPhoto Mode") &&
                      zx::zaxis::replaceIn(text, show, "\\nView Loot\\nTrick List\\nSound Options" + tail,
                                    "\\nView Loot\\nTrick List\\nSound Options\\nLevel Select\\nExit\\nRestart Level\\nPhoto Mode");
            if (!ok)
                return false;
            // The Career line: SET Menu.Items {"Resume Game\nChallenges\n...\nExit\nRestart Level\n..."}
            const std::string career = "SET Menu.Items {\"Resume Game\\nChallenges\\n";
            const size_t at = text.find(career, show.begin);
            const size_t lineEnd = at == std::string::npos || at >= show.end ? std::string::npos : text.find("\r\n", at);
            const size_t close = lineEnd == std::string::npos ? std::string::npos : text.rfind("\"}", lineEnd);
            if (close == std::string::npos || close < at)
                return false;
            const size_t open = at + std::string("SET Menu.Items {\"").size();
            std::string items = text.substr(open, close - open);
            // Retry Challenge between Exit and Restart Level.
            const size_t restartAt = items.find("Exit\\nRestart Level\\n");
            if (restartAt == std::string::npos)
                return false;
            items.insert(restartAt + std::string("Exit\\n").size(), "Retry Challenge\\n");
            {
                std::lock_guard<std::mutex> lock(g_careerItemsMutex);
                g_careerItemsRetry = items;
            }
            const std::string showTabs(static_cast<size_t>(zx::zaxis::indentAt(text, at)), '\t');
            text.insert(lineEnd + 2u, showTabs + "CALL pcPauseCareerItems Menu\r\n");
            pause = zx::zaxis::findObject(text, "InGameUI/PauseMenus/Frame/Pause");
            menu = zx::zaxis::findObject(text, "Menu", pause);
            const std::string t(static_cast<size_t>(zx::zaxis::indentAt(text, menu.begin) + 2), '\t');
            return zx::zaxis::replaceIn(text, menu, t + ":Sound Options\r\n",
                                 t + ":Restart Level\r\n" + t + "PLAY PcRestartGame\r\n" + t + "GOTO End\r\n" + t +
                                     ":Restart Run\r\n" + t + "PLAY .Hide\r\n" + t + "CALL inlinePauseModeExit\r\n" + t +
                                     "CALL inlinePauseRestartTimedRun\r\n" + t + "GOTO End\r\n" + t + ":Retry Challenge\r\n" +
                                     t + "PLAY .Hide\r\n" + t + "CALL inlinePauseModeExit\r\n" + t + "CALL pcRetryChallenge\r\n" +
                                     t + "GOTO End\r\n" + t + ":Sound Options\r\n");
        }

        // "Are you sure?" before the pause menus' Level Select, Exit Game and
        // Quit to Desktop, as the Xbox release asks (its AreYouSure frame in
        // InGameUI and MultiplayerUI: Yes / No, No preselected, Triangle is
        // No). Here with a number for what Yes does (Action: 1 Level
        // Select, 2 Exit Game, 3 Quit to Desktop) and for the menu it came
        // from (From: 0 the pause menu, 1 the Tutorial's lesson list) in
        // place of the Xbox engine's script-name variables; the texts are
        // the English ones (the language's through the text hooks). The
        // UI is 512 units wide (the PS2 frame buffer's width) and the pause
        // frame's middle is at x 269 (PauseMenus 256 + Frame 13): centred
        // on it, as the Xbox release centres its frame on the screen.
        const char *const kConfirmFrame = R"ZSC(OBJECT "guiFrame" "PcConfirm"
{
	//////////////////////////////
	// Property
	OnCreate	{
		DECLARE Sint32 Action
		DECLARE Sint32 From
		SET Action {0}
		SET From {0}
		SET Visible {0}
		}
	OnDestroy	{
		UNDECLARE Action
		UNDECLARE From
		}
	Visible	0
	Position	269.000000 216.000000 0.000000
	Scale	1.000000 1.000000 1.000000
	Alignment	17
	Color	255 255 255 255
	Material_TopLeft	"CommonWindow.window_top_lf"
	Material_Top	"CommonWindow.window_top_lf"
	Material_TopRight	"CommonWindow.window_top_rt"
	Material_Left	"CommonWindow.window_side_lf"
	Material_Center	"CommonWindow.window_side_mid"
	Material_BottomLeft	"CommonWindow.window_btm_lf"
	Material_Bottom	"CommonWindow.window_btm_mid"
	Width	384
	Height	256
	OBJECT "guiImage" "Black"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	-188.000000 -68.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	0
		TopLeftColor	0 0 0 255
		BottomLeftColor	0 0 0 255
		TopRightColor	0 0 0 255
		BottomRightColor	0 0 0 255
		Width	377
		Height	167
		UScale	1
		VScale	1
	}
	OBJECT "guiLabel" "Question"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	0.000000 -40.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	1
		Spacing	1.250000
		Tracking	1
		Charset	"CommonFonts.fnt_ovr_trick"
		Color	255 255 255 255
		Text	"Are you sure?"
		Angle	0
		Monospace	0
	}
	OBJECT "guiMenu" "Menu"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	0.000000 33.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Horizontal	0
		HorizontalSpacing	10
		WrapAround	0
		VisibleCount	4
		SelectedIndex	1
		SelectedText	"No"
		HideSelectionOnLostFocus	1
		Charset	"CommonFonts.fnt_ovr_trick"
		Spacing	1.250000
		Alignment	1
		Color	102 164 223 255
		SelectedColor	234 234 234 255
		DisabledColor	128 128 128 255
		Items	{
			Yes
			No
			}
		OnCancel	"PLAY .Hide"
		OnItemSelect	{
			GOTO SelectedIndex
			:0
			PLAY .OnYes
			GOTO Done
			:1
			PLAY .Hide
			:Done
			}
	}
	OBJECT "guiGroup" "FooterText"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	0.000000 112.000000 0.000000
		OBJECT "guiImage" "Triangle"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	-172.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	16
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"Buttons.cancel_button"
			Width	32
			Height	32
			UScale	1
			VScale	1
			OBJECT "guiLabel" "Back"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	35.000000 4.000000 0.000000
				Scale	0.750000 0.750000 1.000000
				Alignment	16
				Spacing	1.250000
				Tracking	1
				Charset	"CommonFonts.fnt_ovr_trick"
				Color	102 164 223 255
				Text	"Back"
				Angle	0
				Monospace	0
			}
		}
		OBJECT "guiImage" "X"
		{
			//////////////////////////////
			// Property
			Visible	1
			Scale	1.000000 1.000000 1.000000
			Alignment	16
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"Buttons.select_button"
			Width	32
			Height	32
			UScale	1
			VScale	1
			OBJECT "guiLabel" "Select"
			{
				//////////////////////////////
				// Property
				Visible	1
				Position	35.000000 4.000000 0.000000
				Scale	0.750000 0.750000 1.000000
				Alignment	16
				Spacing	1.250000
				Tracking	1
				Charset	"CommonFonts.fnt_ovr_trick"
				Color	102 164 223 255
				Text	"Select"
				Angle	0
				Monospace	0
			}
		}
	}
	ACTION "Script" "OnYes"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		Script	{
			SET Visible {0}
			KILLFOCUS
			IF Action == {3} GOTO Quit
			IF From == {1} GOTO Tutorial
			CALL inlinePauseTimedRunEndRun
			IF Action == {1} GOTO LevelSelect
			@EXIT@
			GOTO End
			:LevelSelect
			@LEVELSELECT@
			GOTO End
			:Tutorial
			IF Action == {1} GOTO TutorialLevelSelect
			@TUTORIALEXIT@
			GOTO End
			:TutorialLevelSelect
			@TUTORIALLEVELSELECT@
			GOTO End
			:Quit
			CALL pcQuitToDesktop
			@NOQUIT@
			:End
			}
	}
	ACTION "Script" "Show"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		Script	{
			SET Visible {1}
			SET Menu.SelectedIndex {1}
			SETFOCUS Menu
			}
	}
	ACTION "Script" "Hide"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		Script	{
			SET Visible {0}
			IF From == {1} GOTO Tutorial
			SETFOCUS @PAUSEMENU@
			GOTO End
			:Tutorial
			SETFOCUS @TUTORIALMENU@
			:End
			}
	}
}
)ZSC";

        std::string confirmFrame(const std::string &root)
        {
            std::string out = kConfirmFrame;
            const auto put = [&out](const std::string &mark, const std::string &with)
            {
                for (size_t at = out.find(mark); at != std::string::npos; at = out.find(mark, at + with.size()))
                    out.replace(at, mark.size(), with);
            };
            const std::string pause = "Root." + root + ".PauseMenus.Frame.Pause.Menu";
            if (root == "InGameUI")
            {
                // As the pause menu's own items: its ExitGame and
                // ExitGameToLevelSelect actions (a frame's delay, then
                // inlinePauseExit / inlinePauseExitToLevelSelect); the
                // Tutorial's Exit Game calls inlinePauseExit at once.
                put("@EXIT@", "PLAY " + pause + ".ExitGame");
                put("@LEVELSELECT@", "PLAY " + pause + ".ExitGameToLevelSelect");
                put("@TUTORIALEXIT@", "CALL inlinePauseExit");
                put("@TUTORIALLEVELSELECT@", "PLAY " + pause + ".ExitGameToLevelSelect");
            }
            else
            {
                put("@EXIT@", "CALL inlinePauseExit");
                put("@LEVELSELECT@", "CALL inlinePauseExitToLevelSelect");
                put("@TUTORIALEXIT@", "CALL inlinePauseExit");
                put("@TUTORIALLEVELSELECT@", "CALL inlinePauseExitToLevelSelect");
            }
            // Quit to Desktop quits at the next frame; the menu gets the
            // focus back meanwhile.
            put("@NOQUIT@", "PLAY .Hide");
            put("@PAUSEMENU@", pause);
            put("@TUTORIALMENU@", "Root." + root + ".PauseMenus.Frame.Tutorial.Menu");
            return out;
        }

        // The lines that ask instead of acting: from (0 the pause menu, 1
        // the Tutorial's), action (1 Level Select, 2 Exit Game, 3 Quit to
        // Desktop).
        std::string askLines(const std::string &root, const std::string &t, int from, int action)
        {
            const std::string frame = "Root." + root + ".PcConfirm";
            return t + "SET " + frame + ".From {" + std::to_string(from) + "}\r\n" + t + "SET " + frame + ".Action {" +
                   std::to_string(action) + "}\r\n" + t + "PLAY " + frame + ".Show\r\n";
        }

        // Adds the frame to the UI root and makes the items ask first.
        bool patchConfirm(std::string &text, const std::string &root)
        {
            std::string edited = text;
            zx::zaxis::Span ui = zx::zaxis::findObject(edited, root);
            if (!ui.found())
                return false;
            edited.insert(zx::zaxis::objectEnd(edited, ui), zx::zaxis::indentBlock(confirmFrame(root), zx::zaxis::indentAt(edited, ui.begin) + 1));
            zx::zaxis::Span menu = zx::zaxis::findObject(edited, root + "/PauseMenus/Frame/Pause/Menu");
            if (!menu.found())
                return false;
            const std::string t(static_cast<size_t>(zx::zaxis::indentAt(edited, menu.begin) + 2), '\t');
            bool ok = true;
            if (root == "InGameUI")
            {
                ok = zx::zaxis::replaceIn(edited, menu,
                                   t + ":Level Select\r\n" + t + "CALL inlinePauseTimedRunEndRun\r\n" + t +
                                       "PLAY ExitGameToLevelSelect\r\n",
                                   t + ":Level Select\r\n" + askLines(root, t, 0, 1)) &&
                     zx::zaxis::replaceIn(edited, menu,
                                   t + "// Exit\r\n" + t + "CALL inlinePauseTimedRunEndRun\r\n" + t + "PLAY ExitGame\r\n",
                                   t + "// Exit\r\n" + askLines(root, t, 0, 2) + t + "GOTO End\r\n") &&
                     zx::zaxis::replaceIn(edited, menu, t + ":Quit to Desktop\r\n" + t + "CALL pcQuitToDesktop\r\n",
                                   t + ":Quit to Desktop\r\n" + askLines(root, t, 0, 3));
                // The Tutorial's lesson list.
                zx::zaxis::Span tutorial = zx::zaxis::findObject(edited, root + "/PauseMenus/Frame/Tutorial/Menu");
                const std::string tt(static_cast<size_t>(tutorial.found() ? zx::zaxis::indentAt(edited, tutorial.begin) + 2 : 0), '\t');
                ok = ok && tutorial.found() &&
                     zx::zaxis::replaceIn(edited, tutorial, tt + "PLAY ..Pause.Menu.ExitGameToLevelSelect \r\n",
                                   askLines(root, tt, 1, 1)) &&
                     zx::zaxis::replaceIn(edited, tutorial, tt + "CALL inlinePauseExit\r\n", askLines(root, tt, 1, 2)) &&
                     zx::zaxis::replaceIn(edited, tutorial, tt + ":PcQuit\r\n" + tt + "CALL pcQuitToDesktop\r\n",
                                   tt + ":PcQuit\r\n" + askLines(root, tt, 1, 3));
            }
            else
            {
                ok = zx::zaxis::replaceIn(edited, menu,
                                   t + "// Level Select\r\n" + t + "CALL inlinePauseTimedRunEndRun\r\n" + t +
                                       "CALL inlinePauseExitToLevelSelect\r\n",
                                   t + "// Level Select\r\n" + askLines(root, t, 0, 1)) &&
                     zx::zaxis::replaceIn(edited, menu,
                                   t + "// Exit\r\n" + t + "CALL inlinePauseTimedRunEndRun\r\n" + t + "CALL inlinePauseExit\r\n",
                                   t + "// Exit\r\n" + askLines(root, t, 0, 2)) &&
                     zx::zaxis::replaceIn(edited, menu, t + "CALL pcQuitToDesktop\r\n", askLines(root, t, 0, 3));
            }
            if (!ok)
                return false;
            text = std::move(edited);
            return true;
        }

        // Text edits of InGame.zsc: the page, a Settings item after Sound
        // Options in the pause menu (each game mode's list) and its action.
        bool patchInGame(std::string &text)
        {
            std::string edited = text;
            zx::zaxis::Span frame = zx::zaxis::findObject(edited, "InGameUI/PauseMenus/Frame");
            zx::zaxis::Span options = zx::zaxis::findObject(edited, "Options", frame);
            zx::zaxis::Span pause = zx::zaxis::findObject(edited, "Pause", frame);
            zx::zaxis::Span menu = zx::zaxis::findObject(edited, "Menu", pause);
            zx::zaxis::Span show = zx::zaxis::findObject(edited, "Show", pause);
            zx::zaxis::Span goToOptions = zx::zaxis::findObject(edited, "GoToOptions", pause);
            if (!frame.found() || !options.found() || !pause.found() || !menu.found() || !show.found() ||
                !goToOptions.found())
            {
                std::cerr << "[aggressive-inline] InGame.zsc: anchors not found, pause settings page not added" << std::endl;
                return false;
            }
            // Later positions first.
            edited.insert(options.end, zx::zaxis::indentBlock(withRows(kPausePage, kPauseLayout, 1), zx::zaxis::indentAt(edited, options.begin)));
            edited.insert(goToOptions.end, zx::zaxis::indentBlock(kPauseGoToPcSettings, zx::zaxis::indentAt(edited, goToOptions.begin)));
            // The Show action comes after the menu: edit it first, so the
            // menu's span stays valid. Each game mode's list (Career,
            // Freeskate, Timed Run) ends with the game's Exit: the port's
            // lines follow it as one block, Photo Mode, Settings, Quit to
            // Desktop (the Restart items go in before Photo Mode,
            // patchRestartItems). The game's own lines stay where they are.
            bool ok = true;
            int step = 0;
            for (int i = 0; i < 3 && ok; ++i, ++step)
                ok = zx::zaxis::replaceIn(edited, show, "Level Select\\nExit\"}",
                                   "Level Select\\nExit\\nPhoto Mode\\nSettings\\nQuit to Desktop\"}");
            const std::string showTabs(static_cast<size_t>(zx::zaxis::indentAt(edited, show.begin) + 2), '\t');
            ok = ok && zx::zaxis::replaceIn(edited, show, showTabs + "SETFOCUS Menu\r\n",
                                     showTabs + "SETFOCUS Menu\r\n" + showTabs + "CALL pcSettingsPauseShown .Pause\r\n");
            step = ok ? 10 : step;
            const int itemDepth = zx::zaxis::indentAt(edited, menu.begin) + 2;
            const std::string tabs(static_cast<size_t>(itemDepth), '\t');
            // The items' scripts (OnItemSelect goes to the label named as
            // the item): Settings opens the page; Photo Mode
            // (aggressive_inline_photo.cpp) keeps the game paused and comes
            // back to this menu after.
            ok = ok && zx::zaxis::replaceIn(edited, menu, tabs + ":Sound Options\r\n",
                                     tabs + ":Settings\r\n" + tabs + "PLAY .GoToPcSettings\r\n" + tabs + "GOTO End\r\n" + tabs +
                                         ":Photo Mode\r\n" + tabs + "CALL pcPhotoMode\r\n" + tabs + "GOTO End\r\n" + tabs +
                                         ":Sound Options\r\n");
            step = ok ? 20 : step;
            ok = ok && zx::zaxis::replaceIn(edited, menu, tabs + ":Exit\r\n",
                                     tabs + ":Quit to Desktop\r\n" + tabs + "CALL pcQuitToDesktop\r\n" + tabs + "Goto End\r\n" +
                                         tabs + ":Exit\r\n");
            step = ok ? 30 : step;
            // The menu's own list (the Career's, until its Show sets it):
            // the same block after Exit.
            ok = ok && zx::zaxis::replaceIn(edited, menu, tabs + "Exit\r\n",
                                     tabs + "Exit\r\n" + tabs + "Photo Mode\r\n" + tabs + "Settings\r\n" + tabs +
                                         "Quit to Desktop\r\n");
            step = ok ? 40 : step;
            if (!ok)
            {
                std::cerr << "[aggressive-inline] InGame.zsc: menu lines not found (" << step
                          << "), pause settings page not added" << std::endl;
                return false;
            }
            {
                std::string withRestart = edited;
                if (patchRestartItems(withRestart))
                    edited = std::move(withRestart);
                else
                    std::cerr << "[aggressive-inline] InGame.zsc: pause menu lines not found, no Restart items" << std::endl;
            }
            // The Tutorial's own pause menu (its lessons): Settings before
            // Level Select (the same page, back to the lesson list) and Quit
            // to Desktop after Exit Game.
            {
                std::string withTutorial = edited;
                if (patchTutorialPause(withTutorial))
                    edited = std::move(withTutorial);
                else
                    std::cerr << "[aggressive-inline] InGame.zsc: Tutorial menu lines not found, no Settings or Quit to Desktop there"
                              << std::endl;
            }
            if (!patchConfirm(edited, "InGameUI"))
                std::cerr << "[aggressive-inline] InGame.zsc: pause menu lines not found, no \"Are you sure?\"" << std::endl;
            text = std::move(edited);
            return true;
        }

        // Text edits of Multiplayer.zsc: the split screen's pause menu (the
        // same frame, Pause group and Options page as InGame.zsc's, with a
        // fixed item list picked by index) gets the page and the port's
        // lines as one block after the game's Exit Game: Restart Run,
        // Restart Level, Photo Mode, Settings, Quit to Desktop.
        bool patchMultiplayer(std::string &text)
        {
            std::string edited = text;
            zx::zaxis::Span frame = zx::zaxis::findObject(edited, "MultiplayerUI/PauseMenus/Frame");
            zx::zaxis::Span options = zx::zaxis::findObject(edited, "Options", frame);
            zx::zaxis::Span pause = zx::zaxis::findObject(edited, "Pause", frame);
            zx::zaxis::Span goToOptions = zx::zaxis::findObject(edited, "GoToOptions", pause);
            if (!frame.found() || !options.found() || !pause.found() || !goToOptions.found() ||
                !zx::zaxis::findObject(edited, "Menu", pause).found() || !zx::zaxis::findObject(edited, "Show", pause).found())
            {
                std::cerr << "[aggressive-inline] Multiplayer.zsc: anchors not found, pause settings page not added" << std::endl;
                return false;
            }
            edited.insert(options.end, zx::zaxis::indentBlock(withRows(kPausePage, kPauseLayout, 1), zx::zaxis::indentAt(edited, options.begin)));
            edited.insert(goToOptions.end, zx::zaxis::indentBlock(kPauseGoToPcSettings, zx::zaxis::indentAt(edited, goToOptions.begin)));
            pause = zx::zaxis::findObject(edited, "MultiplayerUI/PauseMenus/Frame/Pause");
            zx::zaxis::Span show = zx::zaxis::findObject(edited, "Show", pause);
            const std::string showTabs(static_cast<size_t>(zx::zaxis::indentAt(edited, show.begin) + 2), '\t');
            bool ok = zx::zaxis::replaceIn(edited, show, showTabs + "SETFOCUS Menu\r\n",
                                    showTabs + "SETFOCUS Menu\r\n" + showTabs + "CALL pcSettingsPauseShown .Pause\r\n");
            zx::zaxis::Span menu = zx::zaxis::findObject(edited, "Menu", pause);
            const std::string t(static_cast<size_t>(zx::zaxis::indentAt(edited, menu.begin) + 2), '\t');
            // Items 0-5 are the game's: Resume Game, Game Select, Trick List,
            // Sound Options, Level Select, Exit Game (its script falls
            // through to End). After them Photo Mode 6, Settings 7, Quit to
            // Desktop 8, their scripts before Exit Game's label.
            ok = ok && zx::zaxis::replaceIn(edited, menu, t + "Exit Game\r\n",
                                     t + "Exit Game\r\n" + t + "Photo Mode\r\n" + t + "Settings\r\n" + t + "Quit to Desktop\r\n");
            ok = ok && zx::zaxis::replaceIn(edited, menu, t + ":5\r\n",
                                     t + ":6\r\n" + t + "CALL pcPhotoMode\r\n" + t + "GOTO End\r\n" + t + ":7\r\n" + t +
                                         "PLAY .GoToPcSettings\r\n" + t + "GOTO End\r\n" + t + ":8\r\n" + t +
                                         "CALL pcQuitToDesktop\r\n" + t + "GOTO End\r\n" + t + ":5\r\n");
            if (!ok)
            {
                std::cerr << "[aggressive-inline] Multiplayer.zsc: menu lines not found, pause settings page not added" << std::endl;
                return false;
            }
            // Restart Run (the game the players chose, again: pcRestartRun at
            // the next level update) and Restart Level (an action like
            // InGame.zsc's, the level loaded again with its game menu) first
            // of the port's block, after Exit Game: items 6 and 7, Photo
            // Mode, Settings and Quit to Desktop two further down (8, 9, 10).
            {
                std::string withRestart = edited;
                zx::zaxis::Span restartMenu = zx::zaxis::findObject(withRestart, "MultiplayerUI/PauseMenus/Frame/Pause/Menu");
                bool restartOk = restartMenu.found();
                if (restartOk)
                {
                    withRestart.insert(zx::zaxis::objectEnd(withRestart, restartMenu),
                                       zx::zaxis::indentBlock(kRestartAction, zx::zaxis::indentAt(withRestart, restartMenu.begin) + 1));
                    restartMenu = zx::zaxis::findObject(withRestart, "MultiplayerUI/PauseMenus/Frame/Pause/Menu");
                }
                for (int k = 8; k >= 6 && restartOk; --k)
                    restartOk = zx::zaxis::replaceIn(withRestart, restartMenu, t + ":" + std::to_string(k) + "\r\n",
                                              t + ":" + std::to_string(k + 2) + "\r\n");
                restartOk = restartOk &&
                            zx::zaxis::replaceIn(withRestart, restartMenu, t + "Exit Game\r\n" + t + "Photo Mode\r\n",
                                          t + "Exit Game\r\n" + t + "Restart Run\r\n" + t + "Restart Level\r\n" + t +
                                              "Photo Mode\r\n") &&
                            zx::zaxis::replaceIn(withRestart, restartMenu, t + ":8\r\n",
                                          t + ":6\r\n" + t + "PLAY .Hide\r\n" + t + "CALL inlinePauseModeExit\r\n" + t +
                                              "CALL pcRestartRun\r\n" + t + "GOTO End\r\n" + t + ":7\r\n" + t +
                                              "PLAY PcRestartGame\r\n" + t + "GOTO End\r\n" + t + ":8\r\n");
                if (restartOk)
                    edited = std::move(withRestart);
                else
                    std::cerr << "[aggressive-inline] Multiplayer.zsc: pause menu lines not found, no Restart items" << std::endl;
            }
            if (!patchConfirm(edited, "MultiplayerUI"))
                std::cerr << "[aggressive-inline] Multiplayer.zsc: pause menu lines not found, no \"Are you sure?\"" << std::endl;
            text = std::move(edited);
            return true;
        }

        // Background.Main.PcQuit: "Quit to Desktop?" over the main menu
        // (Escape or Back there, caught by PcSniffer), and the poll for
        // Settings... requests.
        const char *const kQuitPrompt = R"ZSC(OBJECT "guiGroup" "PcQuit"
{
	//////////////////////////////
	// Property
	OnCreate	"SET Visible {0}"
	Visible	0
	// Over the movie window (z 198, behind the rest).
	Position	352.000000 150.000000 0.000000
	OBJECT "guiImage" "Shade"
	{
		//////////////////////////////
		// Property
		Visible	1
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		TopLeftColor	0 0 0 220
		BottomLeftColor	0 0 0 220
		TopRightColor	0 0 0 220
		BottomRightColor	0 0 0 220
		Width	340
		Height	126
		UScale	1
		VScale	1
	}
	OBJECT "guiLabel" "Title"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	0.000000 -36.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"CommonFonts.fnt_ovr_trick"
		Color	185 151 16 255
		Text	"Quit to Desktop?"
		Angle	0
		Monospace	0
	}
	OBJECT "guiMenu" "Choice"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	0.000000 -10.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Horizontal	0
		HorizontalSpacing	10
		WrapAround	1
		VisibleCount	2
		SelectedIndex	0
		SelectedText	"No"
		HideSelectionOnLostFocus	0
		Charset	"CommonFonts.fnt_ovr_trick_noborder"
		Spacing	1.250000
		Alignment	1
		Color	102 164 223 255
		SelectedColor	234 234 234 255
		DisabledColor	128 128 128 255
		Items	{
			No
			Yes
			}
		OnCancel	"PLAY .Hide"
		OnItemFocus	"SET Charset {\"CommonFonts.fnt_ovr_trick\"}"
		OnItemUnFocus	"SET Charset {\"CommonFonts.fnt_ovr_trick_noborder\"}"
		OnItemSelect	{
			Goto SelectedText
			Goto End
			:Yes
			CALL pcQuitToDesktop
			Goto End
			:No
			PLAY .Hide
			:End
			}
	}
	ACTION "Interpolate" "Show"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnStart	{
			KILLFOCUS
			SET Visible {1}
			SET Choice.SelectedIndex {0}
			}
		OnUpdate	"LINEAR Scale {1.0 0.0 1.0} {1.0 1.0 1.0} Show.Ratio"
		OnEnd	"SETFOCUS Choice"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	8
	}
	ACTION "Script" "Hide"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		Script	{
			KILLFOCUS
			SET Visible {0}
			SETFOCUS .Menu
			}
	}
}
OBJECT "guiKeySniffer" "PcSniffer"
{
	//////////////////////////////
	// Property
	OnCreate	"SET Active {0}"
	Visible	1
	Active	0
	Flags	3
	EnableAnalogDir	0
	OnCancel	"PLAY .PcQuit.Show"
	OnStart	"CALL pcMainMenuStart ."
}
ACTION "Interpolate" "PcPoll"
{
	//////////////////////////////
	// Property
	StartDelay	0
	OnUpdate	"CALL pcFrontEndPoll .Main"
	InterpolateMode	0
	LoopMode	2
	IntervalMode	1
	IntervalValue	30
}
)ZSC";

        // Front end additions on top of the settings screen: Quit to Desktop
        // and the focus natives. False if an anchor is missing.
        bool patchFrontEndQuit(std::string &text)
        {
            zx::zaxis::Span options = zx::zaxis::findObject(text, "FrontEnd/Background/Options");
            zx::zaxis::Span optionsMenu = zx::zaxis::findObject(text, "Menu", options);
            zx::zaxis::Span main = zx::zaxis::findObject(text, "FrontEnd/Background/Main");
            zx::zaxis::Span mainMenu = zx::zaxis::findObject(text, "Menu", main);
            if (!optionsMenu.found() || !main.found() || !mainMenu.found())
            {
                std::cerr << "[aggressive-inline] FrontEnd.zsc: " << (optionsMenu.found() ? "" : "Options.Menu ")
                          << (main.found() ? "" : "Main ") << (mainMenu.found() ? "" : "Main.Menu ") << "not found" << std::endl;
                return false;
            }
            const std::string optionTabs(static_cast<size_t>(zx::zaxis::indentAt(text, optionsMenu.begin) + 1), '\t');
            if (!zx::zaxis::replaceIn(text, optionsMenu, optionTabs + "OnGetFocus\t\"SET PreviousIndex SelectedIndex\"\r\n",
                               optionTabs + "OnGetFocus\t{\r\n" + optionTabs + "\tSET PreviousIndex SelectedIndex\r\n" +
                                   optionTabs + "\tCALL pcOptionsFocus .\r\n" + optionTabs + "\t}\r\n" + optionTabs +
                                   "OnLostFocus\t\"CALL pcMenuBlur .\"\r\n"))
            {
                std::cerr << "[aggressive-inline] FrontEnd.zsc: Options.Menu has no OnGetFocus line" << std::endl;
                return false;
            }
            // Player and level select (and the park editor's dialog before
            // level select) say when they have the focus, so Settings... can
            // go back from them to the main menu. Later positions first.
            {
                std::string withBack = text;
                zx::zaxis::Span parkMenu = zx::zaxis::findObject(withBack, "FrontEnd/Background/LevelSelect/ParkEditorDialog/ParkEditorMenu");
                const std::string p(static_cast<size_t>(parkMenu.found() ? zx::zaxis::indentAt(withBack, parkMenu.begin) + 1 : 0), '\t');
                bool backOk = parkMenu.found() &&
                              zx::zaxis::replaceIn(withBack, parkMenu, p + "OnGetFocus\t\"SET SelectedIndex {0}\"\r\n",
                                            p + "OnGetFocus\t{\r\n" + p + "\tSET SelectedIndex {0}\r\n" + p +
                                                "\tCALL pcBackFocus .\r\n" + p + "\t}\r\n" + p +
                                                "OnLostFocus\t\"CALL pcMenuBlur .\"\r\n");
                zx::zaxis::Span levelMenu = zx::zaxis::findObject(withBack, "FrontEnd/Background/LevelSelect/Menu");
                const std::string l(static_cast<size_t>(levelMenu.found() ? zx::zaxis::indentAt(withBack, levelMenu.begin) + 1 : 0), '\t');
                backOk = backOk && levelMenu.found() &&
                         zx::zaxis::replaceIn(withBack, levelMenu, l + "OnCancel\t{\r\n",
                                       l + "OnGetFocus\t\"CALL pcBackFocus .\"\r\n" + l +
                                           "OnLostFocus\t\"CALL pcMenuBlur .\"\r\n" + l + "OnCancel\t{\r\n");
                zx::zaxis::Span skater = zx::zaxis::findObject(withBack, "FrontEnd/Background/CharacterSelect/Menu/SkaterSelector");
                const std::string c(static_cast<size_t>(skater.found() ? zx::zaxis::indentAt(withBack, skater.begin) + 1 : 0), '\t');
                backOk = backOk && skater.found() &&
                         zx::zaxis::replaceIn(withBack, skater, c + "OnLostFocus\t\"SET Stats.ScrollBar.Active {0}\"\r\n",
                                       c + "OnLostFocus\t{\r\n" + c + "\tSET Stats.ScrollBar.Active {0}\r\n" + c +
                                           "\tCALL pcMenuBlur .\r\n" + c + "\t}\r\n") &&
                         zx::zaxis::replaceIn(withBack, skater, c + "\tSET Stats.ScrollBar.Active {1}\r\n",
                                       c + "\tSET Stats.ScrollBar.Active {1}\r\n" + c + "\tCALL pcBackFocus .\r\n");
                if (backOk)
                    text = std::move(withBack);
                else
                    std::cerr << "[aggressive-inline] FrontEnd.zsc: player/level select anchors not found, Settings... not from there"
                              << std::endl;
            }
            // Later positions first: the Main group's end, then its menu.
            main = zx::zaxis::findObject(text, "FrontEnd/Background/Main");
            text.insert(zx::zaxis::objectEnd(text, main), zx::zaxis::indentBlock(kQuitPrompt, zx::zaxis::indentAt(text, main.begin) + 1));
            mainMenu = zx::zaxis::findObject(text, "Menu", main);
            const std::string tabs(static_cast<size_t>(zx::zaxis::indentAt(text, mainMenu.begin) + 1), '\t');
            // The menu (a guiToggler) has no Cancel or Start handlers: a key
            // sniffer beside it is active while it has the focus.
            return zx::zaxis::replaceIn(text, mainMenu, tabs + "EnableSound\t1\r\n",
                                 tabs + "EnableSound\t1\r\n" + tabs + "OnGetFocus\t\"CALL pcMainMenuFocus .\"\r\n" + tabs +
                                     "OnLostFocus\t\"CALL pcMainMenuBlur .\"\r\n");
        }

        // Text edits of FrontEnd.zsc. False (and the script left alone) if
        // an anchor is missing.
        bool patchFrontEnd(std::string &text)
        {
            std::string edited = text;
            zx::zaxis::Span options = zx::zaxis::findObject(edited, "FrontEnd/Background/Options");
            zx::zaxis::Span menu = zx::zaxis::findObject(edited, "Menu", options);
            zx::zaxis::Span controller = zx::zaxis::findObject(edited, "FrontEnd/Background/Controller");
            zx::zaxis::Span goToController = zx::zaxis::findObject(edited, "GoToController", options);
            zx::zaxis::Span movieStart = zx::zaxis::findObject(edited, "FrontEnd/Movie/Start");
            if (!options.found() || !menu.found() || !controller.found() || !goToController.found() || !movieStart.found())
            {
                std::cerr << "[aggressive-inline] FrontEnd.zsc: anchors not found, settings screen not added" << std::endl;
                return false;
            }
            // From the end of the file backwards, so earlier spans stay valid.
            const int depth = zx::zaxis::indentAt(edited, controller.begin);
            edited.insert(controller.end, zx::zaxis::indentBlock(withRows(kFrontEndPage, kFrontEndLayout), depth));
            edited.insert(goToController.end, zx::zaxis::indentBlock(kGoToPcSettings, zx::zaxis::indentAt(edited, goToController.begin)));
            if (!zx::zaxis::replaceIn(edited, menu, "SET Label.Text {\"Controller\"}", "SET Label.Text {\"Settings\"}") ||
                !zx::zaxis::replaceIn(edited, menu, "PLAY .GoToController", "PLAY .GoToPcSettings") ||
                !zx::zaxis::replaceIn(edited, movieStart, "PLAY StartAKAMovie", "CALL pcSettingsIntro .Movie"))
            {
                std::cerr << "[aggressive-inline] FrontEnd.zsc: menu lines not found, settings screen not added" << std::endl;
                return false;
            }
            {
                std::string withQuit = edited;
                if (patchFrontEndQuit(withQuit))
                    edited = std::move(withQuit);
                else
                    std::cerr << "[aggressive-inline] FrontEnd.zsc: main menu anchors not found, no Quit to Desktop" << std::endl;
            }
            text = std::move(edited);
            return true;
        }

        // In the collection (ps2_host::menuAvailable: the app has a menu to
        // come back to), the port's Quit to Desktop items and the main
        // menu's question leave for that menu, so they say so: "Exit to
        // Collection" (pcQuitToDesktop calls ps2_host::requestExitToMenu in
        // either app). Every "Quit to Desktop" in the edited scripts is the
        // port's own: the items' texts, the labels their GOTO SelectedText
        // finds and the IF lines comparing SelectedText (all renamed alike),
        // the question's title. The language's words come from the text
        // hooks (aggressive_inline_language.cpp), the English being the key.
        bool exitToCollection(std::string &text)
        {
            static constexpr std::string_view kQuit = "Quit to Desktop", kExit = "Exit to Collection";
            size_t renamed = 0;
            for (size_t at = text.find(kQuit); at != std::string::npos; at = text.find(kQuit, at + kExit.size()))
            {
                text.replace(at, kQuit.size(), kExit);
                ++renamed;
            }
            return renamed != 0u;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            std::string error;
            if (!zx::zaxis::readArchive(bytes, entries, &error))
            {
                std::cerr << "[aggressive-inline] UILIB.ZAL: " << error << std::endl;
                return false;
            }
            // Another language's fonts first: the prompts redraw cells of
            // the font atlases the game loads (zge_prompts.cpp reads them
            // from this patch's result).
            bool changed = aggressive_inline::language::patchUiLibrary(entries);
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (zx::zaxis::scriptName(entry) == "InGameUI")
                {
                    std::string text(entry.data.begin(), entry.data.end());
                    if (patchInGame(text))
                    {
                        entry.data.assign(text.begin(), text.end());
                        changed = true;
                    }
                    continue;
                }
                if (zx::zaxis::scriptName(entry) == "MultiplayerUI")
                {
                    std::string text(entry.data.begin(), entry.data.end());
                    if (patchMultiplayer(text))
                    {
                        entry.data.assign(text.begin(), text.end());
                        changed = true;
                    }
                    continue;
                }
                if (zx::zaxis::scriptName(entry) != "FrontEnd")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                // Two FrontEnd scripts are in the archive; the one the game
                // loads has the Options screen.
                if (text.find("\"GoToController\"") == std::string::npos)
                    continue;
                if (patchFrontEnd(text))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                }
            }
            if (changed && ps2_host::menuAvailable())
                for (zx::zaxis::ArchiveEntry &entry : entries)
                {
                    const std::string script = zx::zaxis::scriptName(entry);
                    if (script != "InGameUI" && script != "MultiplayerUI" && script != "FrontEnd")
                        continue;
                    std::string text(entry.data.begin(), entry.data.end());
                    if (exitToCollection(text))
                        entry.data.assign(text.begin(), text.end());
                }
            if (changed)
                bytes = zx::zaxis::writeArchive(entries);
            return changed;
        }
    }

    void installSettingsMenus(PS2Machine &runtime)
    {
        // On in the app (it sets ZX_GAME_SETTINGS_MENUS=1), off in the
        // developer runner unless asked: the extra widgets and natives move
        // the game's heap, and the logic and replay checks compare heap
        // addresses with PCSX2's.
        if (const char *on = zx::boot::text("ZX_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
        {
            std::cerr << "[aggressive-inline] settings screen off (ZX_GAME_SETTINGS_MENUS=1 adds it)" << std::endl;
            return;
        }
        zge_host::settings::setStyle(kStyle);
        const bool scratch = zge_host::settings::installScriptRunner(runtime, kRunScript);
        engine().registerNative = runtime.lookupFunction(kRegisterNative);
        if (!scratch || !engine().registerNative || !runtime.replaceFunction(kRegisterNative, &registerNativeHook))
        {
            std::cerr << "[aggressive-inline] settings natives not installed" << std::endl;
            return;
        }
        // Entry points of the natives: unused slots of the function table
        // (no code there, so nothing else ever enters them), from the
        // middle of the front end's native registration (registerMenuFunctions,
        // 0x60C bytes) to its end.
        constexpr uint32_t kSlotsFrom = sym::registerMenuFunctions + 0xA0u;
        constexpr uint32_t kSlotsEnd = sym::registerMenuFunctions + 0x60Cu;
        uint32_t address = kSlotsFrom;
        const std::vector<Native> &natives = allNatives();
        for (size_t i = 0; i < natives.size(); ++i)
        {
            while (address < kSlotsEnd && runtime.hasFunction(address))
                address += 4u;
            if (address >= kSlotsEnd || !runtime.registerFunction(address, natives[i].fn))
            {
                std::cerr << "[aggressive-inline] no slot for " << natives[i].name << std::endl;
                break;
            }
            nativeSlots().address[i] = address;
            address += 4u;
        }
        zx::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary, "ai");
        // The pages' room in zgui's heap (aggressive_inline_settings_room.cpp).
        aggressive_inline::settings_room::install(runtime);
        // L2 / R2 on the page (the heading jump), with no line in its scripts.
        if (!zge_host::settings::installPageKeys(runtime, {.drawSniffer = sym::drawGuiKeySniffer,
                                                           .drawGetsMessage = false,
                                                           .findNode = sym::findNode,
                                                           .findTakesKind = true}))
            std::cerr << "[aggressive-inline] the Settings page's L2 / R2 not installed" << std::endl;
        installPrompts(runtime);
        requests().runtime = &runtime;
        // Settings... (Cmd+,) by the engine's own path: in play the game
        // pauses as its Start does and the pause menu's Show opens the page;
        // in the front end, Options and its page.
        aggressive_inline::settings_open::installPlayFrameHook(runtime);
        zge_host::settings_open::install(runtime, {"aggressive-inline", &aggressive_inline::settings_open::inLevel,
                                                   &aggressive_inline::settings_open::playing, &frontEndSettingsRequested,
                                                   &aggressive_inline::settings_open::pausable,
                                                   &aggressive_inline::settings_open::pause,
                                                   &aggressive_inline::settings_open::openFromPause});
    }
}
