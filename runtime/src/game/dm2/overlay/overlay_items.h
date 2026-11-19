// DM2's screen overlay items (zgeif/zoverlay.c in its build): rectangles the
// game makes once and the overlay's task draws every frame until they are
// freed. An item is named by a handle; kNoOverlayItem is none (the functions
// that take a handle do nothing for it). The table holds 1024 items.
#pragma once

#include "ee/ee.h"

#include <cstdint>

namespace game::dm2
{
    constexpr uint32_t kNoOverlayItem = 0xFFFF;
    constexpr uint32_t kOverlayItemGroups = 2;

    // The table, its four materials (one a blend mode) and the drawing task
    // made: 1 when the table cannot be allocated. And all of it freed.
    uint32_t initOverlayItems();
    void shutDownOverlayItems();

    // A rectangle of one colour (its texture coordinates span 0 .. 1), and
    // one whose top and bottom edges have a colour each, added to a group:
    // the new item's handle, or kNoOverlayItem. A colour is 0xBARRGGBB: its
    // top four bits pick the blend mode (3 at most), the next four the
    // alpha.
    uint32_t newOverlayRectangle(uint32_t group, int16_t x, int16_t y, int16_t width, int16_t height, uint32_t colour);
    uint32_t newOverlayGradient(uint32_t group, int16_t x, int16_t y, int16_t width, int16_t height, uint32_t topColour, uint32_t bottomColour);
    // A rectangle of one colour drawn at once, not kept as an item: x, y,
    // width and height in pixels, the colour as an item's (its blend mode
    // up to 4).
    void drawOverlayRectangle(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colour);
    // The item taken off its group and its place free again; a chain of
    // items, every one of the chain.
    void removeOverlayItem(uint32_t item);
    // Which of the two groups the task draws.
    void showOverlayItemGroup(uint8_t group);
    // The task's step: the shown group's items drawn, oldest first, unless
    // a function here is at work on the table (0 then, else 1).
    uint32_t drawOverlayItems();

    // The item's corners set from a place and a size.
    void setOverlayItemRectangle(uint32_t item, int16_t x, int16_t y, int16_t width, int16_t height);
    // The item shown (not 0) or hidden (0); an item that is a chain of
    // items, every one of the chain.
    void setOverlayItemVisible(uint32_t item, uint32_t visible);
    // The item's colour (and with it its blend mode's material).
    void setOverlayItemColour(uint32_t item, uint32_t colour);

    // One corner of an item on the screen.
    struct OverlayCorner
    {
        int16_t x, y;    // +0x00
        ee::Float u, v;  // +0x04
        uint8_t rgba[4]; // +0x0C
    };
    static_assert(sizeof(OverlayCorner) == 0x10);

    // OverlayItem::state.
    constexpr uint8_t kOverlayItemInUse = 0x01;
    constexpr uint8_t kOverlayItemHidden = 0x02;
    constexpr uint8_t kOverlayItemEndsChain = 0x04;
    // OverlayItem::kindAndBlend's low four bits.
    constexpr uint8_t kOverlayRectangle = 0;
    constexpr uint8_t kOverlayGradient = 1;
    constexpr uint8_t kOverlayChain = 3; // one of a row of items linked through `newer`, up to the one that ends the chain

    struct OverlayItem
    {
        uint8_t kindAndBlend;     // +0x00 the kind, and the blend mode in the top four bits
        uint8_t state;            // +0x01 kOverlayItem...
        uint8_t materialSet;      // +0x02 a chain's items: which of the sixteen sets of four materials is theirs
        uint8_t _03;
        uint16_t older, newer;    // +0x04 the handles of its neighbours in the group, kNoOverlayItem at the ends
        uint32_t material;        // +0x08
        OverlayCorner corners[4]; // +0x0C top left, top right, bottom right, bottom left
    };
    static_assert(sizeof(OverlayItem) == 0x4C && offsetof(OverlayItem, older) == 4 && offsetof(OverlayItem, corners) == 0x0C);

    // A place of the table taken for a new item of a group (its handle), a
    // place given back, and a colour put on an item's four corners with its
    // blend mode.
    uint32_t allocateOverlayItem(uint32_t group);
    void freeOverlayItem(uint32_t item);
    void applyOverlayItemColour(ee::Ptr<OverlayItem> item, uint32_t colour);
}
