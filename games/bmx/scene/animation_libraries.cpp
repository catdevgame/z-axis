// BMX XXX's animation archives (ZGE4's zanim.c; ZGE3's loadAnimationArchive is zge/scene/animator.cpp): each archive
// into an animation library of its own (zge::AnimationLibraries: 16, numbered from 1), its entries' pool made for as
// many entries as the archive has, in place of ZGE3's one animation table.

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "zge/assets/asset_library.h"
#include "zge/container/tree.h"
#include "zge/game.h"
#include "zge/memory/pool.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/bmx/scene/animation_libraries.cpp.h"

namespace game::bmx
{
    namespace
    {
        inline ee::Global<zge::AnimationLibraries, sym::g_animationLibraries> g_animationLibraries;
    }

    // loadArchiveIntoLibrary: BMX XXX's loadAnimationArchive: an archive's (.ZAL) motions into the first free
    // animation library (unchecked: with all 16 in use, the one after them), its entries' pool made for the
    // archive's entries; then the motions' track pools destroyed. Returns the library's number, 0 when the archive
    // did not open. (ZGE4's asset library is the game's zfile library: openZfileLibrary is openAssetLibrary there,
    // and its group count the archive's entries.)
    int32_t loadArchiveIntoLibrary(ee::Address path)
    {
        const uint32_t library = zge::openAssetLibrary(path);
        if (!library)
            return 0;
        ee::Ptr<zge::AnimationLibrary> into = &g_animationLibraries->library[0];
        uint32_t number = 1;
        while (number <= zge::kAnimationLibraries && into->entryPool != 0)
        {
            ++number;
            into += 1;
        }
        into->entryPool = zge::createPool(sizeof(zge::AnimationEntry), zge::zfileLibraryGroupCount(ee::Ptr<zge::ZfileLibrary>(library)), 0);
        zge::initSearchTree(&into->table, sym::lessWord);
        ee::Local<zge::ArchiveLoad> load;
        zge::memoryFill(load, 0, sizeof(zge::ArchiveLoad));
        load->table = &into->table;
        load->archive = number;
        zge::forEachAsset(library, sym::loadFormFile, load);
        zge::freeMotionTrackPools();
        zge::closeAssetLibrary(library);
        return static_cast<int32_t>(number);
    }
    READABLE_FUNCTION_WAITS(loadArchiveIntoLibrary, Verified);
}
