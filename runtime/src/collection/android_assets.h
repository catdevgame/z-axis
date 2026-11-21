#pragma once

// The Android build's own files in its APK (android_assets.cpp): read from the
// APK's assets alone. SDL's relative paths are no way to them: SDL opens a
// relative path from the app's internal storage before the assets
// (SDL_iostream.c), so a file left there stands in for the APK's.

#if defined(__ANDROID__)

#include <cstdint>
#include <string>
#include <vector>

namespace zx_android
{
    // The asset at `name` (under the APK's assets/, "game-code-tools/files.txt"); false if there is none.
    bool readAsset(const std::string &name, std::vector<uint8_t> &out);
    // The installed APK's versionCode (android:versionCode: 10200 for 1.2.0); -1 if it cannot be read.
    long long apkVersionCode();
}

#endif // __ANDROID__
