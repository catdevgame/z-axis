// The Android build's own files in its APK (android_assets.h): the asset
// manager of SDL's activity, and the package manager for the APK's version.
#if defined(__ANDROID__)

#include "android_assets.h"

#include <SDL3/SDL.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

namespace zx_android
{
    namespace
    {
        JNIEnv *jniEnv() { return static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv()); }

        // An exception a call left pending is cleared (and the call reported as failed).
        bool failed(JNIEnv *env)
        {
            if (!env->ExceptionCheck())
                return false;
            env->ExceptionClear();
            return true;
        }

        // The activity's AssetManager, kept as a global reference: its native
        // side lives as long as the Java object does.
        AAssetManager *assetManager()
        {
            static jobject g_assets = nullptr;
            static AAssetManager *g_manager = nullptr;
            if (g_manager)
                return g_manager;
            JNIEnv *env = jniEnv();
            jobject activity = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
            if (!activity)
                return nullptr;
            jclass type = env->GetObjectClass(activity);
            jmethodID getAssets = env->GetMethodID(type, "getAssets", "()Landroid/content/res/AssetManager;");
            jobject assets = getAssets ? env->CallObjectMethod(activity, getAssets) : nullptr;
            if (failed(env))
                assets = nullptr;
            if (assets)
            {
                g_assets = env->NewGlobalRef(assets);
                g_manager = AAssetManager_fromJava(env, g_assets);
                env->DeleteLocalRef(assets);
            }
            env->DeleteLocalRef(type);
            env->DeleteLocalRef(activity);
            return g_manager;
        }
    }

    bool readAsset(const std::string &name, std::vector<uint8_t> &out)
    {
        out.clear();
        AAssetManager *manager = assetManager();
        AAsset *asset = manager ? AAssetManager_open(manager, name.c_str(), AASSET_MODE_STREAMING) : nullptr;
        if (!asset)
            return false;
        bool ok = true;
        uint8_t chunk[64 * 1024];
        for (int got; (got = AAsset_read(asset, chunk, sizeof(chunk))) != 0;)
        {
            if (got < 0)
            {
                ok = false;
                break;
            }
            out.insert(out.end(), chunk, chunk + got);
        }
        AAsset_close(asset);
        return ok;
    }

    long long apkVersionCode()
    {
        JNIEnv *env = jniEnv();
        jobject activity = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
        if (!activity)
            return -1;
        long long code = -1;
        jclass type = env->GetObjectClass(activity);
        jmethodID getManager = env->GetMethodID(type, "getPackageManager", "()Landroid/content/pm/PackageManager;");
        jmethodID getName = env->GetMethodID(type, "getPackageName", "()Ljava/lang/String;");
        jobject manager = getManager ? env->CallObjectMethod(activity, getManager) : nullptr;
        jobject name = !failed(env) && getName ? env->CallObjectMethod(activity, getName) : nullptr;
        if (!failed(env) && manager && name)
        {
            jclass managerType = env->GetObjectClass(manager);
            jmethodID getInfo =
                env->GetMethodID(managerType, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
            jobject info = getInfo ? env->CallObjectMethod(manager, getInfo, name, 0) : nullptr;
            if (!failed(env) && info)
            {
                jclass infoType = env->GetObjectClass(info);
                jmethodID getCode = env->GetMethodID(infoType, "getLongVersionCode", "()J");
                if (getCode)
                {
                    const jlong value = env->CallLongMethod(info, getCode);
                    if (!failed(env))
                        code = static_cast<long long>(value);
                }
                env->DeleteLocalRef(infoType);
                env->DeleteLocalRef(info);
            }
            env->DeleteLocalRef(managerType);
        }
        if (manager)
            env->DeleteLocalRef(manager);
        if (name)
            env->DeleteLocalRef(name);
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(activity);
        return code;
    }
}

#endif // __ANDROID__
