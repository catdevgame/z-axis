#pragma once

// The port's Settings page in the game's own menus, shared by the games built
// on the Z-Axis engine (zge_settings.cpp). Every Z-Axis game draws its menus
// with zgui from UIBuilder scripts in ASSETS/UILIB.ZAL (runtime/zaxis_ui.h
// reads and edits them; runtime/disc_patch.h serves the edited copy).
// Each game's override adds to its scripts a page made of its own widgets,
// fonts and materials, and a line in its menus that opens it; the page's rows
// come from the settings registry (runtime/settings_registry.h), so the app's menu
// bar and the page share one model. Which section a row is in and the order
// of the rows is the page table (kPage in zge_settings.cpp), the same for
// every game; a section can hold headings (CAMERA, KEYS), rows that are not
// selectable, drawn in the section title's colour and size.
//
// The page is a dialog widget holding these children, by name:
//   Rows.Name<n>, Rows.Value<n>   a label pair per visible row (rowsBlock)
//   Highlight                     a picture behind the selected row
//   Section, Help                 labels: the section's title, the row's help
//   MoreUp, MoreDown              pictures: more rows above or below
//   KeySniffer                    the pad: its handlers call the page's
//                                 script functions with the dialog (".")
//   TickA, TickB                  two actions whose ends call pcSettingsTick
//                                 (a key being rebound asks every frame)
// and the page's script functions are pcSettingsOpen, pcSettingsUp, -Down,
// -Left, -Right, -Accept, -Prev, -Next (sections, L1 / R1) and -Tick: each
// refreshes the rows with SET commands (runScript, the engine's own). L2 / R2
// (the heading jump) are the host's own (installPageKeys): the scripts have
// no line for them, so the game's heap at boot is as without the jump.
//
// Aggressive Inline (aggressive_inline_settings.cpp), Dave Mirra BMX 2
// (dave_mirra_settings.cpp) and BMX XXX (bmx_xxx_settings.cpp) describe their
// page's look in a Style and add the functions to their script engine.

#include "machine.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zge_host::settings
{
    // ---- the game's script engine -----------------------------------------

    // runScript(widget, "script text", 0): one script line run on a widget,
    // names relative to it (zge/zgui/script.cpp). With guest memory of our
    // own for the strings handed to it and the stack it runs on.
    // ZX_TRACE_SETTINGS=1 logs every line.
    bool installScriptRunner(PS2Machine &runtime, uint32_t runScript);
    void runScript(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t widget,
                   const std::vector<std::string> &lines);
    bool tracing();
    // The scratch memory: the top of its stack, and the strings below it.
    uint32_t scratchTop();
    uint32_t scratchStrings();
    // Where a host call may put a string and the stack of a guest call now:
    // the scratch memory, or, while one of runScript's lines runs (a script
    // function of ours called from it), below that call's stack.
    struct Scratch
    {
        uint32_t strings;
        uint32_t stack;
        uint32_t size; // bytes for strings
    };
    Scratch scratchNow(R5900Context *ctx);

    // UTF-8 (the registry's text) written as the engine's Latin-1, with its
    // terminator; false if it does not fit in RAM.
    bool putString(uint8_t *rdram, uint32_t address, const std::string &utf8);
    std::string getString(const uint8_t *rdram, uint32_t address, size_t limit = 256);
    uint32_t readWord(const uint8_t *rdram, uint32_t address);
    // A script function's first argument (a0 points at the argument words).
    uint32_t firstArgument(const uint8_t *rdram, R5900Context *ctx);
    // Whether a zgui node (a widget) has this name.
    bool widgetIs(const uint8_t *rdram, uint32_t widget, const char *name);
    // Text safe inside a script string literal {"..."}.
    std::string literal(std::string text);

    // ---- the page's table -------------------------------------------------

    // An entry of a section (zge_settings.cpp, kPage): a setting's key, a
    // heading drawn between rows (not selectable), or every registered key
    // with a prefix in registration order.
    struct Entry
    {
        const char *key;
        const char *heading;
        const char *prefix;
    };
    constexpr Entry row(const char *key) { return {key, nullptr, nullptr}; }
    constexpr Entry heading(const char *word) { return {nullptr, word, nullptr}; }
    constexpr Entry rows(const char *prefix) { return {nullptr, nullptr, prefix}; }

    struct Section
    {
        const char *title;
        // A registered key no entry of any section names, starting with one
        // of these, goes last in this section.
        std::vector<const char *> homePrefixes;
        std::vector<Entry> entries;
    };
    struct PageTable
    {
        const Section *sections;
        size_t count;
    };
    // The page's sections in order, the same for the three games.
    PageTable pageTable();
    // L2 / R2 on the page: the row a heading jump lands on from `row`, in a
    // section whose rows are headings where `headings` is true. The parts
    // are the section's top rows and the rows under each heading (Controls:
    // the top options, KEYS, BUTTONS); step -1 goes to the first row of the
    // part before, +1 of the part after, round. A section without headings
    // is one part: its first row.
    int headingJump(const std::vector<bool> &headings, int row, int step);
    // The rows section `section` of pageTable() shows for the settings
    // registered now, as the page lays them out: each a key, a heading as
    // "#" and its word.
    std::vector<std::string> shownRows(size_t section);

    // ---- the page ---------------------------------------------------------

    // Where the rows are in a dialog: positions in its frame.
    struct Layout
    {
        float nameX;
        float valueX;
        float rowTop; // the first row's centre
        float rowPitch;
        // The help line's scale (as in the page's script) and how many
        // characters fit across the frame at that scale; a longer line (a
        // translation, a restart note) is drawn narrower.
        float helpScale;
        float helpChars;
        // A heading row's Scale (as the scripts write it, "1.000000 1.000000
        // 1.000000"): the section title's; null: the rows' own.
        const char *headingScale = nullptr;
    };

    struct Style
    {
        int visibleRows;
        Layout frontEnd;
        Layout pause;
        const char *pauseDialog; // the dialog's widget name in the pause menu (its layout is `pause`)
        // Which dialog a script function runs for: null, its first argument
        // (the scripts pass "."); else this (ZGE2's CALL passes no
        // arguments), which also says whether it is the pause menu's.
        uint32_t (*findDialog)(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, bool &pause);
        // The command that moves the highlight to a row's centre, a printf
        // format of its y ("SET Highlight.Position.y {%.1f}"; ZGE2 sets a
        // vector whole).
        const char *highlightAt;
        // Colours as the scripts write them ("234 234 234 255").
        const char *nameColor;
        const char *valueColor;
        const char *selectedColor;
        // A heading row's colour: the section title's (null: valueColor).
        const char *headingColor = nullptr;
        // The port's words in the game's language (UTF-8), or the English
        // (null: English).
        std::string (*translate)(const std::string &english);
        // The rows' group: its OBJECT type and its properties, one per line
        // ("OnCreate\t\"SET Visible {0}\"\nVisible\t0\n"); each row label's
        // charset, scale ("0.750000 0.750000 1.000000"), and the properties
        // after its Text (the engine's own label properties).
        const char *rowsType;
        const char *rowsProperties;
        const char *rowCharset;
        const char *rowScale;
        const char *rowLabelTail;
    };

    // The page's style for this game; the script functions use it.
    void setStyle(const Style &style);

    // A page template's "@ROWS@" (a line of its own, children two levels
    // below the template's top object) replaced by the rows' group; depth 1
    // for children one level below.
    std::string withRows(std::string page, const Layout &layout, int depth = 2);

    // The page's script functions, by name, for the game's script engine.
    struct Function
    {
        const char *name;
        PS2Machine::RecompiledFunction fn;
    };
    const std::vector<Function> &pageFunctions();

    // The heading jump's keys, L2 and R2 on the open page, without a line
    // in the page's script: the game's key sniffer draw is wrapped, and on
    // the page's own sniffer (its dialog's KeySniffer, found with the game's
    // findNode when the page opens) a new L2 or R2 press in the pad reads
    // (PSPadBackend::buttonPresses) runs the jump before the sniffer's draw.
    struct PageKeys
    {
        uint32_t drawSniffer = 0;     // the key sniffer's Draw handler
        bool drawGetsMessage = false; // its argument is the engine message (ZGE2), not the sniffer
        uint32_t findNode = 0;        // (root, path[, kind]): a node below root
        bool findTakesKind = false;   // findNode's third argument, the node kind (ZGE3, ZGE4)
    };
    bool installPageKeys(PS2Machine &runtime, const PageKeys &keys);
}
