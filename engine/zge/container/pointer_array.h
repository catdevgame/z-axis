// The pointer array's functions that array.h does not declare yet (their bodies are container/array.cpp's): files that
// still call them by their address keep helpers of the same names, which these declarations would clash with
// (cursor_array.h is newCursorArray's). A file that calls them by name includes this.
#pragma once
#include "zge/container/array.h"
#include <cstdint>

namespace zge
{
    // A new array for capacity items, the items zero, the array from the pool and the items from the sized heap.
    ee::Ptr<PointerArray> newPointerArray(uint32_t capacity);
    // The items and the array freed, each where it came from (whoever holds a reference has been told already).
    void freePointerArray(ee::Ptr<PointerArray> array);
    // The item at the cursor, 0 outside a walk.
    uint32_t current(ee::Ptr<PointerArray> array);
    // The item at index, 0 past the end.
    uint32_t pointerArrayAt(ee::Ptr<PointerArray> array, int32_t index);
    // One more holder of the array: the references field as stored (ZGE2: the references word itself).
    uint32_t addArrayReference(ee::Ptr<PointerArray> array);
    // The items in the array.
    uint32_t pointerArrayCount(ee::Ptr<PointerArray> array);
    // The items the array has room for.
    uint32_t cursorArrayCapacity(ee::Ptr<PointerArray> array);
    // The item at index taken out, the last item moved into its place, the cursor kept inside the array. Returns the
    // item, 0 if index is past the end.
    uint32_t pointerArrayRemoveAt(ee::Ptr<PointerArray> array, int32_t index);
}
