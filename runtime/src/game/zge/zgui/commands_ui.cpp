// zgui's script commands on the UI itself (script.h): input locks, focus,
// a widget refreshed or destroyed, a playing animation aborted, a host
// function called, and UI files and libraries loaded (LOADZSC, LOADLIB,
// LOADMODEL, LOADIMAGE, CANCELLOADLIB, REPORTLIB). Commands that act on the
// UI tree send its root (or a widget) a message (widget.h Message).

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/tree.h"
#include "zge/memory/heaps.h"
#include "zge/scene/message.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include "sym/zge/zgui/commands_ui.cpp.h"

namespace zge::zgui
{
    namespace
    {
        // A buffer the command wrote, read as text.
        ee::Ptr<const char> in(ee::Ptr<char> text) { return text.cast<const char>(); }

        constexpr uint32_t kMemset = sym::memset; // libc memset (dst, byte, bytes)

        // The UI files' loaders (zgui_1, the UI libraries' module).

        // The game's functions the scripts' CALL reaches (a search tree by
        // name hash of HostFunction entries, script.h).
        constexpr uint32_t kHostFunctions = sym::hostFunctions;
        // ZGE2's host function entry: the name's hash a second time where
        // ZGE3 keeps a copy of the name.
        struct HashedHostFunction
        {
            zge::SearchTreeNode links; // +0x00 in the host functions' tree
            uint32_t hash;             // +0x08 its key
            uint32_t nameHash;         // +0x0C the same hash, taken again
            ee::Callback<void()> function; // +0x10 called with nothing (CALL)
        };
        static_assert(sizeof(HashedHostFunction) == 0x14);
        constexpr uint32_t kZguiGlobals = sym::g_zgui;      // (the register g_heap is read through)

        // The commands' token buffers (0x100 bytes each).
        constexpr uint32_t kTokenBuffer = sym::tokenBuffer;
        constexpr uint32_t kArgumentBuffer = sym::argumentBuffer;
        constexpr uint32_t kBufferHigh = ee::high(sym::tokenBuffer); // their high half (a register the loaders keep)

        // Messages.
        constexpr uint32_t kActionNotFound = sym::actionNotFound;      // "zgui: (Action not Found: '%s')"
        constexpr uint32_t kHostFunctionNotFound = sym::hostFunctionNotFound; // "zgui: (Host function not Found: '%s')"
        constexpr uint32_t kComponentNotFound = sym::componentNotFound;   // "zgui: (Component not found: '%s')"
        constexpr uint32_t kRefreshNotFound = sym::refreshNotFound;     // "zgui: (Component not found: %s)"
        constexpr uint32_t kDestroyNotFound = sym::destroyNotFound;     // "zguiScript_Destroy: Component not found: %s\n"

        uint32_t message(Message m) { return static_cast<uint32_t>(m); }
        ee::Ptr<Node> scriptOwner() { return (*g_script)->self; }
    }

    void commandUseStr(ee::Ptr<char>)
    {
        // USE_STR: nothing (a leftover).
    }
    // Draft: no UI script of the nine releases uses USE_STR (never recorded).
    READABLE_FUNCTION(commandUseStr, Draft);

    void commandReportLib(ee::Ptr<char>)
    {
        // REPORTLIB: the loaded UI libraries listed (a debug print).
        game::tail(reportUiLibraries); // the original's tail call
    }
    READABLE_FUNCTION(commandReportLib, Verified);

    void commandLockInput(ee::Ptr<char> arguments)
    {
        // LOCKINPUT a [b]: the root told to lock its input (a | b << 2).
        ee::Local<int32_t> first;
        ee::Local<int32_t> second;
        *second = 0;
        const ee::Ptr<const char> rest = parseInt(arguments, first);
        parseInt(rest, second);
        if constexpr (game::readable::kIsDm2)
            zge::sendNewMessage(*g_scriptOwner, *g_root, message(Message::LockInput), static_cast<uint32_t>(*first | *second << 2), zge::kMessageNow);
        else
            sendMessage(*g_root, message(Message::LockInput), static_cast<uint32_t>(*first | *second << 2));
    }
    READABLE_FUNCTION_WAITS(commandLockInput, Verified);

    void commandUnlockInput(ee::Ptr<char> arguments)
    {
        // UNLOCKINPUT a.
        ee::Local<int32_t> value;
        parseInt(arguments, value);
        if constexpr (game::readable::kIsDm2)
            zge::sendNewMessage(*g_scriptOwner, *g_root, message(Message::UnlockInput), static_cast<uint32_t>(*value), zge::kMessageNow);
        else
            sendMessage(*g_root, message(Message::UnlockInput), static_cast<uint32_t>(*value));
    }
    READABLE_FUNCTION_WAITS(commandUnlockInput, Verified);

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    void commandAbort(ee::Ptr<char> arguments)
    {
        // ABORT @ [target]: the script's animation (its argument) stopped
        // on the target widget (its parent without one).
        ee::Local<char> name;
        ee::Local<char> targetName;
        ee::Local<ee::Ptr<Node>> animation;
        ee::Local<ee::Ptr<Node>> target;
        *animation = 0;
        *target = 0;
        memset(ee::Ptr<char>(targetName.address()), 0, 255);
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(name.address())));
        readToken(rest, 0, ee::Ptr<char>(targetName.address()));
        if (resolvePath(in(ee::Ptr<char>(name.address())), ee::Ptr<char>(0u), ee::Ptr<char>(0u), animation) != kPathArgument)
        {
            report(kActionNotFound, name);
            return;
        }
        bool targetFound = false;
        if (*targetName != 0)
            targetFound = resolvePath(in(ee::Ptr<char>(targetName.address())), ee::Ptr<char>(0u), ee::Ptr<char>(0u), target) ==
                          static_cast<int32_t>(NodeKind::Widget);
        if (!targetFound)
            *target = (*animation)->parent;
        stopComponentScript(*animation, *target);
    }
    READABLE_FUNCTION_WAITS(commandAbort, Verified);

    void commandLoadZsc(ee::Ptr<char> arguments)
    {
        // LOADZSC file [argument]: a UI script file loaded under the root
        // ("UILIB:name" runs a library's script instead).
        *ee::Ptr<char>(kTokenBuffer) = 0;
        *ee::Ptr<char>(kArgumentBuffer) = 0;
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(kTokenBuffer)));
        readToken(rest, 0, ee::Ptr<char>(kArgumentBuffer));
        zge::stringToUpper(kTokenBuffer);
        const ee::Ptr<char> file(kTokenBuffer);
        if (file[0] == 'U' && file[1] == 'I' && file[2] == 'L' && file[3] == file[1] && file[4] == 'B' && file[5] == ':')
        {
            game::tail(loadZguiScript, kTokenBuffer + 6u, *g_root); // the original's tail call
            return;
        }
        game::tail(loadZguiScriptNode, kTokenBuffer, *g_root); // the original's tail call
    }
    // Draft: no UI script of the nine releases uses LOADZSC (never recorded).
    READABLE_FUNCTION_WAITS_OWN(commandLoadZsc, Verified, dm2, commandLoadZscTokens);

    void commandCall(ee::Ptr<char> arguments)
    {
        // CALL function [widget or variable ...]: the game's host function
        // of that name called with the nodes the arguments name (a list
        // ended by 0; names that are neither are left out).
        ee::Local<char> token;
        ee::Local<char> name;
        ee::Local<ee::Ptr<Node>> nodes;
        ee::Local<uint32_t> hash;
        memset(ee::Ptr<uint32_t>(nodes.address()), 0, 0x100);
        ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(name.address())));
        *hash = zge::nameHash(ee::Ptr<char>(name.address()).get());
        const ee::Ptr<HostFunction> function = zge::searchTreeEntry<HostFunction>(zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kHostFunctions), hash));
        if (!function)
        {
            report(kHostFunctionNotFound, name);
            return;
        }
        if (*rest != 0)
        {
            if (*rest != '\r')
            {
                ee::Ptr<ee::Ptr<Node>> next = nodes;
                for (;;)
                {
                    rest = ee::Ptr<char>(readToken(rest, 0, ee::Ptr<char>(token.address())));
                    if (*token != 0)
                    {
                        const int32_t kind = resolvePath(in(ee::Ptr<char>(token.address())), ee::Ptr<char>(0u), ee::Ptr<char>(0u), next);
                        if (kind == static_cast<int32_t>(NodeKind::Widget) || kind == static_cast<int32_t>(NodeKind::Variable))
                        {
                            ++next;
                            *next = 0;
                        }
                    }
                    if (*rest == 0)
                    {
                        function->function(nodes);
                        return;
                    }
                    if (*rest == '\r')
                        break;
                }
            }
        }
        function->function(nodes);
    }
    READABLE_FUNCTION_WAITS_OWN(commandCall, Verified, dm2, commandCallByName);

    // commandCallByName: ZGE2's CALL function: the game's host function of
    // that name called, with no arguments (nothing if there is none).
    void commandCallByName(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> name;
        ee::Local<int32_t> key;
        readToken(arguments, 0, name);
        *key = zge::nameHash(ee::Ptr<const char>(name.address()));
        const ee::Ptr<HashedHostFunction> function =
            zge::searchTreeEntry<HashedHostFunction>(zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kHostFunctions), key));
        if (function)
            function->function();
    }

    namespace
    {
        // SETFOCUS and DESTROY: the widget named (the script's own without
        // a name) given to the root with `m`, else reported.
        void toRootAboutWidget(ee::Ptr<char> arguments, Message m, uint32_t notFound)
        {
            ee::Local<char[0x100]> name;
            ee::Local<ee::Ptr<Node>> widget;
            memset(name, 0, 255);
            readToken(arguments, 0, name);
            if (name[0] == 0)
            {
                sendMessage(*g_root, message(m), scriptOwner());
                return;
            }
            if (resolvePath(in(name), ee::Ptr<char>(), ee::Ptr<char>(), widget) != static_cast<int32_t>(NodeKind::Widget))
            {
                report(notFound, name);
                return;
            }
            sendMessage(*g_root, message(m), *widget);
        }
    }

    void commandSetFocus(ee::Ptr<char> arguments)
    {
        // SETFOCUS [widget]: the input's focus to the widget.
        toRootAboutWidget(arguments, Message::SetFocus, kComponentNotFound);
    }
    READABLE_FUNCTION_WAITS_OWN(commandSetFocus, Verified, dm2, commandSetFocusAtPath);

    namespace
    {
        // The widget object hanging from `parent` called `name` (no case), or 0.
        ee::Ptr<zge::SceneObject> childWidgetNamed(ee::Ptr<zge::SceneObject> parent, ee::Ptr<const char> name)
        {
            for (ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(parent); child; child = zge::nextAttachedSibling(child))
            {
                if (zge::stringCompareNoCase(widgetRecord(child)->name, name) == 0)
                    return child;
            }
            return {};
        }
    }

    namespace
    {
        // ZGE2's SETFOCUS and DESTROY [widget]: the message sent to the root
        // for the widget the path names (its last component a child of the
        // widget the path leads to), or for the script's owner without a
        // path. Nothing when there is no such child.
        void toRootAboutChild(ee::Ptr<char> arguments, Message about, uint32_t sendFlags)
        {
            ee::Local<char[0x100]> path;
            ee::Local<char[0x100]> name;
            ee::Local<char[0x100]> typeName;
            ee::Local<ee::Address> found;
            fillBytes(path, 0, 0xFF);
            fillBytes(name, 0, 0xFF);
            readToken(arguments, 0, path);
            if (path[0] == 0)
            {
                const ee::Ptr<Node> owner = *g_scriptOwner;
                *found = owner;
                zge::sendNewMessage(owner, *g_root, message(about), owner, sendFlags);
                return;
            }
            resolveObjectPath(path, name, typeName, found);
            if (name[0] == 0)
                return;
            if (const ee::Ptr<zge::SceneObject> child = childWidgetNamed(foundWidget(*found), name))
            {
                *found = child;
                zge::sendNewMessage(*g_scriptOwner, *g_root, message(about), child, sendFlags);
            }
        }
    }

    // commandSetFocusAtPath: ZGE2's SETFOCUS [widget]: SetFocus sent at once.
    void commandSetFocusAtPath(ee::Ptr<char> arguments)
    {
        toRootAboutChild(arguments, Message::SetFocus, zge::kMessageNow);
    }
    void commandKillFocus(ee::Ptr<char>)
    {
        // KILLFOCUS: no widget has the focus.
        game::tail(zge::zgui::sendMessage, *g_root, message(Message::SetFocus), 0u); // the original's tail call
    }
    READABLE_FUNCTION_WAITS_OWN(commandKillFocus, Verified, dm2, commandKillFocusNow);

    // commandKillFocusNow: ZGE2's KILLFOCUS: SetFocus for no widget sent at
    // once from the script's owner to the root.
    void commandKillFocusNow(ee::Ptr<char>)
    {
        game::tail(zge::sendNewMessage, *g_scriptOwner, *g_root, message(Message::SetFocus), 0u, zge::kMessageNow);
    }

    void commandRefresh(ee::Ptr<char> arguments)
    {
        // REFRESH [widget]: the widget (the script's own without a name)
        // reloads what it shows.
        ee::Local<char[0x100]> name;
        ee::Local<ee::Ptr<Node>> widget;
        int32_t kind = static_cast<int32_t>(NodeKind::Widget);
        memset(name, 0, 255);
        readToken(arguments, 0, name);
        if (name[0] != 0)
            kind = resolvePath(in(name), ee::Ptr<char>(), ee::Ptr<char>(), widget);
        else
            *widget = scriptOwner();
        if (kind == static_cast<int32_t>(NodeKind::Widget))
            sendMessage(*widget, message(Message::Refresh), 0u);
        else
            report(kRefreshNotFound, name);
    }
    READABLE_FUNCTION_WAITS_OWN(commandRefresh, Verified, dm2, commandRefreshAtPath);

    // commandRefreshAtPath: ZGE2's REFRESH widget: Refresh sent at once from
    // the script's owner to the widget the path names (its last component a
    // child of the widget the path leads to). Without a path, or with no such
    // child, nothing.
    void commandRefreshAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> path;
        ee::Local<char[0x100]> name;
        ee::Local<char[0x100]> typeName;
        ee::Local<ee::Address> found;
        fillBytes(path, 0, 0xFF);
        fillBytes(name, 0, 0xFF);
        readToken(arguments, 0, path);
        if (path[0] != 0)
            resolveObjectPath(path, name, typeName, found);
        else
            *found = *g_scriptOwner;
        if (name[0] == 0)
            return;
        if (const ee::Ptr<zge::SceneObject> child = childWidgetNamed(foundWidget(*found), name))
        {
            *found = child;
            zge::sendNewMessage(*g_scriptOwner, child, message(Message::Refresh), 0u, zge::kMessageNow);
        }
    }

    void commandDestroy(ee::Ptr<char> arguments)
    {
        // DESTROY [widget]: the widget queued to be destroyed after the frame.
        toRootAboutWidget(arguments, Message::Destroy, kDestroyNotFound);
    }
    READABLE_FUNCTION_WAITS_OWN(commandDestroy, Verified, dm2, commandDestroyAtPath);

    // commandDestroyAtPath: ZGE2's DESTROY [widget]: Destroy queued for the
    // root (delivered after the frame's update: the root deletes the object).
    void commandDestroyAtPath(ee::Ptr<char> arguments)
    {
        toRootAboutChild(arguments, Message::Destroy, 0);
    }

    namespace
    {
        // The scripts' token buffers (256 characters each).
        inline ee::Global<char[0x100], sym::tokenBuffer> g_tokenBuffer;
        inline ee::Global<char[0x100], sym::argumentBuffer> g_argumentBuffer;
        inline ee::Global<char[0x100], sym::nameBuffer> g_nameBuffer;

        // LOADLIB, LOADMODEL and LOADIMAGE name [file [onLoaded]]: the three
        // tokens into the token, argument and name buffers (a token starting
        // with * stands for the text of the value it names).
        void readLoadArguments(ee::Ptr<char> arguments)
        {
            ee::Local<char[64]> text;
            char *const buffers[3] = {*g_tokenBuffer, *g_argumentBuffer, *g_nameBuffer};
            for (char *buffer : buffers)
                buffer[0] = 0;
            ee::Ptr<char> rest = arguments;
            for (char *buffer : buffers)
            {
                rest = readToken(rest, 0, buffer);
                if (buffer[0] == '*')
                {
                    valueText(buffer + 1, text, 1);
                    zge::stringCopy(buffer, text);
                }
            }
        }

        // ZGE2's: the line's first three tokens, each read from the line's start,
        // into the commands' first three token buffers.
        inline ee::Global<char[3][0x100], sym::tokenBuffer> g_loadTokens;
        void readLoadTokens(ee::Local<char[0x40]> &text, ee::Ptr<char> arguments)
        {
            char *const buffers[3] = {(*g_loadTokens)[0], (*g_loadTokens)[1], (*g_loadTokens)[2]};
            for (char *buffer : buffers)
                buffer[0] = 0;
            for (uint32_t index = 0; index < 3; ++index)
            {
                char *const buffer = buffers[index];
                readToken(arguments, index, buffer);
                if (buffer[0] == '*')
                {
                    valueText(buffer + 1, text, 1);
                    zge::stringCopy(buffer, text);
                }
            }
        }

        // The loaders (library.cpp): (name, file, the script run once it is in).
    }

    void commandLoadLib(ee::Ptr<char> arguments)
    {
        readLoadArguments(arguments);
        loadZguiLibrary(*g_tokenBuffer, *g_argumentBuffer, *g_nameBuffer);
    }
    READABLE_FUNCTION_WAITS_OWN(commandLoadLib, Verified, dm2, commandLoadLibTokens);

    void commandLoadLibTokens(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x40]> text;
        readLoadTokens(text, arguments);
        loadZguiLibrary((*g_loadTokens)[0], (*g_loadTokens)[1], (*g_loadTokens)[2]);
    }

    void commandLoadModel(ee::Ptr<char> arguments)
    {
        readLoadArguments(arguments);
        loadUiAssetLibraryFile(*g_tokenBuffer, *g_argumentBuffer, *g_nameBuffer);
    }
    // commandLoadZscTokens: ZGE2's LOADZSC file [argument]: the same, its two
    // tokens read from the line's start into the first two token buffers.
    void commandLoadZscTokens(ee::Ptr<char> arguments)
    {
        char *const file = (*g_loadTokens)[0];
        char *const argument = (*g_loadTokens)[1];
        file[0] = 0;
        argument[0] = 0;
        readToken(arguments, 0, file);
        readToken(arguments, 1, argument);
        zge::stringToUpper(file);
        if (file[0] == 'U' && file[1] == 'I' && file[2] == 'L' && file[3] == file[1] && file[4] == 'B' && file[5] == ':')
        {
            game::tail(loadZguiScript, file + 6, *g_root);
            return;
        }
        game::tail(loadZguiScriptNode, file, *g_root);
    }

    READABLE_FUNCTION_WAITS_OWN(commandLoadModel, Verified, dm2, commandLoadModelTokens);

    void commandLoadModelTokens(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x40]> text;
        readLoadTokens(text, arguments);
        loadUiAssetLibraryFile((*g_loadTokens)[0], (*g_loadTokens)[1], (*g_loadTokens)[2]);
    }

    void commandLoadImage(ee::Ptr<char> arguments)
    {
        readLoadArguments(arguments);
        loadUiImageFile(*g_tokenBuffer, *g_argumentBuffer, *g_nameBuffer);
    }
    READABLE_FUNCTION_WAITS_OWN(commandLoadImage, Verified, dm2, commandLoadImageTokens);

    void commandLoadImageTokens(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x40]> text;
        readLoadTokens(text, arguments);
        loadUiImageFile((*g_loadTokens)[0], (*g_loadTokens)[1], (*g_loadTokens)[2]);
    }

    void commandCancelLoadLib(ee::Ptr<char> arguments)
    {
        // CANCELLOADLIB name: the library's load cancelled.
        ee::Local<char> text;
        readToken(arguments, 0, ee::Ptr<char>(kTokenBuffer));
        if (*ee::Ptr<char>(kTokenBuffer) == '*')
        {
            valueText(ee::Ptr<const char>(kTokenBuffer + 1u), ee::Ptr<char>(text.address()), 1);
            zge::stringCopy(kTokenBuffer, text);
        }
        findUiLibrary(kTokenBuffer);
    }
    // Draft: no UI script of the nine releases uses CANCELLOADLIB (never recorded).
    READABLE_FUNCTION_WAITS(commandCancelLoadLib, Draft);

    // registerScriptFunction: a game function the scripts' CALL reaches by that name,
    // unless the name has one already. Always 1. A game override wraps it
    // (the settings' natives, registered with the game's).
    uint32_t registerScriptFunction(ee::Address name, uint32_t function)
    {
        ee::Local<int32_t> key2;
        const int32_t hash = zge::nameHash(ee::Ptr<const char>(name));
        *key2 = hash;
        if (zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kHostFunctions), key2))
            return 1;
        const ee::Ptr<HostFunction> entry(zge::heapTake(ee::Ptr<zge::Heap>(*g_heap), sizeof(HostFunction)));
        entry->hash = static_cast<uint32_t>(hash);
        const int32_t length = zge::stringLength(name);
        const uint32_t copy = zge::heapTake(ee::Ptr<zge::Heap>(*g_heap), length + 1);
        entry->name = copy;
        zge::stringCopy(copy, name);
        entry->function = function;
        zge::searchTreeInsert(ee::Ptr<zge::SearchTree>(kHostFunctions), &entry->links);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(registerScriptFunction, Verified, dm2, registerHashedScriptFunction, bmx,
                                registerScriptFunctionByHash);

    // registerHashedScriptFunction: ZGE2's registerScriptFunction: the entry
    // from zgui's memory, with the name's hash twice and no copy of the name.
    uint32_t registerHashedScriptFunction(ee::Address name, uint32_t function)
    {
        ee::Local<int32_t> key;
        *key = zge::nameHash(ee::Ptr<const char>(name));
        if (zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kHostFunctions), key))
            return 1;
        const ee::Ptr<HashedHostFunction> entry(zguiAllocate(*g_heap, sizeof(HashedHostFunction)));
        entry->hash = static_cast<uint32_t>(*key);
        entry->nameHash = zge::nameHash(ee::Ptr<const char>(name));
        entry->function = function;
        zge::searchTreeInsert(ee::Ptr<zge::SearchTree>(kHostFunctions), &entry->links);
        return 1;
    }

    // registerScriptFunctionByHash: ZGE4's registerScriptFunction: the entry
    // keeps the name's hash alone (no copy of the name).
    uint32_t registerScriptFunctionByHash(ee::Address name, uint32_t function)
    {
        ee::Local<int32_t> key;
        *key = zge::nameHash(ee::Ptr<const char>(name));
        if (zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kHostFunctions), key))
            return 1;
        const ee::Ptr<HostFunction> entry(zge::heapTake(ee::Ptr<zge::Heap>(*g_heap), kHostFunctionBytes));
        entry->function = function;
        entry->hash = static_cast<uint32_t>(*key);
        zge::searchTreeInsert(ee::Ptr<zge::SearchTree>(kHostFunctions), &entry->links);
        return 1;
    }

}
