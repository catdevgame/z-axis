// The menus' sounds (menu_sounds.h): DM2's own code. zgui's five sound hooks
// live in action.c's range and the registration in the sound events'; each
// hook is one sound event (Aggressive Inline's playSelectSound ..
// playBlockedSound, sound/events.cpp, with its own event numbers), and the
// registration is AI's registerSoundMenuFunctions without the hooks, which
// DM2's UI set-up stores itself.

#include "menu_sounds.h"
#include "../menu/menu_variables.h"

#include "sound_events.h"
#include "sound_state.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "zge/math/conversions.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/sound/menu_sounds.cpp.h"

namespace game::dm2
{
    namespace
    {
        // soundEvent's menu sounds (the menu bank's samples 0, 1, 2, 3 or 5
        // by the sound mode, 4).
        constexpr uint32_t kMoveSoundEvent = 6, kChangeSoundEvent = 7, kSelectSoundEvent = 8, kCancelSoundEvent = 9,
                           kBlockedSoundEvent = 10;

        // soundEvent 0: a volume set: the SFX volume (0) or the music's (1).
        constexpr uint32_t kVolumeEvent = 0, kSfxVolume = 0, kMusicVolume = 1;
        // The game's volume settings (saved with the game; the level set-up
        // defaults them to 0x80 and 0x40).
        inline ee::Global<uint8_t, sym::g_sfxVolumeSetting> g_sfxVolumeSetting;
        inline ee::Global<uint8_t, sym::g_musicVolumeSetting> g_musicVolumeSetting;

        constexpr ee::Ptr<const char> kUiUpdateName{sym::uiUpdateName};                   // "bmxsoundUIUpdate"
        constexpr ee::Ptr<const char> kChangedTrackName{sym::changedTrackName};           // "bmxsoundChangedTrack"
        constexpr ee::Ptr<const char> kChangedSoundtrackName{sym::changedSoundtrackName}; // "bmxsoundChangedSoundtrack"
    }

    // playSelectSound .. playBlockedSound: zgui's sound hooks.
    uint32_t playSelectSound()
    {
        return static_cast<uint32_t>(game::tail(soundEvent, kSelectSoundEvent, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(playSelectSound, Verified);

    uint32_t playCancelSound()
    {
        return static_cast<uint32_t>(game::tail(soundEvent, kCancelSoundEvent, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(playCancelSound, Verified);

    uint32_t playMoveSound()
    {
        return static_cast<uint32_t>(game::tail(soundEvent, kMoveSoundEvent, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(playMoveSound, Verified);

    uint32_t playChangeSound()
    {
        return static_cast<uint32_t>(game::tail(soundEvent, kChangeSoundEvent, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(playChangeSound, Verified);

    uint32_t playBlockedSound()
    {
        return static_cast<uint32_t>(game::tail(soundEvent, kBlockedSoundEvent, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(playBlockedSound, Verified);

    // registerSoundMenuFunctions: the soundtrack page's three script
    // functions registered (the options menu's music track and soundtrack
    // choices). Always 1.
    uint32_t registerSoundMenuFunctions()
    {
        zge::zgui::registerScriptFunction(kUiUpdateName, sym::inlinesoundUIUpdate);
        zge::zgui::registerScriptFunction(kChangedTrackName, sym::inlinesoundChangedTrack);
        zge::zgui::registerScriptFunction(kChangedSoundtrackName, sym::inlinesoundChangedSoundtrack);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerSoundMenuFunctions, Verified);

    // bmxUpdateVolumeInfo: the script's "BmxUpdateVolumeInfo": the options
    // menu's volumes kept as the game's settings, and the sound told both
    // (the SFX volume, then the music's, whose event's result it answers).
    uint32_t bmxUpdateVolumeInfo()
    {
        *g_musicVolumeSetting = static_cast<uint8_t>(*g_currentMusicVolume);
        *g_sfxVolumeSetting = static_cast<uint8_t>(*g_currentSfxVolume);
        soundEvent(kVolumeEvent, kSfxVolume, *g_currentSfxVolume, 0, 0);
        return static_cast<uint32_t>(game::tail(soundEvent, kVolumeEvent, kMusicVolume, *g_currentMusicVolume, 0, 0));
    }
    READABLE_FUNCTION_WAITS(bmxUpdateVolumeInfo, Verified);

    // bmxPlayMusicVolume: the script's "BmxPlayMusicVolume": the music
    // volume previewed: the music and the SFX volume both set to it, the
    // change sound played, then the SFX volume put back (that event's
    // result answered).
    uint32_t bmxPlayMusicVolume()
    {
        soundEvent(kVolumeEvent, kMusicVolume, *g_currentMusicVolume, 0, 0);
        soundEvent(kVolumeEvent, kSfxVolume, *g_currentMusicVolume, 0, 0);
        soundEvent(kChangeSoundEvent, 0, 0, 0, 0);
        return static_cast<uint32_t>(game::tail(soundEvent, kVolumeEvent, kSfxVolume, *g_currentSfxVolume, 0, 0));
    }
    READABLE_FUNCTION_WAITS(bmxPlayMusicVolume, Verified);

    namespace
    {
        // The pause menu's soundtrack page (InGameUI.FrameOptions.Menu): the
        // track and the soundtrack chosen, each a CurValue of MaxValue and a
        // caption.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        constexpr ee::Ptr<const char> kMusicTrackWidget{sym::musicTrackWidget}; // "InGameUI.FrameOptions.Menu.MusicTrack"
        constexpr ee::Ptr<const char> kSoundtrackWidget{sym::soundtrackWidget}; // "InGameUI.FrameOptions.Menu.MusicSoundTrack"
        constexpr ee::Ptr<const char> kCurValue{sym::curValue2};                // "CurValue"
        constexpr ee::Ptr<const char> kMaxValue{sym::maxValue};                 // "MaxValue"
        // "SET Caption.Text {"\"%s\"\n%s"}": the track's title and artist;
        // "SET Caption.Text {"%s"}": the soundtrack's name.
        constexpr ee::Ptr<const char> kTrackCaption{sym::trackCaption};
        constexpr ee::Ptr<const char> kSoundtrackCaption{sym::soundtrackCaption};
        constexpr uint32_t kCaptionBytes = 0x100;
        // soundEvent's: 2 the track (asked for, else the one playing), 3 the
        // next track a, 4 the soundtrack's index, 5 the soundtrack of index a.
        constexpr uint32_t kTrackEvent = 2, kNextTrackEvent = 3, kSoundtrackIndexEvent = 4, kSoundtrackEvent = 5;
    }

    // inlinesoundUIUpdate, the soundtrack page's update (the script function
    // "bmxsoundUIUpdate"): the track and the soundtrack chosen shown, each
    // with its largest value, the track's title and artist and the
    // soundtrack's name in the captions.
    void inlinesoundUIUpdate()
    {
        ee::Local<char[kCaptionBytes]> caption;
        ee::Local<ee::Float> track;
        ee::Local<ee::Float> largest;
        ee::Local<ee::Float> soundtrackChosen;
        *track = zge::unsignedToFloat(static_cast<uint32_t>(soundEvent(kTrackEvent, 0, 0, 0, 0)));
        *soundtrackChosen = zge::unsignedToFloat(static_cast<uint32_t>(soundEvent(kSoundtrackIndexEvent, 0, 0, 0, 0)));
        const ee::Ptr<zge::Soundtrack> soundtrack = zge::soundtrackAt(sce::libgcc::fptoui(*soundtrackChosen));

        // The track: CurValue, MaxValue (its soundtrack's tracks less one),
        // the caption.
        const ee::Ptr<zge::zgui::Node> trackWidget = zge::zgui::findNode(*g_uiRoot, kMusicTrackWidget, 0);
        zge::zgui::setVariable(trackWidget, kCurValue, track);
        *largest = zge::unsignedToFloat(zge::soundtrackTrackCount(soundtrack) - 1);
        zge::zgui::setVariable(trackWidget, kMaxValue, largest);
        const ee::Ptr<char> title = zge::soundtrackTrackTitle(soundtrack, sce::libgcc::fptoui(*track));
        const ee::Ptr<char> artist = zge::soundtrackTrackArtist(soundtrack, sce::libgcc::fptoui(*track));
        zge::formatString(caption, kTrackCaption, title, artist);
        zge::zgui::runScriptLines(trackWidget, ee::Ptr<char>(caption.address()));

        // The soundtrack: CurValue, MaxValue (the soundtracks less one), the
        // caption.
        const ee::Ptr<zge::zgui::Node> soundtrackWidget = zge::zgui::findNode(*g_uiRoot, kSoundtrackWidget, 0);
        zge::zgui::setVariable(soundtrackWidget, kCurValue, soundtrackChosen);
        *largest = zge::unsignedToFloat(static_cast<uint32_t>(zge::soundRecordsInUse() - 1));
        zge::zgui::setVariable(soundtrackWidget, kMaxValue, largest);
        zge::formatString(caption, kSoundtrackCaption, zge::soundtrackName(soundtrack));
        zge::zgui::runScriptLines(soundtrackWidget, ee::Ptr<char>(caption.address()));
    }
    READABLE_FUNCTION_WAITS(inlinesoundUIUpdate, Verified);

    // inlinesoundChangedTrack ("bmxsoundChangedTrack"): the track page's
    // value asked for as the next track. Returns soundEvent's.
    int32_t inlinesoundChangedTrack()
    {
        ee::Local<ee::Float> value;
        const ee::Ptr<zge::zgui::Node> widget = zge::zgui::findNode(*g_uiRoot, kMusicTrackWidget, 0);
        zge::zgui::copyVariable(widget, kCurValue, value);
        return soundEvent(kNextTrackEvent, sce::libgcc::fptoui(*value), 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(inlinesoundChangedTrack, Draft);

    // inlinesoundChangedSoundtrack ("bmxsoundChangedSoundtrack"): the
    // soundtrack page's value made the soundtrack, its first track asked for
    // next. Returns soundEvent's.
    int32_t inlinesoundChangedSoundtrack()
    {
        ee::Local<ee::Float> value;
        const ee::Ptr<zge::zgui::Node> widget = zge::zgui::findNode(*g_uiRoot, kSoundtrackWidget, 0);
        zge::zgui::copyVariable(widget, kCurValue, value);
        soundEvent(kSoundtrackEvent, sce::libgcc::fptoui(*value), 0, 0, 0);
        return soundEvent(kNextTrackEvent, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(inlinesoundChangedSoundtrack, Draft);
}
