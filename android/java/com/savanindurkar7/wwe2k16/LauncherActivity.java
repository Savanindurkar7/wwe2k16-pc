package com.savanindurkar7.wwe2k16;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Map;

/**
 * The home screen: a sidebar (Home, Graphics, Controls, Game files, Logs, Credits, Settings) and the page: the hero card
 * with PLAY and QUICK SETUP, game files, logs or credits, over the wrestler art.
 */
public class LauncherActivity extends Activity {
    private static final int REQ_FOLDER = 11;
    private static final int REQ_ISO = 12;
    private static final int REQ_LEGACY_STORAGE = 13;
    private static final int REQ_DLC = 14;

    private Runnable afterPermission;
    private String page = "home";
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        Ui.init(this);
        Integrity.enforce(this);
        VolvisionUpdateChannel.checkForUpdates(this, BuildInfo.rawVersion(this));
        checkDriverTrial();
        build();
        prepareDrivers();
    }

    /**
     * r17: reads the GPU name once (Gpu) and unpacks the APK's built-in drivers (first start of each APK version), off
     * the UI thread. On an Adreno phone that never picked a driver, the built-in Turnip for its GPU is selected once.
     */
    private void prepareDrivers() {
        new Thread(() -> {
            Gpu.renderer(this);
            // r18: first start only — detect the GPU and apply its best-settings preset once.
            final String presetFamily = Settings.applyPresetOnceForThisGpu(this);
            int unpacked = Drivers.installBundled(this);
            SharedPreferences prefs = Settings.prefs(this);
            boolean chose = false;
            if (!prefs.getBoolean("driver_auto_done", false)) {
                prefs.edit().putBoolean("driver_auto_done", true).apply();
                Drivers.Driver rec = Drivers.recommended(this);
                if (rec != null && prefs.getString(Settings.KEY_DRIVER, "").isEmpty()) {
                    prefs.edit().putString(Settings.KEY_DRIVER, rec.name).apply();
                    chose = true;
                }
            }
            // r19: a built-in Turnip for another Adreno generation is switched to the matching one (an Adreno 825 ran
            // the A6xx-A7xx build). Only built-in ones: a driver the player imported is left alone.
            String corrected = null;
            Drivers.Driver current = Drivers.find(this, prefs.getString(Settings.KEY_DRIVER, ""));
            Drivers.Driver better = Drivers.betterBuiltIn(this, current);
            if (better != null) {
                prefs.edit().putString(Settings.KEY_DRIVER, better.name).apply();
                corrected = better.name;
                current = better;
            }
            // r19: an Adreno phone on the System driver or a Qualcomm driver is offered the built-in Turnip (once per
            // driver choice).
            final String advice = Drivers.adrenoAdvice(this, current);
            final String adviceKey = prefs.getString(Settings.KEY_DRIVER, "");
            final boolean showAdvice = advice != null && !adviceKey.equals(prefs.getString("driver_advice_for", null));
            final Drivers.Driver rec = Drivers.recommended(this);
            final String fixedTo = corrected;
            final boolean selected = chose;
            final String name = selected ? prefs.getString(Settings.KEY_DRIVER, "") : "";
            if (unpacked > 0 || selected || presetFamily != null || fixedTo != null || showAdvice) handler.post(() -> {
                if (presetFamily != null) Toast.makeText(this, "Best settings applied for your "
                        + presetFamily.toUpperCase(java.util.Locale.ROOT) + " GPU (change them in Settings)",
                        Toast.LENGTH_LONG).show();
                if (selected) Toast.makeText(this, "Built-in driver \"" + name + "\" selected for your GPU "
                        + "(Settings > GPU driver)", Toast.LENGTH_LONG).show();
                if (fixedTo != null) Toast.makeText(this, "Switched to \"" + fixedTo + "\": the driver made for your "
                        + "GPU", Toast.LENGTH_LONG).show();
                if (showAdvice && rec != null && !isFinishing()) {
                    prefs.edit().putString("driver_advice_for", adviceKey).apply();
                    new AlertDialog.Builder(this)
                            .setTitle("Faster driver for your GPU")
                            .setMessage(advice)
                            .setPositiveButton("Use " + rec.name, (d, w) -> {
                                prefs.edit().putString(Settings.KEY_DRIVER, rec.name).apply();
                                build();
                            })
                            .setNegativeButton("Keep current", null)
                            .show();
                }
                build();
            });
        }, "prepare-drivers").start();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        build();
        if (afterPermission != null && hasStorageAccess()) {
            Runnable r = afterPermission;
            afterPermission = null;
            r.run();
        }
    }

    @Override
    public void onBackPressed() {
        if (!"home".equals(page)) {
            page = "home";
            build();
            return;
        }
        super.onBackPressed();
    }

    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    // --- Layout ----------------------------------------------------------------------------------------------------
    // Landscape only (manifest). Left: the sidebar (title, pages, the game files' status, Settings). Right: the page
    // over the dimmed wrestler art (BG1.png, BG2.jpg, BG3.jpg, BG34-.jpg from the repository, packed by the build):
    //   home     the hero card (art slideshow, WWE 2K16, chips, PLAY) and QUICK SETUP (Graphics, Controls)
    //   files    game files status, install location (folder / ISO / verify), shader cache
    //   dlc      DLC packages: add (copied in, installed when the game starts), installed / waiting / failed lists
    //   logs     save / share / clear, the last lines of the log
    //   credits  the porter's card with the links, then everyone else's work
    // Graphics, Controls and Settings open OPTIONS at that section.

    private java.util.List<Integer> art;
    private int artIndex;
    private ImageView heroArt, backArt;
    private final Runnable slideshow = new Runnable() {
        @Override
        public void run() {
            if (art == null || art.size() < 2 || heroArt == null) return;
            artIndex = (artIndex + 1) % art.size();
            final int id = art.get(artIndex);
            heroArt.animate().alpha(0f).setDuration(420).withEndAction(() -> {
                setArt(heroArt, id, 1400);
                setArt(backArt, id, 960);
                heroArt.animate().alpha(1f).setDuration(600).start();
                Motion.kenBurns(heroArt, 7600, (artIndex & 1) == 1);  // r19: slow pan and zoom on each slide
            }).start();
            handler.postDelayed(this, 7000);
        }
    };

    @Override
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(slideshow);
        Motion.stopAll();  // r19: no animation runs while the launcher is not on screen
    }

    private void setArt(ImageView v, int id, int maxWidth) {
        if (v == null) return;
        android.graphics.Bitmap b = Home.art(this, id, maxWidth);
        if (b != null) v.setImageBitmap(b);
    }

    private int screenW() { return getResources().getDisplayMetrics().widthPixels; }
    private int screenH() { return getResources().getDisplayMetrics().heightPixels; }

    private String animatedPage;  // r19: the page whose entrance animation already ran

    private void build() {
        handler.removeCallbacks(slideshow);
        Motion.stopAll();
        art = Home.artIds(this);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF0A0204);

        backArt = Shell.background(this, root, artIndex);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        int sideW = Shell.sidebarWidth(this);
        row.addView(sidebar(), new LinearLayout.LayoutParams(sideW, ViewGroup.LayoutParams.MATCH_PARENT));

        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setFillViewport(true);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(this, 22);
        content.setPadding(pad, Ui.dp(this, 14), pad, Ui.dp(this, 16));
        content.addView(topBar());
        if ("files".equals(page)) filesPage(content);
        else if ("dlc".equals(page)) dlcPage(content);
        else if ("logs".equals(page)) logsPage(content);
        else if ("credits".equals(page)) creditsPage(content);
        else homePage(content);
        scroll.addView(content);
        row.addView(scroll, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
        root.addView(row);

        setContentView(root);
        View first = root.findViewWithTag("first");
        if (first != null) first.requestFocus();
        if ("home".equals(page) && art.size() > 1) handler.postDelayed(slideshow, 7000);
        // r19: a page slides in when it is opened (not on every refresh of the same page).
        if (!page.equals(animatedPage)) {
            animatedPage = page;
            Motion.staggerIn(content, 30);
        }
        if ("home".equals(page) && heroArt != null) {
            final ImageView h = heroArt;
            h.post(() -> Motion.kenBurns(h, 7600, (artIndex & 1) == 1));
        }
    }

    // --- Sidebar (Shell, shared with the settings) ---

    private View sidebar() {
        return Shell.sidebar(this, page, target -> {
            switch (target) {
                case Shell.GRAPHICS: openOptions(Settings.S_GRAPHICS); break;
                case Shell.CONTROLS: openOptions(Settings.S_CONTROLS); break;
                case Shell.SETTINGS: openOptions(Settings.S_PERFORMANCE); break;
                default: show(target); break;
            }
        });
    }

    private void show(String p) {
        page = p;
        build();
    }

    private void openOptions(String section) {
        Shell.toSettings(this, section);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String p = intent.getStringExtra(Shell.EXTRA_PAGE);
        if (p != null) page = p;
    }

    private String filesProblem() {
        SharedPreferences prefs = Settings.prefs(this);
        String path = prefs.getString(Settings.KEY_GAME_PATH, "");
        String kind = prefs.getString(Settings.KEY_GAME_KIND, "folder");
        return hasStorageAccess() ? GameFiles.checkSaved(path, kind) : (path.isEmpty()
                ? "Select your game folder or .iso first." : "Allow file access so the game files can be read.");
    }

    // --- Top bar: the controller status ---

    private View topBar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL | Gravity.END);
        boolean pad = gamepadConnected();
        TextView chip = Ui.text(this, (pad ? "●  Gamepad connected" : "●  Touch controls"), 13,
                pad ? Ui.GREEN : 0xFFCFCFD4, Ui.regular());
        chip.setPadding(Ui.dp(this, 14), Ui.dp(this, 7), Ui.dp(this, 14), Ui.dp(this, 7));
        chip.setBackground(Ui.rect(0x33FFFFFF, Ui.dp(this, 18)));
        bar.addView(chip);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = Ui.dp(this, 12);
        bar.setLayoutParams(lp);
        return bar;
    }

    private boolean gamepadConnected() {
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
                    || (s & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK) return true;
        }
        return false;
    }

    // --- Home ---

    private void homePage(LinearLayout content) {
        SharedPreferences prefs = Settings.prefs(this);
        // The hero card.
        FrameLayout hero = new FrameLayout(this);
        hero.setBackground(Ui.rect(0xFF200408, Ui.dp(this, 20)));
        hero.setClipToOutline(true);
        heroArt = new ImageView(this);
        heroArt.setScaleType(ImageView.ScaleType.CENTER_CROP);
        if (!art.isEmpty()) setArt(heroArt, art.get(artIndex % art.size()), 1400);
        hero.addView(heroArt, matchParent());
        View shade = new View(this);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {0xF2120205, 0x99300509, 0x00000000}));
        hero.addView(shade, matchParent());
        View bottomShade = new View(this);
        bottomShade.setBackground(new GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP,
                new int[] {0xCC000000, 0x00000000}));
        hero.addView(bottomShade, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                Ui.dp(this, 90), Gravity.BOTTOM));

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        int hp = Ui.dp(this, 24);
        text.setPadding(hp, hp, hp, Ui.dp(this, 20));
        TextView tag = Ui.text(this, "XBOX 360  →  ANDROID", 11.5f, Ui.WHITE, Ui.semi());
        tag.setLetterSpacing(0.32f);
        tag.setPadding(Ui.dp(this, 14), Ui.dp(this, 6), Ui.dp(this, 14), Ui.dp(this, 6));
        tag.setBackground(Ui.rect(Ui.RED, Ui.dp(this, 5)));
        text.addView(tag, wrap());
        View grow = new View(this);
        text.addView(grow, new LinearLayout.LayoutParams(1, 0, 1f));
        TextView name = Ui.text(this, "WWE 2K16", 46, Ui.WHITE, Ui.bold());
        name.setShadowLayer(12, 0, 3, 0xAA000000);
        text.addView(name);
        LinearLayout chips = new LinearLayout(this);
        chips.setOrientation(LinearLayout.HORIZONTAL);
        chips.setPadding(0, Ui.dp(this, 10), 0, 0);
        String renderer = Settings.isNativeBuild(this)
                ? Settings.RENDERER.labels[Settings.RENDERER.index(prefs)] : "Classic";
        String fps = "on".equals(Settings.FRAME_GEN.value(prefs)) && Settings.isNativeBuild(this) ? "30 → 60 FG"
                : Settings.FRAME_RATE.labels[Settings.FRAME_RATE.index(prefs)].replace(" (STEADY)", "");
        String version = "v" + BuildInfo.rawVersion(this);
        // r19: a small screen gets the essential chips only, so the row never runs off the card.
        String[] chipText = Ui.scale < 0.86f ? new String[] {renderer, fps, version}
                : new String[] {"Recomp", "Vulkan", renderer, fps, version};
        for (int i = 0; i < chipText.length; i++) {
            LinearLayout.LayoutParams cl = wrap();
            if (i > 0) cl.leftMargin = Ui.dp(this, 8);
            chips.addView(Home.chip(this, chipText[i]), cl);
        }
        text.addView(chips);
        hero.addView(text, matchParent());

        TextView play = Ui.text(this, "▶   Play", 24, Ui.WHITE, Ui.bold());
        play.setGravity(Gravity.CENTER);
        play.setPadding(Ui.dp(this, 34), Ui.dp(this, 12), Ui.dp(this, 38), Ui.dp(this, 12));
        play.setBackground(Home.pill(this, Ui.RED, 0xFFF0364A, Ui.dp(this, 14)));
        play.setElevation(Ui.dp(this, 6));
        play.setFocusable(true);
        play.setClickable(true);
        play.setTag("first");
        play.setOnClickListener(v -> play());
        // r19: PLAY breathes, with a soft red halo pulsing behind it (the game is ready).
        FrameLayout playBox = new FrameLayout(this);
        View halo = new View(this);
        halo.setBackground(Ui.rect(0xAAE01433, Ui.dp(this, 16)));
        playBox.addView(halo, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        playBox.addView(play, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        playBox.setClipChildren(false);
        hero.setClipChildren(false);
        FrameLayout.LayoutParams pl = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.END | Gravity.BOTTOM);
        pl.rightMargin = Ui.dp(this, 22);
        pl.bottomMargin = Ui.dp(this, 20);
        hero.addView(playBox, pl);
        if (filesProblem() == null) {
            Motion.halo(halo, 1900);
            Motion.breathe(play, 0.035f, 2400);
        } else {
            halo.setAlpha(0f);
        }
        content.addView(hero, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                Math.max(Ui.dp(this, 200), Math.round(screenH() * 0.50f))));

        // QUICK SETUP.
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.setPadding(0, Ui.dp(this, 16), 0, Ui.dp(this, 10));
        TextView qs = Ui.text(this, "QUICK SETUP", 18, Ui.WHITE, Ui.bold());
        qs.setLetterSpacing(0.06f);
        head.addView(qs, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView all = Ui.text(this, "View all", 16, Ui.WHITE, Ui.semi());
        all.setPadding(Ui.dp(this, 10), Ui.dp(this, 4), Ui.dp(this, 4), Ui.dp(this, 4));
        all.setFocusable(true);
        all.setClickable(true);
        all.setOnClickListener(v -> openOptions(Settings.S_GRAPHICS));
        head.addView(all);
        content.addView(head);

        LinearLayout cards = new LinearLayout(this);
        cards.setOrientation(LinearLayout.HORIZONTAL);
        String res = Settings.RESOLUTION.labels[Settings.RESOLUTION.index(prefs)];
        String crowd = "Crowd " + Settings.CROWD.labels[Settings.CROWD.index(prefs)];
        cards.addView(setupCard("Graphics", new String[] {renderer, res, crowd}, "Configure ›", 1,
                v -> openOptions(Settings.S_GRAPHICS)), new LinearLayout.LayoutParams(0, Ui.dp(this, 128), 1f));
        LinearLayout.LayoutParams second = new LinearLayout.LayoutParams(0, Ui.dp(this, 128), 1f);
        second.leftMargin = Ui.dp(this, 16);
        boolean pad = gamepadConnected();
        cards.addView(setupCard("Controls", new String[] {"Touch", pad ? "Gamepad ✔" : "Gamepad"}, "Edit layout ›", 2,
                v -> startActivity(new Intent(this, TouchEditActivity.class))), second);
        content.addView(cards);

        // r19: support the port (Buy Me a Coffee), animated.
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        sl.topMargin = Ui.dp(this, 16);
        content.addView(Support.card(this, false), sl);
    }

    private View setupCard(String title, String[] chipText, String action, int artSlot, View.OnClickListener click) {
        FrameLayout card = new FrameLayout(this);
        card.setBackground(Ui.rect(0xFF1A0507, Ui.dp(this, 18)));
        card.setClipToOutline(true);
        if (art.size() > 0) {
            ImageView img = new ImageView(this);
            img.setScaleType(ImageView.ScaleType.CENTER_CROP);
            setArt(img, art.get(artSlot % art.size()), 900);
            img.setAlpha(0.55f);
            card.addView(img, matchParent());
        }
        View shade = new View(this);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {0xE6140306, 0x80140306}));
        card.addView(shade, matchParent());
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.BOTTOM);
        int p = Ui.dp(this, 18);
        col.setPadding(p, p, p, p);
        col.addView(Ui.text(this, title, 28, Ui.WHITE, Ui.bold()));
        LinearLayout chips = new LinearLayout(this);
        chips.setOrientation(LinearLayout.HORIZONTAL);
        chips.setPadding(0, Ui.dp(this, 10), 0, 0);
        for (int i = 0; i < chipText.length; i++) {
            LinearLayout.LayoutParams cl = wrap();
            if (i > 0) cl.leftMargin = Ui.dp(this, 7);
            chips.addView(Home.chip(this, chipText[i]), cl);
        }
        col.addView(chips);
        card.addView(col, matchParent());
        TextView b = Ui.text(this, action, 15, Ui.WHITE, Ui.semi());
        b.setPadding(Ui.dp(this, 16), Ui.dp(this, 8), Ui.dp(this, 16), Ui.dp(this, 8));
        b.setBackground(Home.outline(this, Ui.dp(this, 12)));
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        FrameLayout.LayoutParams bl = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.END | Gravity.BOTTOM);
        bl.rightMargin = Ui.dp(this, 14);
        bl.bottomMargin = Ui.dp(this, 14);
        card.addView(b, bl);
        card.setOnClickListener(click);
        return card;
    }

    // --- Page banners and cards ---

    private View banner(String title, String subtitle, int artSlot) {
        FrameLayout b = new FrameLayout(this);
        b.setBackground(Ui.rect(0xFF140306, Ui.dp(this, 18)));
        b.setClipToOutline(true);
        if (art.size() > 0) {
            ImageView img = new ImageView(this);
            img.setScaleType(ImageView.ScaleType.CENTER_CROP);
            setArt(img, art.get(artSlot % art.size()), 1200);
            card(b, img);
        }
        View shade = new View(this);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {0xFA0E0204, 0xCC1A0306, 0x33000000}));
        b.addView(shade, matchParent());
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_VERTICAL);
        col.setPadding(Ui.dp(this, 24), Ui.dp(this, 14), Ui.dp(this, 24), Ui.dp(this, 14));
        col.addView(Ui.text(this, title, 34, Ui.WHITE, Ui.bold()));
        TextView sub = Ui.body(this, subtitle, 15, 0xFFE6E6EA);
        sub.setPadding(0, Ui.dp(this, 6), 0, 0);
        col.addView(sub);
        b.addView(col, matchParent());
        b.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 104)));
        return b;
    }

    private void card(FrameLayout parent, ImageView img) {
        img.setAlpha(0.8f);
        parent.addView(img, matchParent());
    }

    private LinearLayout panel() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(this, 20);
        p.setPadding(pad, Ui.dp(this, 16), pad, Ui.dp(this, 18));
        p.setBackground(Home.card(this, 18));
        return p;
    }

    private TextView redAction(String title, View.OnClickListener click) {
        TextView b = Ui.text(this, title, 16, Ui.WHITE, Ui.bold());
        b.setGravity(Gravity.CENTER);
        b.setPadding(Ui.dp(this, 20), Ui.dp(this, 10), Ui.dp(this, 20), Ui.dp(this, 10));
        b.setBackground(Home.pill(this, Ui.RED, 0xFFF0364A, Ui.dp(this, 12)));
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        return b;
    }

    private TextView outlineAction(String title, View.OnClickListener click) {
        TextView b = Ui.text(this, title, 16, Ui.WHITE, Ui.bold());
        b.setGravity(Gravity.CENTER);
        b.setPadding(Ui.dp(this, 20), Ui.dp(this, 10), Ui.dp(this, 20), Ui.dp(this, 10));
        b.setBackground(Home.outline(this, Ui.dp(this, 12)));
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        return b;
    }

    private LinearLayout.LayoutParams gapTop(int dp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = Ui.dp(this, dp);
        return lp;
    }

    private View infoColumn(String label, String value, TextView[] out) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.addView(Ui.text(this, label, 13, Ui.GREY, Ui.regular()));
        TextView v = Ui.text(this, value, 18, Ui.WHITE, Ui.bold());
        v.setPadding(0, Ui.dp(this, 6), Ui.dp(this, 14), 0);
        v.setMaxLines(2);
        v.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        c.addView(v);
        if (out != null) out[0] = v;
        return c;
    }

    // --- Game files ---

    private void filesPage(LinearLayout content) {
        content.addView(banner("Game files", "Install location and verification", 0));
        SharedPreferences prefs = Settings.prefs(this);
        String path = prefs.getString(Settings.KEY_GAME_PATH, "");
        String kind = prefs.getString(Settings.KEY_GAME_KIND, "folder");
        String problem = filesProblem();
        boolean ready = problem == null;

        LinearLayout status = panel();
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView mark = Ui.text(this, ready ? "✔" : "✖", 18, Ui.WHITE, android.graphics.Typeface.DEFAULT_BOLD);
        mark.setGravity(Gravity.CENTER);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(ready ? 0xFF1F8A4C : Ui.RED_DARK);
        mark.setBackground(dot);
        head.addView(mark, new LinearLayout.LayoutParams(Ui.dp(this, 34), Ui.dp(this, 34)));
        TextView ht = Ui.text(this, ready ? "Game files verified" : "Game files needed", 22, Ui.WHITE, Ui.bold());
        ht.setPadding(Ui.dp(this, 14), 0, 0, 0);
        head.addView(ht);
        status.addView(head);
        if (!ready) {
            TextView why = Ui.body(this, problem, 14, Ui.GREY);
            why.setPadding(0, Ui.dp(this, 10), 0, 0);
            status.addView(why);
        }
        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.HORIZONTAL);
        info.setPadding(0, Ui.dp(this, 16), 0, 0);
        TextView[] sizeView = new TextView[1];
        info.addView(infoColumn("Folder", path.isEmpty() ? "Not selected" : path, null),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2.2f));
        info.addView(infoColumn("Size", path.isEmpty() ? "-" : "…", sizeView),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        info.addView(infoColumn("Type", "iso".equals(kind) ? "ISO image" : "Extracted folder", null),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.2f));
        info.addView(infoColumn("Version", "Xbox 360", null),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        status.addView(info);
        content.addView(status, gapTop(14));
        if (!path.isEmpty() && hasStorageAccess()) {
            new Thread(() -> {
                long bytes = sizeOf(new File(path), 0);
                String text = bytes <= 0 ? "-" : String.format(java.util.Locale.US, "%.1f GB", bytes / 1073741824.0);
                handler.post(() -> { if (sizeView[0] != null) sizeView[0].setText(text); });
            }, "game-size").start();
        }

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout install = panel();
        install.addView(Ui.text(this, "Install location", 21, Ui.WHITE, Ui.bold()));
        TextView isub = Ui.body(this, "Point the launcher at the extracted game folder or an .iso", 14, Ui.GREY);
        isub.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 14));
        install.addView(isub);
        LinearLayout ib = new LinearLayout(this);
        ib.setOrientation(LinearLayout.HORIZONTAL);
        ib.addView(redAction("Change folder", v -> withStorageAccess(this::pickFolder)), wrap());
        LinearLayout.LayoutParams l2 = wrap();
        l2.leftMargin = Ui.dp(this, 10);
        ib.addView(outlineAction("Select ISO", v -> withStorageAccess(this::pickIso)), l2);
        LinearLayout.LayoutParams l3 = wrap();
        l3.leftMargin = Ui.dp(this, 10);
        ib.addView(outlineAction("Verify again", v -> {
            String pr = filesProblem();
            Toast.makeText(this, pr == null ? "Game files verified" : pr, Toast.LENGTH_LONG).show();
            build();
        }), l3);
        HorizontalScrollView ibs = new HorizontalScrollView(this);
        ibs.setHorizontalScrollBarEnabled(false);
        ibs.addView(ib);
        install.addView(ibs);
        row.addView(install, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.25f));

        LinearLayout shader = panel();
        shader.addView(Ui.text(this, "Shader cache", 21, Ui.WHITE, Ui.bold()));
        long cache = sizeOf(new File(Settings.userDataDir(this), "cache"), 0) + sizeOf(new File(getFilesDir(), ".cache"), 0);
        TextView ssub = Ui.body(this, String.format(java.util.Locale.US, "%.0f MB · rebuilds on the next launch if cleared",
                cache / 1048576.0), 14, Ui.GREY);
        ssub.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 14));
        shader.addView(ssub);
        shader.addView(outlineAction("Clear cache", v -> {
            Settings.clearShaderCaches(this);
            Toast.makeText(this, "Shader cache cleared: the first match compiles them again", Toast.LENGTH_LONG).show();
            build();
        }), wrap());
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        sl.leftMargin = Ui.dp(this, 16);
        row.addView(shader, sl);
        content.addView(row, gapTop(16));
    }

    private static long sizeOf(File f, int depth) {
        if (f == null || !f.exists() || depth > 12) return 0;
        if (f.isFile()) return f.length();
        long total = 0;
        File[] list = f.listFiles();
        if (list != null) for (File c : list) total += sizeOf(c, depth + 1);
        return total;
    }

    // --- DLC (r17) ---

    private void dlcPage(LinearLayout content) {
        content.addView(banner("DLC", "Add Xbox 360 DLC packages: they install when you press PLAY", 2));
        LinearLayout p = panel();
        TextView how = Ui.body(this, "Pick the DLC files themselves (Xbox 360 content packages: LIVE / PIRS / CON, "
                + "usually a long hex name with no extension, from a console's Content/0000000000000000/" + Dlc.TITLE_ID
                + "/00000002 folder or a backup). They are copied into the app and unpacked the next time the game "
                + "starts (that start takes a little longer). Only DLC for WWE 2K16 (" + Dlc.TITLE_ID + ") works.",
                13, 0xFFE0E0E4);
        p.addView(how);
        LinearLayout b = new LinearLayout(this);
        b.setOrientation(LinearLayout.HORIZONTAL);
        TextView add = redAction("Add DLC files", v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            startActivityForResult(i, REQ_DLC);
        });
        add.setTag("first");
        b.addView(add, wrap());
        p.addView(b, gapTop(12));
        content.addView(p, gapTop(14));

        java.util.List<Dlc.Item> waiting = Dlc.waiting(this);
        java.util.List<Dlc.Item> installed = Dlc.installed(this);
        java.util.List<String> results = Dlc.results(this);
        if (!waiting.isEmpty()) {
            LinearLayout w = panel();
            w.addView(Ui.text(this, "WAITING · installs at the next PLAY", 15, Ui.WHITE, Ui.bold()));
            for (Dlc.Item it : waiting) w.addView(dlcRow(it, "Remove", () -> Dlc.delete(this, it)), gapTop(8));
            content.addView(w, gapTop(12));
        }
        LinearLayout inst = panel();
        inst.addView(Ui.text(this, "INSTALLED (" + installed.size() + ")", 15, Ui.WHITE, Ui.bold()));
        if (installed.isEmpty()) {
            inst.addView(Ui.body(this, "No DLC installed yet.", 13, Ui.GREY), gapTop(6));
        }
        for (Dlc.Item it : installed) {
            inst.addView(dlcRow(it, "Delete", () -> Dlc.delete(this, it)), gapTop(8));
        }
        content.addView(inst, gapTop(12));
        if (!results.isEmpty()) {
            LinearLayout r = panel();
            r.addView(Ui.text(this, "LAST INSTALLS", 15, Ui.WHITE, Ui.bold()));
            StringBuilder sb = new StringBuilder();
            for (String line : results) sb.append(line).append('\n');
            TextView t = Ui.body(this, sb.toString().trim(), 11.5f, Ui.GREY);
            t.setTypeface(android.graphics.Typeface.MONOSPACE);
            r.addView(t, gapTop(6));
            LinearLayout.LayoutParams lp = wrap();
            lp.topMargin = Ui.dp(this, 10);
            r.addView(outlineAction("Clear list", v -> {
                Dlc.clearResults(this);
                build();
            }), lp);
            content.addView(r, gapTop(12));
        }
    }

    private View dlcRow(Dlc.Item it, String action, Runnable remove) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 8), Ui.dp(this, 8));
        row.setBackground(Ui.rect(0x66000000, Ui.dp(this, 10)));
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        TextView title = Ui.text(this, it.title, 15, Ui.WHITE, Ui.semi());
        title.setMaxLines(1);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        texts.addView(title);
        TextView sub = Ui.text(this, (it.title.equals(it.name) ? "" : it.name + " · ") + Dlc.formatSize(it.bytes), 11.5f,
                Ui.GREY, Ui.regular());
        sub.setMaxLines(1);
        sub.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        texts.addView(sub);
        row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(Ui.smallButton(this, action, false, v -> new AlertDialog.Builder(this)
                .setTitle(action + " " + it.title + "?")
                .setPositiveButton(action, (d, w) -> {
                    remove.run();
                    build();
                })
                .setNegativeButton("Cancel", null)
                .show()));
        return row;
    }

    private void addDlc(java.util.List<Uri> uris) {
        Toast.makeText(this, "Copying " + uris.size() + " DLC file(s)…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            int ok = 0;
            StringBuilder errors = new StringBuilder();
            for (Uri u : uris) {
                try {
                    Dlc.add(this, u);
                    ok++;
                } catch (IOException e) {
                    errors.append(e.getMessage()).append('\n');
                }
            }
            final int added = ok;
            final String err = errors.toString().trim();
            handler.post(() -> {
                if (added > 0) Toast.makeText(this, added + " DLC file(s) added: they install when you press PLAY",
                        Toast.LENGTH_LONG).show();
                if (!err.isEmpty()) new AlertDialog.Builder(this).setTitle("Some files were not added")
                        .setMessage(err).setPositiveButton("OK", null).show();
                build();
            });
        }, "add-dlc").start();
    }

    // --- Logs ---

    private void logsPage(LinearLayout content) {
        content.addView(banner("Logs", "Logs are saved in Download/" + Logs.FOLDER + " · send them with your report", 1));
        LinearLayout p = panel();
        LinearLayout b = new LinearLayout(this);
        b.setOrientation(LinearLayout.HORIZONTAL);
        TextView save = redAction("Save logs", v -> saveLogs(false));
        save.setTag("first");
        b.addView(save, wrap());
        LinearLayout.LayoutParams l2 = wrap();
        l2.leftMargin = Ui.dp(this, 10);
        b.addView(outlineAction("Share logs", v -> saveLogs(true)), l2);
        LinearLayout.LayoutParams l3 = wrap();
        l3.leftMargin = Ui.dp(this, 10);
        b.addView(outlineAction("Clear logs", v -> {
            Logs.clear(this);
            Toast.makeText(this, "Logs cleared", Toast.LENGTH_SHORT).show();
            build();
        }), l3);
        p.addView(b);
        String tail = Logs.tail(this, 10);
        TextView last = Ui.body(this, tail.isEmpty() ? "No game log yet." : tail, 10.5f, Ui.GREY);
        last.setTypeface(android.graphics.Typeface.MONOSPACE);
        last.setPadding(Ui.dp(this, 12), Ui.dp(this, 10), Ui.dp(this, 12), Ui.dp(this, 10));
        last.setBackground(Ui.rect(0xB3000000, Ui.dp(this, 10)));
        p.addView(last, gapTop(14));
        content.addView(p, gapTop(14));
    }

    // --- Credits ---

    private void creditsPage(LinearLayout content) {
        TextView h = Ui.text(this, "CREDITS", 28, Ui.WHITE, Ui.bold());
        h.setLetterSpacing(0.04f);
        content.addView(h);
        content.addView(Credits.card(this), gapTop(14));
        content.addView(Support.card(this, true), gapTop(14));  // r19
        LinearLayout thanks = new LinearLayout(this);
        thanks.setOrientation(LinearLayout.VERTICAL);
        Credits.addThanks(this, thanks);
        content.addView(thanks);
    }

    private FrameLayout.LayoutParams matchParent() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    // --- Actions ---------------------------------------------------------------------------------------------------

    private void saveLogs(boolean share) {
        Toast.makeText(this, "Saving logs…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try {
                Uri uri = Logs.saveZip(this);
                handler.post(() -> {
                    Toast.makeText(this, "Saved in Download/" + Logs.FOLDER, Toast.LENGTH_LONG).show();
                    if (share) {
                        Intent send = new Intent(Intent.ACTION_SEND);
                        send.setType("application/zip");
                        send.putExtra(Intent.EXTRA_STREAM, uri);
                        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        startActivity(Intent.createChooser(send, "Share WWE 2K16 logs"));
                    }
                });
            } catch (IOException e) {
                handler.post(() -> Toast.makeText(this, "Could not save the logs: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "save-logs").start();
    }

    private void play() {
        withStorageAccess(() -> {
            SharedPreferences prefs = Settings.prefs(this);
            String problem = GameFiles.checkSaved(prefs.getString(Settings.KEY_GAME_PATH, ""),
                    prefs.getString(Settings.KEY_GAME_KIND, "folder"));
            if (problem != null) {
                Toast.makeText(this, problem, Toast.LENGTH_LONG).show();
                return;
            }
            Logs.rotate(this);
            Settings.Launch launch = Settings.buildLaunch(this);
            StringBuilder summary = new StringBuilder(launch.summary);
            summary.append("\nArguments:\n");
            for (String a : launch.args) summary.append("  ").append(a).append('\n');
            summary.append("Environment:\n");
            for (Map.Entry<String, String> e : launch.env.entrySet())
                summary.append("  ").append(e.getKey()).append('=').append(e.getValue()).append('\n');
            Logs.writeSettings(this, summary.toString());
            if (launch.env.containsKey("WWE2K16_DRIVER")) writeTrial(prefs.getString(Settings.KEY_DRIVER, ""));

            Intent game = new Intent(this, GameActivity.class);
            game.putStringArrayListExtra(GameActivity.EXTRA_ARGS, launch.args);
            ArrayList<String> env = new ArrayList<>();
            for (Map.Entry<String, String> e : launch.env.entrySet()) env.add(e.getKey() + "=" + e.getValue());
            env.add("WWE2K16_SUSTAINED=" + Settings.SUSTAINED.value(prefs));
            env.add("WWE2K16_DISPLAY=" + Settings.DISPLAY.value(prefs));
            game.putStringArrayListExtra(GameActivity.EXTRA_ENV, env);
            startActivity(game);
        });
    }

    private void writeTrial(String driver) {
        try (FileOutputStream out = new FileOutputStream(Drivers.trialFile(this))) {
            out.write(driver.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    /** A trial file still there means the last run with a custom driver never drew 600 frames: back to the phone's. */
    private void checkDriverTrial() {
        File trial = Drivers.trialFile(this);
        if (!trial.isFile()) return;
        //noinspection ResultOfMethodCallIgnored
        trial.delete();
        SharedPreferences prefs = Settings.prefs(this);
        String driver = prefs.getString(Settings.KEY_DRIVER, "");
        if (driver.isEmpty()) return;
        prefs.edit().putString(Settings.KEY_DRIVER, "").apply();
        Toast.makeText(this, "The last run with the driver \"" + driver + "\" ended before the game got going, so the "
                + "launcher switched back to the phone's driver. Pick it again in Options to retry.", Toast.LENGTH_LONG).show();
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, REQ_FOLDER);
    }

    private void pickIso() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_ISO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_DLC && resultCode == RESULT_OK && data != null) {
            java.util.List<Uri> uris = new java.util.ArrayList<>();
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            if (!uris.isEmpty()) addDlc(uris);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        String path, problem, kind;
        if (requestCode == REQ_FOLDER) {
            path = GameFiles.treeUriToPath(this, uri);
            problem = GameFiles.checkFolder(path);
            kind = "folder";
        } else if (requestCode == REQ_ISO) {
            path = GameFiles.documentUriToPath(this, uri);
            problem = GameFiles.checkIso(path);
            kind = "iso";
        } else {
            return;
        }
        if (problem != null) {
            Toast.makeText(this, problem, Toast.LENGTH_LONG).show();
            return;
        }
        Settings.prefs(this).edit().putString(Settings.KEY_GAME_PATH, path).putString(Settings.KEY_GAME_KIND, kind).apply();
        Toast.makeText(this, "Game files ready", Toast.LENGTH_SHORT).show();
        build();
    }

    // --- Storage access --------------------------------------------------------------------------------------------

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** The game reads its files by path, which needs "All files access" (Android 11+) or storage permission (10). */
    private void withStorageAccess(Runnable action) {
        if (hasStorageAccess()) {
            action.run();
            return;
        }
        afterPermission = action;
        if (Build.VERSION.SDK_INT >= 30) {
            Toast.makeText(this, "Allow \"All files access\" so the game can read its files, then come back.",
                    Toast.LENGTH_LONG).show();
            try {
                startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE}, REQ_LEGACY_STORAGE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_LEGACY_STORAGE && hasStorageAccess() && afterPermission != null) {
            Runnable r = afterPermission;
            afterPermission = null;
            r.run();
        }
    }
}
