// What the app offers for this player's setup (collection/offered.h).
#include "collection/offered.h"
#include "runtime/panel_snapshot.h"

#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace zx::app
{
    namespace
    {
        namespace fs = std::filesystem;
        using O = GameOption;

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        fs::path folderOf(const GetEnv &bootText, const char *name)
        {
            const char *text = bootText ? bootText(name) : nullptr;
            if (!text || !text[0])
                return {};
            const std::string value(text);
            const fs::path path(std::u8string(value.begin(), value.end())); // (UTF-8 text)
            std::error_code ec;
            return fs::is_directory(path, ec) ? path : fs::path();
        }

        // `dir`'s entry named `name` in any case ("" none).
        fs::path entryOf(const fs::path &dir, const std::string &name)
        {
            std::error_code ec;
            if (dir.empty())
                return {};
            if (fs::exists(dir / name, ec))
                return dir / name;
            const std::string wanted = upper(name);
            for (fs::directory_iterator it(dir, ec), last; !ec && it != last; it.increment(ec))
                if (upper(it->path().filename().string()) == wanted)
                    return it->path();
            return {};
        }

        // `relative` ('/' separated) below `root`, each part in any case ("" none).
        fs::path find(const fs::path &root, std::string_view relative)
        {
            fs::path at = root;
            size_t start = 0;
            while (!at.empty() && start <= relative.size())
            {
                const size_t slash = relative.find('/', start);
                const std::string part(relative.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
                if (!part.empty())
                    at = entryOf(at, part);
                if (slash == std::string_view::npos)
                    break;
                start = slash + 1;
            }
            return at;
        }

        bool isFile(const fs::path &root, std::string_view relative)
        {
            std::error_code ec;
            const fs::path file = find(root, relative);
            return !file.empty() && fs::is_regular_file(file, ec);
        }

        bool isFolder(const fs::path &root, std::string_view relative)
        {
            std::error_code ec;
            const fs::path dir = find(root, relative);
            return !dir.empty() && fs::is_directory(dir, ec);
        }

        // A folder holding a file of one of `extensions` (upper case, with the dot).
        bool holdsFileOf(const fs::path &root, std::string_view relative, std::initializer_list<const char *> extensions)
        {
            std::error_code ec;
            const fs::path dir = find(root, relative);
            if (dir.empty() || !fs::is_directory(dir, ec))
                return false;
            for (fs::directory_iterator it(dir, ec), last; !ec && it != last; it.increment(ec))
                for (const char *extension : extensions)
                    if (upper(it->path().extension().string()) == extension && it->is_regular_file(ec))
                        return true;
            return false;
        }

        // The Xbox files' assets folder: the folder given is the disc's root,
        // its media folder or the assets folder (runtime/xbox_disc.h).
        fs::path xboxAssets(const fs::path &root)
        {
            if (root.empty())
                return {};
            if (const fs::path assets = find(root, "media/assets"); !assets.empty())
                return assets;
            if (const fs::path assets = find(root, "assets"); !assets.empty())
                return assets;
            return upper(root.filename().string()) == "ASSETS" ? root : fs::path();
        }

        void setPlatform(Installed &installed)
        {
#if defined(__ANDROID__)
            installed.set(Need::SecondWindow, false);
            installed.set(Need::WindowModes, false);
            installed.set(Need::Borderless, false);
#else
            installed.set(Need::SecondWindow);
            installed.set(Need::WindowModes);
#if defined(__APPLE__) || defined(_WIN32)
            installed.set(Need::Borderless);
#endif
#endif
            installed.set(Need::BottomScreen, panel::bottomScreenUp());
        }
    }

    Installed platformInstalled()
    {
        Installed installed;
        setPlatform(installed);
        return installed;
    }

    Installed findInstalled(const GetEnv &bootText)
    {
        Installed installed = platformInstalled();

        // Aggressive Inline's Xbox disc: its files, and what the setup made of them.
        const fs::path xbox = xboxAssets(folderOf(bootText, "ZX_XBOX_DIR"));
        installed.set(Need::XboxText, isFile(xbox, "string.fre"));
        installed.set(Need::XboxSpeech, isFile(xbox, "audio/speech01.zsd"));
        installed.set(Need::XboxLevelScripts, isFile(xbox, "levels/CIVIC/CIVIC.bin"));
        installed.set(Need::XboxFreeSkate, isFile(xbox, "levels/BRDWLK/BRDWLKT.bin") || isFile(xbox, "levels/CANNERY/CANNERYT.bin"));
        installed.set(Need::XboxApe, isFile(folderOf(bootText, "ZX_XBOX_CONVERTED"), "ASSETS/SKATERS/GORILLA/GOR.ZIT") &&
                                         isFile(xbox, "uilib.zal"));
        const fs::path pack = folderOf(bootText, "ZX_XBOX_PACK");
        installed.set(Need::XboxTextures, isFolder(pack, "textures"));
        installed.set(Need::XboxSkaters, isFolder(pack, "models"));
        installed.set(Need::XboxMovies, isFolder(pack, "movies"));

        // Aggressive Inline's GameCube disc.
        installed.set(Need::Estate, !folderOf(bootText, "ZX_ESTATE_DIR").empty());

        // Dave Mirra BMX 2's Xbox disc.
        const fs::path dm2 = folderOf(bootText, "ZX_DM2_XBOX_LEVELS_DIR");
        installed.set(Need::Dm2XboxLevels, isFile(dm2, "levels.txt"));
        installed.set(Need::Dm2XboxThemes, isFolder(dm2, "themes"));
        installed.set(Need::Dm2RetroMovie, isFile(dm2, "movies/ASSETS/MOVIE/RETRO.PSS") || isFile(dm2, "movies/ASSETS/MOVIE/RETRO.BIK"));

        // BMX XXX's Xbox and GameCube discs.
        const fs::path bmx = folderOf(bootText, "ZX_BMX_RELEASES_DIR");
        installed.set(Need::BmxText, isFolder(bmx, "strings"));
        installed.set(Need::BmxChallenges, isFolder(bmx, "challenges"));
        installed.set(Need::BmxEditorPieces, isFile(bmx, "editor/OBJECT.OBJ"));
        installed.set(Need::BmxLevelPreviews, holdsFileOf(bmx, "movies/ASSETS/MOVIES/MENU", {".PSM", ".XMV"}));
        installed.set(Need::BmxLogoMovie, isFile(bmx, "movies/ASSETS/MOVIES/ZAXIS.PSS") || isFile(bmx, "movies/ASSETS/MOVIES/ZAXIS.XMV"));
        installed.set(Need::BmxGcLevels, isFolder(bmx, "levels/ASSETS/LEVELS"));
        installed.set(Need::BmxTrailers, holdsFileOf(bmx, "movies/ASSETS/MOVIES/COMPEDGE", {".PSS", ".XMV"}));
        installed.set(Need::BmxCreate, isFile(bmx, "create/added.txt"));
        installed.set(Need::BmxSongs, isFile(bmx, "music/songs.txt"));
        return installed;
    }

    Installed findInstalled(const BootFolders &folders)
    {
        std::map<std::string, std::string> texts; // boot name -> folder, as the boot config holds them
        for (const auto &[name, dir] : releaseFolders(folders))
            texts[name] = dir.string();
        return findInstalled([&texts](const char *name) -> const char *
        {
            const auto it = texts.find(name);
            return it == texts.end() ? nullptr : it->second.c_str();
        });
    }

    Need needOf(std::string_view gameId, GameOption option)
    {
        if (gameId == "ai")
            switch (option)
            {
            case O::Language: return Need::XboxText;
            case O::FrenchVoices: return Need::XboxSpeech;
            case O::CareerScripts: return Need::XboxLevelScripts;
            case O::XboxFreeSkate: return Need::XboxFreeSkate;
            case O::Ape: return Need::XboxApe;
            case O::Estate: return Need::Estate;
            default: return Need::Nothing; // (the Zoo: the PS2 disc's own without the Xbox's)
            }
        if (gameId == "dm2")
            switch (option)
            {
            case O::Dm2XboxLevels: return Need::Dm2XboxLevels;
            case O::Dm2XboxThemes: return Need::Dm2XboxThemes;
            case O::Dm2RetroMovie: return Need::Dm2RetroMovie;
            case O::Dm2CreatePlayer: return Need::Dm2ReadableRider;
            default: return Need::Nothing; // (the attract demo: level 3's own replay without the Xbox's)
            }
        if (gameId == "bmx")
            switch (option)
            {
            case O::Language: return Need::BmxText;
            case O::Tuning: return Need::BmxChallenges; // (Syracuse's timers are in those scripts)
            case O::BmxXboxChallenges: return Need::BmxChallenges;
            case O::BmxEditorPieces: return Need::BmxEditorPieces;
            case O::BmxLevelPreviews: return Need::BmxLevelPreviews;
            case O::BmxLogoMovie: return Need::BmxLogoMovie;
            case O::BmxGcLevels: return Need::BmxGcLevels;
            case O::BmxLogoSplash: return Need::BmxLogoMovie;
            case O::BmxTrailers: return Need::BmxTrailers;
            case O::BmxCarTextures: return Need::BmxCreate;
            case O::BmxGreenSkin: return Need::BmxCreate;
            case O::BmxTopless: return Need::BmxCreate;
            case O::BmxXboxSongs: return Need::BmxSongs;
            default: return Need::Nothing; // (the intro movie is the PS2 disc's own)
            }
        return Need::Nothing;
    }

    bool offered(const GameSettingsProfile &game, GameOption option, const Installed &installed)
    {
        return game.has(option) && installed[needOf(game.id, option)];
    }

    bool offered(Need need, const Installed &installed)
    {
        return installed[need];
    }
}
