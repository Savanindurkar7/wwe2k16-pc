package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.content.Intent;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

/** The credits: the porter's card (home screen > Credits, OPTIONS > CREDITS) and everyone else's work. Edit here. */
final class Credits {
    private Credits() {}

    /** The port's author. */
    static final String PORT_BY = "CATONIX";
    static final String ROLE = "Port of WWE 2K16 to Android";
    static final String YOUTUBE = "https://youtube.com/@catonix-ru7";
    static final String WEBSITE = "http://volvisionmods.com";
    static final String TELEGRAM = "https://t.me/catonixports";
    static final String GITHUB = "github.com/Savanindurkar7";

    /** Everyone whose work is in this app. */
    static final String[][] THANKS = {
            {"Android port", PORT_BY + " (YouTube @catonix-ru7, " + GITHUB + ")"},
            {"WWE 2K16 PC recompilation", "Banshee (the PC port this Android version is built on)"},
            {"ReXGlue SDK", "The recompiler and runtime, with parts from the Xenia project"},
            {"WWE 2K14 recompilation", "Jarrod Feng: the WWE 2K14 SDK and native renderer (native APK), the scene MSAA "
                    + "hook and frame structure work, Android driver loader and SDK fixes (BSD 3-Clause)"},
            {"WWE '12 PC / Android", "iMrBurn: Android audio, timer, UI and thread fixes"},
            {"WWE 2K17 port", "Xav: the game speed fix method (time step from the real frame time)"},
            {"libadrenotools", "bylaws: custom Vulkan driver loading (BSD 2-Clause)"},
            {"Mesa Turnip", "The open Adreno Vulkan driver"},
            {"SDL 3", "Window, input and audio on Android (zlib licence)"},
            {"FFmpeg", "Video and audio decoding (LGPL)"},
            {"Oswald font", "The Oswald Project Authors (SIL Open Font Licence)"},
    };

    static final String DISCLAIMER = "Fan-made and non-commercial. Not affiliated with or endorsed by WWE, 2K, "
            + "Take-Two, Yuke's or Visual Concepts. No game files are included: you need your own WWE 2K16 (Xbox 360).";

    /** The porter's card: avatar, name, role, the three links and a red button for each. */
    static View card(Activity a) {
        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(a, 20);
        card.setPadding(pad, pad, pad, pad);
        GradientDrawable bg = Ui.rect(0xB3050507, Ui.dp(a, 14));
        bg.setStroke(Math.max(1, Ui.dp(a, 1)), 0x40FFFFFF);
        card.setBackground(bg);

        LinearLayout top = new LinearLayout(a);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        int size = Ui.dp(a, 104);
        ImageView avatar = new ImageView(a);
        int id = a.getResources().getIdentifier("credits_avatar", "drawable", a.getPackageName());
        if (id != 0) avatar.setImageResource(id);
        avatar.setScaleType(ImageView.ScaleType.CENTER_CROP);
        GradientDrawable ring = new GradientDrawable();
        ring.setShape(GradientDrawable.OVAL);
        ring.setColor(0xFF9BD110);
        ring.setStroke(Ui.dp(a, 3), 0xFFE9E9EC);
        avatar.setBackground(ring);
        avatar.setPadding(Ui.dp(a, 3), Ui.dp(a, 3), Ui.dp(a, 3), Ui.dp(a, 3));
        avatar.setClipToOutline(true);
        top.addView(avatar, new LinearLayout.LayoutParams(size, size));

        LinearLayout info = new LinearLayout(a);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(Ui.dp(a, 22), 0, 0, 0);
        TextView name = Ui.text(a, PORT_BY, 34, Ui.WHITE, Ui.bold());
        name.setLetterSpacing(0.02f);
        info.addView(name);
        TextView role = Ui.body(a, ROLE, 14, 0xFFE4E4E8);
        role.setPadding(0, Ui.dp(a, 8), 0, Ui.dp(a, 6));
        info.addView(role);
        info.addView(linkLine(a, "YouTube", YOUTUBE));
        info.addView(linkLine(a, "Website", WEBSITE));
        info.addView(linkLine(a, "Telegram", TELEGRAM + "  (updates, help, send logs)"));
        top.addView(info, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(top);

        LinearLayout buttons = new LinearLayout(a);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setPadding(0, Ui.dp(a, 18), 0, 0);
        buttons.addView(redButton(a, "Open YouTube Channel", YOUTUBE), weight(a, 0));
        buttons.addView(redButton(a, "Open Website", WEBSITE), weight(a, 12));
        buttons.addView(redButton(a, "Telegram Channel", TELEGRAM), weight(a, 12));
        card.addView(buttons);
        return card;
    }

    private static LinearLayout.LayoutParams weight(Activity a, int leftDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, Ui.dp(a, 46), 1f);
        lp.leftMargin = Ui.dp(a, leftDp);
        return lp;
    }

    private static TextView linkLine(Activity a, String label, String value) {
        TextView t = Ui.body(a, label + "  " + value, 12.5f, Ui.GREY);
        t.setPadding(0, Ui.dp(a, 3), 0, 0);
        return t;
    }

    static TextView redButton(Activity a, String title, String url) {
        TextView b = Ui.text(a, title, 14, Ui.WHITE, Ui.regular());
        b.setGravity(Gravity.CENTER);
        b.setLetterSpacing(0.03f);
        b.setBackground(Home.pill(a, Ui.RED, 0xFFF0364A, Ui.dp(a, 10)));
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(v -> open(a, url));
        return b;
    }

    static void open(Activity a, String url) {
        try {
            a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            Toast.makeText(a, url, Toast.LENGTH_LONG).show();
        }
    }

    /** Everyone else's work, one row each, then the disclaimer. */
    static void addThanks(Activity a, LinearLayout p) {
        TextView h = Ui.label(a, "Thanks", 15);
        h.setTextColor(Ui.RED);
        h.setPadding(0, Ui.dp(a, 18), 0, Ui.dp(a, 4));
        p.addView(h);
        for (String[] line : THANKS) {
            LinearLayout r = new LinearLayout(a);
            r.setOrientation(LinearLayout.VERTICAL);
            r.setPadding(Ui.dp(a, 14), Ui.dp(a, 9), Ui.dp(a, 14), Ui.dp(a, 9));
            r.setBackground(Ui.rect(0xCC141419, Ui.dp(a, 8)));
            r.addView(Ui.label(a, line[0], 14));
            TextView d = Ui.body(a, line[1], 12.5f, Ui.GREY);
            d.setPadding(0, Ui.dp(a, 3), 0, 0);
            r.addView(d);
            p.addView(r, Ui.fill(Ui.dp(a, 6)));
        }
        TextView disc = Ui.body(a, DISCLAIMER, 11.5f, Ui.DIM);
        disc.setPadding(0, Ui.dp(a, 14), 0, 0);
        p.addView(disc);
    }
}
