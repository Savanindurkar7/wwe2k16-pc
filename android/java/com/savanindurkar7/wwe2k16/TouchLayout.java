package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Where each on-screen control sits (centre as a fraction of the screen), its own size, and the overall size and
 * opacity. Saved per phone; the editor (TouchEditActivity) changes it, the game screen reads it.
 */
final class TouchLayout {
    static final int LSTICK = 0, RSTICK = 1, DPAD = 2, A = 3, B = 4, X = 5, Y = 6, LB = 7, RB = 8, LT = 9, RT = 10,
            BACK = 11, START = 12, L3 = 13, R3 = 14;
    static final int COUNT = 15;
    static final String[] NAMES = {"LEFT STICK", "RIGHT STICK", "D-PAD", "A", "B", "X", "Y", "LB", "RB", "LT", "RT",
            "BACK", "START", "L3", "R3"};

    // Defaults for a landscape phone: centre x, centre y (fractions), base size in dp.
    static final float[][] DEFAULTS = {
            {0.12f, 0.60f, 150},  // left stick
            {0.70f, 0.80f, 120},  // right stick
            {0.27f, 0.80f, 120},  // d-pad
            {0.885f, 0.80f, 58},  // A
            {0.950f, 0.62f, 58},  // B
            {0.820f, 0.62f, 58},  // X
            {0.885f, 0.44f, 58},  // Y
            {0.165f, 0.12f, 64},  // LB
            {0.835f, 0.12f, 64},  // RB
            {0.055f, 0.12f, 64},  // LT
            {0.945f, 0.12f, 64},  // RT
            {0.430f, 0.09f, 46},  // BACK
            {0.570f, 0.09f, 46},  // START
            {0.215f, 0.38f, 44},  // L3
            {0.785f, 0.38f, 44},  // R3
    };

    final float[] x = new float[COUNT], y = new float[COUNT], scale = new float[COUNT];
    float size = 1.0f;     // all controls
    float opacity = 0.6f;

    private static final String PREFS = "touch_layout";

    static TouchLayout load(Context c) {
        SharedPreferences p = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        TouchLayout l = new TouchLayout();
        for (int i = 0; i < COUNT; i++) {
            l.x[i] = p.getFloat("x" + i, DEFAULTS[i][0]);
            l.y[i] = p.getFloat("y" + i, DEFAULTS[i][1]);
            l.scale[i] = p.getFloat("s" + i, 1.0f);
        }
        l.size = p.getFloat("size", 1.0f);
        l.opacity = p.getFloat("opacity", 0.6f);
        return l;
    }

    void save(Context c) {
        SharedPreferences.Editor e = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit();
        for (int i = 0; i < COUNT; i++) {
            e.putFloat("x" + i, x[i]);
            e.putFloat("y" + i, y[i]);
            e.putFloat("s" + i, scale[i]);
        }
        e.putFloat("size", size);
        e.putFloat("opacity", opacity);
        e.apply();
    }

    static TouchLayout defaults() {
        TouchLayout l = new TouchLayout();
        for (int i = 0; i < COUNT; i++) {
            l.x[i] = DEFAULTS[i][0];
            l.y[i] = DEFAULTS[i][1];
            l.scale[i] = 1.0f;
        }
        return l;
    }

    TouchLayout copy() {
        TouchLayout l = new TouchLayout();
        System.arraycopy(x, 0, l.x, 0, COUNT);
        System.arraycopy(y, 0, l.y, 0, COUNT);
        System.arraycopy(scale, 0, l.scale, 0, COUNT);
        l.size = size;
        l.opacity = opacity;
        return l;
    }

    /** Diameter (or width) of control i in dp. */
    float sizeDp(int i) {
        return DEFAULTS[i][2] * scale[i] * size;
    }
}
