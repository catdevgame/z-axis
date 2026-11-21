#include "MiniTest.h"
#include "collection/settings.h"
#include "runtime/gs/gs_quality.h"
#include "runtime/gs/gs_texture_mips.h"

#include <string>
#include <vector>

namespace gq = zx::gs_quality;
namespace gm = zx::gs_mips;

void register_ps2_gs_quality_tests()
{
    MiniTest::Case("GSQuality", [](TestCase &tc)
    {
        tc.Run("options: defaults, names, clamping", [](TestCase &t)
        {
            const auto d = gq::defaults();
            t.Equals(d[static_cast<size_t>(gq::Option::AntiAliasing)], 1, "FXAA by default");
            t.Equals(d[static_cast<size_t>(gq::Option::Anisotropy)], 4, "16x by default");
            t.Equals(d[static_cast<size_t>(gq::Option::Lighting)], 1, "Xbox character lighting by default");
            int l = -1;
            t.IsTrue(gq::parseValue(gq::Option::Lighting, "PS2", l) && l == 0, "character_lighting = ps2");
            int v = -1;
            t.IsTrue(gq::parseValue(gq::Option::Fog, "Smooth", v) && v == 1, "fog smooth by name, any case");
            t.IsTrue(gq::parseValue(gq::Option::Anisotropy, "8x", v) && v == 3, "8x is index 3");
            t.IsTrue(gq::parseValue(gq::Option::Perspective, "false", v) && v == 0, "toggles take false");
            t.IsTrue(gq::parseValue(gq::Option::Anisotropy, "2", v) && v == 2, "an index");
            t.IsTrue(!gq::parseValue(gq::Option::Fog, "fancy", v), "an unknown name");
            t.Equals(gq::valueName(gq::Option::DepthPrecision, 1), std::string("high"), "value names");
            gq::set(gq::Option::Anisotropy, 9);
            t.Equals(gq::get(gq::Option::Anisotropy), 4, "set clamps to the choices");
            t.Equals(gq::anisotropy(), 16, "16x");
            gq::set(gq::Option::Anisotropy, 0);
            t.Equals(gq::anisotropy(), 1, "off");
            gq::set(gq::Option::Anisotropy, d[static_cast<size_t>(gq::Option::Anisotropy)]);
        });

        tc.Run("settings.ini [graphics] quality keys round trip", [](TestCase &t)
        {
            std::vector<std::string> warnings;
            const zx::app::Settings s = zx::app::parseSettings(
                "[graphics]\nanti_aliasing = off\ntexture_filtering = 4x\nfog = original\nperspective_correct = false\n", &warnings);
            t.IsTrue(warnings.empty(), "no warnings");
            t.Equals(s.quality[static_cast<size_t>(gq::Option::AntiAliasing)], 0, "anti_aliasing off");
            t.Equals(s.quality[static_cast<size_t>(gq::Option::Anisotropy)], 2, "4x");
            t.Equals(s.quality[static_cast<size_t>(gq::Option::Fog)], 0, "fog original");
            t.Equals(s.quality[static_cast<size_t>(gq::Option::Shadows)], 1, "shadows left at the default");
            t.Equals(s.quality[static_cast<size_t>(gq::Option::Perspective)], 0, "perspective off");
            const zx::app::Settings back = zx::app::parseSettings(zx::app::formatSettings(s), &warnings);
            t.IsTrue(back.quality == s.quality, "formatSettings writes them back");
        });

        tc.Run("mip chain: box filter, alpha-weighted colour, 1 x n sides", [](TestCase &t)
        {
            t.Equals(gm::chainLength(256, 64), 9u, "256 x 64 has 9 levels");
            gm::Level l;
            l.width = 2;
            l.height = 2;
            // Two opaque red texels, two clear green ones.
            l.texels = {0xFF0000FFu, 0x0000FF00u, 0xFF0000FFu, 0x0000FF00u};
            const gm::Level plain = gm::downsample(l, false);
            t.Equals(plain.texels.size(), static_cast<size_t>(1), "one texel");
            t.Equals(plain.texels[0], 0x80008080u, "plain mean (rounded)");
            const gm::Level w = gm::downsample(l, true);
            t.Equals(w.texels[0], 0x800000FFu, "clear texels give no colour");
            std::vector<gm::Level> chain{l};
            gm::completeChain(chain, true);
            t.Equals(chain.size(), static_cast<size_t>(2), "2 x 2 completes to 1 x 1");
            gm::Level row;
            row.width = 4;
            row.height = 1;
            row.texels = {0xFF000000u, 0xFF000002u, 0xFF000004u, 0xFF000006u};
            const gm::Level r = gm::downsample(row, false);
            t.IsTrue(r.width == 2u && r.height == 1u && r.texels[0] == 0xFF000001u && r.texels[1] == 0xFF000005u,
                     "a row of height 1 halves its width only");
            t.Equals(gm::applyTexa(0x00102030u, 1u, 0x40u, 0x80u, true), 0x40102030u, "24-bit: TA0");
            t.Equals(gm::applyTexa(0x00000000u, 1u, 0x40u, 0x80u, true), 0x00000000u, "24-bit black with AEM: 0");
            t.Equals(gm::applyTexa(0x80102030u, 2u, 0x40u, 0x7Fu, false), 0x7F102030u, "16-bit alpha bit: TA1");
        });
    });
}
