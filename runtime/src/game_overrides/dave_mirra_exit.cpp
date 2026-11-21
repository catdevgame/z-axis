// Dave Mirra Freestyle BMX 2 (SLUS-20159): Exit to Collection in the front
// end's main menu, in the collection only (zge_host.h; the pause menu's
// line is dave_mirra_restart.cpp's).
//
// The main menu is UILIB.ZAL's MainMenu.MainMenuFrame.MainMenu, a guiMenu
// whose OnItemSelect plays the action named by the selected index
// ("PLAY *SelectedIndex": actions "0" to "6", Proquest to Multiplayer).
// Its Items come from the script (the menu's own line, and the frame's
// LockMultiPlayer and UnLockMultiPlayer actions, which the front end plays
// to grey Multiplayer out with one pad): each gets an "Exit to Collection"
// line after Multiplayer, and the menu an action "7" calling
// pcExitToCollection (zge_exit.cpp: registered with the front end's script
// functions, after bmxCheckCheat). The frame that holds the menu
// (Width 340, Height 270 for seven lines) grows by a line and is made wide
// enough for the longer text at the size the menu pulses the selected line
// to (1.2): Width 430, the menu's centre moved to its middle (x 175 -> 215).
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_exit.cpp.h"
#include "zge_host.h"

#include "ps2_runtime.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_zaxis_ui.h"

#include <iostream>
#include <string>
#include <vector>

namespace dave_mirra::exit_menu
{

    constexpr zge_host::ScriptFunctions kScriptFunctions{
        "dave-mirra",
        sym::dm2::registerScriptFunction,
        "bmxCheckCheat", // the main menu's cheat sniffer's; registered by the front end's setup (introStateEnter)
        points::dm2::introStateEnter::scriptFunctionSlots, // inside that setup, its registration calls: no entry points
        points::dm2::introStateEnter::scriptFunctionSlotsEnd,
    };

    constexpr const char *kItem = "Exit to Collection";

    const char *const kAction = R"ZSC(ACTION "Script" "7"
{
	//////////////////////////////
	// Property
	StartDelay	0
	Script	"CALL pcExitToCollection"
}
)ZSC";

    bool patchMainMenu(std::string &text)
    {
        std::string edited = text;
        zx::zaxis::Span frame = zx::zaxis::findObject(edited, "MainMenu/MainMenuFrame");
        zx::zaxis::Span menu = zx::zaxis::findObject(edited, "MainMenu/MainMenuFrame/MainMenu");
        if (!frame.found() || !menu.found())
            return false;
        // Later positions first: the menu's end (its new action), its
        // Items, then the frame's actions and size, which come before it.
        edited.insert(zx::zaxis::objectEnd(edited, menu), zx::zaxis::indentBlock(kAction, zx::zaxis::indentAt(edited, menu.begin) + 1));
        menu = zx::zaxis::findObject(edited, "MainMenu/MainMenuFrame/MainMenu");
        const std::string line = std::string("\\n") + kItem;
        bool ok = zx::zaxis::replaceIn(edited, menu, "\\nMultiplayer\"\r\n", "\\nMultiplayer" + line + "\"\r\n") &&
                  zx::zaxis::replaceIn(edited, menu, "\tPosition\t175.000000 40.000000 0.000000\r\n",
                                "\tPosition\t215.000000 40.000000 0.000000\r\n");
        frame = zx::zaxis::findObject(edited, "MainMenu/MainMenuFrame");
        ok = ok && zx::zaxis::replaceIn(edited, frame, "\\n!Multiplayer\\\"}", "\\n!Multiplayer" + line + "\\\"}") &&
             zx::zaxis::replaceIn(edited, frame, "\\nCool Stuff\\nMultiplayer\\\"}", "\\nCool Stuff\\nMultiplayer" + line + "\\\"}") &&
             zx::zaxis::replaceIn(edited, frame, "\tWidth\t340\r\n", "\tWidth\t430\r\n") &&
             zx::zaxis::replaceIn(edited, frame, "\tHeight\t270\r\n", "\tHeight\t300\r\n");
        if (!ok)
            return false;
        text = std::move(edited);
        return true;
    }

    bool patchUiLibrary(std::vector<uint8_t> &bytes)
    {
        std::vector<zx::zaxis::ArchiveEntry> entries;
        if (!zx::zaxis::readArchive(bytes, entries))
            return false;
        bool changed = false;
        for (zx::zaxis::ArchiveEntry &entry : entries)
        {
            if (zx::zaxis::scriptName(entry) != "MainMenu")
                continue;
            std::string text(entry.data.begin(), entry.data.end());
            if (patchMainMenu(text))
            {
                entry.data.assign(text.begin(), text.end());
                changed = true;
                std::cout << "[dave-mirra] main menu: " << kItem << std::endl;
            }
            else
            {
                std::cerr << "[dave-mirra] MainMenu is not the expected script: no " << kItem << " in the main menu"
                          << std::endl;
            }
        }
        if (changed)
            bytes = zx::zaxis::writeArchive(entries);
        return changed;
    }
}

namespace dave_mirra
{
    const zge_host::ScriptFunctions &scriptFunctions() { return exit_menu::kScriptFunctions; }

    bool installExitToCollection(PS2Runtime &runtime)
    {
        if (!zge_host::inCollection())
            return false;
        if (!zge_host::installExitFunction(runtime, exit_menu::kScriptFunctions))
            return false;
        zx::disc_patch::registerPatch("UILIB.ZAL", &exit_menu::patchUiLibrary);
        return true;
    }
}
