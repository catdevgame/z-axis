#include "readable.h"

#include "fncheck.h"
#include "diffcall.h"
#include "guest_calls.h"

#include "game_overrides.h"
#include "runtime/spot_fork.h"

#include <set>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <sstream>

namespace game::readable
{
    namespace
    {
        std::vector<Function *> &registry()
        {
            static std::vector<Function *> functions;
            return functions;
        }

        // Keyed by (game, address): the games' code shares addresses.
        uint64_t key(Game game, uint32_t address) { return (uint64_t(game) << 32) | address; }

        std::map<uint64_t, PS2Machine::RecompiledFunction> &tracedRegistry()
        {
            static std::map<uint64_t, PS2Machine::RecompiledFunction> traced;
            return traced;
        }

        bool sorted = false;
        Game g_runningGame = Game::Ai;

        // A body with the GAME_CALL marker goes by the name it is called by.
        const char *publicName(const char *written)
        {
            const size_t length = std::strlen(written);
            if (length == 0u || written[length - 1u] != '_')
                return written;
            static std::deque<std::string> names; // (filled at static initialisation; never moves its strings)
            return names.emplace_back(written, length - 1u).c_str();
        }

        bool parseAddress(const std::string &text, uint32_t &address)
        {
            if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
                return false;
            char *end = nullptr;
            const unsigned long value = std::strtoul(text.c_str(), &end, 16);
            if (!end || *end != '\0')
                return false;
            address = static_cast<uint32_t>(value);
            return true;
        }
    }

    const char *statusName(Status status)
    {
        switch (status)
        {
        case Status::Planned:
            return "planned";
        case Status::Draft:
            return "draft";
        case Status::Tested:
            return "tested";
        case Status::Verified:
            return "verified";
        }
        return "?";
    }

    Registration::Registration(uint32_t address, const char *name, const char *file, Status status,
                               PS2Machine::RecompiledFunction readable, ee::ReturnKind returnKind, const void *host,
                               bool waits, bool leavesVu0, Game game)
    {
        function.game = game;
        function.address = address;
        function.name = publicName(name);
        function.file = file;
        function.status = readable ? status : Status::Planned;
        function.readable = readable;
        function.returnKind = returnKind;
        function.host = host;
        function.waits = waits;
        function.leavesVu0 = leavesVu0;
        function.enabled = readable && status == Status::Verified;
        registry().push_back(&function);
        sorted = false;
    }

    TracedRegistration::TracedRegistration(uint32_t address, PS2Machine::RecompiledFunction traced, Game game)
    {
        tracedRegistry()[key(game, address)] = traced;
    }

    PS2Machine::RecompiledFunction tracedOriginal(uint32_t address, Game game)
    {
        const auto it = tracedRegistry().find(key(game, address));
        return it == tracedRegistry().end() ? nullptr : it->second;
    }

    namespace
    {
        std::vector<std::pair<Game, uint32_t>> &libraryLeaves()
        {
            static std::vector<std::pair<Game, uint32_t>> leaves;
            return leaves;
        }
    }

    LibraryLeaf::LibraryLeaf(uint32_t address, Game game)
    {
        if (symbolBound(address))
            libraryLeaves().emplace_back(game, address);
    }

    const std::vector<Function *> &functions()
    {
        if (!sorted)
        {
            std::stable_sort(registry().begin(), registry().end(), [](const Function *a, const Function *b) {
                return key(a->game, a->address) < key(b->game, b->address);
            });
            sorted = true;
        }
        return registry();
    }

    Game runningGame() { return g_runningGame; }

    Function *find(uint32_t address, Game game)
    {
        for (Function *f : functions())
            if (f->address == address && f->game == game)
                return f;
        return nullptr;
    }

    bool runsInGame(uint32_t address, Game game)
    {
        const Function *f = find(address, game);
        return f && f->enabled;
    }

    bool runsInGame(uint32_t address) { return runsInGame(address, Game::Ai); }

    Function *find(const std::string &nameOrAddress, Game game)
    {
        // `dm2:ADDRESS`, `dm2:riderFrame`: another game's.
        if (const size_t colon = nameOrAddress.find(':'); colon != std::string::npos)
        {
            Game prefixed = game;
            if (!parseGame(nameOrAddress.substr(0, colon), prefixed))
                return nullptr;
            return find(nameOrAddress.substr(colon + 1u), prefixed);
        }
        uint32_t address = 0;
        if (parseAddress(nameOrAddress, address))
            return find(address, game);
        for (Function *f : functions())
            if (f->game == game && nameOrAddress == f->name)
                return f;
        return nullptr;
    }

    namespace
    {
        std::set<const void *> &directHosts()
        {
            static std::set<const void *> hosts; // (filled at static initialisation)
            return hosts;
        }
    }

    bool noteCalledDirectly(const void *host)
    {
        directHosts().insert(host);
        return true;
    }

    bool calledDirectly(const void *host) { return directHosts().count(host) != 0; }

    Function *findHost(const void *host)
    {
        // The running game's registration first (one C++ function may one
        // day be registered in two games); the first of any otherwise.
        Function *any = nullptr;
        for (Function *f : functions())
            if (f->host == host)
            {
                if (f->game == g_runningGame)
                    return f;
                if (!any)
                    any = f;
            }
        return any;
    }

    bool applySwitch(const std::string &spec, std::string *error, Game game)
    {
        std::stringstream items(spec);
        std::string item;
        bool ok = true;
        auto setAll = [game](auto &&pick)
        {
            for (Function *f : functions())
                if (f->game == game)
                    f->enabled = f->readable && pick(*f);
        };
        while (std::getline(items, item, ','))
        {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (item.empty())
                continue;
            if (item == "on" || item == "verified" || item == "default")
                setAll([](const Function &f) { return f.status == Status::Verified; });
            else if (item == "off")
                setAll([](const Function &) { return false; });
            else if (item == "tested")
                setAll([](const Function &f) { return f.status >= Status::Tested; });
            else if (item == "all")
                setAll([](const Function &) { return true; });
            else
            {
                const bool off = item[0] == '-';
                const bool on = item[0] == '+';
                Function *f = find((off || on) ? item.substr(1) : item, game);
                if (!f || !f->readable || f->game != game)
                {
                    if (error)
                        *error += (error->empty() ? "" : ", ") + item;
                    ok = false;
                    continue;
                }
                f->enabled = !off;
            }
        }
        return ok;
    }

    bool switchedOnAtBoot(uint32_t address, Game game)
    {
        const Function *f = find(address, game);
        if (!f || !f->readable || f->game != game)
            return false;
        bool on = f->status == Status::Verified;
        const char *spec = std::getenv("ZX_READABLE");
        std::stringstream items(spec ? spec : "");
        std::string item;
        while (std::getline(items, item, ','))
        {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (item == "on" || item == "verified" || item == "default")
                on = f->status == Status::Verified;
            else if (item == "off")
                on = false;
            else if (item == "tested")
                on = f->status >= Status::Tested;
            else if (item == "all")
                on = true;
            else if (!item.empty())
            {
                const bool off = item[0] == '-';
                const bool plus = item[0] == '+';
                if (find((off || plus) ? item.substr(1) : item, game) == f)
                    on = !off;
            }
        }
        return on;
    }

    void install(PS2Machine &runtime) { install(runtime, Game::Ai); }

    void install(PS2Machine &runtime, Game game)
    {
        ee::g_ram = runtime.memory().getRDRAM();
        g_runningGame = game;
        ee::readBootSwitches(); // (this boot's lean parts and held table: one plain read per call after)
        if (game == Game::Ai)
            ee::installStackFill(runtime); // (the original's frames: AI's table)
        bool anyWaits = false;
        for (Function *f : functions())
        {
            if (f->game == game)
            {
                // As registered: an earlier install (this game's, in an earlier
                // run, or another game's, which turns this one's off) changed it.
                f->enabled = f->readable && f->status == Status::Verified;
                f->installedSlot = nullptr;
                f->hooked = false;
            }
            else
            {
                // Another game's: its address means other code here.
                f->original = nullptr;
                f->traced = nullptr;
                f->enabled = false;
                continue;
            }
            anyWaits = anyWaits || (f->readable && f->waits);
            f->original = runtime.hasFunction(f->address) ? runtime.lookupFunction(f->address) : nullptr;
            f->traced = tracedOriginal(f->address, game);
            if (!f->original)
            {
                std::cerr << "[readable] 0x" << std::hex << f->address << std::dec << " " << f->name
                          << ": no recompiled function there" << std::endl;
                f->enabled = false;
            }
        }
        if (const char *spec = std::getenv("ZX_READABLE"); spec && *spec)
        {
            std::string unknown;
            if (!applySwitch(spec, &unknown, game))
                std::cerr << "[readable] ZX_READABLE: unknown or not ported: " << unknown << std::endl;
        }

        // Registrations that share a host function (identical code folded by
        // the linker): game::call<&fn> reaches the first; callers of the
        // others use game::callAt.
        for (Function *f : functions())
            if (f->host && f->game == game)
                if (const Function *first = findHost(f->host); first && first != f)
                    std::cerr << "[readable] 0x" << std::hex << f->address << " " << f->name << " shares its host code with 0x"
                              << first->address << std::dec << " " << first->name << " (game::callAt tells them apart)"
                              << std::endl;

        // The resume slots (two table entries no code maps to) for a game with
        // functions whose calls wait, and for every game while safe points are
        // on: without them no readable function runs on a fiber, so none
        // honours the original's safe points (DM2 had none of its own that
        // wait, and its readable functions ran every call straight through,
        // checkpoints held off: a callee of physicsFrame leaves the
        // original through the scheduler every frame, the readable one never).
        const bool suspendable = (anyWaits || ee::safePointsOn()) && ee::installSuspendableCalls(runtime);
        int on = 0, ported = 0, waiting = 0, planned = 0;
        std::string offList;
        for (Function *f : functions())
        {
            if (f->game != game)
                continue;
            if (!f->readable)
            {
                ++planned;
                continue;
            }
            ++ported;
            if (f->waits && f->enabled && !suspendable)
            {
                // No resume slot: its calls could not wait.
                ++waiting;
                f->enabled = false;
            }
            if (f->enabled && f->original && runtime.replaceFunction(f->address, f->readable))
            {
                ++on;
                ee::noteReadableEntry(f->readable);
            }
            else
            {
                f->enabled = false;
                offList += std::string(offList.empty() ? "" : ", ") + f->name;
            }
        }
        // The runtime's library leaves as the table has them now, before any tool or override wraps one (a
        // wrapped one takes the host stack again: its entry is another).
        ee::clearLibraryLeaves();
        for (const auto &[leafGame, address] : libraryLeaves())
            if (leafGame == game && runtime.hasFunction(address))
                ee::noteLibraryLeaf(runtime.lookupFunction(address));
        std::cout << "[readable] ";
        if (game != Game::Ai)
            std::cout << gameName(game) << ": ";
        std::cout << on << " of " << ported << " readable functions on";
        if (waiting)
            std::cout << ", " << waiting << " left off (no resume slot for suspendable calls)";
        if (!offList.empty() && ported - on <= 12)
            std::cout << " (off: " << offList << ")";
        if (game != Game::Ai)
            std::cout << ", " << planned << " planned (recording only)";
        std::cout << std::endl;

        // The comparison machinery: the dev flavour's alone (ZX_DEV_CHECKS, ps2/CMakeLists.txt). The app's
        // has none of these sources; ZX_FNPROF stays (profile.cpp: a profiler of the readable code itself).
        profile::install(runtime);
#if ZX_DEV_CHECKS
        // A recording run with readable code on would miss calls: the run ends here, saying why (fncheck.h).
        if (!fncheck::installRecorder(runtime, game))
        {
            std::fflush(nullptr);
            std::_Exit(2);
        }
        // A child forked at a spot (ZX_SPOT_FORK) records from there, by its own environment.
        zx::spot::addChildHook([game](PS2Machine &rt)
                                 {
                                     if (!fncheck::installRecorder(rt, game))
                                     {
                                         std::fflush(nullptr);
                                         std::_Exit(2);
                                     }
                                 });
        profile::installTailCallStats(runtime);
        profile::installCycleTrace(runtime);
        guest_calls::install(runtime);
        diffcall::install(runtime, game);
#endif

        // The slots as this function leaves them, the tools' wrappers
        // included (those replay a direct call themselves or do not mind);
        // what a game override does to them from here on is a hook.
        for (Function *f : functions())
            if (f->game == game)
            {
                f->installedSlot = f->enabled ? runtime.lookupFunction(f->address) : nullptr;
                f->hooked = false;
            }
        ps2_game_overrides::addAfterApplyHook(&noteOverrides);
    }

    void noteOverrides(PS2Machine &runtime)
    {
        int hooked = 0;
        std::string names;
        for (Function *f : functions())
        {
            if (f->game != g_runningGame || !f->enabled || !f->installedSlot)
                continue;
            f->hooked = runtime.lookupFunction(f->address) != f->installedSlot;
            if (f->hooked)
                names += std::string(hooked++ ? ", " : "") + f->name;
        }
        if (hooked)
            std::cout << "[readable] " << hooked << " on functions hooked by overrides, called through the table: " << names
                      << std::endl;
    }
}

namespace ee
{
    // (abi.h) The frame-table row of a registered readable function.
    const FrameRow *hostLayout(const void *host)
    {
        const game::readable::Function *f = game::readable::findHost(host);
        return f ? frameLayout(static_cast<uint32_t>(f->game), f->address) : nullptr;
    }
}

namespace game::detail
{
    // A game::tail whose callee made no call: the mark goes (it would make the thread's next call a tail call).
    TailMark::~TailMark()
    {
        if (!ee::takeTailNext())
            return;
        static uint64_t count = 0;
        ++count;
        if ((count & (count - 1)) == 0)
        {
            const ee::FrameRow *row = ee::t_ee.frame ? ee::t_ee.frame->layout : nullptr;
            std::fprintf(stderr, "[readable] game::tail in 0x%08x: its callee made no call (%llu times)\n",
                         row ? row->address : 0u, static_cast<unsigned long long>(count));
        }
    }
}
