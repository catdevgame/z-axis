#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace aggressive_inline
{
    // The Xbox release's career scripts (PS2X_CAREER_SCRIPTS=xbox, or ps2
    // to keep the PS2's for levels not yet played): their bug fixes. A
    // level a skater has played keeps the script its saved progress is from.
    void installXboxCareerScripts(PS2Runtime &runtime);
}

namespace aggressive_inline::careerscripts
{
    enum class Restored
    {
        None,         // nothing to put back in this level's script
        Patched,      // put back
        Unrecognised, // not the Xbox file the patch is for: left as it was
    };

    // Puts back in an Xbox career script (in memory) the two texts the Xbox
    // release lost against the PS2's: the Movie Lot's first-start hint about
    // the select button and the Hangar turret's "challenge complete". `folder`
    // is the level's ("MOVIE"). The data block (the saved variables) and
    // everything already in the script stay where they are.
    Restored restorePs2Texts(const std::string &folder, std::vector<uint8_t> &script);

    // The edit it is made of (for the tests).
    struct Instruction
    {
        uint32_t opcode;
        uint32_t operand;
    };
    constexpr uint32_t kNewString = 0xFFFFFFFEu; // an operand: the offset of the string appended

    // Appends `text` (if not null) to the strings and a copy of the
    // object's state's event handler to the code, with `code` put in after
    // its first `keep` instructions, and points the event at the copy. The
    // old handler stays (unused); nothing moves, so no offset changes.
    // False (the script unchanged) if the handler is not there or is
    // shorter than `keep`.
    bool insertIntoHandler(std::vector<uint8_t> &script, uint32_t objectHash, uint32_t stateHash, uint32_t eventHash,
                           uint32_t keep, const Instruction *code, size_t count, const char *text);

    // zge::nameHash: the scripts' names (case-insensitive ELF hash).
    constexpr uint32_t nameHash(const char *name)
    {
        uint32_t h = 0;
        for (; *name; ++name)
        {
            char c = *name;
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            h = (h << 4) + static_cast<uint8_t>(c);
            const uint32_t g = h & 0xF0000000u;
            if (g)
                h ^= g >> 24;
            h &= ~g;
        }
        return h;
    }
}
