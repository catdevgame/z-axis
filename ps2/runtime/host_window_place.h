#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// Where a window opens again at the place it was remembered at (the Map
// Window, ps2_host::openMapWindow): there while it can still be grabbed on
// one of the displays as they are now; otherwise on the display nearest to
// where it was, moved (and made smaller where the display is) to lie whole
// on it. A display unplugged since, or one made smaller, so leaves no window
// out of reach. Plain maths, in the window library's screen coordinates
// (points), so the tests check it (tests/src/host_window_tests.cpp).
namespace ps2_host
{
    struct ScreenRect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    // How much of a window's title bar must lie on a display to grab it:
    // this wide, its whole height (a bar the system reports none for counts
    // kGrabHeight above the window's top).
    constexpr int kGrabWidth = 96;
    constexpr int kGrabHeight = 32;

    // `window` is its inside (as the window library places it), `titleBar`
    // the height of the bar above it (0: none known), `displays` each
    // display's usable area, the main display first.
    inline ScreenRect keptOnDisplays(ScreenRect window, int titleBar, const std::vector<ScreenRect> &displays)
    {
        if (displays.empty())
            return window;
        const int bar = titleBar > 0 ? titleBar : kGrabHeight;
        for (const ScreenRect &d : displays)
        {
            const int barTop = window.y - bar;
            const int across = std::min(window.x + window.width, d.x + d.width) - std::max(window.x, d.x);
            if (barTop >= d.y && window.y <= d.y + d.height && across >= std::min(kGrabWidth, window.width))
                return window;
        }
        // The display nearest to the window's middle (none inside it: the
        // first of those as near).
        const int64_t midX = window.x + window.width / 2, midY = window.y + window.height / 2;
        const ScreenRect *nearest = &displays.front();
        int64_t nearestDistance = -1;
        for (const ScreenRect &d : displays)
        {
            const int64_t dx = std::max<int64_t>({d.x - midX, 0, midX - (d.x + d.width)});
            const int64_t dy = std::max<int64_t>({d.y - midY, 0, midY - (d.y + d.height)});
            const int64_t distance = dx * dx + dy * dy;
            if (nearestDistance < 0 || distance < nearestDistance)
            {
                nearest = &d;
                nearestDistance = distance;
            }
        }
        const ScreenRect &d = *nearest;
        ScreenRect placed = window;
        placed.width = std::min(window.width, d.width);
        placed.height = std::min(window.height, std::max(1, d.height - bar));
        placed.x = std::clamp(window.x, d.x, d.x + d.width - placed.width);
        placed.y = std::clamp(window.y, d.y + bar, std::max(d.y + bar, d.y + d.height - placed.height));
        return placed;
    }
}
