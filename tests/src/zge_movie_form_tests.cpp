// The engine's movies and the sound driver (game_overrides/zge_movie_form.cpp): a movie in its PS2 form sends its
// sound through the IOP, so the sound driver stays there. A build with no movie host form (the app before b140 had
// no DM2 instance) ran the PS2 form with the native driver, and DM2's intro waited forever in sceMpegGetPicture.
#include "MiniTest.h"

#include "zge_host.h"

void register_zge_movie_form_tests()
{
    MiniTest::Case("ZgeMovieForm", [](TestCase &tc)
    {
        tc.Run("a build with no movie host form runs the PS2 form, with the sound on the IOP", [](TestCase &t)
        {
            const zge_host::MovieForm form = zge_host::movieFormFor(false, false);
            t.IsTrue(form == zge_host::MovieForm::Ps2, "the PS2 form when the host form is not installed");
            t.IsTrue(zge_host::soundOnIop(nullptr, form), "ZAUDIO on the IOP, with ZX_SOUND unset");
        });

        tc.Run("the host form keeps the native sound driver", [](TestCase &t)
        {
            const zge_host::MovieForm form = zge_host::movieFormFor(false, true);
            t.IsTrue(form == zge_host::MovieForm::Host, "the host form when installed");
            t.IsFalse(zge_host::soundOnIop(nullptr, form), "the native driver");
            t.IsTrue(zge_host::soundOnIop("spu2", form), "unless ZX_SOUND=spu2");
        });

        tc.Run("ZX_MOVIES=hle runs the PS2 form, with the sound on the IOP", [](TestCase &t)
        {
            const zge_host::MovieForm form = zge_host::movieFormFor(true, true);
            t.IsTrue(form == zge_host::MovieForm::Ps2, "the PS2 form");
            t.IsTrue(zge_host::soundOnIop(nullptr, form), "ZAUDIO on the IOP");
        });

        tc.Run("the sound driver reads the form the movies' install noted", [](TestCase &t)
        {
            const zge_host::MovieForm before = zge_host::movieForm();
            zge_host::setMovieForm(zge_host::movieFormFor(false, false));
            t.IsTrue(zge_host::movieForm() == zge_host::MovieForm::Ps2, "noted: the PS2 form");
            zge_host::setMovieForm(before);
        });
    });
}
