package com.savanindurkar7.wwe2k16;

import android.Manifest;
import android.app.Activity;
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
 * The home screen: PLAY, SELECT EXTRACTED GAME FOLDER, SELECT ISO, OPTIONS and LOGS over the background art, with the
 * game files' status. Landscape puts the menu in a panel on the left; portrait puts it under the picture.
 */
public class LauncherActivity extends Activity {
    private static final int REQ_FOLDER = 11;
    private static final int REQ_ISO = 12;
    private static final int REQ_LEGACY_STORAGE = 13;

    private Runnable afterPermission;
    private String page = "home";
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        Ui.init(this);
        checkDriverTrial();
        build();
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
    // Landscape only (manifest): the background fills the screen; the menu sits top-left, the tip and the game files'
    // status bottom-left, the credits bottom-right.

    private void build() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);

        // Background art (BG.jpg from the repository, packed by the build); a dark red gradient if it is missing.
        ImageView art = new ImageView(this);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        int bg = getResources().getIdentifier("home_bg", "drawable", getPackageName());
        if (bg != 0) art.setImageResource(bg);
        else art.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {0xFF3A0006, 0xFF000000}));
        root.addView(art, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // Shades so the menu (left) and the text lines (bottom) read over any picture.
        View left = new View(this);
        left.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {0xCC000000, 0x55000000, 0x00000000}));
        root.addView(left, new FrameLayout.LayoutParams(Math.round(getResources().getDisplayMetrics().widthPixels * 0.5f),
                ViewGroup.LayoutParams.MATCH_PARENT, Gravity.START));
        View bottom = new View(this);
        bottom.setBackground(new GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP, new int[] {0xE6000000, 0x00000000}));
        root.addView(bottom, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 120), Gravity.BOTTOM));

        int margin = Ui.dp(this, 22);
        LinearLayout menu = "logs".equals(page) ? logsMenu() : homeMenu();
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(menu);
        FrameLayout.LayoutParams menuLp = new FrameLayout.LayoutParams(
                Math.max(Ui.dp(this, 260), Math.round(getResources().getDisplayMetrics().widthPixels * 0.25f)),
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.START | Gravity.TOP);
        menuLp.leftMargin = margin;
        menuLp.topMargin = Ui.dp(this, 16);
        menuLp.bottomMargin = Ui.dp(this, 70);
        root.addView(scroll, menuLp);

        root.addView(statusLines(), bottomLeft(margin));
        if ("home".equals(page)) root.addView(creditsBlock(), bottomRight(margin));
        else root.addView(logsInfo(), bottomRight(margin));

        setContentView(root);
        View first = menu.findViewWithTag("first");
        if (first != null) first.requestFocus();
    }

    private FrameLayout.LayoutParams bottomLeft(int margin) {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.round(getResources().getDisplayMetrics().widthPixels * 0.55f),
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.START | Gravity.BOTTOM);
        lp.leftMargin = margin;
        lp.bottomMargin = Ui.dp(this, 14);
        return lp;
    }

    private FrameLayout.LayoutParams bottomRight(int margin) {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.END | Gravity.BOTTOM);
        lp.rightMargin = margin;
        lp.bottomMargin = Ui.dp(this, 14);
        return lp;
    }

    /** A home-screen entry in the game's style: dark, outlined, a red edge; PLAY is red. */
    private TextView entry(String title, boolean primary, View.OnClickListener click) {
        TextView t = Ui.text(this, title.toUpperCase(java.util.Locale.ROOT), 25, Ui.WHITE, Ui.bold());
        t.setLetterSpacing(0.03f);
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setPadding(Ui.dp(this, 24), Ui.dp(this, 9), Ui.dp(this, 16), Ui.dp(this, 9));
        t.setBackground(Ui.entryBackground(this, primary));
        t.setFocusable(true);
        t.setClickable(true);
        t.setOnClickListener(click);
        return t;
    }

    private LinearLayout homeMenu() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        int gap = Ui.dp(this, 9);
        View play = entry("Play", true, v -> play());
        play.setTag("first");
        p.addView(play, Ui.fill(0));
        p.addView(entry("Select folder", false, v -> withStorageAccess(this::pickFolder)), Ui.fill(gap));
        p.addView(entry("Select ISO", false, v -> withStorageAccess(this::pickIso)), Ui.fill(gap));
        p.addView(entry("Options", false, v -> startActivity(new Intent(this, OptionsActivity.class))), Ui.fill(gap));
        p.addView(entry("Logs", false, v -> {
            page = "logs";
            build();
        }), Ui.fill(gap));
        return p;
    }

    private LinearLayout logsMenu() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        int gap = Ui.dp(this, 9);
        View save = entry("Save logs", true, v -> saveLogs(false));
        save.setTag("first");
        p.addView(save, Ui.fill(0));
        p.addView(entry("Share logs", false, v -> saveLogs(true)), Ui.fill(gap));
        p.addView(entry("Clear logs", false, v -> {
            Logs.clear(this);
            Toast.makeText(this, "Logs cleared", Toast.LENGTH_SHORT).show();
            build();
        }), Ui.fill(gap));
        p.addView(entry("Back", false, v -> {
            page = "home";
            build();
        }), Ui.fill(gap));
        return p;
    }

    /** Bottom-left: a tip, then the game files' status and the version. */
    private View statusLines() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        SharedPreferences prefs = Settings.prefs(this);
        String path = prefs.getString(Settings.KEY_GAME_PATH, "");
        String kind = prefs.getString(Settings.KEY_GAME_KIND, "folder");
        String problem = hasStorageAccess() ? GameFiles.checkSaved(path, kind) : (path.isEmpty()
                ? "Select your game folder or .iso first." : "Allow file access so the game files can be read.");
        boolean ready = problem == null;
        TextView tip = Ui.text(this, ("logs".equals(page)
                ? "Logs are saved in Download/" + Logs.FOLDER
                : ready ? "Tip: Options > Performance > Frame monitor shows FPS while you play"
                        : problem).toUpperCase(java.util.Locale.ROOT), 12, Ui.GREY, Ui.regular());
        tip.setLetterSpacing(0.04f);
        p.addView(tip);
        TextView status = Ui.text(this, (ready ? "GAME FILES READY" : "GAME FILES NEEDED") + "   |   "
                + BuildInfo.versionName(this), 16, ready ? Ui.GREEN : Ui.RED, Ui.semi());
        status.setLetterSpacing(0.04f);
        status.setPadding(0, Ui.dp(this, 6), 0, 0);
        p.addView(status);
        return p;
    }

    /** Bottom-right: the credits. */
    private View creditsBlock() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setGravity(Gravity.END);
        TextView h = Ui.text(this, "CREDITS", 22, Ui.WHITE, Ui.bold());
        h.setGravity(Gravity.END);
        p.addView(h);
        for (String[] line : Credits.HOME) {
            TextView t = Ui.text(this, line[0] + ": " + line[1], 14, Ui.WHITE, Ui.regular());
            t.setLetterSpacing(0.05f);
            t.setGravity(Gravity.END);
            t.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 3));
            p.addView(t);
            View rule = new View(this);
            rule.setBackgroundColor(0x88FFFFFF);
            p.addView(rule, new LinearLayout.LayoutParams(Ui.dp(this, 200), Math.max(1, Ui.dp(this, 1))));
        }
        return p;
    }

    /** Bottom-right on the logs page: the last lines of the game log. */
    private View logsInfo() {
        String tail = Logs.tail(this, 8);
        TextView last = Ui.body(this, tail.isEmpty() ? "No game log yet." : tail, 10, Ui.GREY);
        last.setTypeface(android.graphics.Typeface.MONOSPACE);
        last.setMaxWidth(Math.round(getResources().getDisplayMetrics().widthPixels * 0.55f));
        last.setPadding(Ui.dp(this, 10), Ui.dp(this, 8), Ui.dp(this, 10), Ui.dp(this, 8));
        last.setBackground(Ui.rect(0xB3000000, Ui.dp(this, 6)));
        return last;
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
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        String path, problem, kind;
        if (requestCode == REQ_FOLDER) {
            path = GameFiles.treeUriToPath(uri);
            problem = GameFiles.checkFolder(path);
            kind = "folder";
        } else if (requestCode == REQ_ISO) {
            path = GameFiles.documentUriToPath(uri);
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
