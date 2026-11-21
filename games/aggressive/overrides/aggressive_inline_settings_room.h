#pragma once

class PS2Machine;

namespace aggressive_inline::settings_room
{
    // zgui's heap given room for the port's own pages (the Settings page,
    // Are You Sure, the pause menu's extra items) from host memory above the
    // 32 MB, so the game's own screens keep the room the original gave them
    // and the game's memory stays as it was. With the settings menus only
    // (installSettingsMenus). False if not installed.
    bool install(PS2Machine &runtime);
}
