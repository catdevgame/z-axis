// Practice's host side (engine/overrides/zge_practice.cpp): off it takes no
// key; on, the practice_point key asks for a point and the Restart key goes
// back to one; each try is timed in the game's seconds of play.
#include "MiniTest.h"

#include "overrides/zge_practice.h"
#include "runtime/settings_registry.h"

namespace
{
    namespace zp = zge_host::practice;

    void setPractice(int value)
    {
        zx::settings::Setting row;
        row.key = "game.practice";
        row.label = "Practice";
        row.current = [value]() { return value; };
        zx::settings::add(row);
    }
}

void register_zge_practice_tests()
{
    MiniTest::Case("ZgePractice", [](TestCase &tc)
    {
        tc.Run("times as the player reads them", [](TestCase &t)
        {
            t.Equals(zp::timeText(0.0), std::string("0.00"), "nothing yet");
            t.Equals(zp::timeText(12.483), std::string("12.48"), "hundredths");
            t.Equals(zp::timeText(62.5), std::string("1:02.50"), "a minute and more");
        });

        tc.Run("off: the keys are the game's", [](TestCase &t)
        {
            zx::settings::clear();
            setPractice(0);
            zp::Practice practice;
            t.IsFalse(practice.onVsync(true, true), "the Restart key left to the game");
            t.IsTrue(practice.take() == zp::Request::None, "no point asked for");
            zx::settings::clear();
        });

        tc.Run("on: a point, tries timed from it, the Restart key going back", [](TestCase &t)
        {
            zx::settings::clear();
            setPractice(1);
            zp::Practice practice;
            t.IsFalse(practice.onVsync(false, true), "no point yet: the Restart key is the game's");
            t.IsFalse(practice.onVsync(true, false), "the point key");
            t.IsTrue(practice.take() == zp::Request::SetPoint, "a point asked for");
            practice.pointSet();
            for (int frame = 0; frame < 120; ++frame)
                practice.played(1.0f / 60.0f);
            t.IsTrue(practice.onVsync(false, true), "the Restart key taken");
            t.IsTrue(practice.take() == zp::Request::Return, "back to the point");
            practice.returned();
            for (int frame = 0; frame < 90; ++frame)
                practice.played(1.0f / 60.0f);
            practice.returned();
            t.Equals(practice.tries().size(), static_cast<size_t>(2), "two tries");
            t.Equals(zp::timeText(practice.tries()[0]), std::string("2.00"), "the first: 120 frames");
            t.Equals(zp::timeText(practice.tries()[1]), std::string("1.50"), "the second: 90 frames");
            setPractice(0);
            t.IsFalse(practice.onVsync(false, true), "turned off: the Restart key the game's again");
            t.IsFalse(practice.hasPoint(), "and the point dropped");
            zx::settings::clear();
        });
    });
}
