package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.lang.reflect.Field;
import java.security.MessageDigest;
import java.util.Random;

/**
 * Self-integrity / anti-repackage check (r18), at the app owner's request: if someone strips the update channel or the
 * site branding (VolvisionUpdateChannel) out of the app, or repackages it under a different signature, the app shuts
 * itself down shortly after start instead of running cracked.
 *
 * What it verifies:
 *   - the package is the real one (com.savanindurkar7.wwe2k16);
 *   - VolvisionUpdateChannel is still present and its APP_ID is unchanged (reflection: removing the class or editing
 *     the id throws / mismatches);
 *   - the native library's copy of the brand id matches the Java one (so the two can't be edited independently) —
 *     checked in the game process, where the library is loaded (enforceWithNative);
 *   - the update check actually runs at launch (checked a few seconds later);
 *   - (optional) the signing certificate matches the release one, when a hash was baked in at build time
 *     (WWE2K16_CERT_SHA256); off by default, so the owner's own debug builds run.
 *
 * On tamper it does not crash at the call site: it schedules a delayed process kill, so the failure is away from the
 * check and harder to patch out.
 */
public final class Integrity {
    private Integrity() {}

    private static final String EXPECTED_PACKAGE = "com.savanindurkar7.wwe2k16";
    private static final String EXPECTED_APP_ID = "hWwLaEjqhdAGNiIBAuyu";

    static native int nativeBrandToken();  // implemented in src/integrity.cpp (game process only)

    /** Launcher process: Java-only checks. Call early in onCreate. */
    public static void enforce(final Activity activity) {
        try {
            if (!EXPECTED_PACKAGE.equals(activity.getPackageName())) {
                trip(activity);
                return;
            }
            if (!brandingIntact()) {
                trip(activity);
                return;
            }
            if (!certificateOk(activity)) {
                trip(activity);
                return;
            }
        } catch (Throwable t) {
            // Any failure of the checks themselves (e.g. the branding class was removed) is treated as tamper.
            trip(activity);
            return;
        }
        // A few seconds after start the update check must have fired. If the call was stripped out, this trips.
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!VolvisionUpdateChannel.checkStarted) trip(activity);
        }, 9000);
    }

    /** Game process: the Java checks plus the native brand-token match (the library is loaded here). */
    public static void enforceWithNative(final Activity activity) {
        try {
            if (!EXPECTED_PACKAGE.equals(activity.getPackageName()) || !brandingIntact()) {
                trip(activity);
                return;
            }
            if (fnv(EXPECTED_APP_ID) != nativeBrandToken()) {
                trip(activity);
                return;
            }
        } catch (Throwable t) {
            // Missing native symbol (integrity.cpp stripped) or missing class → tamper.
            trip(activity);
        }
    }

    /** True only if VolvisionUpdateChannel is present and its APP_ID is unchanged. */
    private static boolean brandingIntact() throws Exception {
        Class<?> cls = Class.forName("com.savanindurkar7.wwe2k16.VolvisionUpdateChannel");
        Field f = cls.getDeclaredField("APP_ID");
        f.setAccessible(true);
        Object v = f.get(null);
        return EXPECTED_APP_ID.equals(v);
    }

    /** Optional signing-certificate pin. Passes when no expected hash was baked in (debug builds). */
    private static boolean certificateOk(Context c) {
        String expected = "";
        try {
            int id = c.getResources().getIdentifier("wwe2k16_cert_sha256", "string", c.getPackageName());
            if (id != 0) expected = c.getString(id).trim().toLowerCase();
        } catch (Throwable ignored) {
        }
        if (expected.isEmpty()) return true;  // not enforced for unsigned / debug builds
        try {
            android.content.pm.PackageManager pm = c.getPackageManager();
            byte[] cert;
            if (android.os.Build.VERSION.SDK_INT >= 28) {
                android.content.pm.SigningInfo si = pm.getPackageInfo(c.getPackageName(),
                        android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES).signingInfo;
                cert = si.getApkContentsSigners()[0].toByteArray();
            } else {
                @SuppressWarnings("deprecation")
                android.content.pm.Signature[] sigs = pm.getPackageInfo(c.getPackageName(),
                        android.content.pm.PackageManager.GET_SIGNATURES).signatures;
                cert = sigs[0].toByteArray();
            }
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] h = md.digest(cert);
            StringBuilder sb = new StringBuilder();
            for (byte b : h) sb.append(String.format("%02x", b));
            return sb.toString().equals(expected);
        } catch (Throwable t) {
            return false;
        }
    }

    /** FNV-1a 32-bit, matched by src/integrity.cpp. */
    static int fnv(String s) {
        int h = 0x811c9dc5;
        for (int i = 0; i < s.length(); i++) {
            h ^= (s.charAt(i) & 0xFF);
            h *= 0x01000193;
        }
        return h;
    }

    private static void trip(final Activity activity) {
        // Kill the process after a random delay, away from the check, so it is harder to locate and patch out.
        final int delay = 1500 + new Random().nextInt(6000);
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            android.os.Process.killProcess(android.os.Process.myPid());
            System.exit(2);
        }, delay);
    }
}
