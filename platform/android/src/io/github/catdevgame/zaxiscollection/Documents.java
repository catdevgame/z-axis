package io.github.catdevgame.zaxiscollection;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

// The documents the player picks in the system's documents UI (a disc
// image of any format, a BIN/CUE dump's files, a folder), for the native setup
// (collection/android_documents.cpp, game_setup.h's DocumentAccess). The app
// may read no path outside its own folders: everything goes through the
// content resolver, by URI, and the files are read through descriptors it
// opens. The grants are kept (persistable), so a game's files can be
// repaired from the same disc later.
final class Documents {
    private static final String TAG = "ZAxisDocuments";
    // Our activity results (SDLActivity's own file dialog has its own codes).
    static final int PICK_FILES = 0x5a01;
    static final int PICK_FOLDER = 0x5a02;
    static final int CREATE_FILE = 0x5a03;

    private static Activity sActivity;

    private Documents() {
    }

    // The native side's class and methods (once the library is loaded).
    private static native void nativeInstall();

    // The picks, none when the player backed out.
    private static native void nativePicked(String[] uris);

    static void install(Activity activity) {
        sActivity = activity;
        nativeInstall();
    }

    // ---- the picker (called from native code, any thread) ----

    static boolean pick(final boolean folder) {
        final Activity activity = sActivity;
        if (activity == null) {
            return false;
        }
        activity.runOnUiThread(() -> {
            Intent intent;
            if (folder) {
                intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            } else {
                intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                // (no disc image format, ISO, BIN/CUE, CHD, CSO or ZSO, has a MIME type every
                // provider knows: the native setup tells them by their bytes)
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            }
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                activity.startActivityForResult(intent, folder ? PICK_FOLDER : PICK_FILES);
            } catch (android.content.ActivityNotFoundException e) {
                Log.e(TAG, "no documents UI", e);
                nativePicked(new String[0]);
            }
        });
        return true;
    }

    // A new document the player names and places (the saves' export).
    static boolean create(final String name, final String mimeType) {
        final Activity activity = sActivity;
        if (activity == null) {
            return false;
        }
        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType(mimeType);
            intent.putExtra(Intent.EXTRA_TITLE, name);
            try {
                activity.startActivityForResult(intent, CREATE_FILE);
            } catch (android.content.ActivityNotFoundException e) {
                Log.e(TAG, "no documents UI", e);
                nativePicked(new String[0]);
            }
        });
        return true;
    }

    // GameActivity's onActivityResult for our codes: true when it was ours.
    static boolean onActivityResult(Activity activity, int requestCode, int resultCode, Intent data) {
        if (requestCode == CREATE_FILE) {
            Uri made = resultCode == Activity.RESULT_OK && data != null ? data.getData() : null;
            nativePicked(made != null ? new String[] { made.toString() } : new String[0]);
            return true;
        }
        if (requestCode != PICK_FILES && requestCode != PICK_FOLDER) {
            return false;
        }
        List<String> uris = new ArrayList<>();
        if (resultCode == Activity.RESULT_OK && data != null) {
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); ++i) {
                    uris.add(data.getClipData().getItemAt(i).getUri().toString());
                }
            } else if (data.getData() != null) {
                uris.add(data.getData().toString());
            }
        }
        ContentResolver resolver = activity.getContentResolver();
        for (String uri : uris) {
            try {
                resolver.takePersistableUriPermission(Uri.parse(uri), Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException e) {
                // (a provider that gives no lasting grant: readable this run)
                Log.w(TAG, "no lasting permission for " + uri);
            }
        }
        nativePicked(uris.toArray(new String[0]));
        return true;
    }

    // ---- reading (called from native code, any thread) ----

    // A tree's URI as the document of its folder; any other URI as it is.
    private static Uri documentUri(Uri uri) {
        if (DocumentsContract.isTreeUri(uri) && !DocumentsContract.isDocumentUri(sActivity, uri)) {
            return DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri));
        }
        return uri;
    }

    private static boolean isFolder(String mime) {
        return DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
    }

    // { name, size, "1" for a folder else "0" }; null when it cannot be read.
    static String[] stat(String text) {
        if (sActivity == null) {
            return null;
        }
        ContentResolver resolver = sActivity.getContentResolver();
        Uri uri = documentUri(Uri.parse(text));
        String[] columns = { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        try (Cursor row = resolver.query(uri, columns, null, null, null)) {
            if (row == null || !row.moveToFirst()) {
                return null;
            }
            String name = row.isNull(0) ? uri.getLastPathSegment() : row.getString(0);
            long size = row.isNull(1) ? 0 : row.getLong(1);
            return new String[] { name, Long.toString(size), isFolder(resolver.getType(uri)) ? "1" : "0" };
        } catch (RuntimeException e) {
            // (SecurityException when the grant is gone, IllegalArgumentException when the document is)
            Log.w(TAG, "cannot read " + text + ": " + e);
            return null;
        }
    }

    // A folder's documents, one level: { uri, name, size, folder } each.
    static String[] list(String text) {
        if (sActivity == null) {
            return new String[0];
        }
        ContentResolver resolver = sActivity.getContentResolver();
        Uri tree = Uri.parse(text);
        List<String> out = new ArrayList<>();
        try {
            String id = DocumentsContract.isDocumentUri(sActivity, tree) ? DocumentsContract.getDocumentId(tree)
                    : DocumentsContract.getTreeDocumentId(tree);
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
            String[] columns = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_SIZE,
                    DocumentsContract.Document.COLUMN_MIME_TYPE };
            try (Cursor rows = resolver.query(children, columns, null, null, null)) {
                while (rows != null && rows.moveToNext()) {
                    out.add(DocumentsContract.buildDocumentUriUsingTree(tree, rows.getString(0)).toString());
                    out.add(rows.isNull(1) ? "" : rows.getString(1));
                    out.add(Long.toString(rows.isNull(2) ? 0 : rows.getLong(2)));
                    out.add(isFolder(rows.getString(3)) ? "1" : "0");
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot list " + text + ": " + e);
        }
        return out.toArray(new String[0]);
    }

    // A descriptor reading the document, the caller's to close; -1 when it
    // cannot be read.
    static int open(String text) {
        if (sActivity == null) {
            return -1;
        }
        try {
            ParcelFileDescriptor file = sActivity.getContentResolver().openFileDescriptor(documentUri(Uri.parse(text)), "r");
            return file != null ? file.detachFd() : -1;
        } catch (java.io.FileNotFoundException | RuntimeException e) {
            Log.w(TAG, "cannot open " + text + ": " + e);
            return -1;
        }
    }
}
