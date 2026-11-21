// The skater's events to the level (were the sweep's sendObjectEvent and
// sendScriptEvent): an event by name ("OnGrind", "OnLand" ...) sent at once
// as an engine message (zge/scene/message.h), to the scene object a
// collision object belongs to or straight to a script object. The level's
// scripts take them (aggressive/script).
//

#include "skater.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/text/strings.h"
#include "zge/scene/scene.h"
#include "sym/aggressive/skater/events.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // Engine calls by address (a game override hooks sendEngineMessage).
        constexpr uint32_t kSendEngineMessage = sym::sendEngineMessage; // (message*) -> handled

        // The message (on the caller's stack) filled in and sent at once.
        int32_t sendEventNow(ee::Ptr<zge::EngineMessage> m, ee::Address target, uint32_t event, uint32_t data)
        {
            m->target = target;
            m->type = event;
            m->flags = zge::kMessageNow;
            m->data = data;
            m->sender = 0;
            return zge::sendEngineMessage(m);
        }
    }

    // sendObjectEvent: the event `name` (with `data`) to the scene object
    // `object` is part of. Its name's hash when there is no object, 1 when
    // it belongs to none, else whether it was handled.
    int32_t sendObjectEvent(ee::Ptr<Player> /*player*/, ee::Ptr<zge::Node> object, ee::Address name, ee::Address data)
    {
        ee::Local<zge::EngineMessage> message;
        const uint32_t event = zge::nameHash(ee::Ptr<const char>(name));
        int32_t result = static_cast<int32_t>(event);
        if (object)
        {
            const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(object, 0);
            result = 1;
            if (owner)
                result = sendEventNow(message, owner, event, data);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(sendObjectEvent, Verified);

    // sendScriptEvent: the event `name` (with `data`) to the script object
    // `script`. Its name's hash when there is none, else whether it was handled.
    int32_t sendScriptEvent(ee::Ptr<Player> /*player*/, ee::Address script, ee::Address name, ee::Address data)
    {
        ee::Local<zge::EngineMessage> message;
        const uint32_t event = zge::nameHash(ee::Ptr<const char>(name));
        int32_t result = static_cast<int32_t>(event);
        if (script)
            result = sendEventNow(message, script, event, data);
        return result;
    }
    READABLE_FUNCTION_WAITS(sendScriptEvent, Verified);
}
