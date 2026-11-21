// Two ACTION kinds of zgui that AI's scripts leave idle. "Ballistic" throws a
// widget (its Position by an Impulse and Gravity, over a Duration): its
// functions have no caller at all. "Animate" plays a model widget's
// animations: start, loop, end, then OnLoop, and again. MAIN.ZSC has one, but
// its MODEL block is DM2's and this game's parser reads MODEL as a property,
// so the Dance ACTION lands on a widget that is not a "guiModel": its start
// and update end at the class's name. DM2's menus (UILIB.ZAL) really use it.

#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/scene/animation.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/render/terrain.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/gui_model.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kSetVariable = sym::setVariable;       // by address: a game override hooks it

        constexpr uint32_t kResetBallistic = sym::resetGuiBallistic, kUpdateBallistic = sym::updateGuiBallistic;
        constexpr uint32_t kUpdateModelAnimation = sym::updateGuiModelAnimation;

        constexpr uint32_t kPositionName = sym::g_textPosition;      // "Position"
        constexpr uint32_t kZ3DVectorType = sym::g_textZ3DVector2;
        constexpr ee::Ptr<const char> kGravityName{sym::g_textGravity};
        constexpr uint32_t kZFloatType = sym::g_textZFloat3;
        constexpr ee::Ptr<const char> kImpulseName{sym::g_textImpulse};
        constexpr ee::Ptr<const char> kDurationName{sym::g_textDuration};
        constexpr uint32_t kEventType = sym::g_textEvent2;
        constexpr uint32_t kGuiModelName = sym::g_textGuiModel2;      // "guiModel"
        constexpr uint32_t kZAssetIdType = sym::g_textZAssetID;
        constexpr ee::Ptr<const char> kStartAnimationName{sym::g_textStartAnimation};
        constexpr ee::Ptr<const char> kOnLoopName{sym::g_textOnLoop};
        constexpr ee::Ptr<const char> kLoopAnimationName{sym::g_textLoopAnimation};
        constexpr ee::Ptr<const char> kEndAnimationName{sym::g_textEndAnimation};
        constexpr uint32_t kBoolType = sym::g_textBool2;
        constexpr ee::Ptr<const char> kLoopModeName{sym::g_textLoopMode};
        constexpr ee::Ptr<const char> kSpeedName{sym::g_textSpeed};
        constexpr ee::Ptr<const char> kLoopTimeName{sym::g_textLoopTime};

        constexpr uint32_t kGuiModelNodeShown = sym::guiModelNodeShown;
        constexpr uint32_t kDrawGuiModel = sym::drawGuiModel, kRefreshGuiModel = sym::refreshGuiModel;

        constexpr uint32_t kGuiModelClass = sym::g_textGuiModel;     // "guiModel", the widget class's own copy
        constexpr uint32_t kAssetNameName = sym::g_textAssetName, kZAssetIdType2 = sym::g_textZAssetID2;
        constexpr uint32_t kShowShadowName = sym::g_textShowShadow, kBoolType2 = sym::g_textBool;

        constexpr uint32_t kAssetModel = 4;                  // findGuiAsset's kind
        constexpr uint32_t kAssetMotion = 3;                 // findGuiAsset's kind
        constexpr uint8_t kModelSceneKind = 1;               // a Scene's first byte when it is a model's

        // The Ballistic kind.
        struct Ballistic
        {
            Animation animation;
            ee::Float time;      // +0x40
            ee::Float duration;  // +0x44 "Duration"
            uint32_t _48[2];
            zge::Vec4 velocity;  // +0x50
            zge::Vec4 gravity;   // +0x60 "Gravity"
            zge::Vec4 impulse;   // +0x70 "Impulse"
        };
        static_assert(offsetof(Ballistic, time) == 0x40 && offsetof(Ballistic, velocity) == 0x50);
        static_assert(offsetof(Ballistic, gravity) == 0x60 && offsetof(Ballistic, impulse) == 0x70);

        enum ModelStage : uint32_t
        {
            kStageStart = 0,
            kStageLoop = 1,
            kStageEnd = 2,
            kStageNone = 3,
        };

        // The Animate kind.
        struct ModelAnimation
        {
            union
            {
                Animation animation;
                struct
                {
                    uint8_t animationFields[offsetof(Animation, script)];
                    // +0x3C each stage's motion asset (ModelStage), the
                    // start's in Animation::script's place: this kind has no script.
                    ee::Ptr<UiAsset> asset[3];
                } stages;
            };
            uint32_t loopMode;       // +0x48 "LoopMode"
            ee::Float speed;         // +0x4C "Speed"
            ee::Float loopTime;      // +0x50 "LoopTime": how long the loop stage lasts
            uint32_t startName;      // +0x54 "StartAnimation"
            uint32_t loopName;       // +0x58 "LoopAnimation"
            uint32_t endName;        // +0x5C "EndAnimation"
            uint32_t onLoop;         // +0x60 "OnLoop"
            ee::Float stageStart;    // +0x64 when the loop stage began
            ee::Float time;          // +0x68
            uint32_t stage;          // +0x6C ModelStage
        };
        static_assert(offsetof(ModelAnimation, stages.asset) == 0x3C && offsetof(ModelAnimation, loopMode) == 0x48);
        static_assert(offsetof(ModelAnimation, startName) == 0x54 && offsetof(ModelAnimation, stage) == 0x6C);

        // ZGE2's Animate kind: its ACTION (ListedAction) and the same fields
        // in another order (DM2's defineGuiAnimationProperties and
        // startGuiModelAnimation read them).
        struct ListedModelAnimation
        {
            union
            {
                ListedAction action;
                struct
                {
                    uint8_t actionFields[offsetof(ListedAction, script)];
                    ee::Ptr<UiAsset> asset[3]; // +0x48 each stage's motion asset, the start's in the script's place
                } stages;
            };
            uint32_t loopMode;       // +0x54 "LoopMode"
            ee::Float speed;         // +0x58 "Speed"
            ee::Float time;          // +0x5C
            ee::Float loopTime;      // +0x60 "LoopTime"
            ee::Float stageStart;    // +0x64
            uint32_t stage;          // +0x68 ModelStage
            uint32_t startName;      // +0x6C "StartAnimation"
            uint32_t loopName;       // +0x70 "LoopAnimation"
            uint32_t endName;        // +0x74 "EndAnimation"
            uint32_t onLoop;         // +0x78 "OnLoop"
        };
        static_assert(offsetof(ListedModelAnimation, stages.asset) == 0x48 && offsetof(ListedModelAnimation, loopMode) == 0x54);
        static_assert(offsetof(ListedModelAnimation, time) == 0x5C && offsetof(ListedModelAnimation, stage) == 0x68);
        static_assert(offsetof(ListedModelAnimation, startName) == 0x6C && offsetof(ListedModelAnimation, onLoop) == 0x78);

        // A "guiModel" widget (0x160 bytes).
        struct GuiModel
        {
            Node base;
            uint8_t _040[0xB0];
            zge::Quaternion turn;      // +0xF0
            uint8_t _100[4];
            ee::Ptr<const char> assetName; // +0x104 "AssetName": the model's asset
            ee::Ptr<zge::SceneObject> model; // +0x108 its scene object, 0 until drawn
            uint32_t showShadow;       // +0x10C "ShowShadow"
            uint32_t modelKept;        // +0x110 cleared with the model; read, never set: the model's scene is let go
            PropertyNode properties[2]; // +0x114 AssetName's and ShowShadow's nodes
        };
        static_assert(offsetof(GuiModel, turn) == 0xF0 && offsetof(GuiModel, modelKept) == 0x110);
        static_assert(offsetof(GuiModel, properties) == 0x114);

        // The guiModel widget's scene object (0 until drawn).
        ee::Ptr<zge::SceneObject> sceneObjectOf(ee::Ptr<Node> widget) { return classCast<GuiModel>(widget)->model; }

        // ZGE2's guiModel record, after WidgetRecord (DM2's MODEL block makes
        // one; its Refresh finds the model again).
        struct ListedGuiModel
        {
            WidgetRecord widget;
            uint8_t _60[4];
            ee::Ptr<const char> assetName;   // +0x64 "AssetName"
            ee::Ptr<zge::SceneObject> model; // +0x68 its scene object, 0 until refreshed
            uint32_t modelKept;              // +0x6C cleared with the model, as ZGE3's
        };
        static_assert(offsetof(ListedGuiModel, assetName) == 0x64 && offsetof(ListedGuiModel, modelKept) == 0x6C);
        static_assert(sizeof(ListedGuiModel) == 0x70);
        // The guiModel record of a ZGE2 ACTION's widget.
        ee::Ptr<ListedGuiModel> guiModelOf(ee::Ptr<ListedAction> action)
        {
            return classCast<ListedGuiModel>(widgetRecord(action->parent));
        }

        // A motion or model asset by its name, none if there is no such asset of that kind.
        ee::Ptr<UiAsset> findMotion(uint32_t name) { return findGuiAsset(name, kAssetMotion); }
        ee::Ptr<UiAsset> findModel(ee::Ptr<const char> path) { return findGuiAsset(path, kAssetModel); }
    }

    namespace
    {
        // ZGE2's Ballistic ACTION: the same fields after the ListedAction's,
        // its playing state in itself.
        struct ListedBallistic
        {
            uint8_t action[offsetof(ListedAction, script)];
            uint8_t _48[4];
            ee::Float time;      // +0x4C
            ee::Float duration;  // +0x50 "Duration"
            uint8_t _54[0xC];
            zge::Vec4 velocity;  // +0x60
            zge::Vec4 gravity;   // +0x70 "Gravity"
            zge::Vec4 impulse;   // +0x80 "Impulse"
        };
        static_assert(offsetof(ListedBallistic, time) == 0x4C && offsetof(ListedBallistic, velocity) == 0x60);
        static_assert(offsetof(ListedBallistic, impulse) == 0x80 && sizeof(ListedBallistic) == 0x90);
    }

    // resetGuiBallistic: the Ballistic kind's start (nothing makes one).
    void resetGuiBallistic(ee::Ptr<Playing> playing)
    {
        const ee::Ptr<Ballistic> ballistic = classCast<Ballistic>(playing->animation);
        ballistic->time = 0;
        ballistic->velocity = ballistic->impulse;
    }
    READABLE_FUNCTION_OWN(resetGuiBallistic, Verified, dm2, resetListedBallistic);

    // resetListedBallistic: ZGE2's resetGuiBallistic, the Ballistic ACTION's start.
    void resetListedBallistic(ee::Ptr<ListedBallistic> ballistic)
    {
        ballistic->time = 0;
        ballistic->velocity = ballistic->impulse;
    }

    // updateGuiBallistic: its update: 0 past its duration.
    int32_t updateGuiBallistic(ee::Ptr<Playing> playing, ee::Float seconds)
    {
        ee::Local<zge::Vec4> position;
        const ee::Ptr<Ballistic> ballistic = classCast<Ballistic>(playing->animation);
        const ee::Float time = ballistic->time + seconds;
        const bool over = ballistic->duration < time;
        ballistic->time = time;
        if (over)
            return 0;
        copyVariable(ballistic->animation.parent, kPositionName, position);
        zge::Vec4 &velocity = ballistic->velocity;
        const zge::Vec4 &gravity = ballistic->gravity;
        position->x = position->x + velocity.x * seconds;
        position->y = position->y + velocity.y * seconds;
        position->z = position->z + velocity.z * seconds;
        velocity.x = velocity.x + gravity.x * seconds;
        velocity.y = velocity.y + gravity.y * seconds;
        velocity.z = velocity.z + gravity.z * seconds;
        setVariable(ballistic->animation.parent, kPositionName, position);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(updateGuiBallistic, Verified, dm2, updateListedBallistic);

    // updateListedBallistic: ZGE2's updateGuiBallistic: the same throw, moving
    // the widget's scene object itself; 0 past its duration.
    int32_t updateListedBallistic(ee::Ptr<ListedBallistic> ballistic, ee::Float seconds)
    {
        ee::Local<zge::Vec4> position;
        const ee::Ptr<ListedAction> action = classCast<ListedAction>(ballistic);
        const ee::Float time = ballistic->time + seconds;
        const bool over = ballistic->duration < time;
        ballistic->time = time;
        if (over)
            return 0;
        *position = *zge::objectPosition(action->parent);
        zge::Vec4 &velocity = ballistic->velocity;
        const zge::Vec4 &gravity = ballistic->gravity;
        position->x = position->x + velocity.x * seconds;
        position->y = position->y + velocity.y * seconds;
        position->z = position->z + velocity.z * seconds;
        velocity.x = velocity.x + gravity.x * seconds;
        velocity.y = velocity.y + gravity.y * seconds;
        velocity.z = velocity.z + gravity.z * seconds;
        zge::setObjectPosition(action->parent, position);
        return 1;
    }

    // defineGuiBallisticProperties: the kind's set-up (no kind table row names it).
    uint32_t defineGuiBallisticProperties(uint32_t address, uint32_t name, ee::Ptr<Node> parent)
    {
        const ee::Ptr<Ballistic> node(address);
        initTimedNode(address, name, parent);
        // Gravity (0, -9.8, 0), no impulse, no duration, no end.
        node->gravity.x = ee::Float(0.0f);
        node->gravity.z = ee::Float(0.0f);
        node->impulse.x = ee::Float(0.0f);
        node->impulse.y = ee::Float(0.0f);
        node->impulse.z = ee::Float(0.0f);
        node->duration = ee::Float(0.0f);
        node->animation.end = 0;
        node->gravity.y = ee::Float(-9.8f);
        node->animation.start = kResetBallistic;
        node->animation.update = kUpdateBallistic;
        defineTypedProperty(node, kGravityName, kZ3DVectorType, &node->gravity);
        defineTypedProperty(node, kImpulseName, kZ3DVectorType, &node->impulse);
        defineTypedProperty(node, kDurationName, kZFloatType, &node->duration);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiBallisticProperties, Verified, dm2, defineListedBallistic);

    // defineListedBallistic: ZGE2's defineGuiBallisticProperties: the Ballistic
    // ACTION over a ListedAction, its properties listed on it.
    uint32_t defineListedBallistic(ee::Address address, ee::Address name, ee::Ptr<zge::SceneObject> parent)
    {
        const ee::Ptr<ListedBallistic> node(address);
        const ee::Ptr<ListedAction> action(address);
        initListedAction(address, name, parent);
        // Gravity (0, -9.8, 0), no impulse, no duration, no end.
        node->gravity.x = ee::Float(0.0f);
        node->gravity.z = ee::Float(0.0f);
        node->impulse.x = ee::Float(0.0f);
        node->impulse.y = ee::Float(0.0f);
        node->impulse.z = ee::Float(0.0f);
        node->duration = ee::Float(0.0f);
        action->end = {};
        node->gravity.y = ee::Float(-9.8f);
        action->start = kResetBallistic;
        action->update = kUpdateBallistic;
        const auto define = [&](ee::Ptr<const char> propertyName, uint32_t typeName, ee::Address value) {
            addListedProperty(actionProperties(action), propertyName, ee::Ptr<const char>(typeName), value);
        };
        define(kGravityName, kZ3DVectorType, valueAddress(node->gravity));
        define(kImpulseName, kZ3DVectorType, valueAddress(node->impulse));
        define(kDurationName, kZFloatType, valueAddress(node->duration));
        return address;
    }

    // startGuiModelAnimation: the Animate kind's start: on a guiModel widget, the first
    // of the three animations it finds played. (Its kind's start callback and
    // updateGuiModelAnimation read no result: the original's is the name's compare,
    // startAnimation's, kStageNone or, in ZGE4 with no scene object yet, whatever
    // its caller left.)
    void startGuiModelAnimation(ee::Ptr<Playing> playing)
    {
        const ee::Ptr<ModelAnimation> animation = classCast<ModelAnimation>(playing->animation);
        const ee::Ptr<Node> widget = animation->animation.parent;
        // (a guiModel's: ZGE4 keeps no kind name and asks for a scene object alone)
        if constexpr (!game::readable::kIsBmx)
        {
            if (zge::stringCompareNoCase(widget->type, kGuiModelName) != 0)
                return;
        }
        const ee::Ptr<zge::SceneObject> sceneObject = sceneObjectOf(widget);
        if (!sceneObject)
            return;
        if (!sceneObject->animator)
            zge::createObjectAnimator(sceneObject);
        ee::Ptr<UiAsset> (&asset)[3] = animation->stages.asset;
        asset[kStageStart] = findMotion(animation->startName);
        asset[kStageLoop] = findMotion(animation->loopName);
        asset[kStageEnd] = findMotion(animation->endName);
        animation->time = 0;
        animation->stageStart = 0;
        // The first stage with an animation plays.
        for (uint32_t stage = kStageStart; stage < kStageNone; ++stage)
        {
            if (asset[stage])
            {
                zge::startAnimation(sceneObjectOf(widget), ee::Ptr<zge::Motion>(asset[stage]->object), animation->time); // the asset's word
                animation->stage = stage;
                return;
            }
        }
        animation->stage = kStageNone;
    }
    READABLE_FUNCTION_WAITS_OWN(startGuiModelAnimation, Verified, dm2, startListedModelAnimation);

    // startListedModelAnimation: ZGE2's startGuiModelAnimation: the same
    // start over the ACTION itself and its widget's guiModel record. Answers
    // the name's compare, startAnimation's result for the start stage, else
    // the stage it set.
    int32_t startListedModelAnimation(ee::Ptr<ListedModelAnimation> animation)
    {
        const ee::Ptr<ListedGuiModel> widget = guiModelOf(&animation->action);
        const int32_t differs = zge::stringCompareNoCase(widget->widget.kind, kGuiModelName);
        if (differs != 0)
            return differs;
        const ee::Ptr<zge::SceneObject> model = widget->model;
        if (!model)
            return differs;
        if (!model->animator)
            zge::createObjectAnimator(model);
        ee::Ptr<UiAsset> (&asset)[3] = animation->stages.asset;
        asset[kStageStart] = findMotion(animation->startName);
        asset[kStageLoop] = findMotion(animation->loopName);
        asset[kStageEnd] = findMotion(animation->endName);
        animation->time = 0;
        animation->stageStart = 0;
        // The first stage with an animation plays.
        for (uint32_t stage = kStageStart; stage < kStageNone; ++stage)
        {
            if (asset[stage])
            {
                const int32_t started = zge::startAnimation(widget->model, ee::Ptr<zge::Motion>(asset[stage]->object), animation->time);
                animation->stage = stage;
                return stage == kStageStart ? started : static_cast<int32_t>(stage);
            }
        }
        animation->stage = kStageNone;
        return kStageNone;
    }

    // updateGuiModelAnimation: its update. A stage's animation ended: the start stage
    // gives way to the loop (or the end), the loop plays again until
    // LoopTime has passed, the end stage is over unless LoopMode runs OnLoop
    // and starts it all again. 0 when it is over.
    int32_t updateGuiModelAnimation(ee::Ptr<Playing> playing, ee::Float seconds)
    {
        const ee::Ptr<ModelAnimation> animation = classCast<ModelAnimation>(playing->animation);
        const ee::Ptr<Node> widget = animation->animation.parent;
        if constexpr (game::readable::kIsBmx) // (ZGE4 asks for a scene object alone)
        {
            if (!sceneObjectOf(widget))
                return 0;
        }
        else if (zge::stringCompareNoCase(widget->type, kGuiModelName) != 0 || !sceneObjectOf(widget))
            return 0;
        const ee::Float time = animation->time + seconds * animation->speed;
        animation->time = time;
        const int32_t done = animationDone(sceneObjectOf(widget), time);
        const auto play = [&](ee::Ptr<zge::SceneObject> sceneObject, ee::Ptr<UiAsset> asset, ee::Float at)
        {
            zge::startAnimation(sceneObject, ee::Ptr<zge::Motion>(asset->object), at); // the asset's word
        };
        const auto finish = [&](int32_t result)
        {
            return result;
        };
        // Past the last stage: OnLoop and the start again, if LoopMode says.
        const auto again = [&]()
        {
            if (animation->onLoop != 0)
                runScript(animation->animation.parent, ee::Ptr<char>(animation->onLoop), animation);
            startGuiModelAnimation(playing);
            return finish(1);
        };
        const auto toEnd = [&](ee::Ptr<UiAsset> asset, ee::Float at)
        {
            play(sceneObjectOf(widget), asset, at);
            animation->stage = kStageEnd;
            return finish(1);
        };
        if (done == 0)
        {
            zge::updateAnimation(sceneObjectOf(widget), 0, animation->time);
            return finish(1);
        }
        const uint32_t stage = animation->stage;
        if (stage == kStageLoop)
        {
            const ee::Float now = animation->time;
            if (now - animation->stageStart < animation->loopTime)
            {
                play(sceneObjectOf(widget), animation->stages.asset[kStageLoop], now);
                return finish(1);
            }
            const ee::Ptr<UiAsset> endAsset = animation->stages.asset[kStageEnd];
            if (endAsset)
                return toEnd(endAsset, now);
            return animation->loopMode == 0 ? finish(1) : again();
        }
        if (stage == kStageStart)
        {
            const ee::Float now = animation->time;
            const ee::Ptr<UiAsset> loopAsset = animation->stages.asset[kStageLoop];
            animation->stageStart = now;
            if (loopAsset)
            {
                play(sceneObjectOf(widget), loopAsset, now);
                animation->stage = kStageLoop;
                return finish(1);
            }
            const ee::Ptr<UiAsset> endAsset = animation->stages.asset[kStageEnd];
            if (endAsset)
                return toEnd(endAsset, now);
            return animation->loopMode == 0 ? finish(1) : again();
        }
        if (stage != kStageEnd)
            return 1;
        return animation->loopMode == 0 ? finish(0) : again();
    }
    READABLE_FUNCTION_WAITS_OWN(updateGuiModelAnimation, Verified, dm2, updateListedModelAnimation);

    // updateListedModelAnimation: ZGE2's updateGuiModelAnimation: the same
    // stages over the ACTION itself (the model's motion set to the time while
    // a stage plays). 0 when it is over.
    int32_t updateListedModelAnimation(ee::Ptr<ListedModelAnimation> animation, ee::Float seconds)
    {
        const ee::Ptr<ListedGuiModel> widget = guiModelOf(&animation->action);
        if (zge::stringCompareNoCase(widget->widget.kind, kGuiModelName) != 0 || !widget->model)
            return 0;
        const ee::Float time = animation->time + seconds * animation->speed;
        animation->time = time;
        if (animationDone(widget->model, time) == 0)
        {
            zge::updateMotionTracks(widget->model, animation->time);
            return 1;
        }
        const auto play = [&](ee::Ptr<UiAsset> asset, ee::Float at)
        {
            zge::startAnimation(widget->model, ee::Ptr<zge::Motion>(asset->object), at);
        };
        // Past the last stage: OnLoop and the start again.
        const auto again = [&]()
        {
            if (animation->onLoop != 0)
                runScriptLines(classCast<Node>(animation->action.parent), ee::Ptr<char>(animation->onLoop));
            startListedModelAnimation(animation);
            return 1;
        };
        const uint32_t stage = animation->stage;
        if (stage == kStageLoop)
        {
            const ee::Float now = animation->time;
            if (now - animation->stageStart < animation->loopTime)
            {
                play(animation->stages.asset[kStageLoop], now);
                return 1;
            }
            if (const ee::Ptr<UiAsset> endAsset = animation->stages.asset[kStageEnd])
            {
                play(endAsset, now);
                animation->stage = kStageEnd;
                return 1;
            }
            return animation->loopMode == 0 ? 1 : again();
        }
        if (stage == kStageStart)
        {
            const ee::Float now = animation->time;
            animation->stageStart = now;
            if (const ee::Ptr<UiAsset> loopAsset = animation->stages.asset[kStageLoop])
            {
                play(loopAsset, now);
                animation->stage = kStageLoop;
                return 1;
            }
            if (const ee::Ptr<UiAsset> endAsset = animation->stages.asset[kStageEnd])
            {
                play(endAsset, now);
                animation->stage = kStageEnd;
                return 1;
            }
            return animation->loopMode == 0 ? 1 : again();
        }
        if (stage != kStageEnd)
            return 1;
        return animation->loopMode == 0 ? 0 : again();
    }

    // defineGuiAnimationProperties: the kind's set-up.
    uint32_t defineGuiAnimationProperties(uint32_t address, uint32_t name, ee::Ptr<Node> parent)
    {
        const ee::Ptr<ModelAnimation> node(address);
        initTimedNode(address, name, parent);
        // No assets or names yet, no loop, no end; normal speed.
        node->animation.script = 0;
        node->stages.asset[kStageLoop] = ee::Ptr<UiAsset>();
        node->stages.asset[kStageEnd] = ee::Ptr<UiAsset>();
        node->startName = 0;
        node->loopName = 0;
        node->endName = 0;
        node->onLoop = 0;
        node->loopMode = 0;
        node->animation.end = 0;
        node->loopTime = ee::Float(0.0f);
        node->animation.start = sym::startGuiModelAnimation;
        node->animation.update = kUpdateModelAnimation;
        node->speed = ee::Float(1.0f);
        defineTypedProperty(node, kOnLoopName, kEventType, &node->onLoop);
        defineTypedProperty(node, kStartAnimationName, kZAssetIdType, &node->startName);
        defineTypedProperty(node, kLoopAnimationName, kZAssetIdType, &node->loopName);
        defineTypedProperty(node, kEndAnimationName, kZAssetIdType, &node->endName);
        defineTypedProperty(node, kLoopModeName, kBoolType, &node->loopMode);
        defineTypedProperty(node, kLoopTimeName, kZFloatType, &node->loopTime);
        defineTypedProperty(node, kSpeedName, kZFloatType, &node->speed);
        return address;
    }

    // defineListedModelAnimation: ZGE2's defineGuiAnimationProperties: the
    // same set-up over its ACTION (ListedModelAnimation), the properties on
    // the action's list.
    uint32_t defineListedModelAnimation(uint32_t address, uint32_t name, ee::Ptr<zge::SceneObject> parent)
    {
        const ee::Ptr<ListedModelAnimation> node(address);
        const ee::Ptr<zge::List> properties(valueAddress(node->action.properties));
        const auto define = [&](ee::Ptr<const char> propertyName, uint32_t type, ee::Address value)
        {
            addListedProperty(properties, propertyName, ee::Ptr<const char>(type), value);
        };
        initListedAction(address, name, parent);
        // No assets or names yet, no loop, no end; normal speed.
        node->action.script = ee::Ptr<char>(0u);
        node->stages.asset[kStageLoop] = ee::Ptr<UiAsset>();
        node->stages.asset[kStageEnd] = ee::Ptr<UiAsset>();
        node->startName = 0;
        node->loopName = 0;
        node->endName = 0;
        node->onLoop = 0;
        node->loopMode = 0;
        node->action.end = 0;
        node->loopTime = ee::Float(0.0f);
        node->action.start = sym::startGuiModelAnimation;
        node->action.update = kUpdateModelAnimation;
        node->speed = ee::Float(1.0f);
        define(kOnLoopName, kEventType, valueAddress(node->onLoop));
        define(kStartAnimationName, kZAssetIdType, valueAddress(node->startName));
        define(kLoopAnimationName, kZAssetIdType, valueAddress(node->loopName));
        define(kEndAnimationName, kZAssetIdType, valueAddress(node->endName));
        define(kLoopModeName, kBoolType, valueAddress(node->loopMode));
        define(kLoopTimeName, kZFloatType, valueAddress(node->loopTime));
        define(kSpeedName, kZFloatType, valueAddress(node->speed));
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiAnimationProperties, Verified, dm2, defineListedModelAnimation);

    // setGuiModelCallbacks: the callback on a model's scene node and all under it
    // (drawGuiModel, refreshGuiModel). `scene` is only handed on.
    int32_t setGuiModelCallbacks(ee::Ptr<zge::Scene> scene, ee::Ptr<zge::Node> node, uint32_t callback, ee::Address data);
    int32_t setGuiModelCallbacks(ee::Ptr<zge::Scene> scene, ee::Ptr<zge::Node> node, uint32_t callback, ee::Address data)
    {
        int32_t result = setNodeDrawCheck(node, callback, data);
        for (ee::Ptr<zge::Node> child = node->firstChild; child; child = child->nextSibling)
            result = setGuiModelCallbacks(scene, child, callback, data);
        return result;
    }
    READABLE_FUNCTION(setGuiModelCallbacks, Verified);

    // guiModelNodeShown: the callback the model's nodes carry, with the widget as
    // its data: non-zero while the widget, a "guiModel", is shown (its nodes
    // draw only then). 0 for anything else. (ZGE4's widgets keep no class
    // name: any widget's shown.)
    uint32_t guiModelNodeShown(uint32_t /*unused*/, uint32_t /*unused*/, ee::Ptr<GuiModel> widget)
    {
        uint32_t shown = 0;
        if (widget && widget->base.kind() == NodeKind::Widget)
        {
            if constexpr (game::readable::kIsBmx)
                shown = widget->base.shown;
            else if (zge::stringCompareNoCase(widget->base.type, kGuiModelClass) == 0)
                shown = widget->base.shown;
        }
        return shown;
    }
    READABLE_FUNCTION(guiModelNodeShown, Verified);

    namespace
    {
        // The model asset's scene object made for the widget, its nodes
        // drawn only while the widget is shown (guiModelNodeShown).
        void makeGuiModel(ee::Ptr<GuiModel> widget, ee::Ptr<UiAsset> asset)
        {
            widget->modelKept = 0;
            const ee::Ptr<zge::SceneObject> model(zge::newModelObject(ee::Ptr<zge::Scene>(asset->object), 0));
            widget->model = model;
            const ee::Ptr<zge::Node> sceneNode = zge::sceneRootNode(model->scene);
            const ee::Ptr<zge::Node> root = zge::rootNode(sceneNode);
            setGuiModelCallbacks(widget->model->scene, root, kGuiModelNodeShown, widget);
        }

    }

    // drawGuiModel, a guiModel's Draw message: with a model, the widget's
    // shown state, place, turn and shadow go to it; without one, the
    // AssetName (a model asset) makes it, shown from the next Draw. Returns
    // 1. A widget with no name to load goes on.
    uint32_t drawGuiModel(ee::Ptr<GuiModel> widget)
    {
        if (widget->model)
        {
            setSceneObjectShown(widget->model, widget->base.shown);
            setObjectPosition(widget->model, &widget->base.screenPosition);
            setObjectOrientation(widget->model, &widget->turn);
            zge::setObjectCastsShadow(widget->model, widget->showShadow);
            return 1;
        }
        const ee::Ptr<const char> path = widget->assetName;
        if (path && path[0] != 0)
        {
            const ee::Ptr<UiAsset> asset = findModel(path);
            if (asset)
                makeGuiModel(widget, asset);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawGuiModel, Verified);

    // refreshGuiModel, a guiModel's Refresh message (`message` only held): the
    // model goes, its nodes' callback off and hidden if its scene is a
    // model's, and the AssetName's comes in its place.
    uint32_t refreshGuiModel(ee::Ptr<GuiModel> widget, uint32_t /*message*/)
    {
        if (const ee::Ptr<zge::SceneObject> model = widget->model)
        {
            if (widget->modelKept == 0)
            {
                const ee::Ptr<zge::Scene> scene = model->scene;
                if (static_cast<uint8_t>(scene->flags) == kModelSceneKind)
                {
                    const ee::Ptr<zge::Node> sceneNode = zge::sceneRootNode(scene);
                    const ee::Ptr<zge::Node> root = zge::rootNode(sceneNode);
                    setGuiModelCallbacks(widget->model->scene, root, 0, 0);
                    setSceneObjectShown(widget->model, 0);
                }
                widget->model->scene = ee::Ptr<zge::Scene>();
            }
            zge::deleteSceneObject(widget->model);
            widget->model = ee::Ptr<zge::SceneObject>();
            widget->modelKept = 0;
        }
        const ee::Ptr<const char> path = widget->assetName;
        if (path && path[0] != 0)
        {
            const ee::Ptr<UiAsset> asset = findModel(path);
            if (asset)
                makeGuiModel(widget, asset);
        }
        drawGuiModel(widget);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(refreshGuiModel, Verified, dm2, refreshListedGuiModel);

    namespace
    {
        constexpr uint32_t kModelAssetType = sym::dm2::g_textGuiModelZAssetID; // "ZAssetID", the guiModel's own copy
        // ZGE2's guiModel objects have a class of their own: the widgets'
        // with another destroy and shown hook (newGuiModel fills it in).
        inline ee::Global<zge::SceneObjectClass, sym::dm2::g_widgetObjectClass> g_widgetObjectClass;
        inline ee::Global<zge::SceneObjectClass, sym::dm2::g_guiModelObjectClass> g_guiModelObjectClass;
    }

    // makeGuiModelObject (ZGE2 only): a scene object of the model asset
    // called `assetName` for the guiModel `record` (its modelKept cleared when
    // the asset is found); 0 without a name or the asset.
    inline ee::Ptr<zge::SceneObject> makeGuiModelObject(ee::Ptr<const char> assetName, ee::Ptr<ListedGuiModel> record)
    {
        uint32_t scene = 0;
        if (!assetName || zge::stringLength(assetName) == 0)
            return {};
        if (const ee::Ptr<UiAsset> asset = findModel(assetName))
        {
            scene = asset->object;
            record->modelKept = 0;
        }
        if (!scene)
            return {};
        return zge::newModelObject(ee::Ptr<zge::Scene>(scene), 0);
    }
    READABLE_FUNCTION_ONLY(dm2, makeGuiModelObject);

    // refreshListedGuiModel: ZGE2's refreshGuiModel, the guiModel's Refresh:
    // the model goes (hidden first if its scene's first byte is 0, where ZGE3
    // tests for a model's scene; its scene let go unless the model is kept)
    // and the AssetName's comes in its place: shown as the widget is,
    // attached to the widget's object, casting no shadow. 1.
    int32_t refreshListedGuiModel(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedGuiModel> record = messageRecord<ListedGuiModel>(message);
        if (const ee::Ptr<zge::SceneObject> model = record->model)
        {
            if (record->modelKept == 0)
            {
                if (static_cast<uint8_t>(model->scene->flags) == 0)
                    setSceneObjectShown(model, 0);
                record->model->scene = ee::Ptr<zge::Scene>();
            }
            zge::deleteSceneObject(record->model);
        }
        const ee::Ptr<zge::SceneObject> made = makeGuiModelObject(record->assetName, record);
        record->model = made;
        if (made)
        {
            const ee::Ptr<zge::SceneObject> widget(message->target);
            setSceneObjectShown(made, widgetObjectShown(widget) ? 1 : 0);
            zge::attachObject(record->model, ee::Ptr<zge::SceneObject>(message->target), {});
            zge::setObjectCastsShadow(record->model, 0);
        }
        return 1;
    }

    // guiModelBounds (ZGE2 only): the guiModel's GetBounds: a fixed rectangle
    // (300, 180) to (340, 200). 1.
    inline int32_t guiModelBounds(ee::Ptr<zge::EngineMessage> message)
    {
        zge::TextBounds &bounds = messageBounds(message);
        bounds.left = 300.0f;
        bounds.bottom = 200.0f;
        bounds.top = 180.0f;
        bounds.right = 340.0f;
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, guiModelBounds);

    // guiModelObjectShown (ZGE2 only): the guiModel class's shown hook: the
    // model shown or hidden with the widget, then what every widget's does.
    inline uint32_t guiModelObjectShown(ee::Ptr<zge::SceneObject> object, uint32_t shown)
    {
        const ee::Ptr<ListedGuiModel> record = classCast<ListedGuiModel>(widgetRecord(object));
        if (record->model)
            setSceneObjectShown(record->model, shown);
        return widgetObjectShown(object, shown);
    }
    READABLE_FUNCTION_ONLY(dm2, guiModelObjectShown);

    // destroyGuiModelObject (ZGE2 only): the guiModel class's destroy: the
    // model deleted (its scene let go first unless it is kept; the original
    // hides the widget's own object there, not the model), then the widget
    // as every widget's destroy does.
    inline uint32_t destroyGuiModelObject(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<ListedGuiModel> record = classCast<ListedGuiModel>(widgetRecord(object));
        if (record->model)
        {
            if (record->modelKept == 0)
            {
                setSceneObjectShown(object, 0);
                record->model->scene = ee::Ptr<zge::Scene>();
            }
            zge::deleteSceneObject(record->model);
        }
        return destroyWidgetObject(object);
    }
    READABLE_FUNCTION_WAITS_ONLY(dm2, destroyGuiModelObject);

    // newGuiModel (ZGE2 only): a script's MODEL block: a guiModel widget
    // object called `name`, of the guiModels' class (the widgets' with the two
    // hooks above), its record with an AssetName property and no model yet,
    // its GetBounds and Refresh subscribed. The object, 0 if none is left.
    inline ee::Ptr<zge::SceneObject> newGuiModel(ee::Ptr<const char> name)
    {
        const ee::Ptr<zge::SceneObject> object = zge::newSceneObject();
        if (!object)
            return {};
        zge::initSceneObject(object, kWidgetObjectKind);
        object->objectClass = &*g_guiModelObjectClass;
        *g_guiModelObjectClass = *g_widgetObjectClass;
        g_guiModelObjectClass->destroy = sym::dm2::destroyGuiModelObject;
        g_guiModelObjectClass->shown = sym::dm2::guiModelObjectShown;
        const ee::Ptr<ListedGuiModel> record(zguiAllocate(*g_heap, sizeof(ListedGuiModel)));
        object->animator = classCast<zge::Animator>(record);
        record->widget.kind = ee::Ptr<const char>(kGuiModelClass);
        defineWidgetRecord(record, ee::Address(name));
        record->assetName = {};
        addListedProperty(ee::Ptr<zge::List>(&record->widget.properties), ee::Ptr<const char>(kAssetNameName),
                          ee::Ptr<const char>(kModelAssetType), valueAddress(record->assetName));
        record->modelKept = 0;
        record->model = {};
        subscribeWidget(object, Message::GetBounds, sym::dm2::guiModelBounds);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiModel);
        return object;
    }
    READABLE_FUNCTION_ONLY(dm2, newGuiModel);

    // defineGuiModelProperties: a "guiModel" widget's defaults, its properties
    // (AssetName, ShowShadow) and the messages it listens to.
    ee::Ptr<GuiModel> defineGuiModelProperties(ee::Ptr<GuiModel> widget, uint32_t name)
    {
        defineWidgetProperties(widget, name);
        widget->assetName = ee::Ptr<const char>();
        widget->model = ee::Ptr<zge::SceneObject>();
        widget->modelKept = 0;
        widget->showShadow = 0;
        zge::quaternionIdentity(&widget->turn);
        defineProperty(&widget->properties[0], widget, kAssetNameName, kZAssetIdType2, &widget->assetName);
        defineProperty(&widget->properties[1], widget, kShowShadowName, kBoolType2, &widget->showShadow);
        addListener(widget, Message::Draw, kDrawGuiModel);
        addListener(widget, Message::Refresh, kRefreshGuiModel);
        return widget;
    }
    READABLE_FUNCTION(defineGuiModelProperties, Verified);
}
