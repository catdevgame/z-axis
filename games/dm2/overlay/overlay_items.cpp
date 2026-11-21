// DM2's screen overlay items (overlay_items.h): the table of items, its two
// groups, and the task that draws the shown group. DM2's own code
// (zgeif/zoverlay.c in its build).

#include "overlay_items.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/render/primitives.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/overlay_items.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kItems = 0x400;
        constexpr uint32_t kItemIndexMask = 0xFFF; // a handle's low bits; the group is above them
        constexpr uint32_t kGroupShift = 12;
        constexpr uint32_t kBlendModes = 4;
        constexpr uint32_t kMaterialSets = 16;
        constexpr uint32_t kCorners = 4;
        constexpr uint8_t kKindMask = 0x0F;
        constexpr uint32_t kBlendShift = 4;

        // More than 0 while a function here is at work on the table: the
        // task does not draw then.
        inline ee::Global<uint32_t, sym::g_overlayItemsBusy> g_busy;
        inline ee::Global<uint8_t, sym::g_overlayGroupShown> g_groupShown;
        // Scaffolding (unnamed-field): a byte the set-up clears; nothing
        // read so far uses it.
        inline ee::Global<uint8_t, sym::g_overlayItemsByte5> g_byte5;
        // Where the search for a free place starts.
        inline ee::Global<uint16_t, sym::g_overlayNextFree> g_nextFree;
        inline ee::Global<ee::Ptr<OverlayItem>, sym::g_overlayItems> g_items;
        // A material for each blend mode.
        inline ee::Global<ee::Ptr<zge::Material>[kBlendModes], sym::g_overlayMaterials> g_materials;
        // The materials' table as the words the original walks.
        ee::Ptr<uint32_t> materialWords() { return ee::Ptr<ee::Ptr<zge::Material>>(&(*g_materials)[0]).cast<uint32_t>(); }
        // The newest item of each group.
        inline ee::Global<uint16_t[kOverlayItemGroups], sym::g_overlayGroupNewest> g_groupNewest;
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_overlayItemsTask> g_task;
        // The sets of four materials (one a blend mode) a chain's items
        // take theirs from.
        inline ee::Global<ee::Ptr<uint32_t>[kMaterialSets], sym::g_overlayMaterialSets> g_materialSets;
        // By blend mode: the materials' blend state, and the flags the
        // quads are drawn with.
        inline ee::Global<uint32_t[kBlendModes], sym::overlayBlendStates> g_blendStates;
        inline ee::Global<uint32_t[16], sym::overlayQuadFlags> g_quadFlags;
        // The alpha byte for each of a colour's sixteen alpha steps.
        inline ee::Global<uint8_t[16], sym::overlayAlphaSteps> g_alphaSteps;

        constexpr uint32_t kSourceFile = sym::textOverlaySourceFile; // "./src/com/zgeif/zoverlay.c"
        constexpr uint32_t kSourceLine = 0xD2;
        // The task's stage, and its place in it.
        constexpr uint32_t kTaskStage = 1;
        constexpr uint32_t kTaskAtEnd = 1;
        // The materials' states set when they are made.
        constexpr uint32_t kMaterialStateCleared = 3;
        constexpr uint32_t kMaterialBlendState = 5;
        // A corner is drawn at the middle of its pixel.
        constexpr ee::Float kPixelCentre = 0.5f;

        ee::Ptr<OverlayItem> itemOf(uint32_t handle)
        {
            return *g_items + static_cast<ptrdiff_t>(handle & kItemIndexMask);
        }
        uint32_t blendOf(uint32_t colour)
        {
            const uint32_t blend = colour >> 28;
            return blend < kBlendModes ? blend : kBlendModes - 1;
        }

        // The task's frame: one quad and its four vertices.
        struct DrawFrame
        {
            uint32_t material;                          // +0x00
            ee::Ptr<zge::PrimitiveVertex> order[kCorners]; // +0x04 the corners, last first
            uint8_t _14[0x0C];
            zge::PrimitiveVertex vertices[kCorners];    // +0x20
        };
        static_assert(offsetof(DrawFrame, order) == 4 && offsetof(DrawFrame, vertices) == 0x20);
    }

    uint32_t initOverlayItems()
    {
        *g_nextFree = 0;
        *g_groupShown = 0;
        *g_byte5 = 0;
        *g_busy = 0;
        for (uint32_t group = 0; group < kOverlayItemGroups; ++group)
            (*g_groupNewest)[group] = kNoOverlayItem;
        for (uint32_t set = 0; set < kMaterialSets; ++set)
            (*g_materialSets)[set] = ee::Ptr<uint32_t>();
        for (uint32_t blend = 0; blend < kBlendModes; ++blend)
        {
            const ee::Ptr<zge::Material> material = zge::newMaterial();
            (*g_materials)[blend] = material;
            zge::zgui::rebuildMaterialRegisters(material, 0, kMaterialStateCleared, 0);
            zge::zgui::rebuildMaterialRegisters((*g_materials)[blend], 0, kMaterialBlendState, (*g_blendStates)[blend]);
        }
        const ee::Ptr<OverlayItem> items(zge::heapAllocate(kItems * sizeof(OverlayItem)));
        *g_items = items;
        if (!items)
            return 1;
        zge::memoryFill(items, 0, kItems * sizeof(OverlayItem));
        *g_task = zge::newTask(kTaskStage, sym::drawOverlayItems, 0, kTaskAtEnd, kSourceFile, kSourceLine);
        return 0;
    }
    READABLE_FUNCTION(initOverlayItems, Verified);

    void shutDownOverlayItems()
    {
        if (*g_task)
            zge::deleteTask(*g_task);
        if (*g_items)
            zge::heapFree(*g_items);
        for (uint32_t blend = 0; blend < kBlendModes; ++blend)
            zge::releaseMaterial((*g_materials)[blend]);
    }
    READABLE_FUNCTION(shutDownOverlayItems, Verified);

    uint32_t drawOverlayItems()
    {
        ee::Local<DrawFrame> frame;
        if (*g_busy)
            return 0;
        // From the group's newest item back to its oldest.
        uint32_t handle = (*g_groupNewest)[*g_groupShown];
        if (handle != kNoOverlayItem)
        {
            uint32_t older = handle;
            do
            {
                handle = older;
                older = itemOf(handle)->older;
            } while (older != kNoOverlayItem);
        }
        while (handle != kNoOverlayItem)
        {
            const ee::Ptr<OverlayItem> item = itemOf(handle);
            if (!(item->state & kOverlayItemHidden))
            {
                frame->material = item->material;
                for (uint32_t i = 0; i < kCorners; ++i)
                {
                    const OverlayCorner &corner = item->corners[i];
                    zge::PrimitiveVertex &vertex = frame->vertices[i];
                    const ee::Float x = ee::toFloat(corner.x) + kPixelCentre;
                    const ee::Float y = ee::toFloat(corner.y) + kPixelCentre;
                    vertex.x = x;
                    vertex.z = 0.0f;
                    vertex.y = y;
                    vertex.u = corner.u;
                    vertex.v = corner.v;
                    for (uint32_t channel = 0; channel < 4; ++channel)
                        vertex.rgba[channel] = corner.rgba[channel];
                    frame->order[i] = &frame->vertices[kCorners - 1 - i];
                }
                zge::drawPrimitiveQuads((*g_quadFlags)[item->kindAndBlend >> kBlendShift], frame, 1, 0);
            }
            handle = item->newer;
        }
        return 1;
    }
    READABLE_FUNCTION(drawOverlayItems, Verified);

    void showOverlayItemGroup(uint8_t group)
    {
        *g_groupShown = group;
    }
    READABLE_FUNCTION(showOverlayItemGroup, Verified);

    void setOverlayItemVisible(uint32_t handle, uint32_t visible)
    {
        if ((handle & 0xFFFF) == kNoOverlayItem)
            return;
        uint32_t index = handle & kItemIndexMask;
        *g_busy = *g_busy + 1;
        const ee::Ptr<OverlayItem> first = itemOf(index);
        const uint8_t kind = first->kindAndBlend & kKindMask;
        if (kind < kOverlayChain)
        {
            if (visible == 0)
                first->state = first->state | kOverlayItemHidden;
            else
                first->state = first->state & static_cast<uint8_t>(~kOverlayItemHidden);
        }
        else if (kind == kOverlayChain)
        {
            for (;;)
            {
                const ee::Ptr<OverlayItem> item = itemOf(index);
                const uint8_t state = item->state;
                if (visible == 0)
                    item->state = state | kOverlayItemHidden;
                else
                    item->state = state & static_cast<uint8_t>(~kOverlayItemHidden);
                if (state & kOverlayItemEndsChain)
                    break;
                index = item->newer & kItemIndexMask;
            }
        }
        *g_busy = *g_busy - 1;
    }
    READABLE_FUNCTION(setOverlayItemVisible, Verified);

    void removeOverlayItem(uint32_t handle)
    {
        handle &= 0xFFFF;
        if (handle == kNoOverlayItem)
            return;
        *g_busy = *g_busy + 1;
        const uint8_t kind = itemOf(handle)->kindAndBlend & kKindMask;
        if (kind < kOverlayChain)
        {
            freeOverlayItem(handle);
        }
        else if (kind == kOverlayChain)
        {
            for (;;)
            {
                const ee::Ptr<OverlayItem> item = itemOf(handle);
                const uint8_t state = item->state;
                const uint32_t newer = item->newer;
                freeOverlayItem(handle);
                if (state & kOverlayItemEndsChain)
                    break;
                handle = newer;
            }
        }
        *g_busy = *g_busy - 1;
    }
    READABLE_FUNCTION(removeOverlayItem, Verified);

    // A chain's items also take the material of the colour's blend mode
    // from their set.
    void setOverlayItemColour(uint32_t handle, uint32_t colour)
    {
        if ((handle & 0xFFFF) == kNoOverlayItem)
            return;
        uint32_t index = handle & kItemIndexMask;
        *g_busy = *g_busy + 1;
        const ee::Ptr<OverlayItem> first = itemOf(index);
        const uint8_t kind = first->kindAndBlend & kKindMask;
        if (kind < kOverlayChain)
        {
            applyOverlayItemColour(first, colour);
        }
        else if (kind == kOverlayChain)
        {
            const uint32_t material = (*g_materialSets)[first->materialSet][blendOf(colour)];
            for (;;)
            {
                const ee::Ptr<OverlayItem> item = itemOf(index);
                const uint8_t state = item->state;
                applyOverlayItemColour(item, colour);
                const uint32_t newer = item->newer;
                item->material = material;
                if (state & kOverlayItemEndsChain)
                    break;
                index = newer & kItemIndexMask;
            }
        }
        *g_busy = *g_busy - 1;
    }
    READABLE_FUNCTION(setOverlayItemColour, Verified);

    uint32_t newOverlayRectangle(uint32_t group, int16_t x, int16_t y, int16_t width, int16_t height, uint32_t colour)
    {
        *g_busy = *g_busy + 1;
        const uint32_t handle = allocateOverlayItem(group);
        if (handle != kNoOverlayItem)
        {
            const uint32_t blend = blendOf(colour);
            const ee::Ptr<OverlayItem> item = itemOf(handle);
            item->kindAndBlend = static_cast<uint8_t>(blend << kBlendShift);
            item->corners[0].u = 0.0f;
            item->corners[0].v = 0.0f;
            item->corners[1].v = 0.0f;
            item->corners[3].u = 0.0f;
            item->material = ee::Address((*g_materials)[blend]);
            item->corners[3].v = 1.0f;
            item->corners[1].u = 1.0f;
            item->corners[2].u = 1.0f;
            item->corners[2].v = 1.0f;
            setOverlayItemRectangle(handle, x, y, width, height);
            applyOverlayItemColour(item, colour);
        }
        *g_busy = *g_busy - 1;
        return handle;
    }
    READABLE_FUNCTION(newOverlayRectangle, Verified);

    void setOverlayItemRectangle(uint32_t handle, int16_t x, int16_t y, int16_t width, int16_t height)
    {
        if ((handle & 0xFFFF) == kNoOverlayItem)
            return;
        const ee::Ptr<OverlayItem> item = itemOf(handle);
        const int16_t right = static_cast<int16_t>(x + width);
        const int16_t bottom = static_cast<int16_t>(y + height);
        item->corners[3].y = bottom;
        item->corners[2].x = right;
        item->corners[3].x = x;
        item->corners[0].x = x;
        item->corners[0].y = y;
        item->corners[1].x = right;
        item->corners[1].y = y;
        item->corners[2].y = bottom;
    }
    READABLE_FUNCTION(setOverlayItemRectangle, Verified);

    // The gradient's alpha is the colour's alpha step times 16, not the
    // step's byte of the table the other items use.
    uint32_t newOverlayGradient(uint32_t group, int16_t x, int16_t y, int16_t width, int16_t height, uint32_t topColour, uint32_t bottomColour)
    {
        ee::Local<zge::VertexColour> packed;
        *g_busy = *g_busy + 1;
        const uint32_t handle = allocateOverlayItem(group);
        if (handle != kNoOverlayItem)
        {
            const uint32_t blend = blendOf(topColour);
            const ee::Ptr<OverlayItem> item = itemOf(handle);
            item->kindAndBlend = static_cast<uint8_t>((blend << kBlendShift) | kOverlayGradient);
            item->material = ee::Address((*g_materials)[blend]);
            setOverlayItemRectangle(handle, x, y, width, height);
            const auto pack = [&](uint32_t colour) {
                packed->red = static_cast<uint8_t>(colour >> 16);
                packed->green = static_cast<uint8_t>(colour >> 8);
                packed->alpha = static_cast<uint8_t>((colour & 0x0F000000) >> 20);
                packed->blue = static_cast<uint8_t>(colour);
            };
            const auto paint = [&](OverlayCorner &corner) {
                corner.rgba[0] = packed->red;
                corner.rgba[1] = packed->green;
                corner.rgba[2] = packed->blue;
                corner.rgba[3] = packed->alpha;
            };
            pack(topColour);
            paint(item->corners[0]);
            paint(item->corners[1]);
            pack(bottomColour);
            paint(item->corners[2]);
            paint(item->corners[3]);
        }
        *g_busy = *g_busy - 1;
        return handle;
    }
    READABLE_FUNCTION(newOverlayGradient, Verified);

    namespace
    {
        // drawOverlayRectangle's frame: the quad (DrawFrame's), then the
        // colour its four corners share.
        struct RectangleFrame
        {
            DrawFrame quad;  // +0x00
            uint8_t rgba[4]; // +0xA0
        };
        static_assert(offsetof(RectangleFrame, rgba) == 0xA0);
    }

    // Kept from the original: the blend mode is taken up to 4 here (the
    // items stop at 3), and blend 4 takes as its material the word after the
    // four materials (the two groups' newest items).
    void drawOverlayRectangle(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colour)
    {
        ee::Local<RectangleFrame> frame;
        const uint32_t blend = colour >> 28 < kBlendModes + 1 ? colour >> 28 : kBlendModes;
        const ee::Ptr<uint32_t> materials = materialWords();
        const uint8_t alpha = (*g_alphaSteps)[(colour >> 24) & 0x0F];
        frame->rgba[0] = static_cast<uint8_t>(colour >> 16);
        frame->rgba[3] = alpha;
        frame->quad.material = materials[blend];
        frame->rgba[1] = static_cast<uint8_t>(colour >> 8);
        frame->rgba[2] = static_cast<uint8_t>(colour);
        const int32_t right = x + width, bottom = y + height;
        // The corners from the top left round to the bottom left; drawn
        // last first.
        for (uint32_t i = 0; i < kCorners; ++i)
        {
            zge::PrimitiveVertex &vertex = frame->quad.vertices[i];
            vertex.x = ee::toFloat((i + 1) & 2 ? right : x);
            vertex.y = ee::toFloat(i & 2 ? bottom : y);
            vertex.z = 0.0f;
            for (uint32_t channel = 0; channel < 4; ++channel)
                vertex.rgba[channel] = frame->rgba[channel];
            frame->quad.order[i] = &frame->quad.vertices[kCorners - 1 - i];
        }
        zge::drawPrimitiveQuads((*g_quadFlags)[blend], frame, 1, 0);
    }
    READABLE_FUNCTION(drawOverlayRectangle, Verified);

    // The search goes once round the table from where the last one ended.
    // With every place in use it comes back to where it started and that
    // item, still another's, is linked into the group again (the
    // original's: only a table at address 0 gives kNoOverlayItem).
    uint32_t allocateOverlayItem(uint32_t group)
    {
        const uint32_t start = *g_nextFree;
        uint32_t index = start;
        ee::Ptr<OverlayItem> item = *g_items + static_cast<ptrdiff_t>(index);
        for (;;)
        {
            if (!(item->state & kOverlayItemInUse))
            {
                item->state = kOverlayItemInUse;
                *g_nextFree = static_cast<uint16_t>(index + 1);
                if (((index + 1) & 0xFFFF) == kItems)
                    *g_nextFree = 0;
                break;
            }
            ++index;
            ++item;
            if (index == kItems)
            {
                index = 0;
                item = *g_items;
            }
            if (index == start)
                break;
        }
        if (!item)
            return kNoOverlayItem;
        const uint16_t newest = (*g_groupNewest)[group];
        const uint32_t handle = ((group << kGroupShift) | index) & 0xFFFF;
        item->older = newest;
        if (newest != kNoOverlayItem)
            itemOf(newest)->newer = static_cast<uint16_t>(handle);
        (*g_groupNewest)[group] = static_cast<uint16_t>(handle);
        item->newer = kNoOverlayItem;
        return handle;
    }
    READABLE_FUNCTION(allocateOverlayItem, Verified);

    void freeOverlayItem(uint32_t handle)
    {
        const ee::Ptr<OverlayItem> item = itemOf(handle);
        uint16_t older = item->older;
        item->state = 0;
        if (older != kNoOverlayItem)
        {
            itemOf(older)->newer = item->newer;
            older = item->older;
        }
        if (item->newer == kNoOverlayItem)
            (*g_groupNewest)[(handle & 0xFFFF) >> kGroupShift] = older;
        else
            itemOf(item->newer)->older = older;
        *g_nextFree = static_cast<uint16_t>(handle & kItemIndexMask);
    }
    READABLE_FUNCTION(freeOverlayItem, Verified);

    void applyOverlayItemColour(ee::Ptr<OverlayItem> item, uint32_t colour)
    {
        ee::Local<zge::VertexColour> packed;
        const uint8_t alpha = (*g_alphaSteps)[(colour >> 24) & 0x0F];
        packed->red = static_cast<uint8_t>(colour >> 16);
        packed->green = static_cast<uint8_t>(colour >> 8);
        packed->alpha = alpha;
        packed->blue = static_cast<uint8_t>(colour);
        item->kindAndBlend = static_cast<uint8_t>((item->kindAndBlend & kKindMask) | ((colour >> 28) << kBlendShift));
        for (OverlayCorner &corner : item->corners)
        {
            corner.rgba[0] = packed->red;
            corner.rgba[1] = packed->green;
            corner.rgba[2] = packed->blue;
            corner.rgba[3] = packed->alpha;
        }
    }
    READABLE_FUNCTION(applyOverlayItemColour, Verified);
}
