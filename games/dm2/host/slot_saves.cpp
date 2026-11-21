// DM2's replays and parks as the host's native saves keep them: a file a
// slot (engine/host/save_files.h's SlotLibrary: <saves>/Replays/Replay NN.sav
// and <saves>/Parks/Park NN.sav, each the slot's record and data as the
// card's library has them, BMX2RPLY.DAT and BMX2PARK.DAT; the card folders'
// libraries, and a PCSX2 card's the app imported, are brought in once).
// The memory card frame stays the game's (action/card_frame.cpp) for what
// is not the card's: the slot list, the
// overwrite question, the name entry and the messages that say how it went.
// Its steps that are the card's are the host's (setUiFrameStep's host form):
//   - the card slot choice: there is no card to choose, so the frame opens on
//     its slot list (read from the files) and a message's way back goes to
//     it; backing out of the list closes the frame, as backing out of the
//     card slot choice does;
//   - saving: the slot's file written at once, and two frames later (the
//     frame's menu up) "Save complete." (or "Save failed."), as the game's
//     own save goes on;
//   - loading: the slot's file read into the request's data at once, and
//     half a second later (the list played out) the request told the work is
//     done, as the frame's finish does; "Load failed." when it cannot be read.
// The "Checking...", "Saving..." and "Loading..." steps, the format, the
// space check and the library's creation are not shown.

#include "saves.h"

#include "dm2/action/card_frame.h"
#include "dm2/action/ui_frame.h"
#include "dm2/game/memory_card.h"
#include "ee/ee.h"
#include "ee/host_form.h"
#include "host/kernel.h"
#include "host/save_files.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "sym/dm2/host/slot_saves.cpp.h"

namespace game::dm2::host
{
    namespace
    {
        namespace files = ::game::host::saves;
        namespace platform = ::game::host;

        bool slotUsed(const uint8_t *record)
        {
            uint32_t flags = 0;
            std::memcpy(&flags, record, sizeof(flags)); // a slot record's first word (ReplaySlot, ParkSlot)
            return (flags & kSlotUsed) != 0;
        }

        // The two libraries, by their card files (memory_card.h).
        const files::SlotLibrary kReplays{.folder = "Replays",
                                          .name = "Replay",
                                          .slots = kReplaySlots,
                                          .recordBytes = sizeof(ReplaySlot),
                                          .dataBytes = kReplayBytes,
                                          .cardFolder = "BASLUS-20159BMX2RPLY",
                                          .cardFile = "BMX2RPLY.DAT",
                                          .cardFileBytes = sizeof(ReplayLibraryFile),
                                          .used = &slotUsed,
                                          .log = "[dave-mirra] saves"};
        const files::SlotLibrary kParks{.folder = "Parks",
                                        .name = "Park",
                                        .slots = kParkSlots,
                                        .recordBytes = sizeof(ParkSlot),
                                        .dataBytes = kParkBytes,
                                        .cardFolder = "BASLUS-20159BMX2PARK",
                                        .cardFile = "BMX2PARK.DAT",
                                        .cardFileBytes = sizeof(ParkLibraryFile),
                                        .used = &slotUsed,
                                        .log = "[dave-mirra] saves"};

        // The frame kinds that work on a library, and each one's slot list.
        bool replayFrame(uint32_t kind) { return kind == kUiFrameSaveReplay || kind == kUiFrameLoadReplay; }
        bool parkFrame(uint32_t kind) { return kind == kUiFrameSavePark || kind == kUiFrameLoadPark; }
        uint32_t listStep(uint32_t kind)
        {
            switch (kind)
            {
            case kUiFrameSaveReplay:
                return kStepPickReplayToSave;
            case kUiFrameLoadReplay:
                return kStepPickReplayToLoad;
            case kUiFrameSavePark:
                return kStepPickParkToSave;
            default:
                return kStepPickParkToLoad;
            }
        }
        bool atList(uint32_t step)
        {
            return step == kStepPickReplayToSave || step == kStepPickReplayToLoad || step == kStepPickParkToSave
                   || step == kStepPickParkToLoad;
        }

        // The frame moved to a step, as the game moves it (the step left
        // played out, the new one shown).
        void showStep(uint32_t step) { platform::callGameFunction(sym::setUiFrameStep, {step}); }

        // What the frame's step change plays out when it leaves the name
        // entry or a load's slot list (unless the user backed out): the
        // frame's menu shown again ("PLAY FrameMenu.Show"). It is up a frame
        // later, and a step's message shows only once it is (setUiFrameStep),
        // so the step after the host's save or load comes later
        // (showStepLater), as the card's step that would stand between them.
        void playOutStepLeft()
        {
            if (*g_uiMessageType == kMessageCancel)
                return;
            uint32_t frame = 0;
            platform::callGameFunction(sym::findNode, {(*g_cardFrameUiRoot).address, sym::memCardFramePath, 0}, frame);
            if (frame)
                platform::callGameFunction(sym::runScript, {frame, sym::frameMenuShowScript});
        }

        // The library's slots into the list the frame shows (what the card's
        // file look-up reads): each slot's record, a slot with no file empty.
        template <size_t Slots, typename Record>
        void readList(const files::SlotLibrary &library, Record (&list)[Slots])
        {
            for (uint32_t slot = 0; slot < Slots; ++slot)
            {
                std::vector<uint8_t> file;
                if (files::readSlot(library, slot, file))
                    std::memcpy(&list[slot], file.data(), sizeof(Record));
                else
                    std::memset(&list[slot], 0, sizeof(Record));
            }
        }

        // A guest text into a record's field, ended and cut to the field.
        void copyText(char *field, size_t bytes, ee::Ptr<const char> text)
        {
            std::memset(field, 0, bytes);
            if (text)
                std::strncpy(field, text.get(), bytes - 1u);
        }

        // As the card layer's save (startCardSave): the request's slot marked
        // used and named (a park also by its author and the editor's current
        // theme), given the request's data. Its record is the slot's as it
        // was, when it had one.
        bool saveSlot(uint32_t kind)
        {
            const files::SlotLibrary &library = replayFrame(kind) ? kReplays : kParks;
            const CardRequest &request = *g_cardRequest;
            std::vector<uint8_t> file(library.recordBytes + library.dataBytes, 0u);
            files::readSlot(library, request.slot, file);
            if (replayFrame(kind))
            {
                ReplaySlot &slot = *reinterpret_cast<ReplaySlot *>(file.data());
                slot.flags |= kSlotUsed;
                copyText(slot.name, sizeof(slot.name), request.name);
            }
            else
            {
                ParkSlot &slot = *reinterpret_cast<ParkSlot *>(file.data());
                uint32_t theme = 0;
                platform::callGameFunction(sym::currentThemeName, {}, theme);
                slot.flags |= kSlotUsed;
                copyText(slot.name, sizeof(slot.name), request.name);
                copyText(slot.author, sizeof(slot.author), request.author);
                copyText(slot.theme, sizeof(slot.theme), ee::Ptr<const char>(theme));
            }
            std::memcpy(file.data() + library.recordBytes, ee::at<uint8_t>(g_uiFrameRequest->data), library.dataBytes);
            return files::writeSlot(library, request.slot, file);
        }

        // As the card layer's load: the request's slot's data into the
        // request's data.
        bool loadSlot(uint32_t kind)
        {
            const files::SlotLibrary &library = replayFrame(kind) ? kReplays : kParks;
            std::vector<uint8_t> file;
            if (!files::readSlot(library, g_cardRequest->slot, file))
                return false;
            std::memcpy(ee::at<uint8_t>(g_uiFrameRequest->data), file.data() + library.recordBytes, library.dataBytes);
            return true;
        }
    }

    // setUiFrameStepOnHost: a replay's or a park's frame asked for a step
    // that is the card's: the slot list for the card slot choice (or the
    // frame closed when the list is left), the save or load done at once and
    // the step that follows. Every other step, and every other frame's, the
    // game's.
    void setUiFrameStepOnHost(uint32_t step)
    {
        const uint32_t kind = *g_uiFrameKind;
        if (!replayFrame(kind) && !parkFrame(kind))
            return platform::continueInPs2Form(sym::setUiFrameStep);
        const UiFrameRequest &request = *g_uiFrameRequest;
        switch (step)
        {
        case kStepChooseSaveSlot:
        case kStepChooseLoadSlot:
            if (atList(*g_uiFrameStep))
            {
                // Backing out of the list: as backing out of the card slot choice.
                if (request.onClosed)
                    platform::callGameFunction(request.onClosed.address, {request.data, request.dataHandle});
                return showStep(kStepOver);
            }
            if (replayFrame(kind))
                readList(kReplays, *g_replaySlots);
            else
                readList(kParks, *g_parkSlots);
            return showStep(listStep(kind));
        case kStepSaving:
        {
            const bool saved = saveSlot(kind);
            playOutStepLeft();
            *g_uiFrameStep = kStepSaving;
            if (saved && request.onDone)
                return tellRequestLater(true);
            return showStepLater(saved ? kStepSaveComplete : kStepSaveFailed);
        }
        case kStepLoading:
        {
            // (a load that tells onDone ends the frame: its menu, which the
            // card's step would have shown and the frame's finish taken away,
            // is not shown)
            const bool loaded = loadSlot(kind);
            if (loaded && request.onDone)
            {
                *g_uiFrameStep = kStepLoading;
                return tellRequestLater(true);
            }
            playOutStepLeft();
            *g_uiFrameStep = kStepLoading;
            return showStepLater(loaded ? kStepLoadComplete : kStepLoadFailed);
        }
        default:
            return platform::continueInPs2Form(sym::setUiFrameStep);
        }
    }

    HOST_FORM(saves, sym::setUiFrameStep, setUiFrameStepOnHost);
}
