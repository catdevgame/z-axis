// DM2's trick text (overlay.h): a scored line written out as the trick
// display and the screens show it ("perfect 360 opposite tailwhip to x-up
// barspin", a gap's name after it). DM2's own code (overlay.c).

#include "overlay.h"

#include "dm2/game/gaps.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/trick_text.cpp.h"

namespace game::dm2
{
    namespace
    {
        using rider::kNoTrick;
        using rider::ScoreLine;
        using rider::ScoreTrick;

        // The name tables, by trick number, modifier (the trick's motion),
        // half turns of spin, loops past one ("double" .. "sick") and
        // landing grade.
        constexpr uint32_t kTrickNames = sym::trickNames;       // "turndown", "can-can", ...
        constexpr uint32_t kModifierNames = sym::modifierNames; // "barspin", "one footer", ...
        constexpr uint32_t kSpinNames = sym::spinNames;         // "", "180", "360", ... "extreme"
        constexpr uint32_t kCountWords = sym::countWords;       // "double", "triple", "quad", "sick"
        constexpr uint32_t kLandingNames = sym::landingNames;   // "", green "perfect", red "sketchy", grey "to" " fakie"
        constexpr uint32_t kMostCountWord = 3;
        uint32_t nameIn(uint32_t table, uint32_t index)
        {
            return *(ee::Ptr<uint32_t>(table) + static_cast<ptrdiff_t>(index));
        }

        // ScoreLine::landingGrade: how it landed (rider.h). Perfect and
        // sketchy come first in a spun line's text, to fakie last.
        constexpr uint8_t kLandingNone = 0;
        constexpr uint8_t kLandingToFakie = 3;
        // ScoreLine::spinHalfTurns.
        constexpr uint8_t k180 = 1, k360 = 2;

        // The tricks and modifiers that make another name together
        // (the name tables' numbers).
        constexpr uint8_t kTrickCanCan = 1, kTrickBackflip = 3, kTrickSuperman = 4, kTrickFrontFlip = 10;
        constexpr uint8_t kModifierBarspin = 0, kModifierOneFooter = 1, kModifierSeatGrab = 6, kModifierNoFooter = 7;

        // The words.
        constexpr uint32_t kWordFormat = sym::trickWordFormat;   // "%s%s"
        constexpr uint32_t kNoWord = sym::g_textEmpty;           // ""
        constexpr uint32_t kSpace = sym::trickWordSpace;         // " "
        constexpr uint32_t kTo = sym::textTo;                    // a grey "to" between two tricks
        constexpr uint32_t kHalfCab = sym::textHalfCab;          // "half-cab"
        constexpr uint32_t kOpposite = sym::textOpposite;        // "opposite"
        constexpr uint32_t kAlleyOop = sym::textAlleyOop;        // "alley-oop"
        constexpr uint32_t kSpinFormat = sym::spinWordFormat;    // " %s"
        constexpr uint32_t kSpin = sym::textSpin;                // "spin"
        constexpr uint32_t kTruckDriver = sym::textTruckDriver;  // "truck driver"
        constexpr uint32_t kFlair = sym::textFlair;              // "flair"
        constexpr uint32_t kFlail = sym::textFlail;              // "flail"
        constexpr uint32_t kDriver = sym::textDriver;            // "driver"
        constexpr uint32_t kFlip = sym::textFlip;                // "flip"
        constexpr uint32_t kLimo = sym::textLimo;                // "limo"
        constexpr uint32_t kTripleFlip = sym::textTripleFlip;    // "triple flip"
        constexpr uint32_t kVette = sym::textVette;              // "vette"
        // The gap: its name in blue after a grey " + ".
        constexpr uint32_t kGapFormat = sym::gapNameFormat;      // "%s#RGB{55 138 230}%s#RGB{192 192 192}"
        constexpr uint32_t kGapSeparator = sym::gapWordSeparator; // "#RGB{192 192 192} + "

        // A word put after the text, with a space when the text has any.
        void appendWord(ee::Ptr<char> text, uint32_t word)
        {
            const int32_t end = zge::stringLength(text);
            const uint32_t separator = zge::stringLength(text) != 0 ? kSpace : kNoWord;
            zge::formatString(text + end, kWordFormat, separator, word);
        }

        // appendWord as dm2TrickText's last act: the original ends in a jump to formatString (its tail call).
        void appendLastWord(ee::Ptr<char> text, uint32_t word)
        {
            const int32_t end = zge::stringLength(text);
            const uint32_t separator = zge::stringLength(text) != 0 ? kSpace : kNoWord;
            game::tail(zge::formatString, text + end, kWordFormat, separator, word);
        }

        // "double" .. "sick" for a trick looped two times or more.
        void appendLoops(ee::Ptr<char> text, const ScoreTrick &trick)
        {
            if (trick.loops >= 2)
                appendWord(text, nameIn(kCountWords, std::min<uint32_t>(trick.loops - 2u, kMostCountWord)));
        }

        // A trick's modifier, with how many times it looped.
        void appendModifier(ee::Ptr<char> text, const ScoreTrick &trick)
        {
            if (trick.trickMotion != kNoTrick)
            {
                appendLoops(text, trick);
                appendWord(text, nameIn(kModifierNames, trick.trickMotion));
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
            if (line.spinHalfTurns != 0)
            {
                if (takeoffWords != 0 && (trick.flags & rider::kScoreTrickBackTakeoff))
                {
                    appendWord(text, kHalfCab);
                    if (line.spinHalfTurns >= k360)
                        appendWord(text, nameIn(kSpinNames, line.spinHalfTurns));
                }
                else
                {
                    appendWord(text, nameIn(kSpinNames, line.spinHalfTurns));
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
                if (trick.trickMotion == kModifierSeatGrab)
                {
                    restartWithSpin(text, line, trick, takeoffWords);
                    appendWord(text, nameIn(kTrickNames, trick.trick));
                    appendWord(text, nameIn(kModifierNames, trick.trickMotion));
                }
                break;
            case kTrickCanCan: // with a one footer: "no footer can-can"
                if (trick.trickMotion == kModifierOneFooter)
                {
                    restartWithSpin(text, line, trick, takeoffWords);
                    appendWord(text, nameIn(kModifierNames, kModifierNoFooter));
                    appendWord(text, nameIn(kTrickNames, trick.trick));
                }
                break;
            case kTrickBackflip:
                if (line.spinHalfTurns == k180) // a 180 backflip: a flair
                {
                    text[0] = 0;
                    appendModifier(text, trick);
                    appendWord(text, kFlair);
                }
                else if (trick.trickMotion == kModifierBarspin) // with a barspin: a flip driver, by its loops
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
                if (line.spinHalfTurns == k360)
                {
                    text[0] = 0;
                    appendModifier(text, trick);
                    appendWord(text, kFlail);
                }
                break;
            case kNoTrick: // a barspin spun 360 or more: a truck driver
                if (trick.trickMotion == kModifierBarspin && line.spinHalfTurns >= k360)
                {
                    text[0] = 0;
                    if (takeoffWords != 0)
                    {
                        if (trick.flags & rider::kScoreTrickBackTakeoff)
                            appendWord(text, kHalfCab);
                        appendLineTakeoff(text, line);
                    }
                    if (line.spinHalfTurns != k360)
                        appendWord(text, nameIn(kSpinNames, line.spinHalfTurns));
                    appendLoops(text, trick);
                    appendWord(text, kTruckDriver);
                }
                break;
            default:
                break;
            }
        }
    }

    void dm2TrickText(ee::Ptr<char> text, const rider::ScoreLine *line, uint32_t takeoffWords)
    {
        text[0] = 0;
        // A spun line: how it landed, the take-off's words and the spin.
        if (line->spinHalfTurns != 0)
        {
            if (line->landingGrade != kLandingNone && line->landingGrade != kLandingToFakie)
                appendWord(text, nameIn(kLandingNames, line->landingGrade));
            if (takeoffWords != 0)
            {
                // (The original reads the first trick's flags with no check
                // that the line has one.)
                if (line->tricks->flags & rider::kScoreTrickBackTakeoff)
                    appendWord(text, kHalfCab);
                appendLineTakeoff(text, *line);
            }
            appendWord(text, nameIn(kSpinNames, line->spinHalfTurns));
        }

        // Each trick: "to" from the second on, a backwards grind's
        // "alley-oop", the modifier and the trick; a spin with neither is
        // a "spin". A pair with a name of its own replaces what is there.
        for (ee::Ptr<ScoreTrick> part = line->tricks; part; part = part->next)
        {
            const ScoreTrick &trick = *part;
            if (part != line->tricks)
                appendWord(text, kTo);
            if (takeoffWords != 0 && (trick.flags & rider::kScoreTrickFakie))
                appendWord(text, kAlleyOop);
            appendModifier(text, trick);
            if (trick.trick != kNoTrick)
                appendWord(text, nameIn(kTrickNames, trick.trick));
            else if (line->spinHalfTurns != 0 && trick.trickMotion == kNoTrick)
                zge::formatString(text + zge::stringLength(text), kSpinFormat, kSpin);
            nameCombination(text, *line, trick, takeoffWords);
        }

        if (line->gap != kNoGap)
        {
            const int32_t end = zge::stringLength(text);
            const uint32_t separator = zge::stringLength(text) != 0 ? kGapSeparator : kNoWord;
            zge::formatString(text + end, kGapFormat, separator, gapName(line->gap));
        }
        if (line->landingGrade == kLandingToFakie)
            appendLastWord(text, nameIn(kLandingNames, kLandingToFakie));
    }
    READABLE_FUNCTION(dm2TrickText, Verified);
}
