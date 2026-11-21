// Natives the retail game emptied: the console prints, the objective text
// of an older HUD, the ruler and the acid-drop banner. The GameCube build
// still prints; here each takes its arguments off the script stack, so a
// script that calls one stays balanced, and does nothing with them. Only
// consolePrintString, consolePrintInt and consolePrintNewLine are in the
// disc's scripts (Industrial, the Movie Lot and the test level LOCAL).

#include "native_parts.h"
#include "sym/aggressive/script/natives_cut.cpp.h"

namespace aggressive::script
{
    using namespace parts;

    // consolePrintString(text)
    int32_t nativeConsolePrintString()
    {
        popString();
        return 0;
    }
    READABLE_FUNCTION(nativeConsolePrintString, Verified);

    // consolePrintInt(value)
    int32_t nativeConsolePrintInt()
    {
        popCondition();
        return 0;
    }
    READABLE_FUNCTION(nativeConsolePrintInt, Verified);

    // consolePrintFloat(value)
    int32_t nativeConsolePrintFloat()
    {
        popFloat();
        return 0;
    }
    READABLE_FUNCTION(nativeConsolePrintFloat, Verified);

    // consolePrintHash(hash)
    int32_t nativeConsolePrintHash()
    {
        popInt();
        return 0;
    }
    READABLE_FUNCTION(nativeConsolePrintHash, Verified);

    // consolePrintNewLine()
    int32_t nativeConsolePrintNewLine()
    {
        return 0;
    }
    READABLE_FUNCTION(nativeConsolePrintNewLine, Verified);

    // addTrick(name)
    int32_t nativeAddTrick()
    {
        popString();
        return 0;
    }
    READABLE_FUNCTION(nativeAddTrick, Verified);

    // playTrickScore(score)
    int32_t nativePlayTrickScore()
    {
        popInt();
        return 0;
    }
    READABLE_FUNCTION(nativePlayTrickScore, Verified);

    // shatterObjective(objective)
    int32_t nativeShatterObjective()
    {
        popInt();
        return 0;
    }
    READABLE_FUNCTION(nativeShatterObjective, Verified);

    // newRecord(name, score)
    int32_t nativeNewRecord()
    {
        popString();
        popInt();
        return 0;
    }
    READABLE_FUNCTION(nativeNewRecord, Verified);

    // showAcidDrop(show)
    int32_t nativeShowAcidDrop()
    {
        popInt();
        return 0;
    }
    READABLE_FUNCTION(nativeShowAcidDrop, Verified);

    // setRuler(value, text)
    int32_t nativeSetRuler()
    {
        popFloat();
        popString();
        return 0;
    }
    READABLE_FUNCTION(nativeSetRuler, Verified);

    // showRuler(show)
    int32_t nativeShowRuler()
    {
        popInt();
        return 0;
    }
    READABLE_FUNCTION(nativeShowRuler, Verified);

    // showObjCurr(text, seconds)
    int32_t nativeShowObjCurr()
    {
        popString();
        popFloat();
        return 0;
    }
    READABLE_FUNCTION(nativeShowObjCurr, Verified);

    // fadeObjCurr()
    int32_t nativeFadeObjCurr()
    {
        return 0;
    }
    READABLE_FUNCTION(nativeFadeObjCurr, Verified);

    // breakPoint(): a debugger's trap. The original's `break` is never taken
    // by a shipped script and has nothing to do on the PC.
    int32_t nativeBreakPoint()
    {
        return 0;
    }
    READABLE_FUNCTION(nativeBreakPoint, Draft);
}
