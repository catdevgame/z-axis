// Aggressive Inline (SLUS-20327): the Xbox release's career scripts
// (ZX_CAREER_SCRIPTS=xbox; the app: Settings > Game > Career Scripts,
// settings.ini [gameplay] career_scripts = xbox|ps2, default xbox).
//
// The career runs <LEVEL>.BIN (func_1A4D78). The Xbox release's are the PS2
// ones with bug fixes (docs/BRINGUP.md "Xbox-only content", the survey
// /tmp/ai-xboxextras/levels-survey.md): the Hangar's points NPC no longer
// stays waiting after its 60-second run, the Museum's flagpole no longer
// touches the finished tank challenge, the Industrial crash test's NPC
// finishes, the Movie Lot photographer's second challenge shows as active
// when accepted and its grave run and Civic's bus run cannot be left
// pending by a trick that lands as the time runs out, Civic's "find the
// airfield key" no longer shows another challenge's preview and its four
// window ledges get their own (lawStatueLedges, in the Xbox Civic
// MOTIONS.ZAL, served with it: its other 50 animations are the PS2's byte
// for byte), park editor challenges are not wiped on re-entry, counters are
// reset, completions wait for their reward animation, descriptions say
// what the code checks (Civic's 5,000,000, the Cannery's ten seconds). The
// same challenges, numbers and timers (but Civic's 350,000 trick, which
// the Gameplay Tuning setting covers), the same objects and animations.
// Lost against the PS2's, and put back here (restorePs2Texts, in memory as
// the Xbox file is read): the Movie Lot's first-start hint about the select
// button and the Hangar turret's "challenge complete" text.
// The Tutorial stays the PS2's (the Xbox plays a copy with its pad's texts).
//
// Saved careers: a skater's progress in a level keeps the script's
// variables (record +8, +0xC bytes: the script's data block, restored by
// func_1DF448 as the loaded script's size). Civic's, the Cannery's, the
// Hangar's and Industrial's Xbox scripts have more variables (their data
// blocks 8 or 40 bytes larger, objects after the new ones moved), so each
// level runs the script its saved variables are from, and the setting's
// for a level the skater has not played yet. The Movie Lot's, the Museum's
// and the Boardwalk's have the same layout: the setting's always.

#include "aggressive_inline_careerscripts.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_xbox_disc.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aggressive_inline::careerscripts
{
    namespace
    {
        namespace gf = zx::game_files;

        struct Level
        {
            const char *folder; // ASSETS/LEVELS/<folder>/<folder>.BIN
            uint32_t index;     // the career's level index (0x24C9B4)
            uint32_t ps2Data;   // the PS2 disc's script's data block bytes (header +8, words)
        };
        constexpr Level kLevels[] = {
            {"CIVIC", 0, 530u * 4u},  {"BRDWLK", 1, 388u * 4u}, {"CANNERY", 2, 331u * 4u}, {"HANGER", 3, 243u * 4u},
            {"INDUST", 4, 270u * 4u}, {"MOVIE", 5, 365u * 4u},  {"MUSEUM", 6, 310u * 4u},
        };

        constexpr uint32_t kSkater = 0x0024C9B1u;   // u8: player one's skater
        constexpr uint32_t kGameType = 0x0024C9B3u; // u8: 1 career
        constexpr uint32_t kLevelRecords = 0x0024DDD8u; // skater 0's level 0 record
        constexpr uint32_t kSkaterStride = 0x510u;
        constexpr uint32_t kLevelStride = 0x2Cu;
        constexpr uint32_t kScriptDataSize = 0xCu; // the saved script variables' bytes

        struct Scripts
        {
            uint8_t *rdram = nullptr;
            bool preferXbox = true;
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> xbox; // path -> the Xbox file
            std::map<std::string, uint32_t> xboxData; // path -> the Xbox script's data block bytes
        };
        Scripts &scripts() { return zx::session::state<Scripts>(); }

        uint32_t rd32(uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(scripts().rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        // A script's data block bytes (header: version, code words, data
        // words, ...).
        uint32_t dataBytes(const std::vector<uint8_t> &bin)
        {
            uint32_t words = 0;
            if (bin.size() >= 12u)
                std::memcpy(&words, bin.data() + 8, 4);
            return words * 4u;
        }

        // Which script the skater's saved variables for this level are
        // from: 1 Xbox, 0 PS2, -1 none saved (or neither).
        int savedScript(const Level &level, const std::string &path)
        {
            const uint8_t *skater = getConstMemPtr(scripts().rdram, kSkater);
            if (!skater)
                return -1;
            const uint32_t saved =
                rd32(kLevelRecords + uint32_t(*skater & 0x1Fu) * kSkaterStride + level.index * kLevelStride + kScriptDataSize);
            const uint32_t ps2 = level.ps2Data, xbox = scripts().xboxData[path];
            if (!saved || ps2 == xbox)
                return -1;
            return saved == xbox ? 1 : saved == ps2 ? 0 : -1;
        }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            const uint8_t *type = getConstMemPtr(scripts().rdram, kGameType);
            if (!type || *type != 1u)
                return nullptr; // the language tables read the scripts at start: the PS2's
            for (const Level &level : kLevels)
            {
                const std::string folder = std::string("ASSETS/LEVELS/") + level.folder + "/";
                if (path == folder + "MOTIONS.ZAL" && std::string(level.folder) == "CIVIC")
                {
                    // With the Xbox script, Civic's preview of the window
                    // ledges challenge (opened before the script; served
                    // either way: the PS2 script never asks for it).
                    std::lock_guard<std::mutex> lock(scripts().mutex);
                    auto it = scripts().xbox.find(path);
                    return it != scripts().xbox.end() ? it->second : nullptr;
                }
                if (path != folder + level.folder + ".BIN")
                    continue;
                std::lock_guard<std::mutex> lock(scripts().mutex);
                auto it = scripts().xbox.find(path);
                if (it == scripts().xbox.end())
                    return nullptr;
                const int saved = savedScript(level, path);
                const bool xbox = saved < 0 ? scripts().preferXbox : saved == 1;
                std::cout << "[careerscripts] " << level.folder << ": the " << (xbox ? "Xbox" : "PS2") << " release's script"
                          << (saved < 0 ? "" : " (the skater's saved progress is from it)") << std::endl;
                return xbox ? it->second : nullptr;
            }
            return nullptr;
        }
    }

    // ---- the PS2's texts put back ------------------------------------------
    //
    // The script file (game/aggressive/script/script.h): a header (version,
    // code words, variable words, data bytes, string bytes), then the
    // strings, the data, the variables, the code, each read into its own
    // heap block. Strings are referred to by their offset, handlers by
    // their offset (in words) from their state's event list. So text and
    // code can be appended at the ends of the strings and of the code
    // without moving anything: a handler is changed by appending a changed
    // copy of it and pointing its event at the copy. The variables (the
    // saved careers' block, header +8) are not touched.

    namespace
    {
        constexpr size_t kHeaderBytes = 0x14;
        constexpr uint32_t kOpPush = 0x0Au, kOpStore = 0x14u, kOpCallNative = 0x1Eu, kOpLoad = 0x3Cu;
        constexpr uint32_t kOpSubtractImmediate = 0x50u, kOpJumpIfNotZero = 0x6Eu, kOpSetType = 0xBEu, kOpEnd = 0x1F40u;
        constexpr uint32_t kEndOfList = 0xFFFFFFFFu;

        bool isJump(uint32_t opcode)
        {
            switch (opcode)
            {
            case 0x6Eu: // jumpIfNotZero
            case 0x78u: // jumpIfZero
            case 0x82u: // jumpIfNegative
            case 0x8Cu: // jumpIfPositive
            case 0x96u: // jumpIfNotPositive
            case 0xA0u: // jumpIfNotNegative
            case 0xAAu: // jump
                return true;
            default:
                return false;
            }
        }

        uint32_t word(const std::vector<uint8_t> &bytes, size_t at)
        {
            uint32_t v = 0;
            std::memcpy(&v, bytes.data() + at, 4);
            return v;
        }

        uint64_t fnv1a64(const std::vector<uint8_t> &bytes)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint8_t b : bytes)
                h = (h ^ b) * 0x100000001B3ull;
            return h;
        }

        // The Movie Lot (the PS2 MOVIE.BIN's scriptObj_playerOne.init.enter,
        // after the opening camera): the hint, once (the object's variable 1,
        // unused by the Xbox script, the same place in both, saved with the
        // career; 0 at first). As with the PS2 script, the overlay's five
        // seconds run under the opening fly-through, so it is hardly seen.
        constexpr Instruction kMovieHint[] = {
            {kOpLoad, 1u},
            {kOpSubtractImmediate, 0u},
            {kOpSetType, 5u},
            {kOpJumpIfNotZero, 6u}, // shown already: on after the overlay
            {kOpPush, 1u},
            {kOpStore, 1u},
            {kOpPush, 0x40A00000u}, // 5.0 seconds
            {kOpPush, 54u},         // the text's bytes
            {kOpPush, kNewString},
            {kOpCallNative, nameHash("displayOverlayText")},
        };

        // The Hangar's turret (#6): the PS2 shows the text in
        // scriptObj_daveHelper07.active.glassBreak with the challenge's
        // completion; the Xbox completes it later, in
        // AnimObj_StrayDrop.active.syncStart (as its bar challenges, which
        // show their text there first), and left only a consolePrintString
        // in glassBreak. The text goes first in syncStart.
        constexpr Instruction kTurretText[] = {
            {kOpPush, 0x40900000u}, // 4.5 seconds
            {kOpPush, 27u},         // the text's bytes
            {kOpPush, kNewString},
            {kOpCallNative, nameHash("displayOverlayText")},
        };

        struct Restore
        {
            const char *folder;
            size_t bytes;     // the Xbox file (US release) the patch is for
            uint64_t fnv1a64; // its FNV-1a 64
            const char *object, *state, *event;
            uint32_t keep; // instructions of the handler before the new ones
            const Instruction *code;
            size_t count;
            const char *text; // the PS2's words, as its script has them (the language tables translate by them)
        };
        constexpr Restore kRestores[] = {
            {"MOVIE", 48576u, 0x3FBACA8038675071ull, "scriptObj_playerOne", "init", "enter", 3u, kMovieHint,
             std::size(kMovieHint), "Use the select button to view the current challenges."},
            {"HANGER", 29240u, 0x8858AC27699FB8BBull, "AnimObj_StrayDrop", "active", "syncStart", 0u, kTurretText,
             std::size(kTurretText), "Turret challenge complete!"},
        };
        static_assert(sizeof("Use the select button to view the current challenges.") == 54);
        static_assert(sizeof("Turret challenge complete!") == 27);
    }

    bool insertIntoHandler(std::vector<uint8_t> &script, uint32_t objectHash, uint32_t stateHash, uint32_t eventHash,
                           uint32_t keep, const Instruction *code, size_t count, const char *text)
    {
        if (script.size() < kHeaderBytes)
            return false;
        const uint32_t codeWords = word(script, 0x04), variableWords = word(script, 0x08);
        const uint32_t dataBytes = word(script, 0x0C), stringBytes = word(script, 0x10);
        const uint64_t codeAt = kHeaderBytes + uint64_t(stringBytes) + dataBytes + uint64_t(variableWords) * 4u;
        if (codeAt + uint64_t(codeWords) * 4u > script.size())
            return false;
        std::vector<uint32_t> words(codeWords);
        if (codeWords)
            std::memcpy(words.data(), script.data() + codeAt, size_t(codeWords) * 4u);
        const auto at = [&](uint64_t i, uint32_t &v)
        {
            if (i >= words.size())
                return false;
            v = words[size_t(i)];
            return true;
        };
        // The lists: {hash, offset} pairs up to kEndOfList.
        const auto find = [&](uint64_t list, uint32_t hash, uint64_t &entry)
        {
            uint32_t h = 0;
            for (uint64_t i = list; at(i, h) && h != kEndOfList; i += 2)
                if (h == hash)
                {
                    entry = i;
                    return true;
                }
            return false;
        };
        uint64_t objectEntry = 0, stateEntry = 0, eventEntry = 0;
        uint32_t object = 0, stateOffset = 0, handlerOffset = 0;
        if (!find(0, objectHash, objectEntry) || !at(objectEntry + 1, object) ||
            !find(uint64_t(object) + 2u, stateHash, stateEntry) || !at(stateEntry + 1, stateOffset))
            return false;
        const uint64_t state = uint64_t(object) + stateOffset;
        if (!find(state, eventHash, eventEntry) || !at(eventEntry + 1, handlerOffset))
            return false;
        // The handler, up to and with its End.
        std::vector<Instruction> handler;
        for (uint64_t i = state + handlerOffset;; i += 2)
        {
            Instruction ins{};
            if (!at(i, ins.opcode) || !at(i + 1, ins.operand) || handler.size() > words.size())
                return false;
            handler.push_back(ins);
            if (ins.opcode == kOpEnd)
                break;
        }
        if (keep >= handler.size())
            return false;
        // No jump of the handler's may cross the place the code goes in.
        for (size_t i = 0; i < handler.size(); ++i)
            if (isJump(handler[i].opcode))
            {
                const int64_t target = int64_t(i) + 1 + int32_t(handler[i].operand);
                if ((i < keep) != (target < int64_t(keep)) && target != int64_t(keep))
                    return false;
            }
        // The text, at the end of the strings (padded to words as the
        // compiler's are).
        std::vector<uint8_t> strings(script.begin() + kHeaderBytes, script.begin() + kHeaderBytes + stringBytes);
        const uint32_t textOffset = stringBytes;
        if (text)
        {
            strings.insert(strings.end(), text, text + std::strlen(text) + 1u);
            while (strings.size() % 4u)
                strings.push_back(0);
        }
        // The copy, at the end of the code, and the event pointed at it.
        const uint64_t copyAt = words.size();
        if (copyAt - state > 0xFFFFFFFFull)
            return false;
        for (size_t i = 0; i < handler.size(); ++i)
        {
            if (i == keep)
                for (size_t k = 0; k < count; ++k)
                {
                    words.push_back(code[k].opcode);
                    words.push_back(code[k].operand == kNewString ? textOffset : code[k].operand);
                }
            words.push_back(handler[i].opcode);
            words.push_back(handler[i].operand);
        }
        words[size_t(eventEntry + 1)] = uint32_t(copyAt - state);
        // The file again: header, strings, data and variables as they were,
        // the code, anything after it.
        std::vector<uint8_t> out(script.begin(), script.begin() + kHeaderBytes);
        const uint32_t newCodeWords = uint32_t(words.size()), newStringBytes = uint32_t(strings.size());
        std::memcpy(out.data() + 0x04, &newCodeWords, 4);
        std::memcpy(out.data() + 0x10, &newStringBytes, 4);
        out.insert(out.end(), strings.begin(), strings.end());
        out.insert(out.end(), script.begin() + kHeaderBytes + stringBytes, script.begin() + size_t(codeAt));
        const size_t codeOut = out.size();
        out.resize(codeOut + words.size() * 4u);
        std::memcpy(out.data() + codeOut, words.data(), words.size() * 4u);
        out.insert(out.end(), script.begin() + size_t(codeAt + uint64_t(codeWords) * 4u), script.end());
        script = std::move(out);
        return true;
    }

    Restored restorePs2Texts(const std::string &folder, std::vector<uint8_t> &script)
    {
        for (const Restore &r : kRestores)
        {
            if (folder != r.folder)
                continue;
            if (script.size() != r.bytes || fnv1a64(script) != r.fnv1a64)
                return Restored::Unrecognised;
            std::vector<uint8_t> patched = script;
            if (!insertIntoHandler(patched, nameHash(r.object), nameHash(r.state), nameHash(r.event), r.keep, r.code,
                                   r.count, r.text))
                return Restored::Unrecognised;
            script = std::move(patched);
            return Restored::Patched;
        }
        return Restored::None;
    }

    void install(PS2Runtime &runtime)
    {
        const char *which = zx::boot::text("ZX_CAREER_SCRIPTS");
        if (!which || (std::string(which) != "xbox" && std::string(which) != "ps2"))
            return;
        if (!ps2_xbox_disc::init())
        {
            std::cout << "[careerscripts] no Xbox files (" << ps2_xbox_disc::description() << ")" << std::endl;
            return;
        }
        scripts().rdram = runtime.memory().getRDRAM();
        scripts().preferXbox = std::string(which) == "xbox";
        size_t count = 0;
        for (const Level &level : kLevels)
        {
            const std::string xboxDir = std::string("levels/") + level.folder + "/";
            const std::string path = std::string("ASSETS/LEVELS/") + level.folder + "/" + level.folder + ".BIN";
            auto bytes = std::make_shared<std::vector<uint8_t>>();
            if (!ps2_xbox_disc::readFile(xboxDir + level.folder + ".bin", *bytes) || dataBytes(*bytes) == 0u)
                continue;
            const uint32_t variables = dataBytes(*bytes);
            switch (restorePs2Texts(level.folder, *bytes))
            {
            case Restored::Patched:
                std::cout << "[careerscripts] " << level.folder << ": the PS2's "
                          << (std::string(level.folder) == "MOVIE" ? "first-start hint" : "turret challenge text")
                          << " put back" << std::endl;
                break;
            case Restored::Unrecognised:
                std::cout << "[careerscripts] " << level.folder
                          << ": not the Xbox script expected (size or hash): the PS2's text not put back" << std::endl;
                break;
            case Restored::None:
                break;
            }
            if (dataBytes(*bytes) != variables)
                continue; // never: the patch keeps the variables
            scripts().xboxData[path] = dataBytes(*bytes);
            scripts().xbox[path] = bytes;
            gf::registerProvider(std::string("ASSETS/LEVELS/") + level.folder + "/", &provide, "careerscripts");
            zx::session::onEnd([]() { gf::removeProviders("careerscripts"); });
            ++count;
            if (std::string(level.folder) == "CIVIC")
            {
                auto motions = std::make_shared<std::vector<uint8_t>>();
                if (ps2_xbox_disc::readFile(xboxDir + "motions.zal", *motions))
                    scripts().xbox[std::string("ASSETS/LEVELS/CIVIC/MOTIONS.ZAL")] = motions;
            }
        }
        std::cout << "[careerscripts] " << count << " Xbox career scripts, " << (scripts().preferXbox ? "the Xbox's" : "the PS2's")
                  << " for levels not yet played (saved progress keeps its script)" << std::endl;
    }
}

namespace aggressive_inline
{
    void installXboxCareerScripts(PS2Runtime &runtime)
    {
        careerscripts::install(runtime);
    }
}
