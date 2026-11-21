// The platform's file devices (zge/assets/file.h): set up and listed for
// the file system ("dvd0", "pcfs", "mc00"), and shut down.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include <cstddef>
#include <cstdint>
#include "zge/assets/file.h"
#include "zge/platform/memory_card/memory_card.h"
#include "sym/zge/assets/file_devices.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_fileDevicesReady> g_fileDevicesReady;
        constexpr uint32_t kDvdInit = sym::dvdInit; // dvdInit (readable: dvdInit, Tested; hooked by a game override: called by address)
        inline ee::Global<int32_t, sym::g_dvdDeviceReady> g_dvdDeviceReady;
    }

    namespace
    {
        namespace routed
        {
        }
    }

    // G_fileDevicesReady = 1.
    uint32_t initFileDevicesFlag()
    {
        *g_fileDevicesReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initFileDevicesFlag, Draft);

    // G_fileDevicesReady cleared, 1.
    uint32_t shutdownFileDevicesFlag()
    {
        if (*g_fileDevicesReady == 0)
            return 1;
        *g_fileDevicesReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownFileDevicesFlag, Draft);

    uint32_t initDvdDevice();

    uint32_t initDvdDevice()
    {
        *g_dvdDeviceReady = 1;
        zge::initDvdRequestPool();
        return 1;
    }
    READABLE_FUNCTION_WAITS(initDvdDevice, Verified);

    // Once set up (g_dvdDeviceReady): its request pool destroyed (shutdownDvdRequestPool)
    uint32_t shutdownDvdDevice()
    {
        if (*g_dvdDeviceReady != 0)
        {
            zge::shutdownDvdRequestPool();
            *g_dvdDeviceReady = 0;
            return 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownDvdDevice, Draft);

    // registerFileDevices: the platform's devices that start, listed for the
    // file system: the DVD reader's ("dvd0"), the disc through the IOP's
    // file system ("pcfs", once its file pool is made: initDvdDevice) and
    // the memory card's ("mc00"). 1.
    uint32_t registerFileDevices(ee::Ptr<FileDeviceList> list)
    {
        list->count = 0;
        if (dvdInit() == 1)
            list->devices[list->count++] = sym::g_dvdDeviceTable;
        if (initDvdDevice() == 1)
            list->devices[list->count++] = sym::g_fileDeviceMethods;
        if (zge::initMemoryCardDevice() == 1)
            list->devices[list->count++] = sym::deviceTable;
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerFileDevices, Verified);


    // shutDownFileDevices: registerFileDevices undone: the platform's devices
    // shut down (the DVD reader; the others keep nothing). 1 if the reader
    // was up.
    uint32_t shutDownFileDevices()
    {
        return zge::clearDvdBusy() != 0 ? 1 : 0;
    }
    READABLE_FUNCTION(shutDownFileDevices, Verified);

}
