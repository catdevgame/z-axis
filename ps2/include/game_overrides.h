#ifndef PS2_GAME_OVERRIDES_H
#define PS2_GAME_OVERRIDES_H

#include <cstdint>
#include <string>
#include <string_view>

class PS2Machine;

namespace ps2_game_overrides
{
    using ApplyFn = void (*)(PS2Machine &runtime);

    struct Descriptor
    {
        const char *name = "";
        const char *elfName = "";
        uint32_t entry = 0;
        uint32_t crc32 = 0;
        ApplyFn apply = nullptr;
    };

    class AutoRegister
    {
    public:
        explicit AutoRegister(const Descriptor &descriptor);
    };

    void registerDescriptor(const Descriptor &descriptor);
    void applyMatching(PS2Machine &runtime, const std::string &elfPath, uint32_t entry, uint32_t crc32, bool crc32Valid);

    // Runtime helper for game-specific modules:
    // bind an address to an existing syscall/stub handler by name.
    bool bindAddressHandler(PS2Machine &runtime, uint32_t address, std::string_view handlerName);

    // Called by the front ends (runner, app) after PS2Machine::run()
    // returns, with the guest stopped and its memory still there. These are
    // the process's (the developer tools' reports); a game module's own (one
    // saving progress on quit) is its session's, zx::session::addShutdownHook
    // (runtime/session.h), run after these.
    using ShutdownFn = void (*)(PS2Machine &runtime);
    void addShutdownHook(ShutdownFn fn);
    void runShutdownHooks(PS2Machine &runtime);

    // Called by applyMatching once every matching module has applied (a
    // module that needs to see what the others hooked; once per function).
    using AfterApplyFn = void (*)(PS2Machine &runtime);
    void addAfterApplyHook(AfterApplyFn fn);
}

#define PS2_GAME_OVERRIDE_JOIN2(a, b) a##b
#define PS2_GAME_OVERRIDE_JOIN(a, b) PS2_GAME_OVERRIDE_JOIN2(a, b)

// Each registration's names are made unique by __COUNTER__, not the line:
// the unity build puts several override sources in one unit, where two
// registrations can sit on the same line number.
#define PS2_REGISTER_GAME_OVERRIDE(NAME_LITERAL, ELF_NAME_LITERAL, ENTRY_U32, CRC32_U32, APPLY_FN) \
    PS2_REGISTER_GAME_OVERRIDE_AS(__COUNTER__, NAME_LITERAL, ELF_NAME_LITERAL, ENTRY_U32, CRC32_U32, APPLY_FN)

#define PS2_REGISTER_GAME_OVERRIDE_AS(ID, NAME_LITERAL, ELF_NAME_LITERAL, ENTRY_U32, CRC32_U32, APPLY_FN) \
    namespace                                                                                       \
    {                                                                                               \
        static const ps2_game_overrides::Descriptor PS2_GAME_OVERRIDE_JOIN(kPs2OverrideDesc_, ID)  \
        {                                                                                           \
            NAME_LITERAL,                                                                           \
            ELF_NAME_LITERAL,                                                                       \
            static_cast<uint32_t>(ENTRY_U32),                                                       \
            static_cast<uint32_t>(CRC32_U32),                                                       \
            APPLY_FN                                                                                \
        };                                                                                          \
        static const ps2_game_overrides::AutoRegister PS2_GAME_OVERRIDE_JOIN(kPs2OverrideReg_, ID)  \
            (PS2_GAME_OVERRIDE_JOIN(kPs2OverrideDesc_, ID));                                        \
    }

#endif // PS2_GAME_OVERRIDES_H
