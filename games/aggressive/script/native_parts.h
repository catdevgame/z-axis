#pragma once

// What the level scripts' natives share: the stack calls under short names
// and the way a native reaches a skater. A native pops its arguments, calls the game, pushes its result and returns 1
// when it pushed one.

#include "script.h"

#include "aggressive/game_state.h"
#include "aggressive/level/level_object.h"
#include "aggressive/skater/skater.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/aggressive/script/native_parts.h.h"

namespace aggressive::script::parts
{
    using aggressive::parts::g_topState;
    using aggressive::parts::kFirstPlayer;
    using aggressive::parts::kPlayerBytes;
    using aggressive::parts::kTopStateLevel;
    using aggressive::parts::levelObject;
    using aggressive::parts::setLevelState;
    using aggressive::parts::skaterHandle;

    // "NULL skater data handle retrieved from inlineGameGetSkaterHandle(%d)"
    constexpr uint32_t kNullSkaterFormat = sym::nullSkaterFormat;

    // The natives that look a skater up complain into a buffer at the bottom
    // of their frame when there is none, and carry on.
    inline void complainIfNoSkater(ee::Ptr<skater::Player> player, ee::Ptr<char> buffer, int32_t index)
    {
        if (!player)
            zge::formatString(buffer, kNullSkaterFormat, index);
    }
    constexpr uint32_t kComplaintBytes = 0x100;

    inline int32_t popInt() { return scriptPopInt(); }
    inline int32_t popCondition() { return scriptPopCondition(); }
    inline ee::Float popFloat() { return scriptPopFloat(); }
    inline ee::Ptr<char> popString() { return scriptPopString(ee::Ptr<uint32_t>()); }
    inline void pushInt(uint32_t value) { scriptPushInt(value); }
    inline void pushResult(uint32_t value) { scriptPushResult(value); }
    inline void pushFloat(ee::Float value) { scriptPushFloat(value); }

    // Natives that run other handlers keep the machine's ip and frame on the
    // script's stack meanwhile, as SEND and SET do.
    struct SavedMachine
    {
        ee::Ptr<ee::Ptr<ScriptInstruction>> ip;
        ee::Ptr<ee::Ptr<ScriptVariable>> frame;
    };
    inline SavedMachine saveMachine()
    {
        SavedMachine saved{scriptIpRegister(), scriptFrameRegister()};
        pushInt(saved.ip->address);
        pushInt(saved.frame->address);
        return saved;
    }
    inline void restoreMachine(const SavedMachine &saved)
    {
        *saved.frame = ee::Ptr<ScriptVariable>(static_cast<uint32_t>(popInt()));
        *saved.ip = ee::Ptr<ScriptInstruction>(static_cast<uint32_t>(popInt()));
    }

    // The message goes into a buffer in the native's frame, nothing reads
    // it: a debug print the retail game compiled out, its formatting kept.
    inline void formatUnread(uint32_t buffer, uint32_t format, int32_t value)
    {
        zge::formatString(buffer, format, value);
    }
}
