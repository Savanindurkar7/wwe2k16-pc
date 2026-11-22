package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * The frame every launcher screen shares: the dimmed wrestler art and the sidebar (2K16 RECOMP, Home, Graphics,
 * Controls, Game files, Logs, Credits, the game files' status, Settings). The home pages (LauncherActivity) and the
 * settings (OptionsActivity) both draw it, so moving between them looks like one screen: no new menu opens.
 */
final class Shell {
    private Shell() {}

    static final String HOME = "home", FILES = "files", LOGS = "logs", CREDITS = "credits";
    static final String GRAPHICS = "graphics", CONTROLS = "controls", SETTINGS = "settings";
    static final String EXTRA_PAGE = "page";

    /** The background: art (dimmed) and the dark red tint. Returns the art view (the home page changes its picture). */
    static ImageView background(Activity a, FrameLayout root, int artIndex) {
        root.setBackgroundColor(0xFF0A0204);
        ImageView back = new ImageView(a);
        back.setScaleType(ImageView.ScaleType.CENTER_CROP);
        back.setAlpha(0.30f);
        java.util.List<Integer> art = Home.artIds(a);
        if (!art.isEmpty()) {
            android.graphics.Bitmap b = Home.art(a, art.get(artIndex % art.size()), 960);
            if (b != null) back.setImageBitmap(b);
        }
        root.addView(back, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        View tint = new View(a);
        tint.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[] {0xE6160306, 0xB3420810, 0xD90A0203}));
        root.addView(tint, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        return back;
    }

    static int sidebarWidth(Activity a) {
        return Math.max(Ui.dp(a, 200), Math.round(a.getResources().getDisplayMetrics().widthPixels * 0.2f));
    }

    /** What a sidebar entry does on this screen. */
    interface Nav {
        void go(String page);
    }

    static View sidebar(Activity a, String selected, Nav nav) {
        LinearLayout side = new LinearLayout(a);
        side.setOrientation(LinearLayout.VERTICAL);
        side.setPadding(Ui.dp(a, 22), Ui.dp(a, 20), Ui.dp(a, 14), Ui.dp(a, 12));
        side.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {0xFA0C0204, 0xF2160407}));

        LinearLayout title = new LinearLayout(a);
        title.setOrientation(LinearLayout.HORIZONTAL);
        title.setGravity(Gravity.BOTTOM);
        title.addView(Ui.text(a, "2K16", 38, Ui.WHITE, Ui.bold()));
        TextView recomp = Ui.text(a, "RECOMP", 11, Ui.RED, Ui.semi());
        recomp.setLetterSpacing(0.35f);
        recomp.setPadding(Ui.dp(a, 10), 0, 0, Ui.dp(a, 5));
        title.addView(recomp);
        side.addView(title);
        side.addView(rule(a), ruleLp(a, 14, 10));

        LinearLayout list = new LinearLayout(a);
        list.setOrientation(LinearLayout.VERTICAL);
        list.addView(item(a, "⌂", "Home", HOME.equals(selected), v -> nav.go(HOME)));
        list.addView(item(a, "▣", "Graphics", GRAPHICS.equals(selected), v -> nav.go(GRAPHICS)));
        list.addView(item(a, "✚", "Controls", CONTROLS.equals(selected), v -> nav.go(CONTROLS)));
        list.addView(item(a, "▤", "Game files", FILES.equals(selected), v -> nav.go(FILES)));
        list.addView(rule(a), ruleLp(a, 8, 8));
        list.addView(item(a, "≡", "Logs", LOGS.equals(selected), v -> nav.go(LOGS)));
        list.addView(item(a, "★", "Credits", CREDITS.equals(selected), v -> nav.go(CREDITS)));
        ScrollView scroll = new ScrollView(a);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(list);
        side.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        side.addView(statusCard(a));
        side.addView(item(a, "⚙", "Settings", SETTINGS.equals(selected), v -> nav.go(SETTINGS)));
        return side;
    }

    private static View rule(Activity a) {
        View r = new View(a);
        r.setBackgroundColor(0x40FFFFFF);
        return r;
    }

    private static LinearLayout.LayoutParams ruleLp(Activity a, int topDp, int bottomDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, Ui.dp(a, 1.5f)));
        lp.topMargin = Ui.dp(a, topDp);
        lp.bottomMargin = Ui.dp(a, bottomDp);
        return lp;
    }

    private static View item(Activity a, String icon, String label, boolean selected, View.OnClickListener click) {
        LinearLayout item = new LinearLayout(a);
        item.setOrientation(LinearLayout.HORIZONTAL);
        item.setGravity(Gravity.CENTER_VERTICAL);
        item.setPadding(0, Ui.dp(a, 7), Ui.dp(a, 6), Ui.dp(a, 7));
        item.setBackground(Home.pill(a, 0x00000000, 0x26FFFFFF, Ui.dp(a, 8)));
        item.setFocusable(true);
        item.setClickable(true);
        item.setOnClickListener(click);
        if (selected) item.setTag("selected");
        View bar = new View(a);
        bar.setBackground(Ui.rect(selected ? Ui.WHITE : 0x00000000, Ui.dp(a, 2)));
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(Ui.dp(a, 4), Ui.dp(a, 26));
        bl.rightMargin = Ui.dp(a, 12);
        item.addView(bar, bl);
        TextView ic = Ui.text(a, icon, 19, selected ? Ui.WHITE : 0xFFCFCFD4, android.graphics.Typeface.DEFAULT_BOLD);
        ic.setGravity(Gravity.CENTER);
        item.addView(ic, new LinearLayout.LayoutParams(Ui.dp(a, 26), ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView t = Ui.text(a, label, 18, selected ? Ui.WHITE : 0xFFD4D4D8, selected ? Ui.bold() : Ui.regular());
        t.setPadding(Ui.dp(a, 12), 0, 0, 0);
        item.addView(t);
        return item;
    }

    private static View statusCard(Activity a) {
        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(Ui.dp(a, 12), Ui.dp(a, 10), Ui.dp(a, 10), Ui.dp(a, 10));
        card.setBackground(Ui.rect(0x99202024, Ui.dp(a, 10)));
        SharedPreferences prefs = Settings.prefs(a);
        String problem = GameFiles.checkSaved(prefs.getString(Settings.KEY_GAME_PATH, ""),
                prefs.getString(Settings.KEY_GAME_KIND, "folder"));
        boolean ready = problem == null;
        card.addView(Ui.text(a, ready ? "✔  Game files ready" : "✖  Game files needed", 14,
                ready ? Ui.GREEN : Ui.RED, Ui.semi()));
        TextView v = Ui.text(a, BuildInfo.versionName(a), 11, Ui.GREY, Ui.regular());
        v.setPadding(0, Ui.dp(a, 5), 0, 0);
        card.addView(v);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = Ui.dp(a, 6);
        card.setLayoutParams(lp);
        return card;
    }

    /** From the settings: back to a home page (the launcher under it comes to the front with that page). */
    static void toHome(Activity from, String page) {
        Intent i = new Intent(from, LauncherActivity.class);
        i.putExtra(EXTRA_PAGE, page);
        i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        from.startActivity(i);
        from.finish();
        from.overridePendingTransition(0, 0);
    }

    /** From the home: the settings at a section, drawn in the same frame. */
    static void toSettings(Activity from, String section) {
        Intent i = new Intent(from, OptionsActivity.class);
        i.putExtra(OptionsActivity.EXTRA_SECTION, section);
        from.startActivity(i);
        from.overridePendingTransition(0, 0);
    }
}
