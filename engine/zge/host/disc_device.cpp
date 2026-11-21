// The engine's file device on the disc ("dvd0", g_dvdDeviceTable) as the
// host runs it: the game's files read straight from the host (engine/host/
// disc_files.h: the Mods folder over the game's own files), in place of the
// CD/DVD client's RPCs to ZCDDVD.IRX on the IOP (zge/assets/dvd_device.cpp,
// the PS2 form). The engine keeps a platform's code in its own folder (ZGE's
// ps2/ beside XBox/, where the Xbox build's file device is); this is the
// host's. [host] disc = native, the default, installs these over the PS2
// forms (ee/host_form.h); = ps2 (ZX_DISC=ps2) runs
// the PS2 forms over the emulated IOP, and ZCDDVD.IRX with them.
//
// They keep the device's contract as its callers (zfile) see it:
//   - the file record: its handle (0: not opened) and the device; the read
//     position is the host's, per handle (zfile keeps its own too);
//   - the results: open 1 or 0, read the bytes read (clamped at the end of
//     the file; -1 for a file not open), seek 1 unless the position is not
//     one, the position and size written to the caller's word and 1 when
//     the file is not empty, stat's 1 or 0 in its result word;
//   - a completion block, when the caller passes one (zfile's request
//     worker: BMX XXX's loads do, AI's have not been seen to), told of the
//     operation after its return as the client's RPC end functions tell it:
//     {its argument, the file or the value, succeeded, the value}.
// The init keeps what other code uses of the client: the lock (the movie
// stream code waits on it), the read semaphore, the completion server's
// thread at priority 1 (it only sleeps here, so the thread and semaphore ids
// are the PS2 form's) and the caller's drop to priority 2. The device is
// mounted from the start; nothing is asked of the IOP.
//
// Reads are instant. On the console a read blocks its caller while the
// other threads run (a movie's demux thread during a load, a lower priority
// thread during the stream thread's priority-1 read); so an open or a read
// without a completion block waits as for the RPC's reply when another
// thread is ready (game::host::waitAsForReply), its data already in place.

#include "host/disc_files.h"
#include "ee/host_form.h"
#include "zge/assets/dvd_client.h"
#include "zge/assets/file.h"

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/host/disc_device.cpp.h"

namespace zge::host
{
    namespace
    {
        namespace files = ::game::host::disc;
        namespace platform = ::game::host;

        constexpr uint32_t kDvdDevice = sym::g_dvdDeviceTable; // the FileDevice
        constexpr uint32_t kDvdServerStack = g_dvd.address + offsetof(DvdClient, serverStack);

        // A result word, when the caller gave one.
        void setResult(ee::Ptr<uint32_t> result, uint32_t value)
        {
            if (result)
                *result = value;
        }

        // The completion block's callback, told after the return.
        void tell(ee::Ptr<FileCallback> callback, uint32_t subject, bool succeeded, uint32_t value)
        {
            platform::reportToCallback(callback.address, subject, succeeded, value);
        }
    }

    // dvdInitOnHost: the device up, with the same kernel objects in the same order
    // as the PS2 form makes them: the read semaphore (0 of 1), the lock (1 of
    // 1), the completion server's thread; then this thread at the client's
    // priority, which lets the server's thread run, and the device mounted.
    uint32_t dvdInitOnHost()
    {
        files::prepare();
        g_dvd->initialised = 1;
        g_dvd->readSema = static_cast<uint32_t>(platform::createSemaphore(0, 1));
        g_dvd->lock = static_cast<uint32_t>(platform::createSemaphore(1, 1));
        g_dvd->lockCount = 1;
        const int server = platform::createThread(sym::dvdCompletionServer, kDvdServerStack, kDvdServerStackBytes,
                                                  sym::gp, kDvdServerPriority);
        g_dvd->serverThread = static_cast<uint32_t>(server);
        platform::changeOwnPriority(kDvdClientPriority);
        g_dvd->resultCallback = 0;
        g_dvd->bufferValid = 0;
        g_dvd->bufferFile = 0;
        g_dvd->bufferPosition = 0;
        g_dvd->resultOut = 0;
        g_dvd->mounted = 1;
        platform::startThread(server);
        platform::letReadyThreadRun();
        return 1;
    }

    // dvdCompletionServerOnHost: nothing completes through the IOP: the thread
    // sleeps for good (a wakeup runs this again).
    void dvdCompletionServerOnHost()
    {
        platform::sleepInto(sym::dvdCompletionServer);
    }

    // dvdMountOnHost: mounted.
    uint32_t dvdMountOnHost(uint32_t, uint32_t, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        g_dvd->mounted = 1;
        setResult(result, 1);
        if (callback)
            tell(callback, kDvdDevice, true, 1);
        return 1;
    }

    // dvdStatOnHost: does the file exist.
    uint32_t dvdStatOnHost(uint32_t, ee::Ptr<const char> path, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        const bool found = files::exists(files::guestPath(path.address));
        setResult(result, found ? 1 : 0);
        if (callback)
            tell(callback, kDvdDevice, found, found ? 1 : 0);
        return 1;
    }

    // dvdOpenOnHost: in any mode (the files are only read; the PS2 form reports a
    // mode other than read and opens anyway).
    uint32_t dvdOpenOnHost(ee::Ptr<ZFile> file, ee::Ptr<const char> path, uint32_t, ee::Ptr<FileCallback> callback)
    {
        const uint32_t handle = files::open(files::guestPath(path.address));
        file->handle = handle;
        if (handle != 0 || callback)
            file->device = kDvdDevice;
        if (callback)
        {
            tell(callback, file.address, handle != 0, handle != 0 ? 1 : 0);
            return 1;
        }
        platform::waitAsForReply();
        return handle != 0 ? 1 : 0;
    }

    // dvdCloseOnHost
    int32_t dvdCloseOnHost(ee::Ptr<ZFile> file, ee::Ptr<FileCallback> callback)
    {
        const bool closed = files::close(file->handle);
        if (callback)
            tell(callback, kDvdDevice, closed, closed ? 1 : 0);
        return closed ? 1 : 0;
    }

    // dvdReadOnHost: from the file's position, clipped to its end: the bytes.
    int32_t dvdReadOnHost(ee::Ptr<ZFile> file, uint32_t destination, uint32_t size, ee::Ptr<FileCallback> callback)
    {
        const uint32_t handle = file->handle;
        if (!files::isOpen(handle))
            return -1;
        const uint32_t bytes = files::read(handle, destination, size);
        if (callback)
        {
            tell(callback, file.address, true, bytes);
            return static_cast<int32_t>(bytes);
        }
        platform::waitAsForReply();
        return static_cast<int32_t>(bytes);
    }

    // dvdSeekOnHost: 1 if the position is one.
    int32_t dvdSeekOnHost(ee::Ptr<ZFile> file, int32_t offset, uint32_t origin, ee::Ptr<FileCallback> callback)
    {
        const bool moved = files::seek(file->handle, offset, origin);
        if (callback)
            tell(callback, file.address, moved, moved ? 1 : 0);
        return moved ? 1 : 0;
    }

    // dvdFilePositionOnHost: the position, to the caller's word or its callback;
    // 1 when the file is not empty.
    int32_t dvdFilePositionOnHost(ee::Ptr<ZFile> file, ee::Ptr<uint32_t> positionOut, ee::Ptr<FileCallback> callback)
    {
        const uint32_t handle = file->handle;
        const uint32_t position = files::position(handle);
        const bool notEmpty = files::size(handle) != 0;
        if (callback)
            tell(callback, position, notEmpty, position);
        else
            setResult(positionOut, position);
        return notEmpty ? 1 : 0;
    }

    // dvdFileSizeOnHost: the size, to the caller's word or its callback; 1 when it
    // is not 0.
    int32_t dvdFileSizeOnHost(ee::Ptr<ZFile> file, ee::Ptr<uint32_t> sizeOut, ee::Ptr<FileCallback> callback)
    {
        const uint32_t bytes = files::size(file->handle);
        if (callback)
            tell(callback, bytes, bytes != 0, bytes);
        else
            setResult(sizeOut, bytes);
        return bytes != 0 ? 1 : 0;
    }

    HOST_FORM(disc, sym::dvdInit, dvdInitOnHost);
    HOST_FORM(disc, sym::dvdCompletionServer, dvdCompletionServerOnHost);
    HOST_FORM(disc, sym::dvdMount, dvdMountOnHost);
    HOST_FORM(disc, sym::dvdStat, dvdStatOnHost);
    HOST_FORM(disc, sym::dvdOpen, dvdOpenOnHost);
    HOST_FORM(disc, sym::dvdClose, dvdCloseOnHost);
    HOST_FORM(disc, sym::dvdRead, dvdReadOnHost);
    HOST_FORM(disc, sym::dvdSeek, dvdSeekOnHost);
    HOST_FORM(disc, sym::dvdFilePosition, dvdFilePositionOnHost);
    HOST_FORM(disc, sym::dvdFileSize, dvdFileSizeOnHost);
}
