// The engine's save jobs (zge/platform/save_system.h) as the host runs them:
// the saves are plain files (engine/host/save_files.h), in place of the memory
// card's jobs (zge/platform/memory_card/save_jobs.cpp, the PS2 form: the slot asked
// for, the card checked, the console's dialogs, libmc's commands). The
// native saves, the default, install these over the PS2 forms
// (ee/host_form.h); ZX_SAVES=card runs the PS2 forms on the emulated memory
// card, as on the console.
//
// They keep the jobs' contract as the save system and its client see it:
//   - the client's prepare has filled the save's buffer (a save) or made the
//     one a load fills before the job starts;
//   - the job talks to the client through the two queues saveSystemTask
//     hands on each frame: the lines it shows (kMessageShowText), a list of
//     saves (kMessageSaveFound, kMessageFileMenu) and the player's answers
//     (kMessageAnswer, kMessageFileChosen);
//   - it ends with a message of how it went (kMessageDone, kMessageCancelled,
//     kMessageFailed), then no job thread for saveSystemTask to wake and the
//     thread deleted, as the PS2 form's end (endSaveJob).
//
// What the player sees: no card dialogs. The boot's check for room and its
// quiet load take no time (and the quiet load opens no save screen: each
// game's host form of its screen). A save and a load from the menus show the
// game's screen with one line ("Game saved.") for under a second: no slot
// select, no "Are you sure", no overwrite question. The list of parks and the
// park name keyboard are the game's.
//
// A job that waits (a frame, the time a line stays up, the player's pick from
// the list) sleeps and runs again from its entry when saveSystemTask wakes
// it; where it got to is kept between its runs (save_files.h's Job).

#include "host/kernel.h"
#include "host/save_files.h"
#include "ee/host_form.h"
#include "zge/platform/save_system.h"

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "sym/zge/host/save_jobs.cpp.h"

namespace zge::host
{
    namespace
    {
        namespace files = ::game::host::saves;
        namespace platform = ::game::host;

        // The message line's text, where formatLocalizedText puts it (255
        // characters and the end).
        constexpr uint32_t kMessageText = sym::g_localizedTextScratch;
        constexpr size_t kMessageTextMax = 255u;
        // A listed park's name, below the job's stack (63 characters and the
        // end): the host form keeps nothing on the guest stack.
        constexpr uint32_t kNameBelowStack = 0x100u;
        constexpr size_t kNameMax = 63u;

        // Vsyncs from the job's start until the screen closes. Its opening
        // animation takes ~30 (the line shows when it ends), so a result line
        // is up for ~0.75 s, a failure's for ~2 s.
        constexpr uint64_t kShowDone = 75u, kShowFailed = 150u;

        enum JobKind
        {
            kAutoLoad,
            kSave,
            kLoad,
            kSilentLoad,
            kSaveChoice,
            kLoadChoice,
        };

        // What a step says the job does next: sleep until woken, or the next
        // step now. Any other is its end, the message kind that says how it
        // went.
        constexpr int kWait = -1, kAgain = -2;

        // A choice job's steps: the list started, each park on it, the list
        // shown, the player's pick, what the job does with it; then a line's
        // time up, whichever job showed it.
        enum Step
        {
            kStart = 0,
            kListNext,
            kListShow,
            kListWait,
            kAct,
            kFinal,
        };

        // ---- the queues -----------------------------------------------------

        SaveMessage &messageAt(ee::Ptr<SaveMessageQueue> queue, uint32_t index)
        {
            return ee::Ptr<SaveMessage>(&queue->first)[index];
        }

        // A message on the queue, in the first free place from the oldest on:
        // where it went (0: the queue is full, or there is none).
        uint32_t post(ee::Ptr<SaveMessageQueue> queue, int32_t kind, uint32_t value = 0, uint32_t listBuffer = 0)
        {
            if (!queue)
                return 0;
            const uint32_t limit = queue->limit;
            uint32_t index = queue->oldest;
            for (uint32_t n = 0; n < limit && index < limit; ++n)
            {
                SaveMessage &place = messageAt(queue, index);
                if (place.kind == 0)
                {
                    place.value = value;
                    place.listBuffer = listBuffer;
                    place.choice = 0;
                    place.kind = kind;
                    return ee::addressOf(&place);
                }
                index = index + 1 >= limit ? 0 : index + 1;
            }
            return 0;
        }

        // The oldest message taken off the queue, if there is one.
        bool take(ee::Ptr<SaveMessageQueue> queue, SaveMessage &message)
        {
            if (!queue)
                return false;
            const uint32_t limit = queue->limit;
            const uint32_t index = queue->oldest;
            if (index >= limit)
                return false;
            SaveMessage &oldest = messageAt(queue, index);
            if (oldest.kind == 0)
                return false;
            message = oldest;
            oldest.kind = 0;
            queue->oldest = index + 1 >= limit ? 0 : index + 1;
            return true;
        }

        uint32_t tell(int32_t kind, uint32_t value = 0, uint32_t listBuffer = 0)
        {
            return post(*g_saveMessageQueue, kind, value, listBuffer);
        }

        bool takeReply(SaveMessage &reply) { return take(*g_saveReplyQueue, reply); }

        // ---- text -----------------------------------------------------------

        void writeText(uint32_t address, const std::string &text, size_t max)
        {
            const size_t length = text.size() < max ? text.size() : max;
            char *out = ee::at<char>(address);
            std::memcpy(out, text.data(), length);
            out[length] = '\0';
        }

        std::string readText(ee::Ptr<const char> text)
        {
            constexpr size_t kLimit = 256u;
            std::string out;
            for (size_t i = 0; text && i < kLimit && text[i] != '\0'; ++i)
                out.push_back(text[i]);
            return out;
        }

        // The client's message line.
        void showLine(const char *text)
        {
            writeText(kMessageText, text, kMessageTextMax);
            tell(kMessageShowText, kMessageText);
        }

        // ---- where the saves are --------------------------------------------

        // The game save's buffer, as the client's prepare made it (0: none).
        uint32_t gameSaveBuffer() { return *ee::at<uint32_t>(files::game().gameSaveBuffer); }

        // The park editor's buffer, as the games keep it: its bytes and where
        // they are.
        struct ParkBuffer
        {
            uint32_t bytes;
            ee::Ptr<uint8_t> data;
            uint32_t client;
        };
        uint32_t parkData()
        {
            const ee::Ptr<ParkBuffer> buffer = *ee::at<ee::Ptr<ParkBuffer>>(files::game().parkBuffer);
            return buffer ? buffer->data.address : 0u;
        }

        // ---- the steps -------------------------------------------------------

        // The job ends with `result` once its line has been up `vsyncs`.
        int showFor(files::Job &job, uint64_t vsyncs, int32_t result)
        {
            job.result = result;
            job.until = platform::vsyncNow() + vsyncs;
            job.step = kFinal;
            return kAgain;
        }

        // The boot's check that a save has somewhere to go: it has.
        int autoLoad() { return kMessageDone; }

        // The boot's quiet load: no save is not a failure.
        int silentLoad()
        {
            const files::Loaded how = files::loadGameSaveAtBoot(gameSaveBuffer());
            files::bootLoadDone();
            return files::loaded(how) ? kMessageDone : how == files::Loaded::NoSave ? kMessageCancelled : kMessageFailed;
        }

        int save(files::Job &job)
        {
            const bool saved = files::writeGameSave(gameSaveBuffer(), "save");
            showLine(saved ? "Game saved." : "The game could not be saved.");
            return showFor(job, saved ? kShowDone : kShowFailed, saved ? kMessageDone : kMessageFailed);
        }

        int load(files::Job &job)
        {
            const files::Loaded how = files::loadGameSave(gameSaveBuffer());
            if (files::loaded(how))
            {
                showLine("Game loaded.");
                return showFor(job, kShowDone, kMessageDone);
            }
            if (how == files::Loaded::NoSave)
            {
                showLine("There is no saved game yet.");
                return showFor(job, kShowFailed, kMessageCancelled);
            }
            showLine("The saved game is damaged and could not be loaded.");
            return showFor(job, kShowFailed, kMessageFailed);
        }

        // The list both choice jobs show: emptied, every park added (one a
        // frame: each takes the list buffer and the name's place until the
        // client has it), shown, and the player's pick waited for.
        int listParks(files::Job &job)
        {
            switch (job.step)
            {
            case kStart:
                job.parks = files::parks();
                job.next = 0;
                job.pendingMessage = tell(kMessageSaveFound); // no name: the list starts again
                if (!job.pendingMessage)
                    return kWait;
                job.step = kListNext;
                return kWait;
            case kListNext:
            {
                if (job.pendingMessage && ee::Ptr<SaveMessage>(job.pendingMessage)->kind != 0)
                    return kWait; // the client has not had the last one yet
                job.pendingMessage = 0;
                if (job.next >= job.parks.size())
                {
                    job.step = kListShow;
                    return kAgain;
                }
                const int number = job.parks[job.next];
                const uint32_t listBuffer = *g_saveListBuffer;
                const uint32_t name = platform::belowStack(kNameBelowStack);
                if (listBuffer && files::readParkHeader(number, listBuffer, (*g_saveClient)->listBufferBytes))
                {
                    writeText(name, files::parkName(number), kNameMax);
                    job.pendingMessage = tell(kMessageSaveFound, name, listBuffer);
                    if (!job.pendingMessage)
                        return kWait; // the queue is full: again next frame
                }
                ++job.next;
                return kWait;
            }
            case kListShow:
            {
                SaveMessage stale;
                while (takeReply(stale))
                {
                }
                if (!tell(kMessageFileMenu, 1))
                    return kWait;
                job.answered = false;
                job.chosenName.clear();
                job.step = kListWait;
                return kWait;
            }
            case kListWait:
            {
                SaveMessage reply;
                while (takeReply(reply))
                {
                    if (reply.kind == kMessageFileChosen)
                        job.chosenName = readText(ee::Ptr<const char>(reply.value));
                    else if (reply.kind == kMessageAnswer)
                    {
                        job.choice = static_cast<int32_t>(reply.value);
                        job.answered = true;
                    }
                }
                if (!job.answered)
                    return kWait;
                job.step = kAct;
                return kAgain;
            }
            default:
                return kAgain;
            }
        }

        // A park saved over the one picked, or as a new one ("New": the first
        // free number).
        int saveChoice(files::Job &job)
        {
            if (job.step != kAct)
                return listParks(job);
            if (job.choice < 0)
                return kMessageCancelled;
            int number = files::parkNumber(job.chosenName);
            if (number < 0)
                number = files::firstFreePark();
            const bool saved = files::writePark(number, parkData());
            showLine(saved                        ? "Park saved."
                     : number >= files::kNoRoom ? "There is no room for another park."
                                                : "The park could not be saved.");
            return showFor(job, saved ? kShowDone : kShowFailed, saved ? kMessageDone : kMessageFailed);
        }

        // The park picked, loaded (a line only when there is none or it fails).
        int loadChoice(files::Job &job)
        {
            if (job.step == kListShow && job.parks.empty())
            {
                showLine("There are no saved parks yet.");
                return showFor(job, kShowFailed, kMessageCancelled);
            }
            if (job.step != kAct)
                return listParks(job);
            if (job.choice < 0)
                return kMessageCancelled;
            if (files::loadPark(job.chosenName, parkData()))
                return kMessageDone;
            showLine("The park is damaged and could not be loaded.");
            return showFor(job, kShowFailed, kMessageFailed);
        }

        int step(files::Job &job)
        {
            if (job.step == kFinal)
                return job.result;
            switch (job.kind)
            {
            case kAutoLoad:
                return autoLoad();
            case kSilentLoad:
                return silentLoad();
            case kSave:
                return save(job);
            case kLoad:
                return load(job);
            case kSaveChoice:
                return saveChoice(job);
            case kLoadChoice:
                return loadChoice(job);
            }
            return kMessageFailed;
        }

        // A job's run: from where it got to until it waits (it sleeps, and
        // runs again from `entry` when woken) or ends.
        void runJob(JobKind kind, uint32_t entry)
        {
            files::Job &job = files::job();
            const int thread = platform::currentThread();
            if (!job.running || job.thread != thread || job.kind != kind)
            {
                job = files::Job{};
                job.running = true;
                job.thread = thread;
                job.kind = kind;
                files::importFromCards();
            }
            for (;;)
            {
                if (job.until != 0u && platform::vsyncNow() < job.until)
                    return platform::sleepInto(entry);
                job.until = 0;
                const int next = step(job);
                if (next == kAgain)
                    continue;
                if (next == kWait)
                    return platform::sleepInto(entry);
                job.result = next;
                job.step = kFinal;
                if (!tell(next))
                    return platform::sleepInto(entry); // the queue is full: again next frame
                *g_jobThread = 0;
                job.running = false;
                return platform::endThread();
            }
        }
    }

    // The jobs (save_system.h), each a run of runJob from its own entry.
    void autoLoadJobOnHost() { runJob(kAutoLoad, sym::autoLoadJob); }
    void saveJobOnHost() { runJob(kSave, sym::saveJob); }
    void loadJobOnHost() { runJob(kLoad, sym::loadJob); }
    void silentLoadJobOnHost() { runJob(kSilentLoad, sym::silentLoadJob); }
    void saveChoiceJobOnHost() { runJob(kSaveChoice, sym::saveChoiceJob); }
    void loadChoiceJobOnHost() { runJob(kLoadChoice, sym::loadChoiceJob); }

    HOST_FORM(saves, sym::autoLoadJob, autoLoadJobOnHost);
    HOST_FORM(saves, sym::saveJob, saveJobOnHost);
    HOST_FORM(saves, sym::loadJob, loadJobOnHost);
    HOST_FORM(saves, sym::silentLoadJob, silentLoadJobOnHost);
    HOST_FORM(saves, sym::saveChoiceJob, saveChoiceJobOnHost);
    HOST_FORM(saves, sym::loadChoiceJob, loadChoiceJobOnHost);
}
