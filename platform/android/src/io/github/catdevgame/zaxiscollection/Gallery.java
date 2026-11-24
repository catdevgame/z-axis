package io.github.catdevgame.zaxiscollection;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.provider.MediaStore;
import android.util.Log;

import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;

// The screenshots in the system's Gallery: each PNG the game saved in the
// app's own folder is copied into the shared pictures (MediaStore, under
// Pictures/<app>/<game>, as the Mac and Windows keep theirs in the user's
// Pictures), for the native side (platform/android/android_gallery.cpp).
// An app adds its own pictures there without asking for any permission
// (Android 10 on).
final class Gallery {
    private static final String TAG = "ZAxisGallery";

    private static Context sContext;

    private Gallery() {
    }

    // The native side's class and method (once the library is loaded).
    private static native void nativeInstall();

    static void install(Context context) {
        sContext = context.getApplicationContext();
        nativeInstall();
    }

    // Called from native code, any thread: the picture at `path` as `name`
    // in `folder` (a relative path, "Pictures/..."). Its content URI, or
    // null when it could not be added (nothing is left behind then).
    static String addPicture(String path, String name, String folder) {
        final Context context = sContext;
        if (context == null) {
            return null;
        }
        ContentResolver resolver = context.getContentResolver();
        ContentValues values = new ContentValues();
        values.put(MediaStore.Images.Media.DISPLAY_NAME, name);
        values.put(MediaStore.Images.Media.MIME_TYPE, "image/png");
        values.put(MediaStore.Images.Media.RELATIVE_PATH, folder);
        // (hidden from other apps until it is written whole)
        values.put(MediaStore.Images.Media.IS_PENDING, 1);
        Uri uri = null;
        try {
            uri = resolver.insert(MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
            if (uri == null) {
                Log.w(TAG, "no entry for " + name + " in " + folder);
                return null;
            }
            try (InputStream in = new FileInputStream(path); OutputStream out = resolver.openOutputStream(uri)) {
                if (out == null) {
                    throw new java.io.IOException("no stream for " + uri);
                }
                byte[] buffer = new byte[1 << 16];
                for (int read; (read = in.read(buffer)) > 0;) {
                    out.write(buffer, 0, read);
                }
            }
            values.clear();
            values.put(MediaStore.Images.Media.IS_PENDING, 0);
            resolver.update(uri, values, null, null);
            return uri.toString();
        } catch (Exception e) {
            Log.w(TAG, "cannot add " + path + ": " + e);
            if (uri != null) {
                resolver.delete(uri, null, null);
            }
            return null;
        }
    }
}
