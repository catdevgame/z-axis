#pragma once

// What readable game code is written with beyond plain C++ (docs/READABLE.md).
//
//   ee::Local<T>     a local whose address goes to game code, so it has to
//                    live on the guest stack. Reads like the variable it is.
//   ee::game<R>(..)  the one line behind a typed declaration of a game
//                    function: the arguments by what they are, the routing
//                    (hooks, recording, a function switched off) in ee::call.
//
// A stack offset, a register or a resume point never appears in game code:
// a Local goes where its declaration order puts it in the function's frame,
// and tools/readable_locals.tsv names the place of one the original keeps
// elsewhere (ee::frameLocal).

#include "abi.h"
#include "ee.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace ee
{
    // The next local of the running function, by its place in the frame
    // table, else the next free place in declaration order. abi.cpp.
    uint32_t frameLocal(uint32_t size, uint32_t alignment);

    // Its place is given back at the end of its scope: a Local in a loop has
    // the same place every time round, as a C local does.
    class LocalScope
    {
    protected:
        LocalScope() : m_frame(t_ee.frame), m_count(m_frame ? m_frame->localCount : 0u), m_next(m_frame ? m_frame->localNext : 0u) {}
        ~LocalScope()
        {
            if (m_frame)
            {
                m_frame->localCount = m_count;
                m_frame->localNext = m_next;
            }
        }

    private:
        Frame *m_frame;
        uint32_t m_count, m_next;
    };

    // The bytes a Local of T has in the frame: sizeof(T), or T::frameBytes() where a version's record is shorter
    // than the layout shared code compiles against (the other versions' fields past its end, which that version
    // never reaches: ZGE2's frame holds collision/faces.cpp's NodeSpaceShape up to its sphere).
    template <typename T>
    constexpr uint32_t localBytes()
    {
        if constexpr (requires { T::frameBytes(); })
            return T::frameBytes();
        else
            return sizeof(T);
    }

    template <typename T>
    class Local : LocalScope
    {
    public:
        Local() : m_address(frameLocal(std::integral_constant<uint32_t, localBytes<T>()>::value, alignof(T))) {}
        Local(const Local &) = delete;
        Local &operator=(const Local &) = delete;
        Local &operator=(const T &value)
        {
            **this = value;
            return *this;
        }

        uint32_t address() const { return m_address; }
        T &operator*() const { return *at<T>(m_address); }
        T *operator->() const { return at<T>(m_address); }
        operator T &() const { return **this; }
        // Handed on as its address, as an ee::Ptr to it is: to a T * or an ee::Ptr<T> parameter.
        operator T *() const { return at<T>(m_address); }
        operator Ptr<T>() const { return Ptr<T>(m_address); }
        operator Ptr<const T>() const { return Ptr<const T>(m_address); }

    private:
        uint32_t m_address;
    };

    // A character buffer: hands itself to game code as its first character.
    template <typename T, size_t N>
    class Local<T[N]> : LocalScope
    {
    public:
        Local() : m_address(frameLocal(N * sizeof(T), 16u)) {}
        Local(const Local &) = delete;
        Local &operator=(const Local &) = delete;
        uint32_t address() const { return m_address; }
        operator Ptr<T>() const { return Ptr<T>(m_address); }
        operator Ptr<const T>() const { return Ptr<const T>(m_address); }
        operator T *() const { return at<T>(m_address); }
        T &operator[](size_t i) const { return Ptr<T>(m_address)[i]; }
        T &operator*() const { return *at<T>(m_address); }

    private:
        uint32_t m_address;
    };

    // A local the original never initialises (a bug the game keeps): its
    // compiler kept it in a register the function's caller gets back
    // unchanged, so what it holds is what the caller left there. Game code
    // names it for what it is and reads it like the value:
    //
    //     const ee::LeftByCaller<int32_t> flags; // the bit-fields the original never sets
    //
    // Its value is the one at the function's entry. Where the original keeps
    // it is never in game code: tools/readable_entry_reads.tsv names the
    // register for each function, and the build's copy of a plain source
    // (tools/readable_entries.py) gives each declaration in a registered
    // function's body its row's, in order, as `Register`, which a source never
    // writes. In the game the value is the caller's (a readable caller's call
    // made directly: what it published for its callee, Frame::view; else the
    // context, as the caller left it); under zx-fncheck the context is the
    // recording's registers at entry, so the replay hands the function what
    // the original's caller left.
    template <typename T, int Register = -1>
    class LeftByCaller
    {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T> || std::is_same_v<T, float>,
                      "a word or a float, as the register holds it");

    public:
        LeftByCaller() : m_value(read()) {}
        LeftByCaller(const LeftByCaller &) = delete;
        LeftByCaller &operator=(const LeftByCaller &) = delete;
        operator T() const { return m_value; }

    private:
        static T read()
        {
            if constexpr (Register < 0)
            {
                std::fprintf(stderr, "ee::LeftByCaller: no place: declare it in a registered function of a plain source "
                                     "(tools/readable_entries.py gives it its register from readable_entry_reads.tsv)\n");
                std::abort();
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                static_assert(Register >= 32 && savedSlot(Register) >= 0, "a callee-saved float register");
                return callerFloat(Register - 32);
            }
            else
            {
                static_assert(Register < 32 && savedSlot(Register) >= 0, "a callee-saved register");
                return Abi<T>::fromReg(callerRegister(Register));
            }
        }

        T m_value;
    };

    namespace detail
    {
        uint32_t guestAddress(const void *pointer);
    }

    // What a typed declaration takes where its callers hand over different
    // things for one address: a Ptr, a Local, a number. ee::game passes it on
    // as what it is, the address.
    struct Address
    {
        uint32_t address = 0u;
        // 0: what a function returning an address has before it returns (game::detail::Route's self-call).
        Address() = default;
        template <typename I>
            requires std::is_integral_v<I>
        Address(I value) : address(static_cast<uint32_t>(value)) {}
        template <typename T>
        Address(Ptr<T> pointer) : address(pointer.address) {}
        template <typename T>
        Address(const Local<T> &local) : address(local.address()) {}
        Address(const void *pointer) : address(detail::guestAddress(pointer)) {}
        // The word it is, as a table of words holds it (a PointerArray's items).
        operator uint32_t() const { return address; }
    };

    template <>
    struct Abi<Address, void>
    {
        static constexpr bool isFloat = false;
        static uint64_t toReg(Address a) { return Abi<uint32_t>::toReg(a.address); }
        // A readable function's parameter (pointerArrayAdd's item: any record's address).
        static Address fromReg(uint64_t r) { return Address(static_cast<uint32_t>(r)); }
    };

    // One word of a variadic function's `...` (formatString's words after its format). The original's caller sets
    // as many argument registers as the format reads, one word each (an int, a pointer, a double's 64 bits); the
    // function spills its registers to its frame and vsprintf reads as far as the format asks. The function is
    // declared with its words defaulted, `ee::VarArg first = {}`, so a call names the words its format reads and
    // no more: only those are passed (detail::pushArg), so the replay compares those and a hook or a function
    // switched off gets the rest of the registers as the call left them. A word the call leaves out reads 0 in
    // the body.
    class VarArg
    {
    public:
        VarArg() = default;
        template <typename T>
            requires(!std::is_same_v<std::remove_cvref_t<T>, VarArg>)
        VarArg(const T &value)
            : m_word(wordOf(value)), m_named(true), m_bytes(std::is_arithmetic_v<T> && sizeof(T) == 8u ? 8u : 4u)
        {
        }
        // A register as the table's entry reads it: every word is there.
        static VarArg fromRegister(uint64_t word)
        {
            VarArg named;
            named.m_word = word;
            named.m_named = true;
            named.m_bytes = 8u;
            return named;
        }

        // The word as the register holds it.
        operator uint64_t() const { return m_word; }
        // Not named by the call, so not passed.
        bool leftOut() const { return !m_named; }
        // What the original's caller stores of it where the registers are taken (the seventh word on of
        // formatString): sw for an int or an address, sd for a 64-bit word (a double's bits).
        uint32_t stackBytes() const { return m_bytes; }

    private:
        template <typename T>
        static uint64_t wordOf(const T &value)
        {
            if constexpr (requires { value.address(); }) // an ee::Local
                return Abi<uint32_t>::toReg(value.address());
            else if constexpr (requires { value.address; }) // an ee::Ptr or an ee::Address
                return Abi<uint32_t>::toReg(value.address);
            else if constexpr (std::is_pointer_v<T> || std::is_array_v<T>) // an array: a text in a record, its address
                return Abi<uint32_t>::toReg(detail::guestAddress(value));
            else
            {
                static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                              "a variadic word is an integer or an address: a float goes as its double's bits (fptodp)");
                return Abi<T>::toReg(value);
            }
        }

        uint64_t m_word = 0;
        bool m_named = false;
        uint8_t m_bytes = 4u;
    };

    template <>
    struct Abi<VarArg, void>
    {
        static constexpr bool isFloat = false;
        static uint64_t toReg(VarArg word) { return word; }
        static VarArg fromReg(uint64_t r) { return VarArg::fromRegister(r); }
    };

    // The words of a variadic call past the ones its registers hold (formatString's seventh on): the original's
    // caller stores them in its own frame's lowest slots, 8 bytes apart, where the callee's sp will be, and
    // vsprintf walks on to them from the words the callee spilled. Called by the variadic function's inline for
    // the long call (zge/text/cstring.h), in the caller, before the call; game code passes the words plainly.
    template <typename... Words>
    void storeVariadicStackWords(const Words &...words)
    {
        const CallRegs call; // (no target: the words are stored before the call is made)
        int index = 0;
        const auto store = [&](const VarArg &word)
        {
            if (!word.leftOut())
                detail::storeStackWord(call, index++, word, word.stackBytes());
        };
        (store(VarArg(words)), ...);
    }

    namespace detail
    {
        // A host object handed to game code has no guest address: stop here
        // rather than hand on a wild pointer.
        inline uint32_t guestAddress(const void *pointer)
        {
            const auto *byte = static_cast<const uint8_t *>(pointer);
            if (pointer && !inGuestMap(byte))
            {
                std::fprintf(stderr, "[readable] a host object was handed to game code\n");
                std::abort();
            }
            return addressOf(pointer);
        }

        template <typename T>
        auto passArgument(T &&value)
        {
            using V = std::decay_t<T>;
            if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V> || std::is_same_v<V, Float> || std::is_same_v<V, Qword>)
                return value;
            else if constexpr (std::is_pointer_v<V>)
                return guestAddress(value);
            else if constexpr (requires { value.address; })
                return value.address; // ee::Ptr
            else if constexpr (requires { value.address(); })
                return value.address(); // ee::Local
            else
                return guestAddress(&value); // an object in game memory
        }
    }

    // A call of the game function at `target`: references and pointers go as
    // the addresses they are.
    template <typename R = void, typename... A>
    R game(uint32_t target, A &&...args)
    {
        return call<R>(target, detail::passArgument(std::forward<A>(args))...);
    }
}
