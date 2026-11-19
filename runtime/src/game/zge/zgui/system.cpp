// zgui's own start and end, the tables of what a widget can be, and the
// tokens of a UI file (zge/ZgeMods/zgui.c, freeZguiList ..; BMX XXX PS2 has the
// reader word for word, its set-up with its own types). Here and not in
// game/zge: the set-up names AI's widget kinds and variable types, and all
// that it calls is this folder's.

#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/memory/heaps.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/assets/asset_library.h"
#include "zge/platform/tasks.h"
#include "zge/zgui/widget.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/system.cpp.h"

namespace zge
{
}

namespace zge::zgui
{

    namespace
    {
        constexpr uint32_t kSearchTreeVisitAll = sym::searchTreeClear; // (tree, visit(node, context), context)
        constexpr uint32_t kCompareNameHashes = sym::lessWord;
        constexpr uint32_t kFilePath = sym::filePath;         // callAt: identical code folds with others
        constexpr uint32_t kSetVariable = sym::setVariable;      // (widget, name, value *): by address, a game override hooks it
        constexpr uint32_t kFreeZguiList = sym::freeZguiList;     // (list)
        constexpr uint32_t kZguiFree = sym::zguiFree;         // the host functions' visit
        constexpr uint32_t kQueueNodeDestroy = sym::queueNodeDestroy; // the root's listener
        constexpr uint32_t kRegisterWidgetKind = sym::registerWidgetKind;
        constexpr uint32_t kRegisterVariableType = sym::registerVariableType;
        constexpr uint32_t kNewWidget = sym::newZguiWidget;

        constexpr uint32_t kUpdateZgui = sym::updateZgui;       // the two tasks
        constexpr uint32_t kDrawZgui = sym::drawZgui;
        constexpr uint32_t kDrawTaskPriority = game::readable::perGame(4u, 1u); // (ZGE2 draws its UI at 1)
        constexpr uint32_t kUpdateTaskPriority = 6;
        constexpr uint32_t kThisFile = sym::g_textNgengineZge3ZgeModsZguiC;         // the path of zgui.c
        constexpr uint32_t kDrawTaskLine = game::readable::perGame(214u, 214u, 228u); // (ZGE4's zgui.c: 228, 233)
        constexpr uint32_t kRestartDrawTaskLine = game::readable::perGame(312u, 295u); // setGuiTaskRunning's (ZGE2's zgui.c: 295)
        constexpr uint32_t kUpdateTaskLine = game::readable::perGame(219u, 219u, 233u);

        // The root's listeners: (message, listener).
        struct RootListener
        {
            Message message;
            uint32_t listener;
        };
        constexpr uint32_t kSetPadMapping = sym::setPadMapping;
        constexpr uint32_t kResetPadMapping = sym::resetPadMapping;
        constexpr uint32_t kSetTopPanel = sym::setTopPanel;
        const RootListener kRootListeners[] = {
            {Message::LockInput, kSetPadMapping},
            {Message::UnlockInput, kResetPadMapping},
            {Message::SetFocus, kSetTopPanel},
            {Message::Destroy, kQueueNodeDestroy},
        };

        // The rest of zgui's globals (Zgui has what the widgets use).
        //         // (+0x25C, +0x260) the UI's camera object and strings
        constexpr uint32_t kPlatformId = sym::platformId;
        constexpr uint32_t kWidgetKinds = sym::g_widgetKindList;      // the Lists
        constexpr uint32_t kNodeKinds = sym::g_nodeKinds;
        constexpr uint32_t kVariableTypes = sym::g_variableTypes;
        constexpr uint32_t kHostFunctions = sym::hostFunctions;    // a SearchTree
        constexpr uint32_t kUiLibraries = sym::g_uiLibraries;
        constexpr uint32_t kPlaying = sym::g_playing;
        constexpr uint32_t kPendingLoads = sym::g_pendingLoads;
        constexpr uint32_t kPalette = sym::g_palette;
        inline ee::Global<ee::Ptr<Task>, sym::g_drawZguiTask> g_drawTask;
        inline ee::Global<ee::Ptr<Task>, sym::g_updateZguiTask> g_updateTask;
        inline ee::Global<uint32_t, sym::g_editCameraObject> g_uiCamera; // the UI's camera object
        inline ee::Global<uint32_t, sym::g_uiStrings> g_uiStrings;
        constexpr uint32_t kLibraryFromLink = 0x88;         // a library's list node to the library

        constexpr uint32_t kTextGuiComponent = sym::g_textGuiComponent;
        constexpr uint32_t kTextGuiScreenSpace = sym::g_textGuiScreenSpace;
        constexpr uint32_t kTextScript = sym::g_textScript;
        constexpr uint32_t kTextInterpolate = sym::g_textInterpolate;
        constexpr uint32_t kTextAnimate = sym::g_textAnimate;
        constexpr uint32_t kTextTrajectory = sym::g_textTrajectory;
        constexpr uint32_t kTextApplication = sym::g_textApplication;
        constexpr uint32_t kTextRgbModulate = sym::g_textRGBModulate;
        constexpr uint32_t kTextZFloat = sym::g_textZFloat5;
        constexpr uint32_t kTextPlatformId = sym::g_textPlatformID;
        constexpr uint32_t kTextString = sym::g_textString3;
        constexpr uint32_t kTextPs2 = sym::g_textPs2;
        // ZGE4's root has three more Sint32 variables over the engine's: the
        // screen's size and the file operations under way.
        inline ee::Global<uint32_t, sym::platformId> g_platformId;
        constexpr uint32_t kTextScreenWidth = sym::bmx::g_textScreenWidth;
        constexpr uint32_t kTextScreenHeight = sym::bmx::g_textScreenHeight;
        constexpr uint32_t kTextFileOperationCount = sym::bmx::g_textFileOperationCount;
        constexpr uint32_t kTextSint32 = sym::bmx::g_textSint32Type;
        constexpr uint32_t kScreenWidth = sym::g_screenWidth, kScreenHeight = sym::g_screenHeight;
        constexpr uint32_t kFileOperationCount = sym::bmx::g_fileOperationCount;

        constexpr uint32_t kDefineGuiComponent = sym::defineWidgetProperties;
        constexpr uint32_t kDefineGuiScreenSpace = sym::defineGuiScreenSpaceProperties;
        constexpr uint32_t kInitScriptAnimation = sym::initScriptAnimation;
        constexpr uint32_t kInitAnimationNode = sym::initAnimationNode;
        constexpr uint32_t kDefineGuiAnimation = sym::defineGuiAnimationProperties;
        constexpr uint32_t kDefineGuiBallistic = sym::defineGuiBallisticProperties;

        // The node kinds: (name, bytes, create).
        struct BaseNodeKind
        {
            uint32_t name, bytes, create, returnAddress;
        };
        const BaseNodeKind kBaseNodeKinds[] = {
            {kTextScript, 0x40, kInitScriptAnimation, points::initZgui::afterRegisterZguiNodeKind},
            {kTextInterpolate, 0x50, kInitAnimationNode, points::initZgui::afterRegisterZguiNodeKind2},
            {kTextAnimate, 0x70, kDefineGuiAnimation, points::initZgui::afterRegisterZguiNodeKind3},
            {kTextTrajectory, 0x80, kDefineGuiBallistic, points::initZgui::afterRegisterZguiNodeKind4},
        };
        // The variable types, in registering order (each a VariableType).
        constexpr uint32_t kBoolType = sym::boolType;
        constexpr uint32_t kUint8Type = sym::uint8Type;
        constexpr uint32_t kSint32Type = sym::sint32Type;
        constexpr uint32_t kZFloatType = sym::zFloatType;
        constexpr uint32_t kZRgbaPalType = sym::zRgbaPalType;
        constexpr uint32_t kZ3DVectorType = sym::z3DVectorType;
        constexpr uint32_t kZQuaternionType = sym::zQuaternionType;
        constexpr uint32_t kAlignmentType = sym::alignmentType;
        constexpr uint32_t kStringType = sym::stringType;
        constexpr uint32_t kZAssetIdType = sym::zAssetIdType;
        constexpr uint32_t kEventType = sym::eventType;
        constexpr uint32_t kVoidType = sym::voidType;
        const uint32_t kBaseVariableTypes[] = {
            kBoolType, kUint8Type, kSint32Type, kZFloatType, kZRgbaPalType, kZ3DVectorType,
            kZQuaternionType, kAlignmentType, kStringType, kZAssetIdType, kEventType, kVoidType,
        };
        constexpr uint32_t kFirstTypeReturn = points::initZgui::afterRegisterVariableType;
        constexpr uint32_t kTypeCallBytes = 0x0C;

        // A row of the table of widget kinds.
        struct WidgetKind
        {
            zge::ListNode link;  // +0x00
            char name[0x20];     // +0x08
            uint32_t bytes;      // +0x28
            uint8_t flags;       // +0x2C
            uint8_t _2d[3];
            ee::Callback<void(ee::Ptr<Node>, uint32_t)> define; // +0x30 (widget, name; ZGE2: record, name, scene object)
            uint32_t objectClass; // +0x34 ZGE2: its widgets' scene object class (newZguiWidget sets it); ZGE3 never sets or reads it
        };
        static_assert(sizeof(WidgetKind) == 0x38 && offsetof(WidgetKind, define) == 0x30);

        // A row of the destroy queue.

        inline ee::Global<ee::Ptr<WidgetKind>, sym::g_widgetKindList> g_widgetKindList;
        inline ee::Global<uint32_t, sym::g_uiLibraries> g_uiLibraryList;
        inline ee::Global<ee::Ptr<char>, sym::g_parseCursor> g_tokenCursor;
        inline ee::Global<ee::Ptr<char>, sym::g_parseEnd> g_tokenEnd;

        inline ee::Ptr<zge::Heap> zguiHeap() { return g_zgui->heap; }
        inline ee::Ptr<WidgetKind> nextKind(ee::Ptr<WidgetKind> kind) { return classCast<WidgetKind>(kind->link.next); }
    }

    // freeZguiList: its rows given back to zgui's memory (ZGE2's a DMM
    // region: zguiRelease).
    void freeZguiList(ee::Address list)
    {
        const ee::Ptr<zge::List> rows(list);
        for (ee::Ptr<zge::ListNode> row = rows->head; row; row = rows->head)
        {
            zge::listRemove(list, row);
            zguiRelease(*g_heap, row);
        }
    }
    READABLE_FUNCTION(freeZguiList, Verified);

    // zguiFree.
    uint32_t zguiFree(uint32_t memory)
    {
        if constexpr (game::readable::kIsDm2)
            zguiRelease(*g_heap, memory); // (ZGE2's zgui memory is a DMM region)
        else
            zge::heapGive(zguiHeap(), memory);
        return 1;
    }
    READABLE_FUNCTION(zguiFree, Verified);

    uint32_t registerWidgetKind(uint32_t name, uint32_t bytes, uint32_t flags, uint32_t define READABLE_PARAM_ONLY(dm2, uint32_t objectClass));

    uint32_t registerVariableType(ee::Ptr<VariableType> type);

    ee::Ptr<Node> newZguiWidget(ee::Address kindName, ee::Address name);

    // initZgui: zgui up: its heap of `bytes`, the tables of kinds and
    // types, the root widget (with RGBModulate, the colours' scale, and
    // PlatformID "ps2"; ZGE4's also ScreenWidth, ScreenHeight and
    // FileOperationCount), the default material, the root's listeners, the
    // tasks that update and draw it. Once only. A kept bug: the default
    // material's flags are bit-fields of a word the original never
    // initialises, so the bits it does not set are whatever its caller left in
    // that word.
    uint32_t initZgui(uint32_t bytes)
    {
        const ee::LeftByCaller<int32_t> flagWord; // never initialised: as the caller left it
        if (g_zgui->initialised != 0)
            return 1;
        g_zgui->heap = zge::newHeap(bytes, 0);
        initList(&g_zgui->destroyQueue);
        initList(kWidgetKinds);
        registerWidgetKind(kTextGuiComponent, 0xF0, 0, kDefineGuiComponent READABLE_PARAM_ONLY(dm2, sym::dm2::g_widgetObjectClass));
        registerWidgetKind(kTextGuiScreenSpace, 0x100, 0, kDefineGuiScreenSpace READABLE_PARAM_ONLY(dm2, sym::dm2::g_widgetObjectClass));
        registerWidgetKinds();
        registerZguiLibraryChunkHandlers();
        initScriptHashes();
        initZguiPlayingPool();
        initList(kNodeKinds);
        for (const BaseNodeKind &kind : kBaseNodeKinds)
            registerZguiNodeKind(kind.name, kind.bytes, kind.create);
        initList(kVariableTypes);
        for (const uint32_t type : kBaseVariableTypes)
            registerVariableType(ee::Ptr<VariableType>(type));
        const ee::Ptr<Node> root(newZguiWidget(kTextGuiComponent, kTextApplication));
        g_zgui->root = root;
        defineTypedProperty(root, ee::Ptr<const char>(kTextRgbModulate), ee::Ptr<const char>(kTextZFloat), &g_zgui->alpha);
        if constexpr (game::readable::kIsBmx)
            *g_platformId = 0;
        defineTypedProperty(g_zgui->root, ee::Ptr<const char>(kTextPlatformId), ee::Ptr<const char>(kTextString), kPlatformId);
        setVariable(g_zgui->root, kTextPlatformId, kTextPs2);
        if constexpr (game::readable::kIsBmx)
        {
            addVariableNode(g_zgui->root, kTextScreenWidth, kTextSint32, kScreenWidth);
            addVariableNode(g_zgui->root, kTextScreenHeight, kTextSint32, kScreenHeight);
            addVariableNode(g_zgui->root, kTextFileOperationCount, kTextSint32, kFileOperationCount);
        }
        g_zgui->alpha = ee::Float(1.0f);
        const int32_t materialFlags = ((flagWord & -241 | 0x10) & -769 | 0x100) & -2049 & -1025 & -16;
        g_zgui->stickAsDpad = 1;
        g_zgui->focus = ee::Ptr<Node>();
        const ee::Ptr<zge::Material> material = newUiDefaultMaterial();
        g_zgui->defaultTexture = ee::Address(material);
        setupUiMaterial(material, materialFlags);
        initList(kPlaying);
        initSearchTree(ee::Ptr<zge::SearchTree>(kHostFunctions), kCompareNameHashes);
        initList(kUiLibraries);
        initList(kPendingLoads);
        initList(kPalette);
        for (const RootListener &listener : kRootListeners)
            addListener(g_zgui->root, listener.message, listener.listener);
        *g_drawTask = newTask(kDrawTaskPriority, kDrawZgui, 0, 1, kThisFile, kDrawTaskLine);
        *g_updateTask = newTask(kUpdateTaskPriority, kUpdateZgui, 0, 1, kThisFile, kUpdateTaskLine);
        *g_uiCamera = 0;
        *g_uiStrings = 0;
        g_zgui->selectSound = {};
        g_zgui->cancelSound = {};
        g_zgui->moveSound = {};
        g_zgui->changeSound = {};
        g_zgui->blockedSound = {};
        for (uint32_t port = 0;;)
        {
            PadInput &pad = g_zgui->pads[port];
            pad.pad = ee::Ptr<Pad>();
            pad.down = 0;
            pad.pressed = 0;
            pad.mask = 0xFFFFFFFFu;
            for (uint32_t button = 0;;)
            {
                pad.repeat[button] = ee::Float(0.0f);
                if (++button >= 32u)
                    break;
            }
            if (++port >= 4u)
                break;
        }
        g_zgui->initialised = 1;
        g_zgui->input.idleFrames = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(initZgui, Verified, dm2, initZguiObjects);

    namespace
    {
        // (DM2's own row: the bind table pairs AI's g_textGuiComponent with a function's address.)
        constexpr uint32_t kListedGuiComponent = sym::dm2::g_textGuiComponent; // "guiComponent"
        constexpr uint32_t kTextGuiWorldSpace = sym::dm2::g_textGuiWorldSpace; // "guiWorldSpace"
        // ZGE2's types, in registering order (no Uint8; its colour is ZRGBA).
        const uint32_t kListedVariableTypes[] = {
            kBoolType, sym::dm2::sint32Type, sym::dm2::zFloatType, sym::dm2::zRgbaType, sym::dm2::z3DVectorType,
            sym::dm2::zQuaternionType, sym::dm2::alignmentType, sym::dm2::stringType, sym::dm2::zAssetIdType,
            sym::dm2::eventType, sym::dm2::voidType,
        };
        // ZGE2's ACTION kinds: the same four, their records the ListedAction's size on.
        const BaseNodeKind kListedNodeKinds[] = {
            {kTextScript, 0x4C, kInitScriptAnimation, 0},
            {kTextInterpolate, 0x64, kInitAnimationNode, 0},
            {kTextAnimate, 0x7C, kDefineGuiAnimation, 0},
            {kTextTrajectory, 0x90, kDefineGuiBallistic, 0},
        };
        // The file's chunks ZGE2 registers here itself (ZGE3: registerZguiLibraryChunkHandlers).
        constexpr uint32_t kListedChunkHandlers[][2] = {
            {0x42494C48u, sym::readHlibChunk}, // "HLIB"
            {0x54534155u, sym::readUastChunk}, // "UAST"
            {0x54534148u, sym::readHastChunk}, // "HAST"
            {0x4C544D55u, sym::readUmtlChunk}, // "UMTL"
            {0x4C544D48u, sym::readHmtlChunk}, // "HMTL"
            {0x544E4648u, sym::readHfntChunk}, // "HFNT"
            {0x544E4655u, sym::readUfntChunk}, // "UFNT"
            {0x44414E55u, sym::readUnadChunk}, // "UNAD"
            {0x4C444D55u, sym::readSceneChunks}, // "UMDL"
        };
        // The root's handlers: (message, handler).
        const RootListener kRootHandlers[] = {
            {Message::PlayAction, sym::dm2::playListedAction},
            {Message::StopAction, sym::dm2::stopListedAction},
            {Message::EndAction, sym::dm2::endListedAction},
            {Message::LockInput, kSetPadMapping},
            {Message::UnlockInput, kResetPadMapping},
            {Message::SetFocus, kSetTopPanel},
            {Message::Destroy, kQueueNodeDestroy},
        };
        constexpr uint32_t kListedUpdateTaskPriority = 5;
        constexpr uint32_t kListedDrawTaskLine = 212, kListedUpdateTaskLine = 213; // ZGE2's zgui.c
    }

    // initZguiObjects: ZGE2's initZgui: zgui's memory is a DMM region; three
    // plain widget kinds (guiComponent, guiScreenSpace, guiWorldSpace) before
    // the others; the file chunks' handlers added here; the ACTION kinds and
    // the types (no Uint8); the root a scene object, its RGBModulate and
    // PlatformID listed, its handlers subscribed (PlayAction, StopAction and
    // EndAction among them: ZGE2's root plays the ACTIONs); the two tasks.
    // The default material's flags are built on a register the caller left
    // (the kept bug of ZGE3's). 1.
    uint32_t initZguiObjects(uint32_t bytes)
    {
        const ee::LeftByCaller<int32_t> flagWord; // never initialised: as the caller left it
        if (g_zgui->initialised != 0)
            return 1;
        g_zgui->heap = ee::Ptr<zge::Heap>(zge::newDmmRegion(bytes).address);
        initList(kWidgetKinds);
        registerWidgetKind(kListedGuiComponent, sizeof(WidgetRecord), 0, kDefineGuiComponent READABLE_PARAM_ONLY(dm2, sym::dm2::g_widgetObjectClass));
        registerWidgetKind(kTextGuiScreenSpace, sizeof(WidgetRecord), 0, kDefineGuiComponent READABLE_PARAM_ONLY(dm2, sym::dm2::g_widgetObjectClass));
        registerWidgetKind(kTextGuiWorldSpace, sizeof(WidgetRecord), 0, kDefineGuiComponent READABLE_PARAM_ONLY(dm2, sym::dm2::g_widgetObjectClass));
        registerWidgetKinds();
        for (const auto &handler : kListedChunkHandlers)
            zge::addChunkHandler(handler[0], handler[1]);
        initList(kNodeKinds);
        for (const BaseNodeKind &kind : kListedNodeKinds)
            registerZguiNodeKind(kind.name, kind.bytes, kind.create);
        initList(kVariableTypes);
        for (const uint32_t type : kListedVariableTypes)
            registerVariableType(ee::Ptr<VariableType>(type));
        const ee::Ptr<Node> root(newZguiWidget(kListedGuiComponent, kTextApplication));
        g_zgui->root = root;
        defineWidgetProperty(ee::Ptr<zge::SceneObject>(root.address), ee::Ptr<const char>(kTextRgbModulate), ee::Ptr<const char>(kTextZFloat), &g_zgui->alpha);
        defineWidgetProperty(ee::Ptr<zge::SceneObject>(g_zgui->root.address), ee::Ptr<const char>(kTextPlatformId), ee::Ptr<const char>(kTextString), kPlatformId);
        setVariable(g_zgui->root, kTextPlatformId, kTextPs2);
        const int32_t materialFlags = ((flagWord & -241 | 0x10) & -769 | 0x100) & -2049 & -1025 & -16;
        g_zgui->alpha = ee::Float(1.0f);
        g_zgui->stickAsDpad = 1;
        g_zgui->focus = ee::Ptr<Node>();
        const ee::Ptr<zge::Material> material = newUiDefaultMaterial();
        g_zgui->defaultTexture = ee::Address(material);
        setupUiMaterial(material, materialFlags);
        initList(kPlaying);
        initSearchTree(ee::Ptr<zge::SearchTree>(kHostFunctions), kCompareNameHashes);
        initList(kUiLibraries);
        initList(kPendingLoads);
        for (const RootListener &handler : kRootHandlers)
            subscribeWidget(g_zgui->root, handler.message, handler.listener);
        *g_drawTask = newTask(kDrawTaskPriority, kDrawZgui, 0, 1, kThisFile, kListedDrawTaskLine);
        *g_updateTask = newTask(kListedUpdateTaskPriority, kUpdateZgui, 0, 1, kThisFile, kListedUpdateTaskLine);
        *g_uiCamera = 0;
        *g_uiStrings = 0;
        g_zgui->selectSound = {};
        g_zgui->cancelSound = {};
        g_zgui->moveSound = {};
        g_zgui->changeSound = {};
        g_zgui->blockedSound = {};
        for (PadInput &pad : g_zgui->pads)
        {
            pad.pad = ee::Ptr<Pad>();
            pad.down = 0;
            pad.pressed = 0;
            pad.mask = 0xFFFFFFFFu;
            for (ee::Float &repeat : pad.repeat)
                repeat = ee::Float(0.0f);
        }
        g_zgui->initialised = 1;
        g_zgui->input.idleFrames = 0;
        return 1;
    }

    namespace
    {
        void searchTreeClear(uint32_t tree, uint32_t node, uint32_t context) { ee::game(kSearchTreeVisitAll, tree, node, context); }
    }

    // shutdownZgui: its tasks, tables, libraries and root gone, the heap
    // last.
    uint32_t shutdownZgui()
    {
        if (g_zgui->initialised != 0)
        {
            deleteTask(*g_drawTask);
            deleteTask(*g_updateTask);
            freeZguiList(kWidgetKinds);
            freeZguiList(kNodeKinds);
            searchTreeClear(kHostFunctions, kZguiFree, 0);
            freeZguiList(kPlaying);
            uint32_t library = *g_uiLibraryList;
            if (library != 0)
            {
                for (;;)
                {
                    unloadUiLibrary(library + kLibraryFromLink);
                    library = *g_uiLibraryList;
                    if (library == 0)
                        break;
                }
            }
            destroyZguiPlayingPool();
            destroyNode(g_zgui->root);
            zge::deleteHeap(zguiHeap());
            g_zgui->initialised = 0;
            g_zgui->root = ee::Ptr<Node>();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(shutdownZgui, Verified, dm2, shutdownZguiObjects);

    // shutdownZguiObjects: ZGE2's shutdownZgui: its tasks, tables and
    // libraries gone, the root's scene object deleted (its class's destroy
    // takes the widgets with it), the DMM region freed last. ZGE2 has no
    // playing pool to destroy and leaves the root's word as it was. 1.
    uint32_t shutdownZguiObjects()
    {
        if (g_zgui->initialised != 0)
        {
            deleteTask(*g_drawTask);
            deleteTask(*g_updateTask);
            freeZguiList(kWidgetKinds);
            freeZguiList(kNodeKinds);
            searchTreeClear(kHostFunctions, kZguiFree, 0);
            freeZguiList(kPlaying);
            while (const uint32_t library = *g_uiLibraryList)
                unloadUiLibrary(library + kLibraryFromLink);
            zge::deleteSceneObject(ee::Ptr<zge::SceneObject>(g_zgui->root.address));
            zge::freeDmmRegion(ee::Ptr<zge::DmmRegion>(g_zgui->heap.address));
            g_zgui->initialised = 0;
        }
        return 1;
    }

    // setGuiTaskRunning (nothing calls it in AI): zgui's draw task made again
    // (on; nothing if it is there) or deleted (off; nothing if it is not).
    void setGuiTaskRunning(uint32_t on)
    {
        if (on != 0)
        {
            if (*g_drawTask == 0)
                *g_drawTask = newTask(kDrawTaskPriority, kDrawZgui, 0, 1, kThisFile, kRestartDrawTaskLine);
        }
        else if (*g_drawTask != 0)
        {
            deleteTask(*g_drawTask);
            *g_drawTask = 0;
        }
    }
    READABLE_FUNCTION_WAITS(setGuiTaskRunning, Draft);

    // queueNodeDestroy: (the root's Destroy message): the node is destroyed after
    // the frame.
    uint32_t queueNodeDestroy(uint32_t /*listener*/, ee::Ptr<Node> node)
    {
        const ee::Ptr<DestroyRow> row(zge::heapTake(zguiHeap(), sizeof(DestroyRow)));
        row->node = node;
        listPush(&g_zgui->destroyQueue, row);
        return 1;
    }
    READABLE_FUNCTION_OWN(queueNodeDestroy, Verified, dm2, deleteWidgetByMessage);

    // deleteWidgetByMessage: ZGE2's queueNodeDestroy, the root's Destroy handler
    // with the EngineMessage: the widget's scene object (the message's word)
    // is deleted there and then (its class's destroy takes the widget with
    // it); ZGE3 queues the node for the end of the frame. 1.
    uint32_t deleteWidgetByMessage(ee::Ptr<zge::EngineMessage> message)
    {
        zge::deleteSceneObject(ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(message->data)));
        return 1;
    }

    // registerWidgetKind. ZGE2 passes the class of the kind's widget scene
    // objects besides (g_widgetObjectClass for all of DM2's).
    uint32_t registerWidgetKind(uint32_t name, uint32_t bytes, uint32_t flags, uint32_t define READABLE_PARAM_ONLY(dm2, uint32_t objectClass = 0))
    {
        const ee::Ptr<WidgetKind> kind(zguiAllocate(*g_heap, sizeof(WidgetKind)));
        zge::stringCopy(kind->name, name);
        kind->bytes = bytes;
        kind->flags = static_cast<uint8_t>(flags);
        kind->define = define;
        if constexpr (game::readable::kIsDm2)
            kind->objectClass = objectClass;
        listPush(kWidgetKinds, kind);
        return 1;
    }
    READABLE_FUNCTION(registerWidgetKind, Verified);

    // registerVariableType: once only per type.
    uint32_t registerVariableType(ee::Ptr<VariableType> type)
    {
        for (ee::Ptr<VariableType> known = *g_variableTypes; known; known = known->next)
        {
            if (known == type)
                return 1;
        }
        if constexpr (kTypesKeepNameHash)
            type->hash = zge::nameHash(type->name);
        listPush(kVariableTypes, type);
        return 1;
    }
    READABLE_FUNCTION(registerVariableType, Verified);

    // newZguiWidget: a new widget of the kind named (any case), defined by
    // its kind, or 0.
    ee::Ptr<Node> newZguiWidget(ee::Address kindName, ee::Address name)
    {
        for (ee::Ptr<WidgetKind> kind = *g_widgetKindList; kind; kind = nextKind(kind))
        {
            if (zge::stringCompareNoCase(kind->name, kindName) == 0)
            {
                const ee::Ptr<Node> widget(zge::heapTake(zguiHeap(), kind->bytes));
                if constexpr (!game::readable::kIsBmx)
                    widget->type = kind->name; // (ZGE4's widget keeps no kind name)
                kind->define(widget, name);
                return widget;
            }
        }
        return {};
    }
    READABLE_FUNCTION_OWN(newZguiWidget, Verified, dm2, newWidgetObject);

    // newWidgetObject: ZGE2's newZguiWidget: a widget is a scene object of
    // the widgets' kind with its kind's class, and its record (of zgui's
    // memory, the kind's size) follows it; the kind defines the record
    // (record, name, object). The object, or 0 (no such kind, no object left).
    ee::Ptr<zge::SceneObject> newWidgetObject(ee::Address kindName, ee::Address name)
    {
        for (ee::Ptr<WidgetKind> kind = *g_widgetKindList; kind; kind = nextKind(kind))
        {
            if (zge::stringCompareNoCase(kind->name, kindName) != 0)
                continue;
            const ee::Ptr<zge::SceneObject> object = zge::newSceneObject();
            if (!object)
                return {};
            zge::initSceneObject(object, kWidgetObjectKind);
            object->objectClass = ee::Ptr<zge::SceneObjectClass>(kind->objectClass);
            const ee::Ptr<WidgetRecord> record(zguiAllocate(*g_heap, kind->bytes));
            object->animator = ee::Ptr<zge::Animator>(record.address);
            record->kind = kind->name;
            const ee::Callback<void(ee::Address, ee::Address, ee::Address)> define(kind->define.address);
            define(record, name, object);
            return object;
        }
        return {};
    }

    // loadZguiScript: a script file of the UI's archive (by its name's hash)
    // read into memory, loaded as a memory file onto `parent`. 0 if it has
    // none. The block is twice the file (the loader's text grows in it).
    ee::Ptr<Node> loadZguiScript(ee::Address fileName, ee::Ptr<Node> parent)
    {
        const uint32_t reader = g_zgui->assets;
        const uint32_t id = zge::nameHash(ee::Ptr<const char>(fileName));
        if (zge::findAsset(reader, id) == 0)
            return {};
        const uint32_t bytes = zge::currentAssetLength(g_zgui->assets);
        const uint32_t memory = zge::allocateMemory(bytes << 1);
        zge::readAsset(g_zgui->assets, memory, 0);
        const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(memory, bytes);
        const ee::Ptr<const char> path = zge::filePath(file);
        const ee::Ptr<Node> script = loadZguiScriptNode(path, parent);
        zge::closeFile(file, 0);
        zge::freeMemory(memory);
        return script;
    }
    READABLE_FUNCTION_WAITS_OWN(loadZguiScript, Verified, bmx, loadZguiScriptAsset);

    // loadZguiScriptAsset: ZGE4's loadZguiScript: the script read from the
    // UI's archive into a block of its length rounded up to the archive's
    // alignment, and its text's tree built under `parent` (no memory file).
    // 0 if the archive has no such script.
    ee::Ptr<Node> loadZguiScriptAsset(ee::Address fileName, ee::Ptr<Node> parent)
    {
        const uint32_t id = zge::nameHash(ee::Ptr<const char>(fileName));
        if (zge::findAsset(g_zgui->assets, id) == 0)
            return {};
        const uint32_t length = zge::currentAssetLength(g_zgui->assets);
        const uint32_t alignment = static_cast<uint32_t>(zge::currentAssetAlignment(g_zgui->assets));
        const uint32_t bytes = (length - 1 + alignment) / alignment * alignment;
        const uint32_t memory = zge::allocateMemory(bytes);
        zge::readAsset(g_zgui->assets, memory, 0);
        const ee::Ptr<Node> script = loadZguiScriptText(memory, bytes, parent);
        zge::freeMemory(memory);
        return script;
    }

    namespace
    {
        bool isTokenSpace(int32_t character)
        {
            return character == ' ' || character == '\t' || character == '\r' || character == '\n';
        }

        // What parseZguiTokens reads.
        constexpr int32_t kReadWord = 0;   // a word, to white space
        constexpr int32_t kReadValue = 1;  // a "string", a { block } or the rest of the line
        constexpr int32_t kExpectText = 2; // the text in `out` must come next
    }

    // parseZguiTokens: the UI file's next token from the cursor (g_tokenCursor,
    // to g_tokenEnd), after white space and // comments (a comment runs to
    // its \r). A word: into `out`, to white space. A value: a "string" with
    // \" \n \\ (a \n between \" and \" stays as it is, as does any other
    // escape); a { block }, its lines joined by \r\n, leading blanks and
    // comment lines off, " and \ escaped, '}' ending it only at a line's
    // start; or the rest of the line. Expected text: it must be next, and the
    // cursor moves past it. 0 at the file's end (the cursor left just past
    // it) or a mismatch; a line break inside a string gives 0 too.
    uint32_t parseZguiTokens(ee::Ptr<char> out, int32_t mode)
    {
        ee::Ptr<char> &cursor = *g_tokenCursor;
        const ee::Ptr<char> end = *g_tokenEnd;
        // Moves the cursor on; false once it is past the file's end.
        auto advance = [&](ptrdiff_t characters) {
            cursor += characters;
            return !(end < cursor);
        };
        auto skipWhiteSpace = [&] {
            while (isTokenSpace(*cursor))
            {
                if (!advance(1))
                    return false;
            }
            return true;
        };
        auto atLineEnd = [&] { return *cursor == '\r' || *cursor == '\n'; };
        auto atComment = [&] { return cursor[0] == '/' && cursor[1] == '/'; };

        if (!skipWhiteSpace())
            return 0;
        while (atComment())
        {
            while (*cursor != '\r')
            {
                if (!advance(1))
                    return 0;
            }
            if (!skipWhiteSpace())
                return 0;
        }

        if (mode == kExpectText)
        {
            // The original compares the expected text's bytes signed and the
            // file's unsigned, so a byte above 127 never matches. Only "{" is
            // ever expected.
            for (int32_t left = zge::stringLength(out); left != 0; --left)
            {
                if (static_cast<uint8_t>(*cursor) != static_cast<int8_t>(*out))
                    return 0;
                ++out;
                ++cursor;
            }
            return 1;
        }
        if (mode == kReadWord)
        {
            while (!isTokenSpace(*cursor))
            {
                *out = *cursor;
                ++out;
                if (!advance(1))
                    return 0;
            }
            *out = 0;
            return 1;
        }
        if (mode != kReadValue)
            return 1;

        if (*cursor == '"')
        {
            // A string. \" writes a quote and opens or closes a quote inside
            // it, where \n is not a line break.
            ++cursor;
            bool insideQuotes = false;
            while (*cursor != '"')
            {
                const char character = cursor[0];
                if (character == '\\')
                {
                    const char escaped = cursor[1];
                    if (escaped == '"')
                    {
                        *out = '"';
                        ++out;
                        insideQuotes = !insideQuotes;
                    }
                    else if (escaped == 'n' && !insideQuotes)
                    {
                        out[0] = '\r';
                        out[1] = '\n';
                        out += 2;
                    }
                    else if (escaped == '\\')
                    {
                        *out = '\\';
                        ++out;
                    }
                    else
                    {
                        out[0] = '\\';
                        out[1] = escaped;
                        out += 2;
                    }
                    if (!advance(2))
                        return 0;
                }
                else
                {
                    if (character == '\r')
                        return 0;
                    *out = character;
                    ++out;
                    if (!advance(1))
                        return 0;
                }
            }
            ++cursor;
            *out = 0;
            return 1;
        }

        if (*cursor == '{')
        {
            // A block of script: its lines, each one's blanks in front off,
            // joined by \r\n. A comment line adds nothing. The text ends at
            // every line's end; an empty block leaves `out` as it was.
            ++cursor;
            if (!skipWhiteSpace())
                return 0;
            bool firstLine = true;
            while (*cursor != '}')
            {
                if (atComment())
                {
                    while (!atLineEnd())
                    {
                        if (!advance(1))
                            return 0;
                    }
                }
                else
                {
                    if (!firstLine)
                    {
                        out[0] = '\r';
                        out[1] = '\n';
                        out += 2;
                    }
                    firstLine = false;
                    while (!atLineEnd())
                    {
                        const char character = *cursor;
                        if (character == '"' || character == '\\')
                        {
                            *out = '\\';
                            ++out;
                        }
                        *out = character;
                        ++out;
                        if (!advance(1))
                            return 0;
                    }
                }
                // Past the line break (a \n after it too), then the next
                // line's blanks.
                ++cursor;
                if (*cursor == '\n')
                    ++cursor;
                *out = 0;
                while (*cursor == ' ' || *cursor == '\t')
                {
                    if (!advance(1))
                        return 0;
                }
            }
            ++cursor;
            return 1;
        }

        // The rest of the line.
        while (!atLineEnd())
        {
            *out = *cursor;
            ++out;
            if (!advance(1))
                return 0;
        }
        *out = 0;
        return 1;
    }
    READABLE_FUNCTION(parseZguiTokens, Verified);
}
