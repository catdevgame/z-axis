// DM2's NPC screen (screens.h; "InGameUI.FrameNPC"): what an NPC the player
// rode up to says, and its script function. DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/camera/camera.h"
#include "dm2/game/goals.h"
#include "dm2/game/npcs.h"
#include "dm2/game/players.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/random.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/screen_npc.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The NPC the screen is about (the one the player met).
        inline ee::Global<uint8_t, sym::g_shownNpc> g_shownNpc;
        // The screen's page (g_screenStep): the NPC has nothing for the
        // player (0), offers a goal (1), or sends the player on (2).
        constexpr uint8_t kPageOffersGoal = 1;
        // The sound the screen closes with (the game's sound event 8).
        constexpr uint32_t kNpcCloseSound = 8;

    }

    namespace
    {
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;

        // The screen's parts and what goes in them.
        constexpr ee::Ptr<const char> kNameLabel{sym::npcNamePath};           // "InGameUI.FrameNPC.NPCName"
        constexpr ee::Ptr<const char> kGoalLevelLabel{sym::npcGoalLevelPath}; // "InGameUI.FrameNPC.GoalLevel"
        constexpr ee::Ptr<const char> kDetailLabel{sym::npcDetailPath};       // "InGameUI.FrameNPC.NPCDetail"
        constexpr ee::Ptr<const char> kChallengeLabel{sym::npcChallengePath}; // "InGameUI.FrameNPC.NPCChallenge"
        constexpr ee::Ptr<const char> kMenuLabel{sym::npcMenuPath};           // "InGameUI.FrameNPC.Menu"
        constexpr ee::Ptr<const char> kTextName{sym::screenTextName};         // "Text"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};           // "Items"
        constexpr uint32_t kContinueItem = sym::npcContinueText;              // "X continue"
        constexpr ee::Ptr<char> kNpcInit{sym::npcInitScript};
        constexpr ee::Ptr<char> kNpcDeInit{sym::npcDeInitScript};
        // The challenges' names ("$KSTRSP_OBJCTV_ROOKIE" ..) and the words of
        // their keys from the second on ("EASY", "HARD", "XTRA", "EXPERT").
        constexpr uint32_t kTierNames = sym::objectiveTierNames;
        constexpr uint32_t kTierWords = sym::objectiveTierWords;
        uint32_t textIn(uint32_t table, uint32_t index)
        {
            return *(ee::Ptr<uint32_t>(table) + static_cast<ptrdiff_t>(index));
        }

        // What the NPC says. Keys with "$" are the UI's to look up; the
        // others are looked up here and are formats themselves. Each comes
        // in four wordings, by the meetings (or at random).
        constexpr uint32_t kRespectFormat = sym::npcRespectFormat;             // "+%d respect"
        constexpr uint32_t kChallengeKey = sym::npcChallengeKey;               // "$KSTRSP_NPC_CHAL_%s_%d"
        constexpr uint32_t kRookieDismissKey = sym::npcRookieDismissKey;       // "$KSTRSP_NPC_DISMISS_ROOKIE_%d"
        constexpr uint32_t kGreetingKey = sym::npcGreetingKey;                 // "KSTRSP_NPC_DISMISS_GREETING_%d"
        constexpr uint32_t kRespectKey = sym::npcRespectKey;                   // "KSTRSP_NPC_RESPECT_%d"
        constexpr uint32_t kOtherNpcKey = sym::npcOtherNpcKey;                 // "KSTRSP_NPC_DISMISS_NPCNAME_%d"
        constexpr uint32_t kLocationKey = sym::npcLocationKey;                 // "KSTRSP_NPC_LOCATION_LEV%02d_%d"
        constexpr uint32_t kOtherLocationKey = sym::npcOtherLocationKey;       // "KSTRSP_NPC_DISMISS_LOCATION_%d"
        constexpr uint32_t kAngryKey = sym::npcAngryKey;                       // "KSTRSP_NPC_ANGRY_%d"
        constexpr uint32_t kRookieGoalKey = sym::npcRookieGoalKey;             // "$KSTRSP_GOAL_LEV%02d_ROOKIE"
        constexpr uint32_t kGoalKey = sym::npcGoalKey;                         // "$KSTRSP_GOAL_LEV%02d_%s_%d"
        constexpr uint32_t kWordings = 4;
        // Met this often, an NPC that has no goal for the player may get
        // angry (one time in eight).
        constexpr uint8_t kAngryMeetings = 15;
        constexpr uint32_t kAngryOneIn = 8;
        // The NPC's pages (g_screenStep), by its state's second level.
        constexpr uint8_t kPageNothing = 0, kPageSendsOn = 2;
        // (By its second level: npcs.h's kNpcOffersGoal, kNpcChallengeAhead, kNpcGoalsBehind.)

        // The string table's text for a key put together in `key`.
        ee::Address keyText(ee::Ptr<char> key)
        {
            return zge::stringTableText(zge::nameHash(key));
        }
        uint32_t wording(uint32_t meetings)
        {
            return meetings & (kWordings - 1);
        }
    }

    // showNpcScreen (screen 27's show handler): as it opens, the NPC the
    // player met: its name and its challenge, and by its state what it says
    // and offers (the page). One with a goal of the player's challenge
    // offers it (shown from now on): its respect, its challenge's words and
    // the goal. A rookie is dismissed; else the NPC greets the player by one
    // of the character's names, maybe says the player has its respect, and
    // names the NPC with the player's challenge and where it is; met often
    // enough it may be angry instead. Then Init. As it closes, DeInit.
    void showNpcScreen(uint32_t shown)
    {
        ee::Local<char[0x200]> text;
        ee::Local<char[0x200]> key;
        ee::Local<char[0x40]> place;
        ee::Local<NpcInfo> info;
        ee::Local<NpcInfo> other;
        if (shown == 0)
        {
            runUiScript(kNpcDeInit);
            return;
        }
        uint32_t goal = 0;
        npcInfo(*g_shownNpc, info);
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(*g_currentPlayer, *g_sessionLevel);
        const ee::Ptr<RiderCharacter> character = playerCharacter(*g_currentPlayer);
        // (A state past kNpcGoalsBehind leaves the page as it was; the
        // NPCs' states never have one.)
        switch (npcState(*g_shownNpc, 1))
        {
        case kNpcChallengeAhead:
            (*g_screenStep)[0] = kPageNothing;
            break;
        case kNpcOffersGoal:
            (*g_screenStep)[0] = kPageOffersGoal;
            goal = info->goal;
            goals->challenges[goals->challenge].goals[goal].flags |= kGoalRevealed;
            break;
        case kNpcGoalsBehind:
            (*g_screenStep)[0] = kPageSendsOn;
            break;
        default:
            break;
        }
        zge::zgui::setVariable(uiWidget(kNameLabel), kTextName, info->name);
        zge::zgui::setVariable(uiWidget(kGoalLevelLabel), kTextName, textIn(kTierNames, info->challenge));
        if ((*g_screenStep)[0] == kPageOffersGoal)
        {
            zge::formatString(text, kRespectFormat, (*g_goalDefinitions)[*g_sessionLevel][goals->challenge][goal].respect);
            zge::zgui::setVariable(uiWidget(kDetailLabel), kTextName, text);
        }

        const uint32_t meetings = info->meetings;
        if ((*g_screenStep)[0] == kPageOffersGoal)
        {
            zge::formatString(text, kChallengeKey, textIn(kTierWords, goals->challenge - 1), wording(meetings));
        }
        else if (goals->challenge == 0)
        {
            zge::formatString(text, kRookieDismissKey, wording(meetings));
        }
        else if (meetings >= kAngryMeetings && (zge::randomNumber() & (kAngryOneIn - 1)) == 0)
        {
            // (Unlike the other keys set as the text, this one has no "$".)
            zge::formatString(text, kAngryKey, wording(meetings));
        }
        else
        {
            text[0] = 0;
            const uint32_t name = zge::randomNumber() & (kWordings - 1);
            const ee::Ptr<char> called = name == 2 ? ee::Ptr<char>(character->lastName)
                                       : name == 3 ? ee::Ptr<char>(character->nickname)
                                                   : ee::Ptr<char>(character->firstName);
            zge::formatString(key, kGreetingKey, wording(meetings));
            zge::formatString(text, keyText(key), called);
            const bool sendsOn = (*g_screenStep)[0] == kPageSendsOn;
            if (sendsOn)
            {
                zge::formatString(key, kRespectKey, wording(meetings));
                zge::formatString(text + zge::stringLength(text), keyText(key));
            }
            if (!sendsOn || info->challenge != goals->challenge)
            {
                npcInfo(npcForChallenge(goals->challenge), other);
                zge::formatString(key, kOtherNpcKey, zge::randomNumber() & (kWordings - 1));
                zge::formatString(text + zge::stringLength(text), keyText(key), other->name);
                zge::formatString(key, kLocationKey, *g_sessionLevel, goals->challenge - 1);
                zge::stringCopy(place, keyText(key));
                zge::formatString(key, kOtherLocationKey, zge::randomNumber() & (kWordings - 1));
                zge::formatString(text + zge::stringLength(text), keyText(key), place);
            }
        }
        zge::zgui::setVariable(uiWidget(kDetailLabel), kTextName, text);

        ee::Address challengeText;
        if ((*g_screenStep)[0] == kPageOffersGoal)
        {
            if (goals->challenge == 0)
                zge::formatString(text, kRookieGoalKey, *g_sessionLevel);
            else
                zge::formatString(text, kGoalKey, *g_sessionLevel, textIn(kTierWords, goals->challenge - 1), goal);
            challengeText = text;
        }
        zge::zgui::setVariable(uiWidget(kChallengeLabel), kTextName, challengeText);
        zge::zgui::setVariable(uiWidget(kMenuLabel), kItemsName, kContinueItem);
        runUiScript(kNpcInit);
    }
    READABLE_FUNCTION(showNpcScreen, Verified);

    // bmxoverlayNPCHandler: the screen closed. An offered goal becomes the
    // player's; the NPC goes back to waiting at its place, the closing
    // sound plays and the player's camera goes back to following.
    void bmxoverlayNPCHandler()
    {
        ee::Local<NpcInfo> info;
        if ((*g_screenStep)[0] == kPageOffersGoal)
        {
            npcInfo(*g_shownNpc, info);
            giveGoal(*g_currentPlayer, info->goal);
        }
        showGameScreen(kScreenNone);
        setNpcState(*g_shownNpc, 0, kNpcWaiting);
        soundEvent(kNpcCloseSound, 0, 0, 0, 0);
        setCameraMode(*g_currentPlayer, 0, 0);
    }
    READABLE_FUNCTION(bmxoverlayNPCHandler, Verified);
}
