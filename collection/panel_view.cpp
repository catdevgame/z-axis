#include "collection/panel_view.h"
#include "collection/panel_layout.h"

#include <algorithm>

namespace panel
{
    float fitZoom(const MapTransform &map, const Rect &pane)
    {
        if (map.width <= 0 || map.height <= 0)
            return 1.0f;
        return std::min(pane.width / static_cast<float>(map.width), pane.height / static_cast<float>(map.height)) * kMinZoomMargin;
    }

    MapView wholeView(const MapTransform &map, const Rect &pane, const Rect &clear)
    {
        MapView view;
        view.follow = false;
        view.zoom = fitZoom(map, clear);
        // the picture's middle at the free area's middle
        const float dx = clear.x + clear.width * 0.5f - (pane.x + pane.width * 0.5f);
        const float dy = clear.y + clear.height * 0.5f - (pane.y + pane.height * 0.5f);
        view.center = {static_cast<float>(map.width) * 0.5f - dx / view.zoom, static_cast<float>(map.height) * 0.5f - dy / view.zoom};
        return view;
    }

    float clampZoom(float zoom, const MapTransform &map, const Rect &pane)
    {
        return std::clamp(zoom, fitZoom(map, pane), kMaxZoom);
    }

    Point clampCenter(Point center, float zoom, const MapTransform &map, const Rect &pane)
    {
        // The picture's edge may reach the pane's centre, no further: the
        // skater at a corner can still be centred.
        (void)zoom;
        (void)pane;
        center.x = std::clamp(center.x, 0.0f, static_cast<float>(map.width));
        center.y = std::clamp(center.y, 0.0f, static_cast<float>(map.height));
        return center;
    }

    void panBy(MapView &view, float dx, float dy, const MapTransform &map, const Rect &pane)
    {
        view.center.x -= dx / view.zoom;
        view.center.y -= dy / view.zoom;
        view.center = clampCenter(view.center, view.zoom, map, pane);
    }

    void pinch(MapView &view, Point a0, Point b0, Point a1, Point b1, const MapTransform &map, const Rect &pane)
    {
        const float before = std::hypot(b0.x - a0.x, b0.y - a0.y);
        const float after = std::hypot(b1.x - a1.x, b1.y - a1.y);
        const Point middle0{(a0.x + b0.x) * 0.5f, (a0.y + b0.y) * 0.5f};
        const Point middle1{(a1.x + b1.x) * 0.5f, (a1.y + b1.y) * 0.5f};
        const Point held = screenToMap(view, pane, middle0);
        if (before > 1.0f && after > 1.0f)
            view.zoom = clampZoom(view.zoom * after / before, map, pane);
        // the centre that puts `held` under the new midpoint
        view.center.x = held.x - (middle1.x - pane.x - pane.width * 0.5f) / view.zoom;
        view.center.y = held.y - (middle1.y - pane.y - pane.height * 0.5f) / view.zoom;
        view.center = clampCenter(view.center, view.zoom, map, pane);
    }

    int nearestWithin(const Point *points, int count, Point tap, float radius)
    {
        int best = -1;
        float bestSquared = radius * radius;
        for (int i = 0; i < count; ++i)
        {
            const float dx = points[i].x - tap.x, dy = points[i].y - tap.y;
            const float squared = dx * dx + dy * dy;
            if (squared <= bestSquared)
            {
                bestSquared = squared;
                best = i;
            }
        }
        return best;
    }

    int rowAt(float y, float top, float height, float scroll, const float *rowHeights, int rows)
    {
        if (y < top || y >= top + height)
            return -1;
        float at = top - scroll;
        for (int i = 0; i < rows; ++i)
        {
            if (y >= at && y < at + rowHeights[i])
                return i;
            at += rowHeights[i];
        }
        return -1;
    }

    std::string statusLine(const char *format, int count, int target)
    {
        std::string out;
        int used = 0;
        for (const char *c = format; *c; ++c)
        {
            if (c[0] == '%' && c[1] == 'd')
            {
                out += std::to_string(used++ == 0 ? count : target);
                ++c;
            }
            else
                out += *c;
        }
        return out;
    }

    std::vector<GoalRow> groupRows(std::vector<GoalEntry> entries, bool lockedOpen)
    {
        std::stable_sort(entries.begin(), entries.end(), [](const GoalEntry &a, const GoalEntry &b) {
            if (a.group != b.group)
                return a.group < b.group;
            return a.group == GoalGroup::CanStart && a.distance < b.distance;
        });
        std::vector<GoalRow> rows;
        for (size_t i = 0; i < entries.size();)
        {
            const GoalGroup group = entries[i].group;
            rows.push_back({-1, group});
            for (; i < entries.size() && entries[i].group == group; ++i)
                if (group != GoalGroup::Locked || lockedOpen)
                    rows.push_back({entries[i].goal, group});
        }
        return rows;
    }

    int groupCount(const std::vector<GoalEntry> &entries, GoalGroup group)
    {
        int count = 0;
        for (const GoalEntry &e : entries)
            count += e.group == group;
        return count;
    }

    const char *groupWord(GoalGroup group)
    {
        switch (group)
        {
        case GoalGroup::Now: return "NOW";
        case GoalGroup::CanStart: return "CAN START";
        case GoalGroup::Running: return "RUNNING";
        case GoalGroup::Done: return "DONE";
        case GoalGroup::Locked: return "LOCKED";
        default: return "";
        }
    }

    float goalRowHeight(int titleLines, float titleGap, int moreLines, float moreGap, bool bar)
    {
        const float text = 24.0f + static_cast<float>(titleLines) * titleGap + static_cast<float>(moreLines) * moreGap;
        return std::max(kGoalRowHeight, text + (bar ? kRowBarHeight + 14.0f : 0.0f));
    }
}
