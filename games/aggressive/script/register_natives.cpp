// The natives registered at boot (registerScriptNatives), by the original's name
// strings and in the original's order, and the child states' pool.

#include "child_states.h"
#include "script.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/script/register_natives.cpp.h"

namespace aggressive::script
{
    namespace
    {
        constexpr uint32_t kChildStateBlocks = 0x40u;

        inline ee::Global<uint32_t, sym::g_hidePointsTimer> g_hidePointsTimer;
        inline ee::Global<uint32_t, sym::g_childStateCount> g_childStateCount;
        inline ee::Global<ee::Ptr<ChildState>, sym::g_childStateFirst> g_childStateFirst;
        inline ee::Global<ee::Ptr<ChildState>, sym::g_childStateLast> g_childStateLast;
        inline ee::Global<uint32_t, sym::g_levelScriptPool> g_levelScriptPool;

        struct Registration
        {
            uint32_t name, function, returnsTo;
        };
        constexpr Registration kRegistrations[] = {
            {sym::g_textPlayAnim, sym::nativePlayAnim, points::registerScriptNatives::afterScriptRegisterNative}, // "playAnim"
            {sym::g_textStopAnim, sym::nativeStopAnim, points::registerScriptNatives::afterScriptRegisterNative2}, // "stopAnim"
            {sym::g_textConsolePrintString, sym::nativeConsolePrintString, points::registerScriptNatives::afterScriptRegisterNative3}, // "consolePrintString"
            {sym::g_textConsolePrintInt, sym::nativeConsolePrintInt, points::registerScriptNatives::afterScriptRegisterNative4}, // "consolePrintInt"
            {sym::g_textConsolePrintFloat, sym::nativeConsolePrintFloat, points::registerScriptNatives::afterScriptRegisterNative5}, // "consolePrintFloat"
            {sym::g_textConsolePrintHash, sym::nativeConsolePrintHash, points::registerScriptNatives::afterScriptRegisterNative6}, // "consolePrintHash"
            {sym::g_textConsolePrintNewLine, sym::nativeConsolePrintNewLine, points::registerScriptNatives::afterScriptRegisterNative7}, // "consolePrintNewLine"
            {sym::g_textExtractFloatFromTagData, sym::nativeExtractFloatFromTagData, points::registerScriptNatives::afterScriptRegisterNative8}, // "extractFloatFromTagData"
            {sym::g_textDisplayObjectiveText, sym::nativeDisplayObjectiveText, points::registerScriptNatives::afterScriptRegisterNative9}, // "displayObjectiveText"
            {sym::g_textStopObjective, sym::nativeStopObjective, points::registerScriptNatives::afterScriptRegisterNative10}, // "stopObjective"
            {sym::g_textAddTrick, sym::nativeAddTrick, points::registerScriptNatives::afterScriptRegisterNative11}, // "addTrick"
            {sym::g_textPlayTrickScore, sym::nativePlayTrickScore, points::registerScriptNatives::afterScriptRegisterNative12}, // "playTrickScore"
            {sym::g_textDisplayObjectiveTitle, sym::nativeDisplayObjectiveTitle, points::registerScriptNatives::afterScriptRegisterNative13}, // "displayObjectiveTitle"
            {sym::g_textShatterObjective, sym::nativeShatterObjective, points::registerScriptNatives::afterScriptRegisterNative14}, // "shatterObjective"
            {sym::g_textPlayCameraAnim, sym::nativePlayCameraAnim, points::registerScriptNatives::afterScriptRegisterNative15}, // "playCameraAnim"
            {sym::g_textPlayCameraAnimWithFOV, sym::nativePlayCameraAnimWithFov, points::registerScriptNatives::afterScriptRegisterNative16}, // "playCameraAnimWithFOV"
            {sym::g_textPlayCameraAnimTarget, sym::nativePlayCameraAnimTarget, points::registerScriptNatives::afterScriptRegisterNative17}, // "playCameraAnimTarget"
            {sym::g_textNewRecord, sym::nativeNewRecord, points::registerScriptNatives::afterScriptRegisterNative18}, // "newRecord"
            {sym::g_textShowAcidDrop, sym::nativeShowAcidDrop, points::registerScriptNatives::afterScriptRegisterNative19}, // "showAcidDrop"
            {sym::g_textSetRuler, sym::nativeSetRuler, points::registerScriptNatives::afterScriptRegisterNative20}, // "setRuler"
            {sym::g_textShowRuler, sym::nativeShowRuler, points::registerScriptNatives::afterScriptRegisterNative21}, // "showRuler"
            {sym::g_textSetObjectiveState, sym::nativeSetObjectiveState, points::registerScriptNatives::afterScriptRegisterNative22}, // "setObjectiveState"
            {sym::g_textShowObjCurr, sym::nativeShowObjCurr, points::registerScriptNatives::afterScriptRegisterNative23}, // "showObjCurr"
            {sym::g_textFadeObjCurr, sym::nativeFadeObjCurr, points::registerScriptNatives::afterScriptRegisterNative24}, // "fadeObjCurr"
            {sym::g_textDisplayOverlayText, sym::nativeDisplayOverlayText, points::registerScriptNatives::afterScriptRegisterNative25}, // "displayOverlayText"
            {sym::g_textSetBonusObjectiveState, sym::nativeSetBonusObjectiveState, points::registerScriptNatives::afterScriptRegisterNative26}, // "setBonusObjectiveState"
            {sym::g_textBreakPoint, sym::nativeBreakPoint, points::registerScriptNatives::afterScriptRegisterNative27}, // "breakPoint"
            {sym::g_textSetObjectVisible, sym::nativeSetObjectVisible, points::registerScriptNatives::afterScriptRegisterNative28}, // "setObjectVisible"
            {sym::g_textPauseAnim, sym::nativePauseAnim, points::registerScriptNatives::afterScriptRegisterNative29}, // "pauseAnim"
            {sym::g_textResumeAnim, sym::nativeResumeAnim, points::registerScriptNatives::afterScriptRegisterNative30}, // "resumeAnim"
            {sym::g_textGetTextFromAnimEventData, sym::nativeGetTextFromAnimEventData, points::registerScriptNatives::afterScriptRegisterNative31}, // "getTextFromAnimEventData"
            {sym::g_textGetGrindStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative32}, // "getGrindStartData"
            {sym::g_textGetManualStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative33}, // "getManualStartData"
            {sym::g_textGetStallStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative34}, // "getStallStartData"
            {sym::g_textGetOverStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative35}, // "getOverStartData"
            {sym::g_textGetOverEndData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative36}, // "getOverEndData"
            {sym::g_textGetWallRideStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative37}, // "getWallRideStartData"
            {sym::g_textGetWallTapStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative38}, // "getWallTapStartData"
            {sym::g_textGetOnStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative39}, // "getOnStartData"
            {sym::g_textGetOnEndData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative40}, // "getOnEndData"
            {sym::g_textGetJumpData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative41}, // "getJumpData"
            {sym::g_textGetSwingData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative42}, // "getSwingData"
            {sym::g_textGetVaultData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative43}, // "getVaultData"
            {sym::g_textGetSlideStartData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative44}, // "getSlideStartData"
            {sym::g_textGetSlideEndData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative45}, // "getSlideEndData"
            {sym::g_textGetSkitchData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative46}, // "getSkitchData"
            {sym::g_textGetGrindEndData, sym::nativeGetGrindEndData, points::registerScriptNatives::afterScriptRegisterNative47}, // "getGrindEndData"
            {sym::g_textGetManualEndData, sym::nativeGetGrindEndData, points::registerScriptNatives::afterScriptRegisterNative48}, // "getManualEndData"
            {sym::g_textGetAcidDropLandData, sym::nativeGetGrindEndData, points::registerScriptNatives::afterScriptRegisterNative49}, // "getAcidDropLandData"
            {sym::g_textGetStallEndData, sym::nativeGetStallEndData, points::registerScriptNatives::afterScriptRegisterNative50}, // "getStallEndData"
            {sym::g_textGetCollideData, sym::nativeGetCollideData, points::registerScriptNatives::afterScriptRegisterNative51}, // "getCollideData"
            {sym::g_textGetWaitSuccessData, sym::nativeGetWaitSuccessData, points::registerScriptNatives::afterScriptRegisterNative52}, // "getWaitSuccessData"
            {sym::g_textGetWallRideEndData, sym::nativeGetWallRideEndData, points::registerScriptNatives::afterScriptRegisterNative53}, // "getWallRideEndData"
            {sym::g_textGetWallTapEndData, sym::nativeGetWallTapEndData, points::registerScriptNatives::afterScriptRegisterNative54}, // "getWallTapEndData"
            {sym::g_textGetScore2, sym::nativeGetScore, points::registerScriptNatives::afterScriptRegisterNative55}, // "getScore"
            {sym::g_textGetPotentialScoreIncrease, sym::nativeGetPotentialScoreIncrease, points::registerScriptNatives::afterScriptRegisterNative56}, // "getPotentialScoreIncrease"
            {sym::g_textIsSkaterNotPullingAnyTricks, sym::nativeIsSkaterNotPullingAnyTricks, points::registerScriptNatives::afterScriptRegisterNative57}, // "isSkaterNotPullingAnyTricks"
            {sym::g_textIsSkaterJizzed, sym::nativeIsSkaterJizzed, points::registerScriptNatives::afterScriptRegisterNative58}, // "isSkaterJizzed"
            {sym::g_textGetNum180s, sym::nativeGetNum180s, points::registerScriptNatives::afterScriptRegisterNative59}, // "getNum180s"
            {sym::g_textGetChallengeScore, sym::nativeGetChallengeScore, points::registerScriptNatives::afterScriptRegisterNative60}, // "getChallengeScore"
            {sym::g_textSetChallengeScore, sym::nativeSetChallengeScore, points::registerScriptNatives::afterScriptRegisterNative61}, // "setChallengeScore"
            {sym::g_textShowChallengeScore, sym::nativeShowChallengeScore, points::registerScriptNatives::afterScriptRegisterNative62}, // "showChallengeScore"
            {sym::g_textHideChallengeScore, sym::nativeHideChallengeScore, points::registerScriptNatives::afterScriptRegisterNative63}, // "hideChallengeScore"
            {sym::g_textAwardInstantaneousPoints, sym::nativeAwardInstantaneousPoints, points::registerScriptNatives::afterScriptRegisterNative64}, // "awardInstantaneousPoints"
            {sym::g_textCreateNPCBubble, sym::nativeCreateNpcBubble, points::registerScriptNatives::afterScriptRegisterNative65}, // "createNPCBubble"
            {sym::g_textDestroyNPCBubble, sym::nativeDestroyNpcBubble, points::registerScriptNatives::afterScriptRegisterNative66}, // "destroyNPCBubble"
            {sym::g_textCreateNPCPointer, sym::nativeCreateNpcPointer, points::registerScriptNatives::afterScriptRegisterNative67}, // "createNPCPointer"
            {sym::g_textDestroyNPCPointer, sym::nativeDestroyNpcPointer, points::registerScriptNatives::afterScriptRegisterNative68}, // "destroyNPCPointer"
            {sym::g_textAddBonusTrickToTrickQueue, sym::nativeAddBonusTrickToTrickQueue, points::registerScriptNatives::afterScriptRegisterNative69}, // "addBonusTrickToTrickQueue"
            {sym::g_textGetOBBData, sym::nativeGetObbData, points::registerScriptNatives::afterScriptRegisterNative70}, // "getOBBData"
            {sym::g_textGetOBBPlayer, sym::nativeGetObbPlayer, points::registerScriptNatives::afterScriptRegisterNative71}, // "getOBBPlayer"
            {sym::g_textGetLandData, sym::nativeGetGrindStartData, points::registerScriptNatives::afterScriptRegisterNative72}, // "getLandData"
            {sym::g_textGetNumOfThisTrickInTrickQueue, sym::nativeGetNumOfThisTrickInTrickQueue, points::registerScriptNatives::afterScriptRegisterNative73}, // "getNumOfThisTrickInTrickQueue"
            {sym::g_textGetNumOfTricksLikeThisTrickInTrick, sym::nativeGetNumOfTricksLikeThisTrickInTrickQueue, points::registerScriptNatives::afterScriptRegisterNative74}, // "getNumOfTricksLikeThisTrickInTrickQueue"
            {sym::g_textGetCurrentTrickUID, sym::nativeGetCurrentTrickUid, points::registerScriptNatives::afterScriptRegisterNative75}, // "getCurrentTrickUID"
            {sym::g_textSetChildVisible, sym::nativeSetChildVisible, points::registerScriptNatives::afterScriptRegisterNative76}, // "setChildVisible"
            {sym::g_textSetChildProperties, sym::nativeSetChildProperties, points::registerScriptNatives::afterScriptRegisterNative77}, // "setChildProperties"
            {sym::g_textLockNPCFromAI, sym::nativeLockNpcFromAi, points::registerScriptNatives::afterScriptRegisterNative78}, // "lockNPCFromAI"
            {sym::g_textUnlockNPCFromAI, sym::nativeUnlockNpcFromAi, points::registerScriptNatives::afterScriptRegisterNative79}, // "unlockNPCFromAI"
            {sym::g_textPlaySound, sym::nativePlaySound, points::registerScriptNatives::afterScriptRegisterNative80}, // "playSound"
            {sym::g_textSetPowerUpState, sym::nativeSetPowerUpState, points::registerScriptNatives::afterScriptRegisterNative81}, // "setPowerUpState"
            {sym::g_textCreateTimer, sym::nativeCreateTimer, points::registerScriptNatives::afterScriptRegisterNative82}, // "createTimer"
            {sym::g_textReleaseTimer, sym::nativeReleaseTimer, points::registerScriptNatives::afterScriptRegisterNative83}, // "releaseTimer"
            {sym::g_textSetTimer, sym::nativeSetTimer, points::registerScriptNatives::afterScriptRegisterNative84}, // "setTimer"
            {sym::g_textGetTimerTime, sym::nativeGetTimerTime, points::registerScriptNatives::afterScriptRegisterNative85}, // "getTimerTime"
            {sym::g_textSetTimerState, sym::nativeSetTimerState, points::registerScriptNatives::afterScriptRegisterNative86}, // "setTimerState"
            {sym::g_textGetTimerState, sym::nativeGetTimerState, points::registerScriptNatives::afterScriptRegisterNative87}, // "getTimerState"
            {sym::g_textGetMessageDataTimerID, sym::nativeGetMessageDataTimerId, points::registerScriptNatives::afterScriptRegisterNative88}, // "getMessageDataTimerID"
            {sym::g_textSetTheVisibleTimer, sym::nativeSetTheVisibleTimer, points::registerScriptNatives::afterScriptRegisterNative89}, // "setTheVisibleTimer"
            {sym::g_textGetTheVisibleTimer, sym::nativeGetTheVisibleTimer, points::registerScriptNatives::afterScriptRegisterNative90}, // "getTheVisibleTimer"
            {sym::g_textHideTheVisibleTimer, sym::nativeHideTheVisibleTimer, points::registerScriptNatives::afterScriptRegisterNative91}, // "hideTheVisibleTimer"
            {sym::g_textExtendTimer, sym::nativeExtendTimer, points::registerScriptNatives::afterScriptRegisterNative92}, // "extendTimer"
            {sym::g_textIsFakie, sym::nativeIsFakie, points::registerScriptNatives::afterScriptRegisterNative93}, // "isFakie"
            {sym::g_textRespawnSkater, sym::nativeRespawnSkater, points::registerScriptNatives::afterScriptRegisterNative94}, // "respawnSkater"
            {sym::g_textRespawnSkaterName, sym::nativeRespawnSkaterName, points::registerScriptNatives::afterScriptRegisterNative95}, // "respawnSkaterName"
            {sym::g_textSetRespawnLocation, sym::nativeSetRespawnLocation, points::registerScriptNatives::afterScriptRegisterNative96}, // "setRespawnLocation"
            {sym::g_textParrotHitBase, sym::nativeParrotHitBase, points::registerScriptNatives::afterScriptRegisterNative97}, // "parrotHitBase"
            {sym::g_textParrotStartGame, sym::nativeParrotStartGame, points::registerScriptNatives::afterScriptRegisterNative98}, // "parrotStartGame"
            {sym::g_textParrotEndGame, sym::nativeParrotEndGame, points::registerScriptNatives::afterScriptRegisterNative99}, // "parrotEndGame"
            {sym::g_textParrotGetScore, sym::nativeParrotGetScore, points::registerScriptNatives::afterScriptRegisterNative100}, // "parrotGetScore"
            {sym::g_textSetEmitterState, sym::nativeSetEmitterState, points::registerScriptNatives::afterScriptRegisterNative101}, // "setEmitterState"
            {sym::g_textCreateCameraFlash, sym::nativeCreateCameraFlash, points::registerScriptNatives::afterScriptRegisterNative102}, // "createCameraFlash"
            {sym::g_textStreamSpeech, sym::nativeStreamSpeech, points::registerScriptNatives::afterScriptRegisterNative103}, // "streamSpeech"
            {sym::g_textStreamPositionalSpeech, sym::nativeStreamPositionalSpeech, points::registerScriptNatives::afterScriptRegisterNative104}, // "streamPositionalSpeech"
            {sym::g_textInitializeAudioSync, sym::nativeInitializeAudioSync, points::registerScriptNatives::afterScriptRegisterNative105}, // "initializeAudioSync"
            {sym::g_textDestroySkidMarks, sym::nativeDestroySkidMarks, points::registerScriptNatives::afterScriptRegisterNative106}, // "destroySkidMarks"
            {sym::g_textSuspendWeatherEffects, sym::nativeSuspendWeatherEffects, points::registerScriptNatives::afterScriptRegisterNative107}, // "suspendWeatherEffects"
            {sym::g_textResumeWeatherEffects, sym::nativeResumeWeatherEffects, points::registerScriptNatives::afterScriptRegisterNative108}, // "resumeWeatherEffects"
            {sym::g_textAwardSecretTrick, sym::nativeAwardSecretTrick, points::registerScriptNatives::afterScriptRegisterNative109}, // "awardSecretTrick"
            {sym::g_textGetCharacterType, sym::nativeGetCharacterType, points::registerScriptNatives::afterScriptRegisterNative110}, // "getCharacterType"
            {sym::g_textIsKeyAwarded, sym::nativeIsKeyAwarded, points::registerScriptNatives::afterScriptRegisterNative111}, // "isKeyAwarded"
            {sym::g_textCreateExplosion, sym::nativeCreateExplosion, points::registerScriptNatives::afterScriptRegisterNative112}, // "createExplosion"
            {sym::g_textCreateSplash, sym::nativeCreateSplash, points::registerScriptNatives::afterScriptRegisterNative113}, // "createSplash"
            {sym::g_textLockSkaters, sym::nativeLockSkaters, points::registerScriptNatives::afterScriptRegisterNative114}, // "lockSkaters"
            {sym::g_textSetTrafficVisible, sym::nativeSetTrafficVisible, points::registerScriptNatives::afterScriptRegisterNative115}, // "setTrafficVisible"
            {sym::g_textResetJuice, sym::nativeResetJuice, points::registerScriptNatives::afterScriptRegisterNative116}, // "resetJuice"
            {sym::g_textSetVehicleVolume, sym::nativeSetVehicleVolume, points::registerScriptNatives::afterScriptRegisterNative117}, // "setVehicleVolume"
            {sym::g_textSoundSourceSetState, sym::nativeSoundSourceSetState, points::registerScriptNatives::afterScriptRegisterNative118}, // "soundSourceSetState"
        };

        // The original keeps the functions it registers many times in
        // saved registers: s1 the page of the first ones, then
        // getGrindStartData's address, s0 getGrindEndData's from there on.
        constexpr uint32_t kFirstFunctionPage = ee::high(sym::nativePlayAnim);
        constexpr uint32_t kGetGrindStartData = sym::nativeGetGrindStartData;
        constexpr uint32_t kGetGrindEndData = sym::nativeGetGrindEndData;
        constexpr size_t kFirstGrindStartRow = 31, kFirstGrindEndRow = 46;
    }

    void registerScriptNatives()
    {
        *g_hidePointsTimer = 0;
        scriptInitNatives();
        size_t row = 0;
        for (const Registration &native : kRegistrations)
        {
            scriptRegisterNative(ee::Ptr<const char>(native.name), native.function);
            ++row;
        }
        *g_childStateCount = 0;
        *g_childStateFirst = ee::Ptr<ChildState>();
        *g_childStateLast = ee::Ptr<ChildState>();
        *g_levelScriptPool = aggressive::newPool(sizeof(ChildState), kChildStateBlocks, 0);
    }
    READABLE_FUNCTION_WAITS(registerScriptNatives, Verified);
}
