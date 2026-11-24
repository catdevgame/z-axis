package io.github.catdevgame.zaxiscollection;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

// The app's own update on Android (collection/updater.h, the Package way;
// the native side is android_updater.cpp): a release's APK downloaded and
// handed to the system's package installer, which replaces the app with it.
// Signed with the same key, the update keeps the app's data (its settings,
// saves and set-up games); the system asks the player to confirm, and closes
// the app to replace it. Updates only: the installer puts no older version
// over a newer one for an app (collection/updater.h).
final class Updater {
    private static final String TAG = "ZAxisUpdater";
    private static final String ACTION = "io.github.catdevgame.zaxiscollection.UPDATE_STATUS";

    private static Activity sActivity;
    private static BroadcastReceiver sReceiver;

    private Updater() {
    }

    // The native side's class (once the library is loaded).
    private static native void nativeInstall();

    // The installer's answer: "" handed over (the app is replaced), else why not.
    private static native void nativeDone(String why);

    static void install(Activity activity) {
        sActivity = activity;
        nativeInstall();
    }

    // ---- called from native code, any thread ----

    // The installed APK (what is kept for Roll Back Update).
    static String installedApk() {
        Activity activity = sActivity;
        return activity == null ? "" : activity.getApplicationInfo().sourceDir;
    }

    // url into the file path; null done, else why not (no connection: nothing else changes).
    static String download(String url, String path) {
        HttpURLConnection connection = null;
        try {
            // (GitHub's release files redirect to its storage host: followed, https to https only)
            URL at = new URL(url);
            for (int hops = 0; ; ++hops) {
                connection = (HttpURLConnection) at.openConnection();
                connection.setConnectTimeout(20000);
                connection.setReadTimeout(60000);
                connection.setInstanceFollowRedirects(false);
                int code = connection.getResponseCode();
                if (code >= 300 && code < 400 && hops < 8) {
                    String location = connection.getHeaderField("Location");
                    connection.disconnect();
                    connection = null;
                    if (location == null) {
                        return "a redirect with no Location from " + at;
                    }
                    URL next = new URL(at, location);
                    if (!next.getProtocol().equals("https") && at.getProtocol().equals("https")) {
                        return "a redirect from https to " + next.getProtocol();
                    }
                    at = next;
                    continue;
                }
                if (code != HttpURLConnection.HTTP_OK) {
                    return "HTTP " + code + " from " + at;
                }
                break;
            }
            try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(path)) {
                byte[] buffer = new byte[1 << 16];
                for (int got; (got = in.read(buffer)) > 0; ) {
                    out.write(buffer, 0, got);
                }
            }
            return null;
        } catch (Exception e) {
            String message = e.getMessage();
            return e.getClass().getSimpleName() + (message != null ? ": " + message : "");
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    // The version an APK says it is (versionName), "" none.
    static String versionOf(String path) {
        Activity activity = sActivity;
        if (activity == null) {
            return "";
        }
        PackageInfo info = activity.getPackageManager().getPackageArchiveInfo(path, 0);
        if (info == null || !activity.getPackageName().equals(info.packageName)) {
            return "";
        }
        return info.versionName != null ? info.versionName : "";
    }

    // The APK handed to the system's installer; null handed over (the answer
    // comes to nativeDone), else why not.
    static String installPackage(String path) {
        final Activity activity = sActivity;
        if (activity == null) {
            return "no activity";
        }
        try {
            PackageInstaller installer = activity.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params =
                    new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(activity.getPackageName());
            int id = installer.createSession(params);
            try (PackageInstaller.Session session = installer.openSession(id)) {
                File apk = new File(path);
                try (InputStream in = new FileInputStream(apk);
                     OutputStream out = session.openWrite("package.apk", 0, apk.length())) {
                    byte[] buffer = new byte[1 << 16];
                    for (int got; (got = in.read(buffer)) > 0; ) {
                        out.write(buffer, 0, got);
                    }
                    session.fsync(out);
                }
                listen(activity);
                Intent status = new Intent(ACTION).setPackage(activity.getPackageName());
                PendingIntent sender = PendingIntent.getBroadcast(activity, id, status,
                        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                session.commit(sender.getIntentSender());
            }
            return null;
        } catch (Exception e) {
            Log.e(TAG, "install", e);
            String message = e.getMessage();
            return e.getClass().getSimpleName() + (message != null ? ": " + message : "");
        }
    }

    // The installer's answers: its confirmation shown, then done or not.
    private static synchronized void listen(Activity activity) {
        if (sReceiver != null) {
            return;
        }
        sReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                Log.i(TAG, "status " + status + ": " + message);
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
                    if (confirm != null && sActivity != null) {
                        sActivity.startActivity(confirm);
                        return;
                    }
                    nativeDone("the system's confirmation could not be shown");
                } else if (status == PackageInstaller.STATUS_SUCCESS) {
                    nativeDone("");
                } else {
                    nativeDone(message != null ? message : "the installer refused it (" + status + ")");
                }
            }
        };
        activity.getApplicationContext().registerReceiver(sReceiver, new IntentFilter(ACTION), Context.RECEIVER_NOT_EXPORTED);
    }
}
