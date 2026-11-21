#pragma once

// The functions the recorder keeps to a stop (fncheck.h, ZX_FNREC_STOPS=1):
// they never return, so a call of one is kept from its entry to the callee
// that never comes back to it (fncheck::kStopsAtCall). The table is part of
// the tree, so a stop recording is made again from it alone; ZX_FNREC_STOP
// adds names for experiments.
//
// The stops:
//   ThreadEnd  the callee ended the thread (ExitDeleteThread, ExitThread):
//              the record stops at that call.
//   Frames     the same, and also at the first wait (a switch to another
//              thread) once ZX_FNREC_STOP_FRAMES vsyncs (default 1) have
//              passed since the entry: a thread's endless loop.
// Every other function is recorded as before, stops on or off.

#include "game_id.h"

#include <cstdint>

namespace fncheck
{
    enum class StopRule : uint8_t
    {
        None,
        ThreadEnd,
        Frames,
    };

    struct StopEntry
    {
        game::Game game;
        const char *name;
        StopRule rule;
        const char *why; // why it never returns
    };

    inline constexpr StopEntry kStops[] = {
        {game::Game::Ai, "endSaveJob", StopRule::ThreadEnd,
         "a memory card job's end: it deletes the job's own thread (ExitDeleteThread)"},
        {game::Game::Bmx, "endSaveJob", StopRule::ThreadEnd,
         "a memory card job's end: it deletes the job's own thread (ExitDeleteThread)"},
        {game::Game::Ai, "movieDefaultMain", StopRule::Frames,
         "the movie player's default thread: it demuxes the stream, then switches threads for good"},
        {game::Game::Dm2, "movieDefaultMain", StopRule::Frames,
         "the movie player's default thread: it demuxes the stream, then switches threads for good"},
        {game::Game::Bmx, "movieDefaultMain", StopRule::Frames,
         "the movie player's default thread: it demuxes the stream, then switches threads for good"},
    };
}
