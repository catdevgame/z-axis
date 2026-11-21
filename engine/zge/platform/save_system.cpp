// The save system (zsaveapi.c, from selectSaveClient on): the games' save clients
// on a list, the engine task that serves the message queue the memory card job
// talks to, and the calls that start a job on a client. A save's whole
// conversation: the job posts on g_saveMessageQueue and this task calls the
// client's interface; the interface answers on g_saveReplyQueue.

#include "save_system.h"

#include "zge/container/array.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "ee/function_ptr.h"
#include "zge/platform/tasks.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/platform/save_system.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kSaveTaskPriority = 7;
        constexpr int32_t kSourceLine = game::readable::perGame(195, 195, 211);
        constexpr uint32_t kQueueMessages = 20;

        // A registered client: the list's node, its id and the game's table.
        struct SaveClientRecord
        {
            ee::Ptr<SaveClientRecord> next; // the list's links (zge/container/list.h: initList, listPush)
            ee::Ptr<SaveClientRecord> prev;
            int32_t id;
            SaveClient client;
        };
        static_assert(sizeof(SaveClientRecord) == 12 + sizeof(SaveClient) && offsetof(SaveClientRecord, client) == 12);

        constexpr uint32_t kSaveClients = sym::g_saveClients; // the list
        inline ee::Global<ee::Ptr<SaveClientRecord>, kSaveClients> g_firstSaveClient;
        inline ee::Global<int32_t, sym::g_nextSaveClientId> g_nextSaveClientId;
        inline ee::Global<int32_t, sym::g_saveJobsReady> g_saveJobsReady;
        inline ee::Global<int32_t, sym::g_saveClientListUnset> g_saveClientListUnset;
        inline ee::Global<ee::Ptr<Task>, sym::g_saveSystemTask> g_saveSystemTask;

        // The game functions below call: ee/plain.h.

        // A reply of a kind and a value (its last two words left as the
        // stack had them).
        int32_t postReply(int32_t kind, uint32_t value)
        {
            ee::Local<SaveMessage> reply;
            const ee::Ptr<SaveMessageQueue> replies = *g_saveReplyQueue;
            reply->kind = kind;
            reply->value = value;
            return static_cast<int32_t>(postSaveMessage(replies, reply));
        }
    }

    // selectSaveClient: the client of the id becomes the selected one and its
    // prepare runs.
    void selectSaveClient(uint32_t id)
    {
        for (ee::Ptr<SaveClientRecord> record = *g_firstSaveClient; record; record = record->next)
        {
            if (static_cast<uint32_t>(record->id) == id)
            {
                *g_saveClient = ee::Ptr<SaveClient>(&record->client);
                const auto prepare = record->client.prepare;
                if (prepare)
                    prepare();
                return;
            }
        }
    }
    READABLE_FUNCTION(selectSaveClient, Verified);

    // registerSaveClient: a copy of the client's table on the list; its id.
    uint32_t registerSaveClient(ee::Address client)
    {
        if (*g_saveClientListUnset != 0)
        {
            initList(kSaveClients);
            *g_saveClientListUnset = 0;
        }
        const ee::Ptr<SaveClientRecord> record(heapAllocate(sizeof(SaveClientRecord)));
        const int32_t id = *g_nextSaveClientId;
        record->id = id;
        *g_nextSaveClientId = id + 1;
        memoryCopy(ee::Ptr<SaveClient>(&record->client), client, sizeof(SaveClient));
        listPush(kSaveClients, record);
        return record->id;
    }
    READABLE_FUNCTION_WAITS(registerSaveClient, Verified);

    // forEachSaveClient: callback(client, value) over the clients until one returns 0.
    uint32_t forEachSaveClient(SaveClientVisit callback, ee::Address value)
    {
        for (ee::Ptr<SaveClientRecord> record = *g_firstSaveClient; record; record = record->next)
        {
            if (callback(ee::Ptr<SaveClient>(&record->client), value) == 0)
                break;
        }
        return 1;
    }
    READABLE_FUNCTION(forEachSaveClient, Verified); // its two callbacks read their two arguments only

    // answerSaveMenu, cancelSaveMenu, answerSaveFileChoice: what the player said.
    int32_t answerSaveMenu(uint32_t answer)
    {
        return postReply(kMessageAnswer, answer);
    }
    READABLE_FUNCTION(answerSaveMenu, Verified);

    int32_t cancelSaveMenu()
    {
        return postReply(kMessageAnswer, static_cast<uint32_t>(-1));
    }
    READABLE_FUNCTION(cancelSaveMenu, Verified);

    // ZGE4 copies the file's name into the save system's own buffer and posts that (ZGE3 posts the caller's).
    namespace
    {
        constexpr uint32_t kChosenFileName = sym::bmx::g_chosenSaveFileName;
    }
    int32_t answerSaveFileChoice(ee::Address name)
    {
        if constexpr (game::readable::kIsBmx)
        {
            stringCopy(kChosenFileName, name);
            return postReply(kMessageFileChosen, kChosenFileName);
        }
        return postReply(kMessageFileChosen, name);
    }
    READABLE_FUNCTION(answerSaveFileChoice, Verified);

    ee::Ptr<SaveMessageQueue> newSaveMessageQueue(uint32_t limit);

    // startSaveSystem
    int32_t startSaveSystem()
    {
        *g_saveMessageQueue = newSaveMessageQueue(kQueueMessages);
        *g_saveReplyQueue = newSaveMessageQueue(kQueueMessages);
        *g_saveSystemTask = newTask(kSaveTaskPriority, sym::saveSystemTask, 0, 1, sym::sourceFile3, kSourceLine);
        const int32_t ready = static_cast<int32_t>(initSaveJobs());
        *g_saveJobsReady = ready;
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 undoes the start when the card's saves did not start: the
            // task deleted and both queues freed.
            if (ready == 0)
            {
                deleteTask(*g_saveSystemTask);
                if (const ee::Ptr<SaveMessageQueue> messages = *g_saveMessageQueue)
                {
                    heapFree(messages);
                    *g_saveMessageQueue = ee::Ptr<SaveMessageQueue>();
                }
                if (const ee::Ptr<SaveMessageQueue> replies = *g_saveReplyQueue)
                {
                    heapFree(replies);
                    *g_saveReplyQueue = ee::Ptr<SaveMessageQueue>();
                }
            }
        }
        return ready;
    }
    READABLE_FUNCTION_WAITS(startSaveSystem, Verified);

    // stopSaveSystem
    int32_t stopSaveSystem()
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4 stops only a started save system)
            if (*g_saveJobsReady == 0)
                return 0;
        freeSaveBuffer();
        int32_t result = deleteTask(*g_saveSystemTask);
        const ee::Ptr<SaveMessageQueue> messages = *g_saveMessageQueue;
        if (messages)
        {
            result = heapFree(messages);
            *g_saveMessageQueue = ee::Ptr<SaveMessageQueue>();
        }
        const ee::Ptr<SaveMessageQueue> replies = *g_saveReplyQueue;
        if (replies)
        {
            result = heapFree(replies);
            *g_saveReplyQueue = ee::Ptr<SaveMessageQueue>();
        }
        *g_saveJobsReady = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(stopSaveSystem, Verified);

    // saveSystemTask: the frame's task: wake the job, then every message it has
    // left goes to the client's interface. Any kind but the ones below is the
    // job's end: the client's `finished` runs, and the task leaves the rest.
    uint32_t saveSystemTask()
    {
        ee::Local<SaveMessage> message;
        wakeSaveJob();
        while (takeSaveMessage(*g_saveMessageQueue, message, 0) != 0)
        {
            const ee::Ptr<SaveClient> client = *g_saveClient;
            // ZGE4's kMessageShowChoiceNoCancel is kMessageShowChoice with showChoice's third word 0 (ZGE3 has no
            // such kind: its 13 is kMessage13).
            const int32_t kind = game::readable::kIsBmx && message->kind == kMessageShowChoiceNoCancel ? kMessageShowChoice : message->kind;
            switch (kind)
            {
            case kMessageShowChoice:
                if (client && client->showChoice)
                    client->showChoice(message->value, message->choice READABLE_PARAM_ONLY(bmx, message->kind == kMessageShowChoice ? 1u : 0u));
                break;
            case kMessage13:
                if (client && client->message13)
                    client->message13(*g_saveMessageQueue, message, ee::Float::fromBits(message->value));
                break;
            case kMessageShowText:
            case kMessageSlotMenu:
            case kMessageFileMenu:
                if (client)
                {
                    // The three that hand the client one word: a text, the slot menu, the file menu.
                    const ee::FunctionPtr<void(uint32_t)> show = message->kind == kMessageShowText ? client->showMessage
                                                               : message->kind == kMessageSlotMenu ? client->showSlotMenu
                                                                                                   : client->showFileMenu;
                    if (show)
                        show(message->value);
                }
                break;
            case kMessageSaveFound:
                if (client && client->saveFound)
                    client->saveFound(message->value, message->listBuffer);
                break;
            default:
                if (client && client->finished)
                    client->finished(message->kind);
                return 1u;
            }
        }
        return 1u;
    }
    READABLE_FUNCTION_WAITS(saveSystemTask, Verified);

    // saveApiAutoLoad .. saveApiLoadChoice: a job on a client: the client selected, then the job's start (its own
    // tail call).
    int32_t saveApiAutoLoad(uint32_t client, uint32_t delayMilliseconds)
    {
        selectSaveClient(client);
        return game::tail(startAutoLoadJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(saveApiAutoLoad, Verified);

    int32_t saveApiNullJob(uint32_t client, uint32_t delayMilliseconds)
    {
        selectSaveClient(client);
        return game::tail(startNullJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(saveApiNullJob, Draft);

    int32_t saveApiSave(uint32_t client, uint32_t delayMilliseconds)
    {
        selectSaveClient(client);
        return game::tail(startSaveJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(saveApiSave, Verified);

    // (ZGE4 takes a third word and hands it on beside the delay; startLoadJob reads the delay alone.)
    int32_t saveApiLoad(uint32_t client, uint32_t delayMilliseconds READABLE_PARAM_ONLY(bmx, uint32_t unreadWord = 0))
    {
        selectSaveClient(client);
        return game::tail(startLoadJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(saveApiLoad, Verified);

    int32_t saveApiSilentLoad(uint32_t client, uint32_t delayMilliseconds)
    {
        selectSaveClient(client);
        return game::tail(startSilentLoadJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(saveApiSilentLoad, Verified);

    int32_t saveApiSaveChoice(uint32_t client, uint32_t delayMilliseconds)
    {
        selectSaveClient(client);
        return game::tail(startSaveChoiceJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(saveApiSaveChoice, Verified);

    // (ZGE4 takes a third word and hands it on beside the delay; startLoadChoiceJob reads the delay alone.)
    int32_t saveApiLoadChoice(uint32_t client, uint32_t delayMilliseconds READABLE_PARAM_ONLY(bmx, uint32_t unreadWord = 0))
    {
        selectSaveClient(client);
        return game::tail(startLoadChoiceJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(saveApiLoadChoice, Verified);

    // saveApiNoop
    uint32_t saveApiNoop()
    {
        return 0;
    }
    READABLE_FUNCTION(saveApiNoop, Verified);

    // newSaveMessageQueue: a ring of `limit` messages, all empty.
    ee::Ptr<SaveMessageQueue> newSaveMessageQueue(uint32_t limit)
    {
        const uint32_t header = offsetof(SaveMessageQueue, first);
        const ee::Ptr<SaveMessageQueue> queue(heapAllocate((limit * sizeof(SaveMessage)) | header));
        queue->limit = limit;
        queue->oldest = 0;
        const ee::Ptr<SaveMessage> messages(&queue->first);
        for (uint32_t i = 0; i < limit; ++i)
            messages[i].kind = 0;
        return queue;
    }
    READABLE_FUNCTION_WAITS(newSaveMessageQueue, Verified);

    namespace
    {
        // The queue's message at index i.
        ee::Ptr<SaveMessage> queueMessage(ee::Ptr<SaveMessageQueue> queue, uint32_t i)
        {
            return ee::Ptr<SaveMessage>(&queue->first) + i;
        }

        // How long the waits for a message sleep, in milliseconds.
        constexpr uint32_t kMessagePollMilliseconds = 16;
        // askSaveQuestion's result when the card poll stops the question.
        constexpr int32_t kQuestionStopped = -2;

        void copyMessage(ee::Ptr<SaveMessage> to, ee::Ptr<SaveMessage> from) { ee::game(sym::memcpy, to, from, sizeof(SaveMessage)); }
    }

    // postSaveMessage: a copy of the message in the first free slot from the
    // oldest on. 1, or 0 for a queue of no slots.
    // Kept bug: with every slot taken it spins for ever (it never sleeps,
    // so nothing can take one).
    uint32_t postSaveMessage(ee::Ptr<SaveMessageQueue> queue, ee::Ptr<SaveMessage> message)
    {
        const uint32_t limit = queue->limit;
        uint32_t i = queue->oldest;
        while (limit != 0)
        {
            const ee::Ptr<SaveMessage> slot = queueMessage(queue, i);
            if (slot->kind == 0)
            {
                copyMessage(slot, message);
                return 1;
            }
            i = i + 1 < limit ? i + 1 : 0;
        }
        return 0;
    }
    READABLE_FUNCTION(postSaveMessage, Verified);

    // takeSaveMessage: the oldest message copied out and its slot freed. With
    // wait it sleeps until there is one; without, 0 when there is none, else 1.
    uint32_t takeSaveMessage(ee::Ptr<SaveMessageQueue> queue, ee::Ptr<SaveMessage> messageOut, uint32_t wait)
    {
        if (wait != 0)
        {
            while (queueMessage(queue, queue->oldest)->kind == 0)
                waitMilliseconds(kMessagePollMilliseconds);
        }
        else if (queueMessage(queue, queue->oldest)->kind == 0)
            return 0;
        copyMessage(messageOut, queueMessage(queue, queue->oldest));
        queueMessage(queue, queue->oldest)->kind = 0;
        queue->oldest = queue->oldest + 1;
        if (!(queue->oldest < queue->limit))
            queue->oldest = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(takeSaveMessage, Verified);

    // byteChecksum: the sum of the bytes (the save files' check).
    int32_t byteChecksum(ee::Address data, uint32_t bytes)
    {
        const ee::Ptr<uint8_t> byte(data);
        int32_t sum = 0;
        for (uint32_t i = 0; i < bytes; ++i)
            sum += byte[i];
        return sum;
    }
    READABLE_FUNCTION(byteChecksum, Verified);

    // askSaveQuestion: a choice of buttons put to the player (firstChoice
    // chosen to begin with), and the button answered. poll (non-zero) is
    // asked each time round with the reply queue; when it says 0 (the card
    // changed) the replies are dropped, the choice taken down and the result
    // is -2. The message's third word is left as the stack had it.
    int32_t askSaveQuestion(uint32_t buttons, uint32_t firstChoice, uint32_t poll)
    {
        const ee::FunctionPtr<int32_t(ee::Ptr<SaveMessageQueue> replies)> cardUnchanged(poll);
        ee::Local<SaveMessage> message;
        message->kind = kMessageShowChoice;
        message->value = buttons;
        message->choice = firstChoice;
        postSaveMessage(*g_saveMessageQueue, message);
        for (bool answered = false; !answered;)
        {
            const ee::Ptr<SaveMessageQueue> replies(*g_saveReplyQueue);
            if (cardUnchanged && cardUnchanged(replies) == 0)
            {
                while (takeSaveMessage(*g_saveReplyQueue, message, 0) != 0)
                {
                }
                message->kind = kMessageShowChoice;
                message->value = 0;
                message->choice = 0;
                postSaveMessage(*g_saveMessageQueue, message);
                return kQuestionStopped;
            }
            if (takeSaveMessage(*g_saveReplyQueue, message, 0) != 0 &&
                message->kind == kMessageAnswer)
                answered = true;
            waitMilliseconds(kMessagePollMilliseconds);
        }
        return static_cast<int32_t>(message->value);
    }
    READABLE_FUNCTION_WAITS(askSaveQuestion, Verified);
}
