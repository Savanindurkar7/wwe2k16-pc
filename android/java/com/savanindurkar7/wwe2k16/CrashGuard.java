package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import java.util.List;

/**
 * r21: what the launcher does after the game crashed, so a player is not left with a game that closes after the main
 * menu every time (1.2.1 logs, 2026-11-25: an Adreno 650 on its own Qualcomm driver, Vulkan 1.1.128, crashed in the
 * driver in every match; the Turnip it had picked had silently not loaded).
 *
 * On the launcher's start it reads Android's record of how the game process (":game") last ended. After a crash:
 *   - a custom driver that the game reported as not loaded is marked and the next built-in Turnip is chosen;
 *   - an Adreno phone on its own driver is moved to the best built-in Turnip that has not failed on it;
 *   - a second crash in a row puts every setting back to the tested values for the phone's GPU (expert mode off).
 * The player sees one dialog saying what was changed.
 */
final class CrashGuard {
    private CrashGuard() {}

    private static final String KEY_SEEN = "crash_guard_seen";
    private static final String KEY_COUNT = "crash_guard_count";

    static void check(Activity a) {
        SharedPreferences prefs = Settings.prefs(a);
        StringBuilder msg = new StringBuilder();

        // 1. The game's own report about the custom driver (Drivers.statusFile).
        String[] status = Drivers.takeStatus(a);
        if (status != null && "notloaded".equals(status[0])) {
            Drivers.Driver cur = Drivers.find(a, prefs.getString(Settings.KEY_DRIVER, ""));
            if (cur != null) {
                Drivers.markFailed(a, cur);
                Drivers.Driver next = Drivers.nextAfter(a, cur);
                prefs.edit().putString(Settings.KEY_DRIVER, next != null ? next.name : "").apply();
                msg.append("The driver \"").append(cur.name).append("\" does not load on your phone (your phone kept its "
                        + "own driver). ").append(next != null ? "Switched to \"" + next.name + "\"."
                        : "Switched back to the System driver.").append("\n\n");
            }
        }

        // 2. How the game process ended last time.
        if (Build.VERSION.SDK_INT >= 30) {
            ApplicationExitInfo last = lastGameExit(a);
            if (last != null && last.getTimestamp() > prefs.getLong(KEY_SEEN, 0)) {
                prefs.edit().putLong(KEY_SEEN, last.getTimestamp()).apply();
                int reason = last.getReason();
                boolean crashed = reason == ApplicationExitInfo.REASON_CRASH_NATIVE
                        || reason == ApplicationExitInfo.REASON_CRASH;
                if (!crashed) {
                    prefs.edit().putInt(KEY_COUNT, 0).apply();
                } else {
                    int count = prefs.getInt(KEY_COUNT, 0) + 1;
                    prefs.edit().putInt(KEY_COUNT, count).apply();
                    afterCrash(a, prefs, count, msg);
                }
            }
        }

        if (msg.length() > 0 && !a.isFinishing()) {
            new AlertDialog.Builder(a)
                    .setTitle("The game closed unexpectedly")
                    .setMessage(msg.toString().trim() + "\n\nPress PLAY to try again. If it still closes, save the logs "
                            + "(Logs page) and send them to Catonix.")
                    .setPositiveButton("OK", null)
                    .show();
        }
    }

    private static void afterCrash(Context c, SharedPreferences prefs, int count, StringBuilder msg) {
        String driverName = prefs.getString(Settings.KEY_DRIVER, "");
        Drivers.Driver cur = Drivers.find(c, driverName);
        if (Gpu.ADRENO.equals(Gpu.family(c))) {
            if (cur == null || cur.isQualcommBlob()) {
                // The phone's own (or a Qualcomm) driver crashed: the old Qualcomm drivers crash in this game.
                Drivers.Driver next = Drivers.nextAfter(c, null);
                if (next != null) {
                    prefs.edit().putString(Settings.KEY_DRIVER, next.name).apply();
                    msg.append("It crashed inside your phone's GPU driver. The built-in \"").append(next.name)
                            .append("\" driver (Turnip) is now selected: it is steadier in this game.\n\n");
                }
            } else if (count >= 2 && cur.isTurnip()) {
                Drivers.markFailed(c, cur);
                Drivers.Driver next = Drivers.nextAfter(c, cur);
                prefs.edit().putString(Settings.KEY_DRIVER, next != null ? next.name : "").apply();
                msg.append("It crashed twice with \"").append(cur.name).append("\". ")
                        .append(next != null ? "Switched to \"" + next.name + "\"." : "Switched to the System driver.")
                        .append("\n\n");
            }
        } else if (cur != null) {
            // Mali and others: a custom driver that crashes goes back to the System driver.
            prefs.edit().putString(Settings.KEY_DRIVER, "").apply();
            msg.append("It crashed with the driver \"").append(cur.name).append("\": back to the System driver.\n\n");
        }
        if (count >= 2) {
            prefs.edit().putBoolean(Settings.KEY_EXPERT, false).apply();
            Settings.applyPreset(c, Gpu.family(c));
            msg.append("It crashed ").append(count).append(" times in a row, so every setting was put back to the "
                    + "tested values for your GPU.\n\n");
        }
    }

    /** True when the game process's last end was a crash (Android 11+; false when unknown). */
    static boolean lastExitWasCrash(Context c) {
        if (Build.VERSION.SDK_INT < 30) return false;
        ApplicationExitInfo last = lastGameExit(c);
        return last != null && (last.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                || last.getReason() == ApplicationExitInfo.REASON_CRASH);
    }

    private static ApplicationExitInfo lastGameExit(Context c) {
        try {
            ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
            List<ApplicationExitInfo> infos = am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 10);
            ApplicationExitInfo best = null;
            for (ApplicationExitInfo i : infos) {
                String p = i.getProcessName();
                if (p == null || !p.endsWith(":game")) continue;
                if (best == null || i.getTimestamp() > best.getTimestamp()) best = i;
            }
            return best;
        } catch (Exception e) {
            return null;
        }
    }
}
