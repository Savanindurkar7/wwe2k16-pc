package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The launcher's look, built in code: black panels, red accents, white condensed type (Oswald), in the spirit of the
 * game's red-and-black menus. Every control is focusable so a gamepad can drive the launcher.
 */
final class Ui {
    static final int RED = 0xFFD7192D;
    static final int RED_DARK = 0xFF8E0F1D;
    static final int GREEN = 0xFF2FBF71;
    static final int WHITE = 0xFFFFFFFF;
    static final int GREY = 0xFFB9BCC2;
    static final int DIM = 0xFF7D828B;
    static final int PANEL = 0xD90B0B0E;
    static final int PANEL_SOLID = 0xFF0E0E12;
    static final int ROW = 0xFF17171C;
    static final int ROW_FOCUS = 0xFF26262E;

    private static Typeface bold, semi, regular;

    private Ui() {}

    static void init(Context c) {
        if (bold != null) return;
        bold = load(c, R.font.oswald_bold, Typeface.DEFAULT_BOLD);
        semi = load(c, R.font.oswald_semibold, Typeface.DEFAULT_BOLD);
        regular = load(c, R.font.oswald_regular, Typeface.DEFAULT);
    }

    private static Typeface load(Context c, int id, Typeface fallback) {
        try {
            Typeface t = c.getResources().getFont(id);
            return t != null ? t : fallback;
        } catch (Exception e) {
            return fallback;
        }
    }

    static Typeface bold() { return bold; }
    static Typeface semi() { return semi; }
    static Typeface regular() { return regular; }

    static int dp(Context c, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics()));
    }

    static TextView text(Context c, String s, float sp, int color, Typeface face) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(face);
        t.setIncludeFontPadding(false);
        return t;
    }

    static TextView label(Context c, String s, float sp) {
        TextView t = text(c, s.toUpperCase(java.util.Locale.ROOT), sp, WHITE, semi);
        t.setLetterSpacing(0.04f);
        return t;
    }

    static TextView body(Context c, String s, float sp, int color) {
        TextView t = text(c, s, sp, color, regular);
        t.setLineSpacing(0, 1.12f);
        return t;
    }

    static GradientDrawable rect(int color, float radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }

    /** Background for a menu button: a red bar on the left, highlighted when focused or pressed. */
    static Drawable menuBackground(Context c, boolean primary) {
        int base = primary ? RED : ROW;
        int focus = primary ? 0xFFF02A3F : ROW_FOCUS;
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {android.R.attr.state_focused}, withBar(c, focus, primary));
        states.addState(new int[] {android.R.attr.state_pressed}, withBar(c, focus, primary));
        states.addState(new int[] {}, withBar(c, base, primary));
        return new RippleDrawable(ColorStateList.valueOf(0x55FFFFFF), states, null);
    }

    private static Drawable withBar(Context c, int color, boolean primary) {
        GradientDrawable fill = rect(color, dp(c, 3));
        if (primary) return fill;
        GradientDrawable bar = rect(RED, 0);
        LayerDrawable layers = new LayerDrawable(new Drawable[] {fill, bar});
        layers.setLayerWidth(1, dp(c, 5));
        layers.setLayerGravity(1, Gravity.START | Gravity.FILL_VERTICAL);
        return layers;
    }

    /** A big menu entry: title and a one-line hint under it. */
    static LinearLayout menuButton(Context c, String title, String hint, boolean primary, View.OnClickListener click) {
        LinearLayout b = new LinearLayout(c);
        b.setOrientation(LinearLayout.VERTICAL);
        b.setGravity(Gravity.CENTER_VERTICAL);
        int padH = dp(c, primary ? 22 : 20);
        b.setPadding(padH, dp(c, primary ? 14 : 11), padH, dp(c, primary ? 14 : 11));
        b.setBackground(menuBackground(c, primary));
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        TextView t = text(c, title.toUpperCase(java.util.Locale.ROOT), primary ? 30 : 19, WHITE, primary ? bold : semi);
        t.setLetterSpacing(0.05f);
        b.addView(t);
        if (hint != null && !hint.isEmpty()) {
            TextView h = body(c, hint, 12.5f, primary ? 0xFFFFE3E6 : GREY);
            h.setPadding(0, dp(c, 3), 0, 0);
            b.addView(h);
        }
        return b;
    }

    /** A small flat button (dialog-style actions). */
    static TextView smallButton(Context c, String title, boolean primary, View.OnClickListener click) {
        TextView t = text(c, title.toUpperCase(java.util.Locale.ROOT), 15, WHITE, semi);
        t.setLetterSpacing(0.05f);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(c, 16), dp(c, 10), dp(c, 16), dp(c, 10));
        t.setBackground(menuBackground(c, primary));
        t.setFocusable(true);
        t.setClickable(true);
        t.setOnClickListener(click);
        return t;
    }

    static View divider(Context c) {
        View v = new View(c);
        v.setBackground(new ColorDrawable(0x33FFFFFF));
        v.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(c, 1))));
        return v;
    }

    static LinearLayout.LayoutParams fill(int topMarginPx) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = topMarginPx;
        return lp;
    }

    static int alpha(int color, float a) {
        return Color.argb(Math.round(255 * a), Color.red(color), Color.green(color), Color.blue(color));
    }
}
