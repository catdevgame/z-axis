// The pause menu's Loot page: the level's special trick, its key and its
// power-up, each with its picture tinted until won, and the counts of the
// level's collectibles and juice extends.

#include "aggressive/career.h"
#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/level/pause_parts.h"
#include "aggressive/script/script.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "aggressive/level/objectives.h"

#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/pause_loot.cpp.h"

namespace aggressive::level
{
    namespace
    {
        using zgui::Node;

        using pause::find;
        using pause::run;
        using pause::setResult;

        constexpr uint32_t kMemset = sym::memset;      // libc

        constexpr uint32_t kLootData = sym::lootData;    // "InGameUI.PauseMenus.Frame.Loot.Data"
        constexpr uint32_t kTangFill = sym::tangFill, kTangExtend = sym::tangExtend, kSpecialTrick = sym::specialTrick, kKey = sym::key, kAttribute = sym::attribute;
        constexpr uint32_t kSetMaterialFormat = sym::setMaterialFormat2; // "SET Material {\"%s\"}"
        constexpr uint32_t kTopLeftColor = sym::topLeftColor3, kTopRightColor = sym::topRightColor3, kBottomLeftColor = sym::bottomLeftColor3;
        constexpr uint32_t kVisible = sym::visible2, kCount = sym::count, kMax = sym::max, kPlayRefresh = sym::g_textPlayRefresh;

        constexpr int32_t kNoKey = 14, kNoPowerUp = 7;

        inline ee::Global<int32_t, sym::g_levelNumber> g_levelNumber;

        // What each level has of the three kinds of loot, and their pictures
        // and bits (LootFlag, by item) in the skater's profile.
        inline ee::Global<int32_t[14], sym::g_pauseKeyOfLevel> g_keyOfLevel;
        inline ee::Global<int32_t[14], sym::g_pausePowerUpOfLevel> g_powerUpOfLevel;
        inline ee::Global<int32_t[14], sym::g_pauseTrickOfLevel> g_trickOfLevel;
        inline ee::Global<uint32_t[14], sym::g_pauseKeyImagePaths> g_keyImages;
        inline ee::Global<uint32_t[7], sym::g_pausePowerUpImagePaths> g_powerUpImages;
        inline ee::Global<uint32_t[20], sym::g_pauseTrickImagePaths> g_trickImages;
        inline ee::Global<uint64_t[14], sym::g_pauseKeyBits> g_keyBits;
        inline ee::Global<uint64_t[7], sym::g_pausePowerUpBits> g_powerUpBits;

        // A picture's tint, as the widgets' colour variables take it.
        struct Tint
        {
            uint8_t bytes[0x10];
        };
        inline ee::Global<Tint, sym::g_wonTint> g_wonTint;
    }

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // inlinePauseTabulateLoot: the Loot page filled for the level. The tints go on
    // TopLeft, TopRight, BottomLeft and then BottomLeft again: the original's
    // fourth should be BottomRight, which keeps its colour.
    void inlinePauseTabulateLoot()
    {
        ee::Local<Tint> unwonTint;
        ee::Local<Tint> wonTint;
        ee::Local<int32_t> keyHidden;
        ee::Local<int32_t> keyShown;
        ee::Local<int32_t> powerUpHidden;
        ee::Local<int32_t> powerUpShown;
        ee::Local<int32_t> value;

        memset(unwonTint, 0, sizeof(Tint));
        const ee::Ptr<zgui::Node> root = *zgui::g_root;
        *wonTint = *g_wonTint;
        unwonTint->bytes[3] = 255;
        const ee::Ptr<zgui::Node> data = find(root, kLootData);
        const int32_t level = *g_levelNumber;
        const int32_t trick = (*g_trickOfLevel)[level];
        const int32_t key = (*g_keyOfLevel)[level];
        const int32_t powerUp = (*g_powerUpOfLevel)[level];
        const ee::Ptr<skater::Player> player = parts::levelPlayer(0);
        const ee::Ptr<SkaterProfile> profile = skater::skaterProfile(player);
        *value = 0;

        const ee::Ptr<zgui::Node> fillWidget = find(data, kTangFill);
        const ee::Ptr<zgui::Node> extendWidget = find(data, kTangExtend);
        const ee::Ptr<zgui::Node> trickWidget = find(data, kSpecialTrick);
        const ee::Ptr<zgui::Node> keyWidget = find(data, kKey);
        const ee::Ptr<zgui::Node> attributeWidget = find(data, kAttribute);

        // A picture's material.
        const auto setPicture = [&](ee::Ptr<zgui::Node> widget, uint32_t image) {
            run(widget, zge::formatScratch(kSetMaterialFormat, image));
        };
        const auto tint = [&](ee::Ptr<zgui::Node> widget, bool won) {
            const ee::Address colour = won ? ee::Address(wonTint) : ee::Address(unwonTint);
            // Stops at a corner refused.
            if (setResult(widget, kTopLeftColor, colour) != 0 && setResult(widget, kTopRightColor, colour) != 0 &&
                setResult(widget, kBottomLeftColor, colour) != 0)
                setResult(widget, kBottomLeftColor, colour);
        };
        const auto setVisible = [&](ee::Ptr<zgui::Node> widget, ee::Local<int32_t> &slot, int32_t visible) {
            *slot = visible;
            setResult(widget, kVisible, slot);
        };
        const auto wonBit = [&](uint64_t bit) { return (profile->lootFlags >> (bit & 0x3F)) & 1; };

        setPicture(trickWidget, (*g_trickImages)[trick]);
        tint(trickWidget, (player->unlockedTricks >> (trick & 0x1F)) & 1);

        if (key == kNoKey)
            setVisible(keyWidget, keyHidden, 0);
        else
        {
            setVisible(keyWidget, keyShown, 1);
            setPicture(keyWidget, (*g_keyImages)[key]);
            tint(keyWidget, wonBit((*g_keyBits)[key]));
        }

        if (powerUp == kNoPowerUp)
            setVisible(attributeWidget, powerUpHidden, 0);
        else
        {
            setVisible(attributeWidget, powerUpShown, 1);
            setPicture(attributeWidget, (*g_powerUpImages)[powerUp]);
            tint(attributeWidget, wonBit((*g_powerUpBits)[powerUp]));
        }

        // The counts, from the skater's record of this level.
        const LevelRecord &record = profile->levels[level];
        const auto setCount = [&](ee::Ptr<zgui::Node> widget, uint32_t name, uint8_t count) {
            *value = count;
            setResult(widget, name, value);
        };
        setCount(fillWidget, kCount, record.collected);
        setCount(fillWidget, kMax, record.collectibles);
        run(fillWidget, kPlayRefresh);
        setCount(extendWidget, kCount, record.juiceExtends);
        setCount(extendWidget, kMax, record.juiceExtendsTotal);
        run(extendWidget, kPlayRefresh);
    }
    READABLE_FUNCTION_WAITS(inlinePauseTabulateLoot, Verified);

    namespace
    {
        constexpr uint32_t kTextInGameUIPauseMenusFrameGapList = sym::g_textInGameUIPauseMenusFrameGapList;
        constexpr uint32_t kNamesHigh = ee::high(sym::g_textCartonSize);
        constexpr uint32_t kTextNames = sym::g_textNames;
        constexpr uint32_t kTextStatus = sym::g_textStatus;
        constexpr uint32_t kTextDone = sym::g_textDone;
        constexpr uint32_t kItems = sym::g_textItems;
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(memset, ee::game, kMemset);
        }
    }

    namespace
    {
        constexpr uint32_t kLineBreak = sym::g_gapListLineBreak;      // "\r\n"
        constexpr uint32_t kNumberFormat = sym::g_gapListNumberFormat; // "%d"
        constexpr uint32_t kColumnBytes = 0x400, kLineBytes = 0x80;
    }

    // inlinePausePopulateGapMenu: the pause menu's gap list (GapList, with
    // its Names and Status) filled with the level's bonus objectives of kind
    // 0: a line each, numbered from 1, its name, and "Done" once done; CR LF
    // between lines. Returns the last setVariable's answer. Nothing calls it
    // (a cut page). The original also keeps the list, the two widgets and
    // the bonus objectives in its frame (spills only).
    int32_t inlinePausePopulateGapMenu()
    {
        ee::Local<char[kColumnBytes]> numbers;
        ee::Local<char[kColumnBytes]> names;
        ee::Local<char[kColumnBytes]> statuses;
        ee::Local<char[kLineBytes]> line;
        const ee::Ptr<pause::BonusObjective> objectives(aggressive::level::bonusObjectives());
        const uint32_t count = aggressive::level::bonusObjectiveCount();
        const ee::Ptr<zgui::Node> list = pause::find(*zgui::g_root, kTextInGameUIPauseMenusFrameGapList);
        memset(numbers, 0, kColumnBytes);
        memset(names, 0, kColumnBytes);
        memset(statuses, 0, kColumnBytes);
        memset(line, 0, kLineBytes);
        const ee::Ptr<zgui::Node> namesWidget = pause::find(list, kTextNames);
        const ee::Ptr<zgui::Node> statusWidget = pause::find(list, kTextStatus);
        uint32_t listed = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const pause::BonusObjective &objective = objectives[i];
            if (objective.kind != 0)
                continue;
            ++listed;
            if (listed >= 2)
            {
                zge::appendString(numbers, kLineBreak);
                zge::appendString(names, kLineBreak);
                zge::appendString(statuses, kLineBreak);
            }
            zge::formatString(line, kNumberFormat, listed); // the entry's number
            zge::appendString(numbers, line);
            zge::appendString(names, script::scriptString(objective.name));
            if (objective.done != 0)
                zge::appendString(statuses, kTextDone);
        }
        pause::setResult(list, kItems, numbers);
        pause::setResult(namesWidget, kItems, names);
        return pause::setResult(statusWidget, kItems, statuses);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateGapMenu, Draft);

}
