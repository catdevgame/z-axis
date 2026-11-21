#include "MiniTest.h"
#include "runtime/gs_texture_audit.h"

#include <string>

namespace
{
    namespace ga = zx::gs_texture_audit;
}

void register_gs_texture_audit_tests()
{
    MiniTest::Case("GsTextureAudit", [](TestCase &tc)
    {
        tc.Run("a level that fits: peak, CLUT floor, no overrun", [](TestCase &t)
        {
            ga::Audit a;
            a.setLevel("AI Movie");
            // 5632 blocks up, 64 x 64 T8 levels (16 blocks each), CLUTs at the top.
            t.Equals(a.noteAllocation(5632u * 64u, 1024u, 0x100000u - 0x100u, "a"), 0, "first level fits");
            t.Equals(a.noteAllocation(5648u * 64u, 1024u + 1u, 0x100000u - 0x200u, "b"), 0, "second fits");
            a.noteFrame();
            const ga::LevelStats &s = a.stats("AI Movie");
            t.Equals(s.allocations, uint64_t{2}, "allocations");
            t.Equals(s.lowestStart, 5632u, "lowest block");
            t.Equals(s.peakEnd, 5665u, "peak: a partial block counts whole");
            t.IsTrue(s.peakTexture == "b", "the texture at the peak");
            t.Equals(s.clutFloor, 16376u, "CLUT floor");
            t.Equals(s.overCluts, uint64_t{0}, "over the CLUTs");
            t.Equals(s.pastEnd, uint64_t{0}, "past the end");
            t.Equals(s.frames, uint64_t{1}, "frames");
            t.Equals(s.overrunFrames, uint64_t{0}, "overrun frames");
        });

        tc.Run("over the CLUTs, then past the end, counted per frame and level", [](TestCase &t)
        {
            ga::Audit a;
            a.setLevel("AI Local");
            const uint32_t clut = 16000u * 64u;
            t.Equals(a.noteAllocation(15990u * 64u, 10u * 64u, clut), 0, "ends exactly at the CLUTs");
            t.Equals(a.noteAllocation(15995u * 64u, 10u * 64u, clut), 1, "into the CLUTs");
            a.noteFrame();
            a.noteFrame();
            t.Equals(a.noteAllocation(16380u * 64u, 8u * 64u, clut), 2, "past block 16384");
            t.Equals(a.noteAllocation(25000u * 64u, 64u, clut), 2, "far past (the game's cursor never wraps)");
            a.noteFrame();
            const ga::LevelStats &s = a.stats("AI Local");
            t.Equals(s.overCluts, uint64_t{3}, "over the CLUTs (past the end included)");
            t.Equals(s.pastEnd, uint64_t{2}, "past the end");
            t.Equals(s.peakEnd, 25001u, "peak");
            t.Equals(s.frames, uint64_t{3}, "frames");
            t.Equals(s.overrunFrames, uint64_t{2}, "frames with an overrun");

            t.IsTrue(a.setLevel("AI front end"), "a new level");
            t.IsTrue(!a.setLevel("AI front end"), "the same level");
            a.noteAllocation(5632u * 64u, 64u, 0x100000u);
            t.Equals(a.stats("AI front end").allocations, uint64_t{1}, "the new level's count");
            t.Equals(a.stats("AI Local").allocations, uint64_t{4}, "the old level's kept");
            a.setLevel("AI Local");
            a.noteAllocation(5632u * 64u, 64u, 0x100000u);
            t.Equals(a.stats("AI Local").allocations, uint64_t{5}, "played again: added to");
            t.Equals(a.all().size(), size_t{2}, "two levels");
            const std::string line = ga::Audit::describe("AI Local", a.stats("AI Local"));
            t.IsTrue(line.find("2 past the end (16384)") != std::string::npos, line);
        });
    });
}
