// The park editor's object classes and its themes (were the sweep's
// blocks_001f23c8).
//
// An edit object is one allocation: a part per class from the object's
// own to the root (EditBaseClass has none), each with a header and the
// class's data. A class names its parent, so a part holds the data its
// class adds, as in C++ inheritance; the pointer handed about is the data of
// the object's own class, and the header is just before it.
//
// A theme is a text file ("ThemeDefBegin" .. "ThemeDefEnd" in the park
// project file): the park's names, its files and the tools it offers.
//
// Everything the editor allocates goes through editAllocate (zeroed, and
// counted for a leak check whose report is compiled out).

#include "zge/editor/park_classes.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "aggressive/skater/skater.h"
#include "zge/editor/park_catalog.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/park_classes.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditBaseClassName = sym::g_editBaseClassName; // "EditBaseClass"
        constexpr uint32_t kTextName = sym::g_textName2;
        constexpr uint32_t kTextDescription = sym::g_textDescription;
        constexpr uint32_t kTextMapFileName = sym::g_textMapFileName;
        constexpr uint32_t kTextApronFileName = sym::g_textApronFileName;
        constexpr uint32_t kTextObjectDetailFileName = sym::g_textObjectDetailFileName;
        constexpr uint32_t kTextObjectGeomFileName = sym::g_textObjectGeomFileName;
        constexpr uint32_t kTextUiScript = sym::textUiScript;
        constexpr uint32_t kTextStartScript = sym::g_textStartScript;
        constexpr uint32_t kTextHighResolution = sym::g_textHighResolution;
        constexpr uint32_t kTextLightSystem = sym::g_textLightSystem;
        constexpr uint32_t kTextStartTool = sym::g_textStartTool2;
        constexpr uint32_t kTextToolsListBegin = sym::g_textToolsListBegin;
        constexpr uint32_t kTextToolName = sym::g_textToolName;
        constexpr uint32_t kTextToolsListEnd = sym::g_textToolsListEnd;
        constexpr uint32_t kTextThemeDefEnd = sym::textThemeDefEnd;

        inline ee::Global<uint32_t, sym::g_editAllocationCount> g_editAllocationCount;
        inline ee::Global<uint32_t, sym::g_editAllocatedBytes> g_editAllocatedBytes;
        inline ee::Global<ee::Ptr<EditClassTable>, sym::g_editClasses> g_editClasses;

        ee::Ptr<EditClass> editClassAt(uint32_t index)
        {
            return &(*g_editClasses)->classes[index];
        }

        ee::Ptr<char> classNameOf(ee::Ptr<EditClass> type)
        {
            return type->name;
        }
    }

    void resetEditAllocations()
    {
        *g_editAllocationCount = 0;
        *g_editAllocatedBytes = 0;
    }
    READABLE_FUNCTION(resetEditAllocations, Verified);

    // checkEditAllocations: the leak check after the editor's frees, compiled out.
    void checkEditAllocations() {}
    READABLE_FUNCTION(checkEditAllocations, Verified);

    uint32_t editAllocate(uint32_t bytes)
    {
        const uint32_t block = zge::heapAllocate(bytes);
        zge::memoryFill(block, 0, bytes);
        *g_editAllocationCount += 1;
        *g_editAllocatedBytes += bytes;
        return block;
    }
    READABLE_FUNCTION_WAITS(editAllocate, Verified);

    int32_t editFree(ee::Address block)
    {
        *g_editAllocationCount -= 1;
        return game::tail(zge::heapFree, block);
    }
    READABLE_FUNCTION_WAITS(editFree, Verified);

    uint32_t alignTo16(uint32_t bytes)
    {
        const uint32_t over = bytes & 0xFu;
        if (over != 0)
            return bytes + 16u - over;
        return bytes;
    }
    READABLE_FUNCTION(alignTo16, Verified);

    uint32_t editAllocationCount()
    {
        return *g_editAllocationCount;
    }
    READABLE_FUNCTION(editAllocationCount, Verified);

    namespace
    {
        namespace tail
        {
        uint32_t stringCopy(ee::Ptr<char> dst, uint32_t src) { return ee::tailCall<uint32_t>(sym::stringCopy, dst, src); }
        }
    }

    // initEditClasses: the table, with the root class in slot 0.
    void initEditClasses()
    {
        const ee::Ptr<EditClassTable> table(editAllocate(static_cast<uint32_t>(sizeof(EditClassTable))));
        *g_editClasses = table;
        for (uint32_t i = 0; i < kEditClasses; ++i)
            table->classes[i].used = 0;
        EditClass &root = table->classes[0];
        root.used = 1;
        table->ready = 1;
        root.index = 0;
        root.parent = 0;
        root.live = 0;
        root.partBytes = 0;
        root.objectBytes = 0;
        root.construct = 0;
        root.destruct = 0;
        root.message = 0;
        tail::stringCopy(classNameOf(editClassAt(0)), kEditBaseClassName);
    }
    READABLE_FUNCTION_WAITS(initEditClasses, Verified);

    // freeEditClasses. Its loop over the 32 classes lost its body, a report of
    // the classes still alive: the time stays.
    int32_t freeEditClasses()
    {
        (*g_editClasses)->ready = 0;
        const int32_t result = editFree(*g_editClasses);
        *g_editClasses = ee::Ptr<EditClassTable>(0u);
        return result;
    }
    READABLE_FUNCTION_WAITS(freeEditClasses, Verified);

    uint32_t findEditClass(uint32_t className, ee::Ptr<uint32_t> index);

    uint32_t freeEditClassSlot();

    int32_t inheritEditClass(ee::Address added, ee::Address parent);

    // registerEditClass: a class in the first free slot, under its parent's name.
    // The name asked for first is looked up, its answer dropped.
    int32_t registerEditClass(uint32_t className, uint32_t dataBytes, uint32_t parentName, uint32_t construct, uint32_t destruct,
                              uint32_t message)
    {
        ee::Local<uint32_t> index2;
        ee::Local<uint32_t> index3;
        findEditClass(className, index2);
        findEditClass(parentName, index3);
        const uint32_t slot = freeEditClassSlot();
        *index2 = slot;
        const ee::Ptr<EditClass> addedClass = editClassAt(slot);
        EditClass &added = *addedClass;
        added.live = 0;
        added.index = slot;
        added.partBytes = alignTo16(dataBytes) + sizeof(EditPart);
        zge::stringCopy(classNameOf(addedClass), className);
        added.construct = construct;
        added.destruct = destruct;
        added.message = message;
        added.used = 1;
        return inheritEditClass(addedClass, editClassAt(*index3));
    }
    READABLE_FUNCTION(registerEditClass, Verified);

    int32_t inheritEditClass(ee::Address added, ee::Address parent)
    {
        const ee::Ptr<EditClass> type(added), base(parent);
        type->parent = base->index;
        type->objectBytes = type->partBytes + base->objectBytes;
        return static_cast<int32_t>(type->objectBytes);
    }
    READABLE_FUNCTION(inheritEditClass, Verified);

    void initEditObjectParts(uint32_t object, uint32_t index);

    void constructEditObjectParts(ee::Address partAddress, uint32_t pieceDef);

    int32_t editPartData(ee::Address part);

    // newEditObject: an object of the class; its data. 0 for no such class.
    uint32_t newEditObject(uint32_t className, ee::Address pieceDef)
    {
        ee::Local<uint32_t> index2;
        const int32_t found = static_cast<int32_t>(findEditClass(className, index2));
        const uint32_t index = *index2;
        if (found == 0)
            return 0;
        const ee::Ptr<EditClass> type = editClassAt(index);
        const uint32_t object = editAllocate(type->objectBytes);
        initEditObjectParts(object, *index2);
        constructEditObjectParts(object, pieceDef);
        type->live += 1;
        const uint32_t data = static_cast<uint32_t>(editPartData(object));
        return data;
    }
    READABLE_FUNCTION_WAITS(newEditObject, Verified);

    int32_t editPartHeader(uint32_t data);

    void destructEditObjectParts(ee::Address first);

    int32_t deleteEditObject(ee::Address data)
    {
        const ee::Ptr<EditPart> part(static_cast<uint32_t>(editPartHeader(data)));
        const ee::Ptr<EditPart> object = part->object;
        const ee::Ptr<EditClass> type = object->type;
        destructEditObjectParts(object);
        type->live -= 1;
        return game::tail(editFree, part->object);
    }
    READABLE_FUNCTION_WAITS(deleteEditObject, Verified);

    void sendEditMessage(uint32_t data, ee::Address message)
    {
        const ee::Ptr<EditPart> part(static_cast<uint32_t>(editPartHeader(data)));
        if (part->type->message)
            part->type->message(data, message);
    }
    READABLE_FUNCTION(sendEditMessage, Verified);

    // initEditObjectParts: each part's header, from the class's own up its parents.
    void initEditObjectParts(uint32_t object, uint32_t index)
    {
        uint32_t address = object;
        while (address != 0)
        {
            const ee::Ptr<EditPart> part(address);
            const ee::Ptr<EditClass> type = editClassAt(index);
            part->type = type;
            part->data = static_cast<uint32_t>(editPartData(part));
            part->object = ee::Ptr<EditPart>(object);
            address = type->parent != 0 ? address + type->partBytes : 0;
            part->next = ee::Ptr<EditPart>(address);
            index = type->parent;
        }
    }
    READABLE_FUNCTION(initEditObjectParts, Verified);

    void constructEditObjectParts(ee::Address partAddress, uint32_t pieceDef)
    {
        const ee::Ptr<EditPart> part(partAddress);
        if (part->next)
            constructEditObjectParts(part->next, pieceDef);
        if (part->type->construct)
            part->type->construct(part->data, pieceDef);
    }
    READABLE_FUNCTION_WAITS(constructEditObjectParts, Verified);

    void destructEditObjectParts(ee::Address first)
    {
        for (ee::Ptr<EditPart> part(first); part; part = part->next)
        {
            if (part->type->destruct)
                part->type->destruct(part->data);
        }
    }
    READABLE_FUNCTION(destructEditObjectParts, Verified);

    // editObjectPart: the object's data of the named class, 0 if not one.
    uint32_t editObjectPart(ee::Address data, uint32_t className)
    {
        const ee::Ptr<EditPart> header(static_cast<uint32_t>(editPartHeader(data)));
        for (ee::Ptr<EditPart> part = header->object; part; part = part->next)
        {
            if (zge::compareStrings(className, classNameOf(part->type)) == 0)
                return part->data;
        }
        return 0;
    }
    READABLE_FUNCTION(editObjectPart, Verified);

    int32_t editPartHeader(uint32_t data)
    {
        return static_cast<int32_t>(data - sizeof(EditPart));
    }
    READABLE_FUNCTION(editPartHeader, Verified);

    int32_t editPartData(ee::Address part)
    {
        return static_cast<int32_t>(part + sizeof(EditPart));
    }
    READABLE_FUNCTION(editPartData, Verified);

    // findEditClass: the class of that name: whether there is one, its index
    // to `index`. The root's slot counts.
    uint32_t findEditClass(uint32_t className, ee::Ptr<uint32_t> index)
    {
        for (uint32_t i = 0; i < kEditClasses; ++i)
        {
            const ee::Ptr<EditClass> type = editClassAt(i);
            if (type->used != 0 && zge::compareStrings(className, classNameOf(type)) == 0)
            {
                *index = type->index;
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(findEditClass, Verified);

    // freeEditClassSlot: the first free slot; 0 with none (the root's: not
    // checked by the callers).
    uint32_t freeEditClassSlot()
    {
        const ee::Ptr<EditClassTable> table = *g_editClasses;
        for (uint32_t i = 0;;)
        {
            if (table->classes[i].used == 0)
                return i;
            ++i;
            if (!(i < kEditClasses))
                return 0;
        }
    }
    READABLE_FUNCTION(freeEditClassSlot, Verified);

    ee::Ptr<const char> editObjectClassName(ee::Address data)
    {
        const ee::Ptr<EditPart> part(static_cast<uint32_t>(editPartHeader(data)));
        return part->object->type->name;
    }
    READABLE_FUNCTION(editObjectClassName, Verified);

    int32_t newParkTheme()
    {
        const uint32_t address = editAllocate(static_cast<uint32_t>(sizeof(ParkTheme)));
        const ee::Ptr<ParkTheme> theme(address);
        theme->lightSystem = 1;
        theme->uiScript[0] = 0;
        theme->toolCount = 0;
        theme->flagA28 = 0;
        theme->startScript[0] = 0;
        theme->highResolution = 1;
        return static_cast<int32_t>(address);
    }
    READABLE_FUNCTION_WAITS(newParkTheme, Verified);

    void freeParkTheme(uint32_t theme)
    {
        game::tail(editFree, theme);
    }
    READABLE_FUNCTION_WAITS(freeParkTheme, Verified);

    // readParkThemeDef: a theme from the script being read, to "ThemeDefEnd".
    // Unknown tokens go by. Each token is tried twice over: a name of the
    // first group is not one of the second.
    void readParkThemeDef(uint32_t address)
    {
        ee::Local<uint8_t[0x100]> token;
        ee::Local<int32_t> value;
        ee::Local<int32_t> value2;
        const ee::Ptr<ParkTheme> theme(address);
        const uint32_t uiScript = address + offsetof(ParkTheme, uiScript);
        const uint32_t startScript = address + offsetof(ParkTheme, startScript);
        const uint32_t startTool = address + offsetof(ParkTheme, startTool);
        const uint32_t objectGeomFile = address + offsetof(ParkTheme, objectGeomFile);

        const auto next = [&](uint32_t into, uint32_t bytes) {
            zge::nextToken(into, bytes);
        };
        const auto is = [&](uint32_t name) {
            return zge::stringCompareNoCase(token, name) == 0;
        };

        for (;;)
        {
            next(token.address(), 0x100);

            // The names and the files.
            uint32_t file = 0;
            if (is(kTextName))
                next(address, sizeof(ParkTheme::name));
            else
            {
                if (is(kTextDescription))
                    next(address + offsetof(ParkTheme, description), 0x100);
                else
                {
                    if (is(kTextMapFileName))
                        file = address + offsetof(ParkTheme, mapFile);
                    else
                    {
                        if (is(kTextApronFileName))
                            file = address + offsetof(ParkTheme, apronFile);
                        else
                        {
                            if (is(kTextObjectDetailFileName))
                                file = address + offsetof(ParkTheme, objectDetailFile);
                        }
                    }
                    if (file != 0)
                        next(file, 0x40);
                    else
                    {
                        if (is(kTextObjectGeomFileName))
                            next(objectGeomFile, 0x40);
                    }
                }
            }

            // The scripts, the flags and the tools.
            if (is(kTextUiScript))
                next(uiScript, 0x80);
            else
            {
                if (is(kTextStartScript))
                    next(startScript, 0x400);
                else
                {
                    if (is(kTextHighResolution))
                    {
                        zge::readNumberToken(value);
                        theme->highResolution = *value == 1 ? 1 : 0;
                    }
                    else
                    {
                        if (is(kTextLightSystem))
                        {
                            zge::readNumberToken(value2);
                            theme->lightSystem = *value2 == 1 ? 1 : 0;
                        }
                        else
                        {
                            if (is(kTextStartTool))
                                zge::readNumberToken(startTool);
                            else
                            {
                                if (is(kTextToolsListBegin))
                                {
                                    for (;;)
                                    {
                                        next(token.address(), 0x400);
                                        if (is(kTextToolName))
                                        {
                                            next(address + offsetof(ParkTheme, toolNames) + (theme->toolCount << 5), 0x20);
                                            theme->toolCount += 1;
                                        }
                                        if (is(kTextToolsListEnd))
                                            break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (is(kTextThemeDefEnd))
                return;
        }
    }
    READABLE_FUNCTION_WAITS(readParkThemeDef, Verified);

    int32_t themeMapFileName(uint32_t theme)
    {
        return static_cast<int32_t>(theme + offsetof(ParkTheme, mapFile));
    }
    READABLE_FUNCTION(themeMapFileName, Verified);

    int32_t themeObjectDetailFileName(uint32_t theme)
    {
        return static_cast<int32_t>(theme + offsetof(ParkTheme, objectDetailFile));
    }
    READABLE_FUNCTION(themeObjectDetailFileName, Verified);

    int32_t themeObjectGeomFileName(uint32_t theme)
    {
        return static_cast<int32_t>(theme + offsetof(ParkTheme, objectGeomFile));
    }
    READABLE_FUNCTION(themeObjectGeomFileName, Verified);

    int32_t themeUiScript(uint32_t theme)
    {
        return static_cast<int32_t>(theme + offsetof(ParkTheme, uiScript));
    }
    READABLE_FUNCTION(themeUiScript, Verified);

    int32_t themeStartScript(uint32_t theme)
    {
        return static_cast<int32_t>(theme + offsetof(ParkTheme, startScript));
    }
    READABLE_FUNCTION(themeStartScript, Verified);

    uint32_t themeToolCount(uint32_t theme)
    {
        return ee::Ptr<ParkTheme>(theme)->toolCount;
    }
    READABLE_FUNCTION(themeToolCount, Verified);

    int32_t themeToolName(uint32_t theme, uint32_t index)
    {
        return static_cast<int32_t>(theme + offsetof(ParkTheme, toolNames) + (index << 5));
    }
    READABLE_FUNCTION(themeToolName, Verified);

    uint32_t themeHighResolution(uint32_t theme)
    {
        return ee::Ptr<ParkTheme>(theme)->highResolution;
    }
    READABLE_FUNCTION(themeHighResolution, Verified);
}
