// The level scripts' stack machine: its stack, the opcodes and the
// interpreter loop (script.h describes the bytecode).

#include "script.h"

#include "ee/callback.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <climits>
#include "sym/aggressive/script/vm.cpp.h"

namespace aggressive::script
{
    namespace
    {
        // ---- the stack, inline as the game's handlers have it ----

        void push(uint32_t value)
        {
            ScriptVm &vm = *g_vm;
            *vm.stackTop = value;
            ++vm.stackTop;
        }

        // An address pushed: the stack keeps it as a word.
        void pushAddress(ee::Ptr<uint32_t> address)
        {
            ScriptVm &vm = *g_vm;
            *vm.addressStackTop = address;
            ++vm.stackTop;
        }

        uint32_t pop()
        {
            ScriptVm &vm = *g_vm;
            vm.stackTop -= 1;
            return *vm.stackTop;
        }

        // The word on top of the stack, operated on in place.
        uint32_t &top()
        {
            return *((*g_vm).stackTop - 1);
        }

        ee::Float topFloat()
        {
            return ee::Float::fromBits(top());
        }

        void setTopFloat(ee::Float value)
        {
            top() = value.bits();
        }

        bool floatValues()
        {
            return (*g_vm).valueType == ValueType::Float;
        }

        ScriptVariable &variable(uint32_t index)
        {
            return (*g_vm).frame[index];
        }

        // DIV: the EE's signed division, as the recompiled code does it for
        // the cases C++ leaves undefined (the game's code traps a zero
        // divisor with BREAK first, which the runtime only reports).
        int32_t divide(int32_t dividend, int32_t divisor)
        {
            if (divisor == 0)
                return dividend < 0 ? 1 : -1;
            if (divisor == -1 && dividend == INT32_MIN)
                return INT32_MIN;
            return dividend / divisor;
        }
    }

    // ---- the stack, as natives use it ----------------------------------------

    int32_t scriptPopInt()
    {
        return static_cast<int32_t>(pop());
    }
    READABLE_FUNCTION(scriptPopInt, Verified);

    int32_t scriptPopCondition()
    {
        return static_cast<int32_t>(pop());
    }
    READABLE_FUNCTION(scriptPopCondition, Verified);

    ee::Float scriptPopFloat()
    {
        return ee::Float::fromBits(pop());
    }
    READABLE_FUNCTION(scriptPopFloat, Verified);

    uint32_t scriptPopMessageData()
    {
        return pop();
    }
    READABLE_FUNCTION(scriptPopMessageData, Verified);

    void scriptPushInt(ee::Address value)
    {
        push(value);
    }
    READABLE_FUNCTION(scriptPushInt, Verified);

    void scriptPushResult(uint32_t value)
    {
        push(value);
    }
    READABLE_FUNCTION(scriptPushResult, Verified);

    void scriptPushFloat(ee::Float value)
    {
        push(value.bits());
    }
    READABLE_FUNCTION(scriptPushFloat, Verified);

    void scriptPushConstant(uint32_t value)
    {
        push(value);
    }
    READABLE_FUNCTION(scriptPushConstant, Verified);

    void scriptPushMessageData(uint32_t value)
    {
        push(value);
    }
    READABLE_FUNCTION(scriptPushMessageData, Verified);

    namespace
    {
        // The stack's own calls, by address: identical bodies fold into one
        // host function on some linkers.
        int32_t popInt() { return scriptPopInt(); }
        int32_t popCondition() { return scriptPopCondition(); }
        ee::Float popFloat() { return scriptPopFloat(); }
        void pushInt(uint32_t value) { scriptPushInt(value); }
        void pushConstant(uint32_t value) { scriptPushConstant(value); }

        ee::Ptr<uint32_t> valueAddress(ee::Ptr<ScriptVariable> variable)
        {
            return &variable->value;
        }
    }

    ee::Ptr<char> scriptPopString(ee::Ptr<uint32_t> second)
    {
        const uint32_t offset = static_cast<uint32_t>(popInt());
        const uint32_t below = static_cast<uint32_t>(popInt());
        if (second)
            *second = below;
        return (*g_vm).strings + offset;
    }
    READABLE_FUNCTION(scriptPopString, Verified);

    // ---- variables -----------------------------------------------------------

    void scriptLoadVariable(uint32_t index)
    {
        const ScriptVariable &v = variable(index);
        // The type decides how the arithmetic and jumps after it see values.
        (*g_vm).valueType = v.type;
        push(v.value);
    }
    READABLE_FUNCTION(scriptLoadVariable, Verified);

    void scriptPushVariableAddress(uint32_t index)
    {
        pushAddress(valueAddress((*g_vm).frame + index));
    }
    READABLE_FUNCTION(scriptPushVariableAddress, Verified);

    void scriptPick(uint32_t depth)
    {
        ScriptVm &vm = *g_vm;
        push(*(vm.stackTop - depth));
    }
    READABLE_FUNCTION(scriptPick, Verified);

    void scriptStoreVariable(uint32_t index)
    {
        // Whatever the variable's type, the word is stored as it is.
        ScriptVariable &v = variable(index);
        v.value = pop();
    }
    READABLE_FUNCTION(scriptStoreVariable, Verified);

    // ---- arithmetic on the top of the stack ----------------------------------
    //
    // Floats when the value type says so (the operand is then a float's
    // bits), else 32-bit ints.

    void scriptAddImmediate(uint32_t operand)
    {
        if (floatValues())
            setTopFloat(topFloat() + ee::Float::fromBits(operand));
        else
            top() += operand;
    }
    READABLE_FUNCTION(scriptAddImmediate, Verified);

    void scriptAddVariable(uint32_t index)
    {
        const ScriptVariable &v = variable(index);
        if (floatValues())
            setTopFloat(topFloat() + ee::Float::fromBits(v.value));
        else
            top() += v.value;
    }
    READABLE_FUNCTION(scriptAddVariable, Verified);

    void scriptSubtractImmediate(uint32_t operand)
    {
        if (floatValues())
            setTopFloat(topFloat() - ee::Float::fromBits(operand));
        else
            top() -= operand;
    }
    READABLE_FUNCTION(scriptSubtractImmediate, Verified);

    void scriptSubtractVariable(uint32_t index)
    {
        const ScriptVariable &v = variable(index);
        if (floatValues())
            setTopFloat(topFloat() - ee::Float::fromBits(v.value));
        else
            top() -= v.value;
    }
    READABLE_FUNCTION(scriptSubtractVariable, Verified);

    void scriptMultiplyImmediate(uint32_t operand)
    {
        if (floatValues())
            setTopFloat(topFloat() * ee::Float::fromBits(operand));
        else
            top() *= operand; // the low 32 bits of the signed product
    }
    READABLE_FUNCTION(scriptMultiplyImmediate, Verified);

    void scriptMultiplyVariable(uint32_t index)
    {
        const ScriptVariable &v = variable(index);
        if (floatValues())
            setTopFloat(topFloat() * ee::Float::fromBits(v.value));
        else
            top() *= v.value;
    }
    READABLE_FUNCTION(scriptMultiplyVariable, Verified);

    void scriptDivideImmediate(uint32_t operand)
    {
        if (floatValues())
            setTopFloat(topFloat() / ee::Float::fromBits(operand));
        else
            top() = static_cast<uint32_t>(divide(static_cast<int32_t>(top()), static_cast<int32_t>(operand)));
    }
    READABLE_FUNCTION(scriptDivideImmediate, Verified);

    void scriptDivideVariable(uint32_t index)
    {
        const ScriptVariable &v = variable(index);
        if (floatValues())
            setTopFloat(topFloat() / ee::Float::fromBits(v.value));
        else
            top() = static_cast<uint32_t>(divide(static_cast<int32_t>(top()), static_cast<int32_t>(v.value)));
    }
    READABLE_FUNCTION(scriptDivideVariable, Verified);

    // Bit operations: ints only.

    void scriptOrImmediate(uint32_t operand)
    {
        top() |= operand;
    }
    READABLE_FUNCTION(scriptOrImmediate, Verified);

    void scriptOrVariable(uint32_t index)
    {
        top() |= variable(index).value;
    }
    READABLE_FUNCTION(scriptOrVariable, Verified);

    void scriptAndImmediate(uint32_t operand)
    {
        top() &= operand;
    }
    READABLE_FUNCTION(scriptAndImmediate, Verified);

    void scriptAndVariable(uint32_t index)
    {
        top() &= variable(index).value;
    }
    READABLE_FUNCTION(scriptAndVariable, Verified);

    void scriptShiftImmediate(uint32_t amount, int32_t left)
    {
        // SLLV / SRLV: the amount's low 5 bits; right shifts are logical.
        if (left)
            top() <<= (amount & 31u);
        else
            top() >>= (amount & 31u);
    }
    READABLE_FUNCTION(scriptShiftImmediate, Verified);

    void scriptShiftVariable(uint32_t index, int32_t left)
    {
        const uint32_t amount = variable(index).value & 31u;
        if (left)
            top() <<= amount;
        else
            top() >>= amount;
    }
    READABLE_FUNCTION(scriptShiftVariable, Verified);

    // ---- natives ---------------------------------------------------------------

    void scriptCallNative(uint32_t nameHash)
    {
        ScriptVm &vm = *g_vm;
        // A linear search of the registered natives; an unknown one is
        // skipped.
        const uint32_t count = vm.nativeCount;
        uint32_t index = 0;
        while (index < count && vm.natives[index].nameHash != nameHash)
            ++index;
        if (index >= count)
            return;
        const ee::Callback<int32_t()> native{vm.natives[index].function};
        const int32_t pushedResult = native();
        // A result the next instruction does not store is dropped.
        if (pushedResult && vm.ip[1].opcode != Opcode::Store)
            vm.stackTop -= 1;
    }
    READABLE_FUNCTION(scriptCallNative, Verified);

    // ---- the interpreter -------------------------------------------------------

    namespace
    {
        // The conditional jumps' test of the popped value: as a float when the
        // value type is Float, else as an int.
        struct JumpBlocks
        {
            uint32_t intTest, floatNotTaken, intNotTaken;
        };
        constexpr uint32_t kReloadIp = points::scriptExecute::join2, kHandled = points::scriptExecute::join3;

        template <typename Test>
        bool popAndTest(Test test, const JumpBlocks &at)
        {
            if (floatValues())
                return test(popFloat(), ee::Float(0.0f));
            return test(popCondition(), 0);
        }

        void searchOpcode(uint32_t op)
        {
            if (op == 0x78)
                return;
            if (op >= 0x79)
            {
                if (op == 0xC8)
                    return;
                if (op >= 0xC9)
                {
                    if (op == 0xDC)
                        return;
                    if (op >= 0xDD)
                    {
                        if (op == 0xE6)
                            return;
                        if (op >= 0xE7)
                        {
                            if (op == 0xE7)
                                return;
                            return;
                        }
                        if (op == 0xDD)
                            return;
                        return;
                    }
                    if (op == 0xD2)
                        return;
                    if (op >= 0xD3)
                    {
                        if (op == 0xD3)
                            return;
                        return;
                    }
                    if (op == 0xC9)
                        return;
                    return;
                }
                if (op == 0xA0)
                    return;
                if (op >= 0xA1)
                {
                    if (op == 0xB4)
                        return;
                    if (op >= 0xB5)
                    {
                        if (op == 0xBE)
                            return;
                        return;
                    }
                    if (op == 0xAA)
                        return;
                    return;
                }
                if (op == 0x8C)
                    return;
                if (op >= 0x8D)
                {
                    if (op == 0x96)
                        return;
                    return;
                }
                if (op == 0x82)
                    return;
                return;
            }
            if (op == 0x46)
                return;
            if (op >= 0x47)
            {
                if (op == 0x5A)
                    return;
                if (op >= 0x5B)
                {
                    if (op == 0x64)
                        return;
                    if (op >= 0x65)
                    {
                        if (op == 0x65)
                            return;
                        if (op == 0x6E)
                            return;
                        return;
                    }
                    if (op == 0x5B)
                        return;
                    return;
                }
                if (op == 0x50)
                    return;
                if (op >= 0x51)
                {
                    if (op == 0x51)
                        return;
                    return;
                }
                if (op == 0x47)
                    return;
                return;
            }
            if (op == 0x28)
                return;
            if (op >= 0x29)
            {
                if (op == 0x3C)
                    return;
                if (op >= 0x3D)
                {
                    if (op == 0x3D)
                        return;
                    return;
                }
                if (op == 0x32)
                    return;
                return;
            }
            if (op == 0x14)
                return;
            if (op >= 0x15)
            {
                if (op == 0x1E)
                    return;
                return;
            }
            if (op == 0xA)
                return;
        }

        // SEND and SET may run other handlers (nested scriptExecute calls,
        // which move the machine's ip and frame): both are kept on the stack.
        void saveMachine()
        {
            scriptPushInt((*g_vm).ip);
            scriptPushInt((*g_vm).frame);
        }

        ee::Ptr<ScriptInstruction> restoreMachine()
        {
            ScriptVm &vm = *g_vm;
            vm.frame = ee::Ptr<ScriptVariable>(static_cast<uint32_t>(popInt()));
            vm.ip = ee::Ptr<ScriptInstruction>(static_cast<uint32_t>(popInt()));
            return vm.ip;
        }
    }

    void scriptExecute(ee::Ptr<ScriptInstruction> code)
    {
        namespace here = points::scriptExecute;
        ScriptVm &vm = *g_vm;
        vm.ip = code;
        if (code->opcode == Opcode::End)
            return;
        ee::Ptr<ScriptInstruction> ip = code;
        for (;;)
        {
            const uint32_t operand = ip->operand;
            searchOpcode(static_cast<uint32_t>(ip->opcode));
            const auto jumpIf = [&](bool taken, const JumpBlocks &at, bool intFallsIn = false) {
                ip = vm.ip;
                if (taken)
                    ip = vm.ip = ip + static_cast<int32_t>(ip->operand);
            };
            switch (ip->opcode)
            {
            case Opcode::Push: pushConstant(operand); ip = vm.ip; break;
            case Opcode::Store: scriptStoreVariable(operand); ip = vm.ip; break;
            case Opcode::CallNative: scriptCallNative(operand); ip = vm.ip; break;
            case Opcode::Load: scriptLoadVariable(operand); ip = vm.ip; break;
            case Opcode::PushAddress: scriptPushVariableAddress(operand); ip = vm.ip; break;
            case Opcode::Pick: scriptPick(operand); ip = vm.ip; break;
            case Opcode::AddImmediate: scriptAddImmediate(operand); ip = vm.ip; break;
            case Opcode::AddVariable: scriptAddVariable(operand); ip = vm.ip; break;
            case Opcode::SubtractImmediate: scriptSubtractImmediate(operand); ip = vm.ip; break;
            case Opcode::SubtractVariable: scriptSubtractVariable(operand); ip = vm.ip; break;
            case Opcode::MultiplyImmediate: scriptMultiplyImmediate(operand); ip = vm.ip; break;
            case Opcode::MultiplyVariable: scriptMultiplyVariable(operand); ip = vm.ip; break;
            case Opcode::DivideImmediate: scriptDivideImmediate(operand); ip = vm.ip; break;
            case Opcode::DivideVariable: scriptDivideVariable(operand); ip = vm.ip; break;
            case Opcode::OrImmediate: scriptOrImmediate(operand); ip = vm.ip; break;
            case Opcode::OrVariable: scriptOrVariable(operand); ip = vm.ip; break;
            case Opcode::AndImmediate: scriptAndImmediate(operand); ip = vm.ip; break;
            case Opcode::AndVariable: scriptAndVariable(operand); ip = vm.ip; break;
            case Opcode::ShiftLeftImmediate: scriptShiftImmediate(operand, 1); ip = vm.ip; break;
            case Opcode::ShiftLeftVariable: scriptShiftVariable(operand, 1); ip = vm.ip; break;
            case Opcode::ShiftRightImmediate: scriptShiftImmediate(operand, 0); ip = vm.ip; break;
            case Opcode::ShiftRightVariable: scriptShiftVariable(operand, 0); ip = vm.ip; break;

            case Opcode::Send:
            {
                const uint32_t sender = static_cast<uint32_t>(popInt());
                const uint32_t event = static_cast<uint32_t>(popInt());
                saveMachine();
                scriptSendEvent(vm.ip->operand, sender, event);
                ip = restoreMachine();
                break;
            }
            case Opcode::SetState:
            {
                const uint32_t state = static_cast<uint32_t>(popInt());
                saveMachine();
                // By address: the restart override hooks the state change.
                scriptSetState(vm.ip->operand, state);
                ip = restoreMachine();
                break;
            }

            case Opcode::JumpIfNotZero:
            {
                constexpr JumpBlocks at{here::taken41, kHandled, kReloadIp};
                jumpIf(popAndTest([](auto x, auto zero) { return x != zero; }, at), at);
                break;
            }
            case Opcode::JumpIfZero:
            {
                constexpr JumpBlocks at{here::taken43, kReloadIp, kHandled};
                jumpIf(popAndTest([](auto x, auto zero) { return x == zero; }, at), at);
                break;
            }
            case Opcode::JumpIfNegative:
            {
                constexpr JumpBlocks at{here::taken45, kReloadIp, kHandled};
                jumpIf(popAndTest([](auto x, auto zero) { return x < zero; }, at), at);
                break;
            }
            case Opcode::JumpIfPositive:
            {
                constexpr JumpBlocks at{here::taken47, kReloadIp, kReloadIp};
                jumpIf(popAndTest([](auto x, auto zero) { return x > zero; }, at), at);
                break;
            }
            case Opcode::JumpIfNotPositive:
            {
                constexpr JumpBlocks at{here::taken49, kReloadIp, kHandled};
                jumpIf(popAndTest([](auto x, auto zero) { return x <= zero; }, at), at);
                break;
            }
            case Opcode::JumpIfNotNegative:
            {
                constexpr JumpBlocks at{here::taken51, kReloadIp, kReloadIp};
                jumpIf(popAndTest([](auto x, auto zero) { return x >= zero; }, at), at, true);
                break;
            }
            case Opcode::Jump:
                ip = vm.ip = ip + static_cast<int32_t>(operand);
                break;

            case Opcode::SetType: vm.valueType = static_cast<ValueType>(operand); break;

            case Opcode::End:
            default:
                // Unknown opcodes are skipped.
                break;
            }
            // On to the next instruction (a jump's target is counted from it).
            ++ip;
            vm.ip = ip;
            if (ip->opcode == Opcode::End)
                break;
        }
    }
    READABLE_FUNCTION(scriptExecute, Verified);
}
