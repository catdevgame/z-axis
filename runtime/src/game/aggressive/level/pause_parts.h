#pragma once

// What the pause menu's pages share: the widget calls under short names, and
// a level's objective (the scripts fill the list, currentObjectives's).

#include "aggressive/career.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/aggressive/level/pause_parts.h.h"

namespace aggressive::pause
{
    struct Objective
    {
        uint32_t lockedArea;   // +0x00 non-zero: in the level's locked area
        uint32_t kind;         // +0x04 kHiddenChallenge
        uint32_t points;       // +0x08
        uint32_t state;        // +0x0C init or complete by name hash, or the running state's
        uint32_t reward;       // +0x10 its row of the experience table (challengeRewards)
        uint32_t objectName;   // +0x14 the script object the crash messages go to (a name hash)
        uint32_t name;         // +0x18 a script string
        uint32_t description;  // +0x1C a script string
        uint32_t hasFlyThru;   // +0x20
        uint32_t flyThru;      // +0x24 a script string: the camera's animation
        uint32_t done;         // +0x28 finished once (the tutorial's page shows it as a checkbox)
    };
    static_assert(sizeof(Objective) == 0x2C);

    // A level's bonus objective (bonusObjectives's list): gaps and the like.
    struct BonusObjective
    {
        uint32_t kind;       // +0x00 8: a gap's (objectives.cpp kBonusKindGap)
        uint32_t state;      // +0x04
        uint32_t points;     // +0x08
        uint32_t objectName; // +0x0C the script object the crash messages go to
        uint32_t name;       // +0x10 a script string
        uint32_t done;       // +0x14
        int32_t experience[kStatCount]; // +0x18 by stat
    };
    static_assert(sizeof(BonusObjective) == 0x34);

    constexpr uint32_t kCurrentObjectives = sym::currentObjectives; // () -> the level's Objective list
    constexpr uint32_t kRunScript = sym::runScript;         // (widget, text, 0), by address where its result is ours

    // A script's CALL hands its function the arguments' widgets.
    using Arguments = ee::Ptr<ee::Ptr<zgui::Node>>;

    inline ee::Ptr<zgui::Node> find(ee::Ptr<zgui::Node> parent, ee::Address path)
    {
        return zgui::findNode(parent, path, uint64_t{1});
    }

    inline void run(ee::Address widget, ee::Address script)
    {
        zgui::runScript(ee::Ptr<zgui::Node>(widget), ee::Ptr<char>(script), 0);
    }

    // setVariable with its answer.
    inline int32_t setResult(ee::Address widget, ee::Address name, ee::Address value)
    {
        return zgui::setVariable(widget, name, value);
    }

    // variable's value as an integer into out.
    inline int32_t intValue(ee::Ptr<zgui::Node> variable, ee::Ptr<int32_t> out)
    {
        return zgui::variableIntValue(variable, out);
    }

    // Variable `name` of widget copied into out: 1, 0 without it.
    inline int32_t copyInto(ee::Address widget, ee::Address name, ee::Address out)
    {
        return zgui::copyVariable(widget, name, out);
    }
}
