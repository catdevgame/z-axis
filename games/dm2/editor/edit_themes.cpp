// DM2's park editor themes (edit_themes.h). DM2's own code.

#include "edit_themes.h"
#include "editor_state.h"
#include "edit_core.h"
#include "park_grid.h"
#include "edit_pools.h"
#include "edit_lights.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"

#include <cstdint>
#include "sym/dm2/editor/edit_themes.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The themes' record pool (an index into the editor's pools) and the
        // themes in the file's order; how many were read.
        inline ee::Global<int8_t, sym::g_themePool> g_themePool;
        inline ee::Global<zge::List, sym::g_themeList> g_themeList;
        inline ee::Global<uint8_t, sym::g_themeCount> g_themeCount;
        inline ee::Global<ee::Ptr<EditTheme>, sym::g_currentTheme> g_currentTheme;
        // The park's theme, as its place on the list.
        inline ee::Global<uint32_t, sym::g_parkThemePosition> g_parkThemePosition;

        constexpr uint32_t kThemePoolRecords = 10;
        constexpr uint32_t kTokenBytes = 0x100;

        constexpr ee::Ptr<const char> kThemesFilePath{sym::themesFilePath}; // "assets\editor\themes.txt"
        constexpr ee::Ptr<const char> kEntryRails{sym::toolEntryRails};     // "Rails"
        constexpr ee::Ptr<const char> kEntryStart{sym::toolEntryStart};     // "Start"
        constexpr ee::Ptr<const char> kEntryGaps{sym::toolEntryGaps};       // "Gaps"
        constexpr ee::Ptr<const char> kKeyClose{sym::themeKeyClose};        // "}"
        constexpr ee::Ptr<const char> kKeyName{sym::themeKeyName};          // "NAME"
        constexpr ee::Ptr<const char> kKeyFile{sym::themeKeyFile};          // "FILE"
        constexpr ee::Ptr<const char> kKeyHeight{sym::themeKeyHeight};      // "HEIGHT"
        constexpr ee::Ptr<const char> kKeyWidth{sym::themeKeyWidth};        // "WIDTH"
        constexpr ee::Ptr<const char> kKeyCursor{sym::themeKeyCursor};      // "CURSOR"
        constexpr ee::Ptr<const char> kKeyOffset{sym::themeKeyOffset};      // "OFFSET"
        constexpr ee::Ptr<const char> kKeyAmbient{sym::themeKeyAmbient};    // "AMBIANT"
        constexpr ee::Ptr<const char> kKeySupportLight{sym::themeKeySupportLight}; // "SUPPORTLIGHT"
        constexpr ee::Ptr<const char> kKeyBorder{sym::themeKeyBorder};      // "BORDER"
        constexpr ee::Ptr<const char> kKeyMap{sym::themeKeyMap};            // "MAP"
        constexpr ee::Ptr<const char> kKeyEnd{sym::themesKeyEnd};           // "END"
        constexpr ee::Ptr<const char> kKeyTheme{sym::themesKeyTheme};       // "THEME"

        // Scaffolding (address-call): the editor's pools, its tool entries
        // and its cells are other files' (the editor core, the park's cells)
        // and have no readable body yet.
        ee::Ptr<EditTheme> newThemeRecord(int8_t pool) { return ee::Ptr<EditTheme>(editPoolAllocate(pool)); }

        ee::Ptr<EditTheme> firstTheme()
        {
            return zge::recordOfNode<EditTheme>(g_themeList->head);
        }
        ee::Ptr<EditTheme> nextTheme(ee::Ptr<EditTheme> theme)
        {
            return zge::recordOfNode<EditTheme>(theme->link.next);
        }
        uint32_t mapCell(uint8_t column, uint8_t row)
        {
            return column + row * kThemeMapStride;
        }
    }

    void initEditThemes()
    {
        *g_themePool = newEditPool(sizeof(EditTheme), kThemePoolRecords);
        zge::initList(&*g_themeList);
        readThemes();
        *g_currentTheme = ee::Ptr<EditTheme>();
    }
    READABLE_FUNCTION_WAITS(initEditThemes, Verified);

    void clearEditThemes(ee::Ptr<ParkEditor> editor)
    {
        clearParkCells(editor);
        game::tail(zge::initList, &*g_themeList);
    }
    READABLE_FUNCTION(clearEditThemes, Verified);

    uint8_t themeWidth(uint8_t theme)
    {
        return findTheme(theme)->width;
    }
    READABLE_FUNCTION(themeWidth, Verified);

    uint32_t applyThemeLighting(uint8_t theme)
    {
        const ee::Ptr<EditTheme> found = findTheme(theme);
        if (found->supportsLights)
            offerLightTool();
        setParkAmbientColour(&found->ambient);
        return found->supportsLights;
    }
    READABLE_FUNCTION(applyThemeLighting, Verified);

    uint8_t themeHeight(uint8_t theme)
    {
        return findTheme(theme)->height;
    }
    READABLE_FUNCTION(themeHeight, Verified);

    uint8_t themeBorder(uint8_t theme)
    {
        return findTheme(theme)->border;
    }
    READABLE_FUNCTION(themeBorder, Draft);

    ee::Ptr<char> themeFile(uint8_t theme)
    {
        return ee::Ptr<char>(findTheme(theme)->file);
    }
    READABLE_FUNCTION(themeFile, Verified);

    uint8_t themeCellHeight(uint8_t theme, uint8_t column, uint8_t row)
    {
        const ee::Ptr<EditTheme> found = findTheme(theme);
        return ee::Ptr<uint8_t>(found->cellHeights)[mapCell(column, row)];
    }
    READABLE_FUNCTION(themeCellHeight, Verified);

    uint8_t themeCellKind(uint8_t theme, uint8_t column, uint8_t row)
    {
        const ee::Ptr<EditTheme> found = findTheme(theme);
        return ee::Ptr<uint8_t>(found->cellKinds)[mapCell(column, row)];
    }
    READABLE_FUNCTION(themeCellKind, Verified);

    void openParkTheme()
    {
        const ee::Ptr<ParkEditor> editor = parkEditor();
        addEditToolEntry(kEditToolRail, kToolEntryNoCategory, kEntryRails, 1);
        addEditToolEntry(kEditToolStart, kToolEntryNoCategory, kEntryStart, 1);
        addEditToolEntry(kEditToolGap, kToolEntryNoCategory, kEntryGaps, 1);
        uint32_t position = 0;
        for (ee::Ptr<EditTheme> theme = firstTheme(); theme; theme = nextTheme(theme), ++position)
        {
            if (position != *g_parkThemePosition)
                continue;
            setUpParkTheme(editor, theme->index);
            selectEditToolEntry();
            editor->cursorColumn = static_cast<uint8_t>(theme->cursorColumn);
            editor->cursorRow = static_cast<uint8_t>(theme->cursorRow);
            break;
        }
    }
    READABLE_FUNCTION_WAITS(openParkTheme, Verified);

    ee::Ptr<EditTheme> findTheme(uint8_t theme)
    {
        for (ee::Ptr<EditTheme> each = firstTheme(); each; each = nextTheme(each))
            if (each->index == theme)
                return each;
        return ee::Ptr<EditTheme>();
    }
    READABLE_FUNCTION(findTheme, Verified);

    void setThemeCellKind(ee::Ptr<EditTheme> theme, uint8_t column, uint8_t row, uint8_t kind)
    {
        ee::Ptr<uint8_t>(theme->cellKinds)[mapCell(column, row)] = kind;
    }
    READABLE_FUNCTION(setThemeCellKind, Verified);

    ee::Ptr<EditTheme> readTheme()
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<zge::VertexColour> ambient;
        ee::Local<int32_t> number;
        ee::Local<int32_t> red;
        ee::Local<int32_t> green;
        ee::Local<int32_t> blue;

        zge::nextToken(token, kTokenBytes); // the block's "{"
        const ee::Ptr<EditTheme> theme = newThemeRecord(*g_themePool);
        theme->supportsLights = 0;
        theme->offset = 0;
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kKeyClose, token) != 0)
        {
            if (zge::compareStrings(kKeyName, token) == 0)
                zge::nextToken(ee::Ptr<char>(theme->name), kTokenBytes);
            else if (zge::compareStrings(kKeyFile, token) == 0)
                zge::nextToken(ee::Ptr<char>(theme->file), kTokenBytes);
            else if (zge::compareStrings(kKeyHeight, token) == 0)
            {
                zge::readNumberToken(number);
                theme->height = static_cast<uint8_t>(*number);
            }
            else if (zge::compareStrings(kKeyWidth, token) == 0)
            {
                zge::readNumberToken(number);
                theme->width = static_cast<uint8_t>(*number);
            }
            else if (zge::compareStrings(kKeyCursor, token) == 0)
            {
                zge::readNumberToken(&theme->cursorColumn);
                zge::readNumberToken(&theme->cursorRow);
            }
            else if (zge::compareStrings(kKeyOffset, token) == 0)
            {
                zge::readNumberToken(number);
                theme->offset = *number;
            }
            else if (zge::compareStrings(kKeyAmbient, token) == 0)
            {
                zge::readNumberToken(red);
                zge::readNumberToken(green);
                zge::readNumberToken(blue);
                ambient->red = static_cast<uint8_t>(*red);
                ambient->blue = static_cast<uint8_t>(*blue);
                ambient->green = static_cast<uint8_t>(*green);
                ambient->alpha = 0xFF;
                theme->ambient = *ambient;
            }
            else if (zge::compareStrings(kKeySupportLight, token) == 0)
                theme->supportsLights = 1;
            else if (zge::compareStrings(kKeyBorder, token) == 0)
            {
                zge::readNumberToken(number);
                theme->border = static_cast<uint8_t>(*number);
            }
            else if (zge::compareStrings(kKeyMap, token) == 0)
            {
                // A number a cell, a row after a row: 0, or the cell's height,
                // negative for the other kind of cell.
                for (uint8_t row = 0; row < theme->height; ++row)
                {
                    for (uint8_t column = 0; column < theme->width; ++column)
                    {
                        const ee::Ptr<uint8_t> height = ee::Ptr<uint8_t>(theme->cellHeights) + mapCell(column, row);
                        zge::readNumberToken(number);
                        if (*number == 0)
                        {
                            setThemeCellKind(theme, column, row, kThemeCellKind0);
                            *height = 0;
                        }
                        else if (*number < 0)
                        {
                            setThemeCellKind(theme, column, row, kThemeCellKind1);
                            *height = static_cast<uint8_t>(-static_cast<uint8_t>(*number));
                        }
                        else
                        {
                            setThemeCellKind(theme, column, row, kThemeCellKind3);
                            *height = static_cast<uint8_t>(*number);
                        }
                    }
                }
            }
            zge::nextToken(token, kTokenBytes);
        }
        return theme;
    }
    READABLE_FUNCTION(readTheme, Verified);

    void readThemes()
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<uint32_t> bytes;

        const ee::Ptr<zge::ZFile> file = zge::openFile(kThemesFilePath, zge::kFileModeRead, 0).file();
        zge::fileSize(file, bytes, 0);
        const uint32_t text = zge::heapAllocate(*bytes);
        zge::fileRead(file, text, *bytes, 0);
        zge::closeFile(file, 0);
        const ee::Ptr<zge::ZFile> memory = zge::openMemoryFile(text, *bytes);
        zge::beginScript(zge::filePath(memory));
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kKeyEnd, token) != 0)
        {
            if (zge::compareStrings(kKeyTheme, token) == 0)
            {
                const ee::Ptr<EditTheme> theme = readTheme();
                theme->index = static_cast<uint8_t>(g_themeList->count);
                zge::listPush(&*g_themeList, theme);
            }
            zge::nextToken(token, kTokenBytes);
        }
        *g_themeCount = static_cast<uint8_t>(g_themeList->count);
        zge::endScript();
        zge::closeFile(memory, 0);
        zge::heapFree(text);
    }
    READABLE_FUNCTION_WAITS(readThemes, Verified);

    ee::Ptr<EditTheme> currentTheme()
    {
        return *g_currentTheme;
    }
    READABLE_FUNCTION(currentTheme, Draft);

    int32_t themeOffset(uint8_t theme)
    {
        return findTheme(theme)->offset;
    }
    READABLE_FUNCTION(themeOffset, Verified);

    // The current theme stays as it was when no theme has the index.
    void setCurrentTheme(uint32_t theme)
    {
        for (ee::Ptr<EditTheme> each = firstTheme(); each; each = nextTheme(each))
        {
            if (each->index == theme)
            {
                *g_currentTheme = each;
                return;
            }
        }
    }
    READABLE_FUNCTION(setCurrentTheme, Verified);

    uint8_t currentThemeIndex()
    {
        return (*g_currentTheme)->index;
    }
    READABLE_FUNCTION(currentThemeIndex, Draft);

    ee::Ptr<char> currentThemeName()
    {
        return ee::Ptr<char>((*g_currentTheme)->name);
    }
    READABLE_FUNCTION(currentThemeName, Verified);
}
