// DM2's trick list screen (screens.h; "InGameUI.FrameTrickList"): the
// current player's moves of a category in a tree view, each greyed until
// landed, with the selected one's buttons; its menu's script function.
// DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/game/players.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/screen_trick_list.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kFrameTrickListMenu{sym::g_textFrameTrickListMenu}; // "InGameUI.FrameTrickList.Menu"
        constexpr ee::Ptr<const char> kTrickListFrame{sym::trickListFramePath};           // "InGameUI.FrameTrickList"
        constexpr ee::Ptr<const char> kTreeViewName{sym::g_textTreeViewTrickList};        // "TreeViewTrickList"
        constexpr ee::Ptr<const char> kTreeViewPath{sym::trickListTreePath};              // "...FrameTrickList.TreeViewTrickList"
        constexpr ee::Ptr<const char> kTrickInfoName{sym::g_textLabelTrickInfo};          // "LabelTrickInfo"
        constexpr ee::Ptr<const char> kCategoryName{sym::screenCategoryName};             // "Category"
        constexpr ee::Ptr<const char> kRootNodeName{sym::screenRootNodeName};             // "RootNode"
        constexpr ee::Ptr<const char> kParentNodeName{sym::screenParentNodeName};         // "ParentNode"
        constexpr ee::Ptr<const char> kTopItemText{sym::screenMasterText}; // "Master"
        constexpr ee::Ptr<const char> kText{sym::screenTextName};                         // "Text"
        constexpr uint32_t kNoText = sym::g_textEmpty;                                    // ""
        constexpr ee::Ptr<const char> kTitlePath{sym::trickListTitlePath}; // "InGameUI.FrameTrickList.Title.Text"

        // The moves: the tricks by number, then the modifiers (a modifier's
        // move number is its number plus kFirstModifierBit). Their kinds'
        // table covers both; a kind with kMoveOffAtFirst is a move the
        // characters start without (initGame).
        constexpr uint32_t kTricks = 0x48;
        constexpr uint32_t kModifiers = 0x56;
        constexpr ee::Ptr<uint16_t> kMoveKinds{sym::g_trickKindsR};
        constexpr uint16_t kMoveOffAtFirst = 0x8;
        constexpr uint32_t kTrickNames = sym::trickNames;
        constexpr uint32_t kModifierNames = sym::modifierNames;
        // Each move's buttons, as the font's button glyphs ("G 2", ...).
        constexpr uint32_t kTrickButtons = sym::trickButtonTexts;
        constexpr uint32_t kModifierButtons = sym::modifierButtonTexts;
        uint32_t textIn(uint32_t table, uint32_t index)
        {
            return *(ee::Ptr<uint32_t>(table) + static_cast<ptrdiff_t>(index));
        }

        // The tree view's categories (its "Category" variable), their
        // titles and the tricks of each (by number, first to last).
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
        constexpr uint32_t kFirstOther = 0x43, kLastOther = 0x47;

        // The listed lines' moves (trick or modifier numbers), and how many.
        inline ee::Global<uint8_t[kModifiers], sym::g_trickListMoves> g_trickListMoves;
        inline ee::Global<uint32_t, sym::g_trickListCount> g_trickListCount;
        // A line's colour: grey until the character has landed the move.
        constexpr uint32_t kLandedColours = sym::trickListLandedColours; // "#RGB{86 86 86}", "#RGB{192 192 192}"
        constexpr uint32_t kLineFormat = sym::trickWordFormat;           // "%s%s"

        uint32_t bitOf(const uint8_t *bits, uint32_t index)
        {
            return (bits[index >> 3] >> (index & 7)) & 1u;
        }

        // A move's line: its name in the colour of whether it was landed.
        void addMoveLine(ee::Ptr<char> text, const ee::Local<uint32_t[2]> &colours, const RiderCharacter &character,
                         ee::Ptr<zge::zgui::ListedTreeItem> top, uint32_t move, uint32_t name, uint32_t &count)
        {
            zge::formatString(text, kLineFormat, colours[bitOf(character.movesLanded, move)], name);
            zge::zgui::newTreeItem(top, text);
            (*g_trickListMoves)[count] = static_cast<uint8_t>(move < kTricks ? move : move - kFirstModifierBit);
            ++count;
        }
    }

    // bmxtrickSetUpdateCategory: the tree view filled for its category: its
    // title, then a line for each move of it the character has. The
    // modifiers listed are the ones not marked off at first, and the marked
    // ones whose bit in the character's tricksOff is set; the signature
    // moves are the tricks marked off at first whose bit is clear; the other
    // categories list their tricks that are not marked.
    void bmxtrickSetUpdateCategory()
    {
        ee::Local<char[0x80]> text;
        ee::Local<uint32_t[2]> colours;
        ee::Local<int32_t> category;
        ee::Local<ee::Ptr<zge::zgui::ListedTreeItem>> top;

        const ee::Ptr<zge::zgui::Node> tree = uiWidget(kTreeViewPath);
        colours[0] = textIn(kLandedColours, 0);
        colours[1] = textIn(kLandedColours, 1);
        const RiderCharacter &character = *playerCharacter(*g_currentPlayer);
        zge::zgui::copyVariable(tree, kCategoryName, category);
        zge::zgui::copyVariable(tree, kRootNodeName, top);
        if (*top)
            zge::zgui::freeTreeItem(*top);
        *top = zge::zgui::newTreeItem(ee::Ptr<zge::zgui::ListedTreeItem>(), kTopItemText); // (a tree's top item: no parent)
        zge::zgui::setVariable(tree, kParentNodeName, top);
        zge::zgui::setVariable(tree, kRootNodeName, top);

        uint32_t count = 0;
        if (*category == kCategoryModifiers)
        {
            zge::zgui::setVariableAtPath(kTitlePath, kModifiersTitle);
            for (uint32_t modifier = 0; modifier < kModifiers; ++modifier)
            {
                const uint32_t move = modifier + kFirstModifierBit;
                if (!(kMoveKinds[move] & kMoveOffAtFirst) || bitOf(character.tricksOff, move))
                    addMoveLine(text, colours, character, *top, move, textIn(kModifierNames, modifier), count);
            }
            *g_trickListCount = count;
            return;
        }
        if (*category == kCategorySignatureMoves)
        {
            zge::zgui::setVariableAtPath(kTitlePath, kSignatureMovesTitle);
            for (uint32_t trick = 0; trick < kTricks; ++trick)
                if ((kMoveKinds[trick] & kMoveOffAtFirst) && !bitOf(character.tricksOff, trick))
                    addMoveLine(text, colours, character, *top, trick, textIn(kTrickNames, trick), count);
            *g_trickListCount = count;
            return;
        }
        uint32_t first, last;
        switch (*category)
        {
        case kCategoryStalls:
            zge::zgui::setVariableAtPath(kTitlePath, kStallsTitle);
            first = kFirstStall, last = kLastStall;
            break;
        case kCategoryGrinds:
            zge::zgui::setVariableAtPath(kTitlePath, kGrindsTitle);
            first = kFirstGrind, last = kLastGrind;
            break;
        case kCategoryOthers:
            zge::zgui::setVariableAtPath(kTitlePath, kOthersTitle);
            first = kFirstOther, last = kLastOther;
            break;
        default:
            zge::zgui::setVariableAtPath(kTitlePath, kAirTricksTitle);
            first = kFirstAirTrick, last = kLastAirTrick;
            break;
        }
        for (uint32_t trick = first; trick <= last; ++trick)
            if (!(kMoveKinds[trick] & kMoveOffAtFirst))
                addMoveLine(text, colours, character, *top, trick, textIn(kTrickNames, trick), count);
        *g_trickListCount = count;
    }
    READABLE_FUNCTION(bmxtrickSetUpdateCategory, Verified);

    // bmxtrickSetGetTrick: the selected line's buttons shown under the list
    // (nothing past the last line).
    void bmxtrickSetGetTrick()
    {
        ee::Local<int32_t> category;
        ee::Local<uint32_t> line;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kTrickListFrame);
        const ee::Ptr<zge::zgui::Node> tree = zge::zgui::findNode(frame, kTreeViewName, 0);
        const ee::Ptr<zge::zgui::Node> info = zge::zgui::findNode(frame, kTrickInfoName, 0);
        zge::zgui::copyVariable(tree, kCategoryName, category);
        zge::zgui::copyVariable(tree, kSelectedIndexName, line);
        if (*line < *g_trickListCount)
        {
            const uint32_t buttons = *category == kCategoryModifiers ? kModifierButtons : kTrickButtons;
            zge::zgui::setVariable(info, kText, textIn(buttons, (*g_trickListMoves)[*line]));
        }
        else
        {
            zge::zgui::setVariable(info, kText, kNoText);
        }
    }
    READABLE_FUNCTION(bmxtrickSetGetTrick, Verified);

    // showTrickListScreen (the four trick list screens' show handler): as
    // it opens, the frame's Init and the tree view filled; as it closes, the
    // tree's items freed and its RootNode cleared, and the frame's DeInit.
    void showTrickListScreen(uint32_t shown)
    {
        ee::Local<ee::Ptr<zge::zgui::ListedTreeItem>> top;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kTrickListFrame);
        const ee::Ptr<zge::zgui::Node> tree = zge::zgui::findNode(frame, kTreeViewName, 0);
        if (shown != 0)
        {
            playWidgetAction(frame, kInitAction);
            bmxtrickSetUpdateCategory();
            return;
        }
        zge::zgui::copyVariable(tree, kRootNodeName, top);
        if (*top)
            zge::zgui::freeTreeItem(*top);
        *top = ee::Ptr<zge::zgui::ListedTreeItem>();
        zge::zgui::setVariable(tree, kRootNodeName, top);
        playWidgetAction(frame, kDeInitAction);
    }
    READABLE_FUNCTION(showTrickListScreen, Verified);

    // bmxoverlayTrickListHandler: the trick list left for the screen it was
    // asked from (the menu's selected line is read and not used).
    void bmxoverlayTrickListHandler()
    {
        ee::Local<uint32_t> selected;
        zge::zgui::copyVariable(uiWidget(kFrameTrickListMenu), kSelectedIndexName, selected);
        switch (*g_gameScreen)
        {
        case kScreenTrickList:
            showGameScreen(kScreenPause);
            break;
        case kScreenRoundOverTrickList:
            showGameScreen(kScreenRoundOver);
            break;
        case kScreenGameOverTrickList:
            showGameScreen(kScreenGameOver);
            break;
        case kScreenReadyTrickList:
            showGameScreen(kScreenMultiplayerReady);
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(bmxoverlayTrickListHandler, Verified);
}
