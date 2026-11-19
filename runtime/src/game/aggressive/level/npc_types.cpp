// The NPC type table (npc.h): looked up by a data point's type hash, and its
// animations found when a level's motions are in. The Zoo's override hooks
// both: the Xbox release's six more types.

#include "aggressive/level/npc_types.h"
#include "aggressive/npc.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/aggressive/level/npc_types.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kEmptyString = sym::emptyString3;  // a slot with no animation has this name
        constexpr uint32_t kCouldNotFind = sym::g_textCouldNotFindTheAnimation; // "Could not find the '%s' animation."

        // The challenge bubbles' props by size and colour, and the icons'.
        // The bubbles' table is written as nine words but the icons' starts
        // at its ninth: Icon Break overwrites Bubble RXXL's hash, as in the
        // original.
        constexpr uint32_t kBubbleHashes = sym::bubbleHashes, kIconHashes = sym::bubbleIcons;
        constexpr uint32_t kBubbleNames[] = {sym::g_textPropDefBubbleGL, sym::g_textPropDefBubbleGXL, sym::g_textPropDefBubbleGXXL, sym::g_textPropDefBubbleYL, sym::g_textPropDefBubbleYXL,
                                             sym::g_textPropDefBubbleYXXL, sym::g_textPropDefBubbleRL, sym::g_textPropDefBubbleRXL, sym::g_textPropDefBubbleRXXL};
        constexpr uint32_t kIconNames[] = {sym::g_textPropDefIconBreak, sym::g_textPropDefIconEditor, sym::g_textPropDefIconGap, sym::g_textPropDefIconGrind, sym::g_textPropDefIconPlant, sym::g_textPropDefIconRide,
                                           sym::g_textPropDefIconScore, sym::g_textPropDefIconTap, sym::g_textPropDefIconTrick, sym::g_textPropDefIconGrab, sym::g_textPropDefIconTime};
        constexpr uint32_t kPointerName = sym::g_textPropDefPointerA;  // "PropDef_PointerA"
        inline ee::Global<uint32_t, sym::g_npcPointerObjectHash> g_npcPointerObjectHash;
    }

    // findNpcType: the row of a type hash, 0 if there is none.
    ee::Ptr<NpcType> findNpcType(uint32_t key)
    {
        for (NpcType &row : *g_npcTypes)
            if (row.typeHash == key)
                return &row;
        return ee::Ptr<NpcType>();
    }
    READABLE_FUNCTION(findNpcType, Verified);

    // setupNpcTypeAnimations: each type's five animations found by name (a name not
    // found goes into a message nothing shows), then the hashes of the props
    // the NPCs show over their heads.
    int32_t setupNpcTypeAnimations()
    {
        ee::Local<char[0x1A0]> message;
        for (NpcType &type : *g_npcTypes)
            for (NpcAnimationSlot &slot : type.animations)
            {
                const ee::Ptr<const char> name = slot.name;
                if (!name || zge::compareStrings(kEmptyString, name) == 0)
                    continue;
                if (zge::findAnimation(name, &slot.animation) == 0)
                    zge::formatString(message, kCouldNotFind, name);
            }

        const auto hashes = [](uint32_t table, const uint32_t *names, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
                *(ee::Ptr<int32_t>(table) + i) = zge::nameHash(ee::Ptr<const char>(names[i]));
        };
        hashes(kBubbleHashes, kBubbleNames, 9);
        hashes(kIconHashes, kIconNames, 11);
        const int32_t pointer = zge::nameHash(ee::Ptr<const char>(kPointerName));
        *g_npcPointerObjectHash = pointer;
        return pointer;
    }
    READABLE_FUNCTION_WAITS(setupNpcTypeAnimations, Verified);
}
