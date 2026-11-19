#pragma once
// BMX XXX's UI as a whole (bmx.c, ui_system.cpp): its start, its end and the
// sounds its menus make. BMX XXX's own code.

namespace game::bmx
{
    // The UI's start (the game's start runs it): the movie player and zgui
    // started, the icon roll widget registered, both pads given to zgui, the
    // UI's properties defined (the accept and cancel buttons' pictures, the
    // display mode, the menu machine's message and player, the menus'
    // colours), the menus' sounds and buttons set, the UI's library opened,
    // the saved language's strings loaded and the common UI's script run.
    void setUpUi();
    // The UI's sound hooks: each sends its menu sound event.
    void playSelectSound();
    void playCancelSound();
    void playMoveSound();
    void playChangeSound();
    void playBlockedSound();
    // The UI's end (the game's end runs it): the game's string table freed,
    // the common UI destroyed, the UI's library closed, zgui's memory
    // tidied, and the movie player shut down.
    void shutDownUi();
}
