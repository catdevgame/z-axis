// Host forms: the registry and the work a form leaves for after its return
// (ee/host_form.h).

#include "ee/host_form.h"

#include <cstring>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

namespace game::host
{
    namespace
    {
        std::vector<Form> &forms()
        {
            static std::vector<Form> all;
            return all;
        }

        thread_local Call t_call;
        thread_local AfterReturn t_afterReturn;

        // The PS2 forms install replaced, by runtime and address.
        std::map<std::pair<const PS2Machine *, uint32_t>, PS2Machine::RecompiledFunction> &ps2Forms()
        {
            static std::map<std::pair<const PS2Machine *, uint32_t>, PS2Machine::RecompiledFunction> all;
            return all;
        }
    }

    Registration::Registration(const Form &form)
    {
        // (a form of a function this engine version has not got: its game's symbol is unbound, and it registers
        // nothing, as a planned readable function does)
        if (readable::symbolBound(form.address))
            forms().push_back(form);
    }

    bool install(PS2Machine &runtime, Game game, const char *system)
    {
        std::vector<const Form *> chosen;
        for (const Form &form : forms())
        {
            if (form.game != game || std::strcmp(form.system, system) != 0)
                continue;
            if (!runtime.hasFunction(form.address))
            {
                std::cerr << "[host] " << system << ": " << form.name << " has no function at 0x" << std::hex
                          << form.address << std::dec << " in this game: the PS2 form stays" << std::endl;
                return false;
            }
            chosen.push_back(&form);
        }
        if (chosen.empty())
            return false;
        for (const Form *form : chosen)
        {
            ps2Forms()[{&runtime, form->address}] = runtime.lookupFunction(form->address);
            if (!runtime.replaceFunction(form->address, form->entry))
                return false;
        }
        return true;
    }

    const Call &currentCall() { return t_call; }

    void afterReturn(AfterReturn work) { t_afterReturn = std::move(work); }

    void continueInPs2Form(uint32_t function)
    {
        const auto found = ps2Forms().find({currentCall().runtime, function});
        const PS2Machine::RecompiledFunction ps2Form = found != ps2Forms().end() ? found->second : nullptr;
        if (!ps2Form)
        {
            std::cerr << "[host] no PS2 form at 0x" << std::hex << function << std::dec << " to go on in" << std::endl;
            return;
        }
        // (the form's return left the registers as the caller set them, pc at the return address: the PS2 form
        // runs from its start and returns there itself)
        afterReturn([ps2Form](const Call &call) { ps2Form(call.rdram, call.ctx, call.runtime); });
    }

    namespace detail
    {
        CallScope::CallScope(const Call &call) : outer(t_call) { t_call = call; }
        CallScope::~CallScope() { t_call = outer; }

        AfterReturn takeAfterReturn() { return std::exchange(t_afterReturn, AfterReturn{}); }
    }
}
