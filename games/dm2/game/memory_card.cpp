// DM2's memory card layer (memory_card.h). DM2's own code (game.c's range).

#include "memory_card.h"

#include "goals.h"
#include "players.h"
#include "round.h"
#include "dm2/action/action.h"
#include "dm2/action/ui_frame.h"
#include "dm2/editor/edit_themes.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "sce/libmc.h"
#include "zge/assets/file.h"
#include "zge/platform/platform.h"
#include "zge/platform/save_system.h"
#include "zge/platform/memory_card/memory_card.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/game/memory_card.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The card in use: its "port:slot" text for the device ("0:0" or
        // "1:0"), the memory card device, the port, and the kind of file meant.
        inline ee::Global<char[16], sym::g_cardPortSlot> g_cardPortSlot;
        inline ee::Global<uint32_t, sym::g_cardDevice> g_cardDevice;
        inline ee::Global<uint8_t, sym::g_mountedCardPort> g_cardPort;
        inline ee::Global<uint8_t, sym::g_cardFileKind> g_cardFileKind;
        constexpr uint32_t kFirstPortSlot = sym::firstCardPortSlot;   // "0:0"
        constexpr uint32_t kMemoryCardDevice = sym::memoryCardDevicePath; // "mc00:\"
        constexpr uint32_t kCardPorts = 2;

        // The game's three files (by CardFileKind): each one's folder on the
        // card, its name and its size; a file's path is put together in
        // g_cardFilePath ("device:folder\name").
        constexpr uint32_t kCardFileKinds = 3;
        inline ee::Global<ee::Ptr<const char>[kCardFileKinds], sym::g_cardFolders> g_cardFolders;
        inline ee::Global<ee::Ptr<const char>[kCardFileKinds], sym::g_cardFileNames> g_cardFileNames;
        inline ee::Global<uint32_t[kCardFileKinds], sym::g_cardFileSizes> g_cardFileSizes;
        constexpr uint32_t kCardFilePath = sym::g_cardFilePath;
        constexpr uint32_t kCardPathFormat = sym::cardPathFormat; // "%s:%s\%s"
        // What a file takes on the card besides its own bytes (its folder, icon and entries).
        constexpr uint32_t kCardFileOverhead = 0x8B5C;
        constexpr uint32_t kOpenToRead = 1;

        // The piece of card work under way (one at a time): what it is, and
        // its record: the caller's call (told the result when the card has
        // answered), the buffer a load or a save goes through, and the slot
        // of the library a load is for.
        enum CardWork : uint8_t
        {
            kCardWorkCheck = 0,
            kCardWorkFileLookup = 1,
            kCardWorkSpaceCheck = 2,
            kCardWorkFormat = 3,
            kCardWorkLoad = 4,
            kCardWorkSave = 5,
            kCardWorkCreate = 6,
            kCardWorkNone = 0xFF,
        };
        struct CardWorkCall
        {
            CardCall call;
            uint32_t buffer;
            uint32_t slot;
        };
        static_assert(offsetof(CardWorkCall, buffer) == 0x08 && sizeof(CardWorkCall) == 0x10);
        inline ee::Global<uint8_t, sym::g_cardWork> g_cardWork;
        inline ee::Global<CardWorkCall, sym::g_cardWorkCall> g_cardWorkCall;

        // The files on the card: the game save (GameSaveFile) and the replay
        // and park libraries (ReplayLibraryFile, ParkLibraryFile: memory_card.h).
        constexpr uint32_t kSavedGame = sym::g_savedGame;
        constexpr uint32_t kSavedRecords = sym::g_savedRecords;

        // The save folder's files the PS2's browser shows it by: the icon
        // (the game's "ASSETS\ICON.DAT" copied as "bmxicon.ico"), icon.sys,
        // and the folder's own file (1 KB of zeros), with the title each kind
        // of file is shown under ("BMX 2 Save Game", "BMX 2 Replays",
        // "BMX 2 Parks").
        inline ee::Global<ee::Ptr<const char>, sym::g_saveIconDataPath> g_saveIconDataPath;
        inline ee::Global<ee::Ptr<const char>, sym::g_saveIconName> g_saveIconName;
        inline ee::Global<ee::Ptr<const char>, sym::g_saveIconSysName> g_saveIconSysName;
        constexpr uint32_t kSaveIconNameText = sym::saveIconNameText; // "bmxicon.ico"
        inline ee::Global<ee::Ptr<const char>[kCardFileKinds], sym::g_cardSaveTitles> g_cardSaveTitles;
        constexpr uint32_t kSaveIconSys = sym::g_saveIconSys;
        inline ee::Global<sce::mcIcon, kSaveIconSys> g_saveIconSys;
        constexpr uint32_t kSaveIconBytes = 0x8398;
        constexpr uint32_t kFolderFileBytes = 0x400;
        constexpr uint32_t kCardFolderFormat = sym::cardFolderFormat;         // "%s:%s"
        constexpr uint32_t kCardFolderFileFormat = sym::cardFolderFileFormat; // "%s:%s%s" (folder, then the folder's name)
        constexpr uint32_t kPortSlotFormat = sym::cardPortSlotFormat;         // "%d:%d"
        constexpr uint32_t kOpenToWrite = 2;
        constexpr int32_t kFileCallFailed = -1; // fileRead and fileWrite: not done (or, with a callback, not queued)

        // g_cardFilePath set to the folder of the kind of file meant, to a
        // file in it, or to the folder's own file.
        void setFolderPath()
        {
            zge::formatString(kCardFilePath, kCardFolderFormat, zge::deviceName(*g_cardDevice),
                              (*g_cardFolders)[*g_cardFileKind]);
        }
        void setFilePath(ee::Ptr<const char> name)
        {
            zge::formatString(kCardFilePath, kCardPathFormat, zge::deviceName(*g_cardDevice),
                              (*g_cardFolders)[*g_cardFileKind], name);
        }
        void setFolderFilePath()
        {
            zge::formatString(kCardFilePath, kCardFolderFileFormat, zge::deviceName(*g_cardDevice),
                              (*g_cardFolders)[*g_cardFileKind], (*g_cardFolders)[*g_cardFileKind]);
        }

        // Whether the file at g_cardFilePath loads with `expected` bytes. It
        // is loaded whole to be measured, and freed when it measures right; a
        // file of another size is left loaded (bug kept: that memory is never
        // freed).
        bool cardFileHasBytes(uint32_t expected, ee::Ptr<uint32_t> bytes)
        {
            const uint32_t data = zge::loadFile(kCardFilePath, bytes);
            if (data == 0 || *bytes != expected)
                return false;
            zge::freeLoadedFile(data);
            return true;
        }

        // Work started: the caller's call kept, and the callback the card
        // answers it through set to cardWorkDone with the work's record.
        void keepCardCall(ee::Ptr<CardCall> call, ee::Ptr<zge::FileCallback> callback)
        {
            g_cardWorkCall->call.done = call->done;
            callback->function = sym::cardWorkDone;
            callback->context = sym::g_cardWorkCall;
            g_cardWorkCall->call.argument = call->argument;
        }
    }

    void cardFrameStartHook()
    {
    }
    READABLE_FUNCTION(cardFrameStartHook, Verified);

    void cardFrameEndHook()
    {
    }
    READABLE_FUNCTION(cardFrameEndHook, Verified);

    void setCardFileKind(uint32_t fileKind)
    {
        *g_cardFileKind = static_cast<uint8_t>(fileKind);
    }
    READABLE_FUNCTION(setCardFileKind, Verified);

    uint32_t selectCardPort(uint32_t port)
    {
        ee::Local<uint32_t> mounted; // the mount's result word (not read here)
        zge::stringCopy(*g_cardPortSlot, kFirstPortSlot);
        (*g_cardPortSlot)[0] = static_cast<char>('0' + port);
        *g_cardDevice = zge::findDevice(kMemoryCardDevice);
        *g_cardPort = static_cast<uint8_t>(port);
        return zge::mountDevice(*g_cardDevice, *g_cardPortSlot, mounted, 0);
    }
    READABLE_FUNCTION(selectCardPort, Verified);

    // checkMemoryCards: both ports looked at for a card the game's files can
    // go on. A port with no card counts as empty; a card whose format check
    // does not run is passed over. On a formatted card the room is asked
    // for: the three files with their overhead, less each file already
    // there. The first port to get that far is left in `port` (when given;
    // 0 before). Returns kCardsNoCard when both ports are empty, kCardsFull
    // when a card was short of room and none was in order, and 0 as soon as
    // a card has the room or the format check answers nothing.
    uint32_t checkMemoryCards(ee::Ptr<uint32_t> port)
    {
        ee::Local<uint32_t> answer;
        if (port)
            *port = 0;
        uint32_t emptyPorts = 0;
        uint32_t found = 0;
        for (uint32_t cardPort = 0; cardPort < kCardPorts; ++cardPort)
        {
            selectCardPort(cardPort);
            answer = 0;
            zge::checkDeviceMedium(*g_cardDevice, answer, 0);
            if (*answer == 0)
            {
                ++emptyPorts;
                continue;
            }
            if (zge::checkDeviceFormatted(*g_cardDevice, answer, 0) != 1)
                continue;
            if (*answer == 0)
                return 0;

            uint32_t needed = 0;
            for (uint32_t kind = 0; kind < kCardFileKinds; ++kind)
                needed += (*g_cardFileSizes)[kind] + kCardFileOverhead;
            for (uint32_t kind = 0; kind < kCardFileKinds; ++kind)
            {
                zge::formatString(kCardFilePath, kCardPathFormat, zge::deviceName(*g_cardDevice), (*g_cardFolders)[kind],
                                  (*g_cardFileNames)[kind]);
                const zge::OpenFileResult opened = zge::openFile(kCardFilePath, kOpenToRead, 0);
                if (opened.value != 0)
                {
                    zge::closeFile(ee::Ptr<zge::ZFile>(opened.value), 0);
                    needed -= (*g_cardFileSizes)[kind] + kCardFileOverhead;
                }
            }
            answer = 0;
            if (zge::checkDeviceFreeSpace(*g_cardDevice, needed, answer, 0) != 1)
                continue;
            if (found == 0 && port)
                *port = cardPort;
            if (*answer != 0)
                return 0;
            if (found == 0)
                found = kCardsFull;
        }
        return emptyPorts == kCardPorts ? kCardsNoCard : found;
    }
    READABLE_FUNCTION(checkMemoryCards, Verified);

    uint32_t recheckCard(ee::Ptr<int32_t> result)
    {
        ee::Local<uint32_t> answer;
        *answer = 0;
        zge::checkDeviceMedium(*g_cardDevice, answer, 0);
        if (*answer != 0)
            return 0;
        // Not the card that was mounted: the port mounted again tells a new
        // card from none.
        zge::mountDevice(*g_cardDevice, *g_cardPortSlot, answer, 0);
        *result = *answer == 0 ? kCardRemoved : kCardChanged;
        return 1;
    }
    READABLE_FUNCTION_WAITS(recheckCard, Verified);

    int32_t createSaveFolder()
    {
        ee::Local<uint8_t[kFolderFileBytes]> folderFile;
        ee::Local<uint32_t> answer;
        ee::Local<uint32_t> iconBytes;
        ee::Local<int32_t> result;
        *result = kCardDone;
        setFolderPath();
        zge::findDirectory(kCardFilePath, answer, 0);
        if (*answer != 0)
            return kCardDone;
        zge::makeDirectory(kCardFilePath, answer, 0);
        if (*answer != 1)
        {
            *result = kCardFailed;
            recheckCard(result);
            return *result;
        }

        // icon.sys: the browser's title for the kind of file, lit from three
        // sides with a white ambient light, its icon "bmxicon.ico" in every view.
        zge::memoryFill(kSaveIconSys, 0, sizeof(sce::mcIcon));
        sce::mcIcon &icon = *g_saveIconSys;
        icon.head[0] = 'P';
        icon.head[1] = 'S';
        icon.head[2] = '2';
        icon.head[3] = 'D';
        icon.nlOffset = 10;
        icon.trans = 0x79;
        for (int colour = 0; colour < 4; ++colour)
            for (int lane = 0; lane < 4; ++lane)
                icon.bgCol[colour][lane] = 0;
        for (int light = 0; light < 3; ++light)
            for (int lane = 0; lane < 4; ++lane)
            {
                icon.lightDir[light][lane] = ee::Float(lane == light ? 1.0f : 0.0f);
                icon.lightCol[light][lane] = ee::Float(0.0f);
            }
        icon.lightAmbient[0] = ee::Float(1.0f);
        icon.lightAmbient[1] = ee::Float(1.0f);
        icon.lightAmbient[2] = ee::Float(1.0f);
        icon.lightAmbient[3] = ee::Float(0.0f);
        zge::stringCopy(ee::Ptr<char>(icon.view), kSaveIconNameText);
        zge::stringCopy(ee::Ptr<char>(icon.copy), kSaveIconNameText);
        zge::stringCopy(ee::Ptr<char>(icon.del), kSaveIconNameText);
        zge::toSjisSaveName((*g_cardSaveTitles)[*g_cardFileKind], ee::Ptr<uint16_t>(icon.title));

        const uint32_t iconData = zge::loadFile(*g_saveIconDataPath, iconBytes);
        if (iconData == 0)
        {
            *result = kCardFailed;
        }
        else
        {
            setFilePath(*g_saveIconName);
            const ee::Ptr<zge::ZFile> file(zge::openFile(kCardFilePath, kOpenToWrite, 0).value);
            if (!file)
            {
                *result = kCardNoFile;
            }
            else
            {
                if (zge::fileWrite(file, iconData, *iconBytes, 0) == kFileCallFailed)
                    *result = kCardFailed;
                zge::closeFile(file, 0);
            }
            zge::freeLoadedFile(iconData);
        }
        if (*result == kCardDone)
        {
            setFilePath(*g_saveIconSysName);
            const ee::Ptr<zge::ZFile> file(zge::openFile(kCardFilePath, kOpenToWrite, 0).value);
            if (!file)
            {
                *result = kCardNoFile;
            }
            else
            {
                if (zge::fileWrite(file, kSaveIconSys, sizeof(sce::mcIcon), 0) == kFileCallFailed)
                    *result = kCardFailed;
                zge::closeFile(file, 0);
            }
        }
        if (*result == kCardDone)
        {
            zge::memoryFill(folderFile, 0, kFolderFileBytes);
            setFolderFilePath();
            const ee::Ptr<zge::ZFile> file(zge::openFile(kCardFilePath, kOpenToWrite, 0).value);
            if (!file)
            {
                *result = kCardNoFile;
            }
            else
            {
                if (zge::fileWrite(file, folderFile, kFolderFileBytes, 0) == kFileCallFailed)
                    *result = kCardFailed;
                zge::closeFile(file, 0);
            }
        }
        if (*result == kCardDone)
            return kCardDone;

        // A file failed: the three deleted again (the folder stays).
        setFilePath(*g_saveIconName);
        zge::deleteFile(kCardFilePath, 0);
        setFilePath(*g_saveIconSysName);
        zge::deleteFile(kCardFilePath, 0);
        setFolderFilePath();
        zge::deleteFile(kCardFilePath, 0);
        recheckCard(result);
        return *result;
    }
    READABLE_FUNCTION_WAITS(createSaveFolder, Verified);

    uint32_t cardSaveComplete()
    {
        ee::Local<uint32_t> found;
        ee::Local<uint32_t> bytes;
        setFolderPath();
        zge::findDirectory(kCardFilePath, found, 0);
        if (*found == 0)
            return 0;
        setFilePath(*g_saveIconName);
        if (!cardFileHasBytes(kSaveIconBytes, bytes))
            return 0;
        setFilePath(*g_saveIconSysName);
        if (!cardFileHasBytes(sizeof(sce::mcIcon), bytes))
            return 0;
        setFolderFilePath();
        if (!cardFileHasBytes(kFolderFileBytes, bytes))
            return 0;
        // (The game save itself is not looked at.)
        if (*g_cardFileKind == kCardFileGame)
            return 1;
        setFilePath((*g_cardFileNames)[*g_cardFileKind]);
        return cardFileHasBytes((*g_cardFileSizes)[*g_cardFileKind], bytes) ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(cardSaveComplete, Verified);

    // cardWorkDone: the card's answer to the work under way (the callback
    // every start... gives the card), made a CardResult and told to the
    // work's call; then no work is under way. The checks are taken as done
    // whatever the report says: their answer is its value (a check's or a
    // mount's answer, the file opened or not, the room there or not).
    void cardWorkDone(ee::Ptr<zge::FileReport> report)
    {
        ee::Local<uint32_t> answer;
        ee::Local<int32_t> result;
        const ee::Ptr<CardWorkCall> work(report->context);
        if (*g_cardWork < kCardWorkFormat)
            report->succeeded = 1;
        if (report->succeeded == 1)
        {
            *result = kCardDone;
            switch (*g_cardWork)
            {
            case kCardWorkCheck:
                if (report->value == 0)
                    *result = kCardMissing;
                else if (zge::checkDeviceFormatted(*g_cardDevice, answer, 0) != 1)
                    *result = kCardFailed;
                else if (*answer == 0)
                    *result = kCardUnformatted;
                break;
            case kCardWorkFileLookup:
            {
                *result = kCardNoFile;
                if (report->value == 0)
                {
                    // No file: is it the card, then?
                    *answer = 0;
                    zge::checkDeviceMedium(*g_cardDevice, answer, 0);
                    if (*answer == 0)
                        *result = kCardMissing;
                    else if (zge::checkDeviceFormatted(*g_cardDevice, answer, 0) != 1)
                        *result = kCardFailed;
                    else if (*answer == 0)
                        *result = kCardUnformatted;
                    break;
                }
                // The file is there: a library's slots read into the call's
                // argument (a slot list); a short read counts as no file.
                *result = kCardHasFile;
                const ee::Ptr<zge::ZFile> file(report->subject);
                if (*g_cardFileKind == kCardFileReplay)
                {
                    if (zge::fileRead(file, work->call.argument, sizeof(ReplayLibraryFile::slots), 0)
                        != static_cast<int32_t>(sizeof(ReplayLibraryFile::slots)))
                        *result = kCardNoFile;
                }
                else if (*g_cardFileKind == kCardFilePark)
                {
                    if (zge::fileRead(file, work->call.argument, sizeof(ParkLibraryFile::slots), 0)
                        != static_cast<int32_t>(sizeof(ParkLibraryFile::slots)))
                        *result = kCardNoFile;
                }
                zge::closeFile(file, 0);
                break;
            }
            case kCardWorkSpaceCheck:
                if (report->value != 0)
                    break;
                *answer = 0;
                zge::checkDeviceMedium(*g_cardDevice, answer, 0);
                *result = *answer == 0 ? kCardMissing : kCardFull;
                break;
            case kCardWorkFormat:
                recheckCard(result);
                break;
            case kCardWorkLoad:
            {
                // The file read whole into the buffer: the game save into the
                // saved game and records, a library's slot into the call's
                // argument (the request's data).
                const uint32_t bytes = (*g_cardFileSizes)[*g_cardFileKind];
                zge::closeFile(ee::Ptr<zge::ZFile>(report->subject), 0);
                if (report->value != bytes)
                {
                    *result = kCardFailed;
                }
                else if (*g_cardFileKind == kCardFileReplay)
                {
                    const ee::Ptr<ReplayLibraryFile> library(work->buffer);
                    zge::memoryCopy(work->call.argument, library->replays[work->slot], kReplayBytes);
                }
                else if (*g_cardFileKind == kCardFileGame)
                {
                    const ee::Ptr<GameSaveFile> save(work->buffer);
                    if (save->bytes != sizeof(GameSaveFile))
                    {
                        *result = kCardBadSave;
                    }
                    else
                    {
                        zge::memoryCopy(kSavedGame, save->game, kSavedGameBytes);
                        zge::memoryCopy(kSavedRecords, save->records, kSavedRecordsBytes);
                    }
                }
                else if (*g_cardFileKind == kCardFilePark)
                {
                    const ee::Ptr<ParkLibraryFile> library(work->buffer);
                    zge::memoryCopy(work->call.argument, library->parks[work->slot], kParkBytes);
                }
                zge::freeMemory(work->buffer);
                g_cardWorkCall->buffer = 0;
                recheckCard(result);
                break;
            }
            case kCardWorkSave:
            case kCardWorkCreate:
            {
                const uint32_t bytes = (*g_cardFileSizes)[*g_cardFileKind];
                zge::closeFile(ee::Ptr<zge::ZFile>(report->subject), 0);
                if (report->value != bytes)
                    *result = kCardFailed;
                zge::freeMemory(work->buffer);
                g_cardWorkCall->buffer = 0;
                recheckCard(result);
                break;
            }
            default:
                break;
            }
        }
        else if (*g_cardWork >= kCardWorkFormat && *g_cardWork <= kCardWorkCreate)
        {
            *result = kCardFailed;
            recheckCard(result);
        }
        else
        {
            *answer = 0;
            zge::checkDeviceMedium(*g_cardDevice, answer, 0);
            *result = *answer != 0 ? kCardFailed : kCardMissing;
        }

        if (g_cardWorkCall->buffer != 0)
        {
            zge::freeMemory(work->buffer);
            g_cardWorkCall->buffer = 0;
        }
        if (work->call.done)
            work->call.done(work->call.argument, *result);
        *g_cardWork = kCardWorkNone;
        g_cardWorkCall->call.done = {};
    }
    READABLE_FUNCTION_WAITS(cardWorkDone, Verified);

    // startCardCreate: an empty library file (a replay or a park library)
    // made in a new save folder. A game save is never made this way: its
    // kind answers kCardFailed and nobody is told.
    int32_t startCardCreate(ee::Ptr<CardCall> call)
    {
        ee::Local<zge::FileCallback> callback;
        const uint32_t bytes = (*g_cardFileSizes)[*g_cardFileKind];
        if (*g_cardFileKind == kCardFileGame)
            return kCardFailed;
        *g_cardWork = kCardWorkCreate;
        g_cardWorkCall->buffer = zge::allocateMemory(bytes);
        int32_t result = createSaveFolder();
        ee::Ptr<zge::ZFile> file;
        if (result == kCardDone)
        {
            setFilePath((*g_cardFileNames)[*g_cardFileKind]);
            file = ee::Ptr<zge::ZFile>(zge::openFile(kCardFilePath, kOpenToWrite, 0).value);
            if (file)
            {
                keepCardCall(call, callback);
                result = kCardFailed;
                zge::memoryFill(g_cardWorkCall->buffer, 0, bytes);
                if (zge::fileWrite(file, g_cardWorkCall->buffer, bytes, callback.address()) != kFileCallFailed)
                    result = kCardDone;
            }
            else
            {
                result = kCardNoFile;
            }
            if (result == kCardDone)
                return kCardDone;
        }
        if (file)
            zge::closeFile(file, 0);
        zge::freeMemory(g_cardWorkCall->buffer);
        g_cardWorkCall->buffer = 0;
        call->done(call->argument, result);
        return result;
    }
    READABLE_FUNCTION_WAITS(startCardCreate, Verified);

    // startCardCheck: whether the port's card is the one mounted (else the
    // port is mounted again, and the callback goes on from the mount) and
    // formatted. Answered at once, the callback is told what the card would
    // have told it, and then the call's callback too.
    int32_t startCardCheck(ee::Ptr<CardCall> call)
    {
        ee::Local<zge::FileCallback> callback;
        ee::Local<zge::FileReport> report;
        ee::Local<uint32_t> cardThere;
        ee::Local<uint32_t> formatted;
        *g_cardWork = kCardWorkCheck;
        keepCardCall(call, callback);
        *cardThere = 0;
        int32_t result = kCardDone;
        zge::checkDeviceMedium(*g_cardDevice, cardThere, 0);
        if (*cardThere == 0)
        {
            result = kCardFailed;
            zge::formatString(*g_cardPortSlot, kPortSlotFormat, *g_cardPort, 0);
            if (zge::mountDevice(*g_cardDevice, *g_cardPortSlot, 0, callback.address()) == 1)
                result = kCardDone;
            if (result == kCardDone)
                return kCardDone;
        }
        else
        {
            *formatted = 0;
            if (zge::checkDeviceFormatted(*g_cardDevice, formatted, 0) != 1)
                result = kCardFailed;
            else if (*formatted == 0)
                result = kCardUnformatted;
        }
        // (The report's subject is not set: a check's answer does not read it.)
        report->context = sym::g_cardWorkCall;
        report->succeeded = 1;
        report->value = *cardThere;
        cardWorkDone(report);
        call->done(call->argument, result);
        return result;
    }
    READABLE_FUNCTION_WAITS(startCardCheck, Verified);

    // startCardFileLookup: the file of the kind meant opened; the callback
    // reads a library's slots.
    int32_t startCardFileLookup(ee::Ptr<CardCall> call)
    {
        ee::Local<zge::FileCallback> callback;
        *g_cardWork = kCardWorkFileLookup;
        keepCardCall(call, callback);
        setFilePath((*g_cardFileNames)[*g_cardFileKind]);
        if (zge::openFile(kCardFilePath, kOpenToRead, callback.address()).value != 0)
            return kCardDone;
        call->done(call->argument, kCardFailed);
        return kCardFailed;
    }
    READABLE_FUNCTION_WAITS(startCardFileLookup, Verified);

    int32_t startCardSpaceCheck(ee::Ptr<CardCall> call)
    {
        ee::Local<zge::FileCallback> callback;
        *g_cardWork = kCardWorkSpaceCheck;
        keepCardCall(call, callback);
        if (zge::checkDeviceFreeSpace(*g_cardDevice, (*g_cardFileSizes)[*g_cardFileKind], 0, callback.address()) == 1)
            return kCardDone;
        call->done(call->argument, kCardFailed);
        return kCardFailed;
    }
    READABLE_FUNCTION_WAITS(startCardSpaceCheck, Verified);

    int32_t startCardFormat(ee::Ptr<CardCall> call)
    {
        ee::Local<zge::FileCallback> callback;
        ee::Local<int32_t> result;
        *g_cardWork = kCardWorkFormat;
        keepCardCall(call, callback);
        *result = kCardDone;
        if (zge::formatDevice(*g_cardDevice, callback.address()) != 1)
        {
            *result = kCardFailed;
            recheckCard(result);
            call->done(call->argument, *result);
        }
        return *result;
    }
    READABLE_FUNCTION_WAITS(startCardFormat, Verified);

    // A start... that could not start: its file closed, its buffer freed,
    // the card looked at again, and the call's callback told.
    namespace
    {
        int32_t cardWorkNotStarted(ee::Ptr<CardCall> call, ee::Ptr<zge::ZFile> file, ee::Ptr<int32_t> result)
        {
            if (file)
                zge::closeFile(file, 0);
            zge::freeMemory(g_cardWorkCall->buffer);
            g_cardWorkCall->buffer = 0;
            recheckCard(result);
            call->done(call->argument, *result);
            return *result;
        }
    }

    // startCardLoad: the file of the kind meant read whole into a buffer, the
    // callback taking what is asked out of it. For a library the call's
    // argument is a CardRequest: the slot is kept, and the callback is told
    // the request's data in the argument's place (a game save's load leaves
    // the argument of the work before it).
    int32_t startCardLoad(ee::Ptr<CardCall> call)
    {
        ee::Local<zge::FileCallback> callback;
        ee::Local<int32_t> result;
        *g_cardWork = kCardWorkLoad;
        ee::Ptr<zge::ZFile> file;
        *result = kCardDone;
        const uint32_t bytes = (*g_cardFileSizes)[*g_cardFileKind];
        g_cardWorkCall->buffer = zge::allocateMemory(bytes);
        g_cardWorkCall->call.done = call->done;
        if (cardSaveComplete() == 0)
            *result = kCardFailed;
        setFilePath((*g_cardFileNames)[*g_cardFileKind]);
        if (*g_cardFileKind == kCardFileReplay || *g_cardFileKind == kCardFilePark)
        {
            const ee::Ptr<CardRequest> request(call->argument);
            g_cardWorkCall->slot = request->slot;
            g_cardWorkCall->call.argument = request->data;
        }
        if (*result != kCardDone)
            return cardWorkNotStarted(call, file, result);
        file = ee::Ptr<zge::ZFile>(zge::openFile(kCardFilePath, kOpenToRead, 0).value);
        if (!file)
        {
            *result = kCardFailed;
        }
        else
        {
            callback->function = sym::cardWorkDone;
            callback->context = sym::g_cardWorkCall;
            if (zge::fileRead(file, g_cardWorkCall->buffer, bytes, callback.address()) == kFileCallFailed)
                *result = kCardFailed;
        }
        if (*result == kCardDone)
            return kCardDone;
        return cardWorkNotStarted(call, file, result);
    }
    READABLE_FUNCTION_WAITS(startCardLoad, Verified);

    // startCardSave: the file of the kind meant written whole. A game save
    // is the saved game and records (in a ProQuest level the player's goals
    // are first restarted: restartLevelGoals). A library is read whole, the
    // request's slot marked used, named (a park also by its author and the
    // current theme) and given the request's data, and written back.
    int32_t startCardSave(ee::Ptr<CardCall> call)
    {
        ee::Local<zge::FileCallback> callback;
        ee::Local<int32_t> result;
        *g_cardWork = kCardWorkSave;
        ee::Ptr<zge::ZFile> file;
        ee::Ptr<CardRequest> request;
        const uint32_t bytes = (*g_cardFileSizes)[*g_cardFileKind];
        g_cardWorkCall->buffer = zge::allocateMemory(bytes);
        g_cardWorkCall->call.done = call->done;
        g_cardWorkCall->call.argument = call->argument;
        uint32_t data;
        if (*g_cardFileKind == kCardFileReplay || *g_cardFileKind == kCardFilePark)
        {
            request = ee::Ptr<CardRequest>(call->argument);
            data = request->data;
        }
        else
        {
            if (gameState() == kGameStateLevel && singlePlayerMode() == kLevelGoals)
                restartLevelGoals(*g_currentPlayer);
            const ee::Ptr<GameSaveFile> save(g_cardWorkCall->buffer);
            data = ee::Address(save);
            zge::memoryCopy(ee::Ptr<uint8_t>(save->game), kSavedGame, kSavedGameBytes);
            zge::memoryCopy(ee::Ptr<uint8_t>(save->records), kSavedRecords, kSavedRecordsBytes);
            save->bytes = sizeof(GameSaveFile);
            save->magic = kGameSaveMagic;
        }
        *result = createSaveFolder();
        if (*result == kCardDone && cardSaveComplete() == 0)
            *result = kCardFailed;
        setFilePath((*g_cardFileNames)[*g_cardFileKind]);
        if (*result != kCardDone)
            return cardWorkNotStarted(call, file, result);

        if (*g_cardFileKind == kCardFileReplay)
        {
            const ee::Ptr<ReplayLibraryFile> library(g_cardWorkCall->buffer);
            file = ee::Ptr<zge::ZFile>(zge::openFile(kCardFilePath, kOpenToRead, 0).value);
            if (!file)
            {
                *result = kCardFailed;
            }
            else
            {
                if (zge::fileRead(file, library, sizeof(ReplayLibraryFile), 0) != static_cast<int32_t>(sizeof(ReplayLibraryFile)))
                {
                    *result = kCardFailed;
                }
                else
                {
                    data = ee::Address(library);
                    library->slots[request->slot].flags |= kSlotUsed;
                    zge::stringCopy(ee::Ptr<char>(library->slots[request->slot].name), request->name);
                    zge::memoryCopy(ee::Ptr<uint8_t>(library->replays[request->slot]), request->data, kReplayBytes);
                }
                zge::closeFile(file, 0);
            }
        }
        else if (*g_cardFileKind == kCardFilePark)
        {
            const ee::Ptr<ParkLibraryFile> library(g_cardWorkCall->buffer);
            file = ee::Ptr<zge::ZFile>(zge::openFile(kCardFilePath, kOpenToRead, 0).value);
            if (!file)
            {
                *result = kCardFailed;
            }
            else
            {
                if (zge::fileRead(file, library, sizeof(ParkLibraryFile), 0) != static_cast<int32_t>(sizeof(ParkLibraryFile)))
                {
                    *result = kCardFailed;
                }
                else
                {
                    data = ee::Address(library);
                    ParkSlot &slot = library->slots[request->slot];
                    slot.flags |= kSlotUsed;
                    zge::stringCopy(ee::Ptr<char>(slot.name), request->name);
                    zge::stringCopy(ee::Ptr<char>(library->slots[request->slot].author), request->author);
                    zge::stringCopy(ee::Ptr<char>(library->slots[request->slot].theme), currentThemeName());
                    zge::memoryCopy(ee::Ptr<uint8_t>(library->parks[request->slot]), request->data, kParkBytes);
                }
                zge::closeFile(file, 0);
            }
        }
        // (A library whose read fell short is closed again below: bug kept,
        // the file is closed twice.)
        if (*result != kCardDone)
            return cardWorkNotStarted(call, file, result);

        file = ee::Ptr<zge::ZFile>(zge::openFile(kCardFilePath, kOpenToWrite, 0).value);
        if (!file)
        {
            *result = kCardFailed;
        }
        else
        {
            keepCardCall(call, callback);
            if (zge::fileWrite(file, data, bytes, callback.address()) == kFileCallFailed)
                *result = kCardFailed;
        }
        if (*result == kCardDone)
            return kCardDone;
        return cardWorkNotStarted(call, file, result);
    }
    READABLE_FUNCTION_WAITS(startCardSave, Verified);
}
