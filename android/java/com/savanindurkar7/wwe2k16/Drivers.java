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
 *
 * r17: the APK carries built-in drivers (assets/drivers/, fetched by tools/fetch-drivers.sh when the APK is built; the
 * open-source ones only: Mesa Turnip for Adreno 6xx / 7xx / 8xx and malisx2, Mesa's PanVK for Mali). They are unpacked
 * on the first start of each APK version and listed with the imported ones, marked BUILT-IN.
 */
public final class Drivers {
    public static final class Driver {
        public final String name;     // folder name, shown in the launcher
        public final String library;  // the driver's .so file name
        public final String description;
        public final File dir;
        public final boolean bundled;  // shipped in the APK (r17)
        public final String family;    // "adreno" / "mali" / "" (imported: unknown)
        public final String gens;      // built-in Turnip: "6,7", "8" or "710"
        public final String note;

        Driver(String name, String library, String description, File dir, boolean bundled, String family, String gens,
               String note) {
            this.name = name;
            this.library = library;
            this.description = description;
            this.dir = dir;
            this.bundled = bundled;
            this.family = family;
            this.gens = gens;
            this.note = note;
        }

        public boolean isTurnip() { return library.contains("freedreno"); }
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
        return new Driver(dir.getName(), lib, p.getProperty("description", ""), dir,
                "true".equals(p.getProperty("bundled")), p.getProperty("family", ""), p.getProperty("gens", ""),
                p.getProperty("note", ""));
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
        try (InputStream raw = c.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("could not open the file");
            return install(c, raw, null, null);
        }
    }

    /**
     * Unpacks a driver zip from a stream into its own folder. folderName null = from meta.json's name (a number is
     * added if taken); else that folder is replaced. extra: more lines for driver.properties (built-in drivers).
     */
    private static Driver install(Context c, InputStream raw, String folderName, Properties extra) throws IOException {
        File staging = new File(root(c), ".import");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("could not create a folder for the driver");
        String metaText = null;
        String firstVulkanLib = null;
        try (ZipInputStream zip = new ZipInputStream(raw)) {
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
        File target;
        if (folderName != null) {
            target = new File(root(c), sanitize(folderName));
            deleteTree(target);
        } else {
            String folder = sanitize(title != null && !title.isEmpty() ? title : library.replace(".so", ""));
            target = new File(root(c), folder);
            for (int i = 2; target.exists(); i++) target = new File(root(c), folder + " " + i);
        }
        Properties p = new Properties();
        if (extra != null) p.putAll(extra);
        p.setProperty("library", library);
        p.setProperty("description", description);
        try (OutputStream out = new FileOutputStream(new File(staging, "driver.properties"))) {
            p.store(out, "WWE 2K16 launcher: " + (extra != null ? "built-in" : "imported") + " driver");
        }
        if (!staging.renameTo(target)) {
            deleteTree(staging);
            throw new IOException("could not store the driver");
        }
        return read(target);
    }

    // --- Built-in drivers (r17) ------------------------------------------------------------------------------------

    /** One line of assets/drivers/index.txt: file|folder name|family|gens|note. */
    private static final class Bundled {
        String file, name, family, gens, note;
    }

    private static List<Bundled> bundledIndex(Context c) {
        List<Bundled> out = new ArrayList<>();
        try (InputStream in = c.getAssets().open("drivers/index.txt")) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] b = new byte[4096];
            int n;
            while ((n = in.read(b)) > 0) bytes.write(b, 0, n);
            for (String line : bytes.toString("UTF-8").split("\n")) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] f = line.split("\\|", -1);
                if (f.length < 5) continue;
                Bundled d = new Bundled();
                d.file = f[0];
                d.name = f[1];
                d.family = f[2];
                d.gens = f[3];
                d.note = f[4];
                out.add(d);
            }
        } catch (IOException ignored) {
            // No built-in drivers in this APK.
        }
        return out;
    }

    /**
     * Unpacks the APK's built-in drivers once per APK version (and removes built-in ones a newer APK no longer has).
     * Returns how many were unpacked now. Run off the UI thread.
     */
    public static synchronized int installBundled(Context c) {
        String stamp = BuildInfo.versionName(c) + "/" + new File(c.getApplicationInfo().sourceDir).lastModified();
        List<Bundled> index = bundledIndex(c);
        int done = 0;
        java.util.Set<String> keep = new java.util.HashSet<>();
        for (Bundled b : index) {
            String folder = sanitize(b.name);
            keep.add(folder);
            Driver have = find(c, folder);
            if (have != null && have.bundled && stamp.equals(readProperty(have.dir, "bundled_stamp"))) continue;
            Properties extra = new Properties();
            extra.setProperty("bundled", "true");
            extra.setProperty("bundled_stamp", stamp);
            extra.setProperty("family", b.family);
            extra.setProperty("gens", b.gens);
            extra.setProperty("note", b.note);
            try (InputStream in = c.getAssets().open("drivers/" + b.file)) {
                install(c, in, folder, extra);
                done++;
            } catch (IOException e) {
                android.util.Log.w("wwe2k16", "built-in driver " + b.file + ": " + e.getMessage());
            }
        }
        for (Driver d : list(c)) if (d.bundled && !keep.contains(d.name)) deleteTree(d.dir);
        return done;
    }

    private static String readProperty(File dir, String key) {
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(new File(dir, "driver.properties"))) {
            p.load(in);
        } catch (IOException e) {
            return null;
        }
        return p.getProperty(key);
    }

    /** The built-in driver made for this phone's GPU, or null (Mali: the experimental PanVK one is offered, not this). */
    public static Driver recommended(Context c) {
        String family = Gpu.family(c);
        if (!Gpu.ADRENO.equals(family)) return null;
        String renderer = Gpu.renderer(c);
        int gen = Gpu.adrenoGeneration(c);
        boolean small7 = renderer.matches(".*Adreno[^0-9]*7(10|20|22)\\b.*");
        for (Driver d : list(c)) {
            if (!d.bundled || !Gpu.ADRENO.equals(d.family)) continue;
            if (small7 && d.gens.contains("710")) return d;
            if (!small7 && gen > 0 && java.util.Arrays.asList(d.gens.split(",")).contains(String.valueOf(gen))) return d;
        }
        return null;
    }

    /** True when this driver is made for another GPU family than the phone's (shown as a warning). */
    public static boolean wrongFamily(Context c, Driver d) {
        if (d.family.isEmpty()) return false;
        String phone = Gpu.family(c);
        return !Gpu.OTHER.equals(phone) && !d.family.equals(phone);
    }

    private static String sanitize(String s) {
        String out = s.replaceAll("[^A-Za-z0-9 ._()+-]", "_").trim();
        if (out.length() > 48) out = out.substring(0, 48).trim();
        return out.isEmpty() ? "driver" : out;
    }
}
