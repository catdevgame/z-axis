// BMX XXX's trees' draw check (tree_fade.h). BMX XXX's own code (fx.c's
// block): DM2's (games/dm2/fx/tree_fade.cpp) in every level, with the fade
// itself cut to a check that always draws.

#include "tree_fade.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "zge/collision/collision.h"
#include "zge/render/terrain.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/bmx/fx/tree_fade.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The fade's record: 9 and 13 units, "terrain_tree_alpher".
        constexpr uint32_t kTreeFade = sym::treeFade;
    }

    uint32_t treeDrawCheck()
    {
        return 1;
    }
    READABLE_FUNCTION(treeDrawCheck, Draft);

    uint32_t setTreeFadeCheck(ee::Ptr<zge::Node> node, ee::Ptr<TreeFade> fade)
    {
        const ee::Ptr<char> name = zge::nodeName(node);
        if (zge::compareStringsNoCaseN(name, ee::Address(fade->nodeName), fade->nodeNameLength) == 0)
            zge::setNodeDrawCheck(node, sym::treeDrawCheck, fade);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setTreeFadeCheck, Verified);

    void setupTreeFade()
    {
        game::tail(zge::forEachWorldMesh, sym::setTreeFadeCheck, ee::Address(kTreeFade));
    }
    READABLE_FUNCTION_WAITS(setupTreeFade, Verified);
}
