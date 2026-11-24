package com.savanindurkar7.wwe2k16;

import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;

import java.io.File;
import java.util.Locale;

/**
 * Turns what the system file picker returns into a real file path the game can open, and checks it.
 *
 * The game opens its files itself (native code, many at once), so it needs real paths, not content:// documents. With
 * "All files access" an app can read shared storage by path; the picker's document id tells which path was chosen
 * ("primary:Download/WWE2K16" is /storage/emulated/0/Download/WWE2K16).
 */
public final class GameFiles {
    private GameFiles() {}

    public static String treeUriToPath(Uri tree) {
        try {
            return docIdToPath(DocumentsContract.getTreeDocumentId(tree));
        } catch (Exception e) {
            return null;
        }
    }

    public static String documentUriToPath(Uri document) {
        try {
            return docIdToPath(DocumentsContract.getDocumentId(document));
        } catch (Exception e) {
            return null;
        }
    }

    // --- r19: more ways to a real path --------------------------------------------------------------------------------
    // 1.2.0 testers got "Android did not give a file path" when they picked from the Downloads / Recent shortcuts, a
    // phone maker's file manager or a third-party one. In order: the document id, the path inside the URI (file
    // managers' own providers: .../root/storage/emulated/0/...), the media database's file path for Downloads / MediaStore
    // ids (msf:, msd:, a plain number), and as a last resort a search of the usual folders for a file of the same name
    // (and size).

    public static String treeUriToPath(android.content.Context c, Uri tree) {
        String p = treeUriToPath(tree);
        if (p != null) return p;
        p = pathInsideUri(tree);
        return p != null && new File(p).isDirectory() ? p : null;
    }

    public static String documentUriToPath(android.content.Context c, Uri document) {
        String p = documentUriToPath(document);
        if (p != null && new File(p).exists()) return p;
        String inUri = pathInsideUri(document);
        if (inUri != null && new File(inUri).isFile()) return inUri;
        String fromMedia = mediaStorePath(c, document);
        if (fromMedia != null && new File(fromMedia).isFile()) return fromMedia;
        String found = searchByName(c, document);
        if (found != null) return found;
        return p;
    }

    /** "/storage/..." or "/sdcard/..." inside the URI's path (file managers that expose the real path). */
    private static String pathInsideUri(Uri u) {
        String s = Uri.decode(u.toString());
        for (String marker : new String[] {"/storage/", "/sdcard/", "/mnt/"}) {
            int i = s.indexOf(marker);
            if (i >= 0) {
                String path = s.substring(i);
                if (path.startsWith("/sdcard/")) {
                    path = Environment.getExternalStorageDirectory().getAbsolutePath() + path.substring(7);
                }
                int q = path.indexOf('?');
                return q >= 0 ? path.substring(0, q) : path;
            }
        }
        return null;
    }

    /** The media database's file path for a Downloads / MediaStore document ("msf:123", "msd:45", "123"). */
    private static String mediaStorePath(android.content.Context c, Uri document) {
        String id;
        try {
            id = DocumentsContract.getDocumentId(document);
        } catch (Exception e) {
            id = document.getLastPathSegment();
        }
        if (id == null) return null;
        String num = id.replaceFirst("^(msf|msd|downloads|image|video|audio|document):", "");
        if (!num.matches("\\d+")) return null;
        String[] proj = {"_data"};
        Uri[] tables = {android.provider.MediaStore.Files.getContentUri("external"),
                Uri.parse("content://downloads/public_downloads"), Uri.parse("content://downloads/all_downloads")};
        for (Uri table : tables) {
            try (android.database.Cursor cur = c.getContentResolver().query(
                    android.content.ContentUris.withAppendedId(table, Long.parseLong(num)), proj, null, null, null)) {
                if (cur != null && cur.moveToFirst()) {
                    String data = cur.getString(0);
                    if (data != null && !data.isEmpty()) return data;
                }
            } catch (Exception ignored) {
                // Not this table on this phone.
            }
        }
        return null;
    }

    /** Last resort: a file with the picked file's name (and size) in Download, Documents, the storage root or one level
     *  below them. */
    private static String searchByName(android.content.Context c, Uri document) {
        String name = null;
        long size = -1;
        try (android.database.Cursor cur = c.getContentResolver().query(document,
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME, android.provider.OpenableColumns.SIZE},
                null, null, null)) {
            if (cur != null && cur.moveToFirst()) {
                name = cur.getString(0);
                if (!cur.isNull(1)) size = cur.getLong(1);
            }
        } catch (Exception ignored) {
        }
        if (name == null || name.isEmpty()) return null;
        File root = Environment.getExternalStorageDirectory();
        File[] bases = {new File(root, "Download"), new File(root, "Downloads"), new File(root, "Documents"), root};
        for (File base : bases) {
            String hit = findIn(base, name, size, 2);
            if (hit != null) return hit;
        }
        return null;
    }

    private static String findIn(File dir, String name, long size, int depth) {
        File direct = new File(dir, name);
        if (direct.isFile() && (size < 0 || direct.length() == size)) return direct.getAbsolutePath();
        if (depth <= 0) return null;
        File[] children = dir.listFiles();
        if (children == null) return null;
        for (File ch : children) {
            if (ch.isDirectory() && !ch.getName().startsWith(".") && !ch.getName().equals("Android")) {
                String hit = findIn(ch, name, size, depth - 1);
                if (hit != null) return hit;
            }
        }
        return null;
    }

    private static String docIdToPath(String id) {
        if (id == null) return null;
        if (id.startsWith("raw:")) return id.substring(4);  // the Downloads provider sometimes gives the path itself
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon);
        String rest = id.substring(colon + 1);
        String base;
        if (volume.equalsIgnoreCase("primary")) {
            base = Environment.getExternalStorageDirectory().getAbsolutePath();
        } else if (volume.equalsIgnoreCase("home")) {
            base = Environment.getExternalStorageDirectory().getAbsolutePath() + "/Documents";  // r19: "Documents"
        } else if (volume.matches("[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}")) {
            base = "/storage/" + volume;  // SD card
        } else {
            return null;  // e.g. "msf:123" from the Downloads shortcut: no path known
        }
        return rest.isEmpty() ? base : base + "/" + rest;
    }

    /** Null when the folder is usable, else what is wrong (for the player). */
    public static String checkFolder(String path) {
        if (path == null) {
            return "Android did not give a folder path. In the picker open \"Internal storage\" (or your SD card) and "
                    + "choose the folder from there, not from the Downloads shortcut.";
        }
        File dir = new File(path);
        if (!dir.isDirectory()) return "The folder cannot be read: " + path;
        if (new File(dir, "default.xex").isFile()) return null;
        // Look one level down: people often pick the folder that holds the game folder.
        File[] children = dir.listFiles();
        if (children != null) {
            for (File child : children) {
                if (child.isDirectory() && new File(child, "default.xex").isFile()) {
                    return "default.xex is in the sub-folder \"" + child.getName() + "\": choose that folder.";
                }
            }
        }
        return "default.xex is not in this folder. Choose the extracted WWE 2K16 (Xbox 360) game folder.";
    }

    public static String checkIso(String path) {
        if (path == null) {
            return "Android did not give a file path. In the picker open \"Internal storage\" (or your SD card) and "
                    + "choose the .iso from there, not from the Recent or Downloads shortcuts.";
        }
        File f = new File(path);
        if (!path.toLowerCase(Locale.ROOT).endsWith(".iso")) return "That is not an .iso file.";
        if (!f.isFile() || !f.canRead()) return "The file cannot be read: " + path;
        if (f.length() < 1024L * 1024L * 1024L) return "That .iso is too small to be the game disc.";
        return null;
    }

    /** Null when ready to play, else what is missing. */
    public static String checkSaved(String path, String kind) {
        if (path == null || path.isEmpty()) return "Select your game folder or .iso first.";
        return "iso".equals(kind) ? checkIso(path) : checkFolder(path);
    }
}
