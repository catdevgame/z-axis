// Dave Mirra Freestyle BMX 2 (SLUS-20159): the park editor's two Xbox-only
// themes, Desert and Highway, after the nine stock ones in the theme select
// ([extras] xbox_themes, ZX_DM2_XBOX_THEMES=1; off by default).
//
// The Xbox disc holds editor/leDesert and editor/leHighWa: complete themes
// in the PS2's formats (TBMP textures, the PS2's chunk sets), early exports
// (Park_Editor_Alpha_Exports) that every release's SCENE.TXT declares and no
// release's THEMES.TXT lists. tools/modding/dm2_xbox.py makes them, at app
// build, into <ZX_DM2_XBOX_LEVELS_DIR>/themes:
//   - <FOLDER>/<FOLDER>.{ZIT,GEO,INS,TXR,DAT}: raised 15 m (their ring of
//     ground lay at level 0, below every stock theme's), with the stock
//     Beach's light and Globals points (the 32 x 32 outdoor theme whose frame
//     they share): they had no light points, so the relight left them dark.
//   - THEMES.TXT: their THEME blocks, Beach's (AMBIANT, CURSOR, size, BORDER,
//     MAP) with their NAME and FILE.
//   - names.txt: "<FOLDER> <Beach's FILE> <name>" per theme.
//   - PICTURES.ZIF: each theme's picture for the theme select (no release
//     has one), drawn from the theme's own files as the stock pictures show
//     theirs: from above a corner of the floor, its empty cells' towers at
//     the MAP's level, lit by the AMBIANT, its clear colour behind; named
//     by its value ("9", "10").
// This file serves them; nothing here reads a disc image or converts
// anything:
//   - assets\editor\themes.txt gets their blocks before its END, so
//     readThemes lists them after the stock ones (theme 9, 10, ...); the
//     themes' record pool (newEditPool's records of an EditTheme, 10 for
//     the nine) gets one more record each.
//   - ASSETS/EDITOR/<FOLDER>/ is their folder; the park's sound bank of a
//     theme position, SFXLEV1<position>.ZSD (sound_mode.cpp), is the donor's
//     (Beach's SFXLEV13.ZSD: the disc has one bank per stock theme only).
//   - The theme select (UILIB ParkMenu.SelectTheme, MaxValue 8) goes up to
//     the last of them; their name and text rows ($KSTR_THEMENAME_<value>,
//     $KSTR_THEMEDESC_<value>) are put in the string tables (no release has
//     any: the theme's name, and a line saying where it is from); their
//     pictures (Themes.<value>) go into UILIB's ParkMenu.zif.
//   - bmxSetupTheme locks a theme at or past the saved game's open theme
//     count (5 at first, 10 with the cheat): theirs are open.
//
// A park saved on one of them names its theme by number: with the setting
// off that number is no theme (findTheme answers none) and the park does not
// load as it was made.
//
// ZX_TRACE_XBOXTHEMES=1 (developers): each file served.

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_xboxthemes.cpp.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/disc_patch.h"
#include "runtime/game_files.h"
#include "runtime/session.h"
#include "runtime/zaxis_text.h"
#include "runtime/zaxis_ui.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace dave_mirra::xbox_themes
{
    namespace
    {
        namespace gf = zx::game_files;
        namespace fs = std::filesystem;

        constexpr const char kTag[] = "[xbox-themes]";
        constexpr const char kOwner[] = "dm2-xbox-themes";
        constexpr int kRegA0 = 4, kRegA1 = 5, kRegSp = 29;

        // An EditTheme record's bytes (games/dm2/editor/edit_themes.h), what
        // newEditPool is asked for by the themes' pool alone.
        constexpr uint32_t kThemeRecordBytes = 0x2088;
        // The text their rows show (no release has one).
        constexpr const char kThemeText[] = "An unused park editor theme\\nfrom the Xbox release.";

        struct Theme
        {
            std::string folder; // "LEDESERT"
            std::string donor;  // the stock theme's FILE whose sound bank it takes ("LEBeach")
            std::string name;   // "Desert"
        };

        struct State
        {
            fs::path dir; // <ZX_DM2_XBOX_LEVELS_DIR>/themes
            std::vector<Theme> themes;
            std::string blocks;            // their THEME blocks
            std::vector<std::string> stock; // the disc's THEMES.TXT FILEs, in order (the theme select's values)
            bool trace = false;
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> served;
            std::unique_ptr<gf::GameFileSystem> files;
            PS2Machine::RecompiledFunction newEditPool = nullptr;
            PS2Machine::RecompiledFunction setupTheme = nullptr;
        };
        State &state() { return zx::session::state<State>(); }

        std::string upper(std::string text)
        {
            for (char &c : text)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return text;
        }

        std::shared_ptr<const std::vector<uint8_t>> readHost(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return nullptr;
            return std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // The game's own files (the disc, the Mods folder), for the donor's sound bank and the stock themes.
        gf::GameFileSystem &files()
        {
            std::unique_ptr<gf::GameFileSystem> &fileSystem = state().files;
            if (!fileSystem)
            {
                const PS2Machine::IoPaths &paths = PS2Machine::getIoPaths();
                gf::GameFileRoots roots;
                if (!paths.cdFromImage)
                    roots.disc = paths.cdRoot;
                if (roots.disc.empty())
                    roots.image = PS2Machine::cdImageFileSystem();
                if (const char *mods = zx::boot::text("ZX_MODS_DIR"); mods && *mods)
                    roots.mods = gf::modsRoots(mods);
                fileSystem = std::make_unique<gf::GameFileSystem>(std::move(roots));
            }
            return *fileSystem;
        }
        std::shared_ptr<const std::vector<uint8_t>> readGameFile(const std::string &path)
        {
            const std::shared_ptr<gf::GameFile> file = files().open(path);
            auto bytes = std::make_shared<std::vector<uint8_t>>();
            if (!file || !gf::readAll(*file, *bytes))
                return nullptr;
            return bytes;
        }

        // The FILE of each THEME block of a THEMES.TXT, in order.
        std::vector<std::string> themeFiles(const std::string &text)
        {
            std::vector<std::string> out;
            std::istringstream in(text);
            std::string word;
            bool inTheme = false;
            while (in >> word)
            {
                if (word == "THEME")
                    inTheme = true;
                else if (inTheme && word == "FILE" && (in >> word))
                {
                    if (word.size() >= 2 && word.front() == '"' && word.back() == '"')
                        word = word.substr(1, word.size() - 2);
                    out.push_back(word);
                    inTheme = false;
                }
                else if (word == "END")
                    break;
            }
            return out;
        }

        // The theme select's value of our i-th theme.
        uint32_t value(size_t i) { return static_cast<uint32_t>(state().stock.size() + i); }

        // ---- the files ----------------------------------------------------

        std::shared_ptr<const std::vector<uint8_t>> serve(const std::string &path, const std::function<std::shared_ptr<const std::vector<uint8_t>>()> &make)
        {
            State &st = state();
            std::lock_guard<std::mutex> lock(st.mutex);
            auto it = st.served.find(path);
            if (it != st.served.end())
                return it->second;
            auto bytes = make();
            st.served[path] = bytes;
            if (st.trace)
                std::cout << kTag << " " << path << ": " << (bytes ? std::to_string(bytes->size()) + " bytes" : std::string("none")) << std::endl;
            return bytes;
        }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            State &st = state();
            for (size_t i = 0; i < st.themes.size(); ++i)
            {
                const Theme &theme = st.themes[i];
                const std::string folder = "ASSETS/EDITOR/" + theme.folder + "/";
                if (path.rfind(folder, 0) == 0)
                    return serve(path, [&]() { return readHost(st.dir / theme.folder / path.substr(folder.size())); });
                // sound_mode.cpp's "SFXLEV1%1d.ZSD" of the park's theme position.
                if (path == "ASSETS/AUDIO/SFXLEV1" + std::to_string(value(i)) + ".ZSD")
                    for (size_t donor = 0; donor < st.stock.size(); ++donor)
                        if (upper(st.stock[donor]) == upper(theme.donor))
                            return serve(path, [&]() { return readGameFile("ASSETS/AUDIO/SFXLEV1" + std::to_string(donor) + ".ZSD"); });
            }
            return nullptr;
        }

        // assets\editor\themes.txt: ours before its END.
        bool patchThemes(std::vector<uint8_t> &bytes)
        {
            State &st = state();
            std::string text(bytes.begin(), bytes.end());
            const size_t end = text.rfind("END");
            if (end == std::string::npos || themeFiles(text).size() != st.stock.size())
            {
                std::cerr << kTag << " themes.txt is not the one read at the start: no themes added" << std::endl;
                return false;
            }
            text.insert(end, st.blocks + "\r\n");
            bytes.assign(text.begin(), text.end());
            std::cout << kTag << " themes.txt: " << st.themes.size() << " themes after the " << st.stock.size()
                      << " stock ones" << std::endl;
            return true;
        }

        // The theme select's last value, and its pictures.
        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            State &st = state();
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            bool changed = false;
            // ParkMenu.zif ("LOADLIB Themes UILIB:ParkMenu.zif"): our pictures, named by value.
            if (const auto pictures = readHost(st.dir / "PICTURES.ZIF"))
            {
                std::vector<std::string> names;
                for (size_t i = 0; i < st.themes.size(); ++i)
                    names.push_back(std::to_string(value(i)));
                for (zx::zaxis::ArchiveEntry &entry : entries)
                    if (entry.nameHash == zx::zaxis_text::keyHash("PARKMENU.ZIF"))
                    {
                        const size_t added = zx::zaxis::addAssets(entry.data, *pictures, names).size();
                        changed = changed || added != 0;
                        std::cout << kTag << " ParkMenu.zif: " << added << " pictures" << std::endl;
                    }
            }
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (zx::zaxis::scriptName(entry) != "ParkMenu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                zx::zaxis::Span select = zx::zaxis::findObject(text, "ParkMenu/SelectTheme");
                const zx::zaxis::Span last = select.found() ? zx::zaxis::findProperty(text, select, "MaxValue") : zx::zaxis::Span{};
                const std::string stockLast = std::to_string(st.stock.size() - 1);
                if (!last.found() || text.compare(last.begin, stockLast.size(), stockLast) != 0 ||
                    text.find_first_not_of("\r\n \t", last.begin + stockLast.size()) < last.end)
                {
                    std::cerr << kTag << " ParkMenu.SelectTheme is not the expected script: the themes are not offered" << std::endl;
                    continue;
                }
                text.replace(last.begin, stockLast.size(), std::to_string(value(st.themes.size() - 1)));
                entry.data.assign(text.begin(), text.end());
                changed = true;
            }
            if (changed)
                bytes = zx::zaxis::writeArchive(entries);
            return changed;
        }

        // Their name and text rows.
        bool patchStrings(std::vector<uint8_t> &bytes)
        {
            State &st = state();
            zx::zaxis_text::StringTable table;
            if (!zx::zaxis_text::readStringTable(bytes, table))
                return false;
            auto set = [&](const std::string &key, const std::string &text)
            {
                const uint32_t hash = zx::zaxis_text::keyHash(key);
                for (auto &entry : table.entries)
                    if (entry.first == hash)
                    {
                        entry.second = text;
                        return;
                    }
                table.entries.emplace_back(hash, text);
            };
            for (size_t i = 0; i < st.themes.size(); ++i)
            {
                set("KSTR_THEMENAME_" + std::to_string(value(i)), st.themes[i].name);
                set("KSTR_THEMEDESC_" + std::to_string(value(i)), kThemeText);
            }
            bytes = zx::zaxis_text::writeStringTable(table);
            return true;
        }

        // ---- the hooks ----------------------------------------------------

        // newEditPool(record bytes, records): the themes' pool a record
        // longer for each of ours.
        void newEditPoolHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            if (getRegU32(ctx, kRegA0) == kThemeRecordBytes)
                SET_GPR_U32(ctx, kRegA1, getRegU32(ctx, kRegA1) + static_cast<uint32_t>(st.themes.size()));
            st.newEditPool(rdram, ctx, runtime);
        }

        // bmxSetupTheme: the theme select's lock as the game sets it, then
        // ours open.
        void setupThemeHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            st.setupTheme(rdram, ctx, runtime);
            const uint32_t stack = (getRegU32(ctx, kRegSp) - 0x400u) & ~0xFu;
            const uint32_t word = stack + 0x100u;
            uint32_t root = 0, node = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, sym::dm2::g_uiRoot))
                std::memcpy(&root, p, 4);
            if (!runtime->callGuestFunction(rdram, ctx, sym::dm2::findNode, {root, sym::dm2::selectThemePath, 0u}, stack, &node) ||
                node == 0)
                return;
            runtime->callGuestFunction(rdram, ctx, sym::dm2::copyVariable, {node, sym::dm2::g_textCurValueLower, word}, stack);
            float theme = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, word))
                std::memcpy(&theme, p, 4);
            if (theme < static_cast<float>(st.stock.size()))
                return;
            if (uint8_t *p = getMemPtr(rdram, word))
                std::memset(p, 0, 4);
            runtime->callGuestFunction(rdram, ctx, sym::dm2::setVariable, {node, sym::dm2::g_textThemeLock, word}, stack);
        }

        bool wrap(PS2Machine &runtime, uint32_t function, PS2Machine::RecompiledFunction &original,
                  PS2Machine::RecompiledFunction hook, const char *what)
        {
            original = runtime.lookupFunction(function);
            if (original && runtime.replaceFunction(function, hook))
                return true;
            std::cerr << kTag << " " << what << " not hooked" << std::endl;
            original = nullptr;
            return false;
        }

        // names.txt, THEMES.TXT and each theme's five files.
        bool readBundle(State &st)
        {
            std::ifstream names(st.dir / "names.txt");
            std::string line;
            while (std::getline(names, line))
            {
                std::istringstream in(line);
                Theme theme;
                if (!(in >> theme.folder >> theme.donor) || !std::getline(in >> std::ws, theme.name) || theme.name.empty())
                    continue;
                std::error_code ec;
                bool complete = true;
                for (const char *ext : {"ZIT", "GEO", "INS", "TXR", "DAT"})
                    complete = complete && fs::is_regular_file(st.dir / theme.folder / (theme.folder + "." + ext), ec);
                if (complete)
                    st.themes.push_back(std::move(theme));
                else
                    std::cerr << kTag << " " << theme.folder << " is not complete: left out" << std::endl;
            }
            const auto blocks = readHost(st.dir / "THEMES.TXT");
            if (!blocks || st.themes.empty())
                return false;
            st.blocks.assign(blocks->begin(), blocks->end());
            return themeFiles(st.blocks).size() == st.themes.size();
        }
    }

    bool install(PS2Machine &runtime)
    {
        State &st = state();
        const char *on = zx::boot::text("ZX_DM2_XBOX_THEMES");
        const char *dir = zx::boot::text("ZX_DM2_XBOX_LEVELS_DIR");
        if (!on || on[0] != '1')
            return false;
        if (!dir || !*dir)
        {
            std::cout << kTag << " off: no ZX_DM2_XBOX_LEVELS_DIR (the files tools/modding/dm2_xbox.py makes)" << std::endl;
            return false;
        }
        st.dir = fs::path(dir) / "themes";
        st.trace = std::getenv("ZX_TRACE_XBOXTHEMES") != nullptr;
        if (!readBundle(st))
        {
            std::cerr << kTag << " off: " << st.dir.string() << " has no themes made by tools/modding/dm2_xbox.py" << std::endl;
            return false;
        }
        const auto themes = readGameFile("ASSETS/EDITOR/THEMES.TXT");
        if (themes)
            st.stock = themeFiles(std::string(themes->begin(), themes->end()));
        if (st.stock.empty())
        {
            std::cerr << kTag << " off: the game's THEMES.TXT has no theme" << std::endl;
            return false;
        }
        if (!wrap(runtime, sym::dm2::newEditPool, st.newEditPool, &newEditPoolHook, "newEditPool") ||
            !wrap(runtime, sym::dm2::bmxSetupTheme, st.setupTheme, &setupThemeHook, "bmxSetupTheme"))
            return false;
        gf::registerProvider("ASSETS/", &provide, kOwner);
        zx::disc_patch::registerPatch("THEMES.TXT", &patchThemes, kOwner);
        zx::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary, kOwner);
        for (const char *strings : {"STRING.EUS", "STRING.EUK", "STRING.GER", "STRING.FRE"})
            zx::disc_patch::registerPatch(strings, &patchStrings, kOwner);
        std::cout << kTag << " on:";
        for (size_t i = 0; i < st.themes.size(); ++i)
            std::cout << (i ? "," : "") << " " << st.themes[i].name << " (theme " << value(i) << ")";
        std::cout << std::endl;
        return true;
    }
}

namespace dave_mirra
{
    bool installXboxThemes(PS2Machine &runtime)
    {
        return xbox_themes::install(runtime);
    }
}
