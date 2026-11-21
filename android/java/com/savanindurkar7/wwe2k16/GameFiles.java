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
