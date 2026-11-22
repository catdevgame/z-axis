// Aggressive Inline (SLUS-20327): restarts through the game's own state
// machines. See docs/SETTINGS.md ("Restart").
//
// Level. The top-level states (0x24C850: 0 level, 1 front end, 2 loader)
// switch through 0x1BCD38(state): the old state's exit, the new one's
// enter. The loader waits a byte's worth of frames (0x276DD5), then enters
// the state in 0x276DD8. The menus' Start sets 0, 1 and switches to 2; so
// "the level again" runs the level's own exit (the career commit, native
// autosave) and loads the same level, skaters and mode. Split screen too:
// the level comes back with its game menu (Most Points, Egg Hunt, ...).
//
// Timed Run: the game's Play Again (inlinePauseRestartTimedRun -> 0x1A7460:
// respawn, score, juice, a new timer).
//
// Split screen (game type 4): the game the players chose, again. The pause
// menu's Game Select ends the run (inlinePauseMultiplayerEndRun 0x1BB668:
// the run's end callback 0x1A71E0, which also shows the results, the game's
// own teardown, the timer) and the game menu starts one
// (inlinePauseMultiplayerStartEnd 0x1BB738: the menu's SelectedIndex into
// level+2, the game's setup, then Play Again for both players). Here: the
// teardown without the results (Twenty One 0x1AED80; Animal Rescue
// 0x196690 for each player and the parrots 0x1C5FB0(0)), the game menu's
// SelectedIndex set to the running game, then the game's StartEnd.
//
// Career challenges. A challenge giver is a level script object (CIVIC.LOG
// lists them, e.g. InlineAssigner: init --action--> waitForPoints, back to
// init on TimerFinished / wait_fail, assignSecondPoints when done). The
// script engine's state change is 0x1E0798(object hash, state hash): the
// old state's exit event, its handlers out, the new state's in, enter.
// Current states: the table at 0x2798C0 ({object hash, word offset} pairs,
// -1 ended; at the offset the object's slot in the word array 0x2798BC).
// The player gets a challenge by pressing Action by the giver: 0x196BF8
// sends the object the ACTION event (0x152910, synchronous), whose handler
// sets the object's state. So a state change of the ACTION's object while
// the ACTION is handled is "challenge given": from (the giver's state) and
// to (the attempt's), and where the skater was. Giving it again: the
// skater respawned there (the game's named respawn 0x193EB0 with a locator
// name only this code answers, in the locator lookup 0x187800), then the
// object put back in from and into to through 0x1E0798, as a failure and
// the player's Action would (the attempt's exit and enter: its timer,
// challenge score, the other givers told to wait). The record goes when
// the challenge is completed (its objective, whose record at 0x2795A0 + 0x2C n
// names the giver at +0x14, set to complete: 0x1BE1A8 with the value at
// 0x2795DC), when the object reaches a "complete"/"done"
// state, when another challenge is given, and out of the level.
//
// Tutorial lessons. Its pause menu jumps to lesson n with
// inlinePauseJumpToTutorial (0x1BA398): the message "GoToLesson" + letter
// ('A' + n - 1) to ScriptObj_PlayerOne, whose handler ends the lesson and
// starts the new one (TUT.LOG). The current lesson is the state of
// scriptObj_controller (LessonA..LessonW; init or complete between them).

#include "aggressive_inline_restart.h"
#include "sym/game_overrides/aggressive_inline_restart.cpp.h"

#include "aggressive_inline_estate.h"
#include "machine.h"
#include "runtime/session.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace aggressive_inline::restart
{
    namespace
    {
        constexpr uint32_t kGameMode = sym::g_gameMode;
        constexpr uint32_t kLevelFrame = sym::g_levelFrame;
        constexpr uint32_t kModePlay = 2u;
        constexpr uint32_t kModePaused = 3u;
        constexpr uint32_t kGameType = sym::g_gameTypeByte; // byte: 1 Career, 2 Freeskate, 3 Timed Run, 4 split screen
        constexpr uint8_t kTypeCareer = 1u;
        constexpr uint8_t kTypeTimedRun = 3u;
        constexpr uint8_t kTypeSplitScreen = 4u;
        constexpr uint32_t kLevelIndex = sym::g_levelNumber; // byte
        constexpr uint8_t kLevelTutorial = 7u;
        constexpr uint32_t kTopState = sym::g_topState; // the current top-level state record; +0x10 the level
        constexpr uint32_t kTopStateLevel = sym::topStates; // state 0 (records of 0x14 bytes)
        constexpr uint32_t kPlayerOne = 0x10u;      // players in the level object, 0x930 bytes each
        constexpr uint32_t kPlayerTwo = 0x940u;
        constexpr uint32_t kLevelRunTimer = 0x08u;  // the run's timer (Timed Run, split screen games)
        constexpr uint32_t kLevelSplitGame = 0x02u; // byte: 0 Most Points ... 4 Animal Rescue

        // Top-level states.
        constexpr uint32_t kSwitchState = sym::switchGameState;
        constexpr uint32_t kLoaderDelay = sym::g_pendingStateDelay;  // byte (gp - 0x3B1B)
        constexpr uint32_t kLoaderTarget = sym::g_nextGameState; // word (gp - 0x3B18)
        constexpr uint32_t kStateLevel = 0u;
        constexpr uint32_t kStateLoader = 2u;
        constexpr uint32_t kPlayAgain = sym::playAgain;

        // Split screen.
        constexpr uint32_t kTwentyOneEnd = sym::endCardGame;
        constexpr uint32_t kRescueEnd = sym::dropAnimals; // (player, 1)
        constexpr uint32_t kParrots = sym::setAnimalRescue;   // (on)
        constexpr uint32_t kSplitStart = sym::inlinePauseMultiplayerStartEnd; // inlinePauseMultiplayerStartEnd
        constexpr uint32_t kUiRoot = sym::g_uiRoot;
        constexpr uint32_t kFindWidget = sym::findNode;  // (root, path, 1)
        constexpr uint32_t kSetProperty = sym::setVariable; // (widget, name, value*)
        constexpr uint32_t kGetProperty = sym::copyVariable; // (widget, name, value*)
        constexpr uint32_t kSplitMenuPath = sym::g_textMultiplayerUIPauseMenusFrame; // "MultiplayerUI.PauseMenus.Frame.MultiplayerStart.Menu"
        constexpr uint32_t kSelectedIndexName = sym::g_textSelectedIndex; // "SelectedIndex"
        constexpr uint8_t kSplitTwentyOne = 3u;
        constexpr uint8_t kSplitRescue = 4u;

        // Level scripts.
        constexpr uint32_t kScriptTable = sym::g_vmCode;
        constexpr uint32_t kScriptStates = sym::g_vmVariables;
        constexpr uint32_t kSetState = sym::scriptSetState;    // (object hash, state hash)
        constexpr uint32_t kPostMessage = sym::sendEngineMessage; // (message*): +8 object, +0x10 event hash
        constexpr uint32_t kSendMessage = sym::sendNewMessage; // (0, object, event hash, data, synchronous)
        constexpr uint32_t kFindObjects = sym::findObjects; // (hash*, n) -> the objects, in kObjectList
        constexpr uint32_t kObjectList = sym::foundObjects;
        constexpr uint32_t kSetObjective = sym::completeChallenge; // (objective, state)
        // The Complete state's hash (level/objectives.cpp g_stateHashes[kStateComplete], 3).
        constexpr uint32_t kObjectiveComplete = sym::g_stateHashes + 3u * 4u;
        constexpr uint32_t kObjectives = sym::g_objectives;     // records of 0x2C bytes
        constexpr uint32_t kObjectiveCount = sym::g_objectiveCount;
        constexpr uint32_t kObjectiveSize = 0x2Cu;
        constexpr uint32_t kObjectiveGiver = 0x14u; // the script object's name hash
        constexpr uint32_t kObjectiveDone = 0x28u;

        // The skater (a player record).
        constexpr uint32_t kRespawnNamed = sym::placeSkaterAtLevelStart; // (player, locator hash, 1)
        constexpr uint32_t kFindLocator = sym::findLocator;  // (position*, hash, matrix*) -> found
        constexpr uint32_t kPlayerFlags = 0x80u;        // bit 2: respawn at a locator
        constexpr uint32_t kPlayerPosition = 0x90u;
        constexpr uint32_t kPlayerMatrix = 0xB0u;

        struct Hooks
        {
            PS2Machine::RecompiledFunction switchState = nullptr;
            PS2Machine::RecompiledFunction playAgain = nullptr;
            PS2Machine::RecompiledFunction setState = nullptr;
            PS2Machine::RecompiledFunction postMessage = nullptr;
            PS2Machine::RecompiledFunction setObjective = nullptr;
            PS2Machine::RecompiledFunction findLocator = nullptr;
            bool trace = false; // ZX_TRACE_RESTART=1
        };
        Hooks &hooks() { return zx::session::state<Hooks>(); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        // The engine's name hash (0x148888): case-insensitive ELF hash.
        constexpr uint32_t nameHash(const char *name)
        {
            uint32_t h = 0;
            for (; *name; ++name)
            {
                uint32_t c = static_cast<uint8_t>(*name);
                if (c >= 'a' && c <= 'z')
                    c -= 'a' - 'A';
                h = (h << 4) + c;
                const uint32_t high = h & 0xF0000000u;
                if (high)
                    h ^= high >> 24;
                h &= ~high;
            }
            return h;
        }
        static_assert(nameHash("action") == 0x04588E3Eu, "the engine's hash");
        static_assert(nameHash("ScriptObj_PlayerOne") == 0x0230D2C5u, "the engine's hash");

        constexpr uint32_t kActionEvent = nameHash("action");
        constexpr uint32_t kTutorialController = nameHash("scriptObj_controller");
        constexpr uint32_t kTutorialPlayer = nameHash("ScriptObj_PlayerOne");
        // Locator name of the challenge respawn (no level has it).
        constexpr uint32_t kRetryLocator = nameHash("pcRetryChallengeStart");
        constexpr uint32_t kTerminalStates[] = {nameHash("complete"), nameHash("completed"), nameHash("done"),
                                                nameHash("totallyDone")};

        uint32_t levelObject(const uint8_t *rdram)
        {
            const uint32_t state = readWord(rdram, kTopState);
            return state ? readWord(rdram, state + 0x10u) : 0u;
        }

        // A guest call from a hook: arguments and memory below the caller's
        // stack pointer.
        uint32_t scratchBelow(const R5900Context *ctx) { return (getRegU32(ctx, 29) - 0x200u) & ~0xFu; }

        bool call(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t address,
                  std::initializer_list<uint32_t> args, uint32_t *v0 = nullptr)
        {
            const bool ok = runtime->callGuestFunction(rdram, ctx, address, args, scratchBelow(ctx) - 0x100u, v0);
            if (!ok)
                std::cerr << "[restart] guest call 0x" << std::hex << address << std::dec << " failed" << std::endl;
            return ok;
        }

        // ---- level scripts ----------------------------------------------------

        // The object's current state hash, 0 if the level has no such object.
        uint32_t currentState(const uint8_t *rdram, uint32_t object)
        {
            const uint32_t table = readWord(rdram, kScriptTable);
            const uint32_t states = readWord(rdram, kScriptStates);
            if (!table || !states)
                return 0u;
            for (uint32_t entry = table; entry < table + 0x10000u; entry += 8u)
            {
                const uint32_t hash = readWord(rdram, entry);
                if (hash == 0xFFFFFFFFu)
                    break;
                if (hash == object)
                {
                    const uint32_t info = table + readWord(rdram, entry + 4u) * 4u;
                    return readWord(rdram, states + readWord(rdram, info) * 4u);
                }
            }
            return 0u;
        }

        // The script object of a name (what messages are sent to), as the
        // engine finds it (0x1646B8); its result list is put back after.
        uint32_t objectNamed(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t hash)
        {
            const uint32_t slot = scratchBelow(ctx);
            writeWord(rdram, slot, hash);
            const uint32_t saved[2] = {readWord(rdram, kObjectList), readWord(rdram, kObjectList + 4u)};
            uint32_t list = 0;
            uint32_t object = 0;
            if (runtime->callGuestFunction(rdram, ctx, kFindObjects, {slot, 1u}, slot - 0x100u, &list) && list)
                object = readWord(rdram, list);
            writeWord(rdram, kObjectList, saved[0]);
            writeWord(rdram, kObjectList + 4u, saved[1]);
            return object;
        }

        bool terminalState(uint32_t state)
        {
            for (uint32_t terminal : kTerminalStates)
                if (state == terminal)
                    return true;
            return false;
        }

        // ---- challenges -------------------------------------------------------

        struct Change
        {
            uint32_t object, from, to;
        };

        struct Challenge
        {
            bool valid = false;
            uint32_t object = 0, from = 0, to = 0;
            uint32_t matrix[16] = {};   // the skater's orientation rows
            uint32_t position[4] = {}; // and position when the challenge was given
        };

        struct Retry
        {
            // EE thread only (the hooks and the level update).
            Challenge challenge;
            uint32_t actionTarget = 0; // the object an ACTION is being handled by
            std::vector<Change> actionChanges;
            uint32_t actionMatrix[16] = {};
            uint32_t actionPosition[4] = {};
            bool retrying = false; // our own state changes, not recorded
            bool respawnArmed = false; // the locator lookup answers kRetryLocator
            std::atomic<bool> retryable{false}; // challenge.valid, for other threads
        };
        Retry &retry() { return zx::session::state<Retry>(); }

        void dropChallenge(const char *why)
        {
            if (retry().challenge.valid && hooks().trace)
                std::cout << "[restart] challenge record dropped (" << why << ")" << std::endl;
            retry().challenge.valid = false;
            retry().retryable = false;
        }

        bool challengeLevel(const uint8_t *rdram)
        {
            return readByte(rdram, kGameType) == kTypeCareer && readByte(rdram, kLevelIndex) != kLevelTutorial;
        }

        // The original of a hook, run to its end before the hook goes on: a
        // direct call may stop at a safe point (dispatchGuestBranch unwinds
        // to the scheduler and resumes from there, past the hook), a host
        // call has none. Returns as the original would.
        void callOriginal(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, PS2Machine::RecompiledFunction fn,
                          uint32_t entry, std::initializer_list<uint32_t> args)
        {
            uint32_t v0 = 0;
            runtime->callGuestFunction(rdram, ctx, fn, entry, args, getRegU32(ctx, 29), &v0);
            setReturnU32(ctx, v0);
            ctx->pc = getRegU32(ctx, 31);
        }

        void setStateHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (retry().retrying || (!retry().actionTarget && !retry().challenge.valid && !hooks().trace))
            {
                hooks().setState(rdram, ctx, runtime);
                return;
            }
            const uint32_t object = getRegU32(ctx, 4);
            const uint32_t before = currentState(rdram, object);
            callOriginal(rdram, ctx, runtime, hooks().setState, kSetState, {object, getRegU32(ctx, 5)});
            const uint32_t after = currentState(rdram, object);
            if (after == before)
                return;
            if (hooks().trace)
                std::cout << "[restart] state 0x" << std::hex << object << ": 0x" << before << " -> 0x" << after << std::dec
                          << (retry().actionTarget ? " (action)" : "") << " frame " << readWord(rdram, kLevelFrame) << std::endl;
            if (retry().actionTarget)
                retry().actionChanges.push_back({object, before, after});
            else if (retry().challenge.valid && object == retry().challenge.object && terminalState(after))
                dropChallenge("completed");
        }

        // An ACTION handled by a script object: the state change it makes of
        // its own object is a challenge given.
        void postMessageHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t message = getRegU32(ctx, 4);
            const uint32_t target = readWord(rdram, message + 8u);
            if (hooks().trace && readWord(rdram, message + 0x10u) == nameHash("TimerFinished"))
                std::cout << "[restart] TimerFinished to 0x" << std::hex << target << " data 0x"
                          << readWord(rdram, message + 0x14u) << std::dec << " frame " << readWord(rdram, kLevelFrame)
                          << std::endl;
            if (retry().actionTarget || retry().retrying || readWord(rdram, message + 0x10u) != kActionEvent || !target ||
                !challengeLevel(rdram))
            {
                hooks().postMessage(rdram, ctx, runtime);
                return;
            }
            const uint32_t level = levelObject(rdram);
            const uint32_t player = level + kPlayerOne;
            for (uint32_t i = 0; i < 16u; ++i)
                retry().actionMatrix[i] = readWord(rdram, player + kPlayerMatrix + i * 4u);
            for (uint32_t i = 0; i < 4u; ++i)
                retry().actionPosition[i] = readWord(rdram, player + kPlayerPosition + i * 4u);
            retry().actionTarget = target;
            retry().actionChanges.clear();
            callOriginal(rdram, ctx, runtime, hooks().postMessage, kPostMessage, {message});
            retry().actionTarget = 0;
            if (retry().actionChanges.empty())
                return;
            // After the handler (the message was handled synchronously) the
            // engine's object lookup can be called; callGuestFunction keeps
            // the registers the call returned with.
            const Change *given = nullptr;
            for (const Change &change : retry().actionChanges)
                if (objectNamed(rdram, ctx, runtime, change.object) == target)
                    given = &change;
            if (!given || terminalState(given->to))
                return;
            retry().challenge.valid = true;
            retry().challenge.object = given->object;
            retry().challenge.from = given->from;
            retry().challenge.to = given->to;
            std::memcpy(retry().challenge.matrix, retry().actionMatrix, sizeof(retry().challenge.matrix));
            std::memcpy(retry().challenge.position, retry().actionPosition, sizeof(retry().challenge.position));
            retry().retryable = true;
            std::cout << "[restart] challenge given: object 0x" << std::hex << given->object << " 0x" << given->from
                      << " -> 0x" << given->to << std::dec << std::endl;
        }

        // setObjectiveState: the giver's objective (its record names the
        // giver) completed.
        void setObjectiveHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t records = readWord(rdram, kObjectives);
            const uint32_t index = getRegU32(ctx, 4);
            if (retry().challenge.valid && !retry().retrying && getRegU32(ctx, 5) == readWord(rdram, kObjectiveComplete) && records &&
                index < readWord(rdram, kObjectiveCount) &&
                readWord(rdram, records + index * kObjectiveSize + kObjectiveGiver) == retry().challenge.object)
                dropChallenge("objective complete");
            hooks().setObjective(rdram, ctx, runtime);
        }

        // The respawn's locator lookup: our name is where the challenge was
        // given (the respawn copies the matrix, negating its [1][1], and
        // takes the last row as the position).
        void findLocatorHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (!retry().respawnArmed || getRegU32(ctx, 5) != kRetryLocator)
            {
                hooks().findLocator(rdram, ctx, runtime);
                return;
            }
            const uint32_t out = getRegU32(ctx, 6);
            for (uint32_t i = 0; i < 12u; ++i)
            {
                uint32_t word = retry().challenge.matrix[i];
                if (i == 5u)
                    word ^= 0x80000000u;
                writeWord(rdram, out + i * 4u, word);
            }
            for (uint32_t i = 0; i < 4u; ++i)
                writeWord(rdram, out + 0x30u + i * 4u, retry().challenge.position[i]);
            setReturnU32(ctx, 1u);
            ctx->pc = getRegU32(ctx, 31);
        }

        bool retryChallenge(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (!retry().challenge.valid || !challengeLevel(rdram))
                return false;
            const uint32_t now = currentState(rdram, retry().challenge.object);
            if (!now)
            {
                dropChallenge("object gone");
                return false;
            }
            std::cout << "[restart] challenge again: object 0x" << std::hex << retry().challenge.object << " in 0x" << now
                      << " -> 0x" << retry().challenge.from << " -> 0x" << retry().challenge.to << std::dec << " (frame "
                      << readWord(rdram, kLevelFrame) << ")" << std::endl;
            const uint32_t player = levelObject(rdram) + kPlayerOne;
            writeWord(rdram, player + kPlayerFlags, readWord(rdram, player + kPlayerFlags) | 4u);
            retry().respawnArmed = true;
            call(rdram, ctx, runtime, kRespawnNamed, {player, kRetryLocator, 1u});
            retry().respawnArmed = false;
            retry().retrying = true;
            if (now != retry().challenge.from)
                call(rdram, ctx, runtime, kSetState, {retry().challenge.object, retry().challenge.from});
            call(rdram, ctx, runtime, kSetState, {retry().challenge.object, retry().challenge.to});
            retry().retrying = false;
            return true;
        }

        // ---- Tutorial ---------------------------------------------------------

        // The current lesson (0 = A) or -1.
        int currentLesson(const uint8_t *rdram)
        {
            const uint32_t state = currentState(rdram, kTutorialController);
            char name[] = "LessonA";
            for (int n = 0; n < 23; ++n)
            {
                name[6] = static_cast<char>('A' + n);
                if (state == nameHash(name))
                    return n;
            }
            return -1;
        }

        // As inlinePauseJumpToTutorial does: "GoToLesson" + letter to
        // ScriptObj_PlayerOne, handled at once.
        bool jumpToLesson(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, int lesson)
        {
            if (lesson < 0 || lesson >= 23)
                return false;
            const uint32_t player = objectNamed(rdram, ctx, runtime, kTutorialPlayer);
            if (!player)
                return false;
            char message[] = "GoToLessonA";
            message[10] = static_cast<char>('A' + lesson);
            if (hooks().trace)
                std::cout << "[restart] " << message << std::endl;
            return call(rdram, ctx, runtime, kSendMessage, {0u, player, nameHash(message), 0u, 1u});
        }

        bool restartLesson(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (estateActive()) // Estate (TUT/Estate) plays as level 7: its own lessons
            {
                const int item = estateCurrentLessonItem(rdram);
                return item >= 0 && estateJumpToLessonItem(rdram, ctx, runtime, item);
            }
            const int lesson = currentLesson(rdram);
            if (lesson < 0)
                return false;
            std::cout << "[restart] lesson " << static_cast<char>('A' + lesson) << " again" << std::endl;
            return jumpToLesson(rdram, ctx, runtime, lesson);
        }

        // ---- split screen -----------------------------------------------------

        bool restartSplitRun(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t level = levelObject(rdram);
            if (!level || !readWord(rdram, level + kLevelRunTimer))
                return false; // no game running (its menu is up)
            const uint8_t game = readByte(rdram, level + kLevelSplitGame);
            std::cout << "[restart] split screen game " << static_cast<int>(game) << " again" << std::endl;
            if (game == kSplitTwentyOne)
                call(rdram, ctx, runtime, kTwentyOneEnd, {});
            else if (game == kSplitRescue)
            {
                call(rdram, ctx, runtime, kRescueEnd, {level + kPlayerOne, 1u});
                call(rdram, ctx, runtime, kRescueEnd, {level + kPlayerTwo, 1u});
                call(rdram, ctx, runtime, kParrots, {0u});
            }
            // The game menu's choice is what StartEnd starts.
            uint32_t menu = 0;
            call(rdram, ctx, runtime, kFindWidget, {readWord(rdram, kUiRoot), kSplitMenuPath, 1u}, &menu);
            if (menu)
            {
                const uint32_t value = scratchBelow(ctx);
                writeWord(rdram, value, game);
                call(rdram, ctx, runtime, kSetProperty, {menu, kSelectedIndexName, value});
            }
            call(rdram, ctx, runtime, kSplitStart, {});
            return true;
        }

        // ---- requests ---------------------------------------------------------

        struct Requests
        {
            std::atomic<Request> pending{Request::None};
        };
        Requests &requests() { return zx::session::state<Requests>(); }

        // ---- the level's UI leaving ---------------------------------------------
        //
        // The front end gives the level's UI the time it takes to leave; the
        // level again, from the level through the loader to the level, gave it
        // none. A level with the in-game editor (the Cannery, the Airfield,
        // Industrial) loads ParkEditor.zsc beside InGameUI, and its tree goes
        // 2 seconds after the level ends: shutdownParkEditor plays its
        // OnDestroyParkEditor ACTION (StartDelay 2: DESTROY Root.ParkEditor).
        // So the next level's UI was read while the old tree still held its
        // 124 KB of zgui's 768 KB heap: InGame's libraries found no room
        // (heapTake answered 0 and zgui made them at guest address 0, the HUD's
        // boxes then drawn untextured and its text gone), and the old tree's
        // DESTROY then came and took the ParkEditor library the new tree had
        // found already loaded. Not the disc's speed: zgui's ACTIONs do not
        // step while loadLevel runs, so the 2 seconds would start in the new
        // level on the PS2 too; the shipped game never goes from a level to a
        // level without the front end (its one such path,
        // inlinePauseRestartFromLoad, no page calls). The loader now waits, as
        // the front end would, until zgui has no ACTION playing and no node
        // queued to go.
        constexpr uint32_t kZguiPlaying = sym::g_playing;                // zge::List of the ACTIONs playing
        constexpr uint32_t kZguiDestroyQueue = sym::g_zgui + 0x284u;     // Zgui::destroyQueue (zge/zgui/widget.h)
        constexpr uint32_t kListCount = 0x08u;                           // zge::List::count
        constexpr uint32_t kLoaderCountdown = sym::g_frameCountdown;     // byte: the loader's frames so far
        constexpr uint32_t kTopStateLoader = sym::topStates + 2u * 0x14u; // state 2, the loader (records of 0x14 bytes)
        // At most 10 seconds (the ACTION's own wait is 2): a UI that never
        // stops playing still gets its level.
        constexpr uint32_t kTeardownFramesMax = 600u;

        struct Teardown
        {
            std::atomic<bool> waiting{false};
            uint32_t frames = 0;
        };
        Teardown &teardown() { return zx::session::state<Teardown>(); }

        // At VBlank while the level again waits in the loader: its frame
        // count held at 0 while zgui is still busy with the level's UI.
        void waitForUiTeardown(uint8_t *rdram)
        {
            Teardown &t = teardown();
            const uint32_t state = readWord(rdram, kTopState);
            if (state != kTopStateLoader)
            {
                if (state == kTopStateLevel && t.frames == 0)
                    return; // (the restart's frame: the loader not entered yet)
                t.waiting = false;
                return;
            }
            const uint32_t playing = readWord(rdram, kZguiPlaying + kListCount);
            const uint32_t queued = readWord(rdram, kZguiDestroyQueue + kListCount);
            if ((playing != 0u || queued != 0u) && t.frames < kTeardownFramesMax)
            {
                if (uint8_t *p = getMemPtr(rdram, kLoaderCountdown))
                    *p = 0u;
                ++t.frames;
                return;
            }
            std::cout << "[restart] the level's UI gone after " << t.frames << " frames in the loader";
            if (playing != 0u || queued != 0u)
                std::cout << " (still " << playing << " playing, " << queued << " queued: loaded anyway)";
            std::cout << std::endl;
            t.waiting = false;
        }

        void prepareLevelLoad(uint8_t *rdram)
        {
            writeWord(rdram, kLoaderTarget, kStateLevel);
            if (uint8_t *p = getMemPtr(rdram, kLoaderDelay))
                *p = 1u;
            teardown().frames = 0;
            teardown().waiting = true;
        }
    }

    Request keyRequest(const uint8_t *rdram)
    {
        const uint8_t type = readByte(rdram, kGameType);
        if (type == kTypeSplitScreen)
        {
            const uint32_t level = levelObject(rdram);
            return level && readWord(rdram, level + kLevelRunTimer) ? Request::SplitRun : Request::None;
        }
        if (type == kTypeTimedRun)
            return Request::TimedRun;
        if (readByte(rdram, kLevelIndex) == kLevelTutorial)
            return (estateActive() ? estateCurrentLessonItem(rdram) : currentLesson(rdram)) >= 0 ? Request::Lesson
                                                                                                : Request::Level;
        if (type == kTypeCareer && retry().retryable.load())
            return Request::Challenge;
        return Request::Level;
    }

    void request(Request what) { requests().pending = what; }

    bool runPending(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        Request what = requests().pending.load();
        if (what == Request::None || readWord(rdram, kGameMode) != kModePlay)
            return false;
        requests().pending = Request::None;
        if (what == Request::Key)
            what = keyRequest(rdram);
        switch (what)
        {
        case Request::Level:
            std::cout << "[restart] the level again (game type " << static_cast<int>(readByte(rdram, kGameType)) << ")"
                      << std::endl;
            prepareLevelLoad(rdram);
            // As inlinePauseExit does: a tail call of the state switch (the
            // update's caller gets control back from it).
            SET_GPR_U32(ctx, 4, kStateLoader);
            hooks().switchState(rdram, ctx, runtime);
            return true;
        case Request::TimedRun:
            if (readByte(rdram, kGameType) != kTypeTimedRun)
                return false;
            std::cout << "[restart] the timed run again" << std::endl;
            hooks().playAgain(rdram, ctx, runtime);
            return true;
        case Request::SplitRun:
            if (readByte(rdram, kGameType) != kTypeSplitScreen || !restartSplitRun(rdram, ctx, runtime))
                return false;
            break;
        case Request::Challenge:
            if (!retryChallenge(rdram, ctx, runtime))
                return false;
            break;
        case Request::Lesson:
            if (readByte(rdram, kLevelIndex) != kLevelTutorial || !restartLesson(rdram, ctx, runtime))
                return false;
            break;
        default:
            return false;
        }
        // In place of this update.
        ctx->pc = getRegU32(ctx, 31);
        return true;
    }

    void onVsync(uint8_t *rdram)
    {
        if (teardown().waiting.load())
            waitForUiTeardown(rdram);
        if (readWord(rdram, kTopState) == kTopStateLevel)
            return;
        requests().pending = Request::None;
        if (retry().retryable.load())
            dropChallenge("out of the level");
    }

    bool challengeRetryable(const uint8_t *rdram) { return retry().retryable.load() && challengeLevel(rdram); }

    std::string tutorialCheckboxItems(const uint8_t *rdram)
    {
        // "!~" an empty cell, "~" a lesson to do, "`" a lesson done
        // (inlinePausePopulateTutorialCheckboxes 0x1BB008); rows: Resume
        // Tutorial, Restart Lesson, the lessons, then Level Select and Exit
        // Game (the port's lines after it have no cell).
        std::string items = "!~\\n!~";
        const char *const after = "\\n!~\\n!~\\n";
        if (estateActive() && readByte(rdram, kLevelIndex) == kLevelTutorial)
            return items + estateLessonCheckboxes(rdram) + after;
        const uint32_t records = readWord(rdram, kObjectives);
        const uint32_t count = readWord(rdram, kObjectiveCount);
        for (uint32_t i = 0; records && i < count && i < 64u; ++i)
            items += readWord(rdram, records + i * kObjectiveSize + kObjectiveDone) ? "\\n`" : "\\n~";
        items += after;
        return items;
    }

    bool install(PS2Machine &runtime)
    {
        if (const char *trace = std::getenv("ZX_TRACE_RESTART"); trace && trace[0] == '1')
            hooks().trace = true;
        hooks().switchState = runtime.lookupFunction(kSwitchState);
        hooks().playAgain = runtime.lookupFunction(kPlayAgain);
        hooks().setState = runtime.lookupFunction(kSetState);
        hooks().postMessage = runtime.lookupFunction(kPostMessage);
        hooks().setObjective = runtime.lookupFunction(kSetObjective);
        hooks().findLocator = runtime.lookupFunction(kFindLocator);
        const uint32_t needed[] = {kRespawnNamed, kSendMessage, kFindObjects, kTwentyOneEnd, kRescueEnd, kParrots,
                                   kSplitStart, kFindWidget, kSetProperty, kGetProperty};
        for (uint32_t address : needed)
            if (!runtime.hasFunction(address))
                return false;
        return hooks().switchState && hooks().playAgain && hooks().setState && hooks().postMessage && hooks().setObjective && hooks().findLocator &&
               runtime.replaceFunction(kSetState, &setStateHook) &&
               runtime.replaceFunction(kPostMessage, &postMessageHook) &&
               runtime.replaceFunction(kSetObjective, &setObjectiveHook) &&
               runtime.replaceFunction(kFindLocator, &findLocatorHook);
    }
}

namespace aggressive_inline
{
    namespace
    {
        void askAfterPause(R5900Context *ctx, restart::Request what)
        {
            restart::request(what);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void restartLevelNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        if (!restart::hooks().switchState)
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        std::cout << "[restart] the level again (pause menu, game type "
                  << static_cast<int>(restart::readByte(rdram, restart::kGameType)) << ")" << std::endl;
        restart::prepareLevelLoad(rdram);
        // As inlinePauseExit does: a tail call of the state switch.
        SET_GPR_U32(ctx, 4, restart::kStateLoader);
        restart::hooks().switchState(rdram, ctx, runtime);
    }

    void retryChallengeNative(uint8_t *, R5900Context *ctx, PS2Machine *)
    {
        askAfterPause(ctx, restart::Request::Challenge);
    }

    // As the lesson items do (inlinePauseJumpToTutorial), at once.
    void restartLessonNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        if (restart::readByte(rdram, restart::kLevelIndex) == restart::kLevelTutorial)
            restart::restartLesson(rdram, ctx, runtime);
        ctx->pc = getRegU32(ctx, 31);
    }

    // The lesson items with Restart Lesson above them: the menu's
    // SelectedIndex less two (Resume Tutorial, Restart Lesson) is the lesson.
    void jumpToLessonNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        const uint32_t menu = restart::readWord(rdram, getRegU32(ctx, 4));
        if (menu)
        {
            const uint32_t value = restart::scratchBelow(ctx);
            restart::writeWord(rdram, value, 0xFFFFFFFFu);
            restart::call(rdram, ctx, runtime, restart::kGetProperty, {menu, restart::kSelectedIndexName, value});
            const int index = static_cast<int>(restart::readWord(rdram, value));
            if (estateActive() && restart::readByte(rdram, restart::kLevelIndex) == restart::kLevelTutorial)
                estateJumpToLessonItem(rdram, ctx, runtime, index - 2);
            else
                restart::jumpToLesson(rdram, ctx, runtime, index - 2);
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    void restartRunNative(uint8_t *, R5900Context *ctx, PS2Machine *)
    {
        askAfterPause(ctx, restart::Request::SplitRun);
    }
}
