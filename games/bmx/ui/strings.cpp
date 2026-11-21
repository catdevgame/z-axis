// BMX XXX's texts (strings.h): BMX XXX's own code (bmx.c).

#include "strings.h"

#include "bmx/profile/players.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/asset_library.h"
#include "zge/platform/platform.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/bmx/ui/strings.cpp.h"

namespace game::bmx
{
    namespace
    {
        // zgui's word for the table its texts come from.
        inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_uiStrings> g_uiStrings;
        // Each language's three letters.
        inline ee::Global<ee::Ptr<const char>[kLanguages], sym::g_languageCodes> g_languageCodes;
        constexpr ee::Ptr<const char> kStringLibrary{sym::g_textStringLibrary};     // "assets\string.zal"
        constexpr uint32_t kStringTableName = sym::bmx::g_textStringTableName; // "string.common.%s"
        constexpr uint32_t kFeetInches = sym::bmx::g_textFeetInches;           // %0ld' %ld"
        constexpr uint32_t kEllipsis = sym::bmx::g_textEllipsis;              // "..."
        // A foot in metres (0.3048), and its inches.
        inline ee::Global<ee::Float, sym::g_metresAFoot> g_metresAFoot;
        constexpr uint32_t kInchesAFoot = 12;
        // No callback for the library's file reads.
        constexpr uint32_t kNoReadCallback = 0;
    }

    ee::Ptr<zge::StringTable> loadStringLibrary(const char *name)
    {
        const uint32_t library = zge::openAssetLibrary(kStringLibrary);
        ee::Ptr<zge::StringTable> table;
        if (zge::findAsset(library, zge::nameHash(name)))
        {
            const uint32_t bytes = zge::currentAssetLength(library);
            const uint32_t data = zge::allocateMemory(bytes);
            zge::readAsset(library, data, kNoReadCallback);
            table = zge::loadStringTable(data, bytes);
            zge::freeMemory(data);
        }
        zge::closeAssetLibrary(library);
        return table;
    }
    READABLE_FUNCTION_WAITS(loadStringLibrary, Verified);

    void freeGameStrings(ee::Ptr<zge::StringTable> table)
    {
        game::tail(zge::freeStringTable, table);
    }
    READABLE_FUNCTION(freeGameStrings, Verified);

    uint32_t setLanguage(uint32_t language)
    {
        ee::Local<char[0x40]> name;
        if (language >= kLanguages)
            return 0;
        g_savedGame->language = static_cast<uint8_t>(language);
        if (*g_gameStrings)
            freeGameStrings(*g_gameStrings);
        zge::formatString(name, kStringTableName, (*g_languageCodes)[g_savedGame->language]);
        const ee::Ptr<zge::StringTable> table = loadStringLibrary(name);
        *g_uiStrings = table;
        *g_gameStrings = table;
        zge::setStringTable(table);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setLanguage, Verified);

    uint32_t defaultLanguage()
    {
        return 0;
    }
    READABLE_FUNCTION(defaultLanguage, Verified);

    // formatFeetInches: the nearest inch (formatString's result, the
    // original's tail jump).
    int32_t formatFeetInches(ee::Address out, ee::Float metres)
    {
        const uint32_t inches = static_cast<uint32_t>(ee::toInt(metres / *g_metresAFoot * ee::Float(12.0f) + ee::Float(0.5f)));
        return game::tail(zge::formatString, out, kFeetInches, inches / kInchesAFoot, inches % kInchesAFoot);
    }
    READABLE_FUNCTION(formatFeetInches, Verified);

    uint32_t copyOnOneLine(char *out, const char *text, uint32_t limit)
    {
        uint32_t length = 0;
        for (char c = *text++; c != '\0'; c = *text++)
        {
            if (c == '\r')
                continue;
            if (c == '\n' || c == ' ')
            {
                if (limit != 0 && limit < length)
                {
                    zge::formatString(out, kEllipsis);
                    return length + 3;
                }
                c = ' ';
            }
            *out++ = c;
            ++length;
        }
        *out = '\0';
        return length;
    }
    READABLE_FUNCTION(copyOnOneLine, Verified);

    uint32_t wrapText(char *out, const char *text, uint32_t width)
    {
        uint32_t column = 0;
        uint32_t braces = 0;
        uint32_t breaks = 0;
        for (char c = *text; c != '\0'; c = *++text)
        {
            if (c == '{')
                ++braces;
            else if (c == '}')
            {
                if (braces > 0)
                    --braces;
            }
            else if (braces == 0)
                ++column;
            if (column >= width && c == ' ' && braces == 0)
            {
                *out++ = '\\';
                column = 0;
                ++breaks;
                *out++ = 'n';
            }
            else
                *out++ = c;
        }
        *out = '\0';
        return breaks;
    }
    READABLE_FUNCTION(wrapText, Verified);
}
