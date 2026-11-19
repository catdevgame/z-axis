// An event broadcast to the level's script objects that listen for its
// kind (round 9): broadcastEvent, used by the score (skater/score.cpp,
// "skaterPointsChanged" to the score's listeners, kind 2). Two tables of
// listeners (inferred: the level scripts' subscriptions), each a name to
// find the object by and the kind it listens for; every match gets the
// event as an engine message, delivered at once.

#include "script.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "sym/aggressive/script/broadcast.cpp.h"

namespace aggressive::script
{
    namespace
    {
        // The two listener tables: the goals and the bonus goals.
        struct ListenerA
        {
            uint8_t _00[0x0C];
            uint32_t kind;        // +0x0C
            uint8_t _10[4];
            uint32_t name;        // +0x14 the object's name hash
            uint8_t _18[0x14];
        };
        static_assert(sizeof(ListenerA) == 0x2C);
        struct ListenerB
        {
            uint8_t _00[4];
            uint32_t kind;        // +0x04
            uint8_t _08[4];
            uint32_t name;        // +0x0C
            uint8_t _10[0x24];
        };
        static_assert(sizeof(ListenerB) == 0x34);
        inline ee::Global<ee::Ptr<ListenerA>, sym::g_objectives> g_listenersA;
        inline ee::Global<uint32_t, sym::g_objectiveCount> g_listenerCountA;
        inline ee::Global<ee::Ptr<ListenerB>, sym::g_bonusObjectives> g_listenersB;
        inline ee::Global<uint32_t, sym::g_bonusObjectiveCount> g_listenerCountB;
        // The kinds' tags, by the broadcast's kind.
        inline ee::Global<uint32_t[8], sym::g_stateHashes> g_listenerKinds;

    }

    namespace
    {
        struct WalkBlocks
        {
            uint32_t listener, sent, next, end;
        };
    }

    void broadcastEvent(uint32_t event, uint32_t kind)
    {
        ee::Local<Message> message;
        const auto send = [&](ee::Address name) {
            const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(name, 1);
            if (!found)
                return;
            message->sender = 0;
            message->flags = zge::kMessageNow;
            message->target = found[0];
            message->data = 0;
            message->type = event;
            zge::sendEngineMessage(message);
        };
        // A table's listeners for the kind; a message may change the count.
        const auto walk = [&](const auto &table, const auto &count) {
            for (uint32_t i = 0; i < *count; ++i)
            {
                const auto &listener = (*table)[i];
                if (listener.kind == (*g_listenerKinds)[kind])
                    send(&listener.name);
            }
        };
        walk(g_listenersA, g_listenerCountA);
        walk(g_listenersB, g_listenerCountB);
    }
    READABLE_FUNCTION_WAITS(broadcastEvent, Verified);
}
