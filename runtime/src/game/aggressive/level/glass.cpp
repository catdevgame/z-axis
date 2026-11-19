// The level's glass (gamehand13; were the sweep's drafts of initGlass ..
// breakGlass): the "Glass" level object type (registerLevelObjectTypes,
// load.cpp), panes the skater breaks by riding into them. Its init shows the
// pane and makes it collidable unless it is already broken (its tag byte
// 28, kept in the object's own tag data), and subscribes its "Collide"
// handler: the first collision hides the pane (no longer collidable), tells
// the script object its tag word 0x20 names "glassBreak", throws a piece of
// debris from the centre of each face of its model (along the face's
// normal, with the skater's velocity, the kind its tag byte 36 gives),
// plays the main bank's sample 49, marks the pane broken and lets the
// skater through (the collision's push off cleared, move_collide.cpp).
//

#include "matrix_points.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/math/vec.h"
#include "zge/collision/collision.h"
#include "zge/render/part.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/assets/geo.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/library.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/glass.cpp.h"

namespace zge
{
    struct TagRecord;
}
namespace aggressive
{
}

namespace aggressive::level
{
    namespace
    {
        constexpr uint32_t kSendEngineMessage = sym::sendEngineMessage; // zge::sendEngineMessage, by address (hooked)
        constexpr uint32_t kBreakGlass = sym::breakGlass;

        constexpr uint32_t kCollideName = sym::collideName2;     // "Collide"
        constexpr uint32_t kGlassBreakName = sym::glassBreakName;  // "glassBreak"

        // The object's tag data (objectComponent).
        constexpr uint32_t kBrokenTag = 28;     // byte: broken
        constexpr uint32_t kScriptTag = 0x20;   // word: the name hash of the script object told "glassBreak"
        constexpr uint32_t kDebrisTag = 36;     // byte: the debris' kind
        constexpr uint32_t kTagByte = 2, kTagWord = 8; // tagByte's and tagWord's second argument

        // The sound (soundEvent 47: the main bank's sample b placed at a, at
        // level c).
        constexpr uint32_t kPlacedSample = 47;
        constexpr uint32_t kBreakSample = 49;
        constexpr uint32_t kBreakLevel = 127;

        // "Collide"'s data (the skater's collision with a wall's object,
        // move_collide.cpp).
        struct CollideEvent
        {
            uint32_t surface;   // +0x00 the face's surface kind
            ee::Float speed;    // +0x04 the skater's
            uint8_t _08[8];
            zge::Vec4 velocity; // +0x10 the skater's
            uint32_t pushOff;   // +0x20 1 (the sender's): the skater is pushed off; 0 it goes through
        };
        static_assert(offsetof(CollideEvent, velocity) == 0x10 && offsetof(CollideEvent, pushOff) == 0x20);
    }

    uint32_t glassBroken(ee::Ptr<zge::SceneObject> glass);

    // initGlass (the "Glass" type's init, a scene object): shown and
    // collidable unless broken; "Collide" subscribed (breakGlass).
    uint32_t initGlass(ee::Ptr<zge::SceneObject> glass)
    {
        const ee::Ptr<zge::Node> geometry(zge::objectLevelNode(glass));
        if (glassBroken(glass) == 0)
        {
            zge::setNodeShown(geometry, 1, 0);
            zge::setNodeCollides(geometry, 1, 0);
        }
        else
        {
            zge::setNodeShown(geometry, 0, 0);
            zge::setNodeCollides(geometry, 0, 0);
        }
        const uint32_t collide = zge::nameHash(ee::Ptr<const char>(kCollideName));
        zge::subscribe(glass, collide, kBreakGlass);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initGlass, Verified);

    // glassBroken: whether the pane is broken (its tag byte 28).
    uint32_t glassBroken(ee::Ptr<zge::SceneObject> glass)
    {
        const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(glass);
        return zge::tagByte(tags, kTagByte, kBrokenTag);
    }
    READABLE_FUNCTION(glassBroken, Verified);

    // setGlassBroken: the pane marked broken (or not).
    void setGlassBroken(ee::Ptr<zge::SceneObject> glass, uint32_t broken)
    {
        const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(glass);
        game::tail(zge::setTagByte, tags, kTagByte, kBrokenTag, broken & 0xFFu);
    }
    READABLE_FUNCTION(setGlassBroken, Verified);

    namespace
    {
        ee::Ptr<zge::ZgePart> partMesh(ee::Ptr<zge::FramePart> part) { return zge::framePartMesh(part); }

        // Where a face's centre sum starts.
        const zge::Vec4 kEmptySum = {ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
    }

    // breakGlass ("Collide", an EngineMessage whose data is a
    // CollideEvent): the pane broken (see the top). Always 1.
    uint32_t breakGlass(ee::Ptr<zge::EngineMessage> collision)
    {
        ee::Local<zge::EngineMessage> glassBreak;
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Vec4> vertex;
        ee::Local<uint32_t> scriptName;
        ee::Local<ee::Ptr<uint16_t>> vertexIndices;
        const ee::Ptr<zge::SceneObject> glass(collision->target);
        const ee::Ptr<CollideEvent> event(collision->data);

        if (glassBroken(glass) == 1)
        {
            event->pushOff = 0;
            return 1;
        }

        // Hidden, no longer collidable; the script object told.
        const ee::Ptr<zge::Node> geometry(zge::objectLevelNode(glass));
        zge::setNodeShown(geometry, 0, 0);
        zge::setNodeCollides(geometry, 0, 0);
        *scriptName = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(ee::Ptr<zge::SceneObject>(glass)), kTagWord, kScriptTag));
        if (*scriptName != 0)
        {
            const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(scriptName, 1);
            if (found)
            {
                glassBreak->flags = zge::kMessageNow;
                glassBreak->target = *found;
                glassBreak->sender = 0;
                glassBreak->data = 0;
                glassBreak->type = zge::nameHash(ee::Ptr<const char>(kGlassBreakName));
                zge::sendEngineMessage(glassBreak);
            }
        }
        const uint32_t debrisKind = zge::tagByte(zge::objectComponent(ee::Ptr<zge::SceneObject>(glass)), kTagByte, kDebrisTag);

        // A piece of debris from each face of the model.
        const ee::Ptr<zge::CollisionFrame> model = zge::classModel(zge::objectClass(geometry), 0);
        if (model && zge::frameLevels(model) != 0)
        {
            const ee::Ptr<zge::FramePart> part = zge::frameLevelPart(model, 0);
            const ee::Ptr<zge::ZgePart> mesh = partMesh(part);
            *matrix = *zge::nodeMatrix(geometry, 1);
            const uint32_t faces = mesh->faceCount;
            for (uint32_t face = 0; face < faces; ++face)
            {
                const uint32_t corners = zge::faceVertices(mesh, face, vertexIndices) & 0xFFu;
                zge::faceNormal(mesh, face, normal);
                *normal = zge::rotated(*matrix, *normal);
                *centre = kEmptySum;
                for (uint32_t corner = 0; corner < corners; ++corner)
                {
                    zge::vertexPosition(mesh, (*vertexIndices)[corner], vertex);
                    *vertex = zge::transformed(*matrix, *vertex);
                    *centre = *centre + *vertex;
                }
                *centre = *centre * (ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(corners)));
                aggressive::fx::debris(centre, &event->velocity, normal, debrisKind);
            }
        }

        // The sound; broken; the skater goes through.
        soundEvent(kPlacedSample, zge::objectPosition(glass), kBreakSample, kBreakLevel, 0);
        setGlassBroken(glass, 1);
        event->pushOff = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(breakGlass, Verified);
}
