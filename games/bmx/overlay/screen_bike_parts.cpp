// BMX XXX's bike parts screen (screens.h; "FrameBikeParts"): how many of
// each bike part the current player's character has found, and the bike it
// rides. BMX XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/overlay/screen_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_bike_parts.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameBikeParts = sym::g_textFrameBikeParts; // "FrameBikeParts"
        constexpr uint32_t kBikeInfoWidget = sym::g_textFrameInfoData;  // "FrameInfo.Data"
        // The parts' labels in the frame's picture, by part: "LabelFrame",
        // "LabelFork", "LabelHandlebars", "LabelWheel", "LabelCrank",
        // "LabelSeat".
        inline ee::Global<ee::Ptr<const char>[kBikeParts], sym::g_bikePartLabels> g_bikePartLabels;
        constexpr uint32_t kPartLabelPath = sym::bmx::g_textPartLabelPath; // "FramePicture.%s"
        constexpr uint32_t kPartCount = sym::bmx::g_textPartCount;         // "x%d"
        // "%d / %d\n$KSTR_BIKENAME_%d_%d\n%d / %d": the bike's stage of
        // five, its name, the parts found of twenty-four.
        constexpr uint32_t kBikeInfo = sym::bmx::g_textBikeInfo;
        constexpr uint32_t kBikeStages = 5;
        constexpr uint32_t kAllBikeParts = kCharacterBikes * kBikeParts;
    }

    // bikePartsScreenMessage: a part's count is how many sets, from the
    // first, have it found: the count stops at the first set without it.
    void bikePartsScreenMessage(int32_t message)
    {
        ee::Local<char[0x100]> text;
        ee::Local<char[0x40]> path;
        ee::Local<ee::Ptr<const char>[kBikeParts]> labels;

        for (uint32_t part = 0; part < kBikeParts; ++part)
            labels[part] = (*g_bikePartLabels)[part];
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameBikeParts, 1);
        if (!frame)
            return;

        if (message == kScreenEnter)
        {
            const ee::Ptr<RiderCharacter> character = playerCharacter(*calls::g_currentPlayer);
            for (uint32_t part = 0; part < kBikeParts; ++part)
            {
                uint32_t found = 0;
                while (found < kCharacterBikes && (character->bikeParts[found][part] & kBikePartFound) != 0)
                    ++found;
                zge::formatString(path, kPartLabelPath, labels[part]);
                zge::formatString(text, kPartCount, found);
                zge::zgui::setVariable(zge::zgui::findNode(frame, path, 1), kTextName, text);
            }

            const uint8_t stage = playerCharacter(*calls::g_currentPlayer)->bikes;
            const uint8_t bike = playerCharacter(*calls::g_currentPlayer)->bike;
            const uint8_t nameStage = playerCharacter(*calls::g_currentPlayer)->bikes;
            zge::formatString(text, kBikeInfo, stage, kBikeStages, bike, nameStage - 1, character->bikePartsFound, kAllBikeParts);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kBikeInfoWidget, 1), kTextName, text);
            playUiNode(frame, kInitAction);
        }
        else if (message == kScreenLeaveNow)
        {
            playUiNode(frame, kShutdownAction);
        }
        else if (message == kScreenCancelled)
        {
            if (gameScreen() == kScreenGameOverBikeParts)
                showGameScreen(kScreenGameOver);
            else
                showGameScreen(kScreenPause);
        }
    }
    READABLE_FUNCTION(bikePartsScreenMessage, Verified);
}
