// BMX XXX (SLUS-20415): the port's Settings page in the front end's own
// Options menu, made of the game's widgets, fonts and materials
// (zge_settings.h: the page's rows and keys, shared with AI and DM2).
//
// The front end is UILIB.ZAL's UI_menu script. Its Options
// (CommonBackground.FrameOptions) is a guiMenu in the OPTION frame whose six
// lines bmxmenuscriptGetOptionsMenuItems makes when the menu comes in (Load
// and Save to Language: "$KSTR_COM_OPTIONS_OPT_00" and the like) and whose
// choice goes to the menus' handler (bmxuiHandler, the front end's menu
// state: games/bmx/states/front_end_menu.cpp, kMenuOptions). Here the menu
// gets a seventh line, SETTINGS (pcSettingsOptionsLine, after the game's
// own, adds it to the Items it made: as many lines as this build shows),
// and its OnItemSelect plays FrameOptions' GoToPcSettings for that line (by
// its SelectedText) before the handler would see it: Options slides out
// and the page in (CommonBackground.PcSettings), back to Options after, the menus' state left at Options throughout. The page is in
// the look of Options and Audio: a guiFrame of the Common library's corner_TL
// and solid in the boxes' grey, its box, font_normal in grey with the
// menus' amber for the selected row, the Common arrows. ZGE4's key sniffer
// has accept and cancel words, and its CALL passes the nodes named (".").
//
// In a level the pause menu (UI_overlay.FramePause.Menu, made by
// pauseScreenMessage) gets a SETTINGS line (bmx_xxx_exit.cpp, as its
// RESTART and PHOTO MODE lines): chosen, its lines fade out and the same
// page, UI_overlay.PcSettingsPause, comes in; Back brings the lines back.
//
// With the app's settings (ZX_GAME_SETTINGS_MENUS=1): the page's widgets
// and script functions move the game's heap, and the developer runner's
// checks run without them.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_settings.cpp.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/disc_patch.h"
#include "runtime/zaxis_ui.h"
#include "overrides/zge_host.h"
#include "overrides/zge_settings.h"

#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace bmx_xxx::settings_menu
{
    namespace
    {
        using zge_host::settings::Layout;

        constexpr int kRegRa = 31;
        constexpr const char *kLine = "\nSETTINGS";
        constexpr size_t kItemsSize = 0x400u; // the game's Items buffer (bmxmenuscriptGetOptionsMenuItems)

        // The page's frame: origin at its top centre, 560 by 300.
        constexpr Layout kLayout{-262.0f, 262.0f, 58.0f, 24.0f, 0.65f, 52.0f, "0.800000 0.700000 1.000000"};
        // The pause menu's, in UI_overlay's units (inOverlay).
        constexpr float kOverlayX = 512.0f / 640.0f, kOverlayY = 448.0f / 480.0f;
        constexpr Layout kPauseLayout{kLayout.nameX * kOverlayX, kLayout.valueX * kOverlayX, kLayout.rowTop * kOverlayY,
                                      kLayout.rowPitch * kOverlayY, kLayout.helpScale * kOverlayX, kLayout.helpChars,
                                      kLayout.headingScale};

        const zge_host::settings::Style kStyle{
            .visibleRows = 9,
            .frontEnd = kLayout,
            .pause = kPauseLayout,
            .pauseDialog = "PcSettingsPause",
            .findDialog = nullptr, // the scripts pass the page
            .highlightAt = "SET Highlight.Position.y {%.1f}",
            .nameColor = "192 192 192 255",
            .valueColor = "192 192 192 255",
            .selectedColor = "255 200 120 255",
            .headingColor = "192 192 192 255", // the section title's grey
            .translate = nullptr,
            .rowsType = "guiGroup",
            .rowsProperties = "OnCreate\t\"SET Visible {0}\"\nVisible\t0\n",
            .rowCharset = "Common.font_normal",
            .rowScale = "0.650000 0.600000 1.000000",
            .rowLabelTail = "Angle\t0\nMonospace\t0\n",
        };

        // CommonBackground.PcSettings, after FrameOptions (whose OPTION frame
        // hides FrameOptions as it slides out), its title where Options' is.
        // Back gives Options the focus once the page is out, so the Back
        // press is not Options' too.
        const char *const kPage = R"ZSC(OBJECT "guiFrame" "@NAME@"
{
	//////////////////////////////
	// Property
	OnCreate	"SET Visible {0}"
	Visible	0
	Position	800.000000 85.000000 -10.000000
	Scale	1.000000 1.000000 1.000000
	Alignment	1
	Color	37 37 37 @FRAMEALPHA@
	Material_TopLeft	"Common.corner_TL"
	Material_Top	"Common.solid"
	Width	560
	Height	324
	OBJECT "guiImage" "BOX"
	{
		//////////////////////////////
		// Property
		Visible	1
		Position	-0.500000 29.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	1
		TopLeftColor	255 255 255 @BOXALPHA@
		BottomLeftColor	255 255 255 @BOXALPHA@
		TopRightColor	255 255 255 @BOXALPHA@
		BottomRightColor	255 255 255 @BOXALPHA@
		Material	"Common.box"
		Width	556
		Height	292
		UScale	1
		VScale	1
	}
	OBJECT "guiImage" "Highlight"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	0.000000 58.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		TopLeftColor	138 80 0 160
		BottomLeftColor	138 80 0 160
		TopRightColor	138 80 0 160
		BottomRightColor	138 80 0 160
		Material	"Common.solid"
		Width	536
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
		Position	262.000000 40.000000 -2.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		TopLeftColor	255 255 255 255
		BottomLeftColor	255 255 255 255
		TopRightColor	255 255 255 255
		BottomRightColor	255 255 255 255
		Material	"Common.Arrow_up"
		Width	16
		Height	16
		UScale	1
		VScale	1
	}
	OBJECT "guiImage" "MoreDown"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	262.000000 270.000000 -2.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	17
		TopLeftColor	255 255 255 255
		BottomLeftColor	255 255 255 255
		TopRightColor	255 255 255 255
		BottomRightColor	255 255 255 255
		Material	"Common.Arrow_dwn"
		Width	16
		Height	16
		UScale	1
		VScale	1
	}
	OBJECT "guiLabel" "Section"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	0.000000 15.000000 -2.000000
		Scale	0.800000 0.700000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.font_normal"
		Color	192 192 192 255
		Text	"Display"
		Angle	0
		Monospace	0
		OBJECT "guiImage" "LeftArrow"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	-140.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"Common.Arrow_lft"
			Width	16
			Height	16
			UScale	1
			VScale	1
		}
		OBJECT "guiImage" "RightArrow"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	140.000000 0.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			TopLeftColor	255 255 255 255
			BottomLeftColor	255 255 255 255
			TopRightColor	255 255 255 255
			BottomRightColor	255 255 255 255
			Material	"Common.Arrow_rt"
			Width	16
			Height	16
			UScale	1
			VScale	1
		}
		OBJECT "guiLabel" "Hint"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	272.000000 1.000000 0.000000
			Scale	0.500000 0.450000 1.000000
			Alignment	18
			Spacing	1.250000
			Tracking	1
			Charset	"Common.font_normal"
			Color	192 192 192 255
			Text	"L1 R1  Section"
			Angle	0
			Monospace	0
		}
	}
	OBJECT "guiLabel" "Title"
	{
		//////////////////////////////
		// Property
		Visible	@TITLE@
		Position	110.000000 -64.000000 -2.000000
		Scale	0.900000 0.700000 1.000000
		Alignment	1
		Spacing	1.250000
		Tracking	1
		Charset	"Common.font_normal"
		Color	192 192 192 150
		Text	"SETTINGS"
		Angle	0
		Monospace	0
	}
	OBJECT "guiLabel" "Help"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET Visible {0}"
		Visible	0
		Position	0.000000 284.000000 -2.000000
		Scale	0.650000 0.650000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.font_normal"
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
		Position	0.000000 301.000000 -2.000000
		Scale	0.650000 0.650000 1.000000
		Alignment	17
		Spacing	1.250000
		Tracking	1
		Charset	"Common.font_normal"
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
		OnLTrig1	"CALL pcSettingsPrev ."
		OnRTrig1	"CALL pcSettingsNext ."
		OnCancel	"PLAY .GoBack"
		OnStart	"PLAY .GoBack"
	}
	ACTION "Interpolate" "TickA"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnEnd	"CALL pcSettingsTick .@NAME@"
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
		OnEnd	"CALL pcSettingsTick .@NAME@"
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	2
	}
	ACTION "Interpolate" "MoveIn"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		OnStart	{
			SET Visible {1}
			CALL pcSettingsOpen .@NAME@
			}
		OnUpdate	"LINEAR Position {@FROM@ 85 -10} {320 85 -10} MoveIn.Ratio"
		OnEnd	{
			SET Highlight.Visible {1}
			SET Rows.Visible {1}
			SET Help.Visible {1}
			SET Help2.Visible {1}
			SET Section.Visible {1}
			CALL pcSettingsOpen .@NAME@
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
			SET Highlight.Visible {0}
			SET Rows.Visible {0}
			SET Help.Visible {0}
			SET Help2.Visible {0}
			SET Section.Visible {0}
			SET MoreUp.Visible {0}
			SET MoreDown.Visible {0}
			}
		OnUpdate	"LINEAR Position {320 85 -10} {@FROM@ 85 -10} MoveOut.Ratio"
		OnEnd	{
			SET Visible {0}
@BACK@			}
		InterpolateMode	0
		LoopMode	0
		IntervalMode	1
		IntervalValue	20
	}
	ACTION "Script" "GoBack"
	{
		//////////////////////////////
		// Property
		StartDelay	0
		Script	"PLAY MoveOut"
	}
}
)ZSC";

        // UI_overlay draws a widget at the depth its Position's z gives it,
        // tested against the level's (GEQUAL): its own widgets sit at z 0,
        // which is the nearest (GS Z 0x7FFFFFFF), over the paused scene; a
        // widget with a z below 0 is drawn at the farthest (Z 0) and shows
        // only where the scene drew nothing (the page's frame at -10 and
        // everything in it: the rows "smeared" across the picture). And its
        // screen is 512 by 448, UI_menu's 640 by 480, a widget's Scale its
        // own (not its children's). The pause menu's page is the front
        // end's in those units: every z at 0, as UI_overlay's own (its order
        // the tree's: the frame, then its box, highlight and rows), every
        // place and scale (the frame's size) by kOverlayX across and
        // kOverlayY down.

        // A property of the page's script whose numbers inOverlay scales:
        // each by its factor (0: made 0), as many as it has.
        struct OverlayScaled
        {
            const char *property;
            int count;
            float factors[3];
        };
        constexpr OverlayScaled kOverlayScaled[] = {
            {"Position\t", 3, {kOverlayX, kOverlayY, 0.0f}},
            {"\tScale\t", 3, {kOverlayX, kOverlayY, 1.0f}}, // a widget's size is its Width and Height by its Scale
            {"LINEAR Position {", 3, {kOverlayX, kOverlayY, 0.0f}},
            {"} {", 3, {kOverlayX, kOverlayY, 0.0f}},
        };

        // The numbers from `at` on, scaled and written back; where they end.
        size_t scaleNumbers(std::string &text, size_t at, const OverlayScaled &scaled)
        {
            std::string out;
            size_t from = at;
            for (int i = 0; i < scaled.count; ++i)
            {
                const char *begin = text.c_str() + from;
                char *stop = nullptr;
                const float value = std::strtof(begin, &stop);
                if (stop == begin)
                    break;
                char number[32];
                std::snprintf(number, sizeof(number), "%s%.6f", out.empty() ? "" : " ",
                              static_cast<double>(value * scaled.factors[i]));
                out += number;
                from = static_cast<size_t>(stop - text.c_str());
            }
            text.replace(at, from - at, out);
            return at + out.size();
        }

        // The frame's own: a guiFrame draws its border at its Width and
        // Height (its Scale left as it is).
        constexpr OverlayScaled kOverlayFrame[] = {
            {"Position\t", 3, {kOverlayX, kOverlayY, 0.0f}}, {"Width\t", 1, {kOverlayX}}, {"Height\t", 1, {kOverlayY}}};

        void inOverlay(std::string &text)
        {
            // The frame's properties come before its first child.
            const size_t children = text.find("\tOBJECT ");
            std::string frame = text.substr(0, children), rest = children == std::string::npos ? std::string() : text.substr(children);
            for (const OverlayScaled &scaled : kOverlayFrame)
                if (const size_t at = frame.find(scaled.property); at != std::string::npos)
                    scaleNumbers(frame, at + std::strlen(scaled.property), scaled);
            for (const OverlayScaled &scaled : kOverlayScaled)
                for (size_t at = rest.find(scaled.property); at != std::string::npos; at = rest.find(scaled.property, at))
                    at = scaleNumbers(rest, at + std::strlen(scaled.property), scaled);
            text = frame + rest;
        }

        // The page as the front end's or the pause menu's: its name, what
        // Back shows again once it is out, and whether it is the pause
        // menu's. There the game draws the menus over the paused picture
        // without drawing it again: the page comes in where it stays, its
        // frame and box opaque (else what it drew before shows through: a
        // value's old text, the slide's trail), and it has no title (the
        // pause menu has no header for it).
        std::string page(const char *name, const char *back, bool pause)
        {
            std::string text = kPage;
            const std::pair<std::string, std::string> fills[] = {
                {"@NAME@", name}, {"@BACK@", back}, {"@TITLE@", pause ? "0" : "1"}, {"@FROM@", pause ? "320" : "800"},
                {"@FRAMEALPHA@", pause ? "255" : "225"}, {"@BOXALPHA@", pause ? "255" : "200"}};
            for (const auto &[from, to] : fills)
                for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
                    text.replace(at, from.size(), to);
            text = zge_host::settings::withRows(text, kLayout, 1);
            if (pause)
                inOverlay(text);
            return text;
        }
        const char *const kFrontEndBack = "\t\t\tPLAY .FrameOptions.Show\n\t\t\tSETFOCUS .FrameOptions.OPTION.Menu\n";
        // The pause menu's box (FrameNone.Box, its IBox) and footer
        // (FrameSelect) are hidden while the page is up (openFromPause) and
        // shown again as it goes.
        const char *const kPauseBack = "\t\t\tSET .FramePause.Title.Visible {1}\n\t\t\tSET .FrameSelect.Visible {1}\n"
                                       "\t\t\tSET .FrameNone.Box.Visible {1}\n\t\t\tSET .FrameNone.Box.IBox.Visible {1}\n"
                                       "\t\t\tPLAY .FramePause.Menu.Init\n\t\t\tSETFOCUS .FramePause.Menu\n";

        // FrameOptions' way to the page: Options out, the page in.
        const char *const kGoTo = R"ZSC(ACTION "Script" "GoToPcSettings"
{
	//////////////////////////////
	// Property
	StartDelay	0
	Script	{
		KILLFOCUS
		PLAY Hide
		PLAY .PcSettings.MoveIn
		}
}
)ZSC";

        // pcSettingsOptionsLine (Items): the Options menu's Items, as
        // bmxmenuscriptGetOptionsMenuItems has just made them, with the
        // SETTINGS line after them.
        void optionsLineFunction(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t items = zge_host::settings::firstArgument(rdram, ctx);
            const zge_host::settings::Scratch scratch = zge_host::settings::scratchNow(ctx);
            if (items && scratch.size >= kItemsSize &&
                runtime->callGuestFunction(rdram, ctx, sym::bmx::variableToText, {items, scratch.strings}, scratch.stack))
            {
                std::string text = zge_host::settings::getString(rdram, scratch.strings, kItemsSize);
                if (text.find("SETTINGS") == std::string::npos && text.size() + std::char_traits<char>::length(kLine) < kItemsSize)
                {
                    text += kLine;
                    if (zge_host::settings::putString(rdram, scratch.strings, text))
                        runtime->callGuestFunction(rdram, ctx, sym::bmx::setVariableValue, {items, scratch.strings},
                                                   scratch.stack);
                }
            }
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        // Text edits of the UI_menu script. False (and the script left
        // alone) if an anchor is missing.
        bool patchMenus(std::string &text)
        {
            std::string edited = text;
            zx::zaxis::Span options = zx::zaxis::findObject(edited, "UI_menu/CommonBackground/FrameOptions");
            zx::zaxis::Span option = zx::zaxis::findObject(edited, "OPTION", options);
            zx::zaxis::Span show = zx::zaxis::findObject(edited, "Show", options);
            if (!options.found() || !option.found() || !show.found())
                return false;
            // The menu's block closes one tab deeper than it opens (as the
            // disc has it), so findObject does not take it: the menu is from
            // its OBJECT line to the OPTION frame's end.
            const size_t menuAt = edited.find("OBJECT \"guiMenu\" \"Menu\"", option.begin);
            if (menuAt == std::string::npos || menuAt >= option.end)
                return false;
            zx::zaxis::Span menu{menuAt, option.end};
            // Later positions first: the page after FrameOptions, then the
            // menu's lines (inside OPTION), then the way to the page after
            // Show (before both).
            edited.insert(options.end,
                          zx::zaxis::indentBlock(page("PcSettings", kFrontEndBack, false), zx::zaxis::indentAt(edited, options.begin)));
            const std::string tabs(static_cast<size_t>(zx::zaxis::indentAt(edited, menu.begin) + 2), '\t');
            bool ok = zx::zaxis::replaceIn(edited, menu, "CALL bmxmenuscriptGetOptionsMenuItems Items\r\n",
                                    "CALL bmxmenuscriptGetOptionsMenuItems Items\r\n" + tabs + "\tCALL pcSettingsOptionsLine Items\r\n");
            ok = ok && zx::zaxis::replaceIn(edited, menu, "\tOnItemSelect\t{\r\n",
                                     "\tOnItemSelect\t{\r\n" + tabs + "IF SelectedText == {\"SETTINGS\"} PLAY ..GoToPcSettings\r\n" + tabs +
                                         "IF SelectedText == {\"SETTINGS\"} EXIT\r\n");
            if (!ok)
                return false;
            show = zx::zaxis::findObject(edited, "UI_menu/CommonBackground/FrameOptions/Show");
            if (!show.found())
                return false;
            edited.insert(show.end, zx::zaxis::indentBlock(kGoTo, zx::zaxis::indentAt(edited, show.begin)));
            text = std::move(edited);
            return true;
        }

        // The UI_overlay script: the pause menu's page after FramePause.
        bool patchOverlay(std::string &text)
        {
            zx::zaxis::Span pause = zx::zaxis::findObject(text, "UI_overlay/FramePause");
            if (!pause.found())
                return false;
            text.insert(pause.end, zx::zaxis::indentBlock(page("PcSettingsPause", kPauseBack, true), zx::zaxis::indentAt(text, pause.begin)));
            return true;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            bool changed = false;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (zx::zaxis::scriptName(entry) == "UI_overlay")
                {
                    std::string text(entry.data.begin(), entry.data.end());
                    if (patchOverlay(text))
                    {
                        entry.data.assign(text.begin(), text.end());
                        changed = true;
                        std::cout << "[bmx-xxx] pause menu: SETTINGS" << std::endl;
                    }
                    else
                    {
                        std::cerr << "[bmx-xxx] UI_overlay is not the expected script: no Settings page in the pause menu"
                                  << std::endl;
                    }
                    continue;
                }
                if (zx::zaxis::scriptName(entry) != "UI_menu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                if (patchMenus(text))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                    std::cout << "[bmx-xxx] Options: SETTINGS" << std::endl;
                }
                else
                {
                    std::cerr << "[bmx-xxx] UI_menu is not the expected script: no SETTINGS in Options" << std::endl;
                }
            }
            if (changed)
                bytes = zx::zaxis::writeArchive(entries);
            return changed;
        }
    }
}

namespace bmx_xxx::settings_menu
{
    void openFromPause(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t overlay)
    {
        std::cout << "[bmx-xxx] pause menu: Settings" << std::endl;
        zge_host::settings::runScript(rdram, ctx, runtime, overlay,
                                      {"KILLFOCUS", "PLAY FramePause.Menu.Shutdown", "SET FramePause.Title.Visible {0}",
                                       "SET FrameSelect.Visible {0}", "SET FrameNone.Box.Visible {0}",
                                       "SET FrameNone.Box.IBox.Visible {0}", "PLAY PcSettingsPause.MoveIn"});
    }
}

namespace bmx_xxx
{
    void installSettingsMenus(PS2Machine &runtime)
    {
        if (const char *on = zx::boot::text("ZX_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        using namespace settings_menu;
        zge_host::settings::setStyle(kStyle);
        if (!zge_host::settings::installScriptRunner(runtime, sym::bmx::runScript))
        {
            std::cerr << "[bmx-xxx] the Settings page's scratch memory not reserved" << std::endl;
            return;
        }
        for (const zge_host::settings::Function &function : zge_host::settings::pageFunctions())
            if (!zge_host::addScriptFunction(runtime, scriptFunctions(), function.name, function.fn))
                return;
        if (!zge_host::addScriptFunction(runtime, scriptFunctions(), "pcSettingsOptionsLine", &optionsLineFunction))
            return;
        // L2 / R2 on the page (the heading jump), with no line in its script.
        if (!zge_host::settings::installPageKeys(runtime, {.drawSniffer = sym::bmx::drawGuiKeySniffer,
                                                           .drawGetsMessage = false,
                                                           .findNode = sym::bmx::findNode,
                                                           .findTakesKind = true}))
            std::cerr << "[bmx-xxx] the Settings page's L2 / R2 not installed" << std::endl;
        zx::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary);
        exit_menu::addSettingsLine(runtime);
    }
}
