#include "MiniTest.h"
#include "runtime/xbox_audio.h"
#include "runtime/zaxis_text.h"
#include "runtime/zaxis_ui.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace zt = zx::zaxis_text;

    void putU32(std::vector<uint8_t> &out, uint32_t value)
    {
        const size_t at = out.size();
        out.resize(at + 4u);
        std::memcpy(out.data() + at, &value, 4);
    }

    // A level script: header, the string block, a body pointing at strings.
    std::vector<uint8_t> levelScript(const std::vector<std::string> &strings, const std::vector<uint32_t> &bodyStrings,
                                     uint32_t filler)
    {
        std::vector<uint8_t> block;
        std::vector<uint32_t> offsets;
        for (const std::string &s : strings)
        {
            offsets.push_back(static_cast<uint32_t>(block.size()));
            block.insert(block.end(), s.begin(), s.end());
            block.push_back(0);
        }
        std::vector<uint8_t> out;
        const float version = 2.7f;
        out.resize(4);
        std::memcpy(out.data(), &version, 4);
        putU32(out, 1);
        putU32(out, 2);
        putU32(out, 3);
        putU32(out, static_cast<uint32_t>(block.size()));
        out.insert(out.end(), block.begin(), block.end());
        for (uint32_t index : bodyStrings)
        {
            putU32(out, filler);
            putU32(out, offsets[index]);
        }
        return out;
    }

    std::vector<uint8_t> chunk(const char *tag, const std::vector<uint8_t> &data)
    {
        std::vector<uint8_t> out(tag, tag + 4);
        putU32(out, static_cast<uint32_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }

    std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
    {
        std::vector<uint8_t> out;
        for (const auto &p : parts)
            out.insert(out.end(), p.begin(), p.end());
        return out;
    }

    std::vector<uint8_t> text(const std::string &s)
    {
        std::vector<uint8_t> out(s.begin(), s.end());
        out.push_back(0);
        return out;
    }

    // A UAST: its HAST source path, then a UFNT or UMTL with `payload`.
    std::vector<uint8_t> asset(const std::string &source, bool font, const std::string &payload)
    {
        return chunk("UAST", concat({chunk("HAST", text(source)),
                                     chunk(font ? "UFNT" : "UMTL", std::vector<uint8_t>(payload.begin(), payload.end()))}));
    }

    // A picture asset: UAST > HAST, UMTL > TXRC > TXRH (name at +0x14) and the picture.
    std::vector<uint8_t> picture(const std::string &name, const std::string &pixels)
    {
        std::vector<uint8_t> txrh(0x34, 0);
        std::memcpy(txrh.data() + 0x14, name.data(), std::min<size_t>(name.size(), 0x1F));
        return chunk("UAST", concat({chunk("HAST", text("C:\\shots\\" + name + ".zmp")),
                                     chunk("UMTL", concat({chunk("HMTL", std::vector<uint8_t>(8, 0)),
                                                           chunk("TXRC", concat({chunk("TXRH", txrh),
                                                                                 chunk("TZMP", std::vector<uint8_t>(pixels.begin(), pixels.end()))}))}))}));
    }

    // A multi-page HFNT: pages (name) and glyphs (character, page, x).
    std::vector<uint8_t> hfnt(const std::vector<std::string> &pages, const std::vector<std::array<uint8_t, 3>> &glyphs)
    {
        std::vector<uint8_t> out{11, 0, static_cast<uint8_t>(glyphs.size()), 0, static_cast<uint8_t>(pages.size()), 0};
        for (const std::string &name : pages)
        {
            std::vector<uint8_t> page(40, 0);
            page[1] = 1;
            page[3] = 1;
            std::memcpy(page.data() + 4, name.data(), std::min<size_t>(name.size(), 31));
            out.insert(out.end(), page.begin(), page.end());
        }
        for (const auto &g : glyphs)
        {
            std::vector<uint8_t> glyph{g[0], g[1], 0, 0, g[2], 0, 1, 0, 63, 0, 63, 0};
            out.insert(out.end(), glyph.begin(), glyph.end());
        }
        out.resize((out.size() + 3u) & ~size_t{3});
        return chunk("HFNT", out);
    }

    std::vector<uint8_t> fontPage(const std::string &pixels)
    {
        return concat({chunk("HMTL", std::vector<uint8_t>{0x11, 1, 0, 0}),
                       chunk("TXRC", chunk("TZMP", std::vector<uint8_t>(pixels.begin(), pixels.end())))});
    }
}

void register_zaxis_text_tests()
{
    MiniTest::Case("ZAxisText", [](TestCase &tc)
    {
        tc.Run("key hash", [](TestCase &t)
        {
            t.Equals(zt::keyHash("KSTR_CONTINUE"), 0x04B160A5u, "KSTR_CONTINUE");
            t.Equals(zt::keyHash("kstr_resumegame"), 0x019D9015u, "any case");
        });

        tc.Run("string table round trip", [](TestCase &t)
        {
            zt::StringTable table;
            table.entries = {{0x10u, "Resume Game"}, {0x20u, "R\xe9sum\xe9"}, {0x30u, ""}};
            const std::vector<uint8_t> bytes = zt::writeStringTable(table);
            uint32_t size = 0;
            std::memcpy(&size, bytes.data(), 4);
            t.Equals(size_t(size), bytes.size(), "size word");
            zt::StringTable back;
            t.IsTrue(zt::readStringTable(bytes, back), "read");
            t.Equals(back.entries.size(), size_t(3), "count");
            t.IsTrue(back.find(0x20u) && *back.find(0x20u) == "R\xe9sum\xe9", "Latin-1 kept");
            t.IsTrue(back.find(0x40u) == nullptr, "missing key");
            std::vector<uint8_t> bad = bytes;
            bad.resize(12);
            t.IsFalse(zt::readStringTable(bad, back), "truncated");
        });

        tc.Run("level strings paired through the body", [](TestCase &t)
        {
            // The same script in two languages: the strings move, the body
            // points at them.
            const auto english = levelScript({"", "Talking to people", "none", "lessonOne", "Fakie"}, {1, 2, 3, 4, 1}, 7);
            const auto french =
                levelScript({"", "Engager la conversation", "aucun", "Le\xe7onUn", "Fakie"}, {1, 2, 3, 4, 1}, 7);
            zt::LevelText a, b;
            t.IsTrue(zt::readLevelText(english, a) && zt::readLevelText(french, b), "read");
            t.Equals(a.strings.size(), size_t(5), "strings");
            const auto pairs = zt::pairLevelText(a, b);
            bool talking = false, fakie = false;
            for (const auto &[en, fr] : pairs)
            {
                talking |= en == "Talking to people" && fr == "Engager la conversation";
                fakie |= en == "Fakie" && fr == "Fakie";
            }
            t.IsTrue(talking && fakie, "paired");
            const auto other = levelScript({"x"}, {0}, 7);
            zt::LevelText c;
            t.IsTrue(zt::readLevelText(other, c), "read other");
            t.IsTrue(zt::pairLevelText(a, c).empty(), "bodies of another size pair nothing");
        });

        tc.Run("translation lookups", [](TestCase &t)
        {
            zt::Translation tr;
            tr.add("Resume Game", "Reprendre");
            tr.add("Resume Game", "second");
            tr.add("Same", "Same");
            tr.add("a", "SAUVEGARDER");
            tr.add("HINT\\nSometimes", "ASTUCE\\nParfois");
            tr.add("Jump", "Saut");
            tr.add("'s Stats", " Stats");
            tr.add("Next Level", "Niveau suivant");
            tr.add("All Hidden Challenges", "Tous les d\xe9" "fis cach\xe9s");
            tr.add("Off", "Non");
            tr.add("Complete", "Termin\xe9");
            t.Equals(tr.translate("Resume Game"), std::string("Reprendre"), "exact, first wins");
            t.Equals(tr.translate("Same"), std::string(), "a pair of equal texts is left out");
            t.Equals(tr.translate("A"), std::string(), "single characters are left out");
            t.Equals(tr.translate("RESUME GAME"), std::string("REPRENDRE"), "upper case");
            t.Equals(tr.translate("  Resume Game "), std::string("  Reprendre "), "spaces kept");
            t.Equals(tr.translate("HINT\nSometimes"), std::string("ASTUCE\nParfois"), "escaped line breaks");
            t.Equals(tr.translate("JUMP:"), std::string("SAUT:"), "before a colon");
            t.Equals(tr.translate("Jump: 3/10"), std::string("Saut: 3/10"), "a value after the colon");
            t.Equals(tr.translate("JAREN's Stats:"), std::string("JAREN Stats:"), "a name and an ending before a colon");
            t.Equals(tr.translate("JAREN's Stats:\r\nJump: #RGB{1 1 1}1"), std::string("JAREN Stats:\r\nSaut: #RGB{1 1 1}1"),
                     "the player select's stats");
            t.Equals(tr.translate("JAREN's Stats"), std::string("JAREN Stats"), "a name and an ending");
            t.Equals(tr.translate("< Off >"), std::string("< Non >"), "in angle brackets");
            t.Equals(tr.translate("0% Complete"), std::string("0% Termin\xe9"), "after a number");
            t.Equals(tr.translate("#RGB{1 2 3}` #RGB{4 5 6}Next Level #RGB{7 8 9}(all hidden Challenges)"),
                     std::string("#RGB{1 2 3}` #RGB{4 5 6}Niveau suivant #RGB{7 8 9}(Tous les d\xe9" "fis cach\xe9s)"),
                     "markup and brackets");
            t.Equals(tr.translate("Stats:\nJump: #RGB{1 1 1}1"), std::string("Stats:\nSaut: #RGB{1 1 1}1"), "line by line");
            t.Equals(tr.translate("Jump:\r\n 3\r\nNext Level:\r\n 4"), std::string("Saut:\r\n 3\r\nNiveau suivant:\r\n 4"),
                     "every line, not the first with the rest after its colon");
            tr.add("Continue for", "Continuer pour");
            tr.add("pts", "Pkt");
            t.Equals(tr.translate("Continue for 0 pts"), std::string("Continuer pour 0 Pkt"), "words, then a number");
            tr.add("Speed", "Velocidad");
            t.Equals(tr.translate("Speed: #RGB{1 1 1}      2#RGB{2 2 2}/10\nJump: #RGB{1 1 1}      1"),
                     std::string("Velocidad: #RGB{1 1 1}  2#RGB{2 2 2}/10\nSaut: #RGB{1 1 1}      1"),
                     "a longer word takes padding spaces from its line");
            tr.add("Section", "Bereich");
            t.Equals(tr.translate("L1 R1  Section"), std::string("L1 R1  Bereich"), "words after a wide gap");
            t.Equals(tr.translate("Unknown text"), std::string(), "unknown");
            tr.add("Pad", "Manette");
            t.Equals(tr.translate("Pad: Jump"), std::string("Manette: Saut"), "the words after a colon too");
            t.Equals(tr.translate("Pad: Grob"), std::string("Manette: Grob"), "else as they are");
            tr.add("Start at the title screen.", "D\xe9marrer sur l'\xe9" "cran titre.");
            tr.add("Applies at next start.", "Au prochain d\xe9marrage.");
            t.Equals(tr.translate("Start at the title screen. Applies at next start."),
                     std::string("D\xe9marrer sur l'\xe9" "cran titre. Au prochain d\xe9marrage."), "sentence by sentence");
            t.Equals(tr.translate("Start at the title screen. Something else."), std::string(),
                     "only when every sentence has one");
        });

        tc.Run("Latin-1", [](TestCase &t)
        {
            t.Equals(zt::latin1FromUtf8("Fran\xc3\xa7" "ais \xc2\xbf"), std::string("Fran\xe7" "ais \xbf"), "two-byte");
            t.Equals(zt::latin1FromUtf8("\xe2\x82\xac"), std::string("?"), "outside Latin-1");
            t.Equals(zt::asciiUpper("d\xe9" "fis"), std::string("D\xe9" "FIS"), "ASCII only");
        });

        tc.Run("Xbox ADPCM and wave tables", [](TestCase &t)
        {
            // One block: predictor 1000, step index 0, codes 0..
            std::vector<uint8_t> block(36, 0);
            block[0] = 1000 & 0xFF;
            block[1] = 1000 >> 8;
            block[4] = 0x07; // first code +7 (step 7: 7>>3 + 7>>2 + 7>>1 + 7 = 0+1+3+7 = 11)
            const auto pcm = zx::xbox_audio::decodeAdpcm(block.data(), block.size() + 5u);
            t.Equals(pcm.size(), size_t(64), "64 samples, partial block dropped");
            t.Equals(int(pcm[0]), 1000, "header sample");
            t.Equals(int(pcm[1]), 1011, "first code");

            std::vector<uint8_t> header(0x48, 0);
            std::memcpy(header.data(), "ZSNDXBOX", 8);
            const uint32_t count = 2, table = 0x48;
            std::memcpy(header.data() + 0x20, &count, 4);
            std::memcpy(header.data() + 0x24, &table, 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                std::vector<uint8_t> e(84, 0);
                const uint32_t offset = 0x1000u * (i + 1u), size = 36u * (i + 1u), flags = 1;
                std::memcpy(e.data(), &offset, 4);
                std::memcpy(e.data() + 4, &size, 4);
                std::memcpy(e.data() + 8, &flags, 4);
                std::memcpy(e.data() + 20, "line_fr.wav", 11);
                header.insert(header.end(), e.begin(), e.end());
            }
            std::vector<zx::xbox_audio::Wave> waves;
            bool xbox = false;
            t.IsTrue(zx::xbox_audio::readWaveTable(header, waves, &xbox) && xbox, "Xbox table");
            t.Equals(waves.size(), size_t(2), "waves");
            t.Equals(waves[1].offset, 0x2000u, "offset");
            t.Equals(waves[1].name, std::string("line_fr.wav"), "name");
            header.resize(0x60);
            t.IsFalse(zx::xbox_audio::readWaveTable(header, waves), "table past the bytes given");
        });

        tc.Run("fonts from another library", [](TestCase &t)
        {
            const auto hlib = chunk("HLIB", std::vector<uint8_t>(8, 1));
            const auto library = chunk("ULIB", concat({hlib, asset("C:\\ui\\fnt_14pt.zfn", true, "ps2font"),
                                                       asset("C:\\ui\\button.zmp", false, "picture"),
                                                       asset("C:\\ui\\fnt_only_ps2.zfn", true, "keep")}));
            const auto donor = chunk("ULIB", concat({hlib, asset("D:\\xbox\\FNT_14PT.zfn", true, "xbox font, larger"),
                                                     asset("D:\\xbox\\button.zmp", false, "xbox picture")}));
            std::vector<uint8_t> changed = library;
            const auto replaced = zx::zaxis::replaceFonts(changed, donor);
            t.IsTrue(replaced == std::vector<std::string>{"fnt_14pt.zfn"}, "only the font both have");
            const std::string s(changed.begin(), changed.end());
            t.IsTrue(s.find("xbox font, larger") != std::string::npos, "the donor's font");
            t.IsTrue(s.find("picture") != std::string::npos && s.find("xbox picture") == std::string::npos,
                     "pictures kept");
            t.IsTrue(s.find("keep") != std::string::npos, "other fonts kept");
            uint32_t size = 0;
            std::memcpy(&size, changed.data() + 4, 4);
            t.Equals(size_t(size) + 8u, changed.size(), "ULIB size");
            std::vector<uint8_t> same = library;
            t.IsTrue(zx::zaxis::replaceFonts(same, chunk("ULIB", hlib)).empty() && same == library,
                     "nothing to take: unchanged");
        });

        tc.Run("pictures from another library", [](TestCase &t)
        {
            const auto hlib = chunk("HLIB", std::vector<uint8_t>(8, 1));
            const auto library = chunk("ULIB", concat({hlib, picture("cha_chris", "ps2 chris"),
                                                       picture("cha_tinman", "ps2 tinman")}));
            const auto donor = chunk("ULIB", concat({hlib, picture("cha_chris", "xbox chris"),
                                                     picture("cha_ape", "xbox ape"),
                                                     picture("cha_other", "xbox other")}));
            std::vector<uint8_t> changed = library;
            const auto added = zx::zaxis::addAssets(changed, donor, {"cha_ape", "cha_chris", "cha_missing"});
            t.IsTrue(added == std::vector<std::string>{"cha_ape"}, "only the one the library lacks");
            const std::string s(changed.begin(), changed.end());
            t.IsTrue(s.find("xbox ape") != std::string::npos && s.find("ps2 chris") != std::string::npos &&
                         s.find("xbox chris") == std::string::npos && s.find("xbox other") == std::string::npos,
                     "added after the library's own, the rest kept");
            t.IsTrue(s.find("xbox ape") > s.find("ps2 tinman"), "last");
            uint32_t size = 0;
            std::memcpy(&size, changed.data() + 4, 4);
            t.Equals(size_t(size) + 8u, changed.size(), "ULIB size");
            std::vector<uint8_t> same = library;
            t.IsTrue(zx::zaxis::addAssets(same, donor, {"cha_tinman"}).empty() && same == library,
                     "nothing to add: unchanged");
        });

        tc.Run("font pages from a font of their own", [](TestCase &t)
        {
            const auto hlib = chunk("HLIB", std::vector<uint8_t>(8, 1));
            const auto icons = chunk("UAST", concat({chunk("HAST", text("S:\\Fonts\\fnt_lvl_icons.zfn")),
                                                     chunk("UFNT", concat({hfnt({"t0", "t1"}, {{'q', 0, 1}, {'Q', 0, 65}, {'a', 1, 1}}),
                                                                           fontPage("page zero"), fontPage("page one")}))}));
            const auto library = chunk("ULIB", concat({hlib, asset("C:\\ui\\fnt_14pt.zfn", true, "other font"), icons,
                                                       picture("cha_chris", "ps2 chris")}));
            const auto pages = chunk("UFNT", concat({hfnt({"t2"}, {{'b', 0, 1}, {'B', 0, 65}}), fontPage("page two")}));
            std::vector<uint8_t> changed = library;
            std::string error;
            t.IsTrue(zx::zaxis::appendFontPages(changed, "fnt_lvl_icons.zfn", pages, &error), "appended: " + error);
            uint32_t size = 0;
            std::memcpy(&size, changed.data() + 4, 4);
            t.Equals(size_t(size) + 8u, changed.size(), "ULIB size");
            const std::string s(changed.begin(), changed.end());
            const size_t at = s.find("HFNT");
            t.IsTrue(at != std::string::npos, "HFNT");
            const uint8_t *h = changed.data() + at + 8;
            t.Equals(int(h[2]), 5, "glyphs");
            t.Equals(int(h[4]), 3, "pages");
            t.IsTrue(std::memcmp(h + 6 + 80 + 4, "t2", 2) == 0, "the page record after the font's");
            const uint8_t *g = h + 6 + 120;
            t.IsTrue(g[0] == 'q' && g[24] == 'a' && g[25] == 1 && g[36] == 'b' && g[37] == 2 && g[48] == 'B' && g[49] == 2,
                     "the glyphs after the font's, on page 2");
            t.IsTrue(s.find("page zero") < s.find("page one") && s.find("page one") < s.find("page two") &&
                         s.find("page two") < s.find("ps2 chris"),
                     "the page after the font's, inside its asset");
            uint32_t uast = 0, ufnt = 0;
            const size_t uastAt = s.find("UAST", s.find("other font"));
            std::memcpy(&uast, changed.data() + uastAt + 4, 4);
            std::memcpy(&ufnt, changed.data() + s.find("UFNT", uastAt) + 4, 4);
            t.Equals(size_t(uastAt) + 8u + uast, s.find("UAST", uastAt + 4), "UAST size");
            t.Equals(s.find("UFNT", uastAt) + 8u + ufnt, s.find("UAST", uastAt + 4), "UFNT size");
            std::vector<uint8_t> same = library;
            const auto clash = chunk("UFNT", concat({hfnt({"t2"}, {{'a', 0, 1}}), fontPage("page two")}));
            t.IsFalse(zx::zaxis::appendFontPages(same, "fnt_lvl_icons.zfn", clash) || same != library,
                      "a character in both: unchanged");
            t.IsFalse(zx::zaxis::appendFontPages(same, "fnt_missing.zfn", pages) || same != library,
                      "no such font: unchanged");
        });
    });
}
