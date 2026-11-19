#pragma once
// The script functions BMX XXX's menus call ("bmxmenuscript..." in the UI's
// scripts; each function here has that name): BMX XXX's own code (its menu
// block). A script function gets its arguments as a list of the script's
// variables.

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstdint>

namespace game::bmx
{
    // (name, loop): the movie file played on the movie material.
    void bmxmenuscriptPlayMovie(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptStopMovie();
    // The UI's change, cancel and select sounds, where the UI has them.
    void bmxmenuscriptPlayOnChangeSound();
    void bmxmenuscriptPlayOnCancelSound();
    void bmxmenuscriptPlayOnSelectSound();
    // The current player's pad shaken for 300 ms.
    void bmxmenuscriptVibrateController();
    // (player, screen): the current player's controller configuration and
    // vibration setting shown on the screen ("SelectedIndex",
    // "Vibration.isOn"), and its "Vibration.Refresh" played.
    void bmxmenuscriptGetControllerConfig(ee::Ptr<uint32_t> arguments);
    // (list): the cheat list's none-yet line; the options menu's items.
    void bmxmenuscriptPopulateCheatList(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptGetOptionsMenuItems(ee::Ptr<uint32_t> arguments);
    // (player, screen): the screen's two settings taken for both players.
    void bmxmenuscriptSetControllerConfig(ee::Ptr<uint32_t> arguments);
    // (value, test): a sound channel's volume set from a slider's value
    // (90 when it has none; kept to 0 .. 127) and saved, with a test sound
    // unless the second argument says no.
    void bmxmenuscriptSetSFXVolume(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptSetSpeechVolume(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptSetMusicVolume(ee::Ptr<uint32_t> arguments);
    // (slider): the saved volume, rounded up to even and at most 128, set
    // as the slider's "CurValue", then the slider's "OnChange" played.
    void bmxmenuscriptGetSFXVolume(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptGetSpeechVolume(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptGetMusicVolume(ee::Ptr<uint32_t> arguments);
    // (track): the track taken off the playlist when it is on, else put on.
    void bmxmenuscriptEnableDisableTrack(ee::Ptr<uint32_t> arguments);
    // (widget): the next, or the previous, track that is on (round the
    // list; the one playing when no other is): it is played and the widget
    // shows its label; with none on, the widget says so.
    void bmxmenuscriptNextTrack(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptPrevTrack(ee::Ptr<uint32_t> arguments);
    // A track's label, "artist - title", with the string table's texts put
    // in, cut with "..." to fit 950 across in the menus' font at `scale`.
    // The label is in one shared buffer.
    ee::Ptr<char> trackLabel(uint32_t track, const zge::Vec4 *scale, uint32_t monospaced, ee::Float angle, ee::Float lineSpacing);
    // The menus' language, and setting it; (level, frame): the records
    // screen's rows of a level.
    void bmxmenuscriptGetLanguage(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptSetLanguage(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptRecordsUpdate(ee::Ptr<uint32_t> arguments);
    // The front end's music on and off.
    int32_t bmxmenuscriptStartMusic();
    int32_t bmxmenuscriptStopMusic();
    // The video mode: its rate, and the screen's size for the cameras.
    void bmxmenuscriptSetVideoModeToPAL();
    void bmxmenuscriptSetVideoModeToNTSC();
    // (out): the current player's rider character, whether the player
    // created it, the current player, the session's kind.
    void bmxmenuscriptGetCharacterNum(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptIsPlayerCharCreated(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptGetPlayerNum(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptGetGameActionMode(ee::Ptr<uint32_t> arguments);
    // (member, out): a team member's rider character.
    void bmxmenuscriptGetTeamMemCharNum(ee::Ptr<uint32_t> arguments);
    // (out, item): 1 when the item's selected text starts with '!'.
    void bmxmenuscriptSelectedDisabledMenuItem(ee::Ptr<uint32_t> arguments);
    // (list): the playlist's tracks with their checkboxes, and the track
    // playing.
    void bmxmenuscriptPopulatePlaylist(ee::Ptr<uint32_t> arguments);
    // The media viewer's movie table and its size; (-, list): its movies'
    // titles; (out, -, list): the file of the movie the list selected.
    void mediaViewerMovies(uint32_t unused, ee::Ptr<uint32_t> movies, ee::Ptr<uint32_t> count);
    void bmxmenuscriptPopulateMediaViewerMenu(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptGetSelectedMediaFileName(ee::Ptr<uint32_t> arguments);
    // (names, settings): the five special modes' names and whether each is
    // on; (list): the team's seven places' names.
    void bmxmenuscriptPopulateSpecialModeMenu(ee::Ptr<uint32_t> arguments);
    void bmxmenuscriptGetTeamNames(ee::Ptr<uint32_t> arguments);
    // The pools' empty blocks released, then, the first time only, every
    // script function above (and the menus' others) registered under its
    // name for the UI's scripts. Always 1.
    int32_t registerMenuScriptFunctions();
    // The front end's frame and exit hooks into the menu block: empty (the
    // frame's returns 1).
    int32_t unusedFrontEndFrameHook();
    void unusedFrontEndExitHook();
}
