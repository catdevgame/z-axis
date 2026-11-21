// DM2's feature screen (screens.h; "InGameUI.FrameFeature"): what its show
// and its script function do with the frame's FeatureNum. DM2's own code
// (overlay.c).

#include "screens.h"

#include "dm2/action/action.h"
#include "dm2/camera/camera.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_feature.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kFeatureFrame{sym::featureFramePath};   // "InGameUI.FrameFeature"
        constexpr ee::Ptr<const char> kFeatureNumName{sym::g_textFeatureNum}; // "FeatureNum"
        // The scripts run on the root: the frame's Init and DeInit actions,
        // and its caption's Wait stopped.
        constexpr ee::Ptr<char> kInitScript{sym::featureInitScript};         // "PLAY Root.InGameUI.FrameFeature.Init"
        constexpr ee::Ptr<char> kDeInitScript{sym::featureDeInitScript};     // "PLAY Root.InGameUI.FrameFeature.DeInit"
        constexpr ee::Ptr<char> kStopWaitScript{sym::featureStopWaitScript}; // "STOP Root.InGameUI.FrameFeature.Caption.Wait"
        // FeatureNum's values the script function tells apart: play goes
        // on, the front end, and the caption's wait over.
        constexpr int32_t kFeaturePlay = 10, kFeatureFrontEnd = 11;
    }

    // showFeatureScreen: as it opens, the camera at one of the level's
    // camera points for the first player, then the frame's Init; as it
    // closes, its DeInit.
    void showFeatureScreen(uint32_t shown)
    {
        if (!shown)
        {
            game::tail(runUiScript, kDeInitScript);
            return;
        }
        setCameraPlayer(0);
        setCameraMode(0, 0, kCameraLevelPoint);
        game::tail(runUiScript, kInitScript);
    }
    READABLE_FUNCTION(showFeatureScreen, Draft);

    // bmxoverlayFeatureHandler: the frame's FeatureNum taken: 10 closes the
    // screen (play goes on), 11 leaves for the front end; any other becomes
    // 11 and the caption's wait is stopped.
    void bmxoverlayFeatureHandler()
    {
        ee::Local<int32_t> feature;
        zge::zgui::copyVariable(uiWidget(kFeatureFrame), kFeatureNumName, feature);
        if (*feature == kFeaturePlay)
        {
            showGameScreen(kScreenNone);
            return;
        }
        if (*feature == kFeatureFrontEnd)
        {
            setGameState(kGameStateFrontEnd);
            return;
        }
        *feature = kFeatureFrontEnd;
        zge::zgui::setVariable(uiWidget(kFeatureFrame), kFeatureNumName, feature);
        runUiScript(kStopWaitScript);
    }
    READABLE_FUNCTION(bmxoverlayFeatureHandler, Draft);
}
