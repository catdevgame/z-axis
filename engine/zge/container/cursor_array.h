// newCursorArray's declaration on its own (its body is container/array.cpp's). array.h does not carry it: the files
// that still call the original by its address keep a helper of the same name, which this declaration would clash with.
#pragma once
#include "zge/container/array.h"
#include <cstdint>

namespace zge
{
    // A new array for capacity items, the items zero, from the arrays' pool once that exists and from the heap before.
    ee::Ptr<PointerArray> newCursorArray(uint32_t capacity);

    // The typed view of a new array, for a list kept as one with its own item type (zge/scene/query.h's NodeList, the
    // record's first fields): newCursorArray itself, the array as the caller's Array (the same address; 0 stays 0).
    template <typename Array>
    ee::Ptr<Array> newCursorArrayAs(uint32_t capacity)
    {
        return newCursorArray(capacity).template cast<Array>();
    }
}
