// The tuning table's menu (the debug menu's Tune pages; the scripts call
// these natives, inline...): one page lists the categories, the next a
// category's entries with their values, and up and down step the selected
// value within its range.

#include "aggressive/globals.h"
#include "aggressive/tuning.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/tuning_menu.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kSetVariable = sym::setVariable;    // (node, name, value *) (by address: hooked)

        // The zgui names, for the split screen's page or the single
        // player's.
        constexpr uint32_t kGroupMenuSplit = sym::groupMenuSplit;  // "MultiplayerUI.TuneMenuGroup.TuneMenu"
        constexpr uint32_t kGroupMenuSolo = sym::groupMenuSolo;   // "InGameUI.TuneMenuGroup.TuneMenu"
        constexpr uint32_t kDetailRootSplit = sym::detailRootSplit; // "MultiplayerUI.TuneMenuDetail.TuneDetailData.RootNode"
        constexpr uint32_t kDetailParentSplit = sym::detailParentSplit; // "MultiplayerUI.TuneMenuDetail.TuneDetailData.ParentNode"
        constexpr uint32_t kDetailItemsSplit = sym::detailItemsSplit; // "MultiplayerUI.TuneMenuDetail.TuneDetailLabel.Items"
        constexpr uint32_t kDetailRootSolo = sym::detailRootSolo;  // "InGameUI.TuneMenuDetail.TuneDetailData.RootNode"
        constexpr uint32_t kDetailParentSolo = sym::detailParentSolo; // "InGameUI.TuneMenuDetail.TuneDetailData.ParentNode"
        constexpr uint32_t kDetailItemsSolo = sym::detailItemsSolo; // "InGameUI.TuneMenuDetail.TuneDetailLabel.Items"
        constexpr uint32_t kDetailDataSplit = sym::detailDataSplit; // "MultiplayerUI.TuneMenuDetail.TuneDetailData"
        constexpr uint32_t kDetailDataSolo = sym::detailDataSolo;  // "InGameUI.TuneMenuDetail.TuneDetailData"
        constexpr uint32_t kLineBreak = sym::lineBreak;       // "\n "
        constexpr ee::Ptr<const char> kItems{sym::items};           // "Items"
        constexpr uint32_t kSelectedIndex = sym::selectedIndex;   // "SelectedIndex"
        constexpr uint32_t kRoot = sym::root;            // "Root"
        constexpr uint32_t kValueFormat = sym::valueFormat;     // "%.6f"
        constexpr uint32_t kSelectedNode = sym::selectedNode;    // "SelectedNode"
        constexpr uint32_t kText = sym::text;            // "Text"

        constexpr uint32_t kUsedCategories = 20;
        constexpr uint32_t kHeapLimitBytes = 0x400;

        // The category the detail page shows.
        inline ee::Global<uint32_t, sym::g_tuneCategory> g_tuneCategory;

        ee::Ptr<zgui::Node> uiRoot() { return ee::Ptr<zgui::Node>(*g_uiRoot); }
        bool splitScreen() { return *g_gameType == GameType::SplitScreen; }

        uint32_t categoriesAddress() { return sym::g_tuning; }
        uint32_t categoryNames() { return categoriesAddress() + offsetof(TuningCategory, name); }

        // The value of `entry` as the menu shows it, into `out`.
        void formatTuneValue(uint32_t out, const TuningEntry &entry)
        {
            const uint64_t value = sce::libgcc::fptodp(entry.value);
            zge::formatString(out, kValueFormat, value);
        }

        // An entry starts with its name.
        ee::Ptr<const char> nameOf(const TuningEntry &entry)
        {
            return entry.name;
        }

        // Up or down one step, then the new value shown. The two script
        // functions are one body.
        int32_t stepSelectedValue(bool up)
        {
            ee::Local<char[0x100]> valueText;
            ee::Local<uint32_t> index;
            ee::Local<uint32_t> selectedNode;
            const uint32_t path = splitScreen() ? kDetailDataSplit : kDetailDataSolo;
            const ee::Ptr<zgui::Node> node = zgui::findNode(uiRoot(), ee::Ptr<const char>(path), 1);
            zgui::copyVariable(node, ee::Ptr<const char>(kSelectedIndex), ee::Ptr<char>(index.address()));
            TuningEntry &entry = (*g_tuningCategories)[*g_tuneCategory].entries[*index];
            if (up)
            {
                entry.value = entry.value + entry.step;
                if (entry.maxValue < entry.value)
                    entry.value = entry.maxValue;
            }
            else
            {
                entry.value = entry.value - entry.step;
                if (entry.value < entry.minValue)
                    entry.value = entry.minValue;
            }
            zgui::copyVariable(node, ee::Ptr<const char>(kSelectedNode), ee::Ptr<char>(selectedNode.address()));
            formatTuneValue(valueText.address(), entry);
            return zgui::setVariable(*selectedNode, kText, valueText.address());
        }
    }

    // inlineTuneGuiGroupInit: the categories' names into the menu's Items.
    int32_t inlineTuneGuiGroupInit()
    {
        ee::Local<char> text;
        const uint32_t names = categoryNames();
        const uint32_t namesEnd = names + kUsedCategories * sizeof(TuningCategory);
        uint32_t name = names;
        *text = 0;
        for (;;)
        {
            if (name != names)
                zge::appendString(text, kLineBreak);
            zge::appendString(text, name);
            name += sizeof(TuningCategory);
            if (name >= namesEnd)
                break;
        }
        ee::Ptr<zgui::Node> menu;
        int32_t result;
        if (splitScreen())
        {
            menu = zgui::findNode(uiRoot(), ee::Ptr<const char>(kGroupMenuSplit), 1);
            result = zgui::setVariable(menu, kItems, text);
        }
        else
        {
            menu = zgui::findNode(uiRoot(), ee::Ptr<const char>(kGroupMenuSolo), 1);
            result = zgui::setVariable(menu, kItems, text);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(inlineTuneGuiGroupInit, Verified);

    // inlineTuneGuiDetailInit: the selected category's page: a tree node per entry with
    // its value, and the labels' text (the MemCheck category checks the heap
    // first). Returns the category.
    uint32_t inlineTuneGuiDetailInit()
    {
        ee::Local<char> text;
        ee::Local<uint8_t[0x20]> value;
        ee::Local<uint32_t[1]> category;
        ee::Local<ee::Ptr<zgui::Node>[1]> root;
        const uint32_t path = splitScreen() ? kGroupMenuSplit : kGroupMenuSolo;
        const ee::Ptr<zgui::Node> menu = zgui::findNode(uiRoot(), ee::Ptr<const char>(path), 1);
        zgui::copyVariable(menu, ee::Ptr<const char>(kSelectedIndex), ee::Ptr<char>(category.address()));
        const uint32_t selected = *category;
        if (selected == kTuningMemCheck)
        {
            zge::testHeapLimits(kHeapLimitBytes);
            zge::largestFreeBlock();
        }
        *root = zge::zgui::addTreeTextNode(0, kRoot);
        *text = 0;
        for (uint32_t index = 0; index < (*g_tuningCategories)[selected].count; ++index)
        {
            if (index != 0)
                zge::appendString(text, kLineBreak);
            const TuningEntry &entry = (*g_tuningCategories)[selected].entries[index];
            zge::appendString(text, nameOf(entry));
            formatTuneValue(value.address(), entry);
            zge::zgui::addTreeTextNode(*root, value);
        }
        const auto setUiVariable = [&](uint32_t name, uint32_t address) {
            zgui::setVariable(*g_uiRoot, name, address);
        };
        const bool split = splitScreen();
        setUiVariable(split ? kDetailRootSplit : kDetailRootSolo, root.address());
        setUiVariable(split ? kDetailParentSplit : kDetailParentSolo, root.address());
        setUiVariable(split ? kDetailItemsSplit : kDetailItemsSolo, text.address());
        *g_tuneCategory = selected;
        return selected;
    }
    READABLE_FUNCTION_WAITS(inlineTuneGuiDetailInit, Verified);

    // inlineTuneUp: the selected entry up a step, to its maximum, and its
    // new value shown.
    int32_t inlineTuneUp()
    {
        return stepSelectedValue(true);
    }
    READABLE_FUNCTION_WAITS(inlineTuneUp, Verified);

    // inlineTuneDown: the same down, to its minimum.
    int32_t inlineTuneDown()
    {
        return stepSelectedValue(false);
    }
    READABLE_FUNCTION_WAITS(inlineTuneDown, Verified);
}
