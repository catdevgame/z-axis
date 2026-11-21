// The level's texture animations (texture_effects.h; AI's inlineTextureFX.c,
// BMX XXX's ZGE4 ztexturefx.c): each texture effect locator asks for a
// flipbook, or a scroller of one of six kinds, on a material by its name; the
// world's models using it are found, with the meshes, material lists and
// scene objects the frame walks (animateTextures, faceTexturesToCamera). Was
// games/aggressive's (fx/texture_setup.cpp, effects.cpp); the blocks say it is
// the engine's (layer2).

#include "zge/memory/pool.h"
#include "zge/render/texture_effects.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/regions.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/render/material.h"
#include "zge/scene/locators.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/assets/geo.h"
#include "zge/platform/tasks.h"
#include "zge/platform/platform.h"
#include "zge/render/texture.h"
#include "zge/render/fixed_point.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/texture_effects.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kListInit = sym::initList;         // (list) empty
        constexpr uint32_t kFindLocators = sym::findLocators;     // (list *, kind hash, name hash): the locators of that kind and name (0, 0: all)
        constexpr uint32_t kFreeLocators = sym::freeLocators;     // (list *)
        constexpr uint32_t kSetupTextureEffect = sym::setupFireTextureEffect;
        constexpr uint32_t kUpdateTextureEffects = sym::faceTexturesToCamera; // the task
        constexpr uint32_t kMatchScroller = sym::matchScroller, kCollectMaterials = sym::collectTextureMaterials, kCollectObjects = sym::collectObjectOfClass; // the visits
        constexpr uint32_t kVisitsPage = ee::high(sym::collectTextureMaterials);       // the visit's address, as the original makes it
        constexpr uint32_t kSourceFile = sym::g_textSrcComInlineTextureFXC;       // "src/com/inlineTextureFX.c"
        constexpr uint32_t kFireName = sym::g_textFireA00;         // "FireA00", 8 bytes each to "FireA09"
        constexpr uint32_t kFireFrames = 10;
        constexpr uint32_t kFireNameBytes = 8;

        // The tag's byte a flipbook keeps at +0x5C (the last word of its
        // pictures). Nothing reads it back, in AI or BMX XXX on PS2, Xbox or
        // GameCube, and nothing names it: left unnamed.
        uint8_t &lastPictureByte(Flipbook &flipbook)
        {
            return *reinterpret_cast<uint8_t *>(&flipbook.picture[16]);
        }

        // A locator's kinds.
        constexpr uint32_t kEffectFlipbook = 0x0320F5B6u, kEffectScroller = 0x020F5896u;
        struct TextureEffect
        {
            uint8_t _00[0x20];
            uint32_t kind;       // +0x20
            uint32_t texture;    // +0x24 the material's name hash
            uint32_t _28;
            ee::Ptr<zge::TagRecord> tags; // +0x2C its tag record
        };
        static_assert(offsetof(TextureEffect, kind) == 0x20 && offsetof(TextureEffect, tags) == 0x2C);

        constexpr uint32_t kTagBytes = 2, kTagWords = 8;

        // A flipbook's tags.
        constexpr uint32_t kFlipFireKind = 0, kFlipFrames = 1, kFlipFrameTime = 4, kFlipHold = 8, kFlipMode = 12, kFlipByte = 13, kFlipFirstPicture = 0x10;
        // A scroller's.
        constexpr uint32_t kScrollKind = 0, kScrollDu = 4, kScrollDv = 8, kScrollPeriod = 12, kScrollWrapU = 0x10, kScrollWrapV = 20;
        constexpr uint32_t kScrollMode = 24, kScrollFacing = 25, kScrollOwner = 28, kScrollNodeName = 0x20, kScrollAxisU = 36, kScrollAxisV = 37;

        // A world model looked for, what the visits hand on.
        struct TextureSearch
        {
            uint32_t texture;       // +0x00
            uint32_t withObjects;   // +0x04 the scene objects of the model's class too
            ee::Ptr<uint32_t> users; // +0x08 where the first TextureUser goes (the effect's users)
        };

        // The search for a class's scene objects.
        struct ObjectSearch
        {
            uint32_t modelClass;    // +0x00
            ee::Ptr<uint32_t> nodes; // +0x04 where the first goes (the user's nodes)
        };

        // {object or material, next}
        struct ListEntry
        {
            uint32_t item;
            uint32_t next;
        };
        // The same, in a user's list of materials.
        struct MaterialEntry
        {
            ee::Ptr<zge::Material> material;
            uint32_t next;
        };

        inline ee::Global<ee::Ptr<zge::Task>, sym::g_textureFxTask> g_textureFxTask;
        // ZGE4 runs animateTextures as a task of its own (stage 7), made after the level's effects are set up.
        inline ee::Global<ee::Ptr<zge::Task>, sym::bmx::g_animateTexturesTask> g_animateTexturesTask;
        constexpr uint32_t kAnimateTexturesStage = 7;
        // newTask's line numbers (ZGE4's file is ZgeMods/UVanim/ztexturefx.c).
        constexpr int32_t kTextureFxTaskLine = game::readable::perGame(184, 184, 188);
        constexpr int32_t kAnimateTexturesTaskLine = 206;

        // A list's chain freed.
        void freeChain(uint32_t first)
        {
            uint32_t entry = first;
            for (;;)
            {
                const uint32_t next = ee::Ptr<ListEntry>(entry)->next;
                zge::heapFree(entry);
                entry = next;
                if (entry == 0)
                    break;
            }
        }

        // An animation's users freed, with what each user holds.
        void freeUsers()
        {
            for (;;)
            {
                ee::Ptr<TextureUser> user(*g_textureUser);
                *g_textureNode = user->nodes;
                if (user->nodes != 0)
                {
                    for (;;)
                    {
                        const uint32_t entry = *g_textureNode;
                        const uint32_t following = ee::Ptr<ListEntry>(entry)->next;
                        zge::heapFree(entry);
                        *g_textureNode = following;
                        if (following == 0)
                            break;
                    }
                    user = ee::Ptr<TextureUser>(*g_textureUser);
                }
                if (user->materials != 0)
                {
                    freeChain(user->materials);
                    user = ee::Ptr<TextureUser>(*g_textureUser);
                }
                const uint32_t nextUser = user->next;
                zge::heapFree(user);
                *g_textureUser = nextUser;
                if (nextUser == 0)
                    break;
            }
        }
    }

    int32_t setupFireTextureEffect(ee::Ptr<TextureEffect> effect); // (below)

    namespace // the game functions below call: ee/plain.h
    {
        // freeLocators: the list's locators back to their pool. By address: the caller
        // returns what the (void) function leaves in the result register.
        int32_t freeLocatorsKeepingResult(ee::Ptr<zge::LocatorList> list) { return ee::game<int32_t>(sym::freeLocators, list); }
    }
    // initTextureFx: the level's locators of texture effects read, the task
    // that runs them each frame made.
    int32_t initTextureFx()
    {
        ee::Local<zge::LocatorList> locators;
        *g_scrollers = 0;
        *g_scroller = 0;
        *g_flipbooks = 0;
        *g_flipbook = 0;
        *g_textureFxTask = zge::newTask(1, kUpdateTextureEffects, 0, 1, kSourceFile, kTextureFxTaskLine);
        zge::initList(locators);
        zge::findLocators(locators, 0, 0);
        for (ee::Ptr<zge::Locator> locator = locators->head; locator; locator = locator->next)
            setupFireTextureEffect(ee::Ptr<TextureEffect>(locator->point));
        if constexpr (game::readable::kIsBmx)
        {
            freeLocatorsKeepingResult(locators);
            const ee::Ptr<zge::Task> task = zge::newTask(kAnimateTexturesStage, sym::animateTextures, 0, 1, kSourceFile, kAnimateTexturesTaskLine);
            *g_animateTexturesTask = task;
            return static_cast<int32_t>(ee::Address(task)); // ZGE4 returns the task, as the word it is
        }
        return freeLocatorsKeepingResult(locators);
    }
    READABLE_FUNCTION_WAITS(initTextureFx, Verified);

    // freeTextureEffects: all of it freed, at the level's end.
    int32_t freeTextureEffects()
    {
        if (*g_textureFxTask != 0)
            zge::deleteTask(*g_textureFxTask);
        if constexpr (game::readable::kIsBmx)
        {
            if (*g_animateTexturesTask != 0)
                zge::deleteTask(*g_animateTexturesTask);
        }
        *g_flipbook = *g_flipbooks;
        if (*g_flipbooks != 0)
        {
            for (;;)
            {
                const ee::Ptr<Flipbook> flipbook(*g_flipbook);
                const uint32_t next = flipbook->next;
                *g_textureUser = flipbook->users;
                if (flipbook->users != 0)
                    freeUsers();
                zge::heapFree(*g_flipbook);
                *g_flipbook = next;
                if (next == 0)
                    break;
            }
        }
        *g_scroller = *g_scrollers;
        int32_t result = static_cast<int32_t>(*g_scrollers);
        if (*g_scrollers != 0)
        {
            for (;;)
            {
                const ee::Ptr<Scroller> scroller(*g_scroller);
                const uint32_t next = scroller->next;
                *g_textureUser = scroller->users;
                if (scroller->users != 0)
                    freeUsers();
                result = zge::heapFree(*g_scroller);
                *g_scroller = next;
                if (next == 0)
                    break;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(freeTextureEffects, Verified);

    // matchScroller: a walk of a model's nodes: the one of that name becomes
    // the scroller's, and stops the walk.
    uint32_t matchScroller(uint32_t node, ee::Ptr<uint32_t> name)
    {
        if (ee::Ptr<zge::Node>(node)->nameHash == *name)
        {
            ee::Ptr<Scroller>(*g_scroller)->node = node;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(matchScroller, Verified);

    // setupFireTextureEffect: one locator's effect made: a flipbook (the Fire pictures
    // for a kind 1, else its tags'), or a scroller; the world's models that
    // use its material found. BMX XXX's flipbook finds the models' scene
    // objects too, AI's does not. Kept bug: the scroller's search never sets
    // withObjects, so it is the stack's word there: what the last flipbook's
    // search left (AI: 0 on all 404 recorded calls; BMX XXX: 1).
    int32_t setupFireTextureEffect(ee::Ptr<TextureEffect> effect)
    {
        ee::Local<TextureSearch> search;
        ee::Local<zge::Matrix> matrix;
        ee::Local<uint32_t> names;
        ee::Local<uint32_t> context;
        const uint32_t kind = effect->kind, texture = effect->texture;
        const auto tagByte = [&](uint32_t offset)
        {
            return zge::tagByte(effect->tags, kTagBytes, offset);
        };
        const auto tagFloat = [&](uint32_t offset)
        {
            return zge::tagFloat(effect->tags, kTagWords, offset);
        };
        const auto tagWord = [&](uint32_t offset)
        {
            return zge::tagWord(effect->tags, kTagWords, offset);
        };
        const auto pictureOf = [&](uint32_t hash)
        {
            const uint32_t found = findTextureByHash(hash, 0);
            return zge::pictureLevel(ee::Ptr<zge::Material>(found), 0);
        };
        if (kind == kEffectFlipbook)
        {
            uint32_t block = 0;
            if (*g_flipbook == 0)
            {
                block = zge::heapAllocate(sizeof(Flipbook));
                *g_flipbooks = block;
            }
            else
            {
                block = zge::heapAllocate(sizeof(Flipbook));
                ee::Ptr<Flipbook>(*g_flipbook)->next = block;
            }
            *g_flipbook = block;
            ee::Ptr<Flipbook> flipbook(block);
            const uint32_t fire = tagByte(kFlipFireKind);
            if (fire == 1)
            {
                flipbook->last = kFireFrames - 1;
                flipbook->mode = fire;
                lastPictureByte(*flipbook) = 2;
                flipbook->hold = ee::Float(0.05f);
                flipbook->frameTime = ee::Float(0.05f);
                for (uint32_t i = 0; i < kFireFrames; ++i)
                {
                    const uint32_t hash = zge::nameHash(ee::Ptr<const char>(kFireName) + i * kFireNameBytes);
                    flipbook->picture[i] = pictureOf(hash);
                }
            }
            else
            {
                const uint32_t frames = tagByte(kFlipFrames);
                flipbook->last = static_cast<uint8_t>(frames - 1);
                flipbook->frameTime = tagFloat(kFlipFrameTime);
                flipbook->hold = tagFloat(kFlipHold);
                flipbook->mode = tagByte(kFlipMode);
                lastPictureByte(*flipbook) = static_cast<uint8_t>(tagByte(kFlipByte));
                for (uint32_t i = 0; i < frames; ++i)
                {
                    const uint32_t hash = tagWord(kFlipFirstPicture + i * 4);
                    if constexpr (game::readable::kIsBmx)
                    {
                        // ZGE4 gives a tag's picture that no texture has none (ZGE3 asks pictureLevel of 0).
                        const uint32_t found = findTextureByHash(hash, 0);
                        flipbook->picture[i] = found ? zge::pictureLevel(ee::Ptr<zge::Material>(found), 0) : 0u;
                    }
                    else
                        flipbook->picture[i] = pictureOf(hash);
                }
            }
            flipbook->frame = 0;
            flipbook->rising = 1;
            flipbook->next = 0;
            flipbook->timer = flipbook->frameTime;
            flipbook->texture = texture;
            flipbook->users = 0;
            search->texture = texture;
            search->users = &flipbook->users;
            *g_textureUser = 0;
            // ZGE4's flipbook finds the scene objects of the models' classes too (ZGE3's none).
            search->withObjects = game::readable::kIsBmx ? 1u : 0u;
            const int32_t walked = zge::forEachWorldObject(kCollectMaterials, search);
            return walked;
        }
        if (kind != kEffectScroller)
            return static_cast<int32_t>(kEffectScroller);
        uint32_t block = 0;
        if (*g_scroller == 0)
        {
            block = zge::heapAllocate(sizeof(Scroller));
            *g_scrollers = block;
        }
        else
        {
            block = zge::heapAllocate(sizeof(Scroller));
            ee::Ptr<Scroller>(*g_scroller)->next = block;
        }
        *g_scroller = block;
        const ee::Ptr<Scroller> scroller(block);
        scroller->kind = static_cast<uint8_t>(tagByte(kScrollKind));
        const auto rates = [&](uint32_t table)
        {
            scroller->du = zge::tagFloat(effect->tags, table, kScrollDu);
            scroller->dv = tagFloat(kScrollDv);
            scroller->period = tagFloat(kScrollPeriod);
            scroller->mode = tagByte(kScrollMode);
            scroller->facing = static_cast<uint8_t>(tagByte(kScrollFacing));
        };
        const auto fixedRate = [&](ee::Float dv, ee::Float du)
        {
            scroller->mode = 1;
            scroller->dv = dv;
            scroller->facing = 0;
            scroller->stepped = 0;
            scroller->du = du;
            scroller->period = 0;
        };
        const uint32_t row = static_cast<uint32_t>(scroller->kind) - 1;
        if (row >= 6)
        {
            scroller->stepped = 0;
            rates(kTagWords);
        }
        else
            switch (row + 1)
            {
            case 6: // by a node's moves
            {
                scroller->stepped = 0;
                rates(kTagWords);
                const uint32_t ownerName = tagWord(kScrollOwner);
                *names = ownerName;
                const uint32_t nodeName = tagWord(kScrollNodeName);
                *context = nodeName;
                if (ownerName != 0)
                {
                    const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findObjects(names, 1);
                    if (slot)
                    {
                        const ee::Ptr<zge::Node> geometry = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(*slot));
                        if (geometry != 0)
                            zge::forEachNodeInSubtree(geometry, kMatchScroller, context);
                    }
                }
                else
                {
                    if (nodeName != 0)
                    {
                        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findObjects(context, 1);
                        if (slot)
                        {
                            const ee::Ptr<zge::Node> geometry = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(*slot));
                            if (geometry != 0)
                                scroller->node = ee::Address(geometry);
                        }
                    }
                }
                // Where the node is now.
                zge::matrixIdentity(matrix);
                *matrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(ee::Ptr<Scroller>(*g_scroller)->node), 1);
                ee::Ptr<Scroller>(*g_scroller)->lastAt = zge::matrixRow(*matrix, 3);
                scroller->axisU = static_cast<uint8_t>(tagByte(kScrollAxisU));
                scroller->axisV = static_cast<uint8_t>(tagByte(kScrollAxisV));
                break;
            }
            case 1: // a step each period
                scroller->stepped = 1;
                rates(kTagWords);
                break;
            case 2:
                fixedRate(ee::Float(-5.0f), ee::Float(0.0f));
                break;
            case 3:
                fixedRate(ee::Float(-2.0f), ee::Float(0.0f));
                break;
            case 4:
                fixedRate(ee::Float(0.15f), ee::Float(0.15f));
                break;
            default: // 5
                fixedRate(ee::Float(0.1f), ee::Float(0.1f));
                break;
            }
        if (scroller->mode == 2)
        {
            scroller->wrapU = tagFloat(kScrollWrapU);
            scroller->wrapV = tagFloat(kScrollWrapV);
        }
        else
        {
            scroller->wrapU = ee::Float(1.0f);
            scroller->wrapV = ee::Float(1.0f);
        }
        scroller->timer = 0;
        scroller->u = 0;
        scroller->v = 0;
        scroller->bounceU = 0;
        scroller->bounceV = 0;
        scroller->next = 0;
        scroller->texture = texture;
        scroller->users = 0;
        search->texture = texture;
        search->users = &scroller->users;
        *g_textureUser = 0;
        // (withObjects not set: the original leaves the stack's word, see above)
        const int32_t walked = zge::forEachWorldObject(kCollectMaterials, search);
        return walked;
    }
    READABLE_FUNCTION_WAITS(setupFireTextureEffect, Verified);

    // collectTextureMaterials: a world model: the materials of its mesh with the
    // texture's name listed; a model with one gets a user, with (if the
    // search asks) the scene objects of its class.
    int32_t collectTextureMaterials(uint32_t modelClass, ee::Ptr<TextureSearch> search)
    {
        ee::Local<ObjectSearch> objects;
        ee::Local<uint32_t> modelClassSlot;
        ee::Local<ee::Ptr<TextureSearch>> searchSlot; // (the original keeps the search and the class in its frame)
        *searchSlot = search;
        *modelClassSlot = modelClass;
        const uint32_t texture = search->texture;
        const ee::Ptr<zge::CollisionFrame> model = zge::classModel(ee::Ptr<zge::GeoObject>(modelClass), 0);
        const ee::Ptr<zge::FramePart> part = zge::frameLevelPart(model, 0);
        const ee::Ptr<zge::ZgePart> mesh = zge::framePartMesh(part);
        // The materials of that texture, from every material set of the mesh.
        uint32_t first = 0;
        uint32_t last = 0;
        for (uint32_t set = 0; set < static_cast<uint32_t>(zge::materialSetCount(mesh)); ++set)
        {
            const ee::Ptr<zge::MaterialTable> table = zge::meshMaterials(mesh, set);
            if (!table)
                continue;
            const ee::Ptr<ee::Ptr<zge::Material>> materials(&table->materials[0]);
            const int32_t count = table->count;
            for (int32_t i = 0; i < count; ++i)
            {
                const ee::Ptr<zge::Material> material = materials[i];
                if (zge::materialNameHash(material) != texture)
                    continue;
                const uint32_t entry = zge::heapAllocate(sizeof(ListEntry));
                if (last == 0)
                    first = entry;
                else
                    ee::Ptr<ListEntry>(last)->next = entry;
                last = entry;
                ee::Ptr<MaterialEntry>(last)->material = material;
                ee::Ptr<ListEntry>(last)->next = 0;
            }
        }
        if (last != 0)
        {
            const uint32_t added = zge::heapAllocate(sizeof(TextureUser));
            if (*g_textureUser == 0)
                *search->users = added;
            else
                ee::Ptr<TextureUser>(*g_textureUser)->next = added;
            *g_textureUser = added;
            const ee::Ptr<TextureUser> user(added);
            user->mesh = ee::Address(mesh);
            user->materials = first;
            user->next = 0;
            user->model = modelClass;
            user->nodes = 0;
            objects->modelClass = modelClass;
            objects->nodes = &user->nodes;
            *g_textureNode = 0;
            if (search->withObjects != 0)
                zge::forEachWorldMesh(kCollectObjects, objects);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(collectTextureMaterials, Verified);

    // collectObjectOfClass: a visit: an object of the class listed.
    int32_t collectObjectOfClass(ee::Ptr<zge::Node> object, ee::Ptr<ObjectSearch> search)
    {
        // (ZGE4 reads the node's model itself, where ZGE3 calls objectClass.)
        const ee::Ptr<zge::GeoObject> model = game::readable::kIsBmx ? object->model : zge::objectClass(object);
        if (model == ee::Ptr<zge::GeoObject>(search->modelClass))
        {
            const uint32_t entry = zge::heapAllocate(sizeof(ListEntry));
            if (*g_textureNode == 0)
                *search->nodes = entry;
            else
                ee::Ptr<ListEntry>(*g_textureNode)->next = entry;
            *g_textureNode = entry;
            ee::Ptr<ListEntry>(entry)->item = ee::Address(object);
            ee::Ptr<ListEntry>(entry)->next = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(collectObjectOfClass, Draft);

    namespace
    {

        // An offset in 1/4096 steps, kept to the 16 bits of a UV stream's
        // entry (+-8) as the original.
        ee::Float fixedStep(ee::Float x)
        {
            return uvFromFixed(static_cast<int16_t>(fixedUv(x)));
        }

        // A scroller's texture matrix: the identity moved by its offset (by
        // its bounce offset when `bounces` and it goes back and forth), set on
        // every material that has a texture matrix, last first, of the mesh
        // of each of its users (g_textureUser walks them from the first).
        // The translation row takes the whole of `offset`, whose w nothing
        // here writes: the matrix keeps what the stack held there, as the
        // original's does.
        void applyScroll(const Scroller &s, bool bounces, const ee::Local<zge::Matrix> &matrix, const ee::Local<zge::Vec4> &offset)
        {
            do
            {
                matrixIdentity(matrix);
                ee::Float u = s.u, v = s.v;
                if (bounces && s.mode == 2)
                    u = s.bounceU, v = s.bounceV;
                offset->x = u;
                offset->y = v;
                offset->z = ee::Float(0.0f);
                matrix->translation() = *offset;
                if (const ee::Ptr<zge::MaterialTable> table = zge::meshMaterials(ee::Ptr<zge::ZgePart>(ee::Ptr<TextureUser>(*g_textureUser)->mesh), 0))
                {
                    const int32_t count = zge::materialCount(table);
                    if (count != 0)
                    {
                        for (int32_t index = count - 1; index != -1; --index)
                        {
                            const ee::Ptr<zge::Material> material = zge::materialAt(table, index);
                            if (material && zge::hasTextureMatrix(material))
                                zge::setTextureMatrix(material, &matrix->row(0));
                        }
                    }
                }
                *g_textureUser = ee::Ptr<TextureUser>(*g_textureUser)->next;
            } while (*g_textureUser);
        }
    }

    // animateTextures: the levels' texture animations, `seconds` on. Flipbooks
    // step their frame when their timer runs out (up or down, holding at the
    // ends) and set the frame's picture on their materials; scrollers move
    // their texture offset (by a rate, a step a period, or as a node moves),
    // wrap it, and set it as their materials' texture matrix. The globals
    // g_flipbook, g_scroller and g_textureUser walk the lists, as the
    // original's do.
    namespace
    {
    // (In line in both its callers: ZGE4's task must not reach it through animateTextures' own entry.)
    [[gnu::always_inline]] inline void advanceTextureEffects(ee::Float seconds)
    {
        ee::Local<zge::Matrix> nodeWorld; // a node scroller's node's world matrix
        ee::Local<zge::Vec4> nodeAt;
        ee::Local<zge::Matrix> scroll;    // the texture matrix
        ee::Local<zge::Vec4> offset;      // its translation row: u, v, 0
        for (*g_flipbook = *g_flipbooks; *g_flipbook; *g_flipbook = ee::Ptr<Flipbook>(*g_flipbook)->next)
        {
            Flipbook &f = *ee::Ptr<Flipbook>(*g_flipbook);
            if (!(ee::Float(0.0f) < f.timer))
                continue;
            f.timer = f.timer - seconds;
            if (!(f.timer <= ee::Float(0.0f)))
                continue;
            const ee::Float endTime = !(f.hold == ee::Float(0.0f)) ? f.hold : f.frameTime;
            if (!f.rising)
            {
                f.frame = static_cast<uint8_t>(f.frame - 1);
                if (f.frame == 0)
                {
                    f.rising = 1;
                    f.timer = endTime;
                }
                else
                    f.timer = f.frameTime;
            }
            else
            {
                f.frame = static_cast<uint8_t>(f.frame + 1);
                if (f.last < f.frame)
                    f.frame = 0;
                if (f.frame != f.last)
                    f.timer = f.frameTime;
                else if (f.mode == 1 || f.mode == 2)
                {
                    if (f.mode == 2)
                        f.rising = 0;
                    f.timer = endTime;
                }
                // (Any other mode stops at the last frame: its timer stays run out.)
            }
            const uint32_t picture = f.picture[f.frame];
            for (*g_textureUser = f.users; *g_textureUser; *g_textureUser = ee::Ptr<TextureUser>(*g_textureUser)->next)
            {
                for (ee::Ptr<MaterialLink> link(ee::Ptr<TextureUser>(*g_textureUser)->materials); link; link = ee::Ptr<MaterialLink>(link->next))
                    zge::setMaterialPicture(ee::Ptr<zge::Material>(link->material), picture, 0);
            }
        }

        for (*g_scroller = *g_scrollers; *g_scroller; *g_scroller = ee::Ptr<Scroller>(*g_scroller)->next)
        {
            Scroller &s = *ee::Ptr<Scroller>(*g_scroller);
            if (s.kind == 6)
            {
                // By how far the node moved along each of its two axes.
                matrixIdentity(nodeWorld);
                *nodeWorld = *nodeMatrix(ee::Ptr<zge::Node>(s.node), 1);
                *nodeAt = nodeWorld->translation();
                const auto axis = [](const zge::Vec4 &p, uint8_t a) { return a == 0 ? p.x : a == 1 ? p.y : p.z; };
                const ee::Float alongU = axis(*nodeAt, s.axisU) - axis(s.lastAt, s.axisU);
                const ee::Float alongV = (axis(*nodeAt, s.axisV) - axis(s.lastAt, s.axisV)) * s.dv;
                s.u = s.u + fixedStep(alongU * s.du);
                s.v = s.v + fixedStep(alongV);
                s.lastAt = *nodeAt;
                *g_textureUser = s.users;
                if (*g_textureUser)
                    applyScroll(s, false, scroll, offset);
                continue;
            }

            if (seconds < s.timer)
            {
                s.timer = s.timer - seconds;
                continue;
            }
            // A step each period, or a rate a second.
            const ee::Float stepU = s.stepped ? fixedStep(s.du) : fixedStep(s.du * seconds);
            const ee::Float stepV = s.stepped ? fixedStep(s.dv) : fixedStep(s.dv * seconds);
            s.bounceU = s.bounceU + stepU;
            s.u = s.u + stepU;
            if (s.stepped)
                s.timer = s.period;
            s.bounceV = s.bounceV + stepV;
            s.v = s.v + stepV;

            // Wrapped back into -wrap .. wrap; one going back and forth holds
            // its bounce offset at the end it reached.
            const bool bounce = s.mode == 2;
            bool wrappedU = false, wrappedV = false;
            if (s.wrapU < s.u)
            {
                if (bounce)
                    s.bounceU = s.wrapU;
                wrappedU = true;
                s.u = s.u - s.wrapU;
            }
            if (s.u < -s.wrapU)
            {
                if (bounce)
                    s.bounceU = ee::Float(0.0f);
                wrappedU = true;
                s.u = s.u + s.wrapU;
            }
            if (s.wrapV < s.v)
            {
                if (bounce)
                    s.bounceV = s.wrapV;
                wrappedV = true;
                s.v = s.v - s.wrapV;
            }
            if (s.v < -s.wrapV)
            {
                if (bounce)
                    s.bounceV = ee::Float(0.0f);
                wrappedV = true;
                s.v = s.v + s.wrapV;
            }
            // Done when both have wrapped, or one has and the other does not move.
            const bool done = (wrappedV && (wrappedU || s.du == ee::Float(0.0f))) || (wrappedU && (wrappedV || s.dv == ee::Float(0.0f)));
            *g_textureUser = s.users;
            if (*g_textureUser)
                applyScroll(s, true, scroll, offset);
            if (!done)
                continue;
            if (s.mode == 1)
                s.timer = s.period;
            else if (s.mode == 0)
            {
                s.dv = ee::Float(0.0f);
                s.du = ee::Float(0.0f);
            }
            else if (s.mode == 2)
            {
                const ee::Float dv = -s.dv;
                s.du = -s.du;
                s.dv = dv;
                s.timer = s.period;
            }
        }
    }
    }

    void animateTextures(ee::Float seconds)
    {
        advanceTextureEffects(seconds);
    }

    // animateTexturesTask (ZGE4's animateTextures: the task initTextureFx makes, with no seconds handed in): the
    // frame's seconds on. 1 (the task goes on).
    uint32_t animateTexturesTask()
    {
        advanceTextureEffects(frameSeconds());
        return 1;
    }
    READABLE_FUNCTION_OWN(animateTextures, Verified, bmx, animateTexturesTask);

    namespace
    {
        inline ee::Global<uint32_t, sym::g_currentCamera> g_currentCamera;

        // The current user's nodes (g_textureNode walks them) that were drawn
        // in the camera's last frame turned to `facing`, each keeping its own
        // translation.
        void faceNodes(const ee::Local<zge::Matrix> &facing)
        {
            do
            {
                const ee::Ptr<NodeLink> link(*g_textureNode);
                if (!nodeNotDrawn(ee::Ptr<zge::Node>(link->node)))
                {
                    zge::copyTranslation(facing, nodeMatrix(ee::Ptr<zge::Node>(ee::Ptr<NodeLink>(*g_textureNode)->node), 1));
                    setNodeLocalMatrix(ee::Ptr<zge::Node>(ee::Ptr<NodeLink>(*g_textureNode)->node), facing);
                }
                *g_textureNode = ee::Ptr<NodeLink>(*g_textureNode)->next;
            } while (*g_textureNode);
        }

        // Each of the current animation's users (g_textureUser walks them)
        // turned to the camera by `facing`.
        void faceUsers(uint8_t facing, const ee::Local<zge::Matrix> &turned)
        {
            do
            {
                const uint32_t camera = facing == kFaceCamera || facing == kFaceCameraUpright ? *g_currentCamera : 0;
                if (camera)
                {
                    const ee::Ptr<zge::Matrix> cameraMatrix = zge::cachedMatrix(ee::Ptr<zge::MatrixCache>(camera), facing == kFaceCamera ? 2 : 3);
                    *turned = zge::aligned(cameraMatrix);
                    *g_textureNode = ee::Ptr<TextureUser>(*g_textureUser)->nodes;
                    if (*g_textureNode)
                        faceNodes(turned);
                }
                *g_textureUser = ee::Ptr<TextureUser>(*g_textureUser)->next;
            } while (*g_textureUser);
        }
    }

    // faceTexturesToCamera (inlineTextureFX.c): the nodes of the texture
    // animations' users (flipbooks, then scrollers) that face the camera
    // turned to it, those drawn in its last frame. Returns 1.
    uint32_t faceTexturesToCamera()
    {
        ee::Local<zge::Matrix> turned; // the camera's matrix for the facing
        for (*g_flipbook = *g_flipbooks; *g_flipbook; *g_flipbook = ee::Ptr<Flipbook>(*g_flipbook)->next)
        {
            *g_textureUser = ee::Ptr<Flipbook>(*g_flipbook)->users;
            if (*g_textureUser)
                faceUsers(flipbookFacing(*ee::Ptr<Flipbook>(*g_flipbook)), turned);
        }
        for (*g_scroller = *g_scrollers; *g_scroller; *g_scroller = ee::Ptr<Scroller>(*g_scroller)->next)
        {
            *g_textureUser = ee::Ptr<Scroller>(*g_scroller)->users;
            if (*g_textureUser)
                faceUsers(scrollerFacing(*ee::Ptr<Scroller>(*g_scroller)), turned);
        }
        return 1;
    }
    READABLE_FUNCTION(faceTexturesToCamera, Verified);
}
