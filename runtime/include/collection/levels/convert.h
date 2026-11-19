#pragma once

// A model (collection/levels/model.h) made into one game's level files, as
// the tooling's converters make them (ai_level_convert.py,
// dm2_level_convert.py, bmx_level_convert.py convert: the reference), in the
// slot the game lends Custom Freeplay (collection/maps.h mapSlots):
// - Aggressive Inline: ASSETS/LEVELS/TUT, the five level files, two empty
//   level scripts (TUT.BIN, TUTT.BIN) and an empty MOTIONS.ZAL.
// - Dave Mirra BMX 2: a FREERIDE level row's slot, Woodward Camp's
//   (ASSETS/LEVELS/WOODWARD), the five level files.
// - BMX XXX: ASSETS/LEVELS/CHL_MULY, the five level files, the level's four
//   asset libraries empty and an OBJCTV.TXT with no objectives.
// A #dynamics node's object is a dynamics prop with its definition (the
// mesh's extras, as a stock prop exports), the target game's class data and
// sounds unless the definition is that game's own; an object's name keeps
// its whole hash (the mesh's extras', else its whole name's) where the name
// field cuts it.
// Textures are made to fit the game's budgets (halved, resampled to powers
// of two, palettized); objects over 1,024 faces are cut; a level over a
// budget the converter cannot fix (the parts drawn, the texels, the level
// files' bytes: no more than the game's largest stock level's, so it fits
// the game's memory) is refused, with why.

#include "collection/levels/model.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::app::levels
{
    // A converted map's files: the path in a Mods folder's layout
    // ("ASSETS/LEVELS/TUT/TUT.GEO") and the bytes.
    using MapFiles = std::map<std::string, Bytes>;

    struct Converted
    {
        MapFiles files;
        std::vector<std::string> report; // what the converter changed to fit, and the level's numbers
    };

    // The model as the target game's files. Throws LevelError when it cannot
    // be (over a budget, no PlayerOne spawn).
    Converted convertModel(const Model &model, Engine target);
}
