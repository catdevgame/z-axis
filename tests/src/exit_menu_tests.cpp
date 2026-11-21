// Exit to Collection in Dave Mirra BMX 2's and BMX XXX's own menus
// (game_overrides/dave_mirra_exit.cpp, dave_mirra_restart.cpp,
// bmx_xxx_exit.cpp, zge_exit.cpp): the script edits on the menus' own lines
// (copied from the games' UILIB.ZAL), the pause lines, and when the items
// are made at all.
#include "MiniTest.h"

#include "bmx/overrides/bmx_xxx.h"
#include "dm2/overrides/dave_mirra.h"
#include "host_backend.h"
#include "overrides/zge_host.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
    // UIBuilder's header comment and CRLF line ends (findObject takes an
    // object at the text's very start for the top level at every step).
    std::string crlf(const char *text)
    {
        std::string out = "///////////////////////////\r\n// UIBuilder Generated File\r\n\r\n";
        for (const char *p = text; *p; ++p)
        {
            if (*p == '\n')
                out += '\r';
            out += *p;
        }
        return out;
    }

    // DM2's MainMenu: the frame, its two Items actions, the menu (its
    // lines as on the disc, the actions cut to two).
    const char *const kDm2MainMenu = R"ZSC(OBJECT "guiScreenSpace" "MainMenu"
{
	Visible	1
	OBJECT "guiFrame" "MainMenuFrame"
	{
		Position	0.000000 250.000000 0.000000
		Width	340
		Height	270
		ACTION "Script" "LockMultiPlayer"
		{
			StartDelay	0
			Script	"SET MainMenu.Items {\"Proquest\nSession\nFreeride\nPark Editor\nOptions\nCool Stuff\n!Multiplayer\"}\nSET Root.MainMenu.OptionsFrame.Menu.Items {\"Controller 1\n!Controller 2\nAudio\nLoad Game\nSave Game\"}"
		}
		ACTION "Script" "UnLockMultiPlayer"
		{
			StartDelay	0
			Script	"SET MainMenu.Items {\"Proquest\nSession\nFreeride\nPark Editor\nOptions\nCool Stuff\nMultiplayer\"}\nSET Root.MainMenu.OptionsFrame.Menu.Items {\"Controller 1\nController 2\nAudio\nLoad Game\nSave Game\"}"
		}
		OBJECT "guiMenu" "MainMenu"
		{
			Visible	1
			Position	175.000000 40.000000 0.000000
			VisibleCount	9
			Items	"Proquest\nSession\nFreeride\nPark Editor\nOptions\nCool Stuff\nMultiplayer"
			OnItemSelect	"PLAY *SelectedIndex"
			ACTION "Script" "5"
			{
				StartDelay	0
				Script	"PLAY .DeInit"
			}
			ACTION "Script" "6"
			{
				StartDelay	0
				Script	"PLAY .DeInit"
			}
		}
	}
}
)ZSC";

    // BMX XXX's UI_menu: the main menu's scripts as on the disc.
    const char *const kBmxMenu = R"ZSC(OBJECT "guiScreenSpace" "UI_menu"
{
	OBJECT "guiGroup" "CommonBackground"
	{
		OBJECT "guiGroup" "FrameMainMenu"
		{
			OBJECT "guiMenu" "Menu"
			{
				VisibleCount	6
				OnCancel	{
					SET Root.MsgType {-1}
					CALL bmxuiHandler
					}
				OnItemSelect	{
					SET Root.MsgType {0}
					CALL bmxuiHandler
					}
				OnChange	{
					SET Root.MsgType {2}
					CALL bmxuiHandler
					}
			}
		}
	}
}
)ZSC";

    bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
}

void register_exit_menu_tests()
{
    MiniTest::Case("ExitToCollection", [](TestCase &tc)
    {
        tc.Run("only in the collection", [](TestCase &t)
        {
            const bool before = ps2_host::menuAvailable();
            ps2_host::setMenuAvailable(false);
            t.IsTrue(!zge_host::inCollection(), "a single-game app or the runner: no item");
            ps2_host::setMenuAvailable(true);
            t.IsTrue(zge_host::inCollection(), "the collection: the items");
            ps2_host::setMenuAvailable(before);
        });

        tc.Run("DM2's main menu", [](TestCase &t)
        {
            std::string text = crlf(kDm2MainMenu);
            t.IsTrue(dave_mirra::exit_menu::patchMainMenu(text), "patched");
            t.IsTrue(has(text, "Items\t\"Proquest\\nSession\\nFreeride\\nPark Editor\\nOptions\\nCool Stuff\\nMultiplayer"
                               "\\nExit to Collection\"\r\n"),
                     "the menu's own lines end with the item");
            t.IsTrue(has(text, "Cool Stuff\\n!Multiplayer\\nExit to Collection\\\"}"), "LockMultiPlayer's lines too");
            t.IsTrue(has(text, "Cool Stuff\\nMultiplayer\\nExit to Collection\\\"}"), "UnLockMultiPlayer's lines too");
            t.IsTrue(has(text, "\t\t\tACTION \"Script\" \"7\"\r\n") && has(text, "Script\t\"CALL pcExitToCollection\""),
                     "action 7 (PLAY *SelectedIndex) calls the function");
            const size_t seven = text.find("ACTION \"Script\" \"7\"");
            t.IsTrue(seven > text.find("ACTION \"Script\" \"6\"") && seven < text.rfind("\t\t}\r\n\t}\r\n}"),
                     "inside the menu, after its last action");
            t.IsTrue(has(text, "\t\tWidth\t430\r\n\t\tHeight\t300\r\n"), "the frame a line taller and wider");
            t.IsTrue(has(text, "\t\t\tPosition\t215.000000 40.000000 0.000000\r\n"), "the menu in its middle");
            std::string other = crlf("OBJECT \"guiScreenSpace\" \"MainMenu\"\n{\n}\n");
            const std::string copy = other;
            t.IsTrue(!dave_mirra::exit_menu::patchMainMenu(other) && other == copy, "another script: left alone");
        });

        tc.Run("DM2's pause lines", [](TestCase &t)
        {
            // Continue, Options, Trick List, View Records, End Run (action 5).
            const std::string lines =
                "$KSTR_PAUSEMENU_CONTINUE_TEXT\\n$KSTR_PAUSEMENU_OPTIONS_TEXT\\n$KSTR_PAUSEMENU_TRICKLIST_TEXT\\n"
                "$KSTR_PAUSEMENU_RECORDS_TEXT\\n$KSTR_PAUSEMENU_EXITGAME_TEXT";
            uint8_t ids[16] = {0, 2, 3, 4, 5};
            std::string items = lines;
            dave_mirra::restart::PauseLines added = dave_mirra::restart::addPauseLines(items, ids, true, true);
            t.Equals(added.added, 2, "Restart and Exit to Collection");
            t.Equals(added.exitLine, 6, "the last of seven lines");
            t.IsTrue(items == "$KSTR_PAUSEMENU_CONTINUE_TEXT\\n$KSTR_PAUSEMENU_OPTIONS_TEXT\\n$KSTR_PAUSEMENU_TRICKLIST_TEXT\\n"
                              "$KSTR_PAUSEMENU_RECORDS_TEXT\\n$KSTR_PAUSEMENU_EXITGAME_TEXT\\nRESTART\\nEXIT TO COLLECTION",
                     "RESTART after End Run, EXIT TO COLLECTION last");
            t.IsTrue(ids[4] == 5 && ids[5] == 6 && ids[6] == 0, "End Run's id as it was, Restart's after it; none for the new line");

            uint8_t single[16] = {0, 2, 3, 4, 5};
            items = lines;
            added = dave_mirra::restart::addPauseLines(items, single, true, false);
            t.IsTrue(added.added == 1 && added.exitLine == -1, "a single-game app: Restart only, as before");

            uint8_t untouched[16] = {0, 2, 3, 4, 5};
            items = lines;
            added = dave_mirra::restart::addPauseLines(items, untouched, false, true);
            t.IsTrue(added.added == 1 && added.exitLine == 5 && untouched[4] == 5, "ZX_RESTART=0: the new line alone");

            items = std::string(470, 'x');
            added = dave_mirra::restart::addPauseLines(items, untouched, false, true);
            t.IsTrue(added.added == 0 && items.size() == 470u, "no room in the builder's text: no line");

            uint8_t photo[16] = {0, 2, 3, 4, 5};
            items = lines;
            added = dave_mirra::restart::addPauseLines(items, photo, true, true, true);
            t.IsTrue(added.added == 3 && added.photoLine == 6 && added.exitLine == 7, "Restart, Photo Mode and Exit");
            t.IsTrue(items == "$KSTR_PAUSEMENU_CONTINUE_TEXT\\n$KSTR_PAUSEMENU_OPTIONS_TEXT\\n$KSTR_PAUSEMENU_TRICKLIST_TEXT\\n"
                              "$KSTR_PAUSEMENU_RECORDS_TEXT\\n$KSTR_PAUSEMENU_EXITGAME_TEXT\\nRESTART\\nPHOTO MODE\\n"
                              "EXIT TO COLLECTION",
                     "one block after End Run: RESTART, PHOTO MODE, EXIT TO COLLECTION");
            t.IsTrue(photo[4] == 5 && photo[5] == 6, "the game's ids as with Restart alone");
        });

        tc.Run("BMX XXX's main menu scripts", [](TestCase &t)
        {
            std::string text = crlf(kBmxMenu);
            t.IsTrue(bmx_xxx::exit_menu::patchMainMenu(text), "patched");
            const std::string tabs = "\t\t\t\t\t";
            t.IsTrue(has(text, "\t\t\t\tOnItemSelect\t{\r\n" + tabs + "IF SelectedText == {\"EXIT TO COLLECTION\"} GOTO PcExit\r\n" +
                                   tabs + "SET Root.MsgType {0}\r\n" + tabs + "CALL bmxuiHandler\r\n" + tabs +
                                   "GOTO PcEnd\r\n" + tabs + ":PcExit\r\n" + tabs + "CALL pcExitToCollection\r\n" +
                                   tabs + ":PcEnd\r\n" + tabs + "}\r\n"),
                     "Select: the item calls the function, the others the game's handler");
            t.IsTrue(has(text, "\t\t\t\tOnChange\t{\r\n" + tabs + "IF SelectedText == {\"EXIT TO COLLECTION\"} GOTO PcEnd\r\n" +
                                   tabs + "SET Root.MsgType {2}\r\n" + tabs + "CALL bmxuiHandler\r\n" + tabs +
                                   ":PcEnd\r\n" + tabs + "}\r\n"),
                     "Change: the game's handler for the game's lines only");
            t.IsTrue(has(text, "\t\t\t\tOnCancel\t{\r\n" + tabs + "SET Root.MsgType {-1}\r\n"), "Cancel as it was");
            std::string other = crlf("OBJECT \"guiScreenSpace\" \"UI_menu\"\n{\n}\n");
            const std::string copy = other;
            t.IsTrue(!bmx_xxx::exit_menu::patchMainMenu(other) && other == copy, "another script: left alone");
        });

        tc.Run("BMX XXX's built lines", [](TestCase &t)
        {
            char main[0x100] = "$KSTR_COM_MENU_MAIN_OPT00\\n$KSTR_COM_MENU_MAIN_OPT01\\n$KSTR_COM_MENU_MAIN_OPT03\\n"
                               "$KSTR_COM_MENU_MAIN_OPT04\\n$KSTR_COM_MENU_MAIN_OPT05";
            t.Equals(bmx_xxx::exit_menu::addExitLine(main, sizeof(main)), 5, "the sixth line");
            t.IsTrue(std::strcmp(main + std::strlen(main) - std::strlen("\\nEXIT TO COLLECTION"), "\\nEXIT TO COLLECTION") == 0,
                     "after the game's lines");
            // The builder's lines and mainChoices: HARDCORE TOUR, MULTIPLAYER (marked off with one pad),
            // RIDER EDITOR, ACCLAIM EXTRAS, OPTIONS.
            const char *built = "$KSTR_COM_MENU_MAIN_OPT00\\n!$KSTR_COM_MENU_MAIN_OPT01\\n$KSTR_COM_MENU_MAIN_OPT03\\n"
                                "$KSTR_COM_MENU_MAIN_OPT04\\n$KSTR_COM_MENU_MAIN_OPT05";
            char park[0x100] = {};
            std::strcpy(park, built);
            uint8_t mainChoices[16] = {0, 1, 3, 4, 5};
            bmx_xxx::exit_menu::MainMenuLines menuLines =
                bmx_xxx::exit_menu::addMainMenuLines(park, sizeof(park), mainChoices, "$KSTR_COM_MENU_MAIN_OPT02", true);
            t.IsTrue(menuLines.added == 2 && menuLines.parkEditorLine == 2 && menuLines.exitLine == 6, "PARK EDITOR and Exit to Collection");
            t.IsTrue(std::strcmp(park, "$KSTR_COM_MENU_MAIN_OPT00\\n!$KSTR_COM_MENU_MAIN_OPT01\\n$KSTR_COM_MENU_MAIN_OPT02\\n"
                                       "$KSTR_COM_MENU_MAIN_OPT03\\n$KSTR_COM_MENU_MAIN_OPT04\\n$KSTR_COM_MENU_MAIN_OPT05\\n"
                                       "EXIT TO COLLECTION") == 0,
                     "PARK EDITOR third, as the script's own list has it; EXIT TO COLLECTION last");
            t.IsTrue(mainChoices[0] == 0 && mainChoices[1] == 1 && mainChoices[2] == 2 && mainChoices[3] == 3 && mainChoices[4] == 4 &&
                         mainChoices[5] == 5 && mainChoices[6] == 0,
                     "its choice in, the later ones moved down; none for the exit line");
            std::strcpy(park, built);
            uint8_t alone[16] = {0, 1, 3, 4, 5};
            menuLines = bmx_xxx::exit_menu::addMainMenuLines(park, sizeof(park), alone, "$KSTR_COM_MENU_MAIN_OPT02", false);
            t.IsTrue(menuLines.added == 1 && menuLines.parkEditorLine == 2 && menuLines.exitLine == -1, "a single-game app: PARK EDITOR alone");
            std::strcpy(park, built);
            uint8_t untouched[16] = {0, 1, 3, 4, 5};
            menuLines = bmx_xxx::exit_menu::addMainMenuLines(park, sizeof(park), untouched, nullptr, true);
            t.IsTrue(menuLines.added == 1 && menuLines.exitLine == 5 && untouched[2] == 3 &&
                         std::strstr(park, "OPT02") == nullptr,
                     "without the setting: Exit to Collection alone, as before");
            char noMultiplayer[0x100] = "$KSTR_COM_MENU_MAIN_OPT00";
            uint8_t first[16] = {0};
            menuLines = bmx_xxx::exit_menu::addMainMenuLines(noMultiplayer, sizeof(noMultiplayer), first, "$KSTR_COM_MENU_MAIN_OPT02", false);
            t.IsTrue(menuLines.added == 0 && std::strcmp(noMultiplayer, "$KSTR_COM_MENU_MAIN_OPT00") == 0,
                     "no MULTIPLAYER line to follow: no line");

            char empty[32] = "";
            t.Equals(bmx_xxx::exit_menu::addExitLine(empty, sizeof(empty)), 0, "an empty list: the first line");
            t.IsTrue(std::strcmp(empty, "EXIT TO COLLECTION") == 0, "no separator before it");
            char full[24] = "$KSTR_PAUSE_CONTINUE";
            t.Equals(bmx_xxx::exit_menu::addExitLine(full, sizeof(full)), -1, "no room: no line");
            t.IsTrue(std::strcmp(full, "$KSTR_PAUSE_CONTINUE") == 0, "and the text as it was");

            char pause[0x200] = "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_GOALS\\n$KSTR_PAUSE_BIKEPARTS\\n$KSTR_PAUSE_RECORDS\\n"
                                "$KSTR_PAUSE_OPTIONS\\n$KSTR_PAUSE_EXITGAME";
            const bmx_xxx::exit_menu::PauseLines lines =
                bmx_xxx::exit_menu::addPauseLines(pause, sizeof(pause), true, true);
            t.IsTrue(lines.added == 2 && lines.restartLine == 6 && lines.exitLine == 7,
                     "RESTART and Exit to Collection (the game's choices are not touched: none is handed in)");
            t.IsTrue(std::strcmp(pause, "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_GOALS\\n$KSTR_PAUSE_BIKEPARTS\\n$KSTR_PAUSE_RECORDS\\n"
                                        "$KSTR_PAUSE_OPTIONS\\n$KSTR_PAUSE_EXITGAME\\nRESTART\\nEXIT TO COLLECTION") == 0,
                     "RESTART after Exit Game, EXIT TO COLLECTION last");
            char single[0x200] = "$KSTR_PAUSE_CONTINUE";
            t.IsTrue(bmx_xxx::exit_menu::addPauseLines(single, sizeof(single), true, false).restartLine == 1 &&
                         std::strcmp(single, "$KSTR_PAUSE_CONTINUE\\nRESTART") == 0,
                     "a menu of one line: RESTART after it, no choice written");

            char photo[0x200] = "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_GOALS\\n$KSTR_PAUSE_BIKEPARTS\\n$KSTR_PAUSE_RECORDS\\n"
                                "$KSTR_PAUSE_OPTIONS\\n$KSTR_PAUSE_EXITGAME";
            const bmx_xxx::exit_menu::PauseLines withPhoto =
                bmx_xxx::exit_menu::addPauseLines(photo, sizeof(photo), true, true, true);
            t.IsTrue(withPhoto.added == 3 && withPhoto.restartLine == 6 && withPhoto.photoLine == 7 &&
                         withPhoto.exitLine == 8,
                     "RESTART, Photo Mode and Exit to Collection");
            t.IsTrue(std::strcmp(photo, "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_GOALS\\n$KSTR_PAUSE_BIKEPARTS\\n$KSTR_PAUSE_RECORDS\\n"
                                        "$KSTR_PAUSE_OPTIONS\\n$KSTR_PAUSE_EXITGAME\\nRESTART\\nPHOTO MODE\\n"
                                        "EXIT TO COLLECTION") == 0,
                     "one block after Exit Game: RESTART, PHOTO MODE, EXIT TO COLLECTION");
            char over[0x200] = "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_PLAYAGAIN\\n$KSTR_PAUSE_EXITGAME";
            const bmx_xxx::exit_menu::PauseLines photoOnly =
                bmx_xxx::exit_menu::addPauseLines(over, sizeof(over), false, false, true);
            t.IsTrue(photoOnly.added == 1 && photoOnly.photoLine == 3 && photoOnly.restartLine == -1 &&
                         photoOnly.exitLine == -1,
                     "a turn over in a single-game app: Photo Mode alone");

            // The lines the builders leave out, by the menu's order of choices.
            const uint8_t pauseOrder[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
            std::string tour = "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_GOALS\\n$KSTR_PAUSE_BIKEPARTS\\n$KSTR_PAUSE_RECORDS\\n"
                               "$KSTR_PAUSE_OPTIONS\\n$KSTR_PAUSE_EXITGAME";
            uint8_t tourChoices[12] = {0, 2, 3, 4, 6, 10};
            const std::vector<bmx_xxx::exit_menu::MenuLine> pauseLines = {
                {5, "$KSTR_PAUSE_CHANGERIDER"}, {7, "$KSTR_PAUSE_TRICKLIST"}, {9, "$KSTR_PAUSE_LEVELSELECT"}};
            t.Equals(bmx_xxx::exit_menu::insertMenuLines(tour, tourChoices, 6, 12, pauseLines, pauseOrder, sizeof(pauseOrder)), 3,
                     "the pause menu: three lines put in");
            t.IsTrue(tour == "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_GOALS\\n$KSTR_PAUSE_BIKEPARTS\\n$KSTR_PAUSE_RECORDS\\n"
                             "$KSTR_PAUSE_CHANGERIDER\\n$KSTR_PAUSE_OPTIONS\\n$KSTR_PAUSE_TRICKLIST\\n$KSTR_PAUSE_LEVELSELECT\\n"
                             "$KSTR_PAUSE_EXITGAME",
                     "each where its choice comes: CHANGE RIDER after RECORDS, TRICK LIST after OPTIONS, LEVEL SELECT before "
                     "EXIT GAME");
            const uint8_t tourAfter[] = {0, 2, 3, 4, 5, 6, 7, 9, 10};
            t.IsTrue(std::memcmp(tourChoices, tourAfter, sizeof(tourAfter)) == 0, "the choices moved to match");
            t.Equals(bmx_xxx::exit_menu::insertMenuLines(tour, tourChoices, 9, 12, pauseLines, pauseOrder, sizeof(pauseOrder)), 0,
                     "a line already there is not put in twice");

            const uint8_t gameOverOrder[] = {0, 1, 2, 3, 4, 5, 10, 6, 7, 8};
            std::string over2 = "$KSTR_GAMEOVER_PLAYAGAIN0\\n$KSTR_GAMEOVER_SAVE\\n$KSTR_GAMEOVER_VIEWRECORDS\\n$KSTR_GAMEOVER_EXIT";
            uint8_t overChoices[12] = {0, 4, 7, 8};
            t.Equals(bmx_xxx::exit_menu::insertMenuLines(over2, overChoices, 4, 12,
                                                         {{2, "$KSTR_PAUSE_CHANGERIDER"}, {5, "$KSTR_GAMEOVER_VIEWREPLAY"},
                                                          {6, "$KSTR_GAMEOVER_TRICKLIST"}},
                                                         gameOverOrder, sizeof(gameOverOrder)),
                     3, "the game over: three lines put in");
            const uint8_t overAfter[] = {0, 2, 4, 5, 6, 7, 8};
            t.IsTrue(over2 == "$KSTR_GAMEOVER_PLAYAGAIN0\\n$KSTR_PAUSE_CHANGERIDER\\n$KSTR_GAMEOVER_SAVE\\n"
                              "$KSTR_GAMEOVER_VIEWREPLAY\\n$KSTR_GAMEOVER_TRICKLIST\\n$KSTR_GAMEOVER_VIEWRECORDS\\n$KSTR_GAMEOVER_EXIT" &&
                         std::memcmp(overChoices, overAfter, sizeof(overAfter)) == 0,
                     "in the handler's order, EXIT last");
            std::string odd = "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_EXITGAME";
            uint8_t oddChoices[12] = {0, 10, 0};
            t.Equals(bmx_xxx::exit_menu::insertMenuLines(odd, oddChoices, 3, 12, pauseLines, pauseOrder, sizeof(pauseOrder)), 0,
                     "a text without one line a choice is left as it is");
            uint8_t fullChoices[2] = {0, 10};
            std::string full2 = "$KSTR_PAUSE_CONTINUE\\n$KSTR_PAUSE_EXITGAME";
            t.Equals(bmx_xxx::exit_menu::insertMenuLines(full2, fullChoices, 2, 2, pauseLines, pauseOrder, sizeof(pauseOrder)), 0,
                     "no room for another choice: none put in");
        });
    });
}
