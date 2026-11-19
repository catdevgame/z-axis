// The memory card frame (ui_frame.h): DM2's own code (action.c).

#include "ui_frame.h"
#include "card_frame.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/action/ui_frame.cpp.h"

namespace game::dm2
{
    void setUiFrame(uint32_t on, uint32_t kind, ee::Ptr<UiFrameRequest> request)
    {
        if (!on)
        {
            g_uiFrameRequest->onDone = UiFrameCallback();
            g_uiFrameRequest->dataHandle = 0;
            g_uiFrameRequest->onClosed = UiFrameCallback();
            g_uiFrameRequest->data = 0;
            cardFrameEndHook();
            return;
        }

        *g_uiFrameKind = static_cast<uint8_t>(kind);
        *g_uiFrameStep = kStepNone;
        if (*g_uiFrameKind == kUiFrameAutoLoad)
        {
            *g_cardPort = 0;
            *g_uiFrameStep = kStepChooseLoadSlot;
            setUiFrameStep(kStepCheckingToLoad);
        }
        else if (*g_uiFrameKind != kUiFrameStartCheck)
        {
            setUiFrameStep(uiFrameSaves() ? kStepChooseSaveSlot : kStepChooseLoadSlot);
        }
        cardFrameStartHook();
        setCardFileKind(uiFrameFileKind(kind));
        if (request)
        {
            g_uiFrameRequest->onDone = request->onDone;
            g_uiFrameRequest->dataHandle = request->dataHandle;
            g_uiFrameRequest->onClosed = request->onClosed;
            g_uiFrameRequest->data = request->data;
        }

        if (*g_uiFrameKind == kUiFrameStartCheck)
        {
            *g_cardCheckResult = checkMemoryCards(ee::Ptr<uint32_t>(sym::g_cardPort));
            if (*g_cardCheckResult != 0)
                setUiFrameStep(kStepStartCheckReport);
            else if (g_uiFrameRequest->onDone)
                g_uiFrameRequest->onDone(g_uiFrameRequest->data, g_uiFrameRequest->dataHandle);
        }
    }
    READABLE_FUNCTION(setUiFrame, Verified);

    uint32_t uiFrameSaves()
    {
        const uint8_t kind = *g_uiFrameKind;
        return kind == kUiFrameSaveGame || kind == kUiFrameSaveReplay || kind == kUiFrameSavePark ? 1 : 0;
    }
    READABLE_FUNCTION(uiFrameSaves, Verified);

    uint32_t uiFrameFileKind(uint32_t kind)
    {
        switch (kind)
        {
        case kUiFrameSaveReplay:
        case kUiFrameLoadReplay:
            return kCardFileReplay;
        case kUiFrameSavePark:
        case kUiFrameLoadPark:
            return kCardFilePark;
        default:
            return kCardFileGame;
        }
    }
    READABLE_FUNCTION(uiFrameFileKind, Verified);

    void uiFrameCardDone(uint32_t operation, int32_t result)
    {
        (void)operation;
        *g_cardOperationResult = result;
    }
    READABLE_FUNCTION(uiFrameCardDone, Verified);
}
