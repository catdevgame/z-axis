// BMX XXX's attaches (attach.h): the two level object types, "Attach" and
// "HumanAttach", and the messages an attach takes: a rider activates it
// (it is carried from then on), lets it go, it starts to blink, the
// engine's carry messages, and the engine's removed, save and restore.
// BMX XXX's own code.

#include "attach.h"
#include "attach_calls.h"

#include "bmx/pathman/pathman_calls.h"
#include "bmx/pathman/pathman_phrases.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/carried_objects.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/attach/attach_object.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using pathman::ActivationEvent;
    using rider::collision::nodeHandle;

    namespace
    {
        constexpr uint32_t kTypeName = sym::attachRegisteredName;           // "Attach"
        constexpr uint32_t kHumanTypeName = sym::humanAttachRegisteredName; // "HumanAttach"
        constexpr uint32_t kCreateEntity = sym::createEntity;               // the level's plain entity maker

        // The engine's carry messages (their names the wrong way round, as
        // in ZGE3): this one starts a carry, its word the carrier, and the
        // other ends it.
        inline ee::Global<uint32_t, sym::g_parrotReleasedHash> g_carryStartsMessage;
        inline ee::Global<uint32_t, sym::g_parrotAttachedHash> g_carryEndsMessage;

        // kActivateMessage or kReleaseMessage sent to the attach again, at
        // once, for `player` (the caller's two records are filled and sent).
        void resend(ee::Ptr<SceneObject> attach, uint32_t type, uint32_t player, ee::Ptr<ActivationEvent> event, ee::Ptr<EngineMessage> message)
        {
            memoryFill(event, 0, sizeof(ActivationEvent));
            memoryFill(message, 0, sizeof(EngineMessage));
            message->target = attach;
            message->type = type;
            message->flags = kMessageNow;
            message->data = event;
            event->quiet = 1;
            event->player = player;
            message->sender = 0;
            zge::sendEngineMessage(message);
        }
    }

    // registerAttachTypes: both types are made as plain entities, with the
    // same init.
    uint32_t registerAttachTypes()
    {
        registerLevelObjectType(kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initAttach));
        return game::tail(registerLevelObjectType, kHumanTypeName, kCreateEntity, static_cast<uint32_t>(sym::initAttach));
    }
    READABLE_FUNCTION(registerAttachTypes, Verified);

    // initAttach (both types' init): its messages subscribed; riders pass
    // through it, and it is hidden until a rider carries it. Always 1.
    uint32_t initAttach(ee::Ptr<SceneObject> attach)
    {
        subscribe(attach, kBlinkMessage, MessageHandler(sym::startAttachBlink));
        subscribe(attach, kRemovedMessage, MessageHandler(sym::attachRemoved));
        subscribe(attach, kSaveSizeMessage, MessageHandler(sym::attachSaveSize));
        subscribe(attach, kSaveMessage, MessageHandler(sym::saveAttach));
        subscribe(attach, kRestoreMessage, MessageHandler(sym::restoreAttach));
        subscribe(attach, pathman::kActivateMessage, MessageHandler(sym::activateAttach));
        subscribe(attach, kReleaseMessage, MessageHandler(sym::releaseAttach));
        subscribe(attach, *g_carryStartsMessage, MessageHandler(sym::attachCarryStarts));
        subscribe(attach, *g_carryEndsMessage, MessageHandler(sym::attachCarryEnds));
        rider::collision::setNodeCollidable(nodeHandle(ee::Address(objectLevelNode(attach))), 0);
        setSceneObjectShown(attach, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initAttach, Verified);

    // attachRemoved (the engine's removed message): a carried attach is let
    // go first.
    int32_t attachRemoved(ee::Ptr<EngineMessage> message)
    {
        ee::Local<AttachState> state;
        ee::Local<ActivationEvent> event;
        ee::Local<EngineMessage> release;
        const ee::Ptr<SceneObject> attach(message->target);
        attachState(attach, state);
        if (state->carried != 0)
            resend(attach, kReleaseMessage, 0, event, release);
        return 0;
    }
    READABLE_FUNCTION_WAITS(attachRemoved, Verified);

    // startAttachBlink (kBlinkMessage): a carried attach starts to blink,
    // unless one blinks already.
    int32_t startAttachBlink(ee::Ptr<EngineMessage> message)
    {
        if (g_attachBlink->on != 0)
            return 0;
        const ee::Ptr<SceneObject> attach(message->target);
        if (animalCarrier(attach) == 0)
            return 0;
        g_attachBlink->on = 1;
        g_attachBlink->attach = attach;
        g_attachBlink->period = kBlinkStartPeriod;
        g_attachBlink->frames = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(startAttachBlink, Verified);

    // activateAttach (kActivateMessage): the player's rider carries it from
    // now on: the engine is told (attachCarryStarts answers), the state is
    // kept for a save and the attach is shown. A first activation (not one
    // sent again by a restore) also subscribes its update, and a human
    // attach that is carried already speaks its "pickup" phrase.
    int32_t activateAttach(ee::Ptr<EngineMessage> message)
    {
        ee::Local<AttachState> state;
        const ee::Ptr<ActivationEvent> event(message->data);
        const ee::Ptr<SceneObject> attach(message->target);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(event->player & 0xFF);
        if (event->quiet == 0)
        {
            subscribe(attach, kUpdateMessage, MessageHandler(sym::attachUpdate));
            if (animalCarrier(attach) != 0 && attachCarryTag(attach) == kCarryTagHuman)
                sayPickupPhrase(attachFigure(attachFigureName(attach)));
        }
        zge::sendParrotReleasedWith(ee::Address(attach), ee::Address(rider));
        state->carried = 1;
        state->player = event->player;
        setAttachState(attach, state);
        setSceneObjectShown(attach, 1);
        stopAttachBlink();
        return 0;
    }
    READABLE_FUNCTION_WAITS(activateAttach, Verified);

    // releaseAttach (kReleaseMessage): its rider lets it go: the engine is
    // told (attachCarryEnds answers), the attach is hidden and its state
    // cleared, its animate systems are freed, and its figure's rig or
    // system with the figure's animator; the figure is let go, hidden, too.
    // Note: after the second system is freed it is the swing system's tag
    // word that is cleared again; the second system's keeps the freed
    // system (nothing read so far makes a second system).
    int32_t releaseAttach(ee::Ptr<EngineMessage> message)
    {
        ee::Local<AttachState> state;
        const ee::Ptr<SceneObject> attach(message->target);
        const ee::Ptr<ActivationEvent> event(message->data);
        const ee::Ptr<SceneObject> carrier(animalCarrier(attach));
        if (event->quiet == 0)
            unsubscribe(attach, kUpdateMessage, MessageHandler(sym::attachUpdate));
        if (carrier)
        {
            zge::sendParrotAttachedWith(ee::Address(attach), ee::Address(carrier));
            setSceneObjectShown(attach, 0);
            state->carried = 0;
            state->player = 0;
            setAttachState(attach, state);
            if (attachSwingSystem(attach))
            {
                freeAnimateSystem(attachSwingSystem(attach));
                setAttachSwingSystem(attach, {});
            }
            if (attachSecondSystem(attach))
            {
                freeAnimateSystem(attachSecondSystem(attach));
                setAttachSwingSystem(attach, {});
            }
            if (attachFigureSystem(attach))
            {
                ee::Ptr<SceneObject> figure;
                if (attachFigureHasRig(attach) != 0)
                {
                    const ee::Ptr<BodyRig> rig = attachFigureRig(attach);
                    figure = rig->system->object;
                    freeBodyRig(rig);
                    setAttachFigureRig(attach, {});
                }
                else
                {
                    const ee::Ptr<AnimateSystem> system = attachFigureSystem(attach);
                    setAttachFigureSystem(attach, {});
                    figure = system->object;
                    if (zge::attachedParent(figure))
                        attach::detachObject(system->object);
                    freeAnimateSystem(system);
                }
                if (pathman::objectHasAnimator(figure) != 0)
                    pathman::freeAnimatorOf(figure);
                zge::sendParrotAttachedWith(ee::Address(figure), ee::Address(carrier));
                zge::detachAnimal(figure);
                setSceneObjectShown(figure, 0);
                updatePose(figure);
            }
            if (pathman::objectHasAnimator(attach) != 0)
                pathman::freeAnimatorOf(attach);
            if (zge::attachedParent(attach))
                attach::detachObject(attach);
            updatePose(attach);
        }
        stopAttachBlink();
        return 0;
    }
    READABLE_FUNCTION_WAITS(releaseAttach, Verified);

    // attachUpdate (the frame's update, subscribed while it is carried):
    // nothing; the engine's carried objects call its updates.
    int32_t attachUpdate(ee::Ptr<EngineMessage> message)
    {
        (void)message;
        return 0;
    }
    READABLE_FUNCTION(attachUpdate, Verified);

    // attachSaveSize (the engine's save size message).
    int32_t attachSaveSize(ee::Ptr<EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(AttachState);
        return 0;
    }
    READABLE_FUNCTION(attachSaveSize, Verified);

    // restoreAttach (the engine's restore message; the data: the saved
    // state): an attach carried now is let go, the saved state is taken,
    // and an attach the save has carried is activated again.
    // Note: it is activated for the player that carried it before the
    // restore, not the saved one.
    int32_t restoreAttach(ee::Ptr<EngineMessage> message)
    {
        ee::Local<AttachState> before;
        ee::Local<ActivationEvent> releaseEvent;
        ee::Local<EngineMessage> release;
        ee::Local<ActivationEvent> activationEvent;
        ee::Local<EngineMessage> activation;
        const ee::Ptr<SceneObject> attach(message->target);
        const ee::Ptr<AttachState> saved(message->data);
        attachState(attach, before);
        if (before->carried != 0)
            resend(attach, kReleaseMessage, before->player, releaseEvent, release);
        setAttachState(attach, saved);
        if (saved->carried != 0)
            resend(attach, pathman::kActivateMessage, before->player, activationEvent, activation);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreAttach, Verified);

    // saveAttach (the engine's save message).
    int32_t saveAttach(ee::Ptr<EngineMessage> message)
    {
        attachState(ee::Ptr<SceneObject>(message->target), ee::Ptr<AttachState>(message->data));
        return 0;
    }
    READABLE_FUNCTION(saveAttach, Verified);

    // attachCarryStarts (the engine's message that a carry starts; the
    // data points to the carrier): an attach of a carry tag becomes one of
    // the carrier's carried objects, with its set-up and its updates.
    int32_t attachCarryStarts(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<SceneObject> attach(message->target);
        const ee::Ptr<ee::Ptr<SceneObject>> carrier(message->data);
        const uint32_t tag = attachCarryTag(attach);
        if (tag >= kCarryTagLowest && tag <= kCarryTagHuman)
        {
            zge::attachCarriedObject(*carrier, attach, zge::CarriedCall(sym::setUpAttach), zge::CarriedCall(sym::updateAttachSwing),
                                        zge::CarriedCall(sym::updateAttachFigure), zge::CarriedCall(sym::attachLetGo));
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(attachCarryStarts, Verified);

    // attachCarryEnds (the engine's message that a carry ends).
    int32_t attachCarryEnds(ee::Ptr<EngineMessage> message)
    {
        zge::detachAnimal(message->target);
        return 0;
    }
    READABLE_FUNCTION_WAITS(attachCarryEnds, Verified);
}
