// The engine's start-up and main loop (zge/platform/platform.h; the tail of
// zge/ps2/ps2Movie.c, from ps2SifInit on): engineMain prints the versions, sets
// the platform up, reboots the IOP and loads its modules (ps2SifInit), hands
// over to the game and runs frames until something asks to quit; assertFailed
// is the engine's stop screen.
//
// Where the IOP's modules come from depends on how the game was started:
// from the disc, as every retail copy (the only path this build reaches:
// initCdDevice sets it), over a development kit's debugger, or from the host.
// The two others are the original's and stay as they were.

#include "ee/io.h"
#include "platform.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/input/pads.h"
#include "zge/memory/regions.h"
#include "zge/render/display.h"
#include "zge/render/ps2/dma.h"
#include "zge/text/cstring.h"
#include "zge/debug/cycle_counter.h"
#include "zge/game.h"
#include "sce/libc.h"
#include "sce/libgcc.h"

#include <cstdint>
#include "sym/zge/platform/boot.cpp.h"

namespace zge
{

    namespace
    {
        // How the game was started (iopBootAddress, 0x14 bytes; initPlatformSettings
        // clears it, initCdDevice says disc). Nothing here sets the other two.
        constexpr uint32_t kIopBootBytes = 0x14u;

        // The IOP's modules (iopModules): four words each, the paths by where
        // they load from, and the order of the table.
        struct IopModuleRow
        {
            uint32_t hostPath; // "sdk/ps2/sce/iop/modules/sio2man.irx"
            uint32_t fileName; // "sio2man.irx"
            uint32_t discPath; // "cdrom0:\\SIO2MAN.IRX;1"
            uint32_t _0c;      // 0 in every row of the table; nothing reads it
        };
        static_assert(sizeof(IopModuleRow) == 16);
        enum IopModule : uint32_t
        {
            kSio2man,
            kMcman,
            kMcserv,
            kPadman,
            kLibsd,
            kSdrdrv,
            kZaudio,
            kZcddvd, // the disc's only: no file name
        };
        inline ee::Global<IopModuleRow[8], sym::iopModules> g_iopModules;

        constexpr uint32_t kIopReplacementImage = sym::g_iopImagePath;          // its path: "cdrom0:\IOPRP243.IMG;1"
        constexpr uint32_t kDevicePrefix = sym::devicePrefix;                 // g_platformSettings->devicePrefix
        constexpr uint32_t kTextInitializingCd = sym::textInitializingCd;           // "Initializing CD...\n"
        constexpr uint32_t kTextLoadingProView = sym::textLoadingProView;           // "Loading IRX modules (PROVIEW)...\n"
        constexpr uint32_t kTextLoadingAtmon = sym::textLoadingAtmon;             // "Loading IRX modules (ATMON)...\n"
        constexpr uint32_t kFormatCouldNotLoad = sym::formatCouldNotLoad;           // "Could not load %s\n"
        constexpr uint32_t kTextLoadingModules = sym::textLoadingModules;           // "Loading IRX modules...\n"
        constexpr uint32_t kTextCdOk = sym::textCdOk;                     // "CD:OK!\n"
        constexpr uint32_t kPadmanArguments = sym::padmanArguments;              // "thrpri=45,46"
        constexpr uint32_t kFormatPath = sym::formatPath;                   // "%s%s"
        constexpr uint32_t kTextSifOk = sym::textSifOk;                    // "ps2SifInit:OK!\n"
        constexpr uint32_t kFormatCompiler = sym::formatCompiler;               // "GNU COMPILER VERSION:%d.%d\n"
        constexpr uint32_t kFormatLibrary = sym::formatLibrary;                // "PS2 LIBRARY  VERSION:%x\n"
        constexpr uint32_t kFormatEngine = sym::formatEngine;                 // "ZGE LIBRARY  VERSION:%02x.%02x.%02x\n"
        constexpr uint32_t kFormatProgramSize = sym::bmx::formatProgramSize;    // (ZGE4) "ProgSize:%dk @0x%x (Code:%dk) (Data:%dk) (Bss:%dk) (Stack:%dk)\n"
        constexpr uint32_t kDebugFileName = sym::debugFileName;                // "$1"
        constexpr uint32_t kCdDevice = sym::cdDevice;                     // "cdrom0:"
        constexpr uint32_t kApplicationTitle = sym::applicationTitle;             // "PS2 APP"

        // GCC 2.95's, Sony's library and ZGE's versions, as engineMain prints them (ZGE4: 2.9 and 04.01.20).
        constexpr uint32_t kCompilerMajor = 2u, kCompilerMinor = game::readable::perGame(95u, 95u, 9u);
        constexpr uint32_t kSceLibraryVersion = 0x2430u;
        constexpr uint32_t kZgeMajor = game::readable::perGame(1u, 1u, 4u), kZgeMinor = game::readable::perGame(2u, 2u, 1u);
        constexpr uint32_t kZgeBuild = game::readable::perGame(0x49u, 0x49u, 0x20u);

        // The program's sizes as ZGE4's engineMain prints them, in kilobytes: the linked sizes of its code, its
        // data, its cleared data and the main thread's stack (each a constant of the link, a kilobyte being 1024
        // bytes); the whole program, from its entry to its end, is worked out from the two addresses.
        constexpr uint32_t kBytesAKilobyte = 1024u;
        constexpr uint32_t kCodeKilobytes = 1281u;
        constexpr uint32_t kDataKilobytes = 279u;
        constexpr uint32_t kClearedDataKilobytes = 455u;
        constexpr uint32_t kMainStackKilobytes = 128u;

        // The C library's zero-byte test masks (a byte of 0x01s, a byte of
        // 0x80s in every lane), which its strcpy and strlen leave behind in
        // the argument registers past the ones they take. formatScratch
        // stores all seven argument words in its frame whatever the format
        // reads, so the engine's version line hands on what debugWrite's
        // copy and measure of the line before left there; those stack words
        // stay above engineMain's frame for the rest of the game.
        constexpr uint64_t kZeroByteTestOnes = 0x0101010101010101ull;
        constexpr uint64_t kZeroByteTestHighBits = 0x8080808080808080ull;

        constexpr uint32_t kSceOpenWriteOnly = 2u; // SCE_WRONLY
        constexpr uint32_t kScreenWidth = 512u, kScreenHeight = 448u;
        constexpr uint16_t kCdModeDvd = 2u;
        constexpr uint16_t kCdModeCd = 1u; // (SCECdCD)

        // The C library's heap, as the engine's calls it (heapAllocate and
        // the others go through these).
        inline ee::Global<uint32_t, sym::g_heapAllocateFunction> g_heapAllocateFunction;
        inline ee::Global<uint32_t, sym::g_heapMemalignFunction> g_heapMemalignFunction;
        inline ee::Global<uint32_t, sym::g_heapFreeFunction> g_heapFreeFunction;
        inline ee::Global<uint32_t, sym::g_heapReallocateFunction> g_heapReallocateFunction;
        inline ee::Global<uint32_t, sym::g_heapClearAllocateFunction> g_heapClearAllocateFunction;
        constexpr uint32_t kMalloc = sym::malloc, kMemalign = sym::memalign, kFree = sym::free;
        constexpr uint32_t kRealloc = sym::realloc, kCalloc = sym::calloc;

        // What malloc's arena leaves out: above the program's end, the main
        // thread's stack (crt0's 1 MB) and 4 KB (BMX XXX keeps 128 KB and 4 KB);
        // below the top, 4112 bytes.
        constexpr uint32_t kKeptAboveProgram =
            game::readable::perGame((1u << 20) + (1u << 12), (1u << 20) + (1u << 12), (1u << 17) + (1u << 12));
        constexpr uint32_t kKeptBelowTop = 0x1010u;
        constexpr uint32_t kMainMemoryBytes = 0x02000000u;
        constexpr uint32_t kMainRegionBytes = 0x00080000u;
        constexpr uint32_t kMainRegionAlignment = 0x40u;

        inline ee::Global<uint32_t, sym::g_platformInitialized> g_platformInitialized;
        inline ee::Global<uint32_t, sym::g_quitRequested> g_quitRequested;           // requestFrameWait
        inline ee::Global<ee::Float, sym::g_fixedFrameStep> g_fixedFrameStep;         // setGlobalTimeScale; 0: from the vsyncs
        inline ee::Global<int32_t, sym::g_screenWidth> g_screenWidth;
        inline ee::Global<int32_t, sym::g_screenHeight> g_screenHeight;
        inline ee::Global<uint32_t, sym::g_inAssert> g_inAssert;
        constexpr uint32_t kAssertColour = sym::assertColour;                     // R, G, B, A: 128, 64, 64, 128

        // The longest step a frame takes (the game behind by four seconds of
        // vsyncs goes on from there) and a vsync's time.
        constexpr float kMaxFrameSeconds = 4.0f;
        constexpr float kVsyncSeconds = 0.016666668f;

        // The game functions below call: ee/plain.h.
        void sceSifInitRpc(uint32_t mode) { ee::game(sym::sifInitRpc, mode); }
        void sceCdInit(int32_t initMode) { ee::game(sym::sceCdInit, initMode); }
        void sceCdMmode(uint32_t mediaType) { ee::game(sym::sceCdMmode, mediaType); }
        void sceFsReset() { ee::game(sym::sceFsReset); }
        void sceCdDiskReady(int32_t mode) { ee::game(sym::sceCdDiskReady, mode); }
        int32_t sceSifLoadModule(ee::Address path, int32_t argumentBytes, uint32_t arguments) { return ee::game<int32_t>(sym::sceSifLoadModule, path, argumentBytes, arguments); }
        int32_t sceSifRebootIop(uint32_t image) { return ee::game<int32_t>(sym::sceSifRebootIop, image); }
        int32_t sceSifSyncIop() { return ee::game<int32_t>(sym::sceSifSyncIop); }
        void sceSifInitIopHeap() { ee::game(sym::sceSifInitIopHeap); }

        // The disc file system after an IOP reboot or a module that takes
        // the drive over: the mode the settings ask for, ready.
        void startDiscAccess()
        {
            sceCdInit(0);
            if constexpr (game::readable::kIsDm2) // (ZGE2 has no CD mode setting: always a CD)
                sceCdMmode(kCdModeCd);
            else
                sceCdMmode(g_platformSettings->cdMode);
            sceFsReset();
            sceCdDiskReady(0);
        }

        // A module from the disc, tried until it loads.
        void loadFromDisc(IopModule module)
        {
            while (sceSifLoadModule((*g_iopModules)[module].discPath, 0, 0) < 0)
            {
            }
        }

        // The retail way: the IOP rebooted into the disc's replacement
        // modules (it may refuse until it is ready), then the modules from
        // the disc, each tried until it loads. zcddvd, the streaming driver,
        // takes the drive, so the file system starts again after it.
        void loadModulesFromDisc()
        {
            zge::debugWrite(kTextInitializingCd);
            sceSifInitRpc(0);
            startDiscAccess();

            const uint32_t replacementImage = *ee::Ptr<uint32_t>(kIopReplacementImage);
            while (sceSifRebootIop(replacementImage) == 0)
            {
            }
            while (sceSifSyncIop() == 0)
            {
            }
            sceSifInitRpc(0);
            startDiscAccess();

            for (const IopModule module : {kSio2man, kPadman, kMcman, kMcserv, kLibsd, kSdrdrv, kZaudio})
                loadFromDisc(module);
            startDiscAccess();
            loadFromDisc(kZcddvd);
            startDiscAccess();
            zge::debugWrite(kTextCdOk);
        }

        // One module: the device's prefix and `modulePath` formatted into
        // `path` and loaded once, with `arguments` if there are any. False
        // when it failed.
        bool loadPrefixed(ee::Local<char[0x180]> &path, uint32_t modulePath, uint32_t arguments = 0)
        {
            zge::formatString(path, kFormatPath, kDevicePrefix, modulePath);
            if (arguments != 0)
            {
                const int32_t length = zge::stringLength(arguments);
                return sceSifLoadModule(path, length + 1, arguments) >= 0;
            }
            return sceSifLoadModule(path, 0, 0) >= 0;
        }

        // A development kit: the modules from the debugger's file server by
        // file name. ProView has none of the four before libsd loaded, ATMON
        // has them; padman gets its thread priorities. False at the first
        // module that did not load, its name in `failed` (whatever the name
        // is: the load's result decides, as the original's).
        bool loadModulesOverDebugger(ee::Local<char[0x180]> &path, uint32_t &failed)
        {
            const auto load = [&](IopModule module, uint32_t arguments = 0) {
                failed = (*g_iopModules)[module].fileName;
                return loadPrefixed(path, failed, arguments);
            };
            sceSifInitRpc(0);
            if (g_iopBoot->debugger == kProView)
                zge::debugWrite(kTextLoadingProView);
            else
                zge::debugWrite(kTextLoadingAtmon);
            if (g_iopBoot->debugger == kProView)
            {
                for (const IopModule module : {kSio2man, kMcman, kMcserv})
                    if (!load(module))
                        return false;
                if (!load(kPadman, kPadmanArguments))
                    return false;
            }
            for (const IopModule module : {kLibsd, kSdrdrv, kZaudio})
                if (!load(module))
                    return false;
            return true;
        }

        // Neither disc nor debugger: every module but zcddvd from the host,
        // by its path in the SDK. False at the first that did not load, its
        // path in `failed`.
        bool loadModulesFromHost(ee::Local<char[0x180]> &path, uint32_t &failed)
        {
            sceSifInitRpc(0);
            zge::debugWrite(kTextLoadingModules);
            for (const IopModule module : {kSio2man, kMcman, kMcserv, kPadman, kLibsd, kSdrdrv, kZaudio})
            {
                failed = (*g_iopModules)[module].hostPath;
                if (!loadPrefixed(path, failed))
                    return false;
            }
            return true;
        }
    }

    // ps2SifInit: the IOP started and its modules loaded, from wherever
    // the game was started; 1, or 0 with a message naming the module that
    // would not load.
    uint32_t ps2SifInit()
    {
        ee::Local<char[0x180]> path;
        uint32_t failed = 0;
        bool loaded = true;
        if (g_iopBoot->fromDisc != 0)
            loadModulesFromDisc();
        else if (g_iopBoot->onDebugger != 0)
            loaded = loadModulesOverDebugger(path, failed);
        else
            loaded = loadModulesFromHost(path, failed);
        if (!loaded)
        {
            debugWrite(formatScratch(kFormatCouldNotLoad, failed));
            return 0;
        }
        sceSifInitIopHeap();
        debugWrite(kTextSifOk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(ps2SifInit, Verified);

    // initPlatformSettings: the platform's defaults: the title, the screen, DVD
    // mode, and the C library's heap as the engine's allocator.
    uint32_t initPlatformSettings()
    {
        memoryFill(kIopBootAddress, 0, kIopBootBytes);
        memoryFill(kPlatformSettingsAddress, 0, kPlatformSettingsBytes);
        PlatformSettings &settings = *g_platformSettings;
        settings.title = kApplicationTitle;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no CD mode)
            settings.cdMode = kCdModeDvd;
        *g_heapAllocateFunction = kMalloc;
        *g_heapClearAllocateFunction = kCalloc;
        settings.width = kScreenWidth;
        settings.height = kScreenHeight;
        *g_heapMemalignFunction = kMemalign;
        *g_heapFreeFunction = kFree;
        *g_heapReallocateFunction = kRealloc;
        settings.clearedAtStart = 0;
        return 1;
    }
    READABLE_FUNCTION(initPlatformSettings, Verified);

    // initCdDevice: running from the disc: "cdrom0:" before every file name,
    // the console on.
    uint32_t initCdDevice()
    {
        g_iopBoot->fromDisc = 1;
        g_iopBoot->onDebugger = 0;
        g_iopBoot->debugger = 0;
        g_platformSettings->consoleOff = 0;
        stringCopy(kDevicePrefix, kCdDevice);
        return 1;
    }
    READABLE_FUNCTION(initCdDevice, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceDevVif0Reset() { ee::game(sym::sceDevVif0Reset); }
        void sceDevVif1Reset() { ee::game(sym::sceDevVif1Reset); }
        void sceDevVu0Reset() { ee::game(sym::sceDevVu0Reset); }
        void sceDevVu1Reset() { ee::game(sym::sceDevVu1Reset); }
        void sceGsResetPath() { ee::game(sym::sceGsResetPath); }
        namespace tail
        {
        void sceDmaReset(uint32_t mode) { ee::tailCall(sym::sceDmaReset, mode); }
        }
    }

    // resetHardware: the VIFs, VUs, the GIF's paths and the DMA channels back
    // to their start.
    void resetHardware()
    {
        sceDevVif0Reset();
        sceDevVif1Reset();
        sceDevVu0Reset();
        sceDevVu1Reset();
        sceGsResetPath();
        tail::sceDmaReset(1); // the original's tail call
    }
    READABLE_FUNCTION(resetHardware, Verified);

    // reserveMallocArena: the memory between the program's end and `top` (less
    // what kKeptAboveProgram and kKeptBelowTop keep) taken from malloc and given straight back, so the
    // heap's break is at its height from the start and sbrk is not called
    // again. The bytes.
    uint32_t reserveMallocArena(uint32_t top)
    {
        const uint32_t bytes = top - (sym::g_programEnd + kKeptAboveProgram) - kKeptBelowTop;
        const uint32_t arena = sce::libc::malloc(bytes);
        sce::libc::free(arena);
        return bytes;
    }
    READABLE_FUNCTION(reserveMallocArena, Verified);

    // initPlatform: the hardware, DMA, display and clock set up, once.
    // loadVu0Microprogram (ZGE4 only; initPlatform's last step): VU0's
    // microprogram sent to it through VIF0 (the chain's first VIF code is an
    // MPG; dmaSendChain's flags are those initRenderer sends VU1's with), and
    // a flag set that nothing in BMX XXX reads. 1.
    inline uint32_t loadVu0Microprogram()
    {
        constexpr ee::Global<uint32_t, sym::bmx::g_vu0MicroprogramLoaded> loaded;
        *loaded = 1;
        dmaSendChain(0, sym::bmx::vu0MicroprogramChain, 837, 0);
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, loadVu0Microprogram);

    uint32_t initPlatform()
    {
        if (*g_platformInitialized == 0)
        {
            *g_platformInitialized = 1;
            resetHardware();
            initDmaChannels();
            initDisplayState();
            initCpuCycles();
            if constexpr (game::readable::kIsBmx) // (ZGE4 loads VU0's microprogram here too)
                loadVu0Microprogram();
        }
        return 1;
    }
    READABLE_FUNCTION(initPlatform, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t sceOpen(uint32_t name, uint32_t mode) { return ee::game<int32_t>(sym::sceOpen, name, mode); }
        void sceClose(int32_t file) { ee::game(sym::sceClose, file); }
    }

    // A vsync's time: ZGE4 divides it out of the display's rate each time.
    inline ee::Float vsyncSeconds([[maybe_unused]] const DisplaySetup &display)
    {
        if constexpr (game::readable::kIsBmx)
            return ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(vsyncsPerSecond(display)));
        else
            return ee::Float(kVsyncSeconds);
    }

    // engineMain: the program. The versions printed, the platform and the
    // IOP started, the debug file opened when the console is off, the engine
    // and a 512 KB main region set up and the game started; then a frame at
    // a time until requestFrameWait asks to quit. The first two frames of a
    // level are a vsync long and marked shown, whatever they took.
    // ZGE4 prints a fourth line, the program's sizes, measures a vsync from
    // the display's rate (50 in PAL) where ZGE3 has 1/60 s, and its versions
    // are its own.
    uint32_t engineMain(uint32_t /*argumentCount*/, uint32_t arguments)
    {
        sce::libgcc::__main();
        debugWrite(formatScratch(kFormatCompiler, kCompilerMajor, kCompilerMinor));
        debugWrite(formatScratch(kFormatLibrary, kSceLibraryVersion));
        // The last version line passes the words the line before left after
        // its own three: strlen's two masks and strcpy's ones (the seventh
        // word is 0, as nothing before set it). The first two lines' extra
        // words land in the same stack slots, which this line's overwrite.
        debugWrite(formatScratch(kFormatEngine, kZgeMajor, kZgeMinor, kZgeBuild, kZeroByteTestOnes, kZeroByteTestHighBits,
                                 kZeroByteTestOnes));
        if constexpr (game::readable::kIsBmx)
        {
            debugWrite(formatScratch(kFormatProgramSize, static_cast<int32_t>(sym::g_programEnd - sym::start) / static_cast<int32_t>(kBytesAKilobyte),
                                     sym::start, kCodeKilobytes, kDataKilobytes, kClearedDataKilobytes, kMainStackKilobytes));
        }

        if (initPlatformSettings() == 0)
            return 0;
        reserveMallocArena(kMainMemoryBytes);
        if (initCdDevice() == 0)
            return 0;

        PlatformSettings &settings = *g_platformSettings;
        const uint32_t debugToFile = settings.consoleOff;
        settings.debugFile = -1;
        if (debugToFile != 0 && g_iopBoot->debugger == 0)
        {
            const uint32_t name = formatScratch(kFormatPath, kDevicePrefix, kDebugFileName);
            settings.debugFile = sceOpen(name, kSceOpenWriteOnly);
        }
        if (ps2SifInit() == 0)
            return 0;
        initPlatform();
        if (initEngine(0, arguments) != 0)
        {
            const uint32_t mainRegion = allocateAligned(kMainRegionAlignment, kMainRegionBytes);
            setMainRegion(mainRegion, kMainRegionBytes);
            if (gameStartup() != 0)
            {
                DisplaySetup &display = *g_display;
                int32_t lastVsyncs = 0;
                while (*g_quitRequested == 0)
                {
                    ee::Float seconds = *g_fixedFrameStep;
                    const int32_t vsyncsPassed = display.vsyncs - lastVsyncs;
                    if (seconds == ee::Float(0.0f))
                        seconds = ee::min(ee::toFloat(vsyncsPassed) * vsyncSeconds(display), ee::Float(kMaxFrameSeconds));
                    lastVsyncs = display.vsyncs;
                    if (static_cast<uint32_t>(display.levelFrame) < 2u)
                    {
                        seconds = vsyncSeconds(display);
                        display.framesPending = 1;
                    }
                    updateFrameRate(seconds);
                    renderCountedFrame();
                }
                gameShutdown();
            }
            heapFree(mainRegion);
            setMainRegion(0, 0);
            shutdownEngineSystems();
        }
        shutDownCpuCycles();
        const int32_t debugFile = settings.debugFile;
        if (debugFile >= 0)
        {
            sceClose(debugFile);
            settings.debugFile = -1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(engineMain, Verified);

    // assertFailed (its callers, two thunks, are dead): an
    // assertion failed. The pad recording closed, the screen cleared to
    // dark red for two frames (DM2 prints "***ASSERT***", the function, file,
    // line and reason first; this build prints nothing), then a hang
    // (hangOnAssert), or the debugger's trap and on with the game.
    void assertFailed(uint32_t why, uint32_t file, uint32_t line, uint32_t function)
    {
        if (*g_inAssert != 0)
            return;
        *g_inAssert = 1;
        g_display->framesPending = 0;
        closePadRecording();
        showAssertScreen(function, file, line, why, kAssertColour);
        closePadRecording();
        if (g_platformSettings->hangOnAssert != 0)
            ee::io::hang();
        // BREAK 0, 1: the debugger's trap (the original goes on after it).
        g_display->flags &= ~1u;
        resetCameraViewports(*g_screenWidth, *g_screenHeight);
        *g_inAssert = 0;
    }
    READABLE_FUNCTION_WAITS(assertFailed, Draft);
}
