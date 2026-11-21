// BMX XXX's trick text (trick_text.h): a scored line written out as the
// trick display and the multiplayer screens show it. BMX XXX's own code
// (bmx.c), DM2's dm2TrickText (games/dm2/overlay/trick_text.cpp) with BMX
// XXX's two differences: only the line's last six tricks, no gap.

#include "trick_text.h"

#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <algorithm>
#include <cstdint>
#include "sym/bmx/ui/trick_text.cpp.h"

namespace game::bmx
{
    namespace
    {
        using rider::kNoTrick;
        using rider::ScoreLine;
        using rider::ScoreTrick;

        // The name tables, by trick number, modifier, half turns of spin,
        // loops past one and landing grade.
        inline ee::Global<ee::Ptr<const char>[100], sym::g_trickNames> g_trickNames;       // "turndown", "can-can", ...
        inline ee::Global<ee::Ptr<const char>[30], sym::g_modifierNames> g_modifierNames;  // "barspin", "one footer", ...
        inline ee::Global<ee::Ptr<const char>[9], sym::spinNames> g_spinNames;            // "", a blue "180" .. "extreme"
        inline ee::Global<ee::Ptr<const char>[4], sym::countWords> g_countWords;          // "double", "triple", "quad", "sick"
        inline ee::Global<ee::Ptr<const char>[4], sym::landingNames> g_landingNames;      // "", green "perfect", red "sketchy", grey "to" " fakie"
        constexpr uint32_t kMostCountWord = 3;

        // ScoreLine::landingGrade: how it landed. Perfect and sketchy come
        // first in a spun line's text, to fakie last.
        constexpr uint8_t kLandingNone = 0;
        constexpr uint8_t kLandingToFakie = 3;
        // ScoreLine::spin, in half turns.
        constexpr uint8_t k180 = 1, k360 = 2;
        // The most tricks of a line the text has: the last ones.
        constexpr int32_t kMostTricksWritten = 6;

        // The tricks and modifiers that make another name together (the
        // name tables' numbers).
        constexpr uint8_t kTrickCanCan = 1, kTrickBackflip = 3, kTrickSuperman = 4, kTrickFrontFlip = 10;
        constexpr uint8_t kModifierBarspin = 0, kModifierOneFooter = 1, kModifierSeatGrab = 6, kModifierNoFooter = 7;

        // The words.
        constexpr uint32_t kWordFormat = sym::trickWordFormat;              // "%s%s"
        constexpr ee::Ptr<const char> kNoWord{sym::g_textTrickNoWord};      // ""
        constexpr ee::Ptr<const char> kSpace{sym::trickWordSpace};          // " "
        constexpr ee::Ptr<const char> kTo{sym::textTo};                     // a grey "to" between two tricks
        constexpr ee::Ptr<const char> kHalfCab{sym::textHalfCab};           // "half-cab"
        constexpr ee::Ptr<const char> kOpposite{sym::textOpposite};         // "opposite"
        constexpr ee::Ptr<const char> kAlleyOop{sym::textAlleyOop};         // "alley-oop"
        constexpr uint32_t kSpinFormat = sym::spinWordFormat;               // " %s"
        constexpr ee::Ptr<const char> kSpin{sym::textSpin};                 // "spin"
        constexpr ee::Ptr<const char> kTruckDriver{sym::textTruckDriver};   // "truck driver"
        constexpr ee::Ptr<const char> kFlair{sym::textFlair};               // "flair"
        constexpr ee::Ptr<const char> kFlail{sym::textFlail};               // "flail"
        constexpr ee::Ptr<const char> kDriver{sym::textDriver};             // "driver"
        constexpr ee::Ptr<const char> kFlip{sym::textFlip};                 // "flip"
        constexpr ee::Ptr<const char> kLimo{sym::textLimo};                 // "limo"
        constexpr ee::Ptr<const char> kTripleFlip{sym::textTripleFlip};     // "triple flip"
        constexpr ee::Ptr<const char> kVette{sym::textVette};               // "vette"

        // A word put after the text, with a space when the text has any.
        void appendWord(ee::Ptr<char> text, ee::Ptr<const char> word)
        {
            const int32_t end = zge::stringLength(text);
            const ee::Ptr<const char> separator = zge::stringLength(text) != 0 ? kSpace : kNoWord;
            zge::formatString(text + end, kWordFormat, separator, word);
        }

        // appendWord as trickText's last act: the original ends in a jump to formatString (its tail call).
        void appendLastWord(ee::Ptr<char> text, ee::Ptr<const char> word)
        {
            const int32_t end = zge::stringLength(text);
            const ee::Ptr<const char> separator = zge::stringLength(text) != 0 ? kSpace : kNoWord;
            game::tail(zge::formatString, text + end, kWordFormat, separator, word);
        }

        // "double" .. "sick" for a trick looped two times or more.
        void appendLoops(ee::Ptr<char> text, const ScoreTrick &trick)
        {
            if (trick.loops >= 2)
                appendWord(text, (*g_countWords)[std::min<uint32_t>(trick.loops - 2u, kMostCountWord)]);
        }

        // A trick's modifier, with how many times it looped.
        void appendModifier(ee::Ptr<char> text, const ScoreTrick &trick)
        {
            if (trick.modifier != kNoTrick)
            {
                appendLoops(text, trick);
                appendWord(text, (*g_modifierNames)[trick.modifier]);
            }
        }

        // The words of the take-off that come before the tricks.
        void appendLineTakeoff(ee::Ptr<char> text, const ScoreLine &line)
        {
            if (line.flags & rider::kScoreLineOpposite)
                appendWord(text, kOpposite);
            if (line.flags & rider::kScoreLineSpinFlag)
                appendWord(text, kAlleyOop);
        }

        // The text started again for a pair that has its own name: the
        // spin (a half-cab in place of the 180 when the take-off was
        // backwards), then the take-off's words.
        void restartWithSpin(ee::Ptr<char> text, const ScoreLine &line, const ScoreTrick &trick, uint32_t takeoffWords)
        {
            text[0] = 0;
            if (line.spin != 0)
            {
                if (takeoffWords != 0 && (trick.flags & rider::kScoreTrickBackTakeoff))
                {
                    appendWord(text, kHalfCab);
                    if (line.spin >= k360)
                        appendWord(text, (*g_spinNames)[line.spin]);
                }
                else
                {
                    appendWord(text, (*g_spinNames)[line.spin]);
                }
            }
            if (takeoffWords != 0)
                appendLineTakeoff(text, line);
        }

        // The pairs of a trick and its modifier, or of a trick and the
        // line's spin, that have a name of their own: the text so far is
        // replaced by it.
        void nameCombination(ee::Ptr<char> text, const ScoreLine &line, const ScoreTrick &trick, uint32_t takeoffWords)
        {
            switch (trick.trick)
            {
            case kTrickSuperman: // with a seat grab: "superman seat grab"
                if (trick.modifier == kModifierSeatGrab)
                {
                    restartWithSpin(text, line, trick, takeoffWords);
                    appendWord(text, (*g_trickNames)[trick.trick]);
                    appendWord(text, (*g_modifierNames)[trick.modifier]);
                }
                break;
            case kTrickCanCan: // with a one footer: "no footer can-can"
                if (trick.modifier == kModifierOneFooter)
                {
                    restartWithSpin(text, line, trick, takeoffWords);
                    appendWord(text, (*g_modifierNames)[kModifierNoFooter]);
                    appendWord(text, (*g_trickNames)[trick.trick]);
                }
                break;
            case kTrickBackflip:
                if (line.spin == k180) // a 180 backflip: a flair
                {
                    text[0] = 0;
                    appendModifier(text, trick);
                    appendWord(text, kFlair);
                }
                else if (trick.modifier == kModifierBarspin) // with a barspin: a flip driver, by its loops
                {
                    text[0] = 0;
                    if (trick.loops == 2)
                        appendWord(text, kLimo);
                    else if (trick.loops < 3)
                        appendWord(text, kFlip);
                    else if (trick.loops == 3)
                        appendWord(text, kTripleFlip);
                    else
                        appendWord(text, kVette);
                    appendWord(text, kDriver);
                }
                break;
            case kTrickFrontFlip: // a 360 front flip: a flail
                if (line.spin == k360)
                {
                    text[0] = 0;
                    appendModifier(text, trick);
                    appendWord(text, kFlail);
                }
                break;
            case kNoTrick: // a barspin spun 360 or more: a truck driver
                if (trick.modifier == kModifierBarspin && line.spin >= k360)
                {
                    text[0] = 0;
                    if (takeoffWords != 0)
                    {
                        if (trick.flags & rider::kScoreTrickBackTakeoff)
                            appendWord(text, kHalfCab);
                        appendLineTakeoff(text, line);
                    }
                    if (line.spin != k360)
                        appendWord(text, (*g_spinNames)[line.spin]);
                    appendLoops(text, trick);
                    appendWord(text, kTruckDriver);
                }
                break;
            default:
                break;
            }
        }
    }

    void trickText(ee::Ptr<char> text, const rider::ScoreLine *line, uint32_t takeoffWords)
    {
        text[0] = 0;
        // The tricks still to come, counted down as they go: only the
        // last six are written.
        int32_t tricksLeft = 0;
        // A spun line: how it landed, the take-off's words and the spin.
        if (line->spin != 0)
        {
            if (line->landingGrade != kLandingNone && line->landingGrade != kLandingToFakie)
                appendWord(text, (*g_landingNames)[line->landingGrade]);
            if (takeoffWords != 0)
            {
                // (The original reads the first trick's flags with no check
                // that the line has one.)
                if (line->tricks->flags & rider::kScoreTrickBackTakeoff)
                    appendWord(text, kHalfCab);
                appendLineTakeoff(text, *line);
            }
            appendWord(text, (*g_spinNames)[line->spin]);
        }

        for (ee::Ptr<ScoreTrick> part = line->tricks; part; part = part->next)
            ++tricksLeft;
        // Each trick: "to" from the second on (so a text of a line cut to
        // its last six starts with one), a backwards grind's "alley-oop",
        // the modifier and the trick; a spin with neither is a "spin". A
        // pair with a name of its own replaces what is there.
        for (ee::Ptr<ScoreTrick> part = line->tricks; part; part = part->next)
        {
            if (--tricksLeft >= kMostTricksWritten)
                continue;
            const ScoreTrick &trick = *part;
            if (part != line->tricks)
                appendWord(text, kTo);
            if (takeoffWords != 0 && (trick.flags & rider::kScoreTrickAlleyOop))
                appendWord(text, kAlleyOop);
            appendModifier(text, trick);
            if (trick.trick != kNoTrick)
                appendWord(text, (*g_trickNames)[trick.trick]);
            else if (line->spin != 0 && trick.modifier == kNoTrick)
                zge::formatString(text + zge::stringLength(text), kSpinFormat, kSpin);
            nameCombination(text, *line, trick, takeoffWords);
        }

        if (line->landingGrade == kLandingToFakie)
            appendLastWord(text, (*g_landingNames)[kLandingToFakie]);
    }
    READABLE_FUNCTION(trickText, Verified);
}
