package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.widget.TextView;

/** Pieces of the home screen's look (the launcher's red-and-black cards over the wrestler art). */
final class Home {
    private Home() {}

    /** The background art the build packs from the repository (BG1.png, BG2.jpg, BG3.jpg, BG34-.jpg). */
    static final String[] ART = {"home_bg1", "home_bg2", "home_bg3", "home_bg4"};

    /** The art that is in this APK, in order. */
    static java.util.List<Integer> artIds(Context c) {
        java.util.List<Integer> ids = new java.util.ArrayList<>();
        for (String name : ART) {
            int id = c.getResources().getIdentifier(name, "drawable", c.getPackageName());
            if (id != 0) ids.add(id);
        }
        return ids;
    }

    private static final java.util.Map<String, Bitmap> cache = new java.util.HashMap<>();

    /** One picture, decoded at most about `maxWidth` wide (the 2560-wide one would take 16 MB otherwise). */
    static Bitmap art(Context c, int id, int maxWidth) {
        String key = id + "@" + maxWidth;
        Bitmap b = cache.get(key);
        if (b != null) return b;
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeResource(c.getResources(), id, o);
        int sample = 1;
        while (o.outWidth / (sample * 2) >= maxWidth) sample *= 2;
        o = new BitmapFactory.Options();
        o.inSampleSize = sample;
        try {
            b = BitmapFactory.decodeResource(c.getResources(), id, o);
        } catch (OutOfMemoryError e) {
            return null;
        }
        if (b != null) cache.put(key, b);
        return b;
    }

    /** A rounded button background with a pressed / focused colour and a ripple. */
    static Drawable pill(Context c, int color, int focus, float radius) {
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {android.R.attr.state_focused}, Ui.rect(focus, radius));
        states.addState(new int[] {android.R.attr.state_pressed}, Ui.rect(focus, radius));
        states.addState(new int[] {}, Ui.rect(color, radius));
        return new RippleDrawable(ColorStateList.valueOf(0x44FFFFFF), states, null);
    }

    /** An outlined dark button (Verify again, Clear cache, Configure). */
    static Drawable outline(Context c, float radius) {
        GradientDrawable base = Ui.rect(0x66141418, radius);
        base.setStroke(Math.max(1, Ui.dp(c, 1.5f)), 0x80FFFFFF);
        GradientDrawable focus = Ui.rect(0xCC2A2A31, radius);
        focus.setStroke(Math.max(1, Ui.dp(c, 1.5f)), 0xFFFFFFFF);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {android.R.attr.state_focused}, focus);
        states.addState(new int[] {android.R.attr.state_pressed}, focus);
        states.addState(new int[] {}, base);
        return new RippleDrawable(ColorStateList.valueOf(0x44FFFFFF), states, null);
    }

    /** A small grey chip (Recomp, Vulkan, 60 FPS). */
    static TextView chip(Context c, String s) {
        TextView t = Ui.text(c, s, 13, 0xFFEDEDF0, Ui.regular());
        t.setPadding(Ui.dp(c, 12), Ui.dp(c, 5), Ui.dp(c, 12), Ui.dp(c, 5));
        t.setBackground(Ui.rect(0x8C3A3A40, Ui.dp(c, 6)));
        t.setGravity(Gravity.CENTER);
        return t;
    }

    /** A card background: dark glass with a thin light edge. */
    static GradientDrawable card(Context c, int radiusDp) {
        GradientDrawable d = Ui.rect(0x99100608, Ui.dp(c, radiusDp));
        d.setStroke(Math.max(1, Ui.dp(c, 1)), 0x26FFFFFF);
        return d;
    }
}
