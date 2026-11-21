#pragma once

// Native save files instead of the memory card, for the games on ZGE4's save
// system (zsaveapi.c: Aggressive Inline, BMX XXX): the switch and each game's
// part. The save jobs themselves are engine code, one body for both games at
// each one's functions: zge/host/save_jobs.cpp (the host form, over the files
// of game/host/save_files.h), installed over the memory card's jobs
// (zge/platform/memory_card/save_jobs.cpp, the PS2 form) with each game's host form of
// its save screen (game/<game>/host/save_screen.cpp), whatever ZX_READABLE
// says. The game's own save code (its client's prepare and finish: what a
// save holds) is the same in both modes, and so are the bytes on disk: a
// native file is the card's data file.
//
// The files: game/host/save_files.h.

#include "../game/ee/game_id.h"
#include "../game/host/save_files.h"

class PS2Runtime;

namespace zge_host::saves
{
    // The files, the profile, what was saved (game/host/save_files.h).
    using namespace ::game::host::saves;

    // ZX_SAVES=card: false, nothing installed (the card dialogs, as on the
    // console). True when the host forms are in place.
    bool install(PS2Runtime &runtime, ::game::Game id, const ::game::host::saves::Game &game);
}
