// DM2's in-game overlay: the reward display (kOverlayReward,
// "InGameUI.Overlay.DisplayReward"): six items, each a line of text with an
// icon letter. A reward sent takes the first free item; the items fade in
// one at a time, each with its kind's sound. DM2's own code (overlay.c).

#include "frames.h"

#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/frame_reward.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kRewardDisplay{sym::g_textDisplayRewardPath};   // "InGameUI.Overlay.DisplayReward"
        constexpr ee::Ptr<const char> kRewardItemFormat{sym::g_textRewardItemFormat}; // "...DisplayReward.Item%02d"
        constexpr ee::Ptr<const char> kDisplayInitAction{sym::g_textOverlayInit};     // "Init"
        constexpr ee::Ptr<const char> kDisplayDeInitAction{sym::g_textOverlayDeInit}; // "DeInit"
        constexpr ee::Ptr<const char> kStateName{sym::g_textState};                   // "State"
        constexpr ee::Ptr<const char> kDescriptionName{sym::g_textDesc};              // "Desc"
        // The rewards' texts: the string table's entry of the kind, or with
        // a count before it; the table's own entry as a format ("KSTR_"
        // with no "$": looked up here); a bail's.
        constexpr ee::Ptr<const char> kRewardText{sym::g_textRewardFormat};               // "$KSTR_REWARD%02d"
        constexpr ee::Ptr<const char> kCountedRewardText{sym::g_textCountedRewardFormat}; // "%d-$KSTR_REWARD%02d"
        constexpr ee::Ptr<const char> kRewardTextName{sym::g_textRewardName};             // "KSTR_REWARD%02d"
        constexpr ee::Ptr<const char> kFallText{sym::g_textFallFormat};                   // "#RGB{200 32 32}$KSTR_FALL_%02d"
        constexpr ee::Ptr<const char> kRespectPointsFormat{sym::g_textRespectPointsFormat}; // "\n+%d respect points!"
        constexpr ee::Ptr<const char> kDistanceSeparator{sym::g_textRewardSeparator};     // " : "
        constexpr ee::Ptr<const char> kLetterFormat{sym::g_textCharFormat};               // "%c"

        // The display's items: their widgets and what each shows.
        constexpr uint32_t kRewardItems = 6;
        inline ee::Global<ee::Ptr<zge::zgui::Node>[kRewardItems], sym::g_rewardItemWidgets> g_rewardItemWidgets;
        inline ee::Global<OverlayReward[kRewardItems], sym::g_rewardItems> g_rewardItems;

        // An item's state: free, or waiting for its turn to fade in; from
        // there on it is the widget's own "State" variable, which the
        // widget's script moves on (2 and 3 while it is busy, 0 at its end).
        constexpr uint16_t kRewardFree = 0;
        constexpr uint16_t kRewardWaiting = 1;
        constexpr uint16_t kRewardFadingIn = 2;
        constexpr uint32_t kWidgetBusyStates = 2; // kRewardFadingIn and the one after it

        // The reward kinds, as far as the display tells them apart. Each
        // kind has an icon: its letter in the items' font.
        constexpr uint8_t kRewardKindsWithPoints = 3; // kinds 3 .. 10 show "+N respect points!" below their text
        constexpr uint8_t kRewardKindsWithDistance = 22; // 22 .. 28: the text, " : " and a distance in feet and inches
        constexpr uint32_t kRewardKinds = 29;
        inline ee::Global<char[kRewardKinds], sym::g_rewardLetters> g_rewardLetters; // "AAAaaaaaaaaddbgefcccccccccccc"

        // The sound an item fades in with, by its kind.
        constexpr uint32_t kRewardSoundKinds0To2 = 0x2C;
        constexpr uint32_t kRewardSoundKinds3To5 = 0x2D;
        constexpr uint32_t kRewardSoundKinds6To10 = 0x2E;
        constexpr uint32_t kRewardSoundKinds11To13 = 0x31;
        constexpr uint32_t kRewardSoundOtherKinds = 0x30;
        uint32_t rewardSound(uint8_t kind)
        {
            if (kind <= 2)
                return kRewardSoundKinds0To2;
            if (kind <= 5)
                return kRewardSoundKinds3To5;
            if (kind <= 10)
                return kRewardSoundKinds6To10;
            if (kind <= 13)
                return kRewardSoundKinds11To13;
            return kRewardSoundOtherKinds;
        }


        // The gaps each level has (gaps.cpp), and the level being played.
        constexpr uint32_t kLevelGapCountLevels = 9;
        inline ee::Global<uint8_t[kLevelGapCountLevels], sym::g_levelGapCounts> g_levelGapCounts;
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
    }

    // As the overlay starts, the items' widgets are found and freed, and
    // the display plays its "Init"; as the overlay ends, its "DeInit".
    void overlayRewardSetUp(uint32_t starting)
    {
        ee::Local<char[64]> path;
        const ee::Ptr<zge::zgui::Node> display = uiWidget(kRewardDisplay);
        if (starting == 0)
        {
            playWidgetAction(display, kDisplayDeInitAction);
            return;
        }
        for (uint32_t i = 0; i < kRewardItems; ++i)
        {
            zge::formatString(path, kRewardItemFormat, i);
            (*g_rewardItemWidgets)[i] = uiWidget(path);
            OverlayReward &item = (*g_rewardItems)[i];
            item.state = kRewardFree;
            item.kind = kOverlayNone;
        }
        playWidgetAction(display, kDisplayInitAction);
    }
    READABLE_FUNCTION(overlayRewardSetUp, Verified);

    // Each frame: a waiting item fades in, with its kind's sound, once no
    // other item's widget is busy; every other item's state follows its
    // widget's "State".
    void overlayRewardFrame()
    {
        ee::Local<uint32_t> widgetState;
        for (uint32_t i = 0; i < kRewardItems; ++i)
        {
            OverlayReward &item = (*g_rewardItems)[i];
            const ee::Ptr<zge::zgui::Node> widget = (*g_rewardItemWidgets)[i];
            if (item.state != kRewardWaiting)
            {
                zge::zgui::copyVariable(widget, kStateName, widgetState);
                item.state = static_cast<uint16_t>(*widgetState);
                continue;
            }
            bool anotherBusy = false;
            for (uint32_t other = 0; other < kRewardItems; ++other)
            {
                if (other == i)
                    continue;
                zge::zgui::copyVariable((*g_rewardItemWidgets)[other], kStateName, widgetState);
                if (*widgetState - kRewardFadingIn < kWidgetBusyStates)
                {
                    anotherBusy = true;
                    break;
                }
            }
            if (anotherBusy)
                continue;
            playWidgetAction(widget, kFadeInAction);
            soundEvent(rewardSound(item.kind), 0, 0, 0, 0);
            item.state = kRewardFadingIn;
        }
    }
    READABLE_FUNCTION(overlayRewardFrame, Verified);

    void overlayRewardShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = shown;
        zge::zgui::setVariable(uiWidget(kRewardDisplay), kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayRewardShow, Verified);

    // A reward sent: the first free item takes it (none free: it is
    // dropped), its description and icon letter are written, and it waits
    // for its turn to fade in. kOverlayNone hides and frees every item.
    void overlayRewardMessage(ee::Ptr<const OverlayReward> reward)
    {
        ee::Local<char[256]> text;
        ee::Local<char[kRewardKinds]> letters;
        ee::Local<uint32_t> visible;
        for (uint32_t i = 0; i < kRewardKinds; ++i)
            letters[i] = (*g_rewardLetters)[i];

        if (reward->kind == kOverlayNone)
        {
            visible = 0;
            for (uint32_t i = 0; i < kRewardItems; ++i)
            {
                zge::zgui::setVariable((*g_rewardItemWidgets)[i], kVisibleName, visible);
                (*g_rewardItems)[i].state = kRewardFree;
            }
            return;
        }

        uint32_t free = 0;
        while ((*g_rewardItems)[free].state != kRewardFree)
        {
            if (++free >= kRewardItems)
                return;
        }
        OverlayReward &item = (*g_rewardItems)[free];
        const ee::Ptr<zge::zgui::Node> widget = (*g_rewardItemWidgets)[free];
        zge::memoryCopy(&item, reward, sizeof(OverlayReward));

        if (item.kind == kRewardRunTricks)
        {
            zge::formatString(text, kCountedRewardText, sce::libgcc::fptoui(item.value), kRewardRunTricks);
        }
        else if (item.kind == kRewardFall)
        {
            zge::formatString(text, kFallText, sce::libgcc::fptoui(item.value));
        }
        else if (item.kind == kRewardGapFound || item.kind == kRewardAllGapsFound)
        {
            // The string table's text is the format here: the count and
            // how many gaps the level has.
            zge::formatString(text, kRewardTextName, item.kind);
            const ee::Ptr<const char> format = zge::stringTableText(zge::nameHash(text));
            const uint32_t count = sce::libgcc::fptoui(item.value);
            zge::formatString(text, format, count, (*g_levelGapCounts)[*g_sessionLevel]);
        }
        else
        {
            zge::formatString(text, kRewardText, item.kind);
        }

        if (item.kind >= kRewardKindsWithPoints && item.kind < kRewardGapFound)
        {
            const ee::Ptr<char> end = text + zge::stringLength(text);
            zge::formatString(end, kRespectPointsFormat, sce::libgcc::fptoui(item.value));
        }
        else if (item.kind >= kRewardKindsWithDistance && item.kind < kRewardKinds)
        {
            zge::formatString(text + zge::stringLength(text), kDistanceSeparator);
            formatFeetAndInches(text + zge::stringLength(text), item.value);
        }
        zge::zgui::setVariable(zge::zgui::findNode(widget, kDescriptionName, 0), kTextName, text);

        zge::formatString(text, kLetterFormat, static_cast<int32_t>(letters[item.kind]));
        zge::zgui::setVariable(widget, kTextName, text);
        item.state = kRewardWaiting;
    }
    READABLE_FUNCTION(overlayRewardMessage, Verified);
}
