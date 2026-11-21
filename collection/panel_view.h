#pragma once

// The panel's maths, free of drawing: world to map to screen, pan and pinch,
// and which thing a tap hit. Unit tested (tests/ panel_tests.cpp).

#include <cmath>
#include <cstdint>
#include <string>

namespace panel
{
    struct Point
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Rect
    {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height; }
    };

    // World to map picture: a top-down view with +x right and +z up, the view
    // from above in the game's left-handed space (y grows downward). A level
    // wider than tall is turned a quarter clockwise (+z right, +x down) so
    // its long side lies along the tall pane: never mirrored.
    struct MapTransform
    {
        float worldLeft = 0.0f;  // x of the picture's left edge (turned: its top edge)
        float worldTop = 0.0f;   // z of its top edge (turned: its left edge)
        float pixelsPerUnit = 1.0f;
        int width = 0;           // the picture's size
        int height = 0;
        bool turned = false;

        Point toMap(float x, float z) const
        {
            if (turned)
                return {(z - worldTop) * pixelsPerUnit, (x - worldLeft) * pixelsPerUnit};
            return {(x - worldLeft) * pixelsPerUnit, (worldTop - z) * pixelsPerUnit};
        }
        // World x, z (in a Point's x, y).
        Point toWorld(Point map) const
        {
            if (turned)
                return {worldLeft + map.y / pixelsPerUnit, worldTop + map.x / pixelsPerUnit};
            return {worldLeft + map.x / pixelsPerUnit, worldTop - map.y / pixelsPerUnit};
        }
        // A world direction on the picture.
        Point direction(float x, float z) const
        {
            if (turned)
                return {z, x};
            return {x, -z};
        }
    };

    constexpr float kMinZoomMargin = 0.98f;  // the whole level, with this share of the free area
    constexpr float kMaxZoom = 4.0f;         // screen pixels per map pixel
    constexpr float kFollowZoom = 1.35f;     // the zoom a fresh follow view starts at

    // What the map pane shows: a point of the picture at the pane's centre.
    struct MapView
    {
        Point center;       // in map pixels
        float zoom = 1.0f;  // screen pixels per map pixel
        bool follow = true; // centre on the skater each frame
    };

    // The zoom that shows the whole picture inside `pane`.
    float fitZoom(const MapTransform &map, const Rect &pane);
    // The view that fits the picture into `clear` (the pane less what is
    // drawn over it) with `pane` as the view's frame.
    MapView wholeView(const MapTransform &map, const Rect &pane, const Rect &clear);
    float clampZoom(float zoom, const MapTransform &map, const Rect &pane);
    // The centre kept so the picture cannot leave the pane.
    Point clampCenter(Point center, float zoom, const MapTransform &map, const Rect &pane);

    inline Point mapToScreen(const MapView &view, const Rect &pane, Point map)
    {
        return {pane.x + pane.width * 0.5f + (map.x - view.center.x) * view.zoom,
                pane.y + pane.height * 0.5f + (map.y - view.center.y) * view.zoom};
    }
    inline Point screenToMap(const MapView &view, const Rect &pane, Point screen)
    {
        return {view.center.x + (screen.x - pane.x - pane.width * 0.5f) / view.zoom,
                view.center.y + (screen.y - pane.y - pane.height * 0.5f) / view.zoom};
    }

    // One finger dragged by (dx, dy) screen pixels: the map follows it.
    void panBy(MapView &view, float dx, float dy, const MapTransform &map, const Rect &pane);
    // Two fingers from (a0, b0) to (a1, b1): zoomed by their spread, the map
    // point under their midpoint stays under the new midpoint.
    void pinch(MapView &view, Point a0, Point b0, Point a1, Point b1, const MapTransform &map, const Rect &pane);

    // The nearest of `count` points (screen) to `tap` within `radius`, or -1.
    int nearestWithin(const Point *points, int count, Point tap, float radius);

    // A scrolled list of rows: which row a y falls on, -1 for none.
    int rowAt(float y, float top, float height, float scroll, const float *rowHeights, int rows);

    // A game's status format with two %d ("Gaps found: %d of %d") filled
    // in with the count and the target.
    std::string statusLine(const char *format, int count, int target);
}
