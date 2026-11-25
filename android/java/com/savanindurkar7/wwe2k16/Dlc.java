package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * DLC (r17). The player picks Xbox 360 DLC package files (LIVE / PIRS / CON, usually a 42-character hex name, from
 * the content folder of a console or a backup). They are copied into userdata/dlc_import/; when the game starts it
 * unpacks each one before the game runs (src/dlc_install.cpp, the SDK's ContentManager::InstallContent) into
 *   userdata/0000000000000000/315A07D2/00000002/<package name>/   (+ Headers/00000002/<package name>.header)
 * where the game looks for downloaded content. The launcher lists what is installed, waiting or failed.
 */
public final class Dlc {
    /** WWE 2K16's title id. */
    public static final String TITLE_ID = "315A07D2";

    private Dlc() {}

    public static File inbox(Context c) {
        return new File(Settings.userDataDir(c), "dlc_import");
    }

    static File installedRoot(Context c) {
        return new File(Settings.userDataDir(c), "0000000000000000/" + TITLE_ID + "/00000002");
    }

    static File headersRoot(Context c) {
        return new File(Settings.userDataDir(c), "0000000000000000/" + TITLE_ID + "/Headers/00000002");
    }

    public static final class Item {
        public final String name, title;
        public final long bytes;
        public final File file;

        Item(String name, String title, long bytes, File file) {
            this.name = name;
            this.title = title;
            this.bytes = bytes;
            this.file = file;
        }
    }

    /** Installed packages (folders the game reads), with the name from their header when it has one. */
    public static List<Item> installed(Context c) {
        List<Item> out = new ArrayList<>();
        File[] dirs = installedRoot(c).listFiles();
        if (dirs == null) return out;
        for (File d : dirs) {
            if (!d.isDirectory()) continue;
            String title = headerTitle(new File(headersRoot(c), d.getName() + ".header"));
            out.add(new Item(d.getName(), title == null ? d.getName() : title, size(d), d));
        }
        return out;
    }

    /** Packages copied in and not installed yet (they install at the next PLAY). */
    public static List<Item> waiting(Context c) {
        List<Item> out = new ArrayList<>();
        File[] files = inbox(c).listFiles();
        if (files == null) return out;
        for (File f : files) {
            if (!f.isFile() || f.getName().equals("result.txt") || f.getName().startsWith(".")) continue;
            out.add(new Item(f.getName(), f.getName(), f.length(), f));
        }
        return out;
    }

    /** The last lines the game wrote about installs ("OK name" / "FAILED name: why"). */
    public static List<String> results(Context c) {
        List<String> out = new ArrayList<>();
        File r = new File(inbox(c), "result.txt");
        if (!r.isFile()) return out;
        try (InputStream in = new FileInputStream(r)) {
            byte[] b = new byte[(int) Math.min(r.length(), 64 * 1024)];
            int n = in.read(b);
            if (n > 0) for (String line : new String(b, 0, n, StandardCharsets.UTF_8).split("\n")) {
                if (!line.trim().isEmpty()) out.add(line.trim());
            }
        } catch (IOException ignored) {
        }
        int from = Math.max(0, out.size() - 8);
        return new ArrayList<>(out.subList(from, out.size()));
    }

    public static void clearResults(Context c) {
        //noinspection ResultOfMethodCallIgnored
        new File(inbox(c), "result.txt").delete();
        deleteTree(new File(inbox(c), "failed"));
    }

    // --- Known WWE 2K16 Xbox 360 DLC (r21) ----------------------------------------------------------------------------
    // Everything 2K released for the Xbox 360 version (Season Pass content and the pre-order bonus). Shown as a
    // checklist on the DLC page, matched by the name in each installed package's header.
    public static final String[][] KNOWN = {
            {"Terminator Pre-Order Pack", "terminator|t-800|arnold"},
            {"Accelerator", "accelerator"},
            {"Legends Pack", "legends"},
            {"2015 Hall of Fame Showcase", "hall of fame|showcase|hof"},
            {"New Moves Pack", "moves"},
            {"Future Stars Pack", "future stars|future"},
    };

    /** For each KNOWN pack: is a matching package installed? */
    public static boolean[] knownInstalled(Context c) {
        boolean[] out = new boolean[KNOWN.length];
        for (Item it : installed(c)) {
            String t = it.title.toLowerCase(java.util.Locale.ROOT);
            for (int i = 0; i < KNOWN.length; i++) if (t.matches(".*(" + KNOWN[i][1] + ").*")) out[i] = true;
        }
        return out;
    }

    /**
     * Copies one picked file into the inbox: an Xbox 360 DLC package, or (r21) a .zip holding any number of them (in
     * any folders). Each package is checked first: it must be WWE 2K16's (title id 315A07D2) and downloadable content
     * (content type 2), not a title update, a save or another game's DLC. Returns the names added; throws with a
     * message for the player when nothing usable was in it.
     */
    public static List<String> addAny(Context c, Uri uri) throws IOException {
        String name = displayName(c, uri);
        if (name == null || name.isEmpty()) name = "dlc_" + System.currentTimeMillis();
        List<String> added = new ArrayList<>();
        StringBuilder skipped = new StringBuilder();
        try (InputStream raw = c.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("could not open " + name);
            java.io.BufferedInputStream in = new java.io.BufferedInputStream(raw, 256 * 1024);
            in.mark(8);
            byte[] head = new byte[4];
            int got = readFully(in, head);
            in.reset();
            if (got == 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) {
                java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(in);
                java.util.zip.ZipEntry e;
                while ((e = zip.getNextEntry()) != null) {
                    if (e.isDirectory()) continue;
                    String entry = e.getName();
                    String base = entry.substring(entry.lastIndexOf('/') + 1);
                    if (base.isEmpty() || base.startsWith(".")) continue;
                    try {
                        added.add(store(c, new NoClose(zip), base));
                    } catch (IOException ex) {
                        if (!ex.getMessage().contains("not an Xbox 360")) skipped.append(ex.getMessage()).append('\n');
                    }
                }
                if (added.isEmpty()) {
                    throw new IOException(name + ": no WWE 2K16 DLC package inside the zip"
                            + (skipped.length() > 0 ? " (" + skipped.toString().trim() + ")" : ""));
                }
                return added;
            }
            added.add(store(c, in, name));
        }
        return added;
    }

    /** Old entry point: one package file. */
    public static String add(Context c, Uri uri) throws IOException {
        List<String> names = addAny(c, uri);
        return names.isEmpty() ? "" : names.get(0);
    }

    private static String store(Context c, InputStream in, String name) throws IOException {
        name = name.replaceAll("[^A-Za-z0-9._ ()+-]", "_");
        File dir = inbox(c);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create the DLC folder");
        File part = new File(dir, "." + name + ".part");
        try (OutputStream out = new FileOutputStream(part)) {
            // The STFS header up to the title id (0x364 bytes) is checked before the rest is copied.
            byte[] head = new byte[0x364];
            int got = readFully(in, head);
            if (got < 4 || !isPackage(head)) {
                throw new IOException(name + " is not an Xbox 360 DLC package (it must start with LIVE, PIRS or CON)");
            }
            if (got == head.length) {
                long type = be32(head, 0x344);
                long title = be32(head, 0x360);
                if (title != 0x315A07D2L && title != 0) {
                    throw new IOException(name + " is DLC for another game (title id " + String.format("%08X", title)
                            + "), not WWE 2K16 (" + TITLE_ID + ")");
                }
                if (type == 0x000B0000L) throw new IOException(name + " is a title update, not DLC: it is not needed");
                if (type == 0x00000001L) throw new IOException(name + " is a saved game, not DLC");
                if (type != 0x00000002L && type != 0) {
                    throw new IOException(name + " is not downloadable content (content type "
                            + String.format("%08X", type) + ")");
                }
            }
            out.write(head, 0, got);
            byte[] buffer = new byte[256 * 1024];
            int n;
            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            throw e;
        }
        File target = new File(dir, name);
        //noinspection ResultOfMethodCallIgnored
        target.delete();
        if (!part.renameTo(target)) throw new IOException("could not store " + name);
        return name;
    }

    private static long be32(byte[] b, int off) {
        return ((b[off] & 0xFFL) << 24) | ((b[off + 1] & 0xFFL) << 16) | ((b[off + 2] & 0xFFL) << 8) | (b[off + 3] & 0xFFL);
    }

    /** A zip entry stream that must not close the zip. */
    private static final class NoClose extends java.io.FilterInputStream {
        NoClose(InputStream in) {
            super(in);
        }

        @Override public void close() {}
    }

    public static void delete(Context c, Item item) {
        deleteTree(item.file);
        //noinspection ResultOfMethodCallIgnored
        new File(headersRoot(c), item.name + ".header").delete();
    }

    static boolean isPackage(byte[] h) {
        String m = new String(h, 0, 4, StandardCharsets.ISO_8859_1);
        return m.equals("LIVE") || m.equals("PIRS") || m.equals("CON ");
    }

    private static int readFully(InputStream in, byte[] b) throws IOException {
        int off = 0;
        while (off < b.length) {
            int n = in.read(b, off, b.length - off);
            if (n <= 0) break;
            off += n;
        }
        return off;
    }

    private static String displayName(Context c, Uri uri) {
        try (Cursor cur = c.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cur != null && cur.moveToFirst()) return cur.getString(0);
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        if (last != null && last.contains("/")) last = last.substring(last.lastIndexOf('/') + 1);
        return last;
    }

    /** The display name in a .header file (XCONTENT_DATA: 8 bytes, then 128 UTF-16 characters). */
    private static String headerTitle(File header) {
        if (!header.isFile()) return null;
        byte[] b = new byte[8 + 256];
        try (InputStream in = new FileInputStream(header)) {
            if (readFully(in, b) < b.length) return null;
        } catch (IOException e) {
            return null;
        }
        String be = decode(b, true), le = decode(b, false);
        String best = printable(be) >= printable(le) ? be : le;
        return best.trim().isEmpty() ? null : best.trim();
    }

    private static String decode(byte[] b, boolean bigEndian) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 128; i++) {
            int hi = b[8 + i * 2] & 0xFF, lo = b[9 + i * 2] & 0xFF;
            char ch = (char) (bigEndian ? (hi << 8 | lo) : (lo << 8 | hi));
            if (ch == 0) break;
            sb.append(ch);
        }
        return sb.toString();
    }

    private static int printable(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch >= 0x20 && ch < 0x7F) n++;
        }
        return n;
    }

    static long size(File f) {
        if (f.isFile()) return f.length();
        long total = 0;
        File[] children = f.listFiles();
        if (children != null) for (File ch : children) total += size(ch);
        return total;
    }

    public static String formatSize(long bytes) {
        if (bytes >= 1L << 30) return String.format(java.util.Locale.ROOT, "%.1f GB", bytes / (double) (1L << 30));
        return String.format(java.util.Locale.ROOT, "%d MB", Math.max(1, bytes >> 20));
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File ch : children) deleteTree(ch);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
