#pragma once

// The Android build's documents (android_documents.cpp): what its platform
// file (platform_android.cpp) asks of them.

#include <functional>
#include <string>
#include <vector>

namespace zx_android
{
    // The documents UI for disc images (several at once) or a folder; the
    // picks come to `picked` on the UI thread. `keep`: their grants kept past
    // this run (a disc's source); else they are read once, now (a mod copied
    // in), and keep none. False before the activity has installed the
    // documents (Documents.install).
    bool pickDocuments(bool folder, bool keep, std::function<void(std::vector<std::string>)> picked);
    // The documents UI for a new document (`name` suggested); its URI comes
    // to `created` on the UI thread, nothing when the player backs out.
    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created);
}
