// The app's own update on Android: collection/updater.h's Package way through
// Updater.java (the download, the APK's version, the system's package
// installer), called from the updater's thread.
#if defined(__ANDROID__)

#include "android_updater.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace zx_android
{
    namespace
    {
        // Updater.java and its methods (nativeInstall: before any use).
        jclass g_class = nullptr;
        jmethodID g_installedApk = nullptr, g_download = nullptr, g_versionOf = nullptr, g_installPackage = nullptr;

        std::mutex g_doneMutex;
        std::function<void(std::string)> g_done;

        JNIEnv *jniEnv() { return static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv()); }

        // A Java string's text (paths, messages), "" for null.
        std::string text(JNIEnv *env, jstring string)
        {
            if (!string)
                return {};
            const char *chars = env->GetStringUTFChars(string, nullptr);
            std::string out = chars ? chars : "";
            if (chars)
                env->ReleaseStringUTFChars(string, chars);
            return out;
        }

        // One static String call into Updater.java in a local frame of its own (the
        // updater's thread, attached by SDL, frees no local reference itself).
        template <typename... Args>
        std::string callString(jmethodID method, Args... args)
        {
            JNIEnv *env = jniEnv();
            if (!env || !g_class)
                return "no updater";
            env->PushLocalFrame(16);
            std::string out = text(env, static_cast<jstring>(env->CallStaticObjectMethod(g_class, method, args(env)...)));
            if (env->ExceptionCheck())
            {
                env->ExceptionDescribe();
                env->ExceptionClear();
                out = "the updater's Java side failed";
            }
            env->PopLocalFrame(nullptr);
            return out;
        }

        auto string(const std::string &value)
        {
            return [value](JNIEnv *env) { return env->NewStringUTF(value.c_str()); };
        }
    }

    zx::collection::UpdatePlatform updatePlatform()
    {
        zx::collection::UpdatePlatform update;
        const char *internal = SDL_GetAndroidInternalStoragePath();
        if (!g_class || !internal)
            return update;
        const std::string apk = callString(g_installedApk);
        if (apk.empty())
            return update;
        update.install = zx::collection::UpdatePlatform::Install::Package;
        update.packageSuffixes = {"-android-arm64.apk", "-android.apk"};
        update.installed = apk;
        // (the internal storage: the app's own, never the player's support folder in the external one; only the download:
        // Android keeps no older version for a roll back)
        update.updateFolder = std::filesystem::path(internal) / "update";
        update.download = [](const std::string &url, const std::filesystem::path &to, std::string *error)
        {
            const std::string why = callString(g_download, string(url), string(to.string()));
            if (!why.empty() && error)
                *error = why;
            return why.empty();
        };
        update.packageVersion = [](const std::filesystem::path &package) { return callString(g_versionOf, string(package.string())); };
        update.installPackage = [](const std::filesystem::path &package, std::function<void(std::string)> done, std::string *error)
        {
            {
                std::lock_guard lock(g_doneMutex);
                g_done = std::move(done);
            }
            const std::string why = callString(g_installPackage, string(package.string()));
            if (why.empty())
                return true;
            {
                std::lock_guard lock(g_doneMutex);
                g_done = nullptr;
            }
            if (error)
                *error = why;
            return false;
        };
        return update;
    }
}

// Updater.install (GameActivity.onCreate, the library loaded): the class for
// the calls from the updater's thread, which finds no app class by name.
extern "C" JNIEXPORT void JNICALL Java_io_github_catdevgame_zaxiscollection_Updater_nativeInstall(JNIEnv *env, jclass updater)
{
    using namespace zx_android;
    if (g_class)
        return;
    g_class = static_cast<jclass>(env->NewGlobalRef(updater));
    g_installedApk = env->GetStaticMethodID(updater, "installedApk", "()Ljava/lang/String;");
    g_download = env->GetStaticMethodID(updater, "download", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    g_versionOf = env->GetStaticMethodID(updater, "versionOf", "(Ljava/lang/String;)Ljava/lang/String;");
    g_installPackage = env->GetStaticMethodID(updater, "installPackage", "(Ljava/lang/String;)Ljava/lang/String;");
}

// The installer's answer (the UI thread): "" the app is being replaced, else why not.
extern "C" JNIEXPORT void JNICALL Java_io_github_catdevgame_zaxiscollection_Updater_nativeDone(JNIEnv *env, jclass, jstring why)
{
    using namespace zx_android;
    std::function<void(std::string)> done;
    {
        std::lock_guard lock(g_doneMutex);
        done = std::move(g_done);
        g_done = nullptr;
    }
    if (done)
        done(text(env, why));
}

#endif // __ANDROID__
