// BMX XXX's path men (pathman_phrases.h): the phrases one speaks ("proximity",
// "hit", "trick", "crash", "pickup", "ride", "sit" ...). BMX XXX's own code.

#include "pathman_phrases.h"
#include "pathman.h"
#include "pathman_calls.h"
#include "pathman_random.h"

#include "bmx/sound/events.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/bmx/pathman/pathman_phrases.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        constexpr uint32_t kPhraseFormat = sym::bmx::pathManPhraseFormat; // ".phrase.%s"
        // The phrases' names, by PathManPhrase.
        inline ee::Global<ee::Ptr<const char>[9], sym::pathManPhraseNames> g_phraseNames;

        // The capital of each small letter, 'a' first.
        inline ee::Global<uint8_t[26], sym::g_upperCaseLetters> g_upperCaseLetters;

        // The hash keeps 28 bits: the four above them are folded back in.
        constexpr uint32_t kHashOverflow = 0xF0000000u;
    }

    // phraseHash: each character shifted in four bits up (a small letter as
    // its capital), the bits that leave the 28 folded back in lower down.
    uint32_t phraseHash(ee::Ptr<const uint8_t> text, uint32_t hash)
    {
        for (uint32_t character = *text; character != 0; character = *text)
        {
            ++text;
            if (static_cast<uint8_t>(character - 'a') < 26)
                character = (*g_upperCaseLetters)[character - 'a'];
            hash = (hash << 4) + character;
            const uint32_t overflow = hash & kHashOverflow;
            if (overflow != 0)
                hash ^= overflow >> 24;
            hash &= ~overflow;
        }
        return hash;
    }
    READABLE_FUNCTION(phraseHash, Verified);

    // sayPickupPhrase
    uint32_t sayPickupPhrase(ee::Ptr<SceneObject> pathMan)
    {
        return game::tail(sayPathManPhrase, pathMan, kPhrasePickup);
    }
    READABLE_FUNCTION_WAITS(sayPickupPhrase, Draft);

    // sayRidePhrase: called each frame the path man is ridden along with:
    // it speaks its "ride" phrase when its wait for a phrase runs out, and
    // then waits a random time again.
    void sayRidePhrase(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (record->phraseFrames != 0)
        {
            --record->phraseFrames;
            if (record->phraseFrames != 0)
                return;
        }
        sayPathManPhrase(pathMan, kPhraseRide);
        record->phraseFrames = static_cast<uint16_t>(pathManRandom(pathMan, kPhraseWaitSpread) + kLeastPhraseWait);
    }
    READABLE_FUNCTION_WAITS(sayRidePhrase, Verified);

    // sayPathManPhrase: the path man speaks: the voice sound named by its
    // own name and ".phrase." with the phrase's name, played where it
    // stands. Always 1.
    uint32_t sayPathManPhrase(ee::Ptr<SceneObject> pathMan, uint32_t phrase)
    {
        ee::Local<char[0x40]> phraseName;
        ee::Local<Vec4> position;
        formatString(phraseName, kPhraseFormat, (*g_phraseNames)[phrase]);
        const ee::Ptr<Vec4> at = pathManPosition(pathMan, position);
        const uint32_t speaker = objectNameHash(pathMan);
        const uint32_t sound = phraseHash(ee::Ptr<const uint8_t>(phraseName.address()), speaker);
        soundEvent(pathman::kVoiceSoundEvent, at, sound, 0, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sayPathManPhrase, Verified);
}
