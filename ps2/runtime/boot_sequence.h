#pragma once

#include "runtime/boot_config.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class PS2Machine;

// Several games booted one after another in one process, each torn down as
// leaving a game for the collection's menu does (no menu drawn): the check
// that a game booted after others runs as it does in a process of its own
// (tools/checks/seqcheck.py; zx-runner --sequence).
//
//   zx-runner --sequence ai:STEPFILE,dm2:STEPFILE[,...]
//
// A step file is one NAME=VALUE per line (# starts a comment line):
//
//   elf=PATH         the game's ELF (its host, CD and card folders are its
//                    folder's, as a run of that ELF alone)
//   ticks=N          the step's tick limit (VBlank ticks): it is stopped there
//   seconds=N        and after N seconds of host time (optional)
//   stall_seconds=N  and when fewer than 60 VBlanks came in N seconds of
//                    host time (a game that hangs; optional)
//   stop_after=PATH  stopped once this file exists (a check's last dump)
//   mc_root=PATH     the memory card folder (optional)
//   cwd=PATH         the working directory while it runs (optional)
//   ZX_...=VALUE   the boot's BootConfig value (a name it carries)
//
// Each step: the process's I/O folders and working directory as they were
// when the sequence began, a runtime made with the step's BootConfig on the
// host the sequence keeps up (HostOptions::hostOwned), the ELF loaded, run
// until a stop, the game's shutdown hooks, the runtime destroyed. The host
// comes down after the last step.
namespace zx::sequence
{
    struct Step
    {
        std::string game;
        std::filesystem::path elf;
        uint64_t tickLimit = 0;
        double secondsLimit = 0.0;
        double stallSeconds = 0.0;
        std::filesystem::path stopAfter;
        std::filesystem::path mcRoot;
        std::filesystem::path workingDirectory;
        BootConfig config;
    };

    // How a step ended.
    struct Outcome
    {
        bool booted = false;
        // "file" (stop_after written), "ticks", "seconds", "stalled" (under
        // 60 VBlanks in stall_seconds), "exit" (the game or the host ended
        // the run), "" when it did not boot.
        std::string stop;
        uint64_t ticks = 0;
        double seconds = 0.0;
        // The process's resident memory after the runtime was destroyed (MB;
        // 0 where it is not known).
        double residentMb = 0.0;
    };

    // One step from its game and its file's text; false and a reason on a
    // malformed one (no elf, no tick limit, an unknown name).
    bool parseStep(const std::string &game, const std::string &text, Step &step, std::string *error = nullptr);
    // "game:FILE,game:FILE,...": every file read and parsed.
    bool parseArgument(const std::string &argument, std::vector<Step> &steps, std::string *error = nullptr);

    // Runs the steps in order. afterLoad, when given, runs after each ELF is
    // loaded (the runner's ZX_CD_IMAGE and other folder overrides). A step
    // that does not boot ends the sequence. The host is shut down at the end
    // if it is up.
    std::vector<Outcome> run(const std::vector<Step> &steps, const char *title,
                             const std::function<void(PS2Machine &)> &afterLoad = {});
}
