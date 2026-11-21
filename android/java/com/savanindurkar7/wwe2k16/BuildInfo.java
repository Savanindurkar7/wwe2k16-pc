package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.content.pm.PackageInfo;

final class BuildInfo {
    private BuildInfo() {}

    static String versionName(Context c) {
        try {
            PackageInfo info = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return info.versionName + " (" + info.getLongVersionCode() + ")";
        } catch (Exception e) {
            return "?";
        }
    }
}
