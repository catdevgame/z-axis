#pragma once
// BMX XXX's intro state (bmx.c, intro.cpp): the game's first state, which
// loads what every later state uses and shows the intro's UI. BMX XXX's own
// code.

#include "ee/ee.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    // The state's enter: the display at 640 by 448, an empty scene with the
    // UI's camera in it, and the loading at its first step. Always 1.
    int32_t introEnter();
    // The state's loading step: one of seven by the step's number (the
    // intro's UI and its menu, the saves' clients, the sound, the riders'
    // animations); the intro's frame sets the number as its menus go by.
    // 1 while the step leaves more, 0 when it ends the loading.
    int32_t introLoad();
    // The state's frame: the pads read, the menu machine stepped, then the
    // screen that is up timed: its loading step run at its first frame, and
    // the screen ended once it has been up long enough (sooner when a button
    // is pressed, past the screen's own wait). The movie's screen ends when
    // the movie does. Always 1.
    int32_t introFrame();
    // The intro's menu handler: the screens are the menu machine's menus 1
    // to 5 (the piracy, legal, publisher and developer splashes, the movie),
    // each followed by the next; menu 0 is the state's start (which goes
    // straight to a level or the AV menu when the game was started for one)
    // and menu 6 its end (the front end entered).
    void introMenu(int32_t event);
    // The state's exit: the intro's UI destroyed, the UI's camera released,
    // the scene left.
    void introExit();

    // A camera node's camera (the node's scene word): whether its frame is
    // cleared, its clear colour (R, G, B, A at `colour`), and its far plane.
    // bmx.c's own copies of the three the level camera's code has
    // (camera/scene_camera).
    void setCameraNodeClears(ee::Ptr<zge::SceneObject> camera, uint32_t on);
    void setCameraNodeClearColour(ee::Ptr<zge::SceneObject> camera, uint32_t colour);
    void setCameraNodeFarClip(ee::Ptr<zge::SceneObject> camera, ee::Float distance);
}
