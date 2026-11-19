// BMX XXX's three objective types with no parameters: "NO OP" (never done
// by itself), "TRUE" (done by the first game event it is sent) and "FALSE"
// (never done). BMX XXX's own code (its game block; one small file each in
// the Xbox build); the names are from the types' registration strings.

#include "objective_calls.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/simple_types.cpp.h"

namespace game::bmx::objectives
{
    void trueObjectiveEvent(Objective objective);

    // registerNoOpType: the "NO OP" type: no callbacks.
    void registerNoOpType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::noOpTypeName);
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerNoOpType, Verified);

    // registerTrueType: the "TRUE" type: an event completes it.
    void registerTrueType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::trueTypeName);
        setup->hooks.onEvent = sym::trueObjectiveEvent;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerTrueType, Verified);

    // trueObjectiveEvent (the type's event): an objective that is active or
    // in progress is done. The event itself is not looked at.
    void trueObjectiveEvent(Objective objective)
    {
        if (zge::objectiveState(objective) == kObjectiveActive || zge::objectiveState(objective) == kObjectiveInProgress)
            calls::setObjectiveStateTail(objective, kObjectiveDone);
    }
    READABLE_FUNCTION_WAITS(trueObjectiveEvent, Verified);

    // registerFalseType: the "FALSE" type: no callbacks.
    void registerFalseType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::falseTypeName);
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerFalseType, Verified);
}
