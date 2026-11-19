#pragma once

// The front end's high scores ("MainMenu.HiscoresFrame", hiscores_menu.cpp):
// the script function that fills the frame's lists.

#include <cstdint>

namespace game::dm2
{
    // "BmxGetHiscoresData": the frame's level entry ("CurLevel") becomes the
    // session level, and the frame's four columns list that level's twelve
    // saved records: the record's name, the character and player who set it
    // and its value.
    void bmxGetHiscoresData();
}
