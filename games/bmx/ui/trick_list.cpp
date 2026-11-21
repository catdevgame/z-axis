// BMX XXX's trick list (trick_list.h). BMX XXX's own code (bmx.c).

#include "trick_list.h"

#include "bmx/game/trick_types.h"
#include "bmx/profile/players.h"
#include "bmx/ui/ui_nodes.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/ui/trick_list.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        constexpr uint32_t kTreeViewName = sym::g_textTrickListTree;          // "TreeViewTrickList"
        constexpr uint32_t kTrickInfoName = sym::g_textTrickListInfo;         // "LabelTrickInfo"
        constexpr uint32_t kTitleName = sym::g_textTrickListTitle;            // "Title"
        constexpr uint32_t kCategoryName = sym::g_textTrickListCategory;      // "Category"
        constexpr uint32_t kSelectedIndexName = sym::g_textTrickListSelectedIndex; // "SelectedIndex"
        constexpr uint32_t kRootNodeName = sym::g_textTrickListRootNode;      // "RootNode"
        constexpr uint32_t kParentNodeName = sym::g_textTrickListParentNode;  // "ParentNode"
        constexpr uint32_t kTextName = sym::g_textTrickListText;              // "Text"
        constexpr uint32_t kTopItemText = sym::g_textTrickListMaster;         // "Master"
        constexpr uint32_t kInitAction = sym::g_textTrickListInit;            // "Init"
        constexpr uint32_t kShutdownAction = sym::g_textTrickListShutdown;    // "Shutdown"
        constexpr uint32_t kNoText = sym::g_textTrickNoWord;                       // ""

        // The moves: the tricks by number, then the modifiers. What was
        // ever landed is SavedGame::movesLanded's bit, the modifiers' after
        // the tricks'.
        constexpr uint32_t kTricks = 100;
        constexpr uint32_t kModifiers = 0x56;
        constexpr uint32_t kFirstModifierBit = 100;
        inline ee::Global<ee::Ptr<const char>[kTricks], sym::g_trickNames> g_trickNames;
        inline ee::Global<ee::Ptr<const char>[kModifiers], sym::g_modifierNames> g_modifierNames;
        // Each move's buttons, as the font's button glyphs ("G 2", ...).
        inline ee::Global<ee::Ptr<const char>[kTricks], sym::trickButtonTexts> g_trickButtons;
        inline ee::Global<ee::Ptr<const char>[kModifiers], sym::modifierButtonTexts> g_modifierButtons;
        // g_trickFlags: a trick the categories leave out (named by this use).
        constexpr uint32_t kTrickNotListed = 0x8;

        // The tree view's categories (its "Category" variable, which the
        // frame's script turns round from 0 to 5), their titles and the
        // tricks of each (by number, first to last). SIGNATURE lists every
        // trick.
        enum TrickListCategory : int32_t
        {
            kCategoryAirTricks = 0, // and any other value
            kCategoryModifiers = 1,
            kCategoryStalls = 2,
            kCategoryGrinds = 3,
            kCategoryOthers = 4,
            kCategorySignatureMoves = 5,
        };
        constexpr uint32_t kAirTricksTitle = sym::g_textAirTricks;             // "AIR TRICKS"
        constexpr uint32_t kModifiersTitle = sym::g_textModifiers;             // "MODIFIERS"
        constexpr uint32_t kStallsTitle = sym::g_textStalls;                   // "STALLS"
        constexpr uint32_t kGrindsTitle = sym::g_textGrinds;                   // "GRINDS"
        constexpr uint32_t kOthersTitle = sym::g_textOthers;                   // "OTHERS"
        constexpr uint32_t kSignatureMovesTitle = sym::g_textSignatureMoves;   // "SIGNATURE MOVES"
        constexpr uint32_t kFirstAirTrick = 0x00, kLastAirTrick = 0x32;
        constexpr uint32_t kFirstGrind = 0x33, kLastGrind = 0x3B;
        constexpr uint32_t kFirstStall = 0x3C, kLastStall = 0x42;
        constexpr uint32_t kFirstOther = 0x43, kLastOther = 0x63;

        // The listed lines' moves (trick or modifier numbers), and how many.
        inline ee::Global<uint8_t[kTricks], sym::g_trickListMoves> g_trickListMoves;
        inline ee::Global<uint32_t, sym::g_trickListCount> g_trickListCount;
        // A line's colour: grey until the move was landed.
        struct LandedColours
        {
            ee::Ptr<const char> colour[2]; // "#RGB{86 86 86}", "#RGB{192 192 192}"
        };
        inline ee::Global<LandedColours, sym::trickListLandedColours> g_landedColours;
        constexpr uint32_t kLineFormat = sym::trickWordFormat; // "%s%s"

        uint32_t landedBit(uint32_t bit)
        {
            return (g_savedGame->movesLanded[bit >> 3] >> (bit & 7)) & 1u;
        }

        // A move's line: its name in the colour of whether it was landed.
        void addMoveLine(ee::Ptr<char> text, const LandedColours &colours, ee::Ptr<Node> top, uint32_t bit,
                         uint32_t move, ee::Ptr<const char> name, uint32_t &count)
        {
            zge::formatString(text, kLineFormat, colours.colour[landedBit(bit)], name);
            zge::zgui::addTreeTextNode(top, text);
            (*g_trickListMoves)[count] = static_cast<uint8_t>(move);
            ++count;
        }
    }

    void openTrickList(ee::Ptr<Node> frame)
    {
        if (!frame)
            return;
        playUiNode(frame, kInitAction);
        game::tail(updateTrickListCategory, frame);
    }
    READABLE_FUNCTION(openTrickList, Verified);

    void closeTrickList(ee::Ptr<Node> frame)
    {
        ee::Local<ee::Ptr<Node>> top;
        if (!frame)
            return;
        const ee::Ptr<Node> tree = zge::zgui::findNode(frame, kTreeViewName, kFrameNode);
        zge::zgui::copyVariable(tree, kRootNodeName, top);
        if (*top)
        {
            zge::zgui::destroyGuiNodeMethod(*top);
            *top = {};
        }
        zge::zgui::setVariable(tree, kRootNodeName, top);
        playUiNode(frame, kShutdownAction);
    }
    READABLE_FUNCTION(closeTrickList, Verified);

    void updateTrickListTrick(ee::Ptr<Node> frame)
    {
        ee::Local<int32_t> category;
        ee::Local<uint32_t> line;
        const ee::Ptr<Node> tree = zge::zgui::findNode(frame, kTreeViewName, kFrameNode);
        const ee::Ptr<Node> info = zge::zgui::findNode(frame, kTrickInfoName, kFrameNode);
        zge::zgui::copyVariable(tree, kCategoryName, category);
        zge::zgui::copyVariable(tree, kSelectedIndexName, line);
        if (*line < *g_trickListCount)
        {
            const uint8_t move = (*g_trickListMoves)[*line];
            const ee::Ptr<const char> buttons = *category == kCategoryModifiers ? (*g_modifierButtons)[move] : (*g_trickButtons)[move];
            zge::zgui::setVariable(info, kTextName, buttons);
        }
        else
        {
            zge::zgui::setVariable(info, kTextName, kNoText);
        }
    }
    READABLE_FUNCTION(updateTrickListTrick, Verified);

    void updateTrickListCategory(ee::Ptr<Node> frame)
    {
        ee::Local<char[0x80]> text;
        ee::Local<LandedColours> colours;
        ee::Local<int32_t> category;
        ee::Local<ee::Ptr<Node>> top;
        *colours = *g_landedColours;
        if (!frame)
            return;
        const ee::Ptr<Node> tree = zge::zgui::findNode(frame, kTreeViewName, kFrameNode);
        playerCharacter(*g_currentPlayer); // (asked for; the landed moves are the saved game's)
        zge::zgui::copyVariable(tree, kCategoryName, category);
        zge::zgui::copyVariable(tree, kRootNodeName, top);
        if (*top)
            zge::zgui::destroyGuiNodeMethod(*top);
        *top = zge::zgui::addTreeTextNode({}, kTopItemText);
        zge::zgui::setVariable(tree, kParentNodeName, top);
        zge::zgui::setVariable(tree, kRootNodeName, top);

        const auto setTitle = [&](uint32_t title) {
            zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, kFrameNode), kTextName, title);
        };
        uint32_t count = 0;
        if (*category == kCategoryModifiers)
        {
            setTitle(kModifiersTitle);
            for (uint32_t modifier = 0; modifier < kModifiers; ++modifier)
                addMoveLine(text, *colours, *top, modifier + kFirstModifierBit, modifier, (*g_modifierNames)[modifier], count);
            *g_trickListCount = count;
            return;
        }
        if (*category == kCategorySignatureMoves)
        {
            setTitle(kSignatureMovesTitle);
            for (uint32_t trick = 0; trick < kTricks; ++trick)
                addMoveLine(text, *colours, *top, trick, trick, (*g_trickNames)[trick], count);
            *g_trickListCount = count;
            return;
        }
        uint32_t first, last;
        switch (*category)
        {
        case kCategoryStalls:
            setTitle(kStallsTitle);
            first = kFirstStall, last = kLastStall;
            break;
        case kCategoryGrinds:
            setTitle(kGrindsTitle);
            first = kFirstGrind, last = kLastGrind;
            break;
        case kCategoryOthers:
            setTitle(kOthersTitle);
            first = kFirstOther, last = kLastOther;
            break;
        default:
            setTitle(kAirTricksTitle);
            first = kFirstAirTrick, last = kLastAirTrick;
            break;
        }
        for (uint32_t trick = first; trick <= last; ++trick)
            if (!((*g_trickFlags)[trick] & kTrickNotListed))
                addMoveLine(text, *colours, *top, trick, trick, (*g_trickNames)[trick], count);
        *g_trickListCount = count;
    }
    READABLE_FUNCTION(updateTrickListCategory, Verified);
}
