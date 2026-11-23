package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.content.pm.PackageInfo;

final class BuildInfo {
    private BuildInfo() {}

    /** The release version shown in Android's App Info, e.g. "1.2.0" (r18). */
    static String rawVersion(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "1.2.0";
        }
    }

    /** Short git hash of this build (r18), e.g. "7a36925"; "dev" when unknown. */
    static String buildId(Context c) {
        try {
            int id = c.getResources().getIdentifier("wwe2k16_build_id", "string", c.getPackageName());
            if (id != 0) return c.getString(id);
        } catch (Exception ignored) {
        }
        return "dev";
    }

    /** Human string for the status card / logs: "1.2.0 (build 7a36925)". */
    static String versionName(Context c) {
        return rawVersion(c) + " (build " + buildId(c) + ")";
    }
}
