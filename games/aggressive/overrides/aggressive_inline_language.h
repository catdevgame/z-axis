#pragma once

// Aggressive Inline (SLUS-20327) in French, German or Spanish, with the
// text of the game's Xbox release (runtime/xbox_disc.h: files bundled
// when the app is built, never a disc image at run time).
//
// ZX_LANGUAGE = english (us) | french | german | spanish (uk: the PS2
// disc's STRING.EUK, developer use). With French, German or Spanish and the
// Xbox files there, the game shows:
//   - every text it draws (menus, HUD, challenge names and descriptions,
//     tutorial tips, pause menu, the port's own Settings pages) through an
//     English -> language table, applied in the two text primitives
//     (aggressive_inline_prompts.cpp), so the scripts' logic, which goes by
//     the English item texts ("Goto SelectedText"), is untouched. The table
//     comes from the Xbox string tables (string.eus and string.fre/.ger/
//     .spa, 438 keys), the Xbox level scripts (levels/<L>/<l>.bni English
//     and .bnf/.bng/.bns, strings paired through the script bodies'
//     offsets; also the PS2 .BIN against .bni where the bodies line up) and
//     the port's own labels (written here);
//   - assets\string.eus served with the language's text for the keys whose
//     English text the Xbox table has (trick names and the like);
//   - the Xbox fonts with accented characters (CommonFonts, InGameFonts,
//     FrontEndFonts: each font of the PS2 library that the Xbox library
//     of the same name also has), put into UILIB.ZAL when it is opened,
//     and the text widgets as narrow as the Xbox scripts have them;
//   - the Settings rows' help lines in the language (written here).
// English changes nothing: no file is replaced and nothing is hooked here.
// The text primitives are only hooked with the settings menus
// (ZX_GAME_SETTINGS_MENUS=1, the app), so the language needs those.

#include <functional>
#include <string>
#include <vector>

class PS2Machine;

namespace zx::zaxis
{
    struct ArchiveEntry;
}

namespace aggressive_inline::language
{
    enum class Language
    {
        English,
        French,
        German,
        Spanish,
    };

    // Reads ZX_LANGUAGE; registers the string table patch. True when a
    // language other than English is shown with the Xbox text.
    bool install(PS2Machine &runtime);
    bool active();
    Language current();

    // The text to draw in place of `english` (Latin-1, as the game's), or
    // empty to draw it as it is.
    std::string translate(const std::string &english);

    // Texts left in English while `keep` answers true for them (a level whose
    // own scripts the Xbox level scripts' translations are not of: TUT/Estate
    // keeps its tutorial's texts). One filter.
    void keepUntranslated(std::function<bool(const std::string &english)> keep);

    // UILIB.ZAL as the game opens it (aggressive_inline_settings.cpp): the
    // Xbox fonts in place of the PS2 ones, and the text widgets the Xbox
    // scripts narrow for their longer texts as narrow (the Tutorial's
    // lesson list in the pause menu). True if an entry changed.
    bool patchUiLibrary(std::vector<zx::zaxis::ArchiveEntry> &entries);

    // French speech (the Xbox speech01.zsd) in place of SPEECH.ZSD's
    // English: the settings row audio.french_voices when there is one, else
    // ZX_FRENCH_VOICES=1, else on with French. Any thread.
    bool frenchVoices();
}
