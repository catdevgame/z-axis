// The port's Settings page in the game's own menus (zge_settings.h), one body
// for the games built on the Z-Axis engine: the script engine as host code
// reaches it, and the page's rows, keys and refresh.

#include "zge_settings.h"

#include "machine.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/session.h"
#include "runtime/settings_registry.h"
#include "runtime/zaxis_text.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace zge_host::settings
{
    // ---- the page: its sections, in order, and the rows of each -----------
    //
    // One order for the three games. A section lists its rows by setting key
    // (runtime/settings_registry.h: the key a game registers, "display.vsync"),
    // headings drawn between rows (heading("CAMERA"): a title in the section
    // title's colour, not selectable), and rows("controls.key."): every
    // registered key with that prefix, in registration order. A key the game
    // running has not registered is skipped; a heading with no row under it
    // is dropped; a registered key no section names goes last in the
    // section whose homePrefixes it starts with (so nothing is lost). Keys
    // keep their names: settings.ini stays as it was, the table puts
    // display.renderer in Graphics.
    //
    // To move a row: move its line. To add a section: add one here, in the
    // place it goes; the games' pages take any number.
    const Section kPage[] = {
        {"Display",
         {"display."},
         {
             row("display.window"),
             row("display.scaling"),
             row("display.vsync"),
             row("display.variable_refresh"),
             row("display.frame_interpolation"),
             row("display.widescreen"),
             row("display.aspect"),
             row("display.show_fps"),
             row("display.pause_in_background"),
             row("display.map_window"),
             row("display.map_window_on_top"), // (the app's, with the Map Window)
             row("display.clean_top_screen"),
         }},
        {"Graphics",
         {},
         {
             row("display.renderer"),
             row("display.resolution"),
             row("display.full_height"),
             row("display.anti_aliasing"),
             row("display.texture_filtering"),
             row("display.fog"),
             row("display.shadows"),
             row("display.depth_precision"),
             row("display.depth_test"),
             row("display.perspective_correct"),
             row("display.character_lighting"),
             row("display.native_meshes"),
             row("display.motion_blur"),
             row("display.draw_distance"),
             row("display.xbox_textures"),
             row("display.xbox_skaters"),
             row("display.xbox_movies"),
             row("display.texture_dump"),
         }},
        {"Sound",
         {"audio."},
         {
             row("audio.volume"),
             row("audio.music_volume"),
             row("audio.effects_volume"),
             row("audio.speech_volume"),
             row("audio.shuffle"), // the soundtrack's (zge_soundtrack.h)
             row("audio.movie_volume"),
             row("audio.french_voices"),
             row("audio.mute_in_background"),
         }},
        {"Controls",
         {"controls."},
         {
             row("controls.prompts"),
             row("controls.gamepad"),
             row("controls.stick_deadzone"),
             row("controls.gamepads"),
             row("controls.test_rumble"),
             row("controls.gamepad_test"), // (the app's, with the Developer Menu and the debug panel)
             row("controls.swap_players"),
             row("controls.mouse_mode"),
             row("controls.mouse_sensitivity"),
             row("controls.invert_y"),
             heading("KEYS"),
             rows("controls.key."),
             heading("BUTTONS"),
             row("controls.gamepad_layout"),
             rows("controls.pad."),
             row("controls.reset"),
         }},
        {"Gameplay",
         {"camera.", "assist.", "cheat."},
         {
             heading("CAMERA"),
             row("camera.fov"),
             row("camera.distance"),
             row("camera.height"),
             row("camera.style"),
             heading("ASSISTS"),
             row("assist.grind_balance_loss"),
             row("assist.manual_balance_loss"),
             row("assist.juice_runs_out"),
             row("assist.juice_drain"),
             row("assist.crash_juice_loss"),
             heading("CHEATS"),
             row("cheat.perfect_grind"),
             row("cheat.perfect_manual"),
             row("cheat.perfect_handplant"),
             row("cheat.super_spin"),
             row("cheat.low_gravity_wall_ride"),
             row("cheat.juice_regeneration"),
             row("cheat.invulnerability"),
             row("cheat.unlock_all"), // the one row that changes a save: last
         }},
        {"Game",
         {"game."},
         {
             row("game.language"),
             row("game.skip_intro_movies"),
             row("game.tuning"),
             row("game.career_scripts"),
             row("game.developer_menu"),
             row("game.practice"),
             row("game.record_play"),
             row("game.recordings"),
             row("game.custom_freeplay"),
             heading("EXTRAS"), // content beyond the PS2 disc, and cut content
             row("game.zoo"),
             row("game.zoo_unlock"),
             row("game.estate"),
             row("game.ape"),
             row("game.xbox_free_skate"),
             row("game.dm2_xbox_levels"),
             row("game.create_player"),
             row("game.xbox_themes"),
             row("game.retro_movie"),
             row("game.attract_demo"),
             row("game.park_editor"),
             row("game.xbox_challenges"),
             row("game.editor_pieces"),
             row("game.level_previews"),
             row("game.logo_movie"),
             row("game.intro_movie"),
             row("game.gc_levels"),
             row("game.menu_lines"),
             row("game.all_mp_modes"),
             row("game.logo_splash"),
             row("game.trailers"),
             row("game.car_textures"),
             row("game.green_skin"),
             row("game.topless_girl"),
             row("game.xbox_songs"),
             row("game.restart"), // only while a change waits for the next start (the app's)
         }},
    };

    // Rows drawn dim (name and value at 55 % brightness) while another
    // setting makes them idle: the GPU renderer's rows under the CPU
    // renderer (display.renderer 0), Wide Aspect with Widescreen off, Always
    // on Top with no Map Window, the gamepads' rows with Gamepad off. A dim
    // row can still be changed.
    struct DimRule
    {
        const char *key;      // a row's key, or a prefix ending in '.' for every row under it
        const char *whileKey; // the row is dim while this setting...
        int whileValue;       // ...has this value
    };
    const DimRule kDimRules[] = {
        {"display.resolution", "display.renderer", 0},
        {"display.full_height", "display.renderer", 0},
        {"display.anti_aliasing", "display.renderer", 0},
        {"display.texture_filtering", "display.renderer", 0},
        {"display.fog", "display.renderer", 0},
        {"display.shadows", "display.renderer", 0},
        {"display.depth_precision", "display.renderer", 0},
        {"display.perspective_correct", "display.renderer", 0},
        {"display.character_lighting", "display.renderer", 0},
        {"display.native_meshes", "display.renderer", 0},
        {"display.depth_test", "display.renderer", 0},          // the GPU renderer's depth buffer (gs_quality.h)
        {"display.frame_interpolation", "display.renderer", 0}, // pictures the GPU renderer draws (gs_interpolate.h)
        {"display.xbox_textures", "display.renderer", 0},       // pictures handed to the GPU renderer (texture_replace.h)
        {"display.aspect", "display.widescreen", 0},
        {"display.map_window_on_top", "display.map_window", 0}, // (the Map Window's: none open, nothing kept on top)
        // Off: no gamepad is read, so none is listed, rumbles or swaps, and no pad button is bound.
        {"controls.stick_deadzone", "controls.gamepad", 0},
        {"controls.gamepads", "controls.gamepad", 0},
        {"controls.test_rumble", "controls.gamepad", 0},
        {"controls.gamepad_test", "controls.gamepad", 0},
        {"controls.swap_players", "controls.gamepad", 0},
        {"controls.gamepad_layout", "controls.gamepad", 0},
        {"controls.pad.", "controls.gamepad", 0},
    };
    constexpr float kDimBrightness = 0.55f;

    PageTable pageTable() { return {kPage, std::size(kPage)}; }

    int headingJump(const std::vector<bool> &headings, int row, int step)
    {
        // The parts' first rows: the section's first, then each heading's first.
        std::vector<int> starts;
        bool partStarts = true;
        for (size_t i = 0; i < headings.size(); ++i)
        {
            if (headings[i])
                partStarts = true;
            else if (partStarts)
            {
                starts.push_back(static_cast<int>(i));
                partStarts = false;
            }
        }
        if (starts.empty())
            return row;
        const int count = static_cast<int>(starts.size());
        int part = 0;
        while (part + 1 < count && starts[static_cast<size_t>(part + 1)] <= row)
            ++part;
        return starts[static_cast<size_t>(((part + (step < 0 ? -1 : 1)) % count + count) % count)];
    }
}

namespace zge_host::settings
{
    namespace
    {
        namespace st = zx::settings;
        namespace hi = ps2_host_input;

        constexpr int kRegA0 = 4;
        constexpr int kRegSp = 29;
        constexpr int kRegRa = 31;
        // zgui's Node (engine/zge/zgui/widget.h): its name, in every Z-Axis engine.
        constexpr uint32_t kNodeName = 0x14u;
        constexpr uint32_t kWidgetKind = 1u; // NodeKind::Widget, findNode's kind
        // The engine's EngineMessage (engine/zge/scene/message.h): the object it is for.
        constexpr uint32_t kMessageTarget = 0x08u;

        // Guest memory of our own: strings handed to the engine, and the
        // stack of the helpers called with them.
        constexpr uint32_t kScratchSize = 0x8000u;
        constexpr uint32_t kScratchStrings = 0x5000u;

        struct Engine
        {
            uint32_t runScript = 0;
            uint32_t scratchTop = 0;
            int depth = 0; // runScript lines running (EE thread)
            bool trace = false;
            const Style *style = nullptr;
            // The heading jump's keys (installPageKeys): the game's key
            // sniffer draw, wrapped, and how to find the page's sniffer.
            PageKeys keys{};
            PS2Machine::RecompiledFunction drawSniffer = nullptr;
        };
        Engine &engine() { return zx::session::state<Engine>(); }
    }

    bool installScriptRunner(PS2Machine &runtime, uint32_t runScriptFunction)
    {
        Engine &e = engine();
        const char *trace = std::getenv("ZX_TRACE_SETTINGS");
        e.trace = trace && trace[0] == '1';
        e.runScript = runScriptFunction;
        if (!e.scratchTop)
            e.scratchTop = runtime.reserveAsyncCallbackStack(kScratchSize, 16u);
        return e.scratchTop != 0u;
    }

    bool tracing() { return engine().trace; }
    uint32_t scratchTop() { return engine().scratchTop; }
    uint32_t scratchStrings() { return engine().scratchTop ? engine().scratchTop - kScratchSize : 0u; }

    Scratch scratchNow(R5900Context *ctx)
    {
        const Engine &e = engine();
        if (e.depth == 0)
            return {scratchStrings(), e.scratchTop, kScratchStrings};
        constexpr uint32_t kNestedStrings = 0x1000u, kBelowCaller = 0x400u;
        const uint32_t strings = (getRegU32(ctx, kRegSp) - kBelowCaller - kNestedStrings) & ~0xFu;
        return {strings, strings, kNestedStrings};
    }

    bool putString(uint8_t *rdram, uint32_t address, const std::string &utf8)
    {
        const std::string text = zx::zaxis_text::latin1FromUtf8(utf8);
        for (size_t i = 0; i <= text.size(); ++i)
        {
            uint8_t *p = getMemPtr(rdram, address + static_cast<uint32_t>(i));
            if (!p)
                return false;
            *p = i < text.size() ? static_cast<uint8_t>(text[i]) : 0u;
        }
        return true;
    }

    std::string getString(const uint8_t *rdram, uint32_t address, size_t limit)
    {
        std::string out;
        for (size_t i = 0; i < limit; ++i)
        {
            const uint8_t *p = getConstMemPtr(rdram, address + static_cast<uint32_t>(i));
            if (!p || *p == 0u)
                break;
            out.push_back(static_cast<char>(*p));
        }
        return out;
    }

    uint32_t readWord(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        if (const uint8_t *p = getConstMemPtr(rdram, address))
            std::memcpy(&value, p, sizeof(value));
        return value;
    }

    uint32_t firstArgument(const uint8_t *rdram, R5900Context *ctx) { return readWord(rdram, getRegU32(ctx, kRegA0)); }

    bool widgetIs(const uint8_t *rdram, uint32_t widget, const char *name)
    {
        return getString(rdram, readWord(rdram, widget + kNodeName), 32) == name;
    }

    // Runs script commands on a widget (names relative to it), one line
    // each, as a script function of the game's does with runScript.
    void runScript(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t widget,
                   const std::vector<std::string> &lines)
    {
        Engine &e = engine();
        if (!e.scratchTop || !e.runScript || !widget)
            return;
        // A line can call a script function of ours that runs lines again:
        // those go below this call's stack, so this line stays as it is.
        const Scratch scratch = scratchNow(ctx);
        for (const std::string &line : lines)
        {
            if (line.size() + 1u > scratch.size || !putString(rdram, scratch.strings, line))
                continue;
            uint32_t result = 0;
            ++e.depth;
            const bool ok =
                runtime->callGuestFunction(rdram, ctx, e.runScript, {widget, scratch.strings, 0u}, scratch.stack, &result);
            --e.depth;
            if (e.trace)
                std::fprintf(stderr, "[settings] script 0x%x '%s' -> %s %u\n", widget, line.substr(0, 120).c_str(),
                             ok ? "ok" : "failed", result);
        }
    }

    std::string literal(std::string text)
    {
        text.erase(std::remove_if(text.begin(), text.end(), [](char c)
                                  { return c == '"' || c == '{' || c == '}' || c == '\\' || c == '\n' || c == '\r'; }),
                   text.end());
        return text;
    }

    void setStyle(const Style &style) { engine().style = &style; }

    namespace
    {
        // ---- the page's sections and rows -----------------------------------

        // How long a help text is as drawn: its translation's length when
        // the game translates it (Latin-1: a byte per character).
        size_t shownLength(const Style &style, const std::string &text)
        {
            const std::string shown = style.translate ? style.translate(text) : std::string();
            return shown.empty() ? text.size() : shown.size();
        }

        // Help.Scale for a line: narrower (width only, as the Xbox release
        // fits its longer texts) only when one line is still longer than fits.
        std::string helpScaleCommand(const char *label, const Style &style, const Layout &layout, const std::string &line)
        {
            const float length = static_cast<float>(shownLength(style, line));
            const float x = length > layout.helpChars ? layout.helpScale * layout.helpChars / length : layout.helpScale;
            char out[96];
            std::snprintf(out, sizeof(out), "SET %s.Scale {%.3f %.3f 1}", label, static_cast<double>(x),
                          static_cast<double>(layout.helpScale));
            return out;
        }

        // A help text on the page's two lines: whole when it fits, else
        // broken at the sentence end that balances the lines best (each
        // part keeps its translation: the table translates sentence by
        // sentence), else, untranslated, at the last space that fits.
        void helpLines(const Style &style, const Layout &layout, const std::string &help, std::string &first,
                       std::string &second)
        {
            first = help;
            second.clear();
            const size_t fit = static_cast<size_t>(layout.helpChars);
            if (shownLength(style, help) <= fit)
                return;
            size_t best = std::string::npos, bestLongest = 0;
            for (size_t dot = help.find(". "); dot != std::string::npos; dot = help.find(". ", dot + 1))
            {
                const size_t longest =
                    std::max(shownLength(style, help.substr(0, dot + 1)), shownLength(style, help.substr(dot + 2)));
                if (best == std::string::npos || longest < bestLongest)
                    best = dot, bestLongest = longest;
            }
            if (best != std::string::npos)
            {
                first = help.substr(0, best + 1);
                second = help.substr(best + 2);
                return;
            }
            const bool translated = style.translate && !style.translate(help).empty();
            const size_t space = help.rfind(' ', std::min(fit, help.size() - 1));
            if (!translated && space != std::string::npos && space > 0)
            {
                first = help.substr(0, space);
                second = help.substr(space + 1);
            }
        }

        // A colour as the scripts write it ("192 192 192 255") at the dim
        // rows' brightness.
        std::string dimmed(const char *color)
        {
            int r = 0, g = 0, b = 0, a = 255;
            std::sscanf(color, "%d %d %d %d", &r, &g, &b, &a);
            char out[48];
            std::snprintf(out, sizeof(out), "%d %d %d %d", static_cast<int>(static_cast<float>(r) * kDimBrightness),
                          static_cast<int>(static_cast<float>(g) * kDimBrightness),
                          static_cast<int>(static_cast<float>(b) * kDimBrightness), a);
            return out;
        }

        // Whether a row is dim now (kDimRules).
        bool dimNow(const std::string &key)
        {
            for (const DimRule &rule : kDimRules)
            {
                const std::string_view ruleKey = rule.key;
                const bool under = ruleKey.ends_with('.') ? key.starts_with(ruleKey) : key == ruleKey;
                if (under && st::has(rule.whileKey) && st::value(rule.whileKey) == rule.whileValue)
                    return true;
            }
            return false;
        }

        // A row of a section as shown: a setting (its key) or a heading.
        struct Row
        {
            std::string key;
            const char *heading = nullptr; // a heading row: not selectable
            bool selectable() const { return heading == nullptr; }
        };

        // Whether an entry of any section names this key (a row entry, or a
        // prefix entry it falls under).
        bool named(const std::string &key)
        {
            for (const Section &section : kPage)
                for (const Entry &entry : section.entries)
                    if ((entry.key && key == entry.key) || (entry.prefix && key.rfind(entry.prefix, 0) == 0))
                        return true;
            return false;
        }

        // The section's rows for the settings registered now: the table's
        // entries in order (a key the game has not registered skipped, a
        // heading with no row under it dropped), then any registered key of
        // the section's home prefixes the table does not name.
        std::vector<Row> sectionRows(const Section &section)
        {
            std::vector<Row> rows;
            for (const Entry &entry : section.entries)
            {
                if (entry.heading)
                    rows.push_back(Row{{}, entry.heading});
                else if (entry.prefix)
                    for (const std::string &key : st::keys(entry.prefix))
                        rows.push_back(Row{key, nullptr});
                else if (entry.key && st::has(entry.key))
                    rows.push_back(Row{entry.key, nullptr});
            }
            for (const char *prefix : section.homePrefixes)
                for (const std::string &key : st::keys(prefix))
                    if (!named(key))
                        rows.push_back(Row{key, nullptr});
            std::vector<Row> kept;
            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i].selectable() || (i + 1 < rows.size() && rows[i + 1].selectable()))
                    kept.push_back(rows[i]);
            return kept;
        }
    }

    std::vector<std::string> shownRows(size_t section)
    {
        std::vector<std::string> out;
        if (section >= std::size(kPage))
            return out;
        for (const Row &row : sectionRows(kPage[section]))
            out.push_back(row.selectable() ? row.key : std::string("#") + row.heading);
        return out;
    }

    namespace
    {
        int firstSelectable(const std::vector<Row> &rows)
        {
            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i].selectable())
                    return static_cast<int>(i);
            return 0;
        }

        // Which rows are headings, for headingJump.
        std::vector<bool> headingRows(const std::vector<Row> &rows)
        {
            std::vector<bool> headings;
            headings.reserve(rows.size());
            for (const Row &row : rows)
                headings.push_back(!row.selectable());
            return headings;
        }

        // The next selectable row from `from` in direction `step`, wrapping.
        int stepRow(const std::vector<Row> &rows, int from, int step)
        {
            const int count = static_cast<int>(rows.size());
            if (count == 0)
                return 0;
            int at = from;
            for (int n = 0; n < count; ++n)
            {
                at = (at + step + count) % count;
                if (rows[static_cast<size_t>(at)].selectable())
                    return at;
            }
            return from;
        }

        struct Page
        {
            int section = 0;
            int row = 0;
            int top = 0;
            bool capturing = false;
            bool sawCapture = false;
            int ticks = 0;
            int headingSlots = -1; // which slots were last drawn as headings (-1: none drawn yet)
            // The heading jump (L2 / R2, pageKeysOnDraw): the page's key
            // sniffer, found when the page opens, whether it is the pause
            // menu's, and the pads' L2 and R2 presses seen so far.
            uint32_t sniffer = 0;
            bool pause = false;
            uint32_t l2Seen = 0;
            uint32_t r2Seen = 0;
        };
        struct Pages
        {
            std::mutex mutex;
            std::unordered_map<uint32_t, Page> pages; // by dialog widget
        };
        Pages &pageState() { return zx::session::state<Pages>(); }

        // The sections with a row to select, as indexes into kPage.
        std::vector<int> availableSections()
        {
            std::vector<int> out;
            for (int i = 0; i < static_cast<int>(std::size(kPage)); ++i)
                for (const Row &row : sectionRows(kPage[i]))
                    if (row.selectable())
                    {
                        out.push_back(i);
                        break;
                    }
            return out;
        }

        void clampPage(const Style &style, Page &page, const std::vector<Row> &rows)
        {
            const int count = static_cast<int>(rows.size());
            page.row = count == 0 ? 0 : std::clamp(page.row, 0, count - 1);
            if (count && !rows[static_cast<size_t>(page.row)].selectable())
                page.row = stepRow(rows, page.row, 1);
            if (page.row < page.top)
                page.top = page.row;
            if (page.row >= page.top + style.visibleRows)
                page.top = page.row - style.visibleRows + 1;
            page.top = std::clamp(page.top, 0, std::max(0, count - style.visibleRows));
            // A heading scrolls in with its first row.
            while (page.top > 0 && !rows[static_cast<size_t>(page.top - 1)].selectable() &&
                   page.row - (page.top - 1) < style.visibleRows)
                --page.top;
        }

        // Fills the rows, the section name, the help line and the highlight.
        void refresh(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, const Style &style, uint32_t dialog,
                     bool pause, Page &page)
        {
            const Layout &layout = pause ? style.pause : style.frontEnd;
            const std::vector<int> sections = availableSections();
            if (sections.empty())
                return;
            if (std::find(sections.begin(), sections.end(), page.section) == sections.end())
                page.section = sections.front();
            const Section &section = kPage[page.section];
            const std::vector<Row> rows = sectionRows(section);
            clampPage(style, page, rows);

            const char *headingScale = layout.headingScale ? layout.headingScale : style.rowScale;
            const char *headingColor = style.headingColor ? style.headingColor : style.valueColor;
            std::vector<std::string> commands;
            std::string help;
            int headingSlots = 0;
            for (int slot = 0; slot < style.visibleRows; ++slot)
            {
                const int i = page.top + slot;
                std::string name, value;
                bool selected = false, heading = false, dim = false;
                st::Setting setting;
                if (i < static_cast<int>(rows.size()) && !rows[static_cast<size_t>(i)].selectable())
                {
                    heading = true;
                    name = rows[static_cast<size_t>(i)].heading;
                }
                else if (i < static_cast<int>(rows.size()) && st::describe(rows[static_cast<size_t>(i)].key, setting))
                {
                    selected = i == page.row;
                    dim = dimNow(setting.key);
                    name = setting.label;
                    value = st::text(setting.key);
                    if (setting.kind != st::Kind::Action)
                        value = "< " + value + " >";
                    if (selected)
                    {
                        help = setting.help;
                        if (setting.restart)
                            help += help.empty() ? "Applies the next time the game starts."
                                                 : " Applies at next start.";
                    }
                }
                const std::string n = std::to_string(slot);
                commands.push_back("SET Rows.Name" + n + ".Text {\"" + literal(name) + "\"}");
                commands.push_back("SET Rows.Value" + n + ".Text {\"" + literal(value) + "\"}");
                const std::string nameColor = heading ? headingColor : selected ? style.selectedColor : style.nameColor;
                const std::string valueColor = selected ? style.selectedColor : style.valueColor;
                commands.push_back("SET Rows.Name" + n + ".Color {" + (dim ? dimmed(nameColor.c_str()) : nameColor) + "}");
                commands.push_back("SET Rows.Value" + n + ".Color {" + (dim ? dimmed(valueColor.c_str()) : valueColor) +
                                   "}");
                // A heading is drawn at the section title's size; the scale
                // is set only where a slot changes between a heading and a row.
                const bool wasHeading = page.headingSlots >= 0 && (page.headingSlots >> slot) & 1;
                if (page.headingSlots < 0 || wasHeading != heading)
                    commands.push_back("SET Rows.Name" + n + ".Scale {" + std::string(heading ? headingScale : style.rowScale) +
                                       "}");
                if (heading)
                    headingSlots |= 1 << slot;
            }
            page.headingSlots = headingSlots;
            const float y = layout.rowTop + static_cast<float>(page.row - page.top) * layout.rowPitch;
            char highlight[64];
            std::snprintf(highlight, sizeof(highlight), style.highlightAt, static_cast<double>(y));
            commands.push_back(std::string("SET Section.Text {\"") + section.title + "\"}");
            std::string helpFirst, helpSecond;
            helpLines(style, layout, literal(help), helpFirst, helpSecond);
            commands.push_back("SET Help.Text {\"" + helpFirst + "\"}");
            commands.push_back(helpScaleCommand("Help", style, layout, helpFirst));
            commands.push_back("SET Help2.Text {\"" + helpSecond + "\"}");
            commands.push_back(helpScaleCommand("Help2", style, layout, helpSecond));
            commands.push_back(highlight);
            commands.push_back(std::string("SET MoreUp.Visible {") + (page.top > 0 ? "1" : "0") + "}");
            commands.push_back(std::string("SET MoreDown.Visible {") +
                               (page.top + style.visibleRows < static_cast<int>(rows.size()) ? "1" : "0") + "}");
            runScript(rdram, ctx, runtime, dialog, commands);
        }

        enum class Key
        {
            Open,
            Up,
            Down,
            Left,
            Right,
            Accept,
            PrevSection,
            NextSection,
            PrevHeading,
            NextHeading,
            Tick,
        };

        // The pads' L2 and R2 presses so far (both ports: the page's sniffer reads both).
        uint32_t presses(int button)
        {
            return PSPadBackend::buttonPresses(0, button) + PSPadBackend::buttonPresses(1, button);
        }

        // The page's key sniffer (the dialog's child KeySniffer), by the
        // game's own findNode; 0 without installPageKeys.
        uint32_t findSniffer(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t dialog)
        {
            const PageKeys &keys = engine().keys;
            if (!keys.findNode)
                return 0u;
            const Scratch scratch = scratchNow(ctx);
            uint32_t found = 0;
            if (!putString(rdram, scratch.strings, "KeySniffer"))
                return 0u;
            const bool ok = keys.findTakesKind
                                ? runtime->callGuestFunction(rdram, ctx, keys.findNode, {dialog, scratch.strings, kWidgetKind},
                                                             scratch.stack, &found)
                                : runtime->callGuestFunction(rdram, ctx, keys.findNode, {dialog, scratch.strings},
                                                             scratch.stack, &found);
            return ok ? found : 0u;
        }

        void pageKeyOn(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t dialog, bool pause, Key key);

        void pageKey(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, Key key)
        {
            const Style *style = engine().style;
            bool pause = false;
            uint32_t dialog = 0u;
            if (style && style->findDialog)
                dialog = style->findDialog(rdram, ctx, runtime, pause);
            else if (style)
            {
                dialog = firstArgument(rdram, ctx);
                pause = dialog && widgetIs(rdram, dialog, style->pauseDialog);
            }
            if (!dialog || !style)
                return;
            pageKeyOn(rdram, ctx, runtime, dialog, pause, key);
        }

        void pageKeyOn(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t dialog, bool pause, Key key)
        {
            const Style *style = engine().style;
            if (!style)
                return;
            Page page;
            {
                std::lock_guard<std::mutex> lock(pageState().mutex);
                page = pageState().pages[dialog];
            }
            const std::vector<int> sections = availableSections();
            if (sections.empty())
                return;
            auto sectionIndex = std::find(sections.begin(), sections.end(), page.section);
            if (sectionIndex == sections.end())
                sectionIndex = sections.begin();
            std::vector<Row> rows = sectionRows(kPage[*sectionIndex]);
            const std::string current =
                page.row < static_cast<int>(rows.size()) ? rows[static_cast<size_t>(page.row)].key : "";
            st::Setting setting;
            const bool haveSetting = !current.empty() && st::describe(current, setting);
            const std::string before = haveSetting ? st::text(current) : std::string();
            bool tick = false;

            switch (key)
            {
            case Key::Open:
                page.row = firstSelectable(rows);
                page.top = 0;
                page.capturing = false;
                page.headingSlots = -1;
                page.sniffer = findSniffer(rdram, ctx, runtime, dialog);
                page.pause = pause;
                page.l2Seen = presses(hi::kL2);
                page.r2Seen = presses(hi::kR2);
                break;
            case Key::Up:
                page.row = stepRow(rows, page.row, -1);
                break;
            case Key::Down:
                page.row = stepRow(rows, page.row, 1);
                break;
            case Key::PrevHeading:
            case Key::NextHeading:
                page.row = headingJump(headingRows(rows), page.row, key == Key::PrevHeading ? -1 : 1);
                break;
            case Key::Left:
            case Key::Right:
                // (FromGame: the game goes on once the change is run, on every run at this point of its frames)
                if (haveSetting && setting.kind != st::Kind::Action)
                    st::stepFromGame(current, key == Key::Left ? -1 : 1, setting.kind != st::Kind::Range);
                break;
            case Key::Accept:
                if (haveSetting && setting.kind == st::Kind::Action)
                {
                    st::activateFromGame(current);
                    page.capturing = current.rfind("controls.key.", 0) == 0 || current.rfind("controls.pad.", 0) == 0;
                    page.sawCapture = false;
                    page.ticks = 0;
                    tick = page.capturing;
                }
                else if (haveSetting && setting.kind != st::Kind::Range)
                {
                    st::stepFromGame(current, 1, true);
                }
                break;
            case Key::PrevSection:
            case Key::NextSection:
            {
                const int count = static_cast<int>(sections.size());
                int index = static_cast<int>(sectionIndex - sections.begin());
                index = (index + (key == Key::PrevSection ? count - 1 : 1)) % count;
                page.section = sections[static_cast<size_t>(index)];
                rows = sectionRows(kPage[page.section]);
                page.row = firstSelectable(rows);
                page.top = 0;
                break;
            }
            case Key::Tick:
                if (page.capturing)
                {
                    ++page.ticks;
                    if (PSPadBackend::captureActive())
                        page.sawCapture = true;
                    else if (page.sawCapture || page.ticks > 30)
                        page.capturing = false;
                    tick = page.capturing;
                }
                break;
            }
            page.section = page.section < 0 ? sections.front() : page.section;
            if (key != Key::PrevSection && key != Key::NextSection)
                page.section = *sectionIndex;
            refresh(rdram, ctx, runtime, *style, dialog, pause, page);
            // ZX_TRACE_SETTINGS=1: each key, and the row's value before it and as the page now shows it.
            if (engine().trace && key != Key::Tick)
            {
                static const char *const kKeyNames[] = {"Open", "Up",   "Down", "Left",        "Right",       "Accept",
                                                        "Prev", "Next", "PrevHeading", "NextHeading", "Tick"};
                const std::vector<Row> shown = sectionRows(kPage[page.section]);
                const std::string row = page.row < static_cast<int>(shown.size()) ? shown[static_cast<size_t>(page.row)].key : "";
                std::fprintf(stderr, "[settings] page 0x%x %s: %s %s '%s' -> '%s'\n", dialog, kKeyNames[static_cast<int>(key)],
                             kPage[page.section].title, row.c_str(), row == current ? before.c_str() : "",
                             row.empty() ? "" : st::text(row).c_str());
            }
            {
                std::lock_guard<std::mutex> lock(pageState().mutex);
                pageState().pages[dialog] = page;
            }
            // While a key is being rebound the page asks again each frame
            // (two actions playing each other, so neither restarts itself).
            if (tick)
                runScript(rdram, ctx, runtime, dialog, {page.ticks % 2 ? "PLAY TickB" : "PLAY TickA"});
            else if (key == Key::Tick)
                runScript(rdram, ctx, runtime, dialog, {"SET KeySniffer.Active {1}"});
            if (key == Key::Accept && tick)
                runScript(rdram, ctx, runtime, dialog, {"SET KeySniffer.Active {0}"});
        }
        template <Key K>
        void pageFunction(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            pageKey(rdram, ctx, runtime, K);
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        // The game's key sniffer draw, wrapped (installPageKeys): when it is
        // the open page's sniffer (not while a key is being rebound), a new
        // L2 or R2 press jumps to the part before or after (headingJump).
        // Then the sniffer's own draw, as before: its Events, the rest.
        void pageKeysOnDraw(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Engine &e = engine();
            const uint32_t argument = getRegU32(ctx, kRegA0);
            const uint32_t sniffer = e.keys.drawGetsMessage ? readWord(rdram, argument + kMessageTarget) : argument;
            uint32_t dialog = 0;
            Page page;
            Key jump = Key::Tick;
            if (sniffer)
            {
                std::lock_guard<std::mutex> lock(pageState().mutex);
                for (auto &[at, open] : pageState().pages)
                    if (open.sniffer == sniffer)
                    {
                        const uint32_t l2 = presses(hi::kL2), r2 = presses(hi::kR2);
                        if (!open.capturing && l2 != open.l2Seen)
                            jump = Key::PrevHeading;
                        else if (!open.capturing && r2 != open.r2Seen)
                            jump = Key::NextHeading;
                        open.l2Seen = l2;
                        open.r2Seen = r2;
                        dialog = at;
                        page = open;
                        break;
                    }
            }
            if (dialog && jump != Key::Tick)
                pageKeyOn(rdram, ctx, runtime, dialog, page.pause, jump);
            e.drawSniffer(rdram, ctx, runtime);
        }

        // Each line of `lines` ('\n' ended) with `tabs` tabs before it.
        std::string indented(const char *lines, const char *tabs)
        {
            std::string out;
            for (const char *p = lines; p && *p;)
            {
                const char *end = std::strchr(p, '\n');
                const size_t n = end ? static_cast<size_t>(end - p) + 1u : std::strlen(p);
                out += tabs;
                out.append(p, n);
                p += n;
            }
            return out;
        }

        // The rows' group: a name / value label pair per visible row (a
        // label's text is set with one short command each; the engine
        // refuses a long one).
        std::string rowsBlock(const Style &style, const Layout &layout)
        {
            std::string out = std::string("\t\tOBJECT \"") + style.rowsType + "\" \"Rows\"\n\t\t{\n" +
                              "\t\t\t//////////////////////////////\n\t\t\t// Property\n" +
                              indented(style.rowsProperties, "\t\t\t");
            const std::string tail = indented(style.rowLabelTail, "\t\t\t\t");
            for (int i = 0; i < style.visibleRows; ++i)
            {
                const float y = layout.rowTop + static_cast<float>(i) * layout.rowPitch;
                for (int column = 0; column < 2; ++column)
                {
                    char label[1024];
                    std::snprintf(label, sizeof(label),
                                  "\t\t\tOBJECT \"guiLabel\" \"%s%d\"\n\t\t\t{\n"
                                  "\t\t\t\t//////////////////////////////\n\t\t\t\t// Property\n"
                                  "\t\t\t\tVisible\t1\n\t\t\t\tPosition\t%.6f %.6f 0.000000\n"
                                  "\t\t\t\tScale\t%s\n\t\t\t\tAlignment\t%d\n"
                                  "\t\t\t\tSpacing\t1.250000\n\t\t\t\tTracking\t1\n"
                                  "\t\t\t\tCharset\t\"%s\"\n"
                                  "\t\t\t\tColor\t%s\n\t\t\t\tText\t\"\"\n",
                                  column == 0 ? "Name" : "Value", i,
                                  static_cast<double>(column == 0 ? layout.nameX : layout.valueX),
                                  static_cast<double>(y), style.rowScale, column == 0 ? 16 : 18, style.rowCharset,
                                  style.nameColor);
                    out += label;
                    out += tail;
                    out += "\t\t\t}\n";
                }
            }
            out += "\t\t}\n";
            return out;
        }
    }

    std::string withRows(std::string page, const Layout &layout, int depth)
    {
        const Style *style = engine().style;
        if (!style)
            return page;
        std::string rows = rowsBlock(*style, layout);
        if (depth == 1)
        {
            std::string out;
            size_t pos = 0;
            while (pos < rows.size())
            {
                size_t end = rows.find('\n', pos);
                end = end == std::string::npos ? rows.size() : end + 1;
                std::string line = rows.substr(pos, end - pos);
                if (!line.empty() && line[0] == '\t')
                    line.erase(0, 1);
                out += line;
                pos = end;
            }
            rows = out;
        }
        const size_t at = page.find("@ROWS@");
        if (at != std::string::npos)
            page.replace(at, 6, rows);
        return page;
    }

    const std::vector<Function> &pageFunctions()
    {
        static const std::vector<Function> functions = {
            {"pcSettingsOpen", &pageFunction<Key::Open>},
            {"pcSettingsUp", &pageFunction<Key::Up>},
            {"pcSettingsDown", &pageFunction<Key::Down>},
            {"pcSettingsLeft", &pageFunction<Key::Left>},
            {"pcSettingsRight", &pageFunction<Key::Right>},
            {"pcSettingsAccept", &pageFunction<Key::Accept>},
            {"pcSettingsPrev", &pageFunction<Key::PrevSection>},
            {"pcSettingsNext", &pageFunction<Key::NextSection>},
            {"pcSettingsTick", &pageFunction<Key::Tick>},
        };
        return functions;
    }

    bool installPageKeys(PS2Machine &runtime, const PageKeys &keys)
    {
        Engine &e = engine();
        e.keys = keys;
        e.drawSniffer = runtime.lookupFunction(keys.drawSniffer);
        return e.drawSniffer && runtime.lookupFunction(keys.findNode) &&
               runtime.replaceFunction(keys.drawSniffer, &pageKeysOnDraw);
    }
}
