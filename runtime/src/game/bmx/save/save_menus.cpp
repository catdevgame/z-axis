// BMX XXX's game code (bmx.c's park and game saves): Aggressive Inline's game/aggressive/save_menus.cpp, copied
// for BMX XXX. The save system's screens (zge/platform/save_system.h SaveClient: the game's and the park's save
// clients hand these over): the message line and the slot menu, through the SaveLoad pages (zgui paths
// and scripts). The choice of lines and the park file menu are BMX XXX's own.

#include "bmx/save/save_menus.h"
#include "bmx/save/park_save.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/save/save_menus.cpp.h"

namespace game::bmx
{
    namespace
    {
        // SaveLoad.SaveLoadMenu
        constexpr uint32_t kMessageText = sym::g_textSaveLoadSaveLoadMenuMessage;   // "...Message.Text"
        constexpr uint32_t kMessage = sym::g_textSaveLoadSaveLoadMenuMessage2;      // "...Message"
        constexpr uint32_t kNoText = sym::g_saveMessageEmpty;                       // ""
        constexpr uint32_t kFooterVisible = sym::g_textSaveLoadFooterVisible;
        constexpr uint32_t kFooterTriangleVisible = sym::g_textSaveLoadFooterTriangleVisible;
        constexpr uint32_t kFooterXVisible = sym::g_textSaveLoadFooterXVisible;
        constexpr uint32_t kMenuVisibleCount = sym::g_textSaveLoadSaveLoadMenuMemCardMenu; // "...MemCardMenu.VisibleCount"
        constexpr uint32_t kClearMenu = sym::g_textSetSaveLoadSaveLoadMenuMemCardMenu;   // SET ...Items {""}
        constexpr uint32_t kAddMenuItem = sym::g_textStrcatSaveLoadSaveLoadMenu;        // STRCAT ...Items {"%s%s"}
        constexpr uint32_t kItemBreak = sym::g_textMenuItemBreak;                           // "\n" in the script's text
        constexpr uint32_t kResetMenu = sym::g_textPlaySaveLoadMenuReset;
        constexpr uint32_t kMenuItems = sym::g_textCardMenuItems;                 // "...MemCardMenu.Items"
        constexpr uint32_t kMenuSelectedIndex = sym::g_textCardMenuSelectedIndex; // "...MemCardMenu.SelectedIndex"
        constexpr uint32_t kMenuAllowCancel = sym::g_textCardMenuAllowCancel;     // "...MemCardMenu.AllowCancel"
        constexpr uint32_t kHideMenu = sym::g_textHideCardMenu;                   // KILLFOCUS, SET ...MemCardMenu.Visible {0}

        // SaveLoad.ChooseParkMenu: the park file menu's three columns (name, author, theme).
        constexpr uint32_t kBreakFileList = sym::g_textStrcatSaveLoadChooseParkMenu;    // STRCAT ...FileListMenu.Items {"\n"}
        constexpr uint32_t kBreakAuthorList = sym::g_textStrcatSaveLoadChooseParkMenu2; // STRCAT ...AuthorMenu.Items {"\n"}
        constexpr uint32_t kBreakThemeList = sym::g_textStrcatSaveLoadChooseParkMenu3;  // STRCAT ...ThemeMenu.Items {"\n"}
        constexpr uint32_t kAddFileListItem = sym::bmx::g_textFileListAddText;          // STRCAT ...FileListMenu.Items {"%s"}
        constexpr uint32_t kNewFile = sym::g_textNew;                                   // "$KSTR_PARKED_SAVE_NEW"
        constexpr uint32_t kFileListVisibleCount = sym::g_textSaveLoadChooseParkMenuFileListMenu;
        constexpr uint32_t kAuthorListVisibleCount = sym::g_textSaveLoadChooseParkMenuAuthorMenu;
        constexpr uint32_t kThemeListVisibleCount = sym::g_textSaveLoadChooseParkMenuThemeMenu;
        constexpr uint32_t kParkMenuOnInit = sym::g_textPlaySaveLoadChooseParkMenu2;
        constexpr uint32_t kParkMenuOnShutdown = sym::g_textPlaySaveLoadChooseParkMenu;
        constexpr uint32_t kSaveInfoOnShutdown = sym::g_textPlaySaveLoadSaveInfoOnShutdown;
        constexpr uint32_t kMenuOnInit = sym::g_textPlaySaveLoadSaveLoadMenuOnInit;
        constexpr int32_t kParkMenuLines = 10;

        // Scaffolding (address-call): the engine's save system has this reply in BMX XXX's version only, and no
        // shared declaration yet: the save menu answered that there is no save to choose from.
        int32_t answerSaveListEmpty() { return ee::game<int32_t>(sym::answerSaveListEmpty); }

        // The park's save or load under way (park_save.cpp's): which it is, the saves in the menu's list,
        // and the park saves listed that read well (of any theme; the damaged ones are not counted).
        inline ee::Global<uint32_t, sym::g_parkSaving> g_parkSaving;
        inline ee::Global<uint32_t, sym::g_parkSaveFileCount> g_parkSaveFileCount;
        inline ee::Global<uint32_t, sym::g_parkSavesListed> g_parkSavesListed;

        // A line of the menus' script run (no widget); where the original hands on what runScript (void) left in
        // its result register, it is called at its address.
        void runMenuScript(ee::Address text) { zge::zgui::runScript(ee::Ptr<zge::zgui::Node>(), ee::Ptr<char>(text), 0); }
        int32_t runMenuScriptResult(ee::Address text) { return ee::game<int32_t>(sym::runScript, 0, text, 0); }
    }

    // showSaveMessage: the message line shows `text`, or is emptied; the
    // footer hidden.
    int32_t showSaveMessage(uint32_t text)
    {
        ee::Local<int32_t> hidden;
        *hidden = 0;
        if (text != 0)
            zge::zgui::setNodeValueByPath(kMessageText, text);
        else
            zge::zgui::setNodeValueByPath(kMessage, kNoText);
        return zge::zgui::setNodeValueByPath(kFooterVisible, hidden);
    }
    READABLE_FUNCTION_WAITS(showSaveMessage, Verified);

    // showSaveSlotMenu: the screen's menu filled with the null-ended list
    // of labels (the slots), one line each; then reset, its footer and both buttons'
    // hints shown.
    int32_t showSaveSlotMenu(ee::Ptr<uint32_t> labels)
    {
        ee::Local<char[0x100]> script;
        ee::Local<int32_t> lines;
        ee::Local<int32_t> shown;
        *shown = 1;
        *lines = 0;
        runMenuScript(kClearMenu);
        while (labels[*lines] != 0)
        {
            zge::formatString(script, kAddMenuItem, *lines != 0 ? kItemBreak : kNoText, labels[*lines]);
            runMenuScript(script);
            *lines = *lines + 1;
        }
        zge::zgui::setNodeValueByPath(kMenuVisibleCount, lines);
        runMenuScript(kResetMenu);
        zge::zgui::setNodeValueByPath(kFooterVisible, shown);
        zge::zgui::setNodeValueByPath(kFooterTriangleVisible, shown);
        return zge::zgui::setNodeValueByPath(kFooterXVisible, shown);
    }
    READABLE_FUNCTION_WAITS(showSaveSlotMenu, Verified);

    // showSaveChoice: the screen's menu shows `text`'s lines (a line break or
    // the two characters \n start another) with `selected` picked and its
    // footer shown, and is reset. A choice that may be cancelled shows both
    // buttons' hints; one that may not shows the confirm hint only. No
    // text hides the menu.
    int32_t showSaveChoice(ee::Ptr<const char> text, int32_t selected, int32_t allowCancel)
    {
        ee::Local<int32_t> lines;
        ee::Local<int32_t> selectedIndex;
        ee::Local<int32_t> shown;
        ee::Local<int32_t> hidden;
        *lines = 1;
        *selectedIndex = selected;
        *shown = 1;
        *hidden = 0;
        if (!text)
            return runMenuScriptResult(kHideMenu);
        for (int32_t i = 0; text[i] != 0; ++i)
            if (text[i] == '\n' || (text[i] == '\\' && text[i + 1] == 'n'))
                *lines = *lines + 1;
        zge::zgui::setNodeValueByPath(kMenuItems, text);
        zge::zgui::setNodeValueByPath(kMenuVisibleCount, lines);
        zge::zgui::setNodeValueByPath(kMenuSelectedIndex, selectedIndex);
        zge::zgui::setNodeValueByPath(kFooterVisible, shown);
        if (allowCancel != 0)
        {
            zge::zgui::setNodeValueByPath(kMenuAllowCancel, shown);
            zge::zgui::setNodeValueByPath(kFooterTriangleVisible, shown);
        }
        else
        {
            zge::zgui::setNodeValueByPath(kMenuAllowCancel, hidden);
            zge::zgui::setNodeValueByPath(kFooterTriangleVisible, hidden);
        }
        zge::zgui::setNodeValueByPath(kFooterXVisible, shown);
        return runMenuScriptResult(kResetMenu);
    }
    READABLE_FUNCTION_WAITS(showSaveChoice, Verified);

    // showParkFileMenu: the park file menu up (its lists ten lines; when
    // saving with room for another park, "New" after the saves
    // already listed) or down (with the save info page, and the save/load
    // menu back up). Loading with no park listed, the menu stays down and
    // the save system is told there is nothing to choose.
    int32_t showParkFileMenu(uint32_t up)
    {
        ee::Local<char[0x400]> script;
        ee::Local<int32_t> visibleLines;
        *visibleLines = kParkMenuLines;
        if (up == 0)
        {
            runMenuScript(kParkMenuOnShutdown);
            runMenuScript(kSaveInfoOnShutdown);
            return runMenuScriptResult(kMenuOnInit);
        }
        if (*g_parkSaving != 0)
        {
            if (*g_parkSavesListed < kMostParkSaves)
            {
                if (*g_parkSaveFileCount != 0)
                {
                    runMenuScript(kBreakFileList);
                    runMenuScript(kBreakAuthorList);
                    runMenuScript(kBreakThemeList);
                }
                zge::formatString(script, kAddFileListItem, kNewFile);
                runMenuScript(script);
            }
        }
        else if (*g_parkSaveFileCount == 0)
            return answerSaveListEmpty();
        zge::zgui::setNodeValueByPath(kFileListVisibleCount, visibleLines);
        zge::zgui::setNodeValueByPath(kAuthorListVisibleCount, visibleLines);
        zge::zgui::setNodeValueByPath(kThemeListVisibleCount, visibleLines);
        return runMenuScriptResult(kParkMenuOnInit);
    }
    READABLE_FUNCTION_WAITS(showParkFileMenu, Verified);
}
