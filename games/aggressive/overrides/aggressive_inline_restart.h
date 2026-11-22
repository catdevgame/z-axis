#pragma once

#include <cstdint>
#include <string>

class PS2Machine;
struct R5900Context;

namespace aggressive_inline::restart
{
    // Restarts through the game's own state machines (aggressive_inline_restart.cpp):
    // the level loaded again (the top-level state switch), the Timed Run's
    // Play Again, the split screen's game started over, a career challenge
    // given again by its level script object, a Tutorial lesson jumped to
    // again.
    enum class Request
    {
        None,
        Key,       // the Restart key: what fits the mode (keyRequest)
        Level,     // the level loaded again
        TimedRun,  // Timed Run: Play Again
        SplitRun,  // split screen: the same game again, both players
        Challenge, // Career: the last challenge given, again
        Lesson,    // Tutorial: the current lesson, again
    };

    // Hooks of the level scripts (state changes, messages, objectives) and
    // the respawn's locator lookup. False if a function is missing.
    bool install(PS2Machine &runtime);

    // What the Restart key does in the level now: Lesson in the Tutorial (or
    // Level between lessons), TimedRun, SplitRun (None before a game is
    // chosen), Challenge in the Career when one can be retried, else Level.
    Request keyRequest(const uint8_t *rdram);

    // Asks for a restart at the next level update (game mode 2). Pause items
    // ask after closing the menu; the key on its press.
    void request(Request what);
    // The level update: a pending request runs in place of one update.
    // True if it ran (the caller returns without updating).
    bool runPending(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    // At VBlank: out of the level (the top-level state is the front end or
    // the loader), pending requests and the challenge record are dropped;
    // the level again waits in the loader until the level's UI has gone.
    void onVsync(uint8_t *rdram);

    // Career: a challenge was given in this level and can be given again
    // (its attempt is running, or failed back to its giver).
    bool challengeRetryable(const uint8_t *rdram);

    // Tutorial pause menu: the completed-lesson column as the game's
    // inlinePausePopulateTutorialCheckboxes builds it, with a blank row
    // for the Restart Lesson item (a script string literal's text).
    std::string tutorialCheckboxItems(const uint8_t *rdram);
}

namespace aggressive_inline
{
    // Pause menu native "pcRestartLevel" (registered with the settings
    // natives): loads the level again, as the menus' Start does, at once
    // (from an ACTION of the menu, as its Exit).
    void restartLevelNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    // Pause menu natives (registered with the settings natives). The
    // challenge and split screen ones ask for their restart at the next
    // level update (the item closes the menu and leaves the pause mode
    // first, as Resume does); the Tutorial's act at once, as its lesson
    // items do.
    void retryChallengeNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime); // pcRetryChallenge
    void restartLessonNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);  // pcRestartLesson (at once)
    void jumpToLessonNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);   // pcJumpToLesson <menu>
    void restartRunNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);     // pcRestartRun
}
