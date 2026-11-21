// BMX XXX's game code (bmx.c's park and game saves): Aggressive Inline's games/aggressive/level/park_save.cpp,
// copied for BMX XXX: the functions the two games have word for word. Saving a park and
// loading one from it (the park's save client of the engine's save system, zge/platform/save_system.cpp): the
// park's bytes go in a ParkBuffer (park_buffer.h). The script functions here are the save menus'
// (registered as "parkEditorMemCardOnUser..."). The rest of the client (its title, the file list, the
// start of a save or load) is BMX XXX's own.

#include "bmx/save/park_save.h"
#include "bmx/save/park_buffer.h"
#include "bmx/save/game_save.h"
#include "bmx/save/save_menus.h"
#include "bmx/editor/park_editor.h"

#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/platform/save_system.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/save/park_save.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The first 72 bytes of a saved park, what the save list reads of
        // each save (the client's listBufferBytes): the park stream's version
        // word (openParkStream), then saveParkData's theme, author and name.
        struct ParkSaveHeader
        {
            uint32_t version; // +0x00
            int32_t theme;    // +0x04
            char author[0x20]; // +0x08
            char name[0x20];  // +0x28
        };
        static_assert(offsetof(ParkSaveHeader, author) == 0x08 && offsetof(ParkSaveHeader, name) == 0x28 && sizeof(ParkSaveHeader) == 0x48);

        // A park save, as the save list finds it.
        struct ParkSaveFile
        {
            char fileName[0x100];  // +0x000
            ParkSaveHeader header; // +0x100
        };
        static_assert(offsetof(ParkSaveFile, header) == 0x100 && sizeof(ParkSaveFile) == 0x148);

        // A park save's version word (the park stream's); a save with another reads as damaged.
        constexpr uint32_t kParkSaveVersion = 0xE22;

        // A park save or load under way (1), and which.
        inline ee::Global<uint32_t, sym::g_parkSaveUnderway> g_parkSaveUnderway;
        inline ee::Global<uint32_t, sym::g_parkSaving> g_parkSaving; // 1 saving (the list offers a new save), 0 loading
        inline ee::Global<ee::Ptr<ParkBuffer>, sym::g_parkSaveBuffer> g_parkSaveBuffer;
        // What the save job calls when it has ended, with how it ended.
        inline ee::Global<ee::FunctionPtr<void(uint32_t outcome)>, sym::g_parkSaveCallback> g_parkSaveCallback;
        // The save system's ids of the clients, by a buffer's client (one: the park's).
        constexpr uint32_t kParkSaveClients = sym::g_parkSaveClient;
        // The names of the park saves the listing found (for a new save's default name).
        inline ee::Global<char[kMostParkSaves][0x20], sym::g_parkNameList> g_parkNameList;
        inline ee::Global<uint32_t, sym::g_parkNameCount> g_parkNameCount;
        inline ee::Global<char[0x40], sym::g_parkSaveTitle> g_parkSaveTitle;
        // How long the save job waits before it starts.
        constexpr uint32_t kJobDelayMilliseconds = 750;
        // The save menu's kind (openSaveUi) for a park's save.
        constexpr uint32_t kParkSaveMenu = 3, kParkLoadMenu = 4;
        constexpr uint32_t kParkSaveBytes = 0x2000;

        // The park saves the listing found: the list, how many it holds (the menu's rows), and how many
        // park saves listed read well (of any theme: the editor lists only its own theme's).
        inline ee::Global<ee::Ptr<ParkSaveFile>, sym::g_parkFileList> g_parkFileList;
        inline ee::Global<uint32_t, sym::g_parkSaveFileCount> g_parkSaveFileCount;
        inline ee::Global<uint32_t, sym::g_parkSavesListed> g_parkSavesListed;
        // The save system's id of the park's client.
        inline ee::Global<uint32_t, sym::g_parkSaveClient> g_parkSaveClient;
        inline ee::Global<uint32_t, sym::g_parkFileEntryBytes> g_parkFileEntryBytes;
        // The park's name as the player typed it (the save's title).
        inline ee::Global<char[0x20], sym::g_parkFileName> g_parkFileName;

        // The save's icon (assets\EditIcon.icn), read for each save or load.
        inline ee::Global<uint32_t, sym::g_parkSaveIcon> g_parkSaveIcon;
        inline ee::Global<uint32_t, sym::g_parkSaveIconSize> g_parkSaveIconSize;
        constexpr uint32_t kParkSaveIconPath = sym::g_textAssetsEditIconIcn; // "assets\EditIcon.icn"

        // The script variable's value as text, and as a number.
        int32_t readVariableText(ee::Ptr<zge::zgui::Node> variable, ee::Ptr<char> out) { return zge::zgui::variableToText(variable, out); }
        int32_t readVariableNumber(ee::Ptr<zge::zgui::Node> variable, ee::Ptr<int32_t> out) { return zge::zgui::variableIntValue(variable, out); }

        // A line of the menus' script run (no widget).
        void runMenuScript(ee::Ptr<char> line)
        {
            zge::zgui::runScript(ee::Ptr<zge::zgui::Node>(), line, 0);
        }
        void runMenuScript(uint32_t line) { runMenuScript(ee::Ptr<char>(line)); }

        // What a damaged save shows for its name and author.
        constexpr uint32_t kDamagedName = sym::g_textCorruptPark; // "* Corrupt *"
        constexpr char kDamagedThemeIcon = '1';

        // Scaffolding (address-call): BMX XXX's game passes saveApiLoadChoice a third word that the engine's
        // shared declaration does not have yet.
        int32_t saveApiLoadChoiceTail(uint32_t client, uint32_t delayMilliseconds, uint32_t third) { return ee::tailCall<int32_t>(sym::saveApiLoadChoice, client, delayMilliseconds, third); }

        // The blocks of the file list are allocated on this boundary.
        constexpr uint32_t kFileListAlignment = 0x20;

        // The park's header in the buffer being saved.
        ee::Ptr<ParkSaveHeader> savedHeader()
        {
            return (*g_parkSaveBuffer)->data.cast<ParkSaveHeader>();
        }
    }

    void newParkFileList();

    // ---- the save client's methods -------------------------------------------

    // A theme's letter in the menus' icon font.
    uint32_t parkThemeIconChar(int32_t theme)
    {
        switch (theme)
        {
        case 0:
            return 'M';
        case 1:
            return 'U';
        case 2:
            return 'R';
        case 3:
            return 'Z';
        default:
            return 'E';
        }
    }
    READABLE_FUNCTION(parkThemeIconChar, Verified);

    // The save's bytes: a park buffer's size.
    uint32_t parkSaveDataSize()
    {
        return kParkSaveBytes;
    }
    READABLE_FUNCTION(parkSaveDataSize, Verified);

    // The start of a park save's file name.
    uint32_t parkSaveFileName()
    {
        return sym::g_parkSaveFileName; // "BASLUS-20415PARK"
    }
    READABLE_FUNCTION(parkSaveFileName, Verified);

    uint32_t parkSaveIconSize()
    {
        return *g_parkSaveIconSize;
    }
    READABLE_FUNCTION(parkSaveIconSize, Verified);

    uint32_t parkSaveSingleFile()
    {
        return 0;
    }
    READABLE_FUNCTION(parkSaveSingleFile, Verified);

    // The save's title: "Bmx Park", with the park's name after it when it has one.
    ee::Ptr<char> parkSaveTitle()
    {
        if (!*g_parkSaveUnderway)
            return ee::Ptr<char>(sym::g_textBmxPark);
        char *const name = *g_parkFileName;
        if (zge::stringLength(name) != 0)
            zge::formatString(*g_parkSaveTitle, sym::bmx::g_textBmxParkNamed, name); // "Bmx Park %s"
        else
            zge::formatString(*g_parkSaveTitle, sym::bmx::g_textBmxPark); // "Bmx Park"
        return ee::Ptr<char>(*g_parkSaveTitle);
    }
    READABLE_FUNCTION(parkSaveTitle, Verified);

    uint32_t parkSaveIcon()
    {
        return *g_parkSaveIcon;
    }
    READABLE_FUNCTION(parkSaveIcon, Verified);

    ee::Ptr<uint8_t> parkSaveData()
    {
        return (*g_parkSaveBuffer)->data;
    }
    READABLE_FUNCTION(parkSaveData, Verified);

    // The save job ended: the caller's callback told how, the save system and its pages
    // shut, the icon and the file list freed.
    void parkSaveFinish(uint32_t outcome)
    {
        if (!*g_parkSaveUnderway)
            return;
        (*g_parkSaveCallback)(outcome);
        zge::stopSaveSystem();
        bmx::playSaveShutdown();
        bmx::freeParkSaveIcon();
        freeParkFileList();
        (*g_parkFileName)[0] = 0;
        *g_parkSaveUnderway = 0;
    }
    READABLE_FUNCTION_WAITS(parkSaveFinish, Verified);

    // A park save the save list found (the client's saveFound): kept in the list and
    // shown in the choose-park menu's three columns (name, author, theme). A save of another
    // version is listed as damaged. With the editor open, a save of another theme is counted
    // and left out. A null file name starts the listing: the columns and the list emptied.
    // A full list takes no more.
    void addParkSaveFile(ee::Ptr<char> fileName, ee::Ptr<ParkSaveHeader> header)
    {
        ee::Local<char[0x400]> line;
        if (!fileName)
        {
            line[0] = 0;
            zge::zgui::setNodeValueByPath(sym::g_textSaveLoadChooseParkMenuFileListMenu2, line);
            zge::zgui::setNodeValueByPath(sym::g_textSaveLoadChooseParkMenuAuthorMenu2, line);
            zge::zgui::setNodeValueByPath(sym::g_textSaveLoadChooseParkMenuThemeMenu2, line);
            *g_parkSavesListed = 0;
            *g_parkSaveFileCount = 0;
            *g_parkNameCount = 0;
            return;
        }
        if (*g_parkSaveFileCount == kMostParkSaves)
            return;
        ParkSaveFile &file = (*g_parkFileList)[*g_parkSaveFileCount];
        zge::stringCopy(file.fileName, fileName);
        zge::memoryCopy(&file.header, header, sizeof(ParkSaveHeader));
        const auto newRows = [] {
            if (*g_parkSaveFileCount == 0)
                return;
            runMenuScript(sym::g_textStrcatSaveLoadChooseParkMenu);
            runMenuScript(sym::g_textStrcatSaveLoadChooseParkMenu2);
            runMenuScript(sym::g_textStrcatSaveLoadChooseParkMenu3);
        };
        if (file.header.version != kParkSaveVersion)
        {
            newRows();
            zge::stringCopy((*g_parkNameList)[*g_parkNameCount], kDamagedName);
            zge::stringCopy(file.header.name, kDamagedName);
            zge::stringCopy(file.header.author, kDamagedName);
            *g_parkNameCount = *g_parkNameCount + 1;
            zge::formatString(line, sym::bmx::g_textFileListAddName, ee::Ptr<const char>(kDamagedName)); // FileListMenu.Items {"%-16s"}
            runMenuScript(line);
            zge::formatString(line, sym::bmx::g_textAuthorListAddName, ee::Ptr<const char>(kDamagedName)); // AuthorMenu.Items {"%-16s"}
            runMenuScript(line);
            zge::formatString(line, sym::bmx::g_textThemeListAddIcon, kDamagedThemeIcon); // ThemeMenu.Items {"%c"}
            runMenuScript(line);
            *g_parkSaveFileCount = *g_parkSaveFileCount + 1;
            return;
        }
        zge::stringCopy((*g_parkNameList)[*g_parkNameCount], file.header.name);
        *g_parkNameCount = *g_parkNameCount + 1;
        if (editor::inParkEditor() && editor::parkEditorTheme() != file.header.theme)
        {
            *g_parkSavesListed = *g_parkSavesListed + 1;
            return;
        }
        newRows();
        const uint32_t themeIcon = parkThemeIconChar(file.header.theme);
        zge::formatString(line, sym::bmx::g_textFileListAddName, file.header.name);
        runMenuScript(line);
        zge::formatString(line, sym::bmx::g_textAuthorListAddName, file.header.author);
        runMenuScript(line);
        zge::formatString(line, sym::bmx::g_textThemeListAddIcon, themeIcon);
        runMenuScript(line);
        *g_parkSaveFileCount = *g_parkSaveFileCount + 1;
        *g_parkSavesListed = *g_parkSavesListed + 1;
    }
    READABLE_FUNCTION_WAITS(addParkSaveFile, Verified);

    // The park's save client registered (the game's start).
    void registerParkSaveClient()
    {
        ee::Local<zge::SaveClient> client;
        client->listBufferBytes = sizeof(ParkSaveHeader);
        client->showFileMenu = sym::showParkFileMenu;
        client->showChoice = sym::showSaveChoice;
        client->showMessage = sym::showSaveMessage;
        client->dataSize = sym::parkSaveDataSize;
        client->data = sym::parkSaveData;
        client->title = sym::parkSaveTitle;
        client->singleFile = sym::parkSaveSingleFile;
        client->iconSize = sym::parkSaveIconSize;
        client->icon = sym::parkSaveIcon;
        client->fileName = sym::parkSaveFileName;
        client->finished = sym::parkSaveFinish;
        client->saveFound = sym::addParkSaveFile;
        client->showSlotMenu = sym::showSaveSlotMenu;
        client->prepare = 0;
        client->message13 = 0;
        zge::clearSaveClientLeadWord(*client);
        *g_parkSaveClient = zge::registerSaveClient(client);
    }
    READABLE_FUNCTION_WAITS(registerParkSaveClient, Verified);

    // ---- the save menus' script functions --------------------------------------

    // "InlineMemCard.OnUserSelection": the menu answered with the variable's
    // number.
    void answerSaveMenuFromVariable(ee::Ptr<ee::Ptr<zge::zgui::Node>> arguments)
    {
        ee::Local<int32_t> answer;
        if (readVariableNumber(arguments[0], answer))
            zge::answerSaveMenu(*answer);
    }
    READABLE_FUNCTION_WAITS(answerSaveMenuFromVariable, Verified);

    // The author and name of the save in the buffer, and the name as the
    // save's title.
    void setParkSaveAuthorAndName(ee::Ptr<char> author, ee::Ptr<char> name)
    {
        const ee::Ptr<ParkSaveHeader> header = savedHeader();
        zge::stringCopy(header->author, author);
        zge::stringCopy(header->name, name);
        game::tail(zge::stringCopy, *g_parkFileName, name);
    }
    READABLE_FUNCTION(setParkSaveAuthorAndName, Verified);

    // "InlineMemCard.OnUserSetName": the author typed in, the save's info
    // shut, the save menu back.
    void inlineMemCardOnUserSetName(ee::Ptr<ee::Ptr<zge::zgui::Node>> arguments)
    {
        ee::Local<char[0x400]> text;
        if (!readVariableText(arguments[0], text))
            return;
        zge::stringCopy(savedHeader()->author, text);
        runMenuScript(sym::g_textPlaySaveLoadSaveInfoOnShutdown);
        runMenuScript(sym::g_textPlaySaveLoadSaveLoadMenuOnInit);
        zge::answerSaveMenu(0);
    }
    READABLE_FUNCTION_WAITS(inlineMemCardOnUserSetName, Verified);

    // A name no listed park save has: "UNNAMED" and the first number free. (The buffer's
    // room is handed over and never read.)
    void findUnusedParkName(ee::Ptr<char> name, uint32_t /*room*/)
    {
        for (uint32_t number = 0; number < kMostParkSaves; ++number)
        {
            zge::formatString(name, sym::bmx::g_textUnnamedPark, number); // "UNNAMED%2d"
            bool taken = false;
            for (uint32_t i = 0; i < *g_parkNameCount; ++i)
            {
                if (zge::stringCompareNoCase((*g_parkNameList)[i], name) == 0)
                {
                    taken = true;
                    break;
                }
            }
            if (!taken)
                return;
        }
    }
    READABLE_FUNCTION(findUnusedParkName, Verified);

    // "parkEditorMemCardOnUserSetDescription": the park's name typed in (an unused "UNNAMED"
    // name when left empty), as the save's title too; then the author is asked for, the
    // keyboard starting at "Somebody".
    void inlineMemCardOnUserSetDescription(ee::Ptr<ee::Ptr<zge::zgui::Node>> arguments)
    {
        ee::Local<char[0x400]> text;
        if (!readVariableText(arguments[0], text))
            return;
        const ee::Ptr<ParkSaveHeader> header = savedHeader();
        if (zge::stringLength(text) == 0)
            findUnusedParkName(text, 0x400);
        zge::stringCopy(*g_parkFileName, text);
        zge::stringCopy(header->name, text);
        runMenuScript(sym::g_textSetSaveInfoKeyboardSomebody);
        runMenuScript(sym::g_textSetSaveInfoAuthorNameSomebody);
        runMenuScript(sym::g_textSetSaveInfoDataTextSomebody);
        runMenuScript(sym::g_textPlaySaveInfoEnterName);
    }
    READABLE_FUNCTION_WAITS(inlineMemCardOnUserSetDescription, Verified);

    // "parkEditorMemCardOnUserFileSelection": a row chosen in the choose-park menu. While
    // saving, the row past the list is a new save and a damaged save's row is one to write
    // over: the save's info page asks for a name, starting from an unused one. Any other
    // row: the save menu answered with that file, its author and name taken for the buffer's
    // save.
    // Kept from the original: the damaged rows are looked up in the list of names, which
    // also holds the saves of other themes the editor left out of the menu, so with such
    // saves listed the row and the name can be different saves'.
    void inlineMemCardOnUserFileSelection(ee::Ptr<ee::Ptr<zge::zgui::Node>> arguments)
    {
        ee::Local<char[0x100]> name;
        ee::Local<char[0x100]> script;
        ee::Local<int32_t> choice;
        if (!readVariableNumber(arguments[0], choice))
            return;
        runMenuScript(sym::g_textPlaySaveLoadChooseParkMenu);
        const auto damaged = [&] {
            return zge::stringCompareNoCase((*g_parkNameList)[*choice], ee::Ptr<const char>(kDamagedName)) == 0;
        };
        const bool newSave = static_cast<uint32_t>(*choice) == *g_parkSaveFileCount && *g_parkSaving != 0;
        if (newSave || (damaged() && *g_parkSaving != 0))
        {
            runMenuScript(sym::g_textPlaySaveInfoOnInit);
            findUnusedParkName(name, 0x100);
            zge::formatString(script, sym::bmx::g_textSetSaveInfoDescription, ee::Ptr<char>(name)); // SET SaveLoad.SaveInfo.Description {"%s"}
            runMenuScript(script);
            runMenuScript(sym::g_textPlaySaveInfoEnterDescription);
            zge::answerSaveFileChoice(sym::g_saveMessageEmpty);
            // A damaged save is written over: its file is the one chosen.
            if (damaged() && *g_parkSaving != 0)
                zge::answerSaveFileChoice(&(*g_parkFileList)[*choice]);
            return;
        }
        ParkSaveFile &file = (*g_parkFileList)[*choice];
        setParkSaveAuthorAndName(file.header.author, file.header.name);
        zge::answerSaveFileChoice(&file);
        zge::answerSaveMenu(0);
        runMenuScript(sym::g_textPlaySaveLoadSaveLoadMenuOnInit);
    }
    READABLE_FUNCTION_WAITS(inlineMemCardOnUserFileSelection, Verified);

    // ---- a save or load started ------------------------------------------------

    // The save's icon read from the disc.
    void loadParkSaveIcon()
    {
        const ee::Ptr<zge::ZFile> file = zge::openFile(kParkSaveIconPath, 1, 0).file();
        zge::fileSize(file, sym::g_parkSaveIconSize, 0);
        const uint32_t icon = zge::heapAllocate(*g_parkSaveIconSize);
        const uint32_t iconBytes = *g_parkSaveIconSize;
        *g_parkSaveIcon = icon;
        zge::fileRead(file, icon, iconBytes, 0);
        game::tail(zge::closeFile, file, 0);
    }
    READABLE_FUNCTION_WAITS(loadParkSaveIcon, Verified);

    void freeParkFileList()
    {
        zge::heapFree(*g_parkFileList);
        *g_parkFileList = ee::Ptr<ParkSaveFile>();
        *g_parkFileEntryBytes = 0;
    }
    READABLE_FUNCTION_WAITS(freeParkFileList, Verified);

    // A park's save started: its buffer and what to call at the end kept, the icon read, the
    // saves listed, the save menu opened and the player asked which save to write.
    int32_t startParkSave(ee::Ptr<ParkBuffer> buffer, uint32_t done)
    {
        *g_parkSaveCallback = done;
        *g_parkSaveUnderway = 1;
        *g_parkSaveBuffer = buffer;
        *g_parkSaving = 1;
        loadParkSaveIcon();
        newParkFileList();
        openSaveUi(kParkSaveMenu);
        zge::startSaveSystem();
        return game::tail(zge::saveApiSaveChoice, ee::Ptr<uint32_t>(kParkSaveClients)[buffer->client], kJobDelayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startParkSave, Verified);

    // Room for the park saves the save list finds (the park's client only).
    void newParkFileList()
    {
        if ((*g_parkSaveBuffer)->client != 0)
            return;
        *g_parkFileEntryBytes = sizeof(ParkSaveFile);
        *g_parkFileList = ee::Ptr<ParkSaveFile>(zge::allocateAligned(kFileListAlignment, kMostParkSaves * sizeof(ParkSaveFile)));
    }
    READABLE_FUNCTION_WAITS(newParkFileList, Verified);

    // A park's load started: as a save's start, with the load menu and the player asked which
    // save to read; the caller's third word goes on to the save API.
    int32_t startParkLoad(ee::Ptr<ParkBuffer> buffer, uint32_t done, uint32_t third)
    {
        *g_parkSaveUnderway = 1;
        *g_parkSaveBuffer = buffer;
        *g_parkSaveCallback = done;
        *g_parkSaving = 0;
        loadParkSaveIcon();
        newParkFileList();
        openSaveUi(kParkLoadMenu);
        zge::startSaveSystem();
        return saveApiLoadChoiceTail(ee::Ptr<uint32_t>(kParkSaveClients)[buffer->client], kJobDelayMilliseconds, third);
    }
    READABLE_FUNCTION_WAITS(startParkLoad, Verified);
}
