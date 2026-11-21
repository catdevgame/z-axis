#pragma once

// The Z-Axis Collection's shell: the app opens on its menu (a tile per game,
// with each game's own pictures from its disc), boots the chosen game in the
// same process and window, and comes back to the menu when the game is left
// (ps2_host::requestExitToMenu: the game's own quit item, File > Switch Game,
// the switch_game key). Closing the window quits the app, in a game too.
//
// The menu is drawn by the app, not by a game: zgui, the engine's UI, is the
// game's own code running in its memory (zge/zgui), so it exists only while a
// game is booted. What keeps the games' look is their own files: each tile
// is a picture the game itself shows (a loading or start screen), and the
// text is drawn with Aggressive Inline's menu font, all read on the host from
// the discs bundled with the app. The picture is made on the CPU and shown
// as a game frame is (ps2_host::present).
//
// Nothing here needs the window library: the app (collection/app.cpp)
// gives the shell its input and presents the canvas it draws.

#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zx::collection
{
    // ---- pictures ----------------------------------------------------------

    // RGBA, 8 bits each, one uint32_t per pixel: R in the low byte (the byte
    // order ps2_host::Frame::rgba wants), top row first.
    struct Image
    {
        int width = 0;
        int height = 0;
        std::vector<uint32_t> pixels;

        bool empty() const { return width <= 0 || height <= 0; }
        uint32_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]; }
    };

    constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return static_cast<uint32_t>(r) | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b) << 16 |
               static_cast<uint32_t>(a) << 24;
    }

    // The colour of a page's help line (the chosen row's one line under the
    // rows), and of the player's messages over a game (PS2DebugPanel::setNoticeStyle).
    constexpr uint32_t kHelpColour = rgba(170, 170, 180);

    struct Rect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        bool contains(float px, float py) const { return px >= x && py >= y && px < x + width && py < y + height; }
        bool empty() const { return width <= 0 || height <= 0; }
    };

    // A raw picture file as the engine's showRawImage reads it
    // (zge/render/screen_capture.cpp): width x height pixels of 24 (RGB) or
    // 32 (RGBA, alpha ignored) bits, top row first. False if the file is
    // shorter.
    bool readRawImage(const std::vector<uint8_t> &file, int width, int height, int bitsPerPixel, Image &out);

    // A Windows BMP of 4 or 8 bits per pixel with its palette's alpha (the
    // engine's ZMP font pages; zge/native/texture_library.cpp reads the same
    // files without alpha), or of 24 or 32 plain bits (a custom map's
    // preview), uncompressed.
    bool readBmpImage(const std::vector<uint8_t> &file, Image &out);

    // The files of an archive (.ZAL: a ZDIR chunk whose OFST and DLEN tables
    // give each file's offset and length; DLEN's top four bits are flags not
    // read here) as (offset, length) pairs inside it; a file that is no
    // archive is one file. Files that would run past its end are left out.
    std::vector<std::pair<size_t, size_t>> archiveFiles(const std::vector<uint8_t> &file);

    // Every row twice: a field picture (half the screen's height) as shown.
    Image doubleRows(const Image &image);

    // ---- the engine's bitmap font ------------------------------------------

    // A font file (.ZFN, the layout of zge/text/font.h: a 6-byte header, the
    // pages, the glyphs) with its one page (the ZMP it names).
    struct BitmapFont
    {
        struct Glyph
        {
            bool present = false;
            int top = 0; // below the line's top
            int u = 0, v = 0, width = 0, height = 0;
        };
        int height = 0; // a line's height
        std::array<Glyph, 256> glyphs{};
        Image page;

        bool empty() const { return height <= 0 || page.empty(); }
    };
    // False if the file is not a font of one page or a glyph lies outside
    // the page.
    bool readFont(const std::vector<uint8_t> &file, Image page, BitmapFont &out);

    // ---- the canvas --------------------------------------------------------

    enum class Fit
    {
        Cover,   // fills the rectangle, the picture's overflow cut off
        Contain, // the whole picture in the rectangle, centred
    };

    // Draws into an RGBA picture (opaque: alpha is 255 everywhere).
    class Canvas
    {
    public:
        Canvas(int width, int height);
        int width() const { return m_image.width; }
        int height() const { return m_image.height; }
        const Image &image() const { return m_image; }

        void clear(uint32_t colour);
        // Blends colour (its alpha) over the rectangle.
        void fill(const Rect &rect, uint32_t colour);
        // An outline `thickness` pixels wide inside the rectangle.
        void frame(const Rect &rect, int thickness, uint32_t colour);
        // The part `source` of the picture into `target` (fitted, bilinear),
        // its colours times brightness (0-1), blended by its alpha times
        // opacity, cut to `clip` (empty: the target).
        void draw(const Image &picture, const Rect &source, const Rect &target, Fit fit, float brightness = 1.0f,
                  float opacity = 1.0f, Rect clip = {});

        // A line of text (no line breaks) with its top-left at (x, y),
        // `scale` times the font's size, in `colour` (the font's own shading
        // times it). Returns its width.
        int text(const BitmapFont &font, int x, int y, std::string_view line, float scale, uint32_t colour);
        static int textWidth(const BitmapFont &font, std::string_view line, float scale);

    private:
        // "%" in a line of text (the fonts have none that reads as one), in the box given.
        void percentSign(float x, float top, float width, float height, uint32_t colour);

        Image m_image;
    };

    // ---- the menu ----------------------------------------------------------

    // One frame's input to the menu, from the keyboard, the mouse and the
    // gamepads (menuInput), or a menu script.
    struct MenuInput
    {
        bool left = false, right = false, up = false, down = false;
        bool confirm = false; // Enter, Space, Cross, Start
        bool back = false;    // Escape, Circle, the right mouse button
        bool pointerMoved = false;
        float pointerX = 0.0f, pointerY = 0.0f; // canvas pixels
        bool click = false;                     // left button pressed at the pointer
        int scroll = 0;                         // mouse wheel steps, away from the player positive

        bool any() const { return left || right || up || down || confirm || back || pointerMoved || click || scroll != 0; }
    };

    // Keys (ps2_host_input codes) and gamepad buttons (PadButton bits) that
    // went down this frame, as menu input: the arrows, W A S D (the keys the
    // games' controls.ini give the left stick, which moves in their menus
    // too) and the D-pad move, Enter, keypad Enter, Space, Cross and Start
    // choose, Escape and Circle go back.
    MenuInput menuInput(const std::vector<int> &keysPressed, uint16_t padButtonsPressed);

    // Whether a key (a ps2_host_input code) moves the menu, and which way.
    enum class MenuDirection
    {
        None,
        Left,
        Right,
        Up,
        Down,
    };
    MenuDirection menuDirectionOfKey(int key);

    // A direction held down goes again and again, as a key held in a text
    // field does: the press is the first move (menuInput's), then after
    // kDelay seconds one more every kInterval seconds while it is held.
    // Keys, the D-pad and the stick alike, so a range (Master Volume) runs
    // from end to end with one hold.
    class HoldRepeat
    {
    public:
        static constexpr double kDelay = 0.4;
        static constexpr double kInterval = 0.08;

        // The directions held at `now` (seconds, any clock that only goes
        // forward): those due to go again are set in `in`. A direction let
        // go starts over.
        void update(MenuInput &in, bool left, bool right, bool up, bool down, double now);

    private:
        struct Held
        {
            double since = -1.0; // when it was first held, below 0 not held
            int repeats = 0;     // repeats made since
        };
        // (one at a time: a hold that missed frames goes once, not in a burst)
        static bool due(Held &held, bool down, double now);
        Held m_left, m_right, m_up, m_down;
    };

    // The device whose buttons the bottom line names: the one the player
    // last used (the keyboard or the mouse, a gamepad by its face-button
    // labels, or touch).
    enum class HintDevice
    {
        Keyboard,
        PlayStation,
        Xbox,
        Nintendo,
        Touch
    };

    // The bottom line, as the games write theirs: the choose button and its
    // verb, then the back button and its ("Enter Select   Esc Back", "Cross
    // Select   Circle Back", "A Select   B Back"; by position, so Nintendo's
    // are B and A). No choose part when `choose` is null ("Esc Stop"), no
    // back part when `back` is null; "" on touch.
    std::string buttonHint(HintDevice device, const char *choose, const char *back = nullptr);

    // Where the menu's parts are on a canvas of width x height for `games`
    // games, as a cabinet: the title at the top left with Settings and Quit
    // at the top right, the chosen game's riders at the sides, its name and
    // when it was last played in the middle, a card per game in a row below
    // them (the shape of the games' title pictures), the hint at the bottom.
    struct Layout
    {
        Rect title;
        std::vector<Rect> tiles;
        Rect name;
        Rect lastPlayed;
        Rect settings;
        Rect quit;
        Rect riderLeft, riderRight; // the chosen game's two cut-outs (Backdrop::riders)
        Rect hint;
        Rect version; // the app's version, small at the right of the hint's line
    };
    Layout layout(int width, int height, size_t games);

    // ---- the app's settings pages ----------------------------------------------

    // What the menu shows: its tiles, or one of the app's pages over them:
    // Settings, the Mods page it opens and the page of one mod that opens,
    // and Custom Freeplay's two pages (the player, then the map).
    enum class MenuPage
    {
        Tiles,
        Settings,
        Mods,
        Games,          // each game's files from the player's disc: add, check, repair, remove (Settings > Games)
        Mod,            // one mod: what it is, its switch in each game, its place in the order
        FreeplayPlayer, // Custom Freeplay: a player from one of the games (its tile)
        FreeplayMap,    // then a custom map for it
        Recordings,     // the player's recordings, each played back (Settings > Recordings)
        SaveProfiles,   // each game's careers: which one it plays, a new one, a copy (Settings > Save Profiles)
    };

    // What the menu asks for after a frame's input.
    struct MenuAction
    {
        enum Kind
        {
            None,
            Boot,
            Quit,
            Play, // a recording played back (the Recordings page's row)
        } kind = None;
        int game = -1;
        int map = -1; // Boot: the custom map (the index the app gave its row), -1 the game as it is
        int recording = -1; // Play: the recording (the index the app gave its row)
    };

    // A row of the menu's own pages (the app's Settings before any game has
    // booted: display, sound; the Mods page; Custom Freeplay's maps), read
    // and changed through the functions the app gives it. The page knows
    // the rows only by these. A Page row with `current` and `apply` has a
    // switch of its own (a mod on the Mods page): a tick box at its label
    // that Left, Right and a click on the box flip, Enter and a click
    // elsewhere on the row opening its page.
    struct SettingsRow
    {
        enum Kind
        {
            Toggle,  // Off / On
            Choice,  // one of choices
            Range,   // minValue..maxValue by step, with the unit after it
            Page,    // opens the page `opens` (Enter, Right, a click); its value is summary's
            Action,  // does what activate says (Enter, a click): a map to start; its value is summary's
            Heading, // a title over the rows after it (a game's name): never chosen
            Note,    // a line of text (no mods there), its value summary's when it has one: never chosen
            Check,   // a tick box (Enter, Left, Right, a click flip it: current 0 or 1), its value summary's
        } kind = Toggle;
        std::string label;
        std::vector<std::string> choices;
        int minValue = 0, maxValue = 1, step = 1;
        std::string unit;
        std::string help; // one line under the rows while the row is chosen
        std::function<int()> current;
        std::function<void(int)> apply;
        MenuPage opens = MenuPage::Tiles;   // Page
        std::function<std::string()> summary; // Page, Action: its value ("2 of 3 on")
        std::function<MenuAction()> activate; // Action: what happens (a Boot with its map)
        std::shared_ptr<const Image> picture; // shown beside the rows while the row is chosen (a map's preview)
        // A mark at the label's left (a Check's is its tick box, from
        // current): a tick, an empty box, a cross. Null: none.
        enum class Mark
        {
            None,
            Tick,
            Box,
            Cross,
        };
        std::function<Mark()> mark;
        // How far the row's work is, 0-1, as a bar along the row's foot
        // (a game being set up); below 0 or null: no bar.
        std::function<float()> progress;
        // A line of a list (the discs found): one fixed size whatever its
        // text (never made smaller to fit), its label from the left `indent`
        // steps in, its value in a column of its own, both left-aligned.
        bool steady = false;
        int indent = 0;
        // A Note the selection stops on (its help, a disc's file, shown); Enter does nothing.
        bool selectable = false;
        // The bottom line's word for what Enter does to it ("Start": a map),
        // null for its kind's ("Select", "Change", "Tick").
        const char *verb = nullptr;

        // Whether the selection stops on it (not a Heading or a Note).
        bool choosable() const { return kind != Heading && (kind != Note || selectable); }
        // The value's text (Toggle: Off / On; Choice: the choice; Range: the
        // number and the unit; Page, Action, Check, Note: its summary;
        // Heading: none).
        std::string valueText(int value) const;
        // The value `steps` to the right (negative: left): a Toggle, a
        // Check and a Page's switch flip, a Choice goes round (the last to
        // the first), a Range stops at its ends.
        int moved(int value, int steps) const;
        // Whether it is a Page with a switch of its own (current and apply).
        bool pageSwitch() const { return kind == Page && current && apply; }
        // Its mark now: `mark`'s, else a Check's and a Page's switch's tick
        // box (from current), else none.
        Mark markNow() const;
    };

    // Where a page's parts are on a canvas of width x height for `rows`
    // rows: the title, a line per row shown (its label at the left, its
    // value at the right), the help under them, Back at the bottom. Rows
    // that do not fit at the usual spacing come closer; more than fit even
    // then are shown as a window of rows.size() lines that scrolls with the
    // selection (Shell::pageFirst). With `picture` the rows take the left
    // part and a picture (the chosen row's) the right.
    struct PageLayout
    {
        Rect title;
        std::vector<Rect> rows; // the lines shown, top first
        int labelX = 0; // where a row's label starts
        int valueX = 0; // where a row's value ends (right-aligned)
        Rect help;
        Rect notice;    // a message's line, between the help and Back
        Rect back;
        Rect picture;   // empty: the page shows none
    };
    PageLayout pageLayout(int width, int height, size_t rows, bool picture = false);

    // A line made to fit `width`: the largest scale up to `scale` (down to
    // `least`) at which it does, and the line cut short with "..." when even
    // `least` is too wide.
    struct FittedLine
    {
        std::string text;
        float scale = 1.0f;
        int width = 0;
    };
    FittedLine fitLine(const BitmapFont &font, const std::string &line, int width, float scale, float least);

    // A text made to fit `width` as the bottom screen's goal texts are
    // (collection/panel.cpp fittedLines): never cut, it shrinks from `scale`
    // (down to `floor`) while it takes more than `most` lines or a word is
    // wider than the room, then wraps at its spaces into as many as it needs
    // (a word wider than the room on a line of its own).
    struct FittedText
    {
        std::vector<std::string> lines;
        float scale = 1.0f;
        int widest = 0; // its widest line
    };
    FittedText fitText(const BitmapFont &font, const std::string &text, int width, float scale, float floor, size_t most = 1);

    // A row's label and value in the room from `left` to `right`: the label
    // from `left`, the value ending at `right`, at least `gap` apart, both
    // smaller when they do not fit (down to 0.6 of `scale`), then the label
    // cut short (the value, when the label takes half the room or less).
    // Neither leaves the room nor runs into the other.
    struct RowText
    {
        FittedLine label;
        FittedLine value;
        int labelX = 0, valueX = 0;
    };
    RowText layRowText(const BitmapFont &font, const std::string &label, const std::string &value, int left, int right, float scale,
                       int gap);

    // ---- the Mods page ---------------------------------------------------------

    // One mod as the Mods page shows it (collection/mods.h gives the app the
    // mods; this is what the pages show of one), in the order the games
    // read them.
    struct ModEntry
    {
        std::string title;
        std::string version, author; // "" none
        std::string where;           // "Mods/hd.zip", "games/ai/Mods/10-park"
        std::string contents;        // "files, music, textures"
        bool logic = false;          // it changes game logic (files, tuning, a map)
        bool map = false;            // a custom map: Custom Freeplay's, no game's switch
        std::string problem;         // why no game reads it ("" none)
        std::vector<std::string> notes; // its parts no game reads
        struct Game
        {
            std::string title;   // the game's name
            bool on = true;      // its switch there
            bool read = true;    // the game reads it
            std::string why;     // not read: why ("Off", "needs base-pack")
            std::string winners; // where it and another have the same file: "wins 3 over HD Rails" ("" none)
            std::vector<std::string> entriesNotRead; // archive entries it gives that the game does not read (Mod's)
        };
        std::vector<Game> games; // the games it is for
    };
    // The Mods page's rows: a row per mod, numbered in the order, each
    // opening its page (MenuPage::Mod; the row's index is the mod's), its
    // value where it is on; a Note when there are none (`folder`: where mods
    // go). With `switched`, a mod with switches has one on its row too, a
    // tick box: on while some game it is for has it on; Left, Right or a
    // click on the box turns it off in every game it is for when on, else
    // on in all of them, calling `switched(mod, on)` (the app keeps the
    // change and lists the mods again).
    std::vector<SettingsRow> modsPageRows(const std::vector<ModEntry> &mods, const std::string &folder,
                                          std::function<void(size_t mod, bool on)> switched = {});
    // One mod's page: its name over what it is (where, by whom, what it has,
    // whether it changes game logic), a switch per game it is for (and why a
    // game does not read it), Read earlier and Read later (its place in the
    // order), what it wins and loses. Switching one changes `mods` and calls
    // `switched(game, on)`; a move calls `moved(-1 or 1)`.
    std::vector<SettingsRow> modPageRows(const std::shared_ptr<std::vector<ModEntry>> &mods, size_t mod,
                                         std::function<void(size_t game, bool on)> switched, std::function<void(int steps)> moved);
    // "2 of 3 on", "None": the Settings page's Mods row (a mod is on when
    // some game reads it).
    std::string modsSummary(const std::vector<ModEntry> &mods);

    // ---- Custom Freeplay ---------------------------------------------------------

    // What a game's player is called on the player page ("Inline skater",
    // "BMX rider"), by collection id; "Player" for a game not known.
    const char *playerType(std::string_view game);
    // A game's name short enough for one line of the menu's help ("Dave
    // Mirra BMX 2"), by collection id.
    const char *shortGameName(std::string_view game);

    // A custom map as the map page lists it for the player's game
    // (collection/maps.h gives the app the maps; this is what the page
    // shows of one).
    struct MapChoice
    {
        std::string name;
        std::string builtFor;  // the short name of the game it was made in (shortGameName)
        std::string author;    // "" none
        std::string where;     // the folder it was found in, for the page's text ("Maps/park")
        bool playable = false; // the player's game can start it
        std::string why;       // not playable: why, one line (a conversion that failed: its reason)
        std::shared_ptr<const Image> preview; // its picture, null for none
        int game = -1;         // the player's game (the Boot action's)
        int map = -1;          // the app's index of it (the Boot action's)
    };
    // The map page's rows for the player's game `gameTitle`: a Heading, then
    // a row per map, each map once whatever game it was made in: an Action
    // (its Boot) for a playable one, its help saying where it was made and
    // by whom, the same line for every player's game; a Note for one the game cannot
    // start (with why: a conversion that failed says its reason); a Note
    // when there is none (`folder`: where maps go). `lends`: whether the
    // game lends a slot this setup can start (false: one Note saying so, or
    // `why` when given, instead of the maps).
    std::vector<SettingsRow> freeplayMapRows(const std::vector<MapChoice> &maps, const std::string &gameTitle,
                                             const std::string &folder, bool lends, const std::string &why = {});

    // ---- the shell ---------------------------------------------------------

    enum class State
    {
        Menu, // the menu is shown
        Game, // a game runs
        Quit, // the app ends
    };
    const char *stateName(State state);

    // How a game's run ended.
    enum class GameEnd
    {
        ExitToMenu, // the game was left (requestExitToMenu)
        Quit,       // the window was closed, Cmd+Q, Alt+F4
        Failed,     // it did not boot (its files, its program): back to the menu
    };

    // The shell's states: Menu -> Game (boot) -> Menu (the game left, or it
    // failed to boot) ... -> Quit (from the menu's Quit, or a window close
    // anywhere). The menu's items are the games (0..games-1), the Custom
    // Freeplay tile when it is on, then Quit and Settings; the selection
    // comes back to the game last played. Settings opens the app's settings
    // page in the menu (MenuPage::Settings): its input moves over the page's
    // rows and changes them until Back. A Page row opens its page
    // (MenuPage::Mods, and from it MenuPage::Mod), whose Back comes back to it. The Custom Freeplay tile
    // opens the player page (the tiles of the games whose custom maps
    // start: Left, Right, Enter), then the map page (rows: a map's Enter
    // is a Boot with that map). The Recordings page's rows are each a Play:
    // play() runs the game with the recording, and its end comes back to the
    // page on the same row.
    class Shell
    {
    public:
        explicit Shell(size_t games, bool freeplay = false);

        // The first state: the menu, or straight into a game (a developer's
        // --game ID; -1: none).
        void start(int game = -1);

        State state() const { return m_state; }
        // The game running (State::Game), else -1.
        int game() const { return m_state == State::Game ? m_game : -1; }
        // The game last booted, -1 before any.
        int lastGame() const { return m_lastGame; }
        size_t games() const { return m_games; }
        // Whether the Custom Freeplay tile is on the menu (the app's setting).
        bool freeplay() const { return m_freeplay; }
        void setFreeplay(bool on);
        // The menu item chosen: a game's index, freeplayItem() for the
        // Custom Freeplay tile (-1 when off), then Quit and Settings.
        int selection() const { return m_selection; }
        int freeplayItem() const { return m_freeplay ? static_cast<int>(m_games) : -1; }
        int quitItem() const { return static_cast<int>(m_games) + (m_freeplay ? 1 : 0); }
        int settingsItem() const { return quitItem() + 1; }
        // Custom Freeplay: the game whose player is chosen (the player page's
        // tile; the map page lists that game's maps), and whether the player
        // page's selection is on Back.
        int freeplayGame() const { return m_freeplayGame; }
        bool freeplayOnBack() const { return m_playerOnBack; }
        // The games the player page offers (their indices, in tile order):
        // those whose custom maps start with the setup as it is (every game
        // until set). A player no longer offered is left for the first one
        // offered, or Back when there is none.
        const std::vector<int> &freeplayPlayers() const { return m_players; }
        void setFreeplayPlayers(std::vector<int> games);
        // The custom map the running (or last) game was booted with, -1 none.
        int map() const { return m_map; }
        // Games booted so far.
        int boots() const { return m_boots; }
        // The recording the running game plays back (play()), else -1.
        int playback() const { return m_state == State::Game ? m_playback : -1; }
        // The Mods page's row that opened the mod page shown (its mod's index).
        int modsRow() const { return m_modsRow; }
        // The bottom screen's buttons (collection/menu_bottom.h): a game's
        // card chosen (its index; on the player page, that player), and a
        // page opened straight from the tiles (Settings, or Mods or Games
        // with Settings behind them for Back, or Custom Freeplay's player
        // page); the Games page from Settings too (the discs a drop found).
        void choose(int game);
        void showPage(MenuPage page);
        // The mod page shown is now the mod of row `row` (a move changed its
        // place in the order); Back comes back to that row.
        void followMod(int row) { m_modsRow = row; }
        // What the menu shows, and on a settings page the row chosen
        // (rows.size() is Back) and the first row shown (a page with more
        // rows than its layout shows scrolls).
        MenuPage page() const { return m_page; }
        int pageRow() const { return m_pageRow; }
        int pageFirst() const { return m_pageFirst; }
        // The row chosen among `rows` (the page's): pageRow, or the first
        // row after it the selection can stop on (a page opens on its
        // first row, which may be a Heading).
        int chosenRow(const std::vector<SettingsRow> &rows) const;
        // Counts up at every change the menu would show (the selection, the
        // page, its row, a value changed): the app redraws when it differs.
        int revision() const { return m_revision; }

        // The menu's input for one frame: moves the selection (the pointer
        // over a tile, Quit or Settings chooses it) and says what to do. A
        // Boot or Quit action is applied by boot() / quit(); Settings opens
        // the page, whose rows (`rows`, laid out as `page`: the rows of the
        // page() shown) the input then moves over and changes (Left, Right,
        // Enter; a click on the chosen row) until Back (Escape, Circle, or
        // the Back line). Up and Down pass over Headings and Notes; the
        // mouse wheel scrolls a page longer than its lines. On the
        // player page `layout` is that page's (a tile per game) and `page`
        // gives its Back.
        MenuAction input(const MenuInput &in, const Layout &layout, const std::vector<SettingsRow> &rows = {},
                         const PageLayout &page = {});

        // Menu -> Game, with a custom map (the Boot action's) or none. False
        // if not on the menu or no such game.
        bool boot(int game, int map = -1);
        // Menu -> Game, playing back a recording (the Play action's): the
        // page and its row stay as they are for the menu after it.
        bool play(int game, int recording);
        // Game -> Menu (ExitToMenu, Failed) or Quit.
        void gameEnded(GameEnd end);
        // Anything -> Quit.
        void quit();

    private:
        MenuAction tilesInput(const MenuInput &in, const Layout &layout);
        MenuAction playerInput(const MenuInput &in, const Layout &layout, const PageLayout &page);
        MenuAction pageInput(const MenuInput &in, const std::vector<SettingsRow> &rows, const PageLayout &page);
        void changeRow(const std::vector<SettingsRow> &rows, int steps);
        void openPage(MenuPage page);
        void closePage();
        int tileCount() const { return static_cast<int>(m_games) + (m_freeplay ? 1 : 0); }

        size_t m_games = 0;
        bool m_freeplay = false;
        State m_state = State::Menu;
        int m_game = -1;
        int m_lastGame = -1;
        int m_map = -1;
        int m_playback = -1;
        int m_selection = 0;
        int m_boots = 0;
        MenuPage m_page = MenuPage::Tiles;
        int m_pageRow = 0;
        int m_pageFirst = 0;
        int m_settingsRow = 0; // the Settings page's row while a page it opened is shown
        int m_modsRow = 0;     // the Mods page's row while the mod page it opened is shown
        int m_freeplayGame = 0;
        bool m_playerOnBack = false;
        std::vector<int> m_players; // the player page's games
        // The player page's place of `game` among its players, -1 if not offered.
        int playerPlace(int game) const;
        // The chosen player offered still: else the first one, or Back with none.
        void keepPlayerOffered();
        int m_revision = 0;
    };

    // ---- scripted menu input (ZX_MENU_SCRIPT) ----------------------------

    // Comma-separated steps, one every `framesPerStep` menu frames, across
    // every visit to the menu (a visit starts its count again): a key name
    // (ps2_host_input's: Left, Right, Up, Down, Enter, Space, Escape), a
    // gamepad button (pad:cross, pad:circle, pad:start, pad:left, ...),
    // wait, or game:<id>: Left or Right, a step each, until the card chosen
    // is that game's (by its collection id, wherever the menu shows it).
    // Fed through menuInput as real keys and buttons are. drop:<path>
    // drops the file or folder on the window, as the player's drop does
    // (a check's way to a folder of discs: the file dialog takes no script).
    class MenuScript
    {
    public:
        struct Step
        {
            int key = -1;        // a ps2_host_input code
            int padButton = -1;  // a PadButton
            std::string game;    // game:<id>, lower case
            std::string drop;    // drop:<path>, as written
        };

        // The cards the menu shows now (the games' ids, left to right) and
        // the one chosen (-1: none, the selection is elsewhere), for game:
        // steps.
        struct Cards
        {
            std::vector<std::string> games;
            int chosen = -1;
        };

        // False and the reason for a step it cannot read.
        bool parse(const std::string &spec, std::string *error = nullptr);
        bool empty() const { return m_steps.empty(); }
        size_t size() const { return m_steps.size(); }
        // A new visit to the menu.
        void beginVisit() { m_visitFrame = 0; }
        // The input of the visit's next frame; true while steps remain. A
        // game: step gives Left or Right until `cards` has its game chosen
        // (Down first when no card is: from Settings and Quit), then passes;
        // a game the menu does not show, or one not reached in as many moves
        // as there are cards and two, is passed over with `*problem` said.
        // A drop: step's path goes into `dropped` (none given: passed over).
        bool next(std::vector<int> &keys, uint16_t &padButtons, const Cards &cards, std::string *problem = nullptr,
                  std::vector<std::string> *dropped = nullptr);
        bool next(std::vector<int> &keys, uint16_t &padButtons); // (no cards: a game: step is passed over)

        static constexpr int kFramesPerStep = 20;

    private:
        std::vector<Step> m_steps;
        size_t m_next = 0;
        int m_visitFrame = 0;
        int m_moves = 0; // moves made for the game: step under way
    };

    // ---- each game's pictures ------------------------------------------------

    // Where a game's tile picture is on its disc, and the part of it that
    // matters (the logo; empty: all of it).
    struct ArtSource
    {
        const char *game;          // collection id
        const char *file;          // below the disc folder
        enum Kind
        {
            Raw,          // readRawImage(width, height, bits)
            RawField,     // the same, every row twice
            LibraryImage, // a texture of a texture library (TXR, ZIF, ZAL) by name
        } kind = Raw;
        int width = 0, height = 0, bits = 0;
        const char *texture = ""; // LibraryImage
        Rect focus;               // in the picture as shown (after doubleRows)
    };
    // nullptr for a game without one.
    const ArtSource *artSource(std::string_view game);

    // The font the menu writes with, on Aggressive Inline's disc.
    constexpr const char *kFontGame = "ai";
    constexpr const char *kFontFile = "ASSETS/MENU/FNT_14PT.ZFN";
    constexpr const char *kFontPage = "ASSETS/MENU/FNT_14~1.ZMP";

    // What the menu shows for a game.
    struct Tile
    {
        std::string name;       // the game's name
        Image art;              // empty: none on its disc (a plain tile)
        Rect focus;             // of art; empty: all of it
        std::string lastPlayed; // under the name: when its save was last written ("" none)
        std::string player;     // Custom Freeplay's player page: what its player is called (playerType)
        uint32_t colour = rgba(220, 220, 220); // its frame, its page's rule (collection/front_art.h gameColour)
        bool installed = true;  // false: not set up, the tile says Set Up over its colour
        std::string cardName;   // on its card when it has no picture ("" the name): a short one fits ("Dave Mirra BMX 2")
    };

    // What moves behind the menu (collection/menu_scene.h makes it from
    // the games' discs): the shown game's menu movie's picture or its
    // still's part, its riders, and the fades of the intro and a boot. All
    // empty: the chosen game's tile picture behind everything, dark, or
    // with no picture at all the games' colours in stripes.
    struct Backdrop
    {
        const Image *picture = nullptr; // null: none
        Rect part;                      // of picture; empty: all of it
        const Image *riders[2] = {nullptr, nullptr}; // left, right (null: none)
        float riderOpacity = 1.0f;      // their fade as the pair changes
        float black = 0.0f;             // 0-1: black over everything (fading in from the intro or a game)
        float launch = 0.0f;            // 0-1: the chosen card grown to the whole screen as its game boots
    };

    // The Custom Freeplay tile's caption.
    constexpr const char *kFreeplayTitle = "Custom Freeplay";

    // "Last played <day month year>" for a save file written at `time` (the
    // file's last write, as time_t).
    std::string lastPlayedText(std::time_t time);

    // Draws the whole menu for the shell's selection: the tiles (the Custom
    // Freeplay tile after the games' when the shell has it: the games'
    // pictures side by side), or on a page (MenuPage::Settings, Mods,
    // FreeplayMap) the page of `rows` laid out as `page`, or the player page
    // (the tiles of the shell's players as `layout` places them, with their
    // players' names).
    // `notice`: a line over the hint ("" none), e.g. why a game did not start.
    // `device`: whose buttons the bottom line names (buttonHint).
    // `backdrop`: what moves behind it all (Backdrop).
    // A page's `subtitle`: a line under its title ("" none). Its bottom line
    // names what Enter does to the chosen row (the page's Back stands for the
    // back button); `escapeVerb`: the back button's word instead, on a page
    // with no Back (page.back empty: "Stop" while a setup runs).
    // `version`: the app's version ("1.2.0"), small at the right of the
    // tiles' bottom line ("" none).
    void drawMenu(Canvas &canvas, const Layout &layout, const std::vector<Tile> &tiles, const BitmapFont &font,
                  const Shell &shell, const std::string &title, const std::string &notice,
                  const std::vector<SettingsRow> &rows = {}, const PageLayout &page = {},
                  HintDevice device = HintDevice::Keyboard, const Backdrop &backdrop = {},
                  const std::string &subtitle = {}, const char *escapeVerb = nullptr, const std::string &version = {});

    // The intro's picture: a movie's picture fitted to the canvas on black,
    // `black` (0-1) of black over it (its fade out), and the skip hint at
    // the bottom ("" none).
    void drawIntro(Canvas &canvas, const Image &picture, const BitmapFont &font, const std::string &hint, float black);
}
