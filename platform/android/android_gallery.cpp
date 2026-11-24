// The Android build's screenshots in the system's Gallery: the PNG the game
// saved in the app's folder copied into MediaStore by Gallery.java.
#if defined(__ANDROID__)

#include "android_gallery.h"

#include <SDL3/SDL.h>
#include <jni.h>

namespace zx_android
{
    namespace
    {
        // Gallery.java and its method (nativeInstall: before any use; native
        // threads find no app class by name).
        jclass g_class = nullptr;
        jmethodID g_add = nullptr;
    }

    std::string addToGallery(const std::filesystem::path &file, const std::string &folder)
    {
        JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
        if (!env || !g_class)
            return {};
        std::string uri;
        env->PushLocalFrame(8);
        auto added = static_cast<jstring>(env->CallStaticObjectMethod(g_class, g_add, env->NewStringUTF(file.string().c_str()),
                                                                     env->NewStringUTF(file.filename().string().c_str()),
                                                                     env->NewStringUTF(folder.c_str())));
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        else if (added)
        {
            const char *chars = env->GetStringUTFChars(added, nullptr);
            uri = chars;
            env->ReleaseStringUTFChars(added, chars);
        }
        env->PopLocalFrame(nullptr);
        return uri;
    }
}

// Gallery.install (GameActivity.onCreate, the library loaded).
extern "C" JNIEXPORT void JNICALL Java_io_github_catdevgame_zaxiscollection_Gallery_nativeInstall(JNIEnv *env, jclass gallery)
{
    using namespace zx_android;
    if (!g_class)
    {
        g_class = static_cast<jclass>(env->NewGlobalRef(gallery));
        g_add = env->GetStaticMethodID(gallery, "addPicture",
                                       "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    }
}

#endif // __ANDROID__
