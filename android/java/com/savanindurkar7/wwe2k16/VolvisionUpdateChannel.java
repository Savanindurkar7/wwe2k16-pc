package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Checks volvisionmods.com for a newer build and, if there is one, offers to open the game's page (r18). The site is
 * where this port is published; when a new version is pushed there, players see an "Update available" popup. Supplied
 * by the site owner (Catonix) and adapted to this app's package. The tamper check (Integrity) reads APP_ID from here,
 * so this class must stay present and unchanged in a genuine build.
 */
public final class VolvisionUpdateChannel {
    static final String APP_ID = "YAehU4cLlL5Lv4ts7HB0";
    private static final String STORE_URL = "https://volvisionmods.com/game/YAehU4cLlL5Lv4ts7HB0";
    private static final String API_URL = "https://volvision-api.syprusseal2.workers.dev/api/update-check?appId=" + APP_ID;

    /** Set once a check has actually run this session; the tamper check verifies it fires. */
    static volatile boolean checkStarted = false;

    public static void checkForUpdates(final Activity activity, final String currentVersion) {
        checkStarted = true;
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    HttpURLConnection conn = (HttpURLConnection) new URL(API_URL).openConnection();
                    conn.setConnectTimeout(7000);
                    conn.setReadTimeout(7000);
                    conn.setRequestMethod("GET");
                    conn.setRequestProperty("Accept", "application/json");

                    if (conn.getResponseCode() == 200) {
                        BufferedReader reader = new BufferedReader(new InputStreamReader(conn.getInputStream()));
                        StringBuilder sb = new StringBuilder();
                        String line;
                        while ((line = reader.readLine()) != null) sb.append(line);
                        reader.close();

                        JSONObject json = new JSONObject(sb.toString());
                        final String latestVersion = json.optString("latestVersion", "1.0.2");
                        final String whatsNew = json.optString("whatsNew",
                                "New features, improved performance, and bug fixes.");
                        final String redirectUrl = json.optString("storeUrl", STORE_URL);

                        if (isNewerVersion(latestVersion, currentVersion)) {
                            new Handler(Looper.getMainLooper()).post(new Runnable() {
                                @Override
                                public void run() {
                                    showDialog(activity, latestVersion, whatsNew, redirectUrl);
                                }
                            });
                        }
                    }
                } catch (Exception e) {
                    // Offline or the site is unreachable: no popup, the game is unaffected.
                    e.printStackTrace();
                }
            }
        }).start();
    }

    private static void showDialog(final Activity activity, String newVersion, String whatsNew, final String redirectUrl) {
        if (activity == null || activity.isFinishing()) return;
        new AlertDialog.Builder(activity)
                .setTitle("Update Available (v" + newVersion + ")")
                .setMessage(whatsNew + "\n\nTap Update Now to open the game page.")
                .setPositiveButton("Update Now", (dialog, which) -> {
                    Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(redirectUrl));
                    activity.startActivity(intent);
                })
                .setNegativeButton("Later", null)
                .setCancelable(true)
                .show();
    }

    private static boolean isNewerVersion(String remote, String local) {
        String[] r = remote.split("[.-]");
        String[] l = local.split("[.-]");
        int max = Math.max(r.length, l.length);
        for (int i = 0; i < max; i++) {
            int rv = i < r.length ? parseIntSafe(r[i]) : 0;
            int lv = i < l.length ? parseIntSafe(l[i]) : 0;
            if (rv > lv) return true;
            if (rv < lv) return false;
        }
        return false;
    }

    private static int parseIntSafe(String s) {
        try {
            return Integer.parseInt(s.replaceAll("\\D+", ""));
        } catch (Exception e) {
            return 0;
        }
    }
}
