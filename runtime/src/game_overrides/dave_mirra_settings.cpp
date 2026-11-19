// Dave Mirra Freestyle BMX 2 (SLUS-20159): the port's Settings page in the
// front end's own Options menu, made of the game's widgets, fonts and
// materials (zge_settings.h: the page's rows and keys, shared with AI and
// BMX XXX).
//
// The front end is UILIB.ZAL's MainMenu script. Its Options frame
// (MainMenu.OptionsFrame) is a guiMenu of five lines, Controller 1 to Save
// Game, whose OnItemSelect plays the action named by the line's index
// ("PLAY *SelectedIndex"); the frame's LockMultiPlayer and
// UnLockMultiPlayer set the same Items again (Controller 2 greyed out with
// one pad). Each gets a sixth line, "Settings", and the menu an action "5"
// that slides the Options frame out and the page in, as its Audio line
// opens the Audio frame. The page (MainMenu.PcSettings) is a frame in the
// look of the Options and Audio frames: the Common library's frame
// materials, its mnu_small font in the menus' grey and gold, the top
// header's title and the bottom header's Back prompt (CommonUI). ZGE2's key
// sniffer has no accept or cancel words: Cross is its Fire4, Triangle its
// Fire1 (zge/input/pads.h's buttons; zgui's Fire1-4 are Triangle, Square,
// Circle, Cross). ZGE2's CALL passes no arguments: the page's functions find
// the page by its path from zgui's root (findNode).
//
// In a level the pause menu gets a SETTINGS line (dave_mirra_restart.cpp:
// the menu's lines are made by its builder) that shows the same page,
// InGameUI.PcSettingsPause, in the pause frame's place.
//
// With the app's settings (PS2X_GAME_SETTINGS_MENUS=1): the page's widgets
// and script functions move the game's heap, and the developer runner's
// checks run without them.

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_settings.cpp.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_zaxis_ui.h"
#include "zge_host.h"
#include "zge_settings.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace dave_mirra::settings_menu
{
    namespace
    {
        namespace zx = ps2x::zaxis;
        using zge_host::settings::Layout;

        // The page's frame: origin at its top left, 480 by 320 (the front end's
        // screen is 512 wide: Init512, Init640 scale it to the picture).
        constexpr Layout kLayout{24.0f, 456.0f, 62.0f, 24.0f, 0.75f, 50.0f, "1.000000 1.000000 1.000000"};

        // The page shown: the front end's or the pause menu's (whichever
        // script is loaded), found from zgui's root by its path.
        constexpr const char *kFrontEndPage = "MainMenu.PcSettings";
        constexpr const char *kPausePage = "InGameUI.PcSettingsPause";
        uint32_t findPage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool &pause)
        {
            const uint32_t root = zge_host::settings::readWord(rdram, sym::dm2::g_uiRoot);
            const zge_host::settings::Scratch scratch = zge_host::settings::scratchNow(ctx);
            const uint32_t name = scratch.strings, stack = scratch.stack;
            for (const char *path : {kFrontEndPage, kPausePage})
            {
                uint32_t found = 0;
                if (root && zge_host::settings::putString(rdram, name, path) &&
                    runtime->callGuestFunction(rdram, ctx, sym::dm2::findNode, {root, name}, stack, &found) && found)
                {
                    pause = path == kPausePage;
                    return found;
                }
            }
            return 0u;
        }

        const zge_host::settings::Style kStyle{
            .visibleRows = 9,
            .frontEnd = kLayout,
            .pause = kLayout,
            .pauseDialog = "PcSettingsPause",
            .findDialog = &findPage, // ZGE2's CALL passes no arguments
            .highlightAt = "SET Highlight.Position {240.0 %.1f 0.0}",
            .nameColor = "192 192 192 255",
            .valueColor = "238 181 44 255",
            .selectedColor = "255 255 255 255",
            .headingColor = "238 181 44 255", // the section title's gold
            .translate = nullptr,
            .rowsType = "guiImage",
            .rowsProperties = "OnCreate\t\"SET Visible {0}\"\nVisible\t0\nPosition\t0.000000 0.000000 0.000000\n"
                              "Scale\t1.000000 1.000000 1.000000\nAlignment\t0\nTopLeftColor\t255 255 255 0\n"
                              "BottomLeftColor\t255 255 255 0\nTopRightColor\t255 255 255 0\n"
                              "BottomRightColor\t255 255 255 0\nWidth\t0\nHeight\t0\nUScale\t1\nVScale\t1\n",
            .rowCharset = "Common.mnu_small",
            .rowScale = "0.800000 0.800000 1.000000",
            .rowLabelTail = "",
        };

        // The page: MainMenu.PcSettings (a sibling of OptionsFrame; Back
        // opens Options again) and InGameUI.PcSettingsPause (a sibling of
        // FramePause; Back opens the pause menu again). @NAME@ and @BACK@
        // are filled in.
        const char *const kPage = R"ZSC(OBJECT "guiFrame" "@NAME@"
{
	//////////////////////////////
	// Property
	Visible	0
	Position	-480.000000 80.000000 0.000000
	Scale	1.000000 1.000000 1.000000
	Alignment	0
	Material_Top	"Common.top"
	Material_TopRight	"Common.topright"
	Material_Center	"Common.center"
	Material_Right	"Common.right"
	Material_Bottom	"Common.bottom"
	Material_BottomRight	"Common.botright"
	Width	480
	Height	320
	//////////////////////////////
	// Action
	ACTION "Interpolate" "Init"
	{
		StartDelay	0
		OnStart	"SET Visible {1}\nCALL pcSettingsOpen"
		OnUpdate	"LINEAR Position {-480 80 0} {16 80 0} Init.Ratio"
		OnEnd	"SET Root.CommonUI.TopHeader.Title.Text {\"Settings\"}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Caption.Text {\"Back\"}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Text {\"3\"}\nPLAY Root.CommonUI.UnlockUser\nPLAY Root.CommonUI.DisplayButton1\nSET Highlight.Visible {1}\nSET Rows.Visible {1}\nSET Help.Visible {1}\nSET Help2.Visible {1}\nSET Section.Visible {1}\nCALL pcSettingsOpen\nSET KeySniffer.Active {1}\nSETFOCUS KeySniffer"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	20
		Ratio	1
	}
	ACTION "Interpolate" "DeInit"
	{
		StartDelay	0
		OnStart	"KILLFOCUS\nSET KeySniffer.Active {0}\nSET Highlight.Visible {0}\nSET Rows.Visible {0}\nSET Help.Visible {0}\nSET Help2.Visible {0}\nSET Section.Visible {0}\nSET MoreUp.Visible {0}\nSET MoreDown.Visible {0}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Caption.Text {\"Select\"}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Text {\"1\"}\nPLAY Root.CommonUI.LockUser"
		OnUpdate	"LINEAR Position {16 80 0} {-480 80 0} DeInit.Ratio"
		OnEnd	"SET Visible {0}"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	20
		Ratio	1
	}
	ACTION "Script" "GoBack"
	{
		StartDelay	0
		Script	"PLAY DeInit\nPLAY .@BACK@.Init"
	}
	ACTION "Interpolate" "TickA"
	{
		StartDelay	0
		OnEnd	"CALL pcSettingsTick"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	2
		Ratio	1
	}
	ACTION "Interpolate" "TickB"
	{
		StartDelay	0
		OnEnd	"CALL pcSettingsTick"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	2
		Ratio	1
	}
	OBJECT "guiImage" "Highlight"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	240.000000 62.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		TopLeftColor	238 181 44 56
		BottomLeftColor	238 181 44 56
		TopRightColor	238 181 44 56
		BottomRightColor	238 181 44 56
		Width	456
		Height	24
		UScale	1
		VScale	1
	}
@ROWS@	OBJECT "guiLabel" "MoreUp"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	466.000000 42.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.mnu_tiny"
		Color	238 181 44 255
		Text	"^"
	}
	OBJECT "guiLabel" "MoreDown"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	466.000000 266.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.mnu_tiny"
		Color	238 181 44 255
		Text	"v"
	}
	OBJECT "guiLabel" "Section"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	240.000000 24.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.mnu_small"
		Color	238 181 44 255
		Text	"Display"
		OBJECT "guiLabel" "LeftArrow"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	-120.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			Spacing	1.250000
			Tracking	1
			Charset	"Common.mnu_small"
			Color	192 192 192 255
			Text	"<"
		}
		OBJECT "guiLabel" "RightArrow"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	120.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			Spacing	1.250000
			Tracking	1
			Charset	"Common.mnu_small"
			Color	192 192 192 255
			Text	">"
		}
		OBJECT "guiLabel" "Hint"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	226.000000 2.000000 0.000000
			Scale	0.600000 0.600000 1.000000
			Alignment	18
			Spacing	1.250000
			Tracking	1
			Charset	"Common.mnu_small"
			Color	192 192 192 255
			Text	"L1 R1  Section"
		}
	}
	OBJECT "guiLabel" "Help"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	240.000000 284.000000 0.000000
		Scale	0.750000 0.750000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.mnu_small"
		Color	192 192 192 255
		Text	""
	}
	OBJECT "guiLabel" "Help2"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	240.000000 300.000000 0.000000
		Scale	0.750000 0.750000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.mnu_small"
		Color	192 192 192 255
		Text	""
	}
	OBJECT "guiKeySniffer" "KeySniffer"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Active {0}"
		Visible	1
		Scale	1.000000 1.000000 1.000000
		Active	0
		Flags	3
		CurCtrl	0
		LeftAxisX	0
		LeftAxisY	0
		RightAxisX	0
		RightAxisY	0
		OnUpKey	"CALL pcSettingsUp"
		OnDownKey	"CALL pcSettingsDown"
		OnLeftKey	"CALL pcSettingsLeft"
		OnRightKey	"CALL pcSettingsRight"
		OnFire4	"CALL pcSettingsAccept"
		OnLTrig1	"CALL pcSettingsPrev"
		OnRTrig1	"CALL pcSettingsNext"
		OnFire1	"PLAY .GoBack"
		OnStart	"PLAY .GoBack"
	}
}
)ZSC";

        std::string page(const char *name, const char *back)
        {
            std::string text = kPage;
            for (const auto &[from, to] : {std::pair<std::string, std::string>{"@NAME@", name}, {"@BACK@", back}})
                for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
                    text.replace(at, from.size(), to);
            return zge_host::settings::withRows(text, kLayout, 1);
        }

        // The Options menu's sixth line opens the page.
        const char *const kOpenAction = R"ZSC(ACTION "Script" "5"
{
	StartDelay	0
	Script	"PLAY .DeInit\nPLAY ..PcSettings.Init"
}
)ZSC";

        // Text edits of the MainMenu script. False (and the script left
        // alone) if an anchor is missing.
        bool patchMainMenu(std::string &text)
        {
            std::string edited = text;
            zx::Span options = zx::findObject(edited, "MainMenu/OptionsFrame");
            zx::Span menu = zx::findObject(edited, "MainMenu/OptionsFrame/Menu");
            if (!options.found() || !menu.found())
                return false;
            // Later positions first: the page after the frame, the menu's
            // action and lines, then the frame's height (before the menu).
            edited.insert(options.end,
                          zx::indentBlock(page("PcSettings", "OptionsFrame"), zx::indentAt(edited, options.begin)));
            edited.insert(zx::objectEnd(edited, menu), zx::indentBlock(kOpenAction, zx::indentAt(edited, menu.begin) + 1));
            menu = zx::findObject(edited, "MainMenu/OptionsFrame/Menu");
            bool ok = zx::replaceIn(edited, menu, "\\nSave Game\"\r\n", "\\nSave Game\\nSettings\"\r\n") &&
                      zx::replaceIn(edited, menu, "\tVisibleCount\t5\r\n", "\tVisibleCount\t6\r\n");
            options = zx::findObject(edited, "MainMenu/OptionsFrame");
            ok = ok && zx::replaceIn(edited, options, "\tHeight\t200\r\n", "\tHeight\t230\r\n");
            // The two actions that set the lines again (one pad, two pads).
            zx::Span frame = zx::findObject(edited, "MainMenu/MainMenuFrame");
            ok = ok && zx::replaceIn(edited, frame, "\\nSave Game\\\"}", "\\nSave Game\\nSettings\\\"}") &&
                 zx::replaceIn(edited, frame, "\\nSave Game\\\"}", "\\nSave Game\\nSettings\\\"}");
            if (!ok)
                return false;
            text = std::move(edited);
            return true;
        }

        // The InGameUI script: the page after the pause frame.
        bool patchInGame(std::string &text)
        {
            zx::Span pause = zx::findObject(text, "InGameUI/FramePause");
            if (!pause.found())
                return false;
            text.insert(pause.end, zx::indentBlock(page("PcSettingsPause", "FramePause"), zx::indentAt(text, pause.begin)));
            return true;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::ArchiveEntry> entries;
            if (!zx::readArchive(bytes, entries))
                return false;
            bool changed = false;
            for (zx::ArchiveEntry &entry : entries)
            {
                if (zx::scriptName(entry) == "InGameUI")
                {
                    std::string text(entry.data.begin(), entry.data.end());
                    if (patchInGame(text))
                    {
                        entry.data.assign(text.begin(), text.end());
                        changed = true;
                        std::cout << "[dave-mirra] pause menu: Settings" << std::endl;
                    }
                    else
                    {
                        std::cerr << "[dave-mirra] InGameUI is not the expected script: no Settings page in the pause menu"
                                  << std::endl;
                    }
                    continue;
                }
                if (zx::scriptName(entry) != "MainMenu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                if (patchMainMenu(text))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                    std::cout << "[dave-mirra] Options: Settings" << std::endl;
                }
                else
                {
                    std::cerr << "[dave-mirra] MainMenu is not the expected script: no Settings in Options" << std::endl;
                }
            }
            if (changed)
                bytes = zx::writeArchive(entries);
            return changed;
        }
    }
}

namespace dave_mirra::settings_menu
{
    void openFromPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t root)
    {
        std::cout << "[dave-mirra] pause menu: Settings" << std::endl;
        zge_host::settings::runScript(rdram, ctx, runtime, root,
                                      {"PLAY Root.InGameUI.FramePause.DeInit", "PLAY Root.InGameUI.PcSettingsPause.Init"});
    }
}

namespace dave_mirra
{
    void installSettingsMenus(PS2Runtime &runtime)
    {
        if (const char *on = ps2x::boot::text("PS2X_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        using namespace settings_menu;
        zge_host::settings::setStyle(kStyle);
        if (!zge_host::settings::installScriptRunner(runtime, sym::dm2::runScript))
        {
            std::cerr << "[dave-mirra] the Settings page's scratch memory not reserved" << std::endl;
            return;
        }
        for (const zge_host::settings::Function &function : zge_host::settings::pageFunctions())
            if (!zge_host::addScriptFunction(runtime, scriptFunctions(), function.name, function.fn))
                return;
        ps2x::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary);
        restart::addSettingsLine(runtime);
    }
}
