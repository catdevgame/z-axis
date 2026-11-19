// Saving a park and loading one (the park's save
// client of the engine's save system, zge/platform/save_system.cpp): the
// park's bytes go in a ParkBuffer (park_buffer.h); the save menus list the
// park saves it finds with their name, author and theme, ask for a new
// save's name and author, and give the park's own title and icon. The script
// functions here are the save menus' ("InlineMemCard.OnUser...",
// registered by game_save.cpp).

#include "aggressive/level/park_save.h"
#include "aggressive/level/park_buffer.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"

#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/platform/save_system.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"
#include "aggressive/game_save.h"
#include "aggressive/level/park_editor.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/park_save.cpp.h"

namespace zge
{
    // platform/save_system.cpp
}

namespace aggressive::level
{
    void freeParkFileList(); // below

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

        // At most this many park saves are listed (the list's room).
        constexpr uint32_t kMostParkSaves = 550;

        // A park save or load under way (1), and which.
        inline ee::Global<uint32_t, sym::g_parkSaveUnderway> g_parkSaveUnderway;
        inline ee::Global<uint32_t, sym::g_parkSaving> g_parkSaving; // 1 saving (the list offers a new save), 0 loading
        inline ee::Global<ee::Ptr<ParkBuffer>, sym::g_parkSaveBuffer> g_parkSaveBuffer;
        // What the save job calls when it has ended, with how it ended.
        inline ee::Global<ee::FunctionPtr<void(uint32_t outcome)>, sym::g_parkSaveCallback> g_parkSaveCallback;
        // The save system's ids of the clients, by a buffer's client (one: the park's).
        constexpr uint32_t kParkSaveClients = sym::g_parkSaveClient;
        inline ee::Global<uint32_t, sym::g_parkSaveClient> g_parkSaveClient;

        // The park saves the listing found, and their names (for a new save's
        // default name).
        inline ee::Global<ee::Ptr<ParkSaveFile>, sym::g_parkFileList> g_parkFileList;
        inline ee::Global<uint32_t, sym::g_parkFileEntryBytes> g_parkFileEntryBytes;
        inline ee::Global<uint32_t, sym::g_parkSaveFileCount> g_parkSaveFileCount;
        inline ee::Global<char[kMostParkSaves][0x20], sym::g_parkNameList> g_parkNameList;
        inline ee::Global<uint32_t, sym::g_parkNameCount> g_parkNameCount;
        // The park's name as the player typed it (the save's title).
        inline ee::Global<char[0x20], sym::g_parkFileName> g_parkFileName;
        inline ee::Global<char[0x40], sym::g_parkSaveTitle> g_parkSaveTitle;

        // The save's icon (assets\EditIcon.icn), read for each save or load.
        inline ee::Global<uint32_t, sym::g_parkSaveIcon> g_parkSaveIcon;
        inline ee::Global<uint32_t, sym::g_parkSaveIconSize> g_parkSaveIconSize;
        constexpr uint32_t kParkSaveIconPath = sym::g_textAssetsEditIconIcn; // "assets\EditIcon.icn"
        // How long the save job waits before it starts.
        constexpr uint32_t kJobDelayMilliseconds = 750;
        // The save menu's kinds (openSaveUi): a park's save and load.
        constexpr uint32_t kParkSaveMenu = 3, kParkLoadMenu = 4;
    }

    namespace
    {
        // Called by their addresses: a game override hooks openSaveUi; the
        // others are game_save.cpp's.
        // The script variable's value as text, and as a number (its own code folds
        // with another function's on the Windows link: called at its address).
        int32_t readVariableText(ee::Ptr<zgui::Node> variable, ee::Ptr<char> out) { return zgui::variableToText(variable, out); }
        int32_t readVariableNumber(ee::Ptr<zgui::Node> variable, ee::Ptr<int32_t> out) { return zgui::variableIntValue(variable, out); }

        // A line of the menus' script run (no widget).
        void runMenuScript(ee::Ptr<char> line)
        {
            zgui::runScript(ee::Ptr<zgui::Node>(), line, 0);
        }
        void runMenuScript(uint32_t line) { runMenuScript(ee::Ptr<char>(line)); }

        // The park's header in the buffer being saved.
        ee::Ptr<ParkSaveHeader> savedHeader()
        {
            return (*g_parkSaveBuffer)->data.cast<ParkSaveHeader>();
        }
    }

    // ---- the save client's methods -------------------------------------------

    uint32_t parkSaveNoop()
    {
        return 1;
    }
    READABLE_FUNCTION(parkSaveNoop, Draft);

    uint32_t parkSaveDataSize()
    {
        return 0x2000;
    }
    READABLE_FUNCTION(parkSaveDataSize, Verified);

    uint32_t parkSaveSingleFile()
    {
        return 0;
    }
    READABLE_FUNCTION(parkSaveSingleFile, Verified);

    uint32_t parkSaveIconSize()
    {
        return 2940;
    }
    READABLE_FUNCTION(parkSaveIconSize, Verified);

    uint32_t parkSaveIcon()
    {
        return *g_parkSaveIcon;
    }
    READABLE_FUNCTION(parkSaveIcon, Verified);

    uint32_t parkSaveFileName()
    {
        return sym::g_textBaslus20327ipark; // "BASLUS-20327IPARK"
    }
    READABLE_FUNCTION(parkSaveFileName, Verified);

    // The save's title: "Inline Park" and the park's name, if it
    // has one.
    //
    // The original hands formatString one word more than its format reads:
    // the park name's address, which measuring the name (strlen) leaves as
    // the next argument word. formatString stores its argument words on the
    // stack for vsprintf, and the save message built after it copies
    // that stack word into the heap, so it is passed here as the original
    // passes it. Without a name, the word before it is one the format does
    // not read either: the original passes on what its caller left there,
    // 0 in every recorded save.
    ee::Ptr<char> parkSaveTitle()
    {
        if (!*g_parkSaveUnderway)
            return ee::Ptr<char>(sym::g_textInlinePark);
        char *const name = *g_parkFileName;
        char *const nameLeftByMeasuring = name;
        constexpr uint32_t unreadWord = 0;
        if (zge::stringLength(name) != 0)
            zge::formatString(*g_parkSaveTitle, sym::g_textInlinePark2, name, nameLeftByMeasuring); // "Inline Park %s"
        else
            zge::formatString(*g_parkSaveTitle, sym::g_textInlinePark, unreadWord, nameLeftByMeasuring); // "Inline Park"
        return ee::Ptr<char>(*g_parkSaveTitle);
    }
    READABLE_FUNCTION(parkSaveTitle, Verified);

    // The save job has ended (how, as the save system's last message says):
    // the park's own callback told, the save system stopped, the menu shut,
    // the icon and the list freed.
    void parkSaveFinish(uint32_t outcome)
    {
        if (!*g_parkSaveUnderway)
            return;
        (*g_parkSaveCallback)(outcome);
        zge::stopSaveSystem();
        aggressive::playSaveShutdown();
        aggressive::freeParkSaveIcon();
        freeParkFileList();
        (*g_parkFileName)[0] = 0;
        *g_parkSaveUnderway = 0;
    }
    READABLE_FUNCTION_WAITS(parkSaveFinish, Verified);

    ee::Ptr<uint8_t> parkSaveData()
    {
        return (*g_parkSaveBuffer)->data;
    }
    READABLE_FUNCTION(parkSaveData, Verified);

    // The character the choose-park menu's theme column shows for a theme
    // (the menu font's theme icons).
    uint32_t parkThemeIconChar(int32_t theme)
    {
        switch (theme)
        {
        case 0:
            return '&';
        case 1:
            return '%';
        case 2:
            return '|';
        case 3:
            return '$';
        default:
            return 'E';
        }
    }
    READABLE_FUNCTION(parkThemeIconChar, Verified);

    // A park save the save list found (the client's saveFound): kept in
    // the list and its name noted; shown in the choose-park menu's three
    // columns (name, author, theme) unless the editor is open on another
    // theme. A null file name starts the listing: the columns and the list
    // emptied.
    void addParkSaveFile(ee::Ptr<char> fileName, ee::Ptr<ParkSaveHeader> header)
    {
        ee::Local<char[0x400]> line;
        if (!fileName)
        {
            line[0] = 0;
            zgui::setNodeValueByPath(sym::g_textSaveLoadChooseParkMenuFileListMenu2, line);
            zgui::setNodeValueByPath(sym::g_textSaveLoadChooseParkMenuAuthorMenu2, line);
            zgui::setNodeValueByPath(sym::g_textSaveLoadChooseParkMenuThemeMenu2, line);
            *g_parkNameCount = 0;
            *g_parkSaveFileCount = 0;
            return;
        }
        ParkSaveFile &file = (*g_parkFileList)[*g_parkSaveFileCount];
        zge::stringCopy(file.fileName, fileName);
        zge::memoryCopy(&file.header, header, sizeof(ParkSaveHeader));
        zge::stringCopy((*g_parkNameList)[*g_parkNameCount], file.header.name);
        *g_parkNameCount = *g_parkNameCount + 1;
        if (inParkEditor() && parkEditorTheme() != file.header.theme)
            return;
        if (*g_parkSaveFileCount != 0)
        {
            // A new row in each column.
            runMenuScript(sym::g_textStrcatSaveLoadChooseParkMenu);
            runMenuScript(sym::g_textStrcatSaveLoadChooseParkMenu2);
            runMenuScript(sym::g_textStrcatSaveLoadChooseParkMenu3);
        }
        const uint32_t themeIcon = parkThemeIconChar(file.header.theme);
        zge::formatString(line, sym::g_textStrcatSaveLoadChooseParkMenu5, file.header.name); // FileListMenu.Items {"%-16s"}
        runMenuScript(line);
        zge::formatString(line, sym::g_textStrcatSaveLoadChooseParkMenu6, file.header.author); // AuthorMenu.Items {"%-16s"}
        runMenuScript(line);
        zge::formatString(line, sym::g_textStrcatSaveLoadChooseParkMenu7, themeIcon); // ThemeMenu.Items {"%c"}
        runMenuScript(line);
        *g_parkSaveFileCount = *g_parkSaveFileCount + 1;
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
        *g_parkSaveClient = zge::registerSaveClient(client);
    }
    READABLE_FUNCTION_WAITS(registerParkSaveClient, Verified);

    // ---- the save menus' script functions --------------------------------------

    // "InlineMemCard.OnUserSelection": the menu answered with the variable's
    // number.
    void answerSaveMenuFromVariable(ee::Ptr<ee::Ptr<zgui::Node>> arguments)
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
    void inlineMemCardOnUserSetName(ee::Ptr<ee::Ptr<zgui::Node>> arguments)
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

    // A name no listed park save has: "Custom Park" and the first number
    // from 0 that makes one (case aside), into `name`; the last tried if all
    // 550 are taken.
    void findUnusedParkName(ee::Ptr<char> name)
    {
        for (uint32_t number = 0; number < kMostParkSaves; ++number)
        {
            zge::formatString(name, sym::g_textCustomPark, number); // "Custom Park%2d"
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

    // "InlineMemCard.OnUserSetDescription": the park's name typed in (an
    // unused "Custom Park" name when left empty), as the save's title too;
    // then the author is asked for.
    void inlineMemCardOnUserSetDescription(ee::Ptr<ee::Ptr<zgui::Node>> arguments)
    {
        ee::Local<char[0x400]> text;
        if (!readVariableText(arguments[0], text))
            return;
        const ee::Ptr<ParkSaveHeader> header = savedHeader();
        if (zge::stringLength(text) == 0)
            findUnusedParkName(text);
        zge::stringCopy(*g_parkFileName, text);
        zge::stringCopy(header->name, text);
        runMenuScript(sym::g_textPlaySaveLoadSaveInfoEnterName);
    }
    READABLE_FUNCTION_WAITS(inlineMemCardOnUserSetDescription, Verified);

    // "InlineMemCard.OnUserFileSelection": a park chosen in the choose-park
    // menu. The row past the list while saving is a new save: its name is
    // asked for. Any other: the save menu answered with that file, its
    // author and name taken for the buffer's save.
    void inlineMemCardOnUserFileSelection(ee::Ptr<ee::Ptr<zgui::Node>> arguments)
    {
        ee::Local<int32_t> choice;
        if (!readVariableNumber(arguments[0], choice))
            return;
        runMenuScript(sym::g_textPlaySaveLoadChooseParkMenu);
        if (static_cast<uint32_t>(*choice) == *g_parkSaveFileCount && *g_parkSaving)
        {
            runMenuScript(sym::g_textPlaySaveLoadSaveInfoOnInit);
            runMenuScript(sym::g_textPlaySaveLoadSaveInfoEnterDescripti);
            zge::answerSaveFileChoice(sym::g_saveMessageEmpty);
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

    // Room for the whole save list, for the park's client only.
    void newParkFileList()
    {
        if ((*g_parkSaveBuffer)->client != 0)
            return;
        *g_parkFileEntryBytes = sizeof(ParkSaveFile);
        *g_parkFileList = ee::Ptr<ParkSaveFile>(zge::heapAllocate(kMostParkSaves * sizeof(ParkSaveFile)));
    }
    READABLE_FUNCTION_WAITS(newParkFileList, Verified);

    // The park's load: it is read into `buffer`, and `done` is told how
    // it went when the job ends.
    int32_t startParkLoad(ee::Ptr<ParkBuffer> buffer, uint32_t done)
    {
        *g_parkSaveUnderway = 1;
        *g_parkSaveBuffer = buffer;
        *g_parkSaveCallback = done;
        *g_parkSaving = 0;
        loadParkSaveIcon();
        newParkFileList();
        aggressive::openSaveUi(kParkLoadMenu);
        zge::startSaveSystem();
        return game::tail(zge::saveApiLoadChoice, ee::Ptr<uint32_t>(kParkSaveClients)[buffer->client], kJobDelayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startParkLoad, Verified);

    // The save of the park in `buffer`; `done` is told how it went.
    int32_t startParkSave(ee::Ptr<ParkBuffer> buffer, uint32_t done)
    {
        *g_parkSaveCallback = done;
        *g_parkSaveUnderway = 1;
        *g_parkSaveBuffer = buffer;
        *g_parkSaving = 1;
        loadParkSaveIcon();
        newParkFileList();
        aggressive::openSaveUi(kParkSaveMenu);
        zge::startSaveSystem();
        return game::tail(zge::saveApiSaveChoice, ee::Ptr<uint32_t>(kParkSaveClients)[buffer->client], kJobDelayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startParkSave, Verified);
}
