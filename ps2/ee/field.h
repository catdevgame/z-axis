#pragma once

// A member's address through a game pointer, as the game's C computes it:
// the object's EE address plus the member's offset. An object held through
// an uncached mirror (0x20000000, 0x30000000: a pool with kPoolUncached, the
// loaded textures) keeps its member in the same mirror, which `&ptr->member`
// (a host pointer turned back into an address) would lose.
//
//     const ee::Ptr<char> name = ee::field(texture, &Texture::name);
//
// An array member gives a pointer to its first element.

#include "ee.h"

#include <cstdint>
#include <type_traits>

namespace ee
{
    namespace detail
    {
        template <typename T, typename M>
        inline uint32_t memberOffset(M T::*member)
        {
            alignas(T) static unsigned char storage[sizeof(T)];
            const T *object = reinterpret_cast<const T *>(storage);
            return static_cast<uint32_t>(reinterpret_cast<const unsigned char *>(&(object->*member)) - storage);
        }
    }

    template <typename T, typename M>
    inline auto field(Ptr<T> object, M T::*member)
    {
        using Element = std::conditional_t<std::is_array_v<M>, std::remove_extent_t<M>, M>;
        return Ptr<Element>(object.address + detail::memberOffset(member));
    }
}
