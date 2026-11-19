#include "runtime/ps2_vu1.h"
#include "ps2_vu1_exec_upper.h"

VU1Interpreter::FmacFn VU1Interpreter::selectFmacFn(const FmacPlan &plan)
{
    static const std::array<FmacFn, 1024> table = makeFmacTable(std::make_index_sequence<1024>{});
    const size_t index = static_cast<size_t>(plan.kind) * 128u + static_cast<size_t>(plan.source) * 32u +
                         static_cast<size_t>(plan.dest & 0xFu) * 2u + (plan.toAcc ? 1u : 0u);
    return table[index];
}

VU1Interpreter::FmacFn VU1Interpreter::selectUpperFastFn(uint32_t upper)
{
    static const std::array<FmacFn, 11u * 64u> table = makeUpperFastTable(std::make_index_sequence<11u * 64u>{});
    int op = 0, dest = 0, sub = 0;
    upperFastKey(upper, op, dest, sub);
    if (op == UpperFastNone)
        return nullptr;
    return table[static_cast<size_t>(op) * 64u + static_cast<size_t>(dest) * 4u + static_cast<size_t>(sub)];
}
