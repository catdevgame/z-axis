// The save system's screens (zge/platform/save_system.h SaveClient: the
// game's and the park's save clients hand these over): the message line,
// a choice of lines, the slot menu and the park file menu, all
// through the front end's SaveLoad pages (zgui paths and scripts).

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/aggressive/save_menus.cpp.h"

namespace aggressive
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
        constexpr uint32_t kMenuItems = sym::g_textSaveLoadSaveLoadMenuMemCardMenu2;      // "...MemCardMenu.Items"
        constexpr uint32_t kMenuVisibleCount = sym::g_textSaveLoadSaveLoadMenuMemCardMenu; // "...MemCardMenu.VisibleCount"
        constexpr uint32_t kMenuSelectedIndex = sym::g_textSaveLoadMemCardSelectedIndex;
        constexpr uint32_t kClearMenu = sym::g_textSetSaveLoadSaveLoadMenuMemCardMenu;   // SET ...Items {""}
        constexpr uint32_t kAddMenuItem = sym::g_textStrcatSaveLoadSaveLoadMenu;        // STRCAT ...Items {"%s%s"}
        constexpr uint32_t kItemBreak = sym::g_textMenuItemBreak;                           // "\n" in the script's text
        constexpr uint32_t kHideMenu = sym::g_textKillfocusSetSaveLoadSaveLoadMenu;     // KILLFOCUS, ...Visible {0}
        constexpr uint32_t kResetMenu = sym::g_textPlaySaveLoadMenuReset;
        constexpr uint32_t kMenuOnInit = sym::g_textPlaySaveLoadSaveLoadMenuOnInit;
        constexpr uint32_t kSaveInfoOnShutdown = sym::g_textPlaySaveLoadSaveInfoOnShutdown;
        // SaveLoad.ChooseParkMenu: three lists side by side.
        constexpr uint32_t kBreakFileList = sym::g_textStrcatSaveLoadChooseParkMenu;    // STRCAT ...FileListMenu.Items {"\n"}
        constexpr uint32_t kBreakAuthorList = sym::g_textStrcatSaveLoadChooseParkMenu2;
        constexpr uint32_t kBreakThemeList = sym::g_textStrcatSaveLoadChooseParkMenu3;
        constexpr uint32_t kAddFileListItem = sym::g_textStrcatSaveLoadChooseParkMenu4; // STRCAT ...FileListMenu.Items {"%s"}
        constexpr uint32_t kNewFile = sym::g_textNew;                                   // "New"
        constexpr uint32_t kFileListVisibleCount = sym::g_textSaveLoadChooseParkMenuFileListMenu;
        constexpr uint32_t kAuthorListVisibleCount = sym::g_textSaveLoadChooseParkMenuAuthorMenu;
        constexpr uint32_t kThemeListVisibleCount = sym::g_textSaveLoadChooseParkMenuThemeMenu;
        constexpr uint32_t kParkMenuOnInit = sym::g_textPlaySaveLoadChooseParkMenu2;
        constexpr uint32_t kParkMenuOnShutdown = sym::g_textPlaySaveLoadChooseParkMenu;
        constexpr int32_t kParkMenuLines = 10;

        // level/park_save.cpp's: 1 while the park is being saved (0 loading),
        // and the park saves listed.
        inline ee::Global<int32_t, sym::g_parkSaving> g_parkSaving;
        inline ee::Global<int32_t, sym::g_parkSaveFileCount> g_parkSaveFileCount;

        // A script run (by address: a game override hooks it); its result is what runScript leaves in v0.
        int32_t runScript(ee::Address text) { return ee::game<int32_t>(sym::runScript, 0, text, 0); }
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
        runScript(kClearMenu);
        while (labels[*lines] != 0)
        {
            zge::formatString(script, kAddMenuItem, *lines != 0 ? kItemBreak : kNoText, labels[*lines]);
            runScript(script);
            *lines = *lines + 1;
        }
        zge::zgui::setNodeValueByPath(kMenuVisibleCount, lines);
        runScript(kResetMenu);
        zge::zgui::setNodeValueByPath(kFooterVisible, shown);
        zge::zgui::setNodeValueByPath(kFooterTriangleVisible, shown);
        return zge::zgui::setNodeValueByPath(kFooterXVisible, shown);
    }
    READABLE_FUNCTION_WAITS(showSaveSlotMenu, Verified);

    // showSaveChoice: the screen's menu shows `text`'s lines (a line break or
    // the two characters \n start another) with `selected` picked, its
    // footer and hints shown, and the menu reset; no text hides the menu.
    int32_t showSaveChoice(ee::Ptr<const char> text, int32_t selected)
    {
        ee::Local<int32_t> lines;
        ee::Local<int32_t> selectedIndex;
        ee::Local<int32_t> shown;
        *selectedIndex = selected;
        *lines = 1;
        *shown = 1;
        if (!text)
            return runScript(kHideMenu);
        for (int32_t i = 0; text[i] != 0; ++i)
            if (text[i] == '\n' || (text[i] == '\\' && text[i + 1] == 'n'))
                *lines = *lines + 1;
        zge::zgui::setNodeValueByPath(kMenuItems, text);
        zge::zgui::setNodeValueByPath(kMenuVisibleCount, lines);
        zge::zgui::setNodeValueByPath(kMenuSelectedIndex, selectedIndex);
        zge::zgui::setNodeValueByPath(kFooterVisible, shown);
        zge::zgui::setNodeValueByPath(kFooterTriangleVisible, shown);
        zge::zgui::setNodeValueByPath(kFooterXVisible, shown);
        return runScript(kResetMenu);
    }
    READABLE_FUNCTION_WAITS(showSaveChoice, Verified);

    // showParkFileMenu: the park file menu up (its lists ten lines; when
    // saving, "New" after the saves already listed) or down (with the save
    // info page, and the save/load menu back up).
    int32_t showParkFileMenu(uint32_t up)
    {
        ee::Local<char[0x400]> script;
        ee::Local<int32_t> visibleLines;
        *visibleLines = kParkMenuLines;
        if (up == 0)
        {
            runScript(kParkMenuOnShutdown);
            runScript(kSaveInfoOnShutdown);
            return runScript(kMenuOnInit);
        }
        if (*g_parkSaving != 0)
        {
            if (*g_parkSaveFileCount != 0)
            {
                runScript(kBreakFileList);
                runScript(kBreakAuthorList);
                runScript(kBreakThemeList);
            }
            zge::formatString(script, kAddFileListItem, kNewFile);
            runScript(script);
        }
        zge::zgui::setNodeValueByPath(kFileListVisibleCount, visibleLines);
        zge::zgui::setNodeValueByPath(kAuthorListVisibleCount, visibleLines);
        zge::zgui::setNodeValueByPath(kThemeListVisibleCount, visibleLines);
        return runScript(kParkMenuOnInit);
    }
    READABLE_FUNCTION_WAITS(showParkFileMenu, Verified);
}
