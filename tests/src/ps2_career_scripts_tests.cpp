// Aggressive Inline's career scripts setting (game_overrides/
// aggressive_inline_careerscripts.cpp): the edit that puts the PS2's texts
// back in the Xbox scripts, on a script written for the test, and on the
// Xbox files themselves when PS2X_XBOX_DIR has them.
#include "MiniTest.h"

#include "aggressive_inline_careerscripts.h"
#include "runtime/ps2_xbox_disc.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace cs = aggressive_inline::careerscripts;

    constexpr uint32_t kEnd = 0xFFFFFFFFu;
    constexpr uint32_t kPush = 0x0Au, kCall = 0x1Eu, kJumpIfNotZero = 0x6Eu, kEndOp = 0x1F40u;

    struct Script
    {
        uint32_t codeWords = 0, variableWords = 0, dataBytes = 0, stringBytes = 0;
        std::vector<uint8_t> strings, data;
        std::vector<uint32_t> variables, code;
    };

    void putWords(std::vector<uint8_t> &out, const std::vector<uint32_t> &words)
    {
        const size_t at = out.size();
        out.resize(at + words.size() * 4u);
        if (!words.empty())
            std::memcpy(out.data() + at, words.data(), words.size() * 4u);
    }

    std::vector<uint8_t> build(const Script &s)
    {
        std::vector<uint8_t> out(0x14, 0);
        const float version = 2.73f;
        const uint32_t header[4] = {uint32_t(s.code.size()), uint32_t(s.variables.size()), uint32_t(s.data.size()),
                                    uint32_t(s.strings.size())};
        std::memcpy(out.data(), &version, 4);
        std::memcpy(out.data() + 4, header, 16);
        out.insert(out.end(), s.strings.begin(), s.strings.end());
        out.insert(out.end(), s.data.begin(), s.data.end());
        putWords(out, s.variables);
        putWords(out, s.code);
        return out;
    }

    bool parse(const std::vector<uint8_t> &bin, Script &s)
    {
        if (bin.size() < 0x14)
            return false;
        std::memcpy(&s.codeWords, bin.data() + 4, 4);
        std::memcpy(&s.variableWords, bin.data() + 8, 4);
        std::memcpy(&s.dataBytes, bin.data() + 12, 4);
        std::memcpy(&s.stringBytes, bin.data() + 16, 4);
        size_t at = 0x14;
        if (at + s.stringBytes + s.dataBytes + (size_t(s.variableWords) + s.codeWords) * 4u != bin.size())
            return false;
        s.strings.assign(bin.begin() + at, bin.begin() + at + s.stringBytes);
        at += s.stringBytes;
        s.data.assign(bin.begin() + at, bin.begin() + at + s.dataBytes);
        at += s.dataBytes;
        s.variables.resize(s.variableWords);
        std::memcpy(s.variables.data(), bin.data() + at, s.variableWords * 4u);
        at += s.variableWords * 4u;
        s.code.resize(s.codeWords);
        std::memcpy(s.code.data(), bin.data() + at, s.codeWords * 4u);
        return true;
    }

    // One object, one state, one event; `handler` its instructions.
    Script oneHandler(const std::vector<uint32_t> &handler)
    {
        Script s;
        s.strings = {'a', 'b', 'c', 0};
        s.data = {1, 2, 3, 4};
        s.variables = {cs::nameHash("idle"), 1u, 0u};
        s.code = {
            cs::nameHash("obj"), 4u, kEnd, 0u, // objects
            0u, 0u,                            // the object: its variables at 0
            cs::nameHash("idle"), 6u, kEnd, 0u, // its states: idle at 4 + 6
            cs::nameHash("go"), 4u, kEnd, 0u,  // idle's events: go at 10 + 4
        };
        s.code.insert(s.code.end(), handler.begin(), handler.end());
        return s;
    }

    // The handler an event leads to (one object, one state), up to End.
    std::vector<uint32_t> handlerOf(const Script &s, uint32_t object, uint32_t state, uint32_t event)
    {
        const auto find = [&](size_t list, uint32_t hash) -> size_t
        {
            for (size_t i = list; i + 1 < s.code.size() && s.code[i] != kEnd; i += 2)
                if (s.code[i] == hash)
                    return i;
            return s.code.size();
        };
        const size_t o = find(0, object);
        if (o >= s.code.size())
            return {};
        const size_t rec = s.code[o + 1];
        const size_t st = find(rec + 2, state);
        if (st >= s.code.size())
            return {};
        const size_t stateAt = rec + s.code[st + 1];
        const size_t ev = find(stateAt, event);
        if (ev >= s.code.size())
            return {};
        std::vector<uint32_t> out;
        for (size_t i = stateAt + s.code[ev + 1]; i + 1 < s.code.size(); i += 2)
        {
            out.push_back(s.code[i]);
            out.push_back(s.code[i + 1]);
            if (s.code[i] == kEndOp)
                break;
        }
        return out;
    }

    bool contains(const std::vector<uint32_t> &words, const std::vector<uint32_t> &part)
    {
        if (part.size() > words.size())
            return false;
        for (size_t i = 0; i + part.size() <= words.size(); i += 2)
            if (std::equal(part.begin(), part.end(), words.begin() + i))
                return true;
        return false;
    }
}

void register_ps2_career_scripts_tests()
{
    MiniTest::Case("CareerScripts", [](TestCase &tc)
    {
        tc.Run("name hashes", [](TestCase &t)
        {
            t.Equals(cs::nameHash("displayOverlayText"), 0x0204A5A4u, "displayOverlayText");
            t.Equals(cs::nameHash("playCameraAnim"), 0x0A08339Du, "playCameraAnim");
            t.Equals(cs::nameHash("COMPLETE"), cs::nameHash("complete"), "case-insensitive");
            t.Equals(cs::nameHash("complete"), 0x04250E05u, "complete");
        });

        tc.Run("code and text appended, the event pointed at the copy", [](TestCase &t)
        {
            const Script before = oneHandler({kPush, 1u, kPush, 2u, kCall, 0x1234u, kEndOp, 0u});
            std::vector<uint8_t> bin = build(before);
            const cs::Instruction code[] = {{kPush, cs::kNewString}, {kCall, 0x5678u}};
            t.IsTrue(cs::insertIntoHandler(bin, cs::nameHash("obj"), cs::nameHash("idle"), cs::nameHash("go"), 1u, code, 2u,
                                           "hello"),
                     "inserted");
            Script after;
            t.IsTrue(parse(bin, after), "well formed");
            t.Equals(after.variables, before.variables, "the variables as they were");
            t.Equals(after.data, before.data, "the data as it was");
            t.Equals(after.strings, std::vector<uint8_t>({'a', 'b', 'c', 0, 'h', 'e', 'l', 'l', 'o', 0, 0, 0}),
                     "the text appended, padded to words");
            t.Equals(after.codeWords, uint32_t(before.code.size() + 12u), "the copy appended");
            t.IsTrue(std::equal(before.code.begin() + 14, before.code.end(), after.code.begin() + 14),
                     "the old handler kept");
            t.Equals(after.code[11], uint32_t(before.code.size() - 10u), "the event at the copy");
            t.Equals(handlerOf(after, cs::nameHash("obj"), cs::nameHash("idle"), cs::nameHash("go")),
                     std::vector<uint32_t>({kPush, 1u, kPush, 4u, kCall, 0x5678u, kPush, 2u, kCall, 0x1234u, kEndOp, 0u}),
                     "the code after the first instruction, the text's offset in");
        });

        tc.Run("refused: unknown event, too short, a jump across", [](TestCase &t)
        {
            const cs::Instruction code[] = {{kPush, 7u}};
            std::vector<uint8_t> bin = build(oneHandler({kPush, 1u, kEndOp, 0u}));
            const std::vector<uint8_t> original = bin;
            t.IsFalse(cs::insertIntoHandler(bin, cs::nameHash("obj"), cs::nameHash("idle"), cs::nameHash("stop"), 0u, code, 1u,
                                            nullptr),
                      "no such event");
            t.IsFalse(cs::insertIntoHandler(bin, cs::nameHash("obj"), cs::nameHash("idle"), cs::nameHash("go"), 2u, code, 1u,
                                            nullptr),
                      "past the End");
            t.Equals(bin, original, "unchanged");
            std::vector<uint8_t> jumps = build(oneHandler({kJumpIfNotZero, 1u, kPush, 2u, kEndOp, 0u}));
            const std::vector<uint8_t> jumpsOriginal = jumps;
            t.IsFalse(cs::insertIntoHandler(jumps, cs::nameHash("obj"), cs::nameHash("idle"), cs::nameHash("go"), 1u, code,
                                            1u, nullptr),
                      "a jump over the place");
            t.Equals(jumps, jumpsOriginal, "unchanged");
            t.IsTrue(cs::insertIntoHandler(jumps, cs::nameHash("obj"), cs::nameHash("idle"), cs::nameHash("go"), 0u, code, 1u,
                                           nullptr),
                     "before the jump");
        });

        tc.Run("only the Xbox files it is for", [](TestCase &t)
        {
            std::vector<uint8_t> bin = build(oneHandler({kPush, 1u, kEndOp, 0u}));
            const std::vector<uint8_t> original = bin;
            t.Equals(cs::restorePs2Texts("MOVIE", bin), cs::Restored::Unrecognised, "another Movie Lot script");
            t.Equals(cs::restorePs2Texts("HANGER", bin), cs::Restored::Unrecognised, "another Hangar script");
            t.Equals(cs::restorePs2Texts("CIVIC", bin), cs::Restored::None, "nothing for Civic");
            t.Equals(bin, original, "unchanged");
        });

        tc.Run("the Xbox scripts (with PS2X_XBOX_DIR)", [](TestCase &t)
        {
            if (!std::getenv("PS2X_XBOX_DIR"))
                return;
            ps2_xbox_disc::reset();
            if (!ps2_xbox_disc::init())
                return;
            struct Level
            {
                const char *folder, *object, *state, *event, *text;
            };
            const Level levels[] = {
                {"MOVIE", "scriptObj_playerOne", "init", "enter", "Use the select button to view the current challenges."},
                {"HANGER", "AnimObj_StrayDrop", "active", "syncStart", "Turret challenge complete!"},
            };
            for (const Level &level : levels)
            {
                std::vector<uint8_t> bin;
                if (!ps2_xbox_disc::readFile(std::string("levels/") + level.folder + "/" + level.folder + ".bin", bin))
                    continue;
                Script before, after;
                t.IsTrue(parse(bin, before), std::string(level.folder) + ": read");
                t.Equals(cs::restorePs2Texts(level.folder, bin), cs::Restored::Patched, std::string(level.folder) + ": patched");
                t.IsTrue(parse(bin, after), std::string(level.folder) + ": well formed");
                t.Equals(after.variables, before.variables, std::string(level.folder) + ": the variables as they were");
                t.Equals(after.data, before.data, std::string(level.folder) + ": the data as it was");
                size_t changed = 0;
                for (size_t i = 0; i < before.code.size() && i < after.code.size(); ++i)
                    changed += before.code[i] != after.code[i];
                t.IsTrue(after.code.size() > before.code.size() && changed == 1u,
                         std::string(level.folder) + ": code appended, one word (the event's offset) changed");
                const std::string text(reinterpret_cast<const char *>(after.strings.data()) + before.strings.size());
                t.Equals(text, std::string(level.text), std::string(level.folder) + ": the text appended");
                const std::vector<uint32_t> handler = handlerOf(after, cs::nameHash(level.object), cs::nameHash(level.state),
                                                                cs::nameHash(level.event));
                t.IsTrue(contains(handler, {kPush, uint32_t(before.strings.size()), kCall, cs::nameHash("displayOverlayText")}),
                         std::string(level.folder) + ": the event shows the text");
                // PS2X_CAREER_SCRIPTS_DUMP=<dir>: the patched script written
                // there (to disassemble it).
                if (const char *dump = std::getenv("PS2X_CAREER_SCRIPTS_DUMP"))
                {
                    std::ofstream out(std::string(dump) + "/" + level.folder + ".BIN", std::ios::binary);
                    out.write(reinterpret_cast<const char *>(bin.data()), std::streamsize(bin.size()));
                }
                // Once only.
                t.Equals(cs::restorePs2Texts(level.folder, bin), cs::Restored::Unrecognised,
                         std::string(level.folder) + ": not again");
            }
            ps2_xbox_disc::reset();
        });
    });
}
