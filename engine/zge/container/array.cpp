#include "array.h"
#include "cursor_array.h"
#include "pointer_array.h"

#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "sym/zge/container/array.cpp.h"

namespace zge
{
    int32_t pointerArrayFind(ee::Ptr<PointerArray> array, ee::Address item)
    {
        const int32_t count = array->count;
        if (count == 0)
            return -1;
        const ee::Ptr<uint32_t> items = array->items;
        for (int32_t i = 0;; ++i)
        {
            if (items[i] == item)
                return i;
            if (i + 1 >= count)
                return -1;
        }
    }
    READABLE_FUNCTION(pointerArrayFind, Verified);

    namespace
    {
        // The array doubled when full, the new half cleared (pointerArrayAdd's
        // and pointerArrayPushBack's start); the count as it is after.
        uint32_t growIfFull(ee::Ptr<PointerArray> array)
        {
            uint32_t count = array->count;
            if (count == array->capacity)
            {
                const uint32_t grown = reallocate(array->items, static_cast<uint32_t>(array->capacity) << 3);
                const uint32_t oldBytes = static_cast<uint32_t>(array->capacity) << 2;
                array->items = ee::Ptr<uint32_t>(grown);
                memoryFill(grown + oldBytes, 0, oldBytes);
                count = array->count;
                array->capacity = static_cast<ArrayCount>(array->capacity << 1);
            }
            return count;
        }
    }

    void pointerArrayAdd(ee::Ptr<PointerArray> array, ee::Address item)
    {
        const uint32_t count = growIfFull(array);
        if (array->cursor == -1)
        {
            array->count = static_cast<ArrayCount>(count + 1);
            array->items[count] = item;
            return;
        }
        const ee::Ptr<uint32_t> items = array->items;
        array->count = static_cast<ArrayCount>(count + 1);
        items[count] = items[array->cursor];
        array->items[array->cursor] = item;
    }
    READABLE_FUNCTION_WAITS(pointerArrayAdd, Verified);

    void pointerArrayPushBack(ee::Ptr<PointerArray> array, ee::Address item)
    {
        const uint32_t count = growIfFull(array);
        const ee::Ptr<uint32_t> items = array->items;
        array->count = static_cast<ArrayCount>(count + 1);
        items[count] = item;
    }
    READABLE_FUNCTION_WAITS(pointerArrayPushBack, Verified);

    uint32_t pointerArrayRemoveCurrent(ee::Ptr<PointerArray> array)
    {
        const int32_t cursor = array->cursor;
        if (cursor == -1)
        {
            const ee::Ptr<uint32_t> items = array->items;
            array->count = static_cast<ArrayCount>(array->count - 1);
            const uint32_t item = items[array->count];
            items[array->count] = 0;
            return item;
        }
        const ee::Ptr<uint32_t> items = array->items;
        const ee::Ptr<uint32_t> slot = items + cursor;
        const ArrayCount last = static_cast<ArrayCount>(array->count - 1);
        const uint32_t item = *slot;
        array->count = last;
        *slot = items[last];
        array->items[array->count] = 0;
        if (array->cursor >= static_cast<int32_t>(array->count))
            array->cursor = static_cast<int32_t>(array->count) - 1;
        return item;
    }
    READABLE_FUNCTION(pointerArrayRemoveCurrent, Verified);

    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_searchTreePool> g_searchTreePool;
        constexpr uint32_t kPoolFree = sym::poolFree;
        constexpr uint32_t kHeapFree = sym::heapFree;

        // A visit of forEachCursorEntry: 0 stops the walk.
        using ArrayVisit = ee::FunctionPtr<int32_t(uint32_t item, uint32_t context)>;

        // The holders of an array: ZGE2 keeps them in a word of their own, ZGE3 and ZGE4 in a signed 13-bit
        // field of the sharing word. (Templates on the record: the other version's field is not in it.)

        // What every array starts with: one reference, no items, no walk.
        template <typename Array>
        void startEmpty(Array &array, uint32_t capacity)
        {
            if constexpr (game::readable::kIsDm2)
                array.references = 1;
            else
                array.sharing = (array.sharing & ~kArrayReferences) | (1u << kArrayReferenceShift);
            array.capacity = static_cast<ArrayCount>(capacity);
            array.count = 0;
            array.cursor = -1;
        }

        uint32_t withItemBytes(uint32_t sharing, uint32_t bytes)
        {
            return (sharing & ~kArrayItemBytes) | (bytes & kArrayItemBytes);
        }

        // The references field as it reads: a signed 13 bits.
        int32_t referencesOf(uint32_t sharing)
        {
            return static_cast<int32_t>(sharing << 3) >> 19;
        }

        // One more holder: the references field as stored (ZGE2: the references word itself).
        template <typename Array>
        uint32_t addHolder(Array &array)
        {
            if constexpr (game::readable::kIsDm2)
                return static_cast<uint32_t>(++array.references);
            else
            {
                const uint32_t references = static_cast<uint32_t>((referencesOf(array.sharing) + 1) << kArrayReferenceShift) & kArrayReferences;
                array.sharing = (array.sharing & ~kArrayReferences) | references;
                return references;
            }
        }

        // One holder fewer: the references left.
        template <typename Array>
        int32_t releaseHolder(Array &array)
        {
            if constexpr (game::readable::kIsDm2)
                return --array.references;
            else
            {
                const int32_t references = referencesOf(array.sharing) - 1;
                array.sharing = (array.sharing & ~kArrayReferences) | ((static_cast<uint32_t>(references) << kArrayReferenceShift) & kArrayReferences);
                return static_cast<int32_t>(static_cast<uint32_t>(references) << 19) >> 19;
            }
        }

        namespace tail
        {
            void poolFree(ee::Ptr<Pool> pool, ee::Ptr<PointerArray> array) { ee::tailCall<void>(kPoolFree, pool, array); }
            void heapFree(ee::Ptr<PointerArray> array) { ee::tailCall<void>(kHeapFree, array); }
        }
    }

    namespace
    {
        // The constructors and the free, on the record each engine version has (templates: the other version's
        // fields are not in it). ZGE2 has one constructor, newCursorArray: record and items from the heap.

        // A new array for capacity items, the items zero, from the arrays' pool
        // once that exists and from the heap before.
        template <typename Array>
        ee::Ptr<Array> makeCursorArray(uint32_t capacity)
        {
            ee::Ptr<Array> array;
            if constexpr (game::readable::kIsDm2)
                array = ee::Ptr<Array>(heapAllocate(sizeof(Array)));
            else if (*g_searchTreePool)
            {
                array = ee::Ptr<Array>(poolAlloc(*g_searchTreePool));
                array->sharing |= kArrayPooled;
            }
            else
            {
                array = ee::Ptr<Array>(heapAllocate(sizeof(Array)));
                array->sharing &= ~kArrayPooled;
            }
            const uint32_t bytes = capacity << 2;
            startEmpty(*array, capacity);

            const ee::Ptr<uint32_t> items(heapAllocate(bytes));
            if constexpr (!game::readable::kIsDm2)
                array->sharing = withItemBytes(array->sharing & ~(kArrayFromNewPointerArray | kArrayItemsSized), bytes);
            array->items = items;
            memoryFill(items, 0, bytes);
            return array;
        }

        // A new array for capacity items, the items zero, the array from the
        // pool and the items from the sized heap (ZGE3 and ZGE4; ZGE2's one
        // constructor stands at this address).
        template <typename Array>
        ee::Ptr<Array> makePointerArray(uint32_t capacity)
        {
            if constexpr (game::readable::kIsDm2)
                return makeCursorArray<Array>(capacity);
            else
            {
                const uint32_t bytes = capacity << 2;
                const ee::Ptr<Array> array(poolAlloc(*g_searchTreePool));
                array->sharing |= kArrayPooled;
                startEmpty(*array, capacity);

                const ee::Ptr<uint32_t> items(allocateSized(bytes));
                array->items = items;
                array->sharing = withItemBytes(array->sharing | kArrayFromNewPointerArray | kArrayItemsSized, bytes);
                memoryFill(items, 0, bytes);
                return array;
            }
        }

        // Frees the items and the array, each where it came from (ZGE2: both
        // from the heap).
        template <typename Array>
        void releaseArray(ee::Ptr<Array> array)
        {
            if constexpr (game::readable::kIsDm2)
            {
                heapFree(array->items);
                tail::heapFree(array);
            }
            else
            {
                if (array->sharing & kArrayItemsSized)
                    freeSized(array->items, array->sharing & kArrayItemBytes);
                else
                    heapFree(array->items);
                if (array->sharing & kArrayPooled)
                    tail::poolFree(*g_searchTreePool, array);
                else
                    tail::heapFree(array);
            }
        }
    }

    // A new array for capacity items, the items zero, from the arrays' pool
    // once that exists and from the heap before (makeCursorArray).
    ee::Ptr<PointerArray> newCursorArray(uint32_t capacity)
    {
        return makeCursorArray<PointerArray>(capacity);
    }
    READABLE_FUNCTION_WAITS(newCursorArray, Verified);

    // A new array for capacity items, the items zero, the array from the
    // pool and the items from the sized heap (makePointerArray).
    ee::Ptr<PointerArray> newPointerArray(uint32_t capacity)
    {
        return makePointerArray<PointerArray>(capacity);
    }
    READABLE_FUNCTION_WAITS(newPointerArray, Verified);

    // Frees the items and the array, each where it came from. Whoever holds a
    // reference has already been told: this does not look at the count.
    void freePointerArray(ee::Ptr<PointerArray> array)
    {
        releaseArray(array);
    }
    READABLE_FUNCTION_WAITS(freePointerArray, Verified);

    // forEachCursorEntry: every item handed to visit with context, in order,
    // until one returns 0; nothing without a visit. The count is read once,
    // before the walk (the cursor is not used).
    void forEachCursorEntry(ee::Ptr<PointerArray> array, ArrayVisit visit, uint32_t context)
    {
        const int32_t count = array->count;
        if (!visit)
            return;
        for (int32_t i = 0; i < count; ++i)
            if (visit(array->items[i], context) == 0)
                return;
    }
    READABLE_FUNCTION_WAITS(forEachCursorEntry, Verified);

    // Empties the array, handing the items to visit first. (ZGE4 calls the walk only with a visit and zeroes only the
    // first item; ZGE2 and ZGE3 zero them all.)
    void clearCursorArray(ee::Ptr<PointerArray> array, ArrayVisit visit, uint32_t context)
    {
        if constexpr (game::readable::kIsBmx)
        {
            if (visit)
                forEachCursorEntry(array, visit, context);
            array->items[0] = 0;
        }
        else
        {
            forEachCursorEntry(array, visit, context);
            memoryFill(array->items, 0, static_cast<uint32_t>(array->capacity) << 2);
        }
        array->count = 0;
        array->cursor = -1;
    }
    READABLE_FUNCTION_WAITS(clearCursorArray, Verified);

    // The item at the cursor, 0 outside a walk.
    uint32_t current(ee::Ptr<PointerArray> array)
    {
        const int32_t cursor = array->cursor;
        return cursor >= 0 ? array->items[cursor] : 0u;
    }
    READABLE_FUNCTION(current, Verified);

    // The item at index, 0 past the end.
    uint32_t pointerArrayAt(ee::Ptr<PointerArray> array, int32_t index)
    {
        return index < static_cast<int32_t>(array->count) ? array->items[index] : 0u;
    }
    READABLE_FUNCTION(pointerArrayAt, Verified);

    uint32_t pointerArrayCount(ee::Ptr<PointerArray> array)
    {
        return array->count;
    }
    READABLE_FUNCTION(pointerArrayCount, Verified);

    uint32_t cursorArrayCapacity(ee::Ptr<PointerArray> array)
    {
        return array->capacity;
    }
    READABLE_FUNCTION(cursorArrayCapacity, Verified);

    // The item at index taken out, the last item moved into its place, the
    // cursor kept inside the array. Returns the item, 0 if index is past the end.
    uint32_t pointerArrayRemoveAt(ee::Ptr<PointerArray> array, int32_t index)
    {
        const int32_t count = array->count;
        if (index >= count)
            return 0;
        const ee::Ptr<uint32_t> items = array->items;
        const uint32_t item = items[index];
        array->count = static_cast<ArrayCount>(count - 1);
        items[index] = items[array->count];
        array->items[array->count] = 0;
        if (array->cursor >= static_cast<int32_t>(array->count))
            array->cursor = static_cast<int32_t>(array->count) - 1;
        return item;
    }
    READABLE_FUNCTION(pointerArrayRemoveAt, Verified);

    // The first item equal to item taken out (see pointerArrayRemoveAt). Kept
    // bug: an item not there is index -1, which passes the signed test against
    // the count, so the word just before the items is returned as the item,
    // the last item is written over it, the last slot is zeroed and the count
    // drops by one.
    uint32_t pointerArrayRemove(ee::Ptr<PointerArray> array, ee::Address item)
    {
        return game::tail(pointerArrayRemoveAt, array, pointerArrayFind(array, item));
    }
    READABLE_FUNCTION(pointerArrayRemove, Verified);

    // One more holder of the array: the references field as stored (ZGE2: the
    // references word itself).
    uint32_t addArrayReference(ee::Ptr<PointerArray> array)
    {
        return addHolder(*array);
    }
    READABLE_FUNCTION(addArrayReference, Verified);

    // One holder fewer: the references left.
    int32_t releaseArrayReference(ee::Ptr<PointerArray> array)
    {
        return releaseHolder(*array);
    }
    READABLE_FUNCTION(releaseArrayReference, Verified);

    // The start of a walk over the array, as walkFirst in array.h; the moved
    // copy of it the effects use is firstCursorItem.
    uint32_t arrayFirst(ee::Ptr<PointerArray> array)
    {
        return walkFirst(*array);
    }
    READABLE_FUNCTION(arrayFirst, Verified);

    uint32_t firstCursorItem(ee::Ptr<PointerArray> array)
    {
        return walkFirst(*array);
    }
    READABLE_FUNCTION(firstCursorItem, Verified);

    // The next item of the walk, 0 at the end of it.
    uint32_t arrayNext(ee::Ptr<PointerArray> array)
    {
        uint32_t item;
        walkNext(*array, item);
        return item;
    }
    READABLE_FUNCTION(arrayNext, Verified);

}
