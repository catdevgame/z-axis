// The level scripts' natives, as the game registers them at boot
// (registerScriptNatives; the park editor's in registerParkEditorNatives and initInlineFx), in
// registration order. Natives pop their arguments and push their results
// on the script stack (script.h). Several names share one function (the
// get*Data natives copy a message's data the same way).

#include "script.h"

#include "zge/text/strings.h"

#include <string_view>
#include "sym/aggressive/script/natives.cpp.h"

namespace aggressive::script
{
    const NativeName kNativeNames[] = {
        {sym::nativePlayAnim, "playAnim"},
        {sym::nativeStopAnim, "stopAnim"},
        {sym::nativeConsolePrintString, "consolePrintString"},
        {sym::nativeConsolePrintInt, "consolePrintInt"},
        {sym::nativeConsolePrintFloat, "consolePrintFloat"},
        {sym::nativeConsolePrintHash, "consolePrintHash"},
        {sym::nativeConsolePrintNewLine, "consolePrintNewLine"},
        {sym::nativeExtractFloatFromTagData, "extractFloatFromTagData"},
        {sym::nativeDisplayObjectiveText, "displayObjectiveText"},
        {sym::nativeStopObjective, "stopObjective"},
        {sym::nativeAddTrick, "addTrick"},
        {sym::nativePlayTrickScore, "playTrickScore"},
        {sym::nativeDisplayObjectiveTitle, "displayObjectiveTitle"},
        {sym::nativeShatterObjective, "shatterObjective"},
        {sym::nativePlayCameraAnim, "playCameraAnim"},
        {sym::nativePlayCameraAnimWithFov, "playCameraAnimWithFOV"},
        {sym::nativePlayCameraAnimTarget, "playCameraAnimTarget"},
        {sym::nativeNewRecord, "newRecord"},
        {sym::nativeShowAcidDrop, "showAcidDrop"},
        {sym::nativeSetRuler, "setRuler"},
        {sym::nativeShowRuler, "showRuler"},
        {sym::nativeSetObjectiveState, "setObjectiveState"},
        {sym::nativeShowObjCurr, "showObjCurr"},
        {sym::nativeFadeObjCurr, "fadeObjCurr"},
        {sym::nativeDisplayOverlayText, "displayOverlayText"},
        {sym::nativeSetBonusObjectiveState, "setBonusObjectiveState"},
        {sym::nativeBreakPoint, "breakPoint"},
        {sym::nativeSetObjectVisible, "setObjectVisible"},
        {sym::nativePauseAnim, "pauseAnim"},
        {sym::nativeResumeAnim, "resumeAnim"},
        {sym::nativeGetTextFromAnimEventData, "getTextFromAnimEventData"},
        {sym::nativeGetGrindStartData, "getGrindStartData"},
        {sym::nativeGetGrindStartData, "getManualStartData"},
        {sym::nativeGetGrindStartData, "getStallStartData"},
        {sym::nativeGetGrindStartData, "getOverStartData"},
        {sym::nativeGetGrindStartData, "getOverEndData"},
        {sym::nativeGetGrindStartData, "getWallRideStartData"},
        {sym::nativeGetGrindStartData, "getWallTapStartData"},
        {sym::nativeGetGrindStartData, "getOnStartData"},
        {sym::nativeGetGrindStartData, "getOnEndData"},
        {sym::nativeGetGrindStartData, "getJumpData"},
        {sym::nativeGetGrindStartData, "getSwingData"},
        {sym::nativeGetGrindStartData, "getVaultData"},
        {sym::nativeGetGrindStartData, "getSlideStartData"},
        {sym::nativeGetGrindStartData, "getSlideEndData"},
        {sym::nativeGetGrindStartData, "getSkitchData"},
        {sym::nativeGetGrindEndData, "getGrindEndData"},
        {sym::nativeGetGrindEndData, "getManualEndData"},
        {sym::nativeGetGrindEndData, "getAcidDropLandData"},
        {sym::nativeGetStallEndData, "getStallEndData"},
        {sym::nativeGetCollideData, "getCollideData"},
        {sym::nativeGetWaitSuccessData, "getWaitSuccessData"},
        {sym::nativeGetWallRideEndData, "getWallRideEndData"},
        {sym::nativeGetWallTapEndData, "getWallTapEndData"},
        {sym::nativeGetScore, "getScore"},
        {sym::nativeGetPotentialScoreIncrease, "getPotentialScoreIncrease"},
        {sym::nativeIsSkaterNotPullingAnyTricks, "isSkaterNotPullingAnyTricks"},
        {sym::nativeIsSkaterJizzed, "isSkaterJizzed"},
        {sym::nativeGetNum180s, "getNum180s"},
        {sym::nativeGetChallengeScore, "getChallengeScore"},
        {sym::nativeSetChallengeScore, "setChallengeScore"},
        {sym::nativeShowChallengeScore, "showChallengeScore"},
        {sym::nativeHideChallengeScore, "hideChallengeScore"},
        {sym::nativeAwardInstantaneousPoints, "awardInstantaneousPoints"},
        {sym::nativeCreateNpcBubble, "createNPCBubble"},
        {sym::nativeDestroyNpcBubble, "destroyNPCBubble"},
        {sym::nativeCreateNpcPointer, "createNPCPointer"},
        {sym::nativeDestroyNpcPointer, "destroyNPCPointer"},
        {sym::nativeAddBonusTrickToTrickQueue, "addBonusTrickToTrickQueue"},
        {sym::nativeGetObbData, "getOBBData"},
        {sym::nativeGetObbPlayer, "getOBBPlayer"},
        {sym::nativeGetGrindStartData, "getLandData"},
        {sym::nativeGetNumOfThisTrickInTrickQueue, "getNumOfThisTrickInTrickQueue"},
        {sym::nativeGetNumOfTricksLikeThisTrickInTrickQueue, "getNumOfTricksLikeThisTrickInTrickQueue"},
        {sym::nativeGetCurrentTrickUid, "getCurrentTrickUID"},
        {sym::nativeSetChildVisible, "setChildVisible"},
        {sym::nativeSetChildProperties, "setChildProperties"},
        {sym::nativeLockNpcFromAi, "lockNPCFromAI"},
        {sym::nativeUnlockNpcFromAi, "unlockNPCFromAI"},
        {sym::nativePlaySound, "playSound"},
        {sym::nativeSetPowerUpState, "setPowerUpState"},
        {sym::nativeCreateTimer, "createTimer"},
        {sym::nativeReleaseTimer, "releaseTimer"},
        {sym::nativeSetTimer, "setTimer"},
        {sym::nativeGetTimerTime, "getTimerTime"},
        {sym::nativeSetTimerState, "setTimerState"},
        {sym::nativeGetTimerState, "getTimerState"},
        {sym::nativeGetMessageDataTimerId, "getMessageDataTimerID"},
        {sym::nativeSetTheVisibleTimer, "setTheVisibleTimer"},
        {sym::nativeGetTheVisibleTimer, "getTheVisibleTimer"},
        {sym::nativeHideTheVisibleTimer, "hideTheVisibleTimer"},
        {sym::nativeExtendTimer, "extendTimer"},
        {sym::nativeIsFakie, "isFakie"},
        {sym::nativeRespawnSkater, "respawnSkater"},
        {sym::nativeRespawnSkaterName, "respawnSkaterName"},
        {sym::nativeSetRespawnLocation, "setRespawnLocation"},
        {sym::nativeParrotHitBase, "parrotHitBase"},
        {sym::nativeParrotStartGame, "parrotStartGame"},
        {sym::nativeParrotEndGame, "parrotEndGame"},
        {sym::nativeParrotGetScore, "parrotGetScore"},
        {sym::nativeSetEmitterState, "setEmitterState"},
        {sym::nativeCreateCameraFlash, "createCameraFlash"},
        {sym::nativeStreamSpeech, "streamSpeech"},
        {sym::nativeStreamPositionalSpeech, "streamPositionalSpeech"},
        {sym::nativeInitializeAudioSync, "initializeAudioSync"},
        {sym::nativeDestroySkidMarks, "destroySkidMarks"},
        {sym::nativeSuspendWeatherEffects, "suspendWeatherEffects"},
        {sym::nativeResumeWeatherEffects, "resumeWeatherEffects"},
        {sym::nativeAwardSecretTrick, "awardSecretTrick"},
        {sym::nativeGetCharacterType, "getCharacterType"},
        {sym::nativeIsKeyAwarded, "isKeyAwarded"},
        {sym::nativeCreateExplosion, "createExplosion"},
        {sym::nativeCreateSplash, "createSplash"},
        {sym::nativeLockSkaters, "lockSkaters"},
        {sym::nativeSetTrafficVisible, "setTrafficVisible"},
        {sym::nativeResetJuice, "resetJuice"},
        {sym::nativeSetVehicleVolume, "setVehicleVolume"},
        {sym::nativeSoundSourceSetState, "soundSourceSetState"},
        {sym::activateParkEditor, "activateParkEditor"},
        {sym::deactivateParkEditor, "deactivateParkEditor"},
        {sym::cleanParkEditor, "cleanParkEditor"},
        {sym::parkEditorAllEditorPiecesTouched, "parkEditorAllEditorPiecesTouched"},
        {sym::parkEditorResetTouchesPiece, "parkEditorResetTouchesPiece"},
    };
    const size_t kNativeNameCount = sizeof(kNativeNames) / sizeof(kNativeNames[0]);

    namespace
    {
        // zge::nameHash on the host, for names that are not in EE memory.
        uint32_t hostNameHash(std::string_view name)
        {
            uint32_t hash = 0;
            for (char c : name)
            {
                uint8_t letter = static_cast<uint8_t>(c);
                if (letter >= 'a' && letter <= 'z')
                    letter = static_cast<uint8_t>(letter - 'a' + 'A');
                hash = (hash << 4) + letter;
                const uint32_t high = hash & 0xF0000000u;
                if (high)
                    hash ^= high >> 24;
                hash &= ~high;
            }
            return hash;
        }
    }

    const char *nativeName(uint32_t nameHash)
    {
        for (const NativeName &native : kNativeNames)
            if (hostNameHash(native.name) == nameHash)
                return native.name;
        return nullptr;
    }
}
