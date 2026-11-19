// BMX XXX's NPR screen (screens.h; "FrameNPR"): what a rider met in the
// level says: a greeting, a challenge (one of the level's goals, activated
// as the screen leaves), or its respect. BMX XXX's own code
// (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/camera/action_camera.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/screen_calls.h"
#include "bmx/rider/rider.h"
#include "zge/scene/objectives.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/random.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_npr.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameNpr = sym::g_textFrameNPR;             // "FrameNPR"
        constexpr uint32_t kNameWidget = sym::g_textNPRName;            // "NPRName"
        constexpr uint32_t kDetailWidget = sym::g_textNPRDetail;        // "NPRDetail"
        constexpr uint32_t kChallengeNameWidget = sym::g_textChallengeName; // "ChallengeName"
        constexpr uint32_t kChallengeWidget = sym::g_textNPRChallenge;  // "NPRChallenge"
        // The texts' keys, each with one of four variants.
        constexpr uint32_t kGreetingKey = sym::bmx::g_textNprGreetingKey;   // "KSTRSP_NPR_DISMISS_GREETING_%d"
        constexpr uint32_t kRespectKey = sym::bmx::g_textNprRespectKey;     // "KSTRSP_NPR_RESPECT_%d"
        constexpr uint32_t kAngryText = sym::bmx::g_textNprAngryKey;        // "KSTRSP_NPR_ANGRY_%d"
        constexpr uint32_t kChallengeText = sym::bmx::g_textNprChallengeKey; // "$KSTRSP_NPR_CHAL_%d"
        constexpr uint32_t kNameKey = sym::bmx::g_textObjectiveNameKey;     // "%s_NAME"
        constexpr uint32_t kDescriptionKey = sym::bmx::g_textObjectiveDescKey; // "%s_DESC"
        constexpr uint32_t kTextVariants = 4;

        // The NPC the game is showing, the one the screen is about (game.c
        // sets it as the player meets one, then opens this screen and turns
        // the camera to it: DM2's name).
        inline ee::Global<uint8_t, sym::g_shownNpc> g_shownNpc;
        // What the rider says (the screen's step), from its state at its
        // second level as the screen enters.
        enum NprStep : uint32_t
        {
            kNprGreeting = 0,
            kNprChallenge = 1, // state 0: it has not challenged the player yet
            kNprRespect = 2,   // state 2
        };
        constexpr uint32_t kNpcTalkLevel = 1;
        // After this many meetings a greeting is, one time in eight, an angry one.
        constexpr uint8_t kMeetingsBeforeAnger = 15;
    }

    void nprScreenMessage(int32_t message)
    {
        ee::Local<char[0x200]> text;
        ee::Local<char[0x200]> key;
        ee::Local<char[0x40]> descriptionKey;
        ee::Local<calls::NpcInfo> info;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameNpr, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenEnter:
        {
            calls::npcFacts(*g_shownNpc, info);
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(*calls::g_currentPlayer, *calls::g_currentLevel);
            const ee::Ptr<RiderCharacter> character = playerCharacter(*calls::g_currentPlayer);
            const uint32_t state = npcState(*g_shownNpc, kNpcTalkLevel);
            if (state == 0)
            {
                *g_screenStep = kNprChallenge;
                goals->marks[info->goal] |= calls::kGoalActive;
            }
            else if (state == kNprRespect)
            {
                *g_screenStep = kNprRespect;
            }
            else
            {
                *g_screenStep = kNprGreeting;
            }
            zge::zgui::setVariable(zge::zgui::findNode(frame, kNameWidget, 1), kTextName, info->name);

            if (*g_screenStep == kNprChallenge)
            {
                zge::formatString(text, kChallengeText, (info->goal + info->meetings) & (kTextVariants - 1));
            }
            else if (info->meetings >= kMeetingsBeforeAnger && (zge::randomNumber() & 7) == 0)
            {
                zge::formatString(text, kAngryText, (info->goal + info->meetings) & (kTextVariants - 1));
            }
            else
            {
                // The greeting names the player's character by its first or
                // its last name, one of the two at random.
                const bool lastName = (zge::randomNumber() & 1) != 0;
                text[0] = '\0';
                const ee::Ptr<char> called{lastName ? &character->lastName[0] : &character->firstName[0]};
                zge::formatString(key, kGreetingKey, info->meetings & (kTextVariants - 1));
                zge::formatString(text, zge::stringTableText(zge::nameHash(key)), called);
                if (*g_screenStep == kNprRespect)
                {
                    zge::formatString(key, kRespectKey, info->meetings & (kTextVariants - 1));
                    const ee::Ptr<char> end = ee::Ptr<char>(text) + zge::stringLength(text);
                    zge::formatString(end, zge::stringTableText(zge::nameHash(key)));
                }
            }
            zge::zgui::setVariable(zge::zgui::findNode(frame, kDetailWidget, 1), kTextName, text);

            // The challenge: the goal's name and its description, from the
            // level's strings.
            text[0] = '\0';
            if (*g_screenStep == kNprChallenge)
            {
                zge::setStringTable(*calls::g_levelStrings);
                const ee::Ptr<zge::ObjectiveInfo> goal =
                    zge::objectiveInfo(calls::objectiveByName(objectives::goalNameHash(*calls::g_currentPlayer, info->goal)));
                zge::formatString(text, kNameKey, goal->stringId);
                const char *name = zge::stringTableText(zge::nameHash(text));
                zge::zgui::setVariable(zge::zgui::findNode(frame, kChallengeNameWidget, 1), kTextName, name);
                zge::formatString(descriptionKey, kDescriptionKey, goal->stringId);
                copyOnOneLine(text, zge::stringTableText(zge::nameHash(descriptionKey)), 0);
                zge::setStringTable(*calls::g_gameStrings);
            }
            else
            {
                zge::zgui::setVariable(zge::zgui::findNode(frame, kChallengeNameWidget, 1), kTextName, text);
            }
            zge::zgui::setVariable(zge::zgui::findNode(frame, kChallengeWidget, 1), kTextName, text);
            playUiNode(frame, kInitAction);
            return;
        }

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenLeft:
            // The challenge is on; the rider goes back to waiting, and the
            // player's rider and camera start over.
            if (*g_screenStep == kNprChallenge)
            {
                calls::npcFacts(*g_shownNpc, info);
                objectives::activatePlayerObjectiveByName(*calls::g_currentPlayer, info->objectiveName);
            }
            setNpcState(*g_shownNpc, 0, calls::kNpcWaiting);
            rider::riderReset(riderIndexOf(*calls::g_currentPlayer), 1);
            rider::riderFrame(riderIndexOf(*calls::g_currentPlayer));
            setCameraMode(playerActionCamera(*calls::g_currentPlayer), 0, 0);
            return;

        case kScreenChosen:
            showGameScreen(kScreenPlay);
            *g_resumeAction = kResumePlay;
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(nprScreenMessage, Verified);
}
