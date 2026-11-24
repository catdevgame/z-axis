#if defined(__ANDROID__)

#include "platform/platform.h"

#include "android_documents.h"
#include "android_updater.h"
#include "collection/refresh_rate.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <iostream>
#include <vector>

// Android's: the system's dialogs through SDL (its message boxes are the
// activity's own), the documents UI for discs (android_documents.cpp); no
// menu bar, and the window is always the whole screen.

namespace zx_app::platform
{
    std::filesystem::path resourceFolder(const char *argv0)
    {
        // (android_app.cpp's prepareApp makes argv[0] the external files folder's)
        std::error_code ec;
        return std::filesystem::absolute(argv0 ? argv0 : "", ec).parent_path();
    }

    void showMessage(const std::string &title, const std::string &message, bool critical)
    {
        std::cerr << "[app] " << title << ": " << message << std::endl;
        SDL_ShowSimpleMessageBox(critical ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, title.c_str(), message.c_str(),
                                 nullptr);
    }

    bool ask(const std::string &title, const std::string &message, const std::string &yes, const std::string &no)
    {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no.c_str()},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes.c_str()},
        };
        const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, title.c_str(), message.c_str(), 2, buttons, nullptr};
        int pressed = 0;
        if (!SDL_ShowMessageBox(&box, &pressed))
        {
            std::cerr << "[app] " << title << ": " << message << " (" << no << ": " << SDL_GetError() << ")" << std::endl;
            return false;
        }
        return pressed == 1;
    }

    void installMenus(const MenuActions &actions)
    {
        (void)actions;
    }

    bool toggleFullscreen()
    {
        return false;
    }

    bool isFullscreen(bool &known)
    {
        known = false;
        return false;
    }

    bool setBorderless(bool on)
    {
        (void)on;
        return false;
    }

    bool isBorderless()
    {
        return false;
    }

    void setOpenSettingsAction(std::function<void()> action, std::function<bool()> available)
    {
        (void)action;
        (void)available;
    }

    void frameInterpolationChanged(bool on)
    {
        JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
        jobject activity = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
        if (!activity)
            return;
        jclass type = env->GetObjectClass(activity);
        jmethodID offered = env->GetMethodID(type, "offeredRefreshRates", "()[F");
        jmethodID ask = env->GetMethodID(type, "askRefreshRate", "(F)V");
        std::vector<float> rates;
        if (offered && ask)
        {
            auto array = static_cast<jfloatArray>(env->CallObjectMethod(activity, offered));
            if (!env->ExceptionCheck() && array)
            {
                rates.resize(static_cast<size_t>(env->GetArrayLength(array)));
                env->GetFloatArrayRegion(array, 0, static_cast<jsize>(rates.size()), rates.data());
                env->DeleteLocalRef(array);
            }
            if (!env->ExceptionCheck())
                env->CallVoidMethod(activity, ask, static_cast<jfloat>(zx::app::refreshRateToAsk(on, rates)));
        }
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(activity);
    }

    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked)
    {
        return zx_android::pickDocuments(folder, true, std::move(picked));
    }

    bool pickDocumentsOnce(bool folder, std::function<void(std::vector<std::string>)> picked)
    {
        return zx_android::pickDocuments(folder, false, std::move(picked));
    }

    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created)
    {
        return zx_android::createDocument(name, mimeType, std::move(created));
    }

    bool pickFolderOrFile(const std::string &extension, std::function<void(std::vector<std::string>)> picked)
    {
        // (the documents UI opens a folder's tree or documents, never both at once:
        // the menu asks which, then pickDocuments)
        (void)extension;
        (void)picked;
        return false;
    }

    zx::collection::UpdatePlatform updatePlatform(const char *argv0)
    {
        (void)argv0;
        return zx_android::updatePlatform();
    }
}

#endif // __ANDROID__
