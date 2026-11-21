#include "MiniTest.h"
#include "runtime/host_window_place.h"

#include <vector>

using ps2_host::keptOnDisplays;
using ps2_host::ScreenRect;

namespace
{
    // A 1920x1080 main display (its menu bar's 25 points not usable) and a 2560x1440 one to its right.
    constexpr ScreenRect mainDisplay{0, 25, 1920, 1055};
    constexpr ScreenRect rightDisplay{1920, 0, 2560, 1440};

    bool same(const ScreenRect &a, const ScreenRect &b)
    {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
}

void register_host_window_tests()
{
    MiniTest::Case("HostWindow", [](TestCase &tc) {
        tc.Run("a window on a display stays where it was", [](TestCase &t) {
            const ScreenRect onMain{300, 200, 620, 540}, onRight{2600, 400, 930, 810};
            t.IsTrue(same(keptOnDisplays(onMain, 28, {mainDisplay, rightDisplay}), onMain), "on the main display");
            t.IsTrue(same(keptOnDisplays(onRight, 28, {mainDisplay, rightDisplay}), onRight), "on the second display");
            const ScreenRect across{1700, 300, 620, 540};
            t.IsTrue(same(keptOnDisplays(across, 28, {mainDisplay, rightDisplay}), across), "across the two: grabbed on either");
            t.IsTrue(same(keptOnDisplays(across, 28, {mainDisplay}), across), "partly off the edge, its title bar 220 wide on it");
        });

        tc.Run("a window on a display unplugged since comes onto the nearest one, whole", [](TestCase &t) {
            const ScreenRect wasRight{2600, 400, 930, 810};
            const ScreenRect placed = keptOnDisplays(wasRight, 28, {mainDisplay});
            t.IsTrue(same(placed, {1920 - 930, 1080 - 810, 930, 810}), "against the main display's right edge, moved up to lie whole");
            const ScreenRect wasLeft{-1500, -300, 620, 540};
            t.IsTrue(same(keptOnDisplays(wasLeft, 28, {mainDisplay}), {0, 25 + 28, 620, 540}), "a display left of and above: the top left corner, its bar below the menu bar");
            const ScreenRect tooBig{2600, 100, 2400, 1400};
            t.IsTrue(same(keptOnDisplays(tooBig, 28, {mainDisplay}), {0, 25 + 28, 1920, 1055 - 28}), "larger than the display: the display's size");
        });

        tc.Run("a window whose title bar is out of reach is moved", [](TestCase &t) {
            const ScreenRect barAbove{400, 30, 620, 540};
            t.IsTrue(same(keptOnDisplays(barAbove, 28, {mainDisplay}), {400, 25 + 28, 620, 540}), "its bar under the menu bar: moved just below it");
            const ScreenRect sliver{1880, 300, 620, 540};
            t.IsTrue(same(keptOnDisplays(sliver, 28, {mainDisplay}), {1920 - 620, 300, 620, 540}), "40 points of its bar on the display: brought on whole");
            const ScreenRect below{400, 1100, 620, 540};
            t.IsTrue(same(keptOnDisplays(below, 0, {mainDisplay}), {400, 1080 - 540, 620, 540}), "below the display, no bar height known: kGrabHeight");
        });

        tc.Run("the nearest display is the one the window was closest to", [](TestCase &t) {
            const ScreenRect farRight{5000, 600, 620, 540};
            t.IsTrue(same(keptOnDisplays(farRight, 28, {mainDisplay, rightDisplay}), {4480 - 620, 600, 620, 540}), "past the second display: onto it");
            const ScreenRect below{100, 2000, 620, 540};
            t.IsTrue(same(keptOnDisplays(below, 28, {mainDisplay, rightDisplay}), {100, 1080 - 540, 620, 540}), "below the main display: onto it");
            const ScreenRect belowRight{3000, 2000, 620, 540};
            t.IsTrue(same(keptOnDisplays(belowRight, 28, {mainDisplay, rightDisplay}), {3000, 1440 - 540, 620, 540}), "below the second display: onto that one");
            t.IsTrue(same(keptOnDisplays(farRight, 28, {}), farRight), "no display known: left as it was");
        });
    });
}
