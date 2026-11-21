package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.net.Uri;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Custom Vulkan drivers in the adrenotools package format (a zip with meta.json and the driver library, e.g. the
 * Turnip builds from Banners-Turnip or K11MCH1/AdrenoToolsDrivers). They are unpacked into the app's own storage:
 * Android does not let an app load code from shared storage.
 */
public final class Drivers {
    public static final class Driver {
        public final String name;     // folder name, shown in the launcher
        public final String library;  // the driver's .so file name
        public final String description;
        public final File dir;

        Driver(String name, String library, String description, File dir) {
            this.name = name;
            this.library = library;
            this.description = description;
            this.dir = dir;
        }
    }

    private Drivers() {}

    public static File root(Context c) {
        File d = new File(c.getFilesDir(), "drivers");
        d.mkdirs();
        return d;
    }

    /** Written before PLAY with a custom driver, deleted by the game after 600 good frames. */
    public static File trialFile(Context c) {
        return new File(root(c), "trial.txt");
    }

    public static List<Driver> list(Context c) {
        List<Driver> out = new ArrayList<>();
        File[] dirs = root(c).listFiles();
        if (dirs == null) return out;
        for (File d : dirs) {
            Driver driver = read(d);
            if (driver != null) out.add(driver);
        }
        return out;
    }

    public static Driver find(Context c, String name) {
        for (Driver d : list(c)) if (d.name.equals(name)) return d;
        return null;
    }

    private static Driver read(File dir) {
        File info = new File(dir, "driver.properties");
        if (!dir.isDirectory() || !info.isFile()) return null;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(info)) {
            p.load(in);
        } catch (IOException e) {
            return null;
        }
        String lib = p.getProperty("library", "");
        if (lib.isEmpty() || !new File(dir, lib).isFile()) return null;
        return new Driver(dir.getName(), lib, p.getProperty("description", ""), dir);
    }

    public static boolean delete(Context c, String name) {
        Driver d = find(c, name);
        if (d == null) return false;
        deleteTree(d.dir);
        return true;
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    /** Unpacks a driver zip. Returns the new driver, or throws with a message for the player. */
    public static Driver importZip(Context c, Uri uri) throws IOException {
        File staging = new File(root(c), ".import");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("could not create a folder for the driver");
        String metaText = null;
        String firstVulkanLib = null;
        try (InputStream raw = c.getContentResolver().openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(raw)) {
            ZipEntry entry;
            byte[] buffer = new byte[64 * 1024];
            while ((entry = zip.getNextEntry()) != null) {
                if (entry.isDirectory()) continue;
                String name = new File(entry.getName()).getName();  // flatten: drivers keep everything in one folder
                if (name.isEmpty() || name.startsWith(".")) continue;
                if (name.equals("meta.json")) {
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    int n;
                    while ((n = zip.read(buffer)) > 0) bytes.write(buffer, 0, n);
                    metaText = bytes.toString("UTF-8");
                    continue;
                }
                try (OutputStream out = new FileOutputStream(new File(staging, name))) {
                    int n;
                    while ((n = zip.read(buffer)) > 0) out.write(buffer, 0, n);
                }
                if (firstVulkanLib == null && name.startsWith("libvulkan") && name.endsWith(".so")) firstVulkanLib = name;
            }
        }
        String library = null, title = null, description = "";
        if (metaText != null) {
            try {
                JSONObject meta = new JSONObject(metaText);
                library = meta.optString("libraryName", null);
                title = meta.optString("name", null);
                String version = meta.optString("driverVersion", "");
                description = (meta.optString("description", "") + " " + version).trim();
            } catch (Exception ignored) {
                // A broken meta.json: fall back to the first libvulkan*.so.
            }
        }
        if (library == null || library.isEmpty()) library = firstVulkanLib;
        if (library == null || !new File(staging, library).isFile()) {
            deleteTree(staging);
            throw new IOException("this zip has no Vulkan driver library (libvulkan_*.so)");
        }
        String folder = sanitize(title != null && !title.isEmpty() ? title : library.replace(".so", ""));
        File target = new File(root(c), folder);
        for (int i = 2; target.exists(); i++) target = new File(root(c), folder + " " + i);
        Properties p = new Properties();
        p.setProperty("library", library);
        p.setProperty("description", description);
        try (OutputStream out = new FileOutputStream(new File(staging, "driver.properties"))) {
            p.store(out, "WWE 2K16 launcher: imported driver");
        }
        if (!staging.renameTo(target)) {
            deleteTree(staging);
            throw new IOException("could not store the driver");
        }
        return read(target);
    }

    private static String sanitize(String s) {
        String out = s.replaceAll("[^A-Za-z0-9 ._()+-]", "_").trim();
        if (out.length() > 48) out = out.substring(0, 48).trim();
        return out.isEmpty() ? "driver" : out;
    }
}
