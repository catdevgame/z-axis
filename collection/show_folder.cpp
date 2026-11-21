#include "collection/show_folder.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <iostream>
#include <system_error>

namespace zx::collection
{
    std::string fileUrl(const std::filesystem::path &folder)
    {
        // (the generic form: "/" between the parts, Windows' too)
        std::u8string utf8 = folder.generic_u8string();
        // A Windows path starts with its drive ("C:/..."): the URL's path
        // is "/C:/...".
        auto drive = [](const std::u8string &p)
        { return p.size() >= 2 && p[1] == u8':' && std::isalpha(static_cast<unsigned char>(p[0])); };
        // A path from where the app runs ("Mods"): from the root, as a URL's
        // path is (else its first part would be read as a host).
        if (!folder.has_root_directory() && !drive(utf8))
        {
            std::error_code ec;
            if (const std::filesystem::path whole = std::filesystem::absolute(folder, ec); !ec)
                utf8 = whole.generic_u8string();
        }
        std::string path(utf8.begin(), utf8.end());
        if (drive(utf8))
            path.insert(path.begin(), '/');
        static const char kHex[] = "0123456789ABCDEF";
        std::string url = "file://";
        for (size_t i = 0; i < path.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(path[i]);
            const bool drive = c == ':' && i == 2 && path[0] == '/';
            if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || drive)
                url += static_cast<char>(c);
            else
            {
                url += '%';
                url += kHex[c >> 4];
                url += kHex[c & 15];
            }
        }
        return url;
    }

    bool showFolder(const std::filesystem::path &folder)
    {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::string url = fileUrl(folder);
        if (!SDL_OpenURL(url.c_str()))
        {
            std::cout << "[app] " << folder.string() << " not shown: " << SDL_GetError() << std::endl;
            return false;
        }
        std::cout << "[app] shown: " << url << std::endl;
        return true;
    }

    std::string folderNotice(const std::string &name, const std::filesystem::path &folder)
    {
        return name + ": " + folder.string();
    }
}
