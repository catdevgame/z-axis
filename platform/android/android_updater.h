#pragma once

// The app's own update on Android (android_updater.cpp, Updater.java): what
// its platform file (platform_android.cpp) gives collection/updater.h.

#include "collection/updater.h"

namespace zx_android
{
    // The Package way: the APK downloaded, handed to the system's package
    // installer; the installed APK and the downloads in the internal
    // storage's update folder. Install::None before the activity has
    // installed the updater (Updater.install).
    zx::collection::UpdatePlatform updatePlatform();
}
