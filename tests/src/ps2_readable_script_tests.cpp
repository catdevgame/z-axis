// The level scripts' stack machine (runtime/src/game/aggressive/script):
// a handler written by hand run through the readable interpreter, the
// natives' names. The game's own scripts are checked by ps2xFnCheck.
#include "MiniTest.h"

#include "aggressive/script/script.h"
#include "ee/abi.h"
#include "ee/readable.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    namespace s = aggressive::script;
    using s::Opcode;

    struct GuestRam
    {
        // RAM as the game has it: the guest map (ee::at is its base + address).
        guest_map::Map map;
        uint8_t *previous = ee::g_ram;
        GuestRam() { ee::g_ram = map.create(PS2_RAM_SIZE, guest_map::Layout::Game) ? map.base() : nullptr; }
        ~GuestRam() { ee::g_ram = previous; }
    };

    constexpr uint32_t kStack = 0x00100000u;
    constexpr uint32_t kFrame = 0x00110000u;
    constexpr uint32_t kNatives = 0x00120000u;
    constexpr uint32_t kCode = 0x00130000u;
    constexpr uint32_t kNative = 0x00200000u; // a made-up native: pushes 99, returns 1
    constexpr uint32_t kNativeHash = 0x0ABCDEF0u;

    // Calls a readable function by address runs it (as the game would with
    // it on); the native is answered here.
    struct Machine : ee::CallHandler
    {
        std::vector<uint32_t> called;
        void call(ee::CallRegs &regs) override
        {
            called.push_back(regs.target);
            if (regs.target == kNative)
            {
                s::ScriptVm &vm = *s::g_vm;
                *vm.stackTop = 99u;
                ++vm.stackTop;
                regs.v0 = 1;
                return;
            }
            const game::readable::Function *f = game::readable::find(regs.target);
            if (!f || !f->readable)
                return;
            R5900Context ctx;
            for (int i = 0; i < regs.intCount; ++i)
                ee::setGpr64(&ctx, ee::reg::a0 + i, regs.a[i]);
            for (int i = 0; i < regs.floatCount; ++i)
                ctx.f[ee::reg::f12 + i] = regs.f[i];
            ee::setGpr64(&ctx, ee::reg::sp, 0x01F00000u);
            ee::setGpr64(&ctx, ee::reg::ra, 0x00100000u);
            f->readable(ee::g_ram, &ctx, nullptr);
            regs.v0 = ee::gpr64(&ctx, ee::reg::v0);
            regs.f0 = ctx.f[0];
        }
    };

    void put(uint32_t address, std::initializer_list<uint32_t> words)
    {
        for (uint32_t w : words)
        {
            *ee::at<uint32_t>(address) = w;
            address += 4;
        }
    }

    uint32_t op(Opcode o) { return static_cast<uint32_t>(o); }
}

void register_ps2_readable_script_tests()
{
    MiniTest::Case("ReadableScript", [](TestCase &tc)
    {
        tc.Run("the bytecode's layout", [](TestCase &t)
        {
            t.Equals(sizeof(s::ScriptInstruction), size_t(8), "instructions are 8 bytes");
            t.Equals(op(Opcode::End), 0x1F40u, "End");
            t.Equals(s::g_vm.address + offsetof(s::ScriptVm, ip), 0x002798E4u, "ip at gp-0x100C");
            t.Equals(s::g_vm.address + offsetof(s::ScriptVm, valueType), 0x002798E8u, "value type at gp-0x1008");
        });

        tc.Run("a handler runs to End: arithmetic, a jump, a native, stores", [](TestCase &t)
        {
            GuestRam ram;
            s::ScriptVm &vm = *s::g_vm;
            vm.stackBase = vm.stackTop = ee::Ptr<uint32_t>(kStack);
            vm.frame = ee::Ptr<s::ScriptVariable>(kFrame);
            vm.natives = ee::Ptr<s::ScriptNative>(kNatives);
            vm.nativeCount = 1;
            put(kNatives, {kNativeHash, kNative});
            // var0 = 5 (int); var1, var2 = 0
            put(kFrame, {1u, 5u, 1u, 0u, 1u, 0u});
            // if (var0 - 5 != 0) skip the store of 42; var1 = 42; var2 = native(); end
            put(kCode, {op(Opcode::Load), 0u,
                        op(Opcode::SubtractImmediate), 5u,
                        op(Opcode::JumpIfNotZero), 2u,
                        op(Opcode::Push), 42u,
                        op(Opcode::Store), 1u,
                        op(Opcode::CallNative), kNativeHash,
                        op(Opcode::Store), 2u,
                        op(Opcode::End), 0u});
            Machine machine;
            ee::t_ee.callHandler = &machine;
            s::scriptExecute(ee::Ptr<s::ScriptInstruction>(kCode));
            ee::t_ee.callHandler = nullptr;
            t.Equals(*ee::at<uint32_t>(kFrame + 0x0C), 42u, "var1: the jump was not taken");
            t.Equals(*ee::at<uint32_t>(kFrame + 0x14), 99u, "var2: the native's result, kept for the store");
            t.Equals(vm.stackTop.address, kStack, "the stack is empty again");
            t.Equals(vm.ip.address, kCode + 7u * 8u, "ip on End");
            t.IsTrue(std::find(machine.called.begin(), machine.called.end(), kNative) != machine.called.end(), "the native ran");
        });

        tc.Run("float values: the jump tests the popped float", [](TestCase &t)
        {
            GuestRam ram;
            s::ScriptVm &vm = *s::g_vm;
            vm.stackBase = vm.stackTop = ee::Ptr<uint32_t>(kStack);
            vm.frame = ee::Ptr<s::ScriptVariable>(kFrame);
            const float half = 0.5f, one = 1.0f;
            uint32_t halfBits, oneBits;
            std::memcpy(&halfBits, &half, 4);
            std::memcpy(&oneBits, &one, 4);
            put(kFrame, {2u, halfBits, 1u, 0u});
            // var0 (float 0.5) - 1.0 < 0: jump over "var1 = 1"; var1 = 2
            put(kCode, {op(Opcode::Load), 0u,
                        op(Opcode::SubtractImmediate), oneBits,
                        op(Opcode::JumpIfNegative), 2u,
                        op(Opcode::Push), 1u,
                        op(Opcode::Store), 1u,
                        op(Opcode::Push), 2u,
                        op(Opcode::Store), 1u,
                        op(Opcode::End), 0u});
            Machine machine;
            ee::t_ee.callHandler = &machine;
            s::scriptExecute(ee::Ptr<s::ScriptInstruction>(kCode));
            ee::t_ee.callHandler = nullptr;
            t.Equals(*ee::at<uint32_t>(kFrame + 0x0C), 2u, "jumped over the first store");
            t.Equals(static_cast<uint32_t>(vm.valueType), 2u, "the loaded variable's type stays the value type");
        });

        tc.Run("native names", [](TestCase &t)
        {
            t.Equals(s::kNativeNameCount, size_t(123), "123 natives registered by the game");
            const char *name = s::nativeName(0x0A08339Du);
            t.IsTrue(name != nullptr && std::strcmp(name, "playCameraAnim") == 0, "the Tutorial's first native");
            t.IsTrue(s::nativeName(0x12345678u) == nullptr, "an unknown hash");
        });
    });
}
