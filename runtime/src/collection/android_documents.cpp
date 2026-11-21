// The Android build's documents: the setup's disc images and folders as the
// system's documents UI gives them (content:// URIs, read through the
// descriptors the content resolver opens), for collection/game_setup.h's
// DocumentAccess, and the picker behind platform::pickDocuments. The Java
// side is android/src/.../Documents.java.
#if defined(__ANDROID__)

#include "android_documents.h"
#include "collection/game_setup.h"

#include <SDL3/SDL.h>
#include <jni.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zx_android
{
    namespace
    {
        // Documents.java and its methods (nativeInstall: before any use).
        jclass g_class = nullptr;
        jmethodID g_pick = nullptr, g_create = nullptr, g_stat = nullptr, g_list = nullptr, g_open = nullptr;

        std::mutex g_pickedMutex;
        std::function<void(std::vector<std::string>)> g_picked;

        JNIEnv *jniEnv() { return static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv()); }

        // A Java string as UTF-8 (JNI's own "UTF" is modified UTF-8, which
        // writes characters past the BMP as two surrogates).
        std::string utf8(JNIEnv *env, jstring text)
        {
            std::string out;
            if (!text)
                return out;
            const jsize length = env->GetStringLength(text);
            const jchar *chars = env->GetStringChars(text, nullptr);
            for (jsize i = 0; i < length; ++i)
            {
                uint32_t c = chars[i];
                if (c >= 0xD800 && c < 0xDC00 && i + 1 < length && chars[i + 1] >= 0xDC00 && chars[i + 1] < 0xE000)
                    c = 0x10000 + ((c - 0xD800) << 10) + (chars[++i] - 0xDC00);
                if (c < 0x80)
                    out += static_cast<char>(c);
                else if (c < 0x800)
                {
                    out += static_cast<char>(0xC0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
                else if (c < 0x10000)
                {
                    out += static_cast<char>(0xE0 | (c >> 12));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xF0 | (c >> 18));
                    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            env->ReleaseStringChars(text, chars);
            return out;
        }

        // The strings of a String[] (null: none).
        std::vector<std::string> strings(JNIEnv *env, jobjectArray array)
        {
            std::vector<std::string> out;
            if (!array)
                return out;
            const jsize count = env->GetArrayLength(array);
            for (jsize i = 0; i < count; ++i)
            {
                auto item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
                out.push_back(utf8(env, item));
                env->DeleteLocalRef(item);
            }
            return out;
        }

        // One call into Documents.java in a local frame of its own (the
        // setup's thread, attached by SDL, frees no local reference itself),
        // its exception (none expected: the Java side catches its own)
        // cleared.
        template <typename Call>
        auto inFrame(JNIEnv *env, Call call)
        {
            env->PushLocalFrame(16);
            auto result = call();
            if (env->ExceptionCheck())
            {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
            env->PopLocalFrame(nullptr);
            return result;
        }

        class AndroidDocuments final : public zx::app::DocumentAccess
        {
        public:
            bool isDocument(std::string_view text) const override { return text.rfind("content://", 0) == 0; }

            bool stat(const std::string &uri, Entry &out) const override
            {
                JNIEnv *env = jniEnv();
                if (!env || !g_class)
                    return false;
                const std::vector<std::string> fields = inFrame(env, [&]()
                {
                    return strings(env, static_cast<jobjectArray>(env->CallStaticObjectMethod(g_class, g_stat, env->NewStringUTF(uri.c_str()))));
                });
                if (fields.size() != 3u)
                    return false;
                out = {uri, fields[0], std::strtoull(fields[1].c_str(), nullptr, 10), fields[2] == "1"};
                return true;
            }

            std::vector<Entry> list(const std::string &folderUri) const override
            {
                std::vector<Entry> out;
                JNIEnv *env = jniEnv();
                if (!env || !g_class)
                    return out;
                const std::vector<std::string> fields = inFrame(env, [&]()
                {
                    return strings(env, static_cast<jobjectArray>(env->CallStaticObjectMethod(g_class, g_list, env->NewStringUTF(folderUri.c_str()))));
                });
                for (size_t i = 0; i + 4 <= fields.size(); i += 4)
                    out.push_back({fields[i], fields[i + 1], std::strtoull(fields[i + 2].c_str(), nullptr, 10), fields[i + 3] == "1"});
                return out;
            }

            std::FILE *open(const std::string &uri) const override
            {
                JNIEnv *env = jniEnv();
                if (!env || !g_class)
                    return nullptr;
                const int fd = inFrame(env, [&]()
                {
                    return static_cast<int>(env->CallStaticIntMethod(g_class, g_open, env->NewStringUTF(uri.c_str())));
                });
                if (fd < 0)
                    return nullptr;
                std::FILE *file = fdopen(fd, "rb");
                if (!file)
                    close(fd);
                return file;
            }
        };

        AndroidDocuments g_documents;
    }

    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked)
    {
        JNIEnv *env = jniEnv();
        if (!env || !g_class)
            return false;
        {
            std::lock_guard lock(g_pickedMutex);
            g_picked = std::move(picked);
        }
        return inFrame(env, [&]() { return env->CallStaticBooleanMethod(g_class, g_pick, static_cast<jboolean>(folder)) == JNI_TRUE; });
    }

    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created)
    {
        JNIEnv *env = jniEnv();
        if (!env || !g_class)
            return false;
        {
            std::lock_guard lock(g_pickedMutex);
            g_picked = [created = std::move(created)](std::vector<std::string> uris) { created(std::move(uris.front())); };
        }
        return inFrame(env, [&]()
        {
            return env->CallStaticBooleanMethod(g_class, g_create, env->NewStringUTF(name.c_str()), env->NewStringUTF(mimeType.c_str())) ==
                   JNI_TRUE;
        });
    }
}

// Documents.install (GameActivity.onCreate, the library loaded): the class
// for the calls from native threads, which find no app class by name.
extern "C" JNIEXPORT void JNICALL Java_io_github_catdevgame_zaxiscollection_Documents_nativeInstall(JNIEnv *env, jclass documents)
{
    using namespace zx_android;
    if (!g_class)
    {
        g_class = static_cast<jclass>(env->NewGlobalRef(documents));
        g_pick = env->GetStaticMethodID(documents, "pick", "(Z)Z");
        g_create = env->GetStaticMethodID(documents, "create", "(Ljava/lang/String;Ljava/lang/String;)Z");
        g_stat = env->GetStaticMethodID(documents, "stat", "(Ljava/lang/String;)[Ljava/lang/String;");
        g_list = env->GetStaticMethodID(documents, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
        g_open = env->GetStaticMethodID(documents, "open", "(Ljava/lang/String;)I");
    }
    zx::app::installDocumentAccess(&g_documents);
}

// The documents UI's picks or made document (the UI thread): to whoever asked.
extern "C" JNIEXPORT void JNICALL Java_io_github_catdevgame_zaxiscollection_Documents_nativePicked(JNIEnv *env, jclass,
                                                                                                  jobjectArray uris)
{
    using namespace zx_android;
    std::vector<std::string> picks = strings(env, uris);
    std::function<void(std::vector<std::string>)> picked;
    {
        std::lock_guard lock(g_pickedMutex);
        picked = std::move(g_picked);
        g_picked = nullptr;
    }
    if (picked && !picks.empty())
        picked(std::move(picks));
}

#endif // __ANDROID__
