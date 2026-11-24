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
        public String kind = "";       // r19: turnip / qualcomm / mali / other

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

        public boolean isTurnip() { return library.contains("freedreno") || "turnip".equals(kind); }

        /** Qualcomm's own Adreno driver (imported): slow shader compiles in this game (r19). */
        public boolean isQualcommBlob() { return "qualcomm".equals(kind) && !library.contains("freedreno"); }
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
        Driver d = new Driver(dir.getName(), lib, p.getProperty("description", ""), dir,
                "true".equals(p.getProperty("bundled")), p.getProperty("family", ""), p.getProperty("gens", ""),
                p.getProperty("note", ""));
        d.kind = p.getProperty("kind", "");
        if (d.kind.isEmpty()) d.kind = kindOf(lib, "", d.description + " " + d.name + " " + d.note);
        return d;
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

    /**
     * Imports a driver file the player picked. r19: any packaging is accepted — a zip (any folder layout, a zip inside
     * the zip), a .tar / .tar.gz / .tgz, or the driver library itself (.so) — and the library is found under any of
     * the names drivers use (meta.json's libraryName / library, libvulkan_*.so, vulkan.*.so like Qualcomm's
     * vulkan.adreno.so, any *vulkan*.so, or the only .so). Throws with a message for the player.
     */
    public static Driver importZip(Context c, Uri uri) throws IOException {
        String hint = displayName(c, uri);
        try (InputStream raw = c.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("could not open the file");
            return install(c, raw, hint, null, null);
        }
    }

    private static Driver install(Context c, InputStream raw, String folderName, Properties extra) throws IOException {
        return install(c, raw, null, folderName, extra);
    }

    private static String displayName(Context c, Uri uri) {
        try (android.database.Cursor cur = c.getContentResolver().query(uri,
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cur != null && cur.moveToFirst()) return cur.getString(0);
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        if (last != null && last.contains("/")) last = last.substring(last.lastIndexOf('/') + 1);
        return last;
    }

    /** What was unpacked: meta.json's text (if any) and every file name, in order. */
    private static final class Unpacked {
        String metaText;
        final List<String> files = new ArrayList<>();
    }

    /**
     * Unpacks a driver package from a stream into its own folder. nameHint: the picked file's name (for a bare .so).
     * folderName null = from meta.json's name (a number is added if taken); else that folder is replaced. extra: more
     * lines for driver.properties (built-in drivers).
     */
    private static Driver install(Context c, InputStream raw, String nameHint, String folderName, Properties extra)
            throws IOException {
        File staging = new File(root(c), ".import");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("could not create a folder for the driver");
        Unpacked u = new Unpacked();
        try {
            unpackAny(new java.io.BufferedInputStream(raw, 1 << 16), nameHint, staging, u, 0);
        } catch (IOException e) {
            deleteTree(staging);
            throw e;
        }
        String library = null, title = null, description = "", vendor = "";
        if (u.metaText != null) {
            try {
                String t = u.metaText.startsWith("﻿") ? u.metaText.substring(1) : u.metaText;
                JSONObject meta = new JSONObject(t);
                library = firstNonEmpty(meta.optString("libraryName", ""), meta.optString("library", ""),
                        meta.optString("lib", ""));
                if (library != null) library = new File(library).getName();  // some list a path
                title = firstNonEmpty(meta.optString("name", ""), meta.optString("driverName", ""));
                vendor = meta.optString("vendor", "");
                String version = meta.optString("driverVersion", "");
                description = (meta.optString("description", "") + " " + version).trim();
            } catch (Exception ignored) {
                // A broken meta.json: the library is found by name below.
            }
        }
        if (library == null || !new File(staging, library).isFile()) library = pickLibrary(u.files);
        if (library == null || !new File(staging, library).isFile()) {
            deleteTree(staging);
            throw new IOException("no Vulkan driver library (.so) was found in this file");
        }
        File target;
        if (folderName != null) {
            target = new File(root(c), sanitize(folderName));
            deleteTree(target);
        } else {
            String base = title != null && !title.isEmpty() ? title
                    : (nameHint != null && !nameHint.isEmpty() ? nameHint.replaceAll("\\.(zip|tar|tgz|gz|so|adpkg)$", "")
                    : library.replace(".so", ""));
            String folder = sanitize(base);
            target = new File(root(c), folder);
            for (int i = 2; target.exists(); i++) target = new File(root(c), folder + " " + i);
        }
        Properties p = new Properties();
        if (extra != null) p.putAll(extra);
        p.setProperty("library", library);
        p.setProperty("description", description);
        if (extra == null) {
            // Imported: note what kind of driver it is, for the advice in the driver list.
            String kind = kindOf(library, vendor, description + " " + (title == null ? "" : title));
            p.setProperty("kind", kind);
            if (!p.containsKey("family")) {
                p.setProperty("family", "mali".equals(kind) ? Gpu.MALI
                        : ("turnip".equals(kind) || "qualcomm".equals(kind)) ? Gpu.ADRENO : "");
            }
        }
        try (OutputStream out = new FileOutputStream(new File(staging, "driver.properties"))) {
            p.store(out, "WWE 2K16 launcher: " + (extra != null ? "built-in" : "imported") + " driver");
        }
        if (!staging.renameTo(target)) {
            deleteTree(staging);
            throw new IOException("could not store the driver");
        }
        return read(target);
    }

    private static String firstNonEmpty(String... v) {
        for (String s : v) if (s != null && !s.isEmpty()) return s;
        return null;
    }

    /** turnip (Mesa freedreno), qualcomm (Adreno's own), mali, or other. */
    static String kindOf(String library, String vendor, String text) {
        String l = (library + " " + vendor + " " + text).toLowerCase(java.util.Locale.ROOT);
        if (l.contains("freedreno") || l.contains("turnip") || l.contains("mesa")) return "turnip";
        if (l.contains("mali") || l.contains("panvk") || l.contains("panfrost")) return "mali";
        if (l.contains("adreno") || l.contains("qualcomm") || l.contains("qcom") || l.contains("qti")) return "qualcomm";
        return "other";
    }

    /** The driver library among the unpacked files, by the names drivers use. */
    private static String pickLibrary(List<String> files) {
        String libvulkan = null, vulkanDot = null, anyVulkan = null, onlySo = null;
        int soCount = 0;
        for (String f : files) {
            String n = f.toLowerCase(java.util.Locale.ROOT);
            if (!n.endsWith(".so")) continue;
            soCount++;
            onlySo = f;
            if (libvulkan == null && n.startsWith("libvulkan")) libvulkan = f;
            if (vulkanDot == null && n.startsWith("vulkan.")) vulkanDot = f;
            if (anyVulkan == null && n.contains("vulkan")) anyVulkan = f;
        }
        if (libvulkan != null) return libvulkan;
        if (vulkanDot != null) return vulkanDot;
        if (anyVulkan != null) return anyVulkan;
        return soCount == 1 ? onlySo : null;
    }

    /** Unpacks by content: zip, gzip (tar.gz), tar, or a bare ELF library. Archives may nest (zip in zip). */
    private static void unpackAny(java.io.BufferedInputStream in, String nameHint, File dir, Unpacked u, int depth)
            throws IOException {
        if (depth > 3) return;
        in.mark(600);
        byte[] head = new byte[512];
        int got = readUpTo(in, head);
        in.reset();
        if (got >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) {
            unpackZip(in, dir, u, depth);
        } else if (got >= 2 && (head[0] & 0xFF) == 0x1F && (head[1] & 0xFF) == 0x8B) {
            unpackAny(new java.io.BufferedInputStream(new java.util.zip.GZIPInputStream(in, 1 << 16), 1 << 16),
                    nameHint, dir, u, depth + 1);
        } else if (got >= 262 && head[257] == 'u' && head[258] == 's' && head[259] == 't' && head[260] == 'a'
                && head[261] == 'r') {
            unpackTar(in, dir, u, depth);
        } else if (got >= 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
            String name = nameHint != null && nameHint.endsWith(".so") ? sanitizeFile(nameHint) : "libvulkan_custom.so";
            copyTo(in, new File(dir, name));
            u.files.add(name);
        } else if (got >= 512 && looksLikeOldTar(head)) {
            unpackTar(in, dir, u, depth);
        } else {
            throw new IOException("this file is not a driver package (zip, tar, tar.gz) or a driver library (.so)");
        }
    }

    private static void unpackZip(InputStream in, File dir, Unpacked u, int depth) throws IOException {
        ZipInputStream zip = new ZipInputStream(in);
        ZipEntry entry;
        while ((entry = zip.getNextEntry()) != null) {
            if (entry.isDirectory()) continue;
            entry(new NoClose(zip), new File(entry.getName()).getName(), dir, u, depth);
        }
    }

    private static void unpackTar(InputStream in, File dir, Unpacked u, int depth) throws IOException {
        byte[] h = new byte[512];
        String longName = null;
        while (readUpTo(in, h) == 512) {
            boolean empty = true;
            for (byte b : h) if (b != 0) { empty = false; break; }
            if (empty) break;
            String name = cstr(h, 0, 100);
            String prefix = cstr(h, 345, 155);
            if (!prefix.isEmpty()) name = prefix + "/" + name;
            long size = 0;
            String oct = cstr(h, 124, 12).trim();
            if (!oct.isEmpty()) size = Long.parseLong(oct, 8);
            char type = (char) h[156];
            long padded = (size + 511) / 512 * 512;
            if (type == 'L') {  // GNU long name: the next entry's name
                byte[] nb = new byte[(int) padded];
                readUpTo(in, nb);
                longName = cstr(nb, 0, (int) size);
                continue;
            }
            if (longName != null) {
                name = longName;
                longName = null;
            }
            if (type == '0' || type == 0) {
                LimitedStream body = new LimitedStream(in, size);
                entry(body, new File(name).getName(), dir, u, depth);
                body.drain();
                skipFully(in, padded - size);
            } else {
                skipFully(in, padded);
            }
        }
    }

    /** One file from an archive: meta.json kept as text, a nested archive unpacked, anything else copied flat. */
    private static void entry(InputStream body, String name, File dir, Unpacked u, int depth) throws IOException {
        if (name.isEmpty() || name.startsWith(".")) return;
        String lower = name.toLowerCase(java.util.Locale.ROOT);
        if (lower.equals("meta.json")) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] b = new byte[8192];
            int n;
            while ((n = body.read(b)) > 0) bytes.write(b, 0, n);
            u.metaText = bytes.toString("UTF-8");
            return;
        }
        if (lower.endsWith(".zip") || lower.endsWith(".tar") || lower.endsWith(".tgz") || lower.endsWith(".tar.gz")) {
            unpackAny(new java.io.BufferedInputStream(body, 1 << 16), name, dir, u, depth + 1);
            return;
        }
        String safe = sanitizeFile(name);
        copyTo(body, new File(dir, safe));
        u.files.add(safe);
    }

    private static boolean looksLikeOldTar(byte[] h) {
        // A pre-POSIX tar: a name, then an octal size field ending in a space or NUL.
        if (h[0] == 0) return false;
        for (int i = 124; i < 135; i++) {
            byte b = h[i];
            if (b != ' ' && b != 0 && (b < '0' || b > '7')) return false;
        }
        return true;
    }

    private static String sanitizeFile(String n) {
        return n.replaceAll("[^A-Za-z0-9._+-]", "_");
    }

    private static String cstr(byte[] b, int off, int len) {
        int end = off;
        while (end < off + len && end < b.length && b[end] != 0) end++;
        return new String(b, off, end - off, java.nio.charset.StandardCharsets.UTF_8);
    }

    private static int readUpTo(InputStream in, byte[] b) throws IOException {
        int off = 0;
        while (off < b.length) {
            int n = in.read(b, off, b.length - off);
            if (n <= 0) break;
            off += n;
        }
        return off;
    }

    private static void skipFully(InputStream in, long n) throws IOException {
        byte[] b = new byte[8192];
        while (n > 0) {
            int r = in.read(b, 0, (int) Math.min(b.length, n));
            if (r <= 0) break;
            n -= r;
        }
    }

    private static void copyTo(InputStream in, File f) throws IOException {
        try (OutputStream out = new FileOutputStream(f)) {
            byte[] b = new byte[64 * 1024];
            int n;
            while ((n = in.read(b)) > 0) out.write(b, 0, n);
        }
    }

    /** Reads at most `left` bytes of the underlying stream (one tar entry). */
    private static final class LimitedStream extends InputStream {
        private final InputStream in;
        private long left;

        LimitedStream(InputStream in, long left) {
            this.in = in;
            this.left = left;
        }

        @Override public int read() throws IOException {
            if (left <= 0) return -1;
            int b = in.read();
            if (b >= 0) left--;
            return b;
        }

        @Override public int read(byte[] b, int off, int len) throws IOException {
            if (left <= 0) return -1;
            int n = in.read(b, off, (int) Math.min(len, left));
            if (n > 0) left -= n;
            return n;
        }

        void drain() throws IOException {
            skipFully(in, left);
            left = 0;
        }
    }

    /** A zip entry stream that a nested reader must not close (closing would end the outer zip). */
    private static final class NoClose extends java.io.FilterInputStream {
        NoClose(InputStream in) {
            super(in);
        }

        @Override public void close() {}
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

    /**
     * r19: a built-in Turnip made for another Adreno generation than the phone's (an Adreno 825 was running the
     * A6xx-A7xx build, 2026-11-23) — the matching built-in one, or null when the choice is fine.
     */
    public static Driver betterBuiltIn(Context c, Driver selected) {
        if (selected == null || !selected.bundled || !Gpu.ADRENO.equals(selected.family)) return null;
        // Alternatives (gens "any": other builders' Turnips) are a deliberate choice: never switched away from.
        if (selected.gens.isEmpty() || selected.gens.contains("any")) return null;
        String code = phoneGenCode(c);
        if (code == null) return null;
        if (java.util.Arrays.asList(selected.gens.split(",")).contains(code)) return null;  // made for this GPU
        Driver rec = recommended(c);
        if (rec == null || rec.name.equals(selected.name)) return null;
        return rec;
    }

    /** "8", "7", "6" or "710" (Adreno 710/720/722) for this phone; null when not an Adreno. */
    static String phoneGenCode(Context c) {
        if (!Gpu.ADRENO.equals(Gpu.family(c))) return null;
        if (Gpu.renderer(c).matches(".*Adreno[^0-9]*7(10|20|22)\\b.*")) return "710";
        int gen = Gpu.adrenoGeneration(c);
        return gen > 0 ? String.valueOf(gen) : null;
    }

    /**
     * r19: advice for an Adreno phone not on Turnip: the System driver or an imported Qualcomm driver. Qualcomm's own
     * drivers make this game's shaders slowly (34,393 draws skipped while compiling in one session on an Adreno 650
     * with v849: black replays, 60 -> 11 FPS), Turnip made the same phone's pipelines with none skipped. Null = fine.
     */
    public static String adrenoAdvice(Context c, Driver selected) {
        if (!Gpu.ADRENO.equals(Gpu.family(c))) return null;
        if (selected != null && selected.isTurnip()) return null;
        Driver rec = recommended(c);
        if (rec == null) return null;
        if (selected == null) {
            return "Your Adreno GPU runs this game much better on the built-in " + rec.name + " driver (smoother, "
                    + "about 20 FPS more than the System driver on many phones).";
        }
        if (selected.isQualcommBlob()) {
            return "\"" + selected.name + "\" is a Qualcomm driver. In this game it builds shaders very slowly: black "
                    + "screens in replays and FPS drops. The built-in " + rec.name + " driver fixes both.";
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
