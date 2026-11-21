package com.savanindurkar7.wwe2k16;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Collects everything needed to diagnose a run into one zip in Download/WWE 2K16 Logs/: the game's log (this run and
 * the one before), the per-frame log (frames.csv), the settings used, device details, this app's logcat and Android's
 * record of how the game process ended (with the native crash trace, Android 12+).
 */
public final class Logs {
    public static final String FOLDER = "WWE 2K16 Logs";

    private Logs() {}

    /** Before a new run: the last run's logs become the "previous" ones. */
    public static void rotate(Context c) {
        File dir = Settings.logsDir(c);
        for (String base : new String[] {"wwe2k16.log", "frames.csv"}) {
            File current = new File(dir, base);
            if (!current.isFile()) continue;
            int dot = base.lastIndexOf('.');
            File previous = new File(dir, base.substring(0, dot) + "-previous" + base.substring(dot));
            //noinspection ResultOfMethodCallIgnored
            previous.delete();
            //noinspection ResultOfMethodCallIgnored
            current.renameTo(previous);
        }
    }

    public static void writeSettings(Context c, String summary) {
        try (OutputStream out = new java.io.FileOutputStream(new File(Settings.logsDir(c), "settings.txt"))) {
            out.write(("Started " + new Date() + "\n\n" + summary).getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    public static void clear(Context c) {
        File[] files = Settings.logsDir(c).listFiles();
        if (files != null) for (File f : files) //noinspection ResultOfMethodCallIgnored
            f.delete();
    }

    /** Builds the zip and stores it in Download/WWE 2K16 Logs. Returns its content URI (for sharing). */
    public static Uri saveZip(Context c) throws IOException {
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
        String name = "wwe2k16-logs-" + stamp + ".zip";
        ContentResolver resolver = c.getContentResolver();
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        values.put(MediaStore.MediaColumns.MIME_TYPE, "application/zip");
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/" + FOLDER);
        values.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (uri == null) throw new IOException("Android did not create the file in Download");
        try (OutputStream raw = resolver.openOutputStream(uri); ZipOutputStream zip = new ZipOutputStream(raw)) {
            String prefix = FOLDER + "/";
            File[] files = Settings.logsDir(c).listFiles();
            if (files != null) {
                for (File f : files) if (f.isFile()) addFile(zip, prefix + f.getName(), f);
            }
            addText(zip, prefix + "device.txt", deviceInfo(c));
            addText(zip, prefix + "exit-reasons.txt", exitReasons(c, zip, prefix));
            addText(zip, prefix + "logcat.txt", logcat());
        } catch (IOException e) {
            resolver.delete(uri, null, null);
            throw e;
        }
        values.clear();
        values.put(MediaStore.MediaColumns.IS_PENDING, 0);
        resolver.update(uri, values, null, null);
        return uri;
    }

    private static void addFile(ZipOutputStream zip, String name, File f) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        try (InputStream in = new FileInputStream(f)) {
            copy(in, zip);
        }
        zip.closeEntry();
    }

    private static void addText(ZipOutputStream zip, String name, String text) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(text.getBytes(StandardCharsets.UTF_8));
        zip.closeEntry();
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        int n;
        while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
    }

    static String deviceInfo(Context c) {
        StringBuilder s = new StringBuilder();
        s.append("Phone: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE)
                .append(")\n");
        s.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        if (Build.VERSION.SDK_INT >= 31) {
            s.append("Chip: ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        }
        s.append("Board / hardware: ").append(Build.BOARD).append(" / ").append(Build.HARDWARE).append('\n');
        s.append("ABIs: ").append(String.join(", ", Build.SUPPORTED_ABIS)).append('\n');
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (am != null) {
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            s.append("Memory: ").append(mi.totalMem / (1024 * 1024)).append(" MB total, ")
                    .append(mi.availMem / (1024 * 1024)).append(" MB free\n");
        }
        File data = c.getFilesDir();
        s.append("App storage free: ").append(data.getUsableSpace() / (1024 * 1024)).append(" MB\n");
        s.append("App version: ").append(BuildInfo.versionName(c)).append('\n');
        return s.toString();
    }

    /** Android's record of how the game process ended; a native crash also brings its trace (Android 12+). */
    private static String exitReasons(Context c, ZipOutputStream zip, String prefix) {
        if (Build.VERSION.SDK_INT < 30) return "Android 11 or newer is needed for exit reasons.\n";
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return "";
        StringBuilder s = new StringBuilder();
        List<ApplicationExitInfo> infos = am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 8);
        int traces = 0;
        for (ApplicationExitInfo info : infos) {
            s.append(new Date(info.getTimestamp())).append("  process ").append(info.getProcessName())
                    .append("  reason ").append(reasonName(info.getReason())).append("  status ")
                    .append(info.getStatus()).append("  importance ").append(info.getImportance())
                    .append("  memory ").append(info.getPss() / 1024).append(" MB\n    ")
                    .append(info.getDescription()).append('\n');
            if (Build.VERSION.SDK_INT >= 31 && traces < 2 && (info.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                    || info.getReason() == ApplicationExitInfo.REASON_ANR)) {
                try (InputStream trace = info.getTraceInputStream()) {
                    if (trace != null) {
                        zip.putNextEntry(new ZipEntry(prefix + "crash-trace-" + (++traces) + ".txt"));
                        copy(trace, zip);
                        zip.closeEntry();
                        s.append("    trace saved as crash-trace-").append(traces).append(".txt\n");
                    }
                } catch (IOException ignored) {
                }
            }
        }
        return s.toString();
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
            case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY";
            case ApplicationExitInfo.REASON_CRASH: return "CRASH (Java)";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
            case ApplicationExitInfo.REASON_ANR: return "ANR (not responding)";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INITIALIZATION_FAILURE";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "PERMISSION_CHANGE";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "USER_STOPPED";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY_DIED";
            case ApplicationExitInfo.REASON_OTHER: return "OTHER";
            default: return "UNKNOWN(" + reason + ")";
        }
    }

    /** This app's own log lines (both processes run as the same app, so the game's lines are included). */
    private static String logcat() {
        try {
            Process p = new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", "6000").redirectErrorStream(true).start();
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (InputStream in = p.getInputStream()) {
                copy(in, bytes);
            }
            p.waitFor();
            return bytes.toString("UTF-8");
        } catch (Exception e) {
            return "logcat unavailable: " + e + "\n";
        }
    }

    /** The last lines of the game log, for the launcher's "last run" message. */
    public static String tail(Context c, int lines) {
        File log = new File(Settings.logsDir(c), "wwe2k16.log");
        if (!log.isFile()) return "";
        java.util.ArrayDeque<String> last = new java.util.ArrayDeque<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(log), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                last.addLast(line);
                if (last.size() > lines) last.removeFirst();
            }
        } catch (IOException ignored) {
        }
        return String.join("\n", last);
    }
}
