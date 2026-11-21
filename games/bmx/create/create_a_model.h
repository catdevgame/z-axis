#pragma once
// BMX XXX's create-a-model (the rider the player puts together from parts:
// assets\create): the module's start and end, which the game's own start and
// shutdown call, and the libraries' names. BMX XXX's own code
// (create_a_model.cpp); the rest of the module is not written yet.

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // The module's three pools and two lists made, the lists filled from
    // "assets\create\category.txt" (a name and a number a line) and
    // "assets\create\default.txt" (four names a line), and its five script
    // functions registered (createAModelGetFilteredListOfAssetsInLibrary,
    // createAModelReplaceTexture, createAModelMatchSkinTxrToFaceTxr,
    // createAModelSetProperty, createAModelGetProperty).
    void initCreateAModel();
    // The pools destroyed and both lists emptied.
    void shutDownCreateAModel();
    // Which of the module's libraries has the name ("parts", "lattices",
    // "skeleton", "texture00" .. "texture15"): 0 .. 18, or 19 for none.
    uint32_t createAModelLibraryIndex(ee::Ptr<const char> name);
}
