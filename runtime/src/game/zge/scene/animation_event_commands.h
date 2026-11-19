#pragma once
// ZGE4's animation event commands (zge/scene/animation_event_commands.cpp; BMX XXX only): what the engine does for
// the events an animation fires, by the fields the event carries (OBJECT0 .. OBJECT8 the ids of objects,
// CATEGORY0 .. CATEGORY8 the names of categories, RATE0 .. RATE8 particle rates). The game registers them with
// registerAnimationEventHandler under its own command names (HIDE_OBJ, SHOW_OBJ, HIDE_CATEGORY, SHOW_CATEGORY,
// PARTICLE).

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge
{
    struct SceneObject;    // zge/scene/scene.h
    struct AnimationEvent; // zge/scene/animation.h
    struct EngineMessage;  // zge/scene/message.h

    // The event's OBJECT fields this many, OBJECT0 .. OBJECT8 (CATEGORY and RATE likewise).
    constexpr uint32_t kEventObjectFields = 9;

    // (animation_event_commands.cpp, BMX XXX only)
    // The event's objects hidden or shown (their nodes' trees, and whether they collide), and the objects of its
    // categories.
    void setEventObjectsShown(ee::Ptr<AnimationEvent> event, uint32_t shown);
    void setEventCategoriesShown(ee::Ptr<AnimationEvent> event, uint32_t shown);
    void hideObjectCommand(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event);
    void showObjectCommand(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event);
    void hideCategoryCommand(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event);
    void showCategoryCommand(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event);
    // particleCommand: each OBJECT field's particle emitter given its RATE (particles a second).
    void particleCommand(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event);
    // sendMessageToEventObjects: the message sent to each of the event's objects, in turn its target.
    void sendMessageToEventObjects(ee::Ptr<AnimationEvent> event, ee::Ptr<EngineMessage> message);
}
