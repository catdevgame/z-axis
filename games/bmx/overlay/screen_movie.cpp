// BMX XXX's movie confirmation and feature screens (screens.h;
// "FrameMovieConf", "FrameFeature"): asked before a turn is given up for a
// movie, and the captions shown one after another before the level is left.
// BMX XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/camera/action_camera.h"
#include "bmx/overlay/screen_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_movie.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameMovieConf = sym::g_textFrameMovieConf; // "FrameMovieConf"
        constexpr uint32_t kFrameFeature = sym::g_textFrameFeature;     // "FrameFeature"
        constexpr uint32_t kCaptionWidget = sym::g_textCaption;         // "Caption"
        constexpr uint32_t kFeatureNumName = sym::g_textFeatureNum;     // "FeatureNum"
        constexpr uint32_t kWaitAction = sym::g_textWait;               // "Wait"
        constexpr uint32_t kFadeInAction = sym::g_textFadeIn;           // "FadeIn"
        constexpr uint32_t kMovieConfText = sym::bmx::g_textMovieConfDescription; // "$KSTR_MOVIECONF_DESC00"
        constexpr uint32_t kMovieConfNo = sym::bmx::g_textMovieConfNo;            // "%s$KSTR_MOVIECONF_NO"
        constexpr uint32_t kMovieConfYes = sym::bmx::g_textMovieConfYes;          // "%s$KSTR_MOVIECONF_YES"
        constexpr uint32_t kFeatureText = sym::bmx::g_textFeatureKey;             // "$KSTR_FEATURE_%02d"

        // The confirmation's two lines.
        constexpr uint8_t kMovieNo = 0, kMovieYes = 1;

        // The feature frame's own messages: its caption's "Init" script
        // asks for the caption's text, and the caption's "FadeOut" ends.
        constexpr int32_t kFeatureCaptionWanted = 30;
        constexpr int32_t kFeatureCaptionGone = 31;
        // The captions a level's end shows.
        constexpr int32_t kFeatureCaptions = 8;
        // Whether the captions were cut short (the frame's button).
        inline ee::Global<uint32_t, sym::g_featureCutShort> g_featureCutShort;
        // The front end's first menu after the captions ran to their end.
        constexpr uint8_t kFrontEndAfterFeature = 1;
    }

    // movieConfirmScreenMessage: no goes back to play; yes ends the turn
    // and leaves for the movie; cancelled, play goes on with the rider put
    // back.
    void movieConfirmScreenMessage(int32_t message)
    {
        ee::Local<char[0x100]> text;
        ee::Local<uint32_t> selected;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameMovieConf, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenChosen:
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, 1), kSelectedIndexName, selected);
            if ((*g_menuChoices)[selected] == kMovieNo)
            {
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumePlay;
            }
            else if ((*g_menuChoices)[selected] == kMovieYes)
            {
                setRoundPhase(calls::kTurnOverPhase);
                showGameScreen(kScreenLeaveToMovie);
            }
            return;

        case kScreenEnter:
            zge::formatString(text, kMovieConfText);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kCaptionWidget, 1), kTextName, text);
            (*g_menuChoices)[0] = kMovieNo;
            text[0] = '\0';
            zge::formatString(ee::Ptr<char>(text) + zge::stringLength(text), kMovieConfNo, kNoSeparator);
            (*g_menuChoices)[1] = kMovieYes;
            zge::formatString(ee::Ptr<char>(text) + zge::stringLength(text), kMovieConfYes, kLineBreak);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 1), kItemsName, text);
            playUiNode(frame, kInitAction);
            return;

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenCancelled:
            showGameScreen(kScreenPlay);
            *g_resumeAction = kResumeRespawn;
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(movieConfirmScreenMessage, Verified);

    // featureScreenMessage: the frame counts its captions in its
    // "FeatureNum". Each caption fades in with its text and, once it has
    // gone, the next is asked for; after the last, or once the button cut
    // them short, the level is left for the front end.
    void featureScreenMessage(int32_t message, uint32_t data)
    {
        ee::Local<char[0x100]> text;
        ee::Local<int32_t> feature;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameFeature, 1);
        if (!frame)
            return;
        const ee::Ptr<Node> caption = zge::zgui::findNode(frame, kCaptionWidget, 1);
        zge::zgui::copyVariable(frame, kFeatureNumName, feature);

        switch (message)
        {
        case kScreenEnter:
            showCamera(*calls::g_gameCamera, 1);
            playUiNode(frame, kInitAction);
            *g_featureCutShort = 0;
            *calls::g_modalPlayerNum = -1;
            return;

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenChosen:
            stopUiNode(caption, kWaitAction);
            *g_featureCutShort = 1;
            return;

        case kFeatureCaptionWanted:
            zge::formatString(text, kFeatureText, feature);
            zge::zgui::setVariable(caption, kTextName, text);
            playUiNode(caption, kFadeInAction);
            return;

        case kFeatureCaptionGone:
            feature = feature + 1;
            zge::zgui::setVariable(frame, kFeatureNumName, feature);
            if (*g_featureCutShort != 0)
            {
                *g_frontEndFirstMenu = kFrontEndAfterRound;
                showGameScreen(kScreenLeave);
            }
            else if (feature < kFeatureCaptions)
            {
                featureScreenMessage(kFeatureCaptionWanted, data);
            }
            else
            {
                *g_frontEndFirstMenu = kFrontEndAfterFeature;
                showGameScreen(kScreenLeave);
            }
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(featureScreenMessage, Draft);
}
