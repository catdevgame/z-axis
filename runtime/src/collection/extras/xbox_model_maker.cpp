// The engine's Xbox model converter (zge/native/xbox_models.h) for the
// extras' setup (collection/extras/ai_xbox_skaters.h engineXboxModelMaker).
// Built into the app and zx-extras, which link the game's code; not into
// the collection's library (game.cmake).
#include "collection/extras/ai_xbox_skaters.h"

#include "../../game/zge/native/xbox_models.h"

#include <sstream>

namespace zx::app::extras
{
    XboxModelMaker engineXboxModelMaker()
    {
        return [](const std::string &relative, const Bytes &ps2, const Bytes &xbox, Bytes &out)
        {
            zge::native::XboxModelReport report;
            XboxModelMade made;
            made.made = zge::native::makeXboxModel(relative, ps2, xbox, out, report);
            made.why = report.why;
            made.sameGeometry = made.made && report.xboxVertices == report.ps2Vertices;
            std::ostringstream summary;
            summary << report.ps2Vertices << " -> " << report.xboxVertices << " vertices, " << report.remapped << " of "
                    << report.materials << " material textures renamed, largest packet " << report.largestPacket
                    << " bytes, " << report.triangles << " triangles, the same";
            made.summary = summary.str();
            return made;
        };
    }
}
