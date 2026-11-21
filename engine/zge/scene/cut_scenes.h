#pragma once

// ZGE4's cut scenes (BMX XXX only; zge/scene/cut_scenes.cpp): a level's cut scene scripts, linked to its objects
// once it is loaded, played by name and ended.

#include "ee/ee.h"

#include <cstdint>

namespace zge
{
    struct CutScene;
    struct SceneObject;

    // The store: its chunk handlers registered (and the camera's name hash); its records and scripts freed; all
    // of it emptied at the engine's end.
    uint32_t initCutScenes();
    void freeCutScenes();
    uint32_t shutDownCutScenes();
    // The scripts linked to the level loaded, then each cut scene's set-up script run.
    void linkCutSceneScripts();
    void runCutSceneSetupScripts();
    // The library the scripts' motions come from, and the bytes of the block they are copied into.
    void setCutSceneLibrary(ee::Ptr<const char> path, uint32_t motionBytes);
    // The object a script's camera commands move.
    void setCutSceneCamera(ee::Ptr<SceneObject> camera);
    // Every cut scene read, then 0.
    ee::Ptr<ee::Ptr<CutScene>> cutSceneList();
    // The cut scenes of a name's hash played (atOnce: run to their ends there and then); the one playing ended;
    // whether it is played at once (or ending).
    void startCutScene(uint32_t nameHash, uint32_t atOnce);
    void endCutScene();
    uint32_t cutSceneAtOnce();
}
