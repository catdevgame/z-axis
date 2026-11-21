// DM2's in-game overlay (overlay.h): its start and end with the level. DM2's
// own code (overlay.c).

#include "overlay.h"

#include "overlay_items.h"
#include "screens.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/assets/chunk_reader.h"
#include "zge/memory/dmm.h"
#include "zge/platform/tasks.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/overlay/overlay_start.cpp.h"

namespace zge
{
    ChunkProgressHook chunkProgressHook(); // assets/chunk_reader.cpp
}

namespace game::dm2
{
    namespace
    {
        using zge::zgui::g_zgui;

        // The load's progress: the overlay's four steps.
        constexpr ee::Ptr<const char> kProgressName{sym::overlayProgressName}; // "OVERLAY"
        constexpr uint32_t kProgressSteps = 4;
        void progress(uint32_t step)
        {
            zge::chunkProgressHook()(kProgressName, step, kProgressSteps);
        }

        // What is shown around the screens: nothing yet, so the first
        // frame puts up what is asked for.
        constexpr uint8_t kHeadersUnset = 0xFF;

        constexpr ee::Ptr<char> kStopLoading{sym::stopLoadingScript};      // "PLAY CommonUI.StopLoading"
        constexpr uint32_t kInGameScriptFile = sym::inGameScriptFile;       // "ingame.zsc"
        constexpr ee::Ptr<const char> kInGameUiName{sym::inGameUiName};     // "InGameUI"
        // The UI's string table (zgui keeps it for the texts' $KEYs).
        inline ee::Global<uint32_t, sym::g_uiStrings> g_uiStrings;

        // The screens' task: its frame runs the open screen (gameScreensFrame).
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_overlayTask> g_overlayTask;
        constexpr uint32_t kOverlaySourcePath = sym::overlaySourcePath; // "./src/com/overlay.c"
        constexpr uint32_t kInitOverlayLine = 652;
        constexpr uint32_t kTaskStage = 1;

        // The screens' script functions, by the names their scripts call.
        struct ScreenFunction
        {
            uint32_t name, function;
        };
        constexpr ScreenFunction kScreenFunctions[] = {
            {sym::g_textBmxoverlayPauseHandler, sym::bmxoverlayPauseHandler},
            {sym::g_textBmxoverlayOptionsHandler, sym::bmxoverlayOptionsHandler},
            {sym::g_textBmxoverlayFeatureHandler, sym::bmxoverlayFeatureHandler},
            {sym::g_textBmxoverlayExitConfHandler, sym::bmxoverlayExitConfHandler},
            {sym::g_textBmxoverlayExitConfReturnToMenu, sym::bmxoverlayExitConfReturnToMenu},
            {sym::g_textBmxoverlayInstructionsHandler, sym::bmxoverlayInstructionsHandler},
            {sym::g_textBmxoverlayObjectivesHandler, sym::bmxoverlayObjectivesHandler},
            {sym::g_textBmxoverlayLeaderboardHandler, sym::bmxoverlayLeaderboardHandler},
            {sym::g_textBmxoverlayRewardHandler, sym::bmxoverlayRewardHandler},
            {sym::g_textBmxoverlayRecordsHandler, sym::bmxoverlayRecordsHandler},
            {sym::g_textBmxoverlayTrickListHandler, sym::bmxoverlayTrickListHandler},
            {sym::g_textBmxoverlayRoundOverHandler, sym::bmxoverlayRoundOverHandler},
            {sym::g_textBmxoverlayGameOverHandler, sym::bmxoverlayGameOverHandler},
            {sym::g_textBmxoverlayNPCHandler, sym::bmxoverlayNPCHandler},
            {sym::g_textBmxoverlayHighScoreHandler, sym::bmxoverlayHighScoreHandler},
            {sym::g_textBmxoverlayMPGameSelectHandler, sym::bmxoverlayMPGameSelectHandler},
            {sym::g_textBmxoverlayMPHorseWordHandler, sym::bmxoverlayMPHorseWordHandler},
            {sym::g_textBmxoverlayMPReadyHandler, sym::bmxoverlayMPReadyHandler},
            {sym::g_textBmxoverlayMPTimeUpHandler, sym::bmxoverlayMPTimeUpHandler},
            {sym::g_textBmxtrickSetGetTrick, sym::bmxtrickSetGetTrick},
            {sym::g_textBmxtrickSetUpdateCategory, sym::bmxtrickSetUpdateCategory},
        };

        // The three screens with a choice of their own keep it in a Sint32
        // variable of their frame: the feature, the NPC and the option picked.
        constexpr uint32_t kSint32TypeName = sym::overlaySint32TypeName; // "Sint32"
        struct ScreenVariable
        {
            uint32_t frame, name, value;
        };
        constexpr ScreenVariable kScreenVariables[] = {
            {sym::featureFramePath, sym::g_textFeatureNum, sym::g_featureNum},     // "InGameUI.FrameFeature"
            {sym::npcFramePath, sym::g_textNpcSelect, sym::g_npcSelect},           // "InGameUI.FrameNPC"
            {sym::optionsFramePath, sym::g_textOptionsSelect, sym::g_optionsSelect}, // "InGameUI.FrameOptions"
        };
    }

    void initOverlay()
    {
        progress(1);
        zge::zgui::runScriptLines(g_zgui->root, kStopLoading);
        zge::dmmCompact(zgui::heapRegion(g_zgui->heap));
        zge::zgui::loadZguiScript(kInGameScriptFile, g_zgui->root);
        progress(2);
        zge::setStringTable(ee::Ptr<zge::StringTable>(*g_uiStrings));
        initOverlayItems();
        *g_gameScreenRecord = ee::Ptr<GameScreenRecord>();
        *g_shownScreenHeaders = kHeadersUnset;
        setScreenHeaders(kHeadersPlay);
        progress(3);
        for (uint32_t i = 0; i < kScreenItems; ++i)
            (*g_screenItems)[i] = kNoScreenItem;
        openOverlayFrames();
        *g_gameScreen = kScreenNone;
        for (const ScreenFunction &function : kScreenFunctions)
            zge::zgui::registerScriptFunction(function.name, function.function);
        for (const ScreenVariable &variable : kScreenVariables)
        {
            const ee::Ptr<zge::zgui::Node> frame = zge::zgui::findNode(g_zgui->root, variable.frame, 0);
            zge::zgui::defineWidgetProperty(widgetObject(frame), ee::Ptr<const char>(variable.name),
                                            ee::Ptr<const char>(kSint32TypeName), variable.value);
        }
        *g_overlayTask = zge::newTask(kTaskStage, sym::gameScreensFrame, 0, 1, kOverlaySourcePath, kInitOverlayLine);
        progress(4);
    }
    READABLE_FUNCTION(initOverlay, Verified);

    uint32_t shutdownOverlay()
    {
        zge::deleteTask(*g_overlayTask);
        const ee::Ptr<GameScreenRecord> open = *g_gameScreenRecord;
        if (open && open->show)
            open->show(0);
        for (uint32_t i = 0; i < kScreenItems; ++i)
        {
            uint16_t &item = (*g_screenItems)[i];
            if (item != kNoScreenItem)
            {
                removeOverlayItem(item);
                item = kNoScreenItem;
            }
        }
        closeOverlayFrames();
        shutDownOverlayItems();
        zge::deleteSceneObject(widgetObject(zge::zgui::findNode(g_zgui->root, kInGameUiName, 0)));
        return game::tail(zge::dmmCompact, zgui::heapRegion(g_zgui->heap));
    }
    READABLE_FUNCTION(shutdownOverlay, Verified);
}
